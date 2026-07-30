//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_compress.c
// Purpose: Implements RFC 1951 DEFLATE and RFC 1952 GZIP compression and
//          decompression without external dependencies. Uses LZ77 with a 32KB
//          sliding window and stored or fixed-Huffman blocks to compress
//          data at configurable levels (1-9).
//
// Key invariants:
//   - Compression level 6 is the default; levels 1-9 are supported.
//   - Decompression accepts both raw DEFLATE and GZIP-wrapped streams.
//   - Compression produces block type 0 (stored) or type 1 (fixed Huffman).
//     Decompression consumes type 0, type 1, and type 2 (dynamic) blocks.
//   - CRC32 is computed and validated for GZIP streams.
//   - Fixed Huffman lookup tables are initialized exactly once with native
//     one-time primitives and remain immutable afterward.
//
// Ownership/Lifetime:
//   - Compressed and decompressed output is returned as a fresh rt_bytes
//     allocation owned by the caller.
//   - Input rt_bytes buffers are read-only and not retained by the functions.
//
// Links: src/runtime/io/rt_compress.h (public API),
//        src/runtime/core/rt_crc32.h (CRC32 used for GZIP footer validation),
//        src/runtime/io/rt_archive.c (consumes this for ZIP DEFLATE entries),
//        docs/adr/0229-bounded-native-gzip-decoding.md
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements raw DEFLATE, zlib-wrapped inflate, and GZIP APIs.
 * @details Provides sticky bounded bit reading, canonical fixed and dynamic
 * Huffman decoding, stored-block handling, sliding-window expansion,
 * allocation limits, zlib Adler-32 and GZIP CRC/trailer validation, and
 * transactional managed Bytes/string wrappers.
 */

#include "rt_compress.h"

#include "rt_bytes.h"
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

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "rt_compress_internal.h"

//=============================================================================
// Constants
//=============================================================================

/// @brief DEFLATE block types (RFC 1951 §3.2.3).
typedef enum {
    RT_HUFFMAN_STORED = 0,  ///< No compression (stored block).
    RT_HUFFMAN_FIXED = 1,   ///< Fixed Huffman codes.
    RT_HUFFMAN_DYNAMIC = 2, ///< Dynamic Huffman codes.
} rt_huffman_type_t;

/** @name Inflate alphabet sizes and default compression level
 * @{ */
#define DEFLATE_DEFAULT_LEVEL 6
#define MAX_BITS 15           // Maximum Huffman code length
#define MAX_LIT_CODES 286     // 0-255 literals + 256 end + 257-285 lengths
#define MAX_DIST_CODES 30     // Distance codes
#define MAX_CODE_LEN_CODES 19 // Code length alphabet size
/** @} */

/// @copydoc rt_trap_set_recovery()
extern void rt_trap_set_recovery(jmp_buf *buf);
/// @copydoc rt_trap_clear_recovery()
extern void rt_trap_clear_recovery(void);
/// @copydoc rt_trap_get_error()
extern const char *rt_trap_get_error(void);


// Fixed Huffman code lengths (RFC 1951)
/** @name Fixed-Huffman alphabet sizes
 * @{ */
#define FIXED_LIT_CODES 288
#define FIXED_DIST_CODES 32

/** @} */

//=============================================================================
// Internal Bytes Access
//=============================================================================

/// @brief Borrow the raw storage of a runtime Bytes object.
/// @param obj Runtime Bytes handle.
/// @return Borrowed writable data pointer reported by the Bytes API.
uint8_t *bytes_data(void *obj) {
    return rt_bytes_data(obj);
}

/// @brief Query the signed length of a runtime Bytes object.
/// @param obj Runtime Bytes handle.
/// @return Byte count reported by the Bytes API.
int64_t bytes_len(void *obj) {
    return rt_bytes_len(obj);
}

/// @brief Validate a Bytes input and borrow its contiguous storage.
/// @param obj Candidate runtime Bytes object.
/// @param what Diagnostic raised for invalid object state.
/// @param out_len Receives the non-negative byte length when non-`NULL`.
/// @return Borrowed data pointer, which may be `NULL` for an empty Bytes value;
/// invalid input raises a trap and returns `NULL`.
static const uint8_t *compress_bytes_view(void *obj, const char *what, int64_t *out_len) {
    if (!obj) {
        rt_trap(what);
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    int64_t len = bytes_len(obj);
    uint8_t *data = bytes_data(obj);
    if (len < 0 || (len > 0 && !data)) {
        rt_trap(what);
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    if (out_len)
        *out_len = len;
    return data;
}

/// @brief Release a temporary GC object that is no longer needed.
/// @param obj Temporary object; `NULL` is accepted. Storage is reclaimed
/// immediately only when this release drops the final reference.
void compress_release_temp_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Snapshot the active trap message or a fallback into a fixed buffer.
/// @param buffer Writable destination.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Text copied when no active non-empty diagnostic exists.
static void compress_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Consume native bytes into one newly managed Bytes object.
/// @details A local recovery frame frees @p raw if managed allocation traps
///          non-locally. Returning trap hooks are handled by the ordinary NULL
///          result check. On success the native input is also freed after its
///          bytes are copied.
/// @param raw Malloc-owned input buffer, consumed on every path.
/// @param len Input byte count.
/// @param fallback Diagnostic used when allocation traps without a message.
/// @return Fresh managed Bytes, or NULL after cleanup and failure.
static void *compress_native_to_managed_bytes(uint8_t *raw, size_t len, const char *fallback) {
    uint8_t *volatile owned_raw = raw;
    void *result = NULL;
    jmp_buf recovery;

    if (len > (size_t)INT64_MAX) {
        free(raw);
        rt_trap("rt_compress: decoded output exceeds managed size");
        return NULL;
    }

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        compress_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        free((void *)owned_raw);
        rt_trap(saved_error);
        return NULL;
    }

    result = rt_bytes_new((int64_t)len);
    rt_trap_clear_recovery();
    if (!result) {
        free((void *)owned_raw);
        return NULL;
    }
    if (len > 0)
        memcpy(bytes_data(result), (const void *)owned_raw, len);
    free((void *)owned_raw);
    return result;
}

/// @brief Compress a temporary Bytes conversion and release it transactionally.
/// @param bytes Temporary Bytes object whose reference this helper consumes.
/// @param gzip Nonzero to emit GZIP; zero to emit raw DEFLATE.
/// @param fallback Diagnostic used if compression traps without a message.
/// @return Fresh compressed Bytes object, or `NULL` after cleanup and failure.
static void *compress_run_string_bytes(void *bytes, int gzip, const char *fallback) {
    void *volatile owned_bytes = bytes;
    void *result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        compress_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        compress_release_temp_object((void *)owned_bytes);
        rt_trap(saved_error);
        return NULL;
    }

    int64_t len = bytes_len((void *)owned_bytes);
    if (len < 0) {
        rt_trap_clear_recovery();
        compress_release_temp_object((void *)owned_bytes);
        rt_trap(fallback);
        return NULL;
    }
    if (gzip)
        result = gzip_data(bytes_data((void *)owned_bytes), (size_t)len, DEFLATE_DEFAULT_LEVEL);
    else
        result = deflate_data(bytes_data((void *)owned_bytes), (size_t)len, DEFLATE_DEFAULT_LEVEL);
    rt_trap_clear_recovery();
    compress_release_temp_object((void *)owned_bytes);
    return result;
}

/// @brief Convert temporary Bytes to a runtime string and release the Bytes.
/// @param bytes Temporary Bytes object whose reference this helper consumes.
/// @param fallback Diagnostic used if string conversion traps without a message.
/// @return Fresh runtime string on success; failure propagates a trap and
/// returns the runtime empty string only as a control-flow fallback.
static rt_string compress_bytes_to_str_or_release(void *bytes, const char *fallback) {
    void *volatile owned_bytes = bytes;
    rt_string result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        compress_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        compress_release_temp_object((void *)owned_bytes);
        rt_trap(saved_error);
        return rt_str_empty();
    }

    result = rt_bytes_to_str((void *)owned_bytes);
    rt_trap_clear_recovery();
    compress_release_temp_object((void *)owned_bytes);
    return result;
}

//=============================================================================
// Bit Stream Reader (for decompression)
//=============================================================================

/// @brief LSB-first bit-stream reader used by the DEFLATE decompressor.
typedef struct {
    const uint8_t *data; ///< Source byte buffer (borrowed, caller keeps alive).
    size_t len;          ///< Total length of `data` in bytes.
    size_t pos;          ///< Current byte offset into `data`.
    int bit_pos;         ///< Unused — superseded by `bits_in_buf` (kept for alignment).
    uint32_t buffer;     ///< Rolling bit accumulator (LSB = next bit).
    int bits_in_buf;     ///< Number of valid bits currently in `buffer`.
    bool error;          ///< Set to true after a truncated or invalid bit read.
} bit_reader_t;

/// @brief Initialize a bit reader over a byte buffer.
///
/// Resets the rolling buffer and bit-position state. The reader holds
/// `data` by pointer, so the caller must keep the buffer alive for the
/// reader's lifetime. Bit reads pull from the LSB of `buffer`,
/// refilled in 8-bit chunks from `data` on demand.
///
/// @param br Reader state to initialize.
/// @param data Borrowed encoded byte buffer.
/// @param len Number of accessible bytes in @p data.
static void br_init(bit_reader_t *br, const uint8_t *data, size_t len) {
    br->data = data;
    br->len = len;
    br->pos = 0;
    br->bit_pos = 0;
    br->buffer = 0;
    br->bits_in_buf = 0;
    br->error = false;
}

/// @brief Refill the accumulator toward a requested bit count.
/// @details At end-of-stream, returns true when any buffered bits remain even
/// if fewer than requested; consuming helpers perform the exact-count check.
/// @param br Bit reader to refill.
/// @param n Desired number of buffered bits.
/// @return `true` when at least one buffered bit remains or @p n bits are
/// available; `false` only when no more bits exist.
static bool br_fill(bit_reader_t *br, int n) {
    while (br->bits_in_buf < n) {
        if (br->pos >= br->len) {
            return br->bits_in_buf > 0;
        }
        br->buffer |= ((uint32_t)br->data[br->pos++]) << br->bits_in_buf;
        br->bits_in_buf += 8;
    }
    return true;
}

/// @brief Read an exact number of bits in least-significant-bit-first order.
/// @param br Reader whose cursor is advanced.
/// @param n Bit count requested by the DEFLATE parser.
/// @return Decoded low-order value, or 0 with `br->error` set when truncated.
static uint32_t br_read(bit_reader_t *br, int n) {
    if (!br_fill(br, n) || br->bits_in_buf < n) {
        br->error = true;
        return 0;
    }
    uint32_t val = br->buffer & ((1U << n) - 1);
    br->buffer >>= n;
    br->bits_in_buf -= n;
    return val;
}

/// @brief Peek at low-order buffered bits without consuming them.
/// @param br Reader to inspect.
/// @param n Number of bits to mask from the accumulator.
/// @return Masked value; missing high bits appear as zero when the stream ends
/// before @p n bits are buffered.
static uint32_t br_peek(bit_reader_t *br, int n) {
    if (!br_fill(br, n))
        return 0;
    return br->buffer & ((1U << n) - 1);
}

/// @brief Consume a known number of buffered bits.
/// @param br Reader whose accumulator is advanced.
/// @param n Number of bits to discard.
static void br_consume(bit_reader_t *br, int n) {
    if (n > br->bits_in_buf) {
        br->bits_in_buf = 0;
        br->buffer = 0;
        br->error = true;
        return;
    }
    br->buffer >>= n;
    br->bits_in_buf -= n;
}

/// @brief Discard all buffered partial-byte state.
/// @param br Reader to align before stored-block byte access.
static void br_align(bit_reader_t *br) {
    br->buffer = 0;
    br->bits_in_buf = 0;
}

/// @brief Check whether encoded bytes or buffered bits remain.
/// @param br Reader to inspect.
/// @return `true` while any unread input remains; otherwise `false`.
static bool br_has_data(bit_reader_t *br) {
    return br->pos < br->len || br->bits_in_buf > 0;
}

//=============================================================================
// Huffman Tree
//=============================================================================

/// @brief A single Huffman code assignment (symbol → code + length).

/// @brief Build a canonical-Huffman lookup table from per-symbol code lengths.
///
/// Follows RFC 1951 §3.2.2: from the length histogram it derives the
/// starting code for each bit-length, assigns codes in symbol order,
/// then bit-reverses each code to LSB-first order (since DEFLATE reads
/// bits LSB-first). The resulting table is a 2^max_len array where each
/// slot holds a packed `(len << 12) | symbol` entry — one lookup decodes
/// any symbol using `tree->table_bits` bits peeked from the stream.
/// Short codes are replicated across all matching prefix slots so the
/// table is a direct-mapped decoder. Returns false on invalid input
/// (code length >15) or allocation failure.
/// @param tree Caller-owned tree state that receives an allocated lookup table.
/// @param lengths Per-symbol canonical code lengths.
/// @param num_codes Number of entries in @p lengths.
/// @return `true` for a complete non-oversubscribed table; otherwise `false`.
static bool build_huffman_tree(huffman_tree_t *tree, const uint8_t *lengths, int num_codes) {
    // Count code lengths
    int bl_count[MAX_BITS + 1] = {0};
    for (int i = 0; i < num_codes; i++) {
        if (lengths[i] > MAX_BITS)
            return false;
        bl_count[lengths[i]]++;
    }
    bl_count[0] = 0;

    int left = 1;
    for (int bits = 1; bits <= MAX_BITS; bits++) {
        left <<= 1;
        left -= bl_count[bits];
        if (left < 0)
            return false;
    }

    // Calculate next code for each length
    uint16_t next_code[MAX_BITS + 1];
    uint16_t code = 0;
    for (int bits = 1; bits <= MAX_BITS; bits++) {
        code = (uint16_t)((code + bl_count[bits - 1]) << 1);
        next_code[bits] = code;
    }

    // Determine table size
    int max_len = 0;
    for (int i = 0; i < num_codes; i++) {
        if (lengths[i] > max_len)
            max_len = lengths[i];
    }
    if (max_len == 0)
        max_len = 1;

    tree->table_bits = max_len; /* Support full 15-bit Huffman codes per RFC 1951 */
    tree->table_size = (size_t)1 << tree->table_bits;
    tree->max_code = num_codes;

    // Allocate symbol table
    tree->symbols = (uint16_t *)calloc(tree->table_size + num_codes * 2, sizeof(uint16_t));
    if (!tree->symbols)
        return false;

    // Build direct lookup table for short codes
    for (int i = 0; i < num_codes; i++) {
        int len = lengths[i];
        if (len == 0)
            continue;
        if (len > MAX_BITS)
            return false;

        uint16_t sym_code = next_code[len]++;

        if (len <= tree->table_bits) {
            // Direct table entry
            // Reverse the code bits for LSB-first reading
            uint16_t rev_code = 0;
            for (int b = 0; b < len; b++) {
                if (sym_code & (1 << b))
                    rev_code |= 1 << (len - 1 - b);
            }

            // Fill in all entries that match this prefix
            int fill = 1 << (tree->table_bits - len);
            for (int j = 0; j < fill; j++) {
                int idx = rev_code | (j << len);
                if ((size_t)idx >= tree->table_size) {
                    free(tree->symbols);
                    tree->symbols = NULL;
                    return false;
                }
                // Pack length and symbol: high bits = length, low bits = symbol
                tree->symbols[idx] = (uint16_t)((len << 12) | i);
            }
        }
    }

    return true;
}

/// @brief Decode a single Huffman-coded symbol from the bit stream.
///
/// Peeks `table_bits` bits, uses them as a direct index into the
/// precomputed lookup table, and consumes only the actual code length
/// stored in the high 4 bits of the entry. Returns -1 if the stream
/// runs dry or the entry is zero (invalid code — the canonical
/// construction leaves unassigned prefixes as zero entries).
/// @param tree Initialized direct-mapped Huffman table.
/// @param br Bit reader positioned at the next code.
/// @return Decoded symbol, or -1 for truncation or an unassigned prefix.
static int decode_symbol(huffman_tree_t *tree, bit_reader_t *br) {
    if (!br_fill(br, tree->table_bits))
        return -1;

    uint32_t bits = br_peek(br, tree->table_bits);
    uint16_t entry = tree->symbols[bits];

    if (entry == 0)
        return -1; // Invalid code

    int len = entry >> 12;
    int symbol = entry & 0xFFF;

    if (len == 0)
        return -1;

    br_consume(br, len);
    if (br->error)
        return -1;
    return symbol;
}

/// @brief Release a Huffman tree's symbol table.
///
/// The tree struct itself is caller-owned; only the dynamically
/// allocated `symbols` buffer is freed.
/// @param tree Tree whose lookup allocation is released and nulled.
static void free_huffman_tree(huffman_tree_t *tree) {
    free(tree->symbols);
    tree->symbols = NULL;
}

//=============================================================================
// Fixed Huffman Trees (for block type 1)
//=============================================================================

/** Process-lifetime fixed literal/length decoder table. */
huffman_tree_t fixed_lit_tree = {0};
/** Process-lifetime fixed distance decoder table. */
huffman_tree_t fixed_dist_tree = {0};
#ifdef _WIN32
/** Windows one-time token guarding fixed-tree publication. */
static INIT_ONCE fixed_trees_once = INIT_ONCE_STATIC_INIT;
#else
/** POSIX one-time token guarding fixed-tree publication. */
static pthread_once_t fixed_trees_once = PTHREAD_ONCE_INIT;
#endif

/// @brief Lazily build the fixed-Huffman literal/distance trees.
///
/// RFC 1951 defines a single canonical set of fixed Huffman codes
/// used by block type 1. Building the lookup tables once and reusing
/// them across calls saves work but introduces a one-time race the
/// `fixed_trees_init` flag mitigates (single-threaded init is fine
/// since runtime startup is serialized — no concurrent inflate calls
/// can occur before the runtime is up).
static void init_fixed_trees_impl(void) {
    // Fixed literal/length code lengths (RFC 1951 section 3.2.6)
    uint8_t lit_lengths[FIXED_LIT_CODES];
    for (int i = 0; i <= 143; i++)
        lit_lengths[i] = 8;
    for (int i = 144; i <= 255; i++)
        lit_lengths[i] = 9;
    for (int i = 256; i <= 279; i++)
        lit_lengths[i] = 7;
    for (int i = 280; i <= 287; i++)
        lit_lengths[i] = 8;

    if (!build_huffman_tree(&fixed_lit_tree, lit_lengths, FIXED_LIT_CODES)) {
        rt_trap("Inflate: failed to initialize fixed literal tree");
        return;
    }

    // Fixed distance code lengths (all 5 bits)
    uint8_t dist_lengths[FIXED_DIST_CODES];
    for (int i = 0; i < FIXED_DIST_CODES; i++)
        dist_lengths[i] = 5;

    if (!build_huffman_tree(&fixed_dist_tree, dist_lengths, FIXED_DIST_CODES)) {
        rt_trap("Inflate: failed to initialize fixed distance tree");
        return;
    }
}

#ifdef _WIN32
/// @brief `InitOnce` callback that builds the fixed Huffman trees (Windows).
/// @param InitOnce Windows one-time initialization token.
/// @param Parameter Unused callback parameter.
/// @param Context Unused callback result slot.
/// @return `TRUE` after invoking the fixed-tree builder.
static BOOL CALLBACK init_fixed_trees_once_cb(PINIT_ONCE InitOnce,
                                              PVOID Parameter,
                                              PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;
    init_fixed_trees_impl();
    return TRUE;
}
#endif

/// @brief Ensure the fixed Huffman trees are built exactly once (thread-safe).
void init_fixed_trees(void) {
#ifdef _WIN32
    InitOnceExecuteOnce(&fixed_trees_once, init_fixed_trees_once_cb, NULL, NULL);
#else
    pthread_once(&fixed_trees_once, init_fixed_trees_impl);
#endif
}

//=============================================================================
// Length and Distance Tables
//=============================================================================

// Extra bits for length codes 257-285

// Code length alphabet order (for dynamic Huffman)
/** RFC 1951 transmission order for the dynamic code-length alphabet. */
static const int code_length_order[MAX_CODE_LEN_CODES] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

//=============================================================================
// Decompression Output Buffer
//=============================================================================

/// @brief Growable byte buffer used to accumulate DEFLATE decompressor output.
typedef struct {
    uint8_t *data;     ///< Heap-allocated output buffer (grown by out_ensure).
    size_t len;        ///< Number of valid bytes written to `data`.
    size_t capacity;   ///< Allocated capacity of `data` in bytes.
    size_t max_output; ///< Maximum allowed decompressed bytes.
    bool owns_data;    ///< True when @c data must be freed/reallocated by this buffer.
    const char *error; ///< Deferred capacity/allocation diagnostic, or NULL.
} output_buffer_t;

/// @brief Initialize a growable output buffer.
///
/// Allocates `initial_cap` bytes (clamped to a 256-byte minimum). Used
/// to accumulate inflated output, which can be larger than the input
/// by an arbitrary factor. `out_ensure` enforces the caller-supplied logical
/// ceiling even when the minimum physical allocation is larger. Traps on OOM.
/// @param out Buffer state to initialize.
/// @param initial_cap Requested initial allocation estimate.
/// @param max_output Maximum logical decoded length.
/// @return 1 on success, or 0 after raising an allocation trap.
static int out_init(output_buffer_t *out, size_t initial_cap, size_t max_output) {
    out->capacity = initial_cap > 256 ? initial_cap : 256;
    out->data = (uint8_t *)malloc(out->capacity);
    if (!out->data) {
        rt_trap("rt_compress: memory allocation failed");
        out->data = NULL;
        out->capacity = 0;
        out->len = 0;
        out->max_output = max_output;
        out->owns_data = false;
        out->error = "rt_compress: memory allocation failed";
        return 0;
    }
    out->len = 0;
    out->max_output = max_output;
    out->owns_data = true;
    out->error = NULL;
    return 1;
}

/// @brief Initialize an inflate output view over an exact caller-owned destination.
/// @details The fixed view never grows or frees @p data. Setting both capacity and output limit
///          to @p size makes any decoded byte beyond the caller's exact destination fail through
///          the ordinary output-limit path before a write occurs.
/// @param out Output-buffer state to initialize.
/// @param data Borrowed destination; must be non-NULL even when @p size is zero.
/// @param size Exact destination capacity in bytes.
/// @return 1 for valid arguments, otherwise 0.
static int out_init_fixed(output_buffer_t *out, uint8_t *data, size_t size) {
    if (!out || !data)
        return 0;
    out->data = data;
    out->len = 0;
    out->capacity = size;
    out->max_output = size;
    out->owns_data = false;
    out->error = NULL;
    return 1;
}

/* S-20: Maximum decompressed output size (256 MB) to prevent decompression bombs */
/** Default decoded-output ceiling for public inflate and gunzip entry points. */
#define INFLATE_DEFAULT_MAX_OUTPUT (256u * 1024u * 1024u)

/// @brief Ensure room for more decoded bytes without exceeding the configured cap.
///
/// If the request would push total size past `out->max_output`, traps with an
/// output-limit message. Otherwise grows owned storage geometrically; fixed
/// caller-owned destinations never grow.
/// @param out Output buffer to check or grow.
/// @param need Additional decoded byte count.
/// @return One when capacity is available; otherwise zero with
///         `out->error` set and the prior allocation still owned by @p out.
static int out_ensure(output_buffer_t *out, size_t need) {
    if (need > SIZE_MAX - out->len) {
        out->error = "Inflate: output size overflow";
        return 0;
    }
    size_t required = out->len + need;
    if (required > out->max_output) {
        out->error = "Inflate: decompressed output exceeds limit";
        return 0;
    }

    if (required <= out->capacity)
        return 1;
    if (!out->owns_data) {
        out->error = "Inflate: decoded output exceeds destination";
        return 0;
    }
    size_t new_cap = out->capacity ? out->capacity : 256;
    while (new_cap < required) {
        if (new_cap > out->max_output / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap > out->max_output)
        new_cap = out->max_output;
    uint8_t *new_data = (uint8_t *)realloc(out->data, new_cap);
    if (!new_data) {
        out->error = "Inflate: out of memory";
        return 0;
    }
    out->data = new_data;
    out->capacity = new_cap;
    return 1;
}

/// @brief Append a single literal byte to the output buffer.
/// @param out Output buffer receiving the byte.
/// @param b Literal value.
/// @return 1 on success, or 0 after a capacity/limit trap.
static int out_byte(output_buffer_t *out, uint8_t b) {
    if (!out_ensure(out, 1))
        return 0;
    out->data[out->len++] = b;
    return 1;
}

/// @brief Copy `length` bytes from `distance` back in the output (LZ77 back-ref).
///
/// Implements the DEFLATE back-reference primitive: copy a run from
/// already-decompressed output. Crucially, copies must overlap correctly
/// when `length > distance` (e.g., RLE-style "AAAAA" expansion), so the
/// loop must walk byte-by-byte rather than `memcpy`. Caller has already
/// validated that `distance <= out->len`.
/// @param out Output buffer containing the already-decoded history window.
/// @param distance Positive backward distance.
/// @param length Positive number of bytes to reproduce.
/// @return 1 on success, or 0 after a capacity/limit trap.
static int out_copy(output_buffer_t *out, int distance, int length) {
    if (!out_ensure(out, length))
        return 0;
    size_t src = out->len - distance;
    if (distance == 1) {
        memset(out->data + out->len, out->data[src], (size_t)length);
        out->len += (size_t)length;
        return 1;
    }
    if (distance >= length) {
        memcpy(out->data + out->len, out->data + src, (size_t)length);
        out->len += (size_t)length;
        return 1;
    }
    for (int i = 0; i < length; i++) {
        out->data[out->len++] = out->data[src++];
    }
    return 1;
}

/// @brief Append an exact byte span to accumulated output.
/// @param out Output buffer to extend.
/// @param data Source bytes; may be `NULL` only when @p len is zero.
/// @param len Number of bytes to append.
/// @return 1 on success, or 0 after a capacity/limit trap.
static int out_append(output_buffer_t *out, const uint8_t *data, size_t len) {
    if (!out_ensure(out, len))
        return 0;
    if (len > 0)
        memcpy(out->data + out->len, data, len);
    out->len += len;
    return 1;
}

/// @brief Release the output buffer's heap allocation.
/// @param out Buffer to reset; borrowed fixed destinations are not freed.
static void out_free(output_buffer_t *out) {
    if (out->owns_data)
        free(out->data);
    out->data = NULL;
    out->capacity = 0;
    out->len = 0;
    out->owns_data = false;
    out->error = NULL;
}

//=============================================================================
// DEFLATE Decompression
//=============================================================================

/// @brief Inflate a DEFLATE type-0 (stored/uncompressed) block.
///
/// Aligns the bit reader to a byte boundary, reads the 16-bit LEN and
/// its bitwise complement NLEN, verifies LEN ^ NLEN == 0xFFFF as
/// specified in RFC 1951 §3.2.4, then copies LEN bytes verbatim into
/// the output. Returns false on truncated data or the LEN/NLEN check
/// failing.
/// @param br Reader positioned immediately after the stored block header.
/// @param out Output buffer receiving the literal payload.
/// @return `true` when the complete stored block is valid and copied;
/// otherwise `false`.
static bool inflate_stored(bit_reader_t *br, output_buffer_t *out) {
    // Align to byte boundary
    br_align(br);

    // Read LEN and NLEN
    if (br->pos > br->len || br->len - br->pos < 4)
        return false;

    uint16_t len = br->data[br->pos] | (br->data[br->pos + 1] << 8);
    uint16_t nlen = br->data[br->pos + 2] | (br->data[br->pos + 3] << 8);
    br->pos += 4;

    // Verify
    if ((len ^ nlen) != 0xFFFF)
        return false;

    // Copy bytes
    if ((size_t)len > br->len - br->pos)
        return false;

    if (!out_ensure(out, len))
        return false;
    memcpy(out->data + out->len, br->data + br->pos, len);
    out->len += len;
    br->pos += len;

    return true;
}

/// @brief Inflate a DEFLATE Huffman-coded block (type 1 fixed or type 2 dynamic).
///
/// Runs the literal/length/end-of-block symbol loop: symbols 0..255
/// are literal bytes written straight to output; 256 ends the block;
/// 257..285 are length codes that pair with a following distance code
/// to form an LZ77 back-reference. Extra bits follow length/distance
/// codes per RFC 1951 §3.2.5. Validates the back-reference distance
/// against the current output length before calling `out_copy` (which
/// handles overlapping copies for RLE-style expansion). The two tree
/// arguments are shared between fixed and dynamic callers — same loop,
/// different trees.
/// @param br Reader positioned at the first encoded symbol.
/// @param out Output/history buffer.
/// @param lit_tree Literal/length decode table.
/// @param dist_tree Distance decode table.
/// @return `true` after a valid end-of-block symbol; otherwise `false`.
static bool inflate_huffman(bit_reader_t *br,
                            output_buffer_t *out,
                            huffman_tree_t *lit_tree,
                            huffman_tree_t *dist_tree) {
    while (true) {
        int sym = decode_symbol(lit_tree, br);
        if (sym < 0)
            return false;

        if (sym < 256) {
            // Literal byte
            if (!out_byte(out, (uint8_t)sym))
                return false;
        } else if (sym == 256) {
            // End of block
            return true;
        } else if (sym <= 285) {
            // Length code
            int len_idx = sym - 257;
            int length = length_base[len_idx];
            int extra = length_extra_bits[len_idx];
            if (extra > 0) {
                length += br_read(br, extra);
                if (br->error)
                    return false;
            }

            // Distance code
            int dist_sym = decode_symbol(dist_tree, br);
            if (dist_sym < 0 || dist_sym >= 30)
                return false;

            int distance = dist_base[dist_sym];
            extra = dist_extra_bits[dist_sym];
            if (extra > 0) {
                distance += br_read(br, extra);
                if (br->error)
                    return false;
            }

            // Validate distance
            if ((size_t)distance > out->len)
                return false;

            // Copy from output buffer
            if (!out_copy(out, distance, length))
                return false;
        } else {
            return false; // Invalid symbol
        }
    }
}

/// @brief Inflate a DEFLATE type-2 (dynamic Huffman) block.
///
/// Dynamic blocks ship their own Huffman trees, built via a three-level
/// scheme (RFC 1951 §3.2.7):
///   1. Read HLIT/HDIST/HCLEN counts, then HCLEN 3-bit lengths in the
///      permuted `code_length_order` sequence to build a meta-tree for
///      decoding code-length values.
///   2. Use the meta-tree to decode HLIT+HDIST code lengths, with
///      special symbols 16/17/18 encoding run-length-compressed
///      repeats of prior/zero lengths.
///   3. Split the decoded lengths into literal/length and distance
///      trees, then delegate the symbol loop to `inflate_huffman`.
/// Frees all transient trees on every exit path — traps aren't used
/// here because the caller may need to fall back cleanly.
/// @param br Reader positioned after the dynamic block header.
/// @param out Output/history buffer.
/// @return `true` when the dynamic tables and encoded block are valid;
/// otherwise `false`.
static bool inflate_dynamic(bit_reader_t *br, output_buffer_t *out) {
    // Read header
    int hlit = br_read(br, 5) + 257; // Number of literal/length codes
    int hdist = br_read(br, 5) + 1;  // Number of distance codes
    int hclen = br_read(br, 4) + 4;  // Number of code length codes
    if (br->error)
        return false;

    if (hlit > MAX_LIT_CODES || hdist > MAX_DIST_CODES)
        return false;

    // Read code length code lengths
    uint8_t cl_lengths[MAX_CODE_LEN_CODES] = {0};
    for (int i = 0; i < hclen; i++) {
        cl_lengths[code_length_order[i]] = (uint8_t)br_read(br, 3);
        if (br->error)
            return false;
    }

    // Build code length tree
    huffman_tree_t cl_tree = {0};
    if (!build_huffman_tree(&cl_tree, cl_lengths, MAX_CODE_LEN_CODES))
        return false;

    // Decode literal/length and distance code lengths
    uint8_t lengths[MAX_LIT_CODES + MAX_DIST_CODES] = {0};
    int total_codes = hlit + hdist;
    int i = 0;

    while (i < total_codes) {
        int sym = decode_symbol(&cl_tree, br);
        if (sym < 0) {
            free_huffman_tree(&cl_tree);
            return false;
        }

        if (sym < 16) {
            // Literal length
            lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            // Repeat previous
            if (i == 0) {
                free_huffman_tree(&cl_tree);
                return false;
            }
            int repeat = br_read(br, 2) + 3;
            if (br->error) {
                free_huffman_tree(&cl_tree);
                return false;
            }
            uint8_t prev = lengths[i - 1];
            if (repeat > total_codes - i) {
                free_huffman_tree(&cl_tree);
                return false;
            }
            while (repeat-- > 0 && i < total_codes)
                lengths[i++] = prev;
        } else if (sym == 17) {
            // Repeat 0, 3-10 times
            int repeat = br_read(br, 3) + 3;
            if (br->error) {
                free_huffman_tree(&cl_tree);
                return false;
            }
            if (repeat > total_codes - i) {
                free_huffman_tree(&cl_tree);
                return false;
            }
            while (repeat-- > 0 && i < total_codes)
                lengths[i++] = 0;
        } else if (sym == 18) {
            // Repeat 0, 11-138 times
            int repeat = br_read(br, 7) + 11;
            if (br->error) {
                free_huffman_tree(&cl_tree);
                return false;
            }
            if (repeat > total_codes - i) {
                free_huffman_tree(&cl_tree);
                return false;
            }
            while (repeat-- > 0 && i < total_codes)
                lengths[i++] = 0;
        } else {
            free_huffman_tree(&cl_tree);
            return false;
        }
    }

    free_huffman_tree(&cl_tree);

    // Build literal/length and distance trees
    huffman_tree_t lit_tree = {0}, dist_tree = {0};

    if (hlit <= 256 || lengths[256] == 0)
        return false;

    if (!build_huffman_tree(&lit_tree, lengths, hlit))
        return false;

    if (!build_huffman_tree(&dist_tree, lengths + hlit, hdist)) {
        free_huffman_tree(&lit_tree);
        return false;
    }

    // Decode block
    bool ok = inflate_huffman(br, out, &lit_tree, &dist_tree);

    free_huffman_tree(&lit_tree);
    free_huffman_tree(&dist_tree);

    return ok;
}

/// @brief Top-level DEFLATE decompression driver.
///
/// Walks a stream of DEFLATE blocks until BFINAL=1 is seen. Each block
/// is decoded via `inflate_stored`, `inflate_huffman` (with shared
/// fixed trees), or `inflate_dynamic` based on its 2-bit type. Traps
/// on truncated input, invalid block types, or output exceeding the
/// 256MB decompression-bomb limit. After the final block, any residual
/// non-zero bits in the byte-aligned tail are a stream corruption —
/// not just padding — and also trap.
/// @param data Borrowed raw RFC 1951 stream.
/// @param len Number of accessible encoded bytes.
/// @param max_output Maximum decoded bytes, and exact fixed-buffer capacity
/// when @p fixed_output is non-`NULL`.
/// @param out_len Receives the decoded byte count when non-`NULL`.
/// @param consumed_bytes Receives the byte-aligned encoded member length when
/// non-`NULL`.
/// @param allow_trailing Permit caller-owned bytes after the final DEFLATE
/// block, as required for wrapped formats.
/// @param fixed_output Optional caller-owned exact destination; `NULL` selects
/// a growable `malloc`-owned result.
/// @return The fixed destination or a heap allocation on success; traps and
/// returns `NULL` for malformed input, limit violations, or allocation failure.
static uint8_t *inflate_raw_limited_to_ex(const uint8_t *data,
                                          size_t len,
                                          size_t max_output,
                                          size_t *out_len,
                                          size_t *consumed_bytes,
                                          bool allow_trailing,
                                          uint8_t *fixed_output) {
    init_fixed_trees();
    if (!fixed_lit_tree.symbols || !fixed_dist_tree.symbols) {
        rt_trap("Inflate: fixed Huffman tables are unavailable");
        return NULL;
    }

    bit_reader_t br;
    br_init(&br, data, len);

    output_buffer_t out;
    if (fixed_output) {
        if (!out_init_fixed(&out, fixed_output, max_output))
            return NULL;
    } else {
        size_t estimate = len > max_output / 4 ? max_output : len * 4;
        if (!out_init(&out, estimate, max_output))
            return NULL; // Estimate 4x expansion without overflowing.
    }

    bool last_block = false;

    while (!last_block) {
        if (!br_has_data(&br)) {
            out_free(&out);
            rt_trap("Inflate: unexpected end of data");
            return NULL;
        }

        // Read block header
        last_block = br_read(&br, 1);
        int block_type = br_read(&br, 2);
        if (br.error) {
            out_free(&out);
            rt_trap("Inflate: unexpected end of data");
            return NULL;
        }

        bool ok = false;
        switch (block_type) {
            case RT_HUFFMAN_STORED:
                ok = inflate_stored(&br, &out);
                break;
            case RT_HUFFMAN_FIXED:
                ok = inflate_huffman(&br, &out, &fixed_lit_tree, &fixed_dist_tree);
                break;
            case RT_HUFFMAN_DYNAMIC:
                ok = inflate_dynamic(&br, &out);
                break;
            default:
                out_free(&out);
                rt_trap("Inflate: invalid block type");
                return NULL;
        }

        if (!ok) {
            const char *error = out.error;
            out_free(&out);
            rt_trap(error ? error : "Inflate: invalid compressed data");
            return NULL;
        }
    }

    size_t consumed_bits = br.pos * 8u - (size_t)br.bits_in_buf;
    int byte_padding_bits = (int)((8u - (consumed_bits & 7u)) & 7u);
    if (byte_padding_bits > 0) {
        uint32_t padding_mask = (1U << byte_padding_bits) - 1U;
        if ((br.buffer & padding_mask) != 0) {
            out_free(&out);
            rt_trap("Inflate: trailing data after final block");
            return NULL;
        }
    }

    size_t compressed_bytes = (consumed_bits + 7u) / 8u;
    if (compressed_bytes > len) {
        out_free(&out);
        rt_trap("Inflate: invalid compressed data");
        return NULL;
    }
    if (consumed_bytes)
        *consumed_bytes = compressed_bytes;

    if (!allow_trailing && compressed_bytes < len) {
        out_free(&out);
        rt_trap("Inflate: trailing data after final block");
        return NULL;
    }

    if (!allow_trailing && br.bits_in_buf > 0) {
        int padding_bits = br.bits_in_buf > 7 ? 7 : br.bits_in_buf;
        uint32_t padding_mask = (1U << padding_bits) - 1U;
        if ((br.buffer & padding_mask) != 0 || br.bits_in_buf > 7) {
            out_free(&out);
            rt_trap("Inflate: trailing data after final block");
            return NULL;
        }
    }

    if (out_len)
        *out_len = out.len;
    return out.data;
}

/// @brief Allocate and decode a bounded raw DEFLATE stream.
/// @details Compatibility wrapper around the destination-aware driver. A NULL fixed destination
///          selects the original growable malloc-owned output behavior.
/// @param data Borrowed raw RFC 1951 stream.
/// @param len Number of accessible encoded bytes.
/// @param max_output Maximum allowed decoded length.
/// @param out_len Receives the decoded byte count.
/// @param consumed_bytes Receives encoded member length when non-`NULL`.
/// @param allow_trailing Whether bytes after the final block are permitted.
/// @return `malloc`-owned decoded bytes on success, or `NULL` after a trap.
static uint8_t *inflate_raw_limited_ex(const uint8_t *data,
                                       size_t len,
                                       size_t max_output,
                                       size_t *out_len,
                                       size_t *consumed_bytes,
                                       bool allow_trailing) {
    return inflate_raw_limited_to_ex(
        data, len, max_output, out_len, consumed_bytes, allow_trailing, NULL);
}

/// @brief Inflate raw DEFLATE into a fresh runtime Bytes object.
/// @details Converts the native decoder's temporary output into GC-managed
/// storage and releases the native buffer.
/// @param data Borrowed raw RFC 1951 stream.
/// @param len Number of accessible encoded bytes.
/// @param max_output Maximum allowed decoded length.
/// @param consumed_bytes Receives encoded member length when non-`NULL`.
/// @param allow_trailing Whether wrapped-format bytes may follow the stream.
/// @return Fresh runtime Bytes object, or `NULL` on failure.
static void *inflate_data_limited_ex(const uint8_t *data,
                                     size_t len,
                                     size_t max_output,
                                     size_t *consumed_bytes,
                                     bool allow_trailing) {
    size_t raw_len = 0;
    uint8_t *raw =
        inflate_raw_limited_ex(data, len, max_output, &raw_len, consumed_bytes, allow_trailing);
    if (!raw)
        return NULL;
    return compress_native_to_managed_bytes(raw, raw_len, "Inflate: result allocation failed");
}

/// @brief Inflate one complete raw DEFLATE stream with an explicit ceiling.
/// @param data Borrowed encoded bytes.
/// @param len Encoded byte count.
/// @param max_output Maximum decoded byte count.
/// @return Fresh runtime Bytes object, or `NULL` on failure.
static void *inflate_data_limited(const uint8_t *data, size_t len, size_t max_output) {
    return inflate_data_limited_ex(data, len, max_output, NULL, false);
}

/// @brief Inflate one complete raw DEFLATE stream using the default 256 MiB ceiling.
/// @param data Borrowed encoded bytes.
/// @param len Encoded byte count.
/// @return Fresh runtime Bytes object, or `NULL` on failure.
static void *inflate_data(const uint8_t *data, size_t len) {
    return inflate_data_limited(data, len, INFLATE_DEFAULT_MAX_OUTPUT);
}

/// @brief Decode one GZIP-wrapped DEFLATE member per RFC 1952.
///
/// Validates magic bytes (0x1F 0x8B), checks method=deflate, then walks
/// the optional header fields selected by the flags byte: FEXTRA (2-byte
/// length + payload), FNAME (NUL-terminated), FCOMMENT (NUL-terminated),
/// FHCRC (2-byte CRC16 of the header so far). After the DEFLATE stream
/// ends, validates the 8-byte trailer (CRC32 of inflated data + ISIZE)
/// against the actual inflated result. Traps on any mismatch.
/// @param data Borrowed bytes beginning at a GZIP member header.
/// @param len Number of accessible bytes, including any later concatenated
/// members.
/// @param max_output Maximum decoded bytes permitted for this member.
/// @param member_len Receives this member's complete encoded byte length.
/// @param output_len Receives this member's decoded byte length.
/// @return Fresh malloc-owned bytes containing this member's payload, or
/// `NULL` after a format, checksum, limit, or allocation trap.
static uint8_t *gunzip_member_raw(
    const uint8_t *data, size_t len, size_t max_output, size_t *member_len, size_t *output_len) {
    if (len < 18) {
        rt_trap("Gunzip: data too short");
        return NULL;
    }

    // Verify magic
    if (data[0] != 0x1F || data[1] != 0x8B) {
        rt_trap("Gunzip: invalid magic number");
        return NULL;
    }

    // Check compression method
    if (data[2] != 0x08) {
        rt_trap("Gunzip: unsupported compression method");
        return NULL;
    }

    uint8_t flags = data[3];
    if (flags & 0xE0) {
        rt_trap("Gunzip: reserved flags set");
        return NULL;
    }

    // Skip header
    size_t pos = 10;

    // Skip FEXTRA
    if (flags & 0x04) {
        if (pos > len || len - pos < 2) {
            rt_trap("Gunzip: truncated header");
            return NULL;
        }
        size_t xlen = data[pos] | (data[pos + 1] << 8);
        if (xlen > len - pos - 2) {
            rt_trap("Gunzip: truncated extra field");
            return NULL;
        }
        pos += 2 + xlen;
    }

    // Skip FNAME
    if (flags & 0x08) {
        while (pos < len && data[pos] != 0)
            pos++;
        if (pos >= len) {
            rt_trap("Gunzip: truncated filename");
            return NULL;
        }
        pos++; // Skip null terminator
    }

    // Skip FCOMMENT
    if (flags & 0x10) {
        while (pos < len && data[pos] != 0)
            pos++;
        if (pos >= len) {
            rt_trap("Gunzip: truncated comment");
            return NULL;
        }
        pos++;
    }

    // Skip FHCRC
    if (flags & 0x02) {
        if (pos > len || len - pos < 2) {
            rt_trap("Gunzip: truncated header CRC");
            return NULL;
        }
        uint16_t expected_hcrc = (uint16_t)(data[pos] | (data[pos + 1] << 8));
        uint16_t actual_hcrc = (uint16_t)(rt_crc32_compute(data, pos) & 0xFFFFu);
        if (actual_hcrc != expected_hcrc) {
            rt_trap("Gunzip: header CRC mismatch");
            return NULL;
        }
        pos += 2;
    }

    if (pos >= len - 8) {
        rt_trap("Gunzip: truncated data");
        return NULL;
    }

    size_t deflate_consumed = 0;
    size_t result_len = 0;
    uint8_t *result = inflate_raw_limited_ex(
        data + pos, len - pos, max_output, &result_len, &deflate_consumed, true);
    if (!result)
        return NULL;

    if (deflate_consumed > len - pos || len - pos - deflate_consumed < 8) {
        free(result);
        rt_trap("Gunzip: truncated trailer");
        return NULL;
    }

    // Extract trailer at the byte immediately after the DEFLATE member.
    size_t trailer_pos = pos + deflate_consumed;
    uint32_t expected_crc = (uint32_t)data[trailer_pos] | ((uint32_t)data[trailer_pos + 1] << 8) |
                            ((uint32_t)data[trailer_pos + 2] << 16) |
                            ((uint32_t)data[trailer_pos + 3] << 24);
    uint32_t expected_size =
        (uint32_t)data[trailer_pos + 4] | ((uint32_t)data[trailer_pos + 5] << 8) |
        ((uint32_t)data[trailer_pos + 6] << 16) | ((uint32_t)data[trailer_pos + 7] << 24);

    // Verify CRC
    uint32_t actual_crc = rt_crc32_compute(result, result_len);
    if (actual_crc != expected_crc) {
        free(result);
        rt_trap("Gunzip: CRC mismatch");
        return NULL;
    }

    // Verify size (mod 2^32)
    if ((result_len & UINT32_MAX) != expected_size) {
        free(result);
        rt_trap("Gunzip: size mismatch");
        return NULL;
    }

    if (member_len)
        *member_len = trailer_pos + 8;
    if (output_len)
        *output_len = result_len;
    return result;
}

/// @brief Derive the smaller absolute/ratio-based GZIP output ceiling.
/// @details A zero ratio disables the ratio constraint. Multiplication and
///          slack addition saturate so hostile encoded lengths cannot wrap the
///          computed bound.
/// @param encoded_len Complete encoded GZIP byte count.
/// @param max_output Absolute decoded byte ceiling.
/// @param max_expansion_ratio Maximum decoded/encoded multiplier, or zero.
/// @param expansion_slack Additional bytes allowed by the ratio constraint.
/// @return Effective decoded byte ceiling.
static size_t gunzip_output_limit(size_t encoded_len,
                                  size_t max_output,
                                  size_t max_expansion_ratio,
                                  size_t expansion_slack) {
    if (max_expansion_ratio == 0)
        return max_output;

    size_t ratio_limit;
    if (encoded_len > (SIZE_MAX - expansion_slack) / max_expansion_ratio)
        ratio_limit = SIZE_MAX;
    else
        ratio_limit = encoded_len * max_expansion_ratio + expansion_slack;
    return ratio_limit < max_output ? ratio_limit : max_output;
}

/// @brief Decode concatenated GZIP members into one bounded native buffer.
/// @details The first member's allocation becomes the aggregate directly, so
///          the common single-member path performs no decoded-body copy.
///          Later members are limited by the aggregate remaining budget and
///          appended after checksum validation. Local recovery frees both
///          aggregate and in-flight member storage before converting traps to
///          a zero status.
/// @param data Borrowed complete GZIP stream.
/// @param len Encoded byte count.
/// @param max_output Absolute aggregate decoded byte ceiling.
/// @param max_expansion_ratio Maximum decoded/encoded multiplier, or zero.
/// @param expansion_slack Additional bytes allowed by the ratio constraint.
/// @param out_data Receives malloc-owned aggregate bytes.
/// @param out_len Receives aggregate decoded length.
/// @param error_buffer Optional failure diagnostic destination.
/// @param error_buffer_size Capacity of @p error_buffer.
/// @return One on complete success, otherwise zero.
static int gunzip_raw_limited(const uint8_t *data,
                              size_t len,
                              size_t max_output,
                              size_t max_expansion_ratio,
                              size_t expansion_slack,
                              uint8_t **out_data,
                              size_t *out_len,
                              char *error_buffer,
                              size_t error_buffer_size) {
    uint8_t *volatile aggregate = NULL;
    uint8_t *volatile member = NULL;
    jmp_buf recovery;

    if (out_data)
        *out_data = NULL;
    if (out_len)
        *out_len = 0;
    if (error_buffer && error_buffer_size > 0)
        error_buffer[0] = '\0';
    if (!data || !out_data || !out_len || len < 18) {
        if (error_buffer && error_buffer_size > 0)
            snprintf(error_buffer, error_buffer_size, "%s", "Gunzip: data too short");
        return 0;
    }

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        if (error_buffer && error_buffer_size > 0)
            compress_save_trap_error(
                error_buffer, error_buffer_size, "Gunzip: invalid compressed data");
        rt_trap_clear_recovery();
        free((void *)member);
        free((void *)aggregate);
        return 0;
    }

    const size_t effective_limit =
        gunzip_output_limit(len, max_output, max_expansion_ratio, expansion_slack);
    output_buffer_t out = {0};
    size_t pos = 0;
    int have_aggregate = 0;

    while (pos < len) {
        size_t member_len = 0;
        size_t member_output_len = 0;
        size_t remaining = have_aggregate ? effective_limit - out.len : effective_limit;
        member =
            gunzip_member_raw(data + pos, len - pos, remaining, &member_len, &member_output_len);
        if (!member) {
            rt_trap("Gunzip: member decode failed");
            break;
        }
        if (member_len == 0 || member_len > len - pos) {
            rt_trap("Gunzip: invalid member length");
            break;
        }

        if (!have_aggregate) {
            out.data = (uint8_t *)member;
            out.len = member_output_len;
            out.capacity = member_output_len;
            out.max_output = effective_limit;
            out.owns_data = true;
            out.error = NULL;
            aggregate = member;
            member = NULL;
            have_aggregate = 1;
        } else {
            if (!out_append(&out, (const uint8_t *)member, member_output_len)) {
                rt_trap(out.error ? out.error : "Gunzip: output append failed");
                break;
            }
            aggregate = out.data;
            free((void *)member);
            member = NULL;
        }
        pos += member_len;
    }

    rt_trap_clear_recovery();
    if (pos != len || !have_aggregate) {
        free((void *)member);
        free((void *)aggregate);
        if (error_buffer && error_buffer_size > 0 && error_buffer[0] == '\0')
            snprintf(error_buffer, error_buffer_size, "%s", "Gunzip: invalid compressed data");
        return 0;
    }

    *out_data = (uint8_t *)aggregate;
    *out_len = out.len;
    return 1;
}

/// @brief Decode a possibly concatenated GZIP stream into managed Bytes.
/// @details Uses the bounded native decoder, then performs one final copy into
///          runtime-managed storage. Allocation recovery releases the native
///          result before re-raising.
/// @param data Borrowed complete GZIP byte stream.
/// @param len Encoded byte count.
/// @return Fresh Bytes containing all member payloads concatenated in order,
/// or `NULL` after a validation, limit, or allocation trap.
static void *gunzip_data(const uint8_t *data, size_t len) {
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    char decode_error[256];
    if (!gunzip_raw_limited(data,
                            len,
                            INFLATE_DEFAULT_MAX_OUTPUT,
                            0,
                            0,
                            &raw,
                            &raw_len,
                            decode_error,
                            sizeof(decode_error))) {
        rt_trap(decode_error[0] ? decode_error : "Gunzip: invalid compressed data");
        return NULL;
    }

    return compress_native_to_managed_bytes(raw, raw_len, "Gunzip: result allocation failed");
}

/// @copydoc rt_compress_gunzip_raw()
int rt_compress_gunzip_raw(const uint8_t *data,
                           size_t len,
                           size_t max_output,
                           size_t max_expansion_ratio,
                           size_t expansion_slack,
                           uint8_t **out_data,
                           size_t *out_len) {
    return gunzip_raw_limited(
        data, len, max_output, max_expansion_ratio, expansion_slack, out_data, out_len, NULL, 0);
}

//=============================================================================
// Public API
//=============================================================================

/// @brief `Compress.Deflate(data)` — RFC 1951 raw DEFLATE at default level (6).
///
/// Produces a raw DEFLATE bitstream with no GZIP/zlib wrapper. Use
/// `Gzip` if you need a `.gz`-compatible output. Traps on NULL input.
///
/// @param data Source `rt_bytes`.
/// @return Owned `rt_bytes` containing the compressed stream.
void *rt_compress_deflate(void *data) {
    int64_t len = 0;
    const uint8_t *src = compress_bytes_view(data, "Compress.Deflate: invalid data", &len);
    if (!src && len > 0)
        return NULL;
    return deflate_data(src, len, DEFLATE_DEFAULT_LEVEL);
}

/// @brief `Compress.DeflateLvl(data, level)` — explicit-level DEFLATE.
///
/// `level` is 1 (fastest, stored blocks) through 9 (best compression).
/// Levels 2-9 currently all use the same fixed-Huffman path; the level
/// only changes the LZ77 hash-chain depth (`max_chain = 4 << level`).
/// Traps on NULL data or out-of-range level.
///
/// @param data  Source `rt_bytes`.
/// @param level Compression level (1..9).
/// @return Owned `rt_bytes` containing the compressed stream.
void *rt_compress_deflate_lvl(void *data, int64_t level) {
    int64_t len = 0;
    const uint8_t *src = compress_bytes_view(data, "Compress.DeflateLvl: invalid data", &len);
    if (!src && len > 0)
        return NULL;
    if (level < DEFLATE_MIN_LEVEL || level > DEFLATE_MAX_LEVEL) {
        rt_trap("Compress.DeflateLvl: level must be 1-9");
        return NULL;
    }
    return deflate_data(src, len, (int)level);
}

/// @brief `Compress.Inflate(data)` — decompress a raw DEFLATE stream.
///
/// Accepts only the bare RFC 1951 bitstream — see `Gunzip` for
/// GZIP-wrapped data. Traps on NULL input or any structural error
/// in the compressed stream.
///
/// @param data Compressed `rt_bytes`.
/// @return Owned `rt_bytes` containing the original payload.
void *rt_compress_inflate(void *data) {
    int64_t len = 0;
    const uint8_t *src = compress_bytes_view(data, "Compress.Inflate: invalid data", &len);
    if (!src && len > 0)
        return NULL;
    return inflate_data(src, len);
}

/// @brief Inflate a complete raw DEFLATE stream under a caller-supplied ceiling.
/// @param data Runtime Bytes containing RFC 1951 data.
/// @param max_output Maximum decoded length; negative values trap.
/// @return Fresh decoded Bytes, or `NULL` after invalid input, malformed data,
/// output-limit, or allocation failure.
void *rt_compress_inflate_limit(void *data, int64_t max_output) {
    int64_t len = 0;
    const uint8_t *src = compress_bytes_view(data, "Compress.InflateLimit: invalid data", &len);
    if (!src && len > 0)
        return NULL;
    if (max_output < 0) {
        rt_trap("Compress.InflateLimit: max output is negative");
        return NULL;
    }
    return inflate_data_limited(src, len, (size_t)max_output);
}

/// @brief Inflate raw DEFLATE into caller-owned native output.
/// @details Converts internal traps into a zero status so worker/native
/// decoders can operate without allocating runtime objects.
/// @param data Borrowed encoded bytes.
/// @param len Encoded byte count.
/// @param max_output Maximum decoded byte count.
/// @param out_data Receives a `malloc`-owned buffer on success and `NULL` on
/// failure.
/// @param out_len Receives decoded length on success and zero on failure.
/// @return 1 on success; 0 for invalid arguments, malformed data, limit
/// violation, or allocation failure.
int rt_compress_inflate_raw(
    const uint8_t *data, size_t len, size_t max_output, uint8_t **out_data, size_t *out_len) {
    jmp_buf recovery;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    int ok = 0;
    if (out_data)
        *out_data = NULL;
    if (out_len)
        *out_len = 0;
    if (!data || !out_data || !out_len)
        return 0;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        raw = inflate_raw_limited_ex(data, len, max_output, &raw_len, NULL, false);
        ok = raw != NULL || raw_len == 0;
    }
    rt_trap_clear_recovery();
    if (!ok) {
        free(raw);
        return 0;
    }
    *out_data = raw;
    *out_len = raw_len;
    return 1;
}

/// @brief Compute RFC 1950 Adler-32 over a bounded byte span.
/// @details Processes at most 5,552 bytes between reductions, the largest safe block for the
///          32-bit running sums. A NULL pointer is accepted only for an empty span.
/// @param data Input bytes.
/// @param len Number of input bytes.
/// @return `(s2 << 16) | s1`, initialized to the RFC-mandated value one.
static uint32_t compress_adler32(const uint8_t *data, size_t len) {
    uint32_t s1 = 1;
    uint32_t s2 = 0;

    while (len > 0) {
        size_t chunk = len < 5552u ? len : 5552u;
        for (size_t i = 0; i < chunk; i++) {
            s1 += data[i];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
        data += chunk;
        len -= chunk;
    }
    return (s2 << 16) | s1;
}

/// @brief Validate the RFC 1950 CMF/FLG header of a complete zlib stream.
/// @details Requires DEFLATE compression, a window no larger than 32 KiB, a valid FCHECK value,
///          and no preset dictionary. The six-byte minimum covers CMF/FLG and Adler-32; the raw
///          DEFLATE decoder subsequently rejects a missing block payload.
/// @param data Complete candidate stream.
/// @param len Candidate byte count.
/// @return 1 when the wrapper header can be decoded without a dictionary, otherwise 0.
static int compress_validate_zlib_header(const uint8_t *data, size_t len) {
    uint8_t cmf;
    uint8_t flg;

    if (!data || len < 6)
        return 0;
    cmf = data[0];
    flg = data[1];
    if ((cmf & 0x0Fu) != 8u || (cmf >> 4) > 7u)
        return 0;
    if ((((uint16_t)cmf << 8) | (uint16_t)flg) % 31u != 0u)
        return 0;
    if ((flg & 0x20u) != 0u)
        return 0;
    return 1;
}

/// @brief Decode one complete zlib-wrapped DEFLATE stream into exact caller storage.
/// @details See the public declaration for the wrapper checks. Trap recovery converts every raw
///          DEFLATE structural failure into a zero return while preserving the caller-owned
///          destination. Passing `allow_trailing=false` makes the raw decoder consume precisely
///          the bytes between CMF/FLG and Adler-32, so hidden bytes before the checksum cannot be
///          accepted as padding.
/// @param data Complete RFC 1950 stream.
/// @param len Encoded byte count.
/// @param output Caller-owned destination.
/// @param output_size Exact required decoded length and destination capacity.
/// @return 1 only when framing, exact output length, raw stream, and Adler-32
/// all validate; otherwise 0.
int rt_compress_inflate_zlib_into(const uint8_t *data,
                                  size_t len,
                                  uint8_t *output,
                                  size_t output_size) {
    jmp_buf recovery;
    size_t decoded_size = 0;
    uint8_t *decoded = NULL;
    uint32_t expected_adler;
    volatile int ok = 0;

    if (!output || !compress_validate_zlib_header(data, len))
        return 0;
    expected_adler = ((uint32_t)data[len - 4] << 24) | ((uint32_t)data[len - 3] << 16) |
                     ((uint32_t)data[len - 2] << 8) | (uint32_t)data[len - 1];

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        decoded = inflate_raw_limited_to_ex(
            data + 2, len - 6, output_size, &decoded_size, NULL, false, output);
        ok = decoded == output && decoded_size == output_size;
    }
    rt_trap_clear_recovery();
    if (!ok)
        return 0;
    return compress_adler32(output, output_size) == expected_adler ? 1 : 0;
}

/// @brief `Compress.Gzip(data)` — RFC 1952 GZIP wrap at default level.
///
/// Emits a 10-byte GZIP header + DEFLATE payload + 8-byte trailer
/// (CRC32 + uncompressed size mod 2^32). Suitable for writing `.gz`
/// files or HTTP `Content-Encoding: gzip`.
///
/// @param data Source `rt_bytes`.
/// @return Owned `rt_bytes` containing the GZIP-wrapped stream.
void *rt_compress_gzip(void *data) {
    int64_t len = 0;
    const uint8_t *src = compress_bytes_view(data, "Compress.Gzip: invalid data", &len);
    if (!src && len > 0)
        return NULL;
    return gzip_data(src, len, DEFLATE_DEFAULT_LEVEL);
}

/// @brief `Compress.GzipLvl(data, level)` — explicit-level GZIP wrap.
///
/// Like `Gzip` but uses the supplied compression level (1..9) for the
/// inner DEFLATE pass.
///
/// @param data  Source `rt_bytes`.
/// @param level Compression level (1..9).
/// @return Owned `rt_bytes` containing the GZIP-wrapped stream.
void *rt_compress_gzip_lvl(void *data, int64_t level) {
    int64_t len = 0;
    const uint8_t *src = compress_bytes_view(data, "Compress.GzipLvl: invalid data", &len);
    if (!src && len > 0)
        return NULL;
    if (level < DEFLATE_MIN_LEVEL || level > DEFLATE_MAX_LEVEL) {
        rt_trap("Compress.GzipLvl: level must be 1-9");
        return NULL;
    }
    return gzip_data(src, len, (int)level);
}

/// @brief `Compress.Gunzip(data)` — decode a GZIP-wrapped DEFLATE stream.
///
/// Validates each member's magic bytes, optional FEXTRA/FNAME/FCOMMENT/FHCRC
/// header fields, and trailing CRC32 + size. Concatenated GZIP members are
/// inflated in order and returned as one Bytes payload.
///
/// @param data GZIP-wrapped `rt_bytes`.
/// @return Owned `rt_bytes` containing the inflated payload.
void *rt_compress_gunzip(void *data) {
    int64_t len = 0;
    const uint8_t *src = compress_bytes_view(data, "Compress.Gunzip: invalid data", &len);
    if (!src && len > 0)
        return NULL;
    return gunzip_data(src, len);
}

/// @brief `Compress.DeflateStr(text)` — string convenience wrapper.
///
/// Copies the runtime String bytes, deflates them at the default level, and
/// returns the result. Useful for compressing JSON / log payloads.
///
/// @param text Source string.
/// @return Owned `rt_bytes` of the compressed stream.
void *rt_compress_deflate_str(rt_string text) {
    if (!text) {
        rt_trap("Compress.DeflateStr: text is null");
        return NULL;
    }
    void *bytes = rt_bytes_from_str(text);
    if (!bytes)
        return NULL;
    return compress_run_string_bytes(bytes, 0, "Compress.DeflateStr: failed to deflate text");
}

/// @brief `Compress.InflateStr(data)` — inflate then copy bytes into a runtime String.
///
/// No UTF-8 validation is performed; use only when the original payload's
/// encoding is already known, or use `Inflate` plus an explicit codec.
///
/// @param data Compressed `rt_bytes`.
/// @return Owned `rt_string` with the inflated text.
rt_string rt_compress_inflate_str(void *data) {
    int64_t len = 0;
    const uint8_t *src = compress_bytes_view(data, "Compress.InflateStr: invalid data", &len);
    if (!src && len > 0)
        return rt_str_empty();
    void *result = inflate_data(src, len);
    if (!result)
        return rt_str_empty();
    return compress_bytes_to_str_or_release(result, "Compress.InflateStr: invalid UTF-8");
}

/// @brief `Compress.GzipStr(text)` — string convenience wrapper around Gzip.
///
/// @param text Source string.
/// @return Owned `rt_bytes` of the GZIP-wrapped stream.
void *rt_compress_gzip_str(rt_string text) {
    if (!text) {
        rt_trap("Compress.GzipStr: text is null");
        return NULL;
    }
    void *bytes = rt_bytes_from_str(text);
    if (!bytes)
        return NULL;
    return compress_run_string_bytes(bytes, 1, "Compress.GzipStr: failed to gzip text");
}

/// @brief `Compress.GunzipStr(data)` — gunzip then copy bytes into a runtime String.
///
/// @param data GZIP-wrapped `rt_bytes`.
/// @return Owned `rt_string` with the inflated text.
rt_string rt_compress_gunzip_str(void *data) {
    int64_t len = 0;
    const uint8_t *src = compress_bytes_view(data, "Compress.GunzipStr: invalid data", &len);
    if (!src && len > 0)
        return rt_str_empty();
    void *result = gunzip_data(src, len);
    if (!result)
        return rt_str_empty();
    return compress_bytes_to_str_or_release(result, "Compress.GunzipStr: invalid UTF-8");
}
