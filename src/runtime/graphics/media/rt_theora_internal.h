//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_theora_internal.h
// Purpose: Shared decoder state types, lookup tables, and helper declarations
//          for the Theora video decoder, split between rt_theora.c (bitstream +
//          header + frame decode) and rt_theora_recon.c (DC prediction, IDCT,
//          loop filter, motion-comp reconstruction).
//
// Key invariants:
//   - Engine-internal; included only by the graphics/media/ theora TUs.
//   - theora_priv_t holds all per-decoder state; the recon TU reads decoded
//     coefficients/modes from it and writes reconstructed planes back.
//
// Ownership/Lifetime:
//   - theora_priv_t and its buffers are owned by the enclosing rt_theora object.
//
// Links: src/runtime/graphics/media/rt_theora.c (decode),
//        src/runtime/graphics/media/rt_theora_recon.c (reconstruction)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Shares private Theora decoder state between entropy and reconstruction units.
 * @details Defines the sticky bit reader, Huffman representation, coded block
 * and macroblock layouts, quantizer state, per-frame header, DC predictor
 * weights, and cross-unit helper contracts. This header is internal to the
 * graphics media decoder and does not transfer ownership of any buffers.
 */
#pragma once

#include "rt_theora.h"

#include <stddef.h>
#include <stdint.h>

// --- Core decoder state + bitstream/huffman/block types ---
/**
 * @brief MSB-first packet bit reader with sticky failure state.
 * @details Once @ref failed becomes nonzero, subsequent reads return zero so
 * callers can check truncation once at the end of a syntax section.
 */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t byte_pos;
    int bit_pos;
    int failed;
} bitreader_t;

/** One node in a bounded binary Huffman decoding tree. */
typedef struct {
    int16_t left;
    int16_t right;
    uint8_t leaf;
    int8_t token;
} theora_huff_node_t;

/** Parsed Huffman tree and number of initialized nodes. */
typedef struct {
    theora_huff_node_t nodes[64];
    int node_count;
} theora_huff_table_t;

/** Coded-order location and macroblock ownership of one 8-by-8 block. */
typedef struct {
    int32_t plane;
    int32_t bx;
    int32_t by;
    int32_t plane_raster;
    int32_t mb;
} theora_block_info_t;

/** Coded block indexes associated with one luma/chroma macroblock. */
typedef struct {
    int32_t luma[4];
    int32_t chroma[2][4];
} theora_mb_info_t;

/**
 * @brief Complete decoder-private setup, layout, and per-frame entropy state.
 * @details Owns geometry lookup arrays, coded flags, modes, vectors,
 * coefficients, quantizer ranges, base matrices, and Huffman trees. Storage
 * referenced by pointer fields is allocated after setup-header validation and
 * released with the enclosing decoder.
 */
typedef struct {
    int32_t plane_width[3];
    int32_t plane_height[3];
    int32_t plane_block_cols[3];
    int32_t plane_block_rows[3];
    int32_t plane_sb_cols[3];
    int32_t plane_sb_rows[3];
    int32_t plane_block_offsets[3];
    int32_t plane_sb_offsets[3];
    int32_t plane_block_counts[3];
    int32_t plane_sb_counts[3];
    int32_t total_blocks;
    int32_t total_superblocks;
    int32_t total_macroblocks;

    int32_t *plane_raster_to_coded[3];
    int32_t *mb_raster_to_coded;
    theora_block_info_t *blocks;
    theora_mb_info_t *mbs;

    uint8_t *bcoded;
    uint8_t *sb_partcoded;
    uint8_t *sb_fullcoded;
    uint8_t *qiis;
    uint8_t *mbmodes;
    uint8_t *tis;
    uint8_t *ncoeffs;
    int8_t (*mvects)[2];
    int16_t (*coeffs)[64];

    uint16_t dc_scale[64];
    uint16_t ac_scale[64];
    uint8_t bms[384][64];
    uint8_t nqrs[2][3];
    uint8_t qrsizes[2][3][64];
    uint16_t qrbmis[2][3][64];
    int nbms;

    theora_huff_table_t huff[80];
    uint8_t first_frame_decoded;
} theora_priv_t;

// --- DC prediction weight table entry ---
/** Numerator weights and divisor for one available-neighbor DC prediction pattern. */
typedef struct {
    int16_t w[4];
    int16_t div;
} theora_dc_weight_t;

// --- Per-frame header (parsed from each frame packet) ---
/** Frame type and the one to three quantizer indexes selected by a packet header. */
typedef struct {
    int frame_type;
    int nqi;
    uint8_t qi[3];
} theora_frame_header_t;

//=============================================================================
// Shared lookup table + decode helpers (defined in rt_theora.c)
//=============================================================================

/// @brief Spec-defined mapping from coefficient scan order to raster coefficient index.
extern const uint8_t theora_zigzag[64];

/// @brief Initialize a sticky-failure MSB-first packet bit reader.
/// @param br Writable reader state.
/// @param data Borrowed packet bytes.
/// @param len Accessible packet length.
void br_init(bitreader_t *br, const uint8_t *data, size_t len);

/// @brief Clamp an integer to inclusive bounds.
/// @param v Value to clamp.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return Bounded value.
int clampi(int v, int lo, int hi);

/// @brief Wrap a signed value to the low sixteen bits without signed-overflow behavior.
/// @param v Value to truncate.
/// @return Low sixteen bits interpreted as int16_t.
int16_t trunc_i16(int32_t v);

/// @brief Map a macroblock prediction mode to its reference-frame slot.
/// @param mode Decoded Theora macroblock mode.
/// @return 0 for previous, 1 for last, or 2 for golden reference.
int theora_ref_index_for_mode(int mode);

/// @brief Decode frame type and one to three quantizer indices.
/// @param dec Decoder whose state determines whether an inter frame is currently legal.
/// @param br Bit reader positioned at a video packet.
/// @param fh Output frame-header state.
/// @return 0 on success, otherwise -1.
int decode_frame_header(theora_decoder_t *dec, bitreader_t *br, theora_frame_header_t *fh);

/// @brief Decode per-block coded flags for an intra or inter frame.
/// @param dec Decoder geometry.
/// @param priv Private block/superblock state receiving flags.
/// @param br Bit reader positioned at block flags.
/// @param frame_type 0 for intra or 1 for inter.
/// @return 0 on success, otherwise -1.
int decode_block_flags(theora_decoder_t *dec, theora_priv_t *priv, bitreader_t *br, int frame_type);

/// @brief Decode the prediction mode of every macroblock.
/// @param dec Decoder context.
/// @param priv Private macroblock state receiving modes.
/// @param br Bit reader positioned at the mode scheme.
/// @param frame_type 0 for intra or 1 for inter.
/// @return 0 on success, otherwise -1.
int decode_mb_modes(theora_decoder_t *dec, theora_priv_t *priv, bitreader_t *br, int frame_type);

/// @brief Decode and distribute motion vectors for all coded macroblocks.
/// @param dec Decoder supplying chroma geometry.
/// @param priv Private block state receiving vectors.
/// @param br Bit reader positioned at motion-vector data.
/// @param frame_type 0 for intra or 1 for inter.
/// @return 0 on success, otherwise -1.
int decode_motion_vectors(theora_decoder_t *dec, theora_priv_t *priv, bitreader_t *br, int frame_type);

/// @brief Decode each coded block's frame-quantizer selector.
/// @param priv Private block state receiving QII values.
/// @param br Bit reader positioned at QII runs.
/// @param nqi Number of available frame quantizer indices.
/// @return 0 on success, otherwise -1.
int decode_qiis(theora_priv_t *priv, bitreader_t *br, int nqi);

/// @brief Decode all Huffman-coded transform coefficients for coded blocks.
/// @param priv Private Huffman and coefficient state.
/// @param br Bit reader positioned at coefficient data.
/// @return 0 on success, otherwise -1.
int decode_coefficients(theora_priv_t *priv, bitreader_t *br);

/// @brief Predict one block's absolute DC coefficient from compatible neighbors.
/// @param priv Private block layout, mode, and coefficient state.
/// @param lastdc Last DC value for each reference-frame slot.
/// @param bi Coded-order block index.
/// @return Predicted DC coefficient.
int compute_dc_pred(const theora_priv_t *priv, const int16_t lastdc[3], int bi);
