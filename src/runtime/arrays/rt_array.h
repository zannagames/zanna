//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares the C ABI for reference-counted runtime arrays of i32 values.
/// @details Public handles address contiguous elements after an internal heap
///          header. Checked accessors trap on invalid indices, fast and inline
///          accessors require dominating compiler proof, and resize accepts a
///          pointer-to-handle because successful growth or copy-on-write may
///          replace the payload address.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/arrays/rt_array.h
// Purpose: Dynamic array API for 32-bit integers (i32) backing BASIC DIM/REDIM statements,
// providing allocation, reference counting, bounds-checked access, and resize operations.
//
// Key invariants:
//   - Payload pointers are preceded by an rt_heap_hdr_t header at a negative offset.
//   - length <= capacity at all times; indexed access traps on out-of-bounds.
//   - New arrays start with refcount 1.
//   - Resize may reallocate and rebind the payload pointer.
//
// Ownership/Lifetime:
//   - Reference-counted via rt_arr_i32_retain/release.
//   - The caller owns the initial reference from rt_arr_i32_new.
//   - Resize transfers ownership of the old allocation.
//
// Links: src/runtime/arrays/rt_array.c (implementation), src/runtime/core/rt_heap.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_heap.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Helper returning the heap header associated with @p payload.
/// @param payload Array payload pointer (may be NULL).
/// @return Heap header describing the allocation, or NULL for NULL payloads.
rt_heap_hdr_t *rt_arr_i32_hdr(const int32_t *payload);

#if defined(__cplusplus)
#define RT_ARR_NORETURN [[noreturn]]
#elif defined(_MSC_VER)
#define RT_ARR_NORETURN __declspec(noreturn)
#else
#define RT_ARR_NORETURN __attribute__((noreturn))
#endif

/// @brief Allocate a new dynamic array of 32-bit integers.
/// @param len Number of elements to allocate.
/// @return Array payload pointer or NULL on allocation failure.
int32_t *rt_arr_i32_new(size_t len);

/// @brief Increment the reference count for @p arr.
/// @param arr Array payload pointer (may be NULL).
void rt_arr_i32_retain(int32_t *arr);

/// @brief Decrement the reference count for @p arr and free on zero.
/// @param arr Array payload pointer (may be NULL).
void rt_arr_i32_release(int32_t *arr);

/// @brief Query the current logical length of an array.
/// @param arr Array payload pointer returned by rt_arr_i32_new.
/// @return Number of accessible elements; 0 when @p arr is NULL.
size_t rt_arr_i32_len(int32_t *arr);

/// @brief Query the current capacity in elements.
/// @param arr Array payload pointer returned by rt_arr_i32_new.
/// @return Capacity in elements; 0 when @p arr is NULL.
size_t rt_arr_i32_cap(int32_t *arr);

/// @brief Read element at index @p idx with bounds checking.
/// @param arr Array payload pointer; must be non-null.
/// @param idx Zero-based index within the array length.
/// @return Stored value at @p idx.
/// @note Traps on out-of-bounds access.
int32_t rt_arr_i32_get(int32_t *arr, size_t idx);

/// @brief Write @p value to index @p idx with bounds checking.
/// @param arr Array payload pointer; must be non-null.
/// @param idx Zero-based index within the array length.
/// @param value Value to store.
/// @note Traps on out-of-bounds access.
void rt_arr_i32_set(int32_t *arr, size_t idx, int32_t value);

/// @brief Read element at index @p idx after the compiler proved bounds.
/// @param arr Array payload pointer; must be non-null.
/// @param idx Zero-based index; caller guarantees idx < length.
/// @return Stored value at @p idx.
/// @warning No bounds checking. Only generated after an explicit dominating check.
int32_t rt_arr_i32_get_fast(int32_t *arr, size_t idx);

/// @brief Write @p value to index @p idx after the compiler proved bounds.
/// @param arr Array payload pointer; must be non-null.
/// @param idx Zero-based index; caller guarantees idx < length.
/// @param value Value to store.
/// @warning No bounds checking. Only generated after an explicit dominating check.
void rt_arr_i32_set_fast(int32_t *arr, size_t idx, int32_t value);

/// @brief Read element at index @p idx WITHOUT bounds checking.
/// @param arr Array payload pointer; must be non-null and in range.
/// @param idx Zero-based index; caller guarantees idx < length.
/// @return Stored value at @p idx.
/// @warning No bounds checking! Use only when compiler has verified safety.
/// @note This is an inline function for maximum performance in tight loops.
static inline int32_t rt_arr_i32_get_unchecked(const int32_t *arr, size_t idx) {
    return arr[idx];
}

/// @brief Write @p value to index @p idx WITHOUT bounds checking.
/// @param arr Array payload pointer; must be non-null and in range.
/// @param idx Zero-based index; caller guarantees idx < length.
/// @param value Value to store.
/// @warning No bounds checking! Use only when compiler has verified safety.
/// @note This is an inline function for maximum performance in tight loops.
static inline void rt_arr_i32_set_unchecked(int32_t *arr, size_t idx, int32_t value) {
    arr[idx] = value;
}

/// @brief Resize an array under copy-on-write ownership semantics.
/// @details Shared storage is copied before mutation; unique storage is updated
///          in place when capacity suffices or may be reallocated when growing.
/// @param[in,out] a_inout Address of the array payload pointer (may point to NULL).
/// @param new_len Requested logical length.
/// @return 0 on success, -1 on allocation failure; pointer may be rebound on success.
int rt_arr_i32_resize(int32_t **a_inout, size_t new_len);

/// @brief Copy @p count elements between array payloads, trapping on invalid buffers.
/// @param dst Destination payload pointer; must be non-null when @p count > 0.
/// @param src Source payload pointer; must be non-null when @p count > 0.
/// @param count Number of elements to copy.
void rt_arr_i32_copy_payload(int32_t *dst, const int32_t *src, size_t count);

/// @brief Abort execution due to an out-of-bounds access.
/// @param idx Failing index.
/// @param len Array length observed by the caller.
RT_ARR_NORETURN void rt_arr_oob_panic(size_t idx, size_t len);

#ifdef __cplusplus
}
#endif

#undef RT_ARR_NORETURN
