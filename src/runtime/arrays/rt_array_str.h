//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares the C ABI for arrays of reference-counted runtime strings.
/// @details Container and element lifetimes are independent: every populated
///          slot owns one string reference, reads return an additional retained
///          handle to the caller, and only the last container release tears down
///          the stored handles.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/arrays/rt_array_str.h
// Purpose: Dynamic string array API for BASIC DIM'd string collections, providing two-level
// reference counting over both the array container and each individual string element.
//
// Key invariants:
//   - Slots are initialized to NULL on allocation.
//   - rt_arr_str_put retains the new value and releases the old slot.
//   - rt_arr_str_get returns a retained reference; caller must release it.
//   - rt_arr_str_release tears down elements only after the final container release.
//
// Ownership/Lifetime:
//   - Array container is refcounted via the heap header.
//   - Each element is independently refcounted.
//   - Callers must release references obtained from rt_arr_str_get.
//
// Links: src/runtime/arrays/rt_array_str.c (implementation), src/runtime/core/rt_string.h,
// src/runtime/core/rt_heap.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_heap.h"
#include "rt_string.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Allocate a new dynamic array of string handles.
/// @details Allocates an array of @p len string pointers, all initialized to NULL.
///          Callers must use rt_arr_str_put to assign values, which handles retain/release.
/// @param len Number of string slots to allocate.
/// @return Array payload pointer or NULL on allocation failure.
rt_string *rt_arr_str_alloc(size_t len);

/// @brief Release one string-array ownership reference.
/// @details Shared arrays keep their elements intact. The final release iterates
///          through the authoritative element count, releases each string, and
///          then reclaims the array allocation.
/// @param arr Array payload pointer (may be NULL).
/// @param size Historical caller-supplied element count; ignored.
void rt_arr_str_release(rt_string *arr, size_t size);

/// @brief Read string element at index @p idx and return a retained handle.
/// @details Returns the string at @p idx after incrementing its reference count.
///          Caller must call rt_str_release_maybe on the returned handle when done.
/// @param arr Array payload pointer; must be non-null and in range.
/// @param idx Zero-based index within the array length.
/// @return String handle at @p idx (retained for caller), or NULL if slot is empty.
rt_string rt_arr_str_get(rt_string *arr, size_t idx);

/// @brief Write @p value to index @p idx with proper reference counting.
/// @details Retains the new @p value, releases the old value at @p idx,
///          then stores the new value. This maintains proper ownership semantics.
/// @param arr Array payload pointer; must be non-null and in range.
/// @param idx Zero-based index within the array length.
/// @param value String handle to store (may be NULL); will be retained.
void rt_arr_str_put(rt_string *arr, size_t idx, rt_string value);

/// @brief Query the current logical length of a string array.
/// @details Returns the element count stored in the heap header.
/// @param arr Array payload pointer returned by rt_arr_str_alloc.
/// @return Number of accessible elements; 0 when @p arr is NULL.
size_t rt_arr_str_len(rt_string *arr);

#ifdef __cplusplus
}
#endif
