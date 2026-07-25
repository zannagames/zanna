//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares the unboxed i64 runtime list ABI and inline array adapters.
/// @details Lists reuse the i64 array payload/header representation while
///          exposing append, pop, and peek operations. Growth may replace and
///          release the old payload, so mutating append accepts the address of
///          the caller's handle.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/arrays/rt_list_i64.h
// Purpose: Unboxed dynamic-append list of 64-bit integers (P2-3.7) avoiding the boxing overhead of
// the generic rt_list for hot integer collections.
//
// Key invariants:
//   - len <= cap at all times; push is amortized O(1).
//   - No boxing overhead: elements are stored as raw int64_t values.
//   - Push growth doubles capacity and transfers ownership of the old allocation.
//
// Ownership/Lifetime:
//   - Reference-counted via rt_list_i64_retain/release.
//   - The caller owns the initial reference from rt_list_i64_new.
//   - Resize releases the old pointer; callers must not hold raw pointers across push.
//
// Links: src/runtime/arrays/rt_list_i64.c (implementation), src/runtime/arrays/rt_array_i64.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_array_i64.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Allocate an empty typed int64 list with reserved capacity.
/// @details Avoids boxing overhead for integer collections by storing
///          raw @c int64_t values directly in the heap payload.
/// @param init_cap Initial capacity in elements (≥ 1 recommended; 0 → 8).
/// @return List payload pointer or NULL on allocation failure.
int64_t *rt_list_i64_new(size_t init_cap);

/// @brief Increment the reference count for @p list.
/// @param list List payload pointer (may be NULL).
static inline void rt_list_i64_retain(int64_t *list) {
    rt_arr_i64_retain(list);
}

/// @brief Decrement the reference count and free when it reaches zero.
/// @param list List payload pointer (may be NULL).
static inline void rt_list_i64_release(int64_t *list) {
    rt_arr_i64_release(list);
}

/// @brief Return the current number of elements.
/// @param list List payload pointer (may be NULL).
/// @return Element count; 0 when @p list is NULL.
static inline size_t rt_list_i64_len(int64_t *list) {
    return rt_arr_i64_len(list);
}

/// @brief Return the current capacity in elements.
/// @param list List payload pointer (may be NULL).
/// @return Reserved element slots; zero when @p list is NULL.
static inline size_t rt_list_i64_cap(int64_t *list) {
    return rt_arr_i64_cap(list);
}

/// @brief Read the element at @p idx with bounds checking.
/// @param list List payload pointer.
/// @param idx  Zero-based index.
/// @return Element value.
static inline int64_t rt_list_i64_get(int64_t *list, size_t idx) {
    return rt_arr_i64_get(list, idx);
}

/// @brief Write @p val at @p idx with bounds checking.
/// @param list Non-null list payload.
/// @param idx Zero-based index within logical length.
/// @param val Value to store.
/// @note Traps on invalid handles or indices.
static inline void rt_list_i64_set(int64_t *list, size_t idx, int64_t val) {
    rt_arr_i64_set(list, idx, val);
}

/// @brief Append @p val to the end of the list with amortized O(1) growth.
/// @details When capacity is exhausted the buffer is doubled.  The pointer
///          at @p list_inout may change after a grow; callers must use the
///          updated value.
/// @param list_inout Pointer to the list payload pointer; updated on realloc.
/// @param val        Value to append.
/// @return 0 on success, -1 on allocation failure.
int rt_list_i64_push(int64_t **list_inout, int64_t val);

/// @brief Remove and return the last element.
/// @details Traps when the list is empty; does not release memory.
/// @param list_inout Pointer to the list payload pointer.
/// @return The removed element value.
int64_t rt_list_i64_pop(int64_t **list_inout);

/// @brief Return the last element without removing it.
/// @details Traps when the list is empty.
/// @param list List payload pointer.
/// @return The last element value.
int64_t rt_list_i64_peek(int64_t *list);

#ifdef __cplusplus
}
#endif
