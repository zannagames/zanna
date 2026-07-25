//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements the Zia language adapter for interactive REPL sessions.
/// @details The adapter rebuilds a complete synthetic module around each
///          submission, preserving binds, type/function definitions, and typed
///          module-global declarations between calls. Candidate state changes
///          are compile-checked and rolled back on failure. Evaluation then
///          verifies IL, lowers to a fresh bytecode module, executes a fresh VM,
///          and captures runtime output.
///
///          Expressions and `.type` requests are probed through ordered
///          formatter wrappers. Completion builds the same synthetic module and
///          translates the editor byte offset into compiler line/column
///          coordinates before supplementing engine results with session names.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ZiaReplAdapter.cpp
// Purpose: Zia REPL adapter implementation. Compiles each input as a fresh
//          IL module using the Zia compiler and executes via BytecodeVM.
// Key invariants:
//   - buildSource() constructs a complete, compilable Zia program.
//   - Session state is only updated after successful compilation+execution.
//   - Variable persistence across inputs via statement replay in start().
//   - Expression auto-print tries Bool/Int/Num/String wrappers in order.
// Ownership/Lifetime:
//   - Each eval() creates a fresh SourceManager, Module, BytecodeVM.
//   - RtContext is a global singleton that persists across calls.
// Links: src/repl/ZiaReplAdapter.hpp, src/frontends/zia/Compiler.hpp
//
//===----------------------------------------------------------------------===//

#include "ZiaReplAdapter.hpp"

#include "ReplOutputCapture.hpp"

#include "bytecode/BytecodeCompiler.hpp"
#include "bytecode/BytecodeVM.hpp"
#include "frontends/zia/Compiler.hpp"
#include "il/io/Serializer.hpp"
#include "il/verify/Verifier.hpp"
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace zanna::repl {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Advance a byte offset past contiguous whitespace.
/// @param s String to scan.
/// @param pos Initial byte offset.
/// @return First non-whitespace offset at or after @p pos, or `s.size()`.
static size_t skipWhitespace(const std::string &s, size_t pos = 0) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
        ++pos;
    return pos;
}

/// @brief Match a case-sensitive Zia keyword at a byte offset.
/// @param s Source string to inspect.
/// @param offset Candidate keyword offset.
/// @param kw Null-terminated keyword.
/// @return `true` when spelling matches and the following byte is whitespace,
///         `(`, `{`, or end-of-input.
static bool startsWithKeyword(const std::string &s, size_t offset, const char *kw) {
    size_t kwLen = std::strlen(kw);
    if (s.size() - offset < kwLen)
        return false;
    if (s.compare(offset, kwLen, kw) != 0)
        return false;
    // Must be followed by space, '(', '{', or end of string
    if (offset + kwLen < s.size()) {
        char next = s[offset + kwLen];
        return std::isspace(static_cast<unsigned char>(next)) || next == '(' || next == '{';
    }
    return true; // keyword at end of string
}

/// @brief Return true when @p c can appear in a Zia identifier.
/// @param c Character to classify.
/// @return True for ASCII letters, digits, and underscores.
static bool isIdentifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// @brief Find a top-level assignment operator in a Zia input line.
/// @details String literals and nested delimiters are ignored. Comparison operators
///          (`==`, `!=`, `<=`, `>=`) are deliberately skipped so expressions do not
///          get misclassified as statements.
/// @param input Source text to inspect.
/// @return Offset of the assignment operator, or `std::string::npos` when absent.
static size_t findTopLevelAssignmentOperator(const std::string &input) {
    bool inString = false;
    bool escape = false;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    for (size_t pos = 0; pos < input.size(); ++pos) {
        char c = input[pos];
        if (escape) {
            escape = false;
            continue;
        }
        if (inString && c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            inString = !inString;
            continue;
        }
        if (inString)
            continue;

        if (c == '(') {
            ++parenDepth;
            continue;
        }
        if (c == ')' && parenDepth > 0) {
            --parenDepth;
            continue;
        }
        if (c == '[') {
            ++bracketDepth;
            continue;
        }
        if (c == ']' && bracketDepth > 0) {
            --bracketDepth;
            continue;
        }
        if (c == '{') {
            ++braceDepth;
            continue;
        }
        if (c == '}' && braceDepth > 0) {
            --braceDepth;
            continue;
        }

        if (c != '=' || parenDepth != 0 || bracketDepth != 0 || braceDepth != 0)
            continue;

        char prev = pos > 0 ? input[pos - 1] : '\0';
        char next = pos + 1 < input.size() ? input[pos + 1] : '\0';
        if (prev == '=' || prev == '!' || prev == '<' || prev == '>' || next == '=')
            continue;
        return pos;
    }

    return std::string::npos;
}

/// @brief Append a statement terminator when source does not already end one.
/// @param src Destination synthetic source to extend.
/// @param input Fragment whose last non-space byte controls insertion.
static void appendAutoSemicolon(std::string &src, const std::string &input) {
    size_t lastNonSpace = input.find_last_not_of(" \t\r\n");
    if (lastNonSpace != std::string::npos && input[lastNonSpace] != ';' &&
        input[lastNonSpace] != '}') {
        src += ";";
    }
}

/// @brief Return a source literal that default-initializes @p type.
/// @param type Source-level Zia type name.
/// @return Default expression text, or empty when no safe literal is known.
static std::string defaultExprForZiaType(const std::string &type) {
    if (type == "Integer")
        return "0";
    if (type == "Number")
        return "0.0";
    if (type == "Boolean")
        return "false";
    if (type == "String")
        return "\"\"";
    return "";
}

// ---------------------------------------------------------------------------
// Construction / Reset
// ---------------------------------------------------------------------------

/// @brief Construct a Zia adapter with the default runtime binds.
ZiaReplAdapter::ZiaReplAdapter() {
    bindStatements_.push_back("bind Zanna.Terminal");
    bindStatements_.push_back("bind Zanna.Text.Fmt as Fmt");
    bindStatements_.push_back("bind Zanna.Core.Object as Obj");
}

/// @brief Return the language identifier exposed by this adapter.
/// @return Static view containing `"zia"`.
std::string_view ZiaReplAdapter::languageName() const {
    return "zia";
}

/// @brief Clear session state and restore the default runtime binds.
/// @details User binds, definitions, persistent variables, and global type
///          metadata are discarded together.
void ZiaReplAdapter::reset() {
    bindStatements_.clear();
    definedFunctions_.clear();
    definedTypes_.clear();
    persistentVars_.clear();
    globalVarDecls_.clear();

    bindStatements_.push_back("bind Zanna.Terminal");
    bindStatements_.push_back("bind Zanna.Text.Fmt as Fmt");
    bindStatements_.push_back("bind Zanna.Core.Object as Obj");
}

// ---------------------------------------------------------------------------
// Persistent variable management
// ---------------------------------------------------------------------------

/// @brief Find mutable persistent-variable metadata by exact Zia name.
/// @param name Case-sensitive variable name.
/// @return Pointer into adapter-owned vector storage, or `nullptr`; vector
///         reallocation and reset invalidate the pointer.
PersistentVar *ZiaReplAdapter::findPersistentVar(const std::string &name) {
    for (auto &pv : persistentVars_)
        if (pv.name == name)
            return &pv;
    return nullptr;
}

/// @brief Find persistent-variable metadata by exact Zia name.
/// @param name Case-sensitive variable name.
/// @return Read-only pointer into adapter-owned storage, or `nullptr`.
const PersistentVar *ZiaReplAdapter::findPersistentVar(const std::string &name) const {
    for (const auto &pv : persistentVars_)
        if (pv.name == name)
            return &pv;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Input classification
// ---------------------------------------------------------------------------

/// @brief Detect a top-level bind statement.
/// @param input Zia source fragment.
/// @return `true` when the leading keyword is `bind`.
bool ZiaReplAdapter::isBind(const std::string &input) const {
    size_t start = skipWhitespace(input);
    return startsWithKeyword(input, start, "bind");
}

/// @brief Detect a top-level function definition.
/// @param input Zia source fragment.
/// @return `true` when the leading keyword is `func`.
bool ZiaReplAdapter::isFuncDef(const std::string &input) const {
    size_t start = skipWhitespace(input);
    return startsWithKeyword(input, start, "func");
}

/// @brief Detect a class, struct, or interface definition.
/// @param input Zia source fragment.
/// @return `true` when a recognized type-definition keyword leads the input.
bool ZiaReplAdapter::isTypeDef(const std::string &input) const {
    size_t start = skipWhitespace(input);
    return startsWithKeyword(input, start, "class") || startsWithKeyword(input, start, "struct") ||
           startsWithKeyword(input, start, "interface");
}

/// @brief Detect a top-level variable declaration.
/// @param input Zia source fragment.
/// @return `true` when the leading keyword is `var`.
bool ZiaReplAdapter::isVarDecl(const std::string &input) const {
    size_t start = skipWhitespace(input);
    return startsWithKeyword(input, start, "var");
}

/// @brief Detect assignment through a known persistent root variable.
/// @details A leading identifier must name persistent state and a top-level
///          assignment operator must exist; member and index writes are allowed
///          while comparison operators are excluded.
/// @param input Zia source fragment.
/// @return `true` when the fragment mutates a tracked persistent root.
bool ZiaReplAdapter::isAssignment(const std::string &input) const {
    size_t pos = skipWhitespace(input);
    if (pos >= input.size() ||
        !(std::isalpha(static_cast<unsigned char>(input[pos])) || input[pos] == '_'))
        return false;

    // Read identifier
    size_t idStart = pos;
    while (pos < input.size() && isIdentifierChar(input[pos]))
        ++pos;
    std::string ident = input.substr(idStart, pos - idStart);

    if (findTopLevelAssignmentOperator(input) == std::string::npos)
        return false;

    // Must assign to a known persistent root variable, including member/index writes.
    return findPersistentVar(ident) != nullptr;
}

/// @brief Extract the leading root identifier from a known assignment.
/// @param input Zia assignment source.
/// @return Leading alphanumeric/underscore identifier.
std::string ZiaReplAdapter::extractAssignTarget(const std::string &input) const {
    size_t pos = skipWhitespace(input);
    size_t idStart = pos;
    while (pos < input.size() &&
           (std::isalnum(static_cast<unsigned char>(input[pos])) || input[pos] == '_'))
        ++pos;
    return input.substr(idStart, pos - idStart);
}

/// @brief Heuristically determine whether input should be auto-printed.
/// @details Empty input, known statement keywords, explicit print calls, and
///          persistent assignments are excluded. Wrapper compilation later
///          confirms whether the remaining input is a printable expression.
/// @param input Zia source fragment.
/// @return `true` when ordered expression wrappers should be attempted.
bool ZiaReplAdapter::isLikelyExpression(const std::string &input) const {
    size_t start = skipWhitespace(input);
    if (start >= input.size())
        return false;

    // Statement keywords — these are never expressions
    static const char *stmtKeywords[] = {"var",
                                         "func",
                                         "class",
                                         "struct",
                                         "interface",
                                         "if",
                                         "while",
                                         "for",
                                         "return",
                                         "bind",
                                         "module",
                                         "throw",
                                         "try",
                                         "break",
                                         "continue"};
    for (const char *kw : stmtKeywords) {
        if (startsWithKeyword(input, start, kw))
            return false;
    }

    // Already printing — no need to auto-print
    if (startsWithKeyword(input, start, "Say") || startsWithKeyword(input, start, "Print") ||
        startsWithKeyword(input, start, "SayInt") || startsWithKeyword(input, start, "PrintInt"))
        return false;

    // Assignment to a known variable — not an expression
    if (isAssignment(input))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Name / type extraction
// ---------------------------------------------------------------------------

/// @brief Extract a function name from a definition.
/// @param input Zia function-definition source.
/// @return Identifier following `func`, or an empty string on mismatch.
std::string ZiaReplAdapter::extractFuncName(const std::string &input) const {
    size_t pos = input.find("func ");
    if (pos == std::string::npos)
        return "";
    pos += 5;
    pos = skipWhitespace(input, pos);
    size_t nameStart = pos;
    while (pos < input.size() &&
           (std::isalnum(static_cast<unsigned char>(input[pos])) || input[pos] == '_'))
        ++pos;
    return input.substr(nameStart, pos - nameStart);
}

/// @brief Extract a declared class, struct, or interface name.
/// @param input Zia type-definition source.
/// @return Identifier following the first recognized definition keyword, or
///         an empty string.
std::string ZiaReplAdapter::extractTypeName(const std::string &input) const {
    size_t pos = 0;
    if (input.find("class ") != std::string::npos)
        pos = input.find("class ") + 6;
    else if (input.find("struct ") != std::string::npos)
        pos = input.find("struct ") + 7;
    else if (input.find("interface ") != std::string::npos)
        pos = input.find("interface ") + 10;
    else
        return "";

    pos = skipWhitespace(input, pos);
    size_t nameStart = pos;
    while (pos < input.size() &&
           (std::isalnum(static_cast<unsigned char>(input[pos])) || input[pos] == '_'))
        ++pos;
    return input.substr(nameStart, pos - nameStart);
}

/// @brief Extract variable name and declared type from a `var` declaration.
/// @details The returned type is `"auto"` when no explicit colon type appears.
/// @param input Zia variable-declaration source.
/// @return Name/type pair, or two empty strings when no `var` keyword exists.
std::pair<std::string, std::string> ZiaReplAdapter::extractVarInfo(const std::string &input) const {
    size_t pos = input.find("var ");
    if (pos == std::string::npos)
        return {"", ""};

    pos += 4;
    pos = skipWhitespace(input, pos);

    size_t nameStart = pos;
    while (pos < input.size() &&
           (std::isalnum(static_cast<unsigned char>(input[pos])) || input[pos] == '_'))
        ++pos;
    std::string name = input.substr(nameStart, pos - nameStart);

    pos = skipWhitespace(input, pos);

    std::string type = "auto";
    if (pos < input.size() && input[pos] == ':') {
        ++pos;
        pos = skipWhitespace(input, pos);
        size_t typeStart = pos;
        while (pos < input.size() && !std::isspace(static_cast<unsigned char>(input[pos])) &&
               input[pos] != '=' && input[pos] != ';')
            ++pos;
        type = input.substr(typeStart, pos - typeStart);
    }

    return {name, type};
}

/// @brief Extract and normalize a variable initializer.
/// @details The first equals sign outside a quoted string begins the
///          initializer. Leading whitespace and trailing whitespace/semicolons
///          are removed.
/// @param input Zia variable-declaration source.
/// @return Initializer expression, or an empty string when none exists.
std::string ZiaReplAdapter::extractVarInitializer(const std::string &input) const {
    bool inString = false;
    bool escape = false;
    for (size_t pos = 0; pos < input.size(); ++pos) {
        char c = input[pos];
        if (escape) {
            escape = false;
            continue;
        }
        if (inString && c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            inString = !inString;
            continue;
        }
        if (!inString && c == '=') {
            std::string init = input.substr(pos + 1);
            size_t start = skipWhitespace(init);
            init.erase(0, start);
            while (!init.empty() &&
                   (init.back() == ';' || std::isspace(static_cast<unsigned char>(init.back()))))
                init.pop_back();
            return init;
        }
    }
    return "";
}

/// @brief Resolve a stable type for a persistent module-global variable.
/// @details Explicit non-auto types win. `new` expressions yield their
///          constructed qualified name, while primitive initializers are probed
///          through `getExprType()`. Unsupported inference returns `"auto"`.
/// @param input Original declaration retained for diagnostic/context symmetry.
/// @param explicitType Parsed type spelling or `"auto"`.
/// @param initializer Normalized initializer expression.
/// @return Resolved source-level type name or `"auto"`.
std::string ZiaReplAdapter::inferPersistentVarType(const std::string &input,
                                                   const std::string &explicitType,
                                                   const std::string &initializer) {
    if (!explicitType.empty() && explicitType != "auto")
        return explicitType;

    size_t pos = skipWhitespace(initializer);
    if (startsWithKeyword(initializer, pos, "new")) {
        pos = skipWhitespace(initializer, pos + 3);
        size_t typeStart = pos;
        while (pos < initializer.size() &&
               (std::isalnum(static_cast<unsigned char>(initializer[pos])) ||
                initializer[pos] == '_' || initializer[pos] == '.'))
            ++pos;
        if (pos > typeStart)
            return initializer.substr(typeStart, pos - typeStart);
    }

    if (!initializer.empty()) {
        std::string probed = getExprType(initializer);
        if (probed == "Boolean" || probed == "Integer" || probed == "Number" ||
            probed == "String")
            return probed;
    }

    (void)input;
    return "auto";
}

/// @brief Build a typed module-global declaration for persistent storage.
/// @param name Variable identifier.
/// @param type Resolved non-auto type.
/// @return `var <name>: <type>`, or empty when type is unavailable.
std::string ZiaReplAdapter::makePersistentVarDecl(const std::string &name,
                                                  const std::string &type) const {
    if (type.empty() || type == "auto")
        return "";
    return "var " + name + ": " + type;
}

// ---------------------------------------------------------------------------
// Source building
// ---------------------------------------------------------------------------

/// @brief Assemble a complete synthetic Zia module for one operation.
/// @details The source contains default/user binds, stored type and function
///          definitions, persistent typed module globals, optional additional
///          top-level source, and a `start()` function containing @p input.
///          Statement terminators are inserted where required.
/// @param input Current statement or wrapped expression placed in `start()`.
/// @param extraTopLevel Optional top-level declaration used while validating a
///        newly introduced persistent global.
/// @return Self-contained Zia source module.
std::string ZiaReplAdapter::buildSource(const std::string &input,
                                        const std::string &extraTopLevel) const {
    std::string src;
    src.reserve(2048);

    src += "module Repl;\n\n";

    // Bind statements
    for (const auto &b : bindStatements_) {
        src += b;
        src += ";\n";
    }
    src += "\n";

    // Type definitions
    for (const auto &[name, typeSrc] : definedTypes_) {
        src += typeSrc;
        src += "\n\n";
    }

    // Function definitions
    for (const auto &[name, funcSrc] : definedFunctions_) {
        src += funcSrc;
        src += "\n\n";
    }

    // Persistent REPL variables live as module globals so runtime storage
    // persists across fresh VM/module instances without replaying initializers.
    for (const auto &pv : persistentVars_) {
        if (!pv.declStatement.empty()) {
            src += pv.declStatement;
            appendAutoSemicolon(src, pv.declStatement);
            src += "\n";
        }
    }

    if (!extraTopLevel.empty()) {
        src += extraTopLevel;
        appendAutoSemicolon(src, extraTopLevel);
        src += "\n";
    }

    // Entry point
    src += "func start() {\n";

    // Current user input
    if (!input.empty()) {
        src += "    ";
        src += input;
        appendAutoSemicolon(src, input);
        src += "\n";
    }

    src += "}\n";

    return src;
}

// ---------------------------------------------------------------------------
// Compilation helpers
// ---------------------------------------------------------------------------

/// @brief Check whether synthetic Zia source compiles and verifies.
/// @param source Complete source module.
/// @return `true` only when frontend compilation and IL verification succeed.
bool ZiaReplAdapter::tryCompileOnly(const std::string &source) const {
    using namespace il::frontends::zia;
    il::support::SourceManager sm;
    CompilerOptions opts;
    auto compileResult = compile({source, "<repl>"}, opts, sm);
    if (!compileResult.succeeded())
        return false;
    auto verification = il::verify::Verifier::verify(compileResult.module);
    return verification.hasValue();
}

/// @brief Compile and verify Zia source while collecting diagnostics.
/// @param source Complete source module.
/// @return Empty string on success; frontend or verifier diagnostic text on
///         failure.
std::string ZiaReplAdapter::compileOnlyDiagnostic(const std::string &source) const {
    using namespace il::frontends::zia;

    il::support::SourceManager sm;
    CompilerOptions opts;
    auto compileResult = compile({source, "<repl>"}, opts, sm);
    if (!compileResult.succeeded()) {
        std::ostringstream errStream;
        compileResult.diagnostics.printAll(errStream, &sm);
        return errStream.str();
    }

    auto verification = il::verify::Verifier::verify(compileResult.module);
    if (!verification)
        return "Type error: " + verification.error().message;
    return "";
}

/// @brief Compile, verify, lower, and execute one synthetic Zia module.
/// @details Each call owns fresh source manager, compiler result, bytecode
///          module, and VM state. Threaded dispatch and the runtime bridge are
///          enabled, output is captured, and traps become unsuccessful results.
/// @param source Complete source module.
/// @return Evaluation status, captured output, and any compile/verify/trap
///         diagnostic.
EvalResult ZiaReplAdapter::compileAndRun(const std::string &source) {
    using namespace il::frontends::zia;

    EvalResult result;

    il::support::SourceManager sm;
    CompilerOptions opts;
    auto compileResult = compile({source, "<repl>"}, opts, sm);

    if (!compileResult.succeeded()) {
        std::ostringstream errStream;
        compileResult.diagnostics.printAll(errStream, &sm);
        result.success = false;
        result.errorMessage = errStream.str();
        return result;
    }

    auto verification = il::verify::Verifier::verify(compileResult.module);
    if (!verification) {
        result.success = false;
        result.errorMessage = "Type error: " + verification.error().message;
        return result;
    }

    // Compile to bytecode and execute
    zanna::bytecode::BytecodeCompiler bcCompiler;
    zanna::bytecode::BytecodeModule bcModule = bcCompiler.compile(compileResult.module);

    zanna::bytecode::BytecodeVM bcVm;
    bcVm.setThreadedDispatch(true);
    bcVm.setRuntimeBridgeEnabled(true);
    bcVm.load(&bcModule);

    ScopedReplOutputCapture outputCapture;
    bcVm.exec("main", {});

    if (bcVm.state() == zanna::bytecode::VMState::Trapped) {
        result.success = false;
        result.trapped = true;
        result.errorMessage = "Runtime error: " + bcVm.trapMessage();
        return result;
    }

    result.success = true;
    result.output = outputCapture.output();
    return result;
}

// ---------------------------------------------------------------------------
// eval() — main REPL evaluation entry point
// ---------------------------------------------------------------------------

/// @brief Evaluate one Zia REPL submission transactionally.
/// @details Binds and named definitions are compile-checked before commitment
///          and restored on failure. Variable declarations create typed
///          persistent module globals only after successful initialization.
///          Likely expressions are tried through ordered Boolean, integer,
///          number, string, and object wrappers before bare execution.
/// @param input Complete user submission.
/// @return Evaluation result with captured output, inferred display type, or
///         diagnostic/trap state.
EvalResult ZiaReplAdapter::eval(const std::string &input) {
    using namespace il::frontends::zia;

    EvalResult result;

    // --- Bind statements ---
    if (isBind(input)) {
        std::string testBind = input;
        while (!testBind.empty() && (testBind.back() == ';' || testBind.back() == ' '))
            testBind.pop_back();

        for (const auto &existing : bindStatements_) {
            if (existing == testBind) {
                result.success = true;
                result.output = "(already bound)";
                return result;
            }
        }

        bindStatements_.push_back(testBind);
        std::string testSrc = buildSource("");
        if (!tryCompileOnly(testSrc)) {
            bindStatements_.pop_back();
            result.success = false;
            result.errorMessage = compileOnlyDiagnostic(testSrc);
            return result;
        }

        result.success = true;
        return result;
    }

    // --- Function definitions ---
    if (isFuncDef(input)) {
        std::string funcName = extractFuncName(input);
        if (funcName.empty()) {
            result.success = false;
            result.errorMessage = "Could not parse function name.";
            return result;
        }

        auto oldFunc = definedFunctions_.find(funcName);
        std::string oldFuncSrc;
        if (oldFunc != definedFunctions_.end())
            oldFuncSrc = oldFunc->second;

        definedFunctions_[funcName] = input;
        std::string testSrc = buildSource("");
        if (!tryCompileOnly(testSrc)) {
            if (oldFuncSrc.empty())
                definedFunctions_.erase(funcName);
            else
                definedFunctions_[funcName] = oldFuncSrc;

            result.success = false;
            result.errorMessage = compileOnlyDiagnostic(testSrc);
            return result;
        }

        result.success = true;
        return result;
    }

    // --- Type definitions ---
    if (isTypeDef(input)) {
        std::string typeName = extractTypeName(input);
        if (typeName.empty()) {
            result.success = false;
            result.errorMessage = "Could not parse type name.";
            return result;
        }

        auto oldType = definedTypes_.find(typeName);
        std::string oldTypeSrc;
        if (oldType != definedTypes_.end())
            oldTypeSrc = oldType->second;

        definedTypes_[typeName] = input;
        std::string testSrc = buildSource("");
        if (!tryCompileOnly(testSrc)) {
            if (oldTypeSrc.empty())
                definedTypes_.erase(typeName);
            else
                definedTypes_[typeName] = oldTypeSrc;

            result.success = false;
            result.errorMessage = compileOnlyDiagnostic(testSrc);
            return result;
        }

        result.success = true;
        return result;
    }

    // --- Variable declarations ---
    if (isVarDecl(input)) {
        auto [varName, declaredType] = extractVarInfo(input);
        if (varName.empty()) {
            result.success = false;
            result.errorMessage = "Could not parse variable name.";
            return result;
        }

        std::string initializer = extractVarInitializer(input);
        std::string varType = inferPersistentVarType(input, declaredType, initializer);
        std::string globalDecl = makePersistentVarDecl(varName, varType);
        if (globalDecl.empty()) {
            // Fall back to the compiler's diagnostic for unsupported inference cases.
            result = compileAndRun(buildSource(input));
            return result;
        }

        std::string initStatement;
        if (!initializer.empty())
            initStatement = varName + " = " + initializer;
        else if (std::string defaultExpr = defaultExprForZiaType(varType); !defaultExpr.empty())
            initStatement = varName + " = " + defaultExpr;

        auto existing = findPersistentVar(varName);
        std::string oldDecl;
        std::string oldType;
        if (existing) {
            oldDecl = existing->declStatement;
            oldType = existing->type;
            existing->declStatement = globalDecl;
            existing->lastAssignment.clear();
            existing->type = varType;
        }

        std::string source = buildSource(initStatement, existing ? std::string() : globalDecl);
        result = compileAndRun(source);
        if (!result.success) {
            if (existing) {
                existing->declStatement = oldDecl;
                existing->type = oldType;
            }
            return result;
        }

        if (!existing)
            persistentVars_.push_back({varName, globalDecl, "", varType});
        globalVarDecls_[varName] = varType;
        result.resultType = ResultType::Statement;
        return result;
    }

    // --- Expression auto-print ---
    // Try wrapping the input as an expression with different formatters.
    // The wrapper order ensures the most specific type matches first.
    if (isLikelyExpression(input)) {
        struct Wrapper {
            const char *format; // printf-style: %s is the expression
            ResultType type;
        };

        static const Wrapper wrappers[] = {
            {"Say(Fmt.Bool(%s))", ResultType::Boolean},
            {"Say(Fmt.Int(%s))", ResultType::Integer},
            {"Say(Fmt.Num(%s))", ResultType::Number},
            {"Say(%s)", ResultType::String},
            {"Say(Obj.ToString(%s))", ResultType::Object},
        };

        // Strip trailing semicolons from expression for wrapping
        std::string expr = input;
        while (!expr.empty() &&
               (expr.back() == ';' || std::isspace(static_cast<unsigned char>(expr.back()))))
            expr.pop_back();

        for (const auto &w : wrappers) {
            // Build wrapped input: e.g., "Say(Fmt.Int(x + 1))"
            std::string wrapped;
            const char *fmt = w.format;
            const char *pct = std::strstr(fmt, "%s");
            if (pct) {
                wrapped.append(fmt, pct);
                wrapped += expr;
                wrapped += (pct + 2);
            }

            std::string testSource = buildSource(wrapped);
            if (tryCompileOnly(testSource)) {
                // Compile and execute with this wrapper
                result = compileAndRun(testSource);
                if (result.success)
                    result.resultType = w.type;
                return result;
            }
        }

        // No wrapper matched — fall through to bare statement execution
    }

    // --- Variable assignment persistence ---
    if (isAssignment(input)) {
        std::string target = extractAssignTarget(input);
        PersistentVar *pv = findPersistentVar(target);
        if (pv) {
            (void)pv;
            result = compileAndRun(buildSource(input));
            return result;
        }
    }

    // --- Regular statement / expression: compile and execute ---
    std::string source = buildSource(input);
    result = compileAndRun(source);

    // Track new variable declarations on success
    if (result.success && isVarDecl(input)) {
        auto [varName, varType] = extractVarInfo(input);
        if (!varName.empty()) {
            // Strip trailing semicolons from the declaration for storage
            std::string cleanDecl = input;
            while (!cleanDecl.empty() &&
                   (cleanDecl.back() == ';' ||
                    std::isspace(static_cast<unsigned char>(cleanDecl.back()))))
                cleanDecl.pop_back();

            // Add to persistent state first (so buildSource includes it)
            PersistentVar *existing = findPersistentVar(varName);
            if (existing) {
                existing->declStatement = cleanDecl;
                existing->lastAssignment.clear();
                existing->type = varType;
            } else {
                persistentVars_.push_back({varName, cleanDecl, "", varType});
            }

            // Now probe the actual type if it was inferred
            if (varType == "auto") {
                varType = getExprType(varName);
                // Update the stored type
                PersistentVar *pv = findPersistentVar(varName);
                if (pv)
                    pv->type = varType;
            }

            globalVarDecls_[varName] = varType;
        }
    }

    if (result.success)
        result.resultType = ResultType::Statement;

    return result;
}

// ---------------------------------------------------------------------------
// .type meta-command
// ---------------------------------------------------------------------------

/// @brief Infer a presentation type for an expression through compile probes.
/// @details Trailing separators are removed, then the same ordered formatter
///          wrappers used by evaluation are compiled. A fragment that only
///          compiles as a bare statement reports `Void`.
/// @param expr Expression or statement source.
/// @return `Boolean`, `Integer`, `Number`, `String`, `Object`, `Void`, or an
///         explanatory unknown marker.
std::string ZiaReplAdapter::getExprType(const std::string &expr) {
    // Try each type wrapper to determine the expression's type.
    // The first wrapper that compiles successfully reveals the type.
    std::string cleanExpr = expr;
    while (!cleanExpr.empty() &&
           (cleanExpr.back() == ';' || std::isspace(static_cast<unsigned char>(cleanExpr.back()))))
        cleanExpr.pop_back();

    struct TypeProbe {
        const char *format;
        const char *typeName;
    };

    static const TypeProbe probes[] = {
        {"Say(Fmt.Bool(%s))", "Boolean"},
        {"Say(Fmt.Int(%s))", "Integer"},
        {"Say(Fmt.Num(%s))", "Number"},
        {"Say(%s)", "String"},
        {"Say(Obj.ToString(%s))", "Object"},
    };

    for (const auto &p : probes) {
        std::string wrapped;
        const char *pct = std::strstr(p.format, "%s");
        if (pct) {
            wrapped.append(p.format, pct);
            wrapped += cleanExpr;
            wrapped += (pct + 2);
        }

        std::string testSource = buildSource(wrapped);
        if (tryCompileOnly(testSource))
            return p.typeName;
    }

    // Try compiling as a void statement (no return value)
    std::string testSource = buildSource(cleanExpr);
    if (tryCompileOnly(testSource))
        return "Void";

    return "(unknown — expression does not compile)";
}

// ---------------------------------------------------------------------------
// Tab completion
// ---------------------------------------------------------------------------

/// @brief Build synthetic source and compiler coordinates for completion.
/// @details Session binds, definitions, and persistent globals precede a
///          `start()` body containing the complete editor buffer. The cursor's
///          byte offset is translated after the body's four-space indentation.
/// @param input Current editor buffer.
/// @param cursor Clamped byte offset within @p input.
/// @param[out] line Receives the one-based synthetic source line.
/// @param[out] col Receives the zero-based synthetic source column.
/// @return Complete source module supplied to the completion engine.
std::string ZiaReplAdapter::buildSourceForCompletion(const std::string &input,
                                                     size_t cursor,
                                                     int &line,
                                                     int &col) const {
    std::string src;
    src.reserve(2048);

    src += "module Repl;\n\n";

    for (const auto &b : bindStatements_) {
        src += b;
        src += ";\n";
    }
    src += "\n";

    for (const auto &[name, typeSrc] : definedTypes_) {
        src += typeSrc;
        src += "\n\n";
    }

    for (const auto &[name, funcSrc] : definedFunctions_) {
        src += funcSrc;
        src += "\n\n";
    }

    for (const auto &pv : persistentVars_) {
        if (!pv.declStatement.empty()) {
            src += pv.declStatement;
            appendAutoSemicolon(src, pv.declStatement);
            src += "\n";
        }
    }
    src += "\n";

    src += "func start() {\n";

    // Count newlines to find the line where the user input begins
    int inputLine = 1;
    for (char c : src) {
        if (c == '\n')
            ++inputLine;
    }

    // Add user input (only up to the source length, completion engine needs full source)
    src += "    ";
    src += input;
    appendAutoSemicolon(src, input);
    src += "\n";
    src += "}\n";

    // The cursor is at (inputLine, 4 + cursor) — 4 for the "    " indent
    line = inputLine;
    col = 4 + static_cast<int>(cursor);

    return src;
}

/// @brief Produce compiler-backed and session-backed Zia completions.
/// @details Engine insertion texts are deduplicated and expanded into complete
///          editor-buffer replacements. Prefix-matching persistent variables and
///          stored functions supplement symbols unavailable to the completion
///          engine.
/// @param input Current editor buffer.
/// @param cursor Byte offset of the insertion point; oversized values are
///        clamped.
/// @return Full-buffer completion alternatives in engine/session order.
std::vector<std::string> ZiaReplAdapter::complete(const std::string &input, size_t cursor) {
    using namespace il::frontends::zia;

    std::vector<std::string> matches;
    cursor = std::min(cursor, input.size());

    if (input.empty())
        return matches;

    // Find the prefix being completed: scan back from cursor for identifier chars
    size_t tokenStart = cursor;
    while (tokenStart > 0 && (std::isalnum(static_cast<unsigned char>(input[tokenStart - 1])) ||
                              input[tokenStart - 1] == '_'))
        --tokenStart;

    std::string prefix = input.substr(tokenStart, cursor - tokenStart);
    std::string beforeToken = input.substr(0, tokenStart);
    std::string afterCursor = (cursor < input.size()) ? input.substr(cursor) : "";

    // Use CompletionEngine for rich completions (member access, types, runtime)
    int line = 0, col = 0;
    std::string source = buildSourceForCompletion(input, cursor, line, col);
    auto items = completionEngine_.complete(source, line, col, "<repl>", 30);

    // Track which labels we've already added (avoid duplicates)
    std::set<std::string> seen;

    for (const auto &item : items) {
        if (seen.insert(item.insertText).second)
            matches.push_back(beforeToken + item.insertText + afterCursor);
    }

    // Supplement with session variables (local to start(), invisible to CompletionEngine)
    if (!prefix.empty()) {
        for (const auto &pv : persistentVars_) {
            if (pv.name.size() >= prefix.size() && pv.name.compare(0, prefix.size(), prefix) == 0 &&
                seen.insert(pv.name).second) {
                matches.push_back(beforeToken + pv.name + afterCursor);
            }
        }

        // Supplement with session functions
        for (const auto &[name, src] : definedFunctions_) {
            if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
                seen.insert(name).second) {
                matches.push_back(beforeToken + name + afterCursor);
            }
        }
    }

    return matches;
}

// ---------------------------------------------------------------------------
// .il meta-command — show generated IL
// ---------------------------------------------------------------------------

/// @brief Compile input and serialize its generated IL module.
/// @details Printable wrapper forms are attempted first so expressions have a
///          valid statement context. If none compile, the cleaned input is
///          retried as a bare statement and its diagnostics are returned on
///          failure.
/// @param input Expression or statement source.
/// @return Serialized IL on success, or prefixed compilation diagnostics.
std::string ZiaReplAdapter::getIL(const std::string &input) {
    using namespace il::frontends::zia;

    std::string cleanInput = input;
    while (!cleanInput.empty() && (cleanInput.back() == ';' ||
                                   std::isspace(static_cast<unsigned char>(cleanInput.back()))))
        cleanInput.pop_back();

    static const char *wrappers[] = {
        "Say(Fmt.Bool(%s))",
        "Say(Fmt.Int(%s))",
        "Say(Fmt.Num(%s))",
        "Say(%s)",
        "Say(Obj.ToString(%s))",
    };

    il::support::SourceManager sm;
    CompilerOptions opts;
    il::frontends::zia::CompilerResult compileResult;
    bool compiled = false;

    for (const char *fmt : wrappers) {
        std::string wrapped;
        const char *pct = std::strstr(fmt, "%s");
        if (!pct)
            continue;
        wrapped.append(fmt, pct);
        wrapped += cleanInput;
        wrapped += (pct + 2);

        std::string source = buildSource(wrapped);
        compileResult = compile({source, "<repl>"}, opts, sm);
        if (compileResult.succeeded()) {
            compiled = true;
            break;
        }
    }

    if (!compiled) {
        // Try as bare statement
        std::string source = buildSource(cleanInput);
        il::support::SourceManager sm2;
        compileResult = compile({source, "<repl>"}, opts, sm2);
        if (!compileResult.succeeded()) {
            std::ostringstream errStream;
            compileResult.diagnostics.printAll(errStream, &sm2);
            return "Compilation error: " + errStream.str();
        }
    }

    // Print the IL module
    return il::io::Serializer::toString(compileResult.module);
}

// ---------------------------------------------------------------------------
// Session state queries
// ---------------------------------------------------------------------------

/// @brief Snapshot persistent variables known to the session.
/// @return Value-owned name/type records in declaration order.
std::vector<VarInfo> ZiaReplAdapter::listVariables() const {
    std::vector<VarInfo> vars;
    for (const auto &pv : persistentVars_) {
        vars.push_back({pv.name, pv.type});
    }
    return vars;
}

/// @brief Snapshot stored user function definitions.
/// @details Display signatures span from the first opening parenthesis to the
///          definition body and have trailing whitespace removed.
/// @return Value-owned function metadata in map iteration order.
std::vector<FuncInfo> ZiaReplAdapter::listFunctions() const {
    std::vector<FuncInfo> funcs;
    for (const auto &[name, src] : definedFunctions_) {
        size_t parenStart = src.find('(');
        size_t braceStart = src.find('{');
        std::string sig;
        if (parenStart != std::string::npos) {
            size_t end = (braceStart != std::string::npos) ? braceStart : src.size();
            sig = src.substr(parenStart, end - parenStart);
            while (!sig.empty() && std::isspace(static_cast<unsigned char>(sig.back())))
                sig.pop_back();
        }
        funcs.push_back({name, sig});
    }
    return funcs;
}

/// @brief Snapshot active bind statements.
/// @return Value-owned bind strings in registration order.
std::vector<std::string> ZiaReplAdapter::listBinds() const {
    return bindStatements_;
}

} // namespace zanna::repl
