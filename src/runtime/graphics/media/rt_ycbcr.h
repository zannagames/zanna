//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_ycbcr.h
// Purpose: YCbCr planar -> RGBA conversion for video decoders (Theora, etc.)
//   Uses BT.601 matrix coefficients (standard for SD video).
//
// Key invariants:
//   - Input: separate Y, Cb, Cr planes. Y is full resolution; Cb/Cr may
//     be 4:2:0, 4:2:2, or 4:4:4 depending on the decoder header.
//   - Output: 0xRRGGBBAA packed uint32_t array (Zanna Pixels format).
//   - Clamping: output values clamped to [0, 255].
//
// Ownership/Lifetime:
//   - All plane and output buffers are caller-owned and borrowed only for the call.
//
// Links: src/runtime/graphics/media/rt_theora.h, src/runtime/graphics/media/rt_videoplayer.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Convert YCbCr 4:2:0 planes to RGBA pixel array.
/// @details Uses nearest-neighbor chroma upsampling and limited-range BT.601 conversion. Invalid
///          pointers, dimensions, or strides are treated as a no-op.
/// @param y_plane   Luma plane (width × height).
/// @param cb_plane  Cb chroma plane (ceil(width/2) × ceil(height/2)).
/// @param cr_plane  Cr chroma plane (ceil(width/2) × ceil(height/2)).
/// @param width     Positive visible frame width in pixels.
/// @param height    Positive visible frame height in pixels.
/// @param y_stride  Bytes per row in Y plane.
/// @param c_stride  Bytes per row in Cb/Cr planes.
/// @param rgba_out  Output array (width × height uint32_t, 0xRRGGBBAA).
void ycbcr420_to_rgba(const uint8_t *y_plane,
                      const uint8_t *cb_plane,
                      const uint8_t *cr_plane,
                      int32_t width,
                      int32_t height,
                      int32_t y_stride,
                      int32_t c_stride,
                      uint32_t *rgba_out);

/// @brief Convert YCbCr 4:2:2 planes to an RGBA pixel array.
///
/// The luma plane is full resolution, while Cb and Cr are horizontally
/// subsampled by two and have one chroma row for every luma row. The output
/// buffer receives @p width * @p height pixels in Zanna's packed
/// 0xRRGGBBAA format.
///
/// @param y_plane   Luma plane (width x height).
/// @param cb_plane  Cb chroma plane ((width + 1) / 2 x height).
/// @param cr_plane  Cr chroma plane ((width + 1) / 2 x height).
/// @param width     Visible frame width in pixels.
/// @param height    Visible frame height in pixels.
/// @param y_stride  Bytes per row in Y plane.
/// @param c_stride  Bytes per row in Cb/Cr planes.
/// @param rgba_out  Output array (width x height uint32_t, 0xRRGGBBAA).
void ycbcr422_to_rgba(const uint8_t *y_plane,
                      const uint8_t *cb_plane,
                      const uint8_t *cr_plane,
                      int32_t width,
                      int32_t height,
                      int32_t y_stride,
                      int32_t c_stride,
                      uint32_t *rgba_out);

/// @brief Convert YCbCr 4:4:4 planes to an RGBA pixel array.
///
/// All three planes are full resolution, so each luma pixel has a matching
/// Cb and Cr sample. The conversion uses the same BT.601 limited-range
/// coefficients as the 4:2:0 path and writes Zanna packed 0xRRGGBBAA pixels.
///
/// @param y_plane   Luma plane (width x height).
/// @param cb_plane  Cb chroma plane (width x height).
/// @param cr_plane  Cr chroma plane (width x height).
/// @param width     Visible frame width in pixels.
/// @param height    Visible frame height in pixels.
/// @param y_stride  Bytes per row in Y plane.
/// @param c_stride  Bytes per row in Cb/Cr planes.
/// @param rgba_out  Output array (width x height uint32_t, 0xRRGGBBAA).
void ycbcr444_to_rgba(const uint8_t *y_plane,
                      const uint8_t *cb_plane,
                      const uint8_t *cr_plane,
                      int32_t width,
                      int32_t height,
                      int32_t y_stride,
                      int32_t c_stride,
                      uint32_t *rgba_out);

#ifdef __cplusplus
}
#endif
