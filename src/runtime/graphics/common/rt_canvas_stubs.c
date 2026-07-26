//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_canvas_stubs.c
/// @brief Graphics-disabled Canvas2D entry points and window-state stubs.
///
/// @details This split source keeps all Canvas2D unavailable-backend behavior
/// in one translation unit while preserving the public runtime symbols from the
/// original monolithic stub implementation.
///
// File: src/runtime/graphics/common/rt_canvas_stubs.c
// Purpose: Graphics-disabled Canvas2D entry points and window-state stubs.
//
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Stateful graphics APIs fail with the shared InvalidOperation trap.
//   - Backend-independent query helpers keep their documented fallback values.
//
// Ownership/Lifetime:
//   - Stub entry points allocate no graphics resources and retain no handles.
//
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"

/// @brief Report whether the Canvas runtime is usable in this build.
///
/// Mirror of the real implementation's availability flag. In the stubs build
/// this always returns 0 so frontends and Zia/BASIC programs can branch on
/// graphics availability with the canonical
/// `if (Canvas.IsAvailable()) { ... }` pattern instead of crashing on the
/// first Canvas operation. Cheap enough to be called on every frame.
///
/// @return 0 — graphics support was excluded at configure time. The
///         non-stub build returns 1.
int8_t rt_canvas_is_available(void) {
    return 0;
}

/// @brief Stub for `Canvas.New` — would normally allocate and return a
///        Canvas window handle.
///
/// This stub is reached when a program calls `Canvas.New(title, w, h)` in a
/// build without `ZANNA_ENABLE_GRAPHICS`. There is no underlying window
/// system, so we cannot construct a Canvas at all; the only correct response
/// is to raise an `InvalidOperation` trap so the failure surfaces at the call
/// site instead of returning a NULL handle that would crash later inside
/// `Flip`/`Box`/etc.
///
/// @param title  Window title (ignored).
/// @param width  Requested canvas width in pixels (ignored).
/// @param height Requested canvas height in pixels (ignored).
///
/// @return Never returns normally; control transfers to the active trap
///         handler. The `NULL` literal in the macro exists only to satisfy
///         the C return-type rule.
void *rt_canvas_new(rt_string title, int64_t width, int64_t height) {
    (void)title;
    (void)width;
    (void)height;
    RT_GRAPHICS_TRAP_RET("Canvas.New: graphics support not compiled in", NULL);
}

/// @brief Stub: graphics disabled — returns 0; no canvas handle can be valid without graphics.
///
/// @param canvas Candidate Canvas handle (ignored).
///
/// @return `0`.
int8_t rt_canvas_is_handle(void *canvas) {
    (void)canvas;
    return 0;
}

/// @brief Stub for `Canvas.Destroy` — would normally release the underlying
///        OS window, framebuffer, and event queue.
///
/// In the stubs build there is no Canvas to destroy, so this is a true no-op
/// (rather than a trap) — destroying a never-acquired resource is safe and
/// makes finalizer calls in defensive code paths harmless. Reaching this
/// stub almost always means the caller already trapped at `Canvas.New`.
///
/// @param canvas Canvas handle (always NULL in the stubs build; ignored).
void rt_canvas_destroy(void *canvas) {
    (void)canvas;
}

/// @brief Stub for `Canvas.Width` — would normally return the canvas width
///        in logical pixels.
///
/// Without a real Canvas there is no meaningful width to report. Traps with
/// `InvalidOperation` so the failure surfaces at the property access rather
/// than silently feeding `0` into downstream layout / draw math.
///
/// @param canvas Canvas handle (ignored).
///
/// @return Never returns normally; the `0` exists only to satisfy the
///         C type system after the trap.
int64_t rt_canvas_width(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.Width: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.Height` — would normally return the canvas height
///        in logical pixels.
///
/// Same rationale as `rt_canvas_width`: traps so the absent backend cannot
/// be mistaken for a zero-sized window.
///
/// @param canvas Canvas handle (ignored).
///
/// @return Never returns normally.
int64_t rt_canvas_height(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.Height: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.ShouldClose` — would normally report whether the
///        OS window has received a close request.
///
/// Returning `1` (close requested) would be tempting but unreachable, since
/// the trap fires first. The `1` literal exists for type-system reasons; the
/// trap is the actual contract.
///
/// @param canvas Canvas handle (ignored).
///
/// @return Never returns normally.
int64_t rt_canvas_should_close(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.ShouldClose: graphics support not compiled in", 1);
}

/// @brief Stub for `Canvas.Flip` — would normally present the back buffer
///        to the screen (double-buffer swap).
///
/// `Flip` is the most common per-frame call site, so the diagnostic message
/// uses the fully qualified name to make profiling output unambiguous when a
/// game loop hits this stub on every frame.
///
/// @param canvas Canvas handle (ignored).
void rt_canvas_flip(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_VOID("Canvas.Flip: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Clear` — would normally fill the back buffer
///        with a solid color.
///
/// @param canvas Canvas handle (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_clear(void *canvas, int64_t color) {
    (void)canvas;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Clear: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Line` — would normally draw a 1px line segment.
///
/// Endpoints are inclusive on both ends, in canvas-pixel coordinates with
/// origin at the top-left and +Y pointing down (matches the screen-space
/// convention used everywhere else in the runtime).
///
/// @param canvas Canvas handle (ignored).
/// @param x1     Start point x (ignored).
/// @param y1     Start point y (ignored).
/// @param x2     End point x (ignored).
/// @param y2     End point y (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_line(void *canvas, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
    (void)canvas;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Line: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Box` — would normally draw a filled axis-aligned
///        rectangle.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Left edge in canvas pixels (ignored).
/// @param y      Top edge in canvas pixels (ignored).
/// @param w      Width in pixels; must be positive (ignored).
/// @param h      Height in pixels; must be positive (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_box(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Box: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Frame` — would normally draw an unfilled axis-
///        aligned rectangle (outline only, 1px wide).
///
/// @param canvas Canvas handle (ignored).
/// @param x      Left edge in canvas pixels (ignored).
/// @param y      Top edge in canvas pixels (ignored).
/// @param w      Width in pixels; must be positive (ignored).
/// @param h      Height in pixels; must be positive (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_frame(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Frame: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Disc` — would normally draw a filled circle.
///
/// Center is given in canvas pixels; radius is the distance from the center
/// to the rim (so the bounding box is `2*radius+1` pixels wide and tall).
///
/// @param canvas Canvas handle (ignored).
/// @param cx     Center x (ignored).
/// @param cy     Center y (ignored).
/// @param radius Radius in pixels; must be non-negative (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_disc(void *canvas, int64_t cx, int64_t cy, int64_t radius, int64_t color) {
    (void)canvas;
    (void)cx;
    (void)cy;
    (void)radius;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Disc: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Ring` — would normally draw an unfilled circle
///        (outline only, 1px wide).
///
/// @param canvas Canvas handle (ignored).
/// @param cx     Center x (ignored).
/// @param cy     Center y (ignored).
/// @param radius Radius in pixels; must be non-negative (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_ring(void *canvas, int64_t cx, int64_t cy, int64_t radius, int64_t color) {
    (void)canvas;
    (void)cx;
    (void)cy;
    (void)radius;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Ring: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Plot` — would normally light a single pixel.
///
/// `Plot` is the lowest-level draw primitive; everything else (Line, Box,
/// Disc) ultimately decomposes into Plot calls in the software renderer.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Pixel x in canvas coordinates (ignored).
/// @param y      Pixel y in canvas coordinates (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_plot(void *canvas, int64_t x, int64_t y, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Plot: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Poll` — would normally pump the OS event queue
///        and update keyboard/mouse/gamepad input snapshots for this frame.
///
/// In the real implementation this is the per-frame entry point that drives
/// `WasPressed` / `WasReleased` edge detection. Without a window there is
/// nothing to poll, so this traps so the missing event source is loud.
///
/// @param canvas Canvas handle (ignored).
///
/// @return Never returns normally.
int64_t rt_canvas_poll(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.Poll: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.KeyHeld` — would normally report whether the
///        given key is currently held down.
///
/// @param canvas Canvas handle (ignored).
/// @param key    Platform-native key code (ignored).
///
/// @return Never returns normally.
int64_t rt_canvas_key_held(void *canvas, int64_t key) {
    (void)canvas;
    (void)key;
    RT_GRAPHICS_TRAP_RET("Canvas.KeyHeld: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.Text` — would normally render a string in the
///        built-in 8x8 bitmap font.
///
/// `(x, y)` is the top-left of the first glyph; subsequent glyphs are laid
/// out left-to-right at 8px stride. No kerning, no bidi, no shaping.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Top-left x of the first glyph in canvas pixels (ignored).
/// @param y      Top-left y of the first glyph in canvas pixels (ignored).
/// @param text   Glyph source string (ignored).
/// @param color  Packed 0xAARRGGBB foreground color (ignored).
void rt_canvas_text(void *canvas, int64_t x, int64_t y, rt_string text, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)text;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Text: graphics support not compiled in");
}

/// @brief Stub for `Canvas.TextBg` — would normally render text with both a
///        foreground glyph color and an opaque background fill behind each
///        glyph cell.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Top-left x of the first glyph (ignored).
/// @param y      Top-left y of the first glyph (ignored).
/// @param text   Glyph source string (ignored).
/// @param fg     Packed 0xAARRGGBB foreground color (ignored).
/// @param bg     Packed 0xAARRGGBB background fill color (ignored).
void rt_canvas_text_bg(void *canvas, int64_t x, int64_t y, rt_string text, int64_t fg, int64_t bg) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)text;
    (void)fg;
    (void)bg;
    RT_GRAPHICS_TRAP_VOID("Canvas.TextBg: graphics support not compiled in");
}

/// @brief Compute the width in pixels of `text` rendered in the built-in
///        8x8 bitmap font.
///
/// Unlike most stubs in this file, text *measurement* is intentionally still
/// functional in graphics-disabled builds — see the file header. The font is
/// fixed-width 8 pixels per glyph, so the answer is always
/// `length(text) * 8` (or `0` for a NULL string).
///
/// @param text Source string. NULL is treated as empty.
///
/// @return Width in canvas pixels. `0` if `text` is NULL.
int64_t rt_canvas_text_width(rt_string text) {
    if (!text)
        return 0;
    return rt_str_len(text) * 8;
}

/// @brief Return the row height of the built-in 8x8 bitmap font.
///
/// Constant 8 — the font is fixed-cell. Like `rt_canvas_text_width`, this is
/// a real implementation rather than a trap stub: text metrics work in any
/// build because they don't depend on the windowing backend.
///
/// @return Always `8`.
int64_t rt_canvas_text_height(void) {
    return 8;
}

/// @brief Stub for `Canvas.TextScaled` — would normally render text in the
///        built-in font with each pixel duplicated `scale` times along both
///        axes (nearest-neighbor blow-up, no smoothing).
///
/// @param canvas Canvas handle (ignored).
/// @param x      Top-left x of the first glyph (ignored).
/// @param y      Top-left y of the first glyph (ignored).
/// @param text   Glyph source string (ignored).
/// @param scale  Integer pixel multiplier; must be >= 1 (ignored).
/// @param color  Packed 0xAARRGGBB foreground color (ignored).
void rt_canvas_text_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)text;
    (void)scale;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.TextScaled: graphics support not compiled in");
}

/// @brief Stub for `Canvas.TextScaledBg` — scaled-text variant with an
///        opaque background fill behind each glyph cell.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Top-left x of the first glyph (ignored).
/// @param y      Top-left y of the first glyph (ignored).
/// @param text   Glyph source string (ignored).
/// @param scale  Integer pixel multiplier; must be >= 1 (ignored).
/// @param fg     Packed 0xAARRGGBB foreground color (ignored).
/// @param bg     Packed 0xAARRGGBB background fill color (ignored).
void rt_canvas_text_scaled_bg(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t fg, int64_t bg) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)text;
    (void)scale;
    (void)fg;
    (void)bg;
    RT_GRAPHICS_TRAP_VOID("Canvas.TextScaledBg: graphics support not compiled in");
}

/// @brief Compute the rendered width of `text` at the given integer scale
///        in the built-in 8x8 bitmap font.
///
/// Real implementation (not a trap stub) — text metrics are backend-free.
/// Returns `0` for NULL text or non-positive `scale` so callers can use this
/// for layout math without first validating arguments.
///
/// @param text  Source string. NULL is treated as empty.
/// @param scale Integer pixel multiplier; values < 1 produce `0`.
///
/// @return Width in canvas pixels: `length(text) * 8 * scale`, or `0`.
int64_t rt_canvas_text_scaled_width(rt_string text, int64_t scale) {
    if (!text || scale < 1)
        return 0;
    return rt_str_len(text) * 8 * scale;
}

/// @brief Stub for `Canvas.TextCentered` — would normally horizontally
///        center `text` on the canvas at vertical position `y`.
///
/// @param canvas Canvas handle (ignored).
/// @param y      Top-left y for the rendered text (ignored).
/// @param text   Glyph source string (ignored).
/// @param color  Packed 0xAARRGGBB foreground color (ignored).
void rt_canvas_text_centered(void *canvas, int64_t y, rt_string text, int64_t color) {
    (void)canvas;
    (void)y;
    (void)text;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.TextCentered: graphics support not compiled in");
}

/// @brief Stub for `Canvas.TextRight` — would normally right-align `text`
///        with the right edge `margin` pixels in from the canvas's right
///        side.
///
/// @param canvas Canvas handle (ignored).
/// @param margin Distance from the right edge of the canvas in pixels (ignored).
/// @param y      Top-left y for the rendered text (ignored).
/// @param text   Glyph source string (ignored).
/// @param color  Packed 0xAARRGGBB foreground color (ignored).
void rt_canvas_text_right(void *canvas, int64_t margin, int64_t y, rt_string text, int64_t color) {
    (void)canvas;
    (void)margin;
    (void)y;
    (void)text;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.TextRight: graphics support not compiled in");
}

/// @brief Stub for `Canvas.TextCenteredScaled` — would horizontally center
///        `text` rendered at `scale` magnification at vertical position `y`.
///
/// @param canvas Canvas handle (ignored).
/// @param y      Top-left y for the rendered text (ignored).
/// @param text   Glyph source string (ignored).
/// @param color  Packed 0xAARRGGBB foreground color (ignored).
/// @param scale  Integer pixel multiplier; must be >= 1 (ignored).
void rt_canvas_text_centered_scaled(
    void *canvas, int64_t y, rt_string text, int64_t color, int64_t scale) {
    (void)canvas;
    (void)y;
    (void)text;
    (void)scale;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.TextCenteredScaled: graphics support not compiled in");
}

/// @brief Stub for `Canvas.BoxAlpha` — would normally draw a filled
///        rectangle blended with the destination using `alpha` as the
///        per-channel coverage (0 = transparent, 255 = opaque).
///
/// The `color` argument's own alpha byte is ignored; only the explicit
/// `alpha` parameter controls blending so callers can reuse opaque palette
/// constants without rebuilding them.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Left edge in canvas pixels (ignored).
/// @param y      Top edge in canvas pixels (ignored).
/// @param w      Width in pixels; must be positive (ignored).
/// @param h      Height in pixels; must be positive (ignored).
/// @param color  Packed 0xAARRGGBB color; alpha byte ignored (ignored).
/// @param alpha  Coverage 0..255 (ignored).
void rt_canvas_box_alpha(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, int64_t alpha) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
    (void)alpha;
    RT_GRAPHICS_TRAP_VOID("Canvas.BoxAlpha: graphics support not compiled in");
}

/// @brief Stub for `Canvas.DiscAlpha` — alpha-blended filled circle. See
///        `rt_canvas_box_alpha` for blending semantics.
///
/// @param canvas Canvas handle (ignored).
/// @param cx     Center x (ignored).
/// @param cy     Center y (ignored).
/// @param radius Radius in pixels; must be non-negative (ignored).
/// @param color  Packed 0xAARRGGBB color; alpha byte ignored (ignored).
/// @param alpha  Coverage 0..255 (ignored).
void rt_canvas_disc_alpha(
    void *canvas, int64_t cx, int64_t cy, int64_t radius, int64_t color, int64_t alpha) {
    (void)canvas;
    (void)cx;
    (void)cy;
    (void)radius;
    (void)color;
    (void)alpha;
    RT_GRAPHICS_TRAP_VOID("Canvas.DiscAlpha: graphics support not compiled in");
}

/// @brief Stub for `Canvas.EllipseAlpha` — alpha-blended filled axis-aligned
///        ellipse with independent x and y radii.
///
/// @param canvas Canvas handle (ignored).
/// @param cx     Center x (ignored).
/// @param cy     Center y (ignored).
/// @param rx     X-axis radius in pixels (ignored).
/// @param ry     Y-axis radius in pixels (ignored).
/// @param color  Packed 0xAARRGGBB color; alpha byte ignored (ignored).
/// @param alpha  Coverage 0..255 (ignored).
void rt_canvas_ellipse_alpha(
    void *canvas, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color, int64_t alpha) {
    (void)canvas;
    (void)cx;
    (void)cy;
    (void)rx;
    (void)ry;
    (void)color;
    (void)alpha;
    RT_GRAPHICS_TRAP_VOID("Canvas.EllipseAlpha: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Blit` — would normally copy a Pixels surface
///        onto the canvas at `(x, y)` using straight overwrite (no blending).
///
/// `pixels` is opaque to this function — the real implementation reads the
/// width/height/stride out of the underlying `rt_pixels` object.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Top-left destination x (ignored).
/// @param y      Top-left destination y (ignored).
/// @param pixels Source `rt_pixels` handle (ignored).
void rt_canvas_blit(void *canvas, int64_t x, int64_t y, void *pixels) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)pixels;
    RT_GRAPHICS_TRAP_VOID("Canvas.Blit: graphics support not compiled in");
}

/// @brief Stub for `Canvas.BlitRegion` — would normally copy a sub-rect of
///        a Pixels surface onto the canvas with straight overwrite.
///
/// `(sx, sy, w, h)` selects a region within the source; `(dx, dy)` is the
/// destination origin. Used by sprite sheets and tile atlases to draw a
/// single frame without slicing the source asset.
///
/// @param canvas Canvas handle (ignored).
/// @param dx     Destination top-left x (ignored).
/// @param dy     Destination top-left y (ignored).
/// @param pixels Source `rt_pixels` handle (ignored).
/// @param sx     Source rect top-left x (ignored).
/// @param sy     Source rect top-left y (ignored).
/// @param w      Source rect width in pixels (ignored).
/// @param h      Source rect height in pixels (ignored).
void rt_canvas_blit_region(void *canvas,
                           int64_t dx,
                           int64_t dy,
                           void *pixels,
                           int64_t sx,
                           int64_t sy,
                           int64_t w,
                           int64_t h) {
    (void)canvas;
    (void)dx;
    (void)dy;
    (void)pixels;
    (void)sx;
    (void)sy;
    (void)w;
    (void)h;
    RT_GRAPHICS_TRAP_VOID("Canvas.BlitRegion: graphics support not compiled in");
}

/// @brief Stub for `Canvas.BlitAlpha` — would normally copy a Pixels surface
///        onto the canvas honoring per-pixel alpha (source-over blending).
///
/// Distinct from `rt_canvas_blit` which always overwrites; this variant is
/// what sprites with transparent pixels use.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Top-left destination x (ignored).
/// @param y      Top-left destination y (ignored).
/// @param pixels Source `rt_pixels` handle with per-pixel alpha (ignored).
void rt_canvas_blit_alpha(void *canvas, int64_t x, int64_t y, void *pixels) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)pixels;
    RT_GRAPHICS_TRAP_VOID("Canvas.BlitAlpha: graphics support not compiled in");
}

/// @brief Stub for `Canvas.ThickLine` — would normally draw a line segment
///        with `thickness` pixels of width.
///
/// `thickness == 1` would degenerate to `rt_canvas_line`. The real
/// implementation expands the centerline by `thickness/2` perpendicular to
/// the segment direction.
///
/// @param canvas    Canvas handle (ignored).
/// @param x1        Start point x (ignored).
/// @param y1        Start point y (ignored).
/// @param x2        End point x (ignored).
/// @param y2        End point y (ignored).
/// @param thickness Line width in pixels; must be >= 1 (ignored).
/// @param color     Packed 0xAARRGGBB color (ignored).
void rt_canvas_thick_line(void *canvas,
                          int64_t x1,
                          int64_t y1,
                          int64_t x2,
                          int64_t y2,
                          int64_t thickness,
                          int64_t color) {
    (void)canvas;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)thickness;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.ThickLine: graphics support not compiled in");
}

/// @brief Stub for `Canvas.RoundBox` — would normally draw a filled
///        rounded-rectangle with corner `radius`.
///
/// Used by GUI widgets and HUD chrome. `radius` is clamped to half of the
/// shorter side in the real implementation; passing `0` degenerates to
/// `rt_canvas_box`.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Left edge in canvas pixels (ignored).
/// @param y      Top edge in canvas pixels (ignored).
/// @param w      Width in pixels; must be positive (ignored).
/// @param h      Height in pixels; must be positive (ignored).
/// @param radius Corner radius in pixels (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_round_box(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)radius;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.RoundBox: graphics support not compiled in");
}

/// @brief Stub for `Canvas.RoundFrame` — would normally draw the outline
///        (1px wide) of a rounded rectangle with corner `radius`.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Left edge in canvas pixels (ignored).
/// @param y      Top edge in canvas pixels (ignored).
/// @param w      Width in pixels; must be positive (ignored).
/// @param h      Height in pixels; must be positive (ignored).
/// @param radius Corner radius in pixels (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_round_frame(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)radius;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.RoundFrame: graphics support not compiled in");
}

/// @brief Stub for `Canvas.FloodFill` — would normally flood-fill the
///        4-connected region of pixels matching the color at `(x, y)` with
///        `color`.
///
/// Operates in screen-space using the framebuffer's current contents. The
/// real implementation uses an iterative scanline fill to avoid recursion
/// blowups on large regions.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Seed pixel x (ignored).
/// @param y      Seed pixel y (ignored).
/// @param color  Replacement color, packed 0xAARRGGBB (ignored).
void rt_canvas_flood_fill(void *canvas, int64_t x, int64_t y, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.FloodFill: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Triangle` — would normally draw a filled
///        triangle with the three given vertices.
///
/// Vertex winding does not matter; the rasterizer is direction-agnostic.
///
/// @param canvas Canvas handle (ignored).
/// @param x1     Vertex 1 x (ignored).
/// @param y1     Vertex 1 y (ignored).
/// @param x2     Vertex 2 x (ignored).
/// @param y2     Vertex 2 y (ignored).
/// @param x3     Vertex 3 x (ignored).
/// @param y3     Vertex 3 y (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_triangle(void *canvas,
                        int64_t x1,
                        int64_t y1,
                        int64_t x2,
                        int64_t y2,
                        int64_t x3,
                        int64_t y3,
                        int64_t color) {
    (void)canvas;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Triangle: graphics support not compiled in");
}

/// @brief Stub for `Canvas.TriangleFrame` — would normally draw the outline
///        (3 line segments) of a triangle.
///
/// @param canvas Canvas handle (ignored).
/// @param x1     Vertex 1 x (ignored).
/// @param y1     Vertex 1 y (ignored).
/// @param x2     Vertex 2 x (ignored).
/// @param y2     Vertex 2 y (ignored).
/// @param x3     Vertex 3 x (ignored).
/// @param y3     Vertex 3 y (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_triangle_frame(void *canvas,
                              int64_t x1,
                              int64_t y1,
                              int64_t x2,
                              int64_t y2,
                              int64_t x3,
                              int64_t y3,
                              int64_t color) {
    (void)canvas;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.TriangleFrame: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Ellipse` — would normally draw a filled
///        axis-aligned ellipse with independent x and y radii.
///
/// @param canvas Canvas handle (ignored).
/// @param cx     Center x (ignored).
/// @param cy     Center y (ignored).
/// @param rx     X-axis radius in pixels (ignored).
/// @param ry     Y-axis radius in pixels (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_ellipse(
    void *canvas, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color) {
    (void)canvas;
    (void)cx;
    (void)cy;
    (void)rx;
    (void)ry;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Ellipse: graphics support not compiled in");
}

/// @brief Stub for `Canvas.EllipseFrame` — would normally draw the outline
///        of an axis-aligned ellipse.
///
/// @param canvas Canvas handle (ignored).
/// @param cx     Center x (ignored).
/// @param cy     Center y (ignored).
/// @param rx     X-axis radius in pixels (ignored).
/// @param ry     Y-axis radius in pixels (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_ellipse_frame(
    void *canvas, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color) {
    (void)canvas;
    (void)cx;
    (void)cy;
    (void)rx;
    (void)ry;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.EllipseFrame: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Arc` — would normally draw a filled circular
///        arc between `start_angle` and `end_angle`.
///
/// Angles are in degrees, measured clockwise from 12 o'clock (matching the
/// rest of the runtime's angle convention). The "filled" form is a pie
/// slice — connecting the arc endpoints back to the center.
///
/// @param canvas      Canvas handle (ignored).
/// @param cx          Center x (ignored).
/// @param cy          Center y (ignored).
/// @param radius      Radius in pixels (ignored).
/// @param start_angle Sweep start in degrees (ignored).
/// @param end_angle   Sweep end in degrees (ignored).
/// @param color       Packed 0xAARRGGBB color (ignored).
void rt_canvas_arc(void *canvas,
                   int64_t cx,
                   int64_t cy,
                   int64_t radius,
                   int64_t start_angle,
                   int64_t end_angle,
                   int64_t color) {
    (void)canvas;
    (void)cx;
    (void)cy;
    (void)radius;
    (void)start_angle;
    (void)end_angle;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Arc: graphics support not compiled in");
}

/// @brief Stub for `Canvas.ArcFrame` — outline-only variant of `Arc`. Draws
///        just the curved rim, not the radial spokes connecting the
///        endpoints to the center.
///
/// @param canvas      Canvas handle (ignored).
/// @param cx          Center x (ignored).
/// @param cy          Center y (ignored).
/// @param radius      Radius in pixels (ignored).
/// @param start_angle Sweep start in degrees (ignored).
/// @param end_angle   Sweep end in degrees (ignored).
/// @param color       Packed 0xAARRGGBB color (ignored).
void rt_canvas_arc_frame(void *canvas,
                         int64_t cx,
                         int64_t cy,
                         int64_t radius,
                         int64_t start_angle,
                         int64_t end_angle,
                         int64_t color) {
    (void)canvas;
    (void)cx;
    (void)cy;
    (void)radius;
    (void)start_angle;
    (void)end_angle;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.ArcFrame: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Bezier` — would normally draw a quadratic
///        Bézier curve from `(x1, y1)` to `(x2, y2)` with `(cx, cy)` as the
///        single control point.
///
/// @param canvas Canvas handle (ignored).
/// @param x1     Start point x (ignored).
/// @param y1     Start point y (ignored).
/// @param cx     Control point x (ignored).
/// @param cy     Control point y (ignored).
/// @param x2     End point x (ignored).
/// @param y2     End point y (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_bezier(void *canvas,
                      int64_t x1,
                      int64_t y1,
                      int64_t cx,
                      int64_t cy,
                      int64_t x2,
                      int64_t y2,
                      int64_t color) {
    (void)canvas;
    (void)x1;
    (void)y1;
    (void)cx;
    (void)cy;
    (void)x2;
    (void)y2;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Bezier: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Polyline` — would normally draw a sequence of
///        line segments connecting consecutive vertices in `points`.
///
/// `points` is an `rt_int64list`-style buffer of interleaved `(x, y)` pairs;
/// `count` is the number of vertices (so the buffer holds `2*count` ints).
/// The polyline is open — the last vertex is *not* connected back to the
/// first. Use `rt_canvas_polygon_frame` for a closed outline.
///
/// @param canvas Canvas handle (ignored).
/// @param points Vertex buffer of interleaved (x, y) ints (ignored).
/// @param count  Vertex count (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_polyline(void *canvas, void *points, int64_t count, int64_t color) {
    (void)canvas;
    (void)points;
    (void)count;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Polyline: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Polygon` — would normally draw a filled polygon
///        with the given vertices.
///
/// Vertex buffer layout matches `rt_canvas_polyline`. The polygon is implicitly
/// closed (an edge is drawn from the last vertex back to the first).
/// Self-intersecting polygons follow the even-odd fill rule.
///
/// @param canvas Canvas handle (ignored).
/// @param points Vertex buffer of interleaved (x, y) ints (ignored).
/// @param count  Vertex count (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_polygon(void *canvas, void *points, int64_t count, int64_t color) {
    (void)canvas;
    (void)points;
    (void)count;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.Polygon: graphics support not compiled in");
}

/// @brief Stub for `Canvas.PolygonFrame` — outline-only variant of
///        `Polygon`. Draws closed-path edges only, no fill.
///
/// @param canvas Canvas handle (ignored).
/// @param points Vertex buffer of interleaved (x, y) ints (ignored).
/// @param count  Vertex count (ignored).
/// @param color  Packed 0xAARRGGBB color (ignored).
void rt_canvas_polygon_frame(void *canvas, void *points, int64_t count, int64_t color) {
    (void)canvas;
    (void)points;
    (void)count;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.PolygonFrame: graphics support not compiled in");
}

/// @brief Trapping stub for drawing the open polyline stored in a path object.
///
/// @param canvas Canvas handle (ignored before trapping).
/// @param path   Path2D-style vertex collection (ignored before trapping).
/// @param color  Packed color (ignored before trapping).
void rt_canvas_polyline_path(void *canvas, void *path, int64_t color) {
    (void)canvas;
    (void)path;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.PolylinePath: graphics support not compiled in");
}

/// @brief Trapping stub for filling the closed polygon stored in a path object.
///
/// @param canvas Canvas handle (ignored before trapping).
/// @param path   Path2D-style vertex collection (ignored before trapping).
/// @param color  Packed color (ignored before trapping).
void rt_canvas_polygon_path(void *canvas, void *path, int64_t color) {
    (void)canvas;
    (void)path;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.PolygonPath: graphics support not compiled in");
}

/// @brief Trapping stub for outlining the closed polygon stored in a path object.
///
/// @param canvas Canvas handle (ignored before trapping).
/// @param path   Path2D-style vertex collection (ignored before trapping).
/// @param color  Packed color (ignored before trapping).
void rt_canvas_polygon_frame_path(void *canvas, void *path, int64_t color) {
    (void)canvas;
    (void)path;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas.PolygonFramePath: graphics support not compiled in");
}

/// @brief Stub for `Canvas.GetPixel` — would normally read the
///        framebuffer's current color at `(x, y)`.
///
/// Out-of-bounds reads in the real implementation return `0`; here the call
/// traps before it can be tested.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Pixel x in canvas coordinates (ignored).
/// @param y      Pixel y in canvas coordinates (ignored).
///
/// @return Never returns normally.
int64_t rt_canvas_get_pixel(void *canvas, int64_t x, int64_t y) {
    (void)canvas;
    (void)x;
    (void)y;
    RT_GRAPHICS_TRAP_RET("Canvas.GetPixel: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.CopyRect` — would normally allocate a fresh
///        Pixels surface and copy the canvas's `(x, y, w, h)` rect into it.
///
/// In the real implementation the returned object is GC-managed and owns
/// its own storage (no aliasing back to the canvas).
///
/// @param canvas Canvas handle (ignored).
/// @param x      Source rect top-left x (ignored).
/// @param y      Source rect top-left y (ignored).
/// @param w      Source rect width (ignored).
/// @param h      Source rect height (ignored).
///
/// @return Never returns normally.
void *rt_canvas_copy_rect(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    RT_GRAPHICS_TRAP_RET("Canvas.CopyRect: graphics support not compiled in", NULL);
}

/// @brief Stub for `Canvas.SaveBmp` — would normally serialize the
///        canvas's current framebuffer to a 24-bit BMP file at `path`.
///
/// @param canvas Canvas handle (ignored).
/// @param path   Output file path (ignored).
///
/// @return Never returns normally; would otherwise return non-zero on
///         success and `0` on I/O failure.
int64_t rt_canvas_save_bmp(void *canvas, rt_string path) {
    (void)canvas;
    (void)path;
    RT_GRAPHICS_TRAP_RET("Canvas.SaveBmp: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.SavePng` — would normally serialize the
///        canvas's current framebuffer to a PNG file at `path`.
///
/// The real implementation uses Zanna's from-scratch PNG encoder (no
/// libpng) and supports all 5 PNG color types.
///
/// @param canvas Canvas handle (ignored).
/// @param path   Output file path (ignored).
///
/// @return Never returns normally; would otherwise return non-zero on
///         success and `0` on I/O failure.
int64_t rt_canvas_save_png(void *canvas, rt_string path) {
    (void)canvas;
    (void)path;
    RT_GRAPHICS_TRAP_RET("Canvas.SavePng: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.SetClipRect` — would normally constrain
///        subsequent draw calls to the rectangle `(x, y, w, h)`.
///
/// The clip rect is stateful and persists until the next call to
/// `SetClipRect` or `ClearClipRect`. Pixels outside the clip are dropped
/// before they hit the framebuffer.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Left edge of clip rect in canvas pixels (ignored).
/// @param y      Top edge of clip rect (ignored).
/// @param w      Width of clip rect; must be positive (ignored).
/// @param h      Height of clip rect; must be positive (ignored).
void rt_canvas_set_clip_rect(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    RT_GRAPHICS_TRAP_VOID("Canvas.SetClipRect: graphics support not compiled in");
}

/// @brief Stub for `Canvas.ClearClipRect` — would normally restore the
///        clip rect to the full canvas (no clipping).
///
/// @param canvas Canvas handle (ignored).
void rt_canvas_clear_clip_rect(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_VOID("Canvas.ClearClipRect: graphics support not compiled in");
}

/// @brief Stub for `Canvas.SetTitle` — would normally update the OS
///        window's title bar.
///
/// @param canvas Canvas handle (ignored).
/// @param title  New title string; lifetime managed by the caller (ignored).
void rt_canvas_set_title(void *canvas, rt_string title) {
    (void)canvas;
    (void)title;
    RT_GRAPHICS_TRAP_VOID("Canvas.SetTitle: graphics support not compiled in");
}

/// @brief Stub for `Canvas.GetTitle` — would normally return the title
///        string passed to `Canvas.New` (or last `SetTitle` call).
///
/// @param canvas Canvas handle (ignored).
///
/// @return Never returns normally; the empty-string fallback is unreachable.
rt_string rt_canvas_get_title(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.GetTitle: graphics support not compiled in", rt_str_empty());
}

/// @brief Stub for `Canvas.Resize` — would normally request the OS to
///        resize the underlying window to `(width, height)` logical pixels.
///
/// On macOS the window's frame origin is preserved; the new size may be
/// clipped by the screen bounds.
///
/// @param canvas Canvas handle (ignored).
/// @param width  Requested logical width in pixels (ignored).
/// @param height Requested logical height in pixels (ignored).
void rt_canvas_resize(void *canvas, int64_t width, int64_t height) {
    (void)canvas;
    (void)width;
    (void)height;
    RT_GRAPHICS_TRAP_VOID("Canvas.Resize: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Close` — would normally request the window be
///        closed and signal `should_close` to the game loop.
///
/// Distinct from `Canvas.Destroy`: `Close` is a graceful request that the
/// game loop can observe and respond to (saving state, etc.); `Destroy`
/// tears the window down immediately.
///
/// @param canvas Canvas handle (ignored).
void rt_canvas_close(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_VOID("Canvas.Close: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Screenshot` — would normally allocate a fresh
///        Pixels surface containing the current framebuffer contents.
///
/// The returned object is GC-managed and owns its own storage in the real
/// implementation.
///
/// @param canvas Canvas handle (ignored).
///
/// @return Never returns normally.
void *rt_canvas_screenshot(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.Screenshot: graphics support not compiled in", NULL);
}

/// @brief Stub for `Canvas.Fullscreen` — would normally switch the window
///        to exclusive or borderless fullscreen mode.
///
/// Mode selection (exclusive vs borderless) is a backend choice; the
/// runtime API does not expose it.
///
/// @param canvas Canvas handle (ignored).
void rt_canvas_fullscreen(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_VOID("Canvas.Fullscreen: graphics support not compiled in");
}

/// @brief Stub for `Canvas.Windowed` — would normally restore a fullscreen
///        window to its previous windowed dimensions and frame origin.
///
/// @param canvas Canvas handle (ignored).
void rt_canvas_windowed(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_VOID("Canvas.Windowed: graphics support not compiled in");
}

/// @brief Stub for `Canvas.GradientH` — would normally draw a horizontal
///        linear gradient from `c1` (left edge) to `c2` (right edge) inside
///        the rectangle `(x, y, w, h)`.
///
/// Interpolation is per-channel linear in 0xRRGGBB space (no gamma
/// correction); see `rt_color_lerp` for the same primitive used for single
/// colors.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Left edge of the gradient rect (ignored).
/// @param y      Top edge of the gradient rect (ignored).
/// @param w      Width in pixels; must be positive (ignored).
/// @param h      Height in pixels; must be positive (ignored).
/// @param c1     Color at the left edge, packed 0xAARRGGBB (ignored).
/// @param c2     Color at the right edge, packed 0xAARRGGBB (ignored).
void rt_canvas_gradient_h(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t c1, int64_t c2) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c1;
    (void)c2;
    RT_GRAPHICS_TRAP_VOID("Canvas.GradientH: graphics support not compiled in");
}

/// @brief Stub for `Canvas.GradientV` — vertical gradient variant of
///        `GradientH`. `c1` is at the top edge, `c2` at the bottom.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Left edge of the gradient rect (ignored).
/// @param y      Top edge of the gradient rect (ignored).
/// @param w      Width in pixels; must be positive (ignored).
/// @param h      Height in pixels; must be positive (ignored).
/// @param c1     Color at the top edge, packed 0xAARRGGBB (ignored).
/// @param c2     Color at the bottom edge, packed 0xAARRGGBB (ignored).
void rt_canvas_gradient_v(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t c1, int64_t c2) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c1;
    (void)c2;
    RT_GRAPHICS_TRAP_VOID("Canvas.GradientV: graphics support not compiled in");
}

/// @brief Stub for `Canvas.GetScale` — would normally return the HiDPI
///        backing scale factor (e.g., 2.0 on Retina displays).
///
/// In the real implementation this is the ratio of physical to logical
/// pixels; canvas drawing math always operates in logical pixels and the
/// runtime applies the scale at present time.
///
/// @param canvas Canvas handle (ignored).
///
/// @return Never returns normally; the `1.0` fallback is unreachable.
double rt_canvas_get_scale(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.GetScale: graphics support not compiled in", 1.0);
}

/// @brief Stub for `Canvas.GetWindowX`.
///
/// @param canvas Canvas handle (ignored before trapping).
///
/// @return Never returns normally.
int64_t rt_canvas_get_window_x(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.GetWindowX: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.GetWindowY`.
///
/// @param canvas Canvas handle (ignored before trapping).
///
/// @return Never returns normally.
int64_t rt_canvas_get_window_y(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.GetWindowY: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.GetPosition` — would normally write the
///        window's current screen-relative top-left corner to `*x` and
///        `*y`.
///
/// @param canvas Canvas handle (ignored).
/// @param x      Out-parameter for the window x position (ignored).
/// @param y      Out-parameter for the window y position (ignored).
void rt_canvas_get_position(void *canvas, int64_t *x, int64_t *y) {
    (void)canvas;
    (void)x;
    (void)y;
    RT_GRAPHICS_TRAP_VOID("Canvas.GetPosition: graphics support not compiled in");
}

/// @brief Stub for `Canvas.SetPosition` — would normally request the OS
///        to move the window so its top-left corner is at `(x, y)` in
///        screen coordinates.
///
/// @param canvas Canvas handle (ignored).
/// @param x      New window x position in screen pixels (ignored).
/// @param y      New window y position in screen pixels (ignored).
void rt_canvas_set_position(void *canvas, int64_t x, int64_t y) {
    (void)canvas;
    (void)x;
    (void)y;
    RT_GRAPHICS_TRAP_VOID("Canvas.SetPosition: graphics support not compiled in");
}

/// @brief Stub for `Canvas.GetFps` — would normally return the target
///        frames-per-second cap requested at `Canvas.New` (or `-1` if
///        uncapped / vsync-driven).
///
/// @param canvas Canvas handle (ignored).
///
/// @return Never returns normally; the `-1` fallback is unreachable.
int64_t rt_canvas_get_fps(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.GetFps: graphics support not compiled in", -1);
}

/// @brief Stub for `Canvas.SetFps` — would normally adjust the
///        frames-per-second cap. `0` disables the cap.
///
/// @param canvas Canvas handle (ignored).
/// @param fps    Target FPS; `0` for uncapped (ignored).
void rt_canvas_set_fps(void *canvas, int64_t fps) {
    (void)canvas;
    (void)fps;
    RT_GRAPHICS_TRAP_VOID("Canvas.SetFps: graphics support not compiled in");
}

/// @brief Get the delta time value.
/// @param canvas
/// @return Result value.
int64_t rt_canvas_get_delta_time(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.DeltaTime: graphics support not compiled in", 0);
}

/// @brief Get the delta time value in seconds.
/// @param canvas
/// @return Result value.
double rt_canvas_get_delta_time_sec(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.DeltaTimeSec: graphics support not compiled in", 0.0);
}

/// @brief Set the dt max value.
/// @param canvas
/// @param max_ms
void rt_canvas_set_dt_max(void *canvas, int64_t max_ms) {
    (void)canvas;
    (void)max_ms;
    RT_GRAPHICS_TRAP_VOID("Canvas.SetDTMax: graphics support not compiled in");
}

/// @brief Begin frame.
/// @param canvas
/// @return Result value.
int64_t rt_canvas_begin_frame(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.BeginFrame: graphics support not compiled in", 0);
}

/// @brief Check if maximized.
/// @param canvas
/// @return Result value.
int8_t rt_canvas_is_maximized(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.IsMaximized: graphics support not compiled in", 0);
}

/// @brief Maximize operation.
///
/// @param canvas Canvas handle (ignored before trapping).
void rt_canvas_maximize(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_VOID("Canvas.Maximize: graphics support not compiled in");
}

/// @brief Check if minimized.
/// @param canvas
/// @return Result value.
int8_t rt_canvas_is_minimized(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.IsMinimized: graphics support not compiled in", 0);
}

/// @brief Minimize operation.
///
/// @param canvas Canvas handle (ignored before trapping).
void rt_canvas_minimize(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_VOID("Canvas.Minimize: graphics support not compiled in");
}

/// @brief Restore operation.
///
/// @param canvas Canvas handle (ignored before trapping).
void rt_canvas_restore(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_VOID("Canvas.Restore: graphics support not compiled in");
}

/// @brief Check if focused.
/// @param canvas
/// @return Result value.
int8_t rt_canvas_is_focused(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.IsFocused: graphics support not compiled in", 0);
}

/// @brief Focus operation.
///
/// @param canvas Canvas handle (ignored before trapping).
void rt_canvas_focus(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_VOID("Canvas.Focus: graphics support not compiled in");
}

/// @brief Close the prevent.
///
/// @param canvas  Canvas handle (ignored before trapping).
/// @param prevent Non-zero to suppress close requests (ignored before trapping).
void rt_canvas_prevent_close(void *canvas, int64_t prevent) {
    (void)canvas;
    (void)prevent;
    RT_GRAPHICS_TRAP_VOID("Canvas.PreventClose: graphics support not compiled in");
}

/// @brief Get the monitor size value.
/// @param canvas
/// @param w
/// @param h
void rt_canvas_get_monitor_size(void *canvas, int64_t *w, int64_t *h) {
    (void)canvas;
    (void)w;
    (void)h;
    RT_GRAPHICS_TRAP_VOID("Canvas.GetMonitorSize: graphics support not compiled in");
}

/// @brief Stub for `Canvas.GetMonitorWidth`.
///
/// @param canvas Canvas handle (ignored before trapping).
///
/// @return Never returns normally.
int64_t rt_canvas_get_monitor_width(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.GetMonitorWidth: graphics support not compiled in", 0);
}

/// @brief Stub for `Canvas.GetMonitorHeight`.
///
/// @param canvas Canvas handle (ignored before trapping).
///
/// @return Never returns normally.
int64_t rt_canvas_get_monitor_height(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_TRAP_RET("Canvas.GetMonitorHeight: graphics support not compiled in", 0);
}
