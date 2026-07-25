//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_set.h
// Purpose: Generic heterogeneous hash set using shared boxed-value
// comparison, providing O(1) average add, remove, and contains operations.
//
// Key invariants:
//   - Boxed numbers, booleans, floats, and strings use content-aware hash and
//     equality; ordinary objects use identity.
//   - Storing any equal value twice is idempotent.
//   - Elements are retained when added and released when removed.
//   - rt_set_has returns 1 if an equal element is present, 0 otherwise.
//
// Ownership/Lifetime:
//   - Set objects are runtime/GC-managed.
//   - Elements are retained by the set while stored.
//
// Error conventions:
//   - Constructor allocation failure → returns NULL
//   - Mutating allocation failure → runtime trap
//   - Membership (Has) → returns 1 (found) or 0 (not found)
//   - Removal (Remove) → returns 1 (removed) or 0 (not found)
//
// Links: src/runtime/collections/rt_set.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for the heterogeneous mutable hash Set.
/// @details The Set uses the runtime's matching hash/equality pair: boxed
///          scalar/string values compare by content and ordinary objects by
///          identity. Null is a valid element, algebra results independently
///          retain their entries, and iteration order is unspecified.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty set.
/// @return New runtime-managed Set, or `NULL` on allocation failure.
void *rt_set_new(void);

/// @brief Get number of elements in set.
/// @param obj Set handle, or `NULL`.
/// @return Unique-element count, or 0 for `NULL`.
int64_t rt_set_len(void *obj);

/// @brief Check if set is empty.
/// @param obj Set handle, or `NULL`.
/// @return 1 if empty, otherwise 0; `NULL` is empty.
int8_t rt_set_is_empty(void *obj);

/// @brief Add an element to the set.
/// @details Uses shared boxed-value hash/equality and retains only a newly
///          published element. Null is a valid element.
/// @param obj Set handle, or `NULL`.
/// @param elem Element to add; may be `NULL`.
/// @return 1 if element was new (added), 0 if already present.
int8_t rt_set_add(void *obj, void *elem);

/// @brief Remove an element from the set.
/// @details Releases the equal stored element, which may differ in pointer
///          identity from a boxed @p elem.
/// @param obj Set handle, or `NULL`.
/// @param elem Lookup value; may be `NULL`.
/// @return 1 if removed, 0 if not found.
int8_t rt_set_remove(void *obj, void *elem);

/// @brief Check if element exists in set.
/// @param obj Set handle, or `NULL`.
/// @param elem Lookup value under shared boxed equality.
/// @return 1 if present, 0 otherwise.
int8_t rt_set_has(void *obj, void *elem);

/// @brief Remove all elements from set.
/// @details Releases all elements/entries while preserving bucket capacity.
/// @param obj Set handle, or `NULL`.
void rt_set_clear(void *obj);

/// @brief Get all elements as a Seq.
/// @details The owning result independently retains elements in unspecified
///          bucket order.
/// @param obj Set handle, or `NULL`.
/// @return New runtime-managed owning Seq; empty for `NULL`.
void *rt_set_items(void *obj);

/// @brief Create union of two sets.
/// @param obj First Set, or `NULL` as empty.
/// @param other Second Set, or `NULL` as empty.
/// @return New runtime-managed Set retaining every unique operand element.
void *rt_set_union(void *obj, void *other);

/// @brief Create intersection of two sets.
/// @param obj First Set, or `NULL` as empty.
/// @param other Second Set, or `NULL` as empty.
/// @return New runtime-managed Set containing shared equal elements.
void *rt_set_intersect(void *obj, void *other);

/// @brief Create difference of two sets.
/// @param obj Minuend Set, or `NULL` as empty.
/// @param other Subtrahend Set, or `NULL` as empty.
/// @return New runtime-managed Set containing elements unique to @p obj.
void *rt_set_diff(void *obj, void *other);

/// @brief Check if this set is a subset of another.
/// @param obj Candidate subset, or `NULL` as empty.
/// @param other Candidate superset, or `NULL` as empty.
/// @return 1 if all elements of obj are in other, 0 otherwise.
int8_t rt_set_is_subset(void *obj, void *other);

/// @brief Check if this set is a superset of another.
/// @param obj Candidate superset, or `NULL` as empty.
/// @param other Candidate subset, or `NULL` as empty.
/// @return 1 if all elements of other are in obj, 0 otherwise.
int8_t rt_set_is_superset(void *obj, void *other);

/// @brief Check if two sets are disjoint (no common elements).
/// @param obj First Set, or `NULL` as empty.
/// @param other Second Set, or `NULL` as empty.
/// @return 1 if no elements in common, 0 otherwise.
int8_t rt_set_is_disjoint(void *obj, void *other);

/// @brief Create a shallow copy of the set.
/// @details Copies bucket/entry structure and independently retains each
///          shared element; element objects are not deep-cloned.
/// @param obj Source Set, or `NULL`.
/// @return New runtime-managed Set, empty for `NULL`.
void *rt_set_clone(void *obj);

#ifdef __cplusplus
}
#endif
