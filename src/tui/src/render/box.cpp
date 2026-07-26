//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tui/src/render/box.cpp
// Purpose: Implements box drawing utilities for the TUI rendering subsystem.
// Key invariants: Box dimensions are assumed valid; callers ensure bounds.
// Ownership/Lifetime: Stateless utility; no dynamic allocations.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the box/border drawing utility for screen buffers.
/// @details The utility draws ASCII-style boxes using +, -, | characters and
///          optionally fills interior cells with spaces. Styles are applied
///          when provided.

#include "tui/render/box.hpp"

namespace zanna::tui::render {

/// @brief Draw an ASCII bordered rectangle and optionally clear its interior.
/// @details Widths or heights below two produce no output. Border and fill
///          styles are applied only when their pointers are non-null, allowing
///          callers to preserve existing cell styles.
/// @param sb Screen buffer to mutate; the requested rectangle must be in bounds.
/// @param x Zero-based left column.
/// @param y Zero-based top row.
/// @param w Width including both border columns.
/// @param h Height including both border rows.
/// @param borderStyle Optional style for corners and edges.
/// @param fillStyle Optional style for cleared interior cells.
/// @param fill Whether to replace interior glyphs with spaces.
void drawBox(ScreenBuffer &sb,
             int x,
             int y,
             int w,
             int h,
             const Style *borderStyle,
             const Style *fillStyle,
             bool fill) {
    if (w < 2 || h < 2) {
        return;
    }

    // Top and bottom borders
    for (int col = 0; col < w; ++col) {
        auto &top = sb.at(y, x + col);
        top.ch = (col == 0 || col == w - 1) ? U'+' : U'-';
        if (borderStyle) {
            top.style = *borderStyle;
        }

        auto &bot = sb.at(y + h - 1, x + col);
        bot.ch = (col == 0 || col == w - 1) ? U'+' : U'-';
        if (borderStyle) {
            bot.style = *borderStyle;
        }
    }

    // Side borders and optional fill
    for (int row = 1; row < h - 1; ++row) {
        auto &left = sb.at(y + row, x);
        left.ch = U'|';
        if (borderStyle) {
            left.style = *borderStyle;
        }

        auto &right = sb.at(y + row, x + w - 1);
        right.ch = U'|';
        if (borderStyle) {
            right.style = *borderStyle;
        }

        if (fill) {
            for (int col = 1; col < w - 1; ++col) {
                auto &cell = sb.at(y + row, x + col);
                cell.ch = U' ';
                if (fillStyle) {
                    cell.style = *fillStyle;
                }
            }
        }
    }
}

} // namespace zanna::tui::render
