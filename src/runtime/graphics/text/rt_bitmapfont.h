//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/text/rt_bitmapfont.h
// Purpose: Custom bitmap font loading (BDF/PSF formats) and Canvas text rendering,
//   providing variable-size fonts beyond the built-in 8x8 monospace font.
//
// Key invariants:
//   - BitmapFont objects are GC-managed via rt_obj_new_i64.
//   - Glyph bitmaps are heap-allocated; freed in the GC finalizer.
//   - Glyph tables cover BMP codepoints. UTF-8 text is decoded to codepoints
//     during measurement/rendering, and missing glyphs use the fallback glyph.
//   - With graphics enabled, Canvas drawing functions are no-ops when the
//     canvas, text, or font handle is invalid. Graphics-disabled builds raise
//     an InvalidOperation trap instead.
//
// Ownership/Lifetime:
//   - BitmapFont handles are GC-managed; no manual free needed.
//   - Glyph bitmap data is owned by the font and freed on destruction.
//
// Links: src/runtime/graphics/text/rt_bitmapfont.c,
//        src/runtime/graphics/text/rt_font.h,
//        src/runtime/graphics/2d/rt_drawing.c
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares managed BDF/PSF bitmap-font loading, metrics, and Canvas rendering.
 * @details Provides BitmapFont and SpriteFont construction, finalization,
 * glyph and line metrics, UTF-8 text measurement, and aligned or scaled
 * drawing operations. Loaded glyph data remains owned by the managed font.
 */
#pragma once

#include "rt_string.h"

#include <stdint.h>

/// Runtime object class identifier for BitmapFont handles.
#define RT_BITMAPFONT_CLASS_ID INT64_C(-0x600202)
/// Runtime object class identifier for SpriteFont-compatible handles.
#define RT_SPRITEFONT_CLASS_ID INT64_C(-0x600203)

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// BitmapFont Loading
//=========================================================================

/// @brief Load a BDF (Bitmap Distribution Format) font file.
/// @param path Runtime string containing the `.bdf` file path.
/// @return A GC-managed BitmapFont handle, or `NULL` for an unreadable,
/// malformed, truncated, empty, or unallocatable input.
void *rt_bitmapfont_load_bdf(rt_string path);

/// @brief Load a BDF font with SpriteFont runtime class identity.
/// @details The returned object uses the same glyph representation and
/// rendering APIs as BitmapFont; only its runtime class ID differs.
/// @param path Runtime string containing the `.bdf` file path.
/// @return A GC-managed SpriteFont handle, or `NULL` if loading fails.
void *rt_spritefont_load_bdf(rt_string path);

/// @brief Load a PSF (PC Screen Font) v1 or v2 file.
/// @details An optional PSF Unicode table is used to copy glyphs into their
/// BMP codepoint slots; unsupported combining sequences are skipped.
/// @param path Runtime string containing the `.psf` file path.
/// @return A GC-managed BitmapFont handle, or `NULL` for an unreadable,
/// malformed, truncated, unsupported, or unallocatable input.
void *rt_bitmapfont_load_psf(rt_string path);

/// @brief Load a PSF font with SpriteFont runtime class identity.
/// @details The returned object uses the same glyph representation and
/// rendering APIs as BitmapFont; only its runtime class ID differs.
/// @param path Runtime string containing the `.psf` file path.
/// @return A GC-managed SpriteFont handle, or `NULL` if loading fails.
void *rt_spritefont_load_psf(rt_string path);

/// @brief Release all per-glyph bitmap allocations during finalization.
/// @details Runtime object storage is reclaimed separately by the GC/object
/// manager. Invalid handles are ignored.
/// @param font BitmapFont or SpriteFont handle being finalized.
void rt_bitmapfont_destroy(void *font);

//=========================================================================
// BitmapFont Properties
//=========================================================================

/// @brief Get the fixed glyph advance of a monospace font.
/// @param font BitmapFont or SpriteFont handle.
/// @return The maximum advance in pixels for a monospace font, or 0 for a
/// proportional or invalid font.
int64_t rt_bitmapfont_char_width(void *font);

/// @brief Get the line height in pixels.
/// @param font BitmapFont or SpriteFont handle.
/// @return The ascent-plus-descent line height, or 0 for an invalid handle.
int64_t rt_bitmapfont_char_height(void *font);

/// @brief Get the number of populated glyph-table entries.
/// @param font BitmapFont or SpriteFont handle.
/// @return The number of bitmap-bearing slots, including Unicode aliases, or
/// 0 for an invalid handle.
int64_t rt_bitmapfont_glyph_count(void *font);

/// @brief Check if all glyphs have the same advance width.
/// @param font BitmapFont or SpriteFont handle.
/// @return 1 when the valid font is monospace; otherwise 0.
int8_t rt_bitmapfont_is_monospace(void *font);

//=========================================================================
// Text Measurement
//=========================================================================

/// @brief Measure the pixel width of a UTF-8 string rendered with this font.
/// @details Includes advance widths and left/right glyph overhangs. Invalid
/// UTF-8 bytes are measured through the font's fallback glyph.
/// @param font BitmapFont or SpriteFont handle.
/// @param text Runtime UTF-8 string to measure.
/// @return The horizontal rendered extent in pixels, or 0 for invalid or
/// empty input.
int64_t rt_bitmapfont_text_width(void *font, rt_string text);

/// @brief Get the text height (same as line height).
/// @details This is a single-line metric and does not inspect any text.
/// @param font BitmapFont or SpriteFont handle.
/// @return The line height in pixels, or 0 for an invalid handle.
int64_t rt_bitmapfont_text_height(void *font);

//=========================================================================
// Canvas Drawing with Custom Font
//=========================================================================

/// @brief Draw text at (x,y) using a custom BitmapFont.
/// @details The coordinates identify the initial pen X and top of the line.
/// The renderer applies each glyph's baseline bearings and UTF-8 fallback.
/// @param canvas Canvas object receiving the glyph pixels.
/// @param x Initial pen X coordinate.
/// @param y Top-of-line Y coordinate.
/// @param text Runtime UTF-8 string to render.
/// @param font BitmapFont or SpriteFont handle.
/// @param color Runtime Canvas foreground color.
void rt_canvas_text_font(
    void *canvas, int64_t x, int64_t y, rt_string text, void *font, int64_t color);

/// @brief Draw text with foreground and background colors.
/// @details Each opaque background cell covers the glyph advance and expands
/// to include visible ink overhang.
/// @param canvas Canvas object receiving the text.
/// @param x Initial pen X coordinate.
/// @param y Top-of-line Y coordinate.
/// @param text Runtime UTF-8 string to render.
/// @param font BitmapFont or SpriteFont handle.
/// @param fg_color Runtime Canvas foreground color.
/// @param bg_color Runtime Canvas background color.
void rt_canvas_text_font_bg(void *canvas,
                            int64_t x,
                            int64_t y,
                            rt_string text,
                            void *font,
                            int64_t fg_color,
                            int64_t bg_color);

/// @brief Draw text with integer scaling (1=normal, 2=double, etc.).
/// @param canvas Canvas object receiving the scaled glyph pixels.
/// @param x Initial pen X coordinate.
/// @param y Top-of-line Y coordinate.
/// @param text Runtime UTF-8 string to render.
/// @param font BitmapFont or SpriteFont handle.
/// @param scale Positive integer enlargement factor; values below one make
/// graphics-enabled builds return without drawing.
/// @param color Runtime Canvas foreground color.
void rt_canvas_text_font_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, void *font, int64_t scale, int64_t color);

/// @brief Draw text horizontally centered on the canvas.
/// @details Centering uses both advances and actual glyph overhangs.
/// @param canvas Canvas whose current width defines the center.
/// @param y Top-of-line Y coordinate.
/// @param text Runtime UTF-8 string to render.
/// @param font BitmapFont or SpriteFont handle.
/// @param color Runtime Canvas foreground color.
void rt_canvas_text_font_centered(
    void *canvas, int64_t y, rt_string text, void *font, int64_t color);

/// @brief Draw text right-aligned with a margin from the right edge.
/// @details Alignment places the rightmost advance-or-ink extent @p margin
/// pixels from the current canvas width.
/// @param canvas Canvas whose current width defines the right edge.
/// @param margin Requested right-edge padding in pixels.
/// @param y Top-of-line Y coordinate.
/// @param text Runtime UTF-8 string to render.
/// @param font BitmapFont or SpriteFont handle.
/// @param color Runtime Canvas foreground color.
void rt_canvas_text_font_right(
    void *canvas, int64_t margin, int64_t y, rt_string text, void *font, int64_t color);

#ifdef __cplusplus
}
#endif
