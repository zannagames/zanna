//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_pixels_draw.c
// Purpose: Clipped raster primitives and bitmap-font rendering for
//   Zanna.Graphics.Pixels, including lines, rectangles, circles, ellipses,
//   triangles, quadratic Beziers, flood fill, text, and explicit-alpha
//   compositing.
//
// Key invariants:
//   - Public drawing colors are normalized by rt_pixels_color_to_rgba(), so
//     Canvas-style 0x00RRGGBB, raw 0xRRGGBBAA, and tagged Color.RGBA values
//     share the Pixels buffer's canonical 0xRRGGBBAA storage.
//   - All drawing is clipped to buffer bounds (no out-of-bounds writes).
//   - Flood fill uses an iterative scanline algorithm with a malloc'd stack.
//   - Thick lines are drawn as a series of filled discs along the line.
//   - Triangle fill uses scanline rasterization with sorted vertices.
//   - Text uses the built-in 8x8 ASCII font; unsupported or malformed UTF-8
//     input renders as the question-mark glyph.
//   - Alpha blending uses Porter-Duff "over" compositing.
//
// Ownership/Lifetime:
//   - Drawing functions modify the target Pixels in place (no new allocations).
//   - Flood fill temporarily allocates a work stack (freed before return).
//
// Links: src/runtime/graphics/2d/rt_pixels_internal.h (shared representation),
//        src/runtime/graphics/2d/rt_pixels.c (core operations),
//        src/runtime/graphics/2d/rt_pixels.h (public API),
//        src/runtime/graphics/text/rt_font.h (built-in glyph data)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements clipped Pixels drawing primitives, text, and compositing.
///
/// Every public operation validates the opaque Pixels handle, clips writes to
/// the destination extent, and advances the mutation generation only when at
/// least one stored pixel is written by the requested raster operation.
/// Geometry calculations use saturating integer helpers and bounded
/// `long double` intermediates to keep extreme caller coordinates defined.

#include "rt_pixels.h"
#include "rt_pixels_internal.h"

#include "rt_font.h"
#include "rt_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Drawing Primitives
//=============================================================================

/// @brief Write one opaque Canvas-style RGB pixel.
/// @details Converts the low 24 bits of @p color from `0x00RRGGBB` to canonical
///          `0xRRGGBBFF` storage and delegates validation, clipping, and
///          generation tracking to `rt_pixels_set()`.
/// @param pixels Opaque Pixels handle; a null or invalid handle traps through
///        `rt_pixels_set()`.
/// @param x Zero-based destination column; an out-of-bounds value is a no-op.
/// @param y Zero-based destination row; an out-of-bounds value is a no-op.
/// @param color Canvas-style RGB value whose high bits are ignored.
void rt_pixels_set_rgb(void *pixels, int64_t x, int64_t y, int64_t color) {
    rt_pixels_set(pixels, x, y, (int64_t)rgb_to_rgba(color));
}

/// @brief Read one pixel as Canvas-style RGB.
/// @details Delegates validation and bounds behavior to `rt_pixels_get()` and
///          discards the stored alpha byte.
/// @param pixels Opaque Pixels handle; a null or invalid handle traps through
///        `rt_pixels_get()`.
/// @param x Zero-based source column.
/// @param y Zero-based source row.
/// @return The pixel's `0x00RRGGBB` channels, or the delegated out-of-bounds
///         sentinel after its trap behavior.
int64_t rt_pixels_get_rgb(void *pixels, int64_t x, int64_t y) {
    return rt_pixels_get(pixels, x, y) >> 8;
}

/// @brief Round a long double to int64_t, saturating at INT64_MAX / INT64_MIN.
/// @details Guards the (int64_t) cast against undefined behavior when the value
///   is outside the representable range.  Used when converting floating-point
///   coordinates produced by the rasterizer back into pixel indices.
/// @param value Floating-point coordinate to round to the nearest integer, with
///        halves rounded away from zero.
/// @return The rounded coordinate, clamped to the signed 64-bit range.
static int64_t pixels_round_ld_to_i64_sat(long double value) {
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(value >= 0.0L ? floorl(value + 0.5L) : ceill(value - 0.5L));
}

/// @brief Floor a long double to int64_t, saturating at INT64_MAX / INT64_MIN.
/// @details Used for the left-edge pixel coordinate in span fills — ensures the
///   span starts on the leftmost integer column that is fully inside the shape.
/// @param value Floating-point coordinate to round toward negative infinity.
/// @return The floored coordinate, clamped to the signed 64-bit range.
static int64_t pixels_floor_ld_to_i64_sat(long double value) {
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)floorl(value);
}

/// @brief Ceil a long double to int64_t, saturating at INT64_MAX / INT64_MIN.
/// @details Used for the right-edge pixel coordinate in span fills — ensures the
///   span ends on the rightmost column that is at least partially inside the shape.
/// @param value Floating-point coordinate to round toward positive infinity.
/// @return The ceiling coordinate, clamped to the signed 64-bit range.
static int64_t pixels_ceil_ld_to_i64_sat(long double value) {
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)ceill(value);
}

/// @brief Truncate (round toward zero) a long double to int64_t, saturating at extremes.
/// @details Used for the scanline x-intercept in triangle fills where truncation toward
///   zero matches the expected top-left rasterization rule.
/// @param value Floating-point coordinate to truncate toward zero.
/// @return The truncated coordinate, clamped to the signed 64-bit range.
static int64_t pixels_trunc_ld_to_i64_sat(long double value) {
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)value;
}

/// @brief Cohen–Sutherland / Liang–Barsky inner test for one rectangle boundary.
/// @details Called four times per line (left, right, top, bottom) to shrink the
///   parametric interval [u1, u2] onto the visible portion of the line.  Returns 0
///   (reject) when the line is proven entirely outside the boundary.  @p p is the
///   component of the direction vector pointing into the boundary (negative = moving
///   toward boundary); @p q is the signed distance from the start point to the boundary.
/// @param p Signed direction component for the tested clip boundary.
/// @param q Signed distance from the segment start to that boundary.
/// @param u1 In/out lower bound of the retained parametric segment interval.
/// @param u2 In/out upper bound of the retained parametric segment interval.
/// @return Nonzero when the interval still intersects the boundary half-plane;
///         zero when the segment can be rejected.
static int8_t pixels_clip_line_test(long double p,
                                    long double q,
                                    long double *u1,
                                    long double *u2) {
    if (p == 0.0L)
        return q >= 0.0L;
    long double r = q / p;
    if (p < 0.0L) {
        if (r > *u2)
            return 0;
        if (r > *u1)
            *u1 = r;
    } else {
        if (r < *u1)
            return 0;
        if (r < *u2)
            *u2 = r;
    }
    return 1;
}

/// @brief Forward declaration of the axis-aligned Liang-Barsky clipper.
/// @param min_x Inclusive rectangle left edge.
/// @param min_y Inclusive rectangle top edge.
/// @param max_x Inclusive rectangle right edge.
/// @param max_y Inclusive rectangle bottom edge.
/// @param x1 In/out first endpoint X coordinate.
/// @param y1 In/out first endpoint Y coordinate.
/// @param x2 In/out second endpoint X coordinate.
/// @param y2 In/out second endpoint Y coordinate.
/// @return Nonzero when a visible segment remains; zero otherwise.
static int8_t pixels_clip_line_to_rect(int64_t min_x,
                                       int64_t min_y,
                                       int64_t max_x,
                                       int64_t max_y,
                                       int64_t *x1,
                                       int64_t *y1,
                                       int64_t *x2,
                                       int64_t *y2);

/// @brief Clip a line to the bounds of a Pixels buffer using Liang–Barsky.
/// @details Thin wrapper around pixels_clip_line_to_rect that converts the buffer's
///   width/height into a [0, width-1] × [0, height-1] clip rectangle.  Returns 0
///   if @p p is NULL or has no pixel data, or if the line is entirely outside bounds.
/// @param p Destination implementation whose inclusive pixel bounds form the
///        clip rectangle.
/// @param x1 In/out first endpoint X coordinate.
/// @param y1 In/out first endpoint Y coordinate.
/// @param x2 In/out second endpoint X coordinate.
/// @param y2 In/out second endpoint Y coordinate.
/// @return Nonzero when a visible segment remains; zero for invalid storage,
///         invalid endpoint pointers, or a fully clipped segment.
static int8_t pixels_clip_line_to_bounds(
    rt_pixels_impl *p, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2) {
    if (!p || !p->data || p->width <= 0 || p->height <= 0 || !x1 || !y1 || !x2 || !y2)
        return 0;
    return pixels_clip_line_to_rect(0, 0, p->width - 1, p->height - 1, x1, y1, x2, y2);
}

/// @brief Clip a line segment to an arbitrary axis-aligned rectangle using Liang–Barsky.
/// @details Computes the visible sub-segment of the line defined by (x1,y1)→(x2,y2) that
///   lies inside the rectangle [min_x, max_x] × [min_y, max_y].  Works in long double to
///   avoid integer intermediate overflow when dealing with large coordinates.  Updates
///   the endpoint pointers in place to the clipped values on success.
/// @param min_x Inclusive rectangle left edge.
/// @param min_y Inclusive rectangle top edge.
/// @param max_x Inclusive rectangle right edge.
/// @param max_y Inclusive rectangle bottom edge.
/// @param x1 In/out first endpoint X coordinate.
/// @param y1 In/out first endpoint Y coordinate.
/// @param x2 In/out second endpoint X coordinate.
/// @param y2 In/out second endpoint Y coordinate.
/// @return 1 if any portion of the line is inside the rectangle, 0 if entirely outside.
static int8_t pixels_clip_line_to_rect(int64_t min_x,
                                       int64_t min_y,
                                       int64_t max_x,
                                       int64_t max_y,
                                       int64_t *x1,
                                       int64_t *y1,
                                       int64_t *x2,
                                       int64_t *y2) {
    if (!x1 || !y1 || !x2 || !y2 || min_x > max_x || min_y > max_y)
        return 0;

    long double lx1 = (long double)*x1;
    long double ly1 = (long double)*y1;
    long double dx = (long double)*x2 - lx1;
    long double dy = (long double)*y2 - ly1;
    long double u1 = 0.0L;
    long double u2 = 1.0L;
    long double xmin = (long double)min_x;
    long double ymin = (long double)min_y;
    long double xmax = (long double)max_x;
    long double ymax = (long double)max_y;

    if (!pixels_clip_line_test(-dx, lx1 - xmin, &u1, &u2) ||
        !pixels_clip_line_test(dx, xmax - lx1, &u1, &u2) ||
        !pixels_clip_line_test(-dy, ly1 - ymin, &u1, &u2) ||
        !pixels_clip_line_test(dy, ymax - ly1, &u1, &u2))
        return 0;

    long double nx1 = lx1 + u1 * dx;
    long double ny1 = ly1 + u1 * dy;
    long double nx2 = lx1 + u2 * dx;
    long double ny2 = ly1 + u2 * dy;
    *x1 = pixels_round_ld_to_i64_sat(nx1);
    *y1 = pixels_round_ld_to_i64_sat(ny1);
    *x2 = pixels_round_ld_to_i64_sat(nx2);
    *y2 = pixels_round_ld_to_i64_sat(ny2);
    return 1;
}

/// @brief Return the last pixel coordinate of a range starting at @p start with @p length pixels.
/// @details Returns @p start when length <= 1 (degenerate single-pixel rect).  Uses
///   saturating addition to avoid overflow when @p start is near INT64_MAX.
/// @param start First coordinate in the range.
/// @param length Requested number of coordinates.
/// @return @p start for a non-positive or unit length; otherwise the saturated
///         inclusive coordinate `start + length - 1`.
static int64_t pixels_rect_last(int64_t start, int64_t length) {
    if (length <= 1)
        return start;
    return rt_pixels_add_sat64(start, length - 1);
}

/// @brief Fill a horizontal run of pixels on row @p y from x0 to x1 (inclusive) with @p rgba.
/// @details Clips the run to the buffer's x extent silently.  Skips the row entirely if
///   @p y is out of the buffer's y range or if x1 < x0.  Does NOT call pixels_touch —
///   callers must do that before the first span of a primitive.
/// @param p Destination implementation.
/// @param y Destination row.
/// @param x0 Inclusive first column.
/// @param x1 Inclusive last column.
/// @param rgba Canonical raw `0xRRGGBBAA` value to store.
/// @return Nonzero when a clipped span was written; zero when storage or the
///         requested span does not intersect the destination.
static int8_t pixels_fill_span(
    rt_pixels_impl *p, int64_t y, int64_t x0, int64_t x1, uint32_t rgba) {
    if (!p || !p->data || y < 0 || y >= p->height || x1 < x0 || x1 < 0 || x0 >= p->width)
        return 0;
    if (x0 < 0)
        x0 = 0;
    if (x1 >= p->width)
        x1 = p->width - 1;
    for (int64_t x = x0; x <= x1; x++)
        p->data[y * p->width + x] = rgba;
    return 1;
}

/// @brief Bresenham line rasterizer onto a Pixels buffer.
/// @details Clips first via pixels_clip_line_to_bounds, then walks the
///          standard Bresenham error-accumulator loop. Each step writes one
///          pixel directly into p->data[y * width + x]. Used by Line, Polyline,
///          and the disc/ring outline primitives.
/// @param p Destination implementation.
/// @param x1 First endpoint X coordinate.
/// @param y1 First endpoint Y coordinate.
/// @param x2 Second endpoint X coordinate.
/// @param y2 Second endpoint Y coordinate.
/// @param rgba Canonical raw `0xRRGGBBAA` value to store.
/// @return 1 if any pixel was written, 0 if the line lies outside the buffer.
static int8_t pixels_draw_line_raw(
    rt_pixels_impl *p, int64_t x1, int64_t y1, int64_t x2, int64_t y2, uint32_t rgba) {
    if (!pixels_clip_line_to_bounds(p, &x1, &y1, &x2, &y2))
        return 0;

    int64_t adx = rt_pixels_abs_diff_sat64(x2, x1);
    int64_t ady = rt_pixels_abs_diff_sat64(y2, y1);
    int64_t sx = x2 >= x1 ? 1 : -1;
    int64_t sy = y2 >= y1 ? 1 : -1;

    int64_t err = adx - ady;
    int64_t x = x1;
    int64_t y = y1;
    int8_t wrote = 0;

    for (;;) {
        wrote |= set_pixel_raw(p, x, y, rgba);
        if (x == x2 && y == y2)
            break;
        int64_t e2 = err > INT64_MAX / 2 ? INT64_MAX : err < INT64_MIN / 2 ? INT64_MIN : err * 2;
        if (e2 > -ady) {
            err -= ady;
            x += sx;
        }
        if (e2 < adx) {
            err += adx;
            y += sy;
        }
    }
    return wrote;
}

/// @brief Squared distance between two points in long double (no sqrt, no
///        overflow) — used only to compare relative edge lengths.
/// @param x1 First point X coordinate.
/// @param y1 First point Y coordinate.
/// @param x2 Second point X coordinate.
/// @param y2 Second point Y coordinate.
/// @return Squared Euclidean distance in extended precision.
static long double pixels_dist2_ld(int64_t x1, int64_t y1, int64_t x2, int64_t y2) {
    long double dx = (long double)x2 - (long double)x1;
    long double dy = (long double)y2 - (long double)y1;
    return dx * dx + dy * dy;
}

/// @brief Degenerate-triangle fallback: when the three vertices are collinear
///        (zero area), rasterize just the longest of the three edges so the
///        triangle still renders as the line it visually is.
/// @param pixels Opaque destination Pixels handle.
/// @param x1 First vertex X coordinate.
/// @param y1 First vertex Y coordinate.
/// @param x2 Second vertex X coordinate.
/// @param y2 Second vertex Y coordinate.
/// @param x3 Third vertex X coordinate.
/// @param y3 Third vertex Y coordinate.
/// @param color Runtime drawing color forwarded to `rt_pixels_draw_line()`.
static void pixels_draw_degenerate_triangle_line(void *pixels,
                                                 int64_t x1,
                                                 int64_t y1,
                                                 int64_t x2,
                                                 int64_t y2,
                                                 int64_t x3,
                                                 int64_t y3,
                                                 int64_t color) {
    long double d12 = pixels_dist2_ld(x1, y1, x2, y2);
    long double d23 = pixels_dist2_ld(x2, y2, x3, y3);
    long double d31 = pixels_dist2_ld(x3, y3, x1, y1);
    if (d12 >= d23 && d12 >= d31) {
        rt_pixels_draw_line(pixels, x1, y1, x2, y2, color);
    } else if (d23 >= d31) {
        rt_pixels_draw_line(pixels, x2, y2, x3, y3, color);
    } else {
        rt_pixels_draw_line(pixels, x3, y3, x1, y1, color);
    }
}

/// @brief Draw a clipped one-pixel-wide Bresenham line.
/// @details Both endpoints are inclusive. The color accepts Canvas
///          `0x00RRGGBB`, raw `0xRRGGBBAA`, or tagged `Color.RGBA` form. A
///          line wholly outside the destination is a no-op.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x1 First endpoint X coordinate.
/// @param y1 First endpoint Y coordinate.
/// @param x2 Second endpoint X coordinate.
/// @param y2 Second endpoint Y coordinate.
/// @param color Runtime drawing color.
void rt_pixels_draw_line(
    void *pixels, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawLine: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawLine: invalid pixels");
    if (!p)
        return;
    uint32_t rgba = rt_pixels_color_to_rgba(color);

    if (pixels_draw_line_raw(p, x1, y1, x2, y2, rgba))
        pixels_touch(p);
}

/// @brief Fill a clipped axis-aligned rectangle.
/// @details The rectangle covers half-open coordinates `[x, x + w)` and
///          `[y, y + h)`. Non-positive or wholly out-of-bounds rectangles are
///          no-ops. The color accepts every form supported by
///          `rt_pixels_color_to_rgba()`.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x Rectangle left coordinate.
/// @param y Rectangle top coordinate.
/// @param w Rectangle width in pixels.
/// @param h Rectangle height in pixels.
/// @param color Runtime drawing color.
void rt_pixels_draw_box(void *pixels, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawBox: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawBox: invalid pixels");
    if (!p)
        return;
    uint32_t rgba = rt_pixels_color_to_rgba(color);

    if (!rt_pixels_clip_rect_to_bounds(p, &x, &y, &w, &h))
        return;

    pixels_touch(p);

    for (int64_t row = y; row < y + h; row++)
        for (int64_t col = x; col < x + w; col++)
            p->data[row * p->width + col] = rgba;
}

/// @brief Draw a clipped one-pixel-wide rectangle outline.
/// @details Draws the top, bottom, left, and right inclusive edges by invoking
///          `rt_pixels_draw_line()`. Non-positive dimensions are no-ops;
///          unit dimensions may redraw the same row or column.
/// @param pixels Opaque destination Pixels handle; a null handle traps.
/// @param x Rectangle left coordinate.
/// @param y Rectangle top coordinate.
/// @param w Rectangle width in pixels.
/// @param h Rectangle height in pixels.
/// @param color Runtime drawing color.
void rt_pixels_draw_frame(void *pixels, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawFrame: null pixels");
        return;
    }
    if (w <= 0 || h <= 0)
        return;

    int64_t x1 = pixels_rect_last(x, w);
    int64_t y1 = pixels_rect_last(y, h);
    rt_pixels_draw_line(pixels, x, y, x1, y, color);
    rt_pixels_draw_line(pixels, x, y1, x1, y1, color);
    rt_pixels_draw_line(pixels, x, y, x, y1, color);
    rt_pixels_draw_line(pixels, x1, y, x1, y1, color);
}

/// @brief Fill a clipped disc centered at an integer coordinate.
/// @details For each visible row, solves the circle equation in `long double`
///          and fills the resulting inclusive horizontal span. A zero radius
///          addresses the center pixel; a negative radius is a no-op.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param r Radius in pixels.
/// @param color Runtime drawing color.
void rt_pixels_draw_disc(void *pixels, int64_t cx, int64_t cy, int64_t r, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawDisc: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawDisc: invalid pixels");
    if (!p)
        return;
    uint32_t rgba = rt_pixels_color_to_rgba(color);

    if (r < 0)
        return;

    int64_t y0 = rt_pixels_sub_nonneg_sat64(cy, r);
    int64_t y1 = rt_pixels_add_sat64(cy, r);
    if (y1 < 0 || y0 >= p->height)
        return;
    if (y0 < 0)
        y0 = 0;
    if (y1 >= p->height)
        y1 = p->height - 1;

    long double r2 = (long double)r * (long double)r;
    int8_t wrote = 0;
    for (int64_t py = y0; py <= y1; py++) {
        long double dy = (long double)py - (long double)cy;
        long double rem = r2 - dy * dy;
        if (rem < 0.0L)
            continue;
        long double dx = sqrtl(rem);
        int64_t x0 = pixels_floor_ld_to_i64_sat((long double)cx - dx);
        int64_t x1 = pixels_ceil_ld_to_i64_sat((long double)cx + dx);
        wrote |= pixels_fill_span(p, py, x0, x1, rgba);
    }
    if (wrote)
        pixels_touch(p);
}

/// @brief Draw a clipped one-pixel-wide circle outline.
/// @details Solves the circle equation along both visible axes in `long
///          double`, rounds the complementary coordinate, and emits symmetric
///          boundary pixels. A zero radius addresses the center pixel; a
///          negative radius is a no-op.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param r Radius in pixels.
/// @param color Runtime drawing color.
void rt_pixels_draw_ring(void *pixels, int64_t cx, int64_t cy, int64_t r, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawRing: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawRing: invalid pixels");
    if (!p)
        return;
    uint32_t rgba = rt_pixels_color_to_rgba(color);

    if (r < 0)
        return;
    if (r == 0) {
        if (set_pixel_raw(p, cx, cy, rgba))
            pixels_touch(p);
        return;
    }

    int64_t y0 = rt_pixels_sub_nonneg_sat64(cy, r);
    int64_t y1 = rt_pixels_add_sat64(cy, r);
    if (y0 < 0)
        y0 = 0;
    if (y1 >= p->height)
        y1 = p->height - 1;
    long double r2 = (long double)r * (long double)r;
    int8_t wrote = 0;
    for (int64_t py = y0; py <= y1; py++) {
        long double dy = (long double)py - (long double)cy;
        long double rem = r2 - dy * dy;
        if (rem < 0.0L)
            continue;
        int64_t dx = pixels_round_ld_to_i64_sat(sqrtl(rem));
        wrote |= set_pixel_raw(p, rt_pixels_sub_nonneg_sat64(cx, dx), py, rgba);
        wrote |= set_pixel_raw(p, rt_pixels_add_sat64(cx, dx), py, rgba);
    }
    int64_t x0 = rt_pixels_sub_nonneg_sat64(cx, r);
    int64_t x1 = rt_pixels_add_sat64(cx, r);
    if (x0 < 0)
        x0 = 0;
    if (x1 >= p->width)
        x1 = p->width - 1;
    for (int64_t px = x0; px <= x1; px++) {
        long double dx = (long double)px - (long double)cx;
        long double rem = r2 - dx * dx;
        if (rem < 0.0L)
            continue;
        int64_t dy = pixels_round_ld_to_i64_sat(sqrtl(rem));
        wrote |= set_pixel_raw(p, px, rt_pixels_sub_nonneg_sat64(cy, dy), rgba);
        wrote |= set_pixel_raw(p, px, rt_pixels_add_sat64(cy, dy), rgba);
    }
    if (wrote)
        pixels_touch(p);
}

/// @brief Fill a clipped axis-aligned ellipse.
/// @details Evaluates the ellipse equation in `long double` for every visible
///          row and fills the inclusive horizontal span bounded by the two
///          roots. Either non-positive radius makes the request a no-op.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param rx Horizontal radius in pixels.
/// @param ry Vertical radius in pixels.
/// @param color Runtime drawing color.
void rt_pixels_draw_ellipse(
    void *pixels, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawEllipse: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawEllipse: invalid pixels");
    if (!p)
        return;
    uint32_t rgba = rt_pixels_color_to_rgba(color);

    if (rx <= 0 || ry <= 0) {
        return;
    }

    int64_t y0 = rt_pixels_sub_nonneg_sat64(cy, ry);
    int64_t y1 = rt_pixels_add_sat64(cy, ry);
    if (y1 < 0 || y0 >= p->height)
        return;
    if (y0 < 0)
        y0 = 0;
    if (y1 >= p->height)
        y1 = p->height - 1;

    long double rx_ld = (long double)rx;
    long double ry_ld = (long double)ry;
    int8_t wrote = 0;
    for (int64_t py = y0; py <= y1; py++) {
        long double dy = (long double)py - (long double)cy;
        long double ratio = 1.0L - (dy * dy) / (ry_ld * ry_ld);
        if (ratio < 0.0L)
            continue;
        long double dx = rx_ld * sqrtl(ratio);
        int64_t x0 = pixels_floor_ld_to_i64_sat((long double)cx - dx);
        int64_t x1 = pixels_ceil_ld_to_i64_sat((long double)cx + dx);
        wrote |= pixels_fill_span(p, py, x0, x1, rgba);
    }
    if (wrote)
        pixels_touch(p);
}

/// @brief Draw a clipped one-pixel-wide axis-aligned ellipse outline.
/// @details Evaluates the ellipse equation along both visible axes in `long
///          double`, rounds the complementary coordinate, and writes the
///          symmetric boundary pixels. Either non-positive radius is a no-op.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param rx Horizontal radius in pixels.
/// @param ry Vertical radius in pixels.
/// @param color Runtime drawing color.
void rt_pixels_draw_ellipse_frame(
    void *pixels, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawEllipseFrame: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawEllipseFrame: invalid pixels");
    if (!p)
        return;
    uint32_t rgba = rt_pixels_color_to_rgba(color);

    if (rx <= 0 || ry <= 0) {
        return;
    }

    long double rx_ld = (long double)rx;
    long double ry_ld = (long double)ry;
    int64_t y0 = rt_pixels_sub_nonneg_sat64(cy, ry);
    int64_t y1 = rt_pixels_add_sat64(cy, ry);
    if (y0 < 0)
        y0 = 0;
    if (y1 >= p->height)
        y1 = p->height - 1;
    int8_t wrote = 0;
    for (int64_t py = y0; py <= y1; py++) {
        long double dy = (long double)py - (long double)cy;
        long double ratio = 1.0L - (dy * dy) / (ry_ld * ry_ld);
        if (ratio < 0.0L)
            continue;
        int64_t dx = pixels_round_ld_to_i64_sat(rx_ld * sqrtl(ratio));
        wrote |= set_pixel_raw(p, rt_pixels_sub_nonneg_sat64(cx, dx), py, rgba);
        wrote |= set_pixel_raw(p, rt_pixels_add_sat64(cx, dx), py, rgba);
    }

    int64_t x0 = rt_pixels_sub_nonneg_sat64(cx, rx);
    int64_t x1 = rt_pixels_add_sat64(cx, rx);
    if (x0 < 0)
        x0 = 0;
    if (x1 >= p->width)
        x1 = p->width - 1;
    for (int64_t px = x0; px <= x1; px++) {
        long double dx = (long double)px - (long double)cx;
        long double ratio = 1.0L - (dx * dx) / (rx_ld * rx_ld);
        if (ratio < 0.0L)
            continue;
        int64_t dy = pixels_round_ld_to_i64_sat(ry_ld * sqrtl(ratio));
        wrote |= set_pixel_raw(p, px, rt_pixels_sub_nonneg_sat64(cy, dy), rgba);
        wrote |= set_pixel_raw(p, px, rt_pixels_add_sat64(cy, dy), rgba);
    }
    if (wrote)
        pixels_touch(p);
}

/// @brief One pending horizontal flood-fill seed.
/// @details The scanline flood-fill implementation expands this seed to a full
///          `[left, right]` run when popped, then pushes only runs discovered
///          immediately above and below. This stores work per span instead of
///          per pixel and avoids recursion.
typedef struct rt_pixels_fill_segment {
    int64_t x;
    int64_t y;
} rt_pixels_fill_segment;

/// @brief Push a flood-fill segment onto a dynamically grown stack.
/// @details Capacity doubles geometrically from a small initial allocation.
///          The helper is overflow-checked and leaves the existing stack valid
///          on allocation failure.
/// @param stack_io In/out pointer to the segment stack.
/// @param count_io In/out active segment count.
/// @param cap_io In/out allocated segment capacity.
/// @param x Seed X coordinate.
/// @param y Seed Y coordinate.
/// @return 1 on success, 0 on overflow or allocation failure.
static int pixels_fill_segment_push(
    rt_pixels_fill_segment **stack_io, size_t *count_io, size_t *cap_io, int64_t x, int64_t y) {
    if (!stack_io || !count_io || !cap_io)
        return 0;
    if (*count_io >= *cap_io) {
        size_t new_cap = *cap_io ? *cap_io * 2u : 1024u;
        if (new_cap < *cap_io || new_cap > SIZE_MAX / sizeof(**stack_io))
            return 0;
        rt_pixels_fill_segment *grown =
            (rt_pixels_fill_segment *)realloc(*stack_io, new_cap * sizeof(**stack_io));
        if (!grown)
            return 0;
        *stack_io = grown;
        *cap_io = new_cap;
    }
    (*stack_io)[(*count_io)++] = (rt_pixels_fill_segment){x, y};
    return 1;
}

/// @brief Replace the connected region of pixels matching the seed color with @p color.
/// @details Uses an iterative scanline algorithm: each popped seed expands left
///          and right across a contiguous target-color run, writes that run, then
///          scans the adjacent rows for new target-color runs. This keeps memory
///          proportional to the boundary/run complexity of the region instead of
///          allocating `width * height` queue and visited arrays. If stack
///          growth fails after writes begin, the already visited part remains
///          filled, the stack is released, and the mutation generation still
///          advances. An out-of-bounds seed or a color already equal to the
///          seed pixel is a no-op.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x Seed X coordinate.
/// @param y Seed Y coordinate.
/// @param color Replacement color in any supported runtime drawing form.
void rt_pixels_flood_fill(void *pixels, int64_t x, int64_t y, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.FloodFill: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.FloodFill: invalid pixels");
    if (!p)
        return;

    if (x < 0 || x >= p->width || y < 0 || y >= p->height)
        return;

    uint32_t target = p->data[y * p->width + x];
    uint32_t fill_c = rt_pixels_color_to_rgba(color);

    if (target == fill_c)
        return;

    rt_pixels_fill_segment *stack = NULL;
    size_t stack_count = 0;
    size_t stack_cap = 0;
    int8_t wrote = 0;
    int failed = 0;
    if (!pixels_fill_segment_push(&stack, &stack_count, &stack_cap, x, y))
        return;

    while (stack_count > 0 && !failed) {
        rt_pixels_fill_segment seed = stack[--stack_count];
        if (seed.y < 0 || seed.y >= p->height || seed.x < 0 || seed.x >= p->width)
            continue;
        if (p->data[seed.y * p->width + seed.x] != target)
            continue;

        int64_t left = seed.x;
        int64_t right = seed.x;
        while (left > 0 && p->data[seed.y * p->width + (left - 1)] == target)
            left--;
        while (right + 1 < p->width && p->data[seed.y * p->width + (right + 1)] == target)
            right++;

        uint32_t *row = p->data + seed.y * p->width;
        for (int64_t px = left; px <= right; px++)
            row[px] = fill_c;
        wrote = 1;

        for (int dy = -1; dy <= 1; dy += 2) {
            int64_t scan_y = seed.y + dy;
            if (scan_y < 0 || scan_y >= p->height)
                continue;
            uint32_t *scan_row = p->data + scan_y * p->width;
            int64_t px = left;
            while (px <= right) {
                while (px <= right && scan_row[px] != target)
                    px++;
                if (px > right)
                    break;
                int64_t run_start = px;
                while (px <= right && scan_row[px] == target)
                    px++;
                if (!pixels_fill_segment_push(
                        &stack, &stack_count, &stack_cap, run_start, scan_y)) {
                    failed = 1;
                    break;
                }
            }
        }
    }

    free(stack);
    if (wrote)
        pixels_touch(p);
}

/// @brief Draw a rounded line by stamping filled discs along a Bresenham path.
/// @details A thickness of one or less delegates to
///          `rt_pixels_draw_line()`. Larger values use `thickness / 2` as the
///          disc radius, clipped to a buffer-derived maximum so extreme input
///          cannot create unbounded work. The expanded line is clipped before
///          stamping, and individual disc calls perform final pixel clipping.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x1 First centerline endpoint X coordinate.
/// @param y1 First centerline endpoint Y coordinate.
/// @param x2 Second centerline endpoint X coordinate.
/// @param y2 Second centerline endpoint Y coordinate.
/// @param thickness Requested width in pixels.
/// @param color Runtime drawing color.
void rt_pixels_draw_thick_line(void *pixels,
                               int64_t x1,
                               int64_t y1,
                               int64_t x2,
                               int64_t y2,
                               int64_t thickness,
                               int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawThickLine: null pixels");
        return;
    }
    if (thickness <= 1) {
        rt_pixels_draw_line(pixels, x1, y1, x2, y2, color);
        return;
    }

    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawThickLine: invalid pixels");
    if (!p)
        return;
    if (!p->data || p->width <= 0 || p->height <= 0)
        return;

    int64_t radius = thickness / 2;
    int64_t max_extent = rt_pixels_add_sat64(p->width, p->height);
    int64_t clip_radius = radius > max_extent ? max_extent : radius;
    if (!pixels_clip_line_to_rect(rt_pixels_sub_nonneg_sat64(0, clip_radius),
                                  rt_pixels_sub_nonneg_sat64(0, clip_radius),
                                  rt_pixels_add_sat64(p->width - 1, clip_radius),
                                  rt_pixels_add_sat64(p->height - 1, clip_radius),
                                  &x1,
                                  &y1,
                                  &x2,
                                  &y2))
        return;

    int64_t adx = rt_pixels_abs_diff_sat64(x2, x1);
    int64_t ady = rt_pixels_abs_diff_sat64(y2, y1);
    int64_t sx = x2 >= x1 ? 1 : -1;
    int64_t sy = y2 >= y1 ? 1 : -1;

    int64_t err = adx - ady;
    int64_t x = x1;
    int64_t y = y1;

    /* Stamp with the image-clamped radius, not the raw thickness/2: an enormous
     * thickness (up to INT64_MAX) would otherwise make each Bresenham-step disc
     * attempt to fill a radius-sized area, turning one call into an unbounded
     * hang. A disc larger than the image already fills it, so clamping to
     * clip_radius is visually identical while bounding the work. */
    for (;;) {
        rt_pixels_draw_disc(pixels, x, y, clip_radius, color);
        if (x == x2 && y == y2)
            break;
        int64_t e2 = err > INT64_MAX / 2 ? INT64_MAX : err < INT64_MIN / 2 ? INT64_MIN : err * 2;
        if (e2 > -ady) {
            err -= ady;
            x += sx;
        }
        if (e2 < adx) {
            err += adx;
            y += sy;
        }
    }
}

/// @brief Fill a clipped solid triangle with scanline rasterization.
/// @details Vertices are sorted by ascending Y, extended-precision edge
///          intersections produce each inclusive span, and only visible rows
///          are visited. A zero-area triangle draws its longest edge so
///          collinear input remains visible.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x1 First vertex X coordinate.
/// @param y1 First vertex Y coordinate.
/// @param x2 Second vertex X coordinate.
/// @param y2 Second vertex Y coordinate.
/// @param x3 Third vertex X coordinate.
/// @param y3 Third vertex Y coordinate.
/// @param color Runtime drawing color.
void rt_pixels_draw_triangle(void *pixels,
                             int64_t x1,
                             int64_t y1,
                             int64_t x2,
                             int64_t y2,
                             int64_t x3,
                             int64_t y3,
                             int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawTriangle: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawTriangle: invalid pixels");
    if (!p)
        return;
    uint32_t rgba = rt_pixels_color_to_rgba(color);

    long double area = ((long double)x2 - (long double)x1) * ((long double)y3 - (long double)y1) -
                       ((long double)y2 - (long double)y1) * ((long double)x3 - (long double)x1);
    if (area == 0.0L) {
        pixels_draw_degenerate_triangle_line(pixels, x1, y1, x2, y2, x3, y3, color);
        return;
    }

    // Sort vertices by y ascending (bubble sort 3 elements)
    if (y1 > y2) {
        int64_t tx = x1;
        x1 = x2;
        x2 = tx;
        int64_t ty = y1;
        y1 = y2;
        y2 = ty;
    }
    if (y1 > y3) {
        int64_t tx = x1;
        x1 = x3;
        x3 = tx;
        int64_t ty = y1;
        y1 = y3;
        y3 = ty;
    }
    if (y2 > y3) {
        int64_t tx = x2;
        x2 = x3;
        x3 = tx;
        int64_t ty = y2;
        y2 = y3;
        y3 = ty;
    }

    int64_t total_h = rt_pixels_abs_diff_sat64(y3, y1);
    if (total_h == 0)
        return;
    if (y3 < 0 || y1 >= p->height)
        return;

    int8_t wrote = 0;
    // Upper half: y1 .. y2
    int64_t upper_h = rt_pixels_abs_diff_sat64(y2, y1);
    int64_t upper_start = y1 < 0 ? 0 : y1;
    int64_t upper_end = y2 >= p->height ? p->height - 1 : y2;
    for (int64_t scan_y = upper_start; scan_y <= upper_end; scan_y++) {
        long double row = (long double)scan_y - (long double)y1;
        int64_t ax = pixels_trunc_ld_to_i64_sat(
            (long double)x1 + ((long double)x3 - (long double)x1) * row / (long double)total_h);
        int64_t bx = pixels_trunc_ld_to_i64_sat((long double)x1 +
                                                ((long double)x2 - (long double)x1) * row /
                                                    (long double)(upper_h > 0 ? upper_h : 1));
        if (ax > bx) {
            int64_t tmp = ax;
            ax = bx;
            bx = tmp;
        }
        wrote |= pixels_fill_span(p, scan_y, ax, bx, rgba);
    }

    // Lower half: y2 .. y3
    int64_t lower_h = rt_pixels_abs_diff_sat64(y3, y2);
    int64_t lower_start = y2 < 0 ? 0 : y2;
    int64_t lower_end = y3 >= p->height ? p->height - 1 : y3;
    for (int64_t scan_y = lower_start; scan_y <= lower_end; scan_y++) {
        long double row = (long double)scan_y - (long double)y2;
        long double total_row = (long double)upper_h + row;
        int64_t ax =
            pixels_trunc_ld_to_i64_sat((long double)x1 + ((long double)x3 - (long double)x1) *
                                                             total_row / (long double)total_h);
        int64_t bx = pixels_trunc_ld_to_i64_sat((long double)x2 +
                                                ((long double)x3 - (long double)x2) * row /
                                                    (long double)(lower_h > 0 ? lower_h : 1));
        if (ax > bx) {
            int64_t tmp = ax;
            ax = bx;
            bx = tmp;
        }
        wrote |= pixels_fill_span(p, scan_y, ax, bx, rgba);
    }
    if (wrote)
        pixels_touch(p);
}

/// @brief Draw a clipped quadratic Bezier curve through one control point.
/// @details Selects an adaptive sample count from the endpoint and control
///          extents, clamps it to `[2, 10000]`, evaluates de Casteljau
///          interpolation in `long double`, and joins rounded adjacent samples
///          with raw Bresenham segments.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x1 Start-point X coordinate.
/// @param y1 Start-point Y coordinate.
/// @param cx_ctrl Control-point X coordinate.
/// @param cy_ctrl Control-point Y coordinate.
/// @param x2 End-point X coordinate.
/// @param y2 End-point Y coordinate.
/// @param color Runtime drawing color.
void rt_pixels_draw_bezier(void *pixels,
                           int64_t x1,
                           int64_t y1,
                           int64_t cx_ctrl,
                           int64_t cy_ctrl,
                           int64_t x2,
                           int64_t y2,
                           int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawBezier: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawBezier: invalid pixels");
    if (!p)
        return;
    uint32_t rgba = rt_pixels_color_to_rgba(color);

    // Adaptive step count: enough steps to avoid gaps
    int64_t adx = rt_pixels_abs_diff_sat64(x2, x1);
    int64_t ady = rt_pixels_abs_diff_sat64(y2, y1);
    int64_t acx = rt_pixels_abs_diff_sat64(cx_ctrl, x1);
    int64_t acy = rt_pixels_abs_diff_sat64(cy_ctrl, y1);
    int64_t steps = adx > ady ? adx : ady;
    if (acx > steps)
        steps = acx;
    if (acy > steps)
        steps = acy;
    steps = steps > (INT64_MAX - 1) / 2 ? INT64_MAX : steps * 2 + 1;
    if (steps < 2)
        steps = 2;
    if (steps > 10000)
        steps = 10000; // Cap to prevent excessive loops

    int8_t wrote = 0;
    int64_t prev_x = 0;
    int64_t prev_y = 0;
    int8_t have_prev = 0;
    // Integer de Casteljau: P(t) via linear interpolation at t = i/steps
    for (int64_t i = 0; i <= steps; i++) {
        long double t = (long double)i / (long double)steps;
        long double lx0 = (long double)x1 + ((long double)cx_ctrl - (long double)x1) * t;
        long double ly0 = (long double)y1 + ((long double)cy_ctrl - (long double)y1) * t;
        long double lx1 = (long double)cx_ctrl + ((long double)x2 - (long double)cx_ctrl) * t;
        long double ly1 = (long double)cy_ctrl + ((long double)y2 - (long double)cy_ctrl) * t;
        int64_t bx = pixels_round_ld_to_i64_sat(lx0 + (lx1 - lx0) * t);
        int64_t by = pixels_round_ld_to_i64_sat(ly0 + (ly1 - ly0) * t);
        if (have_prev)
            wrote |= pixels_draw_line_raw(p, prev_x, prev_y, bx, by, rgba);
        else
            wrote |= set_pixel_raw(p, bx, by, rgba);
        prev_x = bx;
        prev_y = by;
        have_prev = 1;
    }
    if (wrote)
        pixels_touch(p);
}

//=============================================================================
// Text Rendering
//=============================================================================

/// @brief Multiply non-negative dimensions with signed 64-bit saturation.
/// @param a First factor.
/// @param b Second factor.
/// @return Zero if either factor is non-positive; otherwise the product
///         clamped to `INT64_MAX`.
static int64_t pixels_mul_nonneg_sat64(int64_t a, int64_t b) {
    if (a <= 0 || b <= 0)
        return 0;
    if (a > INT64_MAX / b)
        return INT64_MAX;
    return a * b;
}

/// @brief Subtract signed 64-bit alignment operands with saturation.
/// @param a Minuend.
/// @param b Subtrahend.
/// @return `a - b` clamped to the signed 64-bit range.
static int64_t pixels_sub_sat64(int64_t a, int64_t b) {
    long double value = (long double)a - (long double)b;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)value;
}

/// @brief Decode one UTF-8 code point with deterministic malformed-input recovery.
/// @details Valid one- through four-byte sequences advance as a unit.
///          Overlong encodings, surrogates, out-of-range values, truncated
///          sequences, and stray continuation bytes produce `'?'` while
///          advancing at least one byte. Glyph support is applied separately.
/// @param str UTF-8 byte buffer.
/// @param byte_len Number of readable bytes in @p str.
/// @param index In/out byte offset; advanced past the consumed sequence.
/// @param codepoint_out Receives the decoded scalar value or `'?'`.
/// @return Nonzero when one input unit was consumed; zero for invalid pointers
///         or an offset at or beyond @p byte_len.
static int pixels_next_codepoint(const char *str,
                                 size_t byte_len,
                                 size_t *index,
                                 int *codepoint_out) {
    if (!str || !index || !codepoint_out || *index >= byte_len)
        return 0;

    size_t i = *index;
    unsigned char c0 = (unsigned char)str[i];
    uint32_t cp = '?';
    size_t advance = 1;

    if (c0 < 0x80) {
        cp = c0;
    } else if ((c0 & 0xE0u) == 0xC0u && i + 1 < byte_len) {
        unsigned char c1 = (unsigned char)str[i + 1];
        if ((c1 & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c0 & 0x1Fu) << 6) | (uint32_t)(c1 & 0x3Fu);
            advance = 2;
            if (cp < 0x80u)
                cp = '?';
        }
    } else if ((c0 & 0xF0u) == 0xE0u && i + 2 < byte_len) {
        unsigned char c1 = (unsigned char)str[i + 1];
        unsigned char c2 = (unsigned char)str[i + 2];
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c0 & 0x0Fu) << 12) | ((uint32_t)(c1 & 0x3Fu) << 6) |
                 (uint32_t)(c2 & 0x3Fu);
            advance = 3;
            if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
                cp = '?';
        }
    } else if ((c0 & 0xF8u) == 0xF0u && i + 3 < byte_len) {
        unsigned char c1 = (unsigned char)str[i + 1];
        unsigned char c2 = (unsigned char)str[i + 2];
        unsigned char c3 = (unsigned char)str[i + 3];
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u && (c3 & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c0 & 0x07u) << 18) | ((uint32_t)(c1 & 0x3Fu) << 12) |
                 ((uint32_t)(c2 & 0x3Fu) << 6) | (uint32_t)(c3 & 0x3Fu);
            advance = 4;
            if (cp < 0x10000u || cp > 0x10FFFFu)
                cp = '?';
        }
    }

    *index = i + advance;
    *codepoint_out = (int)cp;
    return 1;
}

/// @brief Fill a clipped rectangle directly with canonical raw RGBA.
/// @details This internal helper does not advance the Pixels mutation
///          generation; the owning primitive performs one touch after all
///          glyph runs have been emitted.
/// @param p Destination implementation.
/// @param x Rectangle left coordinate.
/// @param y Rectangle top coordinate.
/// @param w Rectangle width in pixels.
/// @param h Rectangle height in pixels.
/// @param rgba Canonical raw `0xRRGGBBAA` value to store.
/// @return Nonzero when at least one clipped pixel was written; zero when the
///         rectangle does not intersect valid storage.
static int8_t pixels_fill_rect_raw(
    rt_pixels_impl *p, int64_t x, int64_t y, int64_t w, int64_t h, uint32_t rgba) {
    if (!rt_pixels_clip_rect_to_bounds(p, &x, &y, &w, &h))
        return 0;

    for (int64_t row = y; row < y + h; row++)
        for (int64_t col = x; col < x + w; col++)
            p->data[row * p->width + col] = rgba;
    return 1;
}

/// @brief Measure built-in monospace text by decoded UTF-8 code-point count.
/// @details Every decoded scalar, including unsupported scalars that will use
///          the fallback glyph, occupies one eight-pixel cell multiplied by
///          @p scale. Arithmetic saturates at `INT64_MAX`.
/// @param text Retained runtime string to measure; null measures as zero.
/// @param scale Positive integer pixel scale.
/// @return Saturated width in pixels, or zero when @p text is null, has no C
///         string storage, is empty, or @p scale is less than one.
static int64_t pixels_text_codepoint_width(rt_string text, int64_t scale) {
    if (!text || scale < 1)
        return 0;

    const char *str = rt_string_cstr(text);
    if (!str)
        return 0;

    size_t byte_len = (size_t)rt_str_len(text);
    size_t index = 0;
    int64_t count = 0;
    int codepoint = 0;
    while (pixels_next_codepoint(str, byte_len, &index, &codepoint))
        count++;

    return pixels_mul_nonneg_sat64(pixels_mul_nonneg_sat64(count, 8), scale);
}

/// @brief Rasterize built-in 8x8 text directly into a Pixels buffer.
/// @details Decodes UTF-8 with `pixels_next_codepoint()`, maps code points
///          outside printable ASCII `[32, 126]` to `'?'`, coalesces adjacent
///          equal-state cells into clipped rectangles, and advances by
///          `8 * scale` pixels per glyph. When @p bg is non-null, unlit cells
///          are filled too, producing a complete scaled 8x8 background cell.
///          This helper does not touch the mutation generation.
/// @param p Destination implementation.
/// @param x Left coordinate of the first glyph cell.
/// @param y Top coordinate shared by all glyph cells.
/// @param text Runtime UTF-8 string to render.
/// @param scale Positive integer scale applied to both glyph axes.
/// @param fg Canonical raw `0xRRGGBBAA` foreground color.
/// @param bg Optional pointer to a canonical raw background color; null leaves
///        unlit cells unchanged.
/// @return Nonzero when at least one clipped foreground or background pixel
///         was written; zero for invalid input or wholly clipped output.
static int8_t pixels_draw_text_raw(rt_pixels_impl *p,
                                   int64_t x,
                                   int64_t y,
                                   rt_string text,
                                   int64_t scale,
                                   uint32_t fg,
                                   const uint32_t *bg) {
    if (!p || !p->data || !text || scale < 1)
        return 0;

    const char *str = rt_string_cstr(text);
    if (!str)
        return 0;

    size_t byte_len = (size_t)rt_str_len(text);
    size_t index = 0;
    int codepoint = 0;
    int64_t cx = x;
    int64_t advance = pixels_mul_nonneg_sat64(8, scale);
    int8_t wrote = 0;

    while (pixels_next_codepoint(str, byte_len, &index, &codepoint)) {
        int glyph_cp = (codepoint >= 32 && codepoint <= 126) ? codepoint : '?';
        const uint8_t *glyph = rt_font_get_glyph(glyph_cp);

        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            int64_t py = rt_pixels_add_sat64(y, pixels_mul_nonneg_sat64((int64_t)row, scale));
            /* Coalesce horizontal runs of same-state cells into one fill instead of one fill/set
             * per cell, so a glyph row costs at most a few clipped fills rather than eight (each
             * fill otherwise re-runs full rect clipping). A run of unlit cells is skipped when no
             * background colour is supplied. */
            int col = 0;
            while (col < 8) {
                int8_t foreground = (bits & (uint8_t)(0x80u >> col)) != 0;
                if (!foreground && !bg) {
                    col++;
                    continue;
                }
                int run_start = col;
                while (col < 8 && (((bits & (uint8_t)(0x80u >> col)) != 0) == foreground))
                    col++;
                int64_t run_cols = (int64_t)(col - run_start);
                int64_t px =
                    rt_pixels_add_sat64(cx, pixels_mul_nonneg_sat64((int64_t)run_start, scale));
                uint32_t color = foreground ? fg : *bg;
                wrote |= pixels_fill_rect_raw(
                    p, px, py, pixels_mul_nonneg_sat64(run_cols, scale), scale, color);
            }
        }
        cx = rt_pixels_add_sat64(cx, advance);
    }

    return wrote;
}

/// @brief Draw built-in 8x8 bitmap-font text at unit scale.
/// @details Printable ASCII uses its native glyph; valid unsupported Unicode
///          and malformed UTF-8 render as `'?'`. Unlit glyph cells preserve
///          the destination, and all writes are clipped.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x Left coordinate of the first glyph cell.
/// @param y Top coordinate of the glyph row.
/// @param text Runtime UTF-8 string; null is a no-op.
/// @param color Runtime foreground color.
void rt_pixels_draw_text(void *pixels, int64_t x, int64_t y, rt_string text, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawText: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawText: invalid pixels");
    if (!p || !text)
        return;

    uint32_t rgba = rt_pixels_color_to_rgba(color);
    if (pixels_draw_text_raw(p, x, y, text, 1, rgba, NULL))
        pixels_touch(p);
}

/// @brief Draw unit-scale text with foreground and full-cell background colors.
/// @details Every decoded character occupies a clipped 8x8 cell: lit glyph
///          bits receive @p fg and all remaining bits receive @p bg.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x Left coordinate of the first glyph cell.
/// @param y Top coordinate of the glyph row.
/// @param text Runtime UTF-8 string; null is a no-op.
/// @param fg Runtime foreground color.
/// @param bg Runtime background color.
void rt_pixels_draw_text_bg(
    void *pixels, int64_t x, int64_t y, rt_string text, int64_t fg, int64_t bg) {
    if (!pixels) {
        rt_trap("Pixels.DrawTextBg: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawTextBg: invalid pixels");
    if (!p || !text)
        return;

    uint32_t fg_rgba = rt_pixels_color_to_rgba(fg);
    uint32_t bg_rgba = rt_pixels_color_to_rgba(bg);
    if (pixels_draw_text_raw(p, x, y, text, 1, fg_rgba, &bg_rgba))
        pixels_touch(p);
}

/// @brief Measure built-in monospace text at unit scale.
/// @param text Runtime UTF-8 string; null measures as zero.
/// @return Eight pixels per decoded code point, saturated at `INT64_MAX`.
int64_t rt_pixels_text_width(rt_string text) {
    return pixels_text_codepoint_width(text, 1);
}

/// @brief Return the built-in bitmap font's unscaled line height.
/// @return Eight pixels.
int64_t rt_pixels_text_height(void) {
    return 8;
}

/// @brief Draw built-in bitmap-font text at an integer scale.
/// @details Each source glyph bit becomes a `scale`-by-`scale` clipped block.
///          Unsupported or malformed input uses `'?'`; a scale below one is a
///          no-op.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x Left coordinate of the first glyph cell.
/// @param y Top coordinate of the glyph row.
/// @param text Runtime UTF-8 string; null is a no-op.
/// @param scale Positive integer scale applied to both axes.
/// @param color Runtime foreground color.
void rt_pixels_draw_text_scaled(
    void *pixels, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawTextScaled: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawTextScaled: invalid pixels");
    if (!p || !text || scale < 1)
        return;

    uint32_t rgba = rt_pixels_color_to_rgba(color);
    if (pixels_draw_text_raw(p, x, y, text, scale, rgba, NULL))
        pixels_touch(p);
}

/// @brief Draw integer-scaled text with foreground and full-cell background.
/// @details Every glyph occupies an `8 * scale` square cell. Lit blocks
///          receive @p fg and all remaining blocks receive @p bg. A scale
///          below one is a no-op.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x Left coordinate of the first glyph cell.
/// @param y Top coordinate of the glyph row.
/// @param text Runtime UTF-8 string; null is a no-op.
/// @param scale Positive integer scale applied to both axes.
/// @param fg Runtime foreground color.
/// @param bg Runtime background color.
void rt_pixels_draw_text_scaled_bg(
    void *pixels, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t fg, int64_t bg) {
    if (!pixels) {
        rt_trap("Pixels.DrawTextScaledBg: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawTextScaledBg: invalid pixels");
    if (!p || !text || scale < 1)
        return;

    uint32_t fg_rgba = rt_pixels_color_to_rgba(fg);
    uint32_t bg_rgba = rt_pixels_color_to_rgba(bg);
    if (pixels_draw_text_raw(p, x, y, text, scale, fg_rgba, &bg_rgba))
        pixels_touch(p);
}

/// @brief Measure built-in monospace text at an integer scale.
/// @param text Runtime UTF-8 string; null measures as zero.
/// @param scale Positive integer glyph scale.
/// @return `decoded_code_points * 8 * scale`, saturated at `INT64_MAX`, or
///         zero when @p scale is less than one.
int64_t rt_pixels_text_scaled_width(rt_string text, int64_t scale) {
    return pixels_text_codepoint_width(text, scale);
}

/// @brief Draw unit-scale text horizontally centered in the destination.
/// @details Computes the first-cell X coordinate from the measured decoded
///          text width. Text wider than the destination starts at a negative
///          X coordinate and is clipped symmetrically by the rasterizer.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param y Top coordinate of the glyph row.
/// @param text Runtime UTF-8 string; null is a no-op.
/// @param color Runtime foreground color.
void rt_pixels_draw_text_centered(void *pixels, int64_t y, rt_string text, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawTextCentered: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawTextCentered: invalid pixels");
    if (!p || !text)
        return;

    int64_t x = (p->width - rt_pixels_text_width(text)) / 2;
    uint32_t rgba = rt_pixels_color_to_rgba(color);
    if (pixels_draw_text_raw(p, x, y, text, 1, rgba, NULL))
        pixels_touch(p);
}

/// @brief Draw unit-scale text right-aligned inside the destination.
/// @details Places the text so its measured right edge is @p margin pixels
///          from the destination width. Saturating subtraction keeps extreme
///          widths and margins defined; final writes are clipped.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param margin Distance from the destination's right edge; negative values
///        intentionally shift text beyond that edge.
/// @param y Top coordinate of the glyph row.
/// @param text Runtime UTF-8 string; null is a no-op.
/// @param color Runtime foreground color.
void rt_pixels_draw_text_right(
    void *pixels, int64_t margin, int64_t y, rt_string text, int64_t color) {
    if (!pixels) {
        rt_trap("Pixels.DrawTextRight: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DrawTextRight: invalid pixels");
    if (!p || !text)
        return;

    int64_t x = pixels_sub_sat64(pixels_sub_sat64(p->width, rt_pixels_text_width(text)), margin);
    uint32_t rgba = rt_pixels_color_to_rgba(color);
    if (pixels_draw_text_raw(p, x, y, text, 1, rgba, NULL))
        pixels_touch(p);
}

/// @brief Draw integer-scaled text horizontally centered in the destination.
/// @details Measures with the same decoded-code-point and saturation rules as
///          `rt_pixels_text_scaled_width()`. A scale below one is a no-op.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param y Top coordinate of the glyph row.
/// @param text Runtime UTF-8 string; null is a no-op.
/// @param color Runtime foreground color.
/// @param scale Positive integer scale applied to both glyph axes.
void rt_pixels_draw_text_centered_scaled(
    void *pixels, int64_t y, rt_string text, int64_t color, int64_t scale) {
    if (!pixels) {
        rt_trap("Pixels.DrawTextCenteredScaled: null pixels");
        return;
    }
    rt_pixels_impl *p =
        rt_pixels_checked_impl(pixels, "Pixels.DrawTextCenteredScaled: invalid pixels");
    if (!p || !text || scale < 1)
        return;

    int64_t x = (p->width - rt_pixels_text_scaled_width(text, scale)) / 2;
    uint32_t rgba = rt_pixels_color_to_rgba(color);
    if (pixels_draw_text_raw(p, x, y, text, scale, rgba, NULL))
        pixels_touch(p);
}

/// @brief Composite one color over a destination pixel with explicit alpha.
/// @details Converts @p color to RGB but intentionally ignores any alpha
///          carried by that value: @p alpha alone controls source opacity.
///          Values at or below zero are no-ops and values above 255 clamp to
///          255. Fully opaque input stores `0xRRGGBBFF`; intermediate input
///          uses `rt_pixels_alpha_over_rgba()` to preserve the destination's
///          Porter-Duff contribution. Out-of-bounds coordinates are no-ops.
/// @param pixels Opaque destination Pixels handle; null or invalid handles
///        trap.
/// @param x Zero-based destination column.
/// @param y Zero-based destination row.
/// @param color Runtime color supplying source RGB channels.
/// @param alpha Explicit source alpha, clamped to `[0, 255]`.
void rt_pixels_blend_pixel(void *pixels, int64_t x, int64_t y, int64_t color, int64_t alpha) {
    if (!pixels) {
        rt_trap("Pixels.BlendPixel: null pixels");
        return;
    }
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.BlendPixel: invalid pixels");
    if (!p)
        return;
    if (x < 0 || x >= p->width || y < 0 || y >= p->height)
        return;

    // Clamp alpha to [0, 255]
    if (alpha <= 0)
        return; // fully transparent — no-op
    if (alpha > 255)
        alpha = 255;

    // Fully opaque fast path — same as set_rgb
    if (alpha == 255) {
        uint32_t rgb = (rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu;
        p->data[y * p->width + x] = (rgb << 8) | 0xFFu;
        pixels_touch(p);
        return;
    }

    // Extract source channels; the explicit alpha argument controls compositing alpha.
    uint32_t rgb = (rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu;
    uint32_t sr = (rgb >> 16) & 0xFFu;
    uint32_t sg = (rgb >> 8) & 0xFFu;
    uint32_t sb = rgb & 0xFFu;
    uint32_t sa = (uint32_t)alpha;

    uint32_t dst = p->data[y * p->width + x];
    uint32_t src = (sr << 24) | (sg << 16) | (sb << 8) | sa;

    p->data[y * p->width + x] = rt_pixels_alpha_over_rgba(dst, src);
    pixels_touch(p);
}
