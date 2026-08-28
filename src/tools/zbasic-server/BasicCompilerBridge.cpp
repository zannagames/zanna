//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements the protocol-neutral BASIC compiler bridge used by Zanna's
///        language-server frontends.
///
/// Analysis and dump requests create isolated compiler/source-manager state.
/// Completion alone shares a long-lived cache protected by a mutex. Hover
/// cursor extraction uses the common text utilities, and every returned result
/// owns its data.
///
/// @see BasicCompilerBridge.hpp
//
//===----------------------------------------------------------------------===//

#include "tools/zbasic-server/BasicCompilerBridge.hpp"

#include "tools/lsp-common/DiagnosticUtils.hpp"
#include "tools/lsp-common/TextUtils.hpp"

#include "frontends/basic/AstPrinter.hpp"
#include "frontends/basic/BasicAnalysis.hpp"
#include "frontends/basic/BasicCompiler.hpp"
#include "frontends/basic/BasicCompletion.hpp"
#include "frontends/basic/Lexer.hpp"
#include "frontends/basic/OopIndex.hpp"
#include "frontends/basic/Token.hpp"
#include "il/io/Serializer.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include "il/transform/PassManager.hpp"
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zanna::server {

using namespace il::frontends::basic;

/// @brief Uppercase a string to match BASIC's case-folded identifier convention.
/// @param s Identifier or qualified name to fold.
/// @return Uppercase copy using unsigned-byte-safe character classification.
static std::string toUpperStr(const std::string &s) {
    std::string upper;
    upper.reserve(s.size());
    for (char c : s)
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return upper;
}

/// @brief Remove BASIC's single-character type suffix from an identifier spelling.
/// @details Semantic symbols are case-folded and may be stored without `%`, `&`, `!`, `#`, or `$`
/// suffixes, while lexer tokens preserve the original spelling.
/// @param text Identifier spelling to normalize in place.
/// @return Identifier without one recognized trailing type suffix.
static std::string stripBasicTypeSuffix(std::string text) {
    if (!text.empty()) {
        const char suffix = text.back();
        if (suffix == '%' || suffix == '&' || suffix == '!' || suffix == '#' || suffix == '$')
            text.pop_back();
    }
    return text;
}

/// @brief Return text safe to embed inside a fenced BASIC hover block.
/// @details BASIC identifiers should not contain markdown delimiters, but this
///          defensive sanitizer keeps unexpected control characters or backticks
///          from breaking the hover document layout.
/// @param text Text destined for a fenced Markdown code block.
/// @return Copy with control bytes, DEL, and backticks replaced by spaces.
static std::string sanitizeBasicHoverCode(std::string text) {
    for (char &c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F || c == '`')
            c = ' ';
    }
    return text;
}

/// @brief Combine authored runtime class documentation for BASIC tooling.
/// @param qualifiedName Fully qualified runtime class name.
/// @return Summary and details separated by a blank line, or empty when unknown.
static std::string runtimeClassDocumentation(std::string_view qualifiedName) {
    const auto *runtimeClass = il::runtime::findRuntimeClassByQName(qualifiedName);
    if (!runtimeClass)
        return {};
    std::string documentation = runtimeClass->summary ? runtimeClass->summary : "";
    if (runtimeClass->details && *runtimeClass->details) {
        if (!documentation.empty())
            documentation += "\n\n";
        documentation += runtimeClass->details;
    }
    return documentation;
}

/// @brief Build a case-folded map from BASIC identifier spellings to source locations.
/// @details The BASIC semantic model exposes symbol names without declaration locations. This
/// helper lexes the source once, prefers identifiers immediately following declaration leaders
/// such as DIM, CONST, SUB, FUNCTION, and CLASS, and then falls back to each identifier's first
/// source position so LSP document symbols always have a concrete range.
/// @param source Complete BASIC source text.
/// @param fileId Source-manager file identifier assigned to the text.
/// @return Case-folded symbol spellings mapped to preferred declaration locations.
static std::unordered_map<std::string, il::support::SourceLoc> indexBasicIdentifierLocations(
    const std::string &source, uint32_t fileId) {
    std::unordered_map<std::string, il::support::SourceLoc> locations;
    std::unordered_map<std::string, il::support::SourceLoc> fallbackLocations;
    Lexer lexer(source, fileId);
    enum class DeclarationMode { None, SingleIdentifier, VariableList };
    DeclarationMode declarationMode = DeclarationMode::None;
    bool skippingTypeClause = false;
    /// @brief Record a BASIC declaration under its literal and suffix-free spellings.
    /// @param tok Identifier token providing spelling and source location.
    auto rememberDeclaration = [&](const Token &tok) {
        const std::string key = toUpperStr(tok.lexeme);
        locations.emplace(key, tok.loc);
        const std::string stripped = toUpperStr(stripBasicTypeSuffix(tok.lexeme));
        if (!stripped.empty())
            locations.emplace(stripped, tok.loc);
    };
    while (true) {
        Token tok = lexer.next();
        if (tok.kind == TokenKind::EndOfFile)
            break;
        if (tok.kind == TokenKind::KeywordDim || tok.kind == TokenKind::KeywordConst) {
            declarationMode = DeclarationMode::VariableList;
            skippingTypeClause = false;
            continue;
        }
        if (tok.kind == TokenKind::KeywordSub || tok.kind == TokenKind::KeywordFunction ||
            tok.kind == TokenKind::KeywordClass) {
            declarationMode = DeclarationMode::SingleIdentifier;
            skippingTypeClause = false;
            continue;
        }
        if (tok.kind == TokenKind::EndOfLine || tok.kind == TokenKind::Colon) {
            declarationMode = DeclarationMode::None;
            skippingTypeClause = false;
            continue;
        }
        if (declarationMode == DeclarationMode::VariableList && tok.kind == TokenKind::Comma) {
            skippingTypeClause = false;
            continue;
        }
        if (declarationMode == DeclarationMode::VariableList && tok.kind == TokenKind::KeywordAs) {
            skippingTypeClause = true;
            continue;
        }
        if (tok.kind != TokenKind::Identifier || tok.lexeme.empty()) {
            continue;
        }

        const std::string key = toUpperStr(tok.lexeme);
        fallbackLocations.emplace(key, tok.loc);

        const std::string stripped = toUpperStr(stripBasicTypeSuffix(tok.lexeme));
        if (!stripped.empty())
            fallbackLocations.emplace(stripped, tok.loc);
        if (declarationMode == DeclarationMode::SingleIdentifier) {
            rememberDeclaration(tok);
            declarationMode = DeclarationMode::None;
        } else if (declarationMode == DeclarationMode::VariableList && !skippingTypeClause) {
            rememberDeclaration(tok);
        }
    }
    for (const auto &[name, loc] : fallbackLocations)
        locations.emplace(name, loc);
    return locations;
}

/// @brief Find the best known source location for a BASIC semantic symbol name.
/// @details Qualified names are resolved by exact uppercase spelling first and by their final
/// component second, matching how class members and nested class names are commonly displayed.
/// @param locations Case-folded location index.
/// @param name Semantic symbol name, possibly qualified or type-suffixed.
/// @return Indexed source location, or a zero-initialized location when absent.
static il::support::SourceLoc findBasicSymbolLocation(
    const std::unordered_map<std::string, il::support::SourceLoc> &locations,
    const std::string &name) {
    const std::string key = toUpperStr(stripBasicTypeSuffix(name));
    if (auto it = locations.find(key); it != locations.end())
        return it->second;

    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < name.size()) {
        const std::string leaf = toUpperStr(stripBasicTypeSuffix(name.substr(dot + 1)));
        if (auto it = locations.find(leaf); it != locations.end())
            return it->second;
    }

    return {};
}

/// @brief Repair an absent BASIC symbol location to the start of the document.
/// @details LSP ranges are one-based in bridge data and cannot use zero line or
///          column values. Falling back to 1:1 is preferable to sending an
///          invalid range when semantic data lacks a precise declaration span.
/// @param loc Candidate semantic or lexer location.
/// @param fileId Source-manager file identifier for the current document.
/// @return @p loc when complete, otherwise the current document's 1:1 position.
static il::support::SourceLoc validBasicSymbolLocation(il::support::SourceLoc loc,
                                                       uint32_t fileId) {
    if (loc.line != 0 && loc.column != 0)
        return loc;
    loc.file_id = fileId;
    loc.line = 1;
    loc.column = 1;
    return loc;
}

/// @brief Append token lexeme text with printable escapes for line-oriented dumps.
/// @details BASIC token dumps are consumed by humans and MCP clients as plain
///          text. Escaping control characters prevents one token's lexeme from
///          spanning multiple output lines or being confused with dump separators.
/// @param out Destination string receiving escaped token text.
/// @param text Raw lexer spelling to append.
static void appendEscapedTokenText(std::string &out, const std::string &text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char c : text) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20u || c == 0x7Fu) {
                    out += "\\x";
                    out.push_back(kHex[c >> 4u]);
                    out.push_back(kHex[c & 0x0Fu]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
}

// --- Constructor / Destructor ---

/// @brief Construct the bridge and its persistent completion cache.
BasicCompilerBridge::BasicCompilerBridge()
    : completionEngine_(std::make_unique<BasicCompletionEngine>()) {}

/// @brief Destroy the completion engine through its complete implementation type.
BasicCompilerBridge::~BasicCompilerBridge() = default;

// --- Analysis ---

/// @brief Parse and semantically analyze BASIC source.
/// @param source Complete source buffer.
/// @param path Virtual or filesystem path used in source locations.
/// @return Owned normalized diagnostics, including a bridge error if analysis
///         unexpectedly produces no result.
std::vector<DiagnosticInfo> BasicCompilerBridge::check(const std::string &source,
                                                       const std::string &path) {
    il::support::SourceManager sm;
    BasicCompilerInput input{.source = source, .path = path};
    auto result = parseAndAnalyzeBasic(input, sm);
    if (!result) {
        DiagnosticInfo info;
        info.severity = 2;
        info.message = "internal error: BASIC analysis did not produce a result";
        info.file = path;
        info.code = "V-LSP-ANALYSIS";
        return {info};
    }
    return extractDiagnostics(result->diagnostics, &sm);
}

/// @brief Compile BASIC source through the normal frontend pipeline.
/// @param source Complete source buffer.
/// @param path Virtual or filesystem path used in diagnostics.
/// @return Success flag and owned normalized diagnostics.
CompileResult BasicCompilerBridge::compile(const std::string &source, const std::string &path) {
    il::support::SourceManager sm;
    BasicCompilerInput input{.source = source, .path = path};
    BasicCompilerOptions opts{};

    auto result = compileBasic(input, opts, sm);
    return {result.succeeded(), extractDiagnostics(result.diagnostics, &sm)};
}

// --- IDE Features ---

/// @brief Query cached BASIC completions at a compiler-coordinate position.
/// @param source Complete source buffer.
/// @param line One-based cursor line.
/// @param col One-based cursor byte column.
/// @param path Virtual or filesystem path used by the completion engine.
/// @return Owned protocol-neutral completion items.
std::vector<CompletionInfo> BasicCompilerBridge::completions(const std::string &source,
                                                             int line,
                                                             int col,
                                                             const std::string &path) {
    std::lock_guard<std::mutex> lock(completionMutex_);
    auto items = completionEngine_->complete(source, line, col, path);
    std::vector<CompletionInfo> result;
    result.reserve(items.size());
    for (const auto &item : items) {
        result.push_back({item.label,
                          item.insertText,
                          static_cast<int>(item.kind),
                          item.detail,
                          item.sortPriority,
                          item.documentation});
    }
    return result;
}

/// @brief Build Markdown hover information for a BASIC symbol.
/// @param source Complete source buffer.
/// @param line One-based cursor line.
/// @param col One-based cursor byte column.
/// @param path Virtual or filesystem path used for analysis.
/// @return Fenced declaration and optional runtime documentation, or empty text
///         when no variable, procedure, or class resolves.
std::string BasicCompilerBridge::hover(const std::string &source,
                                       int line,
                                       int col,
                                       const std::string &path) {
    auto ctx = extractIdentifierAtCursor(source, line, col);
    if (!ctx.valid)
        return "";

    // BASIC lexer uppercases all identifiers, so normalize for sema lookup.
    std::string ident = toUpperStr(ctx.identifier);

    il::support::SourceManager sm;
    BasicCompilerInput input{.source = source, .path = path};
    auto result = parseAndAnalyzeBasic(input, sm);
    if (!result || !result->sema)
        return "";

    const auto &sema = *result->sema;

    // Try looking up as a variable
    auto varType = sema.lookupVarType(ident);
    if (varType) {
        std::string typeStr;
        std::string documentation;
        switch (*varType) {
            case SemanticAnalyzer::Type::Int:
                typeStr = "INTEGER";
                break;
            case SemanticAnalyzer::Type::Float:
                typeStr = "DOUBLE";
                break;
            case SemanticAnalyzer::Type::String:
                typeStr = "STRING";
                break;
            case SemanticAnalyzer::Type::Bool:
                typeStr = "BOOLEAN";
                break;
            case SemanticAnalyzer::Type::ArrayInt:
                typeStr = "INTEGER()";
                break;
            case SemanticAnalyzer::Type::ArrayString:
                typeStr = "STRING()";
                break;
            case SemanticAnalyzer::Type::ArrayObject:
                typeStr = "OBJECT()";
                break;
            case SemanticAnalyzer::Type::Object: {
                auto cls = sema.lookupObjectClassQName(ident);
                typeStr = cls.value_or("Object");
                documentation = runtimeClassDocumentation(typeStr);
                break;
            }
            case SemanticAnalyzer::Type::Unknown:
                typeStr = "Variant";
                break;
        }

        std::string md = "```basic\n";
        if (sema.isConstSymbol(ident))
            md += "CONST ";
        else
            md += "DIM ";
        md += sanitizeBasicHoverCode(ident) + " AS " + sanitizeBasicHoverCode(typeStr) + "\n```";
        if (!documentation.empty())
            md += "\n\n" + documentation;
        return md;
    }

    // Try looking up as a procedure
    const auto &procTable = sema.procs();
    auto it = procTable.find(ident);
    if (it != procTable.end()) {
        const auto &sig = it->second;
        std::string md = "```basic\n";
        if (sig.kind == ProcSignature::Kind::Function)
            md += "FUNCTION ";
        else
            md += "SUB ";
        md += sanitizeBasicHoverCode(ident);
        md += "(";
        for (size_t i = 0; i < sig.params.size(); ++i) {
            if (i > 0)
                md += ", ";
            switch (sig.params[i].type) {
                case Type::I64:
                    md += "INTEGER";
                    break;
                case Type::F64:
                    md += "DOUBLE";
                    break;
                case Type::Str:
                    md += "STRING";
                    break;
                case Type::Bool:
                    md += "BOOLEAN";
                    break;
                default:
                    md += "?";
                    break;
            }
            if (sig.params[i].is_array)
                md += "()";
        }
        md += ")";
        if (sig.kind == ProcSignature::Kind::Function && sig.retType) {
            md += " AS ";
            switch (*sig.retType) {
                case Type::I64:
                    md += "INTEGER";
                    break;
                case Type::F64:
                    md += "DOUBLE";
                    break;
                case Type::Str:
                    md += "STRING";
                    break;
                case Type::Bool:
                    md += "BOOLEAN";
                    break;
                default:
                    md += "?";
                    break;
            }
        }
        md += "\n```";
        return md;
    }

    // Try looking up as a class
    const auto *classInfo = sema.oopIndex().findClass(ident);
    if (classInfo) {
        std::string md =
            "```basic\nCLASS " + sanitizeBasicHoverCode(classInfo->qualifiedName) + "\n```";
        md += "\n\n*" + std::to_string(classInfo->fields.size()) + " fields, " +
              std::to_string(classInfo->methods.size()) + " methods*";
        if (std::string documentation = runtimeClassDocumentation(classInfo->qualifiedName);
            !documentation.empty())
            md += "\n\n" + documentation;
        return md;
    }

    return "";
}

/// @brief Enumerate variables, procedures, and classes from BASIC semantic analysis.
/// @param source Complete source buffer.
/// @param path Virtual or filesystem path attached to returned locations.
/// @return Owned symbols with lexer-derived declaration coordinates when available.
std::vector<SymbolInfo> BasicCompilerBridge::symbols(const std::string &source,
                                                     const std::string &path) {
    il::support::SourceManager sm;
    const uint32_t fileId = sm.addFile(path);
    const auto symbolLocations = indexBasicIdentifierLocations(source, fileId);
    BasicCompilerInput input{.source = source, .path = path};
    auto result = parseAndAnalyzeBasic(input, sm);
    if (!result || !result->sema)
        return {};

    const auto &sema = *result->sema;
    std::vector<SymbolInfo> out;

    // Variables
    for (const auto &sym : sema.symbols()) {
        std::string typeStr = "unknown";
        auto ty = sema.lookupVarType(sym);
        if (ty) {
            switch (*ty) {
                case SemanticAnalyzer::Type::Int:
                    typeStr = "INTEGER";
                    break;
                case SemanticAnalyzer::Type::Float:
                    typeStr = "DOUBLE";
                    break;
                case SemanticAnalyzer::Type::String:
                    typeStr = "STRING";
                    break;
                case SemanticAnalyzer::Type::Bool:
                    typeStr = "BOOLEAN";
                    break;
                case SemanticAnalyzer::Type::ArrayInt:
                    typeStr = "INTEGER()";
                    break;
                case SemanticAnalyzer::Type::ArrayString:
                    typeStr = "STRING()";
                    break;
                case SemanticAnalyzer::Type::ArrayObject:
                    typeStr = "OBJECT()";
                    break;
                case SemanticAnalyzer::Type::Object: {
                    auto cls = sema.lookupObjectClassQName(sym);
                    typeStr = cls.value_or("Object");
                    break;
                }
                case SemanticAnalyzer::Type::Unknown:
                    typeStr = "Variant";
                    break;
            }
        }
        const auto loc =
            validBasicSymbolLocation(findBasicSymbolLocation(symbolLocations, sym), fileId);
        out.push_back(
            {sym, "variable", typeStr, sema.isConstSymbol(sym), false, loc.line, loc.column, path});
    }

    // Procedures
    for (const auto &[name, sig] : sema.procs()) {
        std::string kind = sig.kind == ProcSignature::Kind::Function ? "function" : "method";
        const auto loc =
            validBasicSymbolLocation(findBasicSymbolLocation(symbolLocations, name), fileId);
        out.push_back({name, kind, "", false, false, loc.line, loc.column, path});
    }

    // Classes
    for (const auto &[name, info] : sema.oopIndex().classes()) {
        const auto loc =
            validBasicSymbolLocation(findBasicSymbolLocation(symbolLocations, name), fileId);
        out.push_back({name, "type", info.qualifiedName, false, false, loc.line, loc.column, path});
    }

    return out;
}

// --- Dump ---

/// @brief Compile BASIC source and serialize its IL module.
/// @param source Complete source buffer.
/// @param path Virtual or filesystem path used during compilation.
/// @param optimized Whether to run the verified O1 IL pipeline before serialization.
/// @return Serialized IL, or human-readable compilation/optimization failure text.
std::string BasicCompilerBridge::dumpIL(const std::string &source,
                                        const std::string &path,
                                        bool optimized) {
    il::support::SourceManager sm;
    BasicCompilerInput input{.source = source, .path = path};
    BasicCompilerOptions opts{};

    auto result = compileBasic(input, opts, sm);
    if (!result.succeeded()) {
        std::string err = "Compilation failed:\n";
        for (const auto &d : result.diagnostics.diagnostics())
            err += "  " + d.message + "\n";
        return err;
    }

    if (optimized) {
        il::transform::PassManager pm;
        if (!pm.runPipeline(result.module, "O1"))
            return "Optimization failed: IL O1 pipeline failed verification\n";
    }
    return il::io::Serializer::toString(result.module);
}

/// @brief Parse BASIC source and dump its abstract syntax tree.
/// @param source Complete source buffer.
/// @param path Virtual or filesystem path used during parsing.
/// @return Printer output, or a no-AST marker when parsing produces no tree.
std::string BasicCompilerBridge::dumpAst(const std::string &source, const std::string &path) {
    il::support::SourceManager sm;
    BasicCompilerInput input{.source = source, .path = path};
    auto result = parseAndAnalyzeBasic(input, sm);
    if (!result || !result->ast)
        return "(no AST produced)";

    AstPrinter printer;
    return printer.dump(*result->ast);
}

/// @brief Lex BASIC source into a stable line-oriented token dump.
/// @param source Complete source buffer.
/// @param path Virtual or filesystem path used to allocate the file ID.
/// @return One line per non-EOF token with location, kind, and escaped spelling.
std::string BasicCompilerBridge::dumpTokens(const std::string &source, const std::string &path) {
    il::support::SourceManager sm;
    uint32_t fileId = sm.addFile(path);
    Lexer lexer(source, fileId);

    std::string out;
    while (true) {
        Token tok = lexer.next();
        if (tok.kind == TokenKind::EndOfFile)
            break;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u:%u", tok.loc.line, tok.loc.column);
        out += buf;
        out += '\t';
        out += tokenKindToString(tok.kind);
        if (!tok.lexeme.empty()) {
            out += '\t';
            appendEscapedTokenText(out, tok.lexeme);
        }
        out += '\n';
    }
    return out;
}

} // namespace zanna::server
