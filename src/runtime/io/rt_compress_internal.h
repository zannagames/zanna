//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_compress_internal.h
// Purpose: Shared Huffman-tree type and the lazily-built fixed trees used by
//   both the inflate path (rt_compress.c) and the deflate path (rt_deflate.c).
//
// Key invariants:
//   - Window, match, length, and distance constants follow RFC 1951.
//   - Fixed trees are initialized exactly once, then shared read-only.
//   - deflate_data() and gzip_data() return fresh runtime Bytes objects.
//
// Ownership/Lifetime:
//   - Huffman tree symbol tables are process-lifetime state after one-time
//     initialization.
//   - Input byte spans are borrowed; managed compression results are returned
//     to the caller.
//
// Links: src/runtime/io/rt_compress.c,
//        src/runtime/io/rt_deflate.c,
//        src/runtime/io/rt_compress.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Shares RFC 1951 constants, Huffman state, and compressor helpers.
 * @details Defines the 32-KiB history limits, canonical-code representations,
 * fixed-tree publication state, length and distance lookup tables, supported
 * levels, and cross-unit entry points used by inflate and deflate.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/** @name RFC 1951 LZ77 window and match limits
 * @{ */
#define WINDOW_SIZE 32768
#define WINDOW_MASK 0x7FFF
#define MAX_MATCH_LEN 258
#define MIN_MATCH_LEN 3
#define MAX_DISTANCE 32768
/** @} */

/** Canonical Huffman symbol, assigned code bits, and bit length. */
typedef struct {
    uint16_t symbol; ///< Symbol value (literal, length code, or distance code).
    uint16_t code;   ///< Assigned bit pattern (MSB-aligned canonical code).
    uint8_t len;     ///< Code length in bits.
} huffman_code_t;

/// @brief Direct-mapped Huffman decode table for O(1) symbol lookup.
typedef struct {
    int max_code;      ///< Highest valid code index in the flat lookup table.
    uint16_t *symbols; ///< Flat table indexed by (bit-reversed) code prefix.
    int table_bits;    ///< Number of bits consumed per table lookup.
    size_t table_size; ///< Number of entries in `symbols`.
} huffman_tree_t;

/// Process-lifetime fixed literal/length decode table.
extern huffman_tree_t fixed_lit_tree;
/// Process-lifetime fixed distance decode table.
extern huffman_tree_t fixed_dist_tree;

/// @brief Initialize both RFC 1951 fixed Huffman tables exactly once.
/// @details Safe for concurrent callers; successful initialization publishes
/// immutable lookup tables for inflate and deflate code.
void init_fixed_trees(void);

// Defined in rt_deflate.c, used by the public compression API in rt_compress.c.
/// @brief Compress a byte span as a raw RFC 1951 DEFLATE stream.
/// @param data Borrowed source bytes; may be `NULL` only when @p len is zero.
/// @param len Source byte count.
/// @param level Compression level from 1 through 9.
/// @return Fresh runtime Bytes object, or `NULL` after a trap/failure.
void *deflate_data(const uint8_t *data, size_t len, int level);

/// @brief Compress a byte span as an RFC 1952 GZIP member.
/// @param data Borrowed source bytes; may be `NULL` only when @p len is zero.
/// @param len Source byte count.
/// @param level Compression level from 1 through 9.
/// @return Fresh runtime Bytes object containing header, DEFLATE payload, and
/// trailer, or `NULL` after a trap/failure.
void *gzip_data(const uint8_t *data, size_t len, int level);

// Shared DEFLATE length/distance code tables (used by inflate + deflate).
/** Extra-bit count indexed by length code 257 through 285. */
static const int length_extra_bits[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

// Base length for length codes 257-285
/** Base match length indexed by length code 257 through 285. */
static const int length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};

// Extra bits for distance codes 0-29
/** Extra-bit count indexed by distance code 0 through 29. */
static const int dist_extra_bits[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Base distance for distance codes 0-29
/** Base backward distance indexed by distance code 0 through 29. */
static const int dist_base[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                  33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};

// Additional shared DEFLATE constants.
/** @name Publicly supported compression-level range
 * @{ */
#define DEFLATE_MAX_LEVEL 9
#define DEFLATE_MIN_LEVEL 1
/** @} */

// Shared byte/string helpers (defined in rt_compress.c).
/// @brief Borrow writable storage from a runtime Bytes object.
/// @param obj Runtime Bytes handle.
/// @return Borrowed data pointer.
uint8_t *bytes_data(void *obj);

/// @brief Query a runtime Bytes object's signed length.
/// @param obj Runtime Bytes handle.
/// @return Byte count reported by the Bytes API.
int64_t bytes_len(void *obj);

/// @brief Eagerly release one reference to a temporary runtime object.
/// @param obj Temporary object; `NULL` is accepted.
void compress_release_temp_object(void *obj);
