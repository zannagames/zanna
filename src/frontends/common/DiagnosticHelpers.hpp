//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/DiagnosticHelpers.hpp
// Purpose: Common diagnostic formatting utilities for language frontends.
// Key invariants:
//   * Type names reflect canonical IL scalar categories.
//   * Candidate lists preserve input order and cap displayed entries without
//     mutating caller-owned vectors.
// Ownership: Header-only utilities; inputs are borrowed and returned strings
//            own their data.
// References: src/il/core/Type.hpp,
//             src/frontends/common/DiagnosticFormatter.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares reusable human-readable diagnostic message builders.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace il::frontends::common::diag_helpers {

/// @brief Maximum number of suggestions to show in "did you mean" hints.
constexpr size_t kMaxSuggestions = 5;

/// @brief Format a "did you mean" suggestion list.
/// @param tried Vector of candidate names that were considered
/// @param maxShow Maximum number to show (default: kMaxSuggestions)
/// @return Formatted string like "tried: foo, bar, baz" or empty if no candidates
[[nodiscard]] inline std::string formatTriedList(const std::vector<std::string> &tried,
                                                 size_t maxShow = kMaxSuggestions) {
    if (tried.empty())
        return "";

    std::string result = "tried: ";
    size_t count = std::min(tried.size(), maxShow);

    for (size_t i = 0; i < count; ++i) {
        if (i > 0)
            result += ", ";
        result += tried[i];
    }

    if (tried.size() > maxShow) {
        result += ", ... (";
        result += std::to_string(tried.size() - maxShow);
        result += " more)";
    }

    return result;
}

/// @brief Format a list of matching candidates for ambiguity errors.
/// @param matches Vector of matching names
/// @param maxShow Maximum number to show
/// @return Formatted string like "matches: foo, bar"
[[nodiscard]] inline std::string formatMatchList(const std::vector<std::string> &matches,
                                                 size_t maxShow = kMaxSuggestions) {
    if (matches.empty())
        return "";

    std::string result = "matches: ";
    size_t count = std::min(matches.size(), maxShow);

    for (size_t i = 0; i < count; ++i) {
        if (i > 0)
            result += ", ";
        result += matches[i];
    }

    if (matches.size() > maxShow) {
        result += ", ... (";
        result += std::to_string(matches.size() - maxShow);
        result += " more)";
    }

    return result;
}

/// @brief Format a type name for display in error messages.
/// @param typeName The internal type name
/// @return User-friendly type name
[[nodiscard]] inline std::string formatTypeName(std::string_view typeName) {
    // Map common internal type names to user-friendly names
    if (typeName == "i64" || typeName == "I64")
        return "Integer";
    if (typeName == "f64" || typeName == "F64")
        return "Real";
    if (typeName == "str")
        return "String";
    if (typeName == "ptr")
        return "Pointer";
    if (typeName == "i1" || typeName == "bool")
        return "Boolean";
    if (typeName == "void")
        return "Void";

    return std::string(typeName);
}

/// @brief Format a type mismatch error message.
/// @param expected The expected type name
/// @param actual The actual type name
/// @return Formatted message like "expected Integer, got Real"
[[nodiscard]] inline std::string formatTypeMismatch(std::string_view expected,
                                                    std::string_view actual) {
    std::string result = "expected ";
    result += formatTypeName(expected);
    result += ", got ";
    result += formatTypeName(actual);
    return result;
}

/// @brief Format a duplicate definition error message.
/// @param kind What kind of thing is duplicated (e.g., "variable", "function", "type")
/// @param name The name of the duplicated item
/// @return Formatted message like "duplicate variable 'foo'"
[[nodiscard]] inline std::string formatDuplicateError(std::string_view kind,
                                                      std::string_view name) {
    std::string result = "duplicate ";
    result += kind;
    result += " '";
    result += name;
    result += "'";
    return result;
}

/// @brief Format an undefined symbol error message.
/// @param kind What kind of thing is undefined (e.g., "variable", "function", "type")
/// @param name The name of the undefined item
/// @return Formatted message like "undefined variable 'foo'"
[[nodiscard]] inline std::string formatUndefinedError(std::string_view kind,
                                                      std::string_view name) {
    std::string result = "undefined ";
    result += kind;
    result += " '";
    result += name;
    result += "'";
    return result;
}

/// @brief Format an argument count mismatch error.
/// @param funcName The function name
/// @param expected Expected argument count
/// @param actual Actual argument count
/// @return Formatted message
[[nodiscard]] inline std::string formatArgCountError(std::string_view funcName,
                                                     size_t expected,
                                                     size_t actual) {
    std::string result = "'";
    result += funcName;
    result += "' expects ";
    result += std::to_string(expected);
    result += " argument";
    if (expected != 1)
        result += "s";
    result += ", got ";
    result += std::to_string(actual);
    return result;
}

/// @brief Format an argument count range mismatch error.
/// @param funcName The function name
/// @param minArgs Minimum expected arguments
/// @param maxArgs Maximum expected arguments
/// @param actual Actual argument count
/// @return Formatted message
[[nodiscard]] inline std::string formatArgCountRangeError(std::string_view funcName,
                                                          size_t minArgs,
                                                          size_t maxArgs,
                                                          size_t actual) {
    std::string result = "'";
    result += funcName;
    result += "' expects ";
    result += std::to_string(minArgs);
    result += "-";
    result += std::to_string(maxArgs);
    result += " arguments, got ";
    result += std::to_string(actual);
    return result;
}

/// @brief Quote a string for display in error messages.
/// @param s The string to quote
/// @return The string wrapped in single quotes
[[nodiscard]] inline std::string quote(std::string_view s) {
    std::string result = "'";
    result += s;
    result += "'";
    return result;
}

/// @brief Join a vector of strings with a separator.
/// @param items The strings to join
/// @param sep The separator
/// @return Joined string
[[nodiscard]] inline std::string join(const std::vector<std::string> &items, std::string_view sep) {
    if (items.empty())
        return "";

    std::string result = items[0];
    for (size_t i = 1; i < items.size(); ++i) {
        result += sep;
        result += items[i];
    }
    return result;
}

} // namespace il::frontends::common::diag_helpers
