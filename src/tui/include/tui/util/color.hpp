//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares hexadecimal RGB parsing for TUI render colors.
/// @details Provides a stateless conversion from optional-`#` RRGGBB text to
///          fully opaque RGBA values.
// Key invariants: Parses hex RGB triplets into RGBA structs.
// Ownership/Lifetime: Stateless utility functions.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/render/screen.hpp"

#include <string>

namespace zanna::tui::util {

/// @brief Parse a hex RGB color string into an RGBA struct.
/// @details Accepts colors in the forms "#RRGGBB" or "RRGGBB" and stores the
///          converted components in the output struct. The alpha channel is
///          set to 255 (fully opaque) on success.
/// @param s Input color string (with or without leading #).
/// @param out Destination RGBA struct populated on success.
/// @return True when parsing succeeds, false otherwise.
[[nodiscard]] bool parseHexColor(const std::string &s, render::RGBA &out);

} // namespace zanna::tui::util
