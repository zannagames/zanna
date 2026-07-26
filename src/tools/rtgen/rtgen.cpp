//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Parses modular runtime definitions and generates registries, frontend
///        name tables, signatures, class metadata, and API documentation.
///
/// Definition includes are relative, cycle-free, confined to the definition
/// root, and processed in declaration order. ParseState owns accumulated
/// metadata until generation completes; generated C++ and Markdown own copied
/// strings.
///
/// @see src/il/runtime/runtime.def
/// @see docs/adr/0101-modular-runtime-definitions-and-documentation.md
//
// Usage: rtgen <input.def> <output_dir>
//        rtgen --validate <input.def>
//        rtgen --docs [--check] <input.def> <output_dir>
//
// Outputs:
//   - RuntimeNameMap.inc     (canonical Zanna.* -> rt_* symbol mapping)
//   - RuntimeClasses.inc     (OOP class/method/property catalog)
//   - RuntimeSignatures.inc  (runtime descriptor rows)
//   - RuntimeNames.hpp       (C++ constants for frontend use)
//
//===----------------------------------------------------------------------===//

#include "../../common/Filesystem.hpp"
#include "../../common/Utf8CommandLine.hpp"
#include "../common/packaging/PkgUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

static constexpr std::uintmax_t kMaxRtgenTextFileBytes = 16ULL * 1024ULL * 1024ULL;

//===----------------------------------------------------------------------===//
// Data Structures
//===----------------------------------------------------------------------===//

/// @brief One RT_FUNC entry from runtime.def: a runtime function and its mapping.
struct RuntimeFunc {
    std::string id;                       // Unique identifier (e.g., "PrintStr")
    std::string c_symbol;                 // C runtime symbol (e.g., "rt_print_str")
    std::string canonical;                // Canonical Zanna.* name (e.g., "Zanna.Console.PrintStr")
    std::string signature;                // Type signature (e.g., "void(str)")
    std::string lowering;                 // Lowering kind: "always" or "" (default: manual)
    std::vector<std::string> bridgeRoles; // Safe Zia bridge roles: none/callback/payload
    bool publicSurface{true};             // False for class-method implementation targets.
};

/// @brief A property exposed by a runtime class (RT_PROP entry).
struct RuntimeProperty {
    std::string name;      // Property name (e.g., "Length")
    std::string type;      // Property type (e.g., "i64")
    std::string getter_id; // Getter function id (or canonical name)
    std::string setter_id; // Setter function id or "none"
};

/// @brief A method exposed by a runtime class (RT_METHOD entry).
struct RuntimeMethod {
    std::string name;      // Method name (e.g., "Substring")
    std::string signature; // Signature without receiver (e.g., "str(i64,i64)")
    std::string target_id; // Target function id (or canonical name)
};

/// @brief Authored documentation attached to a runtime definition row.
struct RuntimeDocumentation {
    std::string summary; ///< Short plain-text description used by completion lists.
    std::string details; ///< Long Markdown description used by hover and reference docs.
};

/// @brief A runtime OOP class (RT_CLASS block) with its properties and methods.
struct RuntimeClass {
    std::string name;                   // Class name (e.g., "Zanna.String")
    std::string type_id;                // Type ID suffix (e.g., "String")
    std::string layout;                 // Layout type (e.g., "opaque*", "obj")
    std::string ctor_id;                // Constructor function id or empty
    std::string base_name;              // Optional fully-qualified base class
    RuntimeDocumentation documentation; // Authored summary and long-form details
    std::vector<RuntimeProperty> props; // Properties
    std::vector<RuntimeMethod> methods; // Methods
};

/// @brief A C function signature parsed from a runtime header declaration.
struct CSignature {
    std::string returnType;            ///< C return type.
    std::vector<std::string> argTypes; ///< C parameter types, in order.
};

/// @brief Fields used to emit one descriptor row in RuntimeSignatures.inc.
struct DescriptorFields {
    std::string signatureId;   ///< Signature identifier.
    std::string spec;          ///< Signature-spec expression.
    std::string handler;       ///< VM handler expression.
    std::string lowering;      ///< Lowering kind.
    std::string hidden;        ///< Hidden-argument expression.
    std::string hiddenCount;   ///< Number of hidden arguments.
    std::string trapClass;     ///< Trap classification.
    std::string publicSurface; ///< Whether this descriptor is frontend-visible as a function.
    std::string cSymbol;       ///< Backing C function symbol for manifest/tooling output.
};

/// @brief A runtime function prototype recovered from a runtime header.
struct RuntimePrototype {
    CSignature signature;                ///< Parsed C signature.
    std::vector<std::string> paramNames; ///< Parameter names, in order.
    std::string headerPath;              ///< Header file the declaration came from.
};

/// @brief A class property with canonical getter/setter names resolved.
struct ResolvedRuntimeProperty {
    std::string name;            ///< Property name.
    std::string type;            ///< Property type.
    std::string getterCanonical; ///< Canonical getter name.
    std::string setterCanonical; ///< Canonical setter name, or empty.
};

/// @brief A class method with its target canonical name resolved.
struct ResolvedRuntimeMethod {
    std::string name;            ///< Method name.
    std::string signature;       ///< Signature without receiver.
    std::string targetCanonical; ///< Canonical target function name.
};

/// @brief A runtime class after id references are resolved to canonical names.
struct ResolvedRuntimeClass {
    std::string name;                           ///< Class name.
    std::string type_id;                        ///< Type ID suffix.
    std::string layout;                         ///< Layout type.
    std::string ctorCanonical;                  ///< Canonical constructor name, or empty.
    std::string baseName;                       ///< Fully-qualified base class, or empty.
    RuntimeDocumentation documentation;         ///< Authored class documentation.
    std::vector<ResolvedRuntimeProperty> props; ///< Resolved properties.
    std::vector<ResolvedRuntimeMethod> methods; ///< Resolved methods.
};

/// @brief Expected runtime surface parsed from the surface-policy file, used by the
///        audit to detect drift between runtime.def and the actual runtime.
struct RuntimeSurfacePolicy {
    std::unordered_set<std::string> internalHeaders; ///< Headers excluded from the surface.
    std::unordered_set<std::string> internalSymbols; ///< Symbols excluded from the surface.
    std::unordered_map<std::string, std::string> expectedFunctions; ///< Required functions.
    std::vector<ResolvedRuntimeMethod> expectedMethods;             ///< Required class methods.
    std::vector<ResolvedRuntimeProperty> expectedProperties;        ///< Required class properties.
};

//===----------------------------------------------------------------------===//
// Parser State
//===----------------------------------------------------------------------===//

/// @brief Mutable parser state accumulated while reading runtime.def.
/// @details Holds the parsed functions/classes plus validation indices and
///          the current line context used for error/warning reporting.
struct ParseState {
    std::vector<RuntimeFunc> functions; ///< All parsed RT_FUNC entries.
    std::vector<RuntimeClass> classes;  ///< All parsed RT_CLASS blocks.

    // Maps for validation
    std::map<std::string, size_t> func_by_id;        ///< Function id → index in @c functions.
    std::map<std::string, size_t> func_by_canonical; ///< Canonical name → index in @c functions.
    std::set<std::string> all_canonicals; ///< All canonical names seen (duplicate guard).

    // Current class being parsed
    std::optional<RuntimeClass> current_class; ///< Class block currently open, if any.
    int line_num = 0;                          ///< 1-based current line for diagnostics.
    std::string filename;                      ///< Source filename for diagnostics.

    /// @brief Documentation block waiting to attach to the next class row.
    struct PendingDocumentation {
        std::string summary;           ///< Parsed @summary text.
        std::string details;           ///< Parsed @details Markdown.
        std::string filename;          ///< File where the block began.
        int line = 0;                  ///< Line where the block began.
        bool sawSummary{false};        ///< Whether @summary appeared.
        bool sawDetails{false};        ///< Whether @details appeared.
        bool collectingDetails{false}; ///< Whether subsequent /// lines are details.

        /// @brief Test whether a documentation block is awaiting attachment.
        /// @return @c true after summary/details content has begun.
        [[nodiscard]] bool active() const noexcept {
            return sawSummary || sawDetails || collectingDetails;
        }

        /// @brief Reset all pending documentation text and state flags.
        void clear() {
            *this = PendingDocumentation{};
        }
    } pendingDocumentation;

    /// @brief Print a file:line error to stderr and terminate the process.
    /// @param msg Error detail appended to the current source location.
    /// @throws std::runtime_error Always, with filename and line context.
    void error(const std::string &msg) const {
        throw std::runtime_error(filename + ":" + std::to_string(line_num) + ": error: " + msg);
    }

    /// @brief Print a non-fatal file:line warning to stderr.
    /// @param msg Warning detail appended to the current source location.
    void warning(const std::string &msg) const {
        std::cerr << filename << ":" << line_num << ": warning: " << msg << "\n";
    }
};

//===----------------------------------------------------------------------===//
// String Utilities
//===----------------------------------------------------------------------===//

/// @brief Return a copy of @p sv with leading/trailing ASCII whitespace removed.
/// @param sv Text view to trim.
/// @return Owned trimmed text.
static std::string trim(std::string_view sv) {
    /// @brief Test whether one byte is supported ASCII whitespace.
    /// @param c Byte to inspect.
    /// @return `true` for space, horizontal tab, line feed, or carriage return.
    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!sv.empty() && is_space(sv.front()))
        sv.remove_prefix(1);
    while (!sv.empty() && is_space(sv.back()))
        sv.remove_suffix(1);
    return std::string(sv);
}

/// @brief Split @p sv on @p delim, ignoring delimiters inside quotes or parentheses.
/// @details Each field is trimmed; empty fields are skipped. Used to parse the
///          comma-separated argument lists of runtime.def directives.
/// @param sv Delimited source text.
/// @param delim Separator recognized outside quotes and parentheses.
/// @return Owned trimmed nonempty fields in source order.
/// @throws std::runtime_error On unterminated quotes or unbalanced parentheses.
static std::vector<std::string> split(std::string_view sv, char delim) {
    std::vector<std::string> result;
    size_t start = 0;
    bool in_quotes = false;
    int paren_depth = 0;

    for (size_t i = 0; i <= sv.size(); ++i) {
        if (i < sv.size()) {
            if (sv[i] == '"' && (i == 0 || sv[i - 1] != '\\'))
                in_quotes = !in_quotes;
            else if (!in_quotes && sv[i] == '(')
                paren_depth++;
            else if (!in_quotes && sv[i] == ')') {
                paren_depth--;
                if (paren_depth < 0)
                    throw std::runtime_error("unbalanced ')' in comma-separated field");
            }
        }

        if (i == sv.size() || (!in_quotes && paren_depth == 0 && sv[i] == delim)) {
            if (i > start)
                result.push_back(trim(sv.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (in_quotes)
        throw std::runtime_error("unterminated quote in comma-separated field");
    if (paren_depth != 0)
        throw std::runtime_error("unbalanced parentheses in comma-separated field");
    return result;
}

/// @brief Split @p sv on @p delim only at the top level of nesting.
/// @details Like split(), but also balances angle brackets, braces, and square
///          brackets in addition to parentheses and quotes — needed to split
///          generic-bearing type signatures (e.g. `List<Map<a,b>>`).
/// @param sv Delimited source text.
/// @param delim Separator recognized only at nesting depth zero.
/// @return Owned trimmed nonempty top-level fields in source order.
static std::vector<std::string> splitTopLevel(std::string_view sv, char delim) {
    std::vector<std::string> result;
    size_t start = 0;
    bool in_quotes = false;
    int paren_depth = 0;
    int angle_depth = 0;
    int brace_depth = 0;
    int bracket_depth = 0;

    for (size_t i = 0; i <= sv.size(); ++i) {
        if (i < sv.size()) {
            if (sv[i] == '"' && (i == 0 || sv[i - 1] != '\\'))
                in_quotes = !in_quotes;
            else if (!in_quotes) {
                if (sv[i] == '(')
                    paren_depth++;
                else if (sv[i] == ')')
                    paren_depth--;
                else if (sv[i] == '<')
                    angle_depth++;
                else if (sv[i] == '>')
                    angle_depth--;
                else if (sv[i] == '{')
                    brace_depth++;
                else if (sv[i] == '}')
                    brace_depth--;
                else if (sv[i] == '[')
                    bracket_depth++;
                else if (sv[i] == ']')
                    bracket_depth--;
            }
        }

        if (i == sv.size() || (!in_quotes && paren_depth == 0 && angle_depth == 0 &&
                               brace_depth == 0 && bracket_depth == 0 && sv[i] == delim)) {
            if (i > start)
                result.push_back(trim(sv.substr(start, i - start)));
            start = i + 1;
        }
    }
    return result;
}

/// @brief Encode @p value as a quoted, escaped C++ string literal for emitted code.
/// @details Escapes quotes/backslashes/standard control chars and renders other
///          bytes below 0x20 (and 0x7F) as `\\xNN`, so generated .inc files always
///          compile regardless of the source text.
/// @param value Raw bytes to encode.
/// @return Quoted C++ string literal suitable for generated source.
static std::string cppStringLiteral(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
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
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

/// @brief Return true if @p sv begins with @p prefix.
/// @param sv Candidate complete text.
/// @param prefix Prefix to compare.
/// @return @c true when @p prefix matches at offset zero.
static bool startsWith(std::string_view sv, std::string_view prefix) {
    return sv.size() >= prefix.size() && sv.substr(0, prefix.size()) == prefix;
}

/// @brief Return @p sv with @p prefix removed, or unchanged if it does not match.
/// @param sv Source view.
/// @param prefix Optional prefix to remove.
/// @return Subview after @p prefix when matched, otherwise @p sv.
static std::string_view stripPrefix(std::string_view sv, std::string_view prefix) {
    if (startsWith(sv, prefix))
        return sv.substr(prefix.size());
    return sv;
}

/// @brief Return @p sv with leading/trailing ASCII whitespace removed (view, no copy).
/// @param sv Source view.
/// @return Trimmed subview borrowing the original storage.
static std::string_view trimView(std::string_view sv) {
    /// @brief Test whether one byte is supported ASCII whitespace.
    /// @param c Byte to inspect.
    /// @return `true` for space, horizontal tab, line feed, or carriage return.
    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!sv.empty() && is_space(sv.front()))
        sv.remove_prefix(1);
    while (!sv.empty() && is_space(sv.back()))
        sv.remove_suffix(1);
    return sv;
}

/// @brief Trim @p sv and remove one layer of surrounding double quotes, if present.
/// @param sv Source text.
/// @return Owned trimmed text with one matching quote pair removed.
static std::string stripQuotes(std::string_view sv) {
    std::string s = trim(sv);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

/// @brief Drop a trailing parameter name from a C parameter declaration.
/// @details Given e.g. "const char *path", returns "const char *"; returns the
///          input unchanged for "void" or when no trailing identifier is found.
/// @param sv C parameter declaration.
/// @return Owned declaration type with a trailing parameter identifier removed.
static std::string stripParamName(std::string_view sv) {
    std::string param = trim(sv);
    if (param.empty() || param == "void")
        return param;

    size_t end = param.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(param[end - 1])))
        --end;

    size_t i = end;
    while (i > 0 && (std::isalnum(static_cast<unsigned char>(param[i - 1])) || param[i - 1] == '_'))
        --i;

    if (i == end)
        return param;

    return trim(param.substr(0, i));
}

/// @brief Extract the parameter name from a C parameter declaration.
/// @details Handles function-pointer parameters (`ret (*name)(...)`) as well as
///          ordinary `type name` declarations; returns "" when there is no name
///          (or the parameter is "void").
/// @param sv C parameter declaration.
/// @return Extracted parameter identifier, or an empty string when absent.
static std::string extractParamName(std::string_view sv) {
    std::string param = trim(sv);
    if (param.empty() || param == "void")
        return {};

    if (size_t fnPtr = param.find("(*"); fnPtr != std::string::npos) {
        size_t start = fnPtr + 2;
        while (start < param.size() && std::isspace(static_cast<unsigned char>(param[start])))
            ++start;
        size_t end = start;
        while (end < param.size() &&
               (std::isalnum(static_cast<unsigned char>(param[end])) || param[end] == '_')) {
            ++end;
        }
        return end > start ? param.substr(start, end - start) : std::string();
    }

    size_t end = param.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(param[end - 1])))
        --end;

    size_t start = end;
    while (start > 0 &&
           (std::isalnum(static_cast<unsigned char>(param[start - 1])) || param[start - 1] == '_'))
        --start;

    if (start == end)
        return {};
    return param.substr(start, end - start);
}

/// @brief Extract the argument text inside a macro call's parentheses.
/// @details For input like `FOO(a, b, c)` with @p macro = "FOO", returns
///          "a, b, c", matching the closing paren while respecting nested parens
///          and quotes. Returns nullopt when @p line is not a call to @p macro or
///          the parentheses are unbalanced.
/// @param line Candidate macro-invocation text.
/// @param macro Required macro name prefix.
/// @return Owned parenthesized argument text, or @c std::nullopt on mismatch.
static std::optional<std::string> extractParens(std::string_view line, std::string_view macro) {
    if (!startsWith(line, macro))
        return std::nullopt;
    line = stripPrefix(line, macro);
    // Skip whitespace to find opening paren
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    if (line.empty() || line.front() != '(')
        return std::nullopt;
    line.remove_prefix(1);
    // Find matching close paren, respecting nested parens and quotes
    int depth = 1;
    bool in_quotes = false;
    size_t i = 0;
    for (; i < line.size() && depth > 0; ++i) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\'))
            in_quotes = !in_quotes;
        else if (!in_quotes && line[i] == '(')
            depth++;
        else if (!in_quotes && line[i] == ')')
            depth--;
    }
    if (depth != 0)
        return std::nullopt;
    return std::string(line.substr(0, i - 1));
}

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

/// @brief Parse an RT_FUNC-like directive and append a RuntimeFunc to @p state.
/// @details Validates the 4-5 argument arity, strips quotes, and enforces unique
///          ids and canonical names (reporting a fatal error otherwise).
/// @param state Parser state and uniqueness indices to update.
/// @param args Directive argument text.
/// @param publicSurface Whether the function is independently frontend-visible.
/// @throws std::runtime_error Through ParseState::error on invalid definitions.
static void parseRtFunc(ParseState &state, const std::string &args, bool publicSurface = true) {
    // RT_FUNC(id, c_symbol, canonical, signature [, lowering])
    auto parts = splitTopLevel(args, ',');
    if (parts.size() < 4 || parts.size() > 5) {
        state.error(
            "RT_FUNC requires 4-5 arguments: id, c_symbol, canonical, signature [, lowering]");
    }

    RuntimeFunc func;
    func.id = parts[0];
    func.c_symbol = parts[1];
    func.canonical = parts[2];
    func.signature = parts[3];
    if (parts.size() == 5)
        func.lowering = parts[4];
    func.publicSurface = publicSurface;

    // Remove quotes from canonical and signature
    if (func.canonical.size() >= 2 && func.canonical.front() == '"')
        func.canonical = func.canonical.substr(1, func.canonical.size() - 2);
    if (func.signature.size() >= 2 && func.signature.front() == '"')
        func.signature = func.signature.substr(1, func.signature.size() - 2);

    // Validate uniqueness
    if (state.func_by_id.count(func.id))
        state.error("Duplicate function id: " + func.id);
    if (state.func_by_canonical.count(func.canonical))
        state.error("Duplicate canonical name: " + func.canonical);

    size_t idx = state.functions.size();
    state.func_by_id[func.id] = idx;
    state.func_by_canonical[func.canonical] = idx;
    state.all_canonicals.insert(func.canonical);
    state.functions.push_back(std::move(func));
}

/// @brief Parse an RT_BRIDGE directive assigning Zia bridge roles to a function's
///        parameters.
/// @details Requires one role per surface parameter and validates each role is
///          one of none/callback/payload before recording them on the target.
/// @param state Parser state containing the referenced function.
/// @param args Bridge target and quoted role-list arguments.
/// @throws std::runtime_error Through ParseState::error on invalid roles or target.
static void parseRtBridge(ParseState &state, const std::string &args) {
    // RT_BRIDGE(target_id, "role0,role1,...")
    auto parts = splitTopLevel(args, ',');
    if (parts.size() != 2)
        state.error("RT_BRIDGE requires 2 arguments: target_id, role_list");

    const std::string targetId = parts[0];
    auto it = state.func_by_id.find(targetId);
    if (it == state.func_by_id.end())
        state.error("RT_BRIDGE target not found: " + targetId);

    RuntimeFunc &func = state.functions[it->second];
    size_t surfaceParamCount = 0;
    size_t parenPos = func.signature.find('(');
    size_t closePos = func.signature.rfind(')');
    if (parenPos != std::string::npos && closePos != std::string::npos && closePos > parenPos + 1) {
        surfaceParamCount =
            splitTopLevel(func.signature.substr(parenPos + 1, closePos - parenPos - 1), ',').size();
    }
    auto roles = splitTopLevel(stripQuotes(parts[1]), ',');
    if (roles.size() != surfaceParamCount) {
        state.error("RT_BRIDGE for " + targetId + " has " + std::to_string(roles.size()) +
                    " roles but signature has " + std::to_string(surfaceParamCount) +
                    " parameters");
    }
    for (const auto &role : roles) {
        if (role != "none" && role != "callback" && role != "payload")
            state.error("RT_BRIDGE role must be none, callback, or payload: " + role);
    }
    func.bridgeRoles = std::move(roles);
}

/// @brief Parse RT_CLASS_BEGIN, opening a new class block in @p state.
/// @details Rejects nested class blocks; the class is finalized by parseRtClassEnd.
/// @param state Parser state whose current class and pending documentation change.
/// @param args Class name, type ID, layout, constructor, and optional base.
/// @throws std::runtime_error Through ParseState::error on malformed or nested blocks.
static void parseRtClassBegin(ParseState &state, const std::string &args) {
    // RT_CLASS_BEGIN(name, type_id, layout, ctor_id[, base_name])
    if (state.current_class.has_value())
        state.error("Nested RT_CLASS_BEGIN not allowed");

    auto parts = split(args, ',');
    if (parts.size() != 4 && parts.size() != 5) {
        state.error("RT_CLASS_BEGIN requires 4 or 5 arguments: name, type_id, layout, ctor_id[, "
                    "base_name]");
    }

    RuntimeClass cls;
    cls.name = parts[0];
    cls.type_id = parts[1];
    cls.layout = parts[2];
    cls.ctor_id = parts[3];
    if (parts.size() == 5)
        cls.base_name = parts[4];

    // Remove quotes from all string fields
    /// @brief Remove one matching pair of double quotes from a parsed field.
    /// @param[in,out] s Field text to normalize in place.
    auto stripQuotes = [](std::string &s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
    };
    stripQuotes(cls.name);
    stripQuotes(cls.layout);
    stripQuotes(cls.ctor_id);
    stripQuotes(cls.base_name);

    if (state.pendingDocumentation.active()) {
        if (!state.pendingDocumentation.sawSummary)
            state.error("runtime class documentation is missing @summary");
        if (!state.pendingDocumentation.sawDetails)
            state.error("runtime class documentation is missing @details");

        while (!state.pendingDocumentation.details.empty() &&
               state.pendingDocumentation.details.front() == '\n') {
            state.pendingDocumentation.details.erase(state.pendingDocumentation.details.begin());
        }
        while (!state.pendingDocumentation.details.empty() &&
               state.pendingDocumentation.details.back() == '\n') {
            state.pendingDocumentation.details.pop_back();
        }

        cls.documentation.summary = std::move(state.pendingDocumentation.summary);
        cls.documentation.details = std::move(state.pendingDocumentation.details);
        state.pendingDocumentation.clear();
    }

    state.current_class = std::move(cls);
}

/// @brief Parse an RT_PROP directive, adding a property to the open class block.
/// @param state Parser state containing the open class.
/// @param args Property name, type, getter, and setter arguments.
/// @throws std::runtime_error Through ParseState::error outside a class or on bad arity.
static void parseRtProp(ParseState &state, const std::string &args) {
    // RT_PROP(name, type, getter_id, setter_id_or_none)
    if (!state.current_class.has_value())
        state.error("RT_PROP outside of RT_CLASS_BEGIN/END block");

    auto parts = split(args, ',');
    if (parts.size() != 4) {
        state.error("RT_PROP requires 4 arguments: name, type, getter_id, setter_id");
    }

    RuntimeProperty prop;
    prop.name = parts[0];
    prop.type = parts[1];
    prop.getter_id = parts[2];
    prop.setter_id = parts[3];

    // Remove quotes from all string fields
    /// @brief Remove one matching pair of double quotes from a parsed field.
    /// @param[in,out] s Field text to normalize in place.
    auto stripQuotes = [](std::string &s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
    };
    stripQuotes(prop.name);
    stripQuotes(prop.type);
    stripQuotes(prop.getter_id);
    stripQuotes(prop.setter_id);

    state.current_class->props.push_back(std::move(prop));
}

/// @brief Parse an RT_METHOD directive, adding a method to the open class block.
/// @param state Parser state containing the open class.
/// @param args Method name, signature, and target-function arguments.
/// @throws std::runtime_error Through ParseState::error outside a class or on bad arity.
static void parseRtMethod(ParseState &state, const std::string &args) {
    // RT_METHOD(name, signature, target_id)
    if (!state.current_class.has_value())
        state.error("RT_METHOD outside of RT_CLASS_BEGIN/END block");

    auto parts = split(args, ',');
    if (parts.size() != 3) {
        state.error("RT_METHOD requires 3 arguments: name, signature, target_id");
    }

    RuntimeMethod method;
    method.name = parts[0];
    method.signature = parts[1];
    method.target_id = parts[2];

    // Remove quotes from all string fields
    /// @brief Remove one matching pair of double quotes from a parsed field.
    /// @param[in,out] s Field text to normalize in place.
    auto stripQuotes = [](std::string &s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
    };
    stripQuotes(method.name);
    stripQuotes(method.signature);
    stripQuotes(method.target_id);

    state.current_class->methods.push_back(std::move(method));
}

/// @brief Parse RT_CLASS_END, committing the open class block to @p state.classes.
/// @param state Parser state containing the class to finalize.
/// @throws std::runtime_error Through ParseState::error when no class is open.
static void parseRtClassEnd(ParseState &state) {
    if (!state.current_class.has_value())
        state.error("RT_CLASS_END without matching RT_CLASS_BEGIN");

    state.classes.push_back(std::move(*state.current_class));
    state.current_class.reset();
}

/// @brief Parse one formal `///` runtime documentation line.
/// @details Documentation blocks support one `@summary` line followed by an
///          `@details` marker and zero or more Markdown lines. The block is
///          attached by parseRtClassBegin() to the immediately following class.
/// @param state Parser state holding pending documentation and source context.
/// @param line Trimmed formal documentation line beginning with @c ///.
/// @throws std::runtime_error Through ParseState::error for invalid command order.
static void parseDocumentationLine(ParseState &state, std::string_view line) {
    std::string_view content = stripPrefix(line, "///");
    if (!content.empty() && content.front() == ' ')
        content.remove_prefix(1);

    auto &doc = state.pendingDocumentation;
    if (!doc.active()) {
        doc.filename = state.filename;
        doc.line = state.line_num;
    }

    /// @brief Test whether the current documentation line begins with a command.
    /// @param command Command name to compare against `content`.
    /// @return `true` for an exact match or a command followed by whitespace.
    const auto isCommand = [content](std::string_view command) {
        return content == command ||
               (startsWith(content, command) && content.size() > command.size() &&
                std::isspace(static_cast<unsigned char>(content[command.size()])));
    };

    if (isCommand("@summary")) {
        if (doc.sawSummary)
            state.error("duplicate @summary in runtime documentation block");
        if (doc.sawDetails)
            state.error("@summary must appear before @details");
        std::string_view value = trimView(stripPrefix(content, "@summary"));
        if (value.empty())
            state.error("@summary requires non-empty text");
        if (value.size() > 200)
            state.error("@summary must not exceed 200 bytes");
        doc.summary = std::string(value);
        doc.sawSummary = true;
        return;
    }

    if (isCommand("@details")) {
        if (!doc.sawSummary)
            state.error("@details requires a preceding @summary");
        if (doc.sawDetails)
            state.error("duplicate @details in runtime documentation block");
        doc.sawDetails = true;
        doc.collectingDetails = true;
        std::string_view firstLine = stripPrefix(content, "@details");
        if (!firstLine.empty() && firstLine.front() == ' ')
            firstLine.remove_prefix(1);
        if (!firstLine.empty()) {
            doc.details.append(firstLine);
            doc.details.push_back('\n');
        }
        return;
    }

    if (!doc.collectingDetails)
        state.error("runtime documentation text requires an @details marker");

    doc.details.append(content);
    doc.details.push_back('\n');
}

/// @brief Return a quoted path from a `#include` directive.
/// @param line Candidate preprocessor line.
/// @return The relative include path, nullopt when @p line is not an include.
/// @throws std::runtime_error For angle-bracket, unterminated, or trailing syntax.
static std::optional<std::string> parseIncludeDirective(std::string_view line) {
    line = trimView(line);
    if (!startsWith(line, "#include"))
        return std::nullopt;
    line = trimView(stripPrefix(line, "#include"));
    if (line.size() < 2 || line.front() != '"')
        throw std::runtime_error("runtime definition includes must use quoted paths");
    const size_t close = line.find('"', 1);
    if (close == std::string_view::npos)
        throw std::runtime_error("unterminated quoted runtime definition include");
    if (!trimView(line.substr(close + 1)).empty())
        throw std::runtime_error("unexpected text after runtime definition include");
    return std::string(line.substr(1, close - 1));
}

/// @brief Return whether @p candidate is equal to or nested beneath @p root.
/// @param candidate Canonical candidate path.
/// @param root Canonical containment root.
/// @return @c true when every component of @p root prefixes @p candidate.
static bool pathIsWithin(const fs::path &candidate, const fs::path &root) {
    auto candidateIt = candidate.begin();
    for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *candidateIt != *rootIt)
            return false;
    }
    return true;
}

static const RuntimeFunc *resolveRuntimeFunc(const ParseState &state,
                                             const std::string &idOrCanonical);

/// @brief Dispatch one runtime.def line to the matching RT_* directive parser.
/// @details Parses formal documentation before ordinary comments. Includes are
///          handled by parseDefinitionFile(); an unrecognised directive is fatal.
/// @param state Parser state and current source location.
/// @param line Raw definition-file line.
/// @throws std::runtime_error Through ParseState::error for unknown or misplaced directives.
static void parseLine(ParseState &state, const std::string &line) {
    std::string trimmed = trim(line);

    if (startsWith(trimmed, "///")) {
        parseDocumentationLine(state, trimmed);
        return;
    }

    // Blank lines and ordinary comments do not interrupt a documentation block;
    // this permits section separators between prose and declarations while still
    // rejecting a block followed by the wrong directive below.
    if (trimmed.empty() || startsWith(trimmed, "//"))
        return;

    if (startsWith(trimmed, "#"))
        state.error("unsupported preprocessor directive: " + trimmed);

    if (state.pendingDocumentation.active() &&
        !extractParens(trimmed, "RT_CLASS_BEGIN").has_value()) {
        state.error("runtime documentation block must immediately precede RT_CLASS_BEGIN");
    }

    // Parse macros. Each branch uses a distinct binding name: an `else if`
    // condition declaration stays in scope through the rest of the chain, so a
    // shared name trips -Wshadow.
    if (auto funcArgs = extractParens(trimmed, "RT_FUNC")) {
        parseRtFunc(state, *funcArgs);
    } else if (auto internalFuncArgs = extractParens(trimmed, "RT_INTERNAL_FUNC")) {
        parseRtFunc(state, *internalFuncArgs, false);
    } else if (auto aliasArgs = extractParens(trimmed, "RT_ALIAS")) {
        (void)aliasArgs;
        state.error("RT_ALIAS is not supported; define a single canonical RT_FUNC name");
    } else if (auto bridgeArgs = extractParens(trimmed, "RT_BRIDGE")) {
        parseRtBridge(state, *bridgeArgs);
    } else if (auto classBeginArgs = extractParens(trimmed, "RT_CLASS_BEGIN")) {
        parseRtClassBegin(state, *classBeginArgs);
    } else if (auto propArgs = extractParens(trimmed, "RT_PROP")) {
        parseRtProp(state, *propArgs);
    } else if (auto methodArgs = extractParens(trimmed, "RT_METHOD")) {
        parseRtMethod(state, *methodArgs);
    } else if (trimmed == "RT_CLASS_END()") {
        parseRtClassEnd(state);
    } else {
        state.error("Unknown directive: " + trimmed);
    }
}

/// @brief Recursively parse one runtime definition file and its quoted includes.
/// @param state Accumulated parse state, temporarily updated with file context.
/// @param path Definition file to resolve and read.
/// @param definitionRoot Canonical root that every include must remain beneath.
/// @param includedFiles Canonical paths used to reject duplicate inclusion.
/// @param includeStack Active canonical paths used to detect cycles.
/// @throws std::runtime_error For I/O, containment, include, or directive errors.
static void parseDefinitionFile(ParseState &state,
                                const fs::path &path,
                                const fs::path &definitionRoot,
                                std::unordered_set<std::string> &includedFiles,
                                std::vector<std::string> &includeStack) {
    std::error_code canonicalEc;
    fs::path canonicalPath = fs::weakly_canonical(path, canonicalEc);
    if (canonicalEc)
        throw std::runtime_error("cannot resolve runtime definition file " +
                                 zanna::filesystem::pathToUtf8(path) + ": " +
                                 canonicalEc.message());
    if (!pathIsWithin(canonicalPath, definitionRoot))
        throw std::runtime_error("runtime definition include escapes definition root: " +
                                 zanna::filesystem::pathToUtf8(canonicalPath));

    const std::string pathKey = zanna::filesystem::genericPathToUtf8(canonicalPath);
    if (std::find(includeStack.begin(), includeStack.end(), pathKey) != includeStack.end())
        throw std::runtime_error("cyclic runtime definition include: " + pathKey);
    if (!includedFiles.insert(pathKey).second)
        throw std::runtime_error("duplicate runtime definition include: " + pathKey);

    std::ifstream in(canonicalPath);
    if (!in) {
        throw std::runtime_error("cannot open " + zanna::filesystem::pathToUtf8(canonicalPath));
    }

    const std::string previousFilename = state.filename;
    const int previousLine = state.line_num;
    state.filename = zanna::filesystem::genericPathToUtf8(canonicalPath);
    state.line_num = 0;
    includeStack.push_back(pathKey);

    std::string line;
    while (std::getline(in, line)) {
        state.line_num++;

        const std::string trimmed = trim(line);
        if (startsWith(trimmed, "#include")) {
            if (state.current_class.has_value())
                state.error("runtime definition include is not allowed inside a class block");
            if (state.pendingDocumentation.active())
                state.error(
                    "runtime definition include cannot follow a pending documentation block");

            std::optional<std::string> includePath;
            try {
                includePath = parseIncludeDirective(trimmed);
            } catch (const std::exception &e) {
                state.error(e.what());
            }
            if (!includePath)
                state.error("invalid runtime definition include");
            fs::path relativePath = zanna::filesystem::pathFromUtf8(*includePath);
            if (relativePath.empty() || relativePath.is_absolute())
                state.error("runtime definition include path must be relative");

            const std::string includeFilename = state.filename;
            const int includeLine = state.line_num;
            try {
                parseDefinitionFile(state,
                                    canonicalPath.parent_path() / relativePath,
                                    definitionRoot,
                                    includedFiles,
                                    includeStack);
            } catch (const std::exception &e) {
                throw std::runtime_error(std::string(e.what()) + "\n  included from " +
                                         includeFilename + ":" + std::to_string(includeLine));
            }
            state.filename = includeFilename;
            state.line_num = includeLine;
            continue;
        }

        parseLine(state, line);
    }

    if (state.pendingDocumentation.active())
        state.error("orphaned runtime documentation block at end of file");
    if (state.current_class.has_value()) {
        state.error("Unclosed RT_CLASS_BEGIN (missing RT_CLASS_END)");
    }

    includeStack.pop_back();
    state.filename = previousFilename;
    state.line_num = previousLine;
}

/// @brief Read and parse a runtime.def manifest and all included fragments.
/// @param path Root definition manifest.
/// @return Fully accumulated parser state.
/// @throws std::runtime_error When root resolution or parsing fails.
static ParseState parseFile(const fs::path &path) {
    ParseState state;
    std::error_code rootEc;
    const fs::path absolutePath = fs::absolute(path, rootEc);
    if (rootEc)
        throw std::runtime_error("cannot resolve runtime definition root: " + rootEc.message());
    const fs::path definitionRoot = fs::weakly_canonical(absolutePath.parent_path(), rootEc);
    if (rootEc)
        throw std::runtime_error("cannot resolve runtime definition root: " + rootEc.message());

    std::unordered_set<std::string> includedFiles;
    std::vector<std::string> includeStack;
    parseDefinitionFile(state, absolutePath, definitionRoot, includedFiles, includeStack);

    return state;
}

/// @brief Validate cross-row references that require the complete definition set.
/// @details Includes may place a referenced function after its class, so these
///          checks run only after the root manifest and all fragments are parsed.
/// @param state Complete parsed definition set.
/// @param inputPath Root manifest path used in aggregate error messages.
/// @throws std::runtime_error When documentation, inheritance, or targets are invalid.
static void validateDefinitionReferences(const ParseState &state, const fs::path &inputPath) {
    std::vector<std::string> errors;
    std::unordered_map<std::string, const RuntimeClass *> classesByName;
    for (const auto &cls : state.classes)
        classesByName.emplace(cls.name, &cls);
    for (const auto &cls : state.classes) {
        if (cls.documentation.summary.empty())
            errors.push_back("runtime class " + cls.name + " is missing authored @summary text");
        if (cls.documentation.details.empty())
            errors.push_back("runtime class " + cls.name + " is missing authored @details text");
        if (!cls.ctor_id.empty() && cls.ctor_id != "none" &&
            !resolveRuntimeFunc(state, cls.ctor_id)) {
            errors.push_back("runtime class " + cls.name + " has unresolved ctor target " +
                             cls.ctor_id);
        }
        if (!cls.base_name.empty() && !classesByName.contains(cls.base_name)) {
            errors.push_back("runtime class " + cls.name + " has unknown base class " +
                             cls.base_name);
        }
        std::unordered_set<std::string> baseChain;
        const RuntimeClass *cursor = &cls;
        while (cursor && !cursor->base_name.empty()) {
            if (!baseChain.insert(cursor->name).second) {
                errors.push_back("runtime class " + cls.name + " has a cyclic base-class chain");
                break;
            }
            auto base = classesByName.find(cursor->base_name);
            cursor = base == classesByName.end() ? nullptr : base->second;
        }
        for (const auto &prop : cls.props) {
            if (!prop.getter_id.empty() && prop.getter_id != "none" &&
                !resolveRuntimeFunc(state, prop.getter_id)) {
                errors.push_back("runtime property " + cls.name + "." + prop.name +
                                 " has unresolved getter target " + prop.getter_id);
            }
            if (!prop.setter_id.empty() && prop.setter_id != "none" &&
                !resolveRuntimeFunc(state, prop.setter_id)) {
                errors.push_back("runtime property " + cls.name + "." + prop.name +
                                 " has unresolved setter target " + prop.setter_id);
            }
        }
        for (const auto &method : cls.methods) {
            if (!method.target_id.empty() && method.target_id != "none" &&
                !resolveRuntimeFunc(state, method.target_id)) {
                errors.push_back("runtime method " + cls.name + "." + method.name +
                                 " has unresolved target " + method.target_id);
            }
        }
    }

    if (errors.empty())
        return;
    std::sort(errors.begin(), errors.end());
    std::ostringstream message;
    message << zanna::filesystem::pathToUtf8(inputPath)
            << ": error: runtime definition validation found " << errors.size()
            << " unresolved reference(s)";
    for (const auto &error : errors)
        message << "\n  " << error;
    throw std::runtime_error(message.str());
}

//===----------------------------------------------------------------------===//
// Type Mapping (IL signature types to C types)
//===----------------------------------------------------------------------===//

/// @brief Map IL type to C type for DirectHandler template.
/// @param ilType IL scalar type, optionally carrying generic arguments.
/// @return Corresponding C/C++ type spelling.
/// @throws std::runtime_error When the base IL type is unknown.
static std::string ilTypeToCType(const std::string &ilType) {
    std::string baseType = ilType;
    size_t langle = baseType.find('<');
    if (langle != std::string::npos)
        baseType.resize(langle);

    if (baseType == "str")
        return "rt_string";
    if (baseType == "i64")
        return "int64_t";
    if (baseType == "i32")
        return "int32_t";
    if (baseType == "i16")
        return "int16_t";
    if (baseType == "i8" || baseType == "i1")
        return "int8_t";
    if (baseType == "f64")
        return "double";
    if (baseType == "f32")
        return "float";
    if (baseType == "void")
        return "void";
    if (baseType == "bool")
        return "int8_t";
    if (baseType == "obj" || baseType == "ptr")
        return "void *";
    throw std::runtime_error("unknown IL type in runtime signature: " + ilType);
}

/// @brief Return the base IL type with any `<...>` generic arguments removed.
/// @param ilType IL type spelling.
/// @return Base type before the first opening angle bracket.
static std::string stripTypeArgs(const std::string &ilType) {
    size_t langle = ilType.find('<');
    if (langle == std::string::npos)
        return ilType;
    return ilType.substr(0, langle);
}

/// @brief Return the text between the outermost `<` and `>` of an IL type, or "".
/// @param ilType Possibly generic IL type spelling.
/// @return Outer generic argument text, or an empty string when absent/malformed.
static std::string extractTypeArg(const std::string &ilType) {
    size_t langle = ilType.find('<');
    size_t rangle = ilType.rfind('>');
    if (langle == std::string::npos || rangle == std::string::npos || rangle <= langle)
        return {};
    return ilType.substr(langle + 1, rangle - langle - 1);
}

/// @brief Map IL type to signature string format.
/// @param ilType IL type spelling, optionally generic or nullable.
/// @return Runtime signature type spelling with Boolean and string normalization.
static std::string ilTypeToSigType(const std::string &ilType) {
    // Handle optional return types (trailing '?') — at the IL level, optional
    // reference types keep their inner type (null pointer = none).
    if (!ilType.empty() && ilType.back() == '?')
        return ilTypeToSigType(ilType.substr(0, ilType.size() - 1));

    std::string baseType = stripTypeArgs(ilType);
    if (baseType == "str")
        return "string";
    if (baseType == "obj")
        return ilType;
    if (baseType == "ptr")
        return "ptr";
    if (baseType == "bool")
        return "i1";
    // Most types map directly
    return baseType;
}

/// @brief A runtime signature split into its return type and argument types.
struct ParsedSignature {
    std::string returnType;            ///< Return type text (before the '(').
    std::vector<std::string> argTypes; ///< Argument type texts, in order.
};

/// @brief Parse a signature like "str(i64,str)" into return type and arg types.
/// @details A signature with no '(' is treated as a bare return type with no args.
/// @param sig Runtime signature text.
/// @return Parsed return type and top-level argument types.
static ParsedSignature parseSignature(const std::string &sig) {
    ParsedSignature result;

    // Find the opening paren
    size_t parenPos = sig.find('(');
    if (parenPos == std::string::npos) {
        result.returnType = sig;
        return result;
    }

    result.returnType = sig.substr(0, parenPos);

    // Extract args between parens
    size_t closePos = sig.rfind(')');
    if (closePos == std::string::npos || closePos <= parenPos + 1) {
        return result; // No args
    }

    std::string argsStr = sig.substr(parenPos + 1, closePos - parenPos - 1);
    if (argsStr.empty()) {
        return result; // Empty args "()"
    }

    for (auto &arg : splitTopLevel(argsStr, ',')) {
        if (!arg.empty()) {
            result.argTypes.push_back(std::move(arg));
        }
    }

    return result;
}

/// @brief Return true if the signature's return or any argument is a raw `ptr`.
/// @details Used by the Zia bridge to flag functions that expose unsafe pointers.
/// @param sig Runtime signature to inspect.
/// @return @c true when a return or parameter base type is @c ptr.
static bool signatureExposesRawPointer(const std::string &sig) {
    ParsedSignature parsed = parseSignature(sig);
    if (stripTypeArgs(parsed.returnType) == "ptr")
        return true;
    for (const auto &arg : parsed.argTypes) {
        if (stripTypeArgs(arg) == "ptr")
            return true;
    }
    return false;
}

//===----------------------------------------------------------------------===//
// Runtime signature helpers
//===----------------------------------------------------------------------===//

/// @brief Read the signature names (first field of each SIG(...) row) from a
///        RuntimeSigs.def file, in declaration order.
/// @param path RuntimeSigs.def path.
/// @return Signature identifiers in declaration order.
/// @throws std::runtime_error When the file cannot be opened.
static std::vector<std::string> parseRtSigNames(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read " + zanna::filesystem::pathToUtf8(path));
    }

    std::vector<std::string> names;
    std::string line;
    while (std::getline(in, line)) {
        std::string_view view = trimView(line);
        if (!startsWith(view, "SIG"))
            continue;

        auto parens = extractParens(view, "SIG");
        if (!parens)
            continue;
        auto parts = split(*parens, ',');
        if (parts.empty())
            continue;
        names.push_back(parts[0]);
    }
    return names;
}

/// @brief Read the ordered symbol-name string list from the kRtSigSymbolNames
///        array in RuntimeSignaturesData.hpp.
/// @details Locates the `kRtSigSymbolNames { ... }` block and extracts each
///          double-quoted string; returns empty if the marker is absent.
/// @param path RuntimeSignaturesData.hpp path.
/// @return Symbol strings in array order, or an empty vector when unrecognized.
/// @throws std::runtime_error When the file cannot be opened.
static std::vector<std::string> parseRtSigSymbols(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read " + zanna::filesystem::pathToUtf8(path));
    }

    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string marker = "kRtSigSymbolNames";
    size_t start = contents.find(marker);
    if (start == std::string::npos) {
        return {};
    }

    start = contents.find('{', start);
    if (start == std::string::npos)
        return {};
    size_t end = contents.find("};", start);
    if (end == std::string::npos)
        return {};

    std::string_view block = std::string_view(contents).substr(start + 1, end - start - 1);
    std::vector<std::string> symbols;
    std::string current;
    bool in_quotes = false;
    for (size_t i = 0; i < block.size(); ++i) {
        char c = block[i];
        if (c == '"' && (i == 0 || block[i - 1] != '\\')) {
            if (in_quotes) {
                symbols.push_back(current);
                current.clear();
            }
            in_quotes = !in_quotes;
            continue;
        }
        if (in_quotes)
            current.push_back(c);
    }
    return symbols;
}

/// @brief Build a map from runtime symbol name to its RtSig:: enum expression.
/// @details Pairs the names from RuntimeSigs.def with the symbol strings from
///          RuntimeSignaturesData.hpp positionally; a length mismatch is fatal.
/// @param runtimeDir Directory containing both signature sources.
/// @return Map from C runtime symbol to generated @c RtSig expression.
/// @throws std::runtime_error When either source is unreadable or counts differ.
static std::unordered_map<std::string, std::string> buildRtSigMap(const fs::path &runtimeDir) {
    const fs::path sigsPath = runtimeDir / "RuntimeSigs.def";
    const fs::path dataPath = runtimeDir / "RuntimeSignaturesData.hpp";
    std::vector<std::string> sigNames = parseRtSigNames(sigsPath);
    std::vector<std::string> sigSymbols = parseRtSigSymbols(dataPath);

    if (sigNames.size() != sigSymbols.size()) {
        throw std::runtime_error("RuntimeSigs.def and RuntimeSignaturesData.hpp mismatch");
    }

    std::unordered_map<std::string, std::string> result;
    for (size_t i = 0; i < sigNames.size(); ++i) {
        result[sigSymbols[i]] = "RtSig::" + sigNames[i];
    }
    return result;
}

/// @brief Build the C++ expression that indexes the spec table by signature id.
/// @param sigId C++ expression naming an @c RtSig enumerator.
/// @return Generated spec-table lookup expression.
static std::string buildSigSpecExpr(const std::string &sigId) {
    return "data::kRtSigSpecs[static_cast<std::size_t>(" + sigId + ")]";
}

/// @brief Remove // line comments and block comments from C source text.
/// @details Quote handling is intentionally simplistic — adequate for scanning
///          runtime headers for declarations, not for full C tokenization.
/// @param input Source text to filter.
/// @return Source text with comment bytes removed and line-comment newlines retained.
static std::string stripComments(const std::string &input) {
    std::string out;
    out.reserve(input.size());
    bool in_line = false;
    bool in_block = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        char next = (i + 1 < input.size()) ? input[i + 1] : '\0';

        if (in_line) {
            if (c == '\n') {
                in_line = false;
                out.push_back(c);
            }
            continue;
        }

        if (in_block) {
            if (c == '*' && next == '/') {
                in_block = false;
                ++i;
            }
            continue;
        }

        if (c == '/' && next == '/') {
            in_line = true;
            ++i;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block = true;
            ++i;
            continue;
        }

        out.push_back(c);
    }
    return out;
}

/// @brief Return true if @p line ends with a backslash (continues a directive).
/// @param line Source line, possibly ending in horizontal whitespace.
/// @return @c true when the last non-whitespace byte is a backslash.
static bool lineContinuesPreprocessorDirective(std::string_view line) {
    size_t end = line.find_last_not_of(" \t\r");
    return end != std::string_view::npos && line[end] == '\\';
}

/// @brief Remove preprocessor directive lines (and their `\`-continuations).
/// @details Drops any line whose first non-space character is `#`, so header
///          scanning sees only declaration text.
/// @param input Source text to filter.
/// @return Text containing only non-directive lines, each newline-terminated.
static std::string stripPreprocessor(const std::string &input) {
    std::ostringstream out;
    std::istringstream in(input);
    std::string line;
    bool inDirectiveContinuation = false;
    while (std::getline(in, line)) {
        if (inDirectiveContinuation) {
            inDirectiveContinuation = lineContinuesPreprocessorDirective(line);
            continue;
        }

        std::string_view trimmed = trimView(line);
        if (!trimmed.empty() && trimmed.front() == '#') {
            inDirectiveContinuation = lineContinuesPreprocessorDirective(line);
            continue;
        }
        out << line << '\n';
    }
    return out.str();
}

/// @brief Read an entire text file into a string with a generator-local size cap.
/// @details Runtime definition and header scans are expected to be small source
///          files. The cap prevents accidental reads of huge generated artifacts
///          or device files when an input path is wrong.
/// @param path File to size-check and read.
/// @return Complete file contents.
/// @throws std::runtime_error On stat/read failure or when the size cap is exceeded.
static std::string readTextFile(const fs::path &path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec)
        throw std::runtime_error("cannot stat " + zanna::filesystem::pathToUtf8(path) + ": " +
                                 ec.message());
    if (size > kMaxRtgenTextFileBytes)
        throw std::runtime_error("rtgen input file is too large: " +
                                 zanna::filesystem::pathToUtf8(path));
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read " + zanna::filesystem::pathToUtf8(path));
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// @brief Atomically write one generated rtgen output file.
/// @details All generator functions build complete text in memory and call this
///          helper once. That keeps stale output intact if a later write fails and
///          avoids consumers observing partially-generated include files.
/// @param path Destination generated-file path.
/// @param contents Complete buffered output.
/// @throws std::runtime_error When the atomic packaging helper cannot write.
static void writeGeneratedTextFile(const fs::path &path, const std::ostringstream &contents) {
    zanna::pkg::writeTextFileAtomic(path, contents.str());
}

/// @brief Normalize @p path and return it with forward slashes.
/// @param path Filesystem path to normalize lexically.
/// @return UTF-8 generic path spelling.
static std::string pathToGenericString(const fs::path &path) {
    return zanna::filesystem::genericPathToUtf8(path.lexically_normal());
}

/// @brief Return @p path relative to @p base in forward-slash form (absolute if
///        no relative path can be formed).
/// @param path Path to express.
/// @param base Base directory for lexical relativization.
/// @return Generic relative spelling, or normalized original path if unavailable.
static std::string relativePathString(const fs::path &path, const fs::path &base) {
    fs::path rel = path.lexically_relative(base);
    if (rel.empty())
        return pathToGenericString(path);
    return pathToGenericString(rel);
}

/// @brief Return true if @p c is a C identifier character (alphanumeric or '_').
/// @param c Candidate source byte.
/// @return @c true for an alphanumeric byte or underscore.
static bool isIdentifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// @brief Return true if a whole-token `rt_` symbol begins at @p pos in @p text.
/// @details Requires `rt_` not to be preceded by an identifier char and to be
///          followed by one, so it matches runtime symbol names but not substrings.
/// @param text Source text to inspect.
/// @param pos Candidate byte offset of @c rt_.
/// @return @c true when a complete runtime-symbol token begins at @p pos.
static bool isRuntimeSymbolAt(const std::string &text, size_t pos) {
    if (pos + 3 > text.size() || text.compare(pos, 3, "rt_") != 0)
        return false;
    if (pos > 0 && isIdentifierChar(text[pos - 1]))
        return false;
    if (pos + 3 >= text.size() || !isIdentifierChar(text[pos + 3]))
        return false;
    return true;
}

/// @brief Find the ')' matching the '(' at @p openPos, honoring nesting and quotes.
/// @param text Source text containing the opening parenthesis.
/// @param openPos Byte offset of the opening parenthesis.
/// @return Index of the matching ')', or std::string::npos if unbalanced.
static size_t findMatchingParen(const std::string &text, size_t openPos) {
    int depth = 1;
    bool inQuotes = false;
    for (size_t cursor = openPos + 1; cursor < text.size(); ++cursor) {
        char c = text[cursor];
        if (c == '"' && (cursor == 0 || text[cursor - 1] != '\\')) {
            inQuotes = !inQuotes;
        } else if (!inQuotes) {
            if (c == '(')
                ++depth;
            else if (c == ')' && --depth == 0)
                return cursor;
        }
    }
    return std::string::npos;
}

/// @brief Scan backwards from a symbol to the start of its declaration.
/// @details Stops at the previous statement boundary (`;`, `{`, or `}`) so the
///          text in between can be parsed as the return-type qualifier list.
/// @param text Source text.
/// @param symbolPos Byte offset of the declaration's runtime symbol.
/// @return Byte offset immediately after the preceding statement boundary.
static size_t findDeclarationStart(const std::string &text, size_t symbolPos) {
    size_t start = symbolPos;
    while (start > 0) {
        char c = text[start - 1];
        if (c == ';' || c == '{' || c == '}')
            break;
        --start;
    }
    return start;
}

/// @brief Trim a declaration's return type and strip leading storage/qualifier
///        keywords (extern, static, inline, _Noreturn).
/// @param retType Raw declaration prefix.
/// @return Normalized C return-type spelling.
static std::string normalizeReturnType(std::string retType) {
    retType = trim(retType);

    // Keep the parser tolerant of annotations that appear on their own line.
    if (size_t newline = retType.find_last_of("\r\n"); newline != std::string::npos)
        retType = trim(retType.substr(newline + 1));

    bool changed = true;
    while (changed) {
        changed = false;
        for (const char *kw : {"extern ", "_Noreturn ", "static ", "inline "}) {
            std::string kwStr(kw);
            if (retType.substr(0, kwStr.size()) == kwStr) {
                retType = trim(retType.substr(kwStr.size()));
                changed = true;
            }
        }
    }
    return retType;
}

/// @brief Scan all runtime headers under @p runtimeDir for `rt_*(...)` prototypes.
/// @details Strips comments/preprocessor lines, then for each whole-token rt_
///          symbol followed by a parenthesised, semicolon-terminated declaration
///          records its return type, argument types, and parameter names.
/// @param runtimeDir Directory tree of runtime .h/.hpp files to scan.
/// @param repoRoot Repository root, used to record header paths relatively.
/// @return Map from runtime symbol name to its parsed prototype.
static std::unordered_map<std::string, RuntimePrototype> loadRuntimeHeaderDeclarations(
    const fs::path &runtimeDir, const fs::path &repoRoot) {
    std::unordered_map<std::string, RuntimePrototype> result;
    if (!fs::exists(runtimeDir))
        return result;

    for (const auto &entry : fs::recursive_directory_iterator(runtimeDir)) {
        if (!entry.is_regular_file())
            continue;
        const fs::path path = entry.path();
        if (path.extension() != ".h" && path.extension() != ".hpp")
            continue;

        std::ifstream in(path);
        if (!in)
            continue;

        std::string contents((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        contents = stripComments(contents);
        contents = stripPreprocessor(contents);

        for (size_t pos = 0; (pos = contents.find("rt_", pos)) != std::string::npos;) {
            if (!isRuntimeSymbolAt(contents, pos)) {
                pos += 3;
                continue;
            }

            size_t nameEnd = pos + 3;
            while (nameEnd < contents.size() && isIdentifierChar(contents[nameEnd]))
                ++nameEnd;

            size_t cursor = nameEnd;
            while (cursor < contents.size() &&
                   std::isspace(static_cast<unsigned char>(contents[cursor])))
                ++cursor;
            if (cursor >= contents.size() || contents[cursor] != '(') {
                pos = nameEnd;
                continue;
            }

            size_t close = findMatchingParen(contents, cursor);
            if (close == std::string::npos) {
                pos = nameEnd;
                continue;
            }

            size_t afterClose = close + 1;
            while (afterClose < contents.size() &&
                   std::isspace(static_cast<unsigned char>(contents[afterClose])))
                ++afterClose;
            if (afterClose >= contents.size() || contents[afterClose] != ';') {
                pos = nameEnd;
                continue;
            }

            const size_t declStart = findDeclarationStart(contents, pos);
            std::string retType = normalizeReturnType(contents.substr(declStart, pos - declStart));
            if (retType.find('=') != std::string::npos) {
                pos = afterClose + 1;
                continue;
            }
            std::string funcName = contents.substr(pos, nameEnd - pos);
            std::string argsStr = contents.substr(cursor + 1, close - cursor - 1);

            auto existing = result.find(funcName);
            bool haveExisting = existing != result.end();

            RuntimePrototype proto;
            CSignature sig;
            sig.returnType = retType;
            std::vector<std::string> args = splitTopLevel(argsStr, ',');
            for (const auto &arg : args) {
                std::string type = stripParamName(arg);
                if (type.empty() || type == "void")
                    continue;
                sig.argTypes.push_back(type);
                proto.paramNames.push_back(extractParamName(arg));
            }

            if (sig.returnType.empty()) {
                pos = afterClose + 1;
                continue;
            }

            proto.signature = std::move(sig);
            proto.headerPath = relativePathString(path, repoRoot);
            if (!haveExisting) {
                result.emplace(funcName, std::move(proto));
            } else if (existing->second.signature.returnType.empty()) {
                existing->second = std::move(proto);
            }

            pos = afterClose + 1;
        }
    }

    return result;
}

/// @brief Convenience over loadRuntimeHeaderDeclarations returning just signatures.
/// @param runtimeDir Directory tree of runtime headers.
/// @param repoRoot Repository root used for relative declaration metadata.
/// @return Map from runtime symbol name to its parsed C signature.
static std::unordered_map<std::string, CSignature> loadRuntimeCSignatures(
    const fs::path &runtimeDir, const fs::path &repoRoot) {
    std::unordered_map<std::string, CSignature> result;
    auto decls = loadRuntimeHeaderDeclarations(runtimeDir, repoRoot);
    for (auto &[symbol, proto] : decls) {
        result.emplace(symbol, std::move(proto.signature));
    }
    return result;
}

/// @brief Collect the set of all `rt_*` symbol tokens referenced in runtime .c/.cpp.
/// @details Used by the audit to detect runtime functions that exist in source but
///          are missing from runtime.def (or vice versa).
/// @param runtimeDir Directory tree containing runtime implementation sources.
/// @return Unique whole-token runtime symbol names found in supported source files.
static std::unordered_set<std::string> loadRuntimeSourceTokens(const fs::path &runtimeDir) {
    std::unordered_set<std::string> tokens;
    if (!fs::exists(runtimeDir))
        return tokens;

    for (const auto &entry : fs::recursive_directory_iterator(runtimeDir)) {
        if (!entry.is_regular_file())
            continue;
        const fs::path path = entry.path();
        const fs::path ext = path.extension();
        // .inc and .m are runtime implementation sources too: large 3D translation units are
        // split into cohesive .inc fragments (#included into their .c), and the Metal backend
        // is .m. Implementation tokens can therefore live in any of these.
        if (ext != ".c" && ext != ".cpp" && ext != ".inc" && ext != ".m")
            continue;

        const std::string text = readTextFile(path);
        for (size_t pos = 0; (pos = text.find("rt_", pos)) != std::string::npos;) {
            if (!isRuntimeSymbolAt(text, pos)) {
                pos += 3;
                continue;
            }
            size_t end = pos + 3;
            while (end < text.size() && isIdentifierChar(text[end]))
                ++end;
            tokens.insert(text.substr(pos, end - pos));
            pos = end;
        }
    }
    return tokens;
}

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

/// @brief A flattened runtime function row used during code generation.
struct RuntimeEntry {
    std::string name;      ///< Canonical Zanna.* name.
    std::string c_symbol;  ///< C runtime symbol.
    std::string signature; ///< IL type signature.
    std::string lowering;  ///< "always" or "" (default: manual)
};

/// @brief Build the standard "AUTO-GENERATED — DO NOT EDIT" banner for an .inc file.
/// @param filename Logical name of the generated file (for the header).
/// @param purpose One-line description of the file's contents.
/// @return The comment-block header text, including a trailing blank line.
static std::string fileHeader(const std::string &filename, const std::string &purpose) {
    std::ostringstream out;
    out << "//===----------------------------------------------------------------------===//\n";
    out << "//\n";
    out << "// AUTO-GENERATED FILE - DO NOT EDIT\n";
    out << "// Generated by rtgen from runtime.def\n";
    out << "//\n";
    out << "//===----------------------------------------------------------------------===//\n";
    out << "//\n";
    out << "// File: " << filename << "\n";
    out << "// Purpose: " << purpose << "\n";
    out << "//\n";
    out << "//===----------------------------------------------------------------------===//\n\n";
    return out.str();
}

/// @brief Resolve a function id or canonical name to its RuntimeFunc.
/// @param state Parsed function storage and lookup indices.
/// @param idOrCanonical Definition ID or canonical runtime name.
/// @return Pointer into @p state, or nullptr when neither lookup matches.
static const RuntimeFunc *resolveRuntimeFunc(const ParseState &state,
                                             const std::string &idOrCanonical) {
    if (auto it = state.func_by_id.find(idOrCanonical); it != state.func_by_id.end())
        return &state.functions[it->second];
    if (auto it = state.func_by_canonical.find(idOrCanonical); it != state.func_by_canonical.end())
        return &state.functions[it->second];
    return nullptr;
}

/// @brief Resolve an id/canonical reference to its canonical name.
/// @param state Parsed function lookup state.
/// @param idOrCanonical Function ID, canonical name, @c none, or empty text.
/// @return The canonical name; an empty string for "none"/empty; nullopt when the
///         reference does not resolve.
static std::optional<std::string> resolveRuntimeCanonical(const ParseState &state,
                                                          const std::string &idOrCanonical) {
    if (idOrCanonical.empty() || idOrCanonical == "none")
        return std::string();
    if (const auto *fn = resolveRuntimeFunc(state, idOrCanonical))
        return fn->canonical;
    return std::nullopt;
}

/// @brief Resolve an id/canonical reference to its underlying C symbol.
/// @param state Parsed function lookup state.
/// @param idOrCanonical Function ID, canonical name, @c none, or empty text.
/// @return The C symbol; an empty string for "none"/empty; nullopt when the
///         reference does not resolve.
static std::optional<std::string> resolveRuntimeSymbol(const ParseState &state,
                                                       const std::string &idOrCanonical) {
    if (idOrCanonical.empty() || idOrCanonical == "none")
        return std::string();
    if (const auto *fn = resolveRuntimeFunc(state, idOrCanonical))
        return fn->c_symbol;
    return std::nullopt;
}

/// @brief Return the final dot-separated segment of @p dotted (e.g. the method name).
/// @param dotted Qualified dotted name.
/// @return Owned final segment, or the complete input when no dot exists.
static std::string lastSegment(std::string_view dotted) {
    size_t pos = dotted.rfind('.');
    if (pos == std::string_view::npos)
        return std::string(dotted);
    return std::string(dotted.substr(pos + 1));
}

/// @brief Build a method-slot key from a (case-insensitive name, signature) pair.
/// @details Used to deduplicate methods across declared, constructor, and
///          synthesized entries when resolving a class.
/// @param name Method name to lowercase for slot identity.
/// @param signature Method signature retained verbatim.
/// @return Stable lowercase-name and signature composite key.
static std::string methodSlotKey(std::string_view name, std::string_view signature) {
    std::string key;
    key.reserve(name.size() + signature.size() + 1);
    for (unsigned char c : name)
        key.push_back(static_cast<char>(std::tolower(c)));
    key.push_back('|');
    key.append(signature);
    return key;
}

/// @brief Resolve every parsed class into a ResolvedRuntimeClass for codegen.
/// @details For each class this resolves ctor/getter/setter/method id references
///          to canonical names, then synthesizes method entries for class-local
///          functions whose canonical name is prefixed by the class (excluding
///          get_/set_ accessors and already-covered slots), deduplicating by
///          canonical name and method slot.
/// @param state Parsed functions and class blocks.
/// @return Classes with every function reference converted to canonical names.
static std::vector<ResolvedRuntimeClass> buildResolvedClasses(const ParseState &state) {
    std::vector<ResolvedRuntimeClass> resolved;
    resolved.reserve(state.classes.size());

    for (const auto &cls : state.classes) {
        ResolvedRuntimeClass outClass;
        outClass.name = cls.name;
        outClass.type_id = cls.type_id;
        outClass.layout = cls.layout;
        outClass.baseName = cls.base_name;
        outClass.documentation = cls.documentation;
        if (auto ctorCanonical = resolveRuntimeCanonical(state, cls.ctor_id))
            outClass.ctorCanonical = *ctorCanonical;

        for (const auto &prop : cls.props) {
            ResolvedRuntimeProperty outProp;
            outProp.name = prop.name;
            outProp.type = prop.type;
            if (auto getterCanonical = resolveRuntimeCanonical(state, prop.getter_id))
                outProp.getterCanonical = *getterCanonical;
            else
                outProp.getterCanonical = prop.getter_id;
            if (auto setterCanonical = resolveRuntimeCanonical(state, prop.setter_id))
                outProp.setterCanonical = *setterCanonical;
            else
                outProp.setterCanonical = prop.setter_id;
            outClass.props.push_back(std::move(outProp));
        }

        std::vector<ResolvedRuntimeMethod> methods;
        methods.reserve(cls.methods.size() + 4);
        std::unordered_set<std::string> coveredCanonicals;
        std::unordered_set<std::string> coveredMethodSlots;

        for (const auto &prop : cls.props) {
            if (auto getterCanonical = resolveRuntimeCanonical(state, prop.getter_id);
                getterCanonical && !getterCanonical->empty()) {
                coveredCanonicals.insert(*getterCanonical);
            }
            if (auto setterCanonical = resolveRuntimeCanonical(state, prop.setter_id);
                setterCanonical && !setterCanonical->empty()) {
                coveredCanonicals.insert(*setterCanonical);
            }
        }

        for (const auto &method : cls.methods) {
            ResolvedRuntimeMethod outMethod;
            outMethod.name = method.name;
            outMethod.signature = method.signature;
            if (auto targetCanonical = resolveRuntimeCanonical(state, method.target_id))
                outMethod.targetCanonical = *targetCanonical;
            else
                outMethod.targetCanonical = method.target_id;
            coveredMethodSlots.insert(methodSlotKey(outMethod.name, outMethod.signature));
            if (!outMethod.targetCanonical.empty())
                coveredCanonicals.insert(outMethod.targetCanonical);
            methods.push_back(std::move(outMethod));
        }

        const std::string classPrefix = cls.name + ".";
        if (!outClass.ctorCanonical.empty() && startsWith(outClass.ctorCanonical, classPrefix)) {
            const std::string ctorMethodName = outClass.ctorCanonical.substr(classPrefix.size());
            bool hasCtorMethod = false;
            for (const auto &method : methods) {
                if (method.targetCanonical == outClass.ctorCanonical) {
                    hasCtorMethod = true;
                    break;
                }
            }
            if (!hasCtorMethod && ctorMethodName.find('.') == std::string::npos) {
                if (const auto *ctorFunc = resolveRuntimeFunc(
                        state, cls.ctor_id.empty() ? outClass.ctorCanonical : cls.ctor_id)) {
                    ResolvedRuntimeMethod ctorMethod;
                    ctorMethod.name = ctorMethodName;
                    ctorMethod.signature = ctorFunc->signature;
                    ctorMethod.targetCanonical = ctorFunc->canonical;
                    coveredMethodSlots.insert(methodSlotKey(ctorMethod.name, ctorMethod.signature));
                    coveredCanonicals.insert(ctorMethod.targetCanonical);
                    methods.push_back(std::move(ctorMethod));
                }
            }
        }

        for (const auto &fn : state.functions) {
            if (!startsWith(fn.canonical, classPrefix))
                continue;
            std::string methodName = fn.canonical.substr(classPrefix.size());
            if (methodName.find('.') != std::string::npos)
                continue;
            if (coveredCanonicals.count(fn.canonical))
                continue;
            if (startsWith(methodName, "get_") || startsWith(methodName, "set_"))
                continue;
            if (coveredMethodSlots.count(methodSlotKey(methodName, fn.signature)))
                continue;

            ResolvedRuntimeMethod synthetic;
            synthetic.name = std::move(methodName);
            synthetic.signature = fn.signature;
            synthetic.targetCanonical = fn.canonical;
            coveredCanonicals.insert(synthetic.targetCanonical);
            coveredMethodSlots.insert(methodSlotKey(synthetic.name, synthetic.signature));
            methods.push_back(std::move(synthetic));
        }

        outClass.methods = std::move(methods);
        resolved.push_back(std::move(outClass));
    }

    return resolved;
}

/// @brief Invoke @p handler with the argument text of every @p macroName call in @p text.
/// @details Recognises whole-token macro names followed by a parenthesised
///          argument list (respecting nesting and quotes); an unterminated call is
///          a fatal error. Used to scan the runtime surface-policy file.
/// @param text Source text to scan.
/// @param macroName Macro identifier to match.
/// @param handler Callback receiving the raw text between the parentheses.
static void scanMacroCalls(const std::string &text,
                           std::string_view macroName,
                           const std::function<void(std::string_view)> &handler) {
    size_t pos = 0;
    while ((pos = text.find(macroName, pos)) != std::string::npos) {
        if (pos > 0) {
            char prev = text[pos - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_') {
                pos += macroName.size();
                continue;
            }
        }

        size_t cursor = pos + macroName.size();
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])))
            ++cursor;
        if (cursor >= text.size() || text[cursor] != '(') {
            pos += macroName.size();
            continue;
        }

        size_t argsStart = cursor + 1;
        int depth = 1;
        bool inQuotes = false;
        ++cursor;
        for (; cursor < text.size() && depth > 0; ++cursor) {
            char c = text[cursor];
            if (c == '"' && (cursor == 0 || text[cursor - 1] != '\\')) {
                inQuotes = !inQuotes;
            } else if (!inQuotes) {
                if (c == '(')
                    ++depth;
                else if (c == ')')
                    --depth;
            }
        }
        if (depth != 0) {
            throw std::runtime_error("unterminated " + std::string(macroName) +
                                     " macro in runtime surface policy");
        }

        handler(std::string_view(text).substr(argsStart, cursor - argsStart - 1));
        pos = cursor;
    }
}

/// @brief Parse the runtime surface-policy file into a RuntimeSurfacePolicy.
/// @details Scans for the RUNTIME_SURFACE_* macros declaring internal headers,
///          internal symbols, and expected functions/methods/properties; a
///          missing file yields an empty (permissive) policy. Used by the audit.
/// @param policyPath Runtime surface-policy header to scan.
/// @return Parsed exclusions and required surface entries.
/// @throws std::runtime_error On malformed macro calls or unreadable input.
static RuntimeSurfacePolicy parseRuntimeSurfacePolicy(const fs::path &policyPath) {
    RuntimeSurfacePolicy policy;
    if (!fs::exists(policyPath))
        return policy;

    std::string text = stripComments(readTextFile(policyPath));

    /// @brief Parse one internal-header policy macro invocation.
    /// @param argsView Comma-separated macro argument text.
    /// @throws std::runtime_error If the invocation does not contain exactly one argument.
    scanMacroCalls(text, "RUNTIME_SURFACE_INTERNAL_HEADER", [&](std::string_view argsView) {
        auto parts = split(argsView, ',');
        if (parts.size() != 1) {
            throw std::runtime_error("RUNTIME_SURFACE_INTERNAL_HEADER requires 1 argument");
        }
        policy.internalHeaders.insert(pathToGenericString(stripQuotes(parts[0])));
    });

    /// @brief Parse one internal-symbol policy macro invocation.
    /// @param argsView Comma-separated macro argument text.
    /// @throws std::runtime_error If the invocation does not contain exactly one argument.
    scanMacroCalls(text, "RUNTIME_SURFACE_INTERNAL_SYMBOL", [&](std::string_view argsView) {
        auto parts = split(argsView, ',');
        if (parts.size() != 1) {
            throw std::runtime_error("RUNTIME_SURFACE_INTERNAL_SYMBOL requires 1 argument");
        }
        policy.internalSymbols.insert(stripQuotes(parts[0]));
    });

    /// @brief Parse one expected-function policy macro invocation.
    /// @param argsView Comma-separated macro argument text.
    /// @throws std::runtime_error If the invocation does not contain exactly two arguments.
    scanMacroCalls(text, "RUNTIME_SURFACE_EXPECT_FUNCTION", [&](std::string_view argsView) {
        auto parts = split(argsView, ',');
        if (parts.size() != 2) {
            throw std::runtime_error("RUNTIME_SURFACE_EXPECT_FUNCTION requires 2 arguments");
        }
        policy.expectedFunctions.emplace(stripQuotes(parts[0]), stripQuotes(parts[1]));
    });

    /// @brief Parse one expected-method policy macro invocation.
    /// @param argsView Comma-separated macro argument text.
    /// @throws std::runtime_error If the invocation does not contain exactly three arguments.
    scanMacroCalls(text, "RUNTIME_SURFACE_EXPECT_METHOD", [&](std::string_view argsView) {
        auto parts = split(argsView, ',');
        if (parts.size() != 3) {
            throw std::runtime_error("RUNTIME_SURFACE_EXPECT_METHOD requires 3 arguments");
        }
        ResolvedRuntimeMethod method;
        method.name = stripQuotes(parts[1]);
        method.signature = stripQuotes(parts[2]);
        method.targetCanonical = stripQuotes(parts[0]);
        policy.expectedMethods.push_back(std::move(method));
    });

    /// @brief Parse one expected-property policy macro invocation.
    /// @param argsView Comma-separated macro argument text.
    /// @throws std::runtime_error If the invocation does not contain exactly three arguments.
    scanMacroCalls(text, "RUNTIME_SURFACE_EXPECT_PROPERTY", [&](std::string_view argsView) {
        auto parts = split(argsView, ',');
        if (parts.size() != 3) {
            throw std::runtime_error("RUNTIME_SURFACE_EXPECT_PROPERTY requires 3 arguments");
        }
        ResolvedRuntimeProperty prop;
        prop.name = stripQuotes(parts[1]);
        prop.type = stripQuotes(parts[2]);
        prop.getterCanonical = stripQuotes(parts[0]);
        policy.expectedProperties.push_back(std::move(prop));
    });

    return policy;
}

/// @brief Build the `&DirectHandler<...>::invoke` expression for a runtime symbol.
/// @details Instantiates the VM's DirectHandler template with the C symbol, return
///          type, and argument types so the interpreter can call it directly.
/// @param c_symbol Runtime C function symbol.
/// @param sig Parsed C return and argument types.
/// @return Generated direct-handler member-function expression.
static std::string buildDirectHandlerExpr(const std::string &c_symbol, const CSignature &sig) {
    std::string args = "&" + c_symbol + ", " + sig.returnType;
    for (const auto &arg : sig.argTypes) {
        args += ", " + arg;
    }
    return "&DirectHandler<" + args + ">::invoke";
}

/// @brief Build the `&ConsumingStringHandler<...>::invoke` expression.
/// @details Same as buildDirectHandlerExpr but for functions that take ownership
///          of (consume) their string arguments, so the VM retains them first.
/// @param c_symbol Runtime C function symbol.
/// @param sig Parsed C return and argument types.
/// @return Generated consuming-string-handler expression.
static std::string buildConsumingStringHandlerExpr(const std::string &c_symbol,
                                                   const CSignature &sig) {
    std::string args = "&" + c_symbol + ", " + sig.returnType;
    for (const auto &arg : sig.argTypes) {
        args += ", " + arg;
    }
    return "&ConsumingStringHandler<" + args + ">::invoke";
}

/// @brief Check if a runtime function consumes its string arguments.
/// @details Functions like rt_str_concat release their string arguments after use,
///          so the VM must retain them before the call to prevent use-after-free.
/// @param c_symbol Runtime C function symbol.
/// @return @c true when the VM must retain string arguments before invocation.
static bool needsConsumingStringHandler(const std::string &c_symbol) {
    // rt_str_concat releases both of its string arguments after use
    return c_symbol == "rt_str_concat";
}

/// @brief Compute the descriptor fields for one runtime entry's signature row.
/// @details Resolves the signature id/spec (preferring a known RtSig entry, else
///          synthesizing a spec string from the IL signature) and the VM handler
///          expression (direct or consuming-string), leaving lowering/hidden/trap
///          fields at their defaults for the caller to override.
/// @param entry The runtime function being described.
/// @param cSignatures Map of C symbol → parsed C signature (for handler types).
/// @param rtSigMap Map of C symbol → RtSig:: signature-id expression.
/// @return The populated descriptor fields.
static DescriptorFields buildDefaultDescriptor(
    const RuntimeEntry &entry,
    const std::unordered_map<std::string, CSignature> &cSignatures,
    const std::unordered_map<std::string, std::string> &rtSigMap) {
    DescriptorFields fields;

    auto sigIt = rtSigMap.find(entry.c_symbol);
    if (sigIt != rtSigMap.end()) {
        fields.signatureId = sigIt->second;
        fields.spec = buildSigSpecExpr(fields.signatureId);
    } else {
        fields.signatureId = "std::nullopt";
        ParsedSignature parsed = parseSignature(entry.signature);
        std::string sigStr = ilTypeToSigType(parsed.returnType) + "(";
        for (size_t i = 0; i < parsed.argTypes.size(); ++i) {
            if (i > 0)
                sigStr += ", ";
            sigStr += ilTypeToSigType(parsed.argTypes[i]);
        }
        sigStr += ")";
        fields.spec = cppStringLiteral(sigStr);
    }

    auto cSigIt = cSignatures.find(entry.c_symbol);
    bool useConsumingHandler = needsConsumingStringHandler(entry.c_symbol);
    if (cSigIt != cSignatures.end()) {
        fields.handler = useConsumingHandler
                             ? buildConsumingStringHandlerExpr(entry.c_symbol, cSigIt->second)
                             : buildDirectHandlerExpr(entry.c_symbol, cSigIt->second);
    } else {
        ParsedSignature parsed = parseSignature(entry.signature);
        CSignature fallback;
        fallback.returnType = ilTypeToCType(parsed.returnType);
        for (const auto &arg : parsed.argTypes)
            fallback.argTypes.push_back(ilTypeToCType(arg));
        fields.handler = useConsumingHandler
                             ? buildConsumingStringHandlerExpr(entry.c_symbol, fallback)
                             : buildDirectHandlerExpr(entry.c_symbol, fallback);
    }

    fields.lowering = (entry.lowering == "always") ? "kAlwaysLowering" : "kManualLowering";
    fields.hidden = "nullptr";
    fields.hiddenCount = "0";
    fields.trapClass = "RuntimeTrapClass::None";
    fields.publicSurface = "true";
    fields.cSymbol = cppStringLiteral(entry.c_symbol);
    return fields;
}

/// @brief Emit one `DescriptorRow{...}` initializer for the signatures table.
/// @param out Output stream for the generated code.
/// @param name Canonical name for the row.
/// @param fields Pre-computed descriptor fields.
/// @param indent Leading indentation (spaces).
static void emitDescriptorRow(std::ostream &out,
                              const std::string &name,
                              const DescriptorFields &fields,
                              int indent = 4) {
    std::string pad(static_cast<size_t>(indent), ' ');
    out << pad << "DescriptorRow{" << cppStringLiteral(name) << ",\n";
    out << pad << "              " << fields.signatureId << ",\n";
    out << pad << "              " << fields.spec << ",\n";
    out << pad << "              " << fields.handler << ",\n";
    out << pad << "              " << fields.lowering << ",\n";
    out << pad << "              " << fields.hidden << ",\n";
    out << pad << "              " << fields.hiddenCount << ",\n";
    out << pad << "              " << fields.trapClass << ",\n";
    out << pad << "              " << fields.publicSurface << ",\n";
    out << pad << "              " << fields.cSymbol << "},\n";
}

/// @brief Generate RuntimeNameMap.inc: canonical Zanna.* → C rt_* symbol mappings.
/// @details Emits a RUNTIME_NAME_ALIAS row for every canonical function. Fatal
///          error on write failure.
/// @param state Parsed runtime functions in definition order.
/// @param outDir Destination directory for generated artifacts.
static void generateNameMap(const ParseState &state, const fs::path &outDir) {
    fs::path outPath = outDir / "RuntimeNameMap.inc";
    std::ostringstream out;

    out << fileHeader("RuntimeNameMap.inc",
                      "Canonical Zanna.* to C rt_* symbol mapping for native codegen.");

    for (const auto &func : state.functions) {
        out << "RUNTIME_NAME_ALIAS(" << cppStringLiteral(func.canonical) << ", "
            << cppStringLiteral(func.c_symbol) << ")\n";
    }

    writeGeneratedTextFile(outPath, out);
    std::cout << "  Generated " << zanna::filesystem::pathToUtf8(outPath) << "\n";
}

/// @brief Generate RuntimeClasses.inc: the OOP class/property/method catalog.
/// @details Resolves classes via buildResolvedClasses() and emits a RUNTIME_CLASS
///          block per class containing RUNTIME_PROPS and RUNTIME_METHODS lists.
///          Fatal error on write failure.
/// @param state Parsed runtime classes and function lookup state.
/// @param outDir Destination directory for generated artifacts.
static void generateClasses(const ParseState &state, const fs::path &outDir) {
    fs::path outPath = outDir / "RuntimeClasses.inc";
    std::ostringstream out;

    out << fileHeader("RuntimeClasses.inc", "Runtime class catalog with properties and methods.");

    for (const auto &cls : buildResolvedClasses(state)) {
        out << "RUNTIME_CLASS(\n";
        out << "    " << cppStringLiteral(cls.name) << ",\n";
        out << "    RTCLS_" << cls.type_id << ",\n";
        out << "    " << cppStringLiteral(cls.layout) << ",\n";
        out << "    " << cppStringLiteral(cls.baseName) << ",\n";

        if (cls.ctorCanonical.empty()) {
            out << "    " << cppStringLiteral("") << ",\n";
        } else {
            out << "    " << cppStringLiteral(cls.ctorCanonical) << ",\n";
        }

        out << "    " << cppStringLiteral(cls.documentation.summary) << ",\n";
        out << "    " << cppStringLiteral(cls.documentation.details) << ",\n";

        out << "    RUNTIME_PROPS(";
        for (size_t i = 0; i < cls.props.size(); ++i) {
            const auto &prop = cls.props[i];
            if (i > 0)
                out << ",\n                  ";

            out << "RUNTIME_PROP(" << cppStringLiteral(prop.name) << ", "
                << cppStringLiteral(prop.type) << ", " << cppStringLiteral(prop.getterCanonical)
                << ", ";

            if (prop.setterCanonical == "none" || prop.setterCanonical.empty()) {
                out << "nullptr";
            } else {
                out << cppStringLiteral(prop.setterCanonical);
            }
            out << ")";
        }
        out << "),\n";

        out << "    RUNTIME_METHODS(";
        for (size_t i = 0; i < cls.methods.size(); ++i) {
            const auto &method = cls.methods[i];
            if (i > 0)
                out << ",\n                    ";
            out << "RUNTIME_METHOD(" << cppStringLiteral(method.name) << ", "
                << cppStringLiteral(method.signature) << ", "
                << cppStringLiteral(method.targetCanonical) << ")";
        }
        out << "))\n\n";
    }

    writeGeneratedTextFile(outPath, out);
    std::cout << "  Generated " << zanna::filesystem::pathToUtf8(outPath) << "\n";
}

/// @brief Generate RuntimeSignatures.inc: the descriptor row per runtime function.
/// @details Loads C signatures from the runtime headers and the RtSig map, then
///          emits a DescriptorRow for every function via buildDefaultDescriptor()
///          and emitDescriptorRow(). Fatal error on write failure.
/// @param state Parsed runtime definitions.
/// @param outDir Directory to write the .inc file into.
/// @param inputPath Path to runtime.def, used to locate the runtime tree.
static void generateSignatures(const ParseState &state,
                               const fs::path &outDir,
                               const fs::path &inputPath) {
    fs::path outPath = outDir / "RuntimeSignatures.inc";
    const fs::path runtimeDir = inputPath.parent_path();

    std::ostringstream out;

    out << fileHeader("RuntimeSignatures.inc",
                      "Runtime descriptor rows for all runtime functions.");

    const fs::path srcRoot = runtimeDir.parent_path().parent_path();
    const fs::path repoRoot = srcRoot.parent_path();
    const fs::path runtimeHeaders = srcRoot / "runtime";
    auto cSignatures = loadRuntimeCSignatures(runtimeHeaders, repoRoot);
    auto rtSigMap = buildRtSigMap(runtimeDir);

    // Build entries from functions.
    std::unordered_map<std::string, RuntimeEntry> entries;
    entries.reserve(state.functions.size());

    std::unordered_map<std::string, const RuntimeFunc *> cSymbolToFunc;
    for (const auto &func : state.functions) {
        entries.emplace(func.canonical,
                        RuntimeEntry{func.canonical, func.c_symbol, func.signature, func.lowering});
        cSymbolToFunc[func.c_symbol] = &func;
    }

    // Emit canonical entries in definition order
    std::vector<std::string> orderedNames;
    orderedNames.reserve(entries.size());
    for (const auto &func : state.functions)
        orderedNames.push_back(func.canonical);

    for (const auto &name : orderedNames) {
        auto entryIt = entries.find(name);
        if (entryIt == entries.end())
            continue;

        const RuntimeEntry &entry = entryIt->second;
        DescriptorFields fields = buildDefaultDescriptor(entry, cSignatures, rtSigMap);
        if (const auto *func = resolveRuntimeFunc(state, name))
            fields.publicSurface = func->publicSurface ? "true" : "false";
        emitDescriptorRow(out, name, fields);
    }

    writeGeneratedTextFile(outPath, out);
    std::cout << "  Generated " << zanna::filesystem::pathToUtf8(outPath) << "\n";
}

/// @brief Encode Zia extern parameter names into a compact generated string.
/// @details Names are newline-separated. Empty or all-empty name lists encode
///          as an empty string so callers inherit or omit parameter names.
/// @param paramNames Surface parameter names in signature order.
/// @return Newline-separated names, or an empty string when none are meaningful.
static std::string encodeZiaParamNames(const std::vector<std::string> &paramNames) {
    bool hasName = false;
    for (const auto &name : paramNames)
        hasName = hasName || !name.empty();
    if (!hasName)
        return {};

    std::string encoded;
    for (size_t i = 0; i < paramNames.size(); ++i) {
        if (i > 0)
            encoded.push_back('\n');
        encoded += paramNames[i];
    }
    return encoded;
}

/// @brief Encode Zia bridge roles as one character per surface parameter.
/// @details `n` means no role, `c` callback, and `p` payload. All-none role
///          vectors encode as an empty string to keep the generated table small.
/// @param bridgeRoles Explicit roles from RT_BRIDGE in surface order.
/// @param paramCount Number of surface parameters to encode.
/// @return Compact role string, or empty when every role is the default.
static std::string encodeZiaBridgeRoles(const std::vector<std::string> &bridgeRoles,
                                        size_t paramCount) {
    std::string encoded;
    encoded.reserve(paramCount);
    bool hasNonDefaultRole = false;
    for (size_t i = 0; i < paramCount; ++i) {
        std::string role = i < bridgeRoles.size() ? bridgeRoles[i] : "none";
        char code = 'n';
        if (role == "callback") {
            code = 'c';
            hasNonDefaultRole = true;
        } else if (role == "payload") {
            code = 'p';
            hasNonDefaultRole = true;
        }
        encoded.push_back(code);
    }
    return hasNonDefaultRole ? encoded : std::string{};
}

/// @brief Determine the Zia extern's parameter names for a runtime function.
/// @details Takes the trailing surface-parameter names from the C prototype (the
///          last N, since leading C params may be hidden receivers), padding with
///          empty names if the prototype has fewer. Returns empty when there is no
///          prototype or no surface parameters.
/// @param func Runtime definition supplying the surface signature.
/// @param proto Optional parsed C prototype.
/// @return Surface parameter names aligned to the runtime signature.
static std::vector<std::string> ziaExternParamNamesFor(const RuntimeFunc &func,
                                                       const RuntimePrototype *proto) {
    if (!proto)
        return {};

    ParsedSignature sig = parseSignature(func.signature);
    const size_t surfaceCount = sig.argTypes.size();
    if (surfaceCount == 0)
        return {};

    std::vector<std::string> names;
    if (proto->paramNames.size() >= surfaceCount) {
        names.assign(proto->paramNames.end() - static_cast<std::ptrdiff_t>(surfaceCount),
                     proto->paramNames.end());
    } else {
        names = proto->paramNames;
    }

    while (names.size() < surfaceCount)
        names.push_back({});
    return names;
}

/// @brief Generate the Zia extern declarations table for runtime functions.
/// @details Emits each runtime function as a Zia extern with its mapped Zia
///          parameter/return types, parameter names recovered from the C
///          prototype, and pointer-safety/bridge-role metadata. Fatal error on
///          write failure.
/// @param state Parsed public runtime functions.
/// @param outDir Destination directory for the generated include.
/// @param inputPath Root definition path used to locate runtime headers.
static void generateZiaExterns(const ParseState &state,
                               const fs::path &outDir,
                               const fs::path &inputPath) {
    fs::path outPath = outDir / "ZiaRuntimeExterns.inc";
    std::ostringstream out;

    out << fileHeader("ZiaRuntimeExterns.inc",
                      "Zia frontend extern metadata generated from runtime.def.");

    out << "// This file is included by Sema_Runtime.cpp after ZiaRuntimeExternSpec is defined.\n";
    out << "// Usage: #include \"il/runtime/ZiaRuntimeExterns.inc\"\n\n";

    const fs::path runtimeDir = inputPath.parent_path();
    const fs::path srcRoot = runtimeDir.parent_path().parent_path();
    const fs::path repoRoot = srcRoot.parent_path();
    const fs::path runtimeHeaders = srcRoot / "runtime";
    auto headerDecls = loadRuntimeHeaderDeclarations(runtimeHeaders, repoRoot);

    out << "static constexpr ZiaRuntimeExternSpec kZiaRuntimeExterns[] = {\n";

    // Group functions by namespace for readability
    std::map<std::string, std::vector<const RuntimeFunc *>> byNamespace;

    for (const auto &func : state.functions) {
        // Extract namespace: "Zanna.GUI.App.New" -> "Zanna.GUI"
        size_t firstDot = func.canonical.find('.');
        size_t secondDot = func.canonical.find('.', firstDot + 1);
        std::string ns = "Other";
        if (secondDot != std::string::npos) {
            ns = func.canonical.substr(0, secondDot);
        }
        if (func.publicSurface)
            byNamespace[ns].push_back(&func);
    }

    // Keep groups only as comments for generated-file readability. The runtime
    // registration loop in Sema_Runtime.cpp consumes the compact table below.
    for (const auto &[ns, funcs] : byNamespace) {
        out << "    // " << ns << "\n";

        for (const auto *func : funcs) {
            ParsedSignature sig = parseSignature(func->signature);
            std::vector<std::string> paramNames;
            if (auto declIt = headerDecls.find(func->c_symbol); declIt != headerDecls.end()) {
                paramNames = ziaExternParamNamesFor(*func, &declIt->second);
            }
            out << "    {" << cppStringLiteral(func->canonical) << ", "
                << cppStringLiteral(func->signature) << ", "
                << cppStringLiteral(encodeZiaParamNames(paramNames)) << ", "
                << cppStringLiteral(encodeZiaBridgeRoles(func->bridgeRoles, sig.argTypes.size()))
                << "},\n";
        }
    }

    out << "};\n";

    writeGeneratedTextFile(outPath, out);
    std::cout << "  Generated " << zanna::filesystem::pathToUtf8(outPath) << "\n";
}

/// @brief Convert a canonical name to a C++ constant identifier.
/// @details "Zanna.String.Concat" -> "kStringConcat"
///          "Zanna.Time.DateTime.Now" -> "kTimeDateTimeNow"
/// @param canonical Canonical dotted runtime name.
/// @return C++ identifier formed by removing the namespace prefix and separators.
static std::string canonicalToIdentifier(const std::string &canonical) {
    // Skip "Zanna." prefix
    std::string name = canonical;
    if (name.substr(0, 6) == "Zanna.") {
        name = name.substr(6);
    }

    // Convert dots to nothing, capitalize each segment
    std::string result = "k";
    bool capitalizeNext = true;

    for (char c : name) {
        if (c == '.') {
            capitalizeNext = true;
        } else if (c == '_') {
            capitalizeNext = true;
        } else {
            if (capitalizeNext) {
                result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                capitalizeNext = false;
            } else {
                result += c;
            }
        }
    }

    return result;
}

/// @brief Convert a runtime class qualified name to a distinct C++ identifier.
/// @details Class identifiers deliberately use a `kRuntimeClass` prefix so
///          that `Zanna.Collections.List` cannot collide with runtime function
///          constants such as `Zanna.Collections.List.New`.
/// @param qname Fully qualified runtime class name from an RT_CLASS block.
/// @return Constant identifier such as `kRuntimeClassCollectionsList`.
static std::string runtimeClassToIdentifier(const std::string &qname) {
    std::string functionLikeId = canonicalToIdentifier(qname);
    if (!functionLikeId.empty() && functionLikeId.front() == 'k')
        functionLikeId.erase(functionLikeId.begin());
    return "kRuntimeClass" + functionLikeId;
}

/// @brief Generate RuntimeNames.hpp: C++ constants exposing canonical names to the
///        frontends. Fatal error on write failure.
/// @param state Parsed runtime functions and classes to expose.
/// @param outDir Destination directory for the generated header.
static void generateFrontendNames(const ParseState &state, const fs::path &outDir) {
    fs::path outPath = outDir / "RuntimeNames.hpp";
    std::ostringstream out;

    out << "//===----------------------------------------------------------------------===//\n";
    out << "//\n";
    out << "// AUTO-GENERATED FILE - DO NOT EDIT\n";
    out << "// Generated by rtgen from runtime.def\n";
    out << "//\n";
    out << "//===----------------------------------------------------------------------===//\n";
    out << "//\n";
    out << "// File: RuntimeNames.hpp\n";
    out << "// Purpose: Canonical runtime function and class name constants for all frontends.\n";
    out << "//\n";
    out << "// Usage: #include \"il/runtime/RuntimeNames.hpp\"\n";
    out << "//        Then use il::runtime::names::kStringConcat,\n";
    out << "//        il::runtime::names::kRuntimeClassString, etc.\n";
    out << "//\n";
    out << "//===----------------------------------------------------------------------===//\n\n";

    out << "#pragma once\n\n";
    out << "namespace il::runtime::names {\n\n";
    out << "/// @brief Canonical prefix shared by all runtime functions and classes.\n";
    out << "inline constexpr const char *kRuntimeNamespacePrefix = \"Zanna.\";\n\n";

    if (!state.classes.empty()) {
        out << "// " << std::string(75, '=') << "\n";
        out << "// RUNTIME CLASSES\n";
        out << "// " << std::string(75, '=') << "\n\n";

        std::map<std::string, std::vector<const RuntimeClass *>> classesByNamespace;
        std::set<std::string> emittedClassIdentifiers;
        for (const auto &cls : state.classes) {
            size_t lastDot = cls.name.rfind('.');
            std::string ns = "Other";
            if (lastDot != std::string::npos)
                ns = cls.name.substr(0, lastDot);
            classesByNamespace[ns].push_back(&cls);
        }

        for (const auto &[ns, classes] : classesByNamespace) {
            out << "// " << ns << "\n\n";
            for (const auto *cls : classes) {
                std::string id = runtimeClassToIdentifier(cls->name);

                std::string uniqueId = id;
                int suffix = 2;
                while (emittedClassIdentifiers.count(uniqueId)) {
                    uniqueId = id + std::to_string(suffix++);
                }
                emittedClassIdentifiers.insert(uniqueId);

                out << "/// @brief Runtime class " << cls->name << "\n";
                out << "inline constexpr const char *" << uniqueId << " = "
                    << cppStringLiteral(cls->name) << ";\n\n";
            }
        }
    }

    // Group functions by namespace for readability
    std::map<std::string, std::vector<const RuntimeFunc *>> byNamespace;
    std::set<std::string> emittedIdentifiers; // Track duplicates

    for (const auto &func : state.functions) {
        // Extract namespace: "Zanna.String.Concat" -> "Zanna.String"
        size_t lastDot = func.canonical.rfind('.');
        std::string ns = "Other";
        if (lastDot != std::string::npos) {
            ns = func.canonical.substr(0, lastDot);
        }
        byNamespace[ns].push_back(&func);
    }

    for (const auto &[ns, funcs] : byNamespace) {
        out << "// " << std::string(75, '=') << "\n";
        out << "// " << ns << "\n";
        out << "// " << std::string(75, '=') << "\n\n";

        for (const auto *func : funcs) {
            std::string id = canonicalToIdentifier(func->canonical);

            // Handle duplicate identifiers by appending a suffix
            std::string uniqueId = id;
            int suffix = 2;
            while (emittedIdentifiers.count(uniqueId)) {
                uniqueId = id + std::to_string(suffix++);
            }
            emittedIdentifiers.insert(uniqueId);

            out << "/// @brief " << func->canonical << "\n";
            out << "inline constexpr const char *" << uniqueId << " = "
                << cppStringLiteral(func->canonical) << ";\n\n";
        }
    }

    out << "} // namespace il::runtime::names\n";

    writeGeneratedTextFile(outPath, out);
    std::cout << "  Generated " << zanna::filesystem::pathToUtf8(outPath) << "\n";
}

/// @brief Return the first namespace segment beneath `Zanna` for @p name.
/// @param name Canonical runtime function or class name.
/// @return First domain segment after the optional @c Zanna prefix.
static std::string runtimeDocumentationDomain(std::string_view name) {
    constexpr std::string_view prefix = "Zanna.";
    if (startsWith(name, prefix))
        name.remove_prefix(prefix.size());
    const size_t dot = name.find('.');
    return std::string(name.substr(0, dot));
}

/// @brief Build the same lowercase dash-separated anchor used by the API dump.
/// @param text Heading or qualified name to normalize.
/// @return Lowercase alphanumeric slug with separator runs collapsed to dashes.
static std::string runtimeDocumentationSlug(std::string_view text) {
    std::string slug;
    bool previousDash = false;
    for (unsigned char ch : text) {
        if (std::isalnum(ch)) {
            slug.push_back(static_cast<char>(std::tolower(ch)));
            previousDash = false;
        } else if (!previousDash) {
            slug.push_back('-');
            previousDash = true;
        }
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    while (!slug.empty() && slug.front() == '-')
        slug.erase(slug.begin());
    return slug;
}

/// @brief Escape text for a Markdown table cell.
/// @param value Raw cell text.
/// @return Text with pipes escaped and line breaks replaced by spaces.
static std::string markdownTableCell(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '|')
            escaped += "\\|";
        else if (ch == '\n' || ch == '\r')
            escaped.push_back(' ');
        else
            escaped.push_back(ch);
    }
    return escaped;
}

/// @brief Compare or write one deterministic generated documentation file.
/// @param path Documentation file to verify or replace atomically.
/// @param contents Complete expected contents.
/// @param checkOnly Whether to compare without writing.
/// @return @c true when the existing file matches or the generated file was written.
static bool emitRuntimeDocumentationFile(const fs::path &path,
                                         const std::ostringstream &contents,
                                         bool checkOnly) {
    const std::string expected = contents.str();
    if (checkOnly) {
        if (!fs::exists(path)) {
            std::cerr << "error: generated runtime documentation is missing: "
                      << zanna::filesystem::pathToUtf8(path) << "\n";
            return false;
        }
        if (readTextFile(path) != expected) {
            std::cerr << "error: generated runtime documentation is stale: "
                      << zanna::filesystem::pathToUtf8(path) << "\n";
            return false;
        }
        return true;
    }

    std::ostringstream writable;
    writable << expected;
    writeGeneratedTextFile(path, writable);
    std::cout << "  Generated " << zanna::filesystem::pathToUtf8(path) << "\n";
    return true;
}

/// @brief Generate or verify exhaustive Markdown runtime reference pages.
/// @param state Parsed functions and classes to document.
/// @param outDir Documentation directory containing the index and domain pages.
/// @param checkOnly Verify exact contents and stale-file absence without mutation.
/// @return @c true when every expected page is current and no stale page remains.
static bool generateRuntimeDocumentation(const ParseState &state,
                                         const fs::path &outDir,
                                         bool checkOnly) {
    const auto resolvedClasses = buildResolvedClasses(state);
    std::map<std::string, std::vector<const ResolvedRuntimeClass *>> classesByDomain;
    std::map<std::string, std::vector<const RuntimeFunc *>> functionsByDomain;
    for (const auto &cls : resolvedClasses)
        classesByDomain[runtimeDocumentationDomain(cls.name)].push_back(&cls);
    for (const auto &func : state.functions) {
        if (func.publicSurface)
            functionsByDomain[runtimeDocumentationDomain(func.canonical)].push_back(&func);
    }

    std::set<std::string> domains;
    for (const auto &[domain, _] : classesByDomain)
        domains.insert(domain);
    for (const auto &[domain, _] : functionsByDomain)
        domains.insert(domain);

    std::error_code directoryEc;
    if (!checkOnly)
        fs::create_directories(outDir, directoryEc);
    if (directoryEc) {
        std::cerr << "error: cannot create runtime documentation directory "
                  << zanna::filesystem::pathToUtf8(outDir) << ": " << directoryEc.message() << "\n";
        return false;
    }

    std::unordered_set<std::string> expectedFiles{"README.md"};
    bool clean = true;
    std::ostringstream index;
    index << "<!-- AUTO-GENERATED by rtgen from src/il/runtime/runtime.def. DO NOT EDIT. -->\n\n";
    index << "# Zanna Runtime API Reference\n\n";
    index << "This exhaustive reference is generated from the modular runtime definition "
             "registry. Conceptual guides live under "
             "[`docs/zannalib`](../../zannalib/README.md).\n\n";
    index << "| Domain | Classes | Functions |\n";
    index << "|---|---:|---:|\n";

    for (const auto &domain : domains) {
        const std::string filename = runtimeDocumentationSlug(domain) + ".md";
        expectedFiles.insert(filename);
        const auto &classes = classesByDomain[domain];
        const auto &functions = functionsByDomain[domain];
        index << "| [" << markdownTableCell(domain) << "](" << filename << ") | " << classes.size()
              << " | " << functions.size() << " |\n";

        std::ostringstream page;
        page
            << "<!-- AUTO-GENERATED by rtgen from src/il/runtime/runtime.def. DO NOT EDIT. -->\n\n";
        page << "# Zanna " << domain << " Runtime Reference\n\n";
        page << "[Back to the runtime reference index](README.md)\n\n";
        std::unordered_set<std::string> emittedPageAnchors;

        if (!classes.empty()) {
            page << "## Classes\n\n";
            for (const auto *cls : classes) {
                const std::string classAnchor = runtimeDocumentationSlug(cls->name);
                if (emittedPageAnchors.insert(classAnchor).second)
                    page << "<a id=\"" << classAnchor << "\"></a>\n";
                page << "### `" << cls->name << "`\n\n";
                page << cls->documentation.summary << "\n\n";
                page << cls->documentation.details << "\n\n";
                if (!cls->ctorCanonical.empty())
                    page << "Constructor: `" << cls->ctorCanonical << "`\n\n";

                if (!cls->props.empty()) {
                    page << "#### Properties\n\n";
                    page << "| Property | Type | Access |\n";
                    page << "|---|---|---|\n";
                    for (const auto &prop : cls->props) {
                        const std::string qualified = cls->name + "." + prop.name;
                        const std::string anchor = runtimeDocumentationSlug(qualified);
                        page << "| ";
                        if (emittedPageAnchors.insert(anchor).second)
                            page << "<a id=\"" << anchor << "\"></a>";
                        page << "`" << markdownTableCell(prop.name) << "` | `"
                             << markdownTableCell(prop.type) << "` | "
                             << ((prop.setterCanonical.empty() || prop.setterCanonical == "none")
                                     ? "read-only"
                                     : "read/write")
                             << " |\n";
                    }
                    page << "\n";
                }

                if (!cls->methods.empty()) {
                    page << "#### Methods\n\n";
                    page << "| Method | Signature | Runtime target |\n";
                    page << "|---|---|---|\n";
                    for (const auto &method : cls->methods) {
                        const std::string qualified = cls->name + "." + method.name;
                        const std::string anchor = runtimeDocumentationSlug(qualified);
                        page << "| ";
                        if (emittedPageAnchors.insert(anchor).second)
                            page << "<a id=\"" << anchor << "\"></a>";
                        page << "`" << markdownTableCell(method.name) << "` | `"
                             << markdownTableCell(method.signature) << "` | `"
                             << markdownTableCell(method.targetCanonical) << "` |\n";
                    }
                    page << "\n";
                }
            }
        }

        if (!functions.empty()) {
            page << "## Functions\n\n";
            page << "| Function | Signature | Runtime symbol |\n";
            page << "|---|---|---|\n";
            for (const auto *func : functions) {
                const std::string anchor = runtimeDocumentationSlug(func->canonical);
                page << "| ";
                if (emittedPageAnchors.insert(anchor).second)
                    page << "<a id=\"" << anchor << "\"></a>";
                page << "`" << markdownTableCell(func->canonical) << "` | `"
                     << markdownTableCell(func->signature) << "` | `"
                     << markdownTableCell(func->c_symbol) << "` |\n";
            }
            page << "\n";
        }

        clean = emitRuntimeDocumentationFile(outDir / filename, page, checkOnly) && clean;
    }

    clean = emitRuntimeDocumentationFile(outDir / "README.md", index, checkOnly) && clean;

    if (fs::exists(outDir)) {
        for (const auto &entry : fs::directory_iterator(outDir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".md")
                continue;
            const std::string filename = zanna::filesystem::pathToUtf8(entry.path().filename());
            if (expectedFiles.count(filename))
                continue;
            if (checkOnly) {
                std::cerr << "error: stale generated runtime documentation file: "
                          << zanna::filesystem::pathToUtf8(entry.path()) << "\n";
                clean = false;
            } else {
                std::error_code removeEc;
                fs::remove(entry.path(), removeEc);
                if (removeEc) {
                    std::cerr << "error: cannot remove stale generated runtime documentation file "
                              << zanna::filesystem::pathToUtf8(entry.path()) << ": "
                              << removeEc.message() << "\n";
                    clean = false;
                }
            }
        }
    }

    return clean;
}

/// @brief Audit runtime.def against the actual runtime headers/sources and policy.
/// @details Cross-checks that declared functions exist, that runtime symbols are
///          classified, and that the surface policy's expectations hold; prints a
///          report and returns non-zero when strict checks fail.
/// @param state Parsed runtime definitions.
/// @param inputPath Path to runtime.def (locates the runtime tree and policy).
/// @param strictHeaderSync Treat header/def drift as a failure.
/// @param strictUnclassified Treat unclassified runtime symbols as a failure.
/// @param summaryOnly Print only the summary counts, not per-item detail.
/// @return 0 when the audit passes (under the chosen strictness), non-zero otherwise.
static int runAudit(const ParseState &state,
                    const fs::path &inputPath,
                    bool strictHeaderSync,
                    bool strictUnclassified,
                    bool summaryOnly) {
    const fs::path runtimeDir = inputPath.parent_path();
    const fs::path srcRoot = runtimeDir.parent_path().parent_path();
    const fs::path repoRoot = srcRoot.parent_path();
    const fs::path runtimeRoot = srcRoot / "runtime";
    const fs::path policyPath = runtimeDir / "RuntimeSurfacePolicy.inc";

    RuntimeSurfacePolicy policy = parseRuntimeSurfacePolicy(policyPath);
    auto headerDecls = loadRuntimeHeaderDeclarations(runtimeRoot, repoRoot);
    auto sourceTokens = loadRuntimeSourceTokens(runtimeRoot);
    auto resolvedClasses = buildResolvedClasses(state);

    std::unordered_map<std::string, std::string> canonicalToSymbol;
    canonicalToSymbol.reserve(state.functions.size());
    for (const auto &func : state.functions)
        canonicalToSymbol.emplace(func.canonical, func.c_symbol);

    std::unordered_map<std::string, const ResolvedRuntimeClass *> classesByName;
    for (const auto &cls : resolvedClasses)
        classesByName.emplace(cls.name, &cls);

    std::unordered_set<std::string> publicSymbols;
    for (const auto &func : state.functions)
        publicSymbols.insert(func.c_symbol);

    std::vector<std::string> errors;
    std::vector<std::string> headerSyncFindings;
    std::vector<std::string> unclassifiedFindings;
    /// @brief Append one fatal runtime-surface audit diagnostic.
    /// @param msg Diagnostic text to move into the error list.
    auto addError = [&](std::string msg) { errors.push_back(std::move(msg)); };
    /// @brief Append one runtime-header synchronization finding.
    /// @param msg Diagnostic text to move into the header finding list.
    auto addHeaderFinding = [&](std::string msg) { headerSyncFindings.push_back(std::move(msg)); };
    /// @brief Append one unclassified runtime-surface finding.
    /// @param msg Diagnostic text to move into the unclassified finding list.
    auto addUnclassifiedFinding = [&](std::string msg) {
        unclassifiedFindings.push_back(std::move(msg));
    };

    for (const auto &func : state.functions) {
        if (signatureExposesRawPointer(func.signature)) {
            addError("runtime.def function " + func.canonical +
                     " exposes raw ptr in frontend signature " + func.signature);
        }
        if (headerDecls.find(func.c_symbol) == headerDecls.end()) {
            addHeaderFinding("runtime.def function " + func.canonical + " maps to " +
                             func.c_symbol +
                             " but no declaration was found in src/runtime headers");
        }
        if (sourceTokens.count(func.c_symbol) == 0) {
            addError("runtime.def function " + func.canonical + " maps to " + func.c_symbol +
                     " but no implementation token was found in src/runtime sources");
        }
    }

    for (const auto &cls : state.classes) {
        if (!cls.ctor_id.empty() && cls.ctor_id != "none" &&
            !resolveRuntimeCanonical(state, cls.ctor_id)) {
            addError("runtime class " + cls.name + " has unresolved ctor target " + cls.ctor_id);
        }
        for (const auto &prop : cls.props) {
            if (!prop.getter_id.empty() && prop.getter_id != "none" &&
                !resolveRuntimeCanonical(state, prop.getter_id)) {
                addError("runtime property " + cls.name + "." + prop.name +
                         " has unresolved getter target " + prop.getter_id);
            }
            if (!prop.setter_id.empty() && prop.setter_id != "none" &&
                !resolveRuntimeCanonical(state, prop.setter_id)) {
                addError("runtime property " + cls.name + "." + prop.name +
                         " has unresolved setter target " + prop.setter_id);
            }
        }
        for (const auto &method : cls.methods) {
            if (signatureExposesRawPointer(method.signature)) {
                addError("runtime method " + cls.name + "." + method.name +
                         " exposes raw ptr in frontend signature " + method.signature);
            }
            if (!method.target_id.empty() && method.target_id != "none" &&
                !resolveRuntimeCanonical(state, method.target_id)) {
                addError("runtime method " + cls.name + "." + method.name +
                         " has unresolved target " + method.target_id);
            }
        }
    }

    for (const auto &[canonical, expectedSymbol] : policy.expectedFunctions) {
        auto it = canonicalToSymbol.find(canonical);
        if (it == canonicalToSymbol.end()) {
            addError("runtime surface policy expects function " + canonical +
                     " but it is missing from runtime.def");
            continue;
        }
        if (it->second != expectedSymbol) {
            addError("runtime surface policy expects " + canonical + " to map to " +
                     expectedSymbol + " but runtime.def maps it to " + it->second);
        }
    }

    for (const auto &expected : policy.expectedMethods) {
        auto classIt = classesByName.find(expected.targetCanonical);
        if (classIt == classesByName.end()) {
            addError("runtime surface policy expects class " + expected.targetCanonical +
                     " but it is missing from the runtime catalog");
            continue;
        }

        bool found = false;
        for (const auto &method : classIt->second->methods) {
            if (method.name == expected.name && method.signature == expected.signature) {
                found = true;
                break;
            }
        }
        if (!found) {
            addError("runtime surface policy expects method " + expected.targetCanonical + "." +
                     expected.name + " with signature " + expected.signature +
                     " but it is missing from the runtime catalog");
        }
    }

    for (const auto &expected : policy.expectedProperties) {
        auto classIt = classesByName.find(expected.getterCanonical);
        if (classIt == classesByName.end()) {
            addError("runtime surface policy expects class " + expected.getterCanonical +
                     " but it is missing from the runtime catalog");
            continue;
        }

        bool found = false;
        for (const auto &prop : classIt->second->props) {
            if (prop.name == expected.name && prop.type == expected.type) {
                found = true;
                break;
            }
        }
        if (!found) {
            addError("runtime surface policy expects property " + expected.getterCanonical + "." +
                     expected.name + " : " + expected.type +
                     " but it is missing from the runtime catalog");
        }
    }

    for (const auto &[symbol, proto] : headerDecls) {
        if (policy.internalSymbols.count(symbol))
            continue;
        if (policy.internalHeaders.count(proto.headerPath))
            continue;
        if (publicSymbols.count(symbol))
            continue;

        addUnclassifiedFinding("unclassified runtime header symbol " + symbol + " declared in " +
                               proto.headerPath + " is not represented in runtime.def or policy");
    }

    std::sort(errors.begin(), errors.end());
    std::sort(headerSyncFindings.begin(), headerSyncFindings.end());
    std::sort(unclassifiedFindings.begin(), unclassifiedFindings.end());

    std::cout << "rtgen audit: " << state.functions.size() << " functions, " << state.classes.size()
              << " classes, " << headerDecls.size() << " header declarations\n";

    if (!summaryOnly) {
        for (const auto &finding : headerSyncFindings)
            std::cerr << "warning: " << finding << "\n";
        for (const auto &finding : unclassifiedFindings)
            std::cerr << "warning: " << finding << "\n";
        for (const auto &error : errors)
            std::cerr << "error: " << error << "\n";
    }

    if (strictHeaderSync && !headerSyncFindings.empty()) {
        std::cerr << "error: strict header sync mode is enabled and " << headerSyncFindings.size()
                  << " runtime.def/header mismatch(es) were found\n";
        return 1;
    }
    if (strictUnclassified && !unclassifiedFindings.empty()) {
        std::cerr << "error: strict unclassified mode is enabled and "
                  << unclassifiedFindings.size()
                  << " unclassified runtime header symbol(s) were found\n";
        return 1;
    }
    if (!errors.empty()) {
        std::cerr << "rtgen audit failed with " << errors.size() << " error(s)";
        if (!headerSyncFindings.empty())
            std::cerr << ", " << headerSyncFindings.size() << " header-sync finding(s)";
        if (!unclassifiedFindings.empty())
            std::cerr << ", and " << unclassifiedFindings.size() << " unclassified finding(s)";
        std::cerr << "\n";
        return 1;
    }

    std::cout << "rtgen audit passed";
    if (!headerSyncFindings.empty() || !unclassifiedFindings.empty()) {
        std::cout << " with " << headerSyncFindings.size() << " header-sync finding(s) and "
                  << unclassifiedFindings.size() << " unclassified finding(s)";
    }
    std::cout << "\n";
    return 0;
}

//===----------------------------------------------------------------------===//
// Main
//===----------------------------------------------------------------------===//

/// @brief Print rtgen command-line usage (generate and audit modes) to stderr.
/// @param prog Executable name displayed in each usage form.
static void printUsage(const char *prog) {
    std::cerr << "Usage: " << prog << " <input.def> <output_dir>\n";
    std::cerr << "       " << prog << " --validate <input.def>\n";
    std::cerr << "       " << prog << " --docs [--check] <input.def> <output_dir>\n";
    std::cerr
        << "       " << prog
        << " --audit [--strict-header-sync] [--strict-unclassified] [--summary-only] <input.def>\n";
    std::cerr << "\n";
    std::cerr << "Generates runtime registry .inc files from runtime.def\n";
}

/// @brief rtgen entry point: parse runtime.def and either audit it or generate the
///        registry .inc files.
/// @details Parses flags (--audit and its --strict-*/--summary-only modifiers),
///          validates positional arguments, then in audit mode runs runAudit() and
///          in generate mode writes RuntimeNameMap.inc, RuntimeClasses.inc,
///          RuntimeSignatures.inc, the Zia externs, and RuntimeNames.hpp.
/// @param argc Argument count from the C runtime.
/// @param argv Argument vector from the C runtime.
/// @return 0 on success; non-zero on usage error or audit failure.
int main(int argc, char **argv) {
    zanna::tools::Utf8CommandLine commandLine(argc, argv);
    if (!commandLine.applyOrReport(argc, argv))
        return 1;
    bool auditMode = false;
    bool validateMode = false;
    bool docsMode = false;
    bool checkDocs = false;
    bool strictHeaderSync = false;
    bool strictUnclassified = false;
    bool summaryOnly = false;
    std::vector<std::string> positional;
    positional.reserve(static_cast<size_t>(argc));
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--audit") {
            auditMode = true;
        } else if (arg == "--validate") {
            validateMode = true;
        } else if (arg == "--docs") {
            docsMode = true;
        } else if (arg == "--check") {
            checkDocs = true;
        } else if (arg == "--strict-header-sync") {
            strictHeaderSync = true;
        } else if (arg == "--strict-unclassified") {
            strictUnclassified = true;
        } else if (arg == "--summary-only") {
            summaryOnly = true;
        } else {
            positional.push_back(std::move(arg));
        }
    }

    const int selectedModes =
        static_cast<int>(auditMode) + static_cast<int>(validateMode) + static_cast<int>(docsMode);
    if (selectedModes > 1 || (checkDocs && !docsMode) ||
        (!auditMode && !validateMode && !docsMode && positional.size() != 2) ||
        ((auditMode || validateMode) && positional.size() != 1) ||
        (docsMode && positional.size() != 2)) {
        printUsage(argv[0]);
        return 1;
    }

    fs::path inputPath = zanna::filesystem::pathFromUtf8(positional[0]);

    std::error_code inputEc;
    if (!fs::exists(inputPath, inputEc)) {
        std::cerr << "error: input file not found: " << zanna::filesystem::pathToUtf8(inputPath)
                  << "\n";
        return 1;
    }

    ParseState state;
    try {
        std::cout << "rtgen: Parsing " << zanna::filesystem::pathToUtf8(inputPath) << "\n";
        state = parseFile(inputPath);
        validateDefinitionReferences(state, inputPath);
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    if (auditMode)
        return runAudit(state, inputPath, strictHeaderSync, strictUnclassified, summaryOnly);
    if (validateMode) {
        std::cout << "rtgen: Validated " << state.functions.size() << " functions, "
                  << state.classes.size() << " classes\n";
        return 0;
    }
    if (docsMode) {
        const fs::path documentationDir = zanna::filesystem::pathFromUtf8(positional[1]);
        if (!generateRuntimeDocumentation(state, documentationDir, checkDocs))
            return 1;
        if (checkDocs)
            std::cout << "rtgen: Runtime documentation is current\n";
        return 0;
    }

    fs::path outputDir = zanna::filesystem::pathFromUtf8(positional[1]);

    // Create output directory if needed
    std::error_code ec;
    if (!fs::exists(outputDir, ec)) {
        if (!fs::create_directories(outputDir, ec) || ec) {
            std::cerr << "error: cannot create output directory "
                      << zanna::filesystem::pathToUtf8(outputDir) << ": " << ec.message() << "\n";
            return 1;
        }
    }

    std::cout << "rtgen: Parsed " << state.functions.size() << " functions, "
              << state.classes.size() << " classes\n";

    std::cout << "rtgen: Generating output files in " << zanna::filesystem::pathToUtf8(outputDir)
              << "\n";
    try {
        generateNameMap(state, outputDir);
        generateClasses(state, outputDir);
        generateSignatures(state, outputDir, inputPath);
        generateZiaExterns(state, outputDir, inputPath);
        generateFrontendNames(state, outputDir);
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "rtgen: Done\n";
    return 0;
}
