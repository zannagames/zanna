//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares the C ABI for reference-counted runtime arrays of i64 values.
/// @details Handles point to contiguous payload elements after an internal heap
///          header. Checked accessors require the i64 element kind, common
///          lifetime queries accept the shared 64-bit numeric ABI, and
///          copy-on-write resize may replace the caller's payload pointer.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/arrays/rt_array_i64.h
// Purpose: Dynamic array API for 64-bit integers (int64_t) supporting BASIC LONG typed collections,
// mirroring the i32 array interface with allocation, refcounting, bounds-checked access, and
// resize.
//
// Key invariants:
//   - Payload pointers are preceded by an rt_heap_hdr_t header at a negative offset.
//   - length <= capacity at all times; indexed access traps on out-of-bounds.
//   - New arrays start with refcount 1.
//   - Resize may reallocate and rebind the payload pointer.
//
// Ownership/Lifetime:
//   - Reference-counted via rt_arr_i64_retain/release.
//   - The caller owns the initial reference from rt_arr_i64_new.
//   - Resize transfers ownership of the old allocation.
//
// Links: src/runtime/arrays/rt_array_i64.c (implementation), src/runtime/core/rt_heap.h
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
/// @param payload Array payload pointer; may be `NULL`.
/// @return Header describing the allocation, or `NULL`.
rt_heap_hdr_t *rt_arr_i64_hdr(const int64_t *payload);

/// @brief Allocate a new dynamic array of 64-bit integers.
/// @param len Number of elements to allocate.
/// @return Array payload pointer or NULL on allocation failure.
int64_t *rt_arr_i64_new(size_t len);

/// @brief Increment the reference count for @p arr.
/// @param arr I64 or ABI-compatible f64 payload; `NULL` is ignored.
void rt_arr_i64_retain(int64_t *arr);

/// @brief Decrement the reference count for @p arr and free on zero.
/// @param arr I64 or ABI-compatible f64 payload; `NULL` is ignored.
void rt_arr_i64_release(int64_t *arr);

/// @brief Query the current logical length of an array.
/// @param arr I64 or ABI-compatible f64 payload; may be `NULL`.
/// @return Number of accessible elements; 0 when @p arr is NULL.
size_t rt_arr_i64_len(int64_t *arr);

/// @brief Query the current capacity in elements.
/// @param arr I64 or ABI-compatible f64 payload; may be `NULL`.
/// @return Reserved element slots; zero for `NULL`.
size_t rt_arr_i64_cap(int64_t *arr);

/// @brief Read element at index @p idx with bounds checking.
/// @param arr Non-null i64 array payload.
/// @param idx Zero-based index within logical length.
/// @return Stored value.
/// @note Traps when the handle, element kind, or index is invalid.
int64_t rt_arr_i64_get(int64_t *arr, size_t idx);

/// @brief Write @p value to index @p idx with bounds checking.
/// @param arr Non-null i64 array payload.
/// @param idx Zero-based index within logical length.
/// @param value Value to store.
/// @note Traps when the handle, element kind, or index is invalid.
void rt_arr_i64_set(int64_t *arr, size_t idx, int64_t value);

/// @brief Read element at index @p idx after the compiler proved bounds.
/// @param arr Non-null i64 array payload.
/// @param idx Compiler-proven in-bounds index.
/// @return Stored value.
/// @warning No bounds checking. Use only when a dominating check guarantees safety.
int64_t rt_arr_i64_get_fast(int64_t *arr, size_t idx);

/// @brief Write @p value to index @p idx after the compiler proved bounds.
/// @param arr Non-null i64 array payload.
/// @param idx Compiler-proven in-bounds index.
/// @param value Value to store.
/// @warning No bounds checking. Use only when a dominating check guarantees safety.
void rt_arr_i64_set_fast(int64_t *arr, size_t idx, int64_t value);

/// @brief Read element at index @p idx WITHOUT bounds checking.
/// @param arr Non-null i64 array payload.
/// @param idx Compiler-proven in-bounds index.
/// @return Stored value.
/// @warning No bounds checking! Use only when compiler has verified safety.
static inline int64_t rt_arr_i64_get_unchecked(int64_t *arr, size_t idx) {
    return arr[idx];
}

/// @brief Write @p value to index @p idx WITHOUT bounds checking.
/// @param arr Non-null i64 array payload.
/// @param idx Compiler-proven in-bounds index.
/// @param value Value to store.
/// @warning No bounds checking! Use only when compiler has verified safety.
static inline void rt_arr_i64_set_unchecked(int64_t *arr, size_t idx, int64_t value) {
    arr[idx] = value;
}

/// @brief Resize an i64 array under copy-on-write ownership semantics.
/// @param[in,out] a_inout Address of the payload handle; may point to `NULL`.
/// @param new_len Requested logical length.
/// @return 0 on success, -1 on allocation failure.
int rt_arr_i64_resize(int64_t **a_inout, size_t new_len);

/// @brief Copy @p count elements between array payloads.
/// @param dst Destination payload; required when @p count is nonzero.
/// @param src Source payload; required when @p count is nonzero.
/// @param count Number of elements to copy.
/// @note Bounds are a caller precondition; invalid nonempty buffers trap.
void rt_arr_i64_copy_payload(int64_t *dst, const int64_t *src, size_t count);

#ifdef __cplusplus
}
#endif
