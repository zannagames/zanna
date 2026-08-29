//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/rt_internal.h
// Purpose: Internal runtime helpers for synchronization, buffer growth,
// allocation hooks, hex utilities, and array macros. This header is
// implementation-only and must never be included by IL-generated or user code.
//
// Key invariants:
//   - Implementation-only: must not be included from public-facing headers.
//   - Successful input growth doubles capacity without overflowing size_t.
//   - Allocation hooks are process-global; only one hook may be active at a time.
//   - Native process-environment access is serialized while borrowed bytes are copied.
//   - Generated array helpers preserve heap-header kind and element-tag checks.
//
// Ownership/Lifetime:
//   - Input buffers remain caller-owned; successful realloc may replace and
//     release the old allocation while updating the caller's pointer.
//   - Allocation hooks are borrowed global callbacks and must be removed
//     before their code or context becomes unavailable.
//
// Links: src/runtime/core/rt_heap.h (heap header),
//        src/runtime/core/rt_string.h (managed string representation),
//        docs/adr/0304-bounded-process-output-and-environment-snapshots.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_internal.h
 * @brief Declares implementation-only runtime allocation and buffer helpers.
 * @details Internal modules use this header for checked scratch-buffer growth,
 *          allocation interception, raw Bytes conversion, shared hexadecimal
 *          tables, and generated array helper macros. It is not part of the
 *          compiler-emitted or user-facing runtime interface.
 */

#pragma once

#include "rt.hpp"
#include "rt_heap.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Outcomes from attempting to double an input scratch buffer.
typedef enum {
    RT_INPUT_GROW_OK = 0,           ///< Buffer and capacity were updated.
    RT_INPUT_GROW_ALLOC_FAILED = 1, ///< Arguments were invalid or `realloc` failed.
    RT_INPUT_GROW_OVERFLOW = 2      ///< Doubling the current capacity would overflow.
} rt_input_grow_result_t;

/// @brief Double an owned input buffer's capacity in place.
/// @details Rejects null pointer arguments and detects multiplication overflow
///          before calling `realloc`. Failure leaves the caller's pointer and
///          capacity unchanged.
/// @param buf Address of a non-NULL owned allocation pointer, updated on success.
/// @param cap Address of its current capacity, doubled on success.
/// @return @ref RT_INPUT_GROW_OK on success, @ref RT_INPUT_GROW_OVERFLOW when
///         doubling is unrepresentable, or @ref RT_INPUT_GROW_ALLOC_FAILED for
///         invalid arguments/allocation failure.
rt_input_grow_result_t rt_input_try_grow(char **buf, size_t *cap);

/// @brief Allocation-hook signature used to intercept @ref rt_alloc.
/// @param bytes Original signed byte request, before default validation.
/// @param next Default allocator callback the hook may invoke.
/// @return Free-compatible allocation or @c NULL according to hook policy.
typedef void *(*rt_alloc_hook_fn)(int64_t bytes, void *(*next)(int64_t bytes));

/// @brief Install or remove the allocation hook used for testing.
/// @details When non-null, @p hook receives the requested byte count and a
///          pointer to the default allocator implementation.  Passing
///          @c NULL restores the default behaviour.
/// @param hook Replacement hook or @c NULL to disable overrides.
void rt_set_alloc_hook(rt_alloc_hook_fn hook);

/// @brief Acquire the process-global lock for native environment storage.
/// @details Callers must copy borrowed `getenv`/`environ` bytes before release
///          and must not hold this lock across blocking process launch.
void rt_env_lock_process_environment(void);

/// @brief Release the process-global native-environment lock.
void rt_env_unlock_process_environment(void);

//=========================================================================
// Bytes Extraction Utilities
//=========================================================================

/// @brief Extract raw bytes from a Bytes object into a newly allocated buffer.
/// @details Validates the opaque object, reports its length, and copies nonempty
///          contents into a `malloc` allocation. Invalid objects, null length
///          output, and allocation failure trap.
/// @param bytes Borrowed Bytes object; @c NULL is treated as empty.
/// @param out_len Non-NULL output receiving the byte count; set to zero for
///        null or a returning invalid-object trap.
/// @return Caller-owned `malloc` buffer to release with `free`, or @c NULL for
///         null/empty input and trap fallback paths.
uint8_t *rt_bytes_extract_raw(void *bytes, size_t *out_len);

/// @brief Create a Bytes object from raw data.
/// @details Validates that @p len fits the runtime's signed Bytes length,
///          allocates an opaque object, and copies the source bytes.
/// @param data Borrowed raw buffer; may be @c NULL only when @p len is zero.
/// @param len Number of bytes to copy, at most `INT64_MAX`.
/// @return Caller-owned Bytes object, or @c NULL after allocation failure.
void *rt_bytes_from_raw(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

//=============================================================================
// Shared Hex Encoding/Decoding Utilities
//=============================================================================

/// @brief Hexadecimal character lookup table for byte-to-hex encoding (lowercase).
/// @details Maps nibble values (0-15) to lowercase hexadecimal characters.
///          Used by hex encoding functions throughout the runtime.
static const char rt_hex_chars[] = "0123456789abcdef";

/// @brief Hexadecimal character lookup table for byte-to-hex encoding (uppercase).
/// @details Maps nibble values (0-15) to uppercase hexadecimal characters.
///          Used for URL encoding and other contexts requiring uppercase hex.
static const char rt_hex_chars_upper[] = "0123456789ABCDEF";

/// @brief Converts a hexadecimal character to its numeric value.
/// @param c The character to convert ('0'-'9', 'a'-'f', or 'A'-'F').
/// @return The numeric value 0-15, or -1 if the character is not valid hex.
static inline int rt_hex_digit_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

//=============================================================================
// Array Implementation Macros
//=============================================================================
//
// These macros reduce boilerplate for array type implementations.
// Each array type (i32, i64, f64, str, obj) has similar header retrieval,
// validation, and bounds checking patterns. These macros generate type-safe
// functions while avoiding code duplication.
//

/// @brief Generate an array header retrieval function.
/// @details The generated `static inline` helper accepts a typed payload,
///          returns @ref rt_heap_hdr for non-null input, and performs no
///          independent validity or element-kind check.
/// @param fn_name Name of the generated function (e.g., rt_arr_i32_hdr).
/// @param elem_type C type of array elements (e.g., int32_t).
/// @par Generated parameters
/// `payload` is a borrowed typed array payload and may be @c NULL.
/// @par Generated return
/// Borrowed preceding heap header for non-null input, otherwise @c NULL.
/// @code
///   RT_ARR_DEFINE_HDR_FN(rt_arr_i32_hdr, int32_t)
/// @endcode
#define RT_ARR_DEFINE_HDR_FN(fn_name, elem_type)                                                   \
    static inline rt_heap_hdr_t *fn_name(const elem_type *payload) {                               \
        return payload ? rt_heap_hdr((void *)(uintptr_t)payload) : NULL;                           \
    }

/// @brief Generate an array header assertion function (static).
/// @details The generated helper traps when its header is null or when magic,
///          heap kind, or element kind differs from the expected array layout.
/// @param fn_name Name of the generated function (e.g., rt_arr_i32_assert_header).
/// @param expected_elem_kind Expected RT_ELEM_* constant (e.g., RT_ELEM_I32).
/// @par Generated parameters
/// `hdr` is the borrowed candidate heap header to validate.
/// @code
///   RT_ARR_DEFINE_ASSERT_HEADER_FN(rt_arr_i32_assert_header, RT_ELEM_I32)
/// @endcode
#define RT_ARR_DEFINE_ASSERT_HEADER_FN(fn_name, expected_elem_kind)                                \
    static void fn_name(rt_heap_hdr_t *hdr) {                                                      \
        if (!hdr) {                                                                                \
            rt_trap(#fn_name ": null array header");                                               \
            return;                                                                                \
        }                                                                                          \
        if (hdr->magic != RT_MAGIC || hdr->kind != RT_HEAP_ARRAY ||                                \
            hdr->elem_kind != (expected_elem_kind)) {                                              \
            rt_trap(#fn_name ": invalid array header");                                            \
        }                                                                                          \
    }

/// @brief Generate a payload byte size calculation function (static).
/// @details The generated helper multiplies capacity by element size after
///          applying the conservative heap-size overflow guard. Zero capacity
///          and overflow both return zero, so callers distinguish them using
///          the original capacity.
/// @param fn_name Name of the generated function (e.g., rt_arr_i32_payload_bytes).
/// @param elem_type C type of array elements (e.g., int32_t).
/// @par Generated parameters
/// `cap` is the requested number of elements.
/// @par Generated return
/// Payload byte count, or zero for zero capacity/arithmetic overflow.
/// @code
///   RT_ARR_DEFINE_PAYLOAD_BYTES_FN(rt_arr_i32_payload_bytes, int32_t)
/// @endcode
#define RT_ARR_DEFINE_PAYLOAD_BYTES_FN(fn_name, elem_type)                                         \
    static size_t fn_name(size_t cap) {                                                            \
        if (cap == 0)                                                                              \
            return 0;                                                                              \
        if (cap > (SIZE_MAX - sizeof(rt_heap_hdr_t)) / sizeof(elem_type))                          \
            return 0;                                                                              \
        return cap * sizeof(elem_type);                                                            \
    }

/// @brief Generate an in-place array grow function.
/// @details The generated helper requests an exact new capacity/length from
///          @ref rt_heap_realloc and updates both header and typed payload
///          outputs only after a usable resized payload/header is obtained.
/// @param fn_name Name of the generated function (e.g., rt_arr_i32_grow_in_place).
/// @param elem_type C type of array elements (e.g., int32_t).
/// @param payload_bytes_fn Name of the payload bytes function (e.g., rt_arr_i32_payload_bytes).
/// @par Generated parameters
/// `hdr_inout` receives the resized header, `payload_inout` owns the current
/// payload pointer and receives its replacement, and `new_len` is the exact
/// requested length/capacity.
/// @par Generated return
/// Zero on success or -1 for overflow/reallocation/header failure.
/// @code
///   RT_ARR_DEFINE_GROW_IN_PLACE_FN(rt_arr_i32_grow_in_place, int32_t, rt_arr_i32_payload_bytes)
/// @endcode
#define RT_ARR_DEFINE_GROW_IN_PLACE_FN(fn_name, elem_type, payload_bytes_fn)                       \
    static int fn_name(rt_heap_hdr_t **hdr_inout, elem_type **payload_inout, size_t new_len) {     \
        size_t new_cap = new_len;                                                                  \
        size_t payload_bytes = payload_bytes_fn(new_cap);                                          \
        if (new_cap > 0 && payload_bytes == 0)                                                     \
            return -1;                                                                             \
        elem_type *payload =                                                                       \
            (elem_type *)rt_heap_realloc(*payload_inout, sizeof(elem_type), new_len, new_cap);     \
        if (!payload)                                                                              \
            return -1;                                                                             \
        rt_heap_hdr_t *resized = rt_heap_hdr((void *)payload);                                     \
        if (!resized)                                                                              \
            return -1;                                                                             \
        *hdr_inout = resized;                                                                      \
        *payload_inout = payload;                                                                  \
        return 0;                                                                                  \
    }

/// @brief Generate an array resize function with copy-on-write semantics.
/// @details The generated public helper allocates when the input is null,
///          validates existing array metadata, clones shared arrays, resizes a
///          unique allocation in place when capacity permits, and otherwise
///          delegates to the generated grow helper. Newly exposed in-capacity
///          elements are zero-initialized.
/// @param fn_name Name of the generated function (e.g., rt_arr_i32_resize).
/// @param elem_type C type of array elements (e.g., int32_t).
/// @param hdr_fn Header retrieval function (e.g., rt_arr_i32_hdr).
/// @param assert_header_fn Header assertion function (e.g., rt_arr_i32_assert_header).
/// @param new_fn Allocation function (e.g., rt_arr_i32_new).
/// @param copy_fn Payload copy function (e.g., rt_arr_i32_copy_payload).
/// @param release_fn Release function (e.g., rt_arr_i32_release).
/// @param grow_fn In-place grow function (e.g., rt_arr_i32_grow_in_place).
/// @par Generated parameters
/// `a_inout` owns one array reference and receives a replacement when allocation,
/// copy-on-write, or reallocation changes it; `new_len` is the requested length.
/// @par Generated return
/// Zero for success/no change, or -1 for null output, allocation, validation,
/// overflow, or growth failure.
#define RT_ARR_DEFINE_RESIZE_FN(                                                                   \
    fn_name, elem_type, hdr_fn, assert_header_fn, new_fn, copy_fn, release_fn, grow_fn)            \
    int fn_name(elem_type **a_inout, size_t new_len) {                                             \
        if (!a_inout)                                                                              \
            return -1;                                                                             \
        elem_type *arr = *a_inout;                                                                 \
        if (!arr) {                                                                                \
            elem_type *fresh = new_fn(new_len);                                                    \
            if (!fresh)                                                                            \
                return -1;                                                                         \
            *a_inout = fresh;                                                                      \
            return 0;                                                                              \
        }                                                                                          \
        rt_heap_hdr_t *hdr = hdr_fn(arr);                                                          \
        assert_header_fn(hdr);                                                                     \
        if (!hdr || hdr->magic != RT_MAGIC || hdr->kind != RT_HEAP_ARRAY)                          \
            return -1;                                                                             \
        size_t old_len = hdr->len;                                                                 \
        size_t cap = hdr->cap;                                                                     \
        if (new_len == old_len)                                                                    \
            return 0;                                                                              \
        if (__atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE) > 1) {                                 \
            elem_type *fresh = new_fn(new_len);                                                    \
            if (!fresh)                                                                            \
                return -1;                                                                         \
            size_t copy_len = old_len < new_len ? old_len : new_len;                               \
            copy_fn(fresh, arr, copy_len);                                                         \
            release_fn(arr);                                                                       \
            *a_inout = fresh;                                                                      \
            return 0;                                                                              \
        }                                                                                          \
        if (new_len <= cap) {                                                                      \
            if (new_len > old_len)                                                                 \
                memset(arr + old_len, 0, (new_len - old_len) * sizeof(elem_type));                 \
            rt_heap_set_len(arr, new_len);                                                         \
            return 0;                                                                              \
        }                                                                                          \
        rt_heap_hdr_t *hdr_mut = hdr;                                                              \
        elem_type *payload = arr;                                                                  \
        if (grow_fn(&hdr_mut, &payload, new_len) != 0)                                             \
            return -1;                                                                             \
        *a_inout = payload;                                                                        \
        return 0;                                                                                  \
    }

//=============================================================================
// String Implementation
//=============================================================================

/// @brief Internal storage record behind the public `rt_string` handle.
/// @details A string uses exactly one backing mode: literal/static bytes,
///          embedded small-string bytes indicated by @ref RT_SSO_SENTINEL, or a
///          separate runtime heap payload tracked by @ref heap.
struct rt_string_impl {
    /// Magic value used to validate a candidate string handle.
    uint64_t magic;

    /// Borrowed literal bytes, embedded bytes, or managed heap payload.
    char *data;

    /// Backing heap header, @ref RT_SSO_SENTINEL for embedded storage, or
    /// @c NULL for literal/static bytes.
    rt_heap_hdr_t *heap;

    /// Byte length used by the literal/static representation.
    size_t literal_len;

    /// Reference count used by the literal/static representation.
    size_t literal_refs;
};

/// @brief Magic constant identifying a live internal string record.
#define RT_STRING_MAGIC 0x5354524D41474943ULL /* "STRMAGIC" */

/// @brief Maximum string length for embedded (SSO) allocation.
/// @details Strings up to this length are allocated with their data embedded
///          immediately after the rt_string_impl struct, eliminating one heap
///          allocation. The value 32 is chosen to balance allocation savings
///          against memory overhead for the combined allocation.
#define RT_SSO_MAX_LEN 32

/// @brief Sentinel value for heap pointer indicating embedded string data.
/// @details When heap equals this value, the string data is embedded directly
///          after the rt_string_impl struct in the same allocation. The data
///          pointer points to this embedded storage.
#define RT_SSO_SENTINEL ((rt_heap_hdr_t *)(uintptr_t)0xDEADBEEFCAFEBABEULL)
