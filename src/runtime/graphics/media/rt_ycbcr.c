//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_ycbcr.c
// Purpose: YCbCr planar to RGBA conversion using BT.601 coefficients.
//   Integer-only arithmetic for performance (no floating point per pixel).
//
// Key invariants:
//   - BT.601 matrix (ITU-R BT.601, standard for Theora/SD video):
//     R = 1.164*(Y-16) + 1.596*(Cr-128)
//     G = 1.164*(Y-16) - 0.392*(Cb-128) - 0.813*(Cr-128)
//     B = 1.164*(Y-16) + 2.017*(Cb-128)
//   - Fixed-point: multiply by 298/256, 409/256, etc. (8-bit shift).
//   - Chroma upsampling: nearest-neighbor for subsampled formats.
//
// Links: src/runtime/graphics/media/rt_ycbcr.h (public conversion API)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements fixed-point BT.601 planar YCbCr-to-RGBA conversion.
 * @details A shared bounded converter maps chroma coordinates for 4:2:0,
 * 4:2:2, and 4:4:4 layouts, converts each limited-range sample with
 * integer arithmetic, saturates color channels, and writes opaque Zanna
 * packed pixels into caller-owned storage.
 */

#include "rt_ycbcr.h"

/// @brief Saturate an int32_t to the [0, 255] byte range.
/// @param v Signed color-channel value.
/// @return @p v clamped to the inclusive byte range.
static inline int32_t clamp255(int32_t v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return v;
}

/// @brief Convert one limited-range BT.601 YCbCr sample to Zanna RGBA.
///
/// @param y_sample  Luma sample exactly as stored in the Y plane.
/// @param cb_sample Blue-difference chroma sample.
/// @param cr_sample Red-difference chroma sample.
/// @return Packed 0xRRGGBBAA pixel with fully opaque alpha.
static uint32_t ycbcr601_sample_to_rgba(uint8_t y_sample, uint8_t cb_sample, uint8_t cr_sample) {
    int32_t y = (int32_t)y_sample - 16;
    int32_t cb = (int32_t)cb_sample - 128;
    int32_t cr = (int32_t)cr_sample - 128;

    /* BT.601 fixed-point conversion (x256, then >>8). */
    int32_t r = (298 * y + 409 * cr + 128) >> 8;
    int32_t g = (298 * y - 100 * cb - 208 * cr + 128) >> 8;
    int32_t b = (298 * y + 516 * cb + 128) >> 8;

    return ((uint32_t)clamp255(r) << 24) | ((uint32_t)clamp255(g) << 16) |
           ((uint32_t)clamp255(b) << 8) | 0xFFu;
}

/// @brief Shared planar YCbCr to RGBA converter for supported chroma layouts.
///
/// @details @p chroma_x_shift and @p chroma_y_shift describe the subsampling
///   ratio. 4:2:0 uses (1,1), 4:2:2 uses (1,0), and 4:4:4 uses (0,0).
///   The caller provides already-cropped plane pointers and visible dimensions.
///   Invalid pointers or non-positive dimensions are treated as no-ops to match
///   the historical conversion API.
///
/// @param y_plane        Luma plane at the visible crop origin.
/// @param cb_plane       Cb chroma plane at the visible crop origin.
/// @param cr_plane       Cr chroma plane at the visible crop origin.
/// @param width          Visible width in pixels.
/// @param height         Visible height in pixels.
/// @param y_stride       Bytes per luma row.
/// @param c_stride       Bytes per chroma row.
/// @param chroma_x_shift Horizontal chroma subsampling shift.
/// @param chroma_y_shift Vertical chroma subsampling shift.
/// @param rgba_out       Output pixels in packed 0xRRGGBBAA format.
static void ycbcr_to_rgba_subsampled(const uint8_t *y_plane,
                                     const uint8_t *cb_plane,
                                     const uint8_t *cr_plane,
                                     int32_t width,
                                     int32_t height,
                                     int32_t y_stride,
                                     int32_t c_stride,
                                     int32_t chroma_x_shift,
                                     int32_t chroma_y_shift,
                                     uint32_t *rgba_out) {
    if (!y_plane || !cb_plane || !cr_plane || !rgba_out)
        return;
    if (width <= 0 || height <= 0 || y_stride <= 0 || c_stride <= 0)
        return;

    for (int32_t row = 0; row < height; row++) {
        const uint8_t *y_row = y_plane + row * y_stride;
        const uint8_t *cb_row = cb_plane + (row >> chroma_y_shift) * c_stride;
        const uint8_t *cr_row = cr_plane + (row >> chroma_y_shift) * c_stride;
        uint32_t *out_row = rgba_out + row * width;

        for (int32_t col = 0; col < width; col++) {
            int32_t chroma_col = col >> chroma_x_shift;
            out_row[col] =
                ycbcr601_sample_to_rgba(y_row[col], cb_row[chroma_col], cr_row[chroma_col]);
        }
    }
}

/// @brief Convert a YCbCr 4:2:0 planar image to packed RGBA (Zanna 0xRRGGBBAA format).
/// @details Uses BT.601 limited-range coefficients with ×256 fixed-point scaling
///   (shift by 8 after accumulation) to avoid floating-point overhead in the inner
///   loop. The chroma planes are 2× subsampled in both axes (4:2:0), so each Cb/Cr
///   sample covers a 2×2 luma block — accessed as `cb_row[col / 2]`. Level offsets
///   are applied per the BT.601 spec: Y is biased by -16 (limited range base),
///   Cb/Cr are biased by -128 (signed centering). All RGB outputs are clamped to
///   [0, 255] via `clamp255` before packing. Alpha is always 0xFF (fully opaque).
///   Used by the Theora decoder's YUV→pixels conversion path.
/// @param y_plane   Luma plane, width × height bytes.
/// @param cb_plane  Blue-difference chroma plane, (width/2) × (height/2) bytes.
/// @param cr_plane  Red-difference chroma plane, (width/2) × (height/2) bytes.
/// @param width     Image width in pixels.
/// @param height    Image height in pixels.
/// @param y_stride  Row stride of the luma plane in bytes (may be > width).
/// @param c_stride  Row stride of the chroma planes in bytes (may be > width/2).
/// @param rgba_out  Output buffer of width × height uint32_t RGBA pixels.
void ycbcr420_to_rgba(const uint8_t *y_plane,
                      const uint8_t *cb_plane,
                      const uint8_t *cr_plane,
                      int32_t width,
                      int32_t height,
                      int32_t y_stride,
                      int32_t c_stride,
                      uint32_t *rgba_out) {
    ycbcr_to_rgba_subsampled(
        y_plane, cb_plane, cr_plane, width, height, y_stride, c_stride, 1, 1, rgba_out);
}

/// @brief Convert a planar BT.601 YCbCr 4:2:2 image to packed opaque RGBA.
/// @details Uses one chroma sample per two horizontal luma samples, with no vertical subsampling.
///          Invalid pointers, dimensions, or strides are treated as a no-op.
/// @param y_plane Luma plane at the visible crop origin.
/// @param cb_plane Horizontally subsampled Cb plane.
/// @param cr_plane Horizontally subsampled Cr plane.
/// @param width Visible image width in pixels.
/// @param height Visible image height in pixels.
/// @param y_stride Luma row stride in bytes.
/// @param c_stride Chroma row stride in bytes.
/// @param rgba_out Output buffer for `width * height` packed `0xRRGGBBAA` pixels.
void ycbcr422_to_rgba(const uint8_t *y_plane,
                      const uint8_t *cb_plane,
                      const uint8_t *cr_plane,
                      int32_t width,
                      int32_t height,
                      int32_t y_stride,
                      int32_t c_stride,
                      uint32_t *rgba_out) {
    ycbcr_to_rgba_subsampled(
        y_plane, cb_plane, cr_plane, width, height, y_stride, c_stride, 1, 0, rgba_out);
}

/// @brief Convert a planar BT.601 YCbCr 4:4:4 image to packed opaque RGBA.
/// @details Uses one Cb and Cr sample per luma sample. Invalid pointers, dimensions, or strides are
///          treated as a no-op.
/// @param y_plane Full-resolution luma plane at the visible crop origin.
/// @param cb_plane Full-resolution Cb plane.
/// @param cr_plane Full-resolution Cr plane.
/// @param width Visible image width in pixels.
/// @param height Visible image height in pixels.
/// @param y_stride Luma row stride in bytes.
/// @param c_stride Chroma row stride in bytes.
/// @param rgba_out Output buffer for `width * height` packed `0xRRGGBBAA` pixels.
void ycbcr444_to_rgba(const uint8_t *y_plane,
                      const uint8_t *cb_plane,
                      const uint8_t *cr_plane,
                      int32_t width,
                      int32_t height,
                      int32_t y_stride,
                      int32_t c_stride,
                      uint32_t *rgba_out) {
    ycbcr_to_rgba_subsampled(
        y_plane, cb_plane, cr_plane, width, height, y_stride, c_stride, 0, 0, rgba_out);
}
