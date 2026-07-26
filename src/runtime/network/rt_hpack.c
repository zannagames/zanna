//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_hpack.c
// Purpose: HPACK (RFC 7541) header compression for HTTP/2: dynamic table,
//   integer/string coding, Huffman codec, and header-block encode/decode.
//
// Key invariants:
//   - HPACK indices are one-based: 1..61 select the RFC static table and
//     values from 62 onward select newest-first dynamic entries.
//   - Decoded names and values are validated before exposure to HTTP/2.
//   - Huffman tables are initialized exactly once per process.
//
// Ownership/Lifetime:
//   - Dynamic tables own copies of indexed names and values.
//   - Successful header-block decoding transfers a linked header list to the
//     caller; all temporary strings are released within this module.
//
// Links: rt_http2.h, rt_http2_internal.h, rt_http2.c (frame layer)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements HPACK header compression for the HTTP/2 runtime.
 * @details Provides RFC static and bounded dynamic tables, prefix integer and
 * string coding, one-time Huffman tree construction and validation, and
 * ownership-safe header block encoding and decoding with protocol checks.
 */

#include "rt_http2.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/** Restrict the Windows SDK surface to core declarations. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/** Windows compatibility alias for case-insensitive string comparison. */
#define strcasecmp _stricmp
/** Windows compatibility alias for bounded case-insensitive comparison. */
#define strncasecmp _strnicmp
#else
#include <pthread.h>
#include <strings.h>
#endif
#include "rt_http2_internal.h"

/** RFC 7541 Appendix A static header table in one-based wire-index order. */
static const struct {
    const char *name;
    const char *value;
} kHpackStaticTable[61] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};

/** RFC 7541 Appendix B canonical Huffman code and width for each byte plus EOS. */
static const struct {
    uint32_t nbits;
    uint32_t code;
} kHpackHuffSym[257] = {
    {13u, 0xffc00000u}, {23u, 0xffffb000u}, {28u, 0xfffffe20u}, {28u, 0xfffffe30u},
    {28u, 0xfffffe40u}, {28u, 0xfffffe50u}, {28u, 0xfffffe60u}, {28u, 0xfffffe70u},
    {28u, 0xfffffe80u}, {24u, 0xffffea00u}, {30u, 0xfffffff0u}, {28u, 0xfffffe90u},
    {28u, 0xfffffea0u}, {30u, 0xfffffff4u}, {28u, 0xfffffeb0u}, {28u, 0xfffffec0u},
    {28u, 0xfffffed0u}, {28u, 0xfffffee0u}, {28u, 0xfffffef0u}, {28u, 0xffffff00u},
    {28u, 0xffffff10u}, {28u, 0xffffff20u}, {30u, 0xfffffff8u}, {28u, 0xffffff30u},
    {28u, 0xffffff40u}, {28u, 0xffffff50u}, {28u, 0xffffff60u}, {28u, 0xffffff70u},
    {28u, 0xffffff80u}, {28u, 0xffffff90u}, {28u, 0xffffffa0u}, {28u, 0xffffffb0u},
    {6u, 0x50000000u},  {10u, 0xfe000000u}, {10u, 0xfe400000u}, {12u, 0xffa00000u},
    {13u, 0xffc80000u}, {6u, 0x54000000u},  {8u, 0xf8000000u},  {11u, 0xff400000u},
    {10u, 0xfe800000u}, {10u, 0xfec00000u}, {8u, 0xf9000000u},  {11u, 0xff600000u},
    {8u, 0xfa000000u},  {6u, 0x58000000u},  {6u, 0x5c000000u},  {6u, 0x60000000u},
    {5u, 0x00000000u},  {5u, 0x08000000u},  {5u, 0x10000000u},  {6u, 0x64000000u},
    {6u, 0x68000000u},  {6u, 0x6c000000u},  {6u, 0x70000000u},  {6u, 0x74000000u},
    {6u, 0x78000000u},  {6u, 0x7c000000u},  {7u, 0xb8000000u},  {8u, 0xfb000000u},
    {15u, 0xfff80000u}, {6u, 0x80000000u},  {12u, 0xffb00000u}, {10u, 0xff000000u},
    {13u, 0xffd00000u}, {6u, 0x84000000u},  {7u, 0xba000000u},  {7u, 0xbc000000u},
    {7u, 0xbe000000u},  {7u, 0xc0000000u},  {7u, 0xc2000000u},  {7u, 0xc4000000u},
    {7u, 0xc6000000u},  {7u, 0xc8000000u},  {7u, 0xca000000u},  {7u, 0xcc000000u},
    {7u, 0xce000000u},  {7u, 0xd0000000u},  {7u, 0xd2000000u},  {7u, 0xd4000000u},
    {7u, 0xd6000000u},  {7u, 0xd8000000u},  {7u, 0xda000000u},  {7u, 0xdc000000u},
    {7u, 0xde000000u},  {7u, 0xe0000000u},  {7u, 0xe2000000u},  {7u, 0xe4000000u},
    {8u, 0xfc000000u},  {7u, 0xe6000000u},  {8u, 0xfd000000u},  {13u, 0xffd80000u},
    {19u, 0xfffe0000u}, {13u, 0xffe00000u}, {14u, 0xfff00000u}, {6u, 0x88000000u},
    {15u, 0xfffa0000u}, {5u, 0x18000000u},  {6u, 0x8c000000u},  {5u, 0x20000000u},
    {6u, 0x90000000u},  {5u, 0x28000000u},  {6u, 0x94000000u},  {6u, 0x98000000u},
    {6u, 0x9c000000u},  {5u, 0x30000000u},  {7u, 0xe8000000u},  {7u, 0xea000000u},
    {6u, 0xa0000000u},  {6u, 0xa4000000u},  {6u, 0xa8000000u},  {5u, 0x38000000u},
    {6u, 0xac000000u},  {7u, 0xec000000u},  {6u, 0xb0000000u},  {5u, 0x40000000u},
    {5u, 0x48000000u},  {6u, 0xb4000000u},  {7u, 0xee000000u},  {7u, 0xf0000000u},
    {7u, 0xf2000000u},  {7u, 0xf4000000u},  {7u, 0xf6000000u},  {15u, 0xfffc0000u},
    {11u, 0xff800000u}, {14u, 0xfff40000u}, {13u, 0xffe80000u}, {28u, 0xffffffc0u},
    {20u, 0xfffe6000u}, {22u, 0xffff4800u}, {20u, 0xfffe7000u}, {20u, 0xfffe8000u},
    {22u, 0xffff4c00u}, {22u, 0xffff5000u}, {22u, 0xffff5400u}, {23u, 0xffffb200u},
    {22u, 0xffff5800u}, {23u, 0xffffb400u}, {23u, 0xffffb600u}, {23u, 0xffffb800u},
    {23u, 0xffffba00u}, {23u, 0xffffbc00u}, {24u, 0xffffeb00u}, {23u, 0xffffbe00u},
    {24u, 0xffffec00u}, {24u, 0xffffed00u}, {22u, 0xffff5c00u}, {23u, 0xffffc000u},
    {24u, 0xffffee00u}, {23u, 0xffffc200u}, {23u, 0xffffc400u}, {23u, 0xffffc600u},
    {23u, 0xffffc800u}, {21u, 0xfffee000u}, {22u, 0xffff6000u}, {23u, 0xffffca00u},
    {22u, 0xffff6400u}, {23u, 0xffffcc00u}, {23u, 0xffffce00u}, {24u, 0xffffef00u},
    {22u, 0xffff6800u}, {21u, 0xfffee800u}, {20u, 0xfffe9000u}, {22u, 0xffff6c00u},
    {22u, 0xffff7000u}, {23u, 0xffffd000u}, {23u, 0xffffd200u}, {21u, 0xfffef000u},
    {23u, 0xffffd400u}, {22u, 0xffff7400u}, {22u, 0xffff7800u}, {24u, 0xfffff000u},
    {21u, 0xfffef800u}, {22u, 0xffff7c00u}, {23u, 0xffffd600u}, {23u, 0xffffd800u},
    {21u, 0xffff0000u}, {21u, 0xffff0800u}, {22u, 0xffff8000u}, {21u, 0xffff1000u},
    {23u, 0xffffda00u}, {22u, 0xffff8400u}, {23u, 0xffffdc00u}, {23u, 0xffffde00u},
    {20u, 0xfffea000u}, {22u, 0xffff8800u}, {22u, 0xffff8c00u}, {22u, 0xffff9000u},
    {23u, 0xffffe000u}, {22u, 0xffff9400u}, {22u, 0xffff9800u}, {23u, 0xffffe200u},
    {26u, 0xfffff800u}, {26u, 0xfffff840u}, {20u, 0xfffeb000u}, {19u, 0xfffe2000u},
    {22u, 0xffff9c00u}, {23u, 0xffffe400u}, {22u, 0xffffa000u}, {25u, 0xfffff600u},
    {26u, 0xfffff880u}, {26u, 0xfffff8c0u}, {26u, 0xfffff900u}, {27u, 0xfffffbc0u},
    {27u, 0xfffffbe0u}, {26u, 0xfffff940u}, {24u, 0xfffff100u}, {25u, 0xfffff680u},
    {19u, 0xfffe4000u}, {21u, 0xffff1800u}, {26u, 0xfffff980u}, {27u, 0xfffffc00u},
    {27u, 0xfffffc20u}, {26u, 0xfffff9c0u}, {27u, 0xfffffc40u}, {24u, 0xfffff200u},
    {21u, 0xffff2000u}, {21u, 0xffff2800u}, {26u, 0xfffffa00u}, {26u, 0xfffffa40u},
    {28u, 0xffffffd0u}, {27u, 0xfffffc60u}, {27u, 0xfffffc80u}, {27u, 0xfffffca0u},
    {20u, 0xfffec000u}, {24u, 0xfffff300u}, {20u, 0xfffed000u}, {21u, 0xffff3000u},
    {22u, 0xffffa400u}, {21u, 0xffff3800u}, {21u, 0xffff4000u}, {23u, 0xffffe600u},
    {22u, 0xffffa800u}, {22u, 0xffffac00u}, {25u, 0xfffff700u}, {25u, 0xfffff780u},
    {24u, 0xfffff400u}, {24u, 0xfffff500u}, {26u, 0xfffffa80u}, {23u, 0xffffe800u},
    {26u, 0xfffffac0u}, {27u, 0xfffffcc0u}, {26u, 0xfffffb00u}, {26u, 0xfffffb40u},
    {27u, 0xfffffce0u}, {27u, 0xfffffd00u}, {27u, 0xfffffd20u}, {27u, 0xfffffd40u},
    {27u, 0xfffffd60u}, {28u, 0xffffffe0u}, {27u, 0xfffffd80u}, {27u, 0xfffffda0u},
    {27u, 0xfffffdc0u}, {27u, 0xfffffde0u}, {27u, 0xfffffe00u}, {26u, 0xfffffb80u},
    {30u, 0xfffffffcu},
};

/** One node in the process-global HPACK Huffman decoding trie. */
typedef struct {
    int child[2]; ///< Child node indices selected by zero or one.
    int symbol;   ///< Decoded byte/EOS symbol, or a negative interior marker.
    int pad_ok;   ///< Whether termination at this node is valid one-bit padding.
} huff_node_t;

/** Fixed storage for the once-built HPACK Huffman decoding trie. */
static huff_node_t g_huff_nodes[H2_MAX_HUFF_NODES];
/** Number of initialized nodes in @ref g_huff_nodes. */
static int g_huff_node_count = 0;
/** Sticky result of process-global Huffman table construction. */
static int g_huff_init_ok = 0;
#ifdef _WIN32
static INIT_ONCE g_huff_once = INIT_ONCE_STATIC_INIT;
#else
static pthread_once_t g_huff_once = PTHREAD_ONCE_INIT;
#endif

/// @brief Release every entry and allocation owned by an HPACK dynamic table.
/// @details Accepts a null pointer as a no-op and resets a released table to
///          its zero state so it can be initialized or freed again safely.
/// @param table Dynamic table to release.
void hpack_dyn_table_free(hpack_dyn_table_t *table) {
    if (!table)
        return;
    for (size_t i = 0; i < table->count; i++) {
        free(table->entries[i].name);
        free(table->entries[i].value);
    }
    free(table->entries);
    memset(table, 0, sizeof(*table));
}

/// @brief Evict oldest dynamic-table entries until the configured byte limit is met.
/// @details HPACK orders the newest entry at index zero, so eviction proceeds
///          from the array's final occupied slot.
/// @param table Mutable dynamic table with a valid entry array.
static void hpack_dyn_table_evict_to_fit(hpack_dyn_table_t *table) {
    while (table->bytes > table->max_bytes && table->count > 0) {
        hpack_dyn_entry_t *entry = &table->entries[table->count - 1];
        table->bytes -= entry->size;
        free(entry->name);
        free(entry->value);
        table->count--;
    }
}

/// @brief Apply a peer-advertised HPACK dynamic-table size and evict excess entries.
/// @param table Dynamic table to update.
/// @param max_bytes New capacity in HPACK bytes, including the 32-byte overhead per entry.
/// @return 1 when the limit is accepted; 0 for a null table or a limit above the implementation
///         maximum.
int hpack_dyn_table_set_max_size(hpack_dyn_table_t *table, size_t max_bytes) {
    if (!table || max_bytes > H2_MAX_DYNAMIC_TABLE_SIZE)
        return 0;
    table->max_bytes = max_bytes;
    hpack_dyn_table_evict_to_fit(table);
    return 1;
}

/// @brief Add a copied header field to the front of the HPACK dynamic table.
/// @details Evicts oldest entries as necessary. An entry larger than the
///          current capacity clears the table and is treated as successfully
///          processed without being inserted, as required by HPACK.
/// @param table Dynamic table that will own the new copies.
/// @param name Null-terminated header name.
/// @param value Null-terminated header value.
/// @return 1 when the HPACK insertion semantics are applied; 0 on invalid input,
///         size overflow, or allocation failure.
static int hpack_dyn_table_add(hpack_dyn_table_t *table, const char *name, const char *value) {
    char *name_copy = NULL;
    char *value_copy = NULL;
    size_t name_len = 0;
    size_t value_len = 0;
    size_t entry_size = 0;
    if (!table || !name || !value)
        return 0;
    name_len = strlen(name);
    value_len = strlen(value);
    if (name_len > SIZE_MAX - value_len - 32)
        return 0;
    entry_size = name_len + value_len + 32;
    if (entry_size > table->max_bytes) {
        hpack_dyn_table_set_max_size(table, table->max_bytes);
        for (size_t i = 0; i < table->count; i++) {
            free(table->entries[i].name);
            free(table->entries[i].value);
        }
        table->count = 0;
        table->bytes = 0;
        return 1;
    }
    while (table->bytes > table->max_bytes - entry_size && table->count > 0) {
        hpack_dyn_entry_t *entry = &table->entries[table->count - 1];
        table->bytes -= entry->size;
        free(entry->name);
        free(entry->value);
        table->count--;
    }
    if (table->count == table->cap) {
        size_t new_cap = table->cap ? table->cap * 2 : 8;
        if ((table->cap && table->cap > SIZE_MAX / 2) ||
            new_cap > SIZE_MAX / sizeof(*table->entries))
            return 0;
        hpack_dyn_entry_t *grown =
            (hpack_dyn_entry_t *)realloc(table->entries, new_cap * sizeof(*grown));
        if (!grown)
            return 0;
        table->entries = grown;
        table->cap = new_cap;
    }
    name_copy = strdup(name);
    value_copy = strdup(value);
    if (!name_copy || !value_copy) {
        free(name_copy);
        free(value_copy);
        return 0;
    }
    memmove(table->entries + 1, table->entries, table->count * sizeof(*table->entries));
    table->entries[0].name = name_copy;
    table->entries[0].value = value_copy;
    table->entries[0].size = entry_size;
    table->count++;
    table->bytes += entry_size;
    return 1;
}

/// @brief Find an exact name-and-value match in the RFC 7541 static table.
/// @param name Null-terminated header name.
/// @param value Null-terminated header value.
/// @return One-based HPACK index for the match, or zero when none exists.
static int hpack_lookup_exact_static(const char *name, const char *value) {
    for (int i = 0; i < 61; i++) {
        if (strcmp(kHpackStaticTable[i].name, name) == 0 &&
            strcmp(kHpackStaticTable[i].value, value) == 0) {
            return i + 1;
        }
    }
    return 0;
}

/// @brief Find the first matching header name in the RFC 7541 static table.
/// @param name Null-terminated header name.
/// @return One-based HPACK index for the matching name, or zero when absent.
static int hpack_lookup_name_static(const char *name) {
    for (int i = 0; i < 61; i++) {
        if (strcmp(kHpackStaticTable[i].name, name) == 0)
            return i + 1;
    }
    return 0;
}

/// @brief Find an exact name-and-value match in an HPACK dynamic table.
/// @param table Dynamic table to search; may be null.
/// @param name Null-terminated header name.
/// @param value Null-terminated header value.
/// @return Combined HPACK index beginning at 62 for a match, or zero when absent.
static int hpack_lookup_exact_dynamic(const hpack_dyn_table_t *table,
                                      const char *name,
                                      const char *value) {
    if (!table)
        return 0;
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].name, name) == 0 &&
            strcmp(table->entries[i].value, value) == 0) {
            return 62 + (int)i;
        }
    }
    return 0;
}

/// @brief Find the newest matching header name in an HPACK dynamic table.
/// @param table Dynamic table to search; may be null.
/// @param name Null-terminated header name.
/// @return Combined HPACK index beginning at 62 for a match, or zero when absent.
static int hpack_lookup_name_dynamic(const hpack_dyn_table_t *table, const char *name) {
    if (!table)
        return 0;
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].name, name) == 0)
            return 62 + (int)i;
    }
    return 0;
}

/// @brief Resolve a combined HPACK index against the static and dynamic tables.
/// @param table Dynamic table used for indices 62 and above; may be null for static lookups.
/// @param index One-based combined HPACK index.
/// @param name_out Optional receiver for a borrowed header-name pointer.
/// @param value_out Optional receiver for a borrowed header-value pointer.
/// @return 1 when the index resolves; 0 for index zero or an unavailable dynamic entry.
static int hpack_get_by_index(const hpack_dyn_table_t *table,
                              size_t index,
                              const char **name_out,
                              const char **value_out) {
    if (index == 0)
        return 0;
    if (index <= 61) {
        if (name_out)
            *name_out = kHpackStaticTable[index - 1].name;
        if (value_out)
            *value_out = kHpackStaticTable[index - 1].value;
        return 1;
    }
    index -= 62;
    if (!table || index >= table->count)
        return 0;
    if (name_out)
        *name_out = table->entries[index].name;
    if (value_out)
        *value_out = table->entries[index].value;
    return 1;
}

/// @brief Decode an HPACK variable-length integer from a prefixed byte sequence.
/// @details Rejects invalid prefix widths, truncated continuation sequences,
///          and any shift or addition that would overflow `size_t`.
/// @param src Encoded bytes beginning with the prefix-bearing byte.
/// @param src_len Number of readable bytes at @p src.
/// @param prefix_bits Number of low bits in the first byte assigned to the integer.
/// @param value_out Optional receiver for the decoded integer.
/// @param consumed_out Optional receiver for the number of encoded bytes consumed.
/// @return 1 on successful decoding; 0 on invalid input, truncation, or overflow.
static int hpack_int_decode(const uint8_t *src,
                            size_t src_len,
                            uint8_t prefix_bits,
                            size_t *value_out,
                            size_t *consumed_out) {
    size_t value = 0;
    size_t consumed = 0;
    size_t shift = 0;
    uint8_t mask = 0;
    if (!src || src_len == 0 || prefix_bits == 0 || prefix_bits > 8)
        return 0;
    mask = (uint8_t)((1u << prefix_bits) - 1u);
    value = (size_t)(src[0] & mask);
    consumed = 1;
    if (value < mask) {
        if (value_out)
            *value_out = value;
        if (consumed_out)
            *consumed_out = consumed;
        return 1;
    }
    while (consumed < src_len) {
        uint8_t b = src[consumed++];
        size_t add = 0;
        if (shift > sizeof(size_t) * 8 - 7)
            return 0;
        add = (size_t)(b & 0x7f) << shift;
        if (value > SIZE_MAX - add)
            return 0;
        value += add;
        if ((b & 0x80) == 0) {
            if (value_out)
                *value_out = value;
            if (consumed_out)
                *consumed_out = consumed;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

/// @brief Append an HPACK variable-length integer with caller-supplied high prefix bits.
/// @param dst Destination byte buffer.
/// @param prefix_bits Number of low bits available in the first byte.
/// @param prefix_high High-bit representation marker to combine with the integer prefix.
/// @param value Unsigned integer to encode.
/// @return 1 on success; 0 for invalid arguments or buffer growth failure.
static int hpack_int_encode(h2_buf_t *dst, uint8_t prefix_bits, uint8_t prefix_high, size_t value) {
    uint8_t first = prefix_high;
    if (!dst || prefix_bits == 0 || prefix_bits > 8)
        return 0;
    size_t max_prefix = ((size_t)1u << prefix_bits) - 1u;
    if (value < max_prefix) {
        first = (uint8_t)(prefix_high | (uint8_t)value);
        return h2_buf_append_byte(dst, first);
    }
    first = (uint8_t)(prefix_high | (uint8_t)max_prefix);
    if (!h2_buf_append_byte(dst, first))
        return 0;
    value -= max_prefix;
    while (value >= 128) {
        uint8_t b = (uint8_t)((value & 0x7f) | 0x80);
        if (!h2_buf_append_byte(dst, b))
            return 0;
        value >>= 7;
    }
    return h2_buf_append_byte(dst, (uint8_t)value);
}

/// @brief Allocate and initialize one node from the fixed Huffman decode-tree arena.
/// @return The new node index, or -1 when the arena is full.
static int huff_new_node(void) {
    int index = g_huff_node_count;
    if (index >= H2_MAX_HUFF_NODES)
        return -1;
    g_huff_nodes[index].child[0] = -1;
    g_huff_nodes[index].child[1] = -1;
    g_huff_nodes[index].symbol = -1;
    g_huff_nodes[index].pad_ok = 0;
    g_huff_node_count++;
    return index;
}

/// @brief Build the RFC 7541 Huffman decode trie and mark legal EOS-prefix padding nodes.
/// @return 1 when all symbols and padding paths fit in the fixed node arena; 0 otherwise.
static int hpack_huffman_build(void) {
    g_huff_node_count = 0;
    if (huff_new_node() != 0)
        return 0;
    for (int sym = 0; sym < 257; sym++) {
        int node = 0;
        uint32_t code = kHpackHuffSym[sym].code;
        uint32_t nbits = kHpackHuffSym[sym].nbits;
        for (uint32_t i = 0; i < nbits; i++) {
            int bit = (int)((code >> (31u - i)) & 1u);
            if (g_huff_nodes[node].child[bit] < 0) {
                int next = huff_new_node();
                if (next < 0)
                    return 0;
                g_huff_nodes[node].child[bit] = next;
            }
            node = g_huff_nodes[node].child[bit];
        }
        g_huff_nodes[node].symbol = sym;
    }
    {
        int node = 0;
        g_huff_nodes[node].pad_ok = 1;
        for (int i = 0; i < 7; i++) {
            node = g_huff_nodes[node].child[1];
            if (node < 0)
                return 0;
            g_huff_nodes[node].pad_ok = 1;
        }
    }
    return 1;
}

#ifdef _WIN32
/// @brief Initialize the global HPACK Huffman trie through the Windows one-time callback API.
/// @param init_once Windows initialization token; unused by the callback.
/// @param parameter Caller parameter; unused.
/// @param context Optional context receiver; unused.
/// @return `TRUE` after recording the trie-build result in the global initialization state.
static BOOL CALLBACK hpack_huffman_once(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
    (void)init_once;
    (void)parameter;
    (void)context;
    g_huff_init_ok = hpack_huffman_build();
    return TRUE;
}
#else
/// @brief Initialize the global HPACK Huffman trie through `pthread_once`.
static void hpack_huffman_once(void) {
    g_huff_init_ok = hpack_huffman_build();
}
#endif

/// @brief Ensure the process-wide HPACK Huffman decode trie has been initialized.
/// @return 1 when one-time initialization and trie construction succeeded; 0 otherwise.
static int hpack_huffman_init(void) {
#ifdef _WIN32
    if (!InitOnceExecuteOnce(&g_huff_once, hpack_huffman_once, NULL, NULL))
        return 0;
#else
    if (pthread_once(&g_huff_once, hpack_huffman_once) != 0)
        return 0;
#endif
    return g_huff_init_ok;
}

/// @brief Decode an RFC 7541 Huffman string into a newly allocated C string.
/// @details Rejects the reserved EOS symbol, invalid tree paths, and terminal
///          padding that is not a permitted prefix of EOS.
/// @param src Huffman-coded bytes.
/// @param src_len Number of encoded bytes.
/// @param decoded_len_out Optional receiver for the decoded byte count, excluding the terminator.
/// @return Newly allocated null-terminated decoded bytes, or null on malformed input,
///         initialization failure, or allocation failure.
static char *hpack_decode_huffman(const uint8_t *src, size_t src_len, size_t *decoded_len_out) {
    h2_buf_t out = {0};
    int node = 0;
    size_t decoded_len = 0;
    if (decoded_len_out)
        *decoded_len_out = 0;
    if (!hpack_huffman_init())
        return NULL;
    for (size_t i = 0; i < src_len; i++) {
        uint8_t byte = src[i];
        for (int bit = 7; bit >= 0; bit--) {
            int one = (byte >> bit) & 1;
            node = g_huff_nodes[node].child[one];
            if (node < 0) {
                h2_buf_free(&out);
                return NULL;
            }
            if (g_huff_nodes[node].symbol >= 0) {
                int sym = g_huff_nodes[node].symbol;
                if (sym == 256) {
                    h2_buf_free(&out);
                    return NULL;
                }
                if (!h2_buf_append_byte(&out, (uint8_t)sym)) {
                    h2_buf_free(&out);
                    return NULL;
                }
                node = 0;
            }
        }
    }
    if (!g_huff_nodes[node].pad_ok) {
        h2_buf_free(&out);
        return NULL;
    }
    decoded_len = out.len;
    if (!h2_buf_append_byte(&out, '\0')) {
        h2_buf_free(&out);
        return NULL;
    }
    if (decoded_len_out)
        *decoded_len_out = decoded_len;
    return (char *)out.data;
}

/// @brief Decode an HPACK string literal in Huffman or raw form.
/// @param src Encoded string beginning with its Huffman flag and length prefix.
/// @param src_len Number of readable encoded bytes.
/// @param out Receives a newly allocated null-terminated string on success.
/// @param value_len_out Optional receiver for the decoded byte length.
/// @param consumed_out Optional receiver for the total encoded byte count consumed.
/// @return 1 on success; 0 on malformed, truncated, or unallocatable input.
static int hpack_decode_string(
    const uint8_t *src, size_t src_len, char **out, size_t *value_len_out, size_t *consumed_out) {
    size_t str_len = 0;
    size_t int_consumed = 0;
    int huffman = 0;
    char *value = NULL;
    if (!src || src_len == 0 || !out)
        return 0;
    huffman = (src[0] & 0x80) != 0;
    if (!hpack_int_decode(src, src_len, 7, &str_len, &int_consumed))
        return 0;
    if (int_consumed > src_len || str_len > src_len - int_consumed)
        return 0;
    if (value_len_out)
        *value_len_out = 0;
    if (huffman) {
        value = hpack_decode_huffman(src + int_consumed, str_len, value_len_out);
    } else {
        value = h2_strdup_range(src + int_consumed, str_len);
        if (value_len_out)
            *value_len_out = str_len;
    }
    if (!value)
        return 0;
    *out = value;
    if (consumed_out)
        *consumed_out = int_consumed + str_len;
    return 1;
}

/// @brief Append a null-terminated string as an uncompressed HPACK string literal.
/// @param dst Destination header-block buffer.
/// @param text Text to encode.
/// @return 1 on success; 0 for a null string or buffer growth failure.
static int hpack_encode_string(h2_buf_t *dst, const char *text) {
    size_t len = text ? strlen(text) : 0;
    if (!text)
        return 0;
    if (!hpack_int_encode(dst, 7, 0x00, len))
        return 0;
    return h2_buf_append(dst, text, len);
}

/// @brief Append one validated header field to an HPACK header block.
/// @details Lowercases non-pseudo-header names, prefers an exact indexed
///          representation, and otherwise emits a literal without incremental
///          indexing using an indexed name when available.
/// @param dst Destination HPACK block buffer.
/// @param table Dynamic table consulted for exact and name-only matches; may be null.
/// @param name Null-terminated header name.
/// @param value Null-terminated header value.
/// @return 1 on success; 0 for invalid input, allocation failure, or buffer growth failure.
int hpack_encode_header_field(h2_buf_t *dst,
                                     const hpack_dyn_table_t *table,
                                     const char *name,
                                     const char *value) {
    int exact_index = 0;
    int name_index = 0;
    char *lower_name = NULL;
    const char *emit_name = name;

    if (!dst || !name || !value)
        return 0;
    if (name[0] != ':') {
        lower_name = h2_strdup_lower(name);
        if (!lower_name)
            return 0;
        emit_name = lower_name;
    }
    if (!h2_header_name_is_valid(emit_name) || !h2_header_value_is_valid(value)) {
        free(lower_name);
        return 0;
    }
    exact_index = hpack_lookup_exact_dynamic(table, emit_name, value);
    if (!exact_index)
        exact_index = hpack_lookup_exact_static(emit_name, value);
    if (exact_index) {
        free(lower_name);
        return hpack_int_encode(dst, 7, 0x80, (size_t)exact_index);
    }
    name_index = hpack_lookup_name_dynamic(table, emit_name);
    if (!name_index)
        name_index = hpack_lookup_name_static(emit_name);
    if (!hpack_int_encode(dst, 4, 0x00, (size_t)name_index)) {
        free(lower_name);
        return 0;
    }
    if (!name_index && !hpack_encode_string(dst, emit_name)) {
        free(lower_name);
        return 0;
    }
    free(lower_name);
    return hpack_encode_string(dst, value);
}

/// @brief Decode a complete HPACK block into a newly allocated HTTP/2 header list.
/// @details Supports indexed fields, dynamic-table size updates, and literal
///          fields with or without incremental indexing. Size updates are
///          accepted only before the first header field. All decoded names and
///          values are validated before being appended.
/// @param table Connection-scoped dynamic table to query and update.
/// @param block Encoded header-block bytes.
/// @param block_len Number of bytes in @p block.
/// @param headers_out Receives the owned decoded header list, or null for an empty block.
/// @return 1 when the entire block decodes successfully; 0 on malformed input,
///         invalid headers, invalid table updates, or allocation failure.
int hpack_decode_header_block(hpack_dyn_table_t *table,
                                     const uint8_t *block,
                                     size_t block_len,
                                     rt_http2_header_t **headers_out) {
    size_t pos = 0;
    rt_http2_header_t *headers = NULL;
    int saw_header = 0;
    if (!table || !headers_out)
        return 0;
    *headers_out = NULL;
    while (pos < block_len) {
        const char *name_ref = NULL;
        const char *value_ref = NULL;
        char *name = NULL;
        char *value = NULL;
        size_t name_len = 0;
        size_t value_len = 0;
        size_t index = 0;
        size_t consumed = 0;
        int add_index = 0;
        uint8_t b = block[pos];

        if ((b & 0x80) != 0) {
            if (!hpack_int_decode(block + pos, block_len - pos, 7, &index, &consumed) ||
                !hpack_get_by_index(table, index, &name_ref, &value_ref)) {
                rt_http2_headers_free(headers);
                return 0;
            }
            name_len = strlen(name_ref);
            value_len = strlen(value_ref);
            if (!h2_header_name_bytes_is_valid(name_ref, name_len) ||
                !h2_header_value_bytes_is_valid(value_ref, value_len) ||
                !rt_http2_header_append_copy(&headers, name_ref, value_ref)) {
                rt_http2_headers_free(headers);
                return 0;
            }
            pos += consumed;
            saw_header = 1;
            continue;
        }

        if ((b & 0x20) != 0) {
            if (saw_header) {
                rt_http2_headers_free(headers);
                return 0;
            }
            if (!hpack_int_decode(block + pos, block_len - pos, 5, &index, &consumed) ||
                !hpack_dyn_table_set_max_size(table, index)) {
                rt_http2_headers_free(headers);
                return 0;
            }
            pos += consumed;
            continue;
        }

        if ((b & 0x40) != 0) {
            add_index = 1;
            if (!hpack_int_decode(block + pos, block_len - pos, 6, &index, &consumed)) {
                rt_http2_headers_free(headers);
                return 0;
            }
        } else {
            add_index = 0;
            if (!hpack_int_decode(block + pos, block_len - pos, 4, &index, &consumed)) {
                rt_http2_headers_free(headers);
                return 0;
            }
        }
        pos += consumed;

        if (index > 0) {
            if (!hpack_get_by_index(table, index, &name_ref, NULL)) {
                rt_http2_headers_free(headers);
                return 0;
            }
            name_len = strlen(name_ref);
            name = strdup(name_ref);
            if (!name) {
                rt_http2_headers_free(headers);
                return 0;
            }
        } else {
            size_t name_consumed = 0;
            if (!hpack_decode_string(
                    block + pos, block_len - pos, &name, &name_len, &name_consumed)) {
                rt_http2_headers_free(headers);
                return 0;
            }
            pos += name_consumed;
        }

        {
            size_t value_consumed = 0;
            if (!hpack_decode_string(
                    block + pos, block_len - pos, &value, &value_len, &value_consumed)) {
                free(name);
                rt_http2_headers_free(headers);
                return 0;
            }
            pos += value_consumed;
        }

        if (!h2_header_name_bytes_is_valid(name, name_len) ||
            !h2_header_value_bytes_is_valid(value, value_len) ||
            !rt_http2_header_append_copy(&headers, name, value)) {
            free(name);
            free(value);
            rt_http2_headers_free(headers);
            return 0;
        }

        if (add_index && !hpack_dyn_table_add(table, name, value)) {
            free(name);
            free(value);
            rt_http2_headers_free(headers);
            return 0;
        }
        free(name);
        free(value);
        saw_header = 1;
    }
    *headers_out = headers;
    return 1;
}
