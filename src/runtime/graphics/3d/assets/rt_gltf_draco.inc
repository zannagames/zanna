//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_gltf_draco.inc  (#included from rt_gltf.c;
// not a standalone translation unit)
// Purpose: KHR_draco_mesh_compression decoding — clean-room implementation from the
//   published Draco Bitstream Specification (google.github.io/draco/spec). Covers the
//   sequential mesh encoding (compressed and uncompressed indices) and the edgebreaker
//   encoding (standard + valence traversal, topology splits, attribute seams,
//   depth-first and prediction-degree traversers) with the rANS entropy decoder,
//   difference/parallelogram/constrained-multi/tex-coords/geometric-normal prediction,
//   wrap and canonicalized-octahedron transforms, quantization and octahedral-normal
//   dequantization.
// Key invariants:
//   - Every read is bounds-checked and every traversal loop is iteration-capped;
//     malformed payloads fail the primitive load with a recoverable diagnostic,
//     never a trap or hang.
//   - Edgebreaker output is remapped to point-indexed attribute arrays before
//     returning, so consumers see the same identity layout as sequential meshes.
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_draco.inc
 * @brief Implements bounded clean-room decoding for `KHR_draco_mesh_compression`.
 * @details This fragment is included by `rt_gltf.c` and implements the published Draco
 *          bitstream algorithms needed by glTF: little-endian and varint reads, rANS/rABS
 *          entropy decoding, sequential and edgebreaker connectivity, seam-aware attribute
 *          traversal, prediction inversion, and final attribute dequantization. All input is
 *          treated as untrusted; counts, offsets, allocation sizes, recursion, and traversal
 *          progress are bounded so malformed payloads return failure instead of trapping or
 *          looping indefinitely.
 */

/// Maximum component count accepted for a decoded Draco attribute.
#define DRACO_MAX_COMPONENTS 4
/// Maximum attribute descriptors accepted in one Draco mesh payload.
#define DRACO_MAX_ATTRIBUTES 16

/* Sequential attribute encoder types */
/// Generic sequential attribute decoder identifier.
#define DRACO_SEQ_GENERIC 0
/// Integer sequential attribute decoder identifier.
#define DRACO_SEQ_INTEGER 1
/// Quantized sequential attribute decoder identifier.
#define DRACO_SEQ_QUANTIZATION 2
/// Octahedral-normal sequential attribute decoder identifier.
#define DRACO_SEQ_NORMALS 3

/* Prediction schemes / transforms */
#define DRACO_PREDICTION_NONE (-2)
#define DRACO_PREDICTION_DIFFERENCE 0
#define DRACO_PREDICTION_PARALLELOGRAM 1
#define DRACO_PREDICTION_CONSTRAINED_MULTI 4
#define DRACO_PREDICTION_TEX_COORDS_PORTABLE 5
#define DRACO_PREDICTION_GEOMETRIC_NORMAL 6
#define DRACO_TRANSFORM_WRAP 1
#define DRACO_TRANSFORM_NORMAL_OCT_CANON 3

/* Edgebreaker constants */
#define DRACO_EB_STANDARD 0
#define DRACO_EB_VALENCE 2
#define DRACO_TOPOLOGY_C 0
#define DRACO_TOPOLOGY_S 1
#define DRACO_TOPOLOGY_L 3
#define DRACO_TOPOLOGY_R 5
#define DRACO_TOPOLOGY_E 7
#define DRACO_EB_MAX_ATT_DATA 8
#define DRACO_EB_MAX_DECODERS 8
#define DRACO_EB_MAX_PARALLELOGRAMS 4
#define DRACO_EB_MIN_VALENCE 2
#define DRACO_EB_NUM_VALENCES 6

/// @brief Bounded little-endian byte reader for one Draco payload.
typedef struct {
    /// Borrowed payload bytes.
    const uint8_t *data;
    /// Total payload byte length.
    size_t size;
    /// Next unread byte offset.
    size_t pos;
    /// Sticky non-zero flag raised by any out-of-bounds read.
    int failed;
} draco_buf;

/* Draco payloads are entropy-coded, so element counts (faces/points/vertices)
 * have no fixed byte cost. But a count wildly out of proportion to the payload
 * is a corrupt or hostile header — a few-byte buffer cannot encode millions of
 * elements. Cap declared counts to a very generous multiple of the payload size,
 * far above any real compression ratio (edgebreaker connectivity floors near a
 * few bits/face even before attribute data), so a tiny buffer cannot drive a
 * multi-gigabyte allocation before decoding begins. Legitimate meshes stay well
 * under this bound; over-claims that slip past it fail cleanly on the truncated
 * symbol stream or the existing malloc NULL checks. */
#define DRACO_MAX_ELEMENTS_PER_INPUT_BYTE 4096u

/// @brief Reject an element count that is grossly disproportionate to the payload size.
/// @param count Declared element count from the payload.
/// @param payload_size Total compressed payload byte length.
/// @return Non-zero when the declaration is plausibly encodable by the payload.
static int draco_count_fits_payload(uint64_t count, size_t payload_size) {
    if (payload_size == 0u)
        return count == 0u;
    if ((uint64_t)payload_size > UINT64_MAX / DRACO_MAX_ELEMENTS_PER_INPUT_BYTE)
        return 1; /* product would overflow; downstream size overflow guards suffice */
    return count <= (uint64_t)payload_size * DRACO_MAX_ELEMENTS_PER_INPUT_BYTE;
}

/// @brief Compute a checked `size_t` product for an allocation or byte span.
/// @param lhs First factor.
/// @param rhs Second factor.
/// @param out_product Receives the product on success.
/// @return Non-zero when the multiplication is representable.
static int draco_size_mul(size_t lhs, size_t rhs, size_t *out_product) {
    if (!out_product || (rhs != 0u && lhs > SIZE_MAX / rhs))
        return 0;
    *out_product = lhs * rhs;
    return 1;
}

/// @brief Checked signed 64-bit addition used by integer mesh predictors.
static int draco_i64_add(int64_t lhs, int64_t rhs, int64_t *out_value) {
    if (!out_value || (rhs > 0 && lhs > INT64_MAX - rhs) || (rhs < 0 && lhs < INT64_MIN - rhs))
        return 0;
    *out_value = lhs + rhs;
    return 1;
}

/// @brief Checked signed 64-bit subtraction used by integer mesh predictors.
static int draco_i64_sub(int64_t lhs, int64_t rhs, int64_t *out_value) {
    if (!out_value || (rhs > 0 && lhs < INT64_MIN + rhs) || (rhs < 0 && lhs > INT64_MAX + rhs))
        return 0;
    *out_value = lhs - rhs;
    return 1;
}

/// @brief Checked signed 64-bit multiplication used by integer mesh predictors.
static int draco_i64_mul(int64_t lhs, int64_t rhs, int64_t *out_value) {
    if (!out_value)
        return 0;
    if (lhs > 0) {
        if ((rhs > 0 && lhs > INT64_MAX / rhs) || (rhs < 0 && rhs < INT64_MIN / lhs))
            return 0;
    } else if (lhs < 0) {
        if ((rhs > 0 && lhs < INT64_MIN / rhs) || (rhs < 0 && lhs < INT64_MAX / rhs))
            return 0;
    }
    *out_value = lhs * rhs;
    return 1;
}

/// @brief Add one checked product to an accumulator.
static int draco_i64_accumulate_product(int64_t *accumulator, int64_t lhs, int64_t rhs) {
    int64_t product;
    int64_t sum;
    if (!accumulator || !draco_i64_mul(lhs, rhs, &product) ||
        !draco_i64_add(*accumulator, product, &sum))
        return 0;
    *accumulator = sum;
    return 1;
}

/// @brief Checked unsigned 64-bit multiplication.
static int draco_u64_mul(uint64_t lhs, uint64_t rhs, uint64_t *out_value) {
    if (!out_value || (rhs != 0u && lhs > UINT64_MAX / rhs))
        return 0;
    *out_value = lhs * rhs;
    return 1;
}

/// @brief Checked absolute value that rejects the unrepresentable `INT64_MIN` case.
static int draco_i64_abs(int64_t value, int64_t *out_value) {
    if (!out_value || value == INT64_MIN)
        return 0;
    *out_value = value < 0 ? -value : value;
    return 1;
}

/// @brief Compute a scalar tuple count that remains representable by decoder APIs.
/// @param tuple_count Number of logical tuples.
/// @param components Scalar components in each tuple.
/// @param out_count Receives the scalar count.
/// @return Non-zero when the product fits both `uint32_t` and `size_t`.
static int draco_scalar_count(uint32_t tuple_count, uint32_t components, uint32_t *out_count) {
    uint64_t count;
    if (!out_count || components == 0u || components > DRACO_MAX_COMPONENTS)
        return 0;
    count = (uint64_t)tuple_count * components;
    if (count > UINT32_MAX || count > SIZE_MAX)
        return 0;
    *out_count = (uint32_t)count;
    return 1;
}

/// @brief Compute the bytes needed for a scalar tuple array.
/// @param tuple_count Number of logical tuples.
/// @param components Scalar components in each tuple.
/// @param element_size Bytes in one scalar.
/// @param out_bytes Receives the complete byte count.
/// @return Non-zero when both count and byte products are representable.
static int draco_tuple_bytes(uint32_t tuple_count,
                             uint32_t components,
                             size_t element_size,
                             size_t *out_bytes) {
    uint32_t scalar_count;
    return draco_scalar_count(tuple_count, components, &scalar_count) &&
           draco_size_mul((size_t)scalar_count, element_size, out_bytes);
}

/// @brief Read one byte from a bounded Draco buffer.
/// @param b Reader state advanced on success and marked failed on exhaustion.
/// @return Decoded byte, or zero after raising the sticky failure flag.
static uint8_t draco_read_u8(draco_buf *b) {
    if (!b || !b->data || b->pos >= b->size) {
        if (b)
            b->failed = 1;
        return 0;
    }
    return b->data[b->pos++];
}

/// @brief Read an unaligned little-endian 16-bit unsigned integer.
/// @param b Bounded reader state.
/// @return Decoded value, with @p b marked failed if either byte is unavailable.
static uint16_t draco_read_u16(draco_buf *b) {
    uint16_t lo = draco_read_u8(b);
    uint16_t hi = draco_read_u8(b);
    return (uint16_t)(lo | (hi << 8));
}

/// @brief Read an unaligned little-endian 32-bit unsigned integer.
/// @param b Bounded reader state.
/// @return Decoded value, with @p b marked failed if any byte is unavailable.
static uint32_t draco_read_u32(draco_buf *b) {
    uint32_t v = draco_read_u16(b);
    v |= (uint32_t)draco_read_u16(b) << 16;
    return v;
}

/// @brief Read a little-endian IEEE-754 float without aliasing or alignment assumptions.
/// @param b Bounded reader state.
/// @return Decoded float bit pattern.
static float draco_read_f32(draco_buf *b) {
    uint32_t bits = draco_read_u32(b);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/// @brief Unsigned LEB128 varint (capped at 64 bits).
/// @param b Bounded reader state.
/// @return Decoded value, or zero after marking @p b failed for truncation or overlong input.
static uint64_t draco_read_varint(draco_buf *b) {
    uint64_t result = 0;
    int shift = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t byte = draco_read_u8(b);
        if (!b || b->failed)
            return 0;
        /* A 64-bit LEB128 has only one payload bit in byte ten. Without this
         * check, hostile high bits are silently truncated by the unsigned shift. */
        if (i == 9 && (byte & 0x7Eu) != 0u) {
            b->failed = 1;
            return 0;
        }
        result |= (uint64_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0)
            return result;
        shift += 7;
    }
    b->failed = 1;
    return 0;
}

/// @brief Read a varint that must fit the decoder's 32-bit identifiers.
/// @param b Bounded reader state.
/// @param out_value Receives the narrowed value on success.
/// @return Non-zero when a complete value no larger than `UINT32_MAX` was read.
static int draco_read_varint_u32(draco_buf *b, uint32_t *out_value) {
    uint64_t value;
    if (!out_value)
        return 0;
    value = draco_read_varint(b);
    if (!b || b->failed || value > UINT32_MAX)
        return 0;
    *out_value = (uint32_t)value;
    return 1;
}

/// @brief Convert Draco's unsigned zigzag representation to a signed correction.
/// @param value Encoded unsigned symbol.
/// @return Exact signed value, including `-1` for encoded symbol one.
static int32_t draco_zigzag_decode_u32(uint32_t value) {
    if ((value & 1u) != 0u)
        return -(int32_t)(value >> 1) - 1;
    return (int32_t)(value >> 1);
}

/// @brief Advance a bounded reader without allowing offset addition to wrap.
/// @param b Reader state to advance.
/// @param count Number of bytes to skip.
/// @return Non-zero when all requested bytes remain available.
static int draco_skip_bytes(draco_buf *b, size_t count) {
    if (!b || b->failed || b->pos > b->size || count > b->size - b->pos) {
        if (b)
            b->failed = 1;
        return 0;
    }
    b->pos += count;
    return 1;
}

//===----------------------------------------------------------------------===//
// rANS entropy decoding (spec "Rans Decoding")
//===----------------------------------------------------------------------===//

/// @brief Mutable reverse-buffer state for one rANS entropy decoder.
typedef struct {
    /// Borrowed encoded byte stream.
    const uint8_t *buf;
    /// Number of unread bytes preceding the current state seed.
    size_t buf_offset;
    /// Current normalized rANS state.
    uint32_t state;
} draco_rans;

/// @brief Probability and cumulative interval assigned to one rANS symbol.
typedef struct {
    /// Symbol frequency in the configured precision range.
    uint32_t prob;
    /// Inclusive start of the symbol's cumulative interval.
    uint32_t cum_prob;
} draco_rans_sym;

/// @brief Initialize a rANS decoder from the tail of @p buf (spec RansInitDecoder).
/// @param ans Decoder state to initialize.
/// @param buf Borrowed entropy-coded byte stream.
/// @param size Number of bytes in @p buf.
/// @param l_base Normalization base added to the serialized state.
/// @return Non-zero when a complete initial state was available.
static int draco_rans_init(draco_rans *ans, const uint8_t *buf, size_t size, uint32_t l_base) {
    uint32_t x;
    if (!ans || !buf || size == 0)
        return 0;
    ans->buf = buf;
    x = buf[size - 1] >> 6;
    if (x == 0) {
        ans->buf_offset = size - 1;
        ans->state = buf[size - 1] & 0x3Fu;
    } else if (x == 1) {
        if (size < 2)
            return 0;
        ans->buf_offset = size - 2;
        ans->state = ((uint32_t)buf[size - 2] | ((uint32_t)buf[size - 1] << 8)) & 0x3FFFu;
    } else if (x == 2) {
        if (size < 3)
            return 0;
        ans->buf_offset = size - 3;
        ans->state = ((uint32_t)buf[size - 3] | ((uint32_t)buf[size - 2] << 8) |
                      ((uint32_t)buf[size - 1] << 16)) &
                     0x3FFFFFu;
    } else {
        if (size < 4)
            return 0;
        ans->buf_offset = size - 4;
        ans->state = ((uint32_t)buf[size - 4] | ((uint32_t)buf[size - 3] << 8) |
                      ((uint32_t)buf[size - 2] << 16) | ((uint32_t)buf[size - 1] << 24)) &
                     0x3FFFFFFFu;
    }
    if (ans->state > UINT32_MAX - l_base)
        return 0;
    ans->state += l_base;
    return 1;
}

/// @brief Read one symbol (spec RansRead) using the lookup/probability tables.
/// @param ans Initialized decoder state, updated in place.
/// @param l_base Lower normalization threshold.
/// @param precision Total probability-table precision.
/// @param lut Lookup table mapping cumulative remainder to symbol.
/// @param probs Per-symbol probability intervals.
/// @param out_val Receives the decoded symbol index.
/// @return Non-zero on success; zero when the state cannot be normalized.
static int draco_rans_read(draco_rans *ans,
                           uint32_t l_base,
                           uint32_t precision,
                           const uint32_t *lut,
                           const draco_rans_sym *probs,
                           uint32_t *out_val) {
    uint32_t quo;
    uint32_t rem;
    uint32_t symbol;
    uint32_t probability;
    uint32_t cumulative;
    if (!ans || !ans->buf || !lut || !probs || !out_val || precision == 0u || l_base == 0u)
        return 0;
    while (ans->state < l_base && ans->buf_offset > 0)
        ans->state = ans->state * 256u + ans->buf[--ans->buf_offset];
    if (ans->state < l_base)
        return 0;
    quo = ans->state / precision;
    rem = ans->state % precision;
    symbol = lut[rem];
    probability = probs[symbol].prob;
    cumulative = probs[symbol].cum_prob;
    if (probability == 0u || cumulative > rem || probability > precision - cumulative ||
        rem - cumulative >= probability)
        return 0;
    {
        uint64_t next_state = (uint64_t)quo * probability + rem - cumulative;
        if (next_state > UINT32_MAX)
            return 0;
        ans->state = (uint32_t)next_state;
    }
    *out_val = symbol;
    return 1;
}

/// @brief Binary rABS reader (spec RabsDescRead) for prediction/seam flag streams.
/// @param ans Initialized binary entropy state, updated in place.
/// @param prob_zero Encoded zero-symbol probability on a 256-unit scale.
/// @param out_val Receives the decoded binary symbol.
/// @return Non-zero after decoding one symbol.
static int draco_rabs_read(draco_rans *ans, uint32_t prob_zero, uint32_t *out_val) {
    uint32_t p = 256u - prob_zero;
    uint32_t x;
    uint32_t quot;
    uint32_t rem;
    uint32_t xn;
    uint32_t val;
    if (!ans || !ans->buf || !out_val || prob_zero > 255u)
        return 0;
    while (ans->state < 4096u && ans->buf_offset > 0)
        ans->state = ans->state * 256u + ans->buf[--ans->buf_offset];
    if (ans->state < 4096u)
        return 0;
    x = ans->state;
    quot = x / 256u;
    rem = x % 256u;
    xn = quot * p;
    val = rem < p;
    ans->state = val ? xn + rem : x - xn - p;
    *out_val = val;
    return 1;
}

/// @brief Parse the run-length coded probability table and build the rANS lookup
///        tables (spec BuildSymbolTables + rans_build_look_up_table).
/// @param b Bounded payload reader positioned at the probability table.
/// @param num_symbols Number of symbol probability entries.
/// @param precision Required cumulative probability total and lookup-table length.
/// @param out_lut Receives the heap-allocated cumulative lookup table.
/// @param out_probs Receives the heap-allocated per-symbol interval table.
/// @return 1 on success; tables are heap allocations owned by the caller.
static int draco_build_symbol_tables(draco_buf *b,
                                     uint32_t num_symbols,
                                     uint32_t precision,
                                     uint32_t **out_lut,
                                     draco_rans_sym **out_probs) {
    uint32_t *token_probs;
    uint32_t *lut = NULL;
    draco_rans_sym *probs = NULL;
    uint64_t cum_prob = 0;
    if (!out_lut || !out_probs)
        return 0;
    *out_lut = NULL;
    *out_probs = NULL;
    if (!b || precision == 0u || num_symbols == 0 || num_symbols > (1u << 18))
        return 0;
    token_probs = (uint32_t *)calloc(num_symbols, sizeof(uint32_t));
    if (!token_probs)
        return 0;
    for (uint32_t i = 0; i < num_symbols; i++) {
        uint8_t prob_data = draco_read_u8(b);
        uint32_t token = prob_data & 3u;
        if (b->failed)
            goto fail;
        if (token == 3u) {
            uint32_t offset = prob_data >> 2;
            if (i + offset >= num_symbols)
                goto fail;
            /* zero-probability run: token_probs entries stay zero */
            i += offset;
        } else {
            uint32_t prob = prob_data >> 2;
            for (uint32_t j = 0; j < token; j++) {
                uint8_t eb = draco_read_u8(b);
                if (b->failed)
                    goto fail;
                prob |= (uint32_t)eb << (8u * (j + 1u) - 2u);
            }
            token_probs[i] = prob;
        }
    }
    probs = (draco_rans_sym *)calloc(num_symbols, sizeof(draco_rans_sym));
    lut = (uint32_t *)calloc(precision, sizeof(uint32_t));
    if (!probs || !lut)
        goto fail;
    for (uint32_t i = 0; i < num_symbols; i++) {
        probs[i].prob = token_probs[i];
        probs[i].cum_prob = (uint32_t)cum_prob;
        cum_prob += token_probs[i];
        if (cum_prob > precision)
            goto fail;
        for (uint64_t j = probs[i].cum_prob; j < cum_prob; j++)
            lut[j] = i;
    }
    if (cum_prob != precision)
        goto fail;
    free(token_probs);
    *out_lut = lut;
    *out_probs = probs;
    return 1;
fail:
    free(token_probs);
    free(lut);
    free(probs);
    return 0;
}

/// @brief Decode @p num_values entropy-coded symbols (spec DecodeSymbols: both the
///        TAGGED and RAW schemes).
/// @param b Bounded payload reader positioned at the symbol stream.
/// @param num_values Number of scalar symbols to decode.
/// @param num_components Number of components grouped under each tagged bit length.
/// @param out_values Caller-allocated destination with @p num_values slots.
/// @return 1 on success; @p out_values (caller-allocated) receives the symbols.
static int draco_decode_symbols(draco_buf *b,
                                uint32_t num_values,
                                uint32_t num_components,
                                uint32_t *out_values) {
    uint8_t scheme = draco_read_u8(b);
    if (!b || b->failed || num_components == 0 || num_components > DRACO_MAX_COMPONENTS ||
        (num_values > 0 && !out_values))
        return 0;
    if (num_values == 0)
        return 1;
    if (scheme == 0) {
        /* TAGGED_SYMBOLS: rANS-coded bit lengths + raw bits per value. */
        uint32_t num_symbols;
        uint32_t *lut = NULL;
        draco_rans_sym *probs = NULL;
        uint64_t size;
        draco_rans ans;
        int ok = 0;
        if (!draco_read_varint_u32(b, &num_symbols) ||
            !draco_build_symbol_tables(
                b, num_symbols, 4096u /* TAGGED_RANS_PRECISION */, &lut, &probs))
            return 0;
        size = draco_read_varint(b);
        if (b->failed || size == 0 || size > b->size - b->pos)
            goto tagged_done;
        if (!draco_rans_init(&ans, b->data + b->pos, (size_t)size, 16384u /* TAGGED_RANS_BASE */))
            goto tagged_done;
        {
            /* Values are stored as contiguously packed raw bits (LSB-first) following
             * the tag stream; the reader realigns to a byte boundary once at the end.
             * (Verified against reference-encoder output; per-group realignment does
             * not match the emitted bitstream.) */
            size_t bit_byte = b->pos + (size_t)size;
            size_t bit_end = b->size;
            uint64_t bit_pos = 0;
            for (uint32_t i = 0; i < num_values;) {
                uint32_t bit_length;
                uint32_t group_count = num_values - i;
                if (group_count > num_components)
                    group_count = num_components;
                if (!draco_rans_read(&ans, 16384u, 4096u, lut, probs, &bit_length))
                    goto tagged_done;
                if (bit_length > 32u)
                    goto tagged_done;
                for (uint32_t c = 0; c < group_count; c++) {
                    uint32_t val = 0;
                    for (uint32_t k = 0; k < bit_length; k++) {
                        uint64_t idx = bit_pos + k;
                        uint64_t byte_offset = idx >> 3;
                        size_t byte;
                        if (byte_offset > SIZE_MAX || (size_t)byte_offset >= bit_end - bit_byte)
                            goto tagged_done;
                        byte = bit_byte + (size_t)byte_offset;
                        val |= (uint32_t)((b->data[byte] >> (idx & 7u)) & 1u) << k;
                    }
                    bit_pos += bit_length;
                    out_values[i + c] = val;
                }
                i += group_count;
            }
            {
                uint64_t byte_count = (bit_pos + 7u) >> 3;
                if (byte_count > SIZE_MAX || (size_t)byte_count > bit_end - bit_byte)
                    goto tagged_done;
                b->pos = bit_byte + (size_t)byte_count;
                ok = 1;
            }
        }
    tagged_done:
        free(lut);
        free(probs);
        return ok;
    }
    if (scheme == 1) {
        /* RAW_SYMBOLS: one rANS symbol per value with adaptive precision. */
        uint8_t max_bit_length = draco_read_u8(b);
        uint32_t num_symbols;
        uint32_t precision_bits = (3u * max_bit_length) / 2u;
        uint32_t precision;
        uint32_t l_base;
        uint32_t *lut = NULL;
        draco_rans_sym *probs = NULL;
        uint64_t size;
        draco_rans ans;
        int ok = 0;
        if (precision_bits > 20u)
            precision_bits = 20u;
        if (precision_bits < 12u)
            precision_bits = 12u;
        precision = 1u << precision_bits;
        l_base = precision * 4u;
        if (!draco_read_varint_u32(b, &num_symbols) ||
            !draco_build_symbol_tables(b, num_symbols, precision, &lut, &probs))
            return 0;
        size = draco_read_varint(b);
        if (b->failed || size == 0 || size > b->size - b->pos)
            goto raw_done;
        if (!draco_rans_init(&ans, b->data + b->pos, (size_t)size, l_base))
            goto raw_done;
        for (uint32_t i = 0; i < num_values; i++) {
            if (!draco_rans_read(&ans, l_base, precision, lut, probs, &out_values[i]))
                goto raw_done;
        }
        b->pos += (size_t)size;
        ok = 1;
    raw_done:
        free(lut);
        free(probs);
        return ok;
    }
    return 0;
}

//===----------------------------------------------------------------------===//
// Draco mesh state
//===----------------------------------------------------------------------===//

/// @brief One decoded attribute: identification plus decoded value storage.
typedef struct {
    /// Draco attribute semantic: position, normal, color, texture coordinate, or generic.
    uint8_t att_type; /* 0 pos, 1 normal, 2 color, 3 tex coord, 4 generic */
    /// Draco scalar data-type code.
    uint8_t data_type; /* draco DT_* */
    /// Number of scalar components per value.
    uint8_t components; /* 1..4 */
    /// Whether integer output carries normalized semantics.
    uint8_t normalized;
    /// Payload-unique attribute identifier referenced by the glTF extension.
    uint32_t unique_id;
    /// Sequential attribute decoder type.
    uint8_t decoder_type; /* DRACO_SEQ_* */
    /* portable (integer) decode state */
    /// Prediction scheme identifier parsed for portable integer values.
    int8_t prediction_scheme;
    /// Prediction-transform identifier applied during reconstruction.
    int8_t prediction_transform;
    /// Owned post-prediction portable integer tuples.
    int32_t *values; /* num_values * components, post-prediction integers */
    /* transform data */
    /// Inclusive lower bound used by the wrapping inverse transform.
    int32_t wrap_min;
    /// Inclusive upper bound used by the wrapping inverse transform.
    int32_t wrap_max;
    /// Maximum quantized coordinate for octahedral normals.
    int32_t normal_max_q;
    /// Number of quantization bits for scalar attributes.
    uint8_t quantization_bits;
    /// Per-component quantization origin.
    float quant_min[DRACO_MAX_COMPONENTS];
    /// Shared quantization range.
    float quant_range;
    /* mesh-prediction side data (edgebreaker schemes) */
    /// Per-context constrained-multi crease decisions.
    uint8_t *crease_bits[DRACO_EB_MAX_PARALLELOGRAMS]; /* constrained-multi, per context */
    /// Number of decisions in each crease context.
    uint32_t crease_count[DRACO_EB_MAX_PARALLELOGRAMS];
    /// Next crease decision consumed from each context.
    uint32_t crease_pos[DRACO_EB_MAX_PARALLELOGRAMS];
    /// Texture-orientation decisions consumed from the end.
    uint8_t *tex_orient; /* tex-coords orientations, consumed from the end */
    /// Current reverse-consumption position for @ref tex_orient.
    uint32_t tex_orient_pos;
    /// Geometric-normal flip decisions consumed in traversal order.
    uint8_t *flip_bits; /* geometric-normal flip flags, consumed in order */
    /// Number of available flip decisions.
    uint32_t flip_count;
    /* final output */
    /// Owned dequantized or decoded floating-point values.
    float *out_floats; /* dequantized / normal-decoded values, or NULL */
    /// Owned original integer values when the attribute remains integral.
    int32_t *out_ints; /* integer values, or NULL */
    /// Number of logical attribute tuples.
    uint32_t num_values;
} draco_attribute;

/// @brief Full decode state for one Draco payload.
typedef struct {
    /// Number of decoded triangular faces.
    uint32_t num_faces;
    /// Number of output points after connectivity reconstruction.
    uint32_t num_points;
    /// Owned interleaved triangle point indices.
    uint32_t *face_to_vertex; /* 3 * num_faces, interleaved v0 v1 v2 */
    /// Fixed-capacity decoded attribute descriptors.
    draco_attribute attributes[DRACO_MAX_ATTRIBUTES];
    /// Number of initialized entries in @ref attributes.
    uint32_t num_attributes;
} draco_mesh;

/// @brief Validate the fixed-width portion of a Draco attribute descriptor.
/// @param att Candidate descriptor populated from untrusted bytes.
/// @return Non-zero for a supported semantic, scalar type, component count, and flag.
static int draco_attribute_descriptor_valid(const draco_attribute *att) {
    return att && att->att_type <= 4u && att->data_type >= 1u && att->data_type <= 11u &&
           att->components >= 1u && att->components <= DRACO_MAX_COMPONENTS &&
           att->normalized <= 1u;
}

/// @brief Release every allocation owned by a decoded Draco mesh and clear its state.
/// @param mesh Mesh state to destroy.
static void draco_mesh_free(draco_mesh *mesh) {
    free(mesh->face_to_vertex);
    for (uint32_t i = 0; i < mesh->num_attributes && i < DRACO_MAX_ATTRIBUTES; i++) {
        free(mesh->attributes[i].values);
        free(mesh->attributes[i].out_floats);
        free(mesh->attributes[i].out_ints);
        for (int c = 0; c < DRACO_EB_MAX_PARALLELOGRAMS; c++)
            free(mesh->attributes[i].crease_bits[c]);
        free(mesh->attributes[i].tex_orient);
        free(mesh->attributes[i].flip_bits);
    }
    memset(mesh, 0, sizeof(*mesh));
}

/// @brief Decode sequential connectivity (spec DecodeSequentialConnectivityData).
/// @param b Bounded reader positioned at the sequential connectivity block.
/// @param mesh Destination mesh receiving face and point counts plus triangle indices.
/// @return Non-zero when all indices decode within the declared point range.
static int draco_decode_sequential_connectivity(draco_buf *b, draco_mesh *mesh) {
    uint64_t num_faces = draco_read_varint(b);
    uint64_t num_points = draco_read_varint(b);
    uint8_t method = draco_read_u8(b);
    if (!b || !mesh || b->failed || num_faces > (uint64_t)UINT32_MAX / 3u ||
        num_points > UINT32_MAX)
        return 0;
    if (!draco_count_fits_payload(num_faces, b->size) ||
        !draco_count_fits_payload(num_points, b->size))
        return 0;
    mesh->num_faces = (uint32_t)num_faces;
    mesh->num_points = (uint32_t)num_points;
    if (num_faces == 0 || num_points == 0)
        return 0;
    if ((uint64_t)mesh->num_faces * 3u > SIZE_MAX / sizeof(uint32_t))
        return 0;
    mesh->face_to_vertex = (uint32_t *)malloc((size_t)mesh->num_faces * 3u * sizeof(uint32_t));
    if (!mesh->face_to_vertex)
        return 0;
    if (method == 0) {
        /* SEQUENTIAL_COMPRESSED_INDICES: zigzag deltas over entropy-coded symbols. */
        uint32_t *symbols = (uint32_t *)malloc((size_t)mesh->num_faces * 3u * sizeof(uint32_t));
        int64_t last = 0;
        if (!symbols)
            return 0;
        if (!draco_decode_symbols(b, mesh->num_faces * 3u, 1, symbols)) {
            free(symbols);
            return 0;
        }
        for (uint32_t i = 0; i < mesh->num_faces * 3u; i++) {
            uint32_t encoded = symbols[i];
            int64_t diff = draco_zigzag_decode_u32(encoded);
            last += diff;
            if (last < 0 || last >= (int64_t)mesh->num_points) {
                free(symbols);
                return 0;
            }
            mesh->face_to_vertex[i] = (uint32_t)last;
        }
        free(symbols);
        return 1;
    }
    if (method == 1) {
        /* SEQUENTIAL_UNCOMPRESSED_INDICES: width depends on the point count. */
        for (uint32_t i = 0; i < mesh->num_faces * 3u; i++) {
            uint32_t v;
            if (mesh->num_points < 256u)
                v = draco_read_u8(b);
            else if (mesh->num_points < (1u << 16))
                v = draco_read_u16(b);
            else if (mesh->num_points < (1u << 21)) {
                if (!draco_read_varint_u32(b, &v))
                    return 0;
            } else
                v = draco_read_u32(b);
            if (b->failed || v >= mesh->num_points)
                return 0;
            mesh->face_to_vertex[i] = v;
        }
        return 1;
    }
    return 0;
}

/// @brief Skip one metadata element (spec ParseMetadataElement, recursive layout).
/// @param b Bounded reader positioned at an encoded metadata element.
/// @param depth Current recursion depth, bounded to prevent hostile nesting.
/// @return Non-zero when the element and all children were skipped safely.
static int draco_skip_metadata_element(draco_buf *b, int depth) {
    uint64_t num_entries = draco_read_varint(b);
    uint64_t num_sub;
    if (b->failed || depth > 8 || num_entries > 100000u)
        return 0;
    for (uint64_t i = 0; i < num_entries; i++) {
        uint8_t key_size = draco_read_u8(b);
        if (b->failed || !draco_skip_bytes(b, key_size))
            return 0;
        {
            uint8_t value_size = draco_read_u8(b);
            if (b->failed || !draco_skip_bytes(b, value_size))
                return 0;
        }
    }
    num_sub = draco_read_varint(b);
    if (b->failed || num_sub > 10000u)
        return 0;
    for (uint64_t i = 0; i < num_sub; i++) {
        uint8_t key_size = draco_read_u8(b);
        if (b->failed || !draco_skip_bytes(b, key_size) ||
            !draco_skip_metadata_element(b, depth + 1))
            return 0;
    }
    return 1;
}

/// @brief Skip the optional metadata section (spec DecodeMetadata).
/// @param b Bounded reader positioned at the metadata section.
/// @return Non-zero when all attribute and file metadata entries were skipped safely.
static int draco_skip_metadata(draco_buf *b) {
    uint64_t num_att_metadata = draco_read_varint(b);
    if (b->failed || num_att_metadata > 10000u)
        return 0;
    for (uint64_t i = 0; i < num_att_metadata; i++) {
        (void)draco_read_varint(b); /* attribute metadata id */
        if (b->failed || !draco_skip_metadata_element(b, 0))
            return 0;
    }
    return draco_skip_metadata_element(b, 0);
}

/// @brief Wrap-transform inverse (spec PredictionSchemeWrapDecodingTransform).
/// @note Predictions arrive as int64 because parallelogram sums of extreme
///       quantized values can exceed the int32 range before clamping.
/// @param att Attribute supplying the inclusive wrapping interval.
/// @param pred Predicted component values.
/// @param corr Encoded correction components.
/// @param out Destination reconstructed components.
/// @param components Number of components in each tuple.
static void draco_wrap_compute_original(const draco_attribute *att,
                                        const int64_t *pred,
                                        const int32_t *corr,
                                        int32_t *out,
                                        int components) {
    int64_t modulus = 1 + (int64_t)att->wrap_max - att->wrap_min;
    for (int c = 0; c < components; c++) {
        int64_t clamped = pred[c];
        int64_t value;
        int64_t relative;
        if (clamped > att->wrap_max)
            clamped = att->wrap_max;
        else if (clamped < att->wrap_min)
            clamped = att->wrap_min;
        value = clamped + corr[c];
        relative = (value - att->wrap_min) % modulus;
        if (relative < 0)
            relative += modulus;
        out[c] = (int32_t)((int64_t)att->wrap_min + relative);
    }
}

/* --- Canonicalized octahedron normal transform (spec Prediction Normal Transform) --- */

static int draco_msb(uint32_t n);

/// @brief Validate and derive canonical octahedral-normal quantization parameters.
/// @param source_max Serialized maximum quantized value.
/// @param out_max_q Receives the next all-ones modulus.
/// @param out_max_value Receives the largest canonical coordinate.
/// @param out_center Receives the canonical diamond center.
/// @return Non-zero when every derived signed value is representable and non-degenerate.
static int draco_normal_quant_params(int32_t source_max,
                                     int32_t *out_max_q,
                                     int32_t *out_max_value,
                                     int32_t *out_center) {
    uint32_t max_q;
    int bits;
    if (!out_max_q || !out_max_value || !out_center || source_max <= 1 ||
        (uint32_t)source_max > ((1u << 30) - 1u))
        return 0;
    bits = draco_msb((uint32_t)source_max) + 1;
    if (bits <= 0 || bits > 30)
        return 0;
    max_q = (1u << bits) - 1u;
    *out_max_q = (int32_t)max_q;
    *out_max_value = (int32_t)max_q - 1;
    *out_center = *out_max_value / 2;
    return *out_max_value > 0;
}

/// @brief Wrap an octahedral coordinate around the quantized interval centered at @p center.
/// @param x Coordinate to wrap.
/// @param center Half-width of the canonical interval.
/// @param max_q Full quantized modulus.
/// @return Coordinate mapped into the canonical interval.
static int64_t draco_mod_max(int64_t x, int32_t center, int32_t max_q) {
    int64_t shifted = (x % max_q) + center;
    shifted %= max_q;
    if (shifted < 0)
        shifted += max_q;
    return shifted - center;
}

/// @brief Reflect a signed octahedral point across the boundary of its canonical diamond.
/// @param s In/out first signed coordinate.
/// @param t In/out second signed coordinate.
/// @param center Diamond extent along either axis.
static void draco_invert_diamond(int64_t *s, int64_t *t, int32_t center) {
    int64_t sign_s = 0;
    int64_t sign_t = 0;
    int64_t corner_s;
    int64_t corner_t;
    int64_t ss = *s;
    int64_t tt = *t;
    if (ss >= 0 && tt >= 0) {
        sign_s = 1;
        sign_t = 1;
    } else if (ss <= 0 && tt <= 0) {
        sign_s = -1;
        sign_t = -1;
    } else {
        sign_s = ss > 0 ? 1 : -1;
        sign_t = tt > 0 ? 1 : -1;
    }
    corner_s = sign_s * center;
    corner_t = sign_t * center;
    ss = 2 * ss - corner_s;
    tt = 2 * tt - corner_t;
    if (sign_s * sign_t >= 0) {
        int64_t temp = ss;
        ss = -tt;
        tt = -temp;
    } else {
        int64_t temp = ss;
        ss = tt;
        tt = temp;
    }
    ss = (ss + corner_s) / 2;
    tt = (tt + corner_t) / 2;
    *s = ss;
    *t = tt;
}

/// @brief Determine the quarter-turn that moves an octahedral point into the bottom-left sector.
/// @param pred Two-dimensional signed prediction coordinate.
/// @return Clockwise rotation count from zero through three.
static int draco_rotation_count(const int64_t *pred) {
    int64_t sign_x = pred[0];
    int64_t sign_y = pred[1];
    if (sign_x == 0)
        return sign_y == 0 ? 0 : (sign_y > 0 ? 3 : 1);
    if (sign_x > 0)
        return sign_y >= 0 ? 2 : 1;
    return sign_y <= 0 ? 0 : 3;
}

/// @brief Rotate a two-dimensional integer point by a multiple of 90 degrees.
/// @param p Source coordinate pair.
/// @param rotation Quarter-turn count modulo four.
/// @param out Destination coordinate pair.
static void draco_rotate_point(const int64_t *p, int rotation, int64_t *out) {
    switch (rotation) {
        case 1:
            out[0] = p[1];
            out[1] = -p[0];
            return;
        case 2:
            out[0] = -p[0];
            out[1] = -p[1];
            return;
        case 3:
            out[0] = -p[1];
            out[1] = p[0];
            return;
        default:
            out[0] = p[0];
            out[1] = p[1];
            return;
    }
}

/// @brief Test whether an octahedral coordinate lies in the canonical bottom-left sector.
/// @param p Signed two-dimensional coordinate.
/// @return Non-zero for the origin or a point with negative X and non-positive Y.
static int draco_is_in_bottom_left(const int64_t *p) {
    if (p[0] == 0 && p[1] == 0)
        return 1;
    return p[0] < 0 && p[1] <= 0;
}

/// @brief Return the index of the most-significant set bit in an unsigned integer.
/// @param n Value to inspect.
/// @return Zero-based bit index, or -1 when @p n is zero.
static int draco_msb(uint32_t n) {
    int msb = -1;
    while (n != 0) {
        msb++;
        n >>= 1;
    }
    return msb;
}

/// @brief Octahedron-canonicalized transform inverse (spec ComputeOriginalValue/2).
/// @param att Attribute supplying the normal quantization maximum.
/// @param pred_vals Predicted two-component octahedral coordinate.
/// @param corr_vals Encoded correction pair.
/// @param out_vals Destination reconstructed coordinate pair.
static void draco_normal_oct_compute_original(const draco_attribute *att,
                                              const int64_t *pred_vals,
                                              const int32_t *corr_vals,
                                              int32_t *out_vals) {
    int32_t max_q;
    int32_t max_value;
    int32_t center;
    int64_t pred[2];
    int64_t orig[2];
    int pred_in_diamond;
    int pred_in_bottom_left;
    int rotation;
    if (!draco_normal_quant_params(att->normal_max_q, &max_q, &max_value, &center)) {
        out_vals[0] = 0;
        out_vals[1] = 0;
        return;
    }
    pred[0] = pred_vals[0] % max_q;
    pred[1] = pred_vals[1] % max_q;
    if (pred[0] < 0)
        pred[0] += max_q;
    if (pred[1] < 0)
        pred[1] += max_q;
    pred[0] -= center;
    pred[1] -= center;
    pred_in_diamond =
        (pred[0] < 0 ? -pred[0] : pred[0]) + (pred[1] < 0 ? -pred[1] : pred[1]) <= center;
    if (!pred_in_diamond)
        draco_invert_diamond(&pred[0], &pred[1], center);
    pred_in_bottom_left = draco_is_in_bottom_left(pred);
    rotation = draco_rotation_count(pred);
    if (!pred_in_bottom_left) {
        int64_t rotated[2];
        draco_rotate_point(pred, rotation, rotated);
        pred[0] = rotated[0];
        pred[1] = rotated[1];
    }
    orig[0] = draco_mod_max(pred[0] + corr_vals[0], center, max_q);
    orig[1] = draco_mod_max(pred[1] + corr_vals[1], center, max_q);
    if (!pred_in_bottom_left) {
        int reverse = (4 - rotation) % 4;
        int64_t rotated[2];
        draco_rotate_point(orig, reverse, rotated);
        orig[0] = rotated[0];
        orig[1] = rotated[1];
    }
    if (!pred_in_diamond)
        draco_invert_diamond(&orig[0], &orig[1], center);
    out_vals[0] = (int32_t)(orig[0] + center);
    out_vals[1] = (int32_t)(orig[1] + center);
}

/// @brief Apply the parsed prediction transform to one value tuple.
/// @param att Attribute selecting the inverse transform and its parameters.
/// @param pred Predicted component tuple.
/// @param corr Encoded correction tuple.
/// @param out Destination reconstructed tuple.
/// @param components Number of components in the tuple.
static void draco_transform_compute_original(const draco_attribute *att,
                                             const int64_t *pred,
                                             const int32_t *corr,
                                             int32_t *out,
                                             int components) {
    if (att->prediction_transform == DRACO_TRANSFORM_NORMAL_OCT_CANON) {
        draco_normal_oct_compute_original(att, pred, corr, out);
    } else {
        draco_wrap_compute_original(att, pred, corr, out, components);
    }
}

/// @brief Difference prediction inverse (spec PredictionSchemeDifference).
/// @param att Attribute whose portable values are reconstructed in place.
/// @param num_values Number of logical tuples in `att->values`.
/// @param components Portable component count (2 for octahedral normals).
/// @return Non-zero when the complete tuple span is valid and reconstructed.
static int draco_difference_compute_original(draco_attribute *att,
                                             uint32_t num_values,
                                             int components) {
    int64_t pred[DRACO_MAX_COMPONENTS] = {0, 0, 0, 0};
    uint32_t scalar_count;
    if (!att || !att->values || components <= 0 || components > DRACO_MAX_COMPONENTS ||
        !draco_scalar_count(num_values, (uint32_t)components, &scalar_count))
        return 0;
    if (num_values == 0)
        return 1;
    draco_transform_compute_original(att, pred, att->values, att->values, components);
    for (uint32_t i = 1; i < num_values; i++) {
        size_t previous_offset = (size_t)(i - 1u) * (uint32_t)components;
        size_t output_offset = (size_t)i * (uint32_t)components;
        if (previous_offset + (uint32_t)components > scalar_count ||
            output_offset + (uint32_t)components > scalar_count)
            return 0;
        for (int c = 0; c < components; c++)
            pred[c] = att->values[previous_offset + (uint32_t)c];
        draco_transform_compute_original(
            att, pred, att->values + output_offset, att->values + output_offset, components);
    }
    return 1;
}

//===----------------------------------------------------------------------===//
// Edgebreaker decoding (spec "EdgeBreaker Decoder")
//===----------------------------------------------------------------------===//

/// @brief Per-attribute-data seam and recomputed-vertex state (one entry per
///        non-position attribute connectivity stream).
typedef struct {
    /// Zero-symbol probability for the seam rABS stream.
    uint8_t prob_zero;
    /// Borrowed encoded seam-decision stream.
    const uint8_t *buf; /* seam rABS stream (borrowed from the payload) */
    /// Byte length of @ref buf.
    size_t size;
    /// Per-corner flag marking a discontinuous attribute edge.
    uint8_t *edge_on_seam; /* [corner_cap] */
    /// Per-position-vertex flag indicating contact with any seam.
    uint8_t *vertex_on_seam; /* [num_vertices] position vertex touches a seam edge */
    /// Recomputed attribute vertex for each corner.
    int32_t *corner_to_vertex; /* [corner_cap] recomputed attribute vertices */
    /// Attribute-specific triangle vertex table.
    int32_t *face_to_vertex; /* [corner_cap] attribute face->vertex table */
    /// Representative corner for each recomputed attribute vertex.
    int32_t *leftmost_corner; /* [corner_cap] per recomputed attribute vertex */
    /// Number of recomputed attribute vertices.
    uint32_t vertex_count;
} draco_eb_data;

/// @brief Per-attributes-decoder traversal output.
typedef struct {
    /// Attribute-connectivity data index, or `0xFF` for the position decoder.
    uint8_t data_id; /* 0xFF = none (position decoder) */
    /// Vertex-mapped or corner-mapped decoder mode.
    uint8_t decoder_type; /* 0 vertex-mapped, 1 corner-mapped */
    /// Depth-first or prediction-degree traversal selector.
    uint8_t traversal; /* 0 depth-first, 1 prediction degree */
    /// First mesh attribute owned by this decoder.
    uint32_t att_start; /* first index into draco_mesh::attributes */
    /// Number of consecutive mesh attributes owned by this decoder.
    uint32_t att_count;
    /// Encoded value index to representative corner mapping.
    int32_t *value_to_corner; /* [corner_cap] encoded value index -> corner */
    /// Number of emitted attribute values.
    uint32_t value_count;
    /// Traversal vertex to encoded value index, or -1 if unvisited.
    int32_t *vertex_to_value; /* [visit_cap] vertex -> encoded value index or -1 */
    /// Final point to encoded value index mapping.
    int32_t *indices_map; /* [point_count] point -> encoded value index */
} draco_eb_dec;

/// @brief Whole-payload edgebreaker decode state.
typedef struct {
    /// Standard or valence connectivity traversal type.
    uint8_t traversal_type;
    /// Encoded vertices including vertices introduced by split symbols.
    uint32_t num_vertices; /* encoded vertices + split symbols */
    /// Declared triangular face count.
    uint32_t num_faces;
    /// Number of non-position attribute connectivity streams.
    uint8_t num_att_data;
    /// Number of topology symbols.
    uint32_t num_symbols;
    /// Number of topology split events.
    uint32_t num_split_symbols;
    /// Allocated corner count, equal to three times @ref num_faces.
    uint32_t corner_cap; /* 3 * num_faces */
    /// Vertex-indexed scratch capacity used by traversal arrays.
    uint32_t visit_cap; /* max(num_vertices, corner_cap) */
    /* connectivity */
    /// Position face-to-vertex table indexed by corner.
    int32_t *face_to_vertex; /* [corner_cap], corner-indexed */
    /// Number of reconstructed faces.
    uint32_t face_count;
    /// Opposite-corner table.
    int32_t *opposite; /* [corner_cap] */
    /// Representative corner for every encoded vertex.
    int32_t *vertex_corners; /* [num_vertices] */
    /// Whether each encoded vertex currently lies on a hole boundary.
    uint8_t *is_vert_hole; /* [num_vertices] */
    /// Highest vertex identifier created by topology reconstruction.
    int32_t last_vert_added;
    /// Most recently decoded topology symbol.
    int last_symbol;
    /// Active valence-context index.
    int active_context;
    /* active corner stack */
    /// Active topology corner stack.
    int32_t *stack;
    /// Number of entries in @ref stack.
    uint32_t stack_size;
    /// Allocated capacity of @ref stack.
    uint32_t stack_cap;
    /* topology splits (consumed back to front) */
    /// Encoder symbol IDs at which split events become active.
    uint32_t *split_source_symbol;
    /// Encoder symbol IDs of the matching split symbols.
    uint32_t *split_symbol;
    /// Encoded split-edge selectors.
    uint8_t *split_edge;
    /// Number of unconsumed split events.
    uint32_t split_count;
    /* matched splits awaiting their S symbol */
    /// Decoder symbol IDs awaiting their matching split.
    int32_t *match_split_id;
    /// Active corners associated with pending split matches.
    int32_t *match_corner;
    /// Number of pending split matches.
    uint32_t match_count;
    /// Allocated capacity of the pending-match arrays.
    uint32_t match_cap;
    /* standard-traversal symbol bits (LSB-first per byte) */
    /// Borrowed standard-traversal topology symbol bitstream.
    const uint8_t *sym_buf;
    /// Byte length of @ref sym_buf.
    size_t sym_size;
    /// Next unread bit position in @ref sym_buf.
    uint64_t sym_bitpos;
    /* start-face rABS stream */
    /// Zero-symbol probability for start-face flags.
    uint8_t start_face_prob_zero;
    /// Borrowed start-face flag stream.
    const uint8_t *start_face_buf;
    /// Byte length of @ref start_face_buf.
    size_t start_face_size;
    /* valence traversal */
    /// Current reconstructed valence of every encoded vertex.
    uint32_t *valences; /* [num_vertices] */
    /// Reverse-consumption positions for valence-context symbol streams.
    uint32_t ctx_pos[DRACO_EB_NUM_VALENCES];
    /// Decoded topology symbols grouped by clamped valence context.
    uint32_t *ctx_syms[DRACO_EB_NUM_VALENCES];
    /* attribute data + decoders */
    /// Fixed-capacity seam-connectivity streams.
    draco_eb_data att_data[DRACO_EB_MAX_ATT_DATA];
    /// Fixed-capacity attribute traversal decoders.
    draco_eb_dec dec[DRACO_EB_MAX_DECODERS];
    /// Number of initialized entries in @ref dec.
    uint32_t num_decoders;
    /* point mapping */
    /// Final output point associated with each position corner.
    int32_t *corner_to_point; /* [corner_cap] */
    /// Number of output points after seam splitting.
    uint32_t point_count;
    /* traversal scratch */
    /// Per-face visited flags.
    uint8_t *face_visited; /* [num_faces] */
    /// Per-traversal-vertex visited flags.
    uint8_t *vertex_visited; /* [visit_cap] */
    /// Corner stack used by attribute traversals.
    int32_t *trav_stack; /* [corner_cap + 2] */
    /// Number of entries in @ref trav_stack.
    uint32_t trav_size;
    /* prediction-degree traversal */
    /// Current prediction degree for each traversal vertex.
    uint32_t *pred_degree; /* [visit_cap] */
    /// Three priority stacks for prediction-degree traversal.
    int32_t *pd_stack[3]; /* [corner_cap + 2] each */
    /// Number of entries in each prediction-degree stack.
    uint32_t pd_size[3];
    /// Lowest non-empty prediction-degree priority.
    int pd_best;
} draco_eb;

/// @brief Release every allocation owned by an edgebreaker decoder and clear its state.
/// @param eb Decoder state to destroy.
static void draco_eb_free(draco_eb *eb) {
    free(eb->face_to_vertex);
    free(eb->opposite);
    free(eb->vertex_corners);
    free(eb->is_vert_hole);
    free(eb->stack);
    free(eb->split_source_symbol);
    free(eb->split_symbol);
    free(eb->split_edge);
    free(eb->match_split_id);
    free(eb->match_corner);
    free(eb->valences);
    for (int i = 0; i < DRACO_EB_NUM_VALENCES; i++)
        free(eb->ctx_syms[i]);
    for (int a = 0; a < DRACO_EB_MAX_ATT_DATA; a++) {
        free(eb->att_data[a].edge_on_seam);
        free(eb->att_data[a].vertex_on_seam);
        free(eb->att_data[a].corner_to_vertex);
        free(eb->att_data[a].face_to_vertex);
        free(eb->att_data[a].leftmost_corner);
    }
    for (int d = 0; d < DRACO_EB_MAX_DECODERS; d++) {
        free(eb->dec[d].value_to_corner);
        free(eb->dec[d].vertex_to_value);
        free(eb->dec[d].indices_map);
    }
    free(eb->corner_to_point);
    free(eb->face_visited);
    free(eb->vertex_visited);
    free(eb->trav_stack);
    free(eb->pred_degree);
    for (int i = 0; i < 3; i++)
        free(eb->pd_stack[i]);
    memset(eb, 0, sizeof(*eb));
}

/// @brief Read @p nbits from the standard-traversal symbol buffer, LSB-first
///        within each byte (verified against reference-encoder output).
/// @param eb Edgebreaker state owning the symbol stream and bit cursor.
/// @param nbits Number of bits to read into the low bits of the result.
/// @param ok In/out success flag cleared when the requested range exceeds the stream.
/// @return Decoded bit field, or zero when unavailable.
static uint32_t draco_eb_read_symbol_bits(draco_eb *eb, uint32_t nbits, int *ok) {
    uint32_t val = 0;
    for (uint32_t k = 0; k < nbits; k++) {
        uint64_t idx = eb->sym_bitpos + k;
        size_t byte = (size_t)(idx >> 3);
        if (byte >= eb->sym_size) {
            *ok = 0;
            return 0;
        }
        val |= (uint32_t)((eb->sym_buf[byte] >> (idx & 7u)) & 1u) << k;
    }
    eb->sym_bitpos += nbits;
    return val;
}

/* --- Corner-table helpers (spec Next/Previous/PosOpposite/Swing*) --- */

/// @brief Return the next counterclockwise corner of the same triangle.
/// @param c Corner index, or a negative sentinel.
/// @return Next local corner, preserving a negative sentinel unchanged.
static int32_t draco_eb_next(int32_t c) {
    if (c < 0)
        return c;
    return ((c % 3) == 2) ? c - 2 : c + 1;
}

/// @brief Return the previous clockwise corner of the same triangle.
/// @param c Corner index, or a negative sentinel.
/// @return Previous local corner, preserving a negative sentinel unchanged.
static int32_t draco_eb_prev(int32_t c) {
    if (c < 0)
        return c;
    return ((c % 3) == 0) ? c + 2 : c - 1;
}

/// @brief Look up the position-connectivity corner opposite @p c.
/// @param eb Edgebreaker state containing the opposite-corner table.
/// @param c Corner index to query.
/// @return Opposite corner index, or -1 for a boundary or invalid corner.
static int32_t draco_eb_pos_opposite(const draco_eb *eb, int32_t c) {
    if (c < 0 || (uint32_t)c >= eb->corner_cap)
        return -1;
    return eb->opposite[c];
}

/// @brief Look up an attribute opposite corner while treating seam edges as boundaries.
/// @param eb Edgebreaker state containing seam and opposite-corner tables.
/// @param a Attribute-connectivity data index.
/// @param c Corner index to query.
/// @return Opposite corner, or -1 for a seam, boundary, or invalid corner.
static int32_t draco_eb_attr_opposite(const draco_eb *eb, uint32_t a, int32_t c) {
    if (c < 0 || (uint32_t)c >= eb->corner_cap || eb->att_data[a].edge_on_seam[c])
        return -1;
    return draco_eb_pos_opposite(eb, c);
}

/// @brief True when decoder @p d uses recomputed (seam-split) attribute vertices.
/// @param eb Edgebreaker state containing decoder descriptors.
/// @param d Decoder index to classify.
/// @return Non-zero when the decoder uses corner-mapped attribute connectivity.
static int draco_eb_dec_is_corner_mapped(const draco_eb *eb, uint32_t d) {
    return d != 0 && eb->dec[d].decoder_type == 1 && eb->dec[d].data_id != 0xFF;
}

/// @brief Resolve an opposite corner through a decoder's position or attribute topology.
/// @param eb Edgebreaker state.
/// @param d Attribute decoder index.
/// @param c Corner index to query.
/// @return Decoder-specific opposite corner, or -1 at a boundary.
static int32_t draco_eb_opposite(const draco_eb *eb, uint32_t d, int32_t c) {
    if (draco_eb_dec_is_corner_mapped(eb, d))
        return draco_eb_attr_opposite(eb, eb->dec[d].data_id, c);
    return draco_eb_pos_opposite(eb, c);
}

/// @brief Swing clockwise around the decoder-specific vertex incident to @p c.
/// @param eb Edgebreaker state.
/// @param d Attribute decoder index.
/// @param c Starting corner.
/// @return Adjacent corner around the vertex, or a negative boundary sentinel.
static int32_t draco_eb_swing_right(const draco_eb *eb, uint32_t d, int32_t c) {
    return draco_eb_prev(draco_eb_opposite(eb, d, draco_eb_prev(c)));
}

/// @brief Swing counterclockwise around the decoder-specific vertex incident to @p c.
/// @param eb Edgebreaker state.
/// @param d Attribute decoder index.
/// @param c Starting corner.
/// @return Adjacent corner around the vertex, or a negative boundary sentinel.
static int32_t draco_eb_swing_left(const draco_eb *eb, uint32_t d, int32_t c) {
    return draco_eb_next(draco_eb_opposite(eb, d, draco_eb_next(c)));
}

/// @brief Seam-gated swing used by seam vertex recomputation (always gated by
///        attribute data @p a regardless of the owning decoder's type).
/// @param eb Edgebreaker state.
/// @param a Attribute-connectivity data index.
/// @param c Starting corner.
/// @return Next seam-aware corner around the attribute vertex, or a negative boundary sentinel.
static int32_t draco_eb_attr_swing_left(const draco_eb *eb, uint32_t a, int32_t c) {
    return draco_eb_next(draco_eb_attr_opposite(eb, a, draco_eb_next(c)));
}

/// @brief Select the face-to-vertex table used by an attribute decoder.
/// @param eb Edgebreaker state.
/// @param d Decoder index.
/// @return Borrowed position table or seam-recomputed attribute table.
static const int32_t *draco_eb_ftv(const draco_eb *eb, uint32_t d) {
    if (draco_eb_dec_is_corner_mapped(eb, d))
        return eb->att_data[eb->dec[d].data_id].face_to_vertex;
    return eb->face_to_vertex;
}

/// @brief Resolve the current, next, and previous vertex IDs around one triangle corner.
/// @param eb Edgebreaker state.
/// @param d Decoder whose connectivity table is used.
/// @param corner Corner index to resolve.
/// @param v Receives the vertex at @p corner.
/// @param n Receives the vertex at the next local corner.
/// @param p Receives the vertex at the previous local corner.
static void draco_eb_corner_to_verts(
    const draco_eb *eb, uint32_t d, int32_t corner, int32_t *v, int32_t *n, int32_t *p) {
    const int32_t *ftv = draco_eb_ftv(eb, d);
    int32_t base;
    int32_t local;
    if (corner < 0 || (uint32_t)corner >= eb->face_count * 3u) {
        *v = *n = *p = -1;
        return;
    }
    base = (corner / 3) * 3;
    local = corner % 3;
    *v = ftv[base + local];
    *n = ftv[base + (local + 1) % 3];
    *p = ftv[base + (local + 2) % 3];
}

/// @brief Resolve only the vertex ID at a decoder-specific corner.
/// @param eb Edgebreaker state.
/// @param d Decoder whose connectivity table is used.
/// @param corner Corner index to resolve.
/// @return Vertex ID, or -1 when the corner is invalid.
static int32_t draco_eb_corner_to_vert(const draco_eb *eb, uint32_t d, int32_t corner) {
    int32_t v, n, p;
    draco_eb_corner_to_verts(eb, d, corner, &v, &n, &p);
    return v;
}

/// @brief spec MapCornerToVertex (the corner->vertex direction is carried by the
///        face table itself; only the vertex->corner link is tracked here).
/// @param eb Edgebreaker state whose representative-corner table is updated.
/// @param corner Corner to associate with the vertex.
/// @param vert Vertex identifier, or a negative value requiring no reverse mapping.
/// @return Non-zero when both identifiers are within their valid domains.
static int draco_eb_map_corner(draco_eb *eb, int32_t corner, int32_t vert) {
    if (corner < 0 || (uint32_t)corner >= eb->corner_cap)
        return 0;
    if (vert >= 0) {
        if ((uint32_t)vert >= eb->num_vertices)
            return 0;
        eb->vertex_corners[vert] = corner;
    }
    return 1;
}

/// @brief Join two valid corners as symmetric opposites.
/// @param eb Edgebreaker state whose opposite table is updated.
/// @param c First corner index.
/// @param oc Second corner index.
/// @return Non-zero when both corners were in range and linked.
static int draco_eb_set_opposite(draco_eb *eb, int32_t c, int32_t oc) {
    if (c < 0 || oc < 0 || (uint32_t)c >= eb->corner_cap || (uint32_t)oc >= eb->corner_cap)
        return 0;
    eb->opposite[c] = oc;
    eb->opposite[oc] = c;
    return 1;
}

/// @brief Append one triangle to the reconstructed face-to-vertex table.
/// @param eb Edgebreaker state receiving the face.
/// @param v Vertex at the first local corner.
/// @param n Vertex at the second local corner.
/// @param p Vertex at the third local corner.
/// @return Non-zero when capacity remained for the new face.
static int draco_eb_push_face(draco_eb *eb, int32_t v, int32_t n, int32_t p) {
    int32_t *ftv;
    if (eb->face_count >= eb->num_faces)
        return 0;
    ftv = eb->face_to_vertex + (size_t)eb->face_count * 3u;
    ftv[0] = v;
    ftv[1] = n;
    ftv[2] = p;
    eb->face_count++;
    return 1;
}

/// @brief Replace every reconstructed position occurrence of one vertex ID with another.
/// @param eb Edgebreaker state whose completed face prefix is rewritten.
/// @param from Vertex identifier to replace.
/// @param to Replacement vertex identifier.
static void draco_eb_replace_verts(draco_eb *eb, int32_t from, int32_t to) {
    uint32_t total = eb->face_count * 3u;
    for (uint32_t i = 0; i < total; i++) {
        if (eb->face_to_vertex[i] == from)
            eb->face_to_vertex[i] = to;
    }
}

/// @brief spec UpdateCornersAfterMerge — remap the swing fan opposite @p c to @p v.
/// @param eb Edgebreaker state whose vertex-to-corner links are updated.
/// @param c Corner adjacent to the merged fan.
/// @param v Surviving vertex identifier.
/// @return Non-zero when the complete fan was remapped without invalid indices or cycles.
static int draco_eb_update_corners_after_merge(draco_eb *eb, int32_t c, int32_t v) {
    int32_t opp = draco_eb_pos_opposite(eb, c);
    uint32_t guard = 0;
    if (opp >= 0) {
        int32_t corner_n = draco_eb_next(opp);
        while (corner_n >= 0) {
            if (guard++ > eb->corner_cap)
                return 0;
            if (!draco_eb_map_corner(eb, corner_n, v))
                return 0;
            corner_n = draco_eb_swing_left(eb, 0, corner_n);
        }
    }
    return 1;
}

/// @brief spec IsTopologySplit — consume a matching split event, back to front.
/// @param eb Edgebreaker state containing unconsumed split events.
/// @param encoder_symbol_id Source encoder symbol being matched.
/// @param out_edge Receives the encoded split-edge selector.
/// @param out_split_id Receives the paired encoder split-symbol ID.
/// @return Non-zero when the last pending event matched and was consumed.
static int draco_eb_is_topology_split(draco_eb *eb,
                                      uint32_t encoder_symbol_id,
                                      uint8_t *out_edge,
                                      uint32_t *out_split_id) {
    if (eb->split_count == 0 || eb->split_source_symbol[eb->split_count - 1] != encoder_symbol_id)
        return 0;
    eb->split_count--;
    *out_edge = eb->split_edge[eb->split_count];
    *out_split_id = eb->split_symbol[eb->split_count];
    return 1;
}

/// @brief spec NewActiveCornerReached — apply one C/S/L/R/E symbol to the corner table.
/// @param eb Edgebreaker state mutated by topology reconstruction.
/// @param new_corner First corner index reserved for the next face.
/// @param symbol_id Decoder-order symbol identifier.
/// @return Non-zero when the symbol produced valid bounded connectivity state.
static int draco_eb_new_corner(draco_eb *eb, int32_t new_corner, uint32_t symbol_id) {
    int check_topology_split = 0;
    int32_t vert = -1;
    int32_t next = -1;
    int32_t prev = -1;
    uint32_t guard;
    if (new_corner != (int32_t)(eb->face_count * 3u))
        return 0;
    switch (eb->last_symbol) {
        case DRACO_TOPOLOGY_C: {
            int32_t corner_a, corner_b;
            if (eb->stack_size == 0)
                return 0;
            corner_a = eb->stack[eb->stack_size - 1];
            corner_b = draco_eb_prev(corner_a);
            guard = 0;
            while (draco_eb_pos_opposite(eb, corner_b) >= 0) {
                if (guard++ > eb->corner_cap)
                    return 0;
                corner_b = draco_eb_prev(draco_eb_pos_opposite(eb, corner_b));
            }
            if (!draco_eb_set_opposite(eb, corner_a, new_corner + 1) ||
                !draco_eb_set_opposite(eb, corner_b, new_corner + 2))
                return 0;
            eb->stack[eb->stack_size - 1] = new_corner;
            vert = draco_eb_corner_to_vert(eb, 0, draco_eb_next(corner_a));
            next = draco_eb_corner_to_vert(eb, 0, draco_eb_next(corner_b));
            prev = draco_eb_corner_to_vert(eb, 0, draco_eb_prev(corner_a));
            if (vert < 0 || next < 0 || prev < 0 || (uint32_t)vert >= eb->num_vertices ||
                (uint32_t)next >= eb->num_vertices || (uint32_t)prev >= eb->num_vertices)
                return 0;
            if (eb->traversal_type == DRACO_EB_VALENCE) {
                eb->valences[next] += 1u;
                eb->valences[prev] += 1u;
            }
            if (!draco_eb_push_face(eb, vert, next, prev))
                return 0;
            eb->is_vert_hole[vert] = 0;
            if (!draco_eb_map_corner(eb, new_corner, vert) ||
                !draco_eb_map_corner(eb, new_corner + 1, next) ||
                !draco_eb_map_corner(eb, new_corner + 2, prev))
                return 0;
            break;
        }
        case DRACO_TOPOLOGY_S: {
            int32_t corner_a, corner_b, corner_n, vertex_n;
            if (eb->stack_size == 0)
                return 0;
            corner_b = eb->stack[--eb->stack_size];
            for (uint32_t i = 0; i < eb->match_count; i++) {
                if (eb->match_split_id[i] == (int32_t)symbol_id) {
                    if (eb->stack_size >= eb->stack_cap)
                        return 0;
                    eb->stack[eb->stack_size++] = eb->match_corner[i];
                }
            }
            if (eb->stack_size == 0)
                return 0;
            corner_a = eb->stack[eb->stack_size - 1];
            if (!draco_eb_set_opposite(eb, corner_a, new_corner + 2) ||
                !draco_eb_set_opposite(eb, corner_b, new_corner + 1))
                return 0;
            eb->stack[eb->stack_size - 1] = new_corner;
            vert = draco_eb_corner_to_vert(eb, 0, draco_eb_prev(corner_a));
            next = draco_eb_corner_to_vert(eb, 0, draco_eb_next(corner_a));
            prev = draco_eb_corner_to_vert(eb, 0, draco_eb_prev(corner_b));
            if (vert < 0 || next < 0 || prev < 0 || (uint32_t)vert >= eb->num_vertices ||
                (uint32_t)next >= eb->num_vertices || (uint32_t)prev >= eb->num_vertices)
                return 0;
            if (!draco_eb_map_corner(eb, new_corner, vert) ||
                !draco_eb_map_corner(eb, new_corner + 1, next) ||
                !draco_eb_map_corner(eb, new_corner + 2, prev))
                return 0;
            corner_n = draco_eb_next(corner_b);
            vertex_n = draco_eb_corner_to_vert(eb, 0, corner_n);
            if (vertex_n < 0 || (uint32_t)vertex_n >= eb->num_vertices)
                return 0;
            if (eb->traversal_type == DRACO_EB_VALENCE)
                eb->valences[vert] += eb->valences[vertex_n];
            draco_eb_replace_verts(eb, vertex_n, vert);
            if (eb->traversal_type == DRACO_EB_VALENCE) {
                eb->valences[next] += 1u;
                eb->valences[prev] += 1u;
            }
            if (!draco_eb_push_face(eb, vert, next, prev))
                return 0;
            if (!draco_eb_update_corners_after_merge(eb, new_corner + 1, vert))
                return 0;
            eb->vertex_corners[vertex_n] = -1;
            break;
        }
        case DRACO_TOPOLOGY_R: {
            int32_t corner_a;
            if (eb->stack_size == 0)
                return 0;
            corner_a = eb->stack[eb->stack_size - 1];
            if (!draco_eb_set_opposite(eb, new_corner + 2, corner_a))
                return 0;
            eb->stack[eb->stack_size - 1] = new_corner;
            check_topology_split = 1;
            vert = draco_eb_corner_to_vert(eb, 0, draco_eb_prev(corner_a));
            next = draco_eb_corner_to_vert(eb, 0, draco_eb_next(corner_a));
            prev = ++eb->last_vert_added;
            if (vert < 0 || next < 0 || (uint32_t)vert >= eb->num_vertices ||
                (uint32_t)next >= eb->num_vertices || (uint32_t)prev >= eb->num_vertices)
                return 0;
            if (eb->traversal_type == DRACO_EB_VALENCE) {
                eb->valences[vert] += 1u;
                eb->valences[next] += 1u;
                eb->valences[prev] += 2u;
            }
            if (!draco_eb_push_face(eb, vert, next, prev))
                return 0;
            if (!draco_eb_map_corner(eb, new_corner + 2, prev) ||
                !draco_eb_map_corner(eb, new_corner, vert) ||
                !draco_eb_map_corner(eb, new_corner + 1, next))
                return 0;
            break;
        }
        case DRACO_TOPOLOGY_L: {
            int32_t corner_a;
            if (eb->stack_size == 0)
                return 0;
            corner_a = eb->stack[eb->stack_size - 1];
            if (!draco_eb_set_opposite(eb, new_corner + 1, corner_a))
                return 0;
            eb->stack[eb->stack_size - 1] = new_corner;
            check_topology_split = 1;
            vert = draco_eb_corner_to_vert(eb, 0, draco_eb_next(corner_a));
            next = ++eb->last_vert_added;
            prev = draco_eb_corner_to_vert(eb, 0, draco_eb_prev(corner_a));
            if (vert < 0 || prev < 0 || (uint32_t)vert >= eb->num_vertices ||
                (uint32_t)next >= eb->num_vertices || (uint32_t)prev >= eb->num_vertices)
                return 0;
            if (eb->traversal_type == DRACO_EB_VALENCE) {
                eb->valences[vert] += 1u;
                eb->valences[next] += 2u;
                eb->valences[prev] += 1u;
            }
            if (!draco_eb_push_face(eb, vert, next, prev))
                return 0;
            if (!draco_eb_map_corner(eb, new_corner + 2, prev) ||
                !draco_eb_map_corner(eb, new_corner, vert) ||
                !draco_eb_map_corner(eb, new_corner + 1, next))
                return 0;
            break;
        }
        case DRACO_TOPOLOGY_E: {
            if (eb->stack_size >= eb->stack_cap)
                return 0;
            eb->stack[eb->stack_size++] = new_corner;
            check_topology_split = 1;
            vert = eb->last_vert_added + 1;
            next = vert + 1;
            prev = next + 1;
            if ((uint32_t)prev >= eb->num_vertices)
                return 0;
            if (eb->traversal_type == DRACO_EB_VALENCE) {
                eb->valences[vert] += 2u;
                eb->valences[next] += 2u;
                eb->valences[prev] += 2u;
            }
            if (!draco_eb_push_face(eb, vert, next, prev))
                return 0;
            eb->last_vert_added = prev;
            if (!draco_eb_map_corner(eb, new_corner, vert) ||
                !draco_eb_map_corner(eb, new_corner + 1, next) ||
                !draco_eb_map_corner(eb, new_corner + 2, prev))
                return 0;
            break;
        }
        default:
            return 0;
    }
    if (eb->traversal_type == DRACO_EB_VALENCE) {
        uint32_t valence = eb->valences[next];
        if (valence < DRACO_EB_MIN_VALENCE)
            valence = DRACO_EB_MIN_VALENCE;
        else if (valence > DRACO_EB_MIN_VALENCE + DRACO_EB_NUM_VALENCES - 1)
            valence = DRACO_EB_MIN_VALENCE + DRACO_EB_NUM_VALENCES - 1;
        eb->active_context = (int)(valence - DRACO_EB_MIN_VALENCE);
    }
    if (check_topology_split) {
        uint32_t encoder_symbol_id = eb->num_symbols - symbol_id - 1u;
        uint8_t split_edge;
        uint32_t enc_split_id;
        while (draco_eb_is_topology_split(eb, encoder_symbol_id, &split_edge, &enc_split_id)) {
            int32_t act_top, new_active;
            if (eb->stack_size == 0 || eb->match_count >= eb->match_cap ||
                enc_split_id >= eb->num_symbols)
                return 0;
            act_top = eb->stack[eb->stack_size - 1];
            new_active = (split_edge == 1 /* RIGHT_FACE_EDGE */) ? draco_eb_next(act_top)
                                                                 : draco_eb_prev(act_top);
            eb->match_split_id[eb->match_count] = (int32_t)(eb->num_symbols - enc_split_id - 1u);
            eb->match_corner[eb->match_count] = new_active;
            eb->match_count++;
        }
    }
    return 1;
}

/// @brief spec EdgebreakerDecodeSymbol (standard bit stream or valence contexts).
/// @param eb Edgebreaker state supplying the active symbol source and context.
/// @return Non-zero after storing a valid topology symbol in `eb->last_symbol`.
static int draco_eb_decode_symbol(draco_eb *eb) {
    if (eb->traversal_type == DRACO_EB_VALENCE) {
        static const int topo[5] = {DRACO_TOPOLOGY_C,
                                    DRACO_TOPOLOGY_S,
                                    DRACO_TOPOLOGY_L,
                                    DRACO_TOPOLOGY_R,
                                    DRACO_TOPOLOGY_E};
        int ctx = eb->active_context;
        uint32_t sym;
        if (ctx < 0) {
            eb->last_symbol = DRACO_TOPOLOGY_E;
            return 1;
        }
        if (eb->ctx_pos[ctx] == 0)
            return 0;
        sym = eb->ctx_syms[ctx][--eb->ctx_pos[ctx]];
        if (sym >= 5u)
            return 0;
        eb->last_symbol = topo[sym];
        return 1;
    }
    {
        int ok = 1;
        uint32_t symbol = draco_eb_read_symbol_bits(eb, 1, &ok);
        if (!ok)
            return 0;
        if (symbol != DRACO_TOPOLOGY_C) {
            uint32_t suffix = draco_eb_read_symbol_bits(eb, 2, &ok);
            if (!ok)
                return 0;
            symbol |= suffix << 1;
        }
        eb->last_symbol = (int)symbol;
        return 1;
    }
}

/// @brief spec ProcessInteriorEdges — close interior fans left by the symbol pass.
/// @param eb Edgebreaker state containing active boundary corners and start-face flags.
/// @return Non-zero when every flagged interior fan was closed consistently.
static int draco_eb_process_interior_edges(draco_eb *eb) {
    draco_rans ans;
    int have_ans = 0;
    if (eb->start_face_size > 0)
        have_ans = draco_rans_init(&ans, eb->start_face_buf, eb->start_face_size, 4096u);
    while (eb->stack_size > 0) {
        int32_t corner_a = eb->stack[--eb->stack_size];
        uint32_t interior = 0;
        uint32_t guard;
        if (!have_ans)
            return 0;
        if (!draco_rabs_read(&ans, eb->start_face_prob_zero, &interior))
            return 0;
        if (!interior)
            continue;
        {
            int32_t corner_b = draco_eb_prev(corner_a);
            int32_t corner_c = draco_eb_next(corner_a);
            int32_t new_corner;
            int32_t tv, tp, next_a, next_b, next_c;
            guard = 0;
            while (draco_eb_pos_opposite(eb, corner_b) >= 0) {
                if (guard++ > eb->corner_cap)
                    return 0;
                corner_b = draco_eb_prev(draco_eb_pos_opposite(eb, corner_b));
            }
            guard = 0;
            while (draco_eb_pos_opposite(eb, corner_c) >= 0) {
                if (guard++ > eb->corner_cap)
                    return 0;
                corner_c = draco_eb_next(draco_eb_pos_opposite(eb, corner_c));
            }
            new_corner = (int32_t)(eb->face_count * 3u);
            if (!draco_eb_set_opposite(eb, new_corner, corner_a) ||
                !draco_eb_set_opposite(eb, new_corner + 1, corner_b) ||
                !draco_eb_set_opposite(eb, new_corner + 2, corner_c))
                return 0;
            draco_eb_corner_to_verts(eb, 0, corner_a, &tv, &next_a, &tp);
            draco_eb_corner_to_verts(eb, 0, corner_b, &tv, &next_b, &tp);
            draco_eb_corner_to_verts(eb, 0, corner_c, &tv, &next_c, &tp);
            if (next_a < 0 || next_b < 0 || next_c < 0 || (uint32_t)next_a >= eb->num_vertices ||
                (uint32_t)next_b >= eb->num_vertices || (uint32_t)next_c >= eb->num_vertices)
                return 0;
            if (!draco_eb_push_face(eb, next_b, next_c, next_a))
                return 0;
            if (!draco_eb_map_corner(eb, new_corner, next_b) ||
                !draco_eb_map_corner(eb, new_corner + 1, next_c) ||
                !draco_eb_map_corner(eb, new_corner + 2, next_a))
                return 0;
            eb->is_vert_hole[next_b] = 0;
            eb->is_vert_hole[next_c] = 0;
            eb->is_vert_hole[next_a] = 0;
        }
    }
    return 1;
}

/// @brief Parse the edgebreaker connectivity header and allocate decode state.
/// @param b Bounded reader positioned at the connectivity header.
/// @param eb Zero-initialized edgebreaker state receiving counts and scratch arrays.
/// @return Non-zero when declarations are valid and all required allocations succeed.
static int draco_eb_parse_and_alloc(draco_buf *b, draco_eb *eb) {
    uint64_t nv, nf, ns, nss, total_verts;
    uint64_t stack_cap;
    size_t corner_i32_bytes;
    size_t vertex_i32_bytes;
    size_t stack_i32_bytes;
    size_t traversal_i32_bytes;
    eb->traversal_type = draco_read_u8(b);
    nv = draco_read_varint(b);
    nf = draco_read_varint(b);
    eb->num_att_data = draco_read_u8(b);
    ns = draco_read_varint(b);
    nss = draco_read_varint(b);
    if (b->failed)
        return 0;
    if (eb->traversal_type != DRACO_EB_STANDARD && eb->traversal_type != DRACO_EB_VALENCE)
        return 0;
    if (eb->num_att_data > DRACO_EB_MAX_ATT_DATA)
        return 0;
    /* Corner IDs and all face/corner sentinels are int32_t. Limiting only the
     * uint32_t multiplication allowed valid-looking counts above INT32_MAX/3
     * to wrap negative whenever a corner was narrowed. */
    if (nf == 0 || nf > (uint64_t)INT32_MAX / 3u)
        return 0;
    /* Bound the allocation drivers to the payload size; nss <= ns <= nf and
     * total_verts = nv + nss, so capping nf and nv caps every derived buffer. */
    if (!draco_count_fits_payload(nf, b->size) || !draco_count_fits_payload(nv, b->size))
        return 0;
    if (ns > nf)
        return 0;
    /* Split symbols are a subset of the S symbols, so nss > ns is malformed.
     * Bounding it here also keeps every count below 2^30, making the
     * total_verts arithmetic and the uint32 narrowings below overflow-free. */
    if (nss > ns)
        return 0;
    if (nv > (uint64_t)INT32_MAX)
        return 0;
    total_verts = nv + nss;
    if (total_verts == 0 || total_verts > 3u * nf + nss || total_verts > (uint64_t)INT32_MAX)
        return 0;
    eb->num_vertices = (uint32_t)total_verts;
    eb->num_faces = (uint32_t)nf;
    eb->num_symbols = (uint32_t)ns;
    eb->num_split_symbols = (uint32_t)nss;
    eb->corner_cap = eb->num_faces * 3u;
    eb->visit_cap = eb->num_vertices > eb->corner_cap ? eb->num_vertices : eb->corner_cap;
    eb->last_vert_added = -1;
    eb->last_symbol = -1;
    eb->active_context = -1;
    stack_cap = (uint64_t)eb->num_symbols + eb->num_split_symbols + 4u;
    if (stack_cap > UINT32_MAX ||
        !draco_size_mul((size_t)eb->corner_cap, sizeof(int32_t), &corner_i32_bytes) ||
        !draco_size_mul((size_t)eb->num_vertices, sizeof(int32_t), &vertex_i32_bytes) ||
        !draco_size_mul((size_t)stack_cap, sizeof(int32_t), &stack_i32_bytes) ||
        !draco_size_mul((size_t)eb->corner_cap + 2u, sizeof(int32_t), &traversal_i32_bytes))
        return 0;
    eb->stack_cap = (uint32_t)stack_cap;
    eb->face_to_vertex = (int32_t *)malloc(corner_i32_bytes);
    eb->opposite = (int32_t *)malloc(corner_i32_bytes);
    eb->vertex_corners = (int32_t *)malloc(vertex_i32_bytes);
    eb->is_vert_hole = (uint8_t *)malloc(eb->num_vertices);
    eb->stack = (int32_t *)malloc(stack_i32_bytes);
    eb->corner_to_point = (int32_t *)malloc(corner_i32_bytes);
    eb->face_visited = (uint8_t *)malloc(eb->num_faces);
    eb->vertex_visited = (uint8_t *)malloc(eb->visit_cap);
    eb->trav_stack = (int32_t *)malloc(traversal_i32_bytes);
    if (!eb->face_to_vertex || !eb->opposite || !eb->vertex_corners || !eb->is_vert_hole ||
        !eb->stack || !eb->corner_to_point || !eb->face_visited || !eb->vertex_visited ||
        !eb->trav_stack)
        return 0;
    memset(eb->face_to_vertex, 0xFF, corner_i32_bytes);
    memset(eb->opposite, 0xFF, corner_i32_bytes);
    memset(eb->vertex_corners, 0xFF, vertex_i32_bytes);
    memset(eb->is_vert_hole, 1, eb->num_vertices);
    if (eb->traversal_type == DRACO_EB_VALENCE) {
        eb->valences = (uint32_t *)calloc(eb->num_vertices, sizeof(uint32_t));
        if (!eb->valences)
            return 0;
    }
    return 1;
}

/// @brief spec ParseTopologySplitEvents + ProcessSplitData.
/// @param b Bounded reader positioned at the split-event block.
/// @param eb Edgebreaker state receiving validated split arrays.
/// @return Non-zero when all declared events are valid for reverse consumption.
static int draco_eb_parse_splits(draco_buf *b, draco_eb *eb) {
    uint64_t n = draco_read_varint(b);
    size_t i32_bytes;
    size_t u32_bytes;
    if (b->failed || n > eb->num_faces)
        return 0;
    eb->split_count = (uint32_t)n;
    eb->match_cap = (uint32_t)n;
    if (n == 0)
        return 1;
    if (!draco_size_mul((size_t)n, sizeof(uint32_t), &u32_bytes) ||
        !draco_size_mul((size_t)n, sizeof(int32_t), &i32_bytes))
        return 0;
    eb->split_source_symbol = (uint32_t *)malloc(u32_bytes);
    eb->split_symbol = (uint32_t *)malloc(u32_bytes);
    eb->split_edge = (uint8_t *)malloc((size_t)n);
    eb->match_split_id = (int32_t *)malloc(i32_bytes);
    eb->match_corner = (int32_t *)malloc(i32_bytes);
    if (!eb->split_source_symbol || !eb->split_symbol || !eb->split_edge || !eb->match_split_id ||
        !eb->match_corner)
        return 0;
    {
        uint32_t last_id = 0;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t source_delta = draco_read_varint(b);
            uint64_t split_delta = draco_read_varint(b);
            uint64_t source_id;
            if (b->failed || source_delta > UINT64_MAX - last_id)
                return 0;
            source_id = source_delta + last_id;
            if (source_id >= eb->num_symbols || split_delta > source_id)
                return 0;
            eb->split_source_symbol[i] = (uint32_t)source_id;
            eb->split_symbol[i] = (uint32_t)(source_id - split_delta);
            last_id = (uint32_t)source_id;
        }
    }
    {
        size_t base = b->pos;
        size_t bytes = (size_t)((n + 7u) >> 3);
        if (bytes > b->size - b->pos) {
            b->failed = 1;
            return 0;
        }
        for (uint64_t i = 0; i < n; i++)
            eb->split_edge[i] = (b->data[base + (size_t)(i >> 3)] >> (i & 7u)) & 1u;
        b->pos = base + bytes; /* ResetBitReader: realign to a byte boundary */
    }
    return 1;
}

/// @brief spec EdgebreakerTraversalStart — standard buffers or valence contexts.
/// @param b Bounded reader positioned at traversal data.
/// @param eb Allocated edgebreaker state receiving symbol streams and contexts.
/// @return Non-zero when the selected traversal representation is valid.
static int draco_eb_traversal_start(draco_buf *b, draco_eb *eb) {
    if (eb->traversal_type == DRACO_EB_STANDARD) {
        uint64_t sz = draco_read_varint(b);
        if (b->failed || sz > b->size - b->pos)
            return 0;
        eb->sym_buf = b->data + b->pos;
        eb->sym_size = (size_t)sz;
        eb->sym_bitpos = 0;
        b->pos += (size_t)sz;
    }
    eb->start_face_prob_zero = draco_read_u8(b);
    {
        uint64_t sz = draco_read_varint(b);
        if (b->failed || sz > b->size - b->pos)
            return 0;
        eb->start_face_buf = b->data + b->pos;
        eb->start_face_size = (size_t)sz;
        b->pos += (size_t)sz;
    }
    for (uint32_t a = 0; a < eb->num_att_data; a++) {
        uint64_t sz;
        eb->att_data[a].prob_zero = draco_read_u8(b);
        sz = draco_read_varint(b);
        if (b->failed || sz > b->size - b->pos)
            return 0;
        eb->att_data[a].buf = b->data + b->pos;
        eb->att_data[a].size = (size_t)sz;
        b->pos += (size_t)sz;
    }
    if (eb->traversal_type == DRACO_EB_VALENCE) {
        for (int i = 0; i < DRACO_EB_NUM_VALENCES; i++) {
            uint64_t count = draco_read_varint(b);
            if (b->failed || count > eb->num_symbols)
                return 0;
            eb->ctx_pos[i] = (uint32_t)count;
            if (count > 0) {
                eb->ctx_syms[i] = (uint32_t *)calloc((size_t)count, sizeof(uint32_t));
                if (!eb->ctx_syms[i] ||
                    !draco_decode_symbols(b, (uint32_t)count, 1, eb->ctx_syms[i]))
                    return 0;
            }
        }
    }
    return 1;
}

/// @brief spec DecodeEdgebreakerConnectivityData + DecodeEdgeBreakerConnectivity.
/// @param b Bounded reader positioned at split and traversal data.
/// @param eb Allocated edgebreaker state receiving reconstructed connectivity.
/// @return Non-zero when all declared faces and corner links are reconstructed.
static int draco_eb_decode_connectivity(draco_buf *b, draco_eb *eb) {
    if (!draco_eb_parse_and_alloc(b, eb))
        return 0;
    if (!draco_eb_parse_splits(b, eb))
        return 0;
    if (!draco_eb_traversal_start(b, eb))
        return 0;
    for (uint32_t i = 0; i < eb->num_symbols; i++) {
        if (!draco_eb_decode_symbol(eb))
            return 0;
        if (!draco_eb_new_corner(eb, (int32_t)(3u * i), i))
            return 0;
    }
    if (!draco_eb_process_interior_edges(eb))
        return 0;
    if (eb->face_count != eb->num_faces)
        return 0;
    return 1;
}

/// @brief spec UpdateVertexToCornerMap — rotate a hole vertex's corner link to
///        the leftmost corner of its fan.
/// @param eb Edgebreaker state whose representative-corner table is repaired.
/// @param v Position vertex index to update.
/// @return Non-zero when a valid representative was found without cycling.
static int draco_eb_update_vertex_corner(draco_eb *eb, uint32_t v) {
    int32_t first_c = eb->vertex_corners[v];
    int32_t act_c, c;
    uint32_t guard = 0;
    if (first_c < 0)
        return 1;
    act_c = draco_eb_swing_left(eb, 0, first_c);
    c = first_c;
    while (act_c >= 0 && act_c != first_c) {
        if (guard++ > eb->corner_cap)
            return 0;
        c = act_c;
        act_c = draco_eb_swing_left(eb, 0, act_c);
    }
    if (act_c != first_c)
        eb->vertex_corners[v] = c;
    return 1;
}

/// @brief spec DecodeAttributeSeams — one rABS stream per attribute data; boundary
///        edges are seams unconditionally, interior edges consume one flag bit.
/// @param b Bounded reader positioned at the seam streams.
/// @param eb Edgebreaker state receiving edge and vertex seam flags.
/// @return Non-zero when every seam decision stream decodes completely.
static int draco_eb_decode_seams(draco_buf *b, draco_eb *eb) {
    draco_rans ans[DRACO_EB_MAX_ATT_DATA];
    int have[DRACO_EB_MAX_ATT_DATA];
    (void)b;
    for (uint32_t a = 0; a < eb->num_att_data; a++) {
        draco_eb_data *ad = &eb->att_data[a];
        ad->edge_on_seam = (uint8_t *)calloc(eb->corner_cap, 1);
        ad->vertex_on_seam = (uint8_t *)calloc(eb->num_vertices, 1);
        ad->corner_to_vertex = (int32_t *)malloc((size_t)eb->corner_cap * sizeof(int32_t));
        ad->face_to_vertex = (int32_t *)malloc((size_t)eb->corner_cap * sizeof(int32_t));
        ad->leftmost_corner = (int32_t *)malloc((size_t)eb->corner_cap * sizeof(int32_t));
        if (!ad->edge_on_seam || !ad->vertex_on_seam || !ad->corner_to_vertex ||
            !ad->face_to_vertex || !ad->leftmost_corner)
            return 0;
        have[a] = ad->size > 0 && draco_rans_init(&ans[a], ad->buf, ad->size, 4096u);
    }
    for (uint32_t corner = 0; corner < eb->face_count * 3u; corner++) {
        int32_t v, n, p;
        int32_t opp = draco_eb_pos_opposite(eb, (int32_t)corner);
        draco_eb_corner_to_verts(eb, 0, (int32_t)corner, &v, &n, &p);
        if (n < 0 || p < 0 || (uint32_t)n >= eb->num_vertices || (uint32_t)p >= eb->num_vertices)
            return 0;
        if (opp >= 0) {
            if (opp < (int32_t)corner)
                continue;
            for (uint32_t a = 0; a < eb->num_att_data; a++) {
                uint32_t val = 0;
                if (!have[a])
                    return 0;
                if (!draco_rabs_read(&ans[a], eb->att_data[a].prob_zero, &val))
                    return 0;
                if (val) {
                    int32_t ov, on, op2;
                    eb->att_data[a].edge_on_seam[corner] = 1;
                    eb->att_data[a].vertex_on_seam[n] = 1;
                    eb->att_data[a].vertex_on_seam[p] = 1;
                    draco_eb_corner_to_verts(eb, 0, opp, &ov, &on, &op2);
                    if (on >= 0 && (uint32_t)on < eb->num_vertices)
                        eb->att_data[a].vertex_on_seam[on] = 1;
                    if (op2 >= 0 && (uint32_t)op2 < eb->num_vertices)
                        eb->att_data[a].vertex_on_seam[op2] = 1;
                    eb->att_data[a].edge_on_seam[opp] = 1;
                }
            }
        } else {
            for (uint32_t a = 0; a < eb->num_att_data; a++) {
                eb->att_data[a].edge_on_seam[corner] = 1;
                eb->att_data[a].vertex_on_seam[n] = 1;
                eb->att_data[a].vertex_on_seam[p] = 1;
            }
        }
    }
    return 1;
}

/// @brief spec RecomputeVerticesInternal — split position vertices along the seams
///        of attribute data @p a into per-attribute vertices.
/// @param eb Edgebreaker state containing decoded seam topology.
/// @param a Attribute-connectivity data index to reconstruct.
/// @return Non-zero when every corner receives a bounded attribute vertex.
static int draco_eb_recompute_vertices(draco_eb *eb, uint32_t a) {
    draco_eb_data *ad = &eb->att_data[a];
    uint32_t num_new = 0;
    memcpy(ad->face_to_vertex, eb->face_to_vertex, (size_t)eb->face_count * 3u * sizeof(int32_t));
    for (uint32_t i = 0; i < eb->corner_cap; i++) {
        ad->corner_to_vertex[i] = -1;
        ad->leftmost_corner[i] = -1;
    }
    for (uint32_t v = 0; v < eb->num_vertices; v++) {
        int32_t c = eb->vertex_corners[v];
        int32_t first_c, act_c, first_vert_id;
        uint32_t guard = 0;
        if (c < 0)
            continue;
        first_vert_id = (int32_t)num_new++;
        /* num_new can reach corner_cap via seam splits before every outer vertex
         * is processed; bound it here as the seam path below already does, so the
         * leftmost_corner[first_vert_id] write cannot overflow (array is corner_cap). */
        if (first_vert_id >= (int32_t)eb->corner_cap)
            return 0;
        first_c = c;
        if (ad->vertex_on_seam[v]) {
            act_c = draco_eb_attr_swing_left(eb, a, first_c);
            while (act_c >= 0) {
                if (guard++ > eb->corner_cap)
                    return 0;
                first_c = act_c;
                act_c = draco_eb_attr_swing_left(eb, a, act_c);
            }
        }
        ad->corner_to_vertex[first_c] = first_vert_id;
        ad->leftmost_corner[first_vert_id] = first_c;
        act_c = draco_eb_swing_right(eb, 0, first_c);
        guard = 0;
        while (act_c >= 0 && act_c != first_c) {
            int32_t next_act_c;
            if (guard++ > eb->corner_cap)
                return 0;
            next_act_c = draco_eb_next(act_c);
            if (next_act_c >= 0 && ad->edge_on_seam[next_act_c]) {
                first_vert_id = (int32_t)num_new++;
                if (first_vert_id >= (int32_t)eb->corner_cap)
                    return 0;
                ad->leftmost_corner[first_vert_id] = act_c;
            }
            ad->corner_to_vertex[act_c] = first_vert_id;
            act_c = draco_eb_swing_right(eb, 0, act_c);
        }
    }
    ad->vertex_count = num_new;
    for (uint32_t i = 0; i < eb->face_count * 3u; i++)
        ad->face_to_vertex[i] = ad->corner_to_vertex[i];
    return 1;
}

/// @brief spec Attribute_AssignPointsToCorners — dedup corners into output points,
///        splitting wherever any attribute's recomputed vertex changes.
/// @param eb Edgebreaker state receiving `corner_to_point` and the final point count.
/// @return Non-zero when all corners are assigned within bounded output capacity.
static int draco_eb_assign_points(draco_eb *eb) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < eb->corner_cap; i++)
        eb->corner_to_point[i] = -1;
    for (uint32_t v = 0; v < eb->num_vertices; v++) {
        int32_t c = eb->vertex_corners[v];
        int32_t dedup_c;
        uint32_t guard;
        if (c < 0)
            continue;
        dedup_c = c;
        if (!eb->is_vert_hole[v]) {
            for (uint32_t a = 0; a < eb->num_att_data; a++) {
                int32_t vert_id, act_c;
                int seam_found = 0;
                if (!eb->att_data[a].vertex_on_seam[v])
                    continue;
                vert_id = eb->att_data[a].corner_to_vertex[c];
                act_c = draco_eb_swing_right(eb, 0, c);
                guard = 0;
                while (act_c >= 0 && act_c != c) {
                    if (guard++ > eb->corner_cap)
                        return 0;
                    if (eb->att_data[a].corner_to_vertex[act_c] != vert_id) {
                        dedup_c = act_c;
                        seam_found = 1;
                        break;
                    }
                    act_c = draco_eb_swing_right(eb, 0, act_c);
                }
                if (seam_found)
                    break;
            }
        }
        c = dedup_c;
        eb->corner_to_point[c] = (int32_t)count++;
        {
            int32_t prev_c = c;
            int32_t c2 = draco_eb_swing_right(eb, 0, c);
            guard = 0;
            while (c2 >= 0 && c2 != dedup_c) {
                int attribute_seam = 0;
                if (guard++ > eb->corner_cap)
                    return 0;
                for (uint32_t a = 0; a < eb->num_att_data; a++) {
                    if (eb->att_data[a].corner_to_vertex[c2] !=
                        eb->att_data[a].corner_to_vertex[prev_c]) {
                        attribute_seam = 1;
                        break;
                    }
                }
                if (attribute_seam)
                    eb->corner_to_point[c2] = (int32_t)count++;
                else
                    eb->corner_to_point[c2] = eb->corner_to_point[prev_c];
                prev_c = c2;
                c2 = draco_eb_swing_right(eb, 0, c2);
            }
        }
    }
    eb->point_count = count;
    for (uint32_t i = 0; i < eb->face_count * 3u; i++) {
        if (eb->corner_to_point[i] < 0)
            return 0;
    }
    return 1;
}

/// @brief Position-vertex boundary test (reference decoder CornerTable::IsOnBoundary:
///        hole vertices remain marked after connectivity decode).
/// @param eb Edgebreaker state containing hole markers.
/// @param v Position vertex index.
/// @return Non-zero when @p v is valid and marked as lying on a hole boundary.
static int draco_eb_pos_boundary(const draco_eb *eb, int32_t v) {
    if (v < 0 || (uint32_t)v >= eb->num_vertices)
        return 1;
    if (eb->vertex_corners[v] < 0)
        return 1;
    return eb->is_vert_hole[v] ? 1 : 0;
}

/// @brief spec IsOnAttributeBoundary for corner-mapped decoders.
/// @param eb Edgebreaker state containing recomputed attribute connectivity.
/// @param a Attribute-connectivity data index.
/// @param v Attribute vertex index.
/// @return Non-zero when the attribute vertex has an open seam-aware fan.
static int draco_eb_attr_boundary(const draco_eb *eb, uint32_t a, int32_t v) {
    const draco_eb_data *ad = &eb->att_data[a];
    int32_t corner, pv;
    if (v < 0 || (uint32_t)v >= ad->vertex_count)
        return 1;
    corner = ad->leftmost_corner[v];
    if (corner < 0)
        return 1;
    pv = draco_eb_corner_to_vert(eb, 0, corner);
    if (pv < 0 || (uint32_t)pv >= eb->num_vertices)
        return 1;
    return ad->vertex_on_seam[pv] ? 1 : 0;
}

/// @brief spec OnNewVertexVisited — record traversal order for decoder @p d.
/// @param eb Edgebreaker state containing traversal and decoder mappings.
/// @param d Decoder index receiving the new value.
/// @param vertex Traversal vertex visited for the first time.
/// @param corner Representative corner associated with the encoded value.
/// @return Non-zero when identifiers and output capacity are valid.
static int draco_eb_on_new_vertex(draco_eb *eb, uint32_t d, int32_t vertex, int32_t corner) {
    draco_eb_dec *dec = &eb->dec[d];
    if (vertex < 0 || (uint32_t)vertex >= eb->visit_cap || dec->value_count >= eb->corner_cap)
        return 0;
    dec->value_to_corner[dec->value_count] = corner;
    dec->vertex_to_value[vertex] = (int32_t)dec->value_count;
    dec->value_count++;
    return 1;
}

/// @brief Depth-first traversal from one seed corner (spec EdgeBreakerTraverser /
///        EdgeBreakerAttributeTraverser ProcessCorner). Vertex-mapped decoders use
///        the position corner table with no seam gating, matching the reference
///        decoder (is_connectivity_used = false); corner-mapped decoders gate the
///        left/right steps on seam edges.
/// @param eb Edgebreaker state containing connectivity and traversal scratch.
/// @param d Attribute decoder index whose value order is generated.
/// @param corner_id Seed corner for the connected traversal component.
/// @return Non-zero when the component is traversed without invalid topology or iteration excess.
static int draco_eb_traverse_dfs(draco_eb *eb, uint32_t d, int32_t corner_id) {
    int corner_mapped = draco_eb_dec_is_corner_mapped(eb, d);
    uint32_t a = corner_mapped ? eb->dec[d].data_id : 0u;
    int32_t face = corner_id / 3;
    int32_t v, next_vert, prev_vert;
    uint64_t guard = 0;
    if (corner_id < 0 || (uint32_t)face >= eb->face_count || eb->face_visited[face])
        return 1;
    eb->trav_size = 0;
    eb->trav_stack[eb->trav_size++] = corner_id;
    draco_eb_corner_to_verts(eb, d, corner_id, &v, &next_vert, &prev_vert);
    if (next_vert < 0 || prev_vert < 0 || (uint32_t)next_vert >= eb->visit_cap ||
        (uint32_t)prev_vert >= eb->visit_cap)
        return 0;
    if (!eb->vertex_visited[next_vert]) {
        eb->vertex_visited[next_vert] = 1;
        if (!draco_eb_on_new_vertex(eb, d, next_vert, draco_eb_next(corner_id)))
            return 0;
    }
    if (!eb->vertex_visited[prev_vert]) {
        eb->vertex_visited[prev_vert] = 1;
        if (!draco_eb_on_new_vertex(eb, d, prev_vert, draco_eb_prev(corner_id)))
            return 0;
    }
    while (eb->trav_size > 0) {
        int32_t face_id;
        corner_id = eb->trav_stack[eb->trav_size - 1];
        face_id = corner_id < 0 ? -1 : corner_id / 3;
        if (corner_id < 0 || face_id >= (int32_t)eb->face_count || eb->face_visited[face_id]) {
            eb->trav_size--;
            continue;
        }
        while (1) {
            int32_t vert_id, right_corner, left_corner, right_face, left_face;
            int right_visited, left_visited;
            if (guard++ > (uint64_t)eb->corner_cap * 4u + 64u)
                return 0;
            face_id = corner_id / 3;
            eb->face_visited[face_id] = 1;
            vert_id = draco_eb_corner_to_vert(eb, d, corner_id);
            if (vert_id < 0 || (uint32_t)vert_id >= eb->visit_cap)
                return 0;
            if (!eb->vertex_visited[vert_id]) {
                int on_boundary;
                if (corner_mapped) {
                    int on_seam = draco_eb_attr_boundary(eb, a, vert_id);
                    on_boundary =
                        on_seam
                            ? 1
                            : draco_eb_pos_boundary(eb, draco_eb_corner_to_vert(eb, 0, corner_id));
                } else {
                    on_boundary = draco_eb_pos_boundary(eb, vert_id);
                }
                eb->vertex_visited[vert_id] = 1;
                if (!draco_eb_on_new_vertex(eb, d, vert_id, corner_id))
                    return 0;
                if (!on_boundary) {
                    corner_id = draco_eb_pos_opposite(eb, draco_eb_next(corner_id));
                    if (corner_id < 0)
                        return 0;
                    continue;
                }
            }
            if (corner_mapped) {
                int32_t next_c = draco_eb_next(corner_id);
                int32_t prev_c = draco_eb_prev(corner_id);
                right_corner = (next_c >= 0 && eb->att_data[a].edge_on_seam[next_c])
                                   ? -1
                                   : draco_eb_pos_opposite(eb, next_c);
                left_corner = (prev_c >= 0 && eb->att_data[a].edge_on_seam[prev_c])
                                  ? -1
                                  : draco_eb_pos_opposite(eb, prev_c);
            } else {
                right_corner = draco_eb_pos_opposite(eb, draco_eb_next(corner_id));
                left_corner = draco_eb_pos_opposite(eb, draco_eb_prev(corner_id));
            }
            right_face = right_corner < 0 ? -1 : right_corner / 3;
            left_face = left_corner < 0 ? -1 : left_corner / 3;
            right_visited = right_face < 0 || right_face >= (int32_t)eb->face_count ||
                            eb->face_visited[right_face];
            left_visited = left_face < 0 || left_face >= (int32_t)eb->face_count ||
                           eb->face_visited[left_face];
            if (right_visited) {
                if (left_visited) {
                    eb->trav_size--;
                    break;
                }
                corner_id = left_corner;
            } else {
                if (left_visited) {
                    corner_id = right_corner;
                } else {
                    if (eb->trav_size >= eb->corner_cap + 1u)
                        return 0;
                    eb->trav_stack[eb->trav_size - 1] = left_corner;
                    eb->trav_stack[eb->trav_size++] = right_corner;
                    break;
                }
            }
        }
    }
    return 1;
}

/* --- Prediction-degree traversal (spec MESH_TRAVERSAL_PREDICTION_DEGREE) --- */

/// @brief Push a corner onto one prediction-degree priority stack.
/// @param eb Edgebreaker traversal state.
/// @param ci Corner index to enqueue.
/// @param priority Stack index from zero through two.
/// @return Non-zero when the selected stack had capacity.
static int draco_eb_pd_push(draco_eb *eb, int32_t ci, int priority) {
    if (eb->pd_size[priority] >= eb->corner_cap + 1u)
        return 0;
    eb->pd_stack[priority][eb->pd_size[priority]++] = ci;
    if (priority < eb->pd_best)
        eb->pd_best = priority;
    return 1;
}

/// @brief Update a candidate tip's prediction degree and choose its traversal priority.
/// @param eb Edgebreaker traversal state.
/// @param d Attribute decoder index controlling corner-to-vertex mapping.
/// @param ci Candidate corner.
/// @return Priority bucket from zero through two.
static int draco_eb_pd_priority(draco_eb *eb, uint32_t d, int32_t ci) {
    int32_t v, n, p;
    int priority = 0;
    draco_eb_corner_to_verts(eb, d, ci, &v, &n, &p);
    if (v >= 0 && (uint32_t)v < eb->visit_cap && !eb->vertex_visited[v]) {
        uint32_t degree = ++eb->pred_degree[v];
        priority = degree > 1u ? 1 : 2;
    }
    if (priority >= 3)
        priority = 2;
    return priority;
}

/// @brief Pop a corner from the highest-priority non-empty prediction-degree stack.
/// @param eb Edgebreaker traversal state.
/// @return Corner index, or -1 when every priority stack is empty.
static int32_t draco_eb_pd_pop(draco_eb *eb) {
    for (int i = eb->pd_best; i < 3; i++) {
        if (eb->pd_size[i] > 0) {
            eb->pd_best = i;
            return eb->pd_stack[i][--eb->pd_size[i]];
        }
    }
    return -1;
}

/// @brief spec PredictionDegree_TraverseFromCorner.
/// @param eb Edgebreaker state containing connectivity and priority traversal scratch.
/// @param d Attribute decoder index whose value order is generated.
/// @param corner_id Seed corner for the connected traversal component.
/// @return Non-zero when the traversal completes within all topology and iteration bounds.
static int draco_eb_traverse_pd(draco_eb *eb, uint32_t d, int32_t corner_id) {
    int32_t tip, next_vert, prev_vert;
    uint64_t guard = 0;
    if (corner_id < 0 || (uint32_t)(corner_id / 3) >= eb->face_count)
        return 0;
    if (!draco_eb_pd_push(eb, corner_id, 0))
        return 0;
    eb->pd_best = 0;
    draco_eb_corner_to_verts(eb, d, corner_id, &tip, &next_vert, &prev_vert);
    if (tip < 0 || next_vert < 0 || prev_vert < 0 || (uint32_t)tip >= eb->visit_cap ||
        (uint32_t)next_vert >= eb->visit_cap || (uint32_t)prev_vert >= eb->visit_cap)
        return 0;
    if (!eb->vertex_visited[next_vert]) {
        eb->vertex_visited[next_vert] = 1;
        if (!draco_eb_on_new_vertex(eb, d, next_vert, draco_eb_next(corner_id)))
            return 0;
    }
    if (!eb->vertex_visited[prev_vert]) {
        eb->vertex_visited[prev_vert] = 1;
        if (!draco_eb_on_new_vertex(eb, d, prev_vert, draco_eb_prev(corner_id)))
            return 0;
    }
    if (!eb->vertex_visited[tip]) {
        eb->vertex_visited[tip] = 1;
        if (!draco_eb_on_new_vertex(eb, d, tip, corner_id))
            return 0;
    }
    while ((corner_id = draco_eb_pd_pop(eb)) >= 0) {
        int32_t face_id = corner_id / 3;
        if ((uint32_t)face_id >= eb->face_count || eb->face_visited[face_id])
            continue;
        while (1) {
            int32_t vert_id, right_c, left_c, right_f, left_f;
            int right_vis, left_vis;
            if (guard++ > (uint64_t)eb->corner_cap * 8u + 64u)
                return 0;
            face_id = corner_id / 3;
            eb->face_visited[face_id] = 1;
            vert_id = draco_eb_corner_to_vert(eb, d, corner_id);
            if (vert_id < 0 || (uint32_t)vert_id >= eb->visit_cap)
                return 0;
            if (!eb->vertex_visited[vert_id]) {
                eb->vertex_visited[vert_id] = 1;
                if (!draco_eb_on_new_vertex(eb, d, vert_id, corner_id))
                    return 0;
            }
            right_c = draco_eb_pos_opposite(eb, draco_eb_next(corner_id));
            left_c = draco_eb_pos_opposite(eb, draco_eb_prev(corner_id));
            right_f = right_c < 0 ? -1 : right_c / 3;
            left_f = left_c < 0 ? -1 : left_c / 3;
            right_vis =
                right_f < 0 || right_f >= (int32_t)eb->face_count || eb->face_visited[right_f];
            left_vis = left_f < 0 || left_f >= (int32_t)eb->face_count || eb->face_visited[left_f];
            if (!left_vis) {
                int priority = draco_eb_pd_priority(eb, d, left_c);
                if (right_vis && priority <= eb->pd_best) {
                    corner_id = left_c;
                    continue;
                }
                if (!draco_eb_pd_push(eb, left_c, priority))
                    return 0;
            }
            if (!right_vis) {
                int priority = draco_eb_pd_priority(eb, d, right_c);
                if (priority <= eb->pd_best) {
                    corner_id = right_c;
                    continue;
                }
                if (!draco_eb_pd_push(eb, right_c, priority))
                    return 0;
            }
            break;
        }
    }
    return 1;
}

/// @brief spec UpdatePointToAttributeIndexMapping — point -> encoded value index.
/// @param eb Edgebreaker state containing point, vertex, and value mappings.
/// @param d Attribute decoder index whose final point map is generated.
/// @return Non-zero when every output point resolves to a decoded value.
static int draco_eb_update_indices_map(draco_eb *eb, uint32_t d) {
    draco_eb_dec *dec = &eb->dec[d];
    for (uint32_t p = 0; p < eb->point_count; p++)
        dec->indices_map[p] = -1;
    for (uint32_t corner = 0; corner < eb->face_count * 3u; corner++) {
        int32_t point = eb->corner_to_point[corner];
        int32_t vert = draco_eb_corner_to_vert(eb, d, (int32_t)corner);
        int32_t entry;
        if (point < 0 || (uint32_t)point >= eb->point_count || vert < 0 ||
            (uint32_t)vert >= eb->visit_cap)
            return 0;
        entry = dec->vertex_to_value[vert];
        if (entry < 0)
            return 0;
        dec->indices_map[point] = entry;
    }
    for (uint32_t p = 0; p < eb->point_count; p++) {
        if (dec->indices_map[p] < 0)
            return 0;
    }
    return 1;
}

/* --- Mesh prediction schemes (edgebreaker) --- */

/// @brief spec ComputeParallelogramPrediction. Returns 1 when all three opposite
///        entries are already decoded; predictions accumulate in int64.
/// @param eb Edgebreaker state providing decoder-specific connectivity.
/// @param d Attribute decoder index.
/// @param data_entry_id Entry currently being predicted; dependencies must precede it.
/// @param ci Representative corner for the current entry.
/// @param data Previously reconstructed portable values.
/// @param ncomp Number of components per value.
/// @param out_pred Receives the parallelogram prediction tuple.
/// @return Non-zero when all three neighboring values are available and earlier than the target.
static int draco_eb_parallelogram_pred(const draco_eb *eb,
                                       uint32_t d,
                                       int32_t data_entry_id,
                                       int32_t ci,
                                       const int32_t *data,
                                       int ncomp,
                                       int64_t *out_pred) {
    const draco_eb_dec *dec = &eb->dec[d];
    int32_t oci = draco_eb_opposite(eb, d, ci);
    int32_t vo, vn, vp;
    int32_t eo, en, ep;
    if (oci < 0)
        return 0;
    draco_eb_corner_to_verts(eb, d, oci, &vo, &vn, &vp);
    if (vo < 0 || vn < 0 || vp < 0 || (uint32_t)vo >= eb->visit_cap ||
        (uint32_t)vn >= eb->visit_cap || (uint32_t)vp >= eb->visit_cap)
        return 0;
    eo = dec->vertex_to_value[vo];
    en = dec->vertex_to_value[vn];
    ep = dec->vertex_to_value[vp];
    if (eo < 0 || en < 0 || ep < 0 || eo >= data_entry_id || en >= data_entry_id ||
        ep >= data_entry_id)
        return 0;
    for (int c = 0; c < ncomp; c++) {
        out_pred[c] = (int64_t)data[(size_t)en * (size_t)ncomp + c] +
                      (int64_t)data[(size_t)ep * (size_t)ncomp + c] -
                      (int64_t)data[(size_t)eo * (size_t)ncomp + c];
    }
    return 1;
}

/// @brief spec MeshPredictionSchemeParallelogramDecoder_ComputeOriginalValues.
/// @param eb Edgebreaker state containing traversal order and connectivity.
/// @param d Attribute decoder index.
/// @param att Attribute whose correction values are reconstructed in place.
/// @param ncomp Number of portable components per value.
/// @return Non-zero after reconstructing all values.
static int draco_eb_parallelogram_compute(draco_eb *eb,
                                          uint32_t d,
                                          draco_attribute *att,
                                          int ncomp) {
    int64_t pred[DRACO_MAX_COMPONENTS] = {0, 0, 0, 0};
    uint32_t num_values = att->num_values;
    if (num_values == 0)
        return 1;
    draco_transform_compute_original(att, pred, att->values, att->values, ncomp);
    for (uint32_t p = 1; p < num_values; p++) {
        int32_t corner_id = eb->dec[d].value_to_corner[p];
        int32_t *dst = att->values + (size_t)p * (size_t)ncomp;
        if (!draco_eb_parallelogram_pred(eb, d, (int32_t)p, corner_id, att->values, ncomp, pred)) {
            const int32_t *src = att->values + (size_t)(p - 1) * (size_t)ncomp;
            for (int c = 0; c < ncomp; c++)
                pred[c] = src[c];
        }
        draco_transform_compute_original(att, pred, dst, dst, ncomp);
    }
    return 1;
}

/// @brief spec MeshPredictionSchemeConstrainedMultiParallelogramDecoder.
/// @param eb Edgebreaker state containing traversal order and connectivity.
/// @param d Attribute decoder index.
/// @param att Attribute containing corrections and per-context crease decisions.
/// @param ncomp Number of portable components per value.
/// @return Non-zero when every value and required crease decision is reconstructed.
static int draco_eb_multi_parallelogram_compute(draco_eb *eb,
                                                uint32_t d,
                                                draco_attribute *att,
                                                int ncomp) {
    int64_t pred[DRACO_EB_MAX_PARALLELOGRAMS][DRACO_MAX_COMPONENTS];
    int64_t multi[DRACO_MAX_COMPONENTS];
    uint32_t num_values = att->num_values;
    if (num_values == 0)
        return 1;
    for (int c = 0; c < DRACO_MAX_COMPONENTS; c++)
        multi[c] = 0;
    draco_transform_compute_original(att, multi, att->values, att->values, ncomp);
    for (uint32_t p = 1; p < num_values; p++) {
        int32_t start_corner = eb->dec[d].value_to_corner[p];
        int32_t corner_id = start_corner;
        int num_para = 0;
        int first_pass = 1;
        int num_used = 0;
        uint64_t guard = 0;
        int32_t *dst = att->values + (size_t)p * (size_t)ncomp;
        while (corner_id >= 0) {
            if (guard++ > (uint64_t)eb->corner_cap * 2u + 64u)
                return 0;
            if (draco_eb_parallelogram_pred(
                    eb, d, (int32_t)p, corner_id, att->values, ncomp, pred[num_para])) {
                if (++num_para == DRACO_EB_MAX_PARALLELOGRAMS)
                    break;
            }
            corner_id = first_pass ? draco_eb_swing_left(eb, d, corner_id)
                                   : draco_eb_swing_right(eb, d, corner_id);
            if (corner_id == start_corner)
                break;
            if (corner_id < 0 && first_pass) {
                first_pass = 0;
                corner_id = draco_eb_swing_right(eb, d, start_corner);
            }
        }
        for (int c = 0; c < ncomp; c++)
            multi[c] = 0;
        if (num_para > 0) {
            int context = num_para - 1;
            for (int i = 0; i < num_para; i++) {
                uint8_t is_crease;
                if (att->crease_pos[context] >= att->crease_count[context])
                    return 0;
                is_crease = att->crease_bits[context][att->crease_pos[context]++];
                if (!is_crease) {
                    num_used++;
                    for (int c = 0; c < ncomp; c++)
                        multi[c] += pred[i][c];
                }
            }
        }
        if (num_used == 0) {
            const int32_t *src = att->values + (size_t)(p - 1) * (size_t)ncomp;
            for (int c = 0; c < ncomp; c++)
                multi[c] = src[c];
        } else {
            for (int c = 0; c < ncomp; c++)
                multi[c] /= num_used;
        }
        draco_transform_compute_original(att, multi, dst, dst, ncomp);
    }
    return 1;
}

/// @brief spec IntSqrt — integer Newton iteration.
/// @param number Unsigned value whose floor square root is requested.
/// @return Greatest integer whose square does not exceed @p number.
static uint64_t draco_int_sqrt(uint64_t number) {
    uint64_t act_number = number;
    uint64_t square_root = 1;
    if (number == 0)
        return 0;
    while (act_number >= 2) {
        square_root *= 2;
        act_number /= 4;
    }
    do {
        square_root = (square_root + number / square_root) / 2;
    } while (square_root * square_root > number);
    return square_root;
}

/// @brief spec GetPositionForEntryId — fetch the portable (quantized) position for
///        an encoded value entry of decoder @p d via the point mapping.
/// @param eb Edgebreaker state containing decoder and point mappings.
/// @param mesh Decoded mesh whose first attribute supplies portable positions.
/// @param d Attribute decoder index owning @p entry_id.
/// @param entry_id Encoded attribute-value index.
/// @param pos Receives the three-component quantized position.
/// @return Non-zero when the entry resolves through a valid corner and point.
static int draco_eb_position_for_entry(
    const draco_eb *eb, const draco_mesh *mesh, uint32_t d, int32_t entry_id, int64_t *pos) {
    const draco_eb_dec *dec = &eb->dec[d];
    const draco_attribute *pos_att = &mesh->attributes[0];
    int32_t corner, point, mapped;
    if (entry_id < 0 || (uint32_t)entry_id >= dec->value_count)
        return 0;
    corner = dec->value_to_corner[entry_id];
    if (corner < 0 || (uint32_t)corner >= eb->face_count * 3u)
        return 0;
    point = eb->corner_to_point[corner];
    if (point < 0 || (uint32_t)point >= eb->point_count)
        return 0;
    mapped = eb->dec[0].indices_map[point];
    if (mapped < 0 || (uint32_t)mapped >= eb->dec[0].value_count)
        return 0;
    if (pos_att->att_type != 0 || !pos_att->values || pos_att->components < 3)
        return 0;
    for (int i = 0; i < 3; i++)
        pos[i] = pos_att->values[(size_t)mapped * pos_att->components + i];
    return 1;
}

/// @brief spec GetPositionForCorner.
/// @param eb Edgebreaker state containing point and decoder mappings.
/// @param mesh Decoded mesh whose first attribute supplies portable positions.
/// @param d Attribute decoder used to interpret @p corner.
/// @param corner Corner whose associated position is requested.
/// @param pos Receives the three-component quantized position.
/// @return Non-zero when the corner and its encoded value mapping are valid.
static int draco_eb_position_for_corner(
    const draco_eb *eb, const draco_mesh *mesh, uint32_t d, int32_t corner, int64_t *pos) {
    int32_t v = draco_eb_corner_to_vert(eb, d, corner);
    if (v < 0 || (uint32_t)v >= eb->visit_cap)
        return 0;
    return draco_eb_position_for_entry(eb, mesh, d, eb->dec[d].vertex_to_value[v], pos);
}

/// @brief spec MeshPredictionSchemeTexCoordsPortableDecoder_ComputeOriginalValues.
/// @param eb Edgebreaker state containing traversal and connectivity mappings.
/// @param mesh Decoded mesh containing the portable position attribute.
/// @param d Texture-coordinate decoder index.
/// @param att Two-component texture-coordinate attribute reconstructed in place.
/// @return Non-zero when all values and orientation decisions are reconstructed.
static int draco_eb_texcoords_compute(draco_eb *eb,
                                      const draco_mesh *mesh,
                                      uint32_t d,
                                      draco_attribute *att) {
    const draco_eb_dec *dec = &eb->dec[d];
    uint32_t num_values = att->num_values;
    for (uint32_t p = 0; p < num_values; p++) {
        int32_t ci = dec->value_to_corner[p];
        int64_t pred[2] = {0, 0};
        int32_t v, nv, pv;
        int32_t next_id = -1;
        int32_t prev_id = -1;
        int done = 0;
        int32_t *dst = att->values + (size_t)p * 2u;
        draco_eb_corner_to_verts(eb, d, ci, &v, &nv, &pv);
        if (nv >= 0 && (uint32_t)nv < eb->visit_cap)
            next_id = dec->vertex_to_value[nv];
        if (pv >= 0 && (uint32_t)pv < eb->visit_cap)
            prev_id = dec->vertex_to_value[pv];
        if (prev_id >= 0 && next_id >= 0 && prev_id < (int32_t)p && next_id < (int32_t)p) {
            int64_t n_uv[2];
            int64_t p_uv[2];
            n_uv[0] = att->values[(size_t)next_id * 2u];
            n_uv[1] = att->values[(size_t)next_id * 2u + 1u];
            p_uv[0] = att->values[(size_t)prev_id * 2u];
            p_uv[1] = att->values[(size_t)prev_id * 2u + 1u];
            if (p_uv[0] == n_uv[0] && p_uv[1] == n_uv[1]) {
                pred[0] = p_uv[0];
                pred[1] = p_uv[1];
                done = 1;
            } else {
                int64_t tip_pos[3], next_pos[3], prev_pos[3];
                if (!draco_eb_position_for_entry(eb, mesh, d, (int32_t)p, tip_pos) ||
                    !draco_eb_position_for_entry(eb, mesh, d, next_id, next_pos) ||
                    !draco_eb_position_for_entry(eb, mesh, d, prev_id, prev_pos))
                    return 0;
                {
                    int64_t pn[3], cn[3];
                    int64_t pn_norm2 = 0;
                    int64_t cn_dot_pn = 0;
                    for (int i = 0; i < 3; i++) {
                        if (!draco_i64_sub(prev_pos[i], next_pos[i], &pn[i]) ||
                            !draco_i64_sub(tip_pos[i], next_pos[i], &cn[i]) ||
                            !draco_i64_accumulate_product(&pn_norm2, pn[i], pn[i]) ||
                            !draco_i64_accumulate_product(&cn_dot_pn, cn[i], pn[i]))
                            return 0;
                    }
                    if (pn_norm2 != 0) {
                        int64_t pn_uv[2];
                        int64_t x_uv[2];
                        int64_t x_pos[3];
                        int64_t cx_norm2 = 0;
                        int64_t cx_uv[2];
                        uint64_t norm_squared;
                        uint8_t orientation;
                        if (!draco_i64_sub(p_uv[0], n_uv[0], &pn_uv[0]) ||
                            !draco_i64_sub(p_uv[1], n_uv[1], &pn_uv[1]))
                            return 0;
                        for (int i = 0; i < 2; i++) {
                            int64_t lhs;
                            int64_t rhs;
                            if (!draco_i64_mul(pn_uv[i], cn_dot_pn, &lhs) ||
                                !draco_i64_mul(n_uv[i], pn_norm2, &rhs) ||
                                !draco_i64_add(lhs, rhs, &x_uv[i]))
                                return 0;
                        }
                        for (int i = 0; i < 3; i++) {
                            int64_t product;
                            int64_t sub;
                            if (!draco_i64_mul(pn[i], cn_dot_pn, &product) ||
                                !draco_i64_add(next_pos[i], product / pn_norm2, &x_pos[i]) ||
                                !draco_i64_sub(tip_pos[i], x_pos[i], &sub) ||
                                !draco_i64_accumulate_product(&cx_norm2, sub, sub))
                                return 0;
                        }
                        {
                            uint64_t norm_product;
                            int64_t cx_uv_1;
                            if (cx_norm2 < 0 || pn_norm2 < 0 ||
                                !draco_u64_mul(
                                    (uint64_t)cx_norm2, (uint64_t)pn_norm2, &norm_product))
                                return 0;
                            norm_squared = draco_int_sqrt(norm_product);
                            if (!draco_i64_mul(pn_uv[1], (int64_t)norm_squared, &cx_uv[0]) ||
                                !draco_i64_mul(pn_uv[0], (int64_t)norm_squared, &cx_uv_1) ||
                                !draco_i64_sub(0, cx_uv_1, &cx_uv[1]))
                                return 0;
                        }
                        if (att->tex_orient_pos == 0)
                            return 0;
                        orientation = att->tex_orient[--att->tex_orient_pos];
                        if (orientation) {
                            if (!draco_i64_add(x_uv[0], cx_uv[0], &pred[0]) ||
                                !draco_i64_add(x_uv[1], cx_uv[1], &pred[1]))
                                return 0;
                        } else {
                            if (!draco_i64_sub(x_uv[0], cx_uv[0], &pred[0]) ||
                                !draco_i64_sub(x_uv[1], cx_uv[1], &pred[1]))
                                return 0;
                        }
                        pred[0] /= pn_norm2;
                        pred[1] /= pn_norm2;
                        done = 1;
                    }
                }
            }
        }
        if (!done) {
            /* spec fallback: reuse a previously decoded neighbour (or the previous
             * entry), or predict zero for the very first value. */
            int has_prev = prev_id >= 0 && prev_id < (int32_t)p;
            int has_next = next_id >= 0 && next_id < (int32_t)p;
            int32_t data_offset = 0;
            int zero_pred = 0;
            if (has_prev)
                data_offset = prev_id * 2;
            if (has_next) {
                data_offset = next_id * 2;
            } else if (!has_prev) {
                if (p > 0)
                    data_offset = ((int32_t)p - 1) * 2;
                else
                    zero_pred = 1;
            }
            if (!zero_pred) {
                pred[0] = att->values[(size_t)data_offset];
                pred[1] = att->values[(size_t)data_offset + 1u];
            }
        }
        draco_transform_compute_original(att, pred, dst, dst, 2);
    }
    return 1;
}

/// @brief spec MeshPredictionSchemeGeometricNormalDecoder_ComputeOriginalValues —
///        area-weighted one-ring normal, canonicalized to octahedral coordinates.
/// @param eb Edgebreaker state containing traversal and connectivity mappings.
/// @param mesh Decoded mesh containing the portable position attribute.
/// @param d Normal decoder index.
/// @param att Two-component octahedral normal corrections reconstructed in place.
/// @return Non-zero when every geometric prediction and flip decision is valid.
static int draco_eb_geometric_normal_compute(draco_eb *eb,
                                             const draco_mesh *mesh,
                                             uint32_t d,
                                             draco_attribute *att) {
    int32_t max_q;
    int32_t max_value;
    int32_t center;
    if (!draco_normal_quant_params(att->normal_max_q, &max_q, &max_value, &center))
        return 0;
    for (uint32_t data_id = 0; data_id < att->num_values; data_id++) {
        int32_t ci = eb->dec[d].value_to_corner[data_id];
        int64_t normal[3] = {0, 0, 0};
        int64_t s, t;
        int64_t pred_oct[2];
        {
            int64_t pos_cent[3];
            int32_t corner = ci;
            int32_t start = ci;
            int left_traversal = 1;
            uint64_t guard = 0;
            if (!draco_eb_position_for_corner(eb, mesh, d, ci, pos_cent))
                return 0;
            while (corner >= 0) {
                int64_t pos_next[3], pos_prev[3];
                int64_t dn[3], dp[3];
                if (guard++ > (uint64_t)eb->corner_cap * 2u + 64u)
                    return 0;
                if (!draco_eb_position_for_corner(eb, mesh, d, draco_eb_next(corner), pos_next) ||
                    !draco_eb_position_for_corner(eb, mesh, d, draco_eb_prev(corner), pos_prev))
                    return 0;
                for (int i = 0; i < 3; i++) {
                    if (!draco_i64_sub(pos_next[i], pos_cent[i], &dn[i]) ||
                        !draco_i64_sub(pos_prev[i], pos_cent[i], &dp[i]))
                        return 0;
                }
                for (int axis = 0; axis < 3; axis++) {
                    int lhs_axis = (axis + 1) % 3;
                    int rhs_axis = (axis + 2) % 3;
                    int64_t lhs;
                    int64_t rhs;
                    int64_t cross;
                    if (!draco_i64_mul(dn[lhs_axis], dp[rhs_axis], &lhs) ||
                        !draco_i64_mul(dn[rhs_axis], dp[lhs_axis], &rhs) ||
                        !draco_i64_sub(lhs, rhs, &cross) ||
                        !draco_i64_add(normal[axis], cross, &normal[axis]))
                        return 0;
                }
                if (left_traversal) {
                    corner = draco_eb_swing_left(eb, d, corner);
                    if (corner < 0) {
                        corner = draco_eb_swing_right(eb, d, start);
                        left_traversal = 0;
                    } else if (corner == start) {
                        corner = -1;
                    }
                } else {
                    corner = draco_eb_swing_right(eb, d, corner);
                }
            }
        }
        {
            int64_t abs_sum = 0;
            int64_t upper_bound = (int64_t)1 << 29;
            for (int i = 0; i < 3; i++) {
                int64_t absolute;
                if (!draco_i64_abs(normal[i], &absolute) ||
                    !draco_i64_add(abs_sum, absolute, &abs_sum))
                    return 0;
            }
            if (abs_sum > upper_bound) {
                int64_t quotient = abs_sum / upper_bound + (abs_sum % upper_bound != 0 ? 1 : 0);
                for (int i = 0; i < 3; i++)
                    normal[i] /= quotient;
            }
        }
        {
            /* CanonicalizeIntegerVector */
            int64_t abs_sum = 0;
            for (int i = 0; i < 3; i++) {
                int64_t absolute;
                if (!draco_i64_abs(normal[i], &absolute) ||
                    !draco_i64_add(abs_sum, absolute, &abs_sum))
                    return 0;
            }
            if (abs_sum == 0) {
                normal[0] = center;
                normal[1] = 0;
                normal[2] = 0;
            } else {
                int64_t a0, a1;
                if (!draco_i64_mul(normal[0], center, &normal[0]) ||
                    !draco_i64_mul(normal[1], center, &normal[1]))
                    return 0;
                normal[0] /= abs_sum;
                normal[1] /= abs_sum;
                if (!draco_i64_abs(normal[0], &a0) || !draco_i64_abs(normal[1], &a1))
                    return 0;
                if (normal[2] >= 0)
                    normal[2] = center - a0 - a1;
                else
                    normal[2] = -(center - a0 - a1);
            }
        }
        if (att->flip_bits && data_id < att->flip_count && att->flip_bits[data_id]) {
            for (int i = 0; i < 3; i++) {
                if (!draco_i64_sub(0, normal[i], &normal[i]))
                    return 0;
            }
        }
        /* IntegerVectorToQuantizedOctahedralCoords */
        if (normal[0] >= 0) {
            s = normal[1] + center;
            t = normal[2] + center;
        } else {
            int64_t a1;
            int64_t a2;
            if (!draco_i64_abs(normal[1], &a1) || !draco_i64_abs(normal[2], &a2))
                return 0;
            s = (normal[1] < 0) ? a2 : (max_value - a2);
            t = (normal[2] < 0) ? a1 : (max_value - a1);
        }
        /* CanonicalizeOctahedralCoords */
        if ((s == 0 && t == 0) || (s == 0 && t == max_value) || (s == max_value && t == 0)) {
            s = max_value;
            t = max_value;
        } else if (s == 0 && t > center) {
            t = center - (t - center);
        } else if (s == max_value && t < center) {
            t = center + (center - t);
        } else if (t == max_value && s < center) {
            s = center + (center - s);
        } else if (t == 0 && s > center) {
            s = center - (s - center);
        }
        pred_oct[0] = s;
        pred_oct[1] = t;
        {
            int32_t *dst = att->values + (size_t)data_id * 2u;
            draco_normal_oct_compute_original(att, pred_oct, dst, dst);
        }
    }
    return 1;
}

/* --- Prediction side-data parsers (spec DecodePredictionData) --- */

/// @brief spec ParsePredictionRansData — prob_zero byte + sized rABS buffer.
/// @param b Bounded reader positioned at the rABS descriptor.
/// @param out_prob Receives the encoded zero-symbol probability.
/// @param out_buf Receives a borrowed pointer to the sized entropy stream.
/// @param out_size Receives the entropy-stream byte length.
/// @return Non-zero when the complete declared stream lies within the payload.
static int draco_parse_prediction_rans(draco_buf *b,
                                       uint8_t *out_prob,
                                       const uint8_t **out_buf,
                                       size_t *out_size) {
    uint64_t sz;
    if (!b || !out_prob || !out_buf || !out_size || b->pos > b->size)
        return 0;
    *out_prob = draco_read_u8(b);
    sz = draco_read_varint(b);
    if (b->failed || sz > b->size - b->pos)
        return 0;
    *out_buf = b->data + b->pos;
    *out_size = (size_t)sz;
    b->pos += (size_t)sz;
    return 1;
}

/// @brief spec DecodePredictionData — scheme-specific flag streams followed by the
///        transform parameters (geometric normal reads its transform first).
/// @param b Bounded reader positioned at prediction side data.
/// @param att Attribute receiving crease, orientation, flip, and transform parameters.
/// @param num_entries Number of predicted logical values bounding side-data counts.
/// @return Non-zero when all data required by the selected prediction scheme is valid.
static int draco_decode_prediction_data(draco_buf *b, draco_attribute *att, uint32_t num_entries) {
    if (!b || !att)
        return 0;
    if (att->prediction_scheme == DRACO_PREDICTION_CONSTRAINED_MULTI) {
        for (int i = 0; i < DRACO_EB_MAX_PARALLELOGRAMS; i++) {
            uint64_t nflags = draco_read_varint(b);
            if (b->failed || nflags > (uint64_t)num_entries * DRACO_EB_MAX_PARALLELOGRAMS + 64u ||
                nflags > UINT32_MAX || nflags > SIZE_MAX)
                return 0;
            att->crease_count[i] = (uint32_t)nflags;
            att->crease_pos[i] = 0;
            if (nflags > 0) {
                uint8_t prob;
                const uint8_t *buf;
                size_t size;
                draco_rans ans;
                att->crease_bits[i] = (uint8_t *)malloc((size_t)nflags);
                if (!att->crease_bits[i] || !draco_parse_prediction_rans(b, &prob, &buf, &size))
                    return 0;
                if (size == 0 || !draco_rans_init(&ans, buf, size, 4096u))
                    return 0;
                for (uint64_t j = 0; j < nflags; j++) {
                    uint32_t val = 0;
                    if (!draco_rabs_read(&ans, prob, &val))
                        return 0;
                    att->crease_bits[i][j] = (uint8_t)(val > 0);
                }
            }
        }
    } else if (att->prediction_scheme == DRACO_PREDICTION_TEX_COORDS_PORTABLE) {
        uint32_t n = draco_read_u32(b);
        uint8_t prob;
        const uint8_t *buf;
        size_t size;
        if (b->failed || n > num_entries)
            return 0;
        att->tex_orient_pos = n;
        if (!draco_parse_prediction_rans(b, &prob, &buf, &size))
            return 0;
        if (n > 0) {
            draco_rans ans;
            int last_orientation = 1;
            att->tex_orient = (uint8_t *)malloc(n);
            if (!att->tex_orient || size == 0 || !draco_rans_init(&ans, buf, size, 4096u))
                return 0;
            for (uint32_t j = 0; j < n; j++) {
                uint32_t val = 0;
                if (!draco_rabs_read(&ans, prob, &val))
                    return 0;
                if (val == 0)
                    last_orientation = !last_orientation;
                att->tex_orient[j] = (uint8_t)last_orientation;
            }
        }
    } else if (att->prediction_scheme == DRACO_PREDICTION_GEOMETRIC_NORMAL) {
        uint8_t prob;
        const uint8_t *buf;
        size_t size;
        uint32_t normal_max_q = draco_read_u32(b);
        int32_t derived_max_q;
        int32_t derived_max_value;
        int32_t derived_center;
        (void)draco_read_u32(b); /* unused center value */
        if (b->failed || normal_max_q > INT32_MAX ||
            !draco_normal_quant_params(
                (int32_t)normal_max_q, &derived_max_q, &derived_max_value, &derived_center) ||
            !draco_parse_prediction_rans(b, &prob, &buf, &size))
            return 0;
        att->normal_max_q = (int32_t)normal_max_q;
        att->flip_count = num_entries;
        if (num_entries > 0) {
            draco_rans ans;
            att->flip_bits = (uint8_t *)malloc(num_entries);
            if (!att->flip_bits || size == 0 || !draco_rans_init(&ans, buf, size, 4096u))
                return 0;
            for (uint32_t j = 0; j < num_entries; j++) {
                uint32_t val = 0;
                if (!draco_rabs_read(&ans, prob, &val))
                    return 0;
                att->flip_bits[j] = (uint8_t)(val > 0);
            }
        }
    }
    if (att->prediction_scheme != DRACO_PREDICTION_GEOMETRIC_NORMAL) {
        if (att->prediction_transform == DRACO_TRANSFORM_WRAP) {
            att->wrap_min = (int32_t)draco_read_u32(b);
            att->wrap_max = (int32_t)draco_read_u32(b);
            if (att->wrap_min > att->wrap_max)
                return 0;
        } else {
            uint32_t normal_max_q = draco_read_u32(b);
            int32_t derived_max_q;
            int32_t derived_max_value;
            int32_t derived_center;
            (void)draco_read_u32(b); /* unused center value */
            if (b->failed || normal_max_q > INT32_MAX ||
                !draco_normal_quant_params(
                    (int32_t)normal_max_q, &derived_max_q, &derived_max_value, &derived_center))
                return 0;
            att->normal_max_q = (int32_t)normal_max_q;
        }
    }
    return b->failed ? 0 : 1;
}

/// @brief GENERIC attributes store raw little-endian values with no entropy coding
///        (reference SequentialAttributeDecoder::DecodeValues).
/// @param b Bounded reader positioned at raw attribute values.
/// @param att Attribute descriptor receiving final float or integer storage.
/// @return Non-zero when the declared values fit and decode completely.
static int draco_decode_generic_attribute(draco_buf *b, draco_attribute *att) {
    /* Draco data types: 1 INT8, 2 UINT8, 3 INT16, 4 UINT16, 5 INT32, 6 UINT32,
     * 7 INT64, 8 UINT64, 9 FLOAT32, 10 FLOAT64, 11 BOOL. */
    static const uint8_t dt_sizes[12] = {0, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 1};
    uint32_t total;
    uint32_t dt;
    uint32_t esize;
    size_t input_bytes;
    size_t output_bytes;
    if (!b || !att || !draco_attribute_descriptor_valid(att) ||
        !draco_scalar_count(att->num_values, att->components, &total))
        return 0;
    dt = att->data_type;
    if (dt >= 12 || dt_sizes[dt] == 0)
        return 0;
    esize = dt_sizes[dt];
    if (total == 0)
        return 1;
    if (b->pos > b->size || !draco_size_mul((size_t)total, esize, &input_bytes) ||
        input_bytes > b->size - b->pos)
        return 0;
    if (dt == 9) {
        if (!draco_size_mul((size_t)total, sizeof(float), &output_bytes))
            return 0;
        att->out_floats = (float *)calloc(1u, output_bytes);
        if (!att->out_floats)
            return 0;
        for (uint32_t i = 0; i < total; i++) {
            att->out_floats[i] = draco_read_f32(b);
            if (!isfinite(att->out_floats[i]))
                return 0;
        }
    } else if (dt == 10) {
        if (!draco_size_mul((size_t)total, sizeof(float), &output_bytes))
            return 0;
        att->out_floats = (float *)calloc(1u, output_bytes);
        if (!att->out_floats)
            return 0;
        for (uint32_t i = 0; i < total; i++) {
            uint64_t bits = draco_read_u32(b);
            double dv;
            bits |= (uint64_t)draco_read_u32(b) << 32;
            memcpy(&dv, &bits, sizeof(dv));
            if (!isfinite(dv) || dv > FLT_MAX || dv < -FLT_MAX)
                return 0;
            att->out_floats[i] = (float)dv;
        }
    } else {
        if (!draco_size_mul((size_t)total, sizeof(int32_t), &output_bytes))
            return 0;
        att->out_ints = (int32_t *)calloc(1u, output_bytes);
        if (!att->out_ints)
            return 0;
        for (uint32_t i = 0; i < total; i++) {
            switch (dt) {
                case 1:
                    att->out_ints[i] = (int8_t)draco_read_u8(b);
                    break;
                case 2:
                    att->out_ints[i] = draco_read_u8(b);
                    break;
                case 11: {
                    uint8_t value = draco_read_u8(b);
                    if (value > 1u)
                        return 0;
                    att->out_ints[i] = value;
                    break;
                }
                case 3:
                    att->out_ints[i] = (int16_t)draco_read_u16(b);
                    break;
                case 4:
                    att->out_ints[i] = draco_read_u16(b);
                    break;
                case 7:
                case 8: {
                    uint64_t lo = draco_read_u32(b);
                    uint64_t hi = draco_read_u32(b);
                    uint64_t raw = lo | (hi << 32);
                    if (dt == 7) {
                        int64_t signed_raw;
                        memcpy(&signed_raw, &raw, sizeof(signed_raw));
                        if (signed_raw < INT32_MIN || signed_raw > INT32_MAX)
                            return 0;
                        att->out_ints[i] = (int32_t)signed_raw;
                    } else {
                        if (raw > INT32_MAX)
                            return 0;
                        att->out_ints[i] = (int32_t)raw;
                    }
                    break;
                }
                case 6: {
                    uint32_t value = draco_read_u32(b);
                    if (value > INT32_MAX)
                        return 0;
                    att->out_ints[i] = (int32_t)value;
                    break;
                }
                default:
                    att->out_ints[i] = (int32_t)draco_read_u32(b);
                    break;
            }
        }
    }
    return b->failed ? 0 : 1;
}

/// @brief Decode one attribute's portable values (spec DecodePortableAttributes +
///        SequentialIntegerAttributeDecoder_DecodeIntegerValues). @p eb is NULL for
///        sequential meshes, where only NONE/DIFFERENCE prediction is legal.
/// @param b Bounded reader positioned at the portable attribute stream.
/// @param mesh Mesh containing position data needed by geometric predictors.
/// @param att Attribute descriptor and destination value storage.
/// @param eb Optional edgebreaker traversal state for mesh prediction schemes.
/// @param dec_idx Attribute decoder index within @p eb.
/// @return Non-zero when entropy decoding, prediction inversion, and side data all succeed.
static int draco_decode_portable_attribute(
    draco_buf *b, draco_mesh *mesh, draco_attribute *att, draco_eb *eb, uint32_t dec_idx) {
    int eff_comps;
    uint32_t num_values;
    size_t value_bytes;
    uint32_t *symbols;
    uint8_t compressed;
    uint8_t prediction_scheme;
    if (!b || !mesh || !att || att->decoder_type > DRACO_SEQ_NORMALS || att->components == 0 ||
        att->components > DRACO_MAX_COMPONENTS || (eb && dec_idx >= eb->num_decoders))
        return 0;
    eff_comps = (att->decoder_type == DRACO_SEQ_NORMALS) ? 2 : att->components;
    if (!draco_scalar_count(att->num_values, (uint32_t)eff_comps, &num_values) ||
        !draco_size_mul((size_t)num_values, sizeof(uint32_t), &value_bytes))
        return 0;
    if (att->decoder_type == DRACO_SEQ_GENERIC)
        return draco_decode_generic_attribute(b, att);
    prediction_scheme = draco_read_u8(b);
    if (prediction_scheme == (uint8_t)DRACO_PREDICTION_NONE)
        att->prediction_scheme = DRACO_PREDICTION_NONE;
    else if (prediction_scheme <= INT8_MAX)
        att->prediction_scheme = (int8_t)prediction_scheme;
    else
        return 0;
    if (b->failed)
        return 0;
    if (att->prediction_scheme != DRACO_PREDICTION_NONE) {
        att->prediction_transform = (int8_t)draco_read_u8(b);
        if (b->failed)
            return 0;
        if (att->prediction_transform != DRACO_TRANSFORM_WRAP &&
            att->prediction_transform != DRACO_TRANSFORM_NORMAL_OCT_CANON)
            return 0;
        if (att->prediction_transform == DRACO_TRANSFORM_NORMAL_OCT_CANON &&
            (att->decoder_type != DRACO_SEQ_NORMALS || eff_comps != 2))
            return 0;
        switch (att->prediction_scheme) {
            case DRACO_PREDICTION_DIFFERENCE:
                break;
            case DRACO_PREDICTION_PARALLELOGRAM:
            case DRACO_PREDICTION_CONSTRAINED_MULTI:
            case DRACO_PREDICTION_TEX_COORDS_PORTABLE:
            case DRACO_PREDICTION_GEOMETRIC_NORMAL:
                if (!eb)
                    return 0; /* mesh schemes need edgebreaker traversal state */
                if (att->prediction_scheme == DRACO_PREDICTION_GEOMETRIC_NORMAL &&
                    att->decoder_type != DRACO_SEQ_NORMALS)
                    return 0;
                break;
            default:
                return 0;
        }
    }
    compressed = draco_read_u8(b);
    if (b->failed || compressed == 0)
        return 0; /* raw (uncompressed) integer storage is not emitted by mesh encoders */
    if (num_values == 0)
        return 1;
    symbols = (uint32_t *)calloc(1u, value_bytes);
    att->values = (int32_t *)calloc(1u, value_bytes);
    if (!symbols || !att->values) {
        free(symbols);
        return 0;
    }
    if (!draco_decode_symbols(b, num_values, (uint32_t)eff_comps, symbols)) {
        free(symbols);
        return 0;
    }
    if (att->prediction_scheme != DRACO_PREDICTION_NONE &&
        att->prediction_transform == DRACO_TRANSFORM_NORMAL_OCT_CANON) {
        for (uint32_t i = 0; i < num_values; i++)
            att->values[i] = (int32_t)symbols[i];
    } else {
        for (uint32_t i = 0; i < num_values; i++) {
            att->values[i] = draco_zigzag_decode_u32(symbols[i]);
        }
    }
    free(symbols);
    if (att->prediction_scheme == DRACO_PREDICTION_NONE)
        return 1;
    if (!draco_decode_prediction_data(b, att, att->num_values))
        return 0;
    switch (att->prediction_scheme) {
        case DRACO_PREDICTION_DIFFERENCE:
            return draco_difference_compute_original(att, att->num_values, eff_comps);
        case DRACO_PREDICTION_PARALLELOGRAM:
            return draco_eb_parallelogram_compute(eb, dec_idx, att, eff_comps);
        case DRACO_PREDICTION_CONSTRAINED_MULTI:
            return draco_eb_multi_parallelogram_compute(eb, dec_idx, att, eff_comps);
        case DRACO_PREDICTION_TEX_COORDS_PORTABLE:
            if (eff_comps != 2)
                return 0;
            return draco_eb_texcoords_compute(eb, mesh, dec_idx, att);
        case DRACO_PREDICTION_GEOMETRIC_NORMAL:
            if (eff_comps != 2)
                return 0;
            return draco_eb_geometric_normal_compute(eb, mesh, dec_idx, att);
        default:
            return 0;
    }
}

/// @brief Dequantize one attribute (spec SequentialQuantizationAttributeDecoder).
/// @param att Quantized attribute whose integer values and transform data are read.
/// @return Non-zero after allocating and filling the final floating-point array.
static int draco_dequantize_attribute(draco_attribute *att) {
    uint32_t max_quantized;
    uint32_t scalar_count;
    size_t output_bytes;
    float factor;
    if (!att || att->components == 0 || att->components > DRACO_MAX_COMPONENTS ||
        att->quantization_bits == 0 || att->quantization_bits > 30 ||
        !draco_scalar_count(att->num_values, att->components, &scalar_count) ||
        !draco_size_mul((size_t)scalar_count, sizeof(float), &output_bytes))
        return 0;
    if (scalar_count == 0)
        return 1;
    if (!att->values || !isfinite(att->quant_range) || att->quant_range < 0.0f)
        return 0;
    for (int c = 0; c < att->components; c++) {
        if (!isfinite(att->quant_min[c]))
            return 0;
    }
    max_quantized = (1u << att->quantization_bits) - 1u;
    for (uint32_t i = 0; i < scalar_count; i++) {
        if (att->values[i] < 0 || (uint32_t)att->values[i] > max_quantized)
            return 0;
    }
    factor = 1.0f / (float)max_quantized;
    att->out_floats = (float *)calloc(1u, output_bytes);
    if (!att->out_floats)
        return 0;
    for (uint32_t i = 0; i < att->num_values; i++) {
        for (int c = 0; c < att->components; c++) {
            int32_t raw = att->values[(size_t)i * att->components + c];
            float norm = (float)raw * factor;
            double output = (double)norm * att->quant_range + (double)att->quant_min[c];
            if (!isfinite(output) || output > FLT_MAX || output < -FLT_MAX) {
                free(att->out_floats);
                att->out_floats = NULL;
                return 0;
            }
            att->out_floats[(size_t)i * att->components + c] = (float)output;
        }
    }
    return 1;
}

/// @brief Convert octahedral normal coordinates to unit vectors (spec
///        OctaherdalCoordsToUnitVector and callers).
/// @details Falls back to the decoder-level quantization bits when the prediction
///          carried no octahedral transform data (PREDICTION_NONE normals).
/// @param att Normal attribute containing portable octahedral coordinates.
/// @return Non-zero after allocating and filling three-component unit-vector output.
static int draco_decode_normals_attribute(draco_attribute *att) {
    int32_t max_q_src;
    int32_t max_q;
    int32_t max_value;
    int32_t center;
    uint32_t input_count;
    size_t output_bytes;
    float scale;
    if (!att || att->components != 3 ||
        (att->normal_max_q <= 0 && (att->quantization_bits == 0 || att->quantization_bits > 30)))
        return 0;
    max_q_src =
        att->normal_max_q > 0 ? att->normal_max_q : (int32_t)((1u << att->quantization_bits) - 1u);
    if (!draco_normal_quant_params(max_q_src, &max_q, &max_value, &center) ||
        !draco_scalar_count(att->num_values, 2u, &input_count) ||
        !draco_tuple_bytes(att->num_values, 3u, sizeof(float), &output_bytes))
        return 0;
    (void)max_q;
    (void)center;
    if (input_count == 0)
        return 1;
    if (!att->values)
        return 0;
    for (uint32_t i = 0; i < input_count; i++) {
        if (att->values[i] < 0 || att->values[i] > max_value)
            return 0;
    }
    scale = 1.0f / (float)max_value;
    att->out_floats = (float *)calloc(1u, output_bytes);
    if (!att->out_floats)
        return 0;
    for (uint32_t i = 0; i < att->num_values; i++) {
        float in_s = (float)att->values[(size_t)i * 2u] * scale;
        float in_t = (float)att->values[(size_t)i * 2u + 1u] * scale;
        float s = in_s;
        float t = in_t;
        float spt = s + t;
        float smt = s - t;
        float x_sign = 1.0f;
        float x;
        float y;
        float z;
        float norm_squared;
        if (spt >= 0.5f && spt <= 1.5f && smt >= -0.5f && smt <= 0.5f) {
            /* right hemisphere */
        } else {
            x_sign = -1.0f;
            if (spt <= 0.5f) {
                s = 0.5f - in_t;
                t = 0.5f - in_s;
            } else if (spt >= 1.5f) {
                s = 1.5f - in_t;
                t = 1.5f - in_s;
            } else if (smt <= -0.5f) {
                s = in_t - 0.5f;
                t = in_s + 0.5f;
            } else {
                s = in_t + 0.5f;
                t = in_s - 0.5f;
            }
            spt = s + t;
            smt = s - t;
        }
        y = 2.0f * s - 1.0f;
        z = 2.0f * t - 1.0f;
        {
            float a = 2.0f * spt - 1.0f;
            float bb = 3.0f - 2.0f * spt;
            float cc = 2.0f * smt + 1.0f;
            float dd = 1.0f - 2.0f * smt;
            float m = a < bb ? a : bb;
            float n = cc < dd ? cc : dd;
            x = (m < n ? m : n) * x_sign;
        }
        norm_squared = x * x + y * y + z * z;
        if (norm_squared < 1e-6f) {
            att->out_floats[(size_t)i * 3u] = 0;
            att->out_floats[(size_t)i * 3u + 1u] = 0;
            att->out_floats[(size_t)i * 3u + 2u] = 0;
        } else {
            float d = (float)(1.0 / sqrt((double)norm_squared));
            att->out_floats[(size_t)i * 3u] = x * d;
            att->out_floats[(size_t)i * 3u + 1u] = y * d;
            att->out_floats[(size_t)i * 3u + 2u] = z * d;
        }
    }
    return 1;
}

/// @brief spec DecodeDataNeededByPortableTransforms — quantization parameters.
/// @param b Bounded reader positioned at transform metadata.
/// @param att Attribute receiving quantization origin, range, and bit count.
/// @return Non-zero when all transform parameters required by the decoder are available.
static int draco_read_transform_data(draco_buf *b, draco_attribute *att) {
    if (!b || !att || att->decoder_type > DRACO_SEQ_NORMALS || att->components == 0 ||
        att->components > DRACO_MAX_COMPONENTS)
        return 0;
    if (att->decoder_type == DRACO_SEQ_NORMALS) {
        att->quantization_bits = draco_read_u8(b);
        if (b->failed || att->quantization_bits == 0 || att->quantization_bits > 30)
            return 0;
    } else if (att->decoder_type == DRACO_SEQ_QUANTIZATION) {
        for (int c = 0; c < att->components; c++)
            att->quant_min[c] = draco_read_f32(b);
        att->quant_range = draco_read_f32(b);
        att->quantization_bits = draco_read_u8(b);
        if (b->failed || att->quantization_bits == 0 || att->quantization_bits > 30 ||
            !isfinite(att->quant_range) || att->quant_range < 0.0f)
            return 0;
        for (int c = 0; c < att->components; c++) {
            if (!isfinite(att->quant_min[c]))
                return 0;
        }
    }
    return 1;
}

/// @brief spec TransformAttributesToOriginalFormat for one attribute.
/// @param att Attribute whose portable representation is converted to final output.
/// @return Non-zero when the selected decoder type has valid transform data and conversion
///         succeeds.
static int draco_transform_attribute_to_original(draco_attribute *att) {
    uint32_t scalar_count;
    size_t output_bytes;
    if (!att || att->decoder_type > DRACO_SEQ_NORMALS || att->components == 0 ||
        att->components > DRACO_MAX_COMPONENTS)
        return 0;
    if (att->decoder_type == DRACO_SEQ_GENERIC)
        return 1; /* raw values are already final */
    if (att->decoder_type == DRACO_SEQ_NORMALS)
        return draco_decode_normals_attribute(att);
    if (att->decoder_type == DRACO_SEQ_QUANTIZATION)
        return draco_dequantize_attribute(att);
    if (att->num_values > 0) {
        if (!att->values || !draco_scalar_count(att->num_values, att->components, &scalar_count) ||
            !draco_size_mul((size_t)scalar_count, sizeof(int32_t), &output_bytes))
            return 0;
        att->out_ints = (int32_t *)calloc(1u, output_bytes);
        if (!att->out_ints)
            return 0;
        memcpy(att->out_ints, att->values, output_bytes);
    }
    return 1;
}

/// @brief Decode an edgebreaker payload (spec DecodeEdgebreakerConnectivityData +
///        DecodeAttributeData) and remap the result to point-indexed arrays so the
///        caller sees the same layout as a sequential mesh.
/// @param b Bounded reader positioned at edgebreaker connectivity data.
/// @param mesh Destination mesh receiving remapped triangles and attributes.
/// @return Non-zero when connectivity, seams, traversals, attributes, and point remapping all
///         succeed.
static int draco_decode_edgebreaker(draco_buf *b, draco_mesh *mesh) {
    draco_eb ebs;
    draco_eb *eb = &ebs;
    int ok = 0;
    memset(eb, 0, sizeof(*eb));
    if (!draco_eb_decode_connectivity(b, eb))
        goto done;

    /* ParseAttributeDecodersData (edgebreaker layout). */
    {
        uint8_t num_decoders = draco_read_u8(b);
        uint32_t total_atts = 0;
        if (b->failed || num_decoders == 0 || num_decoders > DRACO_EB_MAX_DECODERS)
            goto done;
        eb->num_decoders = num_decoders;
        for (uint32_t i = 0; i < num_decoders; i++) {
            eb->dec[i].data_id = draco_read_u8(b);
            eb->dec[i].decoder_type = draco_read_u8(b);
            eb->dec[i].traversal = draco_read_u8(b);
            if (b->failed || eb->dec[i].decoder_type > 1 || eb->dec[i].traversal > 1)
                goto done;
            if (eb->dec[i].data_id != 0xFF && eb->dec[i].data_id >= eb->num_att_data)
                goto done;
            if (i == 0 && (eb->dec[i].data_id != 0xFF || eb->dec[i].decoder_type != 0))
                goto done; /* decoder zero owns unsplit position connectivity */
            if (eb->dec[i].decoder_type == 1 &&
                (eb->dec[i].data_id == 0xFF || eb->dec[i].traversal != 0))
                goto done; /* corner-mapped decoders need seams + depth-first order */
        }
        for (uint32_t i = 0; i < num_decoders; i++) {
            uint64_t natts = draco_read_varint(b);
            if (b->failed || natts == 0 || total_atts + natts > DRACO_MAX_ATTRIBUTES)
                goto done;
            eb->dec[i].att_start = total_atts;
            eb->dec[i].att_count = (uint32_t)natts;
            for (uint64_t j = 0; j < natts; j++) {
                draco_attribute *att = &mesh->attributes[total_atts + j];
                att->att_type = draco_read_u8(b);
                att->data_type = draco_read_u8(b);
                att->components = draco_read_u8(b);
                att->normalized = draco_read_u8(b);
                if (!draco_read_varint_u32(b, &att->unique_id) ||
                    !draco_attribute_descriptor_valid(att))
                    goto done;
            }
            for (uint64_t j = 0; j < natts; j++) {
                draco_attribute *att = &mesh->attributes[total_atts + j];
                att->decoder_type = draco_read_u8(b);
                /* The octahedral normal transform always emits 3-component output
                 * (see the stride-3 allocation in draco_decode_normals_attribute);
                 * a mismatched declared component count would desync the output
                 * stride from the reader's (value*components) indexing. */
                if (att->decoder_type > DRACO_SEQ_NORMALS ||
                    (att->decoder_type == DRACO_SEQ_NORMALS && att->components != 3))
                    goto done;
            }
            if (b->failed)
                goto done;
            total_atts += (uint32_t)natts;
        }
        mesh->num_attributes = total_atts;
    }

    /* DecodeAttributeData preamble: seams, hole-corner rotation, recomputed
     * attribute vertices, point assignment. */
    if (!draco_eb_decode_seams(b, eb))
        goto done;
    for (uint32_t v = 0; v < eb->num_vertices; v++) {
        if (eb->is_vert_hole[v]) {
            if (!draco_eb_update_vertex_corner(eb, v))
                goto done;
        }
    }
    for (uint32_t a = 0; a < eb->num_att_data; a++) {
        if (!draco_eb_recompute_vertices(eb, a))
            goto done;
    }
    if (!draco_eb_assign_points(eb) || eb->point_count == 0)
        goto done;

    /* GenerateSequence + UpdatePointToAttributeIndexMapping per decoder. */
    for (uint32_t d = 0; d < eb->num_decoders; d++) {
        draco_eb_dec *dec = &eb->dec[d];
        size_t corner_bytes;
        size_t visit_bytes;
        size_t point_bytes;
        size_t traversal_bytes;
        if (!draco_size_mul((size_t)eb->corner_cap, sizeof(int32_t), &corner_bytes) ||
            !draco_size_mul((size_t)eb->visit_cap, sizeof(int32_t), &visit_bytes) ||
            !draco_size_mul((size_t)eb->point_count, sizeof(int32_t), &point_bytes) ||
            !draco_size_mul((size_t)eb->corner_cap + 2u, sizeof(int32_t), &traversal_bytes))
            goto done;
        dec->value_to_corner = (int32_t *)malloc(corner_bytes);
        dec->vertex_to_value = (int32_t *)malloc(visit_bytes);
        dec->indices_map = (int32_t *)malloc(point_bytes);
        if (!dec->value_to_corner || !dec->vertex_to_value || !dec->indices_map)
            goto done;
        memset(dec->vertex_to_value, 0xFF, (size_t)eb->visit_cap * sizeof(int32_t));
        memset(eb->face_visited, 0, eb->num_faces);
        memset(eb->vertex_visited, 0, eb->visit_cap);
        if (dec->traversal == 1) {
            if (!eb->pred_degree) {
                eb->pred_degree = (uint32_t *)malloc(visit_bytes);
                for (int i = 0; i < 3; i++)
                    eb->pd_stack[i] = (int32_t *)malloc(traversal_bytes);
                if (!eb->pred_degree || !eb->pd_stack[0] || !eb->pd_stack[1] || !eb->pd_stack[2])
                    goto done;
            }
            memset(eb->pred_degree, 0, (size_t)eb->visit_cap * sizeof(uint32_t));
            eb->pd_size[0] = eb->pd_size[1] = eb->pd_size[2] = 0;
            eb->pd_best = 0;
            for (uint32_t f = 0; f < eb->num_faces; f++) {
                if (!draco_eb_traverse_pd(eb, d, (int32_t)(3u * f)))
                    goto done;
            }
        } else {
            for (uint32_t f = 0; f < eb->num_faces; f++) {
                if (!draco_eb_traverse_dfs(eb, d, (int32_t)(3u * f)))
                    goto done;
            }
        }
        if (dec->value_count == 0 || !draco_eb_update_indices_map(eb, d))
            goto done;
    }

    /* Attribute decode per decoder: portable values, transform parameters, then
     * conversion to the original format (spec order). */
    for (uint32_t d = 0; d < eb->num_decoders; d++) {
        draco_eb_dec *dec = &eb->dec[d];
        for (uint32_t j = 0; j < dec->att_count; j++)
            mesh->attributes[dec->att_start + j].num_values = dec->value_count;
        for (uint32_t j = 0; j < dec->att_count; j++) {
            if (!draco_decode_portable_attribute(
                    b, mesh, &mesh->attributes[dec->att_start + j], eb, d))
                goto done;
        }
        for (uint32_t j = 0; j < dec->att_count; j++) {
            if (!draco_read_transform_data(b, &mesh->attributes[dec->att_start + j]))
                goto done;
        }
        for (uint32_t j = 0; j < dec->att_count; j++) {
            if (!draco_transform_attribute_to_original(&mesh->attributes[dec->att_start + j]))
                goto done;
        }
    }

    /* Remap decoded values and faces to point indices so downstream consumers see
     * the sequential layout (identity mapping). */
    mesh->num_points = eb->point_count;
    mesh->num_faces = eb->num_faces;
    {
        size_t face_bytes;
        if (!draco_size_mul((size_t)eb->corner_cap, sizeof(uint32_t), &face_bytes))
            goto done;
        mesh->face_to_vertex = (uint32_t *)malloc(face_bytes);
    }
    if (!mesh->face_to_vertex)
        goto done;
    for (uint32_t c = 0; c < eb->corner_cap; c++)
        mesh->face_to_vertex[c] = (uint32_t)eb->corner_to_point[c];
    for (uint32_t d = 0; d < eb->num_decoders; d++) {
        draco_eb_dec *dec = &eb->dec[d];
        for (uint32_t j = 0; j < dec->att_count; j++) {
            draco_attribute *att = &mesh->attributes[dec->att_start + j];
            uint32_t out_comps =
                (att->decoder_type == DRACO_SEQ_NORMALS) ? 3u : (uint32_t)att->components;
            size_t output_bytes;
            if (!draco_tuple_bytes(eb->point_count,
                                   out_comps,
                                   att->out_floats ? sizeof(float) : sizeof(int32_t),
                                   &output_bytes))
                goto done;
            if (att->out_floats) {
                float *pf = (float *)calloc(1u, output_bytes);
                if (!pf)
                    goto done;
                for (uint32_t p = 0; p < eb->point_count; p++) {
                    int32_t src = dec->indices_map[p];
                    if (src < 0 || (uint32_t)src >= att->num_values) {
                        free(pf);
                        goto done;
                    }
                    memcpy(pf + (size_t)p * out_comps,
                           att->out_floats + (size_t)src * out_comps,
                           out_comps * sizeof(float));
                }
                free(att->out_floats);
                att->out_floats = pf;
            } else if (att->out_ints) {
                int32_t *pi = (int32_t *)calloc(1u, output_bytes);
                if (!pi)
                    goto done;
                for (uint32_t p = 0; p < eb->point_count; p++) {
                    int32_t src = dec->indices_map[p];
                    if (src < 0 || (uint32_t)src >= att->num_values) {
                        free(pi);
                        goto done;
                    }
                    memcpy(pi + (size_t)p * out_comps,
                           att->out_ints + (size_t)src * out_comps,
                           out_comps * sizeof(int32_t));
                }
                free(att->out_ints);
                att->out_ints = pi;
            } else {
                goto done;
            }
            att->num_values = eb->point_count;
        }
    }
    ok = 1;
done:
    draco_eb_free(eb);
    return ok;
}

/// @brief Decode a complete Draco payload (spec Decode()) — sequential and
///        edgebreaker triangular meshes.
/// @param data Complete compressed Draco payload.
/// @param size Number of readable bytes at @p data.
/// @param mesh Zero-initialized destination mesh, cleared on failure.
/// @param out_unsupported Optional flag set when the payload is valid but uses an unsupported
///                        geometry or encoding mode.
/// @return 1 on success, 0 with @p out_unsupported set when the payload uses an
///         encoding Zanna does not handle (point clouds) rather than being corrupt.
static int draco_decode_mesh(const uint8_t *data,
                             size_t size,
                             draco_mesh *mesh,
                             int *out_unsupported) {
    draco_buf b;
    uint8_t encoder_type;
    uint8_t encoder_method;
    uint16_t flags;
    if (!mesh)
        return 0;
    memset(mesh, 0, sizeof(*mesh));
    if (out_unsupported)
        *out_unsupported = 0;
    b.data = data;
    b.size = size;
    b.pos = 0;
    b.failed = 0;
    if (!data || size < 11 || memcmp(data, "DRACO", 5) != 0)
        return 0;
    b.pos = 5;
    (void)draco_read_u8(&b); /* major version */
    (void)draco_read_u8(&b); /* minor version */
    encoder_type = draco_read_u8(&b);
    encoder_method = draco_read_u8(&b);
    flags = draco_read_u16(&b);
    if (b.failed)
        return 0;
    if (encoder_type != 1 /* TRIANGULAR_MESH */) {
        if (out_unsupported)
            *out_unsupported = 1; /* point clouds */
        return 0;
    }
    if ((flags & 0x8000u) && !draco_skip_metadata(&b))
        return 0;
    if (encoder_method == 1 /* MESH_EDGEBREAKER_ENCODING */) {
        if (!draco_decode_edgebreaker(&b, mesh))
            goto fail;
        return 1;
    }
    if (encoder_method != 0 /* MESH_SEQUENTIAL_ENCODING */) {
        if (out_unsupported)
            *out_unsupported = 1;
        return 0;
    }
    if (!draco_decode_sequential_connectivity(&b, mesh))
        goto fail;

    /* Attribute decoders (spec ParseAttributeDecodersData, sequential layout). */
    {
        uint8_t num_decoders = draco_read_u8(&b);
        uint32_t total_attributes = 0;
        if (b.failed || num_decoders != 1)
            goto fail; /* sequential meshes always use one attributes decoder */
        {
            uint64_t num_attributes = draco_read_varint(&b);
            if (b.failed || num_attributes == 0 || num_attributes > DRACO_MAX_ATTRIBUTES)
                goto fail;
            total_attributes = (uint32_t)num_attributes;
            for (uint32_t j = 0; j < total_attributes; j++) {
                draco_attribute *att = &mesh->attributes[j];
                att->att_type = draco_read_u8(&b);
                att->data_type = draco_read_u8(&b);
                att->components = draco_read_u8(&b);
                att->normalized = draco_read_u8(&b);
                if (!draco_read_varint_u32(&b, &att->unique_id))
                    goto fail;
                att->num_values = mesh->num_points;
                if (b.failed || !draco_attribute_descriptor_valid(att))
                    goto fail;
            }
            for (uint32_t j = 0; j < total_attributes; j++) {
                mesh->attributes[j].decoder_type = draco_read_u8(&b);
                /* NORMALS transform emits stride-3 output regardless of the declared
                 * component count; reject a mismatch that would desync the reader. */
                if (mesh->attributes[j].decoder_type > DRACO_SEQ_NORMALS ||
                    (mesh->attributes[j].decoder_type == DRACO_SEQ_NORMALS &&
                     mesh->attributes[j].components != 3))
                    goto fail;
            }
            if (b.failed)
                goto fail;
        }
        mesh->num_attributes = total_attributes;

        /* DecodePortableAttributes + DecodeDataNeededByPortableTransforms +
         * TransformAttributesToOriginalFormat, in spec order. */
        for (uint32_t j = 0; j < total_attributes; j++) {
            if (!draco_decode_portable_attribute(&b, mesh, &mesh->attributes[j], NULL, 0))
                goto fail;
        }
        for (uint32_t j = 0; j < total_attributes; j++) {
            if (!draco_read_transform_data(&b, &mesh->attributes[j]))
                goto fail;
        }
        for (uint32_t j = 0; j < total_attributes; j++) {
            if (!draco_transform_attribute_to_original(&mesh->attributes[j]))
                goto fail;
        }
    }
    return 1;
fail:
    draco_mesh_free(mesh);
    return 0;
}

/// @brief Find a decoded Draco attribute by its glTF-mapped unique id.
/// @param mesh Decoded mesh whose attribute table is searched.
/// @param unique_id Attribute identifier supplied by the glTF Draco extension.
/// @return Borrowed matching attribute descriptor, or NULL when absent or out of range.
static const draco_attribute *draco_find_attribute(const draco_mesh *mesh, int64_t unique_id) {
    if (!mesh || unique_id < 0)
        return NULL;
    for (uint32_t i = 0; i < mesh->num_attributes && i < DRACO_MAX_ATTRIBUTES; i++) {
        if (mesh->attributes[i].unique_id == (uint32_t)unique_id)
            return &mesh->attributes[i];
    }
    return NULL;
}
