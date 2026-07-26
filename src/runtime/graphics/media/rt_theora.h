//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_theora.h
// Purpose: Theora video codec decoder (decode only, from Theora I spec).
//   Supports I-frames and P-frames in OGG container.
//
// Key invariants:
//   - Based on VP3: 8x8 DCT blocks, Huffman coding, planar YCbCr.
//   - No B-frames (only I and P frames).
//   - Three headers: identification (0x80), comment (0x81), setup (0x82).
//   - Output: planar YCbCr planes, caller converts via rt_ycbcr.h.
//
// Ownership/Lifetime:
//   - theora_decoder_t owns its private tables and current/previous/golden plane buffers.
//   - Packet bytes are borrowed only for the duration of each decode call.
//   - Returned plane pointers remain decoder-owned and are overwritten by later frame decodes.
//
// Links: src/runtime/graphics/media/rt_theora.c (headers and entropy decoding),
//        src/runtime/graphics/media/rt_theora_recon.c (frame reconstruction),
//        src/runtime/graphics/media/rt_ycbcr.h (planar conversion),
//        src/runtime/graphics/media/rt_videoplayer.h (consumer),
//        Theora spec: https://www.theora.org/doc/Theora.pdf
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THEORA_MAX_HUFFMAN_TABLES 80

/// @brief Theora decoder context.
typedef struct {
    /* Identification header fields */
    uint8_t version_major, version_minor, version_sub;
    uint32_t frame_width, frame_height; /* encoded frame (multiple of 16) */
    uint32_t pic_width, pic_height;     /* visible picture */
    uint32_t pic_x, pic_y;              /* picture offset within frame */
    uint32_t fps_num, fps_den;          /* frame rate fraction */
    uint32_t aspect_num, aspect_den;    /* pixel aspect ratio */
    uint8_t color_space;                /* 0=unspecified, 1=Rec470M, 2=Rec470BG */
    uint8_t pixel_format;               /* 0=4:2:0, 2=4:2:2, 3=4:4:4 */
    uint32_t quality_hint;
    uint32_t keyframe_granule_shift;

    /* Setup header fields */
    uint8_t loop_filter_limits[64]; /* per-QI loop filter limit */
    /* Quantization matrices: [inter/intra][plane][qi][coeff] */
    uint16_t qmat[2][3][64][64]; /* simplified: base matrices */
    int32_t qmat_count;

    /* Decode state */
    int8_t headers_complete;                  /* 1 after all 3 headers parsed */
    int32_t superblock_cols, superblock_rows; /* superblock grid dimensions */
    int32_t macro_cols, macro_rows;           /* macroblock grid */
    int32_t block_cols, block_rows;           /* 8x8 block grid */

    /* Reference frames (planar YCbCr planes; chroma layout follows pixel_format) */
    uint8_t *ref_y, *ref_cb, *ref_cr;    /* last decoded reference */
    uint8_t *gold_y, *gold_cb, *gold_cr; /* golden frame reference */
    int32_t y_stride, c_stride;          /* plane strides */
    int32_t y_height, c_height;          /* plane heights */

    /* Current decode output */
    uint8_t *cur_y, *cur_cb, *cur_cr; /* current frame being decoded */

    /* Internal/private decoder state. */
    void *priv;
} theora_decoder_t;

/// @brief Initialize a Theora decoder context.
/// @details Clears caller-owned storage; it does not release any pre-existing allocations. Call
///          theora_decoder_free() before reinitializing an active decoder.
/// @param dec Non-NULL decoder storage to initialize.
void theora_decoder_init(theora_decoder_t *dec);

/// @brief Free decoder resources.
/// @details Releases private setup/layout data and all nine YCbCr frame planes, then zeros the
///          context. Safe to call with NULL or on an already-freed decoder.
/// @param dec Decoder to release.
void theora_decoder_free(theora_decoder_t *dec);

/// @brief Decode a Theora header packet (identification, comment, or setup).
/// @details Dispatches packet types 0x80, 0x81, and 0x82. Callers must provide the required
///          identification/comment/setup sequence before decoding video. Setup success allocates
///          layout tables and current/reference/golden frame planes.
/// @param dec Initialized decoder receiving header state.
/// @param data Borrowed raw Ogg packet data.
/// @param len Accessible packet length in bytes.
/// @return 0 on parsed header success, 1 when the packet is not a Theora header, or -1 on invalid
///         arguments, malformed syntax, unsupported geometry/format, or allocation failure.
int theora_decode_header(theora_decoder_t *dec, const uint8_t *data, size_t len);

/// @brief Decode a Theora data (video frame) packet.
/// @details Runs entropy decoding, DC restoration, inverse transform, motion compensation, loop
///          filtering, and reference-frame updates. The first decoded frame must be intra. Mutable
///          entropy state is restored if packet decoding fails; optional outputs are written only
///          on success and remain owned by @p dec.
/// @param dec Decoder with completed headers.
/// @param data Borrowed compressed video packet.
/// @param len Accessible packet length in bytes.
/// @param out_y Optional destination for the decoder-owned luma plane.
/// @param out_cb Optional destination for the decoder-owned Cb plane.
/// @param out_cr Optional destination for the decoder-owned Cr plane.
/// @return 0 on success, or -1 for invalid state, malformed/truncated data, or snapshot allocation
///         failure.
int theora_decode_frame(theora_decoder_t *dec,
                        const uint8_t *data,
                        size_t len,
                        const uint8_t **out_y,
                        const uint8_t **out_cb,
                        const uint8_t **out_cr);

/// @brief Check if a packet is a Theora header (starts with 0x80-0x82 + "theora").
/// @param data Borrowed packet bytes.
/// @param len Accessible packet length.
/// @return 1 for a bounded identification, comment, or setup signature, otherwise 0.
int theora_is_header_packet(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
