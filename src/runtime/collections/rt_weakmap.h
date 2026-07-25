//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_weakmap.h
/// @file
/// @brief Declares the GC-managed string-keyed WeakMap runtime API.
///
// Purpose: String-keyed map holding zeroing weak references to values, so values may be freed
// without leaving dangling map reads; getting a collected value returns NULL.
//
// Key invariants:
//   - Values are wrapped in rt_weakref handles; the map does not retain values.
//   - rt_weakmap_get returns NULL for both missing keys and collected values.
//   - String keys are retained and compared by byte length; embedded NUL bytes are significant.
//   - Public length, emptiness, Has, and Keys expose only live weak values.
//
// Ownership/Lifetime:
//   - WeakMap objects are GC-managed.
//   - Entry keys and weak-reference handles are owned by the map.
//   - Values should be runtime-managed objects or strings so final release can zero weak refs.
//   - Get returns a borrowed target without extending its lifetime. Keys()
//     returns an owning Seq of retained immutable key strings.
//
// Links: src/runtime/collections/rt_weakmap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty weak map.
/// @return New GC-managed WeakMap, or `NULL` after an allocation trap.
void *rt_weakmap_new(void);

/// @brief Get the number of live entries.
/// @param map Weak map.
/// @return Number of entries whose weak value is still live.
/// @note Runs in O(table capacity); dead slots are not removed.
int64_t rt_weakmap_len(void *map);

/// @brief Check if the map is empty.
/// @param map Weak map.
/// @return 1 if empty, 0 otherwise.
/// @note A map containing only collected targets is considered empty.
int8_t rt_weakmap_is_empty(void *map);

/// @brief Set a value in the map (weak reference).
/// @param map Weak map.
/// @param key Length-aware string key; null denotes the empty key.
/// @param value Nullable runtime-managed target stored weakly.
/// @note The map retains the key but never strongly retains @p value.
void rt_weakmap_set(void *map, rt_string key, void *value);

/// @brief Get a value from the map.
/// @param map Weak map.
/// @param key Length-aware string key; null denotes the empty key.
/// @return Borrowed target, or `NULL` if not found or collected.
void *rt_weakmap_get(void *map, rt_string key);

/// @brief Check if a key exists in the map.
/// @param map Weak map.
/// @param key Length-aware string key; null denotes the empty key.
/// @return 1 only if the key's weak target is live, otherwise 0.
int8_t rt_weakmap_has(void *map, rt_string key);

/// @brief Remove a key from the map.
/// @param map Weak map.
/// @param key Length-aware string key; null denotes the empty key.
/// @return 1 if an occupied entry was removed, including a dead one; otherwise 0.
int8_t rt_weakmap_remove(void *map, rt_string key);

/// @brief Get all keys whose weak values are currently live.
/// @param map Weak map.
/// @return New owning Seq retaining the live entries' keys in unspecified order.
void *rt_weakmap_keys(void *map);

/// @brief Remove all entries from the map.
/// @param map Weak map.
/// @note Preserves table capacity and releases keys and weakref handles.
void rt_weakmap_clear(void *map);

/// @brief Compact the map by removing entries with NULL or collected values.
/// @param map Weak map.
/// @return Number of entries removed.
/// @note Rebuilds at the existing capacity only when dead entries exist.
int64_t rt_weakmap_compact(void *map);

#ifdef __cplusplus
}
#endif
