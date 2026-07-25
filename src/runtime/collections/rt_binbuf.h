//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_binbuf.h
// Purpose: Positioned binary read/write buffer providing sequential byte-level I/O with explicit
// position tracking, supporting creation from raw capacity or from existing Bytes objects.
//
// Key invariants:
//   - Position advances on each read/write operation.
//   - Reads past the current length trap immediately.
//   - Writes past the current capacity trigger automatic growth.
//   - Position is always in [0, len]; writes may extend len up to cap.
//
// Ownership/Lifetime:
//   - BinaryBuffer objects are heap-allocated; caller is responsible for lifetime management.
//   - Data is stored contiguously; the internal buffer may be reallocated on growth.
//
// Links: src/runtime/collections/rt_binbuf.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares positioned binary serialization and parsing operations.
///
/// BinaryBuffer owns a contiguous byte array, a logical length, and a cursor
/// shared by reads and writes. Typed integer operations use explicit endian
/// encodings. Writes grow storage and may extend the logical length; reads
/// advance only when the complete requested value is available. Seeking is
/// limited to the existing logical range.
///
/// String and Bytes writers use a four-byte little-endian length prefix.
/// `rt_binbuf_read_bytes()` instead reads an unframed caller-specified count.
/// Returned strings and Bytes objects are independent copies. Buffer objects
/// are runtime-managed opaque handles, and invalid handles, invalid ranges,
/// overflows, and allocation failures raise runtime traps.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Constructors ----

/// @brief Create a new binary buffer with default capacity (256).
/// @return A new runtime-managed BinaryBuffer, or NULL after reporting an
///         allocation trap.
void *rt_binbuf_new(void);

/// @brief Create a new binary buffer with custom initial capacity.
/// @param capacity Initial capacity in bytes. Zero maps to 1; negative traps.
/// @return A new runtime-managed BinaryBuffer, or NULL after reporting an
///         invalid capacity or allocation trap.
void *rt_binbuf_new_cap(int64_t capacity);

/// @brief Create a binary buffer from existing bytes data.
/// @param bytes_obj Non-null Bytes handle to copy from.
/// @return A new independent BinaryBuffer with position zero and logical
///         length equal to the source length, or NULL after a trap.
void *rt_binbuf_from_bytes(void *bytes_obj);

// ---- Write operations ----

/// @brief Write a single unsigned byte at the cursor.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Byte value in `[0, 255]`; values outside this range trap.
void rt_binbuf_write_byte(void *obj, int64_t value);

/// @brief Write a 16-bit signed integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the signed 16-bit range; out-of-range values trap.
void rt_binbuf_write_i16le(void *obj, int64_t value);

/// @brief Write a 16-bit signed integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the signed 16-bit range; out-of-range values trap.
void rt_binbuf_write_i16be(void *obj, int64_t value);

/// @brief Write a 16-bit unsigned integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the unsigned 16-bit range; out-of-range values trap.
void rt_binbuf_write_u16le(void *obj, int64_t value);

/// @brief Write a 16-bit unsigned integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the unsigned 16-bit range; out-of-range values trap.
void rt_binbuf_write_u16be(void *obj, int64_t value);

/// @brief Write a 32-bit signed integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the signed 32-bit range; out-of-range values trap.
void rt_binbuf_write_i32le(void *obj, int64_t value);

/// @brief Write a 32-bit signed integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the signed 32-bit range; out-of-range values trap.
void rt_binbuf_write_i32be(void *obj, int64_t value);

/// @brief Write a 32-bit unsigned integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the unsigned 32-bit range; out-of-range values trap.
void rt_binbuf_write_u32le(void *obj, int64_t value);

/// @brief Write a 32-bit unsigned integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the unsigned 32-bit range; out-of-range values trap.
void rt_binbuf_write_u32be(void *obj, int64_t value);

/// @brief Write a 64-bit integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Signed value encoded with its 64-bit two's-complement pattern.
void rt_binbuf_write_i64le(void *obj, int64_t value);

/// @brief Write a 64-bit integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Signed value encoded with its 64-bit two's-complement pattern.
void rt_binbuf_write_i64be(void *obj, int64_t value);

/// @brief Write a length-prefixed string (4-byte LE byte length + full string bytes).
/// @param obj Non-null BinaryBuffer handle.
/// @param value String to write. NULL serializes as length 0. Embedded NUL bytes are preserved.
/// @note Byte lengths above `INT32_MAX` trap without advancing the cursor.
void rt_binbuf_write_str(void *obj, rt_string value);

/// @brief Write length-prefixed bytes (4-byte LE byte length + raw bytes).
/// @param obj Non-null BinaryBuffer handle.
/// @param data Non-null Bytes handle to write.
/// @note Invalid Bytes handles and lengths above `INT32_MAX` trap without
///       advancing the cursor.
void rt_binbuf_write_bytes(void *obj, void *data);

// ---- Read operations ----

/// @brief Read a single byte and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Byte value in `[0, 255]`; insufficient input traps and returns 0 if
///         the trap handler resumes.
int64_t rt_binbuf_read_byte(void *obj);

/// @brief Read a 16-bit little-endian integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Sign-extended decoded value; insufficient input traps.
int64_t rt_binbuf_read_i16le(void *obj);

/// @brief Read a 16-bit big-endian integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Sign-extended decoded value; insufficient input traps.
int64_t rt_binbuf_read_i16be(void *obj);

/// @brief Read a 16-bit little-endian unsigned integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Zero-extended decoded value; insufficient input traps.
int64_t rt_binbuf_read_u16le(void *obj);

/// @brief Read a 16-bit big-endian unsigned integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Zero-extended decoded value; insufficient input traps.
int64_t rt_binbuf_read_u16be(void *obj);

/// @brief Read a 32-bit little-endian integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Sign-extended decoded value; insufficient input traps.
int64_t rt_binbuf_read_i32le(void *obj);

/// @brief Read a 32-bit big-endian integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Sign-extended decoded value; insufficient input traps.
int64_t rt_binbuf_read_i32be(void *obj);

/// @brief Read a 32-bit little-endian unsigned integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Zero-extended decoded value; insufficient input traps.
int64_t rt_binbuf_read_u32le(void *obj);

/// @brief Read a 32-bit big-endian unsigned integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Zero-extended decoded value; insufficient input traps.
int64_t rt_binbuf_read_u32be(void *obj);

/// @brief Read a 64-bit little-endian integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Decoded signed value; insufficient input traps.
int64_t rt_binbuf_read_i64le(void *obj);

/// @brief Read a 64-bit big-endian integer and advance position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Decoded signed value; insufficient input traps.
int64_t rt_binbuf_read_i64be(void *obj);

/// @brief Read a length-prefixed string (4-byte LE length + UTF-8 bytes).
/// @param obj Non-null BinaryBuffer handle.
/// @return A newly created string containing the decoded bytes. Negative or
///         incomplete lengths trap and leave the cursor unchanged.
rt_string rt_binbuf_read_str(void *obj);

/// @brief Read count bytes into a new Bytes object.
/// @param obj Non-null BinaryBuffer handle.
/// @param count Non-negative number of unframed bytes to read.
/// @return A new Bytes object; invalid counts or insufficient input trap and
///         return NULL if the trap handler resumes.
void *rt_binbuf_read_bytes(void *obj, int64_t count);

// ---- Properties / Control ----

/// @brief Get the current read/write position.
/// @param obj Non-null BinaryBuffer handle.
/// @return Current byte offset, or 0 after an invalid-handle trap.
int64_t rt_binbuf_get_position(void *obj);

/// @brief Set the read/write position within the existing logical range.
/// @param obj Non-null BinaryBuffer handle.
/// @param pos New offset in `[0, len]`; out-of-range values trap and do not
///        change the cursor.
void rt_binbuf_set_position(void *obj, int64_t pos);

/// @brief Get the logical length of the buffer.
/// @param obj Non-null BinaryBuffer handle.
/// @return Number of meaningful bytes, or 0 after an invalid-handle trap.
int64_t rt_binbuf_get_len(void *obj);

/// @brief Create a Bytes object from the buffer content (0..len).
/// @param obj BinaryBuffer handle.
/// @return A new Bytes object containing a copy of `[0, len)`. If handle
///         validation traps and resumes, returns an empty Bytes object.
void *rt_binbuf_to_bytes(void *obj);

/// @brief Reset the buffer (position=0, len=0).
/// @param obj Non-null BinaryBuffer handle.
/// @note Existing capacity is retained for reuse.
void rt_binbuf_reset(void *obj);

#ifdef __cplusplus
}
#endif
