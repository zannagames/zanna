//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_sortedset.h
// Purpose: Sorted set of unique strings maintained in lexicographic order, enabling range queries
// (floor, ceil, between) and ordered forward/backward iteration.
//
// Key invariants:
//   - Elements are unique byte-length-aware strings maintained in sorted order.
//   - All operations maintain the sorted invariant.
//   - rt_sortedset_floor returns the largest element <= key; rt_sortedset_ceil returns the smallest
//   >= key.
//   - Returned subsets from range queries are owning snapshots of copied strings.
//
// Ownership/Lifetime:
//   - SortedSet objects are runtime/GC-managed.
//   - String elements are copied into the set; caller retains ownership of input strings.
//   - Selected-string accessors retain stored results for the caller; range
//     snapshots and set algebra create independent string copies.
//
// Links: src/runtime/collections/rt_sortedset.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for the length-aware lexicographic string SortedSet.
/// @details Strings are unique under complete byte comparison, including
///          embedded NUL bytes, and are maintained in ascending order. Null
///          input strings normalize to the empty string. Ordered access uses
///          the empty string as a no-result sentinel, so membership must be
///          consulted when an actual empty-string element is significant.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Creation and Lifecycle
//=============================================================================

/// @brief Create a new empty sorted set.
/// @return New runtime-managed SortedSet.
void *rt_sortedset_new(void);

/// @brief Get the number of elements in the set.
/// @param obj SortedSet handle, or `NULL`.
/// @return Unique-string count, or 0 for `NULL`.
int64_t rt_sortedset_len(void *obj);

/// @brief Check if the set is empty.
/// @param obj SortedSet handle, or `NULL`.
/// @return 1 if empty, otherwise 0; `NULL` is empty.
int8_t rt_sortedset_is_empty(void *obj);

//=============================================================================
// Basic Operations
//=============================================================================

/// @brief Add a string to the set.
/// @details Copies the complete string bytes into the sorted array. Null
///          denotes the empty string.
/// @param obj SortedSet handle, or `NULL`.
/// @param str String to copy.
/// @return 1 if string was new (added), 0 if already present.
int8_t rt_sortedset_add(void *obj, rt_string str);

/// @brief Remove a string from the set.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Equal string to remove; `NULL` denotes empty.
/// @return 1 if removed, 0 if not found.
int8_t rt_sortedset_remove(void *obj, rt_string str);

/// @brief Check if a string exists in the set.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Query string; `NULL` denotes empty.
/// @return 1 if present, 0 otherwise.
int8_t rt_sortedset_has(void *obj, rt_string str);

/// @brief Remove all elements from the set.
/// @details Releases stored string copies while preserving backing capacity.
/// @param obj SortedSet handle, or `NULL`.
void rt_sortedset_clear(void *obj);

//=============================================================================
// Ordered Access
//=============================================================================

/// @brief Get the smallest element.
/// @param obj SortedSet handle, or `NULL`.
/// @return Caller-retained first element, or empty-string sentinel.
rt_string rt_sortedset_first(void *obj);

/// @brief Get the largest element.
/// @param obj SortedSet handle, or `NULL`.
/// @return Caller-retained last element, or empty-string sentinel.
rt_string rt_sortedset_last(void *obj);

/// @brief Get the greatest element less than or equal to the given string.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Upper bound (inclusive).
/// @return Caller-retained floor element, or empty-string sentinel.
rt_string rt_sortedset_floor(void *obj, rt_string str);

/// @brief Get the least element greater than or equal to the given string.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Lower bound (inclusive).
/// @return Caller-retained ceiling element, or empty-string sentinel.
rt_string rt_sortedset_ceil(void *obj, rt_string str);

/// @brief Get the greatest element strictly less than the given string.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Upper bound (exclusive).
/// @return Caller-retained lower element, or empty-string sentinel.
rt_string rt_sortedset_lower(void *obj, rt_string str);

/// @brief Get the least element strictly greater than the given string.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Lower bound (exclusive).
/// @return Caller-retained higher element, or empty-string sentinel.
rt_string rt_sortedset_higher(void *obj, rt_string str);

/// @brief Get element at index in sorted order.
/// @param obj SortedSet handle, or `NULL`.
/// @param index 0-based index.
/// @return Caller-retained element, or empty-string sentinel.
rt_string rt_sortedset_at(void *obj, int64_t index);

/// @brief Get the index of an element in sorted order.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Element to find; `NULL` denotes empty.
/// @return Index of element, or -1 if not found.
int64_t rt_sortedset_index_of(void *obj, rt_string str);

//=============================================================================
// Range Operations
//=============================================================================

/// @brief Get all elements in a range [from, to].
/// @param obj SortedSet handle, or `NULL`.
/// @param from Start of range (inclusive), or NULL for an open lower bound.
/// @param to End of range (inclusive), or NULL for an open upper bound.
/// @return New runtime-managed owning Seq of independent string copies.
void *rt_sortedset_range(void *obj, rt_string from, rt_string to);

/// @brief Get all elements as a Seq in sorted order.
/// @param obj SortedSet handle, or `NULL`.
/// @return New runtime-managed owning Seq containing copied elements.
void *rt_sortedset_items(void *obj);

/// @brief Get the first n elements.
/// @param obj SortedSet handle, or `NULL`.
/// @param n Maximum elements to get; nonpositive values select none.
/// @return New runtime-managed owning Seq of copied first elements.
void *rt_sortedset_take(void *obj, int64_t n);

/// @brief Get all elements except the first n.
/// @param obj SortedSet handle, or `NULL`.
/// @param n Number of elements to skip; negative values skip zero.
/// @return New runtime-managed owning Seq of copied remaining elements.
void *rt_sortedset_skip(void *obj, int64_t n);

//=============================================================================
// Set Operations
//=============================================================================

/// @brief Create union of two sorted sets.
/// @param obj First SortedSet, or `NULL` as empty.
/// @param other Second SortedSet, or `NULL` as empty.
/// @return New runtime-managed SortedSet containing independent string copies.
void *rt_sortedset_union(void *obj, void *other);

/// @brief Create intersection of two sorted sets.
/// @param obj First SortedSet, or `NULL` as empty.
/// @param other Second SortedSet, or `NULL` as empty.
/// @return New runtime-managed SortedSet containing independent shared values.
void *rt_sortedset_intersect(void *obj, void *other);

/// @brief Create difference of two sorted sets.
/// @param obj Minuend SortedSet, or `NULL` as empty.
/// @param other Subtrahend SortedSet, or `NULL` as empty.
/// @return New runtime-managed SortedSet containing independent result copies.
void *rt_sortedset_diff(void *obj, void *other);

/// @brief Check if this set is a subset of another.
/// @param obj Candidate subset, or `NULL` as empty.
/// @param other Candidate superset, or `NULL` as empty.
/// @return 1 if obj is a subset of other, 0 otherwise.
int8_t rt_sortedset_is_subset(void *obj, void *other);

#ifdef __cplusplus
}
#endif
