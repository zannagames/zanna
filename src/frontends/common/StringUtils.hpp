//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/StringUtils.hpp
// Purpose: Shared string utility functions for all language frontends.
// Key invariants:
//   * Comparisons are ASCII case-insensitive and locale-independent.
//   * Input string views are never retained.
// Ownership: Header-only stateless utilities; all source storage remains
//            caller-owned.
// References: src/frontends/common/CharUtils.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares lightweight shared string comparison utilities.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/common/CharUtils.hpp"

#include <algorithm>
#include <string_view>

namespace il::frontends::common::string_utils {

/// @brief Case-insensitive comparison of two string views.
/// @details Performs character-by-character comparison ignoring case using
///          ASCII-only folding for locale-independent frontend behavior
///          on negative char values.
/// @param a First string to compare.
/// @param b Second string to compare.
/// @return True if strings are equal ignoring case, false otherwise.
[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;

    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char ca, char cb) {
        return char_utils::toUpper(ca) == char_utils::toUpper(cb);
    });
}

} // namespace il::frontends::common::string_utils
