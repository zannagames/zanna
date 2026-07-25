//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_bytes.c
// Purpose: Implements an immutable-length byte array for binary data storage.
//   The Bytes type provides O(1) random access to raw bytes, base64 encode/
//   decode, hex encode/decode, and bulk copy operations. Unlike strings, Bytes
//   are mutable (individual bytes may be set) and hold arbitrary binary content
//   with no encoding constraints.
//
// Key invariants:
//   - The data array is allocated inline immediately after the rt_bytes_impl
//     header for cache locality; a single allocation covers header + data.
//   - Length is fixed at construction time and cannot change (no resize).
//   - Byte values are in [0, 255]; get and set trap on out-of-bounds indices.
//   - Base64 and hex conversions produce rt_string results allocated via the
//     Zanna string allocator; the Bytes object is not modified.
//   - Not thread-safe; external synchronization required for concurrent writes.
//
// Ownership/Lifetime:
//   - Bytes objects are GC-managed (rt_obj_new_i64). The data array is inline
//     in the same allocation as the header and freed with the object by the GC.
//
// Links: src/runtime/collections/rt_bytes.h (public API),
//        src/runtime/rt_codec.h (base64/hex codec utilities)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements fixed-length mutable runtime byte arrays.
///
/// A Bytes allocation stores its object header and byte payload in one
/// contiguous runtime-managed block. Its length never changes, but callers can
/// mutate elements, fill ranges, perform overlap-safe copies, and read or write
/// fixed-width integers in explicit byte order.
///
/// Conversion helpers copy between Bytes, runtime strings, hexadecimal, and
/// RFC 4648 Base64 representations. Internal raw-buffer helpers make ownership
/// explicit: extracted raw memory is caller-freed, while imported raw memory is
/// copied. Mutation is unsynchronized.

#include "rt_bytes.h"

#include "rt_codec.h"
#include "rt_error.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_string.h"

#include <stdlib.h>
#include <string.h>

/// @brief Internal implementation structure for the Bytes type.
///
/// The Bytes container stores a contiguous array of raw bytes with O(1)
/// random access. Unlike strings which are immutable and UTF-8 encoded,
/// Bytes are mutable and hold raw binary data.
///
/// **Memory layout:**
/// ```
/// +------------------+---------------------------+
/// | rt_bytes_impl    | data bytes (inline)       |
/// | [len][data ptr]  | [b0][b1][b2]...[bN-1]     |
/// +------------------+---------------------------+
///                    ^
///                    |
///              data pointer points here
/// ```
///
/// The data array is allocated inline immediately after the structure header
/// for better cache locality and to avoid a separate heap allocation.
typedef struct rt_bytes_impl {
    int64_t len;   ///< Number of bytes stored (0 to INT64_MAX).
    uint8_t *data; ///< Pointer to inline byte storage (immediately follows struct).
} rt_bytes_impl;

// ---------------------------------------------------------------------------
// Trap helpers — wrappers that pair each trap kind with its
// matching error code. Centralizes the boilerplate so individual
// callers stay terse.
// ---------------------------------------------------------------------------

/// @brief Raise a generic runtime trap (e.g. allocation failure).
/// @param msg Human-readable trap message.
static void rt_bytes_trap_runtime(const char *msg) {
    rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR, Err_RuntimeError, -1, msg);
}

/// @brief Raise a domain-error trap (e.g. invalid argument).
/// @param msg Human-readable trap message.
static void rt_bytes_trap_domain(const char *msg) {
    rt_trap_raise_kind(RT_TRAP_KIND_DOMAIN_ERROR, Err_DomainError, -1, msg);
}

/// @brief Raise an invalid-operation trap (e.g. operation on NULL Bytes).
/// @param msg Human-readable trap message.
static void rt_bytes_trap_invalid_operation(const char *msg) {
    rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, -1, msg);
}

/// @brief Raise an out-of-bounds trap (offset/index outside the byte array).
/// @param msg Human-readable trap message.
static void rt_bytes_trap_bounds(const char *msg) {
    rt_trap_raise_kind(RT_TRAP_KIND_BOUNDS, Err_Bounds, -1, msg);
}

/// @brief Raise an overflow trap (e.g. requested size exceeds INT64_MAX).
/// @param msg Human-readable trap message.
static void rt_bytes_trap_overflow(const char *msg) {
    rt_trap_raise_kind(RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, msg);
}

/// @brief Allocates a new Bytes object with the specified length.
///
/// This internal helper performs the actual memory allocation for a Bytes
/// object. It allocates a single contiguous block containing both the
/// rt_bytes_impl header and the byte array, setting up the data pointer
/// to reference the inline storage.
///
/// **Allocation calculation:**
/// ```
/// total_size = sizeof(rt_bytes_impl) + len
/// ```
///
/// @param len The number of bytes to allocate. Negative values trap.
///            Values exceeding available memory will trigger a trap.
///
/// @return A pointer to the newly allocated rt_bytes_impl structure.
///
/// @note Traps with "Bytes: memory allocation failed" if allocation fails
///       or if the length would cause integer overflow.
/// @note The allocated bytes are zero-initialized.
static rt_bytes_impl *rt_bytes_alloc(int64_t len) {
    if (len < 0) {
        rt_bytes_trap_domain("Bytes.New: negative length");
        return NULL;
    }

    size_t total = sizeof(rt_bytes_impl);
    if (len > 0) {
        if ((uint64_t)len > (uint64_t)SIZE_MAX - total) {
            rt_bytes_trap_runtime("Bytes: memory allocation failed");
            return NULL;
        }
        total += (size_t)len;
    }
    if (total > (size_t)INT64_MAX) {
        rt_bytes_trap_runtime("Bytes: memory allocation failed");
        return NULL;
    }

    rt_bytes_impl *bytes = (rt_bytes_impl *)rt_obj_new_i64(RT_BYTES_CLASS_ID, (int64_t)total);
    if (!bytes) {
        rt_bytes_trap_runtime("Bytes: memory allocation failed");
        return NULL;
    }

    bytes->len = len;
    bytes->data = len > 0 ? ((uint8_t *)bytes + sizeof(rt_bytes_impl)) : NULL;
    return bytes;
}

/// @brief Checked cast of an opaque handle to the Bytes implementation.
/// @details Returns NULL for a NULL @p obj; traps via
///          rt_bytes_trap_invalid_operation(@p what) if @p obj is not a Bytes.
/// @param obj Opaque runtime object handle to inspect.
/// @param what Trap message used for a non-null handle of the wrong class.
/// @return The Bytes implementation pointer, or NULL for a null or invalid
///         handle.
static rt_bytes_impl *rt_bytes_require(void *obj, const char *what) {
    if (!obj)
        return NULL;
    if (!rt_obj_is_instance(obj, RT_BYTES_CLASS_ID, sizeof(rt_bytes_impl))) {
        rt_bytes_trap_invalid_operation(what);
        return NULL;
    }
    return (rt_bytes_impl *)obj;
}

/// @brief Base64 character lookup table for encoding (RFC 4648).
///
/// Maps 6-bit values (0-63) to the standard Base64 alphabet:
/// - 0-25: 'A'-'Z'
/// - 26-51: 'a'-'z'
/// - 52-61: '0'-'9'
/// - 62: '+'
/// - 63: '/'
static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief Converts a Base64 character to its numeric value.
///
/// Parses a single Base64 character and returns its 6-bit value according
/// to RFC 4648. The padding character '=' is treated specially.
///
/// @param c The character to convert.
///
/// @return One of:
///         - 0-63: The 6-bit value for valid Base64 characters
///         - -2: For the padding character '='
///         - -1: For any invalid character
static int b64_digit_value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    if (c == '=')
        return -2;
    return -1;
}

/// @brief Creates a new Bytes object with the specified length.
///
/// Allocates and initializes a new byte array of the given length. All bytes
/// are initialized to zero. The Bytes object is allocated through Zanna's
/// GC-managed object system and will be automatically freed when no longer
/// referenced.
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.New(1024)    ' Create 1KB buffer
/// data.Set(0, 255)              ' Set first byte
/// Print data.Get(0)             ' Outputs: 255
/// ```
///
/// @param len The number of bytes to allocate. Negative values trap.
///
/// @return A pointer to the newly created Bytes object.
///
/// @note O(n) time complexity due to zero-initialization.
/// @note Traps if memory allocation fails.
///
/// @see rt_bytes_from_str For creating Bytes from a string
/// @see rt_bytes_from_hex For creating Bytes from hex encoding
/// @see rt_bytes_from_base64 For creating Bytes from Base64 encoding
void *rt_bytes_new(int64_t len) {
    return rt_bytes_alloc(len);
}

/// @brief Creates a Bytes object from a string's UTF-8 bytes.
///
/// Converts a string to its raw UTF-8 byte representation. This is useful
/// for working with binary protocols or file I/O where you need the actual
/// bytes that make up the string.
///
/// **Important:** The resulting Bytes contains the UTF-8 encoded bytes of
/// the string, NOT the string's characters. Multi-byte UTF-8 characters
/// will occupy multiple bytes in the result.
///
/// **Usage example:**
/// ```
/// Dim str = "Hello"
/// Dim data = Bytes.FromString(str)
/// Print data.Len       ' Outputs: 5
/// Print data.Get(0)    ' Outputs: 72 (ASCII 'H')
/// ```
///
/// @param str The source string to convert. If NULL, returns empty Bytes.
///
/// @return A new Bytes object containing the UTF-8 bytes of the string.
///
/// @note O(n) time complexity where n is the string length.
/// @note The resulting Bytes does NOT include a null terminator.
///
/// @see rt_bytes_to_str For the reverse operation
void *rt_bytes_from_str(rt_string str) {
    // Use rt_str_len (stored byte count) rather than strlen so that strings
    // containing embedded null bytes (e.g. binary data) are preserved in full.
    // strlen would truncate at the first 0x00 byte (BUG-IO-001).
    int64_t len64 = rt_str_len(str);
    if (len64 <= 0)
        return rt_bytes_new(0);

    size_t len = (size_t)len64;
    const char *cstr = rt_string_cstr(str);
    if (!cstr)
        return rt_bytes_new(0);

    rt_bytes_impl *bytes = rt_bytes_alloc((int64_t)len);
    if (!bytes)
        return NULL; // returning-hook NULL after an allocation trap (VDOC-177)
    memcpy(bytes->data, cstr, len);
    return bytes;
}

/// @brief Creates a Bytes object from a hexadecimal string.
///
/// Decodes a hexadecimal string into raw bytes. Each pair of hex characters
/// in the input string becomes one byte in the output. Both uppercase and
/// lowercase hex digits are accepted.
///
/// **Hex encoding format:**
/// ```
/// Input:  "48656c6c6f"
/// Output: [0x48, 0x65, 0x6c, 0x6c, 0x6f] = "Hello" in ASCII
/// ```
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.FromHex("deadbeef")
/// Print data.Len       ' Outputs: 4
/// Print data.Get(0)    ' Outputs: 222 (0xDE)
/// ```
///
/// @param hex The hexadecimal string to decode. Must have even length.
///            If NULL, returns empty Bytes.
///
/// @return A new Bytes object containing the decoded bytes.
///
/// @note O(n) time complexity where n is the hex string length.
/// @note Traps with "Bytes.FromHex: hex string length must be even" if
///       the input has odd length.
/// @note Traps with "Bytes.FromHex: invalid hex character" if the input
///       contains non-hexadecimal characters.
///
/// @see rt_bytes_to_hex For the reverse operation
void *rt_bytes_from_hex(rt_string hex) {
    int64_t hex_len_i64 = rt_str_len(hex);
    if (hex_len_i64 <= 0)
        return rt_bytes_new(0);

    const char *hex_str = rt_string_cstr(hex);
    if (!hex_str)
        return rt_bytes_new(0);

    if ((hex_len_i64 % 2) != 0)
        rt_bytes_trap_domain("Bytes.FromHex: hex string length must be even");

    int64_t len = hex_len_i64 / 2;
    for (int64_t i = 0; i < len; i++) {
        int hi = rt_hex_digit_value(hex_str[i * 2]);
        int lo = rt_hex_digit_value(hex_str[i * 2 + 1]);

        if (hi < 0 || lo < 0)
            rt_bytes_trap_domain("Bytes.FromHex: invalid hex character");
    }

    rt_bytes_impl *bytes = rt_bytes_alloc(len);
    if (!bytes)
        return NULL; // returning-hook NULL after an allocation trap (VDOC-177)
    for (int64_t i = 0; i < len; i++) {
        int hi = rt_hex_digit_value(hex_str[i * 2]);
        int lo = rt_hex_digit_value(hex_str[i * 2 + 1]);
        bytes->data[i] = (uint8_t)((hi << 4) | lo);
    }

    return bytes;
}

/// @brief Returns the length of the Bytes object in bytes.
///
/// Gets the number of bytes stored in the Bytes object. This is the value
/// specified when the object was created and does not change during the
/// object's lifetime.
///
/// @param obj Pointer to a Bytes object. If NULL, returns 0.
///
/// @return The number of bytes, or 0 if obj is NULL.
///
/// @note O(1) time complexity.
int64_t rt_bytes_len(void *obj) {
    if (!obj)
        return 0;
    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes: invalid Bytes object");
    return bytes ? bytes->len : 0; // guard a returning-hook NULL (VDOC-177)
}

/// @brief Tests whether an opaque handle identifies a Bytes object.
/// @param obj Candidate runtime object handle, including NULL.
/// @return 1 for a valid Bytes instance; otherwise 0.
int8_t rt_bytes_is_bytes(void *obj) {
    return rt_obj_is_instance(obj, RT_BYTES_CLASS_ID, sizeof(rt_bytes_impl)) ? 1 : 0;
}

/// @brief Returns direct mutable access to a Bytes payload.
/// @param obj Bytes handle, or NULL.
/// @return Borrowed mutable payload pointer, or NULL for a null, invalid, or
///         zero-length Bytes object.
/// @warning The pointer is valid only while the owning Bytes object remains
///          alive and must not be freed by the caller.
uint8_t *rt_bytes_data(void *obj) {
    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.Data: invalid Bytes object");
    return bytes ? bytes->data : NULL;
}

/// @brief Returns direct read-only access to a Bytes payload.
/// @param obj Bytes handle, or NULL.
/// @return Borrowed payload pointer, or NULL for a null, invalid, or
///         zero-length Bytes object.
const uint8_t *rt_bytes_data_const(void *obj) {
    return rt_bytes_data(obj);
}

/// @brief Check if the byte array is empty (length 0).
/// @param obj Bytes object pointer.
/// @return 1 if empty or NULL, 0 otherwise.
int8_t rt_bytes_is_empty(void *obj) {
    if (!obj)
        return 1;
    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.IsEmpty: invalid Bytes object");
    return (!bytes || bytes->len == 0) ? 1 : 0; // guard returning-hook NULL (VDOC-177)
}

/// @brief Gets the byte value at the specified index.
///
/// Retrieves a single byte from the Bytes object. The returned value is
/// in the range 0-255 (unsigned byte).
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.FromHex("deadbeef")
/// Print data.Get(0)    ' Outputs: 222 (0xDE)
/// Print data.Get(1)    ' Outputs: 173 (0xAD)
/// ```
///
/// @param obj Pointer to a Bytes object. Must not be NULL.
/// @param idx Zero-based index of the byte to retrieve. Must be in range
///            [0, len-1].
///
/// @return The byte value at the specified index (0-255).
///
/// @note O(1) time complexity.
/// @note Traps with "Bytes.Get: null bytes" if obj is NULL.
/// @note Traps with "Bytes.Get: index out of bounds" if idx is negative
///       or >= len.
///
/// @see rt_bytes_set For modifying a byte
int64_t rt_bytes_get(void *obj, int64_t idx) {
    if (!obj)
        rt_bytes_trap_invalid_operation("Bytes.Get: null bytes");

    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.Get: invalid Bytes object");
    if (!bytes)
        return 0; // returning-hook NULL after a wrong-class trap (VDOC-177)

    if (idx < 0 || idx >= bytes->len)
        rt_bytes_trap_bounds("Bytes.Get: index out of bounds");

    return bytes->data[idx];
}

/// @brief Sets the byte value at the specified index.
///
/// Modifies a single byte in the Bytes object. The value is masked to
/// 8 bits (val & 0xFF), so values outside 0-255 are truncated.
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.New(4)
/// data.Set(0, 0xDE)
/// data.Set(1, 0xAD)
/// data.Set(2, 0xBE)
/// data.Set(3, 0xEF)
/// Print data.ToHex()   ' Outputs: "deadbeef"
/// ```
///
/// @param obj Pointer to a Bytes object. Must not be NULL.
/// @param idx Zero-based index of the byte to modify. Must be in range
///            [0, len-1].
/// @param val The value to set. Only the low 8 bits (val & 0xFF) are used.
///
/// @note O(1) time complexity.
/// @note Traps with "Bytes.Set: null bytes" if obj is NULL.
/// @note Traps with "Bytes.Set: index out of bounds" if idx is negative
///       or >= len.
///
/// @see rt_bytes_get For reading a byte
/// @see rt_bytes_fill For setting all bytes to the same value
void rt_bytes_set(void *obj, int64_t idx, int64_t val) {
    if (!obj)
        rt_bytes_trap_invalid_operation("Bytes.Set: null bytes");

    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.Set: invalid Bytes object");
    if (!bytes)
        return; // returning-hook NULL after a wrong-class trap (VDOC-177)

    if (idx < 0 || idx >= bytes->len)
        rt_bytes_trap_bounds("Bytes.Set: index out of bounds");

    bytes->data[idx] = (uint8_t)(val & 0xFF);
}

/// @brief Creates a new Bytes object containing a slice of the original.
///
/// Extracts a contiguous range of bytes from the source Bytes object and
/// returns them as a new Bytes object. The slice includes bytes from index
/// `start` up to but not including `end` (half-open interval [start, end)).
///
/// **Bounds handling:**
/// - Negative start values are clamped to 0
/// - End values beyond length are clamped to length
/// - If start >= end after clamping, returns empty Bytes
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.FromString("Hello, World!")
/// Dim slice = data.Slice(0, 5)
/// Print slice.ToString()    ' Outputs: "Hello"
/// ```
///
/// @param obj Pointer to a Bytes object. If NULL, returns empty Bytes.
/// @param start Starting index (inclusive). Clamped to [0, len].
/// @param end Ending index (exclusive). Clamped to [0, len].
///
/// @return A new Bytes object containing the sliced bytes.
///
/// @note O(n) time complexity where n is the slice length.
/// @note The original Bytes is not modified.
///
/// @see rt_bytes_copy For copying bytes between Bytes objects
/// @see rt_bytes_clone For creating a full copy
void *rt_bytes_slice(void *obj, int64_t start, int64_t end) {
    if (!obj)
        return rt_bytes_new(0);

    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.Slice: invalid Bytes object");
    if (!bytes)
        return rt_bytes_new(0); // returning-hook NULL (VDOC-177)

    // Clamp bounds
    if (start < 0)
        start = 0;
    if (end > bytes->len)
        end = bytes->len;
    if (start >= end)
        return rt_bytes_new(0);

    int64_t new_len = end - start;

    rt_bytes_impl *result = rt_bytes_alloc(new_len);
    if (!result)
        return NULL;
    if (new_len > 0)
        memcpy(result->data, bytes->data + start, (size_t)new_len);
    return result;
}

/// @brief Copies bytes from one Bytes object to another.
///
/// Copies a range of bytes from the source Bytes object to a destination
/// Bytes object. The copy is performed using memmove, so overlapping copies
/// (when src and dst are the same object) are handled correctly.
///
/// **Usage example:**
/// ```
/// Dim src = Bytes.FromString("Hello")
/// Dim dst = Bytes.New(10)
/// Bytes.Copy(dst, 0, src, 0, 5)    ' Copy "Hello" to start of dst
/// Bytes.Copy(dst, 5, src, 0, 5)    ' Copy "Hello" again
/// Print dst.ToString()             ' Outputs: "HelloHello"
/// ```
///
/// @param dst Destination Bytes object. Must not be NULL.
/// @param dst_idx Starting index in the destination.
/// @param src Source Bytes object. Must not be NULL.
/// @param src_idx Starting index in the source.
/// @param count Number of bytes to copy.
///
/// @note O(n) time complexity where n is the count.
/// @note Traps with "Bytes.Copy: null destination" if dst is NULL.
/// @note Traps with "Bytes.Copy: null source" if src is NULL.
/// @note Traps with "Bytes.Copy: count cannot be negative" if count < 0.
/// @note Traps with "Bytes.Copy: source range out of bounds" if source range
///       exceeds source bounds.
/// @note Traps with "Bytes.Copy: destination range out of bounds" if dest
///       range exceeds destination bounds.
///
/// @see rt_bytes_slice For extracting bytes as a new object
void rt_bytes_copy(void *dst, int64_t dst_idx, void *src, int64_t src_idx, int64_t count) {
    if (!dst) {
        rt_bytes_trap_invalid_operation("Bytes.Copy: null destination");
        return;
    }
    if (!src) {
        rt_bytes_trap_invalid_operation("Bytes.Copy: null source");
        return;
    }

    rt_bytes_impl *dst_bytes = rt_bytes_require(dst, "Bytes.Copy: invalid destination");
    rt_bytes_impl *src_bytes = rt_bytes_require(src, "Bytes.Copy: invalid source");
    if (!dst_bytes || !src_bytes)
        return; // returning-hook NULL after a wrong-class trap (VDOC-177)

    if (count < 0) {
        rt_bytes_trap_domain("Bytes.Copy: count cannot be negative");
        return;
    }

    if (count == 0)
        return;

    if (src_idx < 0 || count > src_bytes->len || src_idx > src_bytes->len - count) {
        rt_bytes_trap_bounds("Bytes.Copy: source range out of bounds");
        return;
    }

    if (dst_idx < 0 || count > dst_bytes->len || dst_idx > dst_bytes->len - count) {
        rt_bytes_trap_bounds("Bytes.Copy: destination range out of bounds");
        return;
    }

    memmove(dst_bytes->data + dst_idx, src_bytes->data + src_idx, (size_t)count);
}

/// @brief Converts the Bytes object to a string.
///
/// Interprets the bytes as UTF-8 encoded text and returns a string.
/// This is the inverse of rt_bytes_from_str.
///
/// **Warning:** If the bytes are not valid UTF-8, the resulting string
/// may be malformed. No UTF-8 validation is performed.
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.FromHex("48656c6c6f")
/// Print data.ToString()    ' Outputs: "Hello"
/// ```
///
/// @param obj Pointer to a Bytes object. If NULL, returns empty string.
///
/// @return A string containing the bytes interpreted as UTF-8.
///
/// @note O(n) time complexity where n is the byte count.
///
/// @see rt_bytes_from_str For the reverse operation
rt_string rt_bytes_to_str(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);

    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.ToStr: invalid Bytes object");
    if (!bytes)
        return rt_string_from_bytes("", 0); // returning-hook NULL (VDOC-177)
    return rt_string_from_bytes((const char *)bytes->data, (size_t)bytes->len);
}

/// @brief Converts the Bytes object to a hexadecimal string.
///
/// Encodes each byte as two lowercase hexadecimal characters. The resulting
/// string has exactly twice the length of the Bytes object.
///
/// **Hex encoding format:**
/// ```
/// Input:  [0xDE, 0xAD, 0xBE, 0xEF]
/// Output: "deadbeef"
/// ```
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.New(4)
/// data.Set(0, 0xDE)
/// data.Set(1, 0xAD)
/// data.Set(2, 0xBE)
/// data.Set(3, 0xEF)
/// Print data.ToHex()    ' Outputs: "deadbeef"
/// ```
///
/// @param obj Pointer to a Bytes object. If NULL, returns empty string.
///
/// @return A lowercase hexadecimal string representing the bytes.
///
/// @note O(n) time complexity where n is the byte count.
/// @note Always produces lowercase hex digits (a-f, not A-F).
///
/// @see rt_bytes_from_hex For the reverse operation
rt_string rt_bytes_to_hex(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);

    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.ToHex: invalid Bytes object");
    if (!bytes)
        return rt_string_from_bytes("", 0); // returning-hook NULL (VDOC-177)

    if (bytes->len == 0)
        return rt_string_from_bytes("", 0);

    // Use shared codec utility for hex encoding
    return rt_codec_hex_enc_bytes(bytes->data, (size_t)bytes->len);
}

/// @brief Converts the Bytes object to a Base64-encoded string.
///
/// Encodes the bytes using the standard Base64 alphabet as specified in
/// RFC 4648. The output uses standard padding ('=') and contains no line
/// breaks or whitespace.
///
/// **Base64 encoding:**
/// Every 3 bytes of input produce 4 characters of output. If the input
/// length is not a multiple of 3, padding is added:
/// - 1 byte input → 2 chars + "==" padding
/// - 2 bytes input → 3 chars + "=" padding
/// - 3 bytes input → 4 chars, no padding
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.FromString("Hello")
/// Print data.ToBase64()    ' Outputs: "SGVsbG8="
/// ```
///
/// @param obj Pointer to a Bytes object. If NULL or empty, returns empty string.
///
/// @return A Base64-encoded string representing the bytes.
///
/// @note O(n) time complexity where n is the byte count.
/// @note Uses standard Base64 alphabet (A-Za-z0-9+/) with '=' padding.
/// @note No line breaks are inserted (single continuous string).
///
/// @see rt_bytes_from_base64 For the reverse operation
rt_string rt_bytes_to_base64(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);

    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.ToBase64: invalid Bytes object");
    if (!bytes)
        return rt_string_from_bytes("", 0);
    if (bytes->len <= 0 || !bytes->data)
        return rt_string_from_bytes("", 0);

    size_t input_len = (size_t)bytes->len;
    if (input_len > SIZE_MAX - 2) {
        rt_bytes_trap_overflow("Bytes.ToBase64: output size overflow");
        return rt_string_from_bytes("", 0);
    }
    size_t groups = (input_len + 2) / 3;
    if (groups > SIZE_MAX / 4) {
        rt_bytes_trap_overflow("Bytes.ToBase64: output size overflow");
        return rt_string_from_bytes("", 0);
    }
    size_t output_len = groups * 4;
    if (output_len == SIZE_MAX) {
        rt_bytes_trap_overflow("Bytes.ToBase64: output size overflow");
        return rt_string_from_bytes("", 0);
    }

    char *out = (char *)malloc(output_len + 1);
    if (!out) {
        rt_bytes_trap_runtime("Bytes: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    size_t i = 0;
    size_t o = 0;
    while (i + 3 <= input_len) {
        uint32_t triple = ((uint32_t)bytes->data[i] << 16) | ((uint32_t)bytes->data[i + 1] << 8) |
                          bytes->data[i + 2];
        out[o++] = b64_chars[(triple >> 18) & 0x3F];
        out[o++] = b64_chars[(triple >> 12) & 0x3F];
        out[o++] = b64_chars[(triple >> 6) & 0x3F];
        out[o++] = b64_chars[triple & 0x3F];
        i += 3;
    }

    if (i < input_len) {
        uint32_t triple = (uint32_t)bytes->data[i] << 16;
        int two = 0;
        if (i + 1 < input_len) {
            triple |= (uint32_t)bytes->data[i + 1] << 8;
            two = 1;
        }

        out[o++] = b64_chars[(triple >> 18) & 0x3F];
        out[o++] = b64_chars[(triple >> 12) & 0x3F];
        if (two) {
            out[o++] = b64_chars[(triple >> 6) & 0x3F];
            out[o++] = '=';
        } else {
            out[o++] = '=';
            out[o++] = '=';
        }
    }

    out[o] = '\0';
    rt_string result = rt_string_from_bytes(out, o);
    free(out);
    return result;
}

/// @brief Creates a Bytes object by decoding a Base64-encoded string.
///
/// Decodes a Base64 string using the standard alphabet as specified in
/// RFC 4648. The input must be properly padded with '=' characters.
///
/// **Base64 decoding:**
/// Every 4 characters of input produce 3 bytes of output (less for
/// padded inputs):
/// - 4 chars with no padding → 3 bytes
/// - 4 chars with "=" padding → 2 bytes
/// - 4 chars with "==" padding → 1 byte
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.FromBase64("SGVsbG8=")
/// Print data.ToString()    ' Outputs: "Hello"
/// ```
///
/// @param b64 The Base64-encoded string to decode. If NULL or empty,
///            returns empty Bytes.
///
/// @return A new Bytes object containing the decoded bytes.
///
/// @note O(n) time complexity where n is the input string length.
/// @note Traps with "Bytes.FromBase64: base64 length must be a multiple of 4"
///       if the input length is not a multiple of 4.
/// @note Traps with "Bytes.FromBase64: invalid base64 character" if the input
///       contains characters outside the Base64 alphabet.
/// @note Traps with "Bytes.FromBase64: invalid padding" if the padding is
///       malformed (e.g., '=' in wrong position, non-zero padding bits).
///
/// @see rt_bytes_to_base64 For the reverse operation
void *rt_bytes_from_base64(rt_string b64) {
    int64_t b64_len_i64 = rt_str_len(b64);
    if (b64_len_i64 <= 0)
        return rt_bytes_new(0);

    const char *b64_str = rt_string_cstr(b64);
    if (!b64_str)
        return rt_bytes_new(0);

    size_t b64_len = (size_t)b64_len_i64;
    if (b64_len % 4 != 0)
        rt_bytes_trap_domain("Bytes.FromBase64: base64 length must be a multiple of 4");

    size_t padding = 0;
    if (b64_str[b64_len - 1] == '=') {
        padding = 1;
        if (b64_len >= 2 && b64_str[b64_len - 2] == '=')
            padding = 2;
    }

    for (size_t i = 0; i < b64_len - padding; ++i) {
        if (b64_str[i] == '=')
            rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");
    }

    size_t out_len = (b64_len / 4) * 3 - padding;
    if (out_len == 0)
        return rt_bytes_new(0);

    if (out_len > (size_t)INT64_MAX)
        rt_bytes_trap_overflow("Bytes.FromBase64: decoded data too large");

    for (size_t i = 0; i < b64_len; i += 4) {
        int v0 = b64_digit_value(b64_str[i]);
        int v1 = b64_digit_value(b64_str[i + 1]);
        int v2 = b64_digit_value(b64_str[i + 2]);
        int v3 = b64_digit_value(b64_str[i + 3]);

        if (v0 < 0 || v1 < 0) {
            if (v0 == -2 || v1 == -2)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");
            rt_bytes_trap_domain("Bytes.FromBase64: invalid base64 character");
        }

        if (v2 == -1 || v3 == -1)
            rt_bytes_trap_domain("Bytes.FromBase64: invalid base64 character");

        if (v2 == -2) {
            if (v3 != -2 || i + 4 != b64_len)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");
            if ((v1 & 0x0F) != 0)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");
            continue;
        }

        if (v3 == -2) {
            if (i + 4 != b64_len)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");
            if ((v2 & 0x03) != 0)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");
        }
    }

    rt_bytes_impl *bytes = rt_bytes_alloc((int64_t)out_len);
    if (!bytes)
        return NULL; // returning-hook NULL after an allocation trap (VDOC-177)

    size_t out_pos = 0;
    for (size_t i = 0; i < b64_len; i += 4) {
        char c0 = b64_str[i];
        char c1 = b64_str[i + 1];
        char c2 = b64_str[i + 2];
        char c3 = b64_str[i + 3];

        int v0 = b64_digit_value(c0);
        int v1 = b64_digit_value(c1);
        int v2 = b64_digit_value(c2);
        int v3 = b64_digit_value(c3);

        if (v0 < 0 || v1 < 0) {
            if (v0 == -2 || v1 == -2)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");
            rt_bytes_trap_domain("Bytes.FromBase64: invalid base64 character");
        }

        if (v2 == -1 || v3 == -1)
            rt_bytes_trap_domain("Bytes.FromBase64: invalid base64 character");

        if (v2 == -2) {
            if (v3 != -2 || i + 4 != b64_len)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");
            if ((v1 & 0x0F) != 0)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");

            uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12);
            bytes->data[out_pos++] = (uint8_t)((triple >> 16) & 0xFF);
            break;
        }

        if (v3 == -2) {
            if (i + 4 != b64_len)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");
            if ((v2 & 0x03) != 0)
                rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");

            uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6);
            bytes->data[out_pos++] = (uint8_t)((triple >> 16) & 0xFF);
            bytes->data[out_pos++] = (uint8_t)((triple >> 8) & 0xFF);
            break;
        }

        uint32_t triple =
            ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        bytes->data[out_pos++] = (uint8_t)((triple >> 16) & 0xFF);
        bytes->data[out_pos++] = (uint8_t)((triple >> 8) & 0xFF);
        bytes->data[out_pos++] = (uint8_t)(triple & 0xFF);
    }

    if (out_pos != out_len)
        rt_bytes_trap_domain("Bytes.FromBase64: invalid padding");

    return bytes;
}

/// @brief Fills all bytes in the Bytes object with a single value.
///
/// Sets every byte in the Bytes object to the specified value. This is
/// useful for initializing buffers or clearing sensitive data.
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.New(10)
/// data.Fill(0xFF)              ' Fill with 255
/// Print data.Get(0)            ' Outputs: 255
/// Print data.Get(9)            ' Outputs: 255
///
/// data.Fill(0)                 ' Clear to zeros
/// ```
///
/// @param obj Pointer to a Bytes object. If NULL, this is a no-op.
/// @param val The byte value to fill with. Only the low 8 bits (val & 0xFF)
///            are used.
///
/// @note O(n) time complexity where n is the byte count.
/// @note Uses memset for efficient bulk memory operations.
///
/// @see rt_bytes_set For setting individual bytes
void rt_bytes_fill(void *obj, int64_t val) {
    if (!obj)
        return;

    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.Fill: invalid Bytes object");
    if (bytes && bytes->len > 0 && bytes->data) { // guard returning-hook NULL (VDOC-177)
        memset(bytes->data, (uint8_t)(val & 0xFF), (size_t)bytes->len);
    }
}

/// @brief Finds the first occurrence of a byte value.
///
/// Searches the Bytes object from the beginning for the first occurrence
/// of the specified byte value and returns its index.
///
/// **Usage example:**
/// ```
/// Dim data = Bytes.FromString("Hello, World!")
/// Print data.Find(111)         ' Outputs: 4 (index of 'o')
/// Print data.Find(120)         ' Outputs: -1 (no 'x' found)
/// ```
///
/// @param obj Pointer to a Bytes object. If NULL, returns -1.
/// @param val The byte value to search for. Only the low 8 bits (val & 0xFF)
///            are used.
///
/// @return The zero-based index of the first occurrence, or -1 if the byte
///         is not found or obj is NULL.
///
/// @note O(n) time complexity in the worst case.
/// @note Linear search from the beginning; stops at first match.
int64_t rt_bytes_find(void *obj, int64_t val) {
    if (!obj)
        return -1;

    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.Find: invalid Bytes object");
    if (!bytes)
        return -1; // returning-hook NULL (VDOC-177)
    uint8_t byte = (uint8_t)(val & 0xFF);

    for (int64_t i = 0; i < bytes->len; i++) {
        if (bytes->data[i] == byte)
            return i;
    }

    return -1;
}

/// @brief Find the first occurrence of a byte value as an Option index.
/// @details This is the sentinel-free companion to @ref rt_bytes_find. A match
///          returns `SomeI64(index)`, while a missing byte or NULL Bytes handle
///          returns `None`.
/// @param obj Bytes object pointer, or NULL.
/// @param val Byte value to search for; only the low 8 bits are used.
/// @return Opaque Zanna.Option containing the first index, or None.
void *rt_bytes_find_option(void *obj, int64_t val) {
    int64_t index = rt_bytes_find(obj, val);
    return index >= 0 ? rt_option_some_i64(index) : rt_option_none();
}

/// @brief Creates a copy of the Bytes object.
///
/// Allocates a new Bytes object with the same length and contents as
/// the original. Modifications to the clone do not affect the original,
/// and vice versa.
///
/// **Usage example:**
/// ```
/// Dim original = Bytes.FromString("Hello")
/// Dim copy = original.Clone()
/// copy.Set(0, 74)              ' Change 'H' to 'J'
/// Print original.ToString()   ' Outputs: "Hello" (unchanged)
/// Print copy.ToString()       ' Outputs: "Jello"
/// ```
///
/// @param obj Pointer to a Bytes object. If NULL, returns empty Bytes.
///
/// @return A new Bytes object containing a copy of all bytes.
///
/// @note O(n) time complexity where n is the byte count.
///
/// @see rt_bytes_slice For copying a portion of the bytes
void *rt_bytes_clone(void *obj) {
    if (!obj)
        return rt_bytes_new(0);

    rt_bytes_impl *bytes = rt_bytes_require(obj, "Bytes.Clone: invalid Bytes object");
    if (!bytes)
        return rt_bytes_new(0); // returning-hook NULL (VDOC-177)
    return rt_bytes_slice(obj, 0, bytes->len);
}

//=============================================================================
// Internal Utilities (declared in rt_internal.h)
//=============================================================================

/// @brief Extract raw bytes from a Bytes object into a newly allocated buffer.
///
/// This utility function extracts the contents of a Bytes object into a
/// freshly allocated raw buffer. It's used internally by cryptographic and
/// encoding routines that need to work with raw byte arrays.
///
/// @param bytes Bytes object pointer. May be NULL.
/// @param out_len Output parameter that receives the length of the data.
///
/// @return Pointer to a newly allocated buffer containing the bytes data,
///         or NULL if the input is NULL or empty. Caller must free() the
///         returned buffer when done.
///
/// @note Traps on allocation failure.
uint8_t *rt_bytes_extract_raw(void *bytes, size_t *out_len) {
    if (!out_len) {
        rt_bytes_trap_invalid_operation("Bytes.ExtractRaw: null length output");
        return NULL;
    }
    if (!bytes) {
        *out_len = 0;
        return NULL;
    }

    rt_bytes_impl *impl = rt_bytes_require(bytes, "Bytes.ExtractRaw: invalid Bytes object");
    if (!impl) {
        *out_len = 0;
        return NULL;
    }
    *out_len = (size_t)impl->len;

    if (impl->len == 0)
        return NULL;

    uint8_t *data = (uint8_t *)malloc((size_t)impl->len);
    if (!data) {
        rt_bytes_trap_runtime("Bytes: memory allocation failed");
        return NULL;
    }

    memcpy(data, impl->data, (size_t)impl->len);
    return data;
}

//=============================================================================
// Binary Integer Read/Write Operations
//=============================================================================

/// @brief Validate offset and size for binary read/write.
/// @param b Bytes implementation whose range is checked.
/// @param offset Starting byte offset.
/// @param size Number of bytes required.
/// @return 1 if `[offset, offset + size)` is within the payload; otherwise 0
///         after reporting an invalid-object or bounds trap.
static inline int bytes_check_bounds(rt_bytes_impl *b, int64_t offset, int64_t size) {
    if (!b) {
        rt_bytes_trap_invalid_operation("Bytes: null object");
        return 0;
    }
    if (offset < 0 || size < 0 || size > b->len || offset > b->len - size) {
        rt_bytes_trap_bounds("Bytes: binary read/write out of bounds");
        return 0;
    }
    return 1;
}

// ===========================================================================
// Endian-aware multi-byte read/write — the standard byte-order pairs for
// 16/32/64 bit signed integers. Each LE/BE pair handles its own bounds
// check and traps on overflow. Read functions return values widened to
// int64; write functions accept int64 and truncate.
// ===========================================================================

/// @brief Read a little-endian int16 at `offset` (sign-extended to int64).
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the two-byte field.
/// @return Sign-extended value, or 0 after a failed validation trap.
int64_t rt_bytes_read_i16le(void *obj, int64_t offset) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.ReadI16LE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 2))
        return 0;
    uint8_t *d = b->data + offset;
    uint16_t raw = (uint16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
    return (int64_t)(int16_t)raw;
}

/// @brief Read a big-endian int16 at `offset` (sign-extended).
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the two-byte field.
/// @return Sign-extended value, or 0 after a failed validation trap.
int64_t rt_bytes_read_i16be(void *obj, int64_t offset) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.ReadI16BE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 2))
        return 0;
    uint8_t *d = b->data + offset;
    uint16_t raw = (uint16_t)(((uint16_t)d[0] << 8) | (uint16_t)d[1]);
    return (int64_t)(int16_t)raw;
}

/// @brief Read a little-endian int32 at `offset` (sign-extended).
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the four-byte field.
/// @return Sign-extended value, or 0 after a failed validation trap.
int64_t rt_bytes_read_i32le(void *obj, int64_t offset) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.ReadI32LE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 4))
        return 0;
    uint8_t *d = b->data + offset;
    uint32_t raw = (uint32_t)((uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) |
                              ((uint32_t)d[3] << 24));
    return (int64_t)(int32_t)raw;
}

/// @brief Read a big-endian int32 at `offset` (sign-extended).
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the four-byte field.
/// @return Sign-extended value, or 0 after a failed validation trap.
int64_t rt_bytes_read_i32be(void *obj, int64_t offset) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.ReadI32BE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 4))
        return 0;
    uint8_t *d = b->data + offset;
    uint32_t raw = (uint32_t)(((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                              ((uint32_t)d[2] << 8) | (uint32_t)d[3]);
    return (int64_t)(int32_t)raw;
}

/// @brief Read a little-endian int64 at `offset`.
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the eight-byte field.
/// @return Decoded signed value, or 0 after a failed validation trap.
int64_t rt_bytes_read_i64le(void *obj, int64_t offset) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.ReadI64LE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 8))
        return 0;
    uint8_t *d = b->data + offset;
    return (int64_t)((uint64_t)d[0] | ((uint64_t)d[1] << 8) | ((uint64_t)d[2] << 16) |
                     ((uint64_t)d[3] << 24) | ((uint64_t)d[4] << 32) | ((uint64_t)d[5] << 40) |
                     ((uint64_t)d[6] << 48) | ((uint64_t)d[7] << 56));
}

/// @brief Read a big-endian int64 at `offset`.
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the eight-byte field.
/// @return Decoded signed value, or 0 after a failed validation trap.
int64_t rt_bytes_read_i64be(void *obj, int64_t offset) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.ReadI64BE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 8))
        return 0;
    uint8_t *d = b->data + offset;
    return (int64_t)(((uint64_t)d[0] << 56) | ((uint64_t)d[1] << 48) | ((uint64_t)d[2] << 40) |
                     ((uint64_t)d[3] << 32) | ((uint64_t)d[4] << 24) | ((uint64_t)d[5] << 16) |
                     ((uint64_t)d[6] << 8) | (uint64_t)d[7]);
}

/// @brief Write `value` (truncated to int16) at `offset` in little-endian byte order.
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the two-byte field.
/// @param value Value whose low 16 bits are stored.
void rt_bytes_write_i16le(void *obj, int64_t offset, int64_t value) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.WriteI16LE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 2))
        return;
    uint8_t *d = b->data + offset;
    uint16_t raw = (uint16_t)value;
    d[0] = (uint8_t)(raw & 0xFFu);
    d[1] = (uint8_t)((raw >> 8) & 0xFFu);
}

/// @brief Write `value` (truncated to int16) at `offset` in big-endian byte order.
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the two-byte field.
/// @param value Value whose low 16 bits are stored.
void rt_bytes_write_i16be(void *obj, int64_t offset, int64_t value) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.WriteI16BE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 2))
        return;
    uint8_t *d = b->data + offset;
    uint16_t raw = (uint16_t)value;
    d[0] = (uint8_t)((raw >> 8) & 0xFFu);
    d[1] = (uint8_t)(raw & 0xFFu);
}

/// @brief Write `value` (truncated to int32) at `offset` in little-endian byte order.
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the four-byte field.
/// @param value Value whose low 32 bits are stored.
void rt_bytes_write_i32le(void *obj, int64_t offset, int64_t value) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.WriteI32LE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 4))
        return;
    uint8_t *d = b->data + offset;
    uint32_t raw = (uint32_t)value;
    d[0] = (uint8_t)(raw & 0xFFu);
    d[1] = (uint8_t)((raw >> 8) & 0xFFu);
    d[2] = (uint8_t)((raw >> 16) & 0xFFu);
    d[3] = (uint8_t)((raw >> 24) & 0xFFu);
}

/// @brief Write `value` (truncated to int32) at `offset` in big-endian byte order.
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the four-byte field.
/// @param value Value whose low 32 bits are stored.
void rt_bytes_write_i32be(void *obj, int64_t offset, int64_t value) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.WriteI32BE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 4))
        return;
    uint8_t *d = b->data + offset;
    uint32_t raw = (uint32_t)value;
    d[0] = (uint8_t)((raw >> 24) & 0xFFu);
    d[1] = (uint8_t)((raw >> 16) & 0xFFu);
    d[2] = (uint8_t)((raw >> 8) & 0xFFu);
    d[3] = (uint8_t)(raw & 0xFFu);
}

/// @brief Write a full 64-bit `value` at `offset` in little-endian byte order.
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the eight-byte field.
/// @param value Signed value stored using its complete 64-bit bit pattern.
void rt_bytes_write_i64le(void *obj, int64_t offset, int64_t value) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.WriteI64LE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 8))
        return;
    uint8_t *d = b->data + offset;
    uint64_t raw = (uint64_t)value;
    d[0] = (uint8_t)(raw & 0xFFu);
    d[1] = (uint8_t)((raw >> 8) & 0xFFu);
    d[2] = (uint8_t)((raw >> 16) & 0xFFu);
    d[3] = (uint8_t)((raw >> 24) & 0xFFu);
    d[4] = (uint8_t)((raw >> 32) & 0xFFu);
    d[5] = (uint8_t)((raw >> 40) & 0xFFu);
    d[6] = (uint8_t)((raw >> 48) & 0xFFu);
    d[7] = (uint8_t)((raw >> 56) & 0xFFu);
}

/// @brief Write a full 64-bit `value` at `offset` in big-endian byte order.
/// @param obj Non-null Bytes handle.
/// @param offset Starting byte offset of the eight-byte field.
/// @param value Signed value stored using its complete 64-bit bit pattern.
void rt_bytes_write_i64be(void *obj, int64_t offset, int64_t value) {
    rt_bytes_impl *b = rt_bytes_require(obj, "Bytes.WriteI64BE: invalid Bytes object");
    if (!bytes_check_bounds(b, offset, 8))
        return;
    uint8_t *d = b->data + offset;
    uint64_t raw = (uint64_t)value;
    d[0] = (uint8_t)((raw >> 56) & 0xFFu);
    d[1] = (uint8_t)((raw >> 48) & 0xFFu);
    d[2] = (uint8_t)((raw >> 40) & 0xFFu);
    d[3] = (uint8_t)((raw >> 32) & 0xFFu);
    d[4] = (uint8_t)((raw >> 24) & 0xFFu);
    d[5] = (uint8_t)((raw >> 16) & 0xFFu);
    d[6] = (uint8_t)((raw >> 8) & 0xFFu);
    d[7] = (uint8_t)(raw & 0xFFu);
}

//=============================================================================
// Internal Utilities (declared in rt_internal.h)
//=============================================================================

/// @brief Create a Bytes object from raw data.
///
/// This utility function creates a new Bytes object and initializes it
/// with a copy of the provided raw data. It's used internally by
/// cryptographic routines that produce raw byte arrays.
///
/// @param data Pointer to raw data buffer. May be NULL if len is 0.
/// @param len Length of the data in bytes.
///
/// @return New Bytes object containing a copy of the data, or NULL after
///         reporting an invalid pointer, overflow, or allocation trap.
void *rt_bytes_from_raw(const uint8_t *data, size_t len) {
    if (len > (size_t)INT64_MAX)
        rt_bytes_trap_overflow("Bytes.FromRaw: length exceeds maximum Bytes size");
    if (len > 0 && !data)
        rt_bytes_trap_invalid_operation("Bytes.FromRaw: null data with non-zero length");
    rt_bytes_impl *bytes = rt_bytes_alloc((int64_t)len);
    if (!bytes)
        return NULL; // returning-hook NULL after an allocation trap (VDOC-177)
    if (len > 0 && data)
        memcpy(bytes->data, data, len);
    return bytes;
}
