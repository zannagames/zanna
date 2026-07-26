//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_theora.c
// Purpose: In-tree Theora video-codec decoder for OGG container playback.
//   Parses Theora setup/info/comment headers, builds the Huffman tables,
//   decodes intra and inter frames, and produces YCbCr pixel planes that
//   rt_videoplayer.c hands to rt_ycbcr.c for RGB conversion. Implements the
//   full bitstream syntax of theora-spec-1.1 with no external dependencies.
//
// Key invariants:
//   - All bit-stream reads route through bitreader_t; once `failed` is set,
//     subsequent reads are no-ops returning 0 and the surrounding decoder
//     short-circuits to a graceful error rather than producing garbage.
//   - Frame dimensions, motion-vector ranges, and quantizer indices are
//     bounds-checked before allocation so a malformed setup header cannot
//     OOM or read past the input buffer.
//   - Static const tables (theora_zigzag, sb/mb_hilbert_*, mb_mode_scheme,
//     dc_weights, etc.) are spec-fixed — never modified at runtime. They
//     hold scan orders, super-block traversal patterns, MB-mode coding
//     scheme tables, and DC predictor weights from theora-spec-1.1.
//
// Ownership/Lifetime:
//   - The decoder context owns its Huffman tables and YCbCr plane buffers;
//     all allocations are paired with rt_theora_destroy.
//   - Input packets (`data`, `len`) are caller-owned and only borrowed
//     during decode. No reference is held after the call returns.
//
// Links: src/runtime/graphics/media/rt_theora.h (public API),
//        src/runtime/graphics/media/rt_videoplayer.c (consumer),
//        src/runtime/graphics/media/rt_ycbcr.c (planar-to-RGB conversion)
//
//===----------------------------------------------------------------------===//

#include "rt_theora.h"
#include "rt_theora_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define THEORA_MAX_FRAME_PIXELS ((uint64_t)64u * 1024u * 1024u)
#define THEORA_MAX_FRAME_BUFFER_BYTES ((size_t)768u * 1024u * 1024u)

const uint8_t theora_zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                   12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                   35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                   58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static const uint8_t sb_hilbert_x[16] = {0, 1, 1, 0, 0, 0, 1, 1, 2, 2, 3, 3, 3, 2, 2, 3};

static const uint8_t sb_hilbert_y[16] = {0, 0, 1, 1, 2, 3, 3, 2, 2, 3, 3, 2, 1, 1, 0, 0};

static const uint8_t mb_hilbert_x[4] = {0, 0, 1, 1};
static const uint8_t mb_hilbert_y[4] = {0, 1, 1, 0};

static const uint8_t mb_mode_scheme[7][8] = {{3, 4, 2, 0, 1, 5, 6, 7},
                                             {3, 4, 0, 2, 1, 5, 6, 7},
                                             {3, 2, 4, 0, 1, 5, 6, 7},
                                             {3, 2, 0, 4, 1, 5, 6, 7},
                                             {0, 3, 4, 2, 1, 5, 6, 7},
                                             {0, 5, 3, 4, 2, 1, 6, 7},
                                             {0, 1, 2, 3, 4, 5, 6, 7}};


static const theora_dc_weight_t dc_weights[16] = {{{0, 0, 0, 0}, 1},
                                                  {{1, 0, 0, 0}, 1},
                                                  {{0, 1, 0, 0}, 1},
                                                  {{1, 0, 0, 0}, 1},
                                                  {{0, 0, 1, 0}, 1},
                                                  {{1, 0, 1, 0}, 2},
                                                  {{0, 0, 1, 0}, 1},
                                                  {{29, -26, 29, 0}, 32},
                                                  {{0, 0, 0, 1}, 1},
                                                  {{75, 0, 0, 53}, 128},
                                                  {{0, 1, 0, 1}, 2},
                                                  {{75, 0, 0, 53}, 128},
                                                  {{0, 0, 1, 0}, 1},
                                                  {{75, 0, 0, 53}, 128},
                                                  {{0, 3, 10, 3}, 16},
                                                  {{29, -26, 29, 0}, 32}};

// Bitstream reader — Theora packets are bit-packed with MSB-first
// ordering. Failure is sticky: once `failed` is set, all subsequent
// reads return 0 so the caller can defer error checking until end of
// packet.

/// @brief Initialize a bitreader to consume `data` MSB-first.
/// @param br Writable reader state.
/// @param data Borrowed packet bytes.
/// @param len Accessible length of @p data.
void br_init(bitreader_t *br, const uint8_t *data, size_t len) {
    br->data = data;
    br->len = len;
    br->byte_pos = 0;
    br->bit_pos = 8;
    br->failed = 0;
}

/// @brief Read `nbits` bits (MSB-first) from the stream.
///
/// Capped at 25 bits per call — enough for any single Theora field;
/// 32-bit reads would overflow the accumulator on the last bit.
/// Returns 0 and latches `failed` on EOF or overlong request.
/// @param br Mutable bit-reader state.
/// @param nbits Number of bits to consume, in [0, 25].
/// @return Decoded unsigned value, or 0 after latching failure.
static uint32_t br_read(bitreader_t *br, int nbits) {
    uint32_t value = 0;
    if (!br || nbits < 0 || nbits > 25) {
        if (br)
            br->failed = 1;
        return 0;
    }
    while (nbits-- > 0) {
        if (br->byte_pos >= br->len) {
            br->failed = 1;
            return 0;
        }
        value <<= 1;
        value |= (uint32_t)((br->data[br->byte_pos] >> (br->bit_pos - 1)) & 1u);
        br->bit_pos--;
        if (br->bit_pos == 0) {
            br->byte_pos++;
            br->bit_pos = 8;
        }
    }
    return value;
}

/// @brief Read one bit from the stream (convenience wrapper).
/// @param br Mutable bit-reader state.
/// @return Next bit as 0 or 1; underflow returns 0 and latches failure.
static int br_read1(bitreader_t *br) {
    return (int)br_read(br, 1);
}

// Tiny scalar helpers used throughout the decoder:

/// @brief Clamp `v` to `[lo, hi]`.
/// @param v Value to clamp.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return @p v constrained to the supplied bounds.
int clampi(int v, int lo, int hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/// @brief Truncate `v` to a 16-bit signed value (modular wrap, defined behavior).
///
/// Uses the unsigned-cast trick to avoid implementation-defined
/// signed overflow when `v` is outside int16 range.
/// @param v Signed value to wrap modulo 2^16.
/// @return Low sixteen bits interpreted as an int16_t.
int16_t trunc_i16(int32_t v) {
    return (int16_t)(uint16_t)v;
}

/// @brief Bit-length of `v` (number of bits needed to encode it; 0 → 0).
///
/// Per Theora spec: `ilog(0) = 0`, `ilog(1) = 1`, `ilog(n) = floor(log2(n)) + 1`.
/// @param v Unsigned value to measure.
/// @return Number of significant bits needed to encode @p v.
static int ilog_u32(uint32_t v) {
    int ret = 0;
    while (v) {
        ret++;
        v >>= 1;
    }
    return ret;
}

/// @brief Symmetric integer division with rounding (positive and negative inputs).
///
/// Matches the spec's "rounded division" — `(v + d/2) / d` for
/// positive `v`, mirrored for negative. Returns 0 when `d <= 0`.
/// @param v Signed numerator.
/// @param d Positive divisor.
/// @return Symmetrically rounded quotient, or 0 for a non-positive divisor.
static int round_div_s32(int v, int d) {
    if (d <= 0)
        return 0;
    if (v >= 0)
        return (v + d / 2) / d;
    return -(((-v) + d / 2) / d);
}

/// @brief Map a Theora MB mode to the reference frame index it samples.
///
/// Modes 1 (intra) → previous frame; 5/6 (golden + extra) → golden
/// frame; everything else → last frame. Used during inter-frame
/// motion compensation.
/// @param mode Decoded macroblock mode.
/// @return Reference slot index: 0 for previous, 1 for last, or 2 for golden.
int theora_ref_index_for_mode(int mode) {
    switch (mode) {
        case 1:
            return 0;
        case 5:
        case 6:
            return 2;
        default:
            return 1;
    }
}

/// @brief Number of 8×8 blocks per macroblock width, per plane (chroma format aware).
///
/// Luma always 2; chroma is 2 only for 4:4:4 (pixel_format 3),
/// otherwise 1 (subsampled).
/// @param dec Initialized decoder describing the chroma format.
/// @param plane Plane index: 0 for luma, 1 or 2 for chroma.
/// @return Number of horizontal 8x8 blocks per macroblock.
static int plane_mb_block_w(const theora_decoder_t *dec, int plane) {
    if (plane == 0)
        return 2;
    if (dec->pixel_format == 3)
        return 2;
    return 1;
}

/// @brief Number of 8×8 blocks per macroblock height, per plane.
///
/// Luma always 2; chroma is 1 only for 4:2:0 (pixel_format 0).
/// @param dec Initialized decoder describing the chroma format.
/// @param plane Plane index: 0 for luma, 1 or 2 for chroma.
/// @return Number of vertical 8x8 blocks per macroblock.
static int plane_mb_block_h(const theora_decoder_t *dec, int plane) {
    if (plane == 0)
        return 2;
    if (dec->pixel_format == 0)
        return 1;
    return 2;
}

/// @brief Multiply two non-negative int32 dimensions into an int32 result.
/// @details Theora layout counts are stored as signed int32 values throughout the decoder.
///          This helper keeps malformed headers from wrapping those counts before they are
///          used as allocation sizes or array offsets.
/// @param a First dimension, required to be non-negative.
/// @param b Second dimension, required to be non-negative.
/// @param out Receives @p a * @p b on success.
/// @return 1 when the product fits in int32_t, 0 on negative input, NULL output, or overflow.
static int theora_checked_mul_i32(int32_t a, int32_t b, int32_t *out) {
    int64_t product;
    if (!out || a < 0 || b < 0)
        return 0;
    product = (int64_t)a * (int64_t)b;
    if (product > INT32_MAX)
        return 0;
    *out = (int32_t)product;
    return 1;
}

/// @brief Add two non-negative int32 counts into an int32 result.
/// @details Used for cumulative block and superblock offsets, which are later reused as array
///          indices. Rejecting overflow here prevents a single oversized plane from poisoning
///          all subsequent layout tables.
/// @param a First count, required to be non-negative.
/// @param b Second count, required to be non-negative.
/// @param out Receives @p a + @p b on success.
/// @return 1 when the sum fits in int32_t, 0 on negative input, NULL output, or overflow.
static int theora_checked_add_i32(int32_t a, int32_t b, int32_t *out) {
    int64_t sum;
    if (!out || a < 0 || b < 0)
        return 0;
    sum = (int64_t)a + (int64_t)b;
    if (sum > INT32_MAX)
        return 0;
    *out = (int32_t)sum;
    return 1;
}

/// @brief Multiply two size_t values with overflow checking.
/// @param a First factor.
/// @param b Second factor.
/// @param out Receives @p a * @p b on success.
/// @return 1 when the product fits in size_t, 0 when @p out is NULL or the product overflows.
static int theora_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (!out)
        return 0;
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

/// @brief Validate a frame plane allocation size and enforce the decoder memory budget.
/// @details Theora stores three reference frames (previous, golden, current). Each frame has one
///          luma plane and two chroma planes. This helper validates the per-plane products and the
///          total nine-plane allocation before any buffers are allocated.
/// @param y_stride Luma stride in bytes.
/// @param y_height Luma height in rows.
/// @param c_stride Chroma stride in bytes.
/// @param c_height Chroma height in rows.
/// @param out_y_size Receives one luma-plane byte count.
/// @param out_c_size Receives one chroma-plane byte count.
/// @return 1 when all products fit and the aggregate allocation is within budget.
static int theora_checked_frame_plane_sizes(int32_t y_stride,
                                            int32_t y_height,
                                            int32_t c_stride,
                                            int32_t c_height,
                                            size_t *out_y_size,
                                            size_t *out_c_size) {
    size_t y_size;
    size_t c_size;
    size_t frame_size;
    size_t total_size;
    if (y_stride <= 0 || y_height <= 0 || c_stride <= 0 || c_height <= 0)
        return 0;
    if (!theora_checked_mul_size((size_t)y_stride, (size_t)y_height, &y_size) ||
        !theora_checked_mul_size((size_t)c_stride, (size_t)c_height, &c_size))
        return 0;
    if (y_size > SIZE_MAX - c_size || y_size + c_size > SIZE_MAX - c_size)
        return 0;
    frame_size = y_size + c_size + c_size;
    if (frame_size > SIZE_MAX / 3u)
        return 0;
    total_size = frame_size * 3u;
    if (total_size == 0 || total_size > THEORA_MAX_FRAME_BUFFER_BYTES)
        return 0;
    if (out_y_size)
        *out_y_size = y_size;
    if (out_c_size)
        *out_c_size = c_size;
    return 1;
}

/// @brief Tear down all heap allocations owned by the decoder's `priv`.
///
/// Called from the public destroy path. NULL-safe.
/// @param dec Valid decoder whose private allocation graph is released.
static void theora_priv_free(theora_decoder_t *dec) {
    theora_priv_t *priv = (theora_priv_t *)dec->priv;
    if (!priv)
        return;
    free(priv->plane_raster_to_coded[0]);
    free(priv->plane_raster_to_coded[1]);
    free(priv->plane_raster_to_coded[2]);
    free(priv->mb_raster_to_coded);
    free(priv->blocks);
    free(priv->mbs);
    free(priv->bcoded);
    free(priv->sb_partcoded);
    free(priv->sb_fullcoded);
    free(priv->qiis);
    free(priv->mbmodes);
    free(priv->tis);
    free(priv->ncoeffs);
    free(priv->mvects);
    free(priv->coeffs);
    free(priv);
    dec->priv = NULL;
}

/// @brief Validate a Theora header packet's "type byte + magic 'theora'" prefix.
///
/// Theora's three header packet types (id=0x80, comment=0x81, setup=0x82)
/// all begin with the type byte followed by the ASCII string "theora".
/// @param data Borrowed packet bytes.
/// @param len Accessible packet length.
/// @param type Expected header type byte.
/// @return 1 when the bounded packet has the requested signature, otherwise 0.
static int check_header_sig(const uint8_t *data, size_t len, uint8_t type) {
    if (!data || len < 7 || data[0] != type)
        return 0;
    return data[1] == 't' && data[2] == 'h' && data[3] == 'e' && data[4] == 'o' && data[5] == 'r' &&
           data[6] == 'a';
}

/// @brief Parse the 0x80 identification header (frame size, fps, color, etc.).
///
/// Reads:
///   - Version triple (major.minor.sub)
///   - Frame and picture dimensions (frame is multiple of 16)
///   - Picture offset within frame
///   - Frame rate as num/den fraction
///   - Pixel aspect ratio
///   - Color space + quality hint + keyframe granule shift
///   - Pixel format (4:2:0 / 4:2:2 / 4:4:4)
/// Computes the derived block / superblock / macroblock counts.
/// @param dec Decoder receiving validated identification metadata.
/// @param data Borrowed identification packet.
/// @param len Accessible packet length.
/// @return 0 on success, or -1 for signature, bounds, version, geometry, rate, or format failure.
static int parse_id_header(theora_decoder_t *dec, const uint8_t *data, size_t len) {
    bitreader_t br;
    if (!check_header_sig(data, len, 0x80) || len < 42)
        return -1;

    dec->version_major = data[7];
    dec->version_minor = data[8];
    dec->version_sub = data[9];
    dec->frame_width = (uint32_t)((((uint16_t)data[10] << 8) | data[11]) * 16u);
    dec->frame_height = (uint32_t)((((uint16_t)data[12] << 8) | data[13]) * 16u);
    dec->pic_width = ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 8) | data[16];
    dec->pic_height = ((uint32_t)data[17] << 16) | ((uint32_t)data[18] << 8) | data[19];
    dec->pic_x = data[20];
    dec->pic_y = data[21];
    dec->fps_num = ((uint32_t)data[22] << 24) | ((uint32_t)data[23] << 16) |
                   ((uint32_t)data[24] << 8) | data[25];
    dec->fps_den = ((uint32_t)data[26] << 24) | ((uint32_t)data[27] << 16) |
                   ((uint32_t)data[28] << 8) | data[29];
    dec->aspect_num = ((uint32_t)data[30] << 24) | ((uint32_t)data[31] << 16) |
                      ((uint32_t)data[32] << 8) | data[33];
    dec->aspect_den = ((uint32_t)data[34] << 24) | ((uint32_t)data[35] << 16) |
                      ((uint32_t)data[36] << 8) | data[37];
    dec->color_space = data[38];

    br_init(&br, data + 40, len - 40);
    dec->quality_hint = br_read(&br, 6);
    dec->keyframe_granule_shift = br_read(&br, 5);
    dec->pixel_format = (uint8_t)br_read(&br, 2);
    if (br.failed)
        return -1;
    if (dec->version_major != 3)
        return -1;
    if (dec->frame_width == 0 || dec->frame_height == 0 || dec->frame_width > INT32_MAX ||
        dec->frame_height > INT32_MAX)
        return -1;
    if ((uint64_t)dec->frame_width > THEORA_MAX_FRAME_PIXELS / (uint64_t)dec->frame_height)
        return -1;
    if (dec->pic_width == 0 || dec->pic_height == 0 || dec->pic_width > dec->frame_width ||
        dec->pic_height > dec->frame_height)
        return -1;
    if (dec->pic_x > dec->frame_width - dec->pic_width ||
        dec->pic_y > dec->frame_height - dec->pic_height)
        return -1;
    if (dec->fps_num == 0 || dec->fps_den == 0)
        return -1;
    if (dec->pixel_format == 1 || dec->pixel_format > 3)
        return -1;
    if ((dec->pixel_format == 0 && ((dec->pic_x & 1u) || (dec->pic_y & 1u))) ||
        (dec->pixel_format == 2 && (dec->pic_x & 1u)))
        return -1;

    dec->block_cols = (int32_t)(dec->frame_width / 8);
    dec->block_rows = (int32_t)(dec->frame_height / 8);
    dec->superblock_cols = (dec->block_cols + 3) / 4;
    dec->superblock_rows = (dec->block_rows + 3) / 4;
    dec->macro_cols = (dec->block_cols + 1) / 2;
    dec->macro_rows = (dec->block_rows + 1) / 2;
    return 0;
}

/// @brief Validate the 0x81 comment header (we don't actually parse the comments).
///
/// The decoder only needs to know the comment header was present (it
/// occupies a packet slot in the header sequence). Fields like vendor
/// string and user comments are ignored.
/// @param dec Decoder context; currently unused.
/// @param data Borrowed comment packet.
/// @param len Accessible packet length.
/// @return 0 for a valid comment-header signature, otherwise -1.
static int parse_comment_header(theora_decoder_t *dec, const uint8_t *data, size_t len) {
    (void)dec;
    return check_header_sig(data, len, 0x81) ? 0 : -1;
}

/// @brief Recursive Huffman tree builder — read leaf bit, then either
///        encode a symbol or recurse for left+right children.
///
/// `depth` enforces a 64-level maximum to bound stack use. Returns the
/// new node's index, or -1 on overflow / read failure.
/// @param tab Huffman table receiving parsed nodes.
/// @param br Bit reader positioned at the encoded node.
/// @param depth Current recursive tree depth.
/// @return New node index, or -1 for malformed, oversized, or truncated input.
static int huff_parse_node(theora_huff_table_t *tab, bitreader_t *br, int depth) {
    int idx;
    if (!tab || !br || depth > 64 || tab->node_count >= 64)
        return -1;
    idx = tab->node_count++;
    memset(&tab->nodes[idx], 0, sizeof(tab->nodes[idx]));
    if (br_read1(br)) {
        tab->nodes[idx].leaf = 1;
        /* br_read(br, 5) yields a 5-bit DCT token in [0, 31], which fits int8_t (max 127) with
         * room to spare. If the token alphabet is ever widened past 5 bits, widen this field to
         * int16_t too — the narrowing cast would otherwise silently wrap values >= 128. */
        tab->nodes[idx].token = (int8_t)br_read(br, 5);
    } else {
        int left = huff_parse_node(tab, br, depth + 1);
        int right = huff_parse_node(tab, br, depth + 1);
        if (left < 0 || right < 0)
            return -1;
        tab->nodes[idx].left = (int16_t)left;
        tab->nodes[idx].right = (int16_t)right;
    }
    return br->failed ? -1 : idx;
}

/// @brief Decode one symbol from the bitstream by walking the Huffman tree.
///
/// One bit per tree edge until a leaf node is reached; returns the
/// leaf's token. Returns -1 on bitreader failure or invalid index.
/// @param tab Parsed Huffman table.
/// @param br Bit reader positioned at the encoded symbol.
/// @return Decoded token, or -1 for invalid state or truncated input.
static int huff_decode_token(const theora_huff_table_t *tab, bitreader_t *br) {
    int idx = 0;
    if (!tab || !br || tab->node_count <= 0)
        return -1;
    while (!tab->nodes[idx].leaf) {
        int bit = br_read1(br);
        if (br->failed)
            return -1;
        idx = bit ? tab->nodes[idx].right : tab->nodes[idx].left;
        if (idx < 0 || idx >= tab->node_count)
            return -1;
    }
    return tab->nodes[idx].token;
}

/// @brief Build the per-plane block layout tables and allocate the working buffers.
///
/// Lays out:
///   - Per-plane block/superblock counts and offsets.
///   - Hilbert-curve ordering tables that map raster (x,y) → coded index
///     (Theora codes blocks/superblocks/macroblocks in space-filling
///     curve order for compression efficiency).
///   - Per-block parallel arrays: bcoded, qiis, tis, ncoeffs, mvects, coeffs.
///   - Per-MB and per-SB state arrays.
/// Returns -1 on any malloc failure (caller cleans up via `theora_priv_free`).
/// @param dec Decoder supplying validated frame geometry and chroma format.
/// @param priv Private decoder state receiving layout tables and working arrays.
/// @return 0 on success, or -1 for invalid layout, overflow, or allocation failure.
static int theora_alloc_layout(theora_decoder_t *dec, theora_priv_t *priv) {
    int plane;
    int32_t block_offset = 0;
    int32_t sb_offset = 0;
    int32_t total_blocks = 0;
    int32_t total_sbs = 0;
    int32_t mb_count;

    if (!dec || !priv)
        return -1;

    priv->plane_width[0] = (int32_t)dec->frame_width;
    priv->plane_height[0] = (int32_t)dec->frame_height;
    switch (dec->pixel_format) {
        case 0:
            priv->plane_width[1] = priv->plane_width[2] = (int32_t)(dec->frame_width / 2);
            priv->plane_height[1] = priv->plane_height[2] = (int32_t)(dec->frame_height / 2);
            break;
        case 2:
            priv->plane_width[1] = priv->plane_width[2] = (int32_t)(dec->frame_width / 2);
            priv->plane_height[1] = priv->plane_height[2] = (int32_t)dec->frame_height;
            break;
        case 3:
            priv->plane_width[1] = priv->plane_width[2] = (int32_t)dec->frame_width;
            priv->plane_height[1] = priv->plane_height[2] = (int32_t)dec->frame_height;
            break;
        default:
            return -1;
    }

    for (plane = 0; plane < 3; plane++) {
        priv->plane_block_cols[plane] = priv->plane_width[plane] / 8;
        priv->plane_block_rows[plane] = priv->plane_height[plane] / 8;
        priv->plane_sb_cols[plane] = (priv->plane_block_cols[plane] + 3) / 4;
        priv->plane_sb_rows[plane] = (priv->plane_block_rows[plane] + 3) / 4;
        priv->plane_block_offsets[plane] = block_offset;
        priv->plane_sb_offsets[plane] = sb_offset;
        if (!theora_checked_mul_i32(priv->plane_block_cols[plane],
                                    priv->plane_block_rows[plane],
                                    &priv->plane_block_counts[plane]) ||
            !theora_checked_mul_i32(priv->plane_sb_cols[plane],
                                    priv->plane_sb_rows[plane],
                                    &priv->plane_sb_counts[plane]) ||
            !theora_checked_add_i32(block_offset, priv->plane_block_counts[plane], &block_offset) ||
            !theora_checked_add_i32(sb_offset, priv->plane_sb_counts[plane], &sb_offset))
            return -1;
    }

    total_blocks = block_offset;
    total_sbs = sb_offset;
    if (!theora_checked_mul_i32(dec->macro_cols, dec->macro_rows, &mb_count))
        return -1;
    priv->total_blocks = total_blocks;
    priv->total_superblocks = total_sbs;
    priv->total_macroblocks = mb_count;

    priv->plane_raster_to_coded[0] =
        (int32_t *)malloc((size_t)priv->plane_block_counts[0] * sizeof(int32_t));
    priv->plane_raster_to_coded[1] =
        (int32_t *)malloc((size_t)priv->plane_block_counts[1] * sizeof(int32_t));
    priv->plane_raster_to_coded[2] =
        (int32_t *)malloc((size_t)priv->plane_block_counts[2] * sizeof(int32_t));
    priv->mb_raster_to_coded = (int32_t *)malloc((size_t)mb_count * sizeof(int32_t));
    priv->blocks = (theora_block_info_t *)calloc((size_t)total_blocks, sizeof(theora_block_info_t));
    priv->mbs = (theora_mb_info_t *)calloc((size_t)mb_count, sizeof(theora_mb_info_t));
    priv->bcoded = (uint8_t *)calloc((size_t)total_blocks, 1);
    priv->sb_partcoded = (uint8_t *)calloc((size_t)total_sbs, 1);
    priv->sb_fullcoded = (uint8_t *)calloc((size_t)total_sbs, 1);
    priv->qiis = (uint8_t *)calloc((size_t)total_blocks, 1);
    priv->mbmodes = (uint8_t *)calloc((size_t)mb_count, 1);
    priv->tis = (uint8_t *)calloc((size_t)total_blocks, 1);
    priv->ncoeffs = (uint8_t *)calloc((size_t)total_blocks, 1);
    priv->mvects = (int8_t (*)[2])calloc((size_t)total_blocks, sizeof(int8_t[2]));
    priv->coeffs = (int16_t (*)[64])calloc((size_t)total_blocks, sizeof(int16_t[64]));

    if (!priv->plane_raster_to_coded[0] || !priv->plane_raster_to_coded[1] ||
        !priv->plane_raster_to_coded[2] || !priv->mb_raster_to_coded || !priv->blocks ||
        !priv->mbs || !priv->bcoded || !priv->sb_partcoded || !priv->sb_fullcoded || !priv->qiis ||
        !priv->mbmodes || !priv->tis || !priv->ncoeffs || !priv->mvects || !priv->coeffs) {
        theora_priv_free(dec);
        return -1;
    }

    for (int32_t mbi = 0; mbi < mb_count; mbi++) {
        for (int i = 0; i < 4; i++) {
            priv->mbs[mbi].luma[i] = -1;
            priv->mbs[mbi].chroma[0][i] = -1;
            priv->mbs[mbi].chroma[1][i] = -1;
        }
    }

    {
        int32_t mbi = 0;
        for (int32_t sby = 0; sby < dec->macro_rows; sby += 2) {
            for (int32_t sbx = 0; sbx < dec->macro_cols; sbx += 2) {
                for (int i = 0; i < 4; i++) {
                    int32_t mbx = sbx + mb_hilbert_x[i];
                    int32_t mby = sby + mb_hilbert_y[i];
                    if (mbx < dec->macro_cols && mby < dec->macro_rows)
                        priv->mb_raster_to_coded[mby * dec->macro_cols + mbx] = mbi++;
                }
            }
        }
    }

    {
        int32_t bi = 0;
        for (plane = 0; plane < 3; plane++) {
            for (int32_t sby = 0; sby < priv->plane_sb_rows[plane]; sby++) {
                for (int32_t sbx = 0; sbx < priv->plane_sb_cols[plane]; sbx++) {
                    for (int i = 0; i < 16; i++) {
                        int32_t bx = sbx * 4 + sb_hilbert_x[i];
                        int32_t by = sby * 4 + sb_hilbert_y[i];
                        int32_t raster;
                        int32_t mbx;
                        int32_t mby;
                        int32_t local_x;
                        int32_t local_y;
                        if (bx >= priv->plane_block_cols[plane] ||
                            by >= priv->plane_block_rows[plane])
                            continue;
                        raster = by * priv->plane_block_cols[plane] + bx;
                        priv->plane_raster_to_coded[plane][raster] = bi;
                        priv->blocks[bi].plane = plane;
                        priv->blocks[bi].bx = bx;
                        priv->blocks[bi].by = by;
                        priv->blocks[bi].plane_raster = raster;

                        mbx = bx / plane_mb_block_w(dec, plane);
                        mby = by / plane_mb_block_h(dec, plane);
                        priv->blocks[bi].mb = priv->mb_raster_to_coded[mby * dec->macro_cols + mbx];
                        local_x = bx % plane_mb_block_w(dec, plane);
                        local_y = by % plane_mb_block_h(dec, plane);
                        if (plane == 0) {
                            int slot = local_y * 2 + local_x;
                            priv->mbs[priv->blocks[bi].mb].luma[slot] = bi;
                        } else if (dec->pixel_format == 0) {
                            priv->mbs[priv->blocks[bi].mb].chroma[plane - 1][0] = bi;
                        } else if (dec->pixel_format == 2) {
                            int slot = local_y;
                            priv->mbs[priv->blocks[bi].mb].chroma[plane - 1][slot] = bi;
                        } else {
                            int slot = local_y * 2 + local_x;
                            priv->mbs[priv->blocks[bi].mb].chroma[plane - 1][slot] = bi;
                        }
                        bi++;
                    }
                }
            }
        }
    }

    return 0;
}

/// @brief Free the decoder's reference and current YCbCr frame planes.
///
/// @details Header parsing allocates nine plane buffers after the setup header
///   has been decoded. If one allocation in that group fails, this helper
///   releases any buffers that were already allocated so callers can return a
///   setup-header error without leaving transient heap ownership in the decoder.
///   It does not touch quantization, Huffman, or other private setup data; those
///   remain owned by @c theora_priv_free.
///
/// @param dec Decoder whose frame-plane pointers should be released.
static void theora_free_frame_planes(theora_decoder_t *dec) {
    if (!dec)
        return;
    free(dec->ref_y);
    free(dec->ref_cb);
    free(dec->ref_cr);
    free(dec->gold_y);
    free(dec->gold_cb);
    free(dec->gold_cr);
    free(dec->cur_y);
    free(dec->cur_cb);
    free(dec->cur_cr);
    dec->ref_y = dec->ref_cb = dec->ref_cr = NULL;
    dec->gold_y = dec->gold_cb = dec->gold_cr = NULL;
    dec->cur_y = dec->cur_cb = dec->cur_cr = NULL;
}

/// @brief Compute the quantization scale for one (qti, plane, qi, coeff) cell.
///
/// Combines the DC/AC base scale, the per-plane base matrix, the
/// quantization-range remapping, and the AC/INTER scaling factor.
/// Result is clamped to the spec-defined range [floor, 4096], then
/// to a per-coeff minimum (8 for DC luma, 2 for chroma DC, etc.).
/// Used to pre-bake the qmat tables consulted during inverse quant.
/// @param priv Parsed quantization setup state.
/// @param qti Quantization type: intra or inter.
/// @param pli Plane index.
/// @param qi Quantizer index in [0, 63].
/// @param ci Coefficient index in [0, 63].
/// @return Clamped quantization scale for the specified lookup cell.
static uint16_t theora_compute_qscale(const theora_priv_t *priv, int qti, int pli, int qi, int ci) {
    uint16_t qscale;
    uint16_t bm = 0;
    int qri;
    int qi_start = 0;
    if (ci == 0)
        qscale = priv->dc_scale[qi];
    else
        qscale = priv->ac_scale[qi];
    qri = 0;
    while (qri + 1 < priv->nqrs[qti][pli] && qi >= qi_start + priv->qrsizes[qti][pli][qri]) {
        qi_start += priv->qrsizes[qti][pli][qri];
        qri++;
    }
    bm = priv->bms[priv->qrbmis[qti][pli][qri]][ci];
    {
        uint32_t q = (uint32_t)qscale * (uint32_t)bm / 100u;
        uint16_t floorv = (ci == 0) ? 4 : 2;
        uint16_t minv = (ci == 0) ? 8 : ((ci < 3) ? 2 : 1);
        uint32_t scaled = qti == 0 ? q * 4u : q * 3u;
        uint32_t clamped = scaled < floorv ? floorv : scaled;
        if (clamped > 4096u)
            clamped = 4096u;
        if (clamped < minv)
            clamped = minv;
        return (uint16_t)clamped;
    }
}

/// @brief Finalize decoder setup after all three header packets are parsed.
///
/// Pre-bakes the full quantization matrix lookup table (2 × 3 × 64 ×
/// 64 entries), allocates the layout buffers, computes plane strides,
/// and allocates the YUV output planes. Called once, just before the
/// first compressed packet is decoded.
/// @param dec Decoder receiving matrices, strides, and frame buffers.
/// @param priv Fully parsed private setup state.
/// @return 0 on success, or -1 for invalid sizing or allocation failure.
static int theora_finish_setup(theora_decoder_t *dec, theora_priv_t *priv) {
    int32_t fw, fh;
    size_t y_size, c_size;
    for (int qti = 0; qti < 2; qti++) {
        for (int pli = 0; pli < 3; pli++) {
            for (int qi = 0; qi < 64; qi++) {
                for (int ci = 0; ci < 64; ci++)
                    dec->qmat[qti][pli][qi][ci] = theora_compute_qscale(priv, qti, pli, qi, ci);
            }
        }
    }
    dec->qmat_count = 2 * 3 * 64;

    if (theora_alloc_layout(dec, priv) != 0)
        return -1;

    fw = (int32_t)dec->frame_width;
    fh = (int32_t)dec->frame_height;
    dec->y_stride = fw;
    dec->c_stride = priv->plane_width[1];
    dec->y_height = fh;
    dec->c_height = priv->plane_height[1];
    if (!theora_checked_frame_plane_sizes(
            dec->y_stride, dec->y_height, dec->c_stride, dec->c_height, &y_size, &c_size)) {
        theora_priv_free(dec);
        return -1;
    }

    dec->ref_y = (uint8_t *)calloc(y_size, 1);
    dec->ref_cb = (uint8_t *)calloc(c_size, 1);
    dec->ref_cr = (uint8_t *)calloc(c_size, 1);
    dec->gold_y = (uint8_t *)calloc(y_size, 1);
    dec->gold_cb = (uint8_t *)calloc(c_size, 1);
    dec->gold_cr = (uint8_t *)calloc(c_size, 1);
    dec->cur_y = (uint8_t *)calloc(y_size, 1);
    dec->cur_cb = (uint8_t *)calloc(c_size, 1);
    dec->cur_cr = (uint8_t *)calloc(c_size, 1);
    if (!dec->ref_y || !dec->ref_cb || !dec->ref_cr || !dec->gold_y || !dec->gold_cb ||
        !dec->gold_cr || !dec->cur_y || !dec->cur_cb || !dec->cur_cr) {
        theora_free_frame_planes(dec);
        theora_priv_free(dec);
        return -1;
    }
    dec->headers_complete = 1;
    return 0;
}

/// @brief Parse the Theora setup header (packet type 0x82) — the third of the three
///        required headers. Allocates and initialises `theora_priv_t` with the QP tables,
///        base matrices, quantization ranges, and Huffman codebooks read from the bitstream.
/// @details The setup header contains the loop filter limits array, AC/DC scale arrays,
///   the base matrix table (nbms matrices of 64 bytes each), per-plane quantization
///   ranges, and the Huffman tree definitions. All of these are stored in `priv` for
///   use during frame decoding. Returns -1 if the signature check fails, if the
///   bitstream is malformed, or if any allocation fails; in those cases `dec->priv` is
///   left as set (callers free it via the decoder finalizer). Sets `headers_complete = 1`
///   on success so subsequent calls know the decoder is ready for frame data.
/// @param dec Decoder receiving setup state and allocated frame buffers.
/// @param data Borrowed setup packet.
/// @param len Accessible packet length.
/// @return 0 on success, or -1 for malformed syntax, bounds, or allocation failure.
static int parse_setup_header(theora_decoder_t *dec, const uint8_t *data, size_t len) {
    theora_priv_t *priv;
    bitreader_t br;
    if (!check_header_sig(data, len, 0x82))
        return -1;

    priv = (theora_priv_t *)calloc(1, sizeof(*priv));
    if (!priv)
        return -1;
    dec->priv = priv;

    br_init(&br, data + 7, len - 7);
    {
        int nbits = (int)br_read(&br, 3);
        for (int qi = 0; qi < 64; qi++)
            dec->loop_filter_limits[qi] = (uint8_t)br_read(&br, nbits);
    }

    {
        int nbits = (int)br_read(&br, 4) + 1;
        for (int qi = 0; qi < 64; qi++)
            priv->ac_scale[qi] = (uint16_t)br_read(&br, nbits);
    }

    {
        int nbits = (int)br_read(&br, 4) + 1;
        for (int qi = 0; qi < 64; qi++)
            priv->dc_scale[qi] = (uint16_t)br_read(&br, nbits);
    }

    priv->nbms = (int)br_read(&br, 9) + 1;
    if (priv->nbms <= 0 || priv->nbms > 384 || br.failed)
        return -1;

    for (int bmi = 0; bmi < priv->nbms; bmi++) {
        for (int ci = 0; ci < 64; ci++)
            priv->bms[bmi][ci] = (uint8_t)br_read(&br, 8);
    }

    for (int qti = 0; qti < 2; qti++) {
        for (int pli = 0; pli < 3; pli++) {
            int newqr = (qti == 0 && pli == 0) ? 1 : br_read1(&br);
            if (newqr) {
                int qi = 0;
                int qri = 0;
                while (1) {
                    int nbits = ilog_u32((uint32_t)(priv->nbms - 1));
                    priv->qrbmis[qti][pli][qri] = (uint16_t)br_read(&br, nbits);
                    if (br.failed || priv->qrbmis[qti][pli][qri] >= (uint16_t)priv->nbms)
                        return -1;
                    if (qi >= 63) {
                        priv->qrsizes[qti][pli][qri] = 0;
                        priv->nqrs[qti][pli] = (uint8_t)(qri + 1);
                        break;
                    }
                    nbits = ilog_u32((uint32_t)(62 - qi));
                    priv->qrsizes[qti][pli][qri] = (uint8_t)(br_read(&br, nbits) + 1);
                    qi += priv->qrsizes[qti][pli][qri];
                    qri++;
                    if (qri >= 64 || qi > 63)
                        return -1;
                }
            } else {
                if (qti > 0 && br_read1(&br)) {
                    memcpy(priv->qrbmis[qti][pli],
                           priv->qrbmis[qti - 1][pli],
                           sizeof(priv->qrbmis[qti][pli]));
                    memcpy(priv->qrsizes[qti][pli],
                           priv->qrsizes[qti - 1][pli],
                           sizeof(priv->qrsizes[qti][pli]));
                    priv->nqrs[qti][pli] = priv->nqrs[qti - 1][pli];
                } else {
                    int src_qti = (3 * qti + pli - 1) / 3;
                    int src_pli = (pli + 2) % 3;
                    memcpy(priv->qrbmis[qti][pli],
                           priv->qrbmis[src_qti][src_pli],
                           sizeof(priv->qrbmis[qti][pli]));
                    memcpy(priv->qrsizes[qti][pli],
                           priv->qrsizes[src_qti][src_pli],
                           sizeof(priv->qrsizes[qti][pli]));
                    priv->nqrs[qti][pli] = priv->nqrs[src_qti][src_pli];
                }
            }
        }
    }

    for (int hti = 0; hti < 80; hti++) {
        priv->huff[hti].node_count = 0;
        if (huff_parse_node(&priv->huff[hti], &br, 0) < 0 || br.failed)
            return -1;
    }

    return theora_finish_setup(dec, priv);
}

/// @brief Public: zero-initialize a `theora_decoder_t`.
///
/// Caller-allocated struct; this just clears the slab. Allocations
/// happen lazily as headers and frames are parsed.
/// @param dec Caller-owned decoder storage to zero.
void theora_decoder_init(theora_decoder_t *dec) {
    memset(dec, 0, sizeof(*dec));
}

/// @brief Public: release every heap allocation owned by the decoder.
///
/// Frees the YUV reference frames (current, previous, golden) plus
/// the private state via `theora_priv_free`. Re-zeros the struct
/// so it could be reused with another `_init` call.
/// @param dec Decoder to release; NULL is accepted as a no-op.
void theora_decoder_free(theora_decoder_t *dec) {
    if (!dec)
        return;
    theora_free_frame_planes(dec);
    theora_priv_free(dec);
    memset(dec, 0, sizeof(*dec));
}

/// @brief Public: classify a packet as a Theora header packet (vs. data).
///
/// Header packets begin with type byte 0x80/0x81/0x82 followed by the
/// magic ASCII string "theora". Used by Ogg demuxers to route packets
/// to `decode_header` vs. `decode_frame`.
/// @param data Borrowed packet bytes.
/// @param len Accessible packet length.
/// @return 1 for a recognized bounded Theora header signature, otherwise 0.
int theora_is_header_packet(const uint8_t *data, size_t len) {
    if (!data || len < 7)
        return 0;
    if (data[0] != 0x80 && data[0] != 0x81 && data[0] != 0x82)
        return 0;
    return data[1] == 't' && data[2] == 'h' && data[3] == 'e' && data[4] == 'o' && data[5] == 'r' &&
           data[6] == 'a';
}

/// @brief Public: dispatch one of the three header packets to its parser.
///
/// Returns:
///   - 0 on successful header parse.
///   - 1 if `data` isn't a Theora header packet (caller should treat
///     it as a data packet).
///   - -1 on any structural error in the header payload.
/// Theora requires id (0x80), comment (0x81), setup (0x82) in that
/// order before the first data packet.
/// @param dec Decoder receiving parsed header state.
/// @param data Borrowed packet bytes.
/// @param len Accessible packet length.
/// @return 0 for a parsed header, 1 for a non-header packet, or -1 on invalid input/header syntax.
int theora_decode_header(theora_decoder_t *dec, const uint8_t *data, size_t len) {
    if (!dec || !data || len < 1)
        return -1;
    if (!theora_is_header_packet(data, len))
        return 1;
    switch (data[0]) {
        case 0x80:
            return parse_id_header(dec, data, len);
        case 0x81:
            return parse_comment_header(dec, data, len);
        case 0x82:
            return parse_setup_header(dec, data, len);
        default:
            return -1;
    }
}

/// @brief Read the per-frame header (frame type + 1-3 quantization indices).
///
/// First bit must be 0 (denotes a video data packet). Frame type:
/// 0=intra (keyframe), 1=inter (predicted from previous). 1-3 QI
/// values follow, each indexing the per-color-plane quantization
/// table; the count is encoded as 0/1/2 trailing flag bits.
/// @param dec Decoder whose private state records whether a reference frame exists.
/// @param br Bit reader positioned at a video data packet.
/// @param fh Output frame-header structure.
/// @return 0 on success, or -1 for invalid packet bits or an inter frame before the first keyframe.
int decode_frame_header(theora_decoder_t *dec, bitreader_t *br, theora_frame_header_t *fh) {
    int has_more;
    if (br_read1(br) != 0)
        return -1;
    fh->frame_type = br_read1(br);
    fh->qi[0] = (uint8_t)br_read(br, 6);
    fh->nqi = 1;
    has_more = br_read1(br);
    if (has_more) {
        fh->qi[1] = (uint8_t)br_read(br, 6);
        fh->nqi = 2;
        has_more = br_read1(br);
        if (has_more) {
            fh->qi[2] = (uint8_t)br_read(br, 6);
            fh->nqi = 3;
        }
    }
    if (fh->frame_type == 0)
        (void)br_read(br, 3);
    if (br->failed)
        return -1;
    if (fh->frame_type != 0 && fh->frame_type != 1)
        return -1;
    if (!((theora_priv_t *)dec->priv)->first_frame_decoded && fh->frame_type != 0)
        return -1;
    return 0;
}

/// @brief Decode a long-run-length value (variable-length; 1..4127 range).
///
/// Six-bucket prefix code: 0 → 1; 10x → 2-3; 110xx → 4-7; 1110xxx →
/// 8-15; 11110xxxx → 16-31; 11111xxxxxxxxxxxx → 32-4127. Used to
/// run-length-encode the bcoded bitmap and other per-block flags.
/// @param br Bit reader positioned at a long run.
/// @return Decoded run length in [1, 4127]; reader failure is latched separately.
static int decode_long_run_length(bitreader_t *br) {
    int bit0 = br_read1(br);
    if (bit0 == 0)
        return 1;
    if (br_read1(br) == 0)
        return (int)br_read(br, 1) + 2;
    if (br_read1(br) == 0)
        return (int)br_read(br, 2) + 4;
    if (br_read1(br) == 0)
        return (int)br_read(br, 3) + 8;
    if (br_read1(br) == 0)
        return (int)br_read(br, 4) + 16;
    return (int)br_read(br, 12) + 32;
}

/// @brief Decode a short run-length value from the bitstream (for partially-coded SBs).
/// @details Uses a variable-length prefix code:
///   `0` → run = 1; `10` → reads 1 bit → run in [2,3]; `110` → reads 2 bits → [4,7];
///   `111` → reads 4 bits → [8,23]. Short runs are used in the final pass of
///   `decode_block_flags` for blocks within partially-coded superblocks.
/// @param br Bit reader positioned at a short run.
/// @return Run length in [1, 23].
static int decode_short_run_length(bitreader_t *br) {
    int bit0 = br_read1(br);
    if (bit0 == 0)
        return 1;
    if (br_read1(br) == 0)
        return (int)br_read(br, 1) + 2;
    if (br_read1(br) == 0)
        return (int)br_read(br, 2) + 4;
    return (int)br_read(br, 4) + 8;
}

/// @brief Decode a run-length-encoded bit array from the bitstream into @p out.
/// @details Each iteration reads one bit (the run value), then reads a run length via
///   `decode_long_run_length` (for superblock-level flags) or `decode_short_run_length`
///   (for block-level flags within partial superblocks). The run value is written to
///   that many consecutive entries in @p out. Returns 0 if exactly @p count entries
///   were written, -1 on bitstream failure or if a run length is nonsensical.
/// @param br Bit reader positioned at the first encoded run.
/// @param out Output bit array.
/// @param count Number of entries to fill.
/// @param long_runs 1 for superblock long runs, 0 for block short runs.
/// @return 0 after filling exactly @p count entries, otherwise -1.
static int decode_rle_bits(bitreader_t *br, uint8_t *out, int count, int long_runs) {
    int filled = 0;
    while (filled < count) {
        int bit = br_read1(br);
        int run = long_runs ? decode_long_run_length(br) : decode_short_run_length(br);
        if (br->failed || run <= 0)
            return -1;
        while (run-- > 0 && filled < count)
            out[filled++] = (uint8_t)bit;
    }
    return filled == count ? 0 : -1;
}

/// @brief Decode the coded/uncoded flags for all blocks in the frame.
/// @details For intra frames all blocks are marked coded. For inter frames the
///   Theora spec encodes block flags in a three-pass RLE scheme:
///   1. Partially-coded superblock flags (1 bit/SB, long-run RLE).
///   2. Fully-coded superblock flags for non-partial SBs (1 bit/SB, long-run RLE).
///   3. Per-block flags for blocks inside partially-coded SBs (short-run RLE).
///   Each pass requires a temporary heap buffer sized to the number of items decoded
///   in that pass; both buffers are freed before returning.
/// @param dec Decoder geometry used to traverse coded block order.
/// @param priv Private block and superblock state receiving flags.
/// @param br Bit reader positioned at block flags.
/// @param frame_type 0 for intra or 1 for inter.
/// @return 0 on success, -1 on bitstream error or allocation failure.
int decode_block_flags(theora_decoder_t *dec,
                       theora_priv_t *priv,
                       bitreader_t *br,
                       int frame_type) {
    memset(priv->bcoded, frame_type == 0 ? 1 : 0, (size_t)priv->total_blocks);
    if (frame_type == 0)
        return 0;

    if (decode_rle_bits(br, priv->sb_partcoded, priv->total_superblocks, 1) != 0)
        return -1;

    {
        int nbits = 0;
        uint8_t *bits;
        for (int sbi = 0; sbi < priv->total_superblocks; sbi++) {
            if (!priv->sb_partcoded[sbi])
                nbits++;
        }
        bits = nbits > 0 ? (uint8_t *)malloc((size_t)nbits) : NULL;
        if (nbits > 0) {
            if (!bits)
                return -1;
            if (decode_rle_bits(br, bits, nbits, 1) != 0) {
                free(bits);
                return -1;
            }
        }
        nbits = 0;
        for (int sbi = 0; sbi < priv->total_superblocks; sbi++) {
            if (!priv->sb_partcoded[sbi])
                priv->sb_fullcoded[sbi] = bits[nbits++];
        }
        free(bits);
    }

    for (int bi = 0; bi < priv->total_blocks; bi++) {
        int plane = priv->blocks[bi].plane;
        int sbx = priv->blocks[bi].bx / 4;
        int sby = priv->blocks[bi].by / 4;
        int sbi = priv->plane_sb_offsets[plane] + sby * priv->plane_sb_cols[plane] + sbx;
        if (!priv->sb_partcoded[sbi])
            priv->bcoded[bi] = priv->sb_fullcoded[sbi];
    }

    {
        int nbits = 0;
        uint8_t *bits;
        int cursor = 0;
        /* Count blocks belonging to partially-coded superblocks in a single O(total_blocks)
         * pass. (Previously this summed a per-superblock helper that itself scanned all blocks,
         * giving O(superblocks * blocks).) The superblock index expression and the
         * `sb_partcoded[sbi]` test mirror the distribution loop below exactly, so nbits stays in
         * lockstep with the number of bits that loop consumes. */
        for (int bi = 0; bi < priv->total_blocks; bi++) {
            int plane = priv->blocks[bi].plane;
            int sbx = priv->blocks[bi].bx / 4;
            int sby = priv->blocks[bi].by / 4;
            int sbi = priv->plane_sb_offsets[plane] + sby * priv->plane_sb_cols[plane] + sbx;
            if (priv->sb_partcoded[sbi])
                nbits++;
        }
        bits = nbits > 0 ? (uint8_t *)malloc((size_t)nbits) : NULL;
        if (nbits > 0) {
            if (!bits)
                return -1;
            if (decode_rle_bits(br, bits, nbits, 0) != 0) {
                free(bits);
                return -1;
            }
        }
        for (int bi = 0; bi < priv->total_blocks; bi++) {
            int plane = priv->blocks[bi].plane;
            int sbx = priv->blocks[bi].bx / 4;
            int sby = priv->blocks[bi].by / 4;
            int sbi = priv->plane_sb_offsets[plane] + sby * priv->plane_sb_cols[plane] + sbx;
            if (priv->sb_partcoded[sbi])
                priv->bcoded[bi] = bits[cursor++];
        }
        free(bits);
    }
    return 0;
}

/// @brief Decode one macroblock mode index using the default Theora Huffman code.
/// @details The default mode alphabet is a simple unary code where mode N is
///   encoded as N '1' bits followed by a '0' (except mode 0 which is just '0').
///   Returns the decoded mode index in [0,7] or -1 on bitstream failure.
/// @param br Bit reader positioned at a macroblock mode.
/// @return Mode alphabet index in [0, 7], or -1 on malformed or truncated input.
static int decode_mb_mode_huff(bitreader_t *br) {
    int code = 0;
    for (int len = 1; len <= 8; len++) {
        code = (code << 1) | br_read1(br);
        if (br->failed)
            return -1;
        if (len == 1 && code == 0)
            return 0;
        if (len == 2 && code == 2)
            return 1;
        if (len == 3 && code == 6)
            return 2;
        if (len == 4 && code == 14)
            return 3;
        if (len == 5 && code == 30)
            return 4;
        if (len == 6 && code == 62)
            return 5;
        if (len == 7 && code == 126)
            return 6;
        if (len == 8 && code == 255)
            return 7;
    }
    return -1;
}

/// @brief Test whether a macroblock has at least one coded luma block.
/// @details Used to determine whether motion-vector modes need to be decoded for a
///   macroblock: if none of its four luma blocks (Y0..Y3) are coded, the macroblock
///   is effectively uncoded and its mode defaults to 0 (INTER_NOMV).
/// @param priv Private decoder block and macroblock layout.
/// @param mbi Coded-order macroblock index.
/// @return 1 if any luma block index is valid and coded, 0 otherwise.
static int mb_has_coded_luma(const theora_priv_t *priv, int mbi) {
    for (int i = 0; i < 4; i++) {
        int bi = priv->mbs[mbi].luma[i];
        if (bi >= 0 && priv->bcoded[bi])
            return 1;
    }
    return 0;
}

/// @brief Decode the prediction mode for every macroblock in an inter frame.
/// @details For intra frames all MBs are marked as INTRA (mode 1). For inter frames,
///   a 3-bit mode scheme selector determines how modes are encoded:
///   - Scheme 0: custom 3-bit Huffman alphabet read from the bitstream.
///   - Schemes 1–6: one of the six pre-defined mode alphabets.
///   - Scheme 7: raw 3-bit mode index per MB (no Huffman coding).
///   Macroblocks without coded luma blocks are always assigned mode 0 (INTER_NOMV).
/// @param dec Decoder context; currently unused by mode parsing.
/// @param priv Private macroblock state receiving decoded modes.
/// @param br Bit reader positioned at the mode scheme.
/// @param frame_type 0 for intra or 1 for inter.
/// @return 0 on success, -1 on bitstream error.
int decode_mb_modes(theora_decoder_t *dec, theora_priv_t *priv, bitreader_t *br, int frame_type) {
    (void)dec;
    if (frame_type == 0) {
        memset(priv->mbmodes, 1, (size_t)priv->total_macroblocks);
        return 0;
    }

    {
        int mscheme = (int)br_read(br, 3);
        uint8_t alphabet[8];
        uint8_t seen[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        if (br->failed)
            return -1;
        if (mscheme == 0) {
            memset(alphabet, 0, sizeof(alphabet));
            for (int mode = 0; mode < 8; mode++) {
                int mi = (int)br_read(br, 3);
                if (mi < 0 || mi > 7 || br->failed || seen[mi])
                    return -1;
                seen[mi] = 1;
                alphabet[mi] = (uint8_t)mode;
            }
        } else if (mscheme != 7) {
            memcpy(alphabet, mb_mode_scheme[mscheme - 1], sizeof(alphabet));
        } else {
            memset(alphabet, 0, sizeof(alphabet));
        }

        for (int mbi = 0; mbi < priv->total_macroblocks; mbi++) {
            if (!mb_has_coded_luma(priv, mbi)) {
                priv->mbmodes[mbi] = 0;
            } else if (mscheme == 7) {
                priv->mbmodes[mbi] = (uint8_t)br_read(br, 3);
            } else {
                int mi = decode_mb_mode_huff(br);
                if (mi < 0)
                    return -1;
                priv->mbmodes[mbi] = alphabet[mi];
            }
            if (priv->mbmodes[mbi] > 7 || br->failed)
                return -1;
        }
    }
    return 0;
}

/// @brief Decode one motion-vector component (horizontal or vertical) from the bitstream.
/// @details The Theora motion-vector Huffman code encodes signed integer displacements
///   in the range roughly [-31, +31]. Very small magnitudes (0, ±1, ±2, ±3) have
///   short codewords; larger magnitudes (4..39) use progressively longer codes. The
///   sign of each non-zero displacement is encoded as an additional bit after the
///   magnitude codeword. Returns 0 on bitstream failure (sets br->failed = 1).
/// @param br Bit reader positioned at one motion-vector component.
/// @return Signed displacement in approximately [-31, +31], or 0 on error.
static int decode_mv_component_huff(bitreader_t *br) {
    int code = 0;
    for (int len = 1; len <= 8; len++) {
        code = (code << 1) | br_read1(br);
        if (br->failed)
            return 0;
        switch (len) {
            case 3:
                if (code == 0)
                    return 0;
                if (code == 1)
                    return 1;
                if (code == 2)
                    return -1;
                break;
            case 4:
                if (code == 6)
                    return 2;
                if (code == 7)
                    return -2;
                if (code == 8)
                    return 3;
                if (code == 9)
                    return -3;
                break;
            case 6:
                if (code >= 40 && code <= 55) {
                    int mag = ((code - 40) >> 1) + 4;
                    return ((code - 40) & 1) ? -mag : mag;
                }
                break;
            case 7:
                if (code >= 96 && code <= 127) {
                    int mag = ((code - 96) >> 1) + 8;
                    return ((code - 96) & 1) ? -mag : mag;
                }
                break;
            case 8:
                if (code >= 224 && code <= 255) {
                    int mag = ((code - 224) >> 1) + 24;
                    return ((code - 224) & 1) ? -mag : mag;
                } else if (code >= 192 && code <= 223) {
                    int mag = ((code - 192) >> 1) + 16;
                    return ((code - 192) & 1) ? -mag : mag;
                }
                break;
            default:
                break;
        }
    }
    br->failed = 1;
    return 0;
}

/// @brief Propagate a macroblock's motion vector to all of its coded blocks.
/// @details All four luma blocks and all chroma blocks in both Cb and Cr planes
///   that are marked coded receive the same MV (mvx, mvy). Uncoded blocks retain
///   their zero-initialized MV from the `memset` at the top of `decode_motion_vectors`.
///   This broadcast pattern is required because the residual reconstruction step
///   operates per-block and needs the MV stored alongside the block data.
/// @param priv Private block/macroblock state receiving vectors.
/// @param mbi Coded-order macroblock index.
/// @param mvx Horizontal motion component.
/// @param mvy Vertical motion component.
static void assign_mv_to_coded_blocks(const theora_priv_t *priv, int mbi, int mvx, int mvy) {
    for (int i = 0; i < 4; i++) {
        int bi = priv->mbs[mbi].luma[i];
        if (bi >= 0 && priv->bcoded[bi]) {
            priv->mvects[bi][0] = (int8_t)mvx;
            priv->mvects[bi][1] = (int8_t)mvy;
        }
    }
    for (int plane = 0; plane < 2; plane++) {
        for (int i = 0; i < 4; i++) {
            int bi = priv->mbs[mbi].chroma[plane][i];
            if (bi >= 0 && priv->bcoded[bi]) {
                priv->mvects[bi][0] = (int8_t)mvx;
                priv->mvects[bi][1] = (int8_t)mvy;
            }
        }
    }
}

/// @brief Decode all macroblock motion vectors for an inter frame.
/// @details The MV encoding scheme is determined by a 1-bit selector:
///   - Mode 0: each MV component is decoded with `decode_mv_component_huff`.
///   - Mode 1: each magnitude is read from five bits followed by a sign bit.
///   The MV for each coded macroblock is decoded according to its prediction mode:
///   - INTER modes: one MV per MB, assigned to all coded blocks via
///     `assign_mv_to_coded_blocks`. Last-MV tracking (`last1x`/`last1y`,
///     `last2x`/`last2y`) enables the INTER_MV_LAST and INTER_MV_LAST2 modes.
///   - GOLDEN modes use the same MV decoding but reference the golden frame buffer.
///   - INTRA/INTER_NOMV: MV = (0, 0).
/// @param dec Decoder supplying chroma subsampling geometry.
/// @param priv Private block/macroblock state receiving motion vectors.
/// @param br Bit reader positioned at the motion-vector coding selector.
/// @param frame_type 0 for intra or 1 for inter.
/// @return 0 on success, -1 on bitstream error.
int decode_motion_vectors(theora_decoder_t *dec,
                          theora_priv_t *priv,
                          bitreader_t *br,
                          int frame_type) {
    int mvmode = 0;
    int last1x = 0, last1y = 0, last2x = 0, last2y = 0;

    memset(priv->mvects, 0, (size_t)priv->total_blocks * sizeof(priv->mvects[0]));
    if (frame_type == 0)
        return 0;

    mvmode = br_read1(br);
    if (br->failed)
        return -1;

    for (int mbi = 0; mbi < priv->total_macroblocks; mbi++) {
        int mode = priv->mbmodes[mbi];
        int mvx = 0;
        int mvy = 0;
        if (mode == 7) {
            int a = priv->mbs[mbi].luma[0];
            int b = priv->mbs[mbi].luma[1];
            int c = priv->mbs[mbi].luma[2];
            int d = priv->mbs[mbi].luma[3];
            int last_mv_x = 0;
            int last_mv_y = 0;
            int order[4] = {a, b, c, d};
            for (int i = 0; i < 4; i++) {
                int bi = order[i];
                if (bi >= 0 && priv->bcoded[bi]) {
                    if (mvmode == 0) {
                        mvx = decode_mv_component_huff(br);
                        mvy = decode_mv_component_huff(br);
                    } else {
                        mvx = (int)br_read(br, 5);
                        if (br_read1(br))
                            mvx = -mvx;
                        mvy = (int)br_read(br, 5);
                        if (br_read1(br))
                            mvy = -mvy;
                    }
                    if (br->failed)
                        return -1;
                    priv->mvects[bi][0] = (int8_t)mvx;
                    priv->mvects[bi][1] = (int8_t)mvy;
                    last_mv_x = mvx;
                    last_mv_y = mvy;
                }
            }

            if (dec->pixel_format == 0) {
                int cb = priv->mbs[mbi].chroma[0][0];
                int cr = priv->mbs[mbi].chroma[1][0];
                int sumx = 0, sumy = 0;
                for (int i = 0; i < 4; i++) {
                    int bi = order[i];
                    if (bi >= 0) {
                        sumx += priv->mvects[bi][0];
                        sumy += priv->mvects[bi][1];
                    }
                }
                if (cb >= 0) {
                    priv->mvects[cb][0] = (int8_t)round_div_s32(sumx, 4);
                    priv->mvects[cb][1] = (int8_t)round_div_s32(sumy, 4);
                }
                if (cr >= 0) {
                    priv->mvects[cr][0] = (int8_t)round_div_s32(sumx, 4);
                    priv->mvects[cr][1] = (int8_t)round_div_s32(sumy, 4);
                }
            } else if (dec->pixel_format == 2) {
                int cb0 = priv->mbs[mbi].chroma[0][0];
                int cb1 = priv->mbs[mbi].chroma[0][1];
                int cr0 = priv->mbs[mbi].chroma[1][0];
                int cr1 = priv->mbs[mbi].chroma[1][1];
                int botx = round_div_s32(priv->mvects[a][0] + priv->mvects[b][0], 2);
                int boty = round_div_s32(priv->mvects[a][1] + priv->mvects[b][1], 2);
                int topx = round_div_s32(priv->mvects[c][0] + priv->mvects[d][0], 2);
                int topy = round_div_s32(priv->mvects[c][1] + priv->mvects[d][1], 2);
                if (cb0 >= 0) {
                    priv->mvects[cb0][0] = (int8_t)botx;
                    priv->mvects[cb0][1] = (int8_t)boty;
                }
                if (cr0 >= 0) {
                    priv->mvects[cr0][0] = (int8_t)botx;
                    priv->mvects[cr0][1] = (int8_t)boty;
                }
                if (cb1 >= 0) {
                    priv->mvects[cb1][0] = (int8_t)topx;
                    priv->mvects[cb1][1] = (int8_t)topy;
                }
                if (cr1 >= 0) {
                    priv->mvects[cr1][0] = (int8_t)topx;
                    priv->mvects[cr1][1] = (int8_t)topy;
                }
            } else {
                for (int i = 0; i < 4; i++) {
                    int cb = priv->mbs[mbi].chroma[0][i];
                    int cr = priv->mbs[mbi].chroma[1][i];
                    int src = order[i];
                    if (cb >= 0) {
                        priv->mvects[cb][0] = priv->mvects[src][0];
                        priv->mvects[cb][1] = priv->mvects[src][1];
                    }
                    if (cr >= 0) {
                        priv->mvects[cr][0] = priv->mvects[src][0];
                        priv->mvects[cr][1] = priv->mvects[src][1];
                    }
                }
            }
            last2x = last1x;
            last2y = last1y;
            last1x = last_mv_x;
            last1y = last_mv_y;
        } else {
            if (mode == 6 || mode == 2) {
                if (mvmode == 0) {
                    mvx = decode_mv_component_huff(br);
                    mvy = decode_mv_component_huff(br);
                } else {
                    mvx = (int)br_read(br, 5);
                    if (br_read1(br))
                        mvx = -mvx;
                    mvy = (int)br_read(br, 5);
                    if (br_read1(br))
                        mvy = -mvy;
                }
                if (mode == 2) {
                    last2x = last1x;
                    last2y = last1y;
                    last1x = mvx;
                    last1y = mvy;
                }
            } else if (mode == 4) {
                mvx = last2x;
                mvy = last2y;
                last2x = last1x;
                last2y = last1y;
                last1x = mvx;
                last1y = mvy;
            } else if (mode == 3) {
                mvx = last1x;
                mvy = last1y;
            }
            if (br->failed)
                return -1;
            assign_mv_to_coded_blocks(priv, mbi, mvx, mvy);
        }
    }
    return 0;
}

/// @brief Decode per-block quantization index selectors (QIIs) for the @p nqi QP levels.
/// @details Theora allows up to 3 quantization indices per frame (`nqi` from the frame
///   header). QIIs are encoded per coded block as (nqi-1) binary flags decoded with
///   long-run RLE. The final QII for each block determines which qi[] entry selects
///   the AC scale factor. All qiis are zero-initialized before the pass loop so blocks
///   with fewer QII bits than available QP levels default to QII=0.
/// @param priv Private coded-block state receiving QII selectors.
/// @param br Bit reader positioned at QII runs.
/// @param nqi Number of frame quantizer indices in [1, 3].
/// @return 0 on success, -1 on bitstream error or allocation failure.
int decode_qiis(theora_priv_t *priv, bitreader_t *br, int nqi) {
    memset(priv->qiis, 0, (size_t)priv->total_blocks);
    for (int qii = 0; qii < nqi - 1; qii++) {
        int nbits = 0;
        uint8_t *bits;
        int cursor = 0;
        for (int bi = 0; bi < priv->total_blocks; bi++) {
            if (priv->bcoded[bi] && priv->qiis[bi] == qii)
                nbits++;
        }
        bits = nbits > 0 ? (uint8_t *)malloc((size_t)nbits) : NULL;
        if (nbits > 0) {
            if (!bits)
                return -1;
            if (decode_rle_bits(br, bits, nbits, 1) != 0) {
                free(bits);
                return -1;
            }
        }
        for (int bi = 0; bi < priv->total_blocks; bi++) {
            if (priv->bcoded[bi] && priv->qiis[bi] == qii)
                priv->qiis[bi] = (uint8_t)(priv->qiis[bi] + bits[cursor++]);
        }
        free(bits);
    }
    return 0;
}

/// @brief Process an end-of-block token — zeros remaining coefficients and advances
///        the block's transform index past all 64 positions.
/// @details Tokens 0–5 encode run lengths from 1 to 31 via progressively wider
///   fixed-length bitfields. Token 6 reads a 12-bit field; if zero, it counts all
///   remaining uncompleted coded blocks as the run (a "clear to end of frame" token).
///   The @p *eobs counter is decremented for each subsequent coded block that falls
///   within the run, shortcutting the Huffman decode loop in `decode_coefficients`.
/// @param priv Private coefficient state to update.
/// @param br Bit reader positioned after the EOB token.
/// @param token EOB token in [0, 6].
/// @param bi Coded-order block index.
/// @param ti Current transform coefficient index.
/// @param eobs Output counter for subsequent blocks covered by the run.
/// @return 0 on success, -1 on bitstream error or invalid run.
static int decode_eob_token(
    theora_priv_t *priv, bitreader_t *br, int token, int bi, int ti, int *eobs) {
    int run = 0;
    switch (token) {
        case 0:
            run = 1;
            break;
        case 1:
            run = 2;
            break;
        case 2:
            run = 3;
            break;
        case 3:
            run = (int)br_read(br, 2) + 4;
            break;
        case 4:
            run = (int)br_read(br, 3) + 8;
            break;
        case 5:
            run = (int)br_read(br, 4) + 16;
            break;
        case 6:
            run = (int)br_read(br, 12);
            if (run == 0) {
                /* EOB token with a zero run code means "all remaining coded blocks finish here".
                 * Recompute that remaining count by scanning blocks not yet at ti==64. This is
                 * O(total_blocks) each time the token appears — bounded by the bitstream so never
                 * a hang, but an adversarial stream emitting it repeatedly makes coefficient
                 * decode quadratic. A running "blocks not yet complete" counter (decremented as
                 * each block's ti reaches 64) would make this O(1); it is left out because that
                 * decrement would have to be threaded through every ti update in the hot
                 * coefficient-decode path, a correctness risk disproportionate to the gain. */
                run = 0;
                for (int bj = 0; bj < priv->total_blocks; bj++) {
                    if (priv->bcoded[bj] && priv->tis[bj] < 64)
                        run++;
                }
            }
            break;
        default:
            return -1;
    }
    if (br->failed || run <= 0)
        return -1;
    for (int tj = ti; tj < 64; tj++)
        priv->coeffs[bi][tj] = 0;
    priv->ncoeffs[bi] = priv->tis[bi];
    priv->tis[bi] = 64;
    *eobs = run - 1;
    return 0;
}

/// @brief Decode one coefficient token into the block's coefficient array.
/// @details Theora defines 32 coefficient token values (tokens 7–31 after EOB tokens
///   0–6). Tokens 7–8 are zero-run tokens (write @p rlen zeros, advance ti). Tokens
///   9–22 encode direct coefficient values of increasing magnitude with optional sign
///   bits. Tokens 23–31 combine zero runs with a trailing non-zero coefficient. All
///   token handlers advance `priv->tis[bi]` to track where the next coefficient lands;
///   `priv->ncoeffs[bi]` is also updated to reflect the new highest populated index.
/// @param priv Private coefficient state to update.
/// @param br Bit reader positioned after the coefficient token.
/// @param token Coefficient token in [7, 31].
/// @param bi Coded-order block index.
/// @param ti Current transform coefficient index.
/// @return 0 on success, -1 on bitstream error or out-of-range index.
static int decode_coeff_token(theora_priv_t *priv, bitreader_t *br, int token, int bi, int ti) {
    int sign = 0;
    int mag = 0;
    int rlen = 0;
    if (token == 7 || token == 8) {
        rlen = (int)br_read(br, token == 7 ? 3 : 6) + 1;
        if (br->failed || ti + rlen > 64)
            return -1;
        for (int tj = ti; tj < ti + rlen; tj++)
            priv->coeffs[bi][tj] = 0;
        priv->tis[bi] = (uint8_t)(priv->tis[bi] + rlen);
        return 0;
    }
    if (ti >= 64)
        return -1;

    switch (token) {
        case 9:
            priv->coeffs[bi][ti] = 1;
            priv->tis[bi]++;
            break;
        case 10:
            priv->coeffs[bi][ti] = -1;
            priv->tis[bi]++;
            break;
        case 11:
            priv->coeffs[bi][ti] = 2;
            priv->tis[bi]++;
            break;
        case 12:
            priv->coeffs[bi][ti] = -2;
            priv->tis[bi]++;
            break;
        case 13:
        case 14:
        case 15:
        case 16:
            sign = br_read1(br);
            if (br->failed)
                return -1;
            priv->coeffs[bi][ti] = (int16_t)((sign ? -1 : 1) * (token - 10));
            priv->tis[bi]++;
            break;
        case 17:
            sign = br_read1(br);
            mag = (int)br_read(br, 1) + 7;
            if (br->failed)
                return -1;
            priv->coeffs[bi][ti] = (int16_t)(sign ? -mag : mag);
            priv->tis[bi]++;
            break;
        case 18:
            sign = br_read1(br);
            mag = (int)br_read(br, 2) + 9;
            if (br->failed)
                return -1;
            priv->coeffs[bi][ti] = (int16_t)(sign ? -mag : mag);
            priv->tis[bi]++;
            break;
        case 19:
            sign = br_read1(br);
            mag = (int)br_read(br, 3) + 13;
            if (br->failed)
                return -1;
            priv->coeffs[bi][ti] = (int16_t)(sign ? -mag : mag);
            priv->tis[bi]++;
            break;
        case 20:
            sign = br_read1(br);
            mag = (int)br_read(br, 4) + 21;
            if (br->failed)
                return -1;
            priv->coeffs[bi][ti] = (int16_t)(sign ? -mag : mag);
            priv->tis[bi]++;
            break;
        case 21:
            sign = br_read1(br);
            mag = (int)br_read(br, 5) + 37;
            if (br->failed)
                return -1;
            priv->coeffs[bi][ti] = (int16_t)(sign ? -mag : mag);
            priv->tis[bi]++;
            break;
        case 22:
            sign = br_read1(br);
            mag = (int)br_read(br, 9) + 69;
            if (br->failed)
                return -1;
            priv->coeffs[bi][ti] = (int16_t)(sign ? -mag : mag);
            priv->tis[bi]++;
            break;
        case 23:
            sign = br_read1(br);
            if (br->failed || ti + 1 >= 64)
                return -1;
            priv->coeffs[bi][ti] = 0;
            priv->coeffs[bi][ti + 1] = (int16_t)(sign ? -1 : 1);
            priv->tis[bi] += 2;
            break;
        case 24:
        case 25:
        case 26:
        case 27:
            sign = br_read1(br);
            rlen = token - 22;
            if (br->failed || ti + rlen >= 64)
                return -1;
            for (int tj = ti; tj < ti + rlen; tj++)
                priv->coeffs[bi][tj] = 0;
            priv->coeffs[bi][ti + rlen] = (int16_t)(sign ? -1 : 1);
            priv->tis[bi] += (uint8_t)(rlen + 1);
            break;
        case 28:
            sign = br_read1(br);
            rlen = (int)br_read(br, 2) + 6;
            if (br->failed || ti + rlen >= 64)
                return -1;
            for (int tj = ti; tj < ti + rlen; tj++)
                priv->coeffs[bi][tj] = 0;
            priv->coeffs[bi][ti + rlen] = (int16_t)(sign ? -1 : 1);
            priv->tis[bi] += (uint8_t)(rlen + 1);
            break;
        case 29:
            sign = br_read1(br);
            rlen = (int)br_read(br, 3) + 10;
            if (br->failed || ti + rlen >= 64)
                return -1;
            for (int tj = ti; tj < ti + rlen; tj++)
                priv->coeffs[bi][tj] = 0;
            priv->coeffs[bi][ti + rlen] = (int16_t)(sign ? -1 : 1);
            priv->tis[bi] += (uint8_t)(rlen + 1);
            break;
        case 30:
            sign = br_read1(br);
            mag = (int)br_read(br, 1) + 2;
            if (br->failed || ti + 1 >= 64)
                return -1;
            priv->coeffs[bi][ti] = 0;
            priv->coeffs[bi][ti + 1] = (int16_t)(sign ? -mag : mag);
            priv->tis[bi] += 2;
            break;
        case 31:
            sign = br_read1(br);
            mag = (int)br_read(br, 1) + 2;
            rlen = (int)br_read(br, 1) + 2;
            if (br->failed || ti + rlen >= 64)
                return -1;
            for (int tj = ti; tj < ti + rlen; tj++)
                priv->coeffs[bi][tj] = 0;
            priv->coeffs[bi][ti + rlen] = (int16_t)(sign ? -mag : mag);
            priv->tis[bi] += (uint8_t)(rlen + 1);
            break;
        default:
            return -1;
    }

    priv->ncoeffs[bi] = priv->tis[bi];
    return 0;
}

/// @brief Map a DCT coefficient index @p ti to its Huffman table group (0–4).
/// @details Theora partitions the 64 coefficient positions into 5 frequency bands
///   for Huffman coding: DC (ti=0), low AC (1–5), mid-low AC (6–14), mid-high AC
///   (15–27), and high AC (28–63). Each band can use a different Huffman table
///   for luma and chroma blocks. The group index is combined with `hti_l`/`hti_c`
///   to select the specific Huffman table from `priv->huff`.
/// @param ti Transform coefficient index in [0, 63].
/// @return Huffman group index in [0, 4].
static int huffman_group_for_ti(int ti) {
    if (ti == 0)
        return 0;
    if (ti <= 5)
        return 1;
    if (ti <= 14)
        return 2;
    if (ti <= 27)
        return 3;
    return 4;
}

/// @brief Decode all DCT coefficients for every coded block in the frame.
/// @details Iterates over all 64 coefficient positions (ti = 0..63) in the Theora
///   scan order. New Huffman table indices are read from the bitstream at ti=0 and
///   ti=1 (DC and first AC position each have independent table selectors for luma
///   and chroma). For each coded block at the current ti:
///   - If an eob run is active, the block's remaining coefficients are zeroed and
///     the eob counter is decremented.
///   - Otherwise, a token is decoded from the appropriate Huffman table, dispatched
///     to either `decode_eob_token` or `decode_coeff_token`.
///   Validation at the end confirms every coded block consumed all 64 positions.
/// @param priv Private coded-block and Huffman state receiving coefficients.
/// @param br Bit reader positioned at coefficient table selectors and tokens.
/// @return 0 on success, -1 on bitstream error or validation failure.
int decode_coefficients(theora_priv_t *priv, bitreader_t *br) {
    int hti_l = 0;
    int hti_c = 0;
    int nlbs = priv->total_macroblocks * 4;
    int eobs = 0;

    memset(priv->tis, 0, (size_t)priv->total_blocks);
    memset(priv->ncoeffs, 0, (size_t)priv->total_blocks);
    memset(priv->coeffs, 0, (size_t)priv->total_blocks * sizeof(priv->coeffs[0]));

    for (int ti = 0; ti < 64; ti++) {
        if (ti == 0 || ti == 1) {
            hti_l = (int)br_read(br, 4);
            hti_c = (int)br_read(br, 4);
            if (br->failed)
                return -1;
        }
        for (int bi = 0; bi < priv->total_blocks; bi++) {
            if (!priv->bcoded[bi] || priv->tis[bi] != ti)
                continue;
            priv->ncoeffs[bi] = (uint8_t)ti;
            if (eobs > 0) {
                for (int tj = ti; tj < 64; tj++)
                    priv->coeffs[bi][tj] = 0;
                priv->tis[bi] = 64;
                eobs--;
            } else {
                int hg = huffman_group_for_ti(ti);
                int hti = (bi < nlbs) ? (16 * hg + hti_l) : (16 * hg + hti_c);
                int token = huff_decode_token(&priv->huff[hti], br);
                if (token < 0)
                    return -1;
                if (token < 7) {
                    if (decode_eob_token(priv, br, token, bi, ti, &eobs) != 0)
                        return -1;
                } else {
                    if (decode_coeff_token(priv, br, token, bi, ti) != 0)
                        return -1;
                }
            }
        }
    }
    if (eobs != 0)
        return -1;
    for (int bi = 0; bi < priv->total_blocks; bi++) {
        if (priv->bcoded[bi] && priv->tis[bi] != 64)
            return -1;
    }
    return 0;
}

/// @brief Compute the DC prediction for block @p bi from its coded neighbors.
/// @details Theora's DC prediction uses up to four neighbors (left, upper-left, upper,
///   upper-right) with weights from the `dc_weights` table indexed by a 4-bit presence
///   mask. Only neighbors that are coded AND reference the same frame buffer as the
///   current block (same `rfi` from mode) contribute to the prediction. If no neighbor
///   qualifies, `lastdc[rfi]` is returned as a fall-back. The three-neighbor clamping
///   logic (lines ~1616–1623) guards against DC drift when the weighted average deviates
///   by more than 128 from any single neighbor.
/// @param priv Private block layout, mode, coded-flag, and coefficient state.
/// @param lastdc Last DC value for each of the three reference-frame slots.
/// @param bi Coded-order block index to predict.
/// @return DC prediction value in [-32767, 32767].
int compute_dc_pred(const theora_priv_t *priv, const int16_t lastdc[3], int bi) {
    static const int dx[4] = {-1, -1, 0, 1};
    static const int dy[4] = {0, -1, -1, -1};
    const theora_block_info_t *b = &priv->blocks[bi];
    int rfi = theora_ref_index_for_mode(priv->mbmodes[b->mb]);
    int present[4] = {0, 0, 0, 0};
    int pbi[4] = {-1, -1, -1, -1};
    int mask = 0;

    for (int i = 0; i < 4; i++) {
        int nbx = b->bx + dx[i];
        int nby = b->by + dy[i];
        if (nbx < 0 || nby < 0 || nbx >= priv->plane_block_cols[b->plane] ||
            nby >= priv->plane_block_rows[b->plane])
            continue;
        pbi[i] =
            priv->plane_raster_to_coded[b->plane][nby * priv->plane_block_cols[b->plane] + nbx];
        if (pbi[i] >= 0 && priv->bcoded[pbi[i]] &&
            theora_ref_index_for_mode(priv->mbmodes[priv->blocks[pbi[i]].mb]) == rfi) {
            present[i] = 1;
            mask |= 1 << i;
        }
    }

    if (mask == 0)
        return lastdc[rfi];

    {
        int sum = 0;
        const theora_dc_weight_t *w = &dc_weights[mask];
        for (int i = 0; i < 4; i++) {
            if (present[i])
                sum += w->w[i] * priv->coeffs[pbi[i]][0];
        }
        sum /= w->div;
        if (present[0] && present[1] && present[2]) {
            if (abs(sum - priv->coeffs[pbi[2]][0]) > 128)
                sum = priv->coeffs[pbi[2]][0];
            else if (abs(sum - priv->coeffs[pbi[0]][0]) > 128)
                sum = priv->coeffs[pbi[0]][0];
            else if (abs(sum - priv->coeffs[pbi[1]][0]) > 128)
                sum = priv->coeffs[pbi[1]][0];
        }
        return sum;
    }
}
