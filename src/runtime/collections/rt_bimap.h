//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_bimap.h
// Purpose: Bidirectional string-to-string map where every key maps to exactly one value and every
// value maps to exactly one key, supporting lookup in both directions.
//
// Key invariants:
//   - Every key maps to exactly one value and every value maps to exactly one key.
//   - Putting a key/value pair that conflicts with an existing mapping evicts the old entry.
//   - Lookup by key and lookup by value are both O(1) average.
//   - Keys and values are compared by full runtime string byte length.
//
// Ownership/Lifetime:
//   - BiMap objects are heap-allocated; caller is responsible for lifetime management.
//   - String keys and values are copied internally.
//
// Links: src/runtime/collections/rt_bimap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the runtime bidirectional string-map API.
///
/// BiMap maintains a one-to-one relationship between copied string keys and
/// copied string values. Each key and each value can participate in only one
/// mapping; inserting a conflicting pair evicts the previous pair or pairs.
/// Lookup, membership, and removal are available through either side.
///
/// Returned lookup strings and Seq snapshots are newly created values owned
/// through the normal runtime string/container conventions. BiMap objects are
/// runtime-managed opaque handles. Invalid non-null handles raise a runtime
/// trap, while null handles follow the per-operation behavior below.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Creates a new empty BiMap.
/// @return A new runtime-managed BiMap, or NULL after reporting an allocation
///         trap if construction cannot complete.
void *rt_bimap_new(void);

/// @brief Returns the current number of mappings.
/// @param obj BiMap handle, or NULL to query an empty map.
/// @return Number of key/value pairs, or zero when @p obj is NULL.
int64_t rt_bimap_len(void *obj);

/// @brief Tests whether a BiMap contains no mappings.
/// @param obj BiMap handle, or NULL to test an empty map.
/// @return 1 if @p obj is NULL or empty; otherwise 0.
int8_t rt_bimap_is_empty(void *obj);

/// @brief Insert a bidirectional key<->value mapping.
/// @details If key already exists, the old value mapping is removed.
///          If value already exists under a different key, that old key
///          mapping is removed. This ensures strict 1:1 relationship.
///          Replacement storage is allocated before conflicting mappings are
///          removed, preserving the old state on allocation failure.
/// @param obj BiMap handle; NULL makes the operation a no-op.
/// @param key Key to copy; NULL denotes the empty string.
/// @param value Value to copy; NULL denotes the empty string.
/// @note Invalid non-null handles and allocation failures raise a runtime trap.
void rt_bimap_put(void *obj, rt_string key, rt_string value);

/// @brief Look up value by key.
/// @param obj BiMap handle, or NULL to query an empty map.
/// @param key Key to find; NULL denotes the empty string.
/// @return A newly created mapped value, or an owned empty string if the key is
///         absent or @p obj is NULL.
rt_string rt_bimap_get_by_key(void *obj, rt_string key);

/// @brief Look up key by value (inverse lookup).
/// @param obj BiMap handle, or NULL to query an empty map.
/// @param value Value to find; NULL denotes the empty string.
/// @return A newly created mapped key, or an owned empty string if the value is
///         absent or @p obj is NULL.
rt_string rt_bimap_get_by_value(void *obj, rt_string value);

/// @brief Check if key exists.
/// @param obj BiMap handle, or NULL to query an empty map.
/// @param key Key to find; NULL denotes the empty string.
/// @return 1 if the key exists; otherwise 0.
int8_t rt_bimap_has_key(void *obj, rt_string key);

/// @brief Check if value exists.
/// @param obj BiMap handle, or NULL to query an empty map.
/// @param value Value to find; NULL denotes the empty string.
/// @return 1 if the value exists; otherwise 0.
int8_t rt_bimap_has_value(void *obj, rt_string value);

/// @brief Remove mapping by key.
/// @param obj BiMap handle, or NULL for a no-op.
/// @param key Key to remove; NULL denotes the empty string.
/// @return 1 if a mapping was removed; otherwise 0.
int8_t rt_bimap_remove_by_key(void *obj, rt_string key);

/// @brief Remove mapping by value.
/// @param obj BiMap handle, or NULL for a no-op.
/// @param value Value to remove; NULL denotes the empty string.
/// @return 1 if a mapping was removed; otherwise 0.
int8_t rt_bimap_remove_by_value(void *obj, rt_string value);

/// @brief Get all keys as an owning Seq of copied strings.
/// @param obj BiMap handle, or NULL to enumerate an empty map.
/// @return A new owning Seq containing newly created key strings.
/// @note Enumeration order is unspecified.
void *rt_bimap_keys(void *obj);

/// @brief Get all values as an owning Seq of copied strings.
/// @param obj BiMap handle, or NULL to enumerate an empty map.
/// @return A new owning Seq containing newly created value strings.
/// @note Enumeration order is unspecified and corresponds to key-table order.
void *rt_bimap_values(void *obj);

/// @brief Removes all mappings while retaining index capacities for reuse.
/// @param obj BiMap handle, or NULL for a no-op.
void rt_bimap_clear(void *obj);

#ifdef __cplusplus
}
#endif
