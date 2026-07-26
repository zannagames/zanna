//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares bordered-rectangle drawing for terminal screen buffers.
/// @details Provides a stateless utility that renders ASCII borders and
///          optional styled interior fill into an existing screen buffer.
// Key invariants: Drawing respects buffer bounds; callers must ensure the
//                 rectangle fits within the screen buffer.
// Ownership/Lifetime: Stateless utility functions; no dynamic resources.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/render/screen.hpp"

namespace zanna::tui::render {

/// @brief Draw a bordered box with optional fill into a screen buffer.
/// @details Renders a rectangular border using ASCII box-drawing characters
///          (+, -, |) and optionally fills the interior with spaces. When
///          style pointers are provided, the corresponding cells receive those
///          styles; otherwise existing styles are preserved.
/// @param sb Screen buffer to draw into.
/// @param x Left edge coordinate.
/// @param y Top edge coordinate.
/// @param w Width in columns (including border).
/// @param h Height in rows (including border).
/// @param borderStyle Optional style for border cells (corners and edges).
/// @param fillStyle Optional style for interior cells (when fill is true).
/// @param fill Whether to clear interior cells to spaces.
void drawBox(ScreenBuffer &sb,
             int x,
             int y,
             int w,
             int h,
             const Style *borderStyle = nullptr,
             const Style *fillStyle = nullptr,
             bool fill = true);

} // namespace zanna::tui::render
