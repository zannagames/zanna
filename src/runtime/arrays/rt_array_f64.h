//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares the C ABI for reference-counted runtime arrays of doubles.
/// @details Handles point to contiguous IEEE-754 payload elements after an
///          internal heap header. Checked accessors trap, fast/inline accessors
///          require compiler-proven bounds, and copy-on-write resize may replace
///          the caller's payload pointer.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/arrays/rt_array_f64.h
// Purpose: Dynamic array API for 64-bit floats (double) supporting BASIC SINGLE/DOUBLE typed
// collections, mirroring the i64 array interface with allocation, refcounting, bounds-checked
// access, and resize.
//
// Key invariants:
//   - Payload pointers are preceded by an rt_heap_hdr_t header at a negative offset.
//   - length <= capacity at all times; indexed access traps on out-of-bounds.
//   - New arrays start with refcount 1.
//   - Resize may reallocate and rebind the payload pointer.
//
// Ownership/Lifetime:
//   - Reference-counted via rt_arr_f64_retain/release.
//   - The caller owns the initial reference from rt_arr_f64_new.
//   - Resize transfers ownership of the old allocation.
//
// Links: src/runtime/arrays/rt_array_f64.c (implementation), src/runtime/core/rt_heap.h
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
rt_heap_hdr_t *rt_arr_f64_hdr(const double *payload);

/// @brief Allocate a new dynamic array of 64-bit floats.
/// @param len Number of elements to allocate.
/// @return Array payload pointer or NULL on allocation failure.
double *rt_arr_f64_new(size_t len);

/// @brief Increment the reference count for @p arr.
/// @param arr Array payload pointer; `NULL` is ignored.
void rt_arr_f64_retain(double *arr);

/// @brief Decrement the reference count for @p arr and free on zero.
/// @param arr Array payload pointer; `NULL` is ignored.
void rt_arr_f64_release(double *arr);

/// @brief Query the current logical length of an array.
/// @param arr Array payload pointer; may be `NULL`.
/// @return Number of accessible elements; 0 when @p arr is NULL.
size_t rt_arr_f64_len(double *arr);

/// @brief Query the current capacity in elements.
/// @param arr Array payload pointer; may be `NULL`.
/// @return Reserved element slots; zero for `NULL`.
size_t rt_arr_f64_cap(double *arr);

/// @brief Read element at index @p idx with bounds checking.
/// @param arr Non-null array payload.
/// @param idx Zero-based index within logical length.
/// @return Stored value.
/// @note Traps when the handle or index is invalid.
double rt_arr_f64_get(double *arr, size_t idx);

/// @brief Write @p value to index @p idx with bounds checking.
/// @param arr Non-null array payload.
/// @param idx Zero-based index within logical length.
/// @param value Value to store.
/// @note Traps when the handle or index is invalid.
void rt_arr_f64_set(double *arr, size_t idx, double value);

/// @brief Read element at index @p idx after the compiler proved bounds.
/// @param arr Non-null array payload.
/// @param idx Compiler-proven in-bounds index.
/// @return Stored value.
/// @warning No bounds checking. Use only when a dominating check guarantees safety.
double rt_arr_f64_get_fast(double *arr, size_t idx);

/// @brief Write @p value to index @p idx after the compiler proved bounds.
/// @param arr Non-null array payload.
/// @param idx Compiler-proven in-bounds index.
/// @param value Value to store.
/// @warning No bounds checking. Use only when a dominating check guarantees safety.
void rt_arr_f64_set_fast(double *arr, size_t idx, double value);

/// @brief Read element at index @p idx WITHOUT bounds checking.
/// @param arr Non-null array payload.
/// @param idx Compiler-proven in-bounds index.
/// @return Stored value.
/// @warning No bounds checking! Use only when compiler has verified safety.
static inline double rt_arr_f64_get_unchecked(double *arr, size_t idx) {
    return arr[idx];
}

/// @brief Write @p value to index @p idx WITHOUT bounds checking.
/// @param arr Non-null array payload.
/// @param idx Compiler-proven in-bounds index.
/// @param value Value to store.
/// @warning No bounds checking! Use only when compiler has verified safety.
static inline void rt_arr_f64_set_unchecked(double *arr, size_t idx, double value) {
    arr[idx] = value;
}

/// @brief Resize an array under copy-on-write ownership semantics.
/// @param[in,out] a_inout Address of the payload handle; may point to `NULL`.
/// @param new_len Requested logical length.
/// @return 0 on success, -1 on allocation failure.
int rt_arr_f64_resize(double **a_inout, size_t new_len);

/// @brief Copy @p count elements between array payloads.
/// @param dst Destination payload; required when @p count is nonzero.
/// @param src Source payload; required when @p count is nonzero.
/// @param count Number of elements to copy.
/// @note Bounds are a caller precondition; invalid nonempty buffers trap.
void rt_arr_f64_copy_payload(double *dst, const double *src, size_t count);

#ifdef __cplusplus
}
#endif
