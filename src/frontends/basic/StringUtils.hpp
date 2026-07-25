//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/StringUtils.hpp
// Purpose: Defines allocation-free comparisons, prefix/suffix checks, case
//          conversion, and trimming helpers used by the BASIC frontend.
// Key invariants:
//   - Case and whitespace classification cast bytes to unsigned char before
//     calling C locale functions.
//   - Comparisons and affix checks allocate no storage.
//   - trim returns a view into caller-owned storage and never copies bytes.
// Ownership/Lifetime:
//   - Helpers are stateless; returned string views do not extend input lifetime.
// Links: src/frontends/basic/IdentifierUtil.hpp,
//        src/frontends/basic/Semantic_OOP_Helpers.cpp,
//        src/frontends/basic/TypeSuffix.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

/// @file
/// @brief Defines locale-based byte-string helpers for BASIC identifiers and
///        source text.

namespace il::frontends::basic::string_utils {

/// @brief Case-insensitive comparison of two strings.
/// @details Performs character-by-character comparison through the active C
///          locale's uppercase mapping.
///          This is more efficient than converting both strings to uppercase
///          and then comparing, as it avoids allocations.
/// @param a First string to compare.
/// @param b Second string to compare.
/// @return @c true if strings are equal ignoring case.
/// @note This is byte-oriented and does not perform Unicode normalization or
///       multi-byte case folding.
/// @example
/// ```cpp
/// if (iequals(tok.lexeme, "INTEGER")) { /* ... */ }
/// ```
[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;

    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char ca, char cb) {
        return std::toupper(static_cast<unsigned char>(ca)) ==
               std::toupper(static_cast<unsigned char>(cb));
    });
}

/// @brief Check if a string starts with a prefix (case-insensitive).
/// @param str String to check.
/// @param prefix Prefix to look for.
/// @return @c true if @p str starts with @p prefix, ignoring case; an empty
///         prefix always matches.
[[nodiscard]] inline bool istarts_with(std::string_view str, std::string_view prefix) noexcept {
    if (str.size() < prefix.size())
        return false;

    return iequals(str.substr(0, prefix.size()), prefix);
}

/// @brief Check if a string ends with a suffix (case-insensitive).
/// @param str String to check.
/// @param suffix Suffix to look for.
/// @return @c true if @p str ends with @p suffix, ignoring case; an empty
///         suffix always matches.
[[nodiscard]] inline bool iends_with(std::string_view str, std::string_view suffix) noexcept {
    if (str.size() < suffix.size())
        return false;

    return iequals(str.substr(str.size() - suffix.size()), suffix);
}

/// @brief Convert a string to uppercase (allocating version).
/// @param str Byte string to convert through the active C locale.
/// @return New string with one uppercase-mapped byte per input byte.
/// @note Use sparingly; prefer @ref iequals for comparisons to avoid allocation.
[[nodiscard]] inline std::string to_upper(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str)
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return result;
}

/// @brief Convert a string to lowercase (allocating version).
/// @param str Byte string to convert through the active C locale.
/// @return New string with one lowercase-mapped byte per input byte.
[[nodiscard]] inline std::string to_lower(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return result;
}

/// @brief Trim leading and trailing whitespace.
/// @details Whitespace is classified through the active C locale.
/// @param str Borrowed byte string to trim.
/// @return View of the non-whitespace interior, or a default empty view when
///         the input is empty or entirely whitespace.
/// @warning A non-empty returned view refers into @p str and becomes invalid
///          when its backing storage expires or moves.
[[nodiscard]] inline std::string_view trim(std::string_view str) noexcept {
    auto start = str.begin();
    auto end = str.end();

    while (start != end && std::isspace(static_cast<unsigned char>(*start)))
        ++start;

    while (start != end && std::isspace(static_cast<unsigned char>(*(end - 1))))
        --end;

    auto len = static_cast<std::size_t>(std::distance(start, end));
    return len > 0 ? std::string_view(&*start, len) : std::string_view{};
}

} // namespace il::frontends::basic::string_utils
