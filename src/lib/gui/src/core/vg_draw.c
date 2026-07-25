//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/core/vg_draw.c
// Purpose: Implementation of the shared anti-aliased 2D drawing core. Coverage
//          is computed in Q8 (1/256 px) fixed point with an integer square
//          root, so rendered edges are bit-identical on every platform.
// Key invariants:
//   - DETERMINISM CONTRACT: the per-pixel coverage loops use ONLY integer math.
//     Floating point appears solely in vg__q8()/bbox setup (a deterministic
//     round-to-nearest of caller geometry). Do NOT introduce float, double,
//     fmaf, or -ffast-math-sensitive idioms into the inner loops — that would
//     silently diverge across FPUs and break the golden pixel-hash tests.
//   - Opaque interiors via vgfx_fill_rect; anti-aliased rims via the supplied
//     coverage as the alpha byte to vgfx_pset_alpha (source-over).
//   - vgfx_pset_alpha already honours the active clip and window bounds, so
//     out-of-range writes are safe and need no extra clamping here.
// Ownership/Lifetime:
//   - Stateless. No allocation, no globals.
// Links: lib/gui/include/vg_draw.h, lib/graphics/include/vgfx.h
//
//===----------------------------------------------------------------------===//

#include "vg_draw.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements deterministic fixed-point anti-aliased GUI drawing primitives.
/// @details Floating-point geometry is quantized during setup; coverage loops use Q8 integer
/// arithmetic and an integer square root. Opaque interiors use bulk fills, edge coverage uses
/// source-over alpha pixels, and rounded shadows use a small single-threaded LRU mask cache.

//=============================================================================
// Fixed-point helpers (Q8: 1 pixel == 256 units)
//=============================================================================

/// @brief Round a pixel coordinate to Q8 fixed point (deterministic).
/// @param v Floating-point pixel coordinate; non-finite values become zero.
/// @return Saturated signed Q8 representation of @p v.
static inline int32_t vg__q8(float v) {
    if (!isfinite(v))
        return 0;
    const float max_q8 = (float)INT32_MAX / 256.0f;
    const float min_q8 = (float)INT32_MIN / 256.0f;
    if (v >= max_q8)
        return INT32_MAX;
    if (v <= min_q8)
        return INT32_MIN;
    // Round half away from zero without depending on <math.h>.
    return (int32_t)(v * 256.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

/// @brief Return the Q8 coordinate of an integer pixel center without int overflow.
/// @param pixel Integer pixel coordinate.
/// @return Q8 coordinate for the center of @p pixel.
static inline int64_t vg__pixel_center_q8(int pixel) {
    return (int64_t)pixel * 256 + 128;
}

/// @brief Floor of a float to int (for bounding-box lower edges).
/// @param v Finite value within the representable integer range.
/// @return Greatest integer not larger than @p v.
static inline int vg__floor_i(float v) {
    int i = (int)v;
    return (v < (float)i) ? i - 1 : i;
}

/// @brief Ceil of a float to int (for bounding-box upper edges).
/// @param v Finite value within the representable integer range.
/// @return Smallest integer not less than @p v.
static inline int vg__ceil_i(float v) {
    int i = (int)v;
    return (v > (float)i) ? i + 1 : i;
}

/// @brief Integer square root of a 64-bit value (floor). Fully deterministic.
/// @param n Unsigned radicand.
/// @return Greatest integer whose square does not exceed @p n.
static inline uint32_t vg__isqrt64(uint64_t n) {
    uint64_t x = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > n)
        bit >>= 2;
    while (bit != 0) {
        if (n >= x + bit) {
            n -= x + bit;
            x = (x >> 1) + bit;
        } else {
            x >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)x;
}

/// @brief Convert a Q8 coverage value to an 8-bit alpha (0..255).
/// @param cov_q8 Coverage in Q8 (>=256 fully covered, <=0 empty).
/// @return Clamped alpha value from zero through 255.
static inline int32_t vg__cov_to_alpha(int32_t cov_q8) {
    if (cov_q8 <= 0)
        return 0;
    if (cov_q8 >= 256)
        return 255;
    return (cov_q8 * 255) >> 8;
}

//=============================================================================
// Coverage rasterisers (the integer inner loops)
//=============================================================================

/// @brief Composite an anti-aliased filled disc, restricted to an integer box.
/// @details Coverage = (r + 0.5) - distance, a 1px linear edge ramp. Used both
///          for whole discs and for rounded-rect corner quadrants (the box
///          confines the work to the corner square so it never overdraws the
///          opaque interior bars).
/// @param win Target graphics window.
/// @param cxq Disc-center x coordinate in Q8 units.
/// @param cyq Disc-center y coordinate in Q8 units.
/// @param rq Radius in Q8 units.
/// @param bx0 Inclusive integer box left edge.
/// @param by0 Inclusive integer box top edge.
/// @param bx1 Exclusive integer box right edge.
/// @param by1 Exclusive integer box bottom edge.
/// @param base Source RGB color without alpha.
static void vg__disc_box(vgfx_window_t win,
                         int32_t cxq,
                         int32_t cyq,
                         int32_t rq,
                         int bx0,
                         int by0,
                         int bx1,
                         int by1,
                         uint32_t base) {
    int32_t edge = rq + 128; // r + 0.5px in Q8
    for (int py = by0; py < by1; ++py) {
        int64_t ddy = vg__pixel_center_q8(py) - cyq;
        int64_t ddy2 = ddy * ddy;
        for (int px = bx0; px < bx1; ++px) {
            int64_t ddx = vg__pixel_center_q8(px) - cxq;
            uint64_t d2 = (uint64_t)(ddx * ddx + ddy2);
            int32_t distq = (int32_t)vg__isqrt64(d2);
            int32_t alpha = vg__cov_to_alpha(edge - distq);
            if (alpha != 0)
                vgfx_pset_alpha(win, px, py, ((uint32_t)alpha << 24) | base);
        }
    }
}

/// @brief Composite an anti-aliased ring (annulus) of width t, in an int box.
/// @details Coverage peaks on the mid-radius and feathers over the stroke
///          width plus a 1px ramp, so corner strokes line up with disc fills.
/// @param win Target graphics window.
/// @param cxq Ring-center x coordinate in Q8 units.
/// @param cyq Ring-center y coordinate in Q8 units.
/// @param rmidq Midline radius in Q8 units.
/// @param halfq Half of the stroke width in Q8 units.
/// @param bx0 Inclusive integer box left edge.
/// @param by0 Inclusive integer box top edge.
/// @param bx1 Exclusive integer box right edge.
/// @param by1 Exclusive integer box bottom edge.
/// @param base Source RGB color without alpha.
static void vg__ring_box(vgfx_window_t win,
                         int32_t cxq,
                         int32_t cyq,
                         int32_t rmidq,
                         int32_t halfq,
                         int bx0,
                         int by0,
                         int bx1,
                         int by1,
                         uint32_t base) {
    int32_t edge = halfq + 128; // half stroke + 0.5px ramp
    for (int py = by0; py < by1; ++py) {
        int64_t ddy = vg__pixel_center_q8(py) - cyq;
        int64_t ddy2 = ddy * ddy;
        for (int px = bx0; px < bx1; ++px) {
            int64_t ddx = vg__pixel_center_q8(px) - cxq;
            uint64_t d2 = (uint64_t)(ddx * ddx + ddy2);
            int32_t distq = (int32_t)vg__isqrt64(d2);
            int32_t off = distq - rmidq;
            if (off < 0)
                off = -off;
            int32_t alpha = vg__cov_to_alpha(edge - off);
            if (alpha != 0)
                vgfx_pset_alpha(win, px, py, ((uint32_t)alpha << 24) | base);
        }
    }
}

//=============================================================================
// Public API
//=============================================================================

/// @copydoc vg_draw_round_rect_fill
void vg_draw_round_rect_fill(
    vgfx_window_t win, float x, float y, float w, float h, float radius, uint32_t rgb) {
    if (!win || w <= 0.0f || h <= 0.0f)
        return;

    // Snap the box to integer pixels so the opaque bars and AA corner squares
    // tile exactly with no overlap (no double-blended dark seams).
    int ix = vg__floor_i(x + 0.5f);
    int iy = vg__floor_i(y + 0.5f);
    int iw = vg__floor_i(w + 0.5f);
    int ih = vg__floor_i(h + 0.5f);
    if (iw <= 0 || ih <= 0)
        return;

    int max_r = (iw < ih ? iw : ih) / 2;
    int r = (int)(radius + 0.5f);
    if (r < 0)
        r = 0;
    if (r > max_r)
        r = max_r;

    uint32_t base = rgb & 0x00FFFFFFu;

    if (r <= 0) {
        vgfx_fill_rect(win, ix, iy, iw, ih, base);
        return;
    }

    // Opaque interior: centre column (full height) + left/right side bars.
    vgfx_fill_rect(win, ix + r, iy, iw - 2 * r, ih, base);
    vgfx_fill_rect(win, ix, iy + r, r, ih - 2 * r, base);
    vgfx_fill_rect(win, ix + iw - r, iy + r, r, ih - 2 * r, base);

    // Anti-aliased corners, each confined to its r×r corner square.
    int32_t rq = r * 256;
    // Top-left: arc centre at the inner corner (ix+r, iy+r).
    vg__disc_box(win, (ix + r) * 256, (iy + r) * 256, rq, ix, iy, ix + r, iy + r, base);
    // Top-right.
    vg__disc_box(
        win, (ix + iw - r) * 256, (iy + r) * 256, rq, ix + iw - r, iy, ix + iw, iy + r, base);
    // Bottom-left.
    vg__disc_box(
        win, (ix + r) * 256, (iy + ih - r) * 256, rq, ix, iy + ih - r, ix + r, iy + ih, base);
    // Bottom-right.
    vg__disc_box(win,
                 (ix + iw - r) * 256,
                 (iy + ih - r) * 256,
                 rq,
                 ix + iw - r,
                 iy + ih - r,
                 ix + iw,
                 iy + ih,
                 base);
}

/// @copydoc vg_draw_round_rect_stroke
void vg_draw_round_rect_stroke(vgfx_window_t win,
                               float x,
                               float y,
                               float w,
                               float h,
                               float radius,
                               float stroke_w,
                               uint32_t rgb) {
    if (!win || w <= 0.0f || h <= 0.0f)
        return;

    int ix = vg__floor_i(x + 0.5f);
    int iy = vg__floor_i(y + 0.5f);
    int iw = vg__floor_i(w + 0.5f);
    int ih = vg__floor_i(h + 0.5f);
    if (iw <= 0 || ih <= 0)
        return;

    int t = (int)(stroke_w + 0.5f);
    if (t < 1)
        t = 1;

    int max_r = (iw < ih ? iw : ih) / 2;
    int r = (int)(radius + 0.5f);
    if (r < 0)
        r = 0;
    if (r > max_r)
        r = max_r;

    uint32_t base = rgb & 0x00FFFFFFu;

    if (r <= 0) {
        // Plain rectangle frame, crisp.
        vgfx_fill_rect(win, ix, iy, iw, t, base);
        vgfx_fill_rect(win, ix, iy + ih - t, iw, t, base);
        vgfx_fill_rect(win, ix, iy + t, t, ih - 2 * t, base);
        vgfx_fill_rect(win, ix + iw - t, iy + t, t, ih - 2 * t, base);
        return;
    }

    // Straight edges (crisp), between the corner squares.
    vgfx_fill_rect(win, ix + r, iy, iw - 2 * r, t, base);          // top
    vgfx_fill_rect(win, ix + r, iy + ih - t, iw - 2 * r, t, base); // bottom
    vgfx_fill_rect(win, ix, iy + r, t, ih - 2 * r, base);          // left
    vgfx_fill_rect(win, ix + iw - t, iy + r, t, ih - 2 * r, base); // right

    // Anti-aliased corner rings: outer edge at radius r, width t.
    int32_t rmidq = (r * 256) - (t * 256) / 2; // mid-radius in Q8
    int32_t halfq = (t * 256) / 2;
    vg__ring_box(win, (ix + r) * 256, (iy + r) * 256, rmidq, halfq, ix, iy, ix + r, iy + r, base);
    vg__ring_box(win,
                 (ix + iw - r) * 256,
                 (iy + r) * 256,
                 rmidq,
                 halfq,
                 ix + iw - r,
                 iy,
                 ix + iw,
                 iy + r,
                 base);
    vg__ring_box(win,
                 (ix + r) * 256,
                 (iy + ih - r) * 256,
                 rmidq,
                 halfq,
                 ix,
                 iy + ih - r,
                 ix + r,
                 iy + ih,
                 base);
    vg__ring_box(win,
                 (ix + iw - r) * 256,
                 (iy + ih - r) * 256,
                 rmidq,
                 halfq,
                 ix + iw - r,
                 iy + ih - r,
                 ix + iw,
                 iy + ih,
                 base);
}

/// @copydoc vg_draw_disc_fill
void vg_draw_disc_fill(vgfx_window_t win, float cx, float cy, float r, uint32_t rgb) {
    if (!win || r <= 0.0f)
        return;
    int bx0 = vg__floor_i(cx - r);
    int by0 = vg__floor_i(cy - r);
    int bx1 = vg__ceil_i(cx + r) + 1;
    int by1 = vg__ceil_i(cy + r) + 1;
    vg__disc_box(win, vg__q8(cx), vg__q8(cy), vg__q8(r), bx0, by0, bx1, by1, rgb & 0x00FFFFFFu);
}

/// @copydoc vg_draw_circle_stroke
void vg_draw_circle_stroke(
    vgfx_window_t win, float cx, float cy, float r, float stroke_w, uint32_t rgb) {
    if (!win || r <= 0.0f)
        return;
    float t = stroke_w < 1.0f ? 1.0f : stroke_w;
    int bx0 = vg__floor_i(cx - r - t);
    int by0 = vg__floor_i(cy - r - t);
    int bx1 = vg__ceil_i(cx + r + t) + 1;
    int by1 = vg__ceil_i(cy + r + t) + 1;
    int32_t halfq = vg__q8(t * 0.5f);
    int32_t rmidq = vg__q8(r - t * 0.5f);
    vg__ring_box(win, vg__q8(cx), vg__q8(cy), rmidq, halfq, bx0, by0, bx1, by1, rgb & 0x00FFFFFFu);
}

/// @copydoc vg_draw_line_aa
void vg_draw_line_aa(
    vgfx_window_t win, float x0, float y0, float x1, float y1, float stroke_w, uint32_t rgb) {
    if (!win)
        return;
    float t = stroke_w < 1.0f ? 1.0f : stroke_w;

    // Bounding box of the capsule (segment expanded by half-thickness + ramp).
    float minx = (x0 < x1 ? x0 : x1) - t;
    float maxx = (x0 > x1 ? x0 : x1) + t;
    float miny = (y0 < y1 ? y0 : y1) - t;
    float maxy = (y0 > y1 ? y0 : y1) + t;
    int bx0 = vg__floor_i(minx);
    int by0 = vg__floor_i(miny);
    int bx1 = vg__ceil_i(maxx) + 1;
    int by1 = vg__ceil_i(maxy) + 1;

    int32_t x0q = vg__q8(x0), y0q = vg__q8(y0);
    int64_t abx = (int64_t)vg__q8(x1) - x0q;
    int64_t aby = (int64_t)vg__q8(y1) - y0q;
    int64_t len2 = abx * abx + aby * aby; // Q16
    int32_t halfq = vg__q8(t * 0.5f);
    int32_t edge = halfq + 128;
    uint32_t base = rgb & 0x00FFFFFFu;

    for (int py = by0; py < by1; ++py) {
        for (int px = bx0; px < bx1; ++px) {
            int64_t apx = vg__pixel_center_q8(px) - x0q;
            int64_t apy = vg__pixel_center_q8(py) - y0q;
            int32_t clx, cly;
            if (len2 <= 0) {
                clx = x0q;
                cly = y0q;
            } else {
                int64_t dot = apx * abx + apy * aby; // Q16
                int64_t ttq;                         // Q8 param along segment
                if (dot <= 0)
                    ttq = 0;
                else if (dot >= len2)
                    ttq = 256;
                else
                    ttq = (dot * 256) / len2;
                clx = (int32_t)(x0q + (abx * ttq) / 256);
                cly = (int32_t)(y0q + (aby * ttq) / 256);
            }
            int64_t ddx = vg__pixel_center_q8(px) - clx;
            int64_t ddy = vg__pixel_center_q8(py) - cly;
            int32_t distq = (int32_t)vg__isqrt64((uint64_t)(ddx * ddx + ddy * ddy));
            int32_t alpha = vg__cov_to_alpha(edge - distq);
            if (alpha != 0)
                vgfx_pset_alpha(win, px, py, ((uint32_t)alpha << 24) | base);
        }
    }
}

//=============================================================================
// Colour interpolation
//=============================================================================

/// @brief Linearly interpolate two 0x00RRGGBB colours (t in Q8, 0..256).
/// @param a RGB color selected at a zero interpolation factor.
/// @param b RGB color selected at a full interpolation factor.
/// @param t_q8 Q8 interpolation factor in the inclusive range zero through 256.
/// @return Channel-wise interpolated 24-bit RGB color.
static inline uint32_t vg__lerp_rgb(uint32_t a, uint32_t b, int32_t t_q8) {
    int32_t inv = 256 - t_q8;
    int32_t ar = (int32_t)((a >> 16) & 0xFF), ag = (int32_t)((a >> 8) & 0xFF),
            ab = (int32_t)(a & 0xFF);
    int32_t br = (int32_t)((b >> 16) & 0xFF), bg = (int32_t)((b >> 8) & 0xFF),
            bb = (int32_t)(b & 0xFF);
    uint32_t rr = (uint32_t)((ar * inv + br * t_q8) >> 8);
    uint32_t rg = (uint32_t)((ag * inv + bg * t_q8) >> 8);
    uint32_t rb = (uint32_t)((ab * inv + bb * t_q8) >> 8);
    return (rr << 16) | (rg << 8) | rb;
}

//=============================================================================
// Vertical gradient fill + inner highlight
//=============================================================================

/// @copydoc vg_draw_round_rect_gradient_v
void vg_draw_round_rect_gradient_v(vgfx_window_t win,
                                   float x,
                                   float y,
                                   float w,
                                   float h,
                                   float radius,
                                   uint32_t top_rgb,
                                   uint32_t bottom_rgb) {
    if (!win || w <= 0.0f || h <= 0.0f)
        return;
    int ix = vg__floor_i(x + 0.5f);
    int iy = vg__floor_i(y + 0.5f);
    int iw = vg__floor_i(w + 0.5f);
    int ih = vg__floor_i(h + 0.5f);
    if (iw <= 0 || ih <= 0)
        return;
    int max_r = (iw < ih ? iw : ih) / 2;
    int r = (int)(radius + 0.5f);
    if (r < 0)
        r = 0;
    if (r > max_r)
        r = max_r;

    uint32_t topc = top_rgb & 0x00FFFFFFu;
    uint32_t botc = bottom_rgb & 0x00FFFFFFu;
    int denom = ih > 1 ? ih - 1 : 1;

    // Per-row opaque fill: full width in the middle, central column in the
    // corner bands (where the rounded corners cut in).
    for (int yy = 0; yy < ih; ++yy) {
        int32_t t = (int32_t)((yy * 256) / denom);
        uint32_t col = vg__lerp_rgb(topc, botc, t);
        int gy = iy + yy;
        if (r > 0 && (yy < r || yy >= ih - r))
            vgfx_fill_rect(win, ix + r, gy, iw - 2 * r, 1, col);
        else
            vgfx_fill_rect(win, ix, gy, iw, 1, col);
    }

    if (r > 0) {
        // Corners use a constant colour sampled at their mid-row; across an
        // r-pixel corner the subtle gradient delta is visually negligible.
        int32_t rq = r * 256;
        int32_t tTop = (int32_t)(((r / 2) * 256) / denom);
        int32_t tBot = (int32_t)(((ih - r / 2) * 256) / denom);
        uint32_t cTop = vg__lerp_rgb(topc, botc, tTop);
        uint32_t cBot = vg__lerp_rgb(topc, botc, tBot);
        vg__disc_box(win, (ix + r) * 256, (iy + r) * 256, rq, ix, iy, ix + r, iy + r, cTop);
        vg__disc_box(
            win, (ix + iw - r) * 256, (iy + r) * 256, rq, ix + iw - r, iy, ix + iw, iy + r, cTop);
        vg__disc_box(
            win, (ix + r) * 256, (iy + ih - r) * 256, rq, ix, iy + ih - r, ix + r, iy + ih, cBot);
        vg__disc_box(win,
                     (ix + iw - r) * 256,
                     (iy + ih - r) * 256,
                     rq,
                     ix + iw - r,
                     iy + ih - r,
                     ix + iw,
                     iy + ih,
                     cBot);
    }
}

/// @copydoc vg_draw_inner_highlight_top
void vg_draw_inner_highlight_top(
    vgfx_window_t win, float x, float y, float w, float radius, uint32_t rgb) {
    if (!win || w <= 0.0f)
        return;
    int ix = vg__floor_i(x + 0.5f);
    int iy = vg__floor_i(y + 0.5f);
    int iw = vg__floor_i(w + 0.5f);
    int r = (int)(radius + 0.5f);
    if (r < 0)
        r = 0;
    if (iw - 2 * r <= 0)
        return;
    uint32_t base = rgb & 0x00FFFFFFu;
    // A gentle 1px sheen at ~63% opacity along the top inner edge.
    for (int px = ix + r; px < ix + iw - r; ++px)
        vgfx_pset_alpha(win, px, iy, (160u << 24) | base);
}

//=============================================================================
// Soft drop shadow (separable box blur, cached by silhouette key)
//
// NOTE: GUI painting is single-threaded (main thread), so the cache needs no
// locking. The cache holds at most VG_SHADOW_CACHE_N small alpha bitmaps.
//=============================================================================

#define VG_SHADOW_CACHE_N 8
#define VG_SHADOW_MAX_DIM 1024 // only cache padded bitmaps up to this size

typedef struct vg_shadow_entry {
    int valid;
    int iw, ih, r, br; // cache key
    int W, H, pad;     // padded bitmap dims
    uint8_t *data;     // W*H alpha coverage
    uint32_t use;      // LRU timestamp
} vg_shadow_entry;

static vg_shadow_entry g_shadow_cache[VG_SHADOW_CACHE_N];
static uint32_t g_shadow_clock = 0;

/// @brief Validate and compute padded shadow bitmap dimensions.
/// @details Shadow caches allocate one byte per padded pixel. This helper keeps
///          the int-facing raster loops while doing the padding and W*H product
///          in wider types so oversized blur or widget dimensions fail closed.
/// @param iw Source rectangle width in pixels.
/// @param ih Source rectangle height in pixels.
/// @param br Box-blur radius in pixels.
/// @param outW Receives padded bitmap width.
/// @param outH Receives padded bitmap height.
/// @param outPad Receives padding in pixels.
/// @param outBytes Receives the allocation byte count.
/// @return Non-zero when every output is representable and safe to allocate.
static int vg__shadow_dims(
    int iw, int ih, int br, int *outW, int *outH, int *outPad, size_t *outBytes) {
    if (iw <= 0 || ih <= 0 || br <= 0 || !outW || !outH || !outPad || !outBytes)
        return 0;
    int64_t pad64 = (int64_t)br * 3 + 1;
    int64_t w64 = (int64_t)iw + 2 * pad64;
    int64_t h64 = (int64_t)ih + 2 * pad64;
    if (pad64 > INT32_MAX || w64 <= 0 || h64 <= 0 || w64 > INT32_MAX || h64 > INT32_MAX)
        return 0;
    size_t W = (size_t)w64;
    size_t H = (size_t)h64;
    if (W > SIZE_MAX / H)
        return 0;
    *outW = (int)W;
    *outH = (int)H;
    *outPad = (int)pad64;
    *outBytes = W * H;
    return 1;
}

/// @brief Rasterise a hard rounded-rectangle silhouette into a padded alpha buffer.
/// @param buf Destination buffer containing at least @p W times @p H bytes.
/// @param W Padded buffer width.
/// @param H Padded buffer height.
/// @param pad Empty border preceding the silhouette on each side.
/// @param iw Unpadded silhouette width.
/// @param ih Unpadded silhouette height.
/// @param r Clamped corner radius in pixels.
static void vg__rasterize_silhouette(uint8_t *buf, int W, int H, int pad, int iw, int ih, int r) {
    memset(buf, 0, (size_t)W * (size_t)H);
    for (int ly = 0; ly < ih; ++ly) {
        uint8_t *row = buf + (size_t)(pad + ly) * (size_t)W + (size_t)pad;
        for (int lx = 0; lx < iw; ++lx) {
            int inside = 1;
            int cx = -1, cy = -1;
            if (lx < r && ly < r) {
                cx = r;
                cy = r;
            } else if (lx >= iw - r && ly < r) {
                cx = iw - r;
                cy = r;
            } else if (lx < r && ly >= ih - r) {
                cx = r;
                cy = ih - r;
            } else if (lx >= iw - r && ly >= ih - r) {
                cx = iw - r;
                cy = ih - r;
            }
            if (cx >= 0) {
                int ddx = lx - cx, ddy = ly - cy;
                if (ddx * ddx + ddy * ddy > r * r)
                    inside = 0;
            }
            row[lx] = inside ? 255 : 0;
        }
    }
}

/// @brief Horizontal box blur with zero-padding at the buffer edges.
/// @param src Source coverage buffer containing @p W times @p H bytes.
/// @param dst Distinct destination buffer of the same size.
/// @param W Buffer width.
/// @param H Buffer height.
/// @param r Non-negative box radius.
static void vg__box_blur_h(const uint8_t *src, uint8_t *dst, int W, int H, int r) {
    int norm = 2 * r + 1;
    for (int y = 0; y < H; ++y) {
        const uint8_t *s = src + (size_t)y * (size_t)W;
        uint8_t *d = dst + (size_t)y * (size_t)W;
        int sum = 0;
        for (int x = -r; x <= r; ++x)
            if (x >= 0 && x < W)
                sum += s[x];
        for (int x = 0; x < W; ++x) {
            d[x] = (uint8_t)(sum / norm);
            int add = x + r + 1, sub = x - r;
            if (add < W)
                sum += s[add];
            if (sub >= 0)
                sum -= s[sub];
        }
    }
}

/// @brief Vertical box blur with zero-padding at the buffer edges.
/// @param src Source coverage buffer containing @p W times @p H bytes.
/// @param dst Distinct destination buffer of the same size.
/// @param W Buffer width.
/// @param H Buffer height.
/// @param r Non-negative box radius.
static void vg__box_blur_v(const uint8_t *src, uint8_t *dst, int W, int H, int r) {
    int norm = 2 * r + 1;
    for (int x = 0; x < W; ++x) {
        int sum = 0;
        for (int y = -r; y <= r; ++y)
            if (y >= 0 && y < H)
                sum += src[(size_t)y * (size_t)W + (size_t)x];
        for (int y = 0; y < H; ++y) {
            dst[(size_t)y * (size_t)W + (size_t)x] = (uint8_t)(sum / norm);
            int add = y + r + 1, sub = y - r;
            if (add < H)
                sum += src[(size_t)add * (size_t)W + (size_t)x];
            if (sub >= 0)
                sum -= src[(size_t)sub * (size_t)W + (size_t)x];
        }
    }
}

/// @brief Fetch (or compute and cache) the blurred shadow bitmap.
/// @details Cacheable dimensions use the least-recently-used static entry. Oversized but valid
/// dimensions return a transient allocation instead.
/// @param iw Unpadded silhouette width.
/// @param ih Unpadded silhouette height.
/// @param r Clamped corner radius.
/// @param br Box-blur radius.
/// @param outW Receives padded bitmap width.
/// @param outH Receives padded bitmap height.
/// @param outPad Receives surrounding padding.
/// @param transient_out Receives a freshly-allocated bitmap the caller must
///        free, used only when the result is too large to cache.
/// @return Borrowed cached bytes or owned transient bytes on success; NULL on invalid
/// dimensions or allocation failure.
static const uint8_t *vg__get_shadow(
    int iw, int ih, int r, int br, int *outW, int *outH, int *outPad, uint8_t **transient_out) {
    int pad = 0;
    int W = 0;
    int H = 0;
    size_t bytes = 0;
    if (!vg__shadow_dims(iw, ih, br, &W, &H, &pad, &bytes))
        return NULL;
    *outW = W;
    *outH = H;
    *outPad = pad;
    *transient_out = NULL;
    int cacheable = (W <= VG_SHADOW_MAX_DIM && H <= VG_SHADOW_MAX_DIM);

    if (cacheable) {
        for (int i = 0; i < VG_SHADOW_CACHE_N; ++i) {
            vg_shadow_entry *e = &g_shadow_cache[i];
            if (e->valid && e->iw == iw && e->ih == ih && e->r == r && e->br == br) {
                e->use = ++g_shadow_clock;
                return e->data;
            }
        }
    }

    uint8_t *a = (uint8_t *)malloc(bytes);
    uint8_t *b = (uint8_t *)malloc(bytes);
    if (!a || !b) {
        free(a);
        free(b);
        return NULL;
    }
    vg__rasterize_silhouette(a, W, H, pad, iw, ih, r);
    for (int pass = 0; pass < 3; ++pass) {
        vg__box_blur_h(a, b, W, H, br);
        vg__box_blur_v(b, a, W, H, br);
    }
    free(b);

    if (!cacheable) {
        *transient_out = a;
        return a;
    }

    int lru = 0;
    uint32_t best = 0xFFFFFFFFu;
    for (int i = 0; i < VG_SHADOW_CACHE_N; ++i) {
        if (!g_shadow_cache[i].valid) {
            lru = i;
            break;
        }
        if (g_shadow_cache[i].use < best) {
            best = g_shadow_cache[i].use;
            lru = i;
        }
    }
    vg_shadow_entry *e = &g_shadow_cache[lru];
    free(e->data);
    e->valid = 1;
    e->iw = iw;
    e->ih = ih;
    e->r = r;
    e->br = br;
    e->W = W;
    e->H = H;
    e->pad = pad;
    e->data = a;
    e->use = ++g_shadow_clock;
    return a;
}

/// @copydoc vg_draw_round_rect_shadow
void vg_draw_round_rect_shadow(vgfx_window_t win,
                               float x,
                               float y,
                               float w,
                               float h,
                               float radius,
                               float blur,
                               int dx,
                               int dy,
                               uint8_t alpha,
                               uint32_t shadow_rgb) {
    if (!win || w <= 0.0f || h <= 0.0f || blur <= 0.0f || alpha == 0)
        return;
    int ix = vg__floor_i(x + 0.5f);
    int iy = vg__floor_i(y + 0.5f);
    int iw = vg__floor_i(w + 0.5f);
    int ih = vg__floor_i(h + 0.5f);
    if (iw <= 0 || ih <= 0)
        return;
    int max_r = (iw < ih ? iw : ih) / 2;
    int r = (int)(radius + 0.5f);
    if (r < 0)
        r = 0;
    if (r > max_r)
        r = max_r;
    int br = (int)(blur * 0.5f + 0.5f);
    if (br < 1)
        br = 1;
    int pad_estimate = 0;
    int shadow_w = 0;
    int shadow_h = 0;
    size_t shadow_bytes = 0;
    if (!vg__shadow_dims(iw, ih, br, &shadow_w, &shadow_h, &pad_estimate, &shadow_bytes))
        return;
    (void)shadow_bytes;
    vgfx_framebuffer_t fb;
    if (vgfx_get_framebuffer(win, &fb)) {
        int64_t shadow_left = (int64_t)ix + dx - pad_estimate;
        int64_t shadow_top = (int64_t)iy + dy - pad_estimate;
        int64_t shadow_right = shadow_left + (int64_t)shadow_w;
        int64_t shadow_bottom = shadow_top + (int64_t)shadow_h;
        if (shadow_right <= 0 || shadow_bottom <= 0 || shadow_left >= fb.width ||
            shadow_top >= fb.height)
            return;
    }

    int W, H, pad;
    uint8_t *transient = NULL;
    const uint8_t *bmp = vg__get_shadow(iw, ih, r, br, &W, &H, &pad, &transient);
    if (!bmp)
        return;

    uint32_t base = shadow_rgb & 0x00FFFFFFu;
    int ox = ix + dx - pad, oy = iy + dy - pad;
    for (int py = 0; py < H; ++py) {
        const uint8_t *srow = bmp + (size_t)py * (size_t)W;
        int sy = oy + py;
        for (int px = 0; px < W; ++px) {
            uint8_t cov = srow[px];
            if (!cov)
                continue;
            int a = (cov * (int)alpha) / 255;
            if (a)
                vgfx_pset_alpha(win, ox + px, sy, ((uint32_t)a << 24) | base);
        }
    }
    free(transient);
}
