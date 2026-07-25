//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: frontends/basic/LineUtils.hpp
// Purpose: Provide helpers for reasoning about BASIC line labels.
// Key invariants: Treats non-positive integers as synthetic/unlabeled statements.
// Ownership/Lifetime: Header-only utility with no state.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file LineUtils.hpp
 * @brief Defines stateless helpers for classifying and parsing BASIC line labels.
 *
 * These routines centralize the legacy convention that positive integral
 * values identify user-written line labels while zero and negative signed
 * values denote compiler-generated statement positions.
 */

#pragma once

#include <charconv>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace il::frontends::basic {
/// @brief Determine whether a BASIC line label originates from user input.
/// @details BASIC statements parsed without an explicit numeric label are
///          assigned non-positive synthetic identifiers. This helper normalises
///          checks for such cases so callers do not rely on magic sentinels.
/// @tparam T Integral type representing the stored line number.
/// @param line Candidate line label.
/// @return True when no user-provided line label was supplied.
template <typename T> [[nodiscard]] constexpr bool isUnlabeledLine(T line) noexcept {
    static_assert(std::is_integral_v<T>, "line numbers must be integral");
    if constexpr (std::is_signed_v<T>) {
        return line <= 0;
    } else {
        return line == 0;
    }
}

/// @brief Determine whether a BASIC line label was explicitly provided.
/// @tparam T Integral type representing the stored line number.
/// @param line Candidate line label.
/// @return True when a positive user-specified line label exists.
template <typename T> [[nodiscard]] constexpr bool hasUserLine(T line) noexcept {
    return !isUnlabeledLine(line);
}

/// @brief Parse a BASIC numeric line label as a bounded signed integer.
/// @details Requires the entire token text to be consumed and rejects overflow.
///          BASIC labels are stored as @c int throughout the legacy frontend, so
///          this helper centralizes that boundary check for parser code paths
///          that previously used unchecked C conversions. Leading whitespace
///          and plus signs are rejected; a leading minus sign is accepted by
///          the underlying signed conversion.
/// @param text Complete base-10 token spelling to parse.
/// @return Parsed line number, or @c std::nullopt if the token is malformed or
///         outside the representable @c int range.
[[nodiscard]] inline std::optional<int> parseLineNumberLiteral(std::string_view text) noexcept {
    long long parsed = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end)
        return std::nullopt;
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(parsed);
}

} // namespace il::frontends::basic
