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

/// @brief Pack straight RGBA channel values into canonical storage.
static uint32_t pixels_pack_rgba_straight(int64_t r, int64_t g, int64_t b, int64_t a) {
    uint8_t alpha = pixels_clamp_u8_i64(a);
    if (alpha == 0)
        return 0;
    return ((uint32_t)pixels_clamp_u8_i64(r) << 24) | ((uint32_t)pixels_clamp_u8_i64(g) << 16) |
           ((uint32_t)pixels_clamp_u8_i64(b) << 8) | (uint32_t)alpha;
}

/// @brief Bilinear interpolation of four RGBA pixels in premultiplied-alpha space (fixed-point).
/// @details Weights are expressed as fixed-point fractions in [0, 256]; the
///   four bilinear weights sum to 256*256.  Interpolation is done in premultiplied-alpha
///   space to avoid dark-fringe artefacts at transparent boundaries, then the
///   result is unpremultiplied from the unquantized weighted sums.
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

    int64_t weighted_a = a00 * inv_frac_x * inv_frac_y + a10 * frac_x * inv_frac_y +
                         a01 * inv_frac_x * frac_y + a11 * frac_x * frac_y;
    int64_t a = (weighted_a + INT64_C(32768)) >> 16;
    if (a <= 0 || weighted_a <= 0)
        return 0;

    int64_t weighted_r = (((c00 >> 24) & 0xFF) * a00) * inv_frac_x * inv_frac_y +
                         (((c10 >> 24) & 0xFF) * a10) * frac_x * inv_frac_y +
                         (((c01 >> 24) & 0xFF) * a01) * inv_frac_x * frac_y +
                         (((c11 >> 24) & 0xFF) * a11) * frac_x * frac_y;
    int64_t weighted_g = (((c00 >> 16) & 0xFF) * a00) * inv_frac_x * inv_frac_y +
                         (((c10 >> 16) & 0xFF) * a10) * frac_x * inv_frac_y +
                         (((c01 >> 16) & 0xFF) * a01) * inv_frac_x * frac_y +
                         (((c11 >> 16) & 0xFF) * a11) * frac_x * frac_y;
    int64_t weighted_b = (((c00 >> 8) & 0xFF) * a00) * inv_frac_x * inv_frac_y +
                         (((c10 >> 8) & 0xFF) * a10) * frac_x * inv_frac_y +
                         (((c01 >> 8) & 0xFF) * a01) * inv_frac_x * frac_y +
                         (((c11 >> 8) & 0xFF) * a11) * frac_x * frac_y;
    int64_t r = (weighted_r + weighted_a / 2) / weighted_a;
    int64_t g = (weighted_g + weighted_a / 2) / weighted_a;
    int64_t b = (weighted_b + weighted_a / 2) / weighted_a;
    return pixels_pack_rgba_straight(r, g, b, a);
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

    return pixels_pack_rgba_straight((int64_t)(premul_r / a + 0.5),
                                     (int64_t)(premul_g / a + 0.5),
                                     (int64_t)(premul_b / a + 0.5),
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

    int64_t r = (sum_premul_r + sum_a / 2) / sum_a;
    int64_t g = (sum_premul_g + sum_a / 2) / sum_a;
    int64_t b = (sum_premul_b + sum_a / 2) / sum_a;
    return pixels_pack_rgba_straight(r, g, b, a);
}

/// @brief Divide a conceptual unsigned 128-bit product using uint64 operations.
/// @details The direct-product path handles ordinary image dimensions. The
///          binary quotient/remainder path is used only when @p lhs * @p rhs
///          would overflow uint64. This keeps endpoint mapping exact on
///          platforms without compiler-specific 128-bit integers.
/// @param lhs First factor.
/// @param rhs Second factor.
/// @param divisor Positive divisor.
/// @param limit Largest quotient the caller can represent; must be below
///        `UINT64_MAX`.
/// @param quotient Receives the exact floor quotient.
/// @param remainder Receives the exact remainder.
/// @return Nonzero on success, zero for invalid arguments or a quotient above
///         @p limit.
static int8_t pixels_unsigned_mul_div(uint64_t lhs,
                                      uint64_t rhs,
                                      uint64_t divisor,
                                      uint64_t limit,
                                      uint64_t *quotient,
                                      uint64_t *remainder) {
    if (!quotient || !remainder || divisor == 0 || limit == UINT64_MAX)
        return 0;
    if (lhs == 0 || rhs == 0) {
        *quotient = 0;
        *remainder = 0;
        return 1;
    }

    if (lhs <= UINT64_MAX / rhs) {
        uint64_t product = lhs * rhs;
        uint64_t q = product / divisor;
        if (q > limit)
            return 0;
        *quotient = q;
        *remainder = product % divisor;
        return 1;
    }

    uint64_t result_q = 0;
    uint64_t result_r = 0;
    uint64_t term_q = lhs / divisor;
    uint64_t term_r = lhs % divisor;
    const uint64_t capped = limit + 1u;

    while (rhs != 0) {
        if ((rhs & 1u) != 0u) {
            if (term_q > limit - result_q)
                return 0;
            result_q += term_q;
            if (result_r >= divisor - term_r) {
                result_r -= divisor - term_r;
                if (result_q == limit)
                    return 0;
                result_q++;
            } else {
                result_r += term_r;
            }
        }

        rhs >>= 1u;
        if (rhs == 0)
            break;

        uint64_t carry = 0;
        if (term_r >= divisor - term_r) {
            term_r -= divisor - term_r;
            carry = 1;
        } else {
            term_r += term_r;
        }
        if (term_q >= capped || term_q > (capped - carry) / 2u)
            term_q = capped;
        else
            term_q = term_q * 2u + carry;
    }

    *quotient = result_q;
    *remainder = result_r;
    return 1;
}

/// @brief Map a destination index to its nearest endpoint-aligned source index.
/// @details Computes `dst * (src_size - 1) / (dst_size - 1)`, rounds to the
///   closest integer, and clamps to `[0, src_size - 1]`, so the first and last
///   destination pixels exactly sample the first and last source pixels.
///   Used by the nearest-neighbor scale pass (rt_pixels_scale). Integer
///   quotient/remainder arithmetic makes the result platform-independent.
/// @param dst       Destination pixel coordinate (0-based).
/// @param src_size  Source dimension (width or height).
/// @param dst_size  Destination dimension.
/// @return          Source coordinate in [0, src_size - 1].
static int64_t pixels_map_index(int64_t dst, int64_t src_size, int64_t dst_size) {
    if (src_size <= 1 || dst_size <= 1)
        return 0;
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    uint64_t divisor = (uint64_t)(dst_size - 1);
    if (!pixels_unsigned_mul_div((uint64_t)dst,
                                 (uint64_t)(src_size - 1),
                                 divisor,
                                 (uint64_t)(src_size - 1),
                                 &quotient,
                                 &remainder))
        return src_size - 1;
    if (remainder >= divisor / 2u + divisor % 2u && quotient < (uint64_t)(src_size - 1))
        quotient++;
    return (int64_t)quotient;
}

/// @brief Integer and 8-bit fractional parts of an endpoint-aligned sample.
typedef struct pixels_axis_sample {
    int64_t index;
    int64_t fraction;
} pixels_axis_sample;

/// @brief Map a destination index to an endpoint-aligned bilinear source sample.
/// @details Computes the integer quotient and then scales only the remainder
///   into `[0, 255]`. Unlike a combined 8.8 coordinate, this representation
///   cannot overflow when the source index itself approaches `INT64_MAX`.
/// @param dst       Destination pixel coordinate (0-based).
/// @param src_size  Source dimension.
/// @param dst_size  Destination dimension.
/// @return          Source index and 8-bit fractional weight.
static pixels_axis_sample pixels_map_sample(int64_t dst, int64_t src_size, int64_t dst_size) {
    pixels_axis_sample sample = {0, 0};
    if (src_size <= 1 || dst_size <= 1)
        return sample;

    uint64_t quotient = 0;
    uint64_t remainder = 0;
    uint64_t divisor = (uint64_t)(dst_size - 1);
    if (!pixels_unsigned_mul_div((uint64_t)dst,
                                 (uint64_t)(src_size - 1),
                                 divisor,
                                 (uint64_t)(src_size - 1),
                                 &quotient,
                                 &remainder)) {
        sample.index = src_size - 1;
        return sample;
    }
    sample.index = (int64_t)quotient;
    if (sample.index >= src_size - 1)
        return sample;

    uint64_t fraction = 0;
    uint64_t unused = 0;
    if (pixels_unsigned_mul_div(remainder, 256u, divisor, 255u, &fraction, &unused))
        sample.fraction = (int64_t)fraction;
    return sample;
}

/// @brief Map an endpoint boundary with exact floor division.
/// @param dst Boundary index in `[0, dst_size]`.
/// @param src_size Positive source dimension.
/// @param dst_size Positive destination dimension.
/// @return `floor(dst * src_size / dst_size)`, clamped to @p src_size.
static int64_t pixels_map_boundary(int64_t dst, int64_t src_size, int64_t dst_size) {
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    if (dst <= 0 || src_size <= 0 || dst_size <= 0)
        return 0;
    if (dst >= dst_size)
        return src_size;
    if (!pixels_unsigned_mul_div((uint64_t)dst,
                                 (uint64_t)src_size,
                                 (uint64_t)dst_size,
                                 (uint64_t)src_size,
                                 &quotient,
                                 &remainder))
        return src_size;
    return (int64_t)quotient;
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

    pixels_copy_alpha_classification_cache(result, p);
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

    size_t row_bytes = 0;
    if (!pixels_transform_checked_layout(p, NULL, NULL, &row_bytes)) {
        rt_trap("Pixels.FlipV: image dimensions overflow");
        return NULL;
    }
    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result)
        return NULL;
    if (p->width <= 0 || p->height <= 0)
        return result;

    for (int64_t y = 0; y < p->height; y++) {
        memcpy(&result->data[(p->height - 1 - y) * p->width], &p->data[y * p->width], row_bytes);
    }

    pixels_copy_alpha_classification_cache(result, p);
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
    if (new_width <= 0 || new_height <= 0)
        return result;

    // Rotate 90 CW: emit each destination row contiguously.
    for (int64_t dst_y = 0; dst_y < new_height; dst_y++) {
        uint32_t *dst_row = result->data + dst_y * new_width;
        for (int64_t dst_x = 0; dst_x < new_width; dst_x++) {
            dst_row[dst_x] = p->data[(p->height - 1 - dst_x) * p->width + dst_y];
        }
    }

    pixels_copy_alpha_classification_cache(result, p);
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
    if (new_width <= 0 || new_height <= 0)
        return result;

    // Rotate 90 CCW: emit each destination row contiguously.
    for (int64_t dst_y = 0; dst_y < new_height; dst_y++) {
        uint32_t *dst_row = result->data + dst_y * new_width;
        int64_t src_x = p->width - 1 - dst_y;
        for (int64_t dst_x = 0; dst_x < new_width; dst_x++) {
            dst_row[dst_x] = p->data[dst_x * p->width + src_x];
        }
    }

    pixels_copy_alpha_classification_cache(result, p);
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

    // Rotate 180: src[x,y] -> dst[width-1-x, height-1-y]
    int64_t total = 0;
    if (!pixels_transform_checked_layout(p, &total, NULL, NULL)) {
        rt_trap("Pixels.Rotate180: image dimensions overflow");
        return NULL;
    }
    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result)
        return NULL;
    if (p->width <= 0 || p->height <= 0)
        return result;

    for (int64_t i = 0; i < total; i++) {
        result->data[total - 1 - i] = p->data[i];
    }

    pixels_copy_alpha_classification_cache(result, p);
    return result;
}

/// @brief Return a copy rotated clockwise by an arbitrary angle.
/// @details Normalizes finite @p angle_degrees modulo 360. Exact cardinal
///          orientations use exact copy or quarter-turn paths. Other angles
///          allocate the ceiling-rounded
///          axis-aligned bounds of the rotated pixel-center rectangle and
///          inverse-map each destination sample with premultiplied-alpha
///          bilinear interpolation; source neighbors beyond the image are
///          transparent black. Empty images preserve their dimensions; exact
///          quarter turns swap them.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @param angle_degrees Clockwise angle in degrees; non-finite input traps.
/// @return A distinct caller-owned Pixels handle, or null when validation,
///         dimension calculation, or allocation fails.
void *rt_pixels_rotate(void *pixels, double angle_degrees) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Rotate: null pixels");
    if (!p)
        return NULL;

    if (!isfinite(angle_degrees)) {
        rt_trap("Pixels.Rotate: non-finite angle");
        return NULL;
    }

    // Normalize angle to [0, 360) without unbounded loops for huge inputs.
    angle_degrees = fmod(angle_degrees, 360.0);
    if (angle_degrees < 0.0)
        angle_degrees += 360.0;

    // Fast paths for common angles
    if (angle_degrees == 0.0)
        return rt_pixels_clone(pixels);
    if (angle_degrees == 90.0)
        return rt_pixels_rotate_cw(pixels);
    if (angle_degrees == 180.0)
        return rt_pixels_rotate_180(pixels);
    if (angle_degrees == 270.0)
        return rt_pixels_rotate_ccw(pixels);
    if (p->width <= 0 || p->height <= 0)
        return pixels_alloc(p->width, p->height);

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

            // Reject coordinates before converting to int64_t. A floating
            // value outside int64_t's range must never reach a C cast.
            if (sx <= -1.0 || sx >= (double)p->width || sy <= -1.0 || sy >= (double)p->height)
                continue;

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
    if (new_width == p->width && new_height == p->height)
        return rt_pixels_clone(pixels);

    // Handle empty source
    if (p->width <= 0 || p->height <= 0) {
        return pixels_alloc(new_width, new_height);
    }

    rt_pixels_impl *result = pixels_alloc(new_width, new_height);
    if (!result)
        return NULL;

    int64_t *x_map = NULL;
    if ((uint64_t)new_width <= (uint64_t)SIZE_MAX / sizeof(*x_map)) {
        x_map = (int64_t *)malloc((size_t)new_width * sizeof(*x_map));
        if (x_map) {
            for (int64_t x = 0; x < new_width; x++)
                x_map[x] = pixels_map_index(x, p->width, new_width);
        }
    }

    // Nearest-neighbor scaling
    for (int64_t y = 0; y < new_height; y++) {
        // Map destination y to source y
        int64_t src_y = pixels_map_index(y, p->height, new_height);

        uint32_t *src_row = p->data + src_y * p->width;
        uint32_t *dst_row = result->data + y * new_width;

        for (int64_t x = 0; x < new_width; x++) {
            int64_t src_x = x_map ? x_map[x] : pixels_map_index(x, p->width, new_width);
            dst_row[x] = src_row[src_x];
        }
    }

    free(x_map);
    pixels_copy_alpha_classification_cache(result, p);
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

    pixels_copy_alpha_classification_cache(result, p);
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

    pixels_copy_alpha_classification_cache(result, p);
    return result;
}

/// @brief Return a per-channel multiply-tinted copy.
/// @details Canvas `0x00RRGGBB` supplies opaque tint alpha. Untagged raw
///          `0xRRGGBBAA` and tagged runtime `Color.RGBA` supply all four
///          modulation channels. Each result channel uses rounded integer
///          `(source * tint + 127) / 255`; dimensions are preserved.
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

        // Multiply blend with unbiased nearest-integer rounding.
        r = (r * tr + 127) / 255;
        g = (g * tg + 127) / 255;
        b = (b * tb + 127) / 255;
        a = (a * ta + 127) / 255;

        result->data[i] = ((uint32_t)(r & 0xFF) << 24) | ((uint32_t)(g & 0xFF) << 16) |
                          ((uint32_t)(b & 0xFF) << 8) | (uint32_t)(a & 0xFF);
    }

    if (ta == 255) {
        pixels_copy_alpha_classification_cache(result, p);
    } else if (ta == 0) {
        result->alpha_scan_generation = result->generation;
        result->alpha_scan_classification =
            count > 0 ? RT_PIXELS_ALPHA_BINARY : RT_PIXELS_ALPHA_OPAQUE;
        result->alpha_scan_valid = 1;
    } else if (p->alpha_scan_valid && p->alpha_scan_generation == p->generation &&
               p->alpha_scan_classification == RT_PIXELS_ALPHA_OPAQUE) {
        result->alpha_scan_generation = result->generation;
        result->alpha_scan_classification = RT_PIXELS_ALPHA_FRACTIONAL;
        result->alpha_scan_valid = 1;
    }
    return result;
}

/// @brief Running premultiplied-alpha sums for one blur window.
typedef struct pixels_blur_accumulator {
    int64_t premul_r;
    int64_t premul_g;
    int64_t premul_b;
    int64_t alpha;
    int64_t count;
} pixels_blur_accumulator;

/// @brief Add or remove one RGBA sample from a blur accumulator.
/// @param accumulator Window state to update.
/// @param pixel Canonical raw RGBA sample.
/// @param direction `1` to add or `-1` to remove.
static void pixels_blur_accumulate(pixels_blur_accumulator *accumulator,
                                   uint32_t pixel,
                                   int64_t direction) {
    int64_t alpha = pixel & 0xFFu;
    accumulator->premul_r += direction * (int64_t)((pixel >> 24) & 0xFFu) * alpha;
    accumulator->premul_g += direction * (int64_t)((pixel >> 16) & 0xFFu) * alpha;
    accumulator->premul_b += direction * (int64_t)((pixel >> 8) & 0xFFu) * alpha;
    accumulator->alpha += direction * alpha;
    accumulator->count += direction;
}

/// @brief Add or remove a complete one-dimensional window from a second-pass accumulator.
/// @details Keeping the horizontal sums in their original precision avoids quantizing to
///          straight-alpha RGBA between the two separable passes. The accumulated count is
///          the exact number of source texels in the rectangular two-dimensional footprint.
/// @param accumulator Mutable vertical-window state.
/// @param window Borrowed horizontal-window sums.
/// @param direction `1` to add or `-1` to remove.
static void pixels_blur_accumulate_window(pixels_blur_accumulator *accumulator,
                                          const pixels_blur_accumulator *window,
                                          int64_t direction) {
    accumulator->premul_r += direction * window->premul_r;
    accumulator->premul_g += direction * window->premul_g;
    accumulator->premul_b += direction * window->premul_b;
    accumulator->alpha += direction * window->alpha;
    accumulator->count += direction * window->count;
}

/// @brief Add or remove every horizontal blur window from one source row.
/// @details Recomputing a row when it enters or leaves the vertical window
///          keeps the exact unquantized two-dimensional sums while limiting
///          auxiliary storage to one accumulator per column. Each source row
///          is evaluated at most twice, so the blur remains linear in the
///          number of pixels.
/// @param pixels Valid nonempty source Pixels implementation.
/// @param y Source row index.
/// @param radius Positive blur radius.
/// @param columns Mutable vertical accumulator for every destination column.
/// @param direction `1` to add the row or `-1` to remove it.
static void pixels_blur_accumulate_source_row(const rt_pixels_impl *pixels,
                                              int64_t y,
                                              int64_t radius,
                                              pixels_blur_accumulator *columns,
                                              int64_t direction) {
    int64_t width = pixels->width;
    const uint32_t *source_row = pixels->data + y * width;
    pixels_blur_accumulator window = {0, 0, 0, 0, 0};
    int64_t initial_right = radius < width - 1 ? radius : width - 1;
    for (int64_t source_x = 0; source_x <= initial_right; source_x++)
        pixels_blur_accumulate(&window, source_row[source_x], 1);

    for (int64_t x = 0; x < width; x++) {
        pixels_blur_accumulate_window(&columns[x], &window, direction);

        int64_t remove_x = x - radius;
        if (remove_x >= 0)
            pixels_blur_accumulate(&window, source_row[remove_x], -1);
        if (radius < width - 1 - x) {
            int64_t add_x = x + radius + 1;
            pixels_blur_accumulate(&window, source_row[add_x], 1);
        }
    }
}

/// @brief Convert one blur window accumulator to straight-alpha RGBA.
static uint32_t pixels_blur_average(const pixels_blur_accumulator *accumulator) {
    return pixels_average_rgba_premul(accumulator->premul_r,
                                      accumulator->premul_g,
                                      accumulator->premul_b,
                                      accumulator->alpha,
                                      accumulator->count);
}

/// @brief Return a separable premultiplied-alpha box-blurred copy.
/// @details Applies a uniform horizontal window followed by a uniform vertical
///          sliding window for `O(width * height)` work. Edge windows average
///          only in-bounds samples. A non-positive radius returns a clone;
///          values above ten clamp to ten. Empty images preserve their
///          dimensions. Only one column accumulator per source column is
///          retained, and that temporary state is always freed.
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
    if ((uint64_t)w > (uint64_t)SIZE_MAX / sizeof(pixels_blur_accumulator)) {
        rt_trap("Pixels.Blur: dimensions too large");
        return NULL;
    }

    // Keep exact unquantized column sums. Horizontal windows are recomputed as
    // source rows enter or leave the vertical footprint, avoiding a 40-byte
    // accumulator for every source pixel.
    pixels_blur_accumulator *columns =
        (pixels_blur_accumulator *)calloc((size_t)w, sizeof(*columns));
    if (!columns)
        return NULL;

    rt_pixels_impl *result = pixels_alloc(p->width, p->height);
    if (!result) {
        free(columns);
        return NULL;
    }

    // Seed all vertical column windows for the first destination row.
    int64_t initial_bottom = radius < h - 1 ? radius : h - 1;
    for (int64_t source_y = 0; source_y <= initial_bottom; source_y++)
        pixels_blur_accumulate_source_row(p, source_y, radius, columns, 1);

    // Vertical pass: update every column accumulator in row-major order.
    for (int64_t y = 0; y < h; y++) {
        uint32_t *dst_row = result->data + y * w;
        for (int64_t x = 0; x < w; x++)
            dst_row[x] = pixels_blur_average(&columns[x]);

        int64_t remove_y = y - radius;
        if (remove_y >= 0)
            pixels_blur_accumulate_source_row(p, remove_y, radius, columns, -1);
        if (radius < h - 1 - y) {
            int64_t add_y = y + radius + 1;
            pixels_blur_accumulate_source_row(p, add_y, radius, columns, 1);
        }
    }

    free(columns);
    if (p->alpha_scan_valid && p->alpha_scan_generation == p->generation &&
        p->alpha_scan_classification == RT_PIXELS_ALPHA_OPAQUE)
        pixels_copy_alpha_classification_cache(result, p);
    return result;
}

/// @brief Return whether shrinking @p source to @p destination exceeds 2:1.
/// @details Uses `destination < ceil(source / 2)` instead of multiplying the
///          destination by two, which would overflow near `INT64_MAX`.
static int8_t pixels_is_heavy_downscale(int64_t source, int64_t destination) {
    if (source <= 0 || destination <= 0 || destination >= source)
        return 0;
    return destination < source / 2 + source % 2;
}

/// @brief Release an internal transform result after a later stage fails.
static void pixels_release_transform_result(rt_pixels_impl *result) {
    if (result && rt_obj_release_check0(result))
        rt_obj_free(result);
}

/// @brief Bilinearly resize a known-valid, nonempty source.
/// @details X-axis samples are cached when auxiliary memory is available.
///          Failure to allocate that optional map falls back to exact on-demand
///          mapping instead of failing an otherwise valid resize.
static rt_pixels_impl *pixels_resize_bilinear(const rt_pixels_impl *p,
                                              int64_t new_width,
                                              int64_t new_height) {
    rt_pixels_impl *result = pixels_alloc(new_width, new_height);
    if (!result)
        return NULL;

    pixels_axis_sample *x_map = NULL;
    if ((uint64_t)new_width <= (uint64_t)SIZE_MAX / sizeof(*x_map)) {
        x_map = (pixels_axis_sample *)malloc((size_t)new_width * sizeof(*x_map));
        if (x_map) {
            for (int64_t x = 0; x < new_width; x++)
                x_map[x] = pixels_map_sample(x, p->width, new_width);
        }
    }

    for (int64_t y = 0; y < new_height; y++) {
        pixels_axis_sample y_sample = pixels_map_sample(y, p->height, new_height);
        int64_t src_y = y_sample.index;
        int64_t sy1 = src_y < p->height - 1 ? src_y + 1 : src_y;
        const uint32_t *row0 = p->data + src_y * p->width;
        const uint32_t *row1 = p->data + sy1 * p->width;
        uint32_t *dst_row = result->data + y * new_width;

        for (int64_t x = 0; x < new_width; x++) {
            pixels_axis_sample x_sample =
                x_map ? x_map[x] : pixels_map_sample(x, p->width, new_width);
            int64_t src_x = x_sample.index;
            int64_t sx1 = src_x < p->width - 1 ? src_x + 1 : src_x;
            dst_row[x] = pixels_bilerp_rgba_premul_fixed(row0[src_x],
                                                         row0[sx1],
                                                         row1[src_x],
                                                         row1[sx1],
                                                         x_sample.fraction,
                                                         y_sample.fraction);
        }
    }

    free(x_map);
    return result;
}

/// @brief Area-filter one or both axes of a nonempty source.
/// @details Each output dimension is no larger than its source counterpart.
///          Exact integer boundary mapping prevents multiplication overflow.
///          Accumulator bounds are checked before any output is allocated.
static rt_pixels_impl *pixels_resize_area(const rt_pixels_impl *p,
                                          int64_t new_width,
                                          int64_t new_height) {
    uint64_t max_x_span = (uint64_t)(p->width / new_width + (p->width % new_width != 0));
    uint64_t max_y_span = (uint64_t)(p->height / new_height + (p->height % new_height != 0));
    const uint64_t max_safe_samples = UINT64_MAX / UINT64_C(65153);
    if (max_x_span > max_safe_samples / max_y_span) {
        rt_trap("Pixels.Resize: source footprint too large");
        return NULL;
    }

    rt_pixels_impl *result = pixels_alloc(new_width, new_height);
    if (!result)
        return NULL;

    for (int64_t y = 0; y < new_height; y++) {
        int64_t sy0 = pixels_map_boundary(y, p->height, new_height);
        int64_t sy1 = pixels_map_boundary(y + 1, p->height, new_height);
        for (int64_t x = 0; x < new_width; x++) {
            int64_t sx0 = pixels_map_boundary(x, p->width, new_width);
            int64_t sx1 = pixels_map_boundary(x + 1, p->width, new_width);
            uint64_t sum_ra = 0;
            uint64_t sum_ga = 0;
            uint64_t sum_ba = 0;
            uint64_t sum_a = 0;
            uint64_t count = 0;

            for (int64_t sy = sy0; sy < sy1; sy++) {
                const uint32_t *row = p->data + sy * p->width;
                for (int64_t sx = sx0; sx < sx1; sx++) {
                    uint32_t color = row[sx];
                    uint64_t alpha = color & 0xFFu;
                    sum_ra += (uint64_t)((color >> 24) & 0xFFu) * alpha;
                    sum_ga += (uint64_t)((color >> 16) & 0xFFu) * alpha;
                    sum_ba += (uint64_t)((color >> 8) & 0xFFu) * alpha;
                    sum_a += alpha;
                    count++;
                }
            }

            uint32_t output = 0;
            if (count > 0 && sum_a > 0) {
                uint32_t alpha = (uint32_t)((sum_a + count / 2u) / count);
                uint32_t red = (uint32_t)((sum_ra + sum_a / 2u) / sum_a);
                uint32_t green = (uint32_t)((sum_ga + sum_a / 2u) / sum_a);
                uint32_t blue = (uint32_t)((sum_ba + sum_a / 2u) / sum_a);
                output = (red << 24) | (green << 16) | (blue << 8) | alpha;
            }
            result->data[y * new_width + x] = output;
        }
    }
    return result;
}

/// @brief Resize with alpha-correct bilinear or source-footprint filtering.
/// @details Axes shrinking by more than 2:1 are area-filtered first. Any other
///          axis is then resized with endpoint-aligned bilinear interpolation,
///          so a large horizontal shrink does not degrade a simultaneous
///          vertical upscale (and vice versa). All mapping uses deterministic
///          integer quotient/remainder arithmetic. A source with either empty
///          dimension produces a transparent result of the requested size.
/// @param pixels Opaque source Pixels handle; null or invalid input traps.
/// @param new_width Positive destination width.
/// @param new_height Positive destination height.
/// @return A distinct caller-owned Pixels handle, or null for non-positive
///         dimensions, unsafe footprints, or allocation failure.
void *rt_pixels_resize(void *pixels, int64_t new_width, int64_t new_height) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Resize: null pixels");
    if (!p)
        return NULL;
    if (new_width <= 0 || new_height <= 0)
        return NULL;
    if (new_width == p->width && new_height == p->height)
        return rt_pixels_clone(pixels);
    if (p->width <= 0 || p->height <= 0)
        return pixels_alloc(new_width, new_height);

    int8_t area_x = pixels_is_heavy_downscale(p->width, new_width);
    int8_t area_y = pixels_is_heavy_downscale(p->height, new_height);
    if (!area_x && !area_y)
        return pixels_resize_bilinear(p, new_width, new_height);

    int64_t area_width = area_x ? new_width : p->width;
    int64_t area_height = area_y ? new_height : p->height;
    rt_pixels_impl *area_result = pixels_resize_area(p, area_width, area_height);
    if (!area_result)
        return NULL;
    if (area_width == new_width && area_height == new_height)
        return area_result;

    rt_pixels_impl *result = pixels_resize_bilinear(area_result, new_width, new_height);
    pixels_release_transform_result(area_result);
    return result;
}

/// @brief Blend bright texels toward a team color, keeping shadows and skin.
/// @details Native port of the luminance-masked uniform tint (overhaul G5a):
///          per texel, integer Rec.601-ish luma = (77R + 150G + 29B) / 256;
///          texels at or above @p lum_lo blend toward @p rgb by @p strength,
///          ramped linearly between @p lum_lo and @p lum_hi so mid-tones
///          feather instead of banding. Alpha is preserved. The interpreted
///          per-pixel loop this replaces cost ~4.2M VM iterations per 2048
///          square texture at game startup.
/// @param pixels Borrowed Pixels handle mutated in place.
/// @param rgb Packed 0xRRGGBB target color.
/// @param strength Blend strength at full mask, clamped to [0, 1].
/// @param lum_lo Luma at which the mask begins (0..255).
/// @param lum_hi Luma at which the mask reaches full strength (> lum_lo).
/// Recolor texels whose color sits within `tolerance` (Euclidean RGB
/// distance) of `ref_rgb` toward `target_rgb`, preserving each texel's
/// own luminance relative to the reference — a shaded navy jersey
/// becomes a shaded club-color jersey, not a flat decal. Distance alone
/// cannot separate a dark color class from black/brown neighbors, so a
/// texel must also share the reference's chroma direction: its
/// ref-dominant channel must exceed its ref-weakest channel by at least
/// 6 (skipped for near-neutral references). The mask holds full
/// strength inside tolerance/2 and ramps to zero at tolerance so
/// UV-island edges blend. Alpha untouched.
void rt_pixels_recolor_masked(
    void *pixels, int64_t target_rgb, int64_t ref_rgb, int64_t tolerance) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.RecolorMasked: null pixels");
    int64_t count;
    if (!p || !p->data)
        return;
    if (tolerance < 1)
        tolerance = 1;
    {
        const int64_t rr = (ref_rgb >> 16) & 0xFF;
        const int64_t rg = (ref_rgb >> 8) & 0xFF;
        const int64_t rb = ref_rgb & 0xFF;
        const int64_t tr = (target_rgb >> 16) & 0xFF;
        const int64_t tg = (target_rgb >> 8) & 0xFF;
        const int64_t tb = target_rgb & 0xFF;
        int64_t ref_lum = (rr * 77 + rg * 150 + rb * 29) / 256;
        const int64_t tol2 = tolerance * tolerance;
        const int64_t inner2 = (tolerance / 2) * (tolerance / 2);
        /* Chroma-direction gate: channel index 0=R 1=G 2=B of the
           reference's strongest and weakest channels. */
        int dom = 0;
        int weak = 0;
        {
            const int64_t rc[3] = {rr, rg, rb};
            for (int c = 1; c < 3; c++) {
                if (rc[c] > rc[dom])
                    dom = c;
                if (rc[c] < rc[weak])
                    weak = c;
            }
            if (rc[dom] - rc[weak] < 6)
                dom = weak; /* near-neutral ref: gate disabled */
        }
        if (ref_lum < 1)
            ref_lum = 1;
        count = p->width * p->height;
        for (int64_t i = 0; i < count; i++) {
            uint32_t texel = p->data[i]; /* raw 0xRRGGBBAA */
            int64_t pr = (texel >> 24) & 0xFF;
            int64_t pg = (texel >> 16) & 0xFF;
            int64_t pb = (texel >> 8) & 0xFF;
            uint32_t pa = texel & 0xFF;
            int64_t dr = pr - rr;
            int64_t dg = pg - rg;
            int64_t db = pb - rb;
            int64_t dist2 = dr * dr + dg * dg + db * db;
            double a;
            double shade;
            int64_t lum;
            int64_t nr;
            int64_t ng;
            int64_t nb;
            if (dist2 > tol2)
                continue;
            if (dom != weak) {
                const int64_t pc[3] = {pr, pg, pb};
                if (pc[dom] < pc[weak] + 6)
                    continue;
            }
            a = 1.0;
            if (dist2 > inner2 && tol2 > inner2)
                a = (double)(tol2 - dist2) / (double)(tol2 - inner2);
            lum = (pr * 77 + pg * 150 + pb * 29) / 256;
            shade = (double)lum / (double)ref_lum;
            if (shade > 1.5)
                shade = 1.5;
            nr = (int64_t)((double)pr * (1.0 - a) + (double)tr * shade * a);
            ng = (int64_t)((double)pg * (1.0 - a) + (double)tg * shade * a);
            nb = (int64_t)((double)pb * (1.0 - a) + (double)tb * shade * a);
            if (nr > 255)
                nr = 255;
            if (ng > 255)
                ng = 255;
            if (nb > 255)
                nb = 255;
            p->data[i] = ((uint32_t)nr << 24) | ((uint32_t)ng << 16) |
                         ((uint32_t)nb << 8) | pa;
        }
    }
    pixels_touch(p);
}

/// @brief Grow covered texels outward into uncovered gutters (see rt_pixels.h).
/// One frontier byte-buffer per pass keeps growth uniform: a texel covered
/// this pass never feeds another texel in the same pass.
void rt_pixels_dilate_masked(void *pixels, void *mask, int64_t passes) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DilateMasked: null pixels");
    rt_pixels_impl *m = rt_pixels_checked_impl(mask, "Pixels.DilateMasked: null mask");
    int64_t w;
    int64_t h;
    int64_t count;
    uint8_t *cov;
    uint8_t *next;
    if (!p || !p->data || !m || !m->data)
        return;
    if (p->width != m->width || p->height != m->height)
        return;
    if (passes <= 0)
        return;
    if (passes > 256)
        passes = 256;
    w = p->width;
    h = p->height;
    count = w * h;
    if (count <= 0)
        return;
    cov = (uint8_t *)malloc((size_t)count);
    next = (uint8_t *)malloc((size_t)count);
    if (!cov || !next) {
        free(cov);
        free(next);
        return;
    }
    for (int64_t i = 0; i < count; i++)
        cov[i] = (m->data[i] & 0xFFFFFF00u) != 0u ? 1u : 0u;
    for (int64_t pass = 0; pass < passes; pass++) {
        int64_t grew = 0;
        memcpy(next, cov, (size_t)count);
        for (int64_t y = 0; y < h; y++) {
            for (int64_t x = 0; x < w; x++) {
                int64_t idx = y * w + x;
                int64_t sr = 0;
                int64_t sg = 0;
                int64_t sb = 0;
                int64_t sa = 0;
                int64_t n = 0;
                if (cov[idx])
                    continue;
                for (int64_t dy = -1; dy <= 1; dy++) {
                    for (int64_t dx = -1; dx <= 1; dx++) {
                        int64_t nx = x + dx;
                        int64_t ny = y + dy;
                        int64_t nidx;
                        uint32_t texel;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h)
                            continue;
                        nidx = ny * w + nx;
                        if (!cov[nidx])
                            continue;
                        texel = p->data[nidx];
                        sr += (int64_t)((texel >> 24) & 0xFFu);
                        sg += (int64_t)((texel >> 16) & 0xFFu);
                        sb += (int64_t)((texel >> 8) & 0xFFu);
                        sa += (int64_t)(texel & 0xFFu);
                        n++;
                    }
                }
                if (n == 0)
                    continue;
                p->data[idx] = ((uint32_t)(sr / n) << 24) | ((uint32_t)(sg / n) << 16) |
                               ((uint32_t)(sb / n) << 8) | (uint32_t)(sa / n);
                m->data[idx] = 0xFFFFFFFFu;
                next[idx] = 1u;
                grew = 1;
            }
        }
        memcpy(cov, next, (size_t)count);
        if (!grew)
            break;
    }
    free(cov);
    free(next);
    pixels_touch(p);
    pixels_touch(m);
}

/// @brief Grow covered texels into gutters by exact nearest-owner copy
///   (see rt_pixels.h).
/// Ring-synchronous multi-source growth over frontier lists: every ring's
/// claims read only the PREVIOUS ring's coverage, and a claimed texel copies
/// the exact RGBA of its first covered neighbor in the fixed (dy,dx) scan
/// order — so the result is deterministic regardless of candidate
/// processing order, values never average, and a binary label map grown
/// alongside an albedo shares its exact watershed topology.
void rt_pixels_dilate_owner(void *pixels, void *mask, int64_t passes) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.DilateOwner: null pixels");
    rt_pixels_impl *m = rt_pixels_checked_impl(mask, "Pixels.DilateOwner: null mask");
    int64_t w;
    int64_t h;
    int64_t count;
    uint8_t *cov;
    uint8_t *pending;
    int32_t *cand;
    int32_t *claimed;
    int64_t cand_n;
    int64_t claimed_n;
    if (!p || !p->data || !m || !m->data)
        return;
    if (p->width != m->width || p->height != m->height)
        return;
    w = p->width;
    h = p->height;
    count = w * h;
    if (count <= 0 || count > 0x7FFFFFFF)
        return;
    if (passes <= 0 || passes > count)
        passes = count; /* convergence bound: one ring can never exceed count */
    cov = (uint8_t *)malloc((size_t)count);
    pending = (uint8_t *)malloc((size_t)count);
    cand = (int32_t *)malloc((size_t)count * sizeof(int32_t));
    claimed = (int32_t *)malloc((size_t)count * sizeof(int32_t));
    if (!cov || !pending || !cand || !claimed) {
        free(cov);
        free(pending);
        free(cand);
        free(claimed);
        return;
    }
    memset(pending, 0, (size_t)count);
    for (int64_t i = 0; i < count; i++)
        cov[i] = (m->data[i] & 0xFFFFFF00u) != 0u ? 1u : 0u;
    /* Seed candidates: every uncovered texel with a covered 8-neighbor. */
    cand_n = 0;
    for (int64_t y = 0; y < h; y++) {
        for (int64_t x = 0; x < w; x++) {
            int64_t idx = y * w + x;
            int found = 0;
            if (cov[idx])
                continue;
            for (int64_t dy = -1; dy <= 1 && !found; dy++) {
                for (int64_t dx = -1; dx <= 1; dx++) {
                    int64_t nx = x + dx;
                    int64_t ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    if (cov[ny * w + nx]) {
                        found = 1;
                        break;
                    }
                }
            }
            if (found) {
                pending[idx] = 1u;
                cand[cand_n++] = (int32_t)idx;
            }
        }
    }
    for (int64_t pass = 0; pass < passes && cand_n > 0; pass++) {
        claimed_n = 0;
        /* Resolve every candidate against the previous ring's coverage.
           Each candidate independently picks its FIRST covered neighbor in
           the fixed scan order, so processing order cannot matter. */
        for (int64_t c = 0; c < cand_n; c++) {
            int64_t idx = cand[c];
            int64_t x = idx % w;
            int64_t y = idx / w;
            int done = 0;
            for (int64_t dy = -1; dy <= 1 && !done; dy++) {
                for (int64_t dx = -1; dx <= 1; dx++) {
                    int64_t nx = x + dx;
                    int64_t ny = y + dy;
                    int64_t nidx;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    nidx = ny * w + nx;
                    if (!cov[nidx])
                        continue;
                    p->data[idx] = p->data[nidx];
                    m->data[idx] = 0xFFFFFFFFu;
                    claimed[claimed_n++] = (int32_t)idx;
                    done = 1;
                    break;
                }
            }
            pending[idx] = 0u;
        }
        /* Ring barrier: coverage advances only after every claim resolved. */
        for (int64_t c = 0; c < claimed_n; c++)
            cov[claimed[c]] = 1u;
        /* Next ring's candidates: uncovered neighbors of this ring's claims. */
        cand_n = 0;
        for (int64_t c = 0; c < claimed_n; c++) {
            int64_t idx = claimed[c];
            int64_t x = idx % w;
            int64_t y = idx / w;
            for (int64_t dy = -1; dy <= 1; dy++) {
                for (int64_t dx = -1; dx <= 1; dx++) {
                    int64_t nx = x + dx;
                    int64_t ny = y + dy;
                    int64_t nidx;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    nidx = ny * w + nx;
                    if (cov[nidx] || pending[nidx])
                        continue;
                    pending[nidx] = 1u;
                    cand[cand_n++] = (int32_t)nidx;
                }
            }
        }
    }
    free(cov);
    free(pending);
    free(cand);
    free(claimed);
    pixels_touch(p);
    pixels_touch(m);
}

/// @brief Mask-scoped shade-preserving colorize (see rt_pixels.h).
/// rt_pixels_recolor_masked's interior formula with the color-class gates
/// replaced by an explicit mask, an explicit reference luminance, and an
/// explicit shade clamp — dark authored regions reach bright targets.
void rt_pixels_colorize_masked(void *pixels, void *mask, int64_t rgb, int64_t ref_lum,
                               double max_shade, double strength) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.ColorizeMasked: null pixels");
    rt_pixels_impl *mk = rt_pixels_checked_impl(mask, "Pixels.ColorizeMasked: null mask");
    int64_t tr;
    int64_t tg;
    int64_t tb;
    if (!p || !p->data || !mk || !mk->data)
        return;
    if (mk->width <= 0 || mk->height <= 0)
        return;
    if (!isfinite(strength))
        return;
    if (strength < 0.0)
        strength = 0.0;
    if (strength > 1.0)
        strength = 1.0;
    if (ref_lum < 1)
        ref_lum = 1;
    if (!isfinite(max_shade) || max_shade <= 0.0)
        max_shade = 1.5; /* rt_pixels_recolor_masked parity */
    if (max_shade > 16.0)
        max_shade = 16.0;
    tr = (rgb >> 16) & 0xFF;
    tg = (rgb >> 8) & 0xFF;
    tb = rgb & 0xFF;
    for (int64_t y = 0; y < p->height; y++) {
        int64_t my = y * mk->height / p->height;
        for (int64_t x = 0; x < p->width; x++) {
            int64_t mx = x * mk->width / p->width;
            uint32_t mtex = mk->data[(size_t)my * (size_t)mk->width + (size_t)mx];
            uint32_t texel;
            int64_t pr;
            int64_t pg;
            int64_t pb;
            uint32_t pa;
            int64_t lum;
            double shade;
            double cr;
            double cg;
            double cb;
            int64_t nr;
            int64_t ng;
            int64_t nb;
            if ((mtex >> 8) == 0u) /* RGB all zero = uncovered */
                continue;
            texel = p->data[(size_t)y * (size_t)p->width + (size_t)x];
            pr = (texel >> 24) & 0xFF;
            pg = (texel >> 16) & 0xFF;
            pb = (texel >> 8) & 0xFF;
            pa = texel & 0xFF;
            lum = (pr * 77 + pg * 150 + pb * 29) / 256;
            shade = (double)lum / (double)ref_lum;
            if (shade > max_shade)
                shade = max_shade;
            cr = (double)tr * shade;
            cg = (double)tg * shade;
            cb = (double)tb * shade;
            if (cr > 255.0)
                cr = 255.0;
            if (cg > 255.0)
                cg = 255.0;
            if (cb > 255.0)
                cb = 255.0;
            nr = (int64_t)((double)pr * (1.0 - strength) + cr * strength);
            ng = (int64_t)((double)pg * (1.0 - strength) + cg * strength);
            nb = (int64_t)((double)pb * (1.0 - strength) + cb * strength);
            p->data[(size_t)y * (size_t)p->width + (size_t)x] =
                ((uint32_t)nr << 24) | ((uint32_t)ng << 16) | ((uint32_t)nb << 8) | pa;
        }
    }
    pixels_touch(p);
}

/// @brief Luminance-band tint restricted to a coverage mask and to near-neutral
///   texels. Same blend as rt_pixels_tint_luminance_masked, with two extra
///   gates: the mask (any non-zero RGB at the scaled coordinate) must cover
///   the texel, and the texel's channel spread (max-min) must not exceed
///   @p neutral_max — white/grey fabric passes, skin does not. The mask may be
///   any size; coordinates scale proportionally.
/// @param pixels Borrowed Pixels handle mutated in place.
/// @param mask Borrowed Pixels coverage mask (non-zero RGB = covered).
/// @param rgb Packed 0xRRGGBB target color.
/// @param strength Blend strength at full mask, clamped to [0, 1].
/// @param lum_lo Luma at which the band begins (0..255).
/// @param lum_hi Luma at which the band reaches full strength (> lum_lo).
/// @param neutral_max Maximum channel spread (max-min) for a texel to qualify.
void rt_pixels_tint_masked_neutral(
    void *pixels, void *mask, int64_t rgb, double strength, int64_t lum_lo, int64_t lum_hi,
    int64_t neutral_max) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.TintMaskedNeutral: null pixels");
    rt_pixels_impl *mk = rt_pixels_checked_impl(mask, "Pixels.TintMaskedNeutral: null mask");
    int64_t tr;
    int64_t tg;
    int64_t tb;
    if (!p || !p->data || !mk || !mk->data)
        return;
    if (mk->width <= 0 || mk->height <= 0)
        return;
    if (!isfinite(strength))
        return;
    if (strength < 0.0)
        strength = 0.0;
    if (strength > 1.0)
        strength = 1.0;
    if (lum_lo < 0)
        lum_lo = 0;
    if (lum_hi <= lum_lo)
        lum_hi = lum_lo + 1;
    if (neutral_max < 0)
        neutral_max = 0;
    tr = (rgb >> 16) & 0xFF;
    tg = (rgb >> 8) & 0xFF;
    tb = rgb & 0xFF;
    for (int64_t y = 0; y < p->height; y++) {
        int64_t my = y * mk->height / p->height;
        for (int64_t x = 0; x < p->width; x++) {
            int64_t mx = x * mk->width / p->width;
            uint32_t mtex = mk->data[(size_t)my * (size_t)mk->width + (size_t)mx];
            uint32_t texel;
            int64_t pr;
            int64_t pg;
            int64_t pb;
            uint32_t pa;
            int64_t lum;
            int64_t hi_c;
            int64_t lo_c;
            double a;
            int64_t nr;
            int64_t ng;
            int64_t nb;
            if ((mtex >> 8) == 0u) /* RGB all zero = uncovered */
                continue;
            texel = p->data[(size_t)y * (size_t)p->width + (size_t)x];
            pr = (texel >> 24) & 0xFF;
            pg = (texel >> 16) & 0xFF;
            pb = (texel >> 8) & 0xFF;
            pa = texel & 0xFF;
            hi_c = pr;
            lo_c = pr;
            if (pg > hi_c)
                hi_c = pg;
            if (pb > hi_c)
                hi_c = pb;
            if (pg < lo_c)
                lo_c = pg;
            if (pb < lo_c)
                lo_c = pb;
            if (hi_c - lo_c > neutral_max)
                continue;
            lum = (pr * 77 + pg * 150 + pb * 29) / 256;
            if (lum < lum_lo)
                continue;
            a = strength;
            if (lum < lum_hi)
                a = strength * (double)(lum - lum_lo) / (double)(lum_hi - lum_lo);
            nr = (int64_t)((double)pr * (1.0 - a) + (double)tr * a);
            ng = (int64_t)((double)pg * (1.0 - a) + (double)tg * a);
            nb = (int64_t)((double)pb * (1.0 - a) + (double)tb * a);
            p->data[(size_t)y * (size_t)p->width + (size_t)x] =
                ((uint32_t)nr << 24) | ((uint32_t)ng << 16) | ((uint32_t)nb << 8) | pa;
        }
    }
    pixels_touch(p);
}

void rt_pixels_tint_luminance_masked(
    void *pixels, int64_t rgb, double strength, int64_t lum_lo, int64_t lum_hi) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.TintLuminanceMasked: null pixels");
    int64_t tr;
    int64_t tg;
    int64_t tb;
    int64_t count;
    if (!p || !p->data)
        return;
    if (!isfinite(strength))
        return;
    if (strength < 0.0)
        strength = 0.0;
    if (strength > 1.0)
        strength = 1.0;
    if (lum_lo < 0)
        lum_lo = 0;
    if (lum_hi <= lum_lo)
        lum_hi = lum_lo + 1;
    tr = (rgb >> 16) & 0xFF;
    tg = (rgb >> 8) & 0xFF;
    tb = rgb & 0xFF;
    count = p->width * p->height;
    for (int64_t i = 0; i < count; i++) {
        uint32_t texel = p->data[i]; /* raw 0xRRGGBBAA */
        int64_t pr = (texel >> 24) & 0xFF;
        int64_t pg = (texel >> 16) & 0xFF;
        int64_t pb = (texel >> 8) & 0xFF;
        uint32_t pa = texel & 0xFF;
        int64_t lum = (pr * 77 + pg * 150 + pb * 29) / 256;
        double a;
        int64_t nr;
        int64_t ng;
        int64_t nb;
        if (lum < lum_lo)
            continue;
        a = strength;
        if (lum < lum_hi)
            a = strength * (double)(lum - lum_lo) / (double)(lum_hi - lum_lo);
        nr = (int64_t)((double)pr * (1.0 - a) + (double)tr * a);
        ng = (int64_t)((double)pg * (1.0 - a) + (double)tg * a);
        nb = (int64_t)((double)pb * (1.0 - a) + (double)tb * a);
        p->data[i] = ((uint32_t)nr << 24) | ((uint32_t)ng << 16) | ((uint32_t)nb << 8) | pa;
    }
    pixels_touch(p);
}
