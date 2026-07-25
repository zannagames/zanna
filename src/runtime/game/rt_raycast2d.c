//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_raycast2d.c
/// @file
/// @brief Implements segment/shape intersection and tile-grid DDA raycasting.
//
// Purpose: 2D raycasting — DDA tilemap traversal, line-segment intersection.
//
// Key invariants:
//   - DDA steps through tiles one at a time along the ray.
//   - Line-rect uses Liang-Barsky axis-aligned clipping.
//   - All positions are in pixel coordinates.
//
//===----------------------------------------------------------------------===//

#include "rt_raycast2d.h"
#include "rt_tilemap.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

//=============================================================================
// Line-Rect intersection (Liang-Barsky)
//=============================================================================

/// @brief Test whether a line segment intersects an axis-aligned rectangle (Liang-Barsky).
/// @param x1 Segment start X.
/// @param y1 Segment start Y.
/// @param x2 Segment end X.
/// @param y2 Segment end Y.
/// @param rx Rectangle top-left X.
/// @param ry Rectangle top-left Y.
/// @param rw Nonnegative rectangle width.
/// @param rh Nonnegative rectangle height.
/// @return `1` when the closed segment intersects or touches the rectangle,
///         otherwise `0`.
/// @details Non-finite inputs and negative dimensions are rejected. A
///          zero-length segment is treated as a point test; zero-size
///          rectangles are permitted.
int8_t rt_collision_line_rect(
    double x1, double y1, double x2, double y2, double rx, double ry, double rw, double rh) {
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) || !isfinite(rx) ||
        !isfinite(ry) || !isfinite(rw) || !isfinite(rh) || rw < 0.0 || rh < 0.0)
        return 0;
    double dx = x2 - x1;
    double dy = y2 - y1;
    double p[4] = {-dx, dx, -dy, dy};
    double q[4] = {x1 - rx, rx + rw - x1, y1 - ry, ry + rh - y1};

    double tmin = 0.0, tmax = 1.0;
    for (int i = 0; i < 4; i++) {
        if (fabs(p[i]) < 1e-12) {
            if (q[i] < 0.0)
                return 0;
        } else {
            double t = q[i] / p[i];
            if (p[i] < 0.0) {
                if (t > tmin)
                    tmin = t;
            } else {
                if (t < tmax)
                    tmax = t;
            }
            if (tmin > tmax)
                return 0;
        }
    }
    return 1;
}

//=============================================================================
// Line-Circle intersection
//=============================================================================

/// @brief Test whether a line segment intersects a circle (quadratic formula).
/// @param x1 Segment start X.
/// @param y1 Segment start Y.
/// @param x2 Segment end X.
/// @param y2 Segment end Y.
/// @param cx Circle center X.
/// @param cy Circle center Y.
/// @param r Nonnegative radius.
/// @return `1` when the segment intersects, touches, or lies within the circle,
///         otherwise `0`.
/// @details Non-finite inputs and negative radius are rejected. A zero-length
///          segment becomes a closed point-in-circle test.
int8_t rt_collision_line_circle(
    double x1, double y1, double x2, double y2, double cx, double cy, double r) {
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) || !isfinite(cx) ||
        !isfinite(cy) || !isfinite(r) || r < 0.0)
        return 0;
    double dx = x2 - x1;
    double dy = y2 - y1;
    double fx = x1 - cx;
    double fy = y1 - cy;
    double a = dx * dx + dy * dy;
    if (a < 1e-24)
        return (fx * fx + fy * fy) <= r * r;
    double b = 2.0 * (fx * dx + fy * dy);
    double c = fx * fx + fy * fy - r * r;
    double disc = b * b - 4.0 * a * c;
    if (disc < 0.0)
        return 0;
    disc = sqrt(disc);
    double t1 = (-b - disc) / (2.0 * a);
    double t2 = (-b + disc) / (2.0 * a);
    return (t1 >= 0.0 && t1 <= 1.0) || (t2 >= 0.0 && t2 <= 1.0) || (t1 < 0.0 && t2 > 1.0);
}

//=============================================================================
// Tilemap Raycast (DDA)
//=============================================================================

/// @brief Compute mathematical floor division for signed integers.
/// @param n Dividend.
/// @param d Nonzero divisor.
/// @return `floor(n / d)`, correcting C's truncation toward zero.
static int64_t floor_div_i64(int64_t n, int64_t d) {
    int64_t q = n / d;
    int64_t r = n % d;
    if (r != 0 && ((r < 0) != (d < 0)))
        q--;
    return q;
}

/// @brief Convert a long double to int64 with endpoint saturation.
/// @param value Candidate value.
/// @return Truncated integer, nearest endpoint when outside int64 range, or
///         zero when conversion to double reports non-finite.
static int64_t clamp_ld_to_i64(long double value) {
    if (!isfinite((double)value))
        return 0;
    if (value > (long double)INT64_MAX)
        return INT64_MAX;
    if (value < (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)value;
}

/// @brief Liang–Barsky per-boundary clip test: narrows the parametric ray
///        interval [t0, t1] for one axis/edge.
/// @param p Signed direction coefficient for the boundary.
/// @param q Signed start-to-boundary distance.
/// @param t0 In/out lower parametric bound.
/// @param t1 In/out upper parametric bound.
/// @return `0` when wholly outside that boundary, otherwise `1` with the
///         interval tightened as needed.
static int8_t clip_axis(long double p, long double q, long double *t0, long double *t1) {
    if (fabsl(p) < 1e-18L)
        return q >= 0.0L;
    long double r = q / p;
    if (p < 0.0L) {
        if (r > *t1)
            return 0;
        if (r > *t0)
            *t0 = r;
    } else {
        if (r < *t0)
            return 0;
        if (r < *t1)
            *t1 = r;
    }
    return 1;
}

/// @brief Test one tile cell along a ray for a solid hit and, if so, record
///        the clamped entry point.
/// @param tilemap Tilemap queried for solidity.
/// @param tile_x Candidate tile X.
/// @param tile_y Candidate tile Y.
/// @param mw_tiles Map width in tiles.
/// @param mh_tiles Map height in tiles.
/// @param tw Tile width in pixels.
/// @param th Tile height in pixels.
/// @param x1 Original segment start X.
/// @param y1 Original segment start Y.
/// @param full_dx Original segment X delta.
/// @param full_dy Original segment Y delta.
/// @param current_t Parametric entry time for the candidate cell.
/// @param hit_x Optional hit-X output.
/// @param hit_y Optional hit-Y output.
/// @return `1` for an in-range solid tile, otherwise `0`.
/// @details Hit coordinates are saturated then clamped to the tile's inclusive
///          pixel bounds.
static int8_t raycast_check_tile(void *tilemap,
                                 int64_t tile_x,
                                 int64_t tile_y,
                                 int64_t mw_tiles,
                                 int64_t mh_tiles,
                                 int64_t tw,
                                 int64_t th,
                                 int64_t x1,
                                 int64_t y1,
                                 long double full_dx,
                                 long double full_dy,
                                 long double current_t,
                                 int64_t *hit_x,
                                 int64_t *hit_y) {
    if (tile_x < 0 || tile_x >= mw_tiles || tile_y < 0 || tile_y >= mh_tiles)
        return 0;
    int64_t sample_x = tile_x * tw;
    int64_t sample_y = tile_y * th;
    if (!rt_tilemap_is_solid_at(tilemap, sample_x, sample_y))
        return 0;

    int64_t hx = clamp_ld_to_i64((long double)x1 + full_dx * current_t);
    int64_t hy = clamp_ld_to_i64((long double)y1 + full_dy * current_t);
    int64_t min_x = tile_x * tw;
    int64_t min_y = tile_y * th;
    int64_t max_x = min_x + tw - 1;
    int64_t max_y = min_y + th - 1;
    if (hx < min_x)
        hx = min_x;
    if (hx > max_x)
        hx = max_x;
    if (hy < min_y)
        hy = min_y;
    if (hy > max_y)
        hy = max_y;
    if (hit_x)
        *hit_x = hx;
    if (hit_y)
        *hit_y = hy;
    return 1;
}

/// @brief Cast a ray through a tilemap and return the first solid tile hit.
/// @param tilemap Tilemap whose configured solidity is queried.
/// @param x1 Segment start X in pixels.
/// @param y1 Segment start Y in pixels.
/// @param x2 Segment end X in pixels.
/// @param y2 Segment end Y in pixels.
/// @param hit_x Optional destination for first-hit X.
/// @param hit_y Optional destination for first-hit Y.
/// @details Clips the segment to the finite map bounds, then uses tile DDA so
///          long rays traverse touched tiles rather than a capped pixel sample.
///          The map extent is half-open. Exact corner crossings test both
///          side-adjacent tiles before entering the diagonal tile. Output
///          pointers are written only on a hit.
/// @return `1` if a solid tile was hit, or `0` for a clear segment, null map,
///         invalid/overflowing map dimensions, or a segment missing the map.
int8_t rt_raycast_tilemap(
    void *tilemap, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t *hit_x, int64_t *hit_y) {
    if (!tilemap)
        return 0;

    int64_t tw = rt_tilemap_get_tile_width(tilemap);
    int64_t th = rt_tilemap_get_tile_height(tilemap);
    if (tw <= 0 || th <= 0)
        return 0;
    int64_t mw_tiles = rt_tilemap_get_width(tilemap);
    int64_t mh_tiles = rt_tilemap_get_height(tilemap);
    if (mw_tiles <= 0 || mh_tiles <= 0 || mw_tiles > INT64_MAX / tw || mh_tiles > INT64_MAX / th)
        return 0;

    int64_t map_w = mw_tiles * tw;
    int64_t map_h = mh_tiles * th;
    long double dx = (long double)x2 - (long double)x1;
    long double dy = (long double)y2 - (long double)y1;
    long double t0 = 0.0L;
    long double t1 = 1.0L;
    if (!clip_axis(-dx, (long double)x1, &t0, &t1) ||
        !clip_axis(dx, (long double)map_w - (long double)x1, &t0, &t1) ||
        !clip_axis(-dy, (long double)y1, &t0, &t1) ||
        !clip_axis(dy, (long double)map_h - (long double)y1, &t0, &t1))
        return 0;

    long double sx_ld = (long double)x1 + dx * t0;
    long double sy_ld = (long double)y1 + dy * t0;
    long double ex_ld = (long double)x1 + dx * t1;
    long double ey_ld = (long double)y1 + dy * t1;

    // The tile grid occupies the half-open extent [0, map_w) x [0, map_h). The
    // Liang-Barsky clip above admits the maximum boundary (x == map_w / y ==
    // map_h); a clipped segment lying entirely on that excluded edge is tangent
    // to the outside and crosses no in-bounds tile, so it is clear. Reject it
    // before the clamp below folds the boundary onto the last in-bounds tile,
    // which made a boundary ray spuriously collide (VDOC-242).
    if ((sx_ld >= (long double)map_w && ex_ld >= (long double)map_w) ||
        (sy_ld >= (long double)map_h && ey_ld >= (long double)map_h))
        return 0;

    if (sx_ld >= (long double)map_w)
        sx_ld = (long double)map_w - 1.0L;
    if (sy_ld >= (long double)map_h)
        sy_ld = (long double)map_h - 1.0L;
    if (ex_ld >= (long double)map_w)
        ex_ld = (long double)map_w - 1.0L;
    if (ey_ld >= (long double)map_h)
        ey_ld = (long double)map_h - 1.0L;
    if (sx_ld < 0.0L)
        sx_ld = 0.0L;
    if (sy_ld < 0.0L)
        sy_ld = 0.0L;
    if (ex_ld < 0.0L)
        ex_ld = 0.0L;
    if (ey_ld < 0.0L)
        ey_ld = 0.0L;

    int64_t sx = clamp_ld_to_i64(sx_ld);
    int64_t sy = clamp_ld_to_i64(sy_ld);
    int64_t ex = clamp_ld_to_i64(ex_ld);
    int64_t ey = clamp_ld_to_i64(ey_ld);
    int64_t tile_x = floor_div_i64(sx, tw);
    int64_t tile_y = floor_div_i64(sy, th);
    int64_t end_tx = floor_div_i64(ex, tw);
    int64_t end_ty = floor_div_i64(ey, th);

    long double full_dx = (long double)x2 - (long double)x1;
    long double full_dy = (long double)y2 - (long double)y1;
    int step_x = full_dx > 0.0L ? 1 : (full_dx < 0.0L ? -1 : 0);
    int step_y = full_dy > 0.0L ? 1 : (full_dy < 0.0L ? -1 : 0);
    long double t_max_x = INFINITY;
    long double t_max_y = INFINITY;
    long double t_delta_x = INFINITY;
    long double t_delta_y = INFINITY;
    if (step_x != 0) {
        long double next_x = step_x > 0 ? (long double)(tile_x + 1) * (long double)tw
                                        : (long double)tile_x * (long double)tw;
        t_max_x = ((next_x - (long double)x1) / full_dx);
        t_delta_x = fabsl((long double)tw / full_dx);
    }
    if (step_y != 0) {
        long double next_y = step_y > 0 ? (long double)(tile_y + 1) * (long double)th
                                        : (long double)tile_y * (long double)th;
        t_max_y = ((next_y - (long double)y1) / full_dy);
        t_delta_y = fabsl((long double)th / full_dy);
    }

    long double current_t = t0;
    while (tile_x >= 0 && tile_x < mw_tiles && tile_y >= 0 && tile_y < mh_tiles) {
        if (raycast_check_tile(tilemap,
                               tile_x,
                               tile_y,
                               mw_tiles,
                               mh_tiles,
                               tw,
                               th,
                               x1,
                               y1,
                               full_dx,
                               full_dy,
                               current_t,
                               hit_x,
                               hit_y)) {
            return 1;
        }
        if (tile_x == end_tx && tile_y == end_ty)
            break;
        if (step_x != 0 && step_y != 0 && fabsl(t_max_x - t_max_y) <= 1e-18L) {
            long double corner_t = t_max_x;
            if (corner_t > t1 + 1e-12L)
                break;
            if (raycast_check_tile(tilemap,
                                   tile_x + step_x,
                                   tile_y,
                                   mw_tiles,
                                   mh_tiles,
                                   tw,
                                   th,
                                   x1,
                                   y1,
                                   full_dx,
                                   full_dy,
                                   corner_t,
                                   hit_x,
                                   hit_y) ||
                raycast_check_tile(tilemap,
                                   tile_x,
                                   tile_y + step_y,
                                   mw_tiles,
                                   mh_tiles,
                                   tw,
                                   th,
                                   x1,
                                   y1,
                                   full_dx,
                                   full_dy,
                                   corner_t,
                                   hit_x,
                                   hit_y)) {
                return 1;
            }
            tile_x += step_x;
            tile_y += step_y;
            current_t = corner_t;
            t_max_x += t_delta_x;
            t_max_y += t_delta_y;
        } else if (t_max_x < t_max_y) {
            tile_x += step_x;
            current_t = t_max_x;
            t_max_x += t_delta_x;
        } else {
            tile_y += step_y;
            current_t = t_max_y;
            t_max_y += t_delta_y;
        }
        if (current_t > t1 + 1e-12L)
            break;
    }
    return 0;
}

/// @brief Check whether two pixel positions have no solid tile between them.
/// @param tilemap Tilemap to traverse.
/// @param x1 Segment start X.
/// @param y1 Segment start Y.
/// @param x2 Segment end X.
/// @param y2 Segment end Y.
/// @return Logical inverse of rt_raycast_tilemap(): `1` when no hit is
///         reported, otherwise `0`.
/// @details Because all raycast failure/no-hit cases return zero, a null or
///          invalid-dimension tilemap is reported as unobstructed.
int8_t rt_has_line_of_sight(void *tilemap, int64_t x1, int64_t y1, int64_t x2, int64_t y2) {
    return !rt_raycast_tilemap(tilemap, x1, y1, x2, y2, NULL, NULL);
}
