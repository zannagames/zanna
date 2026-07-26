//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/text/rt_font.h
// Purpose: Embedded 8x8 bitmap font providing per-character glyph bitmaps for software text
// rendering, where each glyph is 8 bytes (one byte per row).
//
// Key invariants:
//   - Printable ASCII characters 32-126 have glyphs; every other integer
//     value maps to an all-zero fallback glyph.
//   - Each glyph is exactly 8 bytes; bit 7 of each byte is the leftmost pixel.
//   - Font width and height are always 8 pixels.
//   - Glyph data is read-only; callers must not modify the returned pointer.
//
// Ownership/Lifetime:
//   - Glyph pointers are into a static read-only table; no allocation or lifetime management.
//   - Callers borrow the returned pointer; they must not free it.
//
// Links: src/runtime/graphics/text/rt_font.c,
//        src/runtime/graphics/2d/rt_pixels_draw.c,
//        src/runtime/graphics/2d/rt_drawing.c,
//        src/runtime/graphics/3d/render/rt_canvas3d_overlay.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Get the 8-byte bitmap for a character.
/// @details Printable ASCII values select their table entry. All other
/// values select the shared all-zero fallback glyph. Within each returned
/// row byte, bit 7 is the leftmost pixel.
/// @param c Integer codepoint to look up.
/// @return A borrowed pointer to exactly eight immutable, process-lifetime row
/// bytes; callers must neither modify nor free it.
const uint8_t *rt_font_get_glyph(int c);

/// @brief Get the font character width.
/// @return The constant fixed width, 8 pixels.
int rt_font_char_width(void);

/// @brief Get the font character height.
/// @return The constant fixed height, 8 pixels.
int rt_font_char_height(void);

#ifdef __cplusplus
}
#endif
