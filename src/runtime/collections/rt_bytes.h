//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_bytes.h
// Purpose: Efficient byte array type for binary data manipulation, providing creation, element
// access, search, encoding/decoding, and conversion to/from strings and hex.
//
// Key invariants:
//   - Byte values are stored as uint8_t; writes truncate to the low 8 bits
//     of the supplied integer (no clamping).
//   - Bytes are stored contiguously in memory.
//   - String conversion produces a copy of the UTF-8 bytes; the original is not modified.
//   - Hex encoding produces lowercase hex digits.
//
// Ownership/Lifetime:
//   - Bytes objects are GC-managed (rt_obj_new_i64) with a runtime finalizer;
//     callers must not free them directly.
//   - rt_bytes_from_str and rt_bytes_from_hex allocate new GC-managed objects.
//
// Error conventions:
//   - Out-of-bounds index → rt_trap()
//   - Allocation failure → returns NULL
//
// Links: src/runtime/collections/rt_bytes.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares fixed-length mutable byte-array operations.
///
/// Bytes objects own contiguous payloads whose lengths are fixed at creation.
/// Element mutation masks values to the requested field width. Slice, clone,
/// string conversion, hexadecimal conversion, and Base64 conversion allocate
/// independent results; direct data access instead returns a borrowed pointer.
///
/// Multi-byte integer helpers operate at caller-supplied offsets using explicit
/// little- or big-endian order and trap unless the complete field is in bounds.
/// Bytes objects are runtime-managed opaque handles and are not safe for
/// unsynchronized concurrent writes.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Bytes class identifier for heap header tagging.
#define RT_BYTES_CLASS_ID INT64_C(-0x430201)

/// @brief Create a new zero-filled byte array of given length.
/// @param len Number of bytes to allocate. Negative values trap.
/// @return A new runtime-managed Bytes object, or NULL after a size or
///         allocation trap.
void *rt_bytes_new(int64_t len);

/// @brief Create a byte array from a string (UTF-8 bytes).
/// @param str Source string; NULL produces an empty Bytes object.
/// @return New Bytes object containing a copy of the full runtime byte
///         sequence, including embedded NUL bytes.
void *rt_bytes_from_str(rt_string str);

/// @brief Create a byte array from a hexadecimal string.
/// @param hex Hex string (must have even full byte length).
/// @return New Bytes object, or NULL after an allocation trap.
void *rt_bytes_from_hex(rt_string hex);

/// @brief Create a byte array from an RFC 4648 Base64 string.
/// @details Uses the standard Base64 alphabet (A–Z a–z 0–9 + /) with '=' padding.
///          Traps on invalid characters, invalid padding, or invalid full byte length.
/// @param b64 Base64 string to decode.
/// @return New Bytes object containing decoded bytes, or NULL after an
///         allocation trap.
void *rt_bytes_from_base64(rt_string b64);

/// @brief Get the length of a byte array.
/// @param obj Bytes handle, or NULL.
/// @return Number of bytes, or zero for NULL.
int64_t rt_bytes_len(void *obj);

/// @brief Check whether an object is a Bytes instance.
/// @param obj Candidate object pointer.
/// @return 1 when obj is a Bytes object, 0 otherwise.
int8_t rt_bytes_is_bytes(void *obj);

/// @brief Get the mutable raw byte buffer for a Bytes object.
/// @param obj Bytes handle, or NULL.
/// @return Borrowed mutable payload pointer, or NULL for a null or empty object.
/// @warning Do not free the pointer or retain it beyond the Bytes lifetime.
uint8_t *rt_bytes_data(void *obj);

/// @brief Get the const raw byte buffer for a Bytes object.
/// @param obj Bytes handle, or NULL.
/// @return Borrowed read-only payload pointer, or NULL for a null or empty object.
const uint8_t *rt_bytes_data_const(void *obj);

/// @brief Check if the byte array is empty (length 0).
/// @param obj Bytes handle, or NULL.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_bytes_is_empty(void *obj);

/// @brief Get a byte value at the specified index.
/// @param obj Bytes object pointer.
/// @param idx Index to read from.
/// @return Byte value (0-255); traps if out of bounds.
int64_t rt_bytes_get(void *obj, int64_t idx);

/// @brief Set a byte value at the specified index.
/// @param obj Bytes object pointer.
/// @param idx Index to write to.
/// @param val Value to write (low 8 bits are stored).
void rt_bytes_set(void *obj, int64_t idx, int64_t val);

/// @brief Create a new byte array from a slice of this one.
/// @param obj Source Bytes object pointer.
/// @param start Start index (inclusive, clamped to 0).
/// @param end End index (exclusive, clamped to len).
/// @return Pointer to new Bytes object.
void *rt_bytes_slice(void *obj, int64_t start, int64_t end);

/// @brief Copy bytes from source to destination array.
/// @param dst Destination Bytes object.
/// @param dst_idx Destination start index.
/// @param src Source Bytes object.
/// @param src_idx Source start index.
/// @param count Number of bytes to copy. Traps if range arithmetic overflows.
void rt_bytes_copy(void *dst, int64_t dst_idx, void *src, int64_t src_idx, int64_t count);

/// @brief Convert byte array to string (interprets as UTF-8).
/// @param obj Bytes object pointer.
/// @return New string containing the bytes.
rt_string rt_bytes_to_str(void *obj);

/// @brief Convert byte array to hexadecimal string.
/// @param obj Bytes object pointer.
/// @return New string with hex representation.
rt_string rt_bytes_to_hex(void *obj);

/// @brief Convert byte array to an RFC 4648 Base64 string.
/// @details Uses the standard Base64 alphabet (A–Z a–z 0–9 + /) with '=' padding and
///          emits no line breaks.
/// @param obj Bytes object pointer.
/// @return New string with Base64 representation.
rt_string rt_bytes_to_base64(void *obj);

/// @brief Fill all bytes with the given value.
/// @param obj Bytes object pointer.
/// @param val Value whose low 8 bits are used.
void rt_bytes_fill(void *obj, int64_t val);

/// @brief Find first occurrence of a byte value.
/// @param obj Bytes object pointer.
/// @param val Value whose low 8 bits are searched for.
/// @return Index of first occurrence, or -1 if not found.
int64_t rt_bytes_find(void *obj, int64_t val);

/// @brief Find first occurrence of a byte value as an Option index.
/// @details Returns `SomeI64(index)` when the byte is present and `None` when
///          the byte is absent or @p obj is NULL. This preserves index 0 as a
///          valid success value without requiring a `-1` sentinel check.
/// @param obj Bytes object pointer, or NULL.
/// @param val Value whose low 8 bits are searched for.
/// @return Opaque Zanna.Option containing the first index, or None.
void *rt_bytes_find_option(void *obj, int64_t val);

/// @brief Create a copy of the byte array.
/// @param obj Bytes object pointer.
/// @return Pointer to new Bytes object with same contents.
void *rt_bytes_clone(void *obj);

//=========================================================================
// Binary Integer Read/Write Operations
//=========================================================================

/// @brief Read a signed 16-bit little-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds two-byte field.
/// @return Sign-extended decoded value; invalid ranges trap.
int64_t rt_bytes_read_i16le(void *obj, int64_t offset);
/// @brief Read a signed 16-bit big-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds two-byte field.
/// @return Sign-extended decoded value; invalid ranges trap.
int64_t rt_bytes_read_i16be(void *obj, int64_t offset);
/// @brief Read a signed 32-bit little-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds four-byte field.
/// @return Sign-extended decoded value; invalid ranges trap.
int64_t rt_bytes_read_i32le(void *obj, int64_t offset);
/// @brief Read a signed 32-bit big-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds four-byte field.
/// @return Sign-extended decoded value; invalid ranges trap.
int64_t rt_bytes_read_i32be(void *obj, int64_t offset);
/// @brief Read a signed 64-bit little-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds eight-byte field.
/// @return Decoded signed value; invalid ranges trap.
int64_t rt_bytes_read_i64le(void *obj, int64_t offset);
/// @brief Read a signed 64-bit big-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds eight-byte field.
/// @return Decoded signed value; invalid ranges trap.
int64_t rt_bytes_read_i64be(void *obj, int64_t offset);

/// @brief Write a 16-bit little-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds two-byte field.
/// @param value Value whose low 16 bits are stored.
void rt_bytes_write_i16le(void *obj, int64_t offset, int64_t value);
/// @brief Write a 16-bit big-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds two-byte field.
/// @param value Value whose low 16 bits are stored.
void rt_bytes_write_i16be(void *obj, int64_t offset, int64_t value);
/// @brief Write a 32-bit little-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds four-byte field.
/// @param value Value whose low 32 bits are stored.
void rt_bytes_write_i32le(void *obj, int64_t offset, int64_t value);
/// @brief Write a 32-bit big-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds four-byte field.
/// @param value Value whose low 32 bits are stored.
void rt_bytes_write_i32be(void *obj, int64_t offset, int64_t value);
/// @brief Write a 64-bit little-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds eight-byte field.
/// @param value Signed value stored using its full 64-bit bit pattern.
void rt_bytes_write_i64le(void *obj, int64_t offset, int64_t value);
/// @brief Write a 64-bit big-endian integer at the given offset.
/// @param obj Non-null Bytes handle.
/// @param offset Start of an in-bounds eight-byte field.
/// @param value Signed value stored using its full 64-bit bit pattern.
void rt_bytes_write_i64be(void *obj, int64_t offset, int64_t value);

#ifdef __cplusplus
}
#endif
