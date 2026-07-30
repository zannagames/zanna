//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_binbuf.c
// Purpose: Implements a positioned binary read/write buffer with dynamic growth.
//   BinaryBuffer maintains a heap-allocated byte array, a logical length (the
//   highest byte written + 1), and a read/write cursor position. Supports typed
//   reads and writes (byte, i16/u16, i32/u32, i64, strings, and bytes) at the cursor, with
//   automatic capacity doubling when the buffer is too small.
//
// Key invariants:
//   - Initial capacity is BINBUF_DEFAULT_CAPACITY (256) bytes.
//   - Capacity doubles on each growth; overflow beyond INT64_MAX traps with
//     "BinaryBuffer: capacity overflow".
//   - Newly allocated bytes beyond the old capacity are zero-filled (memset).
//   - `len` tracks the highest written byte index + 1; it does not shrink on
//     seek-back writes but does grow forward on writes past the current `len`.
//   - `position` is a free-seek cursor: Seek() can move it anywhere in [0, len].
//     Writing past `len` extends `len` to position + bytes_written.
//   - Read operations trap if the requested byte width is not fully available.
//   - Not thread-safe; external synchronization required for concurrent access.
//
// Ownership/Lifetime:
//   - BinaryBuffer objects are GC-managed (rt_obj_new_i64). The data array is
//     realloc-managed and freed by the GC finalizer (binbuf_finalizer).
//
// Links: src/runtime/collections/rt_binbuf.h (public API),
//        src/runtime/collections/rt_bytes.h (byte array type used for bulk I/O)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a growable, positioned binary serialization buffer.
///
/// BinaryBuffer combines a contiguous owned byte array with a single cursor
/// used by both readers and writers. Writes replace bytes at the cursor,
/// extend the logical high-water mark when necessary, and double capacity as
/// needed. Reads require their complete payload to lie within the logical
/// length. Integer methods encode explicitly in little- or big-endian order,
/// independent of host byte order.
///
/// Strings and Bytes blobs use a signed 32-bit little-endian length prefix;
/// raw `read_bytes` is deliberately unframed. Handles are runtime-managed and
/// validated against `RT_BINBUF_CLASS_ID`. The implementation is not
/// synchronized.

#include "rt_binbuf.h"

#include "rt_bytes.h"
#include "rt_internal.h"
#include "rt_io_class_ids.h"
#include "rt_object.h"
#include "rt_string.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// Default initial capacity for new binary buffers.
#define BINBUF_DEFAULT_CAPACITY 256

/// @brief Internal implementation structure for the BinaryBuffer type.
typedef struct rt_binbuf_impl {
    void **vptr;      ///< Vtable pointer placeholder (for OOP compatibility).
    uint8_t *data;    ///< Pointer to heap-allocated byte storage.
    int64_t len;      ///< Logical length (highest byte written + 1).
    int64_t capacity; ///< Allocated capacity in bytes.
    int64_t position; ///< Read/write cursor position.
} rt_binbuf_impl;

/// @brief Checked cast of an opaque handle to the BinaryBuffer impl;
///        traps if @p obj is NULL or not a BinaryBuffer.
/// @param obj Opaque runtime object handle to validate.
/// @return The validated implementation pointer, or NULL if a returning trap
///         handler resumes after validation fails.
static rt_binbuf_impl *binbuf_require(void *obj) {
    if (!rt_obj_is_instance(obj, RT_BINBUF_CLASS_ID, sizeof(rt_binbuf_impl))) {
        rt_trap("BinaryBuffer: invalid buffer");
        return NULL;
    }
    return (rt_binbuf_impl *)obj;
}

/// @brief Ensure the buffer has room for `needed` bytes starting at position.
/// @param buf Buffer implementation pointer.
/// @param needed Number of bytes needed from the current position.
/// @return 1 if the requested range fits after any necessary growth; otherwise
///         0 after reporting a validation, overflow, or allocation trap.
/// @note Failure leaves the original allocation, capacity, cursor, and length
///       unchanged.
static int binbuf_ensure(rt_binbuf_impl *buf, int64_t needed) {
    if (!buf) {
        rt_trap("BinaryBuffer: invalid buffer");
        return 0;
    }
    if (needed < 0) {
        rt_trap("BinaryBuffer: negative size");
        return 0;
    }
    if (buf->position > INT64_MAX - needed) {
        rt_trap("BinaryBuffer: position overflow");
        return 0;
    }
    int64_t required = buf->position + needed;
    if (required <= buf->capacity)
        return 1;

    int64_t new_cap = buf->capacity;
    if (new_cap < 1)
        new_cap = 1;
    // IO-H-3: guard against int64 overflow before doubling
    while (new_cap < required) {
        if (new_cap > (int64_t)(INT64_MAX / 2)) {
            rt_trap("BinaryBuffer: capacity overflow");
            return 0;
        }
        new_cap *= 2;
    }
    if ((uint64_t)new_cap > (uint64_t)SIZE_MAX) {
        rt_trap("BinaryBuffer: capacity exceeds platform limit");
        return 0;
    }

    uint8_t *new_data = (uint8_t *)realloc(buf->data, (size_t)new_cap);
    if (!new_data) {
        rt_trap("BinaryBuffer: memory allocation failed");
        return 0;
    }

    // Zero-fill newly allocated region
    memset(new_data + buf->capacity, 0, (size_t)(new_cap - buf->capacity));
    buf->data = new_data;
    buf->capacity = new_cap;
    return 1;
}

/// @brief Finalizer callback invoked when a BinaryBuffer is garbage collected.
/// @param obj Pointer to the BinaryBuffer object being finalized.
static void binbuf_finalize(void *obj) {
    if (!obj)
        return;
    rt_binbuf_impl *buf = (rt_binbuf_impl *)obj;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->capacity = 0;
    buf->position = 0;
}

/// @brief Advance position after a write and extend len if needed.
/// @param buf Buffer implementation pointer.
/// @param n Number of bytes just written.
/// @note Callers must first ensure that adding @p n cannot overflow and that
///       the written range is backed by allocated storage.
static void binbuf_advance_write(rt_binbuf_impl *buf, int64_t n) {
    buf->position += n;
    if (buf->position > buf->len)
        buf->len = buf->position;
}

/// @brief Check that `count` bytes can be read from the current position.
/// @param buf Buffer implementation pointer.
/// @param count Number of bytes to read.
/// @return 1 if the complete range is readable; otherwise 0 after reporting a
///         null-buffer, negative-count, overflow, or end-of-buffer trap.
static int binbuf_check_read(rt_binbuf_impl *buf, int64_t count) {
    if (!buf) {
        rt_trap("BinaryBuffer: invalid buffer");
        return 0;
    }
    if (count < 0) {
        rt_trap("BinaryBuffer: negative count");
        return 0;
    }
    if (buf->position > INT64_MAX - count) {
        rt_trap("BinaryBuffer: read position overflow");
        return 0;
    }
    if (buf->position + count > buf->len) {
        rt_trap("BinaryBuffer: read past end");
        return 0;
    }
    return 1;
}

// The width validators return 1 when @p value fits and 0 after trapping, so a
// caller keeps writing only when the range is valid — a returning trap hook no
// longer lets a truncated value reach the buffer (VDOC-189).

/// @brief Return 1 if @p value fits a signed 16-bit integer; trap and return 0 otherwise.
/// @param value Candidate value.
/// @return 1 when @p value is in the `int16_t` range; otherwise 0.
static int binbuf_require_i16(int64_t value) {
    if (value < INT16_MIN || value > INT16_MAX) {
        rt_trap("BinaryBuffer: i16 value out of range");
        return 0;
    }
    return 1;
}

/// @brief Return 1 if @p value fits an unsigned 16-bit integer; trap and return 0 otherwise.
/// @param value Candidate value.
/// @return 1 when @p value is in the `uint16_t` range; otherwise 0.
static int binbuf_require_u16(int64_t value) {
    if (value < 0 || value > UINT16_MAX) {
        rt_trap("BinaryBuffer: u16 value out of range");
        return 0;
    }
    return 1;
}

/// @brief Return 1 if @p value fits a signed 32-bit integer; trap and return 0 otherwise.
/// @param value Candidate value.
/// @return 1 when @p value is in the `int32_t` range; otherwise 0.
static int binbuf_require_i32(int64_t value) {
    if (value < INT32_MIN || value > INT32_MAX) {
        rt_trap("BinaryBuffer: i32 value out of range");
        return 0;
    }
    return 1;
}

/// @brief Return 1 if @p value fits an unsigned 32-bit integer; trap and return 0 otherwise.
/// @param value Candidate value.
/// @return 1 when @p value is in the `uint32_t` range; otherwise 0.
static int binbuf_require_u32(int64_t value) {
    if (value < 0 || value > UINT32_MAX) {
        rt_trap("BinaryBuffer: u32 value out of range");
        return 0;
    }
    return 1;
}

//=============================================================================
// Constructors
//=============================================================================

/// @brief Creates an empty BinaryBuffer with the default capacity.
/// @return A new runtime-managed buffer, or NULL after reporting an allocation
///         trap if construction cannot complete.
void *rt_binbuf_new(void) {
    return rt_binbuf_new_cap(BINBUF_DEFAULT_CAPACITY);
}

/// @brief Creates an empty BinaryBuffer with a requested initial capacity.
/// @param capacity Initial byte capacity; zero is normalized to one and a
///        negative value raises a runtime trap.
/// @return A new runtime-managed buffer, or NULL after reporting an invalid
///         capacity or allocation trap.
void *rt_binbuf_new_cap(int64_t capacity) {
    if (capacity < 0) {
        rt_trap("BinaryBuffer: negative capacity");
        return NULL;
    }
    if (capacity < 1)
        capacity = 1;
    if ((uint64_t)capacity > (uint64_t)SIZE_MAX) {
        rt_trap("BinaryBuffer: capacity exceeds platform limit");
        return NULL;
    }

    rt_binbuf_impl *buf =
        (rt_binbuf_impl *)rt_obj_new_i64(RT_BINBUF_CLASS_ID, (int64_t)sizeof(rt_binbuf_impl));
    if (!buf) {
        rt_trap("BinaryBuffer: memory allocation failed");
        return NULL;
    }

    buf->vptr = NULL;
    buf->data = (uint8_t *)calloc((size_t)capacity, 1);
    if (!buf->data) {
        if (rt_obj_release_check0(buf))
            rt_obj_free(buf);
        rt_trap("BinaryBuffer: memory allocation failed");
        return NULL;
    }
    buf->len = 0;
    buf->capacity = capacity;
    buf->position = 0;
    rt_obj_set_finalizer(buf, binbuf_finalize);
    return buf;
}

/// @brief Creates a BinaryBuffer containing a copy of a Bytes object.
/// @param bytes_obj Non-null Bytes handle whose logical contents are copied.
/// @return A new runtime-managed buffer positioned at zero, or NULL after
///         reporting an invalid input, size, or allocation trap.
/// @note The resulting capacity is at least the default capacity, and later
///       changes to either object do not affect the other.
void *rt_binbuf_from_bytes(void *bytes_obj) {
    if (!bytes_obj || !rt_bytes_is_bytes(bytes_obj)) {
        rt_trap("BinaryBuffer: invalid bytes");
        return NULL;
    }
    int64_t blen = rt_bytes_len(bytes_obj);
    if (blen < 0) {
        rt_trap("BinaryBuffer: invalid bytes length");
        return NULL;
    }
    int64_t cap = blen > BINBUF_DEFAULT_CAPACITY ? blen : BINBUF_DEFAULT_CAPACITY;
    if ((uint64_t)cap > (uint64_t)SIZE_MAX) {
        rt_trap("BinaryBuffer: capacity exceeds platform limit");
        return NULL;
    }

    rt_binbuf_impl *buf =
        (rt_binbuf_impl *)rt_obj_new_i64(RT_BINBUF_CLASS_ID, (int64_t)sizeof(rt_binbuf_impl));
    if (!buf) {
        rt_trap("BinaryBuffer: memory allocation failed");
        return NULL;
    }

    buf->vptr = NULL;
    buf->data = (uint8_t *)calloc((size_t)cap, 1);
    if (!buf->data) {
        if (rt_obj_release_check0(buf))
            rt_obj_free(buf);
        rt_trap("BinaryBuffer: memory allocation failed");
        return NULL;
    }
    buf->capacity = cap;
    buf->len = blen;
    buf->position = 0;
    rt_obj_set_finalizer(buf, binbuf_finalize);

    // IO-H-2: use memcpy via raw pointer instead of O(n) rt_bytes_get() calls
    const uint8_t *src = rt_bytes_data_const(bytes_obj);
    if (src && blen > 0)
        memcpy(buf->data, src, (size_t)blen);

    return buf;
}

//=============================================================================
// Write Operations
//
// Each writer below appends `value` at the current cursor, growing the buffer's
// capacity as needed, then advances the cursor past the bytes just written.
// All writers trap on a NULL handle. The little-endian (LE) variants store the
// least-significant byte first; big-endian (BE) variants store the most-
// significant byte first. Width matches the function name (i16/i32/i64).
//=============================================================================

/// @brief Append `value` as one byte; values outside 0..255 trap.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Unsigned byte value to store.
/// @note The cursor advances by one and the logical length may grow.
void rt_binbuf_write_byte(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (value < 0 || value > 255) {
        rt_trap("BinaryBuffer: byte value out of range");
        return;
    }
    if (!binbuf_ensure(buf, 1))
        return;
    buf->data[buf->position] = (uint8_t)value;
    binbuf_advance_write(buf, 1);
}

/// @brief Append a 16-bit signed integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the signed 16-bit range.
void rt_binbuf_write_i16le(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf || !binbuf_require_i16(value))
        return;
    uint16_t raw = (uint16_t)value;
    if (!binbuf_ensure(buf, 2))
        return;
    buf->data[buf->position] = (uint8_t)(raw & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 8) & 0xFF);
    binbuf_advance_write(buf, 2);
}

/// @brief Append a 16-bit signed integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the signed 16-bit range.
void rt_binbuf_write_i16be(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf || !binbuf_require_i16(value))
        return;
    uint16_t raw = (uint16_t)value;
    if (!binbuf_ensure(buf, 2))
        return;
    buf->data[buf->position] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)(raw & 0xFF);
    binbuf_advance_write(buf, 2);
}

/// @brief Append a 16-bit unsigned integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the unsigned 16-bit range.
void rt_binbuf_write_u16le(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf || !binbuf_require_u16(value))
        return;
    uint16_t raw = (uint16_t)value;
    if (!binbuf_ensure(buf, 2))
        return;
    buf->data[buf->position] = (uint8_t)(raw & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 8) & 0xFF);
    binbuf_advance_write(buf, 2);
}

/// @brief Append a 16-bit unsigned integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the unsigned 16-bit range.
void rt_binbuf_write_u16be(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf || !binbuf_require_u16(value))
        return;
    uint16_t raw = (uint16_t)value;
    if (!binbuf_ensure(buf, 2))
        return;
    buf->data[buf->position] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)(raw & 0xFF);
    binbuf_advance_write(buf, 2);
}

/// @brief Append a 32-bit signed integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the signed 32-bit range.
void rt_binbuf_write_i32le(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf || !binbuf_require_i32(value))
        return;
    uint32_t raw = (uint32_t)value;
    if (!binbuf_ensure(buf, 4))
        return;
    buf->data[buf->position] = (uint8_t)(raw & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 2] = (uint8_t)((raw >> 16) & 0xFF);
    buf->data[buf->position + 3] = (uint8_t)((raw >> 24) & 0xFF);
    binbuf_advance_write(buf, 4);
}

/// @brief Append a 32-bit signed integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the signed 32-bit range.
void rt_binbuf_write_i32be(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf || !binbuf_require_i32(value))
        return;
    uint32_t raw = (uint32_t)value;
    if (!binbuf_ensure(buf, 4))
        return;
    buf->data[buf->position] = (uint8_t)((raw >> 24) & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 16) & 0xFF);
    buf->data[buf->position + 2] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 3] = (uint8_t)(raw & 0xFF);
    binbuf_advance_write(buf, 4);
}

/// @brief Append a 32-bit unsigned integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the unsigned 32-bit range.
void rt_binbuf_write_u32le(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf || !binbuf_require_u32(value))
        return;
    uint32_t raw = (uint32_t)value;
    if (!binbuf_ensure(buf, 4))
        return;
    buf->data[buf->position] = (uint8_t)(raw & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 2] = (uint8_t)((raw >> 16) & 0xFF);
    buf->data[buf->position + 3] = (uint8_t)((raw >> 24) & 0xFF);
    binbuf_advance_write(buf, 4);
}

/// @brief Append a 32-bit unsigned integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value in the unsigned 32-bit range.
void rt_binbuf_write_u32be(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf || !binbuf_require_u32(value))
        return;
    uint32_t raw = (uint32_t)value;
    if (!binbuf_ensure(buf, 4))
        return;
    buf->data[buf->position] = (uint8_t)((raw >> 24) & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 16) & 0xFF);
    buf->data[buf->position + 2] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 3] = (uint8_t)(raw & 0xFF);
    binbuf_advance_write(buf, 4);
}

/// @brief Append a 64-bit signed integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value to encode using its 64-bit two's-complement bit pattern.
void rt_binbuf_write_i64le(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    uint64_t raw = (uint64_t)value;
    if (!binbuf_ensure(buf, 8))
        return;
    buf->data[buf->position] = (uint8_t)(raw & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 2] = (uint8_t)((raw >> 16) & 0xFF);
    buf->data[buf->position + 3] = (uint8_t)((raw >> 24) & 0xFF);
    buf->data[buf->position + 4] = (uint8_t)((raw >> 32) & 0xFF);
    buf->data[buf->position + 5] = (uint8_t)((raw >> 40) & 0xFF);
    buf->data[buf->position + 6] = (uint8_t)((raw >> 48) & 0xFF);
    buf->data[buf->position + 7] = (uint8_t)((raw >> 56) & 0xFF);
    binbuf_advance_write(buf, 8);
}

/// @brief Append a 64-bit signed integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Value to encode using its 64-bit two's-complement bit pattern.
void rt_binbuf_write_i64be(void *obj, int64_t value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    uint64_t raw = (uint64_t)value;
    if (!binbuf_ensure(buf, 8))
        return;
    buf->data[buf->position] = (uint8_t)((raw >> 56) & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 48) & 0xFF);
    buf->data[buf->position + 2] = (uint8_t)((raw >> 40) & 0xFF);
    buf->data[buf->position + 3] = (uint8_t)((raw >> 32) & 0xFF);
    buf->data[buf->position + 4] = (uint8_t)((raw >> 24) & 0xFF);
    buf->data[buf->position + 5] = (uint8_t)((raw >> 16) & 0xFF);
    buf->data[buf->position + 6] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 7] = (uint8_t)(raw & 0xFF);
    binbuf_advance_write(buf, 8);
}

/// @brief Append a UTF-8 string with a 4-byte little-endian length prefix. NULL-equivalent
/// strings serialize as length 0 with no payload.
/// @param obj Non-null BinaryBuffer handle.
/// @param value Runtime string to encode; NULL encodes an empty payload.
/// @note Embedded NUL bytes are preserved because the runtime byte length, not
///       C-string termination, controls the copy.
void rt_binbuf_write_str(void *obj, rt_string value) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf)
        return;

    if (value && !rt_string_is_handle(value)) {
        rt_trap("BinaryBuffer: invalid string");
        return;
    }
    int64_t slen = value ? rt_str_len(value) : 0;
    const char *cstr = value ? rt_string_cstr(value) : "";
    if (slen < 0) {
        rt_trap("BinaryBuffer: invalid string length");
        return;
    }
    if (slen > 0 && !cstr) {
        rt_trap("BinaryBuffer: invalid string");
        return;
    }
    if (slen > INT32_MAX) {
        rt_trap("BinaryBuffer: string length exceeds i32 length prefix");
        return;
    }

    if (!binbuf_ensure(buf, 4 + slen))
        return;

    uint32_t raw = (uint32_t)(int32_t)slen;
    buf->data[buf->position] = (uint8_t)(raw & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 2] = (uint8_t)((raw >> 16) & 0xFF);
    buf->data[buf->position + 3] = (uint8_t)((raw >> 24) & 0xFF);
    buf->position += 4;
    if (slen > 0)
        memcpy(buf->data + buf->position, cstr, (size_t)slen);
    binbuf_advance_write(buf, slen);
}

/// @brief Append a Bytes blob with a 4-byte little-endian length prefix.
/// @details The payload is copied with one bulk operation rather than
///          per-element access.
/// @param obj Non-null BinaryBuffer handle.
/// @param data Non-null Bytes handle to encode.
/// @note Invalid Bytes handles and payloads longer than `INT32_MAX` trap
///       without advancing the cursor.
void rt_binbuf_write_bytes(void *obj, void *data) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf)
        return;
    if (!data || !rt_bytes_is_bytes(data)) {
        rt_trap("BinaryBuffer: invalid bytes");
        return;
    }

    int64_t blen = rt_bytes_len(data);
    if (blen < 0) {
        rt_trap("BinaryBuffer: invalid byte length");
        return;
    }
    if (blen > INT32_MAX) {
        rt_trap("BinaryBuffer: byte length exceeds i32 length prefix");
        return;
    }

    // Write 4-byte LE length prefix
    const uint8_t *src = blen > 0 ? rt_bytes_data_const(data) : NULL;
    if (blen > 0 && !src) {
        rt_trap("BinaryBuffer: invalid bytes data");
        return;
    }

    if (!binbuf_ensure(buf, 4 + blen))
        return;

    uint32_t raw = (uint32_t)(int32_t)blen;
    buf->data[buf->position] = (uint8_t)(raw & 0xFF);
    buf->data[buf->position + 1] = (uint8_t)((raw >> 8) & 0xFF);
    buf->data[buf->position + 2] = (uint8_t)((raw >> 16) & 0xFF);
    buf->data[buf->position + 3] = (uint8_t)((raw >> 24) & 0xFF);
    buf->position += 4;

    // Write raw bytes — use memcpy via raw pointer (avoids O(n) rt_bytes_get calls)
    if (blen > 0) {
        memcpy(buf->data + buf->position, src, (size_t)blen);
    }
    binbuf_advance_write(buf, blen);
}

//=============================================================================
// Read Operations
//
// Each reader consumes the appropriate number of bytes at the current cursor
// and returns the decoded value, advancing the cursor. Endianness and width
// are encoded in the function name and must match the writer used. All readers
// trap on a NULL handle or on insufficient remaining bytes (caller must check
// `rt_binbuf_get_position` against `rt_binbuf_get_len` for control-flow reads).
//=============================================================================

/// @brief Read a single byte (returned as an int64 in [0, 255]).
/// @param obj Non-null BinaryBuffer handle.
/// @return The next unsigned byte, or 0 after reporting a failed read trap.
int64_t rt_binbuf_read_byte(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 1))
        return 0;
    int64_t val = buf->data[buf->position];
    buf->position += 1;
    return val;
}

/// @brief Read a 16-bit signed integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The sign-extended decoded value, or 0 after a failed read trap.
int64_t rt_binbuf_read_i16le(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 2))
        return 0;
    int64_t pos = buf->position;
    uint16_t raw = (uint16_t)buf->data[pos] | ((uint16_t)buf->data[pos + 1] << 8);
    int64_t val = (int64_t)(int16_t)raw;
    buf->position += 2;
    return val;
}

/// @brief Read a 16-bit signed integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The sign-extended decoded value, or 0 after a failed read trap.
int64_t rt_binbuf_read_i16be(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 2))
        return 0;
    int64_t pos = buf->position;
    uint16_t raw = ((uint16_t)buf->data[pos] << 8) | (uint16_t)buf->data[pos + 1];
    int64_t val = (int64_t)(int16_t)raw;
    buf->position += 2;
    return val;
}

/// @brief Read a 16-bit unsigned integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The zero-extended decoded value, or 0 after a failed read trap.
int64_t rt_binbuf_read_u16le(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 2))
        return 0;
    int64_t pos = buf->position;
    uint16_t raw = (uint16_t)buf->data[pos] | ((uint16_t)buf->data[pos + 1] << 8);
    buf->position += 2;
    return (int64_t)raw;
}

/// @brief Read a 16-bit unsigned integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The zero-extended decoded value, or 0 after a failed read trap.
int64_t rt_binbuf_read_u16be(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 2))
        return 0;
    int64_t pos = buf->position;
    uint16_t raw = ((uint16_t)buf->data[pos] << 8) | (uint16_t)buf->data[pos + 1];
    buf->position += 2;
    return (int64_t)raw;
}

/// @brief Read a 32-bit signed integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The sign-extended decoded value, or 0 after a failed read trap.
int64_t rt_binbuf_read_i32le(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 4))
        return 0;
    int64_t pos = buf->position;
    uint32_t raw = (uint32_t)buf->data[pos] | ((uint32_t)buf->data[pos + 1] << 8) |
                   ((uint32_t)buf->data[pos + 2] << 16) | ((uint32_t)buf->data[pos + 3] << 24);
    int64_t val = (int64_t)(int32_t)raw;
    buf->position += 4;
    return val;
}

/// @brief Read a 32-bit signed integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The sign-extended decoded value, or 0 after a failed read trap.
int64_t rt_binbuf_read_i32be(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 4))
        return 0;
    int64_t pos = buf->position;
    uint32_t raw = ((uint32_t)buf->data[pos] << 24) | ((uint32_t)buf->data[pos + 1] << 16) |
                   ((uint32_t)buf->data[pos + 2] << 8) | (uint32_t)buf->data[pos + 3];
    int64_t val = (int64_t)(int32_t)raw;
    buf->position += 4;
    return val;
}

/// @brief Read a 32-bit unsigned integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The zero-extended decoded value, or 0 after a failed read trap.
int64_t rt_binbuf_read_u32le(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 4))
        return 0;
    int64_t pos = buf->position;
    uint32_t raw = (uint32_t)buf->data[pos] | ((uint32_t)buf->data[pos + 1] << 8) |
                   ((uint32_t)buf->data[pos + 2] << 16) | ((uint32_t)buf->data[pos + 3] << 24);
    buf->position += 4;
    return (int64_t)raw;
}

/// @brief Read a 32-bit unsigned integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The zero-extended decoded value, or 0 after a failed read trap.
int64_t rt_binbuf_read_u32be(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 4))
        return 0;
    int64_t pos = buf->position;
    uint32_t raw = ((uint32_t)buf->data[pos] << 24) | ((uint32_t)buf->data[pos + 1] << 16) |
                   ((uint32_t)buf->data[pos + 2] << 8) | (uint32_t)buf->data[pos + 3];
    buf->position += 4;
    return (int64_t)raw;
}

/// @brief Read a 64-bit signed integer in little-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The decoded signed value, or 0 after a failed read trap.
int64_t rt_binbuf_read_i64le(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 8))
        return 0;
    int64_t pos = buf->position;
    uint64_t raw = (uint64_t)buf->data[pos] | ((uint64_t)buf->data[pos + 1] << 8) |
                   ((uint64_t)buf->data[pos + 2] << 16) | ((uint64_t)buf->data[pos + 3] << 24) |
                   ((uint64_t)buf->data[pos + 4] << 32) | ((uint64_t)buf->data[pos + 5] << 40) |
                   ((uint64_t)buf->data[pos + 6] << 48) | ((uint64_t)buf->data[pos + 7] << 56);
    int64_t val = (int64_t)raw;
    buf->position += 8;
    return val;
}

/// @brief Read a 64-bit signed integer in big-endian byte order.
/// @param obj Non-null BinaryBuffer handle.
/// @return The decoded signed value, or 0 after a failed read trap.
int64_t rt_binbuf_read_i64be(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!binbuf_check_read(buf, 8))
        return 0;
    int64_t pos = buf->position;
    uint64_t raw = ((uint64_t)buf->data[pos] << 56) | ((uint64_t)buf->data[pos + 1] << 48) |
                   ((uint64_t)buf->data[pos + 2] << 40) | ((uint64_t)buf->data[pos + 3] << 32) |
                   ((uint64_t)buf->data[pos + 4] << 24) | ((uint64_t)buf->data[pos + 5] << 16) |
                   ((uint64_t)buf->data[pos + 6] << 8) | (uint64_t)buf->data[pos + 7];
    int64_t val = (int64_t)raw;
    buf->position += 8;
    return val;
}

/// @brief Read a length-prefixed UTF-8 string written by `_write_str`. Traps on negative or
/// out-of-bounds length. Returns a fresh rt_string copy of the bytes.
/// @param obj Non-null BinaryBuffer handle.
/// @return A newly created runtime string containing the decoded bytes. A
///         failed read returns the runtime empty string after reporting a trap.
/// @note The cursor advances only after both prefix and payload validate.
rt_string rt_binbuf_read_str(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);

    if (!binbuf_check_read(buf, 4))
        return rt_str_empty();

    int64_t pos = buf->position;
    uint32_t raw = (uint32_t)buf->data[pos] | ((uint32_t)buf->data[pos + 1] << 8) |
                   ((uint32_t)buf->data[pos + 2] << 16) | ((uint32_t)buf->data[pos + 3] << 24);
    int64_t slen = (int32_t)raw;
    if (slen < 0) {
        rt_trap("BinaryBuffer: invalid string length");
        return rt_str_empty();
    }

    int64_t payload_pos = pos + 4;
    if (slen > buf->len - payload_pos) {
        rt_trap("BinaryBuffer: read past end");
        return rt_str_empty();
    }

    rt_string result = rt_string_from_bytes((const char *)(buf->data + payload_pos), (size_t)slen);
    buf->position = payload_pos + slen;
    return result;
}

/// @brief Read `count` raw bytes into a fresh Bytes blob (no length prefix — caller owns the
/// framing). Traps on negative count or under-run.
/// @param obj Non-null BinaryBuffer handle.
/// @param count Number of raw bytes to copy.
/// @return A new Bytes object containing the requested range, or NULL after
///         reporting an invalid count or incomplete read.
void *rt_binbuf_read_bytes(void *obj, int64_t count) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (count < 0) {
        rt_trap("BinaryBuffer: negative read count");
        return NULL;
    }

    if (!binbuf_check_read(buf, count))
        return NULL;

    void *result = rt_bytes_new(count);
    if (!result)
        return NULL;
    uint8_t *dst = rt_bytes_data(result);
    if (dst && count > 0)
        memcpy(dst, buf->data + buf->position, (size_t)count);

    buf->position += count;
    return result;
}

//=============================================================================
// Properties / Control
//=============================================================================

/// @brief Return the read/write cursor position in bytes from the buffer start. 0 for NULL.
/// @param obj Non-null BinaryBuffer handle.
/// @return Current cursor offset, or 0 after reporting an invalid-handle trap.
int64_t rt_binbuf_get_position(void *obj) {
    // binbuf_require traps and returns NULL on an invalid receiver; do not
    // dereference NULL if a returning trap hook falls through (VDOC-189).
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf)
        return 0;
    return buf->position;
}

/// @brief Seek the cursor to a byte offset in [0, len]; out-of-range input traps.
/// Subsequent reads/writes happen from this position.
/// @param obj Non-null BinaryBuffer handle.
/// @param pos Requested cursor offset in the closed interval `[0, len]`.
void rt_binbuf_set_position(void *obj, int64_t pos) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf)
        return;
    if (pos < 0 || pos > buf->len) {
        rt_trap("BinaryBuffer: position out of range");
        return;
    }
    buf->position = pos;
}

/// @brief Return the total written size in bytes (the high-water mark, not capacity).
/// @param obj Non-null BinaryBuffer handle.
/// @return Logical byte length, or 0 after reporting an invalid-handle trap.
int64_t rt_binbuf_get_len(void *obj) {
    // See rt_binbuf_get_position: guard against a returning trap hook (VDOC-189).
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf)
        return 0;
    return buf->len;
}

/// @brief Snapshot the buffer's full contents as a fresh Bytes blob (memcpy, not a view).
/// Returns an empty Bytes for a NULL handle.
/// @param obj BinaryBuffer handle to snapshot.
/// @return A new Bytes object containing offsets `[0, len)`. If handle
///         validation traps and returns, an empty Bytes object is returned.
void *rt_binbuf_to_bytes(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf)
        return rt_bytes_new(0);
    void *result = rt_bytes_new(buf->len);
    // IO-M-2: use memcpy via raw pointer instead of O(n) rt_bytes_set() calls
    uint8_t *dst = rt_bytes_data(result);
    if (dst && buf->len > 0)
        memcpy(dst, buf->data, (size_t)buf->len);

    return result;
}

/// @brief Reset position and length to 0 (logical truncation). Capacity is not freed; the
/// backing buffer can be re-used for the next batch without reallocation.
/// @param obj Non-null BinaryBuffer handle.
void rt_binbuf_reset(void *obj) {
    rt_binbuf_impl *buf = binbuf_require(obj);
    if (!buf)
        return;
    buf->position = 0;
    buf->len = 0;
}
