//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file SemanticAnalyzer.cpp
/// @brief Implements core BASIC semantic-analyzer state and shared utilities.
///
/// @details This file contains the main implementation of the BASIC semantic
/// analyzer, including construction, read-only query surfaces, symbol
/// resolution/default typing, and utility functions shared by the split
/// semantic-analysis translation units. Statement, expression, procedure, and
/// OOP passes are implemented in neighboring files.
///
/// ## Semantic Analysis Overview
///
/// The BASIC semantic analyzer performs several critical validation phases:
///
/// ### Phase 1: Name Resolution
///
/// - Resolves variable and function names to their declarations
/// - Validates namespace and USING directive visibility
/// - Checks for undefined identifiers
///
/// ### Phase 2: Type Checking
///
/// - Validates expression types and operator compatibility
/// - Checks assignment compatibility (type coercion rules)
/// - Validates function call argument types
///
/// ### Phase 3: Control Flow Analysis
///
/// - Ensures all code paths return a value (for functions)
/// - Validates GOTO/GOSUB targets
/// - Checks loop and conditional structure
///
/// ### Phase 4: OOP Validation
///
/// - Validates class inheritance chains
/// - Checks interface implementation completeness
/// - Validates property get/set consistency
///
/// ## Symbol Table Management
///
/// The analyzer maintains several symbol tables:
/// - **symbols_**: Global set of all declared symbols
/// - **varTypes_**: Inferred types keyed by resolved symbol name
/// - **procReg_**: Procedure (SUB/FUNCTION) registry
///
/// ## Error Reporting
///
/// All semantic errors are reported through the DiagnosticEmitter interface,
/// providing source locations for precise error messages.
///
/// ## Usage
///
/// ```cpp
/// DiagnosticEmitter emitter;
/// SemanticAnalyzer analyzer(emitter);
/// analyzer.analyze(program);
/// // Check emitter for any errors
/// ```
///
/// @invariant The diagnostic adapter and procedure registry remain associated
///            with the emitter supplied at construction.
/// @invariant Query accessors expose analyzer-owned state and do not transfer
///            ownership.
/// @note Semantic analysis may resolve names and record conversion/type
///       information on or alongside mutable AST nodes.
///
/// @see SemanticAnalyzer.hpp - Public interface
/// @see SemanticAnalyzer_Internal.hpp - Internal helpers
/// @see DiagnosticEmitter - Error reporting interface
///

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/SemanticAnalyzer_Internal.hpp"

#include "frontends/basic/BasicTypes.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

#include "frontends/basic/BuiltinRegistry.hpp"

namespace il::frontends::basic {

/// @brief Constructs an analyzer bound to a diagnostic emitter.
/// @details Initializes both the semantic-diagnostic adapter and procedure
///          registry from @p emitter. Analysis state remains empty until
///          @ref SemanticAnalyzer::analyze is called.
/// @param emitter Diagnostic sink borrowed for the analyzer's lifetime.
SemanticAnalyzer::SemanticAnalyzer(DiagnosticEmitter &emitter) : de(emitter), procReg_(de) {}

/// @brief Returns all symbol names currently tracked by the analyzer.
/// @return Const reference to analyzer-owned storage, valid until this analyzer
///         mutates or is destroyed.
const std::unordered_set<std::string> &SemanticAnalyzer::symbols() const {
    return symbols_;
}

/// @brief Returns numeric labels declared during analysis.
/// @return Const reference to analyzer-owned label storage.
const std::unordered_set<int> &SemanticAnalyzer::labels() const {
    return labels_;
}

/// @brief Returns numeric labels referenced by control-flow statements.
/// @return Const reference to analyzer-owned reference storage. Entries may
///         include targets that were subsequently diagnosed as undefined.
const std::unordered_set<int> &SemanticAnalyzer::labelRefs() const {
    return labelRefs_;
}

/// @brief Returns the current procedure-signature table.
/// @return Const reference owned by the analyzer's @ref ProcRegistry.
const ProcTable &SemanticAnalyzer::procs() const {
    return procReg_.procs();
}

/// @brief Reports the class that supplies the active implicit instance.
/// @details A qualified class name is returned only while analysis is inside a
///          member that has an implicit `ME` receiver and the active class name
///          is non-empty.
/// @return Active qualified class name by value, or @c std::nullopt when no
///         implicit instance receiver is available.
std::optional<std::string> SemanticAnalyzer::activeInstanceClassQName() const {
    if (activeMemberHasMe_ && !activeClassQName_.empty())
        return activeClassQName_;
    return std::nullopt;
}

/// @brief Copies imports from the innermost active USING scope.
/// @details Reads the last @ref usingStack_ entry and copies its canonical
///          namespace strings. Because the source is an unordered set, result
///          order is unspecified. Aliases are not included.
/// @return Imported namespace paths, or an empty vector when no USING scope is
///         active.
std::vector<std::string> SemanticAnalyzer::getUsingImports() const {
    std::vector<std::string> result;
    if (!usingStack_.empty()) {
        const UsingScope &scope = usingStack_.back();
        result.reserve(scope.imports.size());
        for (const auto &ns : scope.imports) {
            result.push_back(ns);
        }
    }
    return result;
}

/// @brief Looks up array metadata by its exact stored key.
/// @param name Resolved array identifier; this function does not canonicalize it.
/// @return Pointer into analyzer-owned map storage, or @c nullptr when absent.
/// @warning The pointer may be invalidated by later mutation of @ref arrays_.
const ArrayMetadata *SemanticAnalyzer::lookupArrayMetadata(const std::string &name) const {
    if (auto it = arrays_.find(name); it != arrays_.end())
        return &it->second;
    return nullptr;
}

/// @brief Looks up the inferred type of a variable.
/// @details Tries @p name exactly first, then retries with
///          @ref CanonicalizeIdent when canonicalization produces a non-empty
///          name. The lookup does not resolve lexical aliases.
/// @param name Variable spelling to query.
/// @return Recorded semantic type, or @c std::nullopt when neither key exists.
std::optional<SemanticAnalyzer::Type> SemanticAnalyzer::lookupVarType(
    const std::string &name) const {
    if (auto it = varTypes_.find(name); it != varTypes_.end())
        return it->second;
    std::string canon = CanonicalizeIdent(name);
    if (!canon.empty()) {
        if (auto it = varTypes_.find(canon); it != varTypes_.end())
            return it->second;
    }
    return std::nullopt;
}

/// @brief Looks up the qualified class associated with an object variable.
/// @details Tries the supplied spelling and then its canonical identifier. The
///          stored qualified class string is copied into the return value.
/// @param name Variable spelling to query.
/// @return Qualified class name, or @c std::nullopt when no mapping exists.
std::optional<std::string> SemanticAnalyzer::lookupObjectClassQName(const std::string &name) const {
    if (auto it = objectClassTypes_.find(name); it != objectClassTypes_.end())
        return it->second;
    std::string canon = CanonicalizeIdent(name);
    if (!canon.empty()) {
        if (auto it = objectClassTypes_.find(canon); it != objectClassTypes_.end())
            return it->second;
    }
    return std::nullopt;
}

/// @brief Tests whether a name appears in the analyzer's symbol set.
/// @details Checks the exact spelling first and then a non-empty canonicalized
///          spelling. The method relies on procedure-scope cleanup having
///          removed locals before clients use this as a module-level query.
/// @param name Symbol spelling to test.
/// @return @c true when either spelling is present.
bool SemanticAnalyzer::isModuleLevelSymbol(const std::string &name) const {
    if (symbols_.contains(name))
        return true;
    std::string canon = CanonicalizeIdent(name);
    return !canon.empty() && symbols_.contains(canon);
}

/// @brief Tests whether a name is recorded as a module-level constant.
/// @details Checks exact and canonicalized spellings. Lowering uses this result
///          to retain module-storage semantics even when local variable rules
///          would otherwise apply.
/// @param name Symbol spelling to test.
/// @return @c true when either spelling occurs in @ref constants_.
bool SemanticAnalyzer::isConstSymbol(const std::string &name) const {
    if (constants_.contains(name))
        return true;
    std::string canon = CanonicalizeIdent(name);
    return !canon.empty() && constants_.contains(canon);
}

/// @brief Resolves a scoped alias and records definition/default-type state.
/// @details Replaces @p name when @ref ScopeTracker resolves it. References
///          stop after resolution and do not alter symbol/type tables. Both
///          definition kinds insert the resolved name into @ref symbols_; a
///          procedure scope records newly inserted names for later rollback.
///          If no type exists, `$` defaults to string, `#` and `!` to float,
///          and every other spelling to integer. Existing explicit types are
///          never overwritten.
/// @param name Identifier updated in place when a scoped mapping exists.
/// @param kind Reference or definition behavior requested by the caller.
/// @post For non-reference kinds, @p name has a tracked type unless one already
///       existed; mutations made inside a procedure are registered with its
///       rollback scope.
void SemanticAnalyzer::resolveAndTrackSymbol(std::string &name, SymbolKind kind) {
    if (auto mapped = scopes_.resolve(name)) {
        name = *mapped;
    } else if (scopes_.hasScope()) {
        if (symbols_.contains(name)) {
            // Name exists at module level and is not shadowed by a local.
            // Keep the original name - it will resolve to the module-level variable.
            // No action needed, name stays unchanged.
        }
    }

    if (kind == SymbolKind::Reference)
        return;

    auto insertResult = symbols_.insert(name);
    if (insertResult.second && activeProcScope_)
        activeProcScope_->noteSymbolInserted(name);

    // BUG-093 fix: Do not override explicitly declared types (from DIM) when processing
    // INPUT statements. Only set default type if the variable has no type yet.
    auto itType = varTypes_.find(name);
    if (itType == varTypes_.end()) {
        Type defaultType = Type::Int;
        if (!name.empty()) {
            if (name.back() == '$')
                defaultType = Type::String;
            else if (name.back() == '#' || name.back() == '!')
                defaultType = Type::Float;
        }
        if (activeProcScope_) {
            std::optional<Type> previous;
            activeProcScope_->noteVarTypeMutation(name, previous);
        }
        varTypes_[name] = defaultType;
    }
}

} // namespace il::frontends::basic

namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief Computes case-sensitive Levenshtein edit distance.
/// @details Uses two rows of dynamic-programming state, requiring O(|@p b|)
///          auxiliary memory and O(|@p a|*|@p b|) time. Insertions, deletions,
///          and substitutions each cost one.
/// @param a Source byte string.
/// @param b Destination byte string.
/// @return Minimum number of single-byte edits needed to transform @p a into
///         @p b.
size_t levenshtein(const std::string &a, const std::string &b) {
    const size_t m = a.size();
    const size_t n = b.size();
    std::vector<size_t> prev(n + 1), cur(n + 1);
    for (size_t j = 0; j <= n; ++j)
        prev[j] = j;
    for (size_t i = 1; i <= m; ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[n];
}

/// @brief Maps the compact AST value type to semantic-analysis type space.
/// @param ty AST type to translate.
/// @return Integer, float, string, or boolean as appropriate. A defensive
///         fall-through returns integer.
SemanticAnalyzer::Type astToSemanticType(::il::frontends::basic::Type ty) {
    switch (ty) {
        case ::il::frontends::basic::Type::I64:
            return SemanticAnalyzer::Type::Int;
        case ::il::frontends::basic::Type::F64:
            return SemanticAnalyzer::Type::Float;
        case ::il::frontends::basic::Type::Str:
            return SemanticAnalyzer::Type::String;
        case ::il::frontends::basic::Type::Bool:
            return SemanticAnalyzer::Type::Bool;
    }
    return SemanticAnalyzer::Type::Int;
}

/// @brief Retrieves a builtin's registry name.
/// @param b Builtin identifier accepted by @ref getBuiltinInfo.
/// @return Non-owning pointer to the registry entry's null-terminated name.
const char *builtinName(BuiltinCallExpr::Builtin b) {
    return getBuiltinInfo(b).name;
}

/// @brief Maps a semantic type to diagnostic text.
/// @param type Type category to render.
/// @return Static null-terminated uppercase text. Array categories include
///         their element category in parentheses; unknown and defensive
///         fall-through both return `"UNKNOWN"`.
const char *semanticTypeName(SemanticAnalyzer::Type type) {
    using Type = SemanticAnalyzer::Type;
    switch (type) {
        case Type::Int:
            return "INT";
        case Type::Float:
            return "FLOAT";
        case Type::String:
            return "STRING";
        case Type::Bool:
            return "BOOLEAN";
        case Type::ArrayInt:
            return "ARRAY(INT)";
        case Type::ArrayString:
            return "ARRAY(STRING)";
        case Type::ArrayObject:
            return "ARRAY(OBJECT)";
        case Type::Object:
            return "OBJECT";
        case Type::Unknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

/// @brief Maps a binary logical operator to its BASIC keyword.
/// @param op Operator to inspect.
/// @return Static keyword text for short-circuit and eager AND/OR operations,
///         or `"<logical>"` for every other operator.
const char *logicalOpName(BinaryExpr::Op op) {
    switch (op) {
        case BinaryExpr::Op::LogicalAndShort:
            return "ANDALSO";
        case BinaryExpr::Op::LogicalOrShort:
            return "ORELSE";
        case BinaryExpr::Op::LogicalAnd:
            return "AND";
        case BinaryExpr::Op::LogicalOr:
            return "OR";
        default:
            break;
    }
    return "<logical>";
}

/// @brief Renders simple condition expressions for diagnostics.
/// @details Recognizes variables and integer, floating, boolean, and string
///          literals. String text is enclosed in quotes without additional
///          escaping; floating values use stream formatting.
/// @param expr Expression to inspect without mutation.
/// @return Rendered source-like text for a recognized node, otherwise an empty
///         string.
std::string conditionExprText(const Expr &expr) {
    if (auto *var = as<const VarExpr>(expr))
        return var->name;
    if (auto *intExpr = as<const IntExpr>(expr))
        return std::to_string(intExpr->value);
    if (auto *floatExpr = as<const FloatExpr>(expr)) {
        std::ostringstream oss;
        oss << floatExpr->value;
        return oss.str();
    }
    if (auto *boolExpr = as<const BoolExpr>(expr))
        return boolExpr->value ? "TRUE" : "FALSE";
    if (auto *strExpr = as<const StringExpr>(expr)) {
        std::string text = "\"";
        text += strExpr->value;
        text += '"';
        return text;
    }
    return {};
}

/// @brief Decodes the supported BASIC type suffix on a name.
/// @param name Name whose final byte is inspected.
/// @return String for `$`, float for `#`, integer for `%`, or
///         @c std::nullopt for an empty name or any other suffix.
std::optional<BasicType> suffixBasicType(std::string_view name) {
    if (name.empty())
        return std::nullopt;
    switch (name.back()) {
        case '$':
            return BasicType::String;
        case '#':
            return BasicType::Float;
        case '%':
            return BasicType::Int;
        default:
            break;
    }
    return std::nullopt;
}

/// @brief Maps an explicit BASIC scalar type into semantic type space.
/// @param type BASIC type to translate.
/// @return Integer, float, or string for those three scalar types;
///         @c std::nullopt for all other values.
std::optional<SemanticAnalyzer::Type> semanticTypeFromBasic(BasicType type) {
    using Type = SemanticAnalyzer::Type;
    switch (type) {
        case BasicType::Int:
            return Type::Int;
        case BasicType::Float:
            return Type::Float;
        case BasicType::String:
            return Type::String;
        default:
            break;
    }
    return std::nullopt;
}

/// @brief Formats a BASIC type name in uppercase for diagnostics.
/// @details Obtains the canonical text from @ref toString and applies
///          @c std::toupper to each unsigned byte.
/// @param type BASIC type to format.
/// @return Uppercase copy of the type text.
std::string uppercaseBasicTypeName(BasicType type) {
    std::string text{toString(type)};
    /// Converts one unsigned input byte to its uppercase character value.
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return text;
}

/// @brief Tests whether semantic rules treat a type as numeric.
/// @param type Type category to classify.
/// @return @c true for integer, float, or boolean; @c false for strings,
///         arrays, objects, and unknown values.
bool isNumericSemanticType(SemanticAnalyzer::Type type) noexcept {
    using Type = SemanticAnalyzer::Type;
    return type == Type::Int || type == Type::Float || type == Type::Bool;
}

} // namespace il::frontends::basic::semantic_analyzer_detail
