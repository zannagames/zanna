//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_pixels_transform.c
// Purpose: Geometric transforms and image processing effects for
//   Zanna.Graphics.Pixels. Includes flips, rotations (90/180/arbitrary),
//   scaling, color inversion, grayscale conversion, tinting, box blur,
//   and high-quality bilinear resize.
//
// Key invariants:
//   - All transforms return a NEW Pixels object; the source is never modified.
//   - Interpolation and blur operate in premultiplied-alpha space before
//     returning canonical straight-alpha pixels, preventing transparent-edge
//     color bleed.
//   - Arbitrary rotation uses bilinear interpolation and transparent samples
//     beyond the source bounds.
//   - Box blur uses separable convolution (horizontal then vertical); the kernel
//     is a uniform (2r+1) box, not a Gaussian.
//   - Resize uses endpoint-aligned bilinear interpolation for mild changes and
//     a source-footprint box average when either destination axis is strictly
//     less than half its source axis.
//   - Pixel format is 32-bit RGBA: 0xRRGGBBAA in row-major order.
//
// Ownership/Lifetime:
//   - Returned Pixels objects are GC-managed via pixels_alloc().
//   - Temporary maps and intermediate images are malloc/freed within the
//     owning transform.
//
// Links: src/runtime/graphics/2d/rt_pixels_internal.h (shared representation),
//        src/runtime/graphics/2d/rt_pixels.c (core operations),
//        src/runtime/graphics/2d/rt_pixels.h (public API)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements non-mutating Pixels transforms, resampling, and effects.
///
/// Every public operation validates the source handle and publishes a distinct
/// GC-managed result. Geometry and allocation sizes are overflow-checked;
/// helpers use extended or fixed-point arithmetic where endpoint mapping and
/// extreme dimensions would otherwise lose definition.

#include "rt_pixels.h"
#include "rt_pixels_internal.h"

#include "rt_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Image Transforms
//=============================================================================

/// @brief Clamp an int64_t to the [0, 255] range and return as uint8_t.
/// @details Used when converting premultiplied-alpha intermediate values back to
///   straight-alpha 8-bit channels after bilinear interpolation or blur passes.
/// @param value Channel value to clamp.
/// @return @p value saturated to the unsigned eight-bit range.
static uint8_t pixels_clamp_u8_i64(int64_t value) {
    if (value <= 0)
        return 0;
    if (value >= 255)
        return 255;
    return (uint8_t)value;
}

/// @brief Pack premultiplied RGB channels and a straight alpha into a 0xRRGGBBAA pixel.
/// @details Unpremultiplies r/g/b by dividing by @p a (with rounding) before packing.
///   Returns 0 (transparent black) when @p a == 0 to avoid divide-by-zero and because
///   a fully transparent pixel should carry no colour information.
/// @param premul_r  Red channel already multiplied by alpha (pre-alpha value).
/// @param premul_g  Green channel already multiplied by alpha.
/// @param premul_b  Blue channel already multiplied by alpha.
/// @param a         Straight alpha value in [0, 255].
/// @return          Packed 0xRRGGBBAA pixel with straight-alpha channels.
static uint32_t pixels_pack_rgba_pm(int64_t premul_r,
                                    int64_t premul_g,
                                    int64_t premul_b,
                                    int64_t a) {
    uint8_t a8 = pixels_clamp_u8_i64(a);
    if (a8 == 0)
        return 0;

    int64_t alpha = a8;
    int64_t r = (premul_r + alpha / 2) / alpha;
    int64_t g = (premul_g + alpha / 2) / alpha;
    int64_t b = (premul_b + alpha / 2) / alpha;
    return ((uint32_t)pixels_clamp_u8_i64(r) << 24) | ((uint32_t)pixels_clamp_u8_i64(g) << 16) |
           ((uint32_t)pixels_clamp_u8_i64(b) << 8) | (uint32_t)a8;
}

/// @brief Bilinear interpolation of four RGBA pixels in premultiplied-alpha space (fixed-point).
/// @details Weights are expressed as fixed-point fractions in [0, 256]; the
///   four bilinear weights sum to 256*256.  Interpolation is done in premultiplied-alpha
///   space to avoid dark-fringe artefacts at transparent boundaries, then the result is
///   unpremultiplied via pixels_pack_rgba_pm.
/// @param c00     Top-left pixel (0xRRGGBBAA).
/// @param c10     Top-right pixel.
/// @param c01     Bottom-left pixel.
/// @param c11     Bottom-right pixel.
/// @param frac_x  Fractional X in [0, 256] (256 = fully right).
/// @param frac_y  Fractional Y in [0, 256] (256 = fully bottom).
/// @return        Interpolated 0xRRGGBBAA pixel.
static uint32_t pixels_bilerp_rgba_premul_fixed(
    uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, int64_t frac_x, int64_t frac_y) {
    int64_t inv_frac_x = 256 - frac_x;
    int64_t inv_frac_y = 256 - frac_y;

    int64_t a00 = c00 & 0xFF;
    int64_t a10 = c10 & 0xFF;
    int64_t a01 = c01 & 0xFF;
    int64_t a11 = c11 & 0xFF;

    int64_t a = (a00 * inv_frac_x * inv_frac_y + a10 * frac_x * inv_frac_y +
                 a01 * inv_frac_x * frac_y + a11 * frac_x * frac_y) >>
                16;
    if (a <= 0)
        return 0;

    int64_t premul_r = ((((c00 >> 24) & 0xFF) * a00) * inv_frac_x * inv_frac_y +
                        (((c10 >> 24) & 0xFF) * a10) * frac_x * inv_frac_y +
                        (((c01 >> 24) & 0xFF) * a01) * inv_frac_x * frac_y +
                        (((c11 >> 24) & 0xFF) * a11) * frac_x * frac_y) >>
                       16;
    int64_t premul_g = ((((c00 >> 16) & 0xFF) * a00) * inv_frac_x * inv_frac_y +
                        (((c10 >> 16) & 0xFF) * a10) * frac_x * inv_frac_y +
                        (((c01 >> 16) & 0xFF) * a01) * inv_frac_x * frac_y +
                        (((c11 >> 16) & 0xFF) * a11) * frac_x * frac_y) >>
                       16;
    int64_t premul_b = ((((c00 >> 8) & 0xFF) * a00) * inv_frac_x * inv_frac_y +
                        (((c10 >> 8) & 0xFF) * a10) * frac_x * inv_frac_y +
                        (((c01 >> 8) & 0xFF) * a01) * inv_frac_x * frac_y +
                        (((c11 >> 8) & 0xFF) * a11) * frac_x * frac_y) >>
                       16;

    return pixels_pack_rgba_pm(premul_r, premul_g, premul_b, a);
}

/// @brief Bilinear interpolation of four RGBA pixels in premultiplied-alpha space (floating-point).
/// @details Double-precision variant of pixels_bilerp_rgba_premul_fixed, used for the
///   arbitrary-angle rotation pass where the source coordinates are already expressed in
///   double-precision floats.  Higher precision than the fixed-point variant at the cost of
///   slightly slower arithmetic.
/// @param c00  Top-left pixel (0xRRGGBBAA).
/// @param c10  Top-right pixel.
/// @param c01  Bottom-left pixel.
/// @param c11  Bottom-right pixel.
/// @param fx   Fractional X in [0.0, 1.0).
/// @param fy   Fractional Y in [0.0, 1.0).
/// @return     Interpolated 0xRRGGBBAA pixel.
static uint32_t pixels_bilerp_rgba_premul_double(
    uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, double fx, double fy) {
    double wx0 = 1.0 - fx;
    double wy0 = 1.0 - fy;
    double w00 = wx0 * wy0;
    double w10 = fx * wy0;
    double w01 = wx0 * fy;
    double w11 = fx * fy;

    double a00 = (double)(c00 & 0xFF);
    double a10 = (double)(c10 & 0xFF);
    double a01 = (double)(c01 & 0xFF);
    double a11 = (double)(c11 & 0xFF);

    double a = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
    if (a <= 0.0)
        return 0;

    double premul_r =
        (double)((c00 >> 24) & 0xFF) * a00 * w00 + (double)((c10 >> 24) & 0xFF) * a10 * w10 +
        (double)((c01 >> 24) & 0xFF) * a01 * w01 + (double)((c11 >> 24) & 0xFF) * a11 * w11;
    double premul_g =
        (double)((c00 >> 16) & 0xFF) * a00 * w00 + (double)((c10 >> 16) & 0xFF) * a10 * w10 +
        (double)((c01 >> 16) & 0xFF) * a01 * w01 + (double)((c11 >> 16) & 0xFF) * a11 * w11;
    double premul_b =
        (double)((c00 >> 8) & 0xFF) * a00 * w00 + (double)((c10 >> 8) & 0xFF) * a10 * w10 +
        (double)((c01 >> 8) & 0xFF) * a01 * w01 + (double)((c11 >> 8) & 0xFF) * a11 * w11;

    return pixels_pack_rgba_pm((int64_t)(premul_r + 0.5),
                               (int64_t)(premul_g + 0.5),
                               (int64_t)(premul_b + 0.5),
                               (int64_t)(a + 0.5));
}

/// @brief Average @p count premultiplied-alpha samples and return as a 0xRRGGBBAA pixel.
/// @details Used by the separable box blur to combine the running sums accumulated over
///   the kernel window.  Rounds all channels using count/2 before dividing to reduce
///   systematic darkening from truncation.  Returns transparent black when count == 0
///   or the alpha average rounds to zero.
/// @param sum_premul_r  Sum of (R * A) values across the kernel window.
/// @param sum_premul_g  Sum of (G * A) values.
/// @param sum_premul_b  Sum of (B * A) values.
/// @param sum_a         Sum of alpha values.
/// @param count         Number of samples summed; must be > 0.
/// @return              Averaged 0xRRGGBBAA pixel.
static uint32_t pixels_average_rgba_premul(int64_t sum_premul_r,
                                           int64_t sum_premul_g,
                                           int64_t sum_premul_b,
                                           int64_t sum_a,
                                           int64_t count) {
    if (count <= 0)
        return 0;

    int64_t a = (sum_a + count / 2) / count;
    if (a <= 0)
        return 0;

    int64_t premul_r = (sum_premul_r + count / 2) / count;
    int64_t premul_g = (sum_premul_g + count / 2) / count;
    int64_t premul_b = (sum_premul_b + count / 2) / count;
    return pixels_pack_rgba_pm(premul_r, premul_g, premul_b, a);
}

/// @brief Map a destination index to its nearest endpoint-aligned source index.
/// @details Computes `dst * (src_size - 1) / (dst_size - 1)`, rounds to the
///   closest integer, and clamps to `[0, src_size - 1]`, so the first and last
///   destination pixels exactly sample the first and last source pixels.
///   Used by the nearest-neighbor scale pass (rt_pixels_scale).  Long double arithmetic
///   avoids integer rounding errors on large images.
/// @param dst       Destination pixel coordinate (0-based).
/// @param src_size  Source dimension (width or height).
/// @param dst_size  Destination dimension.
/// @return          Source coordinate in [0, src_size - 1].
static int64_t pixels_map_index(int64_t dst, int64_t src_size, int64_t dst_size) {
    if (src_size <= 1 || dst_size <= 1)
        return 0;
    long double mapped =
        ((long double)dst * (long double)(src_size - 1)) / (long double)(dst_size - 1);
    if (mapped >= (long double)(src_size - 1))
        return src_size - 1;
    if (mapped <= 0.0L)
        return 0;
    return (int64_t)(mapped + 0.5L);
}

/// @brief Map a destination pixel index to a 8.8 fixed-point source position for bilinear resize.
/// @details Returns dst * (src_size - 1) * 256 / (dst_size - 1), i.e. the source coordinate
/// expressed in
///   units of 1/256 of a pixel.  The caller shifts right by 8 to get the integer part and masks
///   with 0xFF to get the fractional weight for pixels_bilerp_rgba_premul_fixed.
/// @param dst       Destination pixel coordinate (0-based).
/// @param src_size  Source dimension.
/// @param dst_size  Destination dimension.
/// @return          256× magnified source coordinate, clamped to [0, INT64_MAX].
static int64_t pixels_map_fixed_256(int64_t dst, int64_t src_size, int64_t dst_size) {
    if (src_size <= 1 || dst_size <= 1)
        return 0;
    long double mapped =
        ((long double)dst * (long double)(src_size - 1) * 256.0L) / (long double)(dst_size - 1);
    if (mapped >= (long double)INT64_MAX)
        return INT64_MAX;
    if (mapped <= 0.0L)
        return 0;
    return (int64_t)mapped;
}

/// @brief Convert a positive long-double extent (output dimension) to an int64
///        pixel count, ceiling-rounded and bounded to at least 1.
/// @details Used by the rotation/scale paths where the bounding box of the
///          transformed image is computed in long double for precision and
///          must be widened to a whole-pixel allocation. Rejects NaN/inf and
///          negative extents (returns 0 — caller treats as "transform invalid"
///          and produces no output). A zero-or-fraction extent rounds up to 1
///          so degenerate inputs still return a 1-pixel buffer rather than
///          allocating a 0-byte array.
/// @param extent Non-negative finite output extent to ceiling-round.
/// @param out Receives a pixel count of at least one on success.
/// @return 1 on success (out written), 0 if @p extent is invalid or overflows i64.
static int8_t pixels_long_double_extent_to_i64(long double extent, int64_t *out) {
    if (!out || !isfinite(extent) || extent < 0.0L)
        return 0;

    long double rounded = ceill(extent);
    if (rounded < 1.0L)
        rounded = 1.0L;
    if (rounded > (long double)INT64_MAX)
        return 0;

    *out = (int64_t)rounded;
    return 1;
}

/// @brief Validate a Pixels buffer's row and total byte counts.
/// @details Transform functions often index by `width * height` or copy whole
///          rows. This helper performs the signed-dimension, pixel-count, and
///          size_t byte overflow checks before those products are evaluated.
///          Zero-width or zero-height buffers are valid and report zero counts.
/// @param p                Pixels implementation to inspect.
/// @param pixel_count_out  Optional output for width × height.
/// @param pixel_bytes_out  Optional output for pixel_count × sizeof(uint32_t).
/// @param row_bytes_out    Optional output for width × sizeof(uint32_t).
/// @return 1 when all requested counts are representable; 0 otherwise.
static int8_t pixels_transform_checked_layout(const rt_pixels_impl *p,
                                              int64_t *pixel_count_out,
                                              size_t *pixel_bytes_out,
                                              size_t *row_bytes_out) {
    int64_t pixel_count = 0;
    size_t row_bytes = 0;
    size_t pixel_bytes = 0;

    if (!p || p->width < 0 || p->height < 0)
        return 0;
    if (p->width > 0) {
        if ((uint64_t)p->width > (uint64_t)SIZE_MAX / sizeof(uint32_t))
            return 0;
        row_bytes = (size_t)p->width * sizeof(uint32_t);
    }
    if (p->width > 0 && p->height > 0) {
        if (p->width > INT64_MAX / p->height)
            return 0;
        pixel_count = p->width * p->height;
        if ((uint64_t)pixel_count > (uint64_t)SIZE_MAX / sizeof(uint32_t))
            return 0;
        pixel_bytes = (size_t)pixel_count * sizeof(uint32_t);
    }
    if (pixel_count_out)
        *pixel_count_out = pixel_count;
    if (pixel_bytes_out)
        *pixel_bytes_out = pixel_bytes;
    if (row_bytes_out)
        *row_bytes_out = row_bytes;
    return 1;
}

/// @brief Return a horizontally mirrored copy of a Pixels buffer.
/// @details Reverses columns within every row while preserving dimensions and
///          exact RGBA words. Empty dimensions produce a valid empty copy.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @return A distinct caller-owned Pixels handle, or null after invalid input
///         or allocation failure.
void *rt_pixels_flip_h(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.FlipH: null pixels");
    if (!p)
        return NULL;

    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result)
        return NULL;
    if (p->width <= 0 || p->height <= 0)
        return result;

    for (int64_t y = 0; y < p->height; y++) {
        const uint32_t *src_row = p->data + y * p->width;
        uint32_t *dst_row = result->data + y * p->width;
        for (int64_t x = 0; x < p->width; x++) {
            dst_row[p->width - 1 - x] = src_row[x];
        }
    }

    return result;
}

/// @brief Return a vertically mirrored copy of a Pixels buffer.
/// @details Copies complete rows in reverse order after validating row-byte
///          arithmetic. Dimensions and exact RGBA words are preserved.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @return A distinct caller-owned Pixels handle, or null after validation,
///         layout, or allocation failure.
void *rt_pixels_flip_v(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.FlipV: null pixels");
    if (!p)
        return NULL;

    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result)
        return NULL;
    if (p->width <= 0 || p->height <= 0)
        return result;

    size_t row_bytes = 0;
    if (!pixels_transform_checked_layout(p, NULL, NULL, &row_bytes)) {
        rt_trap("Pixels.FlipV: image dimensions overflow");
        return NULL;
    }
    for (int64_t y = 0; y < p->height; y++) {
        memcpy(&result->data[(p->height - 1 - y) * p->width], &p->data[y * p->width], row_bytes);
    }

    return result;
}

/// @brief Return a copy rotated 90 degrees clockwise.
/// @details Maps source coordinates to the swapped `height`-by-`width`
///          destination without interpolation, preserving exact RGBA words.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @return A distinct caller-owned Pixels handle with swapped dimensions, or
///         null after invalid input or allocation failure.
void *rt_pixels_rotate_cw(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.RotateCW: null pixels");
    if (!p)
        return NULL;

    // New dimensions: width becomes height, height becomes width
    int64_t new_width = p->height;
    int64_t new_height = p->width;

    rt_pixels_impl *result = pixels_alloc(new_width, new_height);
    if (!result)
        return NULL;

    // Rotate 90 CW: emit each destination row contiguously.
    for (int64_t dst_y = 0; dst_y < new_height; dst_y++) {
        uint32_t *dst_row = result->data + dst_y * new_width;
        for (int64_t dst_x = 0; dst_x < new_width; dst_x++) {
            dst_row[dst_x] = p->data[(p->height - 1 - dst_x) * p->width + dst_y];
        }
    }

    return result;
}

/// @brief Return a copy rotated 90 degrees counter-clockwise.
/// @details Maps source coordinates to the swapped `height`-by-`width`
///          destination without interpolation, preserving exact RGBA words.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @return A distinct caller-owned Pixels handle with swapped dimensions, or
///         null after invalid input or allocation failure.
void *rt_pixels_rotate_ccw(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.RotateCCW: null pixels");
    if (!p)
        return NULL;

    // New dimensions: width becomes height, height becomes width
    int64_t new_width = p->height;
    int64_t new_height = p->width;

    rt_pixels_impl *result = pixels_alloc(new_width, new_height);
    if (!result)
        return NULL;

    // Rotate 90 CCW: emit each destination row contiguously.
    for (int64_t dst_y = 0; dst_y < new_height; dst_y++) {
        uint32_t *dst_row = result->data + dst_y * new_width;
        int64_t src_x = p->width - 1 - dst_y;
        for (int64_t dst_x = 0; dst_x < new_width; dst_x++) {
            dst_row[dst_x] = p->data[dst_x * p->width + src_x];
        }
    }

    return result;
}

/// @brief Return a copy rotated 180 degrees.
/// @details Reverses the complete row-major word sequence, equivalent to
///          horizontal and vertical mirroring while preserving dimensions.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @return A distinct caller-owned Pixels handle, or null after validation,
///         layout, or allocation failure.
void *rt_pixels_rotate_180(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Rotate180: null pixels");
    if (!p)
        return NULL;

    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result)
        return NULL;
    if (p->width <= 0 || p->height <= 0)
        return result;

    // Rotate 180: src[x,y] -> dst[width-1-x, height-1-y]
    int64_t total = 0;
    if (!pixels_transform_checked_layout(p, &total, NULL, NULL)) {
        rt_trap("Pixels.Rotate180: image dimensions overflow");
        return NULL;
    }
    for (int64_t i = 0; i < total; i++) {
        result->data[total - 1 - i] = p->data[i];
    }

    return result;
}

/// @brief Return a copy rotated clockwise by an arbitrary angle.
/// @details Normalizes finite @p angle_degrees modulo 360. Values within
///          0.001 degrees of a cardinal orientation use exact copy or
///          quarter-turn paths. Other angles allocate the ceiling-rounded
///          axis-aligned bounds of the rotated pixel-center rectangle and
///          inverse-map each destination sample with premultiplied-alpha
///          bilinear interpolation; source neighbors beyond the image are
///          transparent black. A source with either empty dimension produces
///          a 0-by-0 result.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @param angle_degrees Clockwise angle in degrees; non-finite input traps.
/// @return A distinct caller-owned Pixels handle, or null when validation,
///         dimension calculation, or allocation fails.
void *rt_pixels_rotate(void *pixels, double angle_degrees) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Rotate: null pixels");
    if (!p)
        return NULL;

    if (p->width <= 0 || p->height <= 0)
        return pixels_alloc(0, 0);

    if (!isfinite(angle_degrees)) {
        rt_trap("Pixels.Rotate: non-finite angle");
        return NULL;
    }

    // Normalize angle to [0, 360) without unbounded loops for huge inputs.
    angle_degrees = fmod(angle_degrees, 360.0);
    if (angle_degrees < 0.0)
        angle_degrees += 360.0;

    // Fast paths for common angles
    if (fabs(angle_degrees) < 0.001 || fabs(angle_degrees - 360.0) < 0.001) {
        // No rotation - return a copy
        size_t copy_bytes = 0;
        if (!pixels_transform_checked_layout(p, NULL, &copy_bytes, NULL)) {
            rt_trap("Pixels.Rotate: image dimensions overflow");
            return NULL;
        }
        rt_pixels_impl *result = pixels_alloc(p->width, p->height);
        if (!result)
            return NULL;
        memcpy(result->data, p->data, copy_bytes);
        return result;
    }
    if (fabs(angle_degrees - 90.0) < 0.001)
        return rt_pixels_rotate_cw(pixels);
    if (fabs(angle_degrees - 180.0) < 0.001)
        return rt_pixels_rotate_180(pixels);
    if (fabs(angle_degrees - 270.0) < 0.001)
        return rt_pixels_rotate_ccw(pixels);

    // Convert to radians
    double rad = angle_degrees * (3.14159265358979323846 / 180.0);
    double cos_a = cos(rad);
    double sin_a = sin(rad);

    double src_cx = ((double)p->width - 1.0) * 0.5;
    double src_cy = ((double)p->height - 1.0) * 0.5;

    double source_corners[4][2] = {
        {-src_cx, -src_cy},
        {(double)(p->width - 1) - src_cx, -src_cy},
        {(double)(p->width - 1) - src_cx, (double)(p->height - 1) - src_cy},
        {-src_cx, (double)(p->height - 1) - src_cy},
    };
    double corners[4][2];
    for (int i = 0; i < 4; i++) {
        double rx = source_corners[i][0];
        double ry = source_corners[i][1];
        corners[i][0] = rx * cos_a - ry * sin_a;
        corners[i][1] = rx * sin_a + ry * cos_a;
    }

    double min_x = corners[0][0], max_x = corners[0][0];
    double min_y = corners[0][1], max_y = corners[0][1];
    for (int i = 1; i < 4; i++) {
        if (corners[i][0] < min_x)
            min_x = corners[i][0];
        if (corners[i][0] > max_x)
            max_x = corners[i][0];
        if (corners[i][1] < min_y)
            min_y = corners[i][1];
        if (corners[i][1] > max_y)
            max_y = corners[i][1];
    }

    int64_t new_width = 0;
    int64_t new_height = 0;
    if (!pixels_long_double_extent_to_i64((long double)max_x - (long double)min_x + 1.0L,
                                          &new_width) ||
        !pixels_long_double_extent_to_i64((long double)max_y - (long double)min_y + 1.0L,
                                          &new_height)) {
        rt_trap("Pixels.Rotate: dimensions too large");
        return NULL;
    }

    rt_pixels_impl *result = pixels_alloc(new_width, new_height);
    if (!result)
        return NULL;

    double dst_cx = ((double)new_width - 1.0) * 0.5;
    double dst_cy = ((double)new_height - 1.0) * 0.5;

    // For each destination pixel, find source pixel using inverse rotation
    for (int64_t dy = 0; dy < new_height; dy++) {
        for (int64_t dx = 0; dx < new_width; dx++) {
            // Destination position relative to new center
            double dx_c = (double)dx - dst_cx;
            double dy_c = (double)dy - dst_cy;

            // Inverse rotation to find source position
            double sx_c = dx_c * cos_a + dy_c * sin_a;
            double sy_c = -dx_c * sin_a + dy_c * cos_a;

            // Source position in original image coordinates
            double sx = sx_c + src_cx;
            double sy = sy_c + src_cy;

            // Bilinear interpolation
            int64_t x0 = (int64_t)floor(sx);
            int64_t y0 = (int64_t)floor(sy);
            int64_t x1 = x0 + 1;
            int64_t y1 = y0 + 1;

            // Skip if completely outside source
            if (x1 < 0 || x0 >= p->width || y1 < 0 || y0 >= p->height)
                continue;

            // Fractional parts
            double fx = sx - x0;
            double fy = sy - y0;

            // Get the four surrounding pixels (with bounds checking)
            uint32_t c00 = 0, c10 = 0, c01 = 0, c11 = 0;

            if (x0 >= 0 && y0 >= 0)
                c00 = p->data[y0 * p->width + x0];
            if (x1 < p->width && y0 >= 0)
                c10 = p->data[y0 * p->width + x1];
            if (x0 >= 0 && y1 < p->height)
                c01 = p->data[y1 * p->width + x0];
            if (x1 < p->width && y1 < p->height)
                c11 = p->data[y1 * p->width + x1];

            result->data[dy * new_width + dx] =
                pixels_bilerp_rgba_premul_double(c00, c10, c01, c11, fx, fy);
        }
    }

    return result;
}

/// @brief Resize to positive dimensions with nearest-neighbor sampling.
/// @details Endpoint-aligned mapping makes destination corners sample source
///          corners exactly. A source with either empty dimension produces a
///          transparent result of the requested size. An X-coordinate map is
///          precomputed for reuse across destination rows.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @param new_width Positive destination width.
/// @param new_height Positive destination height.
/// @return A distinct caller-owned Pixels handle, or null for non-positive
///         dimensions, map-size overflow, or allocation failure.
void *rt_pixels_scale(void *pixels, int64_t new_width, int64_t new_height) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Scale: null pixels");
    if (!p)
        return NULL;

    if (new_width <= 0 || new_height <= 0)
        return NULL;

    // Handle empty source
    if (p->width <= 0 || p->height <= 0) {
        return pixels_alloc(new_width, new_height);
    }

    rt_pixels_impl *result = pixels_alloc(new_width, new_height);
    if (!result)
        return NULL;

    if ((uint64_t)new_width > (uint64_t)SIZE_MAX / sizeof(int64_t)) {
        if (rt_obj_release_check0(result))
            rt_obj_free(result);
        return NULL;
    }
    int64_t *x_map = (int64_t *)malloc((size_t)new_width * sizeof(*x_map));
    if (!x_map) {
        if (rt_obj_release_check0(result))
            rt_obj_free(result);
        return NULL;
    }
    for (int64_t x = 0; x < new_width; x++)
        x_map[x] = pixels_map_index(x, p->width, new_width);

    // Nearest-neighbor scaling
    for (int64_t y = 0; y < new_height; y++) {
        // Map destination y to source y
        int64_t src_y = pixels_map_index(y, p->height, new_height);

        uint32_t *src_row = p->data + src_y * p->width;
        uint32_t *dst_row = result->data + y * new_width;

        for (int64_t x = 0; x < new_width; x++) {
            dst_row[x] = src_row[x_map[x]];
        }
    }

    free(x_map);
    return result;
}

//=============================================================================
// Image Processing
//=============================================================================

/// @brief Return a copy with every RGB channel inverted.
/// @details Replaces each color channel with `255 - channel` while preserving
///          alpha and dimensions exactly.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @return A distinct caller-owned Pixels handle, or null after layout or
///         allocation failure.
void *rt_pixels_invert(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Invert: null pixels");
    if (!p)
        return NULL;

    int64_t count = 0;
    if (!pixels_transform_checked_layout(p, &count, NULL, NULL)) {
        rt_trap("Pixels.Invert: image dimensions overflow");
        return NULL;
    }
    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result)
        return NULL;

    for (int64_t i = 0; i < count; i++) {
        uint32_t px = p->data[i];
        // Format is 0xRRGGBBAA - invert RGB, keep alpha
        uint8_t r = (px >> 24) & 0xFF;
        uint8_t g = (px >> 16) & 0xFF;
        uint8_t b = (px >> 8) & 0xFF;
        uint8_t a = px & 0xFF;
        result->data[i] = ((uint32_t)(255 - r) << 24) | ((uint32_t)(255 - g) << 16) |
                          ((uint32_t)(255 - b) << 8) | a;
    }

    return result;
}

/// @brief Return a BT.601-luma grayscale copy.
/// @details Computes rounded integer luma as `(77R + 150G + 29B + 128) >> 8`,
///          replicates it to RGB, and preserves source alpha and dimensions.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @return A distinct caller-owned Pixels handle, or null after layout or
///         allocation failure.
void *rt_pixels_grayscale(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Grayscale: null pixels");
    if (!p)
        return NULL;

    int64_t count = 0;
    if (!pixels_transform_checked_layout(p, &count, NULL, NULL)) {
        rt_trap("Pixels.Grayscale: image dimensions overflow");
        return NULL;
    }
    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result)
        return NULL;

    for (int64_t i = 0; i < count; i++) {
        uint32_t px = p->data[i];
        // Format is 0xRRGGBBAA
        uint8_t r = (px >> 24) & 0xFF;
        uint8_t g = (px >> 16) & 0xFF;
        uint8_t b = (px >> 8) & 0xFF;
        uint8_t a = px & 0xFF;

        // Standard grayscale formula: 0.299*R + 0.587*G + 0.114*B
        uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29 + 128) >> 8); /* +128 rounds */
        result->data[i] =
            ((uint32_t)gray << 24) | ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | a;
    }

    return result;
}

/// @brief Return a per-channel multiply-tinted copy.
/// @details Canvas `0x00RRGGBB` supplies opaque tint alpha. Untagged raw
///          `0xRRGGBBAA` and tagged runtime `Color.RGBA` supply all four
///          modulation channels. Each result channel uses integer
///          `(source * tint) / 255`; dimensions are preserved.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @param color Canvas RGB, raw Pixels RGBA, or tagged runtime color.
/// @return A distinct caller-owned Pixels handle, or null after layout or
///         allocation failure.
void *rt_pixels_tint(void *pixels, int64_t color) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Tint: null pixels");
    if (!p)
        return NULL;

    int64_t count = 0;
    if (!pixels_transform_checked_layout(p, &count, NULL, NULL)) {
        rt_trap("Pixels.Tint: image dimensions overflow");
        return NULL;
    }
    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result)
        return NULL;

    int64_t tr = 0;
    int64_t tg = 0;
    int64_t tb = 0;
    int64_t ta = 255;
    uint64_t c = (uint64_t)color;
    if ((c & (uint64_t)RT_PIXELS_COLOR_EXPLICIT_ALPHA_FLAG) != 0 || c > 0x00FFFFFFu) {
        uint32_t tint_rgba = rt_pixels_rgba_or_tagged_color_to_rgba(color);
        tr = (tint_rgba >> 24) & 0xFF;
        tg = (tint_rgba >> 16) & 0xFF;
        tb = (tint_rgba >> 8) & 0xFF;
        ta = tint_rgba & 0xFF;
    } else {
        tr = (color >> 16) & 0xFF;
        tg = (color >> 8) & 0xFF;
        tb = color & 0xFF;
    }

    for (int64_t i = 0; i < count; i++) {
        uint32_t px = p->data[i];
        // Format is 0xRRGGBBAA
        int64_t r = (px >> 24) & 0xFF;
        int64_t g = (px >> 16) & 0xFF;
        int64_t b = (px >> 8) & 0xFF;
        int64_t a = px & 0xFF;

        // Multiply blend: result = (pixel * tint) / 255
        r = (r * tr) / 255;
        g = (g * tg) / 255;
        b = (b * tb) / 255;
        a = (a * ta) / 255;

        result->data[i] = ((uint32_t)(r & 0xFF) << 24) | ((uint32_t)(g & 0xFF) << 16) |
                          ((uint32_t)(b & 0xFF) << 8) | (uint32_t)(a & 0xFF);
    }

    return result;
}

/// @brief Return a separable premultiplied-alpha box-blurred copy.
/// @details Applies a uniform horizontal window followed by a uniform vertical
///          window for `O(width * height * radius)` work. Edge windows average
///          only in-bounds samples. A non-positive radius returns a clone and
///          values above ten clamp to ten. Empty images preserve their
///          dimensions. The temporary horizontal-pass image is always freed.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @param radius Requested half-width of each one-dimensional box window.
/// @return A distinct caller-owned Pixels handle, or null after layout,
///         temporary-storage, or result allocation failure.
void *rt_pixels_blur(void *pixels, int64_t radius) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Blur: null pixels");
    if (!p)
        return NULL;

    if (radius <= 0)
        return rt_pixels_clone(pixels);
    if (radius > 10)
        radius = 10;

    int64_t w = p->width;
    int64_t h = p->height;
    if (w <= 0 || h <= 0)
        return pixels_alloc(w, h);
    if (w > INT64_MAX / h || (uint64_t)(w * h) > SIZE_MAX / sizeof(uint32_t)) {
        rt_trap("Pixels.Blur: dimensions too large");
        return NULL;
    }

    // Separable box blur: horizontal pass → temp, then vertical pass → result.
    // Reduces O(w×h×(2r+1)²) to O(w×h×(2r+1)×2).  Format: 0xRRGGBBAA.
    size_t pixel_count = (size_t)w * (size_t)h;
    uint32_t *tmp = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    if (!tmp)
        return NULL;

    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result) {
        free(tmp);
        return NULL;
    }

    // Horizontal pass: blur each row independently into tmp
    for (int64_t y = 0; y < h; y++) {
        for (int64_t x = 0; x < w; x++) {
            int64_t sum_premul_r = 0, sum_premul_g = 0, sum_premul_b = 0, sum_a = 0;
            int64_t count = 0;
            for (int64_t kdx = -radius; kdx <= radius; kdx++) {
                int64_t sx = x + kdx;
                if (sx >= 0 && sx < w) {
                    uint32_t pixel = p->data[y * w + sx];
                    int64_t a = pixel & 0xFF;
                    sum_premul_r += ((pixel >> 24) & 0xFF) * a;
                    sum_premul_g += ((pixel >> 16) & 0xFF) * a;
                    sum_premul_b += ((pixel >> 8) & 0xFF) * a;
                    sum_a += a;
                    count++;
                }
            }
            if (count > 0)
                tmp[y * w + x] = pixels_average_rgba_premul(
                    sum_premul_r, sum_premul_g, sum_premul_b, sum_a, count);
        }
    }

    // Vertical pass: blur each column from tmp into result (y-outer for cache locality)
    for (int64_t y = 0; y < h; y++) {
        for (int64_t x = 0; x < w; x++) {
            int64_t sum_premul_r = 0, sum_premul_g = 0, sum_premul_b = 0, sum_a = 0;
            int64_t count = 0;
            for (int64_t kdy = -radius; kdy <= radius; kdy++) {
                int64_t sy = y + kdy;
                if (sy >= 0 && sy < h) {
                    uint32_t pixel = tmp[sy * w + x];
                    int64_t a = pixel & 0xFF;
                    sum_premul_r += ((pixel >> 24) & 0xFF) * a;
                    sum_premul_g += ((pixel >> 16) & 0xFF) * a;
                    sum_premul_b += ((pixel >> 8) & 0xFF) * a;
                    sum_a += a;
                    count++;
                }
            }
            if (count > 0)
                result->data[y * w + x] = pixels_average_rgba_premul(
                    sum_premul_r, sum_premul_g, sum_premul_b, sum_a, count);
        }
    }

    free(tmp);
    return result;
}

/// @brief Resize with alpha-correct bilinear or source-footprint filtering.
/// @details When either requested axis is strictly less than integer
///          `source_axis / 2`, each destination pixel averages all source
///          texels in its integer footprint in premultiplied-alpha space.
///          Other downscales and all upscales use endpoint-aligned 8.8
///          fixed-point bilinear interpolation. A source with either empty
///          dimension produces a transparent result of the requested size.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @param new_width Positive destination width.
/// @param new_height Positive destination height.
/// @return A distinct caller-owned Pixels handle, or null for non-positive
///         dimensions or allocation failure.
void *rt_pixels_resize(void *pixels, int64_t new_width, int64_t new_height) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Resize: null pixels");
    if (!p)
        return NULL;

    if (new_width <= 0 || new_height <= 0)
        return NULL;

    // Handle empty source
    if (p->width <= 0 || p->height <= 0) {
        return pixels_alloc(new_width, new_height);
    }

    rt_pixels_impl *result = pixels_alloc(new_width, new_height);
    if (!result)
        return NULL;

    // Large downscale (>2x on either axis): average every source texel in each
    // destination pixel's footprint (area/box filter). A 2x2 bilinear tap misses
    // ~all source pixels when shrinking by a large factor, producing severe aliasing
    // on thumbnails / mip levels. Mild resizes (<=2x, and all upscales) keep the
    // bilinear path so the documented endpoint-preserving behavior is retained.
    // Colors are averaged weighted by alpha (premultiplied) so transparent texels
    // don't bleed color; alpha is a straight average of coverage.
    if (new_width < p->width / 2 || new_height < p->height / 2) {
        for (int64_t y = 0; y < new_height; y++) {
            int64_t sy0 = y * p->height / new_height;
            int64_t sy1 = (y + 1) * p->height / new_height;
            if (sy1 <= sy0)
                sy1 = sy0 + 1;
            if (sy1 > p->height)
                sy1 = p->height;
            for (int64_t x = 0; x < new_width; x++) {
                int64_t sx0 = x * p->width / new_width;
                int64_t sx1 = (x + 1) * p->width / new_width;
                if (sx1 <= sx0)
                    sx1 = sx0 + 1;
                if (sx1 > p->width)
                    sx1 = p->width;

                uint64_t sum_ra = 0, sum_ga = 0, sum_ba = 0, sum_a = 0, count = 0;
                for (int64_t syy = sy0; syy < sy1; syy++) {
                    const uint32_t *row = &p->data[syy * p->width];
                    for (int64_t sxx = sx0; sxx < sx1; sxx++) {
                        uint32_t c = row[sxx];
                        uint64_t a = c & 0xFFu;
                        sum_ra += (uint64_t)((c >> 24) & 0xFFu) * a;
                        sum_ga += (uint64_t)((c >> 16) & 0xFFu) * a;
                        sum_ba += (uint64_t)((c >> 8) & 0xFFu) * a;
                        sum_a += a;
                        count++;
                    }
                }
                uint32_t out;
                if (count == 0) {
                    out = 0;
                } else {
                    uint32_t oa = (uint32_t)(sum_a / count);
                    uint32_t orr = sum_a ? (uint32_t)(sum_ra / sum_a) : 0u;
                    uint32_t ogg = sum_a ? (uint32_t)(sum_ga / sum_a) : 0u;
                    uint32_t obb = sum_a ? (uint32_t)(sum_ba / sum_a) : 0u;
                    out = (orr << 24) | (ogg << 16) | (obb << 8) | oa;
                }
                result->data[y * new_width + x] = out;
            }
        }
        return result;
    }

    // Bilinear interpolation scaling
    for (int64_t y = 0; y < new_height; y++) {
        // Map destination y to source y (with fractional part)
        int64_t src_y_256 = pixels_map_fixed_256(y, p->height, new_height);
        int64_t src_y = src_y_256 >> 8;
        int64_t frac_y = src_y_256 & 0xFF;

        if (src_y >= p->height)
            src_y = p->height - 1;
        if (src_y < 0)
            src_y = 0;
        int64_t sy1 = (src_y + 1 < p->height) ? src_y + 1 : src_y;
        if (src_y >= p->height - 1)
            frac_y = 0;

        for (int64_t x = 0; x < new_width; x++) {
            // Map destination x to source x (with fractional part)
            int64_t src_x_256 = pixels_map_fixed_256(x, p->width, new_width);
            int64_t src_x = src_x_256 >> 8;
            int64_t frac_x = src_x_256 & 0xFF;

            if (src_x >= p->width)
                src_x = p->width - 1;
            if (src_x < 0)
                src_x = 0;
            int64_t sx1 = (src_x + 1 < p->width) ? src_x + 1 : src_x;
            if (src_x >= p->width - 1)
                frac_x = 0;

            // Get four neighboring pixels
            uint32_t p00 = p->data[src_y * p->width + src_x];
            uint32_t p10 = p->data[src_y * p->width + sx1];
            uint32_t p01 = p->data[sy1 * p->width + src_x];
            uint32_t p11 = p->data[sy1 * p->width + sx1];

            result->data[y * new_width + x] =
                pixels_bilerp_rgba_premul_fixed(p00, p10, p01, p11, frac_x, frac_y);
        }
    }

    return result;
}
