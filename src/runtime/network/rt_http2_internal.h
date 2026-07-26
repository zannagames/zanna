//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_http2_internal.h
// Purpose: Shared HTTP/2 buffer + HPACK dynamic-table types and the helper/
//   HPACK API boundary between the frame layer (rt_http2.c) and HPACK
//   header (de)compression (rt_hpack.c).
//
// Key invariants:
//   - General-purpose temporary buffers never grow beyond the frame layer's
//     configured implementation cap.
//   - Dynamic-table entries are stored newest first and account for RFC 7541's
//     32-byte per-entry overhead.
//
// Ownership/Lifetime:
//   - h2_buf_t and hpack_dyn_table_t own their backing allocations.
//   - HPACK decode results are returned as owned rt_http2_header_t lists.
//
// Links: src/runtime/network/rt_http2.h,
//        src/runtime/network/rt_http2.c,
//        src/runtime/network/rt_hpack.c
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include <stddef.h>

#define H2_MAX_DYNAMIC_TABLE_SIZE (64u * 1024u)
#define H2_MAX_HUFF_NODES 8192

/// @brief Growable owned byte buffer shared by framing and HPACK code.
typedef struct {
    /// @brief Owned backing storage, or null while empty.
    uint8_t *data;
    /// @brief Number of initialized bytes.
    size_t len;
    /// @brief Allocated capacity in bytes.
    size_t cap;
} h2_buf_t;

/// @brief One owned entry in an HPACK dynamic table.
typedef struct {
    /// @brief Owned null-terminated header name.
    char *name;
    /// @brief Owned null-terminated header value.
    char *value;
    /// @brief HPACK size: name bytes + value bytes + 32.
    size_t size;
} hpack_dyn_entry_t;

/// @brief Newest-first HPACK dynamic table and its capacity accounting.
typedef struct {
    /// @brief Owned entry array.
    hpack_dyn_entry_t *entries;
    /// @brief Number of occupied entries.
    size_t count;
    /// @brief Allocated entry slots.
    size_t cap;
    /// @brief Current total HPACK entry size.
    size_t bytes;
    /// @brief Configured maximum total entry size.
    size_t max_bytes;
} hpack_dyn_table_t;

/// @brief Append bytes to a growable HTTP/2 buffer.
/// @param buf Destination buffer.
/// @param src Source bytes; may be null only when @p len is zero.
/// @param len Number of bytes to append.
/// @return 1 on success; 0 on invalid input, overflow, size-limit violation, or allocation failure.
int h2_buf_append(h2_buf_t *buf, const void *src, size_t len);

/// @brief Append one byte to a growable HTTP/2 buffer.
/// @param buf Destination buffer.
/// @param b Byte to append.
/// @return 1 on success; 0 on size-limit violation or allocation failure.
int h2_buf_append_byte(h2_buf_t *buf, uint8_t b);

/// @brief Release a growable buffer and reset it to zero.
/// @param buf Buffer to release; may be null.
void h2_buf_free(h2_buf_t *buf);

/// @brief Validate a length-delimited HTTP/2 header name.
/// @param name Header-name bytes.
/// @param len Number of bytes to validate.
/// @return Nonzero for a nonempty lowercase name without control or space bytes.
int h2_header_name_bytes_is_valid(const char *name, size_t len);

/// @brief Validate a null-terminated HTTP/2 header name.
/// @param name Header name to validate.
/// @return Nonzero when the complete name satisfies HTTP/2 name rules.
int h2_header_name_is_valid(const char *name);

/// @brief Validate a length-delimited HTTP/2 header value.
/// @param value Header-value bytes.
/// @param len Number of bytes to validate.
/// @return Nonzero when the range contains no null, carriage-return, or line-feed bytes.
int h2_header_value_bytes_is_valid(const char *value, size_t len);

/// @brief Validate a null-terminated HTTP/2 header value.
/// @param value Header value to validate.
/// @return Nonzero when the complete value satisfies the transport's value rules.
int h2_header_value_is_valid(const char *value);

/// @brief Allocate a lowercase ASCII copy of a null-terminated string.
/// @param src Source string; a null pointer is treated as an empty string.
/// @return Newly allocated lowercase copy, or null on allocation failure.
char *h2_strdup_lower(const char *src);

/// @brief Allocate a null-terminated copy of a byte range.
/// @param src Source bytes; must be readable when @p len is nonzero.
/// @param len Number of bytes to copy.
/// @return Newly allocated string, or null on length overflow or allocation failure.
char *h2_strdup_range(const uint8_t *src, size_t len);

/// @brief Decode a complete HPACK block and update the decode dynamic table.
/// @param table Connection-scoped dynamic table.
/// @param block Encoded HPACK bytes.
/// @param block_len Number of bytes in @p block.
/// @param headers_out Receives an owned decoded header list.
/// @return 1 on success; 0 on malformed input, invalid table updates, or allocation failure.
int hpack_decode_header_block(hpack_dyn_table_t *table, const uint8_t *block, size_t block_len, rt_http2_header_t **headers_out);

/// @brief Release all allocations in an HPACK dynamic table and reset it.
/// @param table Table to release; may be null.
void hpack_dyn_table_free(hpack_dyn_table_t *table);

/// @brief Set an HPACK dynamic table's maximum size and evict excess entries.
/// @param table Dynamic table to update.
/// @param max_bytes New maximum encoded-table size.
/// @return 1 when accepted; 0 for invalid input or a value above the implementation maximum.
int hpack_dyn_table_set_max_size(hpack_dyn_table_t *table, size_t max_bytes);

/// @brief Encode one validated header field into an HPACK block.
/// @param dst Destination block buffer.
/// @param table Dynamic table consulted for indexed representations.
/// @param name Header name.
/// @param value Header value.
/// @return 1 on success; 0 on invalid input or allocation failure.
int hpack_encode_header_field(h2_buf_t *dst, const hpack_dyn_table_t *table, const char *name, const char *value);
