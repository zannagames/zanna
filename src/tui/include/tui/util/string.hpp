//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares ASCII lowercase conversion helpers for TUI strings.
/// @details Provides stateless in-place and copying conversions using
///          unsigned-character-safe standard-library case folding.
// Key invariants: ASCII-only case conversion (no Unicode support).
// Ownership/Lifetime: Stateless inline utilities with no dynamic resources.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace zanna::tui::util {

/// @brief Convert a string to lowercase in-place.
/// @details Uses ASCII-only lowercase conversion via std::tolower.
/// @param s String to convert.
inline void toLowerInPlace(std::string &s) {
    /// @brief Fold one byte to lowercase without signed-character undefined behavior.
    /// @param c Byte to normalize.
    /// @return Lowercase representation converted back to `char`.
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
}

/// @brief Convert a string to lowercase, returning a new string.
/// @details Uses ASCII-only lowercase conversion via std::tolower.
/// @param s Input string.
/// @return Lowercase copy of the input string.
[[nodiscard]] inline std::string toLower(const std::string &s) {
    std::string result = s;
    toLowerInPlace(result);
    return result;
}

} // namespace zanna::tui::util
