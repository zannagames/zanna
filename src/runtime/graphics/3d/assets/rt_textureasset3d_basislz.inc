//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_textureasset3d_basislz.inc  (#included from
// rt_textureasset3d.c; not a standalone translation unit)
// Purpose: KTX2 supercompression scheme 1 (BasisLZ / ETC1S) decoding — clean-room
//   implementation from the published ".basis File Format and ETC1S Texture Video
//   Specification" (Binomial, explicitly public/uncopyrighted) and the KTX 2.0
//   specification's BasisLZ global-data layout. Decodes every mip level directly to
//   RGBA8 Pixels; no external transcoder code is used.
// Key invariants:
//   - Every bit read is bounds-checked; malformed data reports a recoverable
//     CORRUPT/UNSUPPORTED error, never traps.
//   - Alpha slices (odd ETC1S slices) merge their green channel into A per the spec.
//===----------------------------------------------------------------------===//

/**
 * @file rt_textureasset3d_basislz.inc
 * @brief Implements bounded clean-room BasisLZ/ETC1S decoding for KTX2 textures.
 * @details This TextureAsset3D fragment reads canonical Huffman codebooks, endpoint and selector
 *          tables, predictive slice streams, and optional alpha slices; converts every mip
 *          directly to RGBA8 Pixels; and captures canonical uncompressed backing for later
 *          residency reconstruction without retaining decoder-specific state.
 */

#define BASISLZ_HUFF_MAX_CODE_SIZE 16
#define BASISLZ_HUFF_MAX_SYMS (1 << 14)
#define BASISLZ_MAX_SELECTOR_HISTORY 64u

/// @brief LSB-first bit reader over a bounded byte range.
typedef struct {
    /// Borrowed encoded byte range.
    const uint8_t *data;
    /// Number of readable bytes in @ref data.
    size_t size;
    /// Current byte offset.
    size_t byte_pos;
    /// Number of already consumed low bits in the current byte.
    uint32_t bit_pos; /* bits consumed within data[byte_pos] */
    /// Sticky flag raised after any out-of-bounds read.
    int failed;
} basislz_bit_reader;

/// @brief Initialize a bounded BasisLZ bit reader at the start of a byte range.
/// @param[out] br Reader state to initialize.
/// @param[in] data Borrowed encoded bytes.
/// @param size Number of readable bytes at @p data.
static void basislz_bits_init(basislz_bit_reader *br, const uint8_t *data, size_t size) {
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 0;
    br->failed = 0;
}

/// @brief Read @p count bits (LSB-first within each byte, LSB-first assembly).
/// @param[in,out] br Bounded bit reader advanced by consumed bits.
/// @param count Number of bits to read.
/// @return Assembled unsigned value, or zero after raising the reader's failure flag
///         when input ends.
static uint32_t basislz_get_bits(basislz_bit_reader *br, uint32_t count) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bit;
        if (br->byte_pos >= br->size) {
            br->failed = 1;
            return 0;
        }
        bit = (br->data[br->byte_pos] >> br->bit_pos) & 1u;
        br->bit_pos++;
        if (br->bit_pos == 8u) {
            br->bit_pos = 0;
            br->byte_pos++;
        }
        value |= bit << i;
    }
    return value;
}

/// @brief Canonical Huffman decoding table (RFC 1951 §3.2.2 code assignment).
typedef struct {
    /// Number of symbols assigned to each code length.
    int32_t counts[BASISLZ_HUFF_MAX_CODE_SIZE + 1];
    /// First canonical code for each length.
    int32_t base_code[BASISLZ_HUFF_MAX_CODE_SIZE + 1];
    /// First symbol-table index for each length.
    int32_t base_index[BASISLZ_HUFF_MAX_CODE_SIZE + 1];
    /// Owned symbols ordered by code length and symbol value.
    uint16_t *sorted_syms; /* symbols ordered by (code length, symbol) */
    /// Total number of nonzero-length codes.
    int32_t total_codes;
} basislz_huff_table;

/// @brief Release a Huffman table's owned symbol ordering and clear its state.
/// @param[in,out] table Table to dispose.
static void basislz_huff_free(basislz_huff_table *table) {
    free(table->sorted_syms);
    memset(table, 0, sizeof(*table));
}

/// @brief Build a canonical decoding table from per-symbol code lengths (0 = unused).
/// @param[out] table Destination table, reset before construction.
/// @param[in] lengths Per-symbol canonical code lengths.
/// @param num_syms Number of entries in @p lengths.
/// @return Non-zero when a nonempty, non-oversubscribed table is built; zero on an
///         invalid length or allocation failure.
static int basislz_huff_build(basislz_huff_table *table,
                              const uint8_t *lengths,
                              uint32_t num_syms) {
    uint32_t code = 0;
    int32_t index = 0;
    memset(table, 0, sizeof(*table));
    for (uint32_t s = 0; s < num_syms; s++) {
        if (lengths[s] > BASISLZ_HUFF_MAX_CODE_SIZE)
            return 0;
        if (lengths[s])
            table->counts[lengths[s]]++;
    }
    for (int len = 1; len <= BASISLZ_HUFF_MAX_CODE_SIZE; len++)
        table->total_codes += table->counts[len];
    if (table->total_codes == 0)
        return 0;
    table->sorted_syms = (uint16_t *)malloc((size_t)table->total_codes * sizeof(uint16_t));
    if (!table->sorted_syms)
        return 0;
    for (int len = 1; len <= BASISLZ_HUFF_MAX_CODE_SIZE; len++) {
        table->base_code[len] = (int32_t)code;
        table->base_index[len] = index;
        /* Kraft over-subscription check: canonical assignment must fit in the
         * len-bit code space. A malformed length table that over-subscribes
         * would otherwise decode ambiguous, attacker-influenced symbols. */
        if ((uint64_t)code + (uint64_t)table->counts[len] > ((uint64_t)1 << len)) {
            basislz_huff_free(table);
            return 0;
        }
        for (uint32_t s = 0; s < num_syms; s++) {
            if (lengths[s] == len)
                table->sorted_syms[index++] = (uint16_t)s;
        }
        code = (code + (uint32_t)table->counts[len]) << 1;
        index = table->base_index[len] + table->counts[len];
    }
    return 1;
}

/// @brief Decode one Huffman symbol (bit-by-bit canonical walk; -1 on failure).
/// @param[in,out] br Encoded bit reader advanced through the code.
/// @param[in] table Canonical decoding table.
/// @return Decoded nonnegative symbol, or -1 after raising failure for an invalid or
///         truncated code.
static int32_t basislz_huff_decode(basislz_bit_reader *br, const basislz_huff_table *table) {
    uint32_t code = 0;
    for (int len = 1; len <= BASISLZ_HUFF_MAX_CODE_SIZE; len++) {
        code = (code << 1) | basislz_get_bits(br, 1);
        if (br->failed)
            return -1;
        if (table->counts[len] > 0) {
            int32_t offset = (int32_t)code - table->base_code[len];
            if (offset >= 0 && offset < table->counts[len])
                return table->sorted_syms[table->base_index[len] + offset];
        }
    }
    br->failed = 1;
    return -1;
}

/// @brief Read one compressed Huffman table (spec section 6.0: RLE + Huffman coded
///        code lengths, Deflate-style, with the 21-entry reordered length table).
/// @param[in,out] br Encoded bit reader advanced past the table.
/// @param[out] out_table Destination canonical table, cleared before decoding.
/// @return Non-zero when both code-length and final symbol tables decode successfully;
///         zero on malformed input or allocation failure.
static int basislz_read_huff_table(basislz_bit_reader *br, basislz_huff_table *out_table) {
    static const uint8_t reorder[21] = {17, 18, 19, 20, 0,  8, 7,  9, 6,  10, 5,
                                        11, 4,  12, 3,  13, 2, 14, 1, 15, 16};
    uint32_t total_used_syms = basislz_get_bits(br, 14);
    uint32_t num_codelength_codes = basislz_get_bits(br, 5);
    uint8_t small_lengths[21];
    basislz_huff_table small_table;
    uint8_t *lengths = NULL;
    uint32_t decoded = 0;
    int prev_len = -1;
    memset(out_table, 0, sizeof(*out_table));
    if (br->failed || total_used_syms == 0 || total_used_syms > BASISLZ_HUFF_MAX_SYMS ||
        num_codelength_codes == 0 || num_codelength_codes > 21)
        return 0;
    memset(small_lengths, 0, sizeof(small_lengths));
    for (uint32_t i = 0; i < num_codelength_codes; i++)
        small_lengths[reorder[i]] = (uint8_t)basislz_get_bits(br, 3);
    if (br->failed || !basislz_huff_build(&small_table, small_lengths, 21))
        return 0;
    lengths = (uint8_t *)calloc(total_used_syms, 1u);
    if (!lengths) {
        basislz_huff_free(&small_table);
        return 0;
    }
    while (decoded < total_used_syms) {
        int32_t sym = basislz_huff_decode(br, &small_table);
        if (sym < 0)
            goto fail;
        if (sym <= 16) {
            lengths[decoded++] = (uint8_t)sym;
            prev_len = sym;
        } else if (sym == 17 || sym == 18) {
            uint32_t run =
                (sym == 17) ? basislz_get_bits(br, 3) + 3u : basislz_get_bits(br, 7) + 11u;
            if (br->failed || decoded + run > total_used_syms)
                goto fail;
            decoded += run; /* lengths are calloc'd zero */
            prev_len = 0;
        } else {
            uint32_t run =
                (sym == 19) ? basislz_get_bits(br, 2) + 3u : basislz_get_bits(br, 7) + 7u;
            if (br->failed || prev_len <= 0 || decoded + run > total_used_syms)
                goto fail;
            for (uint32_t r = 0; r < run; r++)
                lengths[decoded++] = (uint8_t)prev_len;
        }
    }
    if (!basislz_huff_build(out_table, lengths, total_used_syms))
        goto fail;
    basislz_huff_free(&small_table);
    free(lengths);
    return 1;
fail:
    basislz_huff_free(&small_table);
    free(lengths);
    return 0;
}

/// @brief One ETC1S endpoint codebook entry: 5-bit base color + intensity table index.
typedef struct {
    /// Five-bit red, green, and blue base components.
    uint8_t color5[3];
    /// Three-bit ETC1 intensity-table index.
    uint8_t inten5;
} basislz_endpoint;

/// @brief One ETC1S selector codebook entry: 4x4 2-bit selectors, row-major.
typedef struct {
    /// Selector indices addressed as `[row][column]`.
    uint8_t selectors[4][4]; /* [y][x] */
} basislz_selector;

/// @brief Decode the endpoint codebook (spec section 7.0).
/// @param[in] data Encoded endpoint-codebook bytes.
/// @param size Number of readable bytes at @p data.
/// @param num_endpoints Number of endpoint records to decode.
/// @param[out] endpoints Caller-allocated endpoint table.
/// @return Non-zero when every endpoint is decoded; zero on malformed Huffman data,
///         truncation, or allocation failure.
static int basislz_decode_endpoints(const uint8_t *data,
                                    size_t size,
                                    uint32_t num_endpoints,
                                    basislz_endpoint *endpoints) {
    basislz_bit_reader br;
    basislz_huff_table color5_model[3];
    basislz_huff_table inten_model;
    int grayscale;
    uint8_t prev_color5[3] = {16, 16, 16};
    uint32_t prev_inten = 0;
    int ok = 0;
    memset(color5_model, 0, sizeof(color5_model));
    memset(&inten_model, 0, sizeof(inten_model));
    basislz_bits_init(&br, data, size);
    if (!basislz_read_huff_table(&br, &color5_model[0]) ||
        !basislz_read_huff_table(&br, &color5_model[1]) ||
        !basislz_read_huff_table(&br, &color5_model[2]) ||
        !basislz_read_huff_table(&br, &inten_model))
        goto done;
    grayscale = (int)basislz_get_bits(&br, 1);
    if (br.failed)
        goto done;
    for (uint32_t i = 0; i < num_endpoints; i++) {
        int32_t inten_delta = basislz_huff_decode(&br, &inten_model);
        if (inten_delta < 0)
            goto done;
        endpoints[i].inten5 = (uint8_t)(((uint32_t)inten_delta + prev_inten) & 7u);
        prev_inten = endpoints[i].inten5;
        for (int c = 0; c < (grayscale ? 1 : 3); c++) {
            int32_t delta;
            if (prev_color5[c] <= 9)
                delta = basislz_huff_decode(&br, &color5_model[0]);
            else if (prev_color5[c] <= 21)
                delta = basislz_huff_decode(&br, &color5_model[1]);
            else
                delta = basislz_huff_decode(&br, &color5_model[2]);
            if (delta < 0)
                goto done;
            endpoints[i].color5[c] = (uint8_t)(((uint32_t)prev_color5[c] + (uint32_t)delta) & 31u);
            prev_color5[c] = endpoints[i].color5[c];
        }
        if (grayscale) {
            endpoints[i].color5[1] = endpoints[i].color5[0];
            endpoints[i].color5[2] = endpoints[i].color5[0];
        }
    }
    ok = 1;
done:
    basislz_huff_free(&color5_model[0]);
    basislz_huff_free(&color5_model[1]);
    basislz_huff_free(&color5_model[2]);
    basislz_huff_free(&inten_model);
    return ok;
}

/// @brief Decode the selector codebook (spec section 8.0: raw or byte-wise DPCM).
/// @param[in] data Encoded selector-codebook bytes.
/// @param size Number of readable bytes at @p data.
/// @param num_selectors Number of selector records to decode.
/// @param[out] selectors Caller-allocated selector table.
/// @return Non-zero when raw or DPCM selectors decode completely; zero for unsupported
///         global/hybrid codebooks, malformed data, or truncation.
static int basislz_decode_selectors(const uint8_t *data,
                                    size_t size,
                                    uint32_t num_selectors,
                                    basislz_selector *selectors) {
    basislz_bit_reader br;
    basislz_huff_table delta_model;
    uint8_t prev_bytes[4] = {0, 0, 0, 0};
    int ok = 0;
    memset(&delta_model, 0, sizeof(delta_model));
    basislz_bits_init(&br, data, size);
    if (basislz_get_bits(&br, 1) != 0 || basislz_get_bits(&br, 1) != 0 || br.failed)
        return 0; /* global / hybrid codebooks are never emitted */
    if (basislz_get_bits(&br, 1)) {
        /* Raw selectors: four bytes per entry, 4 selectors per byte from the LSB. */
        for (uint32_t i = 0; i < num_selectors; i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t cur = basislz_get_bits(&br, 8);
                if (br.failed)
                    return 0;
                for (int k = 0; k < 4; k++)
                    selectors[i].selectors[j][k] = (uint8_t)((cur >> (k * 2)) & 3u);
            }
        }
        return 1;
    }
    if (!basislz_read_huff_table(&br, &delta_model))
        return 0;
    for (uint32_t i = 0; i < num_selectors; i++) {
        for (int j = 0; j < 4; j++) {
            uint32_t cur;
            if (i == 0) {
                cur = basislz_get_bits(&br, 8);
            } else {
                int32_t delta = basislz_huff_decode(&br, &delta_model);
                if (delta < 0)
                    goto done;
                cur = (uint32_t)delta ^ prev_bytes[j];
            }
            if (br.failed)
                goto done;
            prev_bytes[j] = (uint8_t)cur;
            for (int k = 0; k < 4; k++)
                selectors[i].selectors[j][k] = (uint8_t)((cur >> (k * 2)) & 3u);
        }
    }
    ok = 1;
done:
    basislz_huff_free(&delta_model);
    return ok;
}

/// @brief The four slice-decoding Huffman tables + selector history size (section 9.0).
typedef struct {
    /// Endpoint prediction-symbol table.
    basislz_huff_table endpoint_pred_model;
    /// Delta endpoint-index table.
    basislz_huff_table delta_endpoint_model;
    /// Selector and history-reference symbol table.
    basislz_huff_table selector_model;
    /// Selector-history run-length table.
    basislz_huff_table selector_history_buf_rle_model;
    /// Approximate move-to-front selector history size.
    uint32_t selector_history_buf_size;
} basislz_slice_tables;

/// @brief Release every Huffman table owned by a slice-table set and reset its history size.
/// @param[in,out] tables Slice-table set to dispose.
static void basislz_slice_tables_free(basislz_slice_tables *tables) {
    basislz_huff_free(&tables->endpoint_pred_model);
    basislz_huff_free(&tables->delta_endpoint_model);
    basislz_huff_free(&tables->selector_model);
    basislz_huff_free(&tables->selector_history_buf_rle_model);
    tables->selector_history_buf_size = 0;
}

/// @brief Decode the four slice Huffman tables and selector-history size.
/// @param[in] data Encoded slice-table bytes.
/// @param size Number of readable bytes at @p data.
/// @param[out] tables Destination table set, cleared before decoding.
/// @return Non-zero on complete valid input; zero on malformed tables, truncation, or
///         an excessive selector-history size.
static int basislz_read_slice_tables(const uint8_t *data,
                                     size_t size,
                                     basislz_slice_tables *tables) {
    basislz_bit_reader br;
    memset(tables, 0, sizeof(*tables));
    basislz_bits_init(&br, data, size);
    if (!basislz_read_huff_table(&br, &tables->endpoint_pred_model) ||
        !basislz_read_huff_table(&br, &tables->delta_endpoint_model) ||
        !basislz_read_huff_table(&br, &tables->selector_model) ||
        !basislz_read_huff_table(&br, &tables->selector_history_buf_rle_model)) {
        basislz_slice_tables_free(tables);
        return 0;
    }
    tables->selector_history_buf_size = basislz_get_bits(&br, 13);
    if (br.failed || tables->selector_history_buf_size > BASISLZ_MAX_SELECTOR_HISTORY) {
        basislz_slice_tables_free(tables);
        return 0;
    }
    return 1;
}

/// @brief Variable-length chunked integer (spec section 10.2).
/// @param[in,out] br Encoded bit reader advanced through all continuation chunks.
/// @param chunk_bits Payload bits carried by each chunk.
/// @return Decoded 32-bit unsigned value, or zero after raising failure on truncation
///         or excessive continuation length.
static uint32_t basislz_decode_vlc(basislz_bit_reader *br, uint32_t chunk_bits) {
    const uint32_t chunk_size = 1u << chunk_bits;
    const uint32_t chunk_mask = chunk_size - 1u;
    uint32_t v = 0;
    uint32_t ofs = 0;
    for (;;) {
        uint32_t s = basislz_get_bits(br, chunk_bits + 1u);
        if (br->failed)
            return 0;
        v |= (s & chunk_mask) << ofs;
        ofs += chunk_bits;
        if ((s & chunk_size) == 0)
            break;
        if (ofs >= 32) {
            br->failed = 1;
            return 0;
        }
    }
    return v;
}

/// @brief ETC1 intensity modifier tables (low-to-high ramp order used by ETC1S).
static const int16_t basislz_inten_tables[8][4] = {{-8, -2, 2, 8},
                                                   {-17, -5, 5, 17},
                                                   {-29, -9, 9, 29},
                                                   {-42, -13, 13, 42},
                                                   {-60, -18, 18, 60},
                                                   {-80, -24, 24, 80},
                                                   {-106, -33, 33, 106},
                                                   {-183, -47, 47, 183}};

/// @brief Clamp an integer color component to the unsigned eight-bit range.
/// @param v Component value to clamp.
/// @return @p v limited to `[0, 255]`.
static uint8_t basislz_clamp255(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

/// @brief Expand an endpoint into the 4-color ramp (5-bit color scaled to 8-bit).
/// @param[in] endpoint ETC1S endpoint record.
/// @param[out] ramp Four RGB8 colors ordered by selector value.
static void basislz_endpoint_ramp(const basislz_endpoint *endpoint, uint8_t ramp[4][3]) {
    const int16_t *inten = basislz_inten_tables[endpoint->inten5 & 7];
    int base[3];
    for (int c = 0; c < 3; c++)
        base[c] = (endpoint->color5[c] << 3) | (endpoint->color5[c] >> 2);
    for (int s = 0; s < 4; s++)
        for (int c = 0; c < 3; c++)
            ramp[s][c] = basislz_clamp255(base[c] + inten[s]);
}

/// @brief Per-block decode output: codebook indices for one slice.
typedef struct {
    /// Endpoint codebook entry selected for the block.
    uint16_t endpoint_index;
    /// Selector codebook entry selected for the block.
    uint16_t selector_index;
} basislz_block_indices;

/// @brief Decode one ETC1S slice into per-block codebook indices (spec section 10.3).
/// @param[in] data Encoded slice bytes.
/// @param size Number of readable bytes at @p data.
/// @param num_blocks_x Horizontal ETC1 block count.
/// @param num_blocks_y Vertical ETC1 block count.
/// @param num_endpoints Size of the endpoint codebook.
/// @param num_selectors Size of the selector codebook.
/// @param[in] tables Decoded slice Huffman and history configuration.
/// @param[out] out_blocks Caller-allocated row-major block-index table.
/// @return Non-zero when all predictive endpoint and selector symbols decode within
///         their codebook bounds; zero on invalid configuration, allocation, or data.
static int basislz_decode_slice(const uint8_t *data,
                                size_t size,
                                uint32_t num_blocks_x,
                                uint32_t num_blocks_y,
                                uint32_t num_endpoints,
                                uint32_t num_selectors,
                                const basislz_slice_tables *tables,
                                basislz_block_indices *out_blocks) {
    enum {
        ENDPOINT_PRED_REPEAT_LAST_SYMBOL = 256,
        ENDPOINT_PRED_MIN_REPEAT_COUNT = 3,
        ENDPOINT_PRED_COUNT_VLC_BITS = 4,
        SELECTOR_HISTORY_BUF_RLE_COUNT_THRESH = 3,
        SELECTOR_HISTORY_BUF_RLE_COUNT_BITS = 6
    };

    basislz_bit_reader br;

    struct {
        uint16_t endpoint_index;
        uint8_t pred_bits;
    } *preds = NULL;

    uint16_t history[BASISLZ_MAX_SELECTOR_HISTORY];
    uint32_t history_size = tables->selector_history_buf_size;
    uint32_t history_rover = history_size / 2u;
    uint32_t cur_selector_rle_count = 0;
    uint32_t cur_pred_bits = 0;
    uint32_t prev_endpoint_pred_sym = 0;
    uint32_t endpoint_pred_repeat_count = 0;
    uint32_t prev_endpoint_index = 0;
    const uint32_t selector_rle_symbol = num_selectors + history_size;
    int ok = 0;
    if (num_blocks_x == 0 || num_blocks_y == 0 || num_endpoints == 0 || num_selectors == 0)
        return 0;
    if (!rt_alloc_count_ok(num_blocks_x, 2u * sizeof(*preds)))
        return 0;
    preds = calloc((size_t)num_blocks_x * 2u, sizeof(*preds));
    if (!preds)
        return 0;
    memset(history, 0, sizeof(history));
    basislz_bits_init(&br, data, size);
    for (uint32_t block_y = 0; block_y < num_blocks_y; block_y++) {
        const uint32_t row = block_y & 1u;
        for (uint32_t block_x = 0; block_x < num_blocks_x; block_x++) {
            uint32_t endpoint_index;
            uint32_t selector_index = 0;
            uint32_t pred;
            if ((block_x & 1u) == 0) {
                if ((block_y & 1u) == 0) {
                    if (endpoint_pred_repeat_count) {
                        endpoint_pred_repeat_count--;
                        cur_pred_bits = prev_endpoint_pred_sym;
                    } else {
                        int32_t sym = basislz_huff_decode(&br, &tables->endpoint_pred_model);
                        if (sym < 0)
                            goto done;
                        if (sym == ENDPOINT_PRED_REPEAT_LAST_SYMBOL) {
                            endpoint_pred_repeat_count =
                                basislz_decode_vlc(&br, ENDPOINT_PRED_COUNT_VLC_BITS) +
                                ENDPOINT_PRED_MIN_REPEAT_COUNT - 1u;
                            if (br.failed)
                                goto done;
                            cur_pred_bits = prev_endpoint_pred_sym;
                        } else {
                            cur_pred_bits = (uint32_t)sym;
                            prev_endpoint_pred_sym = cur_pred_bits;
                        }
                    }
                    preds[(row ^ 1u) * num_blocks_x + block_x].pred_bits =
                        (uint8_t)(cur_pred_bits >> 4);
                } else {
                    cur_pred_bits = preds[row * num_blocks_x + block_x].pred_bits;
                }
            }
            pred = cur_pred_bits & 3u;
            cur_pred_bits >>= 2;
            if (pred == 0) {
                endpoint_index = prev_endpoint_index;
            } else if (pred == 1) {
                endpoint_index = preds[(row ^ 1u) * num_blocks_x + block_x].endpoint_index;
            } else if (pred == 2) {
                /* Non-video: reuse the upper-left block's endpoint index. */
                endpoint_index =
                    block_x > 0 ? preds[(row ^ 1u) * num_blocks_x + block_x - 1u].endpoint_index
                                : prev_endpoint_index;
            } else {
                int32_t delta_sym = basislz_huff_decode(&br, &tables->delta_endpoint_model);
                if (delta_sym < 0)
                    goto done;
                endpoint_index = ((uint32_t)delta_sym + prev_endpoint_index) % num_endpoints;
            }
            if (endpoint_index >= num_endpoints)
                goto done;
            preds[row * num_blocks_x + block_x].endpoint_index = (uint16_t)endpoint_index;
            prev_endpoint_index = endpoint_index;

            /* Selector index (spec section 10.3, second block of code). */
            {
                uint32_t selector_sym;
                if (cur_selector_rle_count > 0) {
                    cur_selector_rle_count--;
                    selector_sym = num_selectors;
                } else {
                    int32_t sym = basislz_huff_decode(&br, &tables->selector_model);
                    if (sym < 0)
                        goto done;
                    selector_sym = (uint32_t)sym;
                    if (selector_sym == selector_rle_symbol) {
                        int32_t run_sym =
                            basislz_huff_decode(&br, &tables->selector_history_buf_rle_model);
                        if (run_sym < 0)
                            goto done;
                        if (run_sym == (1 << SELECTOR_HISTORY_BUF_RLE_COUNT_BITS) - 1)
                            cur_selector_rle_count =
                                basislz_decode_vlc(&br, 7) + SELECTOR_HISTORY_BUF_RLE_COUNT_THRESH;
                        else
                            cur_selector_rle_count =
                                (uint32_t)run_sym + SELECTOR_HISTORY_BUF_RLE_COUNT_THRESH;
                        if (br.failed || cur_selector_rle_count == 0)
                            goto done;
                        selector_sym = num_selectors;
                        cur_selector_rle_count--;
                    }
                }
                if (selector_sym >= num_selectors) {
                    /* Approximate-MTF history buffer reference. */
                    uint32_t history_index = selector_sym - num_selectors;
                    if (history_size == 0 || history_index >= history_size)
                        goto done;
                    selector_index = history[history_index];
                    if (history_index != 0) {
                        uint16_t x = history[history_index / 2u];
                        history[history_index / 2u] = history[history_index];
                        history[history_index] = x;
                    }
                } else {
                    selector_index = selector_sym;
                    if (history_size) {
                        history[history_rover++] = (uint16_t)selector_index;
                        if (history_rover == history_size)
                            history_rover = history_size / 2u;
                    }
                }
                if (selector_index >= num_selectors)
                    goto done;
            }
            out_blocks[block_y * num_blocks_x + block_x].endpoint_index = (uint16_t)endpoint_index;
            out_blocks[block_y * num_blocks_x + block_x].selector_index = (uint16_t)selector_index;
        }
    }
    ok = 1;
done:
    free(preds);
    return ok;
}

/// @brief KTX2 BasisLZ supercompression global data, parsed and validated.
typedef struct {
    /// Number of decoded endpoint records.
    uint32_t endpoint_count;
    /// Number of decoded selector records.
    uint32_t selector_count;
    /// Owned endpoint codebook.
    basislz_endpoint *endpoints;
    /// Owned selector codebook.
    basislz_selector *selectors;
    /// Owned slice decoding tables.
    basislz_slice_tables tables;
    /// Borrowed 20-byte-per-image descriptor sequence inside the global data.
    const uint8_t *image_descs; /* 20 bytes per image, level-major */
    /// Number of descriptors at @ref image_descs.
    uint32_t image_count;
} basislz_global_data;

/// @brief Release decoded BasisLZ global codebooks and clear the aggregate state.
/// @param[in,out] gd Global data object to dispose.
static void basislz_global_free(basislz_global_data *gd) {
    free(gd->endpoints);
    free(gd->selectors);
    basislz_slice_tables_free(&gd->tables);
    memset(gd, 0, sizeof(*gd));
}

/// @brief Parse the KTX2 scheme-1 supercompression global data segment.
/// @details Layout per the KTX 2.0 spec: a 20-byte header (endpointCount u16,
///          selectorCount u16, endpoints/selectors/tables/extended byte lengths u32),
///          then one 20-byte imageDesc per (level, layer, face), then the endpoint,
///          selector, tables, and extended byte streams in that order.
/// @param[in] sgd KTX2 supercompression global-data segment.
/// @param sgd_size Number of readable bytes at @p sgd.
/// @param image_count Expected number of 20-byte image descriptors.
/// @param[out] gd Destination parsed global data, cleared before use.
/// @return Non-zero when segment layout and all codebooks are valid; zero on truncation,
///         invalid counts, allocation, or codebook decode failure.
static int basislz_parse_global_data(const uint8_t *sgd,
                                     size_t sgd_size,
                                     uint32_t image_count,
                                     basislz_global_data *gd) {
    uint32_t endpoints_len;
    uint32_t selectors_len;
    uint32_t tables_len;
    uint32_t extended_len;
    uint64_t need;
    const uint8_t *cursor;
    memset(gd, 0, sizeof(*gd));
    if (sgd_size < 20u)
        return 0;
    gd->endpoint_count = (uint32_t)sgd[0] | ((uint32_t)sgd[1] << 8);
    gd->selector_count = (uint32_t)sgd[2] | ((uint32_t)sgd[3] << 8);
    endpoints_len = textureasset3d_read_u32le(sgd + 4);
    selectors_len = textureasset3d_read_u32le(sgd + 8);
    tables_len = textureasset3d_read_u32le(sgd + 12);
    extended_len = textureasset3d_read_u32le(sgd + 16);
    need = 20u + (uint64_t)image_count * 20u + (uint64_t)endpoints_len + (uint64_t)selectors_len +
           (uint64_t)tables_len + (uint64_t)extended_len;
    if (gd->endpoint_count == 0 || gd->selector_count == 0 || need > (uint64_t)sgd_size)
        return 0;
    gd->image_descs = sgd + 20u;
    gd->image_count = image_count;
    cursor = gd->image_descs + (size_t)image_count * 20u;
    gd->endpoints = (basislz_endpoint *)calloc(gd->endpoint_count, sizeof(basislz_endpoint));
    gd->selectors = (basislz_selector *)calloc(gd->selector_count, sizeof(basislz_selector));
    if (!gd->endpoints || !gd->selectors)
        goto fail;
    if (!basislz_decode_endpoints(cursor, endpoints_len, gd->endpoint_count, gd->endpoints))
        goto fail;
    cursor += endpoints_len;
    if (!basislz_decode_selectors(cursor, selectors_len, gd->selector_count, gd->selectors))
        goto fail;
    cursor += selectors_len;
    if (!basislz_read_slice_tables(cursor, tables_len, &gd->tables))
        goto fail;
    return 1;
fail:
    basislz_global_free(gd);
    return 0;
}

/// @brief Decode one ETC1S slice into an RGBA8 Pixels object (RGB path), or merge the
///        slice's green channel into the A channel of @p pixels (alpha path).
/// @param[in] gd Parsed global endpoint, selector, and slice tables.
/// @param[in] slice_data Encoded ETC1S slice bytes.
/// @param slice_size Number of readable bytes at @p slice_data.
/// @param width Logical mip width in texels.
/// @param height Logical mip height in texels.
/// @param merge_alpha Non-zero to replace destination alpha from decoded green;
///                    zero to write decoded RGB with opaque alpha.
/// @param[in,out] pixels Destination RGBA8 Pixels object.
/// @return Non-zero when all blocks decode and are written; zero on size overflow,
///         allocation failure, or malformed slice data.
static int basislz_decode_slice_to_pixels(const basislz_global_data *gd,
                                          const uint8_t *slice_data,
                                          size_t slice_size,
                                          uint32_t width,
                                          uint32_t height,
                                          int merge_alpha,
                                          void *pixels) {
    uint32_t num_blocks_x = (width + 3u) / 4u;
    uint32_t num_blocks_y = (height + 3u) / 4u;
    basislz_block_indices *blocks;
    int ok = 0;
    if ((uint64_t)num_blocks_x * num_blocks_y > (uint64_t)SIZE_MAX / sizeof(*blocks))
        return 0;
    blocks = (basislz_block_indices *)calloc((size_t)num_blocks_x * num_blocks_y, sizeof(*blocks));
    if (!blocks)
        return 0;
    if (!basislz_decode_slice(slice_data,
                              slice_size,
                              num_blocks_x,
                              num_blocks_y,
                              gd->endpoint_count,
                              gd->selector_count,
                              &gd->tables,
                              blocks))
        goto done;
    for (uint32_t by = 0; by < num_blocks_y; by++) {
        for (uint32_t bx = 0; bx < num_blocks_x; bx++) {
            const basislz_block_indices *bi = &blocks[by * num_blocks_x + bx];
            const basislz_selector *sel = &gd->selectors[bi->selector_index];
            uint8_t ramp[4][3];
            basislz_endpoint_ramp(&gd->endpoints[bi->endpoint_index], ramp);
            for (uint32_t py = 0; py < 4u; py++) {
                uint32_t y = by * 4u + py;
                if (y >= height)
                    break;
                for (uint32_t px = 0; px < 4u; px++) {
                    uint32_t x = bx * 4u + px;
                    const uint8_t *rgb;
                    if (x >= width)
                        break;
                    rgb = ramp[sel->selectors[py][px] & 3u];
                    if (merge_alpha) {
                        uint32_t rgba =
                            (uint32_t)rt_pixels_get_rgba(pixels, (int64_t)x, (int64_t)y);
                        rgba = (rgba & 0xFFFFFF00u) | rgb[1]; /* alpha = green channel */
                        rt_pixels_set_rgba(pixels, (int64_t)x, (int64_t)y, (int64_t)rgba);
                    } else {
                        uint32_t rgba = ((uint32_t)rgb[0] << 24) | ((uint32_t)rgb[1] << 16) |
                                        ((uint32_t)rgb[2] << 8) | 0xFFu;
                        rt_pixels_set_rgba(pixels, (int64_t)x, (int64_t)y, (int64_t)rgba);
                    }
                }
            }
        }
    }
    ok = 1;
done:
    free(blocks);
    return ok;
}

/// @brief Parse a KTX2 BasisLZ texture after generic header validation.
/// @param[in] data Complete KTX2 byte stream.
/// @param size Number of readable bytes at @p data.
/// @param[in] api_name Loader name used in recoverable diagnostics.
/// @param pixel_width Base-level width in texels.
/// @param pixel_height Base-level height in texels.
/// @param level_count Number of KTX2 mip levels.
/// @return New TextureAsset3D handle, or `NULL` after recording an error.
static void *textureasset3d_parse_ktx2_basislz(const uint8_t *data,
                                               size_t size,
                                               const char *api_name,
                                               uint32_t pixel_width,
                                               uint32_t pixel_height,
                                               uint32_t level_count);

/// @brief Decode all mip levels of a KTX2 BasisLZ (scheme 1, ETC1S) texture to Pixels.
/// @details @p mips carries each level's compressed byte range in the file; imageDesc i
///          describes level i's RGB and optional alpha slice, with slice offsets
///          relative to the start of the level's byte range.
/// @param[in] data Complete KTX2 byte stream.
/// @param size Number of readable bytes at @p data.
/// @param[in] mips Validated source range and dimension table.
/// @param mip_count Number of entries in @p mips and @p gd image descriptors.
/// @param[in] gd Parsed BasisLZ global data and codebooks.
/// @return Owned array of decoded Pixels handles, or `NULL` on malformed input,
///         oversize fallback allocation, or slice decode failure.
static void **textureasset3d_decode_basislz_mips(const uint8_t *data,
                                                 size_t size,
                                                 const textureasset3d_mip *mips,
                                                 int64_t mip_count,
                                                 const basislz_global_data *gd) {
    void **mip_pixels = (void **)calloc((size_t)mip_count, sizeof(void *));
    if (!mip_pixels)
        return NULL;
    for (int64_t i = 0; i < mip_count; i++) {
        const uint8_t *desc = gd->image_descs + (size_t)i * 20u;
        uint32_t image_flags = textureasset3d_read_u32le(desc);
        uint32_t rgb_offset = textureasset3d_read_u32le(desc + 4);
        uint32_t rgb_length = textureasset3d_read_u32le(desc + 8);
        uint32_t alpha_offset = textureasset3d_read_u32le(desc + 12);
        uint32_t alpha_length = textureasset3d_read_u32le(desc + 16);
        void *pixels;
        if (image_flags != 0) /* P-frame video images are not supported */
            goto fail;
        if (rgb_length == 0 || (uint64_t)rgb_offset + rgb_length > mips[i].length)
            goto fail;
        if (alpha_length > 0 && (uint64_t)alpha_offset + alpha_length > mips[i].length)
            goto fail;
        if (mips[i].offset > (uint64_t)size || mips[i].length > (uint64_t)size - mips[i].offset)
            goto fail;
        {
            /* A tiny BasisLZ slice can declare huge dimensions; reject them before
             * rt_pixels_new, which TRAPS (aborts) rather than returning NULL on an
             * oversize allocation. Every other KTX2 decode path enforces this cap
             * (see textureasset3d_rgba8_byte_count callers in core.inc/codecs.inc),
             * so malformed input reports recoverably instead of killing the process. */
            uint64_t needed = 0;
            if (!textureasset3d_rgba8_byte_count(mips[i].width, mips[i].height, &needed) ||
                needed > TEXTUREASSET3D_MAX_RGBA8_FALLBACK_BYTES)
                goto fail;
        }
        pixels = rt_pixels_new((int64_t)mips[i].width, (int64_t)mips[i].height);
        if (!pixels) {
            rt_trap("TextureAsset3D.LoadKTX2: BasisLZ Pixels allocation failed");
            goto fail;
        }
        mip_pixels[i] = pixels;
        if (!basislz_decode_slice_to_pixels(gd,
                                            data + mips[i].offset + rgb_offset,
                                            rgb_length,
                                            mips[i].width,
                                            mips[i].height,
                                            0,
                                            pixels))
            goto fail;
        if (alpha_length > 0 &&
            !basislz_decode_slice_to_pixels(gd,
                                            data + mips[i].offset + alpha_offset,
                                            alpha_length,
                                            mips[i].width,
                                            mips[i].height,
                                            1,
                                            pixels))
            goto fail;
    }
    return mip_pixels;
fail:
    textureasset3d_release_mip_pixels(mip_pixels, mip_count);
    return NULL;
}

/// @brief Parse a KTX2 texture using supercompression scheme 1 (BasisLZ / ETC1S).
/// @details Reads the supercompression global data segment (header bytes 64..79 carry
///          its offset/length), decodes the endpoint/selector codebooks and slice
///          tables, then decodes every level's ETC1S slices straight to RGBA8 Pixels.
///          The result is an uncompressed asset (format "rgba8", no native payloads).
/// @param[in] data Complete KTX2 byte stream.
/// @param size Number of readable bytes at @p data.
/// @param[in] api_name Loader name used in recoverable diagnostics.
/// @param pixel_width Base-level width in texels.
/// @param pixel_height Base-level height in texels.
/// @param level_count Number of KTX2 mip levels and BasisLZ image descriptors.
/// @return New TextureAsset3D handle, or `NULL` with a recoverable error recorded.
static void *textureasset3d_parse_ktx2_basislz(const uint8_t *data,
                                               size_t size,
                                               const char *api_name,
                                               uint32_t pixel_width,
                                               uint32_t pixel_height,
                                               uint32_t level_count) {
    uint64_t sgd_offset = textureasset3d_read_u64le(data + 64);
    uint64_t sgd_length = textureasset3d_read_u64le(data + 72);
    basislz_global_data gd;
    textureasset3d_mip *mips;
    void **mip_pixels;
    uint8_t *source_backing = NULL;
    uint8_t *fallback_kinds = NULL;
    uint64_t source_backing_bytes = 0;
    rt_textureasset3d *asset;
    int64_t mip_count = (int64_t)level_count;
    if (sgd_length < 20u || sgd_offset > (uint64_t)size ||
        sgd_length > (uint64_t)size - sgd_offset) {
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT, "%s: truncated BasisLZ global data", api_name);
        return NULL;
    }
    mips = (textureasset3d_mip *)calloc((size_t)mip_count, sizeof(textureasset3d_mip));
    if (!mips) {
        rt_trap("TextureAsset3D.LoadKTX2: mip table allocation failed");
        return NULL;
    }
    for (int64_t i = 0; i < mip_count; i++) {
        const uint8_t *entry = data + TEXTUREASSET3D_KTX2_HEADER_SIZE +
                               (uint64_t)i * TEXTUREASSET3D_KTX2_LEVEL_ENTRY_SIZE;
        mips[i].width = textureasset3d_mip_dimension(pixel_width, (uint32_t)i);
        mips[i].height = textureasset3d_mip_dimension(pixel_height, (uint32_t)i);
        mips[i].offset = textureasset3d_read_u64le(entry);
        mips[i].length = textureasset3d_read_u64le(entry + 8u);
        if (mips[i].length == 0 || mips[i].offset > (uint64_t)size ||
            mips[i].length > (uint64_t)size - mips[i].offset) {
            free(mips);
            rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT, "%s: truncated mip payload", api_name);
            return NULL;
        }
    }
    if (!basislz_parse_global_data(data + sgd_offset, (size_t)sgd_length, level_count, &gd)) {
        free(mips);
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT, "%s: invalid BasisLZ codebooks", api_name);
        return NULL;
    }
    mip_pixels = textureasset3d_decode_basislz_mips(data, size, mips, mip_count, &gd);
    basislz_global_free(&gd);
    if (!mip_pixels) {
        free(mips);
        rt_asset_error_setf(
            RT_ASSET_ERROR_CORRUPT, "%s: corrupt BasisLZ/ETC1S slice data", api_name);
        return NULL;
    }
    /* Capture plain RGBA8 backing once so residency can release/reconstruct decoded Pixels without
     * retaining the BasisLZ codebooks, source file, or a second decoder-specific object graph. */
    if (!textureasset3d_capture_rgba8_backing(
            mip_pixels, mips, mip_count, &source_backing, &source_backing_bytes)) {
        textureasset3d_release_mip_pixels(mip_pixels, mip_count);
        free(mips);
        rt_trap("TextureAsset3D.LoadKTX2: BasisLZ canonical backing allocation failed");
        return NULL;
    }
    fallback_kinds = (uint8_t *)calloc((size_t)mip_count, sizeof(uint8_t));
    if (!fallback_kinds) {
        free(source_backing);
        textureasset3d_release_mip_pixels(mip_pixels, mip_count);
        free(mips);
        rt_trap("TextureAsset3D.LoadKTX2: BasisLZ fallback-state allocation failed");
        return NULL;
    }
    for (int64_t i = 0; i < mip_count; i++)
        fallback_kinds[i] = TEXTUREASSET3D_FALLBACK_DECODE;
    asset = (rt_textureasset3d *)rt_obj_new_i64(RT_G3D_TEXTUREASSET3D_CLASS_ID,
                                                (int64_t)sizeof(rt_textureasset3d));
    if (!asset) {
        textureasset3d_release_mip_pixels(mip_pixels, mip_count);
        free(fallback_kinds);
        free(source_backing);
        free(mips);
        rt_trap("TextureAsset3D.LoadKTX2: allocation failed");
        return NULL;
    }
    memset(asset, 0, sizeof(*asset));
    rt_obj_set_finalizer(asset, textureasset3d_finalize);
    asset->mip_pixels = mip_pixels;
    asset->mip_payloads = NULL;
    asset->source_backing = source_backing;
    asset->source_backing_bytes = source_backing_bytes;
    asset->mip_fallback_kinds = fallback_kinds;
    asset->mips = mips;
    asset->width = (int64_t)pixel_width;
    asset->height = (int64_t)pixel_height;
    asset->mip_count = mip_count;
    asset->mip_capacity = mip_count;
    asset->format = "rgba8";
    asset->degraded_reason = "";
    asset->compressed = 0;
    asset->degraded = 0;
    asset->block_width = 1;
    asset->block_height = 1;
    asset->block_bytes = 4;
    asset->decoder_kind = TEXTUREASSET3D_DECODER_RGBA8;
    asset->native_format_id = RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE;
    asset->backend_capability_bit = RT_TEXTUREASSET3D_BACKEND_CAP_NONE;
    asset->cache_identity = textureasset3d_next_cache_identity();
    asset->native_revision = 1;
    asset->alpha_metadata_known =
        textureasset3d_compute_alpha_metadata(mip_pixels, mip_count, &asset->has_alpha_texels) ? 1
                                                                                               : 0;
    asset->retained_bytes = textureasset3d_compute_retained_bytes(asset);
    textureasset3d_set_resident_mip_range_internal(asset, 0, asset->mip_count, NULL);
    return asset;
}
