//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_drawing_advanced.c
/// @file
/// @brief Implements clipped advanced Canvas shapes, curves, array/Path2D
///        polygons, flood fill, and full-channel linear gradients.
// Purpose: Advanced 2D canvas drawing: thick/round strokes, flood fill, triangles,
//   ellipses, arcs, beziers, polylines/polygons (incl. Path2D), and linear
//   gradients. Color math lives in rt_color.c.
//
// Key invariants:
//   - Every raster operation validates its canvas and computes bounded pixel
//     coordinates before accessing the backing buffer.
//   - Temporary path and flood-fill storage is released on every success,
//     failure, and returning-trap path.
// Ownership/Lifetime:
//   - Canvas storage is borrowed for the duration of each call. Temporary
//     native buffers are owned locally and never escape the drawing helper.
//   - Point arrays and Path2D handles are borrowed. Path flattening,
//     intersection lists, flood stacks, and gradient row buffers are released
//     before their calls return.
//
// Links: rt_graphics2d.h, rt_graphics_internal.h (canvas API),
//        rt_color.c (color utilities used by gradients)
//
//===----------------------------------------------------------------------===//

#include "rt_graphics2d.h"
#include "rt_graphics_internal.h"
#include "rt_heap.h"

#include <limits.h>

#ifdef ZANNA_ENABLE_GRAPHICS


/// @brief Quadrant selectors used by rounded-frame circle rasterization.
enum {
    /// Upper-left circle quadrant.
    RTG_CORNER_TOP_LEFT = 0,
    /// Upper-right circle quadrant.
    RTG_CORNER_TOP_RIGHT = 1,
    /// Lower-left circle quadrant.
    RTG_CORNER_BOTTOM_LEFT = 2,
    /// Lower-right circle quadrant.
    RTG_CORNER_BOTTOM_RIGHT = 3,
};

/// @brief Validate a points array passed to Polyline/Polygon and expose its raw element pointer.
/// @details Polyline/Polygon take an Integer-typed Zanna array of length
///          `count * 2` (interleaved x/y pairs). This helper rejects: NULL
///          array, non-positive count, count > INT64_MAX/2 (so the multiply
///          can't overflow), arrays whose heap header is missing or wrong
///          kind (must be RT_HEAP_ARRAY of RT_ELEM_I64), and arrays shorter
///          than `count * 2` elements.
/// @param points_ptr Opaque pointer to the heap-allocated array.
/// @param count      Number of (x, y) pairs the caller intends to read.
/// @param points_out Out: raw int64_t* into the array on success, NULL on failure.
/// @return 1 if the array is safe to walk for `count` pairs, 0 otherwise (no draw).
static int8_t rt_canvas_points_checked(void *points_ptr,
                                       int64_t count,
                                       const int64_t **points_out) {
    if (points_out)
        *points_out = NULL;
    if (!points_ptr || count <= 0 || count > INT64_MAX / 2)
        return 0;

    rt_heap_info_t heap_info;
    if (!rt_heap_get_info(points_ptr, &heap_info))
        return 0;
    if ((rt_heap_kind_t)heap_info.kind != RT_HEAP_ARRAY ||
        (rt_elem_kind_t)heap_info.elem_kind != RT_ELEM_I64)
        return 0;

    uint64_t required = (uint64_t)count * 2u;
    if (required > heap_info.len)
        return 0;
    if (points_out)
        *points_out = (const int64_t *)points_ptr;
    return 1;
}

/// @brief Floor a long double to int64, saturating at INT64_MIN/MAX instead of overflowing.
/// @param value Finite value to convert.
/// @return Mathematical floor saturated to the int64 range.
static int64_t rt_canvas_adv_floor_ld_to_i64_sat(long double value) {
    if (isnan(value))
        return 0;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)floorl(value);
}

/// @brief Ceil a long double to int64, saturating at INT64_MIN/MAX instead of overflowing.
/// @param value Finite value to convert.
/// @return Mathematical ceiling saturated to the int64 range.
static int64_t rt_canvas_adv_ceil_ld_to_i64_sat(long double value) {
    if (isnan(value))
        return 0;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)ceill(value);
}

/// @brief Convert two's-complement unsigned bits to int64 without relying on an
///        implementation-defined out-of-range cast.
static int64_t rt_canvas_adv_i64_from_bits(uint64_t bits) {
    if (bits <= (uint64_t)INT64_MAX)
        return (int64_t)bits;
    return INT64_MIN + (int64_t)(bits - (UINT64_C(1) << 63u));
}

/// @brief Exact unsigned magnitude of a signed-coordinate difference.
static uint64_t rt_canvas_adv_abs_diff_u64(int64_t a, int64_t b) {
    return a >= b ? (uint64_t)a - (uint64_t)b : (uint64_t)b - (uint64_t)a;
}

/// @brief Compute floor(value * numerator / denominator) without overflow.
/// @param remainder_out Optional flag set when the exact quotient has a
///        non-zero fractional remainder.
static uint64_t rt_canvas_adv_scale_floor_u64(uint64_t value,
                                              uint64_t numerator,
                                              uint64_t denominator,
                                              int8_t *remainder_out) {
    if (remainder_out)
        *remainder_out = 0;
    if (value == 0 || numerator == 0 || denominator == 0)
        return 0;
    if (numerator >= denominator)
        return value;

    uint64_t quotient = 0;
    uint64_t remainder = 0;
    for (int bit = 63; bit >= 0; bit--) {
        uint64_t carry = 0;
        if (remainder >= denominator - remainder) {
            remainder -= denominator - remainder;
            carry = 1;
        } else {
            remainder += remainder;
        }
        quotient = quotient * 2u + carry;

        if (((value >> (unsigned)bit) & 1u) != 0u) {
            if (remainder >= denominator - numerator) {
                remainder -= denominator - numerator;
                quotient++;
            } else {
                remainder += numerator;
            }
        }
    }
    if (remainder_out)
        *remainder_out = remainder != 0;
    return quotient;
}

/// @brief Linearly interpolate the floored x coordinate at scanline @p y.
/// @details Exact quotient/remainder arithmetic keeps rasterization identical
///          across platforms even for segments spanning the full int64 range.
///          Degenerate horizontal segments return @p x0.
/// @param x0 First endpoint X.
/// @param y0 First endpoint Y.
/// @param x1 Second endpoint X.
/// @param y1 Second endpoint Y.
/// @param y Scanline to sample.
/// @return Interpolated x at scanline @p y, saturated to int64.
static int64_t rt_canvas_adv_interp_x(int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t y) {
    if (y1 == y0)
        return x0;

    uint64_t denominator = rt_canvas_adv_abs_diff_u64(y1, y0);
    uint64_t numerator = rt_canvas_adv_abs_diff_u64(y, y0);
    if (numerator >= denominator)
        return x1;
    uint64_t distance = rt_canvas_adv_abs_diff_u64(x1, x0);
    int8_t has_remainder = 0;
    uint64_t magnitude =
        rt_canvas_adv_scale_floor_u64(distance, numerator, denominator, &has_remainder);
    if (x1 < x0 && has_remainder)
        magnitude++;
    uint64_t bits = x1 >= x0 ? (uint64_t)x0 + magnitude : (uint64_t)x0 - magnitude;
    return rt_canvas_adv_i64_from_bits(bits);
}

/// @brief Convert a Zanna packed color to a 24-bit ZannaGFX RGB value.
/// @details Normalizes through rt_pixels_color_to_rgba (0xRRGGBBAA) then drops
///          the alpha byte, since the advanced canvas primitives draw opaque.
/// @param color Runtime RGB, tagged ARGB, or raw RGBA color.
/// @return Opaque backend `0xRRGGBB` value.
static vgfx_color_t rt_canvas_adv_color_to_vgfx_rgb(int64_t color) {
    return (vgfx_color_t)((rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu);
}

/// @brief Interpolate one byte channel at an exact unsigned ratio.
static uint8_t rt_canvas_adv_lerp_channel(uint8_t from,
                                          uint8_t to,
                                          uint64_t numerator,
                                          uint64_t denominator) {
    if (numerator == 0 || from == to || denominator == 0)
        return from;
    if (numerator >= denominator)
        return to;
    uint64_t distance = from >= to ? (uint64_t)from - to : (uint64_t)to - from;
    uint64_t delta = rt_canvas_adv_scale_floor_u64(distance, numerator, denominator, NULL);
    return (uint8_t)(to >= from ? (uint64_t)from + delta : (uint64_t)from - delta);
}

/// @brief Interpolate canonical RGBA channels without overflowing coordinates.
static uint32_t rt_canvas_adv_lerp_rgba(uint32_t from,
                                        uint32_t to,
                                        uint64_t numerator,
                                        uint64_t denominator) {
    uint8_t r = rt_canvas_adv_lerp_channel(
        (uint8_t)(from >> 24u), (uint8_t)(to >> 24u), numerator, denominator);
    uint8_t g = rt_canvas_adv_lerp_channel(
        (uint8_t)(from >> 16u), (uint8_t)(to >> 16u), numerator, denominator);
    uint8_t b = rt_canvas_adv_lerp_channel(
        (uint8_t)(from >> 8u), (uint8_t)(to >> 8u), numerator, denominator);
    uint8_t a = rt_canvas_adv_lerp_channel((uint8_t)from, (uint8_t)to, numerator, denominator);
    return ((uint32_t)r << 24u) | ((uint32_t)g << 16u) | ((uint32_t)b << 8u) | a;
}

/// @brief Portable unsigned 128-bit product represented as high/low words.
typedef struct rt_canvas_adv_u128 {
    uint64_t high;
    uint64_t low;
} rt_canvas_adv_u128;

/// @brief Portable unsigned 129-bit squared-distance representation.
typedef struct rt_canvas_adv_u129 {
    uint8_t top;
    uint64_t high;
    uint64_t low;
} rt_canvas_adv_u129;

/// @brief Multiply two uint64 values without compiler-specific integer types.
static rt_canvas_adv_u128 rt_canvas_adv_mul_u64_wide(uint64_t a, uint64_t b) {
    uint64_t a0 = (uint32_t)a;
    uint64_t a1 = a >> 32u;
    uint64_t b0 = (uint32_t)b;
    uint64_t b1 = b >> 32u;
    uint64_t w0 = a0 * b0;
    uint64_t t = a1 * b0 + (w0 >> 32u);
    uint64_t w1 = t & UINT64_C(0xFFFFFFFF);
    uint64_t w2 = t >> 32u;
    w1 += a0 * b1;
    rt_canvas_adv_u128 result = {
        a1 * b1 + w2 + (w1 >> 32u),
        (w1 << 32u) | (w0 & UINT64_C(0xFFFFFFFF)),
    };
    return result;
}

/// @brief Test triangle collinearity with exact determinant products.
static int8_t rt_canvas_adv_triangle_is_collinear(
    int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t x3, int64_t y3) {
    uint64_t ax = rt_canvas_adv_abs_diff_u64(x2, x1);
    uint64_t ay = rt_canvas_adv_abs_diff_u64(y2, y1);
    uint64_t bx = rt_canvas_adv_abs_diff_u64(x3, x1);
    uint64_t by = rt_canvas_adv_abs_diff_u64(y3, y1);
    int left_negative = (x2 < x1) ^ (y3 < y1);
    int right_negative = (y2 < y1) ^ (x3 < x1);
    rt_canvas_adv_u128 left = rt_canvas_adv_mul_u64_wide(ax, by);
    rt_canvas_adv_u128 right = rt_canvas_adv_mul_u64_wide(ay, bx);
    int left_zero = left.high == 0 && left.low == 0;
    int right_zero = right.high == 0 && right.low == 0;
    if (left_zero || right_zero)
        return left_zero && right_zero;
    if (left_negative != right_negative)
        return 0;
    return left.high == right.high && left.low == right.low;
}

/// @brief Compute an exact squared edge length using a portable 129-bit sum.
static rt_canvas_adv_u129 rt_canvas_adv_edge_distance2(int64_t x1,
                                                       int64_t y1,
                                                       int64_t x2,
                                                       int64_t y2) {
    uint64_t dx = rt_canvas_adv_abs_diff_u64(x1, x2);
    uint64_t dy = rt_canvas_adv_abs_diff_u64(y1, y2);
    rt_canvas_adv_u128 x2_wide = rt_canvas_adv_mul_u64_wide(dx, dx);
    rt_canvas_adv_u128 y2_wide = rt_canvas_adv_mul_u64_wide(dy, dy);
    rt_canvas_adv_u129 result;
    result.low = x2_wide.low + y2_wide.low;
    uint64_t carry = result.low < x2_wide.low;
    result.high = x2_wide.high + y2_wide.high;
    result.top = result.high < x2_wide.high;
    uint64_t prior_high = result.high;
    result.high += carry;
    result.top = (uint8_t)(result.top + (result.high < prior_high));
    return result;
}

/// @brief Compare two portable unsigned 129-bit values.
static int rt_canvas_adv_u129_compare(rt_canvas_adv_u129 a, rt_canvas_adv_u129 b) {
    if (a.top != b.top)
        return a.top < b.top ? -1 : 1;
    if (a.high != b.high)
        return a.high < b.high ? -1 : 1;
    if (a.low != b.low)
        return a.low < b.low ? -1 : 1;
    return 0;
}

/// @brief Degenerate-triangle fallback: when the three vertices are collinear
///        (zero area), draw just the longest of the three edges so the shape
///        still renders as the line it visually is.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x1 First vertex X.
/// @param y1 First vertex Y.
/// @param x2 Second vertex X.
/// @param y2 Second vertex Y.
/// @param x3 Third vertex X.
/// @param y3 Third vertex Y.
/// @param color Packed stroke color.
static void rt_canvas_adv_degenerate_triangle_line(void *canvas_ptr,
                                                   int64_t x1,
                                                   int64_t y1,
                                                   int64_t x2,
                                                   int64_t y2,
                                                   int64_t x3,
                                                   int64_t y3,
                                                   int64_t color) {
    rt_canvas_adv_u129 d12 = rt_canvas_adv_edge_distance2(x1, y1, x2, y2);
    rt_canvas_adv_u129 d23 = rt_canvas_adv_edge_distance2(x2, y2, x3, y3);
    rt_canvas_adv_u129 d31 = rt_canvas_adv_edge_distance2(x3, y3, x1, y1);
    if (rt_canvas_adv_u129_compare(d12, d23) >= 0 && rt_canvas_adv_u129_compare(d12, d31) >= 0) {
        rt_canvas_line(canvas_ptr, x1, y1, x2, y2, color);
    } else if (rt_canvas_adv_u129_compare(d23, d31) >= 0) {
        rt_canvas_line(canvas_ptr, x2, y2, x3, y3, color);
    } else {
        rt_canvas_line(canvas_ptr, x3, y3, x1, y1, color);
    }
}

/// @brief Draw one clipped quarter-circle arc with visible-row scan conversion.
/// @details Work is bounded by the active clip height, even when @p radius is
///          near `INT64_MAX / 2`. Adjacent-row horizontal runs keep the
///          one-pixel outline connected near the circle's top/bottom.
/// @param canvas Canvas. NULL → no-op.
/// @param cx Arc center X in logical coordinates.
/// @param cy Arc center Y in logical coordinates.
/// @param radius Arc radius in pixels.
/// @param corner Quadrant selector (see RTG_CORNER_* enum).
/// @param color  Stroke color (0xAARRGGBB packed).
static void rt_canvas_draw_quarter_circle(
    rt_canvas *canvas, int64_t cx, int64_t cy, int64_t radius, int corner, vgfx_color_t color) {
    if (!canvas || !canvas->gfx_win || radius < 0)
        return;

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t clip_last_x = rtg_add_sat64(clip_x, clip_w - 1);
    int64_t clip_last_y = rtg_add_sat64(clip_y, clip_h - 1);
    int8_t top = corner == RTG_CORNER_TOP_LEFT || corner == RTG_CORNER_TOP_RIGHT;
    int8_t left_side = corner == RTG_CORNER_TOP_LEFT || corner == RTG_CORNER_BOTTOM_LEFT;
    int64_t y0 = top ? rtg_sub_nonneg_sat64(cy, radius) : cy;
    int64_t y1 = top ? cy : rtg_add_sat64(cy, radius);
    if (y0 < clip_y)
        y0 = clip_y;
    if (y1 > clip_last_y)
        y1 = clip_last_y;
    if (y1 < y0)
        return;

    long double radius2 = (long double)radius * (long double)radius;
    int8_t have_previous = 0;
    int64_t previous_x = 0;
    for (int64_t y = y0; y <= y1; y++) {
        long double dy = (long double)y - (long double)cy;
        long double remaining = radius2 - dy * dy;
        if (remaining < 0.0L) {
            have_previous = 0;
            continue;
        }
        int64_t offset = rt_canvas_adv_floor_ld_to_i64_sat(sqrtl(remaining) + 0.5L);
        int64_t x = left_side ? rtg_sub_nonneg_sat64(cx, offset) : rtg_add_sat64(cx, offset);
        int64_t run_x0 = have_previous ? rtg_min64(previous_x, x) : x;
        int64_t run_x1 = have_previous ? rtg_max64(previous_x, x) : x;
        if (run_x0 < clip_x)
            run_x0 = clip_x;
        if (run_x1 > clip_last_x)
            run_x1 = clip_last_x;
        for (int64_t plot_x = run_x0; plot_x <= run_x1; plot_x++)
            vgfx_pset(canvas->gfx_win, (int32_t)plot_x, (int32_t)y, color);
        previous_x = x;
        have_previous = 1;
    }
}

/// @brief Return the canvas clip rect converted from logical to physical pixels.
/// @details The canvas tracks a logical clip rect (in logical pixels), but
///          some primitives (notably gradient_h/gradient_v's per-pixel inner
///          loops) want to walk physical pixels directly. This helper
///          multiplies through by the window's HiDPI scale factor and returns
///          both the scale and the [px0, px1) × [py0, py1) physical-pixel
///          bounds. NULL out-pointers are individually skipped.
/// @param canvas Borrowed live Canvas implementation.
/// @param scale_out Optional output for the effective coordinate scale.
/// @param px0 Optional output for inclusive physical left edge.
/// @param py0 Optional output for inclusive physical top edge.
/// @param px1 Optional output for exclusive physical right edge.
/// @param py1 Optional output for exclusive physical bottom edge.
/// @return `1` when a nonempty effective clip/window intersection is
///         available; otherwise `0`.
static int8_t rt_canvas_get_scaled_clip_bounds(
    rt_canvas *canvas, float *scale_out, int64_t *px0, int64_t *py0, int64_t *px1, int64_t *py1) {
    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return 0;

    float scale = rt_canvas_effective_coord_scale(canvas);
    if (scale_out)
        *scale_out = scale;
    if (px0)
        *px0 = rtg_scale_up_i64(clip_x, scale);
    if (py0)
        *py0 = rtg_scale_up_i64(clip_y, scale);
    if (px1)
        *px1 = rtg_scale_up_i64(rtg_add_sat64(clip_x, clip_w), scale);
    if (py1)
        *py1 = rtg_scale_up_i64(rtg_add_sat64(clip_y, clip_h), scale);
    return 1;
}

//=============================================================================
// Extended Drawing Primitives
//=============================================================================

/// @brief Draw a thick line segment with rounded endcaps.
///
/// `thickness == 1` falls through to the fast hairline `vgfx_line`.
/// Wider lines are tessellated as a filled parallelogram (the body
/// of the line, perpendicular to the segment) plus two filled
/// circles at the endpoints to round off the caps. A coincident segment
/// becomes one disc. Drawing is clipped through the underlying primitives.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x1 Logical start X.
/// @param y1 Logical start Y.
/// @param x2 Logical end X.
/// @param y2 Logical end Y.
/// @param thickness Positive logical stroke width.
/// @param color Packed stroke color; effective alpha is ignored.
void rt_canvas_thick_line(void *canvas_ptr,
                          int64_t x1,
                          int64_t y1,
                          int64_t x2,
                          int64_t y2,
                          int64_t thickness,
                          int64_t color) {
    if (!canvas_ptr || thickness <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    if (thickness == 1) {
        rt_canvas_line(canvas, x1, y1, x2, y2, color);
        return;
    }

    // Draw thick line as a filled parallelogram body + two round endcap circles.
    // This is O((length + r) * r) vs O(length * r^2) for circle-per-step,
    // which is a factor-of-r speedup for large thickness values.
    int64_t half = thickness / 2;

    if (x1 == x2 && y1 == y2) {
        rt_canvas_disc(canvas, x1, y1, half, color);
        return;
    }

    rt_canvas_disc(canvas, x1, y1, half, color);
    rt_canvas_disc(canvas, x2, y2, half, color);

    // Parallelogram body: four corners offset by perpendicular half-width.
    long double ldx = (long double)x2 - (long double)x1;
    long double ldy = (long double)y2 - (long double)y1;
    long double len = sqrtl(ldx * ldx + ldy * ldy);
    if (len <= 0.0L || !isfinite((double)len))
        return;
    // Perpendicular unit vector (rotated 90 degrees)
    long double px = (-ldy / len) * (long double)half;
    long double py = (ldx / len) * (long double)half;

    // Four corners of the parallelogram
    long double ax = (long double)x1 + px, ay = (long double)y1 + py;
    long double bx = (long double)x1 - px, by = (long double)y1 - py;
    long double cx = (long double)x2 + px, cy = (long double)y2 + py;
    long double dx = (long double)x2 - px, dy_c = (long double)y2 - py;

    // Scanline fill the parallelogram (convex 4-vertex polygon).
    int64_t y_lo = rt_canvas_adv_floor_ld_to_i64_sat(fminl(fminl(ay, by), fminl(cy, dy_c)));
    int64_t y_hi = rt_canvas_adv_ceil_ld_to_i64_sat(fmaxl(fmaxl(ay, by), fmaxl(cy, dy_c)));
    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t clip_last_y = rtg_add_sat64(clip_y, clip_h - 1);
    if (y_lo < clip_y)
        y_lo = clip_y;
    if (y_hi > clip_last_y)
        y_hi = clip_last_y;

    for (int64_t scan_y = y_lo; scan_y <= y_hi; scan_y++) {
        long double sv = (long double)scan_y;
        long double x_min = 1e300L, x_max = -1e300L;
        long double xi;

        // Edge A->C
        if (fminl(ay, cy) <= sv && sv <= fmaxl(ay, cy) && ay != cy) {
            xi = ax + (cx - ax) * (sv - ay) / (cy - ay);
            if (xi < x_min)
                x_min = xi;
            if (xi > x_max)
                x_max = xi;
        }
        // Edge C->D
        if (fminl(cy, dy_c) <= sv && sv <= fmaxl(cy, dy_c) && cy != dy_c) {
            xi = cx + (dx - cx) * (sv - cy) / (dy_c - cy);
            if (xi < x_min)
                x_min = xi;
            if (xi > x_max)
                x_max = xi;
        }
        // Edge D->B
        if (fminl(dy_c, by) <= sv && sv <= fmaxl(dy_c, by) && dy_c != by) {
            xi = dx + (bx - dx) * (sv - dy_c) / (by - dy_c);
            if (xi < x_min)
                x_min = xi;
            if (xi > x_max)
                x_max = xi;
        }
        // Edge B->A
        if (fminl(by, ay) <= sv && sv <= fmaxl(by, ay) && by != ay) {
            xi = bx + (ax - bx) * (sv - by) / (ay - by);
            if (xi < x_min)
                x_min = xi;
            if (xi > x_max)
                x_max = xi;
        }

        if (x_max >= x_min) {
            int64_t lx = rt_canvas_adv_floor_ld_to_i64_sat(x_min);
            int64_t rx = rt_canvas_adv_ceil_ld_to_i64_sat(x_max);
            int64_t clip_last_x = rtg_add_sat64(clip_x, clip_w - 1);
            if (rx < clip_x || lx > clip_last_x)
                continue;
            if (lx < clip_x)
                lx = clip_x;
            if (rx > clip_last_x)
                rx = clip_last_x;
            /* Height-1 logical rect (scale-aware) rather than vgfx_line, which would
             * leave gaps between physical rows on a HiDPI canvas (striped fill). */
            rt_canvas_fill_hspan(canvas_ptr, lx, rx, scan_y, color);
        }
    }
}

/// @brief Fill a rectangle whose corners are quarter-circles of radius `radius`.
///
/// Radius clamps to zero through half the smaller dimension. The straight
/// middle is filled as a rectangle and top/bottom curved rows are generated
/// from the circle equation. A zero radius delegates to Canvas.Box.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge.
/// @param y Logical top edge.
/// @param w Positive width.
/// @param h Positive height.
/// @param radius Requested corner radius.
/// @param color Packed fill color; effective alpha is ignored.
void rt_canvas_round_box(
    void *canvas_ptr, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius, int64_t color) {
    if (!canvas_ptr || w <= 0 || h <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    // Clamp radius to half of smallest dimension
    int64_t max_radius = rtg_min64(w, h) / 2;
    if (radius > max_radius)
        radius = max_radius;
    if (radius < 0)
        radius = 0;

    if (radius == 0) {
        rt_canvas_box(canvas, x, y, w, h, color);
        return;
    }

    int64_t middle_h = h - 2 * radius;
    if (middle_h > 0)
        rt_canvas_box(canvas, x, rtg_add_sat64(y, radius), w, middle_h, color);

    int64_t cx_left = rtg_add_sat64(x, radius);
    int64_t cx_right = rtg_add_sat64(x, w - radius - 1);
    int64_t cy_top = rtg_add_sat64(y, radius);
    int64_t cy_bottom = rtg_add_sat64(y, h - radius - 1);
    long double r2 = (long double)radius * (long double)radius;
    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    (void)clip_x;
    (void)clip_w;
    int64_t clip_last_y = rtg_add_sat64(clip_y, clip_h - 1);

    int64_t top_start = rtg_max64(rtg_sub_nonneg_sat64(cy_top, radius), clip_y);
    int64_t top_end = rtg_min64(rtg_sub_nonneg_sat64(cy_top, 1), clip_last_y);
    for (int64_t row = top_start; row <= top_end; row++) {
        long double dy = (long double)row - (long double)cy_top;
        long double rem = r2 - dy * dy;
        int64_t span = rem <= 0.0L ? 0 : rt_canvas_adv_floor_ld_to_i64_sat(sqrtl(rem));
        rt_canvas_fill_hspan(
            canvas, rtg_add_sat64(cx_left, -span), rtg_add_sat64(cx_right, span), row, color);
    }

    int64_t bottom_start = rtg_max64(rtg_add_sat64(cy_bottom, 1), clip_y);
    int64_t bottom_end = rtg_min64(rtg_add_sat64(cy_bottom, radius), clip_last_y);
    for (int64_t row = bottom_start; row <= bottom_end; row++) {
        long double dy = (long double)row - (long double)cy_bottom;
        long double rem = r2 - dy * dy;
        int64_t span = rem <= 0.0L ? 0 : rt_canvas_adv_floor_ld_to_i64_sat(sqrtl(rem));
        rt_canvas_fill_hspan(
            canvas, rtg_add_sat64(cx_left, -span), rtg_add_sat64(cx_right, span), row, color);
    }
}

/// @brief Stroke (outline only) the same rounded-rectangle shape as `rt_canvas_round_box`.
///
/// Renders four clipped straight edge segments and four midpoint-rasterized
/// quarter-circle arcs. Radius follows the same clamp as the filled variant.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge.
/// @param y Logical top edge.
/// @param w Positive width.
/// @param h Positive height.
/// @param radius Requested corner radius.
/// @param color Packed stroke color; effective alpha is ignored.
void rt_canvas_round_frame(
    void *canvas_ptr, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius, int64_t color) {
    if (!canvas_ptr || w <= 0 || h <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    vgfx_color_t col = rt_canvas_adv_color_to_vgfx_rgb(color);

    // Clamp radius
    int64_t max_radius = rtg_min64(w, h) / 2;
    if (radius > max_radius)
        radius = max_radius;
    if (radius < 0)
        radius = 0;

    if (radius == 0) {
        rt_canvas_frame(canvas, x, y, w, h, color);
        return;
    }

    rt_canvas_line(canvas, rtg_add_sat64(x, radius), y, rtg_add_sat64(x, w - radius - 1), y, color);
    rt_canvas_line(canvas,
                   rtg_add_sat64(x, radius),
                   rtg_add_sat64(y, h - 1),
                   rtg_add_sat64(x, w - radius - 1),
                   rtg_add_sat64(y, h - 1),
                   color);

    rt_canvas_line(canvas, x, rtg_add_sat64(y, radius), x, rtg_add_sat64(y, h - radius - 1), color);
    rt_canvas_line(canvas,
                   rtg_add_sat64(x, w - 1),
                   rtg_add_sat64(y, radius),
                   rtg_add_sat64(x, w - 1),
                   rtg_add_sat64(y, h - radius - 1),
                   color);

    // Draw corner arcs as true quarter circles so the outline stays hollow.
    rt_canvas_draw_quarter_circle(canvas,
                                  rtg_add_sat64(x, radius),
                                  rtg_add_sat64(y, radius),
                                  radius,
                                  RTG_CORNER_TOP_LEFT,
                                  col);
    rt_canvas_draw_quarter_circle(canvas,
                                  rtg_add_sat64(x, w - radius - 1),
                                  rtg_add_sat64(y, radius),
                                  radius,
                                  RTG_CORNER_TOP_RIGHT,
                                  col);
    rt_canvas_draw_quarter_circle(canvas,
                                  rtg_add_sat64(x, radius),
                                  rtg_add_sat64(y, h - radius - 1),
                                  radius,
                                  RTG_CORNER_BOTTOM_LEFT,
                                  col);
    rt_canvas_draw_quarter_circle(canvas,
                                  rtg_add_sat64(x, w - radius - 1),
                                  rtg_add_sat64(y, h - radius - 1),
                                  radius,
                                  RTG_CORNER_BOTTOM_RIGHT,
                                  col);
}

/// @brief One pending or completed horizontal Canvas flood-fill run.
typedef struct rt_canvas_flood_segment {
    int64_t left;
    int64_t right;
    int64_t y;
} rt_canvas_flood_segment;

/// @brief Append a flood-fill run to a geometrically grown vector.
static int8_t rt_canvas_flood_segment_push(rt_canvas_flood_segment **segments_io,
                                           size_t *count_io,
                                           size_t *cap_io,
                                           int64_t left,
                                           int64_t right,
                                           int64_t y) {
    if (!segments_io || !count_io || !cap_io)
        return 0;
    if (*count_io >= *cap_io) {
        size_t new_cap = *cap_io ? *cap_io * 2u : 32u;
        if (new_cap < *cap_io || new_cap > SIZE_MAX / sizeof(**segments_io))
            return 0;
        rt_canvas_flood_segment *grown =
            (rt_canvas_flood_segment *)realloc(*segments_io, new_cap * sizeof(**segments_io));
        if (!grown)
            return 0;
        *segments_io = grown;
        *cap_io = new_cap;
    }
    (*segments_io)[(*count_io)++] = (rt_canvas_flood_segment){left, right, y};
    return 1;
}

/// @brief Return a pointer to one validated physical framebuffer pixel.
static uint8_t *rt_canvas_flood_pixel(vgfx_framebuffer_t *fb, int64_t x, int64_t y) {
    return fb->pixels + (size_t)y * (size_t)fb->stride + (size_t)x * 4u;
}

/// @brief Test whether one physical framebuffer pixel matches an RGBA color.
static int8_t rt_canvas_flood_matches(vgfx_framebuffer_t *fb,
                                      int64_t x,
                                      int64_t y,
                                      const uint8_t target[4]) {
    uint8_t *pixel = rt_canvas_flood_pixel(fb, x, y);
    return pixel[0] == target[0] && pixel[1] == target[1] && pixel[2] == target[2] &&
           pixel[3] == target[3];
}

/// @brief Flood-fill a 4-connected region of identical RGBA framebuffer pixels.
/// @details The logical seed and active clip are converted to physical HiDPI
///          coordinates. An iterative scanline traversal stores work per run,
///          rather than four entries per changed pixel. Every changed run is
///          recorded in a rollback journal, so allocation failure restores the
///          original region instead of leaving a partial fill.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param start_x Logical starting X.
/// @param start_y Logical starting Y.
/// @param color Packed fill color including effective alpha.
void rt_canvas_flood_fill(void *canvas_ptr, int64_t start_x, int64_t start_y, int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb) || !rtg_framebuffer_is_valid(&fb))
        return;

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h);
    if (start_x < clip_x || start_x >= clip_x1 || start_y < clip_y || start_y >= clip_y1)
        return;

    float scale = 1.0f;
    int64_t clip_px0 = 0;
    int64_t clip_py0 = 0;
    int64_t clip_px1 = 0;
    int64_t clip_py1 = 0;
    if (!rt_canvas_get_scaled_clip_bounds(
            canvas, &scale, &clip_px0, &clip_py0, &clip_px1, &clip_py1))
        return;

    start_x = rtg_scale_up_i64(start_x, scale);
    start_y = rtg_scale_up_i64(start_y, scale);

    if (clip_px0 < 0)
        clip_px0 = 0;
    if (clip_py0 < 0)
        clip_py0 = 0;
    if (clip_px1 > fb.width)
        clip_px1 = fb.width;
    if (clip_py1 > fb.height)
        clip_py1 = fb.height;
    if (clip_px1 <= clip_px0 || clip_py1 <= clip_py0 || start_x < clip_px0 || start_x >= clip_px1 ||
        start_y < clip_py0 || start_y >= clip_py1)
        return;

    uint8_t *start_pixel = rt_canvas_flood_pixel(&fb, start_x, start_y);
    uint8_t target[4] = {start_pixel[0], start_pixel[1], start_pixel[2], start_pixel[3]};
    uint32_t fill_rgba = rt_pixels_color_to_rgba(color);
    uint8_t fill[4] = {
        (uint8_t)((fill_rgba >> 24) & 0xFFu),
        (uint8_t)((fill_rgba >> 16) & 0xFFu),
        (uint8_t)((fill_rgba >> 8) & 0xFFu),
        (uint8_t)(fill_rgba & 0xFFu),
    };
    if (memcmp(target, fill, sizeof(target)) == 0)
        return;

    rt_canvas_flood_segment *stack = NULL;
    size_t stack_count = 0;
    size_t stack_cap = 0;
    rt_canvas_flood_segment *journal = NULL;
    size_t journal_count = 0;
    size_t journal_cap = 0;
    int8_t failed = 0;
    if (!rt_canvas_flood_segment_push(&stack, &stack_count, &stack_cap, start_x, start_x, start_y))
        return;

    while (stack_count > 0 && !failed) {
        rt_canvas_flood_segment seed = stack[--stack_count];
        if (seed.y < clip_py0 || seed.y >= clip_py1 || seed.left < clip_px0 ||
            seed.left >= clip_px1)
            continue;
        if (!rt_canvas_flood_matches(&fb, seed.left, seed.y, target))
            continue;

        int64_t left = seed.left;
        int64_t right = seed.left;
        while (left > clip_px0 && rt_canvas_flood_matches(&fb, left - 1, seed.y, target))
            left--;
        while (right + 1 < clip_px1 && rt_canvas_flood_matches(&fb, right + 1, seed.y, target))
            right++;

        if (!rt_canvas_flood_segment_push(
                &journal, &journal_count, &journal_cap, left, right, seed.y)) {
            failed = 1;
            break;
        }
        for (int64_t x = left; x <= right; x++) {
            uint8_t *pixel = rt_canvas_flood_pixel(&fb, x, seed.y);
            memcpy(pixel, fill, 4u);
        }

        for (int y_offset = -1; y_offset <= 1; y_offset += 2) {
            int64_t scan_y = seed.y + y_offset;
            if (scan_y < clip_py0 || scan_y >= clip_py1)
                continue;
            int64_t x = left;
            while (x <= right) {
                while (x <= right && !rt_canvas_flood_matches(&fb, x, scan_y, target))
                    x++;
                if (x > right)
                    break;
                int64_t run_start = x;
                while (x <= right && rt_canvas_flood_matches(&fb, x, scan_y, target))
                    x++;
                if (!rt_canvas_flood_segment_push(
                        &stack, &stack_count, &stack_cap, run_start, run_start, scan_y)) {
                    failed = 1;
                    break;
                }
            }
            if (failed)
                break;
        }
    }

    if (failed) {
        for (size_t i = 0; i < journal_count; i++) {
            rt_canvas_flood_segment span = journal[i];
            for (int64_t x = span.left; x <= span.right; x++) {
                uint8_t *pixel = rt_canvas_flood_pixel(&fb, x, span.y);
                memcpy(pixel, target, 4u);
            }
        }
    }
    free(journal);
    free(stack);
}

/// @brief Filled triangle defined by three points (any winding order).
///
/// Uses scanline rasterisation: sorts vertices by Y, then walks each
/// horizontal line filling between the active edges. Sub-pixel
/// vertices are not antialiased — this is the fast solid-fill path. A
/// collinear triangle draws its longest edge.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x1 First vertex X.
/// @param y1 First vertex Y.
/// @param x2 Second vertex X.
/// @param y2 Second vertex Y.
/// @param x3 Third vertex X.
/// @param y3 Third vertex Y.
/// @param color Packed fill color; effective alpha is ignored.
void rt_canvas_triangle(void *canvas_ptr,
                        int64_t x1,
                        int64_t y1,
                        int64_t x2,
                        int64_t y2,
                        int64_t x3,
                        int64_t y3,
                        int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    if (rt_canvas_adv_triangle_is_collinear(x1, y1, x2, y2, x3, y3)) {
        rt_canvas_adv_degenerate_triangle_line(canvas_ptr, x1, y1, x2, y2, x3, y3, color);
        return;
    }

    // Sort vertices by y-coordinate (y1 <= y2 <= y3)
    if (y1 > y2) {
        int64_t tmp = x1;
        x1 = x2;
        x2 = tmp;
        tmp = y1;
        y1 = y2;
        y2 = tmp;
    }
    if (y2 > y3) {
        int64_t tmp = x2;
        x2 = x3;
        x3 = tmp;
        tmp = y2;
        y2 = y3;
        y3 = tmp;
    }
    if (y1 > y2) {
        int64_t tmp = x1;
        x1 = x2;
        x2 = tmp;
        tmp = y1;
        y1 = y2;
        y2 = tmp;
    }

    // Handle degenerate cases
    if (y1 == y3) {
        // Horizontal line
        int64_t min_x = rtg_min64(rtg_min64(x1, x2), x3);
        int64_t max_x = rtg_max64(rtg_max64(x1, x2), x3);
        rt_canvas_fill_hspan(canvas, min_x, max_x, y1, color);
        return;
    }

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    (void)clip_x;
    (void)clip_w;
    int64_t scan_start = rtg_max64(y1, clip_y);
    int64_t scan_end = rtg_min64(y3, rtg_add_sat64(clip_y, clip_h) - 1);
    if (scan_end < scan_start)
        return;

    // Fill triangle using scanline algorithm
    for (int64_t y = scan_start; y <= scan_end; y++) {
        int64_t xa, xb;

        if (y < y2) {
            // Upper part of triangle
            xa = rt_canvas_adv_interp_x(x1, y1, x2, y2, y);
        } else {
            // Lower part of triangle
            xa = rt_canvas_adv_interp_x(x2, y2, x3, y3, y);
        }

        // Long edge from y1 to y3
        xb = rt_canvas_adv_interp_x(x1, y1, x3, y3, y);

        if (xa > xb) {
            int64_t tmp = xa;
            xa = xb;
            xb = tmp;
        }

        rt_canvas_fill_hspan(canvas, xa, xb, y, color);
    }
}

/// @brief Outline-only triangle — three line segments connecting the vertices.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x1 First vertex X.
/// @param y1 First vertex Y.
/// @param x2 Second vertex X.
/// @param y2 Second vertex Y.
/// @param x3 Third vertex X.
/// @param y3 Third vertex Y.
/// @param color Packed stroke color; effective alpha is ignored.
void rt_canvas_triangle_frame(void *canvas_ptr,
                              int64_t x1,
                              int64_t y1,
                              int64_t x2,
                              int64_t y2,
                              int64_t x3,
                              int64_t y3,
                              int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas_line(canvas_ptr, x1, y1, x2, y2, color);
    rt_canvas_line(canvas_ptr, x2, y2, x3, y3, color);
    rt_canvas_line(canvas_ptr, x3, y3, x1, y1, color);
}

/// @brief Filled axis-aligned ellipse centered at `(cx, cy)` with radii `(rx, ry)`.
///
/// Uses the scanline algorithm: for each y from -ry to +ry, compute
/// the x extent from the ellipse equation and fill that horizontal
/// span. Avoids the trig calls of polar tessellation. Equal radii delegate
/// to the filled-circle primitive.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param rx Strictly positive horizontal radius.
/// @param ry Strictly positive vertical radius.
/// @param color Packed fill color; effective alpha is ignored.
void rt_canvas_ellipse(
    void *canvas_ptr, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color) {
    if (!canvas_ptr || rx <= 0 || ry <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    // If rx == ry, it's a circle
    if (rx == ry) {
        rt_canvas_disc(canvas_ptr, cx, cy, rx, color);
        return;
    }

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;

    int64_t y0 = rtg_sub_nonneg_sat64(cy, ry);
    int64_t y1 = rtg_add_sat64(cy, ry);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h) - 1;
    if (y0 < clip_y)
        y0 = clip_y;
    if (y1 > clip_y1)
        y1 = clip_y1;
    if (y1 < y0)
        return;

    long double rx_ld = (long double)rx;
    long double ry_ld = (long double)ry;
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w) - 1;
    for (int64_t py = y0; py <= y1; ++py) {
        long double dy = (long double)py - (long double)cy;
        long double norm = 1.0L - (dy * dy) / (ry_ld * ry_ld);
        if (norm < 0.0L)
            continue;
        long double span = rx_ld * sqrtl(norm);
        int64_t x0 = rt_canvas_adv_floor_ld_to_i64_sat((long double)cx - span);
        int64_t x1 = rt_canvas_adv_ceil_ld_to_i64_sat((long double)cx + span);
        if (x0 < clip_x)
            x0 = clip_x;
        if (x1 > clip_x1)
            x1 = clip_x1;
        if (x1 >= x0)
            /* Scale-aware height-1 rect so the ellipse fill has no HiDPI row gaps. */
            rt_canvas_fill_hspan(canvas_ptr, x0, x1, py, color);
    }
}

/// @brief Stroke (outline only) an ellipse using a polyline approximation.
///
/// Tessellates the ellipse boundary into short line segments — segment
/// count scales with the larger radius so small ellipses don't pay
/// for unnecessary detail, with a 24..4096 segment clamp. Equal radii
/// delegate to the ring primitive.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param rx Strictly positive horizontal radius.
/// @param ry Strictly positive vertical radius.
/// @param color Packed stroke color; effective alpha is ignored.
void rt_canvas_ellipse_frame(
    void *canvas_ptr, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color) {
    if (!canvas_ptr || rx <= 0 || ry <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    // If rx == ry, it's a circle
    if (rx == ry) {
        rt_canvas_ring(canvas_ptr, cx, cy, rx, color);
        return;
    }

    int64_t max_radius = rx > ry ? rx : ry;
    int64_t steps = max_radius > 1024 ? 4096 : max_radius * 4;
    if (steps < 24)
        steps = 24;
    if (steps > 4096)
        steps = 4096;

    long double rx_ld = (long double)rx;
    long double ry_ld = (long double)ry;
    int64_t prev_x = 0;
    int64_t prev_y = 0;
    for (int64_t i = 0; i <= steps; ++i) {
        long double angle = (2.0L * 3.14159265358979323846L * (long double)i) / (long double)steps;
        int64_t px = rt_canvas_adv_floor_ld_to_i64_sat((long double)cx + cosl(angle) * rx_ld);
        int64_t py = rt_canvas_adv_floor_ld_to_i64_sat((long double)cy + sinl(angle) * ry_ld);
        if (i > 0)
            rt_canvas_line(canvas_ptr, prev_x, prev_y, px, py, color);
        prev_x = px;
        prev_y = py;
    }
}

/// @brief Filled ellipse with per-pixel alpha blending against the existing canvas.
///
/// Slower than `rt_canvas_ellipse` because each pixel goes through
/// `vgfx_blend_pixel` instead of a fast solid fill — use only when
/// translucency is needed. Nonpositive alpha draws nothing, and values at
/// least 255 delegate to the opaque ellipse. Encoded color alpha is replaced.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param rx Strictly positive horizontal radius.
/// @param ry Strictly positive vertical radius.
/// @param color Packed source RGB color.
/// @param alpha Explicit blend alpha.
void rt_canvas_ellipse_alpha(void *canvas_ptr,
                             int64_t cx,
                             int64_t cy,
                             int64_t rx,
                             int64_t ry,
                             int64_t color,
                             int64_t alpha) {
    if (!canvas_ptr || rx <= 0 || ry <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win || alpha <= 0)
        return;

    if (alpha >= 255) {
        rt_canvas_ellipse(canvas_ptr, cx, cy, rx, ry, color);
        return;
    }

    if (rx == ry) {
        rt_canvas_disc_alpha(canvas_ptr, cx, cy, rx, color, alpha);
        return;
    }

    uint32_t argb =
        ((uint32_t)(alpha & 0xFF) << 24) | ((rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu);

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t y0 = rtg_sub_nonneg_sat64(cy, ry);
    int64_t y1 = rtg_add_sat64(cy, ry);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h) - 1;
    if (y0 < clip_y)
        y0 = clip_y;
    if (y1 > clip_y1)
        y1 = clip_y1;
    if (y1 < y0)
        return;

    long double rx_ld = (long double)rx;
    long double ry_ld = (long double)ry;
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w) - 1;
    for (int64_t py = y0; py <= y1; ++py) {
        long double dy = (long double)py - (long double)cy;
        long double norm = 1.0L - (dy * dy) / (ry_ld * ry_ld);
        if (norm < 0.0L)
            continue;

        long double span = rx_ld * sqrtl(norm);
        int64_t x0 = rt_canvas_adv_floor_ld_to_i64_sat((long double)cx - span);
        int64_t x1 = rt_canvas_adv_ceil_ld_to_i64_sat((long double)cx + span);
        if (x0 < clip_x)
            x0 = clip_x;
        if (x1 > clip_x1)
            x1 = clip_x1;
        for (int64_t px = x0; px <= x1; ++px)
            vgfx_pset_alpha(canvas->gfx_win, (int32_t)px, (int32_t)py, argb);
    }
}

//=============================================================================
// Advanced Curves & Shapes
//=============================================================================

/// @brief Filled circular arc (pie slice) from `start_deg` to `end_deg`.
///
/// Scans the clipped circle and keeps pixels whose screen-space atan2 angle
/// lies in the normalized interval. Angles increase clockwise from positive X
/// because screen Y increases downward. If normalized end is not greater than
/// start, 360 is added, so equal endpoints select a full circle. The center is
/// always included.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param radius Strictly positive radius.
/// @param start_angle Starting angle in degrees.
/// @param end_angle Ending angle in degrees.
/// @param color Packed fill color; effective alpha is ignored.
void rt_canvas_arc(void *canvas_ptr,
                   int64_t cx,
                   int64_t cy,
                   int64_t radius,
                   int64_t start_angle,
                   int64_t end_angle,
                   int64_t color) {
    if (!canvas_ptr || radius <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    vgfx_color_t col = rt_canvas_adv_color_to_vgfx_rgb(color);

    // Normalize angles (modulo avoids near-infinite loop for extreme values)
    start_angle = ((start_angle % 360) + 360) % 360;
    end_angle = ((end_angle % 360) + 360) % 360;

    if (end_angle <= start_angle)
        end_angle += 360;

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t y0 = rtg_sub_nonneg_sat64(cy, radius);
    int64_t y1 = rtg_add_sat64(cy, radius);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h) - 1;
    if (y0 < clip_y)
        y0 = clip_y;
    if (y1 > clip_y1)
        y1 = clip_y1;
    if (y1 < y0)
        return;

    long double r2 = (long double)radius * (long double)radius;
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w) - 1;
    for (int64_t py = y0; py <= y1; py++) {
        long double y_math = (long double)py - (long double)cy;
        long double rem = r2 - y_math * y_math;
        if (rem < 0.0L)
            continue;
        long double span = sqrtl(rem);
        int64_t x0 = rt_canvas_adv_floor_ld_to_i64_sat((long double)cx - span);
        int64_t x1 = rt_canvas_adv_ceil_ld_to_i64_sat((long double)cx + span);
        if (x0 < clip_x)
            x0 = clip_x;
        if (x1 > clip_x1)
            x1 = clip_x1;
        for (int64_t px = x0; px <= x1; px++) {
            long double x_math = (long double)px - (long double)cx;
            if (x_math == 0.0L && y_math == 0.0L) {
                vgfx_pset(canvas->gfx_win, (int32_t)px, (int32_t)py, col);
                continue;
            }

            long double angle = atan2l(y_math, x_math) * (180.0L / 3.14159265358979323846L);
            if (angle < 0.0L)
                angle += 360.0L;
            long double check_angle = angle;
            if (check_angle < (long double)start_angle)
                check_angle += 360.0L;
            if (check_angle >= (long double)start_angle && check_angle <= (long double)end_angle)
                vgfx_pset(canvas->gfx_win, (int32_t)px, (int32_t)py, col);
        }
    }
}

/// @brief Stroke (outline only) a circular arc segment — just the curve, no radii.
/// @details Normalizes angles with the same wrap policy as rt_canvas_arc() and
///          tessellates the circumference into 10..4096 clipped line segments.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param radius Strictly positive radius.
/// @param start_angle Starting angle in degrees.
/// @param end_angle Ending angle in degrees.
/// @param color Packed stroke color; effective alpha is ignored.
void rt_canvas_arc_frame(void *canvas_ptr,
                         int64_t cx,
                         int64_t cy,
                         int64_t radius,
                         int64_t start_angle,
                         int64_t end_angle,
                         int64_t color) {
    if (!canvas_ptr || radius <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    // Normalize angles (modulo avoids near-infinite loop for extreme values)
    start_angle = ((start_angle % 360) + 360) % 360;
    end_angle = ((end_angle % 360) + 360) % 360;

    if (end_angle <= start_angle)
        end_angle += 360;

    // Draw arc outline by stepping through angles.
    int64_t span = end_angle - start_angle;
    int64_t steps = radius > INT64_MAX / span ? 4096 : (span * radius) / 30;
    if (steps < 10)
        steps = 10;
    if (steps > 4096)
        steps = 4096;

    int64_t prev_x = 0;
    int64_t prev_y = 0;
    long double radius_ld = (long double)radius;
    for (int64_t i = 0; i <= steps; i++) {
        long double angle_deg =
            (long double)start_angle + ((long double)span * (long double)i) / (long double)steps;
        long double angle = angle_deg * (3.14159265358979323846L / 180.0L);
        int64_t px = rt_canvas_adv_floor_ld_to_i64_sat((long double)cx + cosl(angle) * radius_ld);
        int64_t py = rt_canvas_adv_floor_ld_to_i64_sat((long double)cy + sinl(angle) * radius_ld);
        if (i > 0)
            rt_canvas_line(canvas_ptr, prev_x, prev_y, px, py, color);
        prev_x = px;
        prev_y = py;
    }
}

/// @brief Stroke a quadratic Bezier curve from `(x1,y1)` to `(x2,y2)` with one
///        control point `(cx,cy)`.
///
/// Evaluates B(t) = (1-t)^2*P1 + 2(1-t)t*C + t^2*P2 at a step count chosen
/// adaptively from the control-polygon length, so a short curve isn't
/// over-tessellated and a long one doesn't show visible facets. Tessellation
/// clamps to 4..256 line segments.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x1 Start point X.
/// @param y1 Start point Y.
/// @param cx Control point X.
/// @param cy Control point Y.
/// @param x2 End point X.
/// @param y2 End point Y.
/// @param color Packed stroke color; effective alpha is ignored.
void rt_canvas_bezier(void *canvas_ptr,
                      int64_t x1,
                      int64_t y1,
                      int64_t cx,
                      int64_t cy,
                      int64_t x2,
                      int64_t y2,
                      int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    // Quadratic Bezier: B(t) = (1-t)^2*P1 + 2(1-t)t*C + t^2*P2. Choose the segment
    // count from the control-polygon length (|P1-C| + |C-P2|) at ~4 px/segment,
    // clamped to [4, 256] so tiny curves stay cheap and huge ones stay smooth.
    long double d1x = (long double)cx - (long double)x1;
    long double d1y = (long double)cy - (long double)y1;
    long double d2x = (long double)x2 - (long double)cx;
    long double d2y = (long double)y2 - (long double)cy;
    long double approx_len = sqrtl(d1x * d1x + d1y * d1y) + sqrtl(d2x * d2x + d2y * d2y);
    int64_t steps = approx_len >= 1024.0L ? 256 : (int64_t)(approx_len / 4.0L);
    if (steps < 4)
        steps = 4;
    if (steps > 256)
        steps = 256;
    int64_t px = x1, py = y1;

    for (int64_t i = 1; i <= steps; i++) {
        long double t = (long double)i / (long double)steps;
        long double mt = 1.0L - t;
        int64_t nx = rt_canvas_adv_floor_ld_to_i64_sat(
            mt * mt * (long double)x1 + 2.0L * mt * t * (long double)cx + t * t * (long double)x2);
        int64_t ny = rt_canvas_adv_floor_ld_to_i64_sat(
            mt * mt * (long double)y1 + 2.0L * mt * t * (long double)cy + t * t * (long double)y2);

        rt_canvas_line(canvas_ptr, px, py, nx, ny, color);
        px = nx;
        py = ny;
    }
}

/// @brief Draw an open polyline through @p count (x,y) point pairs.
/// @details Connects consecutive vertices with rt_canvas_line; does not close
///          the path. No-op when fewer than 2 points are supplied.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param points Borrowed interleaved coordinate buffer.
/// @param count Number of point pairs.
/// @param color Packed stroke color.
static void rt_canvas_polyline_points(void *canvas_ptr,
                                      const int64_t *points,
                                      int64_t count,
                                      int64_t color) {
    if (!canvas_ptr || !points || count < 2)
        return;

    for (int64_t i = 0; i < count - 1; i++) {
        int64_t x1 = points[i * 2];
        int64_t y1 = points[i * 2 + 1];
        int64_t x2 = points[(i + 1) * 2];
        int64_t y2 = points[(i + 1) * 2 + 1];
        rt_canvas_line(canvas_ptr, x1, y1, x2, y2, color);
    }
}

/// @brief Ascending comparator for polygon scanline intersections.
static int rt_canvas_compare_i64(const void *left_ptr, const void *right_ptr) {
    int64_t left = *(const int64_t *)left_ptr;
    int64_t right = *(const int64_t *)right_ptr;
    return left < right ? -1 : left > right ? 1 : 0;
}

/// @brief Draw a filled polygon through @p count (x,y) point pairs.
/// @details Uses a scanline fill: for each clipped scanline, computes edge
///          intersections (closing the last->first edge), sorts them, and
///          fills horizontal spans between intersection pairs using the
///          even-odd rule.
///          Clipped to the canvas logical clip bounds. No-op for < 3 points.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param points Borrowed interleaved coordinate buffer.
/// @param count Number of point pairs.
/// @param color Packed fill color.
static void rt_canvas_polygon_points(void *canvas_ptr,
                                     const int64_t *points,
                                     int64_t count,
                                     int64_t color) {
    if (!canvas_ptr || !points || count < 3)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    // Find bounding box
    int64_t min_y = points[1], max_y = points[1];
    for (int64_t i = 1; i < count; i++) {
        int64_t y = points[i * 2 + 1];
        if (y < min_y)
            min_y = y;
        if (y > max_y)
            max_y = y;
    }

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;

    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h) - 1;
    if (min_y < clip_y)
        min_y = clip_y;
    if (max_y > clip_y1)
        max_y = clip_y1;
    if (max_y < min_y)
        return;

    if ((uint64_t)count > SIZE_MAX / sizeof(int64_t))
        return;
    int64_t *intersections = (int64_t *)malloc((size_t)count * sizeof(int64_t));
    if (!intersections)
        return;

    // Scanline fill algorithm
    for (int64_t y = min_y; y <= max_y; y++) {
        // Find all edge intersections with this scanline
        int64_t num_intersections = 0;

        for (int64_t i = 0; i < count; i++) {
            int64_t j = (i + 1) % count;
            int64_t y1 = points[i * 2 + 1];
            int64_t y2 = points[j * 2 + 1];

            if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
                int64_t x1 = points[i * 2];
                int64_t x2 = points[j * 2];
                int64_t x = rt_canvas_adv_interp_x(x1, y1, x2, y2, y);
                intersections[num_intersections++] = x;
            }
        }

        qsort(intersections,
              (size_t)num_intersections,
              sizeof(*intersections),
              rt_canvas_compare_i64);

        // Fill between pairs of intersections
        for (int64_t i = 0; i + 1 < num_intersections; i += 2) {
            rt_canvas_fill_hspan(canvas_ptr, intersections[i], intersections[i + 1], y, color);
        }
    }

    free(intersections);
}

/// @brief Draw the outline (frame) of a polygon through @p count point pairs.
/// @details Connects all vertices and closes the last->first edge, without
///          filling. No-op for fewer than 3 points.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param points Borrowed interleaved coordinate buffer.
/// @param count Number of point pairs.
/// @param color Packed stroke color.
static void rt_canvas_polygon_frame_points(void *canvas_ptr,
                                           const int64_t *points,
                                           int64_t count,
                                           int64_t color) {
    if (!canvas_ptr || !points || count < 3)
        return;

    // Draw lines connecting all vertices, including back to start
    for (int64_t i = 0; i < count; i++) {
        int64_t j = (i + 1) % count;
        int64_t x1 = points[i * 2];
        int64_t y1 = points[i * 2 + 1];
        int64_t x2 = points[j * 2];
        int64_t y2 = points[j * 2 + 1];
        rt_canvas_line(canvas_ptr, x1, y1, x2, y2, color);
    }
}

/// @brief Draw an open polyline from an interleaved runtime int64 array.
/// @details The array must contain at least @p count times two elements and
///          have runtime heap kind `array<i64>`. Fewer than two points draw
///          nothing.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param points_ptr Borrowed runtime `array<i64>` of x/y pairs.
/// @param count Number of pairs to consume.
/// @param color Packed stroke color.
void rt_canvas_polyline(void *canvas_ptr, void *points_ptr, int64_t count, int64_t color) {
    const int64_t *points = NULL;
    if (!rt_canvas_points_checked(points_ptr, count, &points))
        return;
    rt_canvas_polyline_points(canvas_ptr, points, count, color);
}

/// @brief Fill a polygon from an interleaved runtime int64 array.
/// @details Validates the array type/length and requires at least three points.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param points_ptr Borrowed runtime `array<i64>` of x/y pairs.
/// @param count Number of pairs to consume.
/// @param color Packed fill color.
void rt_canvas_polygon(void *canvas_ptr, void *points_ptr, int64_t count, int64_t color) {
    const int64_t *points = NULL;
    if (!rt_canvas_points_checked(points_ptr, count, &points))
        return;
    rt_canvas_polygon_points(canvas_ptr, points, count, color);
}

/// @brief Stroke a closed polygon from an interleaved runtime int64 array.
/// @details Validates the array type/length and requires at least three points.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param points_ptr Borrowed runtime `array<i64>` of x/y pairs.
/// @param count Number of pairs to consume.
/// @param color Packed stroke color.
void rt_canvas_polygon_frame(void *canvas_ptr, void *points_ptr, int64_t count, int64_t color) {
    const int64_t *points = NULL;
    if (!rt_canvas_points_checked(points_ptr, count, &points))
        return;
    rt_canvas_polygon_frame_points(canvas_ptr, points, count, color);
}

#ifndef RT_DRAWING_ADVANCED_NO_PATH2D
/// @brief Flatten a Path2D into a freshly-allocated interleaved (x,y) array.
/// @details Reads rt_path2d_count/get_x/get_y into a heap buffer the caller
///          must free(). Fails (returns 0, outputs cleared) when the path has
///          fewer than @p min_count points or the size would overflow.
/// @param path Borrowed Path2D handle.
/// @param min_count Minimum required point count (2 for polyline, 3 polygon).
/// @param points_out Required output receiving an owned `count * 2` int64 array.
/// @param count_out Required output receiving the number of points written.
/// @return `1` on success; `0` for invalid/short paths, overflow, or allocation
///         failure.
static int8_t rt_canvas_path_points(void *path,
                                    int64_t min_count,
                                    int64_t **points_out,
                                    int64_t *count_out) {
    if (!points_out || !count_out)
        return 0;
    *points_out = NULL;
    *count_out = 0;
    if (!path)
        return 0;

    int64_t count = rt_path2d_count(path);
    if (count < min_count || count > INT64_MAX / 2)
        return 0;
    if ((uint64_t)count > SIZE_MAX / (2u * sizeof(int64_t)))
        return 0;

    size_t point_values = (size_t)count * 2u;
    int64_t *points = (int64_t *)malloc(point_values * sizeof(int64_t));
    if (!points)
        return 0;
    for (int64_t i = 0; i < count; ++i) {
        points[i * 2] = rt_path2d_get_x(path, i);
        points[i * 2 + 1] = rt_path2d_get_y(path, i);
    }
    *points_out = points;
    *count_out = count;
    return 1;
}

/// @brief Flatten a Path2D and draw its points as an open polyline.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param path Borrowed Path2D with at least two points.
/// @param color Packed stroke color.
void rt_canvas_polyline_path(void *canvas_ptr, void *path, int64_t color) {
    int64_t *points = NULL;
    int64_t count = 0;
    if (!rt_canvas_path_points(path, 2, &points, &count))
        return;
    rt_canvas_polyline_points(canvas_ptr, points, count, color);
    free(points);
}

/// @brief Flatten a Path2D and fill its points as a closed polygon.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param path Borrowed Path2D with at least three points.
/// @param color Packed fill color.
void rt_canvas_polygon_path(void *canvas_ptr, void *path, int64_t color) {
    int64_t *points = NULL;
    int64_t count = 0;
    if (!rt_canvas_path_points(path, 3, &points, &count))
        return;
    rt_canvas_polygon_points(canvas_ptr, points, count, color);
    free(points);
}

/// @brief Flatten a Path2D and stroke its points as a closed polygon.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param path Borrowed Path2D with at least three points.
/// @param color Packed stroke color.
void rt_canvas_polygon_frame_path(void *canvas_ptr, void *path, int64_t color) {
    int64_t *points = NULL;
    int64_t count = 0;
    if (!rt_canvas_path_points(path, 3, &points, &count))
        return;
    rt_canvas_polygon_frame_points(canvas_ptr, points, count, color);
    free(points);
}
#else
/// @brief No-op Path2D polyline stub used when Path2D integration is disabled.
/// @param canvas_ptr Ignored Canvas handle.
/// @param path Ignored Path2D handle.
/// @param color Ignored color.
void rt_canvas_polyline_path(void *canvas_ptr, void *path, int64_t color) {
    (void)canvas_ptr;
    (void)path;
    (void)color;
}

/// @brief No-op Path2D polygon-fill stub used when Path2D integration is disabled.
/// @param canvas_ptr Ignored Canvas handle.
/// @param path Ignored Path2D handle.
/// @param color Ignored color.
void rt_canvas_polygon_path(void *canvas_ptr, void *path, int64_t color) {
    (void)canvas_ptr;
    (void)path;
    (void)color;
}

/// @brief No-op Path2D polygon-frame stub used when Path2D integration is disabled.
/// @param canvas_ptr Ignored Canvas handle.
/// @param path Ignored Path2D handle.
/// @param color Ignored color.
void rt_canvas_polygon_frame_path(void *canvas_ptr, void *path, int64_t color) {
    (void)canvas_ptr;
    (void)path;
    (void)color;
}
#endif

//=============================================================================
// Gradients
//=============================================================================

/// @brief Fill a rectangle with a horizontal linear gradient between two colors.
///
/// Each column of pixels is a linear interpolation between
/// `color_left` and `color_right`. The blend is per-channel in
/// RGBA space and remains anchored to the original unclipped rectangle, so
/// clipping does not shift the gradient. Direct-framebuffer rendering writes
/// all four channels; the backend line fallback draws opaque RGB. A one-column
/// gradient uses only @p c1.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge.
/// @param y Logical top edge.
/// @param w Positive gradient width.
/// @param h Positive gradient height.
/// @param c1 Color at the original left edge.
/// @param c2 Color at the original right edge.
void rt_canvas_gradient_h(
    void *canvas_ptr, int64_t x, int64_t y, int64_t w, int64_t h, int64_t c1, int64_t c2) {
    if (!canvas_ptr || w <= 0 || h <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    int64_t orig_x = x;
    int64_t orig_w = w;
    if (!rt_canvas_clip_intersect_logical(canvas, &x, &y, &w, &h))
        return;

    uint32_t rgba1 = rt_pixels_color_to_rgba(c1);
    uint32_t rgba2 = rt_pixels_color_to_rgba(c2);
    uint64_t gradient_denominator = (uint64_t)(orig_w > 1 ? orig_w - 1 : 1);

    // Precompute gradient colours for each column, then blit each row of height h
    // with a single memcpy-equivalent pass — avoids w*vgfx_line() call overhead.
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb) || !rtg_framebuffer_is_valid(&fb)) {
        // Fallback: per-column vgfx_line for mock/headless contexts.
        // vgfx_line auto-scales via coord_scale, so pass logical coords.
        for (int64_t col = 0; col < w; col++) {
            int64_t logical_x = rtg_add_sat64(x, col);
            uint64_t gradient_col =
                logical_x >= orig_x ? (uint64_t)logical_x - (uint64_t)orig_x : 0;
            if (gradient_col > gradient_denominator)
                gradient_col = gradient_denominator;
            uint32_t rgba =
                rt_canvas_adv_lerp_rgba(rgba1, rgba2, gradient_col, gradient_denominator);
            vgfx_line(canvas->gfx_win,
                      rtg_clamp_i64_to_i32(logical_x),
                      rtg_clamp_i64_to_i32(y),
                      rtg_clamp_i64_to_i32(logical_x),
                      rtg_clamp_i64_to_i32(rtg_add_sat64(y, h - 1)),
                      (vgfx_color_t)((rgba >> 8u) & 0x00FFFFFFu));
        }
        return;
    }

    float scale = rt_canvas_effective_coord_scale(canvas);
    int64_t px0 = rtg_scale_up_i64(x, scale);
    int64_t px1 = rtg_scale_up_i64(rtg_add_sat64(x, w), scale);
    int64_t py0 = rtg_scale_up_i64(y, scale);
    int64_t py1 = rtg_scale_up_i64(rtg_add_sat64(y, h), scale);
    if (px0 < 0)
        px0 = 0;
    if (py0 < 0)
        py0 = 0;
    if (px1 > fb.width)
        px1 = fb.width;
    if (py1 > fb.height)
        py1 = fb.height;
    if (px1 <= px0 || py1 <= py0)
        return;

    int64_t draw_w = px1 - px0;
    if ((uint64_t)draw_w > SIZE_MAX / 4u)
        return;
    size_t row_bytes = (size_t)draw_w * 4u;
    uint8_t *row_buf = (uint8_t *)malloc(row_bytes);
    if (!row_buf)
        return;

    /* Interpolate channels directly (full 256-level precision) instead of routing
     * through rt_color_lerp's 0..100 percent parameter, which quantizes a wide
     * gradient to at most 101 distinct colors and shows visible banding. */
    memset(row_buf, 0, row_bytes);
    for (int64_t col = 0; col < w; col++) {
        int64_t logical_x = rtg_add_sat64(x, col);
        int64_t col_px0 = rtg_scale_up_i64(logical_x, scale);
        int64_t col_px1 = rtg_scale_up_i64(rtg_add_sat64(logical_x, 1), scale);
        if (col_px1 <= col_px0)
            col_px1 = col_px0 + 1;
        if (col_px0 < px0)
            col_px0 = px0;
        if (col_px1 > px1)
            col_px1 = px1;
        if (col_px1 <= col_px0)
            continue;

        uint64_t gc = logical_x >= orig_x ? (uint64_t)logical_x - (uint64_t)orig_x : 0;
        if (gc > gradient_denominator)
            gc = gradient_denominator;
        uint32_t rgba = rt_canvas_adv_lerp_rgba(rgba1, rgba2, gc, gradient_denominator);
        uint8_t cr = (uint8_t)(rgba >> 24u);
        uint8_t cg = (uint8_t)(rgba >> 16u);
        uint8_t cb = (uint8_t)(rgba >> 8u);
        uint8_t ca = (uint8_t)rgba;
        for (int64_t px = col_px0; px < col_px1; px++) {
            size_t idx = (size_t)(px - px0) * 4u;
            row_buf[idx + 0u] = cr;
            row_buf[idx + 1u] = cg;
            row_buf[idx + 2u] = cb;
            row_buf[idx + 3u] = ca;
        }
    }

    for (int64_t row = py0; row < py1; row++)
        memcpy(&fb.pixels[(size_t)row * (size_t)fb.stride + (size_t)px0 * 4u],
               row_buf,
               (size_t)draw_w * 4u);

    free(row_buf);
}

/// @brief Fill a rectangle with a vertical linear gradient between two colors.
/// @details Interpolates RGBA by original logical row so clipping does not
///          shift the endpoints. Direct-framebuffer rendering writes all four
///          channels; the backend line fallback draws opaque RGB. A one-row
///          gradient uses only @p c1.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge.
/// @param y Logical top edge.
/// @param w Positive gradient width.
/// @param h Positive gradient height.
/// @param c1 Color at the original top edge.
/// @param c2 Color at the original bottom edge.
/// @see rt_canvas_gradient_h
void rt_canvas_gradient_v(
    void *canvas_ptr, int64_t x, int64_t y, int64_t w, int64_t h, int64_t c1, int64_t c2) {
    if (!canvas_ptr || w <= 0 || h <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    int64_t orig_y = y;
    int64_t orig_h = h;
    if (!rt_canvas_clip_intersect_logical(canvas, &x, &y, &w, &h))
        return;

    uint32_t rgba1 = rt_pixels_color_to_rgba(c1);
    uint32_t rgba2 = rt_pixels_color_to_rgba(c2);
    uint64_t gradient_denominator = (uint64_t)(orig_h > 1 ? orig_h - 1 : 1);

    // Write each row directly into the framebuffer — one colour per row, no per-row
    // vgfx_line() overhead.
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb) || !rtg_framebuffer_is_valid(&fb)) {
        // Fallback: vgfx_line auto-scales via coord_scale, so pass logical coords.
        for (int64_t row = 0; row < h; row++) {
            int64_t logical_y = rtg_add_sat64(y, row);
            uint64_t gradient_row =
                logical_y >= orig_y ? (uint64_t)logical_y - (uint64_t)orig_y : 0;
            if (gradient_row > gradient_denominator)
                gradient_row = gradient_denominator;
            uint32_t rgba =
                rt_canvas_adv_lerp_rgba(rgba1, rgba2, gradient_row, gradient_denominator);
            vgfx_line(canvas->gfx_win,
                      rtg_clamp_i64_to_i32(x),
                      rtg_clamp_i64_to_i32(logical_y),
                      rtg_clamp_i64_to_i32(rtg_add_sat64(x, w - 1)),
                      rtg_clamp_i64_to_i32(logical_y),
                      (vgfx_color_t)((rgba >> 8u) & 0x00FFFFFFu));
        }
        return;
    }

    float scale = rt_canvas_effective_coord_scale(canvas);
    int64_t px0 = rtg_scale_up_i64(x, scale);
    int64_t px1 = rtg_scale_up_i64(rtg_add_sat64(x, w), scale);
    int64_t py0 = rtg_scale_up_i64(y, scale);
    int64_t py1 = rtg_scale_up_i64(rtg_add_sat64(y, h), scale);
    if (px0 < 0)
        px0 = 0;
    if (py0 < 0)
        py0 = 0;
    if (px1 > fb.width)
        px1 = fb.width;
    if (py1 > fb.height)
        py1 = fb.height;
    if (px1 <= px0 || py1 <= py0)
        return;

    int64_t draw_w = px1 - px0;
    /* Each logical row is a single solid colour (the gradient runs down the rows), so build one
     * row of `draw_w` pixels and memcpy it across that row's scale-expanded scanlines instead of
     * a per-pixel inner loop — mirrors rt_canvas_gradient_h. draw_w is framebuffer-bounded. */
    if ((uint64_t)draw_w > SIZE_MAX / 4u) /* overflow guard, matching gradient_h */
        return;
    uint8_t *row_buf = (uint8_t *)malloc((size_t)draw_w * 4u);
    if (!row_buf)
        return;
    /* Full-precision per-channel interpolation (avoids rt_color_lerp's 101-step banding). */
    for (int64_t row = 0; row < h; row++) {
        int64_t logical_y = rtg_add_sat64(y, row);
        int64_t row_py0 = rtg_scale_up_i64(logical_y, scale);
        int64_t row_py1 = rtg_scale_up_i64(rtg_add_sat64(logical_y, 1), scale);
        if (row_py1 <= row_py0)
            row_py1 = row_py0 + 1;
        if (row_py0 < py0)
            row_py0 = py0;
        if (row_py1 > py1)
            row_py1 = py1;
        if (row_py1 <= row_py0)
            continue;

        uint64_t gr = logical_y >= orig_y ? (uint64_t)logical_y - (uint64_t)orig_y : 0;
        if (gr > gradient_denominator)
            gr = gradient_denominator;
        uint32_t rgba = rt_canvas_adv_lerp_rgba(rgba1, rgba2, gr, gradient_denominator);
        uint8_t cr = (uint8_t)(rgba >> 24u);
        uint8_t cg = (uint8_t)(rgba >> 16u);
        uint8_t cb = (uint8_t)(rgba >> 8u);
        uint8_t ca = (uint8_t)rgba;
        for (int64_t i = 0; i < draw_w; i++) {
            row_buf[i * 4 + 0] = cr;
            row_buf[i * 4 + 1] = cg;
            row_buf[i * 4 + 2] = cb;
            row_buf[i * 4 + 3] = ca;
        }
        for (int64_t py = row_py0; py < row_py1; py++)
            memcpy(&fb.pixels[(size_t)py * (size_t)fb.stride + (size_t)px0 * 4u],
                   row_buf,
                   (size_t)draw_w * 4u);
    }
    free(row_buf);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
