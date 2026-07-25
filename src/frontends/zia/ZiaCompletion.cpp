//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file ZiaCompletion.cpp
/// @brief Implementation of the Zia code-completion engine.
/// @details Extracts cursor context, maintains a one-entry semantic-analysis cache, gathers
///          completion candidates from lexical/module/runtime providers, ranks and deduplicates
///          them, and computes signature help with error-tolerant source fallbacks.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/ZiaCompletion.hpp"
#include "frontends/zia/Sema.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include "support/source_manager.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_set>

namespace il::frontends::zia {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief True if @p c can appear in an identifier (alphanumeric or '_') —
///        used to scan the identifier under the completion cursor.
/// @param c Candidate source byte.
/// @return True for an ASCII-compatible alphanumeric byte or underscore.
static bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// @brief Map a Symbol::Kind to the corresponding CompletionKind.
/// @param sym Semantic symbol.
/// @return UI completion category, distinguishing extern fields as properties.
static CompletionKind kindFromSymbol(const Symbol &sym) {
    switch (sym.kind) {
        case Symbol::Kind::Variable:
            return CompletionKind::Variable;
        case Symbol::Kind::Parameter:
            return CompletionKind::Parameter;
        case Symbol::Kind::Function:
            return CompletionKind::Function;
        case Symbol::Kind::Method:
            return CompletionKind::Method;
        case Symbol::Kind::Field:
            // getRuntimeMembers() encodes RT properties as Kind::Field with isExtern=true.
            return sym.isExtern ? CompletionKind::Property : CompletionKind::Field;
        case Symbol::Kind::Type:
            return CompletionKind::Entity;
        case Symbol::Kind::Module:
            return CompletionKind::Module;
    }
    return CompletionKind::Variable;
}

/// @brief Build a human-readable detail string for a symbol's type.
/// @param type Semantic type.
/// @return Developer-facing spelling, or an empty string for null.
static std::string typeDetail(const TypeRef &type) {
    if (!type)
        return {};
    return type->toDisplayString();
}

/// @brief Combine authored runtime class summary/details for tooling display.
/// @param runtimeClass Runtime catalog entry.
/// @return Summary and details separated by a blank line, omitting missing portions.
static std::string runtimeClassDocumentation(const il::runtime::RuntimeClass &runtimeClass) {
    std::string documentation = runtimeClass.summary ? runtimeClass.summary : "";
    if (runtimeClass.details && *runtimeClass.details) {
        if (!documentation.empty())
            documentation += "\n\n";
        documentation += runtimeClass.details;
    }
    return documentation;
}

/// @brief Escape a field for the tab/newline-delimited wire rows (VDOC-111).
/// @param field Unescaped field contents.
/// @return Escaped text safe for one tab-delimited record.
/// @details Multiline snippet insertion text would otherwise split one item
///          across several apparent rows. Backslash, tab, newline, and CR are
///          escaped as \\, \t, \n, \r; consumers unescape in reverse.
static std::string escapeField(const std::string &field) {
    std::string out;
    out.reserve(field.size());
    for (char c : field) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

/// @brief Serialize one completion item to the runtime bridge's core four-field row.
/// @param item Completion item to encode.
/// @return Escaped, tab-delimited row ending with a newline.
static std::string serializeItem(const CompletionItem &item) {
    return escapeField(item.label) + '\t' + escapeField(item.insertText) + '\t' +
           std::to_string(static_cast<int>(item.kind)) + '\t' + escapeField(item.detail) + '\n';
}

/// @brief Compare two byte strings case-insensitively using character folding.
/// @param a Left string.
/// @param b Right string.
/// @return True when lengths and all folded bytes match.
static bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

/// @brief Split a dotted expression into nonempty identifier components.
/// @param expr Dotted expression text.
/// @return Components in left-to-right order.
static std::vector<std::string> splitDotted(std::string_view expr) {
    std::vector<std::string> parts;
    std::string token;
    for (char c : expr) {
        if (c == '.') {
            if (!token.empty())
                parts.push_back(token);
            token.clear();
        } else {
            token += c;
        }
    }
    if (!token.empty())
        parts.push_back(token);
    return parts;
}

/// @brief Test whether a byte may participate in a simple call-target expression.
/// @param c Candidate source byte.
/// @return True for identifier bytes or a period.
static bool isCallExprChar(char c) {
    return isIdentChar(c) || c == '.';
}

/// @brief Convert a one-based line and zero-based column to a clamped byte offset.
/// @param src Full source buffer.
/// @param line Requested line.
/// @param col Requested column.
/// @return Offset no later than the requested line's end.
static size_t offsetForLineCol(std::string_view src, int line, int col) {
    if (line < 1)
        line = 1;
    if (col < 0)
        col = 0;

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

    size_t offset = lineStart + static_cast<size_t>(col);
    return offset > lineEnd ? lineEnd : offset;
}

/// @brief Syntactic call context immediately surrounding a signature-help cursor.
struct SignatureCallContext {
    bool valid{false};
    std::string calleeExpr;
    std::string receiverExpr;
    std::string name;
    int activeParameter{0};
};

/// @brief Extract the innermost open call and active parameter at a cursor.
/// @param src Full source buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Parsed call context; `valid` is false when no simple open call is found.
static SignatureCallContext extractSignatureCallContext(std::string_view src, int line, int col) {
    SignatureCallContext ctx;
    size_t cursor = offsetForLineCol(src, line, col);
    if (cursor == 0 || cursor > src.size())
        return ctx;

    int depth = 0;
    size_t open = std::string_view::npos;
    for (size_t i = cursor; i > 0; --i) {
        char c = src[i - 1];
        if (c == ')') {
            ++depth;
        } else if (c == '(') {
            if (depth == 0) {
                open = i - 1;
                break;
            }
            --depth;
        }
    }
    if (open == std::string_view::npos)
        return ctx;

    size_t calleeEnd = open;
    while (calleeEnd > 0 && std::isspace(static_cast<unsigned char>(src[calleeEnd - 1])))
        --calleeEnd;

    size_t calleeStart = calleeEnd;
    while (calleeStart > 0 && isCallExprChar(src[calleeStart - 1]))
        --calleeStart;
    if (calleeStart == calleeEnd)
        return ctx;

    ctx.calleeExpr = std::string(src.substr(calleeStart, calleeEnd - calleeStart));
    size_t dot = ctx.calleeExpr.rfind('.');
    if (dot == std::string::npos) {
        ctx.name = ctx.calleeExpr;
    } else {
        ctx.receiverExpr = ctx.calleeExpr.substr(0, dot);
        ctx.name = ctx.calleeExpr.substr(dot + 1);
    }
    if (ctx.name.empty())
        return ctx;

    int nestedParen = 0;
    int nestedBracket = 0;
    int nestedBrace = 0;
    for (size_t i = open + 1; i < cursor && i < src.size(); ++i) {
        char c = src[i];
        if (c == '(') {
            ++nestedParen;
        } else if (c == ')' && nestedParen > 0) {
            --nestedParen;
        } else if (c == '[') {
            ++nestedBracket;
        } else if (c == ']' && nestedBracket > 0) {
            --nestedBracket;
        } else if (c == '{') {
            ++nestedBrace;
        } else if (c == '}' && nestedBrace > 0) {
            --nestedBrace;
        } else if (c == ',' && nestedParen == 0 && nestedBracket == 0 && nestedBrace == 0) {
            ++ctx.activeParameter;
        }
    }

    ctx.valid = true;
    return ctx;
}

/// @brief Format a semantic function type for signature-help display.
/// @param name Source-visible callable name.
/// @param type Semantic function type.
/// @param activeParameter Zero-based active argument index.
/// @param paramNames Optional declared parameter names.
/// @return Signature plus active-parameter summary, or an empty string for a non-function type.
static std::string formatFunctionSignature(const std::string &name,
                                           const TypeRef &type,
                                           int activeParameter,
                                           const std::vector<std::string> *paramNames = nullptr) {
    if (!type || type->kind != TypeKindSem::Function)
        return {};

    auto params = type->paramTypes();
    TypeRef ret = type->returnType();
    std::ostringstream out;
    out << name << "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0)
            out << ", ";
        std::string paramName = "arg" + std::to_string(i + 1);
        if (paramNames && i < paramNames->size() && !(*paramNames)[i].empty())
            paramName = (*paramNames)[i];
        out << paramName << ": " << (params[i] ? params[i]->toDisplayString() : "Unknown");
    }
    out << ")";
    if (ret)
        out << " -> " << ret->toDisplayString();
    if (!params.empty()) {
        int active = activeParameter;
        if (active < 0)
            active = 0;
        if (active >= static_cast<int>(params.size()))
            active = static_cast<int>(params.size()) - 1;
        out << "\nparameter " << (active + 1) << " of " << params.size();
    }
    return out.str();
}

/// @brief Assign the base ranking priority for a scope symbol.
/// @param sym Candidate semantic symbol.
/// @return Lower-is-better priority favoring local parameters, variables, and fields.
static int sortPriorityForScopeSymbol(const Symbol &sym) {
    switch (sym.kind) {
        case Symbol::Kind::Parameter:
            return 1;
        case Symbol::Kind::Variable:
            return sym.isExtern ? 45 : 2;
        case Symbol::Kind::Field:
            return sym.isExtern ? 35 : 3;
        case Symbol::Kind::Method:
            return sym.isExtern ? 30 : 4;
        case Symbol::Kind::Function:
            return sym.isExtern ? 35 : 10;
        case Symbol::Kind::Type:
            return 20;
        case Symbol::Kind::Module:
            return 12;
    }
    return 50;
}

/// @brief Trim leading and trailing character-class whitespace.
/// @param text Text view to trim.
/// @return Owning trimmed copy.
static std::string trimCopy(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return std::string(text.substr(start, end - start));
}

/// @brief Test a string-view prefix.
/// @param text Candidate full text.
/// @param prefix Prefix to match.
/// @return True when @p text begins with @p prefix.
static bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/// @brief Collect contiguous `///` documentation immediately before a location.
/// @param sm Source manager containing the file text.
/// @param loc Declaration location.
/// @return Comment text in source order with markers removed, or an empty string.
static std::string docCommentBefore(const il::support::SourceManager &sm,
                                    il::support::SourceLoc loc) {
    if (!loc.isValid() || loc.line <= 1)
        return {};

    std::vector<std::string> lines;
    uint32_t line = loc.line - 1;
    while (line > 0) {
        std::string text = trimCopy(sm.getLine(loc.file_id, line));
        if (startsWith(text, "///")) {
            std::string doc = trimCopy(std::string_view(text).substr(3));
            lines.push_back(std::move(doc));
        } else {
            break;
        }
        --line;
    }

    std::string doc;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (!doc.empty())
            doc += "\n";
        doc += *it;
    }
    return doc;
}

/// @brief Resolve authored documentation for a semantic symbol.
/// @param sm Optional source manager for declaration-comment fallback.
/// @param sym Symbol carrying runtime or source declaration metadata.
/// @return Explicit symbol documentation, preceding source comment, or an empty string.
static std::string documentationForSymbol(const il::support::SourceManager *sm, const Symbol &sym) {
    if (!sym.documentation.empty())
        return sym.documentation;
    if (!sm)
        return {};
    il::support::SourceLoc loc = sym.loc.isValid() ? sym.loc : il::support::SourceLoc{};
    if (!loc.isValid() && sym.decl)
        loc = sym.decl->loc;
    return docCommentBefore(*sm, loc);
}

/// @brief Test that a keyword occurrence is not embedded in an identifier.
/// @param source Full source buffer.
/// @param start Candidate keyword offset.
/// @param len Keyword byte length.
/// @return True when both surrounding boundaries are non-identifier bytes or buffer edges.
static bool hasKeywordBoundary(std::string_view source, size_t start, size_t len) {
    if (start > 0 && isIdentChar(source[start - 1]))
        return false;
    size_t end = start + len;
    return end >= source.size() || !isIdentChar(source[end]);
}

/// @brief Find the closing parenthesis matching an opening one.
/// @param source Source buffer.
/// @param open Offset of the opening parenthesis.
/// @return Matching offset, or npos; parentheses inside quoted strings are ignored.
static size_t findMatchingParen(std::string_view source, size_t open) {
    int depth = 0;
    bool inString = false;
    for (size_t i = open; i < source.size(); ++i) {
        char c = source[i];
        if (inString) {
            if (c == '\\' && i + 1 < source.size()) {
                ++i;
                continue;
            }
            if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return std::string_view::npos;
}

/// @brief Recover direct-function signatures from source when semantic analysis is incomplete.
/// @param source Full source buffer.
/// @param call Parsed call context.
/// @return Newline-delimited matching declarations with active-parameter summary, or empty.
static std::string fallbackSignatureFromSource(std::string_view source,
                                               const SignatureCallContext &call) {
    if (!call.valid || !call.receiverExpr.empty() || call.name.empty())
        return {};

    std::vector<std::string> signatures;
    size_t pos = 0;
    while ((pos = source.find("func", pos)) != std::string_view::npos) {
        if (!hasKeywordBoundary(source, pos, 4)) {
            pos += 4;
            continue;
        }

        size_t cursor = pos + 4;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        size_t nameStart = cursor;
        while (cursor < source.size() && isIdentChar(source[cursor]))
            ++cursor;
        if (nameStart == cursor) {
            pos += 4;
            continue;
        }

        std::string name(source.substr(nameStart, cursor - nameStart));
        if (!equalsIgnoreCase(name, call.name)) {
            pos = cursor;
            continue;
        }

        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        if (cursor >= source.size() || source[cursor] != '(') {
            pos = cursor;
            continue;
        }

        size_t close = findMatchingParen(source, cursor);
        if (close == std::string_view::npos) {
            pos = cursor + 1;
            continue;
        }

        std::string params = trimCopy(source.substr(cursor + 1, close - cursor - 1));
        size_t after = close + 1;
        while (after < source.size() && std::isspace(static_cast<unsigned char>(source[after])))
            ++after;

        std::string returnType;
        if (after + 1 < source.size() && source[after] == '-' && source[after + 1] == '>') {
            after += 2;
            size_t retStart = after;
            while (after < source.size() && source[after] != '{' && source[after] != '\n' &&
                   source[after] != ';')
                ++after;
            returnType = trimCopy(source.substr(retStart, after - retStart));
        }

        std::ostringstream sig;
        sig << name << "(" << params << ")";
        if (!returnType.empty())
            sig << " -> " << returnType;
        if (!params.empty()) {
            int paramCount = 1;
            int nestedParen = 0;
            int nestedBracket = 0;
            int nestedBrace = 0;
            for (char c : params) {
                if (c == '(')
                    ++nestedParen;
                else if (c == ')' && nestedParen > 0)
                    --nestedParen;
                else if (c == '[')
                    ++nestedBracket;
                else if (c == ']' && nestedBracket > 0)
                    --nestedBracket;
                else if (c == '{')
                    ++nestedBrace;
                else if (c == '}' && nestedBrace > 0)
                    --nestedBrace;
                else if (c == ',' && nestedParen == 0 && nestedBracket == 0 && nestedBrace == 0)
                    ++paramCount;
            }

            int active = call.activeParameter;
            if (active < 0)
                active = 0;
            if (active >= paramCount)
                active = paramCount - 1;
            sig << "\nparameter " << (active + 1) << " of " << paramCount;
        }
        signatures.push_back(sig.str());
        pos = close + 1;
    }

    if (signatures.empty())
        return {};

    std::ostringstream out;
    for (size_t i = 0; i < signatures.size(); ++i) {
        if (i > 0)
            out << "\n";
        out << signatures[i];
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// serialize
// ---------------------------------------------------------------------------

/// @brief Serialize completion items for the runtime bridge.
/// @param items Ranked completion items.
/// @return Concatenated escaped records, each ending in a newline.
std::string serialize(const std::vector<CompletionItem> &items) {
    std::string out;
    out.reserve(items.size() * 40);
    for (const auto &item : items)
        out += serializeItem(item);
    return out;
}

// ---------------------------------------------------------------------------
// FNV-1a hash
// ---------------------------------------------------------------------------

/// @brief Compute a 64-bit FNV-1a source hash.
/// @param data Source bytes.
/// @return Deterministic hash used to recognize unchanged cached input.
uint64_t CompletionEngine::fnv1a(std::string_view data) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Static keyword / snippet data
// ---------------------------------------------------------------------------

static const char *const kKeywords[] = {
    // Statement keywords
    "var",
    "func",
    "if",
    "else",
    "while",
    "for",
    "in",
    "return",
    "break",
    "continue",
    "defer",
    "try",
    "catch",
    "finally",
    "throw",
    "and",
    "or",
    "not",
    "is",
    "as",
    "new",
    "true",
    "false",
    "null",
    "match",
    // Declaration keywords
    "class",
    "interface",
    "struct",
    "expose",
    "module",
    "bind",
    // Built-in types
    "Integer",
    "Number",
    "Boolean",
    "String",
    "Byte",
    "Bytes",
    "List",
    "Map",
    "Set",
    "Object",
    nullptr,
};

struct SnippetData {
    const char *label;
    const char *insertText;
    int cursorOffset;
};

static const SnippetData kSnippets[] = {
    {"if", "if  {\n    \n}", 3},
    {"if-else", "if  {\n    \n} else {\n    \n}", 3},
    {"while", "while  {\n    \n}", 6},
    {"for", "for i in 0..n {\n    \n}", 14},
    {"for-in", "for item in  {\n    \n}", 12},
    {"func", "func name() {\n    \n}", 10},
    {"class", "class Name {\n    expose func init() {\n    }\n}", 11},
    {nullptr, nullptr, -1},
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

/// @brief Initialize an empty completion engine and source manager.
CompletionEngine::CompletionEngine() : sm_(std::make_unique<il::support::SourceManager>()) {}

/// @brief Destroy cached analysis and source-management state.
CompletionEngine::~CompletionEngine() = default;

/// @brief Discard cached analysis and recreate the source manager.
void CompletionEngine::clearCache() {
    cache_.hash = 0;
    cache_.filePath.clear();
    cache_.result = nullptr;
    // Recreate SourceManager so file IDs are fresh.
    sm_ = std::make_unique<il::support::SourceManager>();
}

// ---------------------------------------------------------------------------
// Context extraction
// ---------------------------------------------------------------------------

/// @brief Classify a completion trigger and calculate its replacement span.
/// @param src Full source buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Trigger, receiver expression, typed prefix, and cursor coordinates.
CompletionEngine::Context CompletionEngine::extractContext(std::string_view src,
                                                           int line,
                                                           int col) const {
    Context ctx;
    ctx.line = line;
    ctx.col = col;

    // Find the start of the requested line (1-based).
    size_t lineStart = 0;
    int curLine = 1;
    for (size_t i = 0; i < src.size() && curLine < line; ++i) {
        if (src[i] == '\n') {
            ++curLine;
            lineStart = i + 1;
        }
    }

    // Extract line text up to col (clamp to line length).
    size_t lineEnd = lineStart;
    while (lineEnd < src.size() && src[lineEnd] != '\n')
        ++lineEnd;

    size_t cursorOff = lineStart + static_cast<size_t>(col);
    if (cursorOff > lineEnd)
        cursorOff = lineEnd;

    std::string_view lineUpToCursor = src.substr(lineStart, cursorOff - lineStart);

    // ── Step 1: collect identifier prefix (chars user has already typed) ────
    int prefixLen = 0;
    for (int i = static_cast<int>(lineUpToCursor.size()) - 1;
         i >= 0 && isIdentChar(lineUpToCursor[i]);
         --i) {
        ++prefixLen;
    }
    ctx.prefix = std::string(lineUpToCursor.substr(lineUpToCursor.size() - prefixLen));
    ctx.replaceStart = col - prefixLen;

    // Position just before the prefix starts.
    int triggerPos = static_cast<int>(lineUpToCursor.size()) - prefixLen - 1;

    // ── Step 2: detect trigger ───────────────────────────────────────────────
    if (triggerPos >= 0 && lineUpToCursor[triggerPos] == '.') {
        ctx.trigger = TriggerKind::MemberAccess;

        // Collect the expression to the left of '.': scan backward through
        // identifier chars and embedded dots (for chained access like a.b.c).
        int exprEnd = triggerPos;
        int exprStart = exprEnd - 1;
        while (exprStart >= 0 &&
               (isIdentChar(lineUpToCursor[exprStart]) || lineUpToCursor[exprStart] == '.')) {
            --exprStart;
        }
        ++exprStart;
        if (exprStart < exprEnd) {
            ctx.triggerExpr = std::string(lineUpToCursor.substr(exprStart, exprEnd - exprStart));
        }
    } else {
        // Check for keyword triggers by looking at the word just before the prefix.
        // We need at least 4 chars before to match "new " or "return ".
        std::string_view before = lineUpToCursor.substr(0, lineUpToCursor.size() - prefixLen);

        auto endsWith = [](std::string_view sv, const char *suffix) -> bool {
            size_t n = std::strlen(suffix);
            return sv.size() >= n && sv.substr(sv.size() - n) == suffix;
        };

        auto trimRight = [](std::string_view sv) -> std::string_view {
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
                sv.remove_suffix(1);
            return sv;
        };

        auto endsWithWord = [](std::string_view sv, const char *word) -> bool {
            size_t n = std::strlen(word);
            if (sv.size() < n || sv.substr(sv.size() - n) != word)
                return false;
            if (sv.size() == n)
                return true;
            return !isIdentChar(sv[sv.size() - n - 1]);
        };

        std::string_view beforeTrimmed = trimRight(before);

        if (endsWith(before, "new ") || endsWithWord(beforeTrimmed, "new"))
            ctx.trigger = TriggerKind::AfterNew;
        else if (endsWith(before, "return "))
            ctx.trigger = TriggerKind::AfterReturn;
        else if (!beforeTrimmed.empty() && beforeTrimmed.back() == ':')
            ctx.trigger = TriggerKind::AfterColon;
        else
            ctx.trigger = TriggerKind::CtrlSpace;
    }

    return ctx;
}

// ---------------------------------------------------------------------------
// Type resolution for dotted expressions
// ---------------------------------------------------------------------------

/// @brief Resolve a dotted receiver expression to its semantic type at the cursor.
/// @param sema Completed semantic analyzer.
/// @param expr Dotted receiver text.
/// @param fileId Cursor file identifier.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Resolved member-chain type, or nullptr when the root/member cannot be found.
TypeRef CompletionEngine::resolveExprType(
    const Sema &sema, const std::string &expr, uint32_t fileId, int line, int col) const {
    if (expr.empty())
        return nullptr;

    // Split on '.'
    std::vector<std::string> parts;
    std::string token;
    for (char c : expr) {
        if (c == '.') {
            if (!token.empty())
                parts.push_back(token);
            token.clear();
        } else {
            token += c;
        }
    }
    if (!token.empty())
        parts.push_back(token);

    if (parts.empty())
        return nullptr;

    // Look up the first part in the most relevant visible scope at the cursor,
    // then fall back to global symbols.
    TypeRef current;
    if (const ScopedSymbol *scoped = sema.findSymbolAtPosition(
            parts[0], fileId, static_cast<uint32_t>(line), static_cast<uint32_t>(col))) {
        current = scoped->symbol.type;
    } else {
        auto globals = sema.getGlobalSymbols();
        for (const auto &sym : globals) {
            if (sym.name == parts[0]) {
                current = sym.type;
                // For Type symbols, the symbol's *type* is a metatype — the actual
                // instance type is what we need for member access.  Use as-is;
                // getMembersOf handles Entity/Value/Ptr kinds.
                break;
            }
        }
    }

    if (!current) {
        // parts[0] not found as a Zia symbol. Try alias expansion:
        // e.g. "GUI.Canvas" → alias "GUI" resolves to "Zanna.GUI"
        //      → reconstruct qname "Zanna.GUI.Canvas"
        std::string ns = sema.resolveModuleAlias(parts[0]);
        if (!ns.empty() && parts.size() > 1) {
            std::string fullQname = ns;
            for (size_t i = 1; i < parts.size(); ++i)
                fullQname += "." + parts[i];
            // Return as runtimeClass (Ptr+name) so getMembersOf delegates to getRuntimeMembers.
            if (!sema.getRuntimeMembers(fullQname).empty())
                return types::runtimeClass(fullQname);
        }
        // Last resort: treat the entire expr as a literal runtime class qname
        // (e.g. "Zanna.GUI.Canvas" typed without a binding alias).
        if (!sema.getRuntimeMembers(expr).empty())
            return types::runtimeClass(expr);
        return nullptr;
    }

    // Walk remaining parts.
    for (size_t i = 1; i < parts.size(); ++i) {
        // When current is a Module type (from a namespace alias like "bind Zanna.GUI as GUI"),
        // getMembersOf returns nothing useful.  Instead, reconstruct the full class qname by
        // appending the remaining parts to the module's namespace name.
        if (current->kind == TypeKindSem::Module && !current->name.empty()) {
            std::string fullQname = current->name;
            for (size_t j = i; j < parts.size(); ++j)
                fullQname += "." + parts[j];
            if (!sema.getRuntimeMembers(fullQname).empty())
                return types::runtimeClass(fullQname);
            return nullptr;
        }

        auto members = sema.getMembersOf(current);
        bool found = false;
        for (const auto &mem : members) {
            if (mem.name == parts[i]) {
                // For method symbols, the type is a function type; we want the
                // return type for further member chaining.
                if (mem.type && mem.type->kind == TypeKindSem::Function)
                    current = mem.type->returnType();
                else
                    current = mem.type;
                found = true;
                break;
            }
        }
        if (!found)
            return nullptr;
    }

    return current;
}

// ---------------------------------------------------------------------------
// Providers
// ---------------------------------------------------------------------------

/// @brief Provide language keyword completions.
/// @param prefix Typed prefix.
/// @return Filtered keyword items.
std::vector<CompletionItem> CompletionEngine::provideKeywords(const std::string &prefix) const {
    std::vector<CompletionItem> items;
    for (int i = 0; kKeywords[i]; ++i) {
        CompletionItem item;
        item.label = kKeywords[i];
        item.insertText = kKeywords[i];
        item.kind = CompletionKind::Keyword;
        item.source = "keyword";
        item.sortPriority = 50;
        items.push_back(std::move(item));
    }
    filterByPrefix(items, prefix);
    return items;
}

/// @brief Provide built-in code-template completions.
/// @param prefix Typed prefix.
/// @return Filtered snippet items with cursor offsets.
std::vector<CompletionItem> CompletionEngine::provideSnippets(const std::string &prefix) const {
    std::vector<CompletionItem> items;
    for (int i = 0; kSnippets[i].label; ++i) {
        CompletionItem item;
        item.label = kSnippets[i].label;
        item.insertText = kSnippets[i].insertText;
        item.kind = CompletionKind::Snippet;
        item.detail = "snippet";
        item.source = "snippet";
        item.isSnippet = true;
        item.cursorOffset = kSnippets[i].cursorOffset;
        item.sortPriority = 40;
        items.push_back(std::move(item));
    }
    filterByPrefix(items, prefix);
    return items;
}

/// @brief Provide symbols visible at a source position.
/// @param sema Completed semantic analyzer.
/// @param prefix Typed prefix.
/// @param fileId Cursor file identifier.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Filtered items with scope-aware priorities and source documentation.
std::vector<CompletionItem> CompletionEngine::provideScopeSymbols(
    const Sema &sema, const std::string &prefix, uint32_t fileId, int line, int col) const {
    std::vector<CompletionItem> items;
    auto symbols = sema.getVisibleSymbolsAtPosition(
        fileId, static_cast<uint32_t>(line), static_cast<uint32_t>(std::max(1, col + 1)));
    std::unordered_set<std::string> seen;
    for (const auto &sym : symbols) {
        seen.insert(sym.name);
        CompletionItem item;
        item.label = sym.name;
        item.insertText = sym.name;
        item.kind = kindFromSymbol(sym);
        item.detail = typeDetail(sym.type);
        item.documentation = documentationForSymbol(sm_.get(), sym);
        item.source = sym.isExtern ? "runtime" : "scope";
        if (item.kind == CompletionKind::Function || item.kind == CompletionKind::Method)
            item.commitCharacters = "(";
        item.sortPriority = sortPriorityForScopeSymbol(sym);
        items.push_back(std::move(item));
    }

    auto globals = sema.getGlobalSymbols();
    for (const auto &sym : globals) {
        if (!seen.insert(sym.name).second)
            continue;
        CompletionItem item;
        item.label = sym.name;
        item.insertText = sym.name;
        item.kind = kindFromSymbol(sym);
        item.detail = typeDetail(sym.type);
        item.documentation = documentationForSymbol(sm_.get(), sym);
        item.source = sym.isExtern ? "runtime" : "scope";
        if (item.kind == CompletionKind::Function || item.kind == CompletionKind::Method)
            item.commitCharacters = "(";
        item.sortPriority = sortPriorityForScopeSymbol(sym);
        items.push_back(std::move(item));
    }
    filterByPrefix(items, prefix);
    return items;
}

/// @brief Provide members for the receiver captured in a member-access context.
/// @param sema Completed semantic analyzer.
/// @param ctx Member-access context.
/// @return File-module exports, runtime namespace/classes, or semantic type members.
std::vector<CompletionItem> CompletionEngine::provideMemberCompletions(const Sema &sema,
                                                                       const Context &ctx) const {
    std::vector<CompletionItem> items;
    if (ctx.triggerExpr.empty())
        return items;

    // ── Step 1: split triggerExpr on '.' ────────────────────────────────────
    std::vector<std::string> parts;
    {
        std::string tok;
        for (char c : ctx.triggerExpr) {
            if (c == '.') {
                if (!tok.empty())
                    parts.push_back(tok);
                tok.clear();
            } else
                tok += c;
        }
        if (!tok.empty())
            parts.push_back(tok);
    }
    if (parts.empty())
        return items;

    // File binds expose exported symbols through their module name. Completing
    // `Lib.` should list the bound file's exported functions/types.
    if (parts.size() == 1) {
        auto moduleMembers = provideModuleMembers(sema, parts[0], ctx.prefix);
        if (!moduleMembers.empty())
            return moduleMembers;
    }

    // ── Step 2: check whether the first part is a bound namespace alias ──────
    // e.g. "GUI"        → resolves to "Zanna.GUI"
    //      "GUI.Canvas" → parts[0]="GUI" → alias → reconstruct "Zanna.GUI.Canvas"
    std::string resolved = sema.resolveModuleAlias(parts[0]);
    if (!resolved.empty()) {
        if (parts.size() == 1) {
            // User typed e.g. "Math." or "GUI." after a namespace alias.
            // Case A: the resolved path IS a class (e.g. "Zanna.Math" with Sqrt/Abs/…).
            auto rtMembers = provideRuntimeMembers(sema, resolved, ctx.prefix);
            if (!rtMembers.empty())
                return rtMembers;
            // Case B: the resolved path is a namespace containing classes (e.g. "Zanna.GUI").
            return provideNamespaceMembers(sema, resolved, ctx.prefix);
        }

        // Reconstruct full class/sub-namespace qname from alias + remaining parts.
        std::string fullClass = resolved;
        for (size_t i = 1; i < parts.size(); ++i)
            fullClass += "." + parts[i];

        // Try as a specific runtime class (has methods/properties).
        auto rtMembers = provideRuntimeMembers(sema, fullClass, ctx.prefix);
        if (!rtMembers.empty())
            return rtMembers;

        // Otherwise it may be a sub-namespace — enumerate its child classes.
        return provideNamespaceMembers(sema, fullClass, ctx.prefix);
    }

    // ── Step 3: try the entire triggerExpr as a literal runtime qname ────────
    // This handles bare "Zanna.GUI.Canvas." typed without a binding alias.
    {
        auto rtMembers = provideRuntimeMembers(sema, ctx.triggerExpr, ctx.prefix);
        if (!rtMembers.empty())
            return rtMembers;
        auto nsMembers = provideNamespaceMembers(sema, ctx.triggerExpr, ctx.prefix);
        if (!nsMembers.empty())
            return nsMembers;
    }

    // ── Step 4: resolve via expression type (for user-defined class fields) ─
    TypeRef type = resolveExprType(
        sema, ctx.triggerExpr, cache_.result ? cache_.result->fileId : 0, ctx.line, ctx.col);
    if (!type)
        return items;

    auto members = sema.getMembersOf(type);
    for (const auto &sym : members) {
        CompletionItem item;
        item.label = sym.name;
        item.insertText = sym.name;
        item.kind = kindFromSymbol(sym);
        item.detail = typeDetail(sym.type);
        item.documentation = documentationForSymbol(sm_.get(), sym);
        item.source = sym.isExtern ? "runtime" : "member";
        if (item.kind == CompletionKind::Function || item.kind == CompletionKind::Method)
            item.commitCharacters = "(";
        item.sortPriority = 5;
        items.push_back(std::move(item));
    }
    filterByPrefix(items, ctx.prefix);
    return items;
}

/// @brief Provide declared semantic type names.
/// @param sema Completed semantic analyzer.
/// @param prefix Typed prefix.
/// @return Filtered type completion items.
std::vector<CompletionItem> CompletionEngine::provideTypeNames(const Sema &sema,
                                                               const std::string &prefix) const {
    std::vector<CompletionItem> items;
    auto names = sema.getTypeNames();
    for (auto &name : names) {
        CompletionItem item;
        item.label = name;
        item.insertText = name;
        item.kind = CompletionKind::Entity;
        item.source = "type";
        item.sortPriority = 20;
        items.push_back(std::move(item));
    }
    filterByPrefix(items, prefix);
    return items;
}

/// @brief Provide exports from a bound file module.
/// @param sema Completed semantic analyzer.
/// @param moduleAlias Visible module root or alias.
/// @param prefix Typed member prefix.
/// @return Filtered exported-symbol items.
std::vector<CompletionItem> CompletionEngine::provideModuleMembers(
    const Sema &sema, const std::string &moduleAlias, const std::string &prefix) const {
    std::vector<CompletionItem> items;
    auto exports = sema.getModuleExports(moduleAlias);
    for (const auto &sym : exports) {
        CompletionItem item;
        item.label = sym.name;
        item.insertText = sym.name;
        item.kind = kindFromSymbol(sym);
        item.detail = typeDetail(sym.type);
        item.documentation = documentationForSymbol(sm_.get(), sym);
        item.source = "module";
        if (item.kind == CompletionKind::Function || item.kind == CompletionKind::Method)
            item.commitCharacters = "(";
        item.sortPriority = 5;
        items.push_back(std::move(item));
    }
    filterByPrefix(items, prefix);
    return items;
}

/// @brief Provide visible bound file-module roots.
/// @param sema Completed semantic analyzer.
/// @param prefix Typed module prefix.
/// @return Filtered module completion items.
std::vector<CompletionItem> CompletionEngine::provideBoundFileModules(
    const Sema &sema, const std::string &prefix) const {
    std::vector<CompletionItem> items;
    auto modules = sema.getBoundFileModuleNames();
    for (const auto &moduleName : modules) {
        CompletionItem item;
        item.label = moduleName;
        item.insertText = moduleName;
        item.kind = CompletionKind::Module;
        item.detail = "module";
        item.source = "module";
        item.sortPriority = 12;
        items.push_back(std::move(item));
    }
    filterByPrefix(items, prefix);
    return items;
}

/// @brief Provide methods and properties of a runtime class.
/// @param sema Completed semantic analyzer.
/// @param fullClassName Fully qualified runtime class name.
/// @param prefix Typed member prefix.
/// @return Filtered runtime member items.
std::vector<CompletionItem> CompletionEngine::provideRuntimeMembers(
    const Sema &sema, const std::string &fullClassName, const std::string &prefix) const {
    std::vector<CompletionItem> items;
    auto members = sema.getRuntimeMembers(fullClassName);
    for (const auto &sym : members) {
        CompletionItem item;
        item.label = sym.name;
        item.insertText = sym.name;
        // Distinguish methods (Function type) from properties.
        if (sym.type && sym.type->kind == TypeKindSem::Function)
            item.kind = CompletionKind::Method;
        else
            item.kind = CompletionKind::Property;
        item.detail = typeDetail(sym.type);
        item.documentation = documentationForSymbol(sm_.get(), sym);
        item.source = "runtime";
        if (item.kind == CompletionKind::Method)
            item.commitCharacters = "(";
        item.sortPriority = 5;
        items.push_back(std::move(item));
    }
    filterByPrefix(items, prefix);
    return items;
}

/// @brief Provide immediate runtime classes below a namespace.
/// @param sema Completed semantic analyzer.
/// @param nsPrefix Fully qualified runtime namespace.
/// @param prefix Typed child prefix.
/// @return Filtered runtime-class items with catalog documentation.
std::vector<CompletionItem> CompletionEngine::provideNamespaceMembers(
    const Sema &sema, const std::string &nsPrefix, const std::string &prefix) const {
    std::vector<CompletionItem> items;
    auto classNames = sema.getNamespaceClasses(nsPrefix);
    for (auto &name : classNames) {
        CompletionItem item;
        item.label = name;
        item.insertText = name;
        item.kind = CompletionKind::RuntimeClass;
        const std::string qualifiedName = nsPrefix + "." + name;
        if (const auto *runtimeClass = il::runtime::findRuntimeClassByQName(qualifiedName))
            item.documentation = runtimeClassDocumentation(*runtimeClass);
        if (item.documentation.empty())
            item.documentation = "Runtime class " + qualifiedName + ".";
        item.source = "runtime";
        item.sortPriority = 5;
        items.push_back(std::move(item));
    }
    filterByPrefix(items, prefix);
    return items;
}

// ---------------------------------------------------------------------------
// Filtering, ranking, deduplication
// ---------------------------------------------------------------------------

/// @brief Remove completion items that do not case-insensitively begin with a prefix.
/// @param items Mutable completion list.
/// @param prefix Typed prefix; an empty prefix leaves the list unchanged.
void CompletionEngine::filterByPrefix(std::vector<CompletionItem> &items,
                                      const std::string &prefix) const {
    if (prefix.empty())
        return;

    items.erase(
        std::remove_if(items.begin(),
                       items.end(),
                       [&](const CompletionItem &item) {
                           // Case-insensitive prefix match.
                           if (item.label.size() < prefix.size())
                               return true;
                           for (size_t i = 0; i < prefix.size(); ++i) {
                               if (std::tolower(static_cast<unsigned char>(item.label[i])) !=
                                   std::tolower(static_cast<unsigned char>(prefix[i])))
                                   return true;
                           }
                           return false;
                       }),
        items.end());
}

/// @brief Stable-sort completion items by textual match and provider priority.
/// @param items Mutable completion list.
/// @param prefix Typed prefix.
void CompletionEngine::rank(std::vector<CompletionItem> &items, const std::string &prefix) const {
    if (prefix.empty()) {
        std::stable_sort(
            items.begin(), items.end(), [](const CompletionItem &a, const CompletionItem &b) {
                return a.sortPriority < b.sortPriority;
            });
        return;
    }

    // Score: 0 = exact, 1 = prefix (case-sensitive), 2 = prefix (insensitive), 3 = other.
    auto score = [&](const CompletionItem &item) -> int {
        if (item.label == prefix)
            return 0;
        if (item.label.size() >= prefix.size() && item.label.substr(0, prefix.size()) == prefix)
            return 1;
        return 2;
    };

    std::stable_sort(
        items.begin(), items.end(), [&](const CompletionItem &a, const CompletionItem &b) {
            int sa = score(a), sb = score(b);
            if (sa != sb)
                return sa < sb;
            return a.sortPriority < b.sortPriority;
        });
}

/// @brief Retain the first completion item for each label.
/// @param items Ranked completion list to deduplicate in place.
void CompletionEngine::deduplicate(std::vector<CompletionItem> &items) const {
    std::unordered_set<std::string> seen;
    items.erase(
        std::remove_if(items.begin(),
                       items.end(),
                       [&](const CompletionItem &item) { return !seen.insert(item.label).second; }),
        items.end());
}

// ---------------------------------------------------------------------------
// Primary entry point
// ---------------------------------------------------------------------------

/// @brief Return cached analysis for unchanged input or perform a new partial compilation.
/// @param source Full source buffer.
/// @param filePath Virtual source path.
/// @return Borrowed cached analysis result, possibly null only if analysis allocation fails.
AnalysisResult *CompletionEngine::analyze(std::string_view source, std::string_view filePath) {
    uint64_t hash = fnv1a(source);
    if (hash == cache_.hash && cache_.filePath == filePath && cache_.result)
        return cache_.result.get();

    cache_.hash = 0;
    cache_.filePath.clear();
    cache_.result = nullptr;
    sm_ = std::make_unique<il::support::SourceManager>();

    std::string sourceStr(source);
    std::string pathStr(filePath);
    CompilerInput input;
    input.source = sourceStr;
    input.path = pathStr;
    CompilerOptions opts{};

    cache_.result = parseAndAnalyze(input, opts, *sm_);
    if (cache_.result) {
        cache_.hash = hash;
        cache_.filePath = pathStr;
    }
    return cache_.result.get();
}

/// @brief Compute ranked completion items at a cursor.
/// @param source Full source buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @param filePath Virtual source path.
/// @param maxResults Maximum returned items, or zero for unlimited.
/// @return Provider results with replacement ranges and snippet metadata populated.
std::vector<CompletionItem> CompletionEngine::complete(
    std::string_view source, int line, int col, std::string_view filePath, int maxResults) {
    AnalysisResult *analysis = analyze(source, filePath);

    // ── Context extraction ───────────────────────────────────────────────────
    // Always extract context first — does not require a valid sema.
    Context ctx = extractContext(source, line, col);

    // ── Provider dispatch ────────────────────────────────────────────────────
    // Gate sema-dependent providers on hasSema; keywords and snippets always run.
    std::vector<CompletionItem> items;
    bool hasSema = analysis && analysis->sema;

    switch (ctx.trigger) {
        case TriggerKind::MemberAccess: {
            if (!hasSema)
                break;
            const Sema &sema = *analysis->sema;
            // Member access: enumerate members of the LHS type.
            // Also check if triggerExpr is a bound module alias with dot
            // (e.g. "Zanna.Math.Pi" — triggerExpr="Zanna.Math", prefix="Pi").
            auto members = provideMemberCompletions(sema, ctx);
            items.insert(items.end(), members.begin(), members.end());
            break;
        }

        case TriggerKind::AfterNew: {
            if (!hasSema)
                break;
            const Sema &sema = *analysis->sema;
            auto types = provideTypeNames(sema, ctx.prefix);
            items.insert(items.end(), types.begin(), types.end());
            break;
        }

        case TriggerKind::AfterColon: {
            if (!hasSema)
                break;
            const Sema &sema = *analysis->sema;
            auto types = provideTypeNames(sema, ctx.prefix);
            items.insert(items.end(), types.begin(), types.end());
            // Built-in type keywords
            auto kws = provideKeywords(ctx.prefix);
            for (auto &kw : kws) {
                // Filter to just type keywords.
                static const char *const typeKws[] = {
                    "Integer",
                    "Number",
                    "Boolean",
                    "String",
                    "Byte",
                    "Bytes",
                    "List",
                    "Map",
                    "Set",
                    "Object",
                    nullptr,
                };
                bool isType = false;
                for (int i = 0; typeKws[i]; ++i)
                    if (kw.label == typeKws[i]) {
                        isType = true;
                        break;
                    }
                if (isType)
                    items.push_back(std::move(kw));
            }
            break;
        }

        case TriggerKind::AfterReturn:
        case TriggerKind::CtrlSpace: {
            // Scope symbols and type names require sema.
            if (hasSema) {
                const Sema &sema = *analysis->sema;
                auto scope =
                    provideScopeSymbols(sema, ctx.prefix, analysis->fileId, ctx.line, ctx.col);
                items.insert(items.end(), scope.begin(), scope.end());
                auto modules = provideBoundFileModules(sema, ctx.prefix);
                items.insert(items.end(), modules.begin(), modules.end());
                auto types = provideTypeNames(sema, ctx.prefix);
                items.insert(items.end(), types.begin(), types.end());
            }
            // Keywords and snippets always available — no sema needed.
            auto kws = provideKeywords(ctx.prefix);
            items.insert(items.end(), kws.begin(), kws.end());
            auto snips = provideSnippets(ctx.prefix);
            items.insert(items.end(), snips.begin(), snips.end());
            break;
        }
    }

    // ── Post-processing ──────────────────────────────────────────────────────
    rank(items, ctx.prefix);
    deduplicate(items);

    for (auto &item : items) {
        item.replacementStartLine = ctx.line;
        item.replacementStartColumn = ctx.replaceStart;
        item.replacementEndLine = ctx.line;
        item.replacementEndColumn = ctx.col;
        item.isSnippet = item.isSnippet || item.kind == CompletionKind::Snippet;
    }

    if (maxResults > 0 && static_cast<int>(items.size()) > maxResults)
        items.resize(static_cast<size_t>(maxResults));

    return items;
}

/// @brief Compute display signatures for the call active at a cursor.
/// @param source Full source buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @param filePath Virtual source path.
/// @return Newline-delimited unique signatures with documentation, or an empty string.
std::string CompletionEngine::signatureHelp(std::string_view source,
                                            int line,
                                            int col,
                                            std::string_view filePath) {
    SignatureCallContext call = extractSignatureCallContext(source, line, col);
    if (!call.valid)
        return {};

    std::string fallback = fallbackSignatureFromSource(source, call);
    if (!fallback.empty())
        return fallback;

    AnalysisResult *analysis = analyze(source, filePath);
    if (!analysis || !analysis->sema)
        return {};

    const Sema &sema = *analysis->sema;
    std::vector<std::string> signatures;
    std::unordered_set<std::string> seen;

    auto addFunctionSymbol = [&](const Symbol &sym) {
        std::string formatted =
            formatFunctionSignature(call.name, sym.type, call.activeParameter, &sym.paramNames);
        if (!formatted.empty()) {
            std::string doc = documentationForSymbol(sm_.get(), sym);
            if (!doc.empty())
                formatted += "\n" + doc;
        }
        if (!formatted.empty() && seen.insert(formatted).second)
            signatures.push_back(std::move(formatted));
    };

    auto addMatchingMembers = [&](const std::vector<Symbol> &members) {
        for (const auto &sym : members) {
            if (equalsIgnoreCase(sym.name, call.name))
                addFunctionSymbol(sym);
        }
    };

    auto addFunctionDeclOverloads = [&]() {
        for (const auto &sym : sema.getFunctionOverloadSymbols(call.name))
            addFunctionSymbol(sym);
    };

    auto runtimeClassNameFromReceiver = [&](const std::string &receiver) -> std::string {
        auto parts = splitDotted(receiver);
        if (parts.empty())
            return {};

        std::string fullName;
        std::string resolved = sema.resolveModuleAlias(parts[0]);
        if (!resolved.empty()) {
            fullName = resolved;
            for (size_t i = 1; i < parts.size(); ++i)
                fullName += "." + parts[i];
        } else {
            fullName = receiver;
        }

        if (!fullName.empty() && !sema.getRuntimeMembers(fullName).empty())
            return fullName;
        return {};
    };

    if (call.receiverExpr.empty()) {
        addFunctionDeclOverloads();

        if (const ScopedSymbol *scoped =
                sema.findSymbolAtPosition(call.name,
                                          analysis->fileId,
                                          static_cast<uint32_t>(line),
                                          static_cast<uint32_t>(col + 1))) {
            addFunctionSymbol(scoped->symbol);
        }

        if (signatures.empty()) {
            for (const auto &sym : sema.getGlobalSymbols()) {
                if (equalsIgnoreCase(sym.name, call.name))
                    addFunctionSymbol(sym);
            }
        }
    } else {
        if (call.receiverExpr.find('.') == std::string::npos) {
            addMatchingMembers(sema.getModuleExports(call.receiverExpr));
        }

        std::string className = runtimeClassNameFromReceiver(call.receiverExpr);
        if (!className.empty())
            addMatchingMembers(sema.getRuntimeMembers(className));

        if (signatures.empty()) {
            TypeRef receiverType =
                resolveExprType(sema, call.receiverExpr, analysis->fileId, line, col + 1);
            if (receiverType)
                addMatchingMembers(sema.getMembersOf(receiverType));
        }
    }

    if (signatures.empty())
        return {};

    std::ostringstream out;
    for (size_t i = 0; i < signatures.size(); ++i) {
        if (i > 0)
            out << "\n";
        out << signatures[i];
    }
    return out.str();
}

} // namespace il::frontends::zia
