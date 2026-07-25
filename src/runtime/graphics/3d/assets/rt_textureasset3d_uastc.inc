//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_textureasset3d_uastc.inc  (#included from
// rt_textureasset3d.c; not a standalone translation unit)
// Purpose: UASTC LDR 4x4 block decoding to RGBA8 — clean-room implementation from the
//   published "UASTC LDR 4x4 Texture Specification" (Binomial, public domain) and the
//   ASTC endpoint unquantization procedure in the Khronos Data Format spec §18.13.
// Key invariants:
//   - Blocks are 128 bits; all field reads are cursor-driven from the per-mode
//     description tables (bit layouts verified against the specification's tables).
//   - Invalid modes/patterns/etq values fail the block decode (recoverable error).
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements a clean-room UASTC LDR 4-by-4 software decoder.
///
/// The decoder consumes the UASTC mode prefix, transcoder hints, partition
/// selection, BISE-encoded endpoints, anchor-shortened weights, and optional
/// dual-plane component selection directly from each 128-bit block. Supported
/// blocks are expanded to row-major RGBA8 without external codec dependencies;
/// reserved modes and malformed bit budgets fail closed.
///
/// @note This file is included by `rt_textureasset3d.c` and is not a standalone
///       translation unit.

/// @brief Mode Huffman acceleration table (spec): low 7 bits of byte 0 -> mode index.
static const uint8_t uastc_mode_lut[128] = {
    11, 0, 10, 3, 11, 15, 12, 7, 11, 18, 10, 5, 11, 14, 12, 9, 11, 0, 10, 4, 11, 16,
    12, 8, 11, 18, 10, 6, 11, 2, 12, 13, 11, 0, 10, 3, 11, 17, 12, 7, 11, 18, 10, 5,
    11, 14, 12, 9, 11, 0, 10, 4, 11, 1, 12, 8, 11, 18, 10, 6, 11, 2, 12, 13, 11, 0,
    10, 3, 11, 19, 12, 7, 11, 18, 10, 5, 11, 14, 12, 9, 11, 0, 10, 4, 11, 16, 12, 8,
    11, 18, 10, 6, 11, 2, 12, 13, 11, 0, 10, 3, 11, 17, 12, 7, 11, 18, 10, 5, 11, 14,
    12, 9, 11, 0, 10, 4, 11, 1, 12, 8, 11, 18, 10, 6, 11, 2, 12, 13};

static const uint8_t uastc_mode_code_len[20] = {4, 6, 5, 5, 5, 5, 5, 5, 5, 5,
                                                3, 2, 3, 5, 5, 7, 6, 6, 4, 7};
static const uint8_t uastc_mode_weight_bits[19] = {4, 2, 3, 2, 2, 3, 2, 2, 0, 2,
                                                   4, 2, 3, 1, 2, 4, 2, 2, 5};
static const uint8_t uastc_mode_endpoint_range[19] = {19, 20, 8, 7, 12, 20, 18, 12, 0, 8,
                                                      13, 13, 19, 20, 20, 20, 20, 20, 11};
static const uint8_t uastc_mode_subsets[19] = {1, 1, 2, 3, 2, 1, 1, 2, 0, 2,
                                               1, 1, 1, 1, 1, 1, 2, 1, 1};
static const uint8_t uastc_mode_planes[19] = {1, 1, 1, 1, 1, 1, 2, 1, 0, 1,
                                              1, 2, 1, 2, 1, 1, 1, 2, 1};
static const uint8_t uastc_mode_comps[19] = {3, 3, 3, 3, 3, 3, 3, 3, 4, 4,
                                             4, 4, 4, 4, 4, 2, 2, 2, 3};
static const uint8_t uastc_mode_has_etc1_bias[19] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 1,
                                                     0, 0, 0, 1, 1, 1, 1, 1, 1};
static const uint8_t uastc_mode_has_bc1_hint1[19] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 1,
                                                     0, 0, 0, 1, 1, 1, 1, 1, 1};
static const uint8_t uastc_mode_has_alpha[19] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                                 1, 1, 1, 1, 1, 1, 1, 1, 0};

/// @brief BISE endpoint range parameters: bits, has-trit, has-quint (spec ranges table).
typedef struct {
    /// Number of directly encoded low bits per endpoint value.
    uint8_t bits;
    /// Nonzero when values also carry a base-3 trit.
    uint8_t trits;
    /// Nonzero when values also carry a base-5 quint.
    uint8_t quints;
} uastc_range_desc;

/// @brief Resolve a UASTC endpoint-range code to its BISE representation.
/// @param[in] range Endpoint range identifier from the selected mode.
/// @param[out] out Destination for direct-bit, trit, and quint metadata.
/// @return 1 for a supported range code, otherwise 0.
static int uastc_range_lookup(int range, uastc_range_desc *out) {
    switch (range) {
    case 7:
        *out = (uastc_range_desc){2, 1, 0};
        return 1;
    case 8:
        *out = (uastc_range_desc){4, 0, 0};
        return 1;
    case 11:
        *out = (uastc_range_desc){5, 0, 0};
        return 1;
    case 12:
        *out = (uastc_range_desc){3, 0, 1};
        return 1;
    case 13:
        *out = (uastc_range_desc){4, 1, 0};
        return 1;
    case 18:
        *out = (uastc_range_desc){5, 0, 1};
        return 1;
    case 19:
        *out = (uastc_range_desc){6, 1, 0};
        return 1;
    case 20:
        *out = (uastc_range_desc){8, 0, 0};
        return 1;
    default:
        return 0;
    }
}

/// @brief ASTC weight dequantization tables for 1..5-bit weights ([0,64] range).
static const uint8_t uastc_weights1[2] = {0, 64};
static const uint8_t uastc_weights2[4] = {0, 21, 43, 64};
static const uint8_t uastc_weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const uint8_t uastc_weights4[16] = {0, 4, 8, 12, 17, 21, 25, 29,
                                           35, 39, 43, 47, 52, 56, 60, 64};
static const uint8_t uastc_weights5[32] = {0,  2,  4,  6,  8,  10, 12, 14, 16, 18, 20,
                                           22, 24, 26, 28, 30, 34, 36, 38, 40, 42, 44,
                                           46, 48, 50, 52, 54, 56, 58, 60, 62, 64};

/// @brief Select the ASTC dequantization table for a stored weight width.
/// @param[in] bits Encoded weight width in the inclusive range 1-5.
/// @return Pointer to the immutable weight table, or NULL for an unsupported width.
static const uint8_t *uastc_weight_table(int bits) {
    switch (bits) {
    case 1:
        return uastc_weights1;
    case 2:
        return uastc_weights2;
    case 3:
        return uastc_weights3;
    case 4:
        return uastc_weights4;
    case 5:
        return uastc_weights5;
    default:
        return NULL;
    }
}

/// @brief UASTC/BC7 common 2-subset partition patterns (30 entries) and anchors.
static const uint8_t uastc_patterns2[30][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1},
    {1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0},
    {1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
    {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1},
    {1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0},
    {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0},
    {1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0},
    {1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0}};

static const uint8_t uastc_pattern2_anchors[30][2] = {
    {0, 2}, {0, 3}, {1, 0}, {0, 3}, {7, 0},  {0, 2}, {3, 0},  {7, 0}, {0, 11}, {2, 0},
    {0, 7}, {11, 0}, {3, 0}, {8, 0}, {0, 4},  {12, 0}, {1, 0}, {8, 0}, {0, 1},  {0, 2},
    {0, 4}, {8, 0}, {1, 0}, {0, 2}, {4, 0},  {0, 1}, {4, 0},  {1, 0}, {4, 0},  {1, 0}};

/// @brief UASTC/BC7 common 3-subset partition patterns (11 entries) and anchors.
static const uint8_t uastc_patterns3[11][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2},
    {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 2, 2, 2, 2},
    {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2},
    {1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2},
    {0, 2, 1, 1, 0, 2, 1, 1, 0, 2, 1, 1, 0, 2, 1, 1},
    {2, 0, 0, 0, 2, 0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1},
    {2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2},
    {1, 1, 1, 1, 0, 0, 0, 0, 2, 2, 2, 2, 1, 1, 1, 1},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2}};

static const uint8_t uastc_pattern3_anchors[11][3] = {{0, 8, 10}, {8, 0, 12}, {4, 0, 12},
                                                      {8, 0, 4},  {3, 0, 2},  {0, 1, 3},
                                                      {0, 2, 1},  {1, 9, 0},  {1, 2, 0},
                                                      {4, 0, 8},  {0, 6, 2}};

/// @brief Mode 7 patterns: 2-subset ASTC mapped onto 3-subset BC7 (19 entries).
static const uint8_t uastc_patterns2_3[19][16] = {
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0},
    {1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0},
    {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0},
    {1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0},
    {0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0},
    {1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}};

static const uint8_t uastc_pattern2_3_anchors[19][2] = {
    {0, 4}, {0, 2}, {2, 0}, {0, 7}, {8, 0}, {0, 1}, {0, 3}, {0, 1}, {2, 0}, {0, 1},
    {0, 8}, {2, 0}, {0, 1}, {0, 7}, {12, 0}, {2, 0}, {9, 0}, {0, 2}, {4, 0}};

/// @brief Read @p count bits from the 128-bit block starting at bit @p *cursor.
/// @details Bounds-guarded like the BC6H reader: blocks come from disk, so a
///   per-mode bit-budget table error must return zeros (cursor saturated past
///   the block) instead of reading past the 16-byte block.
/// @param[in] block Pointer to the complete 16-byte UASTC block.
/// @param[in,out] cursor Zero-based bit cursor, advanced or saturated to 129 on overflow.
/// @param[in] count Number of bits to consume.
/// @return The requested bits right-aligned, or zero when the read exceeds the block.
static uint32_t uastc_read_bits(const uint8_t *block, uint32_t *cursor, uint32_t count) {
    uint32_t value = 0;
    if (*cursor > 128u || count > 128u - *cursor) {
        *cursor = 129u; /* saturate: all further reads fail closed */
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bit_index = *cursor + i;
        uint32_t bit = (block[bit_index >> 3] >> (bit_index & 7u)) & 1u;
        value |= bit << i;
    }
    *cursor += count;
    return value;
}

/// @brief Replicate an n-bit value's MSBs to produce an 8-bit value (bit-only ranges).
/// @param[in] v Right-aligned quantized endpoint value.
/// @param[in] n Number of significant bits in @p v.
/// @return The value expanded to eight bits by repeated-bit replication.
static uint8_t uastc_replicate_bits(uint32_t v, int n) {
    uint32_t r;
    int shift;
    if (n >= 8)
        return (uint8_t)v;
    r = v << (8 - n);
    shift = n;
    while (shift < 8) {
        r |= r >> shift;
        shift += n;
    }
    return (uint8_t)r;
}

/// @brief ASTC color endpoint unquantization (Data Format spec §18.13, Table 93).
/// @details Forms `unq` from the trit/quint contribution and scattered direct bits, XORs it
///          with the replicated low bit, then folds the nine-bit result to the ASTC endpoint.
/// @param[in] bits_val Directly encoded endpoint bits.
/// @param[in] tq Decoded trit or quint value, or zero for a bit-only range.
/// @param[in] rd BISE range description governing the reconstruction.
/// @return The endpoint unquantized to eight bits, or zero for an unsupported descriptor.
static uint8_t uastc_unquant_endpoint(uint32_t bits_val, uint32_t tq, const uastc_range_desc *rd) {
    uint32_t A;
    uint32_t B = 0;
    uint32_t C = 0;
    uint32_t unq;
    uint32_t a = bits_val & 1u;
    uint32_t b = (bits_val >> 1) & 1u;
    uint32_t c = (bits_val >> 2) & 1u;
    uint32_t d = (bits_val >> 3) & 1u;
    uint32_t e = (bits_val >> 4) & 1u;
    uint32_t f = (bits_val >> 5) & 1u;
    if (!rd->trits && !rd->quints)
        return uastc_replicate_bits(bits_val, rd->bits);
    A = a ? 0x1FFu : 0u;
    if (rd->trits) {
        switch (rd->bits) {
        case 2:
            C = 93;
            B = b * 0x116u;
            break;
        case 4:
            C = 22;
            B = d * 0x104u + c * 0x82u + b * 0x41u;
            break;
        case 6:
            C = 5;
            B = f * 0x101u + e * 0x80u + d * 0x40u + c * 0x20u + b * 0x10u;
            break;
        default:
            return 0;
        }
    } else {
        switch (rd->bits) {
        case 3:
            C = 26;
            B = c * 0x105u + b * 0x82u;
            break;
        case 5:
            C = 6;
            B = e * 0x101u + d * 0x80u + c * 0x40u + b * 0x20u;
            break;
        default:
            return 0;
        }
    }
    unq = tq * C + B;
    unq ^= A;
    unq = (A & 0x80u) | (unq >> 2);
    return (uint8_t)unq;
}

/// @brief ASTC endpoint interpolation (weights in [0,64], srgb = false).
/// @param[in] l Low endpoint component.
/// @param[in] h High endpoint component.
/// @param[in] w Dequantized interpolation weight in the inclusive range 0-64.
/// @return The interpolated eight-bit component.
static uint8_t uastc_interpolate(uint32_t l, uint32_t h, uint32_t w) {
    uint32_t k;
    l = (l << 8) | l;
    h = (h << 8) | h;
    k = (l * (64u - w) + h * w + 32u) >> 6;
    return (uint8_t)(k >> 8);
}

/// @brief Decode one 128-bit UASTC block into a 4x4 RGBA texel array.
/// @param[in] block Pointer to a complete 16-byte UASTC block.
/// @param[out] out_rgba Destination buffer for 16 four-byte row-major RGBA texels.
/// @return 1 on success, 0 for reserved/invalid encodings.
static int rt_textureasset3d_decode_uastc_block(const uint8_t *block, uint8_t *out_rgba) {
    uint32_t cursor = 0;
    int mode = uastc_mode_lut[block[0] & 0x7Fu];
    int subsets;
    int planes;
    int comps;
    int weight_bits;
    int endpoint_range;
    uastc_range_desc rd;
    const uint8_t *pattern = NULL;
    const uint8_t *anchors = NULL;
    int compsel = 0;
    uint8_t endpoint_vals[18];
    int num_endpoint_vals;
    uint8_t weights[32];
    const uint8_t *weight_lut;
    uint8_t subset_e0[3][4];
    uint8_t subset_e1[3][4];

    if (mode == 19)
        return 0; /* reserved for future expansion */
    if (mode == 8) {
        /* Void-extent: solid RGBA color at bits 5/13/21/29. */
        uint32_t rgba_bits;
        uint8_t solid[4];
        cursor = 5;
        rgba_bits = uastc_read_bits(block, &cursor, 32);
        solid[0] = (uint8_t)(rgba_bits & 0xFFu);
        solid[1] = (uint8_t)((rgba_bits >> 8) & 0xFFu);
        solid[2] = (uint8_t)((rgba_bits >> 16) & 0xFFu);
        solid[3] = (uint8_t)((rgba_bits >> 24) & 0xFFu);
        for (int t = 0; t < 16; t++) {
            out_rgba[t * 4 + 0] = solid[0];
            out_rgba[t * 4 + 1] = solid[1];
            out_rgba[t * 4 + 2] = solid[2];
            out_rgba[t * 4 + 3] = solid[3];
        }
        return 1;
    }
    subsets = uastc_mode_subsets[mode];
    planes = uastc_mode_planes[mode];
    comps = uastc_mode_comps[mode];
    weight_bits = uastc_mode_weight_bits[mode];
    endpoint_range = uastc_mode_endpoint_range[mode];
    if (!uastc_range_lookup(endpoint_range, &rd))
        return 0;
    weight_lut = uastc_weight_table(weight_bits);
    if (!weight_lut)
        return 0;

    /* Skip the mode code and the transcoder hint fields (cursor-driven layout). */
    cursor = uastc_mode_code_len[mode];
    cursor += 1;                              /* BC1H0 */
    cursor += uastc_mode_has_bc1_hint1[mode]; /* BC1H1 */
    cursor += 1 + 1 + 3 + 3;                  /* ETC1F, ETC1D, ETC1I0, ETC1I1 */
    if (uastc_mode_has_etc1_bias[mode])
        cursor += 5; /* ETC1BIAS */
    if (uastc_mode_has_alpha[mode])
        cursor += 8; /* ETC2TM */

    if (planes == 2) {
        compsel = (int)uastc_read_bits(block, &cursor, 2);
    } else if (subsets > 1) {
        int pat_bits = (mode == 3) ? 4 : 5;
        uint32_t pat = uastc_read_bits(block, &cursor, (uint32_t)pat_bits);
        if (mode == 3) {
            if (pat >= 11)
                return 0;
            pattern = uastc_patterns3[pat];
            anchors = uastc_pattern3_anchors[pat];
        } else if (mode == 7) {
            if (pat >= 19)
                return 0;
            pattern = uastc_patterns2_3[pat];
            anchors = uastc_pattern2_3_anchors[pat];
        } else {
            if (pat >= 30)
                return 0;
            pattern = uastc_patterns2[pat];
            anchors = uastc_pattern2_anchors[pat];
        }
    }

    num_endpoint_vals = comps * 2 * subsets;

    /* ETQ: packed trits (groups of 5) or quints (groups of 3), LSB-order division. */
    {
        uint8_t tq_vals[18];
        memset(tq_vals, 0, sizeof(tq_vals));
        if (rd.trits) {
            static const uint8_t trit_bits[6] = {0, 2, 4, 5, 7, 8};
            static const uint16_t trit_limit[6] = {1, 3, 9, 27, 81, 243};
            int remaining = num_endpoint_vals;
            int base = 0;
            while (remaining > 0) {
                int group = remaining < 5 ? remaining : 5;
                uint32_t packed = uastc_read_bits(block, &cursor, trit_bits[group]);
                if (packed >= trit_limit[group])
                    return 0;
                for (int i = 0; i < group; i++) {
                    tq_vals[base + i] = (uint8_t)(packed % 3u);
                    packed /= 3u;
                }
                base += group;
                remaining -= group;
            }
        } else if (rd.quints) {
            static const uint8_t quint_bits[4] = {0, 3, 5, 7};
            static const uint16_t quint_limit[4] = {1, 5, 25, 125};
            int remaining = num_endpoint_vals;
            int base = 0;
            while (remaining > 0) {
                int group = remaining < 3 ? remaining : 3;
                uint32_t packed = uastc_read_bits(block, &cursor, quint_bits[group]);
                if (packed >= quint_limit[group])
                    return 0;
                for (int i = 0; i < group; i++) {
                    tq_vals[base + i] = (uint8_t)(packed % 5u);
                    packed /= 5u;
                }
                base += group;
                remaining -= group;
            }
        }
        for (int i = 0; i < num_endpoint_vals; i++) {
            uint32_t bits_val = uastc_read_bits(block, &cursor, rd.bits);
            endpoint_vals[i] = uastc_unquant_endpoint(bits_val, tq_vals[i], &rd);
        }
    }

    /* Weights: plain bits, one less bit for each subset's anchor texel (all planes). */
    for (int texel = 0; texel < 16; texel++) {
        int subset = pattern ? pattern[texel] : 0;
        int is_anchor;
        if (pattern) {
            is_anchor = (anchors[subset] == texel);
        } else {
            is_anchor = (texel == 0);
        }
        for (int p = 0; p < planes; p++) {
            uint32_t bits = (uint32_t)weight_bits - (is_anchor ? 1u : 0u);
            uint32_t w = uastc_read_bits(block, &cursor, bits);
            weights[texel * planes + p] = (uint8_t)w;
        }
    }
    if (cursor > 128)
        return 0;

    /* Compose per-subset endpoint pairs from the CEM layout (direct, no blue
     * contraction per the UASTC spec). LA modes swizzle to LLLA. */
    for (int s = 0; s < subsets; s++) {
        const uint8_t *v = endpoint_vals + s * comps * 2;
        if (comps == 2) {
            subset_e0[s][0] = subset_e0[s][1] = subset_e0[s][2] = v[0];
            subset_e1[s][0] = subset_e1[s][1] = subset_e1[s][2] = v[1];
            subset_e0[s][3] = v[2];
            subset_e1[s][3] = v[3];
        } else {
            subset_e0[s][0] = v[0];
            subset_e1[s][0] = v[1];
            subset_e0[s][1] = v[2];
            subset_e1[s][1] = v[3];
            subset_e0[s][2] = v[4];
            subset_e1[s][2] = v[5];
            if (comps == 4) {
                subset_e0[s][3] = v[6];
                subset_e1[s][3] = v[7];
            } else {
                subset_e0[s][3] = 255;
                subset_e1[s][3] = 255;
            }
        }
    }

    for (int texel = 0; texel < 16; texel++) {
        int subset = pattern ? pattern[texel] : 0;
        uint32_t w0 = weight_lut[weights[texel * planes]];
        uint32_t w1 = planes == 2 ? weight_lut[weights[texel * planes + 1]] : w0;
        for (int ch = 0; ch < 4; ch++) {
            uint32_t w = (planes == 2 && ch == compsel) ? w1 : w0;
            out_rgba[texel * 4 + ch] =
                uastc_interpolate(subset_e0[subset][ch], subset_e1[subset][ch], w);
        }
    }
    return 1;
}

/// @brief Block-decode adapter for UASTC LDR 4x4, matching the generic block-decode
///   function pointer used by textureasset3d_decode_compressed_mips.
/// @param[in] block Pointer to a complete 16-byte UASTC block.
/// @param[in] block_width Expected block footprint width, which must be four.
/// @param[in] block_height Expected block footprint height, which must be four.
/// @param[out] out_rgba Destination buffer for 16 four-byte row-major RGBA texels.
/// @return 1 when dimensions and block encoding are valid, otherwise 0.
static int textureasset3d_decode_uastc_block_adapter(const uint8_t *block,
                                                     int32_t block_width,
                                                     int32_t block_height,
                                                     uint8_t *out_rgba) {
    uint8_t texels[16 * 4];
    if (block_width != 4 || block_height != 4)
        return 0;
    if (!rt_textureasset3d_decode_uastc_block(block, texels))
        return 0;
    memcpy(out_rgba, texels, sizeof(texels));
    return 1;
}
