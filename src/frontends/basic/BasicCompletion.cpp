//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/BasicCompletion.cpp
// Purpose: Implementation of the BASIC code-completion engine.
// Key invariants:
//   - Keywords and builtins are static provider lists
//   - Scope symbols come from SemanticAnalyzer query APIs
//   - Member completions use OopIndex for class fields/methods
//   - Runtime members use RuntimeClasses registry
// Ownership/Lifetime:
//   - All returned items are fully owned
// Links: src/frontends/basic/BasicCompletion.hpp,
//        src/frontends/basic/BasicAnalysis.hpp,
//        src/frontends/basic/OopIndex.hpp,
//        src/il/runtime/classes/RuntimeClasses.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file BasicCompletion.cpp
 * @brief Implements cached BASIC completion providers and deterministic ranking.
 */

#include "frontends/basic/BasicCompletion.hpp"

#include "frontends/basic/OopIndex.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace il::frontends::basic {

// --- Constructor / Destructor ---

/// @brief Initialize an empty engine with a fresh source manager.
BasicCompletionEngine::BasicCompletionEngine()
    : sm_(std::make_unique<il::support::SourceManager>()) {}

/// @brief Destroy the current cached analysis and owned source manager.
BasicCompletionEngine::~BasicCompletionEngine() = default;

// --- Cache ---

/// @copydoc BasicCompletionEngine::fnv1a()
uint64_t BasicCompletionEngine::fnv1a(std::string_view data) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : data) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/// @copydoc BasicCompletionEngine::clearCache()
void BasicCompletionEngine::clearCache() {
    cache_ = {};
}

// --- Context extraction ---

/// @copydoc BasicCompletionEngine::extractContext()
BasicCompletionEngine::Context BasicCompletionEngine::extractContext(std::string_view src,
                                                                     int line,
                                                                     int col) const {
    Context ctx;

    // Find the line
    size_t lineStart = 0;
    int curLine = 1;
    for (size_t i = 0; i < src.size() && curLine < line; ++i) {
        if (src[i] == '\n') {
            ++curLine;
            lineStart = i + 1;
        }
    }

    size_t lineEnd = lineStart;
    while (lineEnd < src.size() && src[lineEnd] != '\n')
        ++lineEnd;

    // col is 1-based; clamp invalid client input to the start of the line.
    size_t cursorOff = lineStart + (col <= 1 ? 0U : static_cast<size_t>(col - 1));
    if (cursorOff > lineEnd)
        cursorOff = lineEnd;

    // Scan backwards for identifier chars (prefix)
    size_t prefEnd = cursorOff;
    size_t prefStart = prefEnd;
    while (prefStart > lineStart) {
        char c = src[prefStart - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '%' ||
            c == '!' || c == '#' || c == '&')
            --prefStart;
        else
            break;
    }

    ctx.prefix = std::string(src.substr(prefStart, prefEnd - prefStart));

    // Check for dot trigger
    if (prefStart > lineStart && src[prefStart - 1] == '.') {
        ctx.trigger = TriggerKind::MemberAccess;
        size_t dotPos = prefStart - 1;

        // Walk backwards through identifiers and dots for the trigger expression
        size_t exprStart = dotPos;
        while (exprStart > lineStart) {
            size_t p = exprStart - 1;
            char c = src[p];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
                while (p > lineStart && (std::isalnum(static_cast<unsigned char>(src[p - 1])) ||
                                         src[p - 1] == '_' || src[p - 1] == '$'))
                    --p;
                exprStart = p;
                if (p > lineStart && src[p - 1] == '.')
                    exprStart = p - 1;
                else
                    break;
            } else {
                break;
            }
        }

        ctx.triggerExpr = std::string(src.substr(exprStart, dotPos - exprStart));
    }

    return ctx;
}

// --- Helpers ---

/// @brief Lower-case a string for case-insensitive label/prefix matching.
/// @param s Byte string to fold using the C locale's character classification.
/// @return Copy with every byte converted through `std::tolower`.
static std::string toLowerStr(const std::string &s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

/// @brief Test whether @p label begins with @p prefix, case-insensitively.
/// @details An empty prefix matches everything. The match is anchored at
///          position 0 (true prefix), not a free substring search.
/// @param label Candidate completion label.
/// @param prefix Requested case-insensitive prefix.
/// @return True when @p prefix is empty or @p label begins with @p prefix after
///         case folding.
static bool prefixMatch(const std::string &label, const std::string &prefix) {
    if (prefix.empty())
        return true;
    std::string lLabel = toLowerStr(label);
    std::string lPrefix = toLowerStr(prefix);
    return lLabel.find(lPrefix) == 0;
}

/// @brief Combine authored documentation for a runtime class.
/// @param runtimeClass Registry record whose summary and details are read.
/// @return Summary, details, or both separated by a blank line; empty when
///         neither field contains text.
static std::string runtimeClassDocumentation(const il::runtime::RuntimeClass &runtimeClass) {
    std::string documentation = runtimeClass.summary ? runtimeClass.summary : "";
    if (runtimeClass.details && *runtimeClass.details) {
        if (!documentation.empty())
            documentation += "\n\n";
        documentation += runtimeClass.details;
    }
    return documentation;
}

// --- Providers ---

/// @copydoc BasicCompletionEngine::provideKeywords()
std::vector<CompletionItem> BasicCompletionEngine::provideKeywords(
    const std::string &prefix) const {
    static const char *keywords[] = {
        "AND",    "AS",        "BOOLEAN", "CALL",      "CASE",      "CLASS",    "CLOSE",
        "CONST",  "DECLARE",   "DIM",     "DO",        "DOUBLE",    "EACH",     "ELSE",
        "ELSEIF", "END",       "ENUM",    "ERASE",     "ERROR",     "EXIT",     "FALSE",
        "FOR",    "FUNCTION",  "GET",     "GOSUB",     "GOTO",      "IF",       "IMPLEMENTS",
        "IN",     "INPUT",     "INTEGER", "INHERITS",  "INTERFACE", "LET",      "LINE",
        "LONG",   "LOOP",      "MOD",     "NAMESPACE", "NEW",       "NEXT",     "NOT",
        "ON",     "OPEN",      "OR",      "PRINT",     "PRIVATE",   "PROPERTY", "PUBLIC",
        "PUT",    "RANDOMIZE", "REDIM",   "RESUME",    "RETURN",    "SELECT",   "SHARED",
        "SINGLE", "STATIC",    "STEP",    "STRING",    "SUB",       "SWAP",     "THEN",
        "TO",     "TRUE",      "TYPE",    "UNTIL",     "USING",     "WEND",     "WHILE",
        "XOR",
    };

    std::vector<CompletionItem> items;
    for (const char *kw : keywords) {
        if (prefixMatch(kw, prefix)) {
            items.push_back({kw, kw, CompletionKind::Keyword, "keyword", 200});
        }
    }
    return items;
}

/// @copydoc BasicCompletionEngine::provideSnippets()
std::vector<CompletionItem> BasicCompletionEngine::provideSnippets(
    const std::string &prefix) const {
    std::vector<CompletionItem> items;

    /// Static snippet metadata copied into completion items when selected.
    struct Snippet {
        ///< Popup label used for matching.
        const char *label;
        ///< Editor snippet text with placeholder markers.
        const char *insert;
        ///< Concise construct description.
        const char *detail;
    };

    static const Snippet snippets[] = {
        {"FOR...NEXT", "FOR ${1:i} = ${2:1} TO ${3:10}\n    ${0}\nNEXT ${1:i}", "FOR loop"},
        {"IF...END IF", "IF ${1:condition} THEN\n    ${0}\nEND IF", "IF block"},
        {"IF...ELSE...END IF",
         "IF ${1:condition} THEN\n    ${2}\nELSE\n    ${0}\nEND IF",
         "IF/ELSE block"},
        {"DO...LOOP", "DO\n    ${0}\nLOOP", "DO loop"},
        {"DO WHILE...LOOP", "DO WHILE ${1:condition}\n    ${0}\nLOOP", "DO WHILE loop"},
        {"WHILE...WEND", "WHILE ${1:condition}\n    ${0}\nWEND", "WHILE loop"},
        {"SELECT CASE",
         "SELECT CASE ${1:expr}\n    CASE ${2:value}\n        ${0}\nEND SELECT",
         "SELECT CASE block"},
        {"SUB...END SUB", "SUB ${1:Name}()\n    ${0}\nEND SUB", "SUB procedure"},
        {"FUNCTION...END FUNCTION",
         "FUNCTION ${1:Name}() AS ${2:INTEGER}\n    ${0}\nEND FUNCTION",
         "FUNCTION"},
        {"CLASS...END CLASS", "CLASS ${1:Name}\n    ${0}\nEND CLASS", "CLASS definition"},
    };

    for (const auto &s : snippets) {
        if (prefixMatch(s.label, prefix)) {
            items.push_back({s.label, s.insert, CompletionKind::Snippet, s.detail, 150});
        }
    }
    return items;
}

/// @copydoc BasicCompletionEngine::provideBuiltins()
std::vector<CompletionItem> BasicCompletionEngine::provideBuiltins(
    const std::string &prefix) const {
    /// Name and signature metadata for one intrinsic completion.
    static const struct {
        ///< BASIC intrinsic spelling.
        const char *name;
        ///< Human-readable invocation and result signature.
        const char *detail;
    } builtins[] = {
        {"ABS", "ABS(n) -> number"},
        {"ASC", "ASC(s$) -> INTEGER"},
        {"ATN", "ATN(n) -> DOUBLE"},
        {"CHR$", "CHR$(n) -> STRING"},
        {"COS", "COS(n) -> DOUBLE"},
        {"EXP", "EXP(n) -> DOUBLE"},
        {"FIX", "FIX(n) -> INTEGER"},
        {"HEX$", "HEX$(n) -> STRING"},
        {"INSTR", "INSTR([start,] haystack$, needle$) -> INTEGER"},
        {"INT", "INT(n) -> INTEGER"},
        {"LBOUND", "LBOUND(arr) -> INTEGER"},
        {"LCASE$", "LCASE$(s$) -> STRING"},
        {"LEFT$", "LEFT$(s$, n) -> STRING"},
        {"LEN", "LEN(s$) -> INTEGER"},
        {"LOG", "LOG(n) -> DOUBLE"},
        {"LTRIM$", "LTRIM$(s$) -> STRING"},
        {"MID$", "MID$(s$, start[, len]) -> STRING"},
        {"OCT$", "OCT$(n) -> STRING"},
        {"RIGHT$", "RIGHT$(s$, n) -> STRING"},
        {"RND", "RND -> DOUBLE"},
        {"RTRIM$", "RTRIM$(s$) -> STRING"},
        {"SGN", "SGN(n) -> INTEGER"},
        {"SIN", "SIN(n) -> DOUBLE"},
        {"SPACE$", "SPACE$(n) -> STRING"},
        {"SQR", "SQR(n) -> DOUBLE"},
        {"STR$", "STR$(n) -> STRING"},
        {"STRING$", "STRING$(n, char) -> STRING"},
        {"TAN", "TAN(n) -> DOUBLE"},
        {"TIMER", "TIMER -> DOUBLE"},
        {"UBOUND", "UBOUND(arr) -> INTEGER"},
        {"UCASE$", "UCASE$(s$) -> STRING"},
        {"VAL", "VAL(s$) -> DOUBLE"},
    };

    std::vector<CompletionItem> items;
    for (const auto &b : builtins) {
        if (prefixMatch(b.name, prefix)) {
            items.push_back({b.name, b.name, CompletionKind::Function, b.detail, 80});
        }
    }
    return items;
}

/// @copydoc BasicCompletionEngine::provideScopeSymbols()
std::vector<CompletionItem> BasicCompletionEngine::provideScopeSymbols(
    const SemanticAnalyzer &sema, const std::string &prefix) const {
    std::vector<CompletionItem> items;

    // Variables
    for (const auto &sym : sema.symbols()) {
        if (prefixMatch(sym, prefix)) {
            std::string detail;
            auto ty = sema.lookupVarType(sym);
            if (ty) {
                switch (*ty) {
                    case SemanticAnalyzer::Type::Int:
                        detail = "INTEGER";
                        break;
                    case SemanticAnalyzer::Type::Float:
                        detail = "DOUBLE";
                        break;
                    case SemanticAnalyzer::Type::String:
                        detail = "STRING";
                        break;
                    case SemanticAnalyzer::Type::Bool:
                        detail = "BOOLEAN";
                        break;
                    case SemanticAnalyzer::Type::ArrayInt:
                        detail = "INTEGER()";
                        break;
                    case SemanticAnalyzer::Type::ArrayString:
                        detail = "STRING()";
                        break;
                    case SemanticAnalyzer::Type::ArrayObject:
                        detail = "OBJECT()";
                        break;
                    case SemanticAnalyzer::Type::Object: {
                        auto cls = sema.lookupObjectClassQName(sym);
                        detail = cls.value_or("Object");
                        break;
                    }
                    case SemanticAnalyzer::Type::Unknown:
                        detail = "";
                        break;
                }
            }

            auto kind = sema.isConstSymbol(sym) ? CompletionKind::Value : CompletionKind::Variable;
            items.push_back({sym, sym, kind, detail, 50});
        }
    }

    // Procedures (SUB/FUNCTION)
    for (const auto &[name, sig] : sema.procs()) {
        if (prefixMatch(name, prefix)) {
            std::string detail = sig.kind == ProcSignature::Kind::Function ? "FUNCTION" : "SUB";
            items.push_back({name, name, CompletionKind::Function, detail, 60});
        }
    }

    return items;
}

/// @copydoc BasicCompletionEngine::provideMemberCompletions()
std::vector<CompletionItem> BasicCompletionEngine::provideMemberCompletions(
    const SemanticAnalyzer &sema, const Context &ctx) const {
    std::vector<CompletionItem> items;

    // Try to resolve the trigger expression as a variable with an object type
    auto classQName = sema.lookupObjectClassQName(ctx.triggerExpr);
    if (classQName) {
        // Check OOP index for class fields/methods
        const auto *classInfo = sema.oopIndex().findClass(*classQName);
        if (classInfo) {
            for (const auto &field : classInfo->fields) {
                if (prefixMatch(field.name, ctx.prefix)) {
                    items.push_back({field.name, field.name, CompletionKind::Field, "", 30});
                }
            }
            for (const auto &[mName, mInfo] : classInfo->methods) {
                if (prefixMatch(mName, ctx.prefix)) {
                    items.push_back({mName, mName, CompletionKind::Method, "", 30});
                }
            }

            if (!items.empty())
                return items;
        }

        // Try runtime class
        items = provideRuntimeMembers(*classQName, ctx.prefix);
        if (!items.empty())
            return items;
    }

    // Try trigger expression as a runtime namespace/class directly
    // e.g., "Zanna.Terminal" or just "Terminal"
    std::string runtimeName = ctx.triggerExpr;

    // Check if it resolves via USING imports
    auto imports = sema.getUsingImports();
    for (const auto &ns : imports) {
        std::string candidate = ns + "." + runtimeName;
        auto members = provideRuntimeMembers(candidate, ctx.prefix);
        if (!members.empty())
            return members;
    }

    // Direct lookup
    items = provideRuntimeMembers(runtimeName, ctx.prefix);
    if (!items.empty())
        return items;

    // Try "Zanna." prefix
    items = provideRuntimeMembers("Zanna." + runtimeName, ctx.prefix);

    return items;
}

/// @copydoc BasicCompletionEngine::provideRuntimeMembers()
std::vector<CompletionItem> BasicCompletionEngine::provideRuntimeMembers(
    const std::string &className, const std::string &prefix) const {
    std::vector<CompletionItem> items;
    const auto *cls = il::runtime::findRuntimeClassByQName(className);
    if (!cls)
        return items;
    const std::string classDocumentation = runtimeClassDocumentation(*cls);

    for (const auto &method : cls->methods) {
        if (prefixMatch(method.name, prefix)) {
            CompletionItem item{method.name,
                                method.name,
                                CompletionKind::Method,
                                method.signature ? method.signature : "",
                                30};
            item.documentation = classDocumentation;
            items.push_back(std::move(item));
        }
    }
    for (const auto &prop : cls->properties) {
        if (prefixMatch(prop.name, prefix)) {
            CompletionItem item{
                prop.name, prop.name, CompletionKind::Property, prop.type ? prop.type : "", 30};
            item.documentation = classDocumentation;
            items.push_back(std::move(item));
        }
    }
    return items;
}

// --- Post-processing ---

/// @copydoc BasicCompletionEngine::filterByPrefix()
void BasicCompletionEngine::filterByPrefix(std::vector<CompletionItem> &items,
                                           const std::string &prefix) const {
    if (prefix.empty())
        return;
    std::string lp = toLowerStr(prefix);
    /// Remove labels that do not contain the folded search text.
    items.erase(std::remove_if(items.begin(),
                               items.end(),
                               [&lp](const CompletionItem &item) {
                                   std::string ll = toLowerStr(item.label);
                                   return ll.find(lp) == std::string::npos;
                               }),
                items.end());
}

/// @copydoc BasicCompletionEngine::rank()
void BasicCompletionEngine::rank(std::vector<CompletionItem> &items,
                                 const std::string &prefix) const {
    if (prefix.empty()) {
        /// Without search text, provider priority alone determines ordering.
        std::sort(items.begin(), items.end(), [](const CompletionItem &a, const CompletionItem &b) {
            return a.sortPriority < b.sortPriority;
        });
        return;
    }

    std::string lp = toLowerStr(prefix);
    std::vector<std::string> lowerLabels;
    lowerLabels.reserve(items.size());
    for (const auto &item : items)
        lowerLabels.push_back(toLowerStr(item.label));

    std::vector<size_t> order(items.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;

    /// Compare cached lowercase labels through the documented affinity tiers.
    std::sort(order.begin(), order.end(), [&items, &lowerLabels, &lp](size_t ai, size_t bi) {
        const auto &a = items[ai];
        const auto &b = items[bi];
        const auto &la = lowerLabels[ai];
        const auto &lb = lowerLabels[bi];
        bool aExact = (la == lp);
        bool bExact = (lb == lp);
        if (aExact != bExact)
            return aExact;
        bool aPrefix = (la.find(lp) == 0);
        bool bPrefix = (lb.find(lp) == 0);
        if (aPrefix != bPrefix)
            return aPrefix;
        if (a.sortPriority != b.sortPriority)
            return a.sortPriority < b.sortPriority;
        return la < lb;
    });

    std::vector<CompletionItem> ranked;
    ranked.reserve(items.size());
    for (size_t idx : order)
        ranked.push_back(std::move(items[idx]));
    items = std::move(ranked);
}

/// @copydoc BasicCompletionEngine::deduplicate()
void BasicCompletionEngine::deduplicate(std::vector<CompletionItem> &items) const {
    std::unordered_set<std::string> seen;
    seen.reserve(items.size());
    std::vector<CompletionItem> unique;
    unique.reserve(items.size());
    for (auto &item : items) {
        std::string key = item.label;
        key.push_back('\0');
        key += std::to_string(static_cast<unsigned>(item.kind));
        if (seen.insert(key).second)
            unique.push_back(std::move(item));
    }
    items = std::move(unique);
}

// --- Main entry point ---

/// @copydoc BasicCompletionEngine::complete()
std::vector<CompletionItem> BasicCompletionEngine::complete(
    std::string_view source, int line, int col, std::string_view filePath, int maxResults) {
    // Check cache
    uint64_t hash = fnv1a(source);
    if (cache_.hash != hash || cache_.filePath != filePath || !cache_.result) {
        sm_ = std::make_unique<il::support::SourceManager>();
        BasicCompilerInput input{.source = source, .path = filePath};
        cache_.result = parseAndAnalyzeBasic(input, *sm_);
        cache_.hash = hash;
        cache_.filePath = std::string(filePath);
    }

    auto &ar = *cache_.result;
    if (!ar.sema)
        return {};

    auto ctx = extractContext(source, line, col);

    std::vector<CompletionItem> items;

    if (ctx.trigger == TriggerKind::MemberAccess) {
        items = provideMemberCompletions(*ar.sema, ctx);
    } else {
        // General completion: keywords + builtins + scope symbols
        auto kw = provideKeywords(ctx.prefix);
        auto sn = provideSnippets(ctx.prefix);
        auto bi = provideBuiltins(ctx.prefix);
        auto sc = provideScopeSymbols(*ar.sema, ctx.prefix);

        items.reserve(kw.size() + sn.size() + bi.size() + sc.size());
        items.insert(
            items.end(), std::make_move_iterator(sc.begin()), std::make_move_iterator(sc.end()));
        items.insert(
            items.end(), std::make_move_iterator(bi.begin()), std::make_move_iterator(bi.end()));
        items.insert(
            items.end(), std::make_move_iterator(kw.begin()), std::make_move_iterator(kw.end()));
        items.insert(
            items.end(), std::make_move_iterator(sn.begin()), std::make_move_iterator(sn.end()));
    }

    filterByPrefix(items, ctx.prefix);
    deduplicate(items);
    rank(items, ctx.prefix);

    if (maxResults > 0 && static_cast<int>(items.size()) > maxResults)
        items.resize(static_cast<size_t>(maxResults));

    return items;
}

} // namespace il::frontends::basic
