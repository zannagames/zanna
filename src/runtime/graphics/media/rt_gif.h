//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_gif.h
// Purpose: GIF87a/89a image decoder with LZW decompression and animation support.
// Key invariants:
//   - Supports both GIF87a and GIF89a formats
//   - Returns one Pixels object per frame in RGBA format (0xRRGGBBAA)
//   - Per-frame delay and disposal method extracted from Graphics Control Extension
//   - Transparent color indices preserve the prior composited canvas pixel.
// Ownership/Lifetime:
//   - Caller owns the malloc'd gif_frame_t array and must free it.
//   - Pixels handles stored in successful frames are runtime-managed and must be released normally.
//   - First-frame memory decoding returns a malloc-owned raw RGBA32 buffer.
// Links: src/runtime/graphics/2d/rt_pixels.h (Pixels public API),
//        src/runtime/graphics/2d/rt_sprite.c (animated sprite integration)
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief A single decoded GIF frame.
typedef struct {
    void *pixels;       ///< Pixels object (RGBA, via pixels_alloc)
    int delay_ms;       ///< Frame display time in milliseconds (100 ms fallback when unset)
    int dispose_method; ///< 0=none, 1=keep, 2=restore-to-bg, 3=restore-to-prev
} gif_frame_t;

/// @brief Decode all frames from a GIF file.
/// @details Reads at most 100 MiB through the runtime file API, composites each image on the logical
///          screen canvas, applies transparency and disposal, and retains full-canvas Pixels
///          snapshots within a bounded decoded-output budget. Required outputs are written only on
///          success.
/// @param filepath Non-NULL UTF-8 filesystem path.
/// @param out_frames Required destination receiving a malloc-owned frame array.
/// @param out_frame_count Required destination receiving the decoded frame count.
/// @param out_width Optional destination receiving the logical screen width.
/// @param out_height Optional destination receiving the logical screen height.
/// @return Positive decoded frame count on success, otherwise 0 with required outputs untouched.
int gif_decode_file(const char *filepath,
                    gif_frame_t **out_frames,
                    int *out_frame_count,
                    int *out_width,
                    int *out_height);

/// @brief Decode the first GIF frame from a memory buffer into malloc-owned raw RGBA32 pixels.
/// @details Validates GIF87a/GIF89a structure and configured file/canvas/LZW budgets, composites the
///          first renderable image with its palette and transparency state, and stops without
///          retaining animation frames. All provided outputs are initialized before validation.
/// @param data Read-only GIF file bytes.
/// @param len Accessible byte length of @p data, limited to 100 MiB.
/// @param out_pixels Required destination receiving a malloc-owned row-major `0xRRGGBBAA` buffer.
/// @param out_width Optional destination receiving the logical screen width.
/// @param out_height Optional destination receiving the logical screen height.
/// @return 1 on success, 0 on failure.
int rt_gif_decode_memory_first_rgba32(
    const uint8_t *data, size_t len, uint32_t **out_pixels, int *out_width, int *out_height);

#ifdef __cplusplus
}
#endif
