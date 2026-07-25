//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_frozenset.h
// Purpose: Immutable string set created from a Seq, providing O(1) average membership
// testing with guaranteed read-only semantics after construction.
//
// Key invariants:
//   - Once created, the set cannot be modified.
//   - Elements are byte-length-aware strings; embedded NUL bytes are part of identity.
//   - Membership testing is O(1) average.
//   - Constructed from a Seq of strings; the source is not consumed.
//   - rt_frozenset_has returns 1 if element is present, 0 otherwise.
//
// Ownership/Lifetime:
//   - FrozenSet retains its string elements.
//   - FrozenSet objects are GC-managed (rt_obj_new_i64) with a runtime finalizer;
//     callers must not free them directly.
//
// Links: src/runtime/collections/rt_frozenset.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares immutable runtime string-set operations.
///
/// FrozenSet retains non-null strings extracted from a source Seq and
/// collapses duplicates by complete byte content. No mutation API is exposed
/// after construction. Membership is average constant-time, and completed sets
/// are safe for concurrent read-only access.
///
/// Set algebra returns independent FrozenSets without modifying operands.
/// Enumeration returns a new owning Seq retaining its strings. Null set handles
/// act as empty sets; null element handles are skipped and never match.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a frozen set from a Seq of strings.
/// @param items Seq containing raw or boxed strings, or NULL.
/// @return New FrozenSet with byte-equal duplicates collapsed and null
///         elements skipped.
void *rt_frozenset_from_seq(void *items);

/// @brief Create an empty frozen set.
/// @return New runtime-managed empty FrozenSet.
void *rt_frozenset_empty(void);

/// @brief Get number of elements.
/// @param obj FrozenSet handle, or NULL.
/// @return Element count, or zero for NULL.
int64_t rt_frozenset_len(void *obj);

/// @brief Check if set is empty.
/// @param obj FrozenSet handle, or NULL.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_frozenset_is_empty(void *obj);

/// @brief Check if element exists in set.
/// @param obj FrozenSet handle, or NULL.
/// @param elem Non-null string to check.
/// @return 1 if present, 0 otherwise.
int8_t rt_frozenset_has(void *obj, rt_string elem);

/// @brief Get all elements as a Seq.
/// @param obj FrozenSet handle, or NULL.
/// @return New owning Seq retaining all strings in unspecified slot order.
void *rt_frozenset_items(void *obj);

/// @brief Create union of two frozen sets.
/// @param obj First FrozenSet, or NULL as empty.
/// @param other Second FrozenSet, or NULL as empty.
/// @return New independent FrozenSet containing elements from both.
void *rt_frozenset_union(void *obj, void *other);

/// @brief Create intersection of two frozen sets.
/// @param obj First FrozenSet, or NULL as empty.
/// @param other Second FrozenSet, or NULL as empty.
/// @return New independent FrozenSet containing elements in both.
void *rt_frozenset_intersect(void *obj, void *other);

/// @brief Create difference of two frozen sets.
/// @param obj First FrozenSet, or NULL as empty.
/// @param other Exclusion FrozenSet, or NULL to exclude nothing.
/// @return New independent FrozenSet with elements in first but not second.
void *rt_frozenset_diff(void *obj, void *other);

/// @brief Check if this set is a subset of another.
/// @param obj Candidate subset, or NULL as empty.
/// @param other Candidate superset, or NULL as empty.
/// @return 1 if all elements of obj are in other.
int8_t rt_frozenset_is_subset(void *obj, void *other);

/// @brief Check if two frozen sets are equal (same elements).
/// @param obj First FrozenSet, or NULL as empty.
/// @param other Second FrozenSet, or NULL as empty.
/// @return 1 if equal, 0 otherwise.
int8_t rt_frozenset_equals(void *obj, void *other);

#ifdef __cplusplus
}
#endif
