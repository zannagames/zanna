//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_zstd.c
// Purpose: From-scratch Zstandard (RFC 8878) decompressor. Decode-only,
//   single-frame, no dictionaries — the subset every KTX2 supercompressed
//   texture and general tool output needs. Implements the full compressed
//   format: FSE (tANS) tables with predefined and file-supplied
//   distributions, canonical Huffman literals (1- and 4-stream), the
//   sequence machine with the three repeated offsets, and xxhash64 content
//   checksums.
// Key invariants:
//   - Every read is bounds-checked against the input; every write against the
//     decoded-size budget. Corrupt input returns 0, never traps or overruns.
//   - The backward bitstream (FSE/Huffman) validates its start sentinel bit.
//   - Deterministic: no allocation-order or platform dependence in decode.
// Ownership/Lifetime:
//   - The output buffer is malloc-owned by the caller on success.
// Links: rt_zstd.h, RFC 8878, src/runtime/io/rt_compress.c (DEFLATE peer)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements the dependency-free single-frame Zstandard decoder.
 * @details Provides bounded RFC 8878 frame parsing, backward entropy streams,
 * FSE and Huffman table construction, literal and sequence reconstruction,
 * repeated-offset handling, and optional xxHash64 checksum verification for
 * allocating and exact-destination decode paths.
 */

#include "rt_zstd.h"

#include <stdlib.h>
#include <string.h>

/** Little-endian signature of a standard Zstandard frame. */
#define ZSTD_MAGIC 0xFD2FB528u
/** Largest FSE table logarithm accepted by the bounded decoder. */
#define ZSTD_MAX_TABLELOG 12
/** Largest canonical Huffman code width supported for literal decoding. */
#define ZSTD_MAX_HUF_BITS 11
/** Highest legal literal-length code symbol. */
#define ZSTD_MAX_LL_SYMBOL 35
/** Highest legal match-length code symbol. */
#define ZSTD_MAX_ML_SYMBOL 52
/** Highest accepted offset code symbol, permitting offsets through 2^31. */
#define ZSTD_MAX_OF_SYMBOL 31 /* offsets up to 2^31; generous for one-shot frames */
/** Defensive ceiling on the number of sequences in one compressed block. */
#define ZSTD_MAX_SEQUENCES (1u << 24)

/*==========================================================================
 * xxhash64 (checksum verification)
 *=========================================================================*/

/** First 64-bit xxHash mixing prime. */
#define XXH_PRIME64_1 0x9E3779B185EBCA87ULL
/** Second 64-bit xxHash mixing prime. */
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4FULL
/** Third 64-bit xxHash mixing prime. */
#define XXH_PRIME64_3 0x165667B19E3779F9ULL
/** Fourth 64-bit xxHash mixing prime. */
#define XXH_PRIME64_4 0x85EBCA77C2B2AE63ULL
/** Fifth 64-bit xxHash mixing prime. */
#define XXH_PRIME64_5 0x27D4EB2F165667C5ULL

/// @brief Rotate a 64-bit checksum lane left by a fixed distance.
/// @param x Lane value to rotate.
/// @param r Rotation count in the range 1 through 63.
/// @return Rotated 64-bit value.
static uint64_t xxh_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

/// @brief Load one unaligned little-endian 64-bit checksum lane.
/// @details The supported runtime targets are little-endian, so memcpy avoids
///          alignment and aliasing violations without a byte-swap.
/// @param p Pointer to at least eight readable bytes.
/// @return Loaded host-order 64-bit value.
static uint64_t xxh_read64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v; /* zstd frames are little-endian; Zanna targets are little-endian */
}

/// @brief Load one unaligned little-endian 32-bit checksum lane.
/// @param p Pointer to at least four readable bytes on a supported little-endian target.
/// @return Loaded host-order 32-bit value.
static uint32_t xxh_read32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/// @brief Mix one 64-bit input lane into an xxHash64 accumulator.
/// @param acc Current accumulator lane.
/// @param input Next input word.
/// @return Multiplied and rotated accumulator.
static uint64_t xxh_round(uint64_t acc, uint64_t input) {
    acc += input * XXH_PRIME64_2;
    acc = xxh_rotl64(acc, 31);
    acc *= XXH_PRIME64_1;
    return acc;
}

/// @brief Merge a completed xxHash64 stripe lane into the main accumulator.
/// @param acc Main checksum accumulator.
/// @param val Completed stripe lane to fold in.
/// @return Updated main accumulator.
static uint64_t xxh_merge_round(uint64_t acc, uint64_t val) {
    val = xxh_round(0, val);
    acc ^= val;
    acc = acc * XXH_PRIME64_1 + XXH_PRIME64_4;
    return acc;
}

/// @brief xxhash64 with seed 0 — the digest zstd stores (low 32 bits) as its
///   optional content checksum.
/// @param input Complete byte span to hash.
/// @param len Number of readable bytes at @p input.
/// @return Full 64-bit xxHash64 digest; Zstandard compares its low 32 bits.
static uint64_t xxhash64(const uint8_t *input, size_t len) {
    const uint8_t *p = input;
    const uint8_t *end = input + len;
    uint64_t h;

    if (len >= 32) {
        uint64_t v1 = XXH_PRIME64_1 + XXH_PRIME64_2;
        uint64_t v2 = XXH_PRIME64_2;
        uint64_t v3 = 0;
        uint64_t v4 = (uint64_t)0 - XXH_PRIME64_1;
        const uint8_t *limit = end - 32;
        do {
            v1 = xxh_round(v1, xxh_read64(p));
            p += 8;
            v2 = xxh_round(v2, xxh_read64(p));
            p += 8;
            v3 = xxh_round(v3, xxh_read64(p));
            p += 8;
            v4 = xxh_round(v4, xxh_read64(p));
            p += 8;
        } while (p <= limit);
        h = xxh_rotl64(v1, 1) + xxh_rotl64(v2, 7) + xxh_rotl64(v3, 12) + xxh_rotl64(v4, 18);
        h = xxh_merge_round(h, v1);
        h = xxh_merge_round(h, v2);
        h = xxh_merge_round(h, v3);
        h = xxh_merge_round(h, v4);
    } else {
        h = XXH_PRIME64_5;
    }
    h += (uint64_t)len;
    while (p + 8 <= end) {
        h ^= xxh_round(0, xxh_read64(p));
        h = xxh_rotl64(h, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)xxh_read32(p) * XXH_PRIME64_1;
        h = xxh_rotl64(h, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p) * XXH_PRIME64_5;
        h = xxh_rotl64(h, 11) * XXH_PRIME64_1;
        p++;
    }
    h ^= h >> 33;
    h *= XXH_PRIME64_2;
    h ^= h >> 29;
    h *= XXH_PRIME64_3;
    h ^= h >> 32;
    return h;
}

/*==========================================================================
 * Backward bitstream (FSE / Huffman payloads are read back-to-front)
 *=========================================================================*/

/// @brief Backward bit reader: zstd entropy payloads are written forward but
///   consumed from the last byte backward, with a mandatory 1-bit sentinel
///   marking the true end of the stream.
/// @details Reads past the front of the payload return zero padding — the
///   format legitimately lets final table lookups peek beyond the stream —
///   while `bits_consumed` tracks real usage so callers can verify that no
///   *consumed* bit ever came from padding (`zstd_rbits_ok`) and that streams
///   were fully drained (`zstd_rbits_fully_consumed`).
typedef struct {
    const uint8_t *base; /* first byte of the payload */
    size_t byte_pos;     /* next byte index to refill from (counting down) */
    uint64_t container;
    int bits_in_container;
    size_t total_bits;    /* payload bits below the sentinel */
    size_t bits_consumed; /* bits handed out via zstd_rbits_read */
} zstd_rbits;

/// @brief Initialize a backward entropy bit reader and remove its end sentinel.
/// @param b Reader state to zero and initialize.
/// @param data Complete encoded backward-bitstream payload.
/// @param len Payload size in bytes; must include a nonzero final byte.
/// @return 1 when a sentinel was found and state initialized, otherwise 0.
static int zstd_rbits_init(zstd_rbits *b, const uint8_t *data, size_t len) {
    uint8_t last;
    int sentinel;

    memset(b, 0, sizeof(*b));
    if (!data || len == 0)
        return 0;
    last = data[len - 1];
    if (last == 0)
        return 0; /* missing sentinel bit */
    b->base = data;
    /* Load the final byte and strip the sentinel (highest set bit). */
    b->container = last;
    sentinel = 7;
    while (((last >> sentinel) & 1u) == 0)
        sentinel--;
    b->bits_in_container = sentinel; /* bits below the sentinel are payload */
    b->byte_pos = len - 1;
    b->total_bits = (len - 1) * 8u + (size_t)sentinel;
    b->bits_consumed = 0;
    return 1;
}

/// @brief Ensure the backward reader's shift container exposes enough bits.
/// @details Bytes are prepended while walking toward the payload front. Once
///          exhausted, zero padding supports table peeks; consumption validity
///          remains tracked separately by @ref zstd_rbits_ok.
/// @param b Initialized backward-bitstream reader.
/// @param need Minimum number of bits required in the container.
static void zstd_rbits_refill(zstd_rbits *b, int need) {
    while (b->bits_in_container < need) {
        if (b->byte_pos == 0) {
            /* Zero padding past the front; legality is judged by the caller
             * via bits_consumed vs total_bits. */
            b->container <<= 8;
            b->bits_in_container += 8;
            continue;
        }
        b->byte_pos--;
        b->container = (b->container << 8) | b->base[b->byte_pos];
        b->bits_in_container += 8;
    }
}

/// @brief Read @p n bits (MSB-first from the backward container). n <= 32.
/// @param b Initialized backward-bitstream reader.
/// @param n Number of bits to consume, from zero through 32.
/// @return Decoded unsigned value; zero when @p n is zero.
static uint32_t zstd_rbits_read(zstd_rbits *b, int n) {
    uint32_t v;
    if (n == 0)
        return 0;
    zstd_rbits_refill(b, n);
    v = (uint32_t)((b->container >> (b->bits_in_container - n)) & ((1ull << n) - 1u));
    b->bits_in_container -= n;
    b->bits_consumed += (size_t)n;
    return v;
}

/// @brief Bits still available to consume without touching padding.
/// @param b Initialized backward-bitstream reader.
/// @return Unconsumed real payload bits, saturated at zero.
static size_t zstd_rbits_remaining(const zstd_rbits *b) {
    return b->bits_consumed >= b->total_bits ? 0 : b->total_bits - b->bits_consumed;
}

/// @brief True while no consumed bit has come from padding.
/// @param b Initialized backward-bitstream reader.
/// @return 1 when consumption has not exceeded the real payload, otherwise 0.
static int zstd_rbits_ok(const zstd_rbits *b) {
    return b->bits_consumed <= b->total_bits;
}

/// @brief True when the stream was consumed exactly (all payload, no padding).
/// @param b Initialized backward-bitstream reader.
/// @return 1 when consumed bits equal the payload budget, otherwise 0.
static int zstd_rbits_fully_consumed(const zstd_rbits *b) {
    return b->bits_consumed == b->total_bits;
}

/*==========================================================================
 * FSE (tANS) decode tables
 *=========================================================================*/

/** One state transition in an FSE decoding table. */
typedef struct {
    uint8_t symbol; ///< Alphabet symbol emitted from this state.
    uint8_t nbits;  ///< Number of backward-stream bits consumed next.
    uint16_t base;  ///< Base state to which the consumed bits are added.
} zstd_fse_cell;

/** Fixed-capacity FSE table for one decoded alphabet. */
typedef struct {
    zstd_fse_cell cells[1 << ZSTD_MAX_TABLELOG]; ///< State transition cells.
    int table_log;                              ///< Base-two logarithm of active cells.
} zstd_fse_table;

/// @brief Find the index of the highest set bit in a 32-bit value.
/// @param v Value to classify.
/// @return Floor of log2(@p v) for positive input; zero for zero or one.
static int zstd_highbit32(uint32_t v) {
    int r = 0;
    while (v > 1) {
        v >>= 1;
        r++;
    }
    return r;
}

/// @brief Build an FSE decode table from normalized counts (RFC 8878 §4.1.1).
/// @details Symbols with count -1 ("less than 1") occupy the table tail; the
///   remaining cells are spread with the standard (5/8·size + 3) step. Cell
///   states then get their bit counts and baselines from the per-symbol
///   occurrence counters.
/// @param t Destination decode table.
/// @param norm Normalized counts indexed from zero through @p max_symbol.
/// @param max_symbol Highest populated symbol index, at most 255.
/// @param table_log Base-two log of the requested table size.
/// @return 1 for a complete valid table, otherwise 0.
static int zstd_fse_build(zstd_fse_table *t, const int16_t *norm, int max_symbol, int table_log) {
    uint32_t table_size;
    uint32_t high_threshold;
    uint32_t position = 0;
    uint32_t step;
    uint16_t symbol_next[256];
    int s;

    if (table_log < 1 || table_log > ZSTD_MAX_TABLELOG || max_symbol < 0 || max_symbol > 255)
        return 0;
    table_size = 1u << table_log;
    high_threshold = table_size - 1;
    step = (table_size >> 1) + (table_size >> 3) + 3;
    t->table_log = table_log;

    for (s = 0; s <= max_symbol; s++) {
        if (norm[s] == -1) {
            t->cells[high_threshold].symbol = (uint8_t)s;
            high_threshold--;
            symbol_next[s] = 1;
        } else {
            if (norm[s] < 0)
                return 0;
            symbol_next[s] = (uint16_t)norm[s];
        }
    }
    for (s = 0; s <= max_symbol; s++) {
        int n = norm[s];
        int i;
        if (n <= 0)
            continue;
        for (i = 0; i < n; i++) {
            t->cells[position].symbol = (uint8_t)s;
            do {
                position = (position + step) & (table_size - 1);
            } while (position > high_threshold);
        }
    }
    if (position != 0)
        return 0; /* spread must land exactly back at zero */

    for (uint32_t c = 0; c < table_size; c++) {
        uint8_t sym = t->cells[c].symbol;
        uint16_t x = symbol_next[sym]++;
        int nbits = table_log - zstd_highbit32(x);
        if (nbits < 0 || nbits > table_log)
            return 0;
        t->cells[c].nbits = (uint8_t)nbits;
        t->cells[c].base = (uint16_t)(((uint32_t)x << nbits) - table_size);
    }
    return 1;
}

/// @brief Parse an FSE normalized-count header (RFC 8878 §4.1.1) from a
///   forward bitstream. Returns consumed byte count, or 0 on error.
/// @param data Bounded header bytes.
/// @param len Number of readable bytes at @p data.
/// @param norm Destination array for decoded normalized counts.
/// @param out_max_symbol Receives the last decoded symbol index on success.
/// @param out_table_log Receives the decoded table log on success.
/// @param max_allowed_symbol Largest symbol accepted for this alphabet.
/// @param max_allowed_log Largest table log accepted by this caller.
/// @return Number of whole encoded bytes consumed, or zero for malformed or
///         truncated input.
static size_t zstd_fse_read_ncount(const uint8_t *data,
                                   size_t len,
                                   int16_t *norm,
                                   int *out_max_symbol,
                                   int *out_table_log,
                                   int max_allowed_symbol,
                                   int max_allowed_log) {
    size_t bit_pos = 0;
    int table_log;
    int32_t remaining;
    int symbol = 0;
    int previous_was_zero = 0;

#define ZSTD_NC_READ(nbits, out_v)                                                                 \
    do {                                                                                           \
        uint32_t v_ = 0;                                                                           \
        int i_;                                                                                    \
        for (i_ = 0; i_ < (nbits); i_++) {                                                         \
            size_t bp_ = bit_pos + (size_t)i_;                                                     \
            if ((bp_ >> 3) >= len)                                                                 \
                return 0;                                                                          \
            v_ |= (uint32_t)((data[bp_ >> 3] >> (bp_ & 7)) & 1u) << i_;                            \
        }                                                                                          \
        bit_pos += (size_t)(nbits);                                                                \
        (out_v) = v_;                                                                              \
    } while (0)

    {
        uint32_t acc;
        ZSTD_NC_READ(4, acc);
        table_log = (int)acc + 5;
    }
    if (table_log > max_allowed_log || table_log > ZSTD_MAX_TABLELOG)
        return 0;
    remaining = (int32_t)(1u << table_log) + 1;

    while (remaining > 1 && symbol <= max_allowed_symbol) {
        if (previous_was_zero) {
            uint32_t repeat;
            ZSTD_NC_READ(2, repeat);
            while (repeat == 3) {
                symbol += 3;
                if (symbol > max_allowed_symbol + 1)
                    return 0;
                ZSTD_NC_READ(2, repeat);
            }
            symbol += (int)repeat;
            previous_was_zero = 0;
            continue;
        }
        {
            uint32_t max_v = (uint32_t)remaining; /* == spec's remaining+1 */
            int nbits = zstd_highbit32(max_v) + 1;
            uint32_t low_cut = (uint32_t)((1 << nbits) - 1) - max_v;
            uint32_t value;
            int32_t count;

            ZSTD_NC_READ(nbits - 1, value);
            if (value < low_cut) {
                /* small value: nbits-1 bits were enough */
            } else {
                uint32_t high_bit;
                ZSTD_NC_READ(1, high_bit);
                value |= high_bit << (nbits - 1);
                if (value >= (uint32_t)(1 << (nbits - 1)))
                    value -= low_cut;
            }
            count = (int32_t)value - 1; /* -1 encodes the "less than 1" probability */
            if (count < -1)
                return 0;
            if (symbol > max_allowed_symbol)
                return 0;
            norm[symbol] = (int16_t)count;
            remaining -= count < 0 ? 1 : count;
            if (remaining < 1)
                return 0;
            previous_was_zero = (count == 0);
            symbol++;
        }
    }
    if (remaining != 1)
        return 0;
    *out_max_symbol = symbol - 1;
    *out_table_log = table_log;
#undef ZSTD_NC_READ
    return (bit_pos + 7) / 8;
}

/*==========================================================================
 * Huffman literals
 *=========================================================================*/

/** Canonical Huffman lookup table for one literal alphabet. */
typedef struct {
    uint8_t symbol[1 << ZSTD_MAX_HUF_BITS]; ///< Symbol selected by each lookup prefix.
    uint8_t nbits[1 << ZSTD_MAX_HUF_BITS];  ///< Real code width for each lookup cell.
    int table_log;                          ///< Width of the complete lookup prefix.
} zstd_huf_table;

/// @brief Build the canonical Huffman lookup table from symbol weights
///   (RFC 8878 §4.2.1): codes are assigned by ascending weight, symbols in
///   natural order within a weight, each filling 2^(weight-1) table cells.
/// @param t Destination lookup table.
/// @param weights Decoded weight for every symbol in natural order.
/// @param num_symbols Number of entries in @p weights.
/// @return 1 when weights exactly populate a supported power-of-two table,
///         otherwise 0.
static int zstd_huf_build(zstd_huf_table *t, const uint8_t *weights, int num_symbols) {
    uint32_t total = 0;
    int max_bits;
    uint32_t pos = 0;
    int w;

    for (int s = 0; s < num_symbols; s++) {
        if (weights[s] > ZSTD_MAX_HUF_BITS)
            return 0;
        if (weights[s] > 0)
            total += 1u << (weights[s] - 1);
    }
    if (total == 0)
        return 0;
    /* total must be a power of two after the implied last weight was added. */
    if ((total & (total - 1)) != 0)
        return 0;
    max_bits = zstd_highbit32(total);
    if (max_bits > ZSTD_MAX_HUF_BITS)
        return 0;
    t->table_log = max_bits;
    for (w = 1; w <= max_bits + 1; w++) {
        for (int s = 0; s < num_symbols; s++) {
            uint32_t cells;
            int bits;
            if (weights[s] != w)
                continue;
            cells = 1u << (w - 1);
            bits = max_bits + 1 - w;
            if (pos + cells > (1u << max_bits))
                return 0;
            for (uint32_t c = 0; c < cells; c++) {
                t->symbol[pos + c] = (uint8_t)s;
                t->nbits[pos + c] = (uint8_t)bits;
            }
            pos += cells;
        }
    }
    return pos == (1u << max_bits);
}

/// @brief Decode Huffman weights, either raw 4-bit pairs (header >= 128) or an
///   FSE-compressed weight stream. Fills @p weights (up to 255 symbols + the
///   implied final weight) and returns consumed bytes, or 0 on error.
/// @param data Bounded Huffman tree description.
/// @param len Number of readable bytes at @p data.
/// @param weights Destination with capacity for 256 weights.
/// @param out_num_symbols Receives the explicit symbols plus the implied final symbol.
/// @return Encoded tree-description bytes consumed, or zero on error.
static size_t zstd_huf_read_weights(const uint8_t *data,
                                    size_t len,
                                    uint8_t *weights,
                                    int *out_num_symbols) {
    uint8_t header;
    size_t consumed;
    int num_weights;
    uint32_t total = 0;

    if (len < 1)
        return 0;
    header = data[0];
    if (header >= 128) {
        /* Direct: header-127 weights, 4 bits each. */
        num_weights = header - 127;
        size_t bytes = ((size_t)num_weights + 1) / 2;
        if (1 + bytes > len)
            return 0;
        for (int i = 0; i < num_weights; i++) {
            uint8_t b = data[1 + (size_t)i / 2];
            weights[i] = (i & 1) ? (b & 0x0F) : (b >> 4);
        }
        consumed = 1 + bytes;
    } else {
        /* FSE-compressed weight stream of `header` bytes. */
        int16_t norm[256];
        int max_symbol = 0;
        int table_log = 0;
        size_t ncount_bytes;
        zstd_fse_table fse;
        zstd_rbits bits;
        uint32_t state1;
        uint32_t state2;
        size_t payload = header;

        if (1 + payload > len)
            return 0;
        memset(norm, 0, sizeof(norm));
        ncount_bytes =
            zstd_fse_read_ncount(data + 1, payload, norm, &max_symbol, &table_log, 255, 6);
        if (ncount_bytes == 0 || ncount_bytes >= payload)
            return 0;
        if (!zstd_fse_build(&fse, norm, max_symbol, table_log))
            return 0;
        if (!zstd_rbits_init(&bits, data + 1 + ncount_bytes, payload - ncount_bytes))
            return 0;
        state1 = zstd_rbits_read(&bits, table_log);
        state2 = zstd_rbits_read(&bits, table_log);
        if (!zstd_rbits_ok(&bits))
            return 0;
        num_weights = 0;
        /* Two interleaved states emit weights; when a state update would need
         * more bits than remain, the other state flushes its final symbol. */
        for (;;) {
            zstd_fse_cell c1 = fse.cells[state1];
            if (num_weights >= 255)
                return 0;
            weights[num_weights++] = c1.symbol;
            if (zstd_rbits_remaining(&bits) < (size_t)c1.nbits) {
                zstd_fse_cell c2 = fse.cells[state2];
                if (num_weights >= 255)
                    return 0;
                weights[num_weights++] = c2.symbol;
                break;
            }
            state1 = c1.base + zstd_rbits_read(&bits, c1.nbits);

            zstd_fse_cell c2 = fse.cells[state2];
            if (num_weights >= 255)
                return 0;
            weights[num_weights++] = c2.symbol;
            if (zstd_rbits_remaining(&bits) < (size_t)c2.nbits) {
                zstd_fse_cell c1b = fse.cells[state1];
                if (num_weights >= 255)
                    return 0;
                weights[num_weights++] = c1b.symbol;
                break;
            }
            state2 = c2.base + zstd_rbits_read(&bits, c2.nbits);
        }
        if (!zstd_rbits_ok(&bits))
            return 0;
        consumed = 1 + payload;
    }

    /* The last symbol's weight is implied: it completes the total to the next
     * power of two. */
    for (int i = 0; i < num_weights; i++)
        if (weights[i] > 0)
            total += 1u << (weights[i] - 1);
    if (total == 0)
        return 0;
    {
        uint32_t next_pow2 = 1;
        while (next_pow2 <= total)
            next_pow2 <<= 1;
        uint32_t rest = next_pow2 - total;
        if (rest == 0 || (rest & (rest - 1)) != 0)
            return 0;
        weights[num_weights] = (uint8_t)(zstd_highbit32(rest) + 1);
        num_weights++;
    }
    *out_num_symbols = num_weights;
    return consumed;
}

/// @brief Decode one backward Huffman stream into @p out (exactly @p out_len
///   symbols). Returns 1 on success.
/// @details Each symbol peeks table_log bits (zero padding past the stream
///   start is fine for the peek) but consumes only its code length; the stream
///   is valid when consumption lands exactly on the payload size.
/// @param t Previously validated Huffman lookup table.
/// @param data Complete backward-bitstream payload.
/// @param len Payload size in bytes.
/// @param out Destination for exactly @p out_len decoded symbols.
/// @param out_len Required number of output bytes.
/// @return 1 when exactly the requested symbols and complete stream were
///         consumed, otherwise 0.
static int zstd_huf_decode_stream(
    const zstd_huf_table *t, const uint8_t *data, size_t len, uint8_t *out, size_t out_len) {
    zstd_rbits bits;
    size_t produced = 0;

    if (!zstd_rbits_init(&bits, data, len))
        return 0;
    while (produced < out_len) {
        uint32_t idx;
        zstd_rbits_refill(&bits, t->table_log);
        idx = (uint32_t)((bits.container >> (bits.bits_in_container - t->table_log)) &
                         ((1ull << t->table_log) - 1u));
        out[produced++] = t->symbol[idx];
        bits.bits_in_container -= t->nbits[idx];
        bits.bits_consumed += t->nbits[idx];
        if (!zstd_rbits_ok(&bits))
            return 0;
    }
    return zstd_rbits_fully_consumed(&bits);
}

/*==========================================================================
 * Sequence code tables (RFC 8878 §3.1.1.3.2.1.1)
 *=========================================================================*/

static const uint32_t zstd_ll_base[36] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,   9,   10,  11,   12,   13,   14,   15,    16,    18,
    20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const uint8_t zstd_ll_bits[36] = {0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,
                                         0, 0, 0, 0, 1, 1,  1,  1,  2,  2,  3,  3,
                                         4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint32_t zstd_ml_base[53] = {
    3,  4,  5,  6,  7,  8,  9,  10,  11,  12,  13,   14,   15,   16,   17,    18,    19,   20,
    21, 22, 23, 24, 25, 26, 27, 28,  29,  30,  31,   32,   33,   34,   35,    37,    39,   41,
    43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
static const uint8_t zstd_ml_bits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0, 0,
                                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  1,  1,  1, 1,
                                         2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/* Predefined FSE distributions (RFC 8878 §3.1.1.3.2.2). */
static const int16_t zstd_ll_default[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2,  2,  2,  2,
                                            2, 1, 1, 1, 2, 2, 2, 2, 2,  2,  2,  2,
                                            2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const int16_t zstd_ml_default[53] = {
    1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const int16_t zstd_of_default[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1,  1,  1,  1,  1, 1,
                                            1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

/*==========================================================================
 * Frame decoding
 *=========================================================================*/

/**
 * @brief Mutable state shared by every block in one Zstandard frame.
 * @details Owns or borrows the output destination, reuses literal storage,
 * carries the three repeated offsets, and preserves entropy tables when a
 * later block selects treeless or repeat mode.
 */
typedef struct {
    uint8_t *out;   ///< Destination bytes, either caller-owned or decoder-owned.
    size_t out_len; /* bytes produced so far */
    size_t out_cap; ///< Maximum writable destination size.
    /* Literals buffer for the current block. */
    uint8_t *literals;   ///< Reusable decoded literals for the current block.
    size_t literals_len; ///< Number of current decoded literal bytes.
    size_t literals_cap; ///< Allocated capacity of @ref literals.
    /* Repeated offsets (RFC: initialized to 1, 4, 8). */
    uint32_t rep[3]; ///< Most-recent offset history defined by RFC 8878.
    /* Huffman table persists across blocks for treeless literals. */
    zstd_huf_table huf; ///< Most recently constructed literal Huffman table.
    int huf_valid;      ///< Non-zero when @ref huf is available for reuse.
    /* FSE tables persist across blocks for "repeat" mode. */
    zstd_fse_table ll_fse; ///< Literal-length code table.
    zstd_fse_table ml_fse; ///< Match-length code table.
    zstd_fse_table of_fse; ///< Offset code table.
    int ll_valid;          ///< Whether @ref ll_fse may be repeated.
    int ml_valid;          ///< Whether @ref ml_fse may be repeated.
    int of_valid;          ///< Whether @ref of_fse may be repeated.
} zstd_ctx;

/// @brief Grow the reusable current-block literal buffer when necessary.
/// @details Capacity starts at 4096 bytes and doubles with overflow checks.
///          Existing contents and capacity remain valid when realloc fails.
/// @param ctx Frame context owning the literal buffer.
/// @param need Minimum required literal capacity in bytes.
/// @return 1 when capacity is sufficient, otherwise 0 on overflow/allocation failure.
static int zstd_ensure_literals(zstd_ctx *ctx, size_t need) {
    if (need <= ctx->literals_cap)
        return 1;
    {
        size_t cap = ctx->literals_cap ? ctx->literals_cap : 4096;
        uint8_t *grown;
        while (cap < need) {
            if (cap > (SIZE_MAX >> 1))
                return 0;
            cap <<= 1;
        }
        grown = (uint8_t *)realloc(ctx->literals, cap);
        if (!grown)
            return 0;
        ctx->literals = grown;
        ctx->literals_cap = cap;
    }
    return 1;
}

/// @brief Decode a block's literals section into ctx->literals.
/// @details Supports raw, RLE, compressed, and treeless sections plus one- and
///          four-stream Huffman payloads. Newly described Huffman tables
///          persist in @p ctx for later treeless blocks.
/// @param ctx Current frame context and reusable literals/Huffman state.
/// @param data Complete remaining compressed-block bytes beginning at literals.
/// @param len Number of readable bytes at @p data.
/// @return Encoded literals-section bytes consumed, or zero on error.
static size_t zstd_decode_literals(zstd_ctx *ctx, const uint8_t *data, size_t len) {
    uint8_t type;
    uint8_t size_format;
    size_t regen = 0;
    size_t comp = 0;
    size_t header_bytes = 0;
    int four_streams = 0;

    if (len < 1)
        return 0;
    type = data[0] & 0x3;
    size_format = (data[0] >> 2) & 0x3;

    if (type == 0 || type == 1) {
        /* Raw / RLE literals. */
        switch (size_format) {
            case 0:
            case 2:
                regen = data[0] >> 3;
                header_bytes = 1;
                break;
            case 1:
                if (len < 2)
                    return 0;
                regen = ((size_t)(data[0] >> 4)) | ((size_t)data[1] << 4);
                header_bytes = 2;
                break;
            case 3:
                if (len < 3)
                    return 0;
                regen = ((size_t)(data[0] >> 4)) | ((size_t)data[1] << 4) | ((size_t)data[2] << 12);
                header_bytes = 3;
                break;
        }
        if (!zstd_ensure_literals(ctx, regen))
            return 0;
        if (type == 0) {
            if (header_bytes + regen > len)
                return 0;
            memcpy(ctx->literals, data + header_bytes, regen);
            ctx->literals_len = regen;
            return header_bytes + regen;
        }
        if (header_bytes + 1 > len)
            return 0;
        memset(ctx->literals, data[header_bytes], regen);
        ctx->literals_len = regen;
        return header_bytes + 1;
    }

    /* Compressed (2) or treeless (3) literals. */
    switch (size_format) {
        case 0: /* 1 stream, 10+10 bits */
            if (len < 3)
                return 0;
            regen = ((size_t)(data[0] >> 4)) | (((size_t)data[1] & 0x3F) << 4);
            comp = ((size_t)data[1] >> 6) | ((size_t)data[2] << 2);
            header_bytes = 3;
            four_streams = 0;
            break;
        case 1: /* 4 streams, 10+10 bits */
            if (len < 3)
                return 0;
            regen = ((size_t)(data[0] >> 4)) | (((size_t)data[1] & 0x3F) << 4);
            comp = ((size_t)data[1] >> 6) | ((size_t)data[2] << 2);
            header_bytes = 3;
            four_streams = 1;
            break;
        case 2: /* 4 streams, 14+14 bits */
            if (len < 4)
                return 0;
            regen = ((size_t)(data[0] >> 4)) | ((size_t)data[1] << 4) |
                    (((size_t)data[2] & 0x03) << 12);
            comp = ((size_t)data[2] >> 2) | ((size_t)data[3] << 6);
            header_bytes = 4;
            four_streams = 1;
            break;
        case 3: /* 4 streams, 18+18 bits */
            if (len < 5)
                return 0;
            regen = ((size_t)(data[0] >> 4)) | ((size_t)data[1] << 4) |
                    (((size_t)data[2] & 0x3F) << 12);
            comp = ((size_t)data[2] >> 6) | ((size_t)data[3] << 2) | ((size_t)data[4] << 10);
            header_bytes = 5;
            four_streams = 1;
            break;
    }
    if (header_bytes + comp > len)
        return 0;
    if (!zstd_ensure_literals(ctx, regen))
        return 0;

    {
        const uint8_t *payload = data + header_bytes;
        size_t payload_len = comp;

        if (type == 2) {
            int num_symbols = 0;
            uint8_t weights[256];
            size_t tree_bytes = zstd_huf_read_weights(payload, payload_len, weights, &num_symbols);
            if (tree_bytes == 0 || tree_bytes > payload_len)
                return 0;
            if (!zstd_huf_build(&ctx->huf, weights, num_symbols))
                return 0;
            ctx->huf_valid = 1;
            payload += tree_bytes;
            payload_len -= tree_bytes;
        } else if (!ctx->huf_valid) {
            return 0; /* treeless literals require a previous table */
        }

        if (!four_streams) {
            if (!zstd_huf_decode_stream(&ctx->huf, payload, payload_len, ctx->literals, regen))
                return 0;
        } else {
            size_t seg = (regen + 3) / 4;
            size_t sizes[4];
            size_t off = 0;
            if (payload_len < 6)
                return 0;
            sizes[0] = (size_t)payload[0] | ((size_t)payload[1] << 8);
            sizes[1] = (size_t)payload[2] | ((size_t)payload[3] << 8);
            sizes[2] = (size_t)payload[4] | ((size_t)payload[5] << 8);
            if (sizes[0] + sizes[1] + sizes[2] > payload_len - 6)
                return 0;
            sizes[3] = payload_len - 6 - sizes[0] - sizes[1] - sizes[2];
            payload += 6;
            for (int stream = 0; stream < 4; stream++) {
                size_t want = stream == 3 ? regen - 3 * seg : seg;
                if (stream == 3 && 3 * seg > regen)
                    return 0;
                if (!zstd_huf_decode_stream(&ctx->huf,
                                            payload + off,
                                            sizes[stream],
                                            ctx->literals + stream * seg,
                                            want))
                    return 0;
                off += sizes[stream];
            }
        }
        ctx->literals_len = regen;
    }
    return header_bytes + comp;
}

/// @brief Resolve a sequence-table compression mode into a usable FSE table.
/// @details Handles predefined, single-symbol RLE, newly described FSE, and
///          repeat-previous modes while updating the caller's persistence flag.
/// @param ctx Current frame context; reserved for shared sequence state.
/// @param data Encoded table bytes for modes that consume input.
/// @param len Number of readable bytes at @p data.
/// @param mode Two-bit sequence-table mode from the block header.
/// @param table Destination or previously persisted FSE table.
/// @param valid_flag Persistence flag updated for newly usable FSE tables.
/// @param default_norm Predefined normalized distribution for mode zero.
/// @param default_max_symbol Highest symbol in @p default_norm.
/// @param default_log Table log for the predefined distribution.
/// @param max_symbol_limit Largest symbol accepted for this sequence alphabet.
/// @param max_log Largest table log accepted for this sequence alphabet.
/// @param rle_symbol Receives the single symbol for RLE mode.
/// @param is_rle Set to one for RLE mode and zero otherwise.
/// @return Consumed bytes from @p data (0 is valid for non-FSE modes);
///         (size_t)-1 on error.
static size_t zstd_setup_seq_table(zstd_ctx *ctx,
                                   const uint8_t *data,
                                   size_t len,
                                   int mode,
                                   zstd_fse_table *table,
                                   int *valid_flag,
                                   const int16_t *default_norm,
                                   int default_max_symbol,
                                   int default_log,
                                   int max_symbol_limit,
                                   int max_log,
                                   uint8_t *rle_symbol,
                                   int *is_rle) {
    *is_rle = 0;
    switch (mode) {
        case 0: /* predefined */
            if (!zstd_fse_build(table, default_norm, default_max_symbol, default_log))
                return (size_t)-1;
            *valid_flag = 1;
            return 0;
        case 1: /* RLE: one byte symbol */
            if (len < 1)
                return (size_t)-1;
            *rle_symbol = data[0];
            if (*rle_symbol > max_symbol_limit)
                return (size_t)-1;
            *is_rle = 1;
            return 1;
        case 2: { /* FSE-described */
            int16_t norm[256];
            int max_symbol = 0;
            int table_log = 0;
            size_t consumed;
            memset(norm, 0, sizeof(norm));
            consumed = zstd_fse_read_ncount(
                data, len, norm, &max_symbol, &table_log, max_symbol_limit, max_log);
            if (consumed == 0)
                return (size_t)-1;
            if (!zstd_fse_build(table, norm, max_symbol, table_log))
                return (size_t)-1;
            *valid_flag = 1;
            return consumed;
        }
        case 3: /* repeat previous */
            if (!*valid_flag)
                return (size_t)-1;
            return 0;
    }
    return (size_t)-1;
}

/// @brief Execute one compressed block's sequences against the literals buffer.
/// @details Decodes literal-length, offset, and match-length FSE states,
///          resolves repeated offsets, copies literal runs and overlapping
///          matches within the fixed output budget, and appends trailing
///          literals. The backward sequence bitstream must be consumed exactly.
/// @param ctx Current frame context containing literals, tables, repeats, and output.
/// @param data Encoded sequence section following the literals section.
/// @param len Number of readable sequence bytes.
/// @return 1 when all sequences decode within bounds, otherwise 0.
static int zstd_decode_sequences(zstd_ctx *ctx, const uint8_t *data, size_t len) {
    size_t pos = 0;
    uint32_t num_sequences;
    int ll_mode;
    int of_mode;
    int ml_mode;
    uint8_t ll_rle = 0;
    uint8_t of_rle = 0;
    uint8_t ml_rle = 0;
    int ll_is_rle = 0;
    int of_is_rle = 0;
    int ml_is_rle = 0;
    size_t literals_consumed = 0;

    if (len < 1)
        return 0;
    /* Sequence count: 1-3 byte varint. */
    if (data[0] < 128) {
        num_sequences = data[0];
        pos = 1;
    } else if (data[0] < 255) {
        if (len < 2)
            return 0;
        num_sequences = (uint32_t)((data[0] - 128) << 8) + data[1];
        pos = 2;
    } else {
        if (len < 3)
            return 0;
        num_sequences = (uint32_t)data[1] + ((uint32_t)data[2] << 8) + 0x7F00u;
        pos = 3;
    }
    if (num_sequences == 0) {
        /* All literals copy straight to the output. */
        if (ctx->out_len + ctx->literals_len > ctx->out_cap)
            return 0;
        memcpy(ctx->out + ctx->out_len, ctx->literals, ctx->literals_len);
        ctx->out_len += ctx->literals_len;
        return 1;
    }
    if (num_sequences > ZSTD_MAX_SEQUENCES)
        return 0;

    if (pos >= len)
        return 0;
    {
        uint8_t modes = data[pos++];
        ll_mode = (modes >> 6) & 3;
        of_mode = (modes >> 4) & 3;
        ml_mode = (modes >> 2) & 3;
    }
    {
        size_t used;
        used = zstd_setup_seq_table(ctx,
                                    data + pos,
                                    len - pos,
                                    ll_mode,
                                    &ctx->ll_fse,
                                    &ctx->ll_valid,
                                    zstd_ll_default,
                                    35,
                                    6,
                                    ZSTD_MAX_LL_SYMBOL,
                                    9,
                                    &ll_rle,
                                    &ll_is_rle);
        if (used == (size_t)-1)
            return 0;
        pos += used;
        used = zstd_setup_seq_table(ctx,
                                    data + pos,
                                    len - pos,
                                    of_mode,
                                    &ctx->of_fse,
                                    &ctx->of_valid,
                                    zstd_of_default,
                                    28,
                                    5,
                                    ZSTD_MAX_OF_SYMBOL,
                                    8,
                                    &of_rle,
                                    &of_is_rle);
        if (used == (size_t)-1)
            return 0;
        pos += used;
        used = zstd_setup_seq_table(ctx,
                                    data + pos,
                                    len - pos,
                                    ml_mode,
                                    &ctx->ml_fse,
                                    &ctx->ml_valid,
                                    zstd_ml_default,
                                    52,
                                    6,
                                    ZSTD_MAX_ML_SYMBOL,
                                    9,
                                    &ml_rle,
                                    &ml_is_rle);
        if (used == (size_t)-1)
            return 0;
        pos += used;
    }

    {
        zstd_rbits bits;
        uint32_t ll_state = 0;
        uint32_t of_state = 0;
        uint32_t ml_state = 0;

        if (!zstd_rbits_init(&bits, data + pos, len - pos))
            return 0;
        if (!ll_is_rle)
            ll_state = zstd_rbits_read(&bits, ctx->ll_fse.table_log);
        if (!of_is_rle)
            of_state = zstd_rbits_read(&bits, ctx->of_fse.table_log);
        if (!ml_is_rle)
            ml_state = zstd_rbits_read(&bits, ctx->ml_fse.table_log);

        for (uint32_t seq = 0; seq < num_sequences; seq++) {
            uint8_t ll_code = ll_is_rle ? ll_rle : ctx->ll_fse.cells[ll_state].symbol;
            uint8_t of_code = of_is_rle ? of_rle : ctx->of_fse.cells[of_state].symbol;
            uint8_t ml_code = ml_is_rle ? ml_rle : ctx->ml_fse.cells[ml_state].symbol;
            uint32_t offset_value;
            uint32_t match_length;
            uint32_t literal_length;
            uint32_t offset;

            if (ll_code > ZSTD_MAX_LL_SYMBOL || ml_code > ZSTD_MAX_ML_SYMBOL ||
                of_code > ZSTD_MAX_OF_SYMBOL || of_code > 30)
                return 0;

            /* Extra bits are read offset, match, literals — in that order. */
            offset_value = (1u << of_code) + zstd_rbits_read(&bits, of_code);
            match_length = zstd_ml_base[ml_code] + zstd_rbits_read(&bits, zstd_ml_bits[ml_code]);
            literal_length = zstd_ll_base[ll_code] + zstd_rbits_read(&bits, zstd_ll_bits[ll_code]);

            /* Repeated-offset resolution (RFC 8878 §3.1.1.5): values 1-3 select
             * a repeat slot (shifted by one when the literal run is empty); the
             * chosen offset always moves to the front of the repeat list. */
            if (offset_value <= 3) {
                uint32_t rep_idx = offset_value - 1 + (literal_length == 0 ? 1u : 0u);
                if (rep_idx == 0) {
                    offset = ctx->rep[0];
                } else if (rep_idx == 1) {
                    offset = ctx->rep[1];
                    ctx->rep[1] = ctx->rep[0];
                    ctx->rep[0] = offset;
                } else if (rep_idx == 2) {
                    offset = ctx->rep[2];
                    ctx->rep[2] = ctx->rep[1];
                    ctx->rep[1] = ctx->rep[0];
                    ctx->rep[0] = offset;
                } else { /* rep_idx == 3: Repeated_Offset1 - 1 */
                    offset = ctx->rep[0] - 1;
                    if (offset == 0)
                        return 0;
                    ctx->rep[2] = ctx->rep[1];
                    ctx->rep[1] = ctx->rep[0];
                    ctx->rep[0] = offset;
                }
            } else {
                offset = offset_value - 3;
                ctx->rep[2] = ctx->rep[1];
                ctx->rep[1] = ctx->rep[0];
                ctx->rep[0] = offset;
            }
            if (offset == 0)
                return 0;

            /* Copy literals. */
            if (literals_consumed + literal_length > ctx->literals_len)
                return 0;
            if (ctx->out_len + literal_length > ctx->out_cap)
                return 0;
            memcpy(ctx->out + ctx->out_len, ctx->literals + literals_consumed, literal_length);
            ctx->out_len += literal_length;
            literals_consumed += literal_length;

            /* Copy match (byte-by-byte: overlapping copies are the norm). */
            if (offset > ctx->out_len)
                return 0;
            if (ctx->out_len + match_length > ctx->out_cap)
                return 0;
            for (uint32_t m = 0; m < match_length; m++) {
                ctx->out[ctx->out_len] = ctx->out[ctx->out_len - offset];
                ctx->out_len++;
            }

            /* Update states (skipped after the final sequence). */
            if (seq + 1 < num_sequences) {
                if (!ll_is_rle) {
                    zstd_fse_cell c = ctx->ll_fse.cells[ll_state];
                    ll_state = c.base + zstd_rbits_read(&bits, c.nbits);
                }
                if (!ml_is_rle) {
                    zstd_fse_cell c = ctx->ml_fse.cells[ml_state];
                    ml_state = c.base + zstd_rbits_read(&bits, c.nbits);
                }
                if (!of_is_rle) {
                    zstd_fse_cell c = ctx->of_fse.cells[of_state];
                    of_state = c.base + zstd_rbits_read(&bits, c.nbits);
                }
            }
            if (!zstd_rbits_ok(&bits))
                return 0;
        }
        if (!zstd_rbits_fully_consumed(&bits))
            return 0;

        /* Trailing literals after the last sequence. */
        {
            size_t rest = ctx->literals_len - literals_consumed;
            if (ctx->out_len + rest > ctx->out_cap)
                return 0;
            memcpy(ctx->out + ctx->out_len, ctx->literals + literals_consumed, rest);
            ctx->out_len += rest;
        }
    }
    return 1;
}

/// @brief Shared Zstandard frame worker for allocated and exact-destination decoding.
/// @details When @p fixed_output is non-NULL the worker borrows that exact-size destination;
///          otherwise it allocates the bounded output and transfers it through @p out_data. Both
///          modes require complete frame consumption and run the same dictionary, bounds, entropy,
///          content-size, and checksum validation.
/// @param data Complete frame bytes.
/// @param len Frame byte count.
/// @param max_output Maximum allocation/output size for allocating mode.
/// @param fixed_output Optional caller-owned exact destination.
/// @param fixed_size Exact required output size when @p fixed_output is non-NULL.
/// @param out_data Receives allocated output in allocating mode; ignored in fixed mode.
/// @param out_len Receives decoded bytes in allocating mode; ignored in fixed mode.
/// @return 1 on complete success, otherwise 0.
static int zstd_decompress_impl(const uint8_t *data,
                                size_t len,
                                size_t max_output,
                                uint8_t *fixed_output,
                                size_t fixed_size,
                                uint8_t **out_data,
                                size_t *out_len) {
    size_t pos = 0;
    uint8_t fhd;
    int single_segment;
    int checksum_flag;
    int dict_id_size;
    int fcs_size;
    uint64_t content_size = 0;
    int content_size_known = 0;
    size_t out_cap;
    zstd_ctx ctx;
    int ok = 0;
    int owns_output = fixed_output ? 0 : 1;

    if (out_data)
        *out_data = NULL;
    if (out_len)
        *out_len = 0;
    if (!data || len < 5 || (!fixed_output && (!out_data || !out_len)))
        return 0;
    if (((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24)) != ZSTD_MAGIC)
        return 0;
    pos = 4;

    fhd = data[pos++];
    single_segment = (fhd >> 5) & 1;
    checksum_flag = (fhd >> 2) & 1;
    dict_id_size = fhd & 3;
    if (dict_id_size)
        return 0; /* dictionaries unsupported (never used by KTX2) */
    fcs_size = fhd >> 6;
    if (!single_segment) {
        if (pos >= len)
            return 0;
        pos++; /* window descriptor: one-shot decode ignores the window hint */
    }
    switch (fcs_size) {
        case 0:
            if (single_segment) {
                if (pos >= len)
                    return 0;
                content_size = data[pos++];
                content_size_known = 1;
            }
            break;
        case 1:
            if (pos + 2 > len)
                return 0;
            content_size = 256u + ((uint64_t)data[pos] | ((uint64_t)data[pos + 1] << 8));
            pos += 2;
            content_size_known = 1;
            break;
        case 2:
            if (pos + 4 > len)
                return 0;
            content_size = (uint64_t)data[pos] | ((uint64_t)data[pos + 1] << 8) |
                           ((uint64_t)data[pos + 2] << 16) | ((uint64_t)data[pos + 3] << 24);
            pos += 4;
            content_size_known = 1;
            break;
        case 3:
            if (pos + 8 > len)
                return 0;
            content_size = 0;
            for (int i = 7; i >= 0; i--)
                content_size = (content_size << 8) | data[pos + i];
            pos += 8;
            content_size_known = 1;
            break;
    }
    if (content_size_known && content_size > (uint64_t)max_output)
        return 0;
    out_cap = fixed_output ? fixed_size : (content_size_known ? (size_t)content_size : max_output);
    if (out_cap > max_output)
        return 0;
    if (fixed_output && content_size_known && content_size != (uint64_t)fixed_size)
        return 0;

    memset(&ctx, 0, sizeof(ctx));
    if (fixed_output) {
        ctx.out = fixed_output;
    } else {
        ctx.out = (uint8_t *)malloc(out_cap ? out_cap : 1);
        if (!ctx.out)
            return 0;
    }
    ctx.out_cap = out_cap;
    ctx.rep[0] = 1;
    ctx.rep[1] = 4;
    ctx.rep[2] = 8;

    for (;;) {
        uint32_t block_header;
        int last_block;
        int block_type;
        uint32_t block_size;

        if (pos + 3 > len)
            goto done;
        block_header =
            (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) | ((uint32_t)data[pos + 2] << 16);
        pos += 3;
        last_block = block_header & 1;
        block_type = (block_header >> 1) & 3;
        block_size = block_header >> 3;

        switch (block_type) {
            case 0: /* raw */
                if (pos + block_size > len || ctx.out_len + block_size > ctx.out_cap)
                    goto done;
                memcpy(ctx.out + ctx.out_len, data + pos, block_size);
                ctx.out_len += block_size;
                pos += block_size;
                break;
            case 1: /* RLE: block_size copies of one byte */
                if (pos + 1 > len || ctx.out_len + block_size > ctx.out_cap)
                    goto done;
                memset(ctx.out + ctx.out_len, data[pos], block_size);
                ctx.out_len += block_size;
                pos += 1;
                break;
            case 2: { /* compressed */
                size_t lit_bytes;
                if (pos + block_size > len)
                    goto done;
                lit_bytes = zstd_decode_literals(&ctx, data + pos, block_size);
                if (lit_bytes == 0 || lit_bytes > block_size)
                    goto done;
                if (!zstd_decode_sequences(&ctx, data + pos + lit_bytes, block_size - lit_bytes))
                    goto done;
                pos += block_size;
                break;
            }
            default:
                goto done; /* reserved */
        }
        if (last_block)
            break;
    }

    if (content_size_known && ctx.out_len != (size_t)content_size)
        goto done;
    if (fixed_output && ctx.out_len != fixed_size)
        goto done;
    if (checksum_flag) {
        uint32_t stored;
        if (pos + 4 > len)
            goto done;
        stored = (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) |
                 ((uint32_t)data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24);
        if ((uint32_t)(xxhash64(ctx.out, ctx.out_len) & 0xFFFFFFFFu) != stored)
            goto done;
        pos += 4;
    }
    if (pos != len)
        goto done;

    if (!fixed_output) {
        *out_data = ctx.out;
        *out_len = ctx.out_len;
        ctx.out = NULL;
    }
    ok = 1;

done:
    if (owns_output)
        free(ctx.out);
    free(ctx.literals);
    return ok;
}

/// @brief Decompress one complete Zstandard frame into a malloc-owned buffer.
/// @details Compatibility wrapper retaining the established bounded allocation
///          API. Output pointers are reset before parsing. A successful empty
///          frame still returns a freeable non-NULL allocation.
/// @param data Complete standard Zstandard frame bytes.
/// @param len Number of readable bytes at @p data.
/// @param max_output Maximum permitted decompressed size and allocation bound.
/// @param out_data Receives malloc-owned decoded bytes on success.
/// @param out_len Receives the exact decoded byte count on success.
/// @return 1 on complete success, otherwise 0 with no output allocation returned.
int rt_zstd_decompress_raw(
    const uint8_t *data, size_t len, size_t max_output, uint8_t **out_data, size_t *out_len) {
    return zstd_decompress_impl(data, len, max_output, NULL, 0, out_data, out_len);
}

/// @brief Decompress one complete Zstandard frame into exact caller-owned storage.
/// @details The output buffer is borrowed and remains caller-owned on both success and failure;
///          only temporary entropy/literal state can allocate. The exact-size contract rejects
///          both short and overproducing frames. Contents may be partially
///          modified when decoding ultimately fails.
/// @param data Complete standard Zstandard frame bytes.
/// @param len Number of readable bytes at @p data.
/// @param output Caller-owned destination for exactly @p output_size bytes.
/// @param output_size Required decompressed size and destination capacity.
/// @return 1 only for exact complete-frame decoding, otherwise 0.
int rt_zstd_decompress_into(const uint8_t *data, size_t len, uint8_t *output, size_t output_size) {
    if (!output)
        return 0;
    return zstd_decompress_impl(data, len, output_size, output, output_size, NULL, NULL);
}
