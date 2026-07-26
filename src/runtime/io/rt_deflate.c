//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_deflate.c
// Purpose: DEFLATE compression (RFC 1951) + gzip framing: LZ77 match finder,
//   fixed/stored Huffman block emission, and the bit writer. Inflate lives in
//   rt_compress.c.
//
// Key invariants:
//   - Level 1 and inputs of at most 64 bytes use stored blocks.
//   - Levels 2-5 use fixed Huffman coding; levels 6-9 also try dynamic
//     Huffman coding and keep it only when smaller.
//   - Match distances never exceed the RFC 1951 32 KiB history window.
//   - GZIP trailers contain CRC-32 and input length modulo 2^32.
//
// Ownership/Lifetime:
//   - Input byte spans are borrowed for each call.
//   - Native writer/hash buffers are temporary; successful public worker
//     results are fresh GC-managed Bytes objects.
//
// Links: src/runtime/io/rt_compress_internal.h,
//        src/runtime/io/rt_compress.c,
//        src/runtime/io/rt_compress.h
//
//===----------------------------------------------------------------------===//

#include "rt_bytes.h"
#include "rt_compress.h"
#include "rt_compress_internal.h"
#include "rt_crc32.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Bit Stream Writer (for compression)
//=============================================================================

/// @brief LSB-first bit-stream writer used by the DEFLATE compressor.
typedef struct {
    uint8_t *data;   ///< Output byte buffer (heap-allocated, grown by bw_ensure).
    size_t capacity; ///< Allocated capacity of `data` in bytes.
    size_t len;      ///< Number of complete bytes written to `data`.
    uint32_t buffer; ///< Pending bit accumulator (LSB = next bit to emit).
    int bits_in_buf; ///< Number of valid bits currently in `buffer`.
} bit_writer_t;

/// @brief Initialize a bit writer with a starting buffer capacity.
///
/// Allocates `initial_cap` bytes (clamped to a 256-byte minimum) and
/// zeros the bit accumulator. Traps on OOM.
/// @param bw Writer state to initialize.
/// @param initial_cap Initial encoded-byte capacity estimate.
/// @return 1 on success, or 0 after raising an allocation trap.
static int bw_init(bit_writer_t *bw, size_t initial_cap) {
    bw->capacity = initial_cap > 256 ? initial_cap : 256;
    bw->data = (uint8_t *)malloc(bw->capacity);
    if (!bw->data) {
        rt_trap("rt_compress: memory allocation failed");
        bw->capacity = 0;
        bw->len = 0;
        bw->buffer = 0;
        bw->bits_in_buf = 0;
        return 0;
    }
    bw->len = 0;
    bw->buffer = 0;
    bw->bits_in_buf = 0;
    return 1;
}

/// @brief Grow the bit writer's byte buffer so `need` more bytes will fit.
///
/// Doubles capacity geometrically with an overflow guard, falling back to the
/// exact required length near `SIZE_MAX`. Traps on OOM.
/// @param bw Writer whose byte buffer may be reallocated.
/// @param need Additional complete bytes the caller will append.
/// @return 1 when sufficient capacity exists; otherwise 0 after a trap.
static int bw_ensure(bit_writer_t *bw, size_t need) {
    if (need > SIZE_MAX - bw->len) {
        rt_trap("Compress: output size overflow");
        return 0;
    }
    size_t required = bw->len + need;
    if (required <= bw->capacity)
        return 1;
    size_t new_cap = bw->capacity ? bw->capacity : 256;
    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }
    uint8_t *new_data = (uint8_t *)realloc(bw->data, new_cap);
    if (!new_data) {
        rt_trap("Compress: out of memory");
        return 0;
    }
    bw->data = new_data;
    bw->capacity = new_cap;
    return 1;
}

/// @brief Append a low-order bit field in DEFLATE's LSB-first order.
/// @param bw Initialized bit writer.
/// @param val Value whose low @p n bits are emitted.
/// @param n Number of bits to append.
/// @return 1 on success, or 0 after output growth failure.
static int bw_write(bit_writer_t *bw, uint32_t val, int n) {
    bw->buffer |= val << bw->bits_in_buf;
    bw->bits_in_buf += n;
    while (bw->bits_in_buf >= 8) {
        if (!bw_ensure(bw, 1))
            return 0;
        bw->data[bw->len++] = bw->buffer & 0xFF;
        bw->buffer >>= 8;
        bw->bits_in_buf -= 8;
    }
    return 1;
}

/// @brief Flush a partial output byte, padding its high bits with zero.
/// @param bw Writer to byte-align.
/// @return 1 on success, or 0 after output growth failure.
static int bw_flush(bit_writer_t *bw) {
    if (bw->bits_in_buf > 0) {
        if (!bw_ensure(bw, 1))
            return 0;
        bw->data[bw->len++] = bw->buffer & 0xFF;
        bw->buffer = 0;
        bw->bits_in_buf = 0;
    }
    return 1;
}

/// @brief Append raw bytes to an already byte-aligned writer.
/// @param bw Writer whose accumulator has no pending bits.
/// @param data Source bytes; may be `NULL` only when @p len is zero.
/// @param len Number of bytes to append.
/// @return 1 on success, or 0 after output growth failure.
static int bw_write_bytes(bit_writer_t *bw, const uint8_t *data, size_t len) {
    if (!bw_ensure(bw, len))
        return 0;
    memcpy(bw->data + bw->len, data, len);
    bw->len += len;
    return 1;
}

/// @brief Release the bit writer's heap buffer.
///
/// Should be called once the assembled bytes have been copied out
/// (see `deflate_data`). The struct itself is caller-owned.
/// @param bw Writer whose native output allocation is released and reset.
static void bw_free(bit_writer_t *bw) {
    free(bw->data);
    bw->data = NULL;
    bw->capacity = 0;
    bw->len = 0;
}

//=============================================================================
// DEFLATE Compression
//=============================================================================

/// @brief Hash-chain tables for the LZ77 sliding-window match finder.
typedef struct {
    int *head;      ///< Maps each 3-byte-hash to the most recent matching position.
    int *prev;      ///< Back-chain linking older positions with the same hash.
    int window_pos; ///< Current write position within the 32KB sliding window.
} lz77_state_t;

#define HASH_BITS 15
#define HASH_SIZE (1 << HASH_BITS)
#define HASH_MASK (HASH_SIZE - 1)
#define NIL (-1)

/// @brief Compute the match-finder hash of a three-byte prefix.
/// @param data Pointer to at least three accessible bytes.
/// @return Hash-table index in `[0, HASH_SIZE)`.
static inline int compute_hash(const uint8_t *data) {
    return ((data[0] << 10) ^ (data[1] << 5) ^ data[2]) & HASH_MASK;
}

/// @brief Allocate the LZ77 hash-chain tables for match finding.
///
/// Two arrays back the sliding-window match search: `head` (HASH_SIZE
/// slots) maps a 3-byte-prefix hash to the most recent position seen,
/// and `prev` (WINDOW_SIZE slots) chains older positions sharing the
/// same hash. NIL (-1) marks unused slots. Together they form a
/// hash-chained lookup that `find_match` walks to locate the longest
/// back-reference. Traps on OOM.
/// @param lz Match-finder state to initialize.
/// @return `true` on success, or `false` after raising an allocation trap.
static bool lz77_init(lz77_state_t *lz) {
    lz->head = (int *)malloc(HASH_SIZE * sizeof(int));
    lz->prev = (int *)malloc(WINDOW_SIZE * sizeof(int));
    if (!lz->head || !lz->prev) {
        free(lz->head);
        free(lz->prev);
        lz->head = NULL;
        lz->prev = NULL;
        rt_trap("Compress: memory allocation failed");
        return false;
    }
    for (int i = 0; i < HASH_SIZE; i++)
        lz->head[i] = NIL;
    for (int i = 0; i < WINDOW_SIZE; i++)
        lz->prev[i] = NIL;
    lz->window_pos = 0;
    return true;
}

/// @brief Release LZ77 hash-chain memory (head + prev arrays).
/// @param lz State whose two native arrays are released.
static void lz77_free(lz77_state_t *lz) {
    free(lz->head);
    free(lz->prev);
}

/// @brief Find the longest LZ77 back-reference match at `pos`.
///
/// Hashes the 3 bytes at `pos`, walks the hash chain of prior positions
/// with the same prefix, and returns the longest match found (up to
/// `MAX_MATCH_LEN = 258`). `max_chain` caps the walk depth — higher
/// compression levels use longer chains (`4 << level`) for better
/// matches at the cost of speed. Refuses matches with distance
/// exceeding 32KB or below `MIN_MATCH_LEN = 3` (short matches cost
/// more bits than the literals they replace).
/// @param lz Initialized hash-chain state containing earlier positions.
/// @param data Complete borrowed input buffer.
/// @param pos Current input offset.
/// @param len Total input byte count.
/// @param max_chain Maximum candidate positions to inspect.
/// @param match_dist Receives the backward distance of the selected match.
/// @return Best match length from 3 through 258, or 0 when no encodable match
/// exists.
static int find_match(
    lz77_state_t *lz, const uint8_t *data, size_t pos, size_t len, int max_chain, int *match_dist) {
    if (pos + MIN_MATCH_LEN > len)
        return 0;

    int hash = compute_hash(data + pos);
    int chain_pos = lz->head[hash];
    int best_len = MIN_MATCH_LEN - 1;
    int best_dist = 0;

    int limit = (int)pos - MAX_DISTANCE;
    if (limit < 0)
        limit = 0;

    while (chain_pos >= limit && max_chain-- > 0) {
        // Check match
        int match_len = 0;
        size_t a = pos, b = chain_pos;
        size_t max_len = len - pos;
        if (max_len > MAX_MATCH_LEN)
            max_len = MAX_MATCH_LEN;

        while (match_len < (int)max_len && data[a] == data[b]) {
            match_len++;
            a++;
            b++;
        }

        if (match_len > best_len) {
            best_len = match_len;
            best_dist = (int)(pos - chain_pos);
            if (best_len >= MAX_MATCH_LEN)
                break;
        }

        chain_pos = lz->prev[chain_pos & WINDOW_MASK];
    }

    *match_dist = best_dist;
    return best_len >= MIN_MATCH_LEN ? best_len : 0;
}

/// @brief Insert `pos` at the head of the hash chain for its 3-byte prefix.
///
/// Maintains the `prev[pos] = head[hash]; head[hash] = pos` invariant
/// that lets `find_match` walk back through older occurrences. Called
/// after emitting a literal or (per byte) inside a match so later
/// positions can find it.
/// @param lz Initialized hash-chain state.
/// @param data Complete borrowed input buffer.
/// @param pos Position with at least three accessible bytes.
static void update_hash(lz77_state_t *lz, const uint8_t *data, size_t pos) {
    int hash = compute_hash(data + pos);
    lz->prev[pos & WINDOW_MASK] = lz->head[hash];
    lz->head[hash] = (int)pos;
}

/// @brief Map an LZ77 match length (3..258) to the DEFLATE length code (257..285).
///
/// Scans the `length_base` table for the first entry whose next slot
/// exceeds `length`, which identifies the code whose range covers it.
/// Lengths of exactly 258 are encoded as code 285 (the sentinel final
/// entry, which has zero extra bits).
/// @param length Encodable match length from 3 through 258.
/// @return Literal/length alphabet code from 257 through 285.
static int get_length_code(int length) {
    for (int i = 0; i < 29; i++) {
        if (i == 28)
            return 285;
        if (length < length_base[i + 1])
            return 257 + i;
    }
    return 285;
}

/// @brief Map a back-reference distance (1..32768) to the DEFLATE distance code (0..29).
///
/// Linear scan of `dist_base`; the extra-bits table on the matching
/// code handles the offset within each code's range. 30 distance
/// codes cover the full 32KB window.
/// @param dist Encodable backward distance from 1 through 32,768.
/// @return Distance alphabet code from 0 through 29.
static int get_dist_code(int dist) {
    for (int i = 0; i < 30; i++) {
        if (i == 29 || dist < dist_base[i + 1])
            return i;
    }
    return 29;
}

/// @brief Emit a canonical Huffman code with DEFLATE's LSB-first bit order.
///
/// Canonical Huffman codes are conceptually MSB-first, but DEFLATE's
/// bit stream reads LSB-first — so the code must be bit-reversed
/// before `bw_write` emits it LSB-first and the decoder's
/// `decode_symbol` reassembles the original MSB-first prefix.
/// @param bw Destination bit writer.
/// @param code Canonical MSB-first code value.
/// @param len Code length in bits.
/// @return 1 on success, or 0 after writer growth failure.
static int write_code(bit_writer_t *bw, uint16_t code, int len) {
    // Reverse the code bits
    uint16_t rev = 0;
    for (int i = 0; i < len; i++) {
        if (code & (1 << i))
            rev |= 1 << (len - 1 - i);
    }
    return bw_write(bw, rev, len);
}

/// @brief Emit a sequence of DEFLATE type-0 (stored/uncompressed) blocks.
///
/// Used at level 1 or for very small payloads where Huffman overhead
/// would exceed any savings. Each block carries up to 65535 bytes (LEN
/// is a u16), so large inputs are chunked across multiple back-to-back
/// stored blocks with BFINAL=0 until the last one. An empty input
/// still needs one final block so the stream is well-formed — handled
/// as a special case up front.
/// @param bw Destination bit writer.
/// @param data Borrowed input bytes; may be `NULL` when @p len is zero.
/// @param len Input byte count.
/// @return 1 after emitting all stored blocks; otherwise 0.
static int deflate_stored(bit_writer_t *bw, const uint8_t *data, size_t len) {
    // Handle empty data - still need a final block
    if (len == 0) {
        // Block header
        if (!bw_write(bw, 1, 1) || !bw_write(bw, 0, 2))
            return 0;

        // Align to byte
        if (!bw_flush(bw))
            return 0;

        // LEN = 0 and NLEN = 0xFFFF
        if (!bw_write(bw, 0x00, 8) || !bw_write(bw, 0x00, 8) || !bw_write(bw, 0xFF, 8) ||
            !bw_write(bw, 0xFF, 8))
            return 0;
        return 1;
    }

    size_t pos = 0;
    while (pos < len) {
        size_t block_len = len - pos;
        if (block_len > 65535)
            block_len = 65535;

        bool last = (pos + block_len >= len);

        // Block header
        if (!bw_write(bw, last ? 1 : 0, 1) || !bw_write(bw, 0, 2))
            return 0;

        // Align to byte
        if (!bw_flush(bw))
            return 0;

        // LEN and NLEN
        uint16_t nlen = ~(uint16_t)block_len;
        if (!bw_write(bw, block_len & 0xFF, 8) || !bw_write(bw, (block_len >> 8) & 0xFF, 8) ||
            !bw_write(bw, nlen & 0xFF, 8) || !bw_write(bw, (nlen >> 8) & 0xFF, 8))
            return 0;

        // Data
        if (!bw_flush(bw) || !bw_write_bytes(bw, data + pos, block_len))
            return 0;

        pos += block_len;
    }
    return 1;
}

/// @brief Emit a single DEFLATE type-1 (fixed Huffman) block with LZ77 matching.
///
/// At each input position, queries `find_match` for the longest
/// back-reference. If the match meets the minimum length, emits a
/// length-code + distance-code pair (with their extra-bits suffixes)
/// using the canonical fixed Huffman assignment — literals 0..143
/// use 8-bit codes 0x30..0xBF, 144..255 use 9-bit codes, end-of-block
/// is 7 bits, etc. Otherwise falls back to a literal. Hash chain is
/// advanced one byte per input regardless so future positions can
/// still find matches even inside earlier matches.
/// @param bw Destination bit writer.
/// @param data Borrowed input bytes.
/// @param len Input byte count.
/// @param level Compression level controlling hash-chain search depth.
/// @return 1 after emitting the complete final block; otherwise 0.
static int deflate_fixed(bit_writer_t *bw, const uint8_t *data, size_t len, int level) {
    init_fixed_trees();

    lz77_state_t lz;
    if (!lz77_init(&lz))
        return 0;

    int max_chain = 4 << level;

    // Block header
    if (!bw_write(bw, 1, 1) || !bw_write(bw, 1, 2)) { // BFINAL=1, BTYPE=fixed
        lz77_free(&lz);
        return 0;
    }

    // Fixed literal/length codes
    // 0-143: 8 bits, 144-255: 9 bits, 256-279: 7 bits, 280-287: 8 bits

    size_t pos = 0;
    while (pos < len) {
        int match_dist = 0;
        int match_len = 0;

        if (pos + MIN_MATCH_LEN <= len) {
            match_len = find_match(&lz, data, pos, len, max_chain, &match_dist);
        }

        if (match_len >= MIN_MATCH_LEN) {
            // Write length code
            int len_code = get_length_code(match_len);
            int len_idx = len_code - 257;

            // Encode length code with fixed Huffman
            if (len_code <= 279) {
                // 7 bits: codes 256-279 map to 0000000-0010111
                if (!write_code(bw, (uint16_t)(len_code - 256), 7)) {
                    lz77_free(&lz);
                    return 0;
                }
            } else {
                // 8 bits: codes 280-287 map to 11000000-11000111
                if (!write_code(bw, (uint16_t)(0xC0 + (len_code - 280)), 8)) {
                    lz77_free(&lz);
                    return 0;
                }
            }

            // Extra length bits
            if (length_extra_bits[len_idx] > 0) {
                if (!bw_write(bw, match_len - length_base[len_idx], length_extra_bits[len_idx])) {
                    lz77_free(&lz);
                    return 0;
                }
            }

            // Distance code (5 bits for fixed)
            int dist_code = get_dist_code(match_dist);
            if (!write_code(bw, (uint16_t)dist_code, 5)) {
                lz77_free(&lz);
                return 0;
            }

            // Extra distance bits
            if (dist_extra_bits[dist_code] > 0) {
                if (!bw_write(bw, match_dist - dist_base[dist_code], dist_extra_bits[dist_code])) {
                    lz77_free(&lz);
                    return 0;
                }
            }

            // Update hash for all matched bytes
            for (int i = 0; i < match_len; i++) {
                if (pos + i + MIN_MATCH_LEN <= len)
                    update_hash(&lz, data, pos + i);
            }
            pos += match_len;
        } else {
            // Literal byte
            uint8_t lit = data[pos];
            if (lit <= 143) {
                // 8 bits: 00110000-10111111
                if (!write_code(bw, 0x30 + lit, 8)) {
                    lz77_free(&lz);
                    return 0;
                }
            } else {
                // 9 bits: 110010000-111111111
                if (!write_code(bw, 0x190 + (lit - 144), 9)) {
                    lz77_free(&lz);
                    return 0;
                }
            }

            if (pos + MIN_MATCH_LEN <= len)
                update_hash(&lz, data, pos);
            pos++;
        }
    }

    // End of block (code 256, 7 bits)
    if (!write_code(bw, 0, 7)) {
        lz77_free(&lz);
        return 0;
    }

    lz77_free(&lz);
    return 1;
}

//=============================================================================
// Dynamic Huffman (BTYPE=2)
//=============================================================================

#define DH_LITLEN 286   ///< Literal/length alphabet size (0..285).
#define DH_DIST 30      ///< Distance alphabet size (0..29).
#define DH_CODELEN 19   ///< Code-length alphabet size (0..18).
#define DH_MAXBITS 15   ///< Max code length for the litlen/dist trees.
#define DH_CL_MAXBITS 7 ///< Max code length for the code-length tree.
/// @brief Cap on inputs for which the dynamic path is attempted: the token buffer
///        is up to 4*len bytes, so this bounds its allocation to ~64 MB.
#define DH_MAX_INPUT (16u * 1024u * 1024u)

/// @brief Preferred dynamic-Huffman token-buffer input cap.
/// @details Larger inputs still use the fixed-Huffman encoder. This avoids
///          allocating one token per input byte for multi-megabyte payloads
///          while preserving compression support.
#define DH_MAX_DYNAMIC_TOKEN_INPUT (1024u * 1024u)

/// @brief RFC 1951 §3.2.7 code-length-code transmission order.
static const int dh_cl_order[DH_CODELEN] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/// @brief One collected LZ77 token. `dist == 0` is a literal byte held in `len`;
///        `dist > 0` is a (length 3..258, distance 1..32768) back-reference.
typedef struct {
    uint16_t len;
    uint16_t dist;
} dh_token;

/// @brief Build length-limited canonical Huffman code lengths from symbol
///        frequencies. Produces `lengths[0..n-1]` (0 for unused symbols) with no
///        code longer than `max_bits`. A min-heap builds the optimal tree, then
///        any over-long codes are redistributed the way zlib's gen_bitlen does
///        (least-frequent symbols absorb the longest codes). On allocation
///        failure it leaves all-zero lengths, which makes the caller fall back.
/// @param freq Per-symbol occurrence counts.
/// @param n Number of symbols in @p freq and @p lengths.
/// @param max_bits Maximum permitted code length.
/// @param lengths Output array receiving one code length per symbol.
static void dh_build_lengths(const uint32_t *freq, int n, int max_bits, uint8_t *lengths) {
    for (int i = 0; i < n; i++)
        lengths[i] = 0;

    const int cap = 2 * n;
    uint32_t *nfreq = (uint32_t *)malloc((size_t)cap * sizeof(uint32_t));
    int *nleft = (int *)malloc((size_t)cap * sizeof(int));
    int *nright = (int *)malloc((size_t)cap * sizeof(int));
    int *nsym = (int *)malloc((size_t)cap * sizeof(int));
    int *heap = (int *)malloc((size_t)cap * sizeof(int));
    if (!nfreq || !nleft || !nright || !nsym || !heap) {
        free(nfreq);
        free(nleft);
        free(nright);
        free(nsym);
        free(heap);
        return;
    }

    int node_count = 0, heap_size = 0, last = -1, nonzero = 0;
    for (int i = 0; i < n; i++) {
        if (freq[i]) {
            nonzero++;
            last = i;
            nfreq[node_count] = freq[i];
            nleft[node_count] = -1;
            nright[node_count] = -1;
            nsym[node_count] = i;
            heap[heap_size++] = node_count;
            node_count++;
        }
    }
    if (nonzero <= 1) {
        if (nonzero == 1)
            lengths[last] = 1; // a one-symbol alphabet still needs a 1-bit code
        free(nfreq);
        free(nleft);
        free(nright);
        free(nsym);
        free(heap);
        return;
    }

#define DH_LESS(x, y) (nfreq[x] < nfreq[y])
#define DH_SIFT_DOWN(start)                                                                        \
    do {                                                                                           \
        int p = (start);                                                                           \
        for (;;) {                                                                                 \
            int c = 2 * p + 1;                                                                     \
            if (c >= heap_size)                                                                    \
                break;                                                                             \
            if (c + 1 < heap_size && DH_LESS(heap[c + 1], heap[c]))                                \
                c++;                                                                               \
            if (!DH_LESS(heap[c], heap[p]))                                                        \
                break;                                                                             \
            int t = heap[p];                                                                       \
            heap[p] = heap[c];                                                                     \
            heap[c] = t;                                                                           \
            p = c;                                                                                 \
        }                                                                                          \
    } while (0)

    for (int i = heap_size / 2 - 1; i >= 0; i--)
        DH_SIFT_DOWN(i);

    while (heap_size > 1) {
        int a = heap[0];
        heap[0] = heap[--heap_size];
        DH_SIFT_DOWN(0);
        int b = heap[0];
        heap[0] = heap[--heap_size];
        DH_SIFT_DOWN(0);
        int m = node_count++;
        nfreq[m] = nfreq[a] + nfreq[b];
        nleft[m] = a;
        nright[m] = b;
        nsym[m] = -1;
        int c = heap_size++;
        heap[c] = m;
        while (c > 0) {
            int p = (c - 1) / 2;
            if (!DH_LESS(heap[c], heap[p]))
                break;
            int t = heap[p];
            heap[p] = heap[c];
            heap[c] = t;
            c = p;
        }
    }
    int root = heap[0];

    int *stk_node = (int *)malloc((size_t)cap * sizeof(int));
    int *stk_depth = (int *)malloc((size_t)cap * sizeof(int));
    int bl_count[64];
    for (int i = 0; i < 64; i++)
        bl_count[i] = 0;
    int maxlen_seen = 0;
    if (stk_node && stk_depth) {
        int sp = 0;
        stk_node[sp] = root;
        stk_depth[sp] = 0;
        sp++;
        while (sp > 0) {
            sp--;
            int nd = stk_node[sp], d = stk_depth[sp];
            if (nsym[nd] >= 0) {
                int dd = d < 1 ? 1 : d;
                lengths[nsym[nd]] = (uint8_t)dd;
                if (dd > maxlen_seen)
                    maxlen_seen = dd;
                if (dd < 64)
                    bl_count[dd]++;
            } else if (d + 1 < 60) {
                stk_node[sp] = nleft[nd];
                stk_depth[sp] = d + 1;
                sp++;
                stk_node[sp] = nright[nd];
                stk_depth[sp] = d + 1;
                sp++;
            }
        }
    }
    free(stk_node);
    free(stk_depth);

    if (maxlen_seen > max_bits) {
        int overflow = 0;
        for (int b = max_bits + 1; b <= maxlen_seen && b < 64; b++) {
            overflow += bl_count[b];
            bl_count[max_bits] += bl_count[b];
            bl_count[b] = 0;
        }
        while (overflow > 0) {
            int b = max_bits - 1;
            while (b > 0 && bl_count[b] == 0)
                b--;
            if (b == 0)
                break;
            bl_count[b]--;
            bl_count[b + 1] += 2;
            bl_count[max_bits]--;
            overflow -= 2;
        }
        // Reassign: least-frequent symbols get the longest codes. Collect used
        // symbols sorted by ascending frequency, then hand out lengths from
        // max_bits downward according to bl_count.
        int used = 0;
        int *syms = (int *)malloc((size_t)n * sizeof(int));
        if (syms) {
            for (int i = 0; i < n; i++)
                if (lengths[i])
                    syms[used++] = i;
            for (int i = 1; i < used; i++) { // insertion sort by ascending freq
                int key = syms[i], j = i - 1;
                while (j >= 0 && freq[syms[j]] > freq[key]) {
                    syms[j + 1] = syms[j];
                    j--;
                }
                syms[j + 1] = key;
            }
            int k = 0;
            for (int bits = max_bits; bits >= 1; bits--) {
                int cnt = bl_count[bits];
                while (cnt-- > 0 && k < used)
                    lengths[syms[k++]] = (uint8_t)bits;
            }
            free(syms);
        }
    }

#undef DH_SIFT_DOWN
#undef DH_LESS
    free(nfreq);
    free(nleft);
    free(nright);
    free(nsym);
    free(heap);
}

/// @brief Assign canonical (MSB-first) Huffman codes from code lengths per
///        RFC 1951 §3.2.2. Zero-length symbols get code 0 (unused).
/// @param lengths Per-symbol canonical code lengths.
/// @param n Number of symbols.
/// @param codes Output array receiving MSB-first code values.
static void dh_assign_codes(const uint8_t *lengths, int n, uint16_t *codes) {
    int bl_count[DH_MAXBITS + 1];
    for (int i = 0; i <= DH_MAXBITS; i++)
        bl_count[i] = 0;
    for (int i = 0; i < n; i++)
        if (lengths[i])
            bl_count[lengths[i]]++;
    uint16_t next_code[DH_MAXBITS + 1];
    uint16_t code = 0;
    for (int bits = 1; bits <= DH_MAXBITS; bits++) {
        code = (uint16_t)((code + bl_count[bits - 1]) << 1);
        next_code[bits] = code;
    }
    for (int i = 0; i < n; i++)
        codes[i] = lengths[i] ? next_code[lengths[i]]++ : 0;
}

/// @brief Emit a single DEFLATE type-2 (dynamic Huffman) block with LZ77 matching.
///
/// Runs LZ77 once to collect tokens and symbol frequencies, builds optimal
/// length-limited Huffman trees for the literal/length and distance alphabets,
/// RLE-encodes the two code-length vectors using the code-length alphabet,
/// builds a third tree to describe that, then emits the block header, the tree
/// descriptions, and the tokens. Returns 0 (and emits nothing) on allocation
/// failure or when the input is too large for the token buffer, so the caller
/// can fall back to the fixed block.
/// @param bw Destination writer; callers use a separate candidate writer so a
/// failed attempt cannot contaminate fixed output.
/// @param data Borrowed input bytes.
/// @param len Input byte count.
/// @param level Compression level controlling match search depth.
/// @return 1 after a complete dynamic block is emitted; 0 when unavailable or
/// unsuccessful so the caller can use fixed coding.
static int deflate_dynamic(bit_writer_t *bw, const uint8_t *data, size_t len, int level) {
    if (len == 0 || len > DH_MAX_INPUT || len > DH_MAX_DYNAMIC_TOKEN_INPUT)
        return 0;

    dh_token *tokens = (dh_token *)malloc(len * sizeof(dh_token));
    if (!tokens)
        return 0;

    uint32_t ll_freq[DH_LITLEN];
    uint32_t d_freq[DH_DIST];
    memset(ll_freq, 0, sizeof(ll_freq));
    memset(d_freq, 0, sizeof(d_freq));
    size_t ntok = 0;

    lz77_state_t lz;
    if (!lz77_init(&lz)) {
        free(tokens);
        return 0;
    }
    int max_chain = 4 << level;
    size_t pos = 0;
    while (pos < len) {
        int md = 0, ml = 0;
        if (pos + MIN_MATCH_LEN <= len)
            ml = find_match(&lz, data, pos, len, max_chain, &md);
        if (ml >= MIN_MATCH_LEN) {
            tokens[ntok].len = (uint16_t)ml;
            tokens[ntok].dist = (uint16_t)md;
            ntok++;
            ll_freq[get_length_code(ml)]++;
            d_freq[get_dist_code(md)]++;
            for (int i = 0; i < ml; i++)
                if (pos + i + MIN_MATCH_LEN <= len)
                    update_hash(&lz, data, pos + i);
            pos += ml;
        } else {
            tokens[ntok].len = data[pos];
            tokens[ntok].dist = 0;
            ntok++;
            ll_freq[data[pos]]++;
            if (pos + MIN_MATCH_LEN <= len)
                update_hash(&lz, data, pos);
            pos++;
        }
    }
    lz77_free(&lz);
    ll_freq[256]++; // end-of-block

    uint8_t ll_len[DH_LITLEN], d_len[DH_DIST];
    dh_build_lengths(ll_freq, DH_LITLEN, DH_MAXBITS, ll_len);
    dh_build_lengths(d_freq, DH_DIST, DH_MAXBITS, d_len);
    // A well-formed block needs at least one distance code, even with no matches.
    int any_dist = 0;
    for (int i = 0; i < DH_DIST; i++)
        any_dist |= (d_len[i] != 0);
    if (!any_dist)
        d_len[0] = 1;
    // The end-of-block symbol must be present.
    if (ll_len[256] == 0) {
        free(tokens);
        return 0;
    }

    uint16_t ll_code[DH_LITLEN], d_code[DH_DIST];
    dh_assign_codes(ll_len, DH_LITLEN, ll_code);
    dh_assign_codes(d_len, DH_DIST, d_code);

    int hlit = DH_LITLEN;
    while (hlit > 257 && ll_len[hlit - 1] == 0)
        hlit--;
    int hdist = DH_DIST;
    while (hdist > 1 && d_len[hdist - 1] == 0)
        hdist--;

    // Build the combined code-length vector and RLE-encode it.
    uint8_t cl_seq[DH_LITLEN + DH_DIST];
    int cl_n = 0;
    for (int i = 0; i < hlit; i++)
        cl_seq[cl_n++] = ll_len[i];
    for (int i = 0; i < hdist; i++)
        cl_seq[cl_n++] = d_len[i];

    enum { DH_RLE_CAP = 2 * (DH_LITLEN + DH_DIST) };

    uint8_t rle_sym[DH_RLE_CAP], rle_extra[DH_RLE_CAP], rle_xbits[DH_RLE_CAP];
    int rle_n = 0;
    uint32_t cl_freq[DH_CODELEN];
    memset(cl_freq, 0, sizeof(cl_freq));
    for (int i = 0; i < cl_n;) {
        int v = cl_seq[i];
        int run = 1;
        while (i + run < cl_n && cl_seq[i + run] == v)
            run++;
        i += run;
        int rem = run;
        if (v == 0) {
            while (rem >= 11) {
                int r = rem > 138 ? 138 : rem;
                rle_sym[rle_n] = 18;
                rle_extra[rle_n] = (uint8_t)(r - 11);
                rle_xbits[rle_n] = 7;
                rle_n++;
                cl_freq[18]++;
                rem -= r;
            }
            while (rem >= 3) {
                int r = rem > 10 ? 10 : rem;
                rle_sym[rle_n] = 17;
                rle_extra[rle_n] = (uint8_t)(r - 3);
                rle_xbits[rle_n] = 3;
                rle_n++;
                cl_freq[17]++;
                rem -= r;
            }
            while (rem-- > 0) {
                rle_sym[rle_n] = 0;
                rle_extra[rle_n] = 0;
                rle_xbits[rle_n] = 0;
                rle_n++;
                cl_freq[0]++;
            }
        } else {
            rle_sym[rle_n] = (uint8_t)v;
            rle_extra[rle_n] = 0;
            rle_xbits[rle_n] = 0;
            rle_n++;
            cl_freq[v]++;
            rem--;
            while (rem >= 3) {
                int r = rem > 6 ? 6 : rem;
                rle_sym[rle_n] = 16;
                rle_extra[rle_n] = (uint8_t)(r - 3);
                rle_xbits[rle_n] = 2;
                rle_n++;
                cl_freq[16]++;
                rem -= r;
            }
            while (rem-- > 0) {
                rle_sym[rle_n] = (uint8_t)v;
                rle_extra[rle_n] = 0;
                rle_xbits[rle_n] = 0;
                rle_n++;
                cl_freq[v]++;
            }
        }
    }

    uint8_t cl_len[DH_CODELEN];
    dh_build_lengths(cl_freq, DH_CODELEN, DH_CL_MAXBITS, cl_len);
    uint16_t cl_code[DH_CODELEN];
    dh_assign_codes(cl_len, DH_CODELEN, cl_code);

    int hclen = DH_CODELEN;
    while (hclen > 4 && cl_len[dh_cl_order[hclen - 1]] == 0)
        hclen--;

    int ok = 1;
    ok = ok && bw_write(bw, 1, 1) && bw_write(bw, 2, 2); // BFINAL=1, BTYPE=2 (dynamic)
    ok = ok && bw_write(bw, (uint32_t)(hlit - 257), 5);
    ok = ok && bw_write(bw, (uint32_t)(hdist - 1), 5);
    ok = ok && bw_write(bw, (uint32_t)(hclen - 4), 4);
    for (int j = 0; ok && j < hclen; j++)
        ok = bw_write(bw, cl_len[dh_cl_order[j]], 3);
    for (int j = 0; ok && j < rle_n; j++) {
        ok = write_code(bw, cl_code[rle_sym[j]], cl_len[rle_sym[j]]);
        if (ok && rle_xbits[j])
            ok = bw_write(bw, rle_extra[j], rle_xbits[j]);
    }
    for (size_t t = 0; ok && t < ntok; t++) {
        if (tokens[t].dist == 0) {
            int lit = tokens[t].len;
            ok = write_code(bw, ll_code[lit], ll_len[lit]);
        } else {
            int ml = tokens[t].len, md = tokens[t].dist;
            int lc = get_length_code(ml), li = lc - 257;
            ok = write_code(bw, ll_code[lc], ll_len[lc]);
            if (ok && length_extra_bits[li])
                ok = bw_write(bw, (uint32_t)(ml - length_base[li]), length_extra_bits[li]);
            int dc = get_dist_code(md);
            if (ok)
                ok = write_code(bw, d_code[dc], d_len[dc]);
            if (ok && dist_extra_bits[dc])
                ok = bw_write(bw, (uint32_t)(md - dist_base[dc]), dist_extra_bits[dc]);
        }
    }
    if (ok)
        ok = write_code(bw, ll_code[256], ll_len[256]); // end-of-block

    free(tokens);
    return ok;
}

/// @brief Top-level DEFLATE compression driver.
///
/// Picks the block strategy: inputs of ≤64 bytes or level=1 use stored
/// blocks (no Huffman savings worth the overhead); levels 2–5 use a fixed
/// Huffman block; levels ≥6 emit both a fixed and a dynamic-Huffman block and
/// keep whichever is smaller, so dynamic coding never regresses output size.
/// Level is clamped to [1..9].
/// @param data Borrowed input bytes; may be `NULL` when @p len is zero.
/// @param len Input byte count.
/// @param level Requested compression level, clamped to 1 through 9.
/// @return Fresh GC-managed Bytes containing a complete raw DEFLATE stream, or
/// `NULL` after allocation/encoding failure.
void *deflate_data(const uint8_t *data, size_t len, int level) {
    if (level < DEFLATE_MIN_LEVEL)
        level = DEFLATE_MIN_LEVEL;
    if (level > DEFLATE_MAX_LEVEL)
        level = DEFLATE_MAX_LEVEL;

    bit_writer_t bw;
    if (!bw_init(&bw, len))
        return NULL;

    // For small data or level 1, use stored blocks; levels 2-5 use fixed Huffman;
    // levels >=6 additionally try a dynamic-Huffman block and keep the smaller one.
    if (len <= 64 || level == 1) {
        if (!deflate_stored(&bw, data, len)) {
            bw_free(&bw);
            return NULL;
        }
    } else if (level >= 6) {
        bit_writer_t bw_dyn;
        int have_dyn = 0;
        if (bw_init(&bw_dyn, len)) {
            if (deflate_dynamic(&bw_dyn, data, len, level) && bw_flush(&bw_dyn))
                have_dyn = 1;
            else
                bw_free(&bw_dyn);
        }
        if (!deflate_fixed(&bw, data, len, level) || !bw_flush(&bw)) {
            if (have_dyn)
                bw_free(&bw_dyn);
            bw_free(&bw);
            rt_trap("Deflate: failed to encode fixed Huffman block");
            return NULL;
        }
        if (have_dyn && bw_dyn.len < bw.len) {
            bw_free(&bw); // dynamic won
            bw = bw_dyn;  // take ownership of the smaller stream (already flushed)
        } else if (have_dyn) {
            bw_free(&bw_dyn);
        }
    } else {
        if (!deflate_fixed(&bw, data, len, level)) {
            bw_free(&bw);
            rt_trap("Deflate: failed to encode fixed Huffman block");
            return NULL;
        }
    }

    if (!bw_flush(&bw)) {
        bw_free(&bw);
        return NULL;
    }

    // Create output Bytes object
    void *result = rt_bytes_new(bw.len);
    if (!result) {
        bw_free(&bw);
        return NULL;
    }
    memcpy(bytes_data(result), bw.data, bw.len);
    bw_free(&bw);

    return result;
}

//=============================================================================
// GZIP Wrapper
//=============================================================================

/// @brief Wrap a DEFLATE payload in the RFC 1952 GZIP envelope.
///
/// Layout: 10-byte header (magic 0x1F 0x8B, method=8 deflate, flags=0,
/// zeroed MTIME, XFL, OS=0xFF unknown) + DEFLATE payload + 8-byte
/// trailer (CRC32 of the original uncompressed data + ISIZE =
/// uncompressed length mod 2^32, both little-endian). CRC32 is computed
/// over the raw input, not the compressed bytes.
/// @param data Borrowed uncompressed bytes; may be `NULL` when @p len is zero.
/// @param len Uncompressed byte count.
/// @param level Requested compression level passed to deflate_data().
/// @return Fresh GC-managed Bytes containing one complete GZIP member, or
/// `NULL` after compression, overflow, or allocation failure.
void *gzip_data(const uint8_t *data, size_t len, int level) {
    // First compress with DEFLATE
    void *deflated = deflate_data(data, len, level);
    if (!deflated)
        return NULL;
    size_t deflated_len = bytes_len(deflated);
    uint8_t *deflated_data = bytes_data(deflated);

    // Calculate CRC32
    uint32_t crc = rt_crc32_compute(data, len);

    // Create output: header (10) + deflated + trailer (8)
    if (deflated_len > SIZE_MAX - 18) {
        compress_release_temp_object(deflated);
        rt_trap("Gzip: output size overflow");
        return NULL;
    }
    size_t total_len = 10 + deflated_len + 8;
    void *result = rt_bytes_new(total_len);
    if (!result) {
        compress_release_temp_object(deflated);
        return NULL;
    }
    uint8_t *out = bytes_data(result);

    // GZIP header (RFC 1952)
    out[0] = 0x1F; // Magic
    out[1] = 0x8B; // Magic
    out[2] = 0x08; // Compression method (deflate)
    out[3] = 0x00; // Flags (none)
    out[4] = 0x00; // MTIME (0 = not available)
    out[5] = 0x00;
    out[6] = 0x00;
    out[7] = 0x00;
    out[8] = 0x00; // XFL (extra flags)
    out[9] = 0xFF; // OS (unknown)

    // Compressed data
    memcpy(out + 10, deflated_data, deflated_len);

    // Trailer
    size_t trailer_pos = 10 + deflated_len;
    out[trailer_pos + 0] = crc & 0xFF;
    out[trailer_pos + 1] = (crc >> 8) & 0xFF;
    out[trailer_pos + 2] = (crc >> 16) & 0xFF;
    out[trailer_pos + 3] = (crc >> 24) & 0xFF;
    out[trailer_pos + 4] = len & 0xFF;
    out[trailer_pos + 5] = (len >> 8) & 0xFF;
    out[trailer_pos + 6] = (len >> 16) & 0xFF;
    out[trailer_pos + 7] = (len >> 24) & 0xFF;

    compress_release_temp_object(deflated);
    return result;
}
