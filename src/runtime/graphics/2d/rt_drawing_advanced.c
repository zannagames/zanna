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
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)ceill(value);
}

/// @brief Linearly interpolate the x value of a line segment at scanline @p y.
/// @details Used by triangle/polygon scan-conversion to find the left/right
///          edge x at a given y. Long-double arithmetic keeps precision when
///          int64 endpoints are widely separated; the result floors to int64
///          with saturation. Degenerate horizontal segments (y1 == y0) return x0.
/// @param x0 First endpoint X.
/// @param y0 First endpoint Y.
/// @param x1 Second endpoint X.
/// @param y1 Second endpoint Y.
/// @param y Scanline to sample.
/// @return Interpolated x at scanline @p y, saturated to int64.
static int64_t rt_canvas_adv_interp_x(int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t y) {
    if (y1 == y0)
        return x0;
    long double t = ((long double)y - (long double)y0) / ((long double)y1 - (long double)y0);
    long double x = (long double)x0 + ((long double)x1 - (long double)x0) * t;
    return rt_canvas_adv_floor_ld_to_i64_sat(x);
}

/// @brief Convert a Zanna packed color to a 24-bit ZannaGFX RGB value.
/// @details Normalizes through rt_pixels_color_to_rgba (0xRRGGBBAA) then drops
///          the alpha byte, since the advanced canvas primitives draw opaque.
/// @param color Runtime RGB, tagged ARGB, or raw RGBA color.
/// @return Opaque backend `0xRRGGBB` value.
static vgfx_color_t rt_canvas_adv_color_to_vgfx_rgb(int64_t color) {
    return (vgfx_color_t)((rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu);
}

/// @brief Squared distance between two points in long double (no sqrt, no
///        overflow) — used only to compare relative edge lengths.
/// @param x1 First point X.
/// @param y1 First point Y.
/// @param x2 Second point X.
/// @param y2 Second point Y.
/// @return Squared Euclidean distance in long-double precision.
static long double rt_canvas_adv_dist2_ld(int64_t x1, int64_t y1, int64_t x2, int64_t y2) {
    long double dx = (long double)x2 - (long double)x1;
    long double dy = (long double)y2 - (long double)y1;
    return dx * dx + dy * dy;
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
    long double d12 = rt_canvas_adv_dist2_ld(x1, y1, x2, y2);
    long double d23 = rt_canvas_adv_dist2_ld(x2, y2, x3, y3);
    long double d31 = rt_canvas_adv_dist2_ld(x3, y3, x1, y1);
    if (d12 >= d23 && d12 >= d31) {
        rt_canvas_line(canvas_ptr, x1, y1, x2, y2, color);
    } else if (d23 >= d31) {
        rt_canvas_line(canvas_ptr, x2, y2, x3, y3, color);
    } else {
        rt_canvas_line(canvas_ptr, x3, y3, x1, y1, color);
    }
}

/// @brief Plot the two clip-respecting points of one octant of a circle, mirrored into one of four
/// corners.
/// @details For a Bresenham circle stepper at offset (x, y) inside the
///          first octant (x >= y >= 0), this writes the two pixels that
///          fall in the requested @p corner — the (x, y) and (y, x)
///          reflections within that quadrant. Used by round_box / round_frame
///          to draw only the pixels that belong to a specific corner.
/// @param canvas Canvas to draw into. NULL → no-op.
/// @param cx Circle center X in logical coordinates.
/// @param cy Circle center Y in logical coordinates.
/// @param x Bresenham octant X offset.
/// @param y Bresenham octant Y offset.
/// @param corner One of RTG_CORNER_TOP_LEFT / TOP_RIGHT / BOTTOM_LEFT / BOTTOM_RIGHT.
/// @param color  Pixel color (0xAARRGGBB packed).
static void rt_canvas_plot_quarter_circle(rt_canvas *canvas,
                                          int64_t cx,
                                          int64_t cy,
                                          int64_t x,
                                          int64_t y,
                                          int corner,
                                          vgfx_color_t color) {
    if (!canvas || !canvas->gfx_win)
        return;

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h);
#define RT_CANVAS_PLOT_CLIPPED(px, py)                                                             \
    do {                                                                                           \
        int64_t qx = (px);                                                                         \
        int64_t qy = (py);                                                                         \
        if (qx >= clip_x && qx < clip_x1 && qy >= clip_y && qy < clip_y1 &&                        \
            rtg_i64_fits_i32(qx) && rtg_i64_fits_i32(qy))                                          \
            vgfx_pset(canvas->gfx_win, (int32_t)qx, (int32_t)qy, color);                           \
    } while (0)

    switch (corner) {
        case RTG_CORNER_TOP_LEFT:
            RT_CANVAS_PLOT_CLIPPED(rtg_add_sat64(cx, -x), rtg_add_sat64(cy, -y));
            RT_CANVAS_PLOT_CLIPPED(rtg_add_sat64(cx, -y), rtg_add_sat64(cy, -x));
            break;
        case RTG_CORNER_TOP_RIGHT:
            RT_CANVAS_PLOT_CLIPPED(rtg_add_sat64(cx, x), rtg_add_sat64(cy, -y));
            RT_CANVAS_PLOT_CLIPPED(rtg_add_sat64(cx, y), rtg_add_sat64(cy, -x));
            break;
        case RTG_CORNER_BOTTOM_LEFT:
            RT_CANVAS_PLOT_CLIPPED(rtg_add_sat64(cx, -x), rtg_add_sat64(cy, y));
            RT_CANVAS_PLOT_CLIPPED(rtg_add_sat64(cx, -y), rtg_add_sat64(cy, x));
            break;
        case RTG_CORNER_BOTTOM_RIGHT:
            RT_CANVAS_PLOT_CLIPPED(rtg_add_sat64(cx, x), rtg_add_sat64(cy, y));
            RT_CANVAS_PLOT_CLIPPED(rtg_add_sat64(cx, y), rtg_add_sat64(cy, x));
            break;
    }
#undef RT_CANVAS_PLOT_CLIPPED
}

/// @brief Bresenham-step a circle of @p radius and emit only the pixels in the chosen @p corner.
/// @details Standard mid-point circle rasterizer (8-way symmetry collapsed to
///          one corner via rt_canvas_plot_quarter_circle). Used by round_box
///          and round_frame for the four corner arcs. radius == 0 plots a
///          single pixel at the center; negative radii are no-ops.
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

    if (radius == 0) {
        rt_canvas_plot(canvas, cx, cy, (int64_t)color);
        return;
    }

    int64_t x = radius;
    int64_t y = 0;
    int64_t err = 1 - radius;
    while (x >= y) {
        rt_canvas_plot_quarter_circle(canvas, cx, cy, x, y, corner, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
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

    for (int64_t dy = -radius; dy < 0; dy++) {
        long double rem = r2 - (long double)dy * (long double)dy;
        int64_t span = rem <= 0.0L ? 0 : rt_canvas_adv_floor_ld_to_i64_sat(sqrtl(rem));
        int64_t row = rtg_add_sat64(cy_top, dy);
        rt_canvas_fill_hspan(
            canvas, rtg_add_sat64(cx_left, -span), rtg_add_sat64(cx_right, span), row, color);
    }
    for (int64_t dy = 1; dy <= radius; dy++) {
        long double rem = r2 - (long double)dy * (long double)dy;
        int64_t span = rem <= 0.0L ? 0 : rt_canvas_adv_floor_ld_to_i64_sat(sqrtl(rem));
        int64_t row = rtg_add_sat64(cy_bottom, dy);
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

/// @brief Flood-fill a 4-connected region of identical RGBA framebuffer pixels.
/// @details The logical starting point and active clip are converted to physical
///          HiDPI coordinates. A dynamically growing stack traverses only
///          pixels matching the starting pixel exactly. Filling with the same
///          RGBA is a no-op. Initial allocation failure changes nothing;
///          growth failure can leave a partially filled region.
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
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb))
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

    // Bounds check (physical, constrained to the active clip)
    if (start_x < clip_px0 || start_x >= clip_px1 || start_y < clip_py0 || start_y >= clip_py1 ||
        start_x < 0 || start_x >= fb.width || start_y < 0 || start_y >= fb.height)
        return;

    // Get the target color (color to replace)
    uint8_t *start_pixel = &fb.pixels[start_y * fb.stride + start_x * 4];
    uint32_t target_r = start_pixel[0];
    uint32_t target_g = start_pixel[1];
    uint32_t target_b = start_pixel[2];
    uint32_t target_a = start_pixel[3];

    // Get fill color components
    uint32_t fill_rgba = rt_pixels_color_to_rgba(color);
    uint8_t fill_r = (uint8_t)((fill_rgba >> 24) & 0xFFu);
    uint8_t fill_g = (uint8_t)((fill_rgba >> 16) & 0xFFu);
    uint8_t fill_b = (uint8_t)((fill_rgba >> 8) & 0xFFu);
    uint8_t fill_a = (uint8_t)(fill_rgba & 0xFFu);

    // Don't fill if target color is the same as fill color
    if (target_r == fill_r && target_g == fill_g && target_b == fill_b && target_a == fill_a)
        return;

    /* O-03: Use a dynamically-growing stack starting at 4096 entries
     * instead of pre-allocating the worst-case (width * height) upfront.
     * This avoids O(r^2) allocations for small fill regions. */
    typedef struct {
        int64_t x;
        int64_t y;
    } flood_fill_point;

    int64_t stack_cap = 4096;
    flood_fill_point *stack = (flood_fill_point *)malloc((size_t)stack_cap * sizeof(*stack));
    if (!stack)
        return;

    int64_t stack_top = 0;
    stack[stack_top].x = start_x;
    stack[stack_top].y = start_y;
    stack_top++;

    while (stack_top > 0) {
        stack_top--;
        int64_t x = stack[stack_top].x;
        int64_t y = stack[stack_top].y;

        // Skip if out of bounds
        if (x < clip_px0 || x >= clip_px1 || y < clip_py0 || y >= clip_py1 || x < 0 ||
            x >= fb.width || y < 0 || y >= fb.height)
            continue;

        uint8_t *pixel = &fb.pixels[y * fb.stride + x * 4];

        // Skip if not target color
        if (pixel[0] != target_r || pixel[1] != target_g || pixel[2] != target_b ||
            pixel[3] != target_a)
            continue;

        // Fill this pixel
        pixel[0] = fill_r;
        pixel[1] = fill_g;
        pixel[2] = fill_b;
        pixel[3] = fill_a;

        // Grow stack if needed before pushing 4 neighbors
        if (stack_top > stack_cap - 4) {
            if (stack_top > INT64_MAX - 4)
                break;
            int64_t required = stack_top + 4;
            int64_t new_cap = stack_cap;
            while (new_cap < required) {
                if (new_cap > INT64_MAX / 2) {
                    new_cap = required;
                    break;
                }
                new_cap *= 2;
            }
            if (new_cap > INT64_MAX / (int64_t)sizeof(*stack))
                break;
            flood_fill_point *grown =
                (flood_fill_point *)realloc(stack, (size_t)new_cap * sizeof(*stack));
            if (!grown) {
                free(stack);
                return;
            }
            stack = grown;
            stack_cap = new_cap;
        }

        // Push neighbors (4-connected)
        stack[stack_top].x = x + 1;
        stack[stack_top].y = y;
        stack_top++;
        stack[stack_top].x = x - 1;
        stack[stack_top].y = y;
        stack_top++;
        stack[stack_top].x = x;
        stack[stack_top].y = y + 1;
        stack_top++;
        stack[stack_top].x = x;
        stack[stack_top].y = y - 1;
        stack_top++;
    }

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

    long double area = ((long double)x2 - (long double)x1) * ((long double)y3 - (long double)y1) -
                       ((long double)y2 - (long double)y1) * ((long double)x3 - (long double)x1);
    if (area == 0.0L) {
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
    int64_t steps = (int64_t)(approx_len / 4.0L);
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

        // Sort intersections
        for (int64_t i = 0; i < num_intersections - 1; i++) {
            for (int64_t j = i + 1; j < num_intersections; j++) {
                if (intersections[j] < intersections[i]) {
                    int64_t tmp = intersections[i];
                    intersections[i] = intersections[j];
                    intersections[j] = tmp;
                }
            }
        }

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
    if (points_out)
        *points_out = NULL;
    if (count_out)
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
    if (points_out)
        *points_out = points;
    if (count_out)
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

    // Precompute gradient colours for each column, then blit each row of height h
    // with a single memcpy-equivalent pass — avoids w*vgfx_line() call overhead.
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb)) {
        // Fallback: per-column vgfx_line for mock/headless contexts.
        // vgfx_line auto-scales via coord_scale, so pass logical coords.
        int64_t w_minus1 = orig_w > 1 ? orig_w - 1 : 1;
        for (int64_t col = 0; col < w; col++) {
            int64_t logical_x = rtg_add_sat64(x, col);
            int64_t gradient_col = rtg_add_sat64(logical_x, -orig_x);
            int64_t color = rt_color_lerp(c1, c2, rtg_mul_sat64(gradient_col, 100) / w_minus1);
            vgfx_line(canvas->gfx_win,
                      rtg_clamp_i64_to_i32(logical_x),
                      rtg_clamp_i64_to_i32(y),
                      rtg_clamp_i64_to_i32(logical_x),
                      rtg_clamp_i64_to_i32(rtg_add_sat64(y, h - 1)),
                      rt_canvas_adv_color_to_vgfx_rgb(color));
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

    int64_t w_minus1 = orig_w > 1 ? orig_w - 1 : 1;
    /* Interpolate channels directly (full 256-level precision) instead of routing
     * through rt_color_lerp's 0..100 percent parameter, which quantizes a wide
     * gradient to at most 101 distinct colors and shows visible banding. */
    uint32_t rgba1 = rt_pixels_color_to_rgba(c1);
    uint32_t rgba2 = rt_pixels_color_to_rgba(c2);
    int64_t r1 = (int64_t)((rgba1 >> 24) & 0xFF), g1 = (int64_t)((rgba1 >> 16) & 0xFF),
            b1 = (int64_t)((rgba1 >> 8) & 0xFF), a1 = (int64_t)(rgba1 & 0xFF);
    int64_t r2 = (int64_t)((rgba2 >> 24) & 0xFF), g2 = (int64_t)((rgba2 >> 16) & 0xFF),
            b2 = (int64_t)((rgba2 >> 8) & 0xFF), a2 = (int64_t)(rgba2 & 0xFF);
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

        int64_t gc = rtg_add_sat64(logical_x, -orig_x);
        if (gc < 0)
            gc = 0;
        if (gc > w_minus1)
            gc = w_minus1;
        uint8_t cr = (uint8_t)(r1 + (r2 - r1) * gc / w_minus1);
        uint8_t cg = (uint8_t)(g1 + (g2 - g1) * gc / w_minus1);
        uint8_t cb = (uint8_t)(b1 + (b2 - b1) * gc / w_minus1);
        uint8_t ca = (uint8_t)(a1 + (a2 - a1) * gc / w_minus1);
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

    // Write each row directly into the framebuffer — one colour per row, no per-row
    // vgfx_line() overhead.
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb)) {
        // Fallback: vgfx_line auto-scales via coord_scale, so pass logical coords.
        int64_t h_minus1 = orig_h > 1 ? orig_h - 1 : 1;
        for (int64_t row = 0; row < h; row++) {
            int64_t logical_y = rtg_add_sat64(y, row);
            int64_t gradient_row = rtg_add_sat64(logical_y, -orig_y);
            int64_t color = rt_color_lerp(c1, c2, rtg_mul_sat64(gradient_row, 100) / h_minus1);
            vgfx_line(canvas->gfx_win,
                      rtg_clamp_i64_to_i32(x),
                      rtg_clamp_i64_to_i32(logical_y),
                      rtg_clamp_i64_to_i32(rtg_add_sat64(x, w - 1)),
                      rtg_clamp_i64_to_i32(logical_y),
                      rt_canvas_adv_color_to_vgfx_rgb(color));
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
    int64_t h_minus1 = orig_h > 1 ? orig_h - 1 : 1;

    /* Each logical row is a single solid colour (the gradient runs down the rows), so build one
     * row of `draw_w` pixels and memcpy it across that row's scale-expanded scanlines instead of
     * a per-pixel inner loop — mirrors rt_canvas_gradient_h. draw_w is framebuffer-bounded. */
    if ((uint64_t)draw_w > SIZE_MAX / 4u) /* overflow guard, matching gradient_h */
        return;
    uint8_t *row_buf = (uint8_t *)malloc((size_t)draw_w * 4u);
    if (!row_buf)
        return;
    /* Full-precision per-channel interpolation (avoids rt_color_lerp's 101-step banding). */
    uint32_t rgba1 = rt_pixels_color_to_rgba(c1);
    uint32_t rgba2 = rt_pixels_color_to_rgba(c2);
    int64_t r1 = (int64_t)((rgba1 >> 24) & 0xFF), g1 = (int64_t)((rgba1 >> 16) & 0xFF),
            b1 = (int64_t)((rgba1 >> 8) & 0xFF), a1 = (int64_t)(rgba1 & 0xFF);
    int64_t r2 = (int64_t)((rgba2 >> 24) & 0xFF), g2 = (int64_t)((rgba2 >> 16) & 0xFF),
            b2 = (int64_t)((rgba2 >> 8) & 0xFF), a2 = (int64_t)(rgba2 & 0xFF);
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

        int64_t gr = rtg_add_sat64(logical_y, -orig_y);
        if (gr < 0)
            gr = 0;
        if (gr > h_minus1)
            gr = h_minus1;
        uint8_t cr = (uint8_t)(r1 + (r2 - r1) * gr / h_minus1);
        uint8_t cg = (uint8_t)(g1 + (g2 - g1) * gr / h_minus1);
        uint8_t cb = (uint8_t)(b1 + (b2 - b1) * gr / h_minus1);
        uint8_t ca = (uint8_t)(a1 + (a2 - a1) * gr / h_minus1);
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
