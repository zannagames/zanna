//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_bag.h
// Purpose: String set (Bag) backed by a hash table, providing O(1) average membership testing,
// insertion, and removal of unique string values.
//
// Key invariants:
//   - Stores unique strings only; duplicate insertions are silently ignored.
//   - All operations are O(1) average-case using a separate-chaining hash table.
//   - Iteration order is unspecified and may change after insertions.
//   - rt_bag_has returns 1 if present, 0 if absent.
//
// Ownership/Lifetime:
//   - Bag objects are GC-managed (rt_obj_new_i64) with a runtime finalizer;
//     callers must not free them directly.
//   - String values are copied into the bag; caller retains ownership of input strings.
//
// Error conventions:
//   - Allocation failure → returns NULL
//   - Membership (Contains) → returns 1 (found) or 0 (not found)
//   - Removal (Remove) → returns 1 (removed) or 0 (not found)
//
// Links: src/runtime/collections/rt_bag.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the runtime API for the unique-string Bag collection.
///
/// A Bag stores at most one copy of each byte string. Inserted values are
/// copied, results returned by the set-producing operations are independent
/// Bags, and enumeration produces an owning Seq of newly created runtime
/// strings in unspecified hash-table order. Bag objects are runtime-managed;
/// callers pass them through this API as opaque handles.
///
/// Unless an operation explicitly defines a null handle as an empty Bag,
/// invalid non-null handles raise a runtime trap. Allocation and capacity
/// failures also trap; functions whose signatures permit failure return their
/// documented fallback value if the installed trap handler returns.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Creates a new empty Bag.
/// @details The Bag starts with the implementation's default bucket capacity
///          and owns copies of every subsequently inserted string.
/// @return A new runtime-managed Bag, or NULL after reporting an allocation
///         trap if the trap handler returns.
void *rt_bag_new(void);

/// @brief Returns the number of unique strings in a Bag.
/// @param obj Bag handle, or NULL to query an empty Bag.
/// @return The element count, or zero when @p obj is NULL.
/// @note An invalid non-null handle raises a runtime trap.
int64_t rt_bag_len(void *obj);

/// @brief Tests whether a Bag contains no strings.
/// @param obj Bag handle, or NULL to test an empty Bag.
/// @return 1 when @p obj is NULL or contains no elements; otherwise 0.
/// @note An invalid non-null handle raises a runtime trap.
int8_t rt_bag_is_empty(void *obj);

/// @brief Adds a string to a Bag unless an equal string is already present.
/// @details Duplicate insertion is the only ordinary false result. Invalid
///          handles, capacity overflow, and allocation failure raise a runtime
///          trap and leave the Bag unchanged.
/// @param obj Non-null Bag handle.
/// @param str String value to copy; NULL is treated as an empty string.
/// @return 1 if the value was inserted, or 0 if it was already present. Also
///         returns 0 if a reporting trap handler returns.
int8_t rt_bag_add(void *obj, rt_string str);

/// @brief Removes a string from a Bag when present.
/// @param obj Bag handle, or NULL for a no-op.
/// @param str String value to remove; NULL is treated as an empty string.
/// @return 1 if an entry was removed, or 0 if absent or @p obj is NULL.
/// @note An invalid non-null handle raises a runtime trap.
int8_t rt_bag_remove(void *obj, rt_string str);

/// @brief Tests whether a Bag contains a string.
/// @param obj Bag handle, or NULL to query an empty Bag.
/// @param str String value to find; NULL is treated as an empty string.
/// @return 1 if the value is present, or 0 if absent or @p obj is NULL.
/// @note An invalid non-null handle raises a runtime trap.
int8_t rt_bag_has(void *obj, rt_string str);

/// @brief Removes every string from a Bag while retaining its bucket capacity.
/// @param obj Bag handle, or NULL for a no-op.
/// @note An invalid non-null handle raises a runtime trap.
void rt_bag_clear(void *obj);

/// @brief Copies all Bag elements into a newly allocated Seq.
/// @param obj Bag handle, or NULL to enumerate an empty Bag.
/// @return A new owning Seq of newly created runtime strings. The Seq is empty
///         when @p obj is NULL or the Bag has no elements.
/// @note Element order follows internal bucket and chain order and is not part
///       of the API contract.
/// @note An invalid non-null handle raises a runtime trap.
void *rt_bag_items(void *obj);

/// @brief Creates the set union of two Bags.
/// @param obj First Bag handle, or NULL for an empty first operand.
/// @param other Second Bag handle, or NULL for an empty second operand.
/// @return A new independent Bag containing every value in either operand.
/// @note Invalid non-null handles raise a runtime trap.
void *rt_bag_union(void *obj, void *other);

/// @brief Creates the set intersection of two Bags.
/// @param obj First Bag handle, or NULL for an empty first operand.
/// @param other Second Bag handle, or NULL for an empty second operand.
/// @return A new independent Bag containing values present in both operands;
///         the result is empty if either operand is NULL.
/// @note Invalid non-null handles that are examined raise a runtime trap.
void *rt_bag_intersect(void *obj, void *other);

/// @brief Creates the set difference of two Bags.
/// @param obj Source Bag handle, or NULL for an empty source.
/// @param other Exclusion Bag handle, or NULL to exclude no values.
/// @return A new independent Bag containing values in @p obj but not
///         @p other.
/// @note Invalid non-null handles that are examined raise a runtime trap.
void *rt_bag_diff(void *obj, void *other);

/// @brief Creates an independent copy of a Bag.
/// @param obj Source Bag handle, or NULL to copy an empty Bag.
/// @return A new Bag containing copies of all values from @p obj.
/// @note An invalid non-null handle raises a runtime trap.
void *rt_bag_clone(void *obj);

#ifdef __cplusplus
}
#endif
