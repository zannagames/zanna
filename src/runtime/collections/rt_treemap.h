//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_treemap.h
/// @file
/// @brief Declares the GC-managed sorted string-keyed TreeMap runtime API.
///
// Purpose: Sorted key-value map with string keys maintained in lexicographic order, providing
// floor/ceil/first/last navigation and ordered range iteration.
//
// Key invariants:
//   - Keys are byte-length-aware strings maintained in sorted lexicographic order.
//   - rt_treemap_floor returns the largest key <= query; rt_treemap_ceil returns the smallest >=
//   query.
//   - rt_treemap_first/last return the minimum/maximum keys.
//   - All mutation operations maintain the sort-order invariant.
//
// Ownership/Lifetime:
//   - TreeMap objects are GC-managed (rt_obj_new_i64) with a runtime finalizer;
//     callers must not free them directly.
//   - Keys are copied; null keys denote the empty key. Values are retained
//     while stored, and Get returns a borrowed value.
//   - Keys() and Values() return new owning Seq snapshots. First(), Last(),
//     Floor(), and Ceil() return runtime string handles following the normal
//     string ownership conventions.
//
// Links: src/runtime/collections/rt_treemap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty sorted map.
/// @return New GC-managed TreeMap, or `NULL` after an allocation trap.
void *rt_treemap_new(void);

/// @brief Get the number of entries in the map.
/// @param obj TreeMap pointer.
/// @return Number of key-value pairs.
/// @note A null pointer is treated as an empty map.
int64_t rt_treemap_len(void *obj);

/// @brief Check if the map is empty.
/// @param obj TreeMap pointer.
/// @return True if map contains no entries.
/// @note A null pointer is treated as an empty map.
int8_t rt_treemap_is_empty(void *obj);

/// @brief Set a key-value pair (insert or update).
/// @param obj TreeMap pointer.
/// @param key String key whose bytes are copied; null denotes the empty key.
/// @param value Nullable value retained by the map.
/// @note A null map is ignored. Allocation failure or an invalid object traps.
void rt_treemap_set(void *obj, rt_string key, void *value);

/// @brief Get the value for a key.
/// @param obj TreeMap pointer.
/// @param key String key; null denotes the empty key.
/// @return Borrowed value, or `NULL` when absent, null-valued, or @p obj is null.
void *rt_treemap_get(void *obj, rt_string key);

/// @brief Check if a key exists in the map.
/// @param obj TreeMap pointer.
/// @param key String key; null denotes the empty key.
/// @return True if key exists.
int8_t rt_treemap_has(void *obj, rt_string key);

/// @brief Remove a key-value pair.
/// @param obj TreeMap pointer.
/// @param key String key; null denotes the empty key.
/// @return True if key was found and removed.
/// @note Removing an entry releases the map's retained value reference.
int8_t rt_treemap_remove(void *obj, rt_string key);

/// @brief Remove all entries from the map.
/// @param obj TreeMap pointer.
/// @note Releases every retained value but preserves allocated entry capacity.
void rt_treemap_clear(void *obj);

/// @brief Get all keys as a Seq in sorted order.
/// @param obj TreeMap pointer.
/// @return New owning Seq containing independent key strings in sorted order.
void *rt_treemap_keys(void *obj);

/// @brief Get all values as a Seq in key-sorted order.
/// @param obj TreeMap pointer.
/// @return New owning Seq containing retained values in key-sorted order.
void *rt_treemap_values(void *obj);

/// @brief Get the smallest (first) key.
/// @param obj TreeMap pointer.
/// @return Newly constructed first key or the empty-string constant if empty.
rt_string rt_treemap_first(void *obj);

/// @brief Get the largest (last) key.
/// @param obj TreeMap pointer.
/// @return Newly constructed last key or the empty-string constant if empty.
rt_string rt_treemap_last(void *obj);

/// @brief Get the largest key less than or equal to the given key.
/// @param obj TreeMap pointer.
/// @param key Reference key; null denotes the empty key.
/// @return Newly constructed floor key or the empty-string constant if absent.
/// @note An empty stored key is indistinguishable by contents from no result.
rt_string rt_treemap_floor(void *obj, rt_string key);

/// @brief Get the smallest key greater than or equal to the given key.
/// @param obj TreeMap pointer.
/// @param key Reference key; null denotes the empty key.
/// @return Newly constructed ceiling key or the empty-string constant if absent.
/// @note An empty stored key is indistinguishable by contents from no result.
rt_string rt_treemap_ceil(void *obj, rt_string key);

#ifdef __cplusplus
}
#endif
