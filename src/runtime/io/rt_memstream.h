//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_memstream.h
// Purpose: In-memory binary stream backed by a growable byte buffer, providing seekable read/write
// of primitive types in little-endian encoding, strings, and raw byte arrays.
//
// Key invariants:
//   - Primitive types use little-endian encoding for cross-platform compatibility.
//   - Writing past the current end automatically grows the buffer.
//   - Absolute seek and relative skip may place the cursor beyond logical end;
//     reads past end trap and a later write zero-fills the observable gap.
//   - Signed integer encodings use mathematical two's-complement conversion,
//     independent of implementation-defined signed shifts or casts.
//
// Ownership/Lifetime:
//   - MemStream objects are runtime-managed opaque handles; their finalizer releases the backing
//     allocation when the last reference is reclaimed.
//   - Bytes inputs and string inputs are borrowed and copied rather than retained.
//   - rt_memstream_to_bytes and read_bytes return fresh Bytes snapshots; read_str returns a fresh
//     runtime string. Their references transfer to the caller.
//
// Links: src/runtime/io/rt_memstream.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares the managed growable in-memory binary stream API.
 * @details Exposes construction from capacity or Bytes, cursor and capacity
 * properties, portable little-endian integer and IEEE floating-point I/O,
 * string and Bytes copying, sparse writes with zero-filled gaps, snapshots,
 * clearing, seeking, and skipping.
 */
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Constructors
//=========================================================================

/// @brief Create a new empty expandable memory stream.
/// @details Starts with no backing allocation; the first nonempty write reserves at least 64
///          bytes.
/// @return Fresh runtime-managed MemStream object.
void *rt_memstream_new(void);

/// @brief Determine whether an opaque pointer is a complete MemStream handle.
/// @details Checks managed-heap registration, object kind, class id, and the
///          minimum payload size before implementation fields are accessed.
/// @param obj Candidate runtime pointer; may be NULL or unrelated.
/// @return One for a valid MemStream handle, otherwise zero.
int8_t rt_memstream_is_handle(void *obj);

/// @brief Create a new memory stream with initial capacity hint.
/// @details Zero creates a lazy stream. A positive request reserves at least
///          that many bytes and may round up to the 64-byte minimum growth
///          quantum. Reserved bytes remain outside logical Length until a
///          write; gaps made observable by a sparse write are zero-filled.
/// @param capacity Minimum initial buffer capacity; must be non-negative.
/// @return Owned MemStream object, or NULL after an allocation trap.
void *rt_memstream_new_capacity(int64_t capacity);

/// @brief Create a memory stream from an existing Bytes object.
/// @details Copies the complete payload, sets logical length to that size, and leaves position at
///          zero. The input is not retained.
/// @param bytes Borrowed valid Bytes object to copy.
/// @return Fresh MemStream containing an independent copy.
void *rt_memstream_from_bytes(void *bytes);

//=========================================================================
// Properties
//=========================================================================

/// @brief Get the current position in the stream.
/// @param obj MemStream object.
/// @return Current position.
int64_t rt_memstream_get_pos(void *obj);

/// @brief Set the current position in the stream.
/// @param obj MemStream object.
/// @param pos New position (traps if negative).
void rt_memstream_set_pos(void *obj, int64_t pos);

/// @brief Get the length of data in the stream.
/// @param obj MemStream object.
/// @return Data length in bytes.
int64_t rt_memstream_get_len(void *obj);

/// @brief Get the current buffer capacity.
/// @param obj MemStream object.
/// @return Buffer capacity in bytes.
int64_t rt_memstream_get_capacity(void *obj);

//=========================================================================
// Integer Read/Write (little-endian)
//=========================================================================

/// @brief Read a signed 8-bit integer.
/// @param obj MemStream object.
/// @return Signed byte value (-128 to 127). Traps if insufficient bytes.
int64_t rt_memstream_read_i8(void *obj);

/// @brief Write a signed 8-bit integer.
/// @param obj MemStream object.
/// @param value Value to write; traps outside -128 through 127.
void rt_memstream_write_i8(void *obj, int64_t value);

/// @brief Read an unsigned 8-bit integer.
/// @param obj MemStream object.
/// @return Unsigned byte value (0 to 255). Traps if insufficient bytes.
int64_t rt_memstream_read_u8(void *obj);

/// @brief Write an unsigned 8-bit integer.
/// @param obj MemStream object.
/// @param value Value to write; traps outside 0 through 255.
void rt_memstream_write_u8(void *obj, int64_t value);

/// @brief Read a signed 16-bit integer (little-endian).
/// @param obj MemStream object.
/// @return Signed 16-bit value. Traps if insufficient bytes.
int64_t rt_memstream_read_i16(void *obj);

/// @brief Write a signed 16-bit integer (little-endian).
/// @param obj MemStream object.
/// @param value Value to write; traps outside the signed 16-bit range.
void rt_memstream_write_i16(void *obj, int64_t value);

/// @brief Read an unsigned 16-bit integer (little-endian).
/// @param obj MemStream object.
/// @return Unsigned 16-bit value. Traps if insufficient bytes.
int64_t rt_memstream_read_u16(void *obj);

/// @brief Write an unsigned 16-bit integer (little-endian).
/// @param obj MemStream object.
/// @param value Value to write; traps outside the unsigned 16-bit range.
void rt_memstream_write_u16(void *obj, int64_t value);

/// @brief Read a signed 32-bit integer (little-endian).
/// @param obj MemStream object.
/// @return Signed 32-bit value. Traps if insufficient bytes.
int64_t rt_memstream_read_i32(void *obj);

/// @brief Write a signed 32-bit integer (little-endian).
/// @param obj MemStream object.
/// @param value Value to write; traps outside the signed 32-bit range.
void rt_memstream_write_i32(void *obj, int64_t value);

/// @brief Read an unsigned 32-bit integer (little-endian).
/// @param obj MemStream object.
/// @return Unsigned 32-bit value. Traps if insufficient bytes.
int64_t rt_memstream_read_u32(void *obj);

/// @brief Write an unsigned 32-bit integer (little-endian).
/// @param obj MemStream object.
/// @param value Value to write; traps outside the unsigned 32-bit range.
void rt_memstream_write_u32(void *obj, int64_t value);

/// @brief Read a signed 64-bit integer (little-endian).
/// @param obj MemStream object.
/// @return Signed 64-bit value. Traps if insufficient bytes.
int64_t rt_memstream_read_i64(void *obj);

/// @brief Write a signed 64-bit integer (little-endian).
/// @param obj MemStream object.
/// @param value Value to write.
void rt_memstream_write_i64(void *obj, int64_t value);

//=========================================================================
// Float Read/Write (little-endian IEEE 754)
//=========================================================================

/// @brief Read a 32-bit float (little-endian).
/// @param obj MemStream object.
/// @return Float value (as f64). Traps if insufficient bytes.
double rt_memstream_read_f32(void *obj);

/// @brief Write a 32-bit float (little-endian).
/// @param obj MemStream object.
/// @param value Value to write (converted to 32-bit float).
void rt_memstream_write_f32(void *obj, double value);

/// @brief Read a 64-bit double (little-endian).
/// @param obj MemStream object.
/// @return Double value. Traps if insufficient bytes.
double rt_memstream_read_f64(void *obj);

/// @brief Write a 64-bit double (little-endian).
/// @param obj MemStream object.
/// @param value Value to write.
void rt_memstream_write_f64(void *obj, double value);

//=========================================================================
// Bytes/String Read/Write
//=========================================================================

/// @brief Read count bytes as a Bytes object.
/// @param obj MemStream object.
/// @param count Number of bytes to read. Traps if negative or insufficient.
/// @return New Bytes object.
void *rt_memstream_read_bytes(void *obj, int64_t count);

/// @brief Write a Bytes object to the stream.
/// @details Copies the complete payload at the cursor and grows/zero-fills sparse gaps as needed.
/// @param obj Borrowed MemStream object.
/// @param bytes Borrowed valid Bytes object to write.
void rt_memstream_write_bytes(void *obj, void *bytes);

/// @brief Read count bytes as a string.
/// @details Copies raw bytes without encoding validation or a terminator and advances the cursor.
/// @param obj Borrowed MemStream object.
/// @param count Number of bytes to read. Traps if negative or insufficient.
/// @return New string.
rt_string rt_memstream_read_str(void *obj, int64_t count);

/// @brief Write a string to the stream (no length prefix).
/// @details Copies the runtime byte span verbatim, including embedded NUL bytes.
/// @param obj Borrowed MemStream object.
/// @param text Borrowed non-null runtime string to write.
void rt_memstream_write_str(void *obj, rt_string text);

//=========================================================================
// Stream Operations
//=========================================================================

/// @brief Get entire stream contents as a Bytes object.
/// @details Copies `[0, Length)` without changing the cursor.
/// @param obj Borrowed MemStream object.
/// @return Fresh Bytes snapshot of the logical buffer.
void *rt_memstream_to_bytes(void *obj);

/// @brief Reset the stream to empty state.
/// @param obj MemStream object.
void rt_memstream_clear(void *obj);

/// @brief Set position (alias for set_pos).
/// @param obj MemStream object.
/// @param pos New position. Traps if negative.
void rt_memstream_seek(void *obj, int64_t pos);

/// @brief Advance position by count bytes.
/// @param obj MemStream object.
/// @param count Number of bytes to skip. Traps if result would be negative.
void rt_memstream_skip(void *obj, int64_t count);

#ifdef __cplusplus
}
#endif
