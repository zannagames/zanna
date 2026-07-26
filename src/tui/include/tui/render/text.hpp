//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares inline aligned-text painters for terminal screen buffers.
/// @details Provides clipped left-, right-, and center-aligned byte-text
///          rendering with a uniform cell style and no owned state.
// Key invariants: Drawing respects buffer bounds; strings are clipped to width.
// Ownership/Lifetime: Stateless utility functions; no dynamic resources.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/render/screen.hpp"

#include <algorithm>
#include <string_view>

namespace zanna::tui::render {

/// @brief Render left-aligned text into a screen buffer row.
/// @details Paints characters from the given text starting at position (y, x)
///          and clipping to maxWidth columns. Characters beyond the width are
///          not rendered.
/// @param sb Screen buffer to draw into.
/// @param y Row coordinate.
/// @param x Starting column coordinate.
/// @param maxWidth Maximum number of columns to render.
/// @param text Text to render.
/// @param style Style to apply to each character cell.
inline void renderText(
    ScreenBuffer &sb, int y, int x, int maxWidth, std::string_view text, const Style &style) {
    int len = std::min(static_cast<int>(text.size()), maxWidth);
    for (int i = 0; i < len; ++i) {
        Cell &cell = sb.at(y, x + i);
        cell.ch = static_cast<char32_t>(static_cast<unsigned char>(text[static_cast<size_t>(i)]));
        cell.style = style;
    }
}

/// @brief Render right-aligned text into a screen buffer row.
/// @details Paints characters from the given text aligned to the right edge
///          within the specified width, starting at position (y, x).
/// @param sb Screen buffer to draw into.
/// @param y Row coordinate.
/// @param x Starting column coordinate (left edge of the region).
/// @param width Total width of the region.
/// @param text Text to render.
/// @param style Style to apply to each character cell.
inline void renderTextRight(
    ScreenBuffer &sb, int y, int x, int width, std::string_view text, const Style &style) {
    int textLen = static_cast<int>(text.size());
    int start = x + width - textLen;
    start = std::max(start, x);
    int len = std::min(textLen, width);
    for (int i = 0; i < len; ++i) {
        Cell &cell = sb.at(y, start + i);
        cell.ch = static_cast<char32_t>(static_cast<unsigned char>(text[static_cast<size_t>(i)]));
        cell.style = style;
    }
}

/// @brief Render centered text into a screen buffer row.
/// @details Paints characters from the given text centered within the specified
///          width, starting at position (y, x).
/// @param sb Screen buffer to draw into.
/// @param y Row coordinate.
/// @param x Starting column coordinate (left edge of the region).
/// @param width Total width of the region.
/// @param text Text to render.
/// @param style Style to apply to each character cell.
inline void renderTextCentered(
    ScreenBuffer &sb, int y, int x, int width, std::string_view text, const Style &style) {
    int textLen = static_cast<int>(text.size());
    int renderLen = std::min(textLen, width);
    int start = x + (width - renderLen) / 2;
    for (int i = 0; i < renderLen; ++i) {
        Cell &cell = sb.at(y, start + i);
        cell.ch = static_cast<char32_t>(static_cast<unsigned char>(text[static_cast<size_t>(i)]));
        cell.style = style;
    }
}

} // namespace zanna::tui::render
