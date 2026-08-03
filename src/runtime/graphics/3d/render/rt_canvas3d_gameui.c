//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_gameui.c
// Purpose: Canvas3D binding for the canvas-polymorphic Game.UI widget draw-ops
//   table (ADR 0065): maps each widget primitive onto the Canvas3D screen-space
//   overlay queue so the same widget objects render over 3D scenes.
// Key invariants:
//   - Registered via rt_gameui_register_canvas3d_ops from Canvas3D creation so
//     runtime/game never includes graphics/3d headers (layering).
//   - Text ops draw the built-in Canvas3D 5x7 font advance-matched to the 2D
//     8px-per-character metrics (scale 8/12) so widget layout computed from
//     rt_canvas_text_width stays visually correct; custom Font objects fall
//     back to the built-in font on Canvas3D (v1 limitation, documented).
//   - box_alpha's 0..255 alpha converts to the overlay queue's 0..1 range.
// Ownership/Lifetime:
//   - Stateless: the ops table borrows the canvas handle per Draw call.
// Links: src/runtime/game/rt_gameui_draw.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Adapts Game.UI drawing operations to Canvas3D screen-space overlays.
/// @details The adapter preserves the metrics expected by canvas-polymorphic
///   widgets while translating boxes, frames, lines, text, and pixel blits into
///   deferred Canvas3D overlay commands. The table is stateless and borrows all
///   input handles for the duration of each call.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_gameui_draw.h"

/* Advance-match the Canvas3D built-in font (12px/char at scale 1) to the 2D
 * canvas metrics (8px/char) that widget layout is computed against. */
#define CANVAS3D_GAMEUI_TEXT_SCALE (8.0 / 12.0)

/// @brief Probe: true when @p canvas is a Canvas3D handle.
/// @param canvas Borrowed candidate runtime object.
/// @return One when @p canvas is a valid Canvas3D handle; otherwise zero.
static int8_t canvas3d_gameui_probe(void *canvas) {
    return rt_canvas3d_checked_or_stack(canvas) != NULL ? 1 : 0;
}

/// @brief Queue a solid rectangular Game.UI box on a Canvas3D overlay.
/// @param canvas Borrowed Canvas3D handle receiving the primitive.
/// @param x Left edge in overlay pixels.
/// @param y Top edge in overlay pixels.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param color Packed RGB runtime color.
static void canvas3d_gameui_box(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    rt_canvas3d_draw_rect2d(canvas, x, y, w, h, color);
}

/// @brief Queue a solid rectangular Game.UI box with byte-range opacity.
/// @details Opacity is clamped to the inclusive range 0 through 255 before it
///   is converted to the normalized alpha expected by Canvas3D.
/// @param canvas Borrowed Canvas3D handle receiving the primitive.
/// @param x Left edge in overlay pixels.
/// @param y Top edge in overlay pixels.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param color Packed RGB runtime color.
/// @param alpha Opacity in byte units; values outside 0 through 255 are clamped.
static void canvas3d_gameui_box_alpha(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, int64_t alpha) {
    if (alpha < 0)
        alpha = 0;
    if (alpha > 255)
        alpha = 255;
    rt_canvas3d_draw_rect2d_alpha(canvas, x, y, w, h, color, (double)alpha / 255.0);
}

/// @brief Queue a one-pixel rectangular Game.UI outline.
/// @param canvas Borrowed Canvas3D handle receiving the primitive.
/// @param x Left edge in overlay pixels.
/// @param y Top edge in overlay pixels.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param color Packed RGB runtime color.
static void canvas3d_gameui_frame(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    rt_canvas3d_draw_frame2d(canvas, x, y, w, h, color, 1.0);
}

/// @brief Queue a one-pixel Game.UI line segment.
/// @param canvas Borrowed Canvas3D handle receiving the primitive.
/// @param x1 Horizontal pixel coordinate of the first endpoint.
/// @param y1 Vertical pixel coordinate of the first endpoint.
/// @param x2 Horizontal pixel coordinate of the second endpoint.
/// @param y2 Vertical pixel coordinate of the second endpoint.
/// @param color Packed RGB runtime color.
static void canvas3d_gameui_line(
    void *canvas, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
    rt_canvas3d_draw_line2d(canvas, x1, y1, x2, y2, color, 1.0);
}

/// @brief Queue a filled Game.UI rectangle with rounded corners.
/// @param canvas Borrowed Canvas3D handle receiving the primitive.
/// @param x Left edge in overlay pixels.
/// @param y Top edge in overlay pixels.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param radius Corner radius in pixels.
/// @param color Packed RGB runtime color.
static void canvas3d_gameui_round_box(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius, int64_t color) {
    rt_canvas3d_draw_round_rect2d(canvas, x, y, w, h, radius, color, 1.0);
}

/// @brief Queue a one-pixel Game.UI outline with rounded corners.
/// @param canvas Borrowed Canvas3D handle receiving the primitive.
/// @param x Left edge in overlay pixels.
/// @param y Top edge in overlay pixels.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param radius Corner radius in pixels.
/// @param color Packed RGB runtime color.
static void canvas3d_gameui_round_frame(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius, int64_t color) {
    rt_canvas3d_draw_round_frame2d(canvas, x, y, w, h, radius, color, 1.0);
}

/// @brief Queue Game.UI text using the Canvas3D built-in bitmap font.
/// @details The fixed scale maps the built-in font's twelve-pixel advance to
///   the eight-pixel advance used by Game.UI layout calculations.
/// @param canvas Borrowed Canvas3D handle receiving the text.
/// @param x Left text origin in overlay pixels.
/// @param y Top text origin in overlay pixels.
/// @param text Borrowed runtime string rendered during this call.
/// @param color Packed RGB runtime color.
static void canvas3d_gameui_text(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t color) {
    rt_canvas3d_draw_text2d_scaled(canvas, x, y, text, color, CANVAS3D_GAMEUI_TEXT_SCALE);
}

/// @brief Queue integer-scaled Game.UI text with metric-matched glyph advance.
/// @details Values smaller than one are clamped to one before combining the
///   widget scale with CANVAS3D_GAMEUI_TEXT_SCALE.
/// @param canvas Borrowed Canvas3D handle receiving the text.
/// @param x Left text origin in overlay pixels.
/// @param y Top text origin in overlay pixels.
/// @param text Borrowed runtime string rendered during this call.
/// @param scale Integer Game.UI text scale, clamped to at least one.
/// @param color Packed RGB runtime color.
static void canvas3d_gameui_text_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t color) {
    if (scale < 1)
        scale = 1;
    rt_canvas3d_draw_text2d_scaled(
        canvas, x, y, text, color, (double)scale * CANVAS3D_GAMEUI_TEXT_SCALE);
}

/// @brief Queue nominally custom-font Game.UI text using the built-in fallback.
/// @details Canvas3D does not support Game.UI Font objects in this adapter
///   version, so @p font is ignored and the metric-matched bitmap font is used.
/// @param canvas Borrowed Canvas3D handle receiving the text.
/// @param x Left text origin in overlay pixels.
/// @param y Top text origin in overlay pixels.
/// @param text Borrowed runtime string rendered during this call.
/// @param font Borrowed Font handle accepted for interface compatibility and ignored.
/// @param color Packed RGB runtime color.
static void canvas3d_gameui_text_font(
    void *canvas, int64_t x, int64_t y, rt_string text, void *font, int64_t color) {
    (void)font; /* v1: custom Font objects render with the built-in font on Canvas3D */
    canvas3d_gameui_text(canvas, x, y, text, color);
}

/// @brief Queue scaled nominally custom-font text using the built-in fallback.
/// @details Canvas3D ignores @p font, clamps @p scale to at least one through
///   canvas3d_gameui_text_scaled(), and preserves Game.UI layout metrics.
/// @param canvas Borrowed Canvas3D handle receiving the text.
/// @param x Left text origin in overlay pixels.
/// @param y Top text origin in overlay pixels.
/// @param text Borrowed runtime string rendered during this call.
/// @param font Borrowed Font handle accepted for interface compatibility and ignored.
/// @param scale Integer Game.UI text scale, clamped to at least one.
/// @param color Packed RGB runtime color.
static void canvas3d_gameui_text_font_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, void *font, int64_t scale, int64_t color) {
    (void)font; /* v1: custom Font objects render with the built-in font on Canvas3D */
    canvas3d_gameui_text_scaled(canvas, x, y, text, scale, color);
}

/// @brief Queue an unscaled source region as a Canvas3D overlay image.
/// @details The source and destination extents are both @p w by @p h, so this
///   adapter performs a region copy without intentional scaling.
/// @param canvas Borrowed Canvas3D handle receiving the image.
/// @param dx Destination left edge in overlay pixels.
/// @param dy Destination top edge in overlay pixels.
/// @param pixels Borrowed Pixels object supplying the source image.
/// @param sx Source-region left edge in pixels.
/// @param sy Source-region top edge in pixels.
/// @param w Source and destination width in pixels.
/// @param h Source and destination height in pixels.
static void canvas3d_gameui_blit_region(void *canvas,
                                        int64_t dx,
                                        int64_t dy,
                                        void *pixels,
                                        int64_t sx,
                                        int64_t sy,
                                        int64_t w,
                                        int64_t h) {
    rt_canvas3d_draw_image2d_region(canvas, dx, dy, w, h, pixels, sx, sy, w, h);
}

/// @brief Fill the widget draw-ops table for a Canvas3D handle.
/// @details Assigns every primitive callback plus canvas dimension queries. The
///   function does nothing when @p ops is `NULL`; otherwise it overwrites the
///   relevant table fields and stores @p canvas without retaining it.
/// @param canvas Borrowed Canvas3D handle copied into the operations table.
/// @param ops Borrowed output table to initialize; may be `NULL`.
static void canvas3d_gameui_fill(void *canvas, rt_gameui_draw_ops_t *ops) {
    if (!ops)
        return;
    ops->canvas = canvas;
    ops->box = canvas3d_gameui_box;
    ops->box_alpha = canvas3d_gameui_box_alpha;
    ops->frame = canvas3d_gameui_frame;
    ops->line = canvas3d_gameui_line;
    ops->round_box = canvas3d_gameui_round_box;
    ops->round_frame = canvas3d_gameui_round_frame;
    ops->text = canvas3d_gameui_text;
    ops->text_scaled = canvas3d_gameui_text_scaled;
    ops->text_font = canvas3d_gameui_text_font;
    ops->text_font_scaled = canvas3d_gameui_text_font_scaled;
    ops->blit_region = canvas3d_gameui_blit_region;
    ops->width = rt_canvas3d_get_width;
    ops->height = rt_canvas3d_get_height;
}

/// @brief Register the Canvas3D widget draw-ops binding (idempotent; called at
///        Canvas3D creation).
/// @details Publishes the probe and table-fill callbacks to the Game.UI
///   canvas-polymorphism layer without transferring object ownership.
void canvas3d_register_gameui_ops(void) {
    rt_gameui_register_canvas3d_ops(canvas3d_gameui_probe, canvas3d_gameui_fill);
}

#else
typedef int rt_graphics_disabled_tu_guard_canvas3d_gameui;
#endif /* ZANNA_ENABLE_GRAPHICS */
