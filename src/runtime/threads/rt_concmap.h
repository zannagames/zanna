//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_concmap.h
// Purpose: Thread-safe concurrent hash map with string keys using a mutex and separate chaining,
// providing safe concurrent get/put/remove from multiple threads.
//
// Key invariants:
//   - All operations are protected by a single mutex; thread-safe for concurrent access.
//   - Uses FNV-1a hash with separate chaining for collision resolution.
//   - Keys are copied into the map; values are retained on insertion.
//   - rt_concmap_get_or_default returns a default value for missing keys without trapping.
//
// Ownership/Lifetime:
//   - ConcMap objects are heap-allocated; caller is responsible for lifetime management.
//   - Values are retained while stored; caller manages external references separately.
//
// Links: src/runtime/threads/rt_concmap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief String-keyed concurrent map C ABI.
/// @details One platform mutex protects hash-table state. Keys are copied by
///          complete runtime byte length, including embedded null bytes, and
///          stored runtime values are retained until replacement, removal, or
///          map finalization.
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create an empty map with the initial 16-bucket table.
/// @return New runtime-managed ConcurrentMap, or null after a construction trap.
void *rt_concmap_new(void);

/// @brief Read the entry count under the map mutex.
/// @param obj ConcurrentMap handle; null reports zero.
/// @return Exact count at the instant of the locked read.
int64_t rt_concmap_len(void *obj);

/// @brief Test whether the locked entry count is zero.
/// @param obj ConcurrentMap handle; null is treated as empty.
/// @return 1 when empty, otherwise 0.
int8_t rt_concmap_is_empty(void *obj);

/// @brief Atomically insert or replace a key-value pair.
/// @details A null key denotes the empty string. The complete key bytes are
///          copied and @p value is retained; replaced values are released
///          outside the mutex.
/// @param obj ConcurrentMap handle; null is ignored.
/// @param key String key to copy, or null.
/// @param value Runtime value to retain, or null.
void rt_concmap_set(void *obj, rt_string key, void *value);

/// @brief Look up and retain a value by key.
/// @param obj ConcurrentMap handle; null behaves as a miss.
/// @param key String key, or null for the empty key.
/// @return Retained stored value, or null when absent or explicitly null-valued.
void *rt_concmap_get(void *obj, rt_string key);

/// @brief Look up a retained value or pass through a caller-owned default.
/// @param obj ConcurrentMap handle; null returns @p default_value.
/// @param key String key, or null for the empty key.
/// @param default_value Borrowed fallback value.
/// @return Retained stored value on a hit; otherwise unchanged borrowed default.
void *rt_concmap_get_or(void *obj, rt_string key, void *default_value);

/// @brief Test whether a key exists regardless of its stored value.
/// @param obj ConcurrentMap handle; null behaves as empty.
/// @param key String key, or null for the empty key.
/// @return 1 when an entry exists, otherwise 0.
int8_t rt_concmap_has(void *obj, rt_string key);

/// @brief Atomically insert only if no equal key is present.
/// @param obj ConcurrentMap handle; null returns failure.
/// @param key String key to copy, or null for the empty key.
/// @param value Runtime value retained only on successful insertion.
/// @return 1 when inserted, or 0 when already present or invalid.
int8_t rt_concmap_set_if_missing(void *obj, rt_string key, void *value);

/// @brief Atomically remove an entry and release its storage after unlocking.
/// @param obj ConcurrentMap handle; null behaves as empty.
/// @param key String key, or null for the empty key.
/// @return 1 when removed, otherwise 0.
int8_t rt_concmap_remove(void *obj, rt_string key);

/// @brief Detach all entries atomically and release them after unlocking.
/// @param obj ConcurrentMap handle; null is ignored.
void rt_concmap_clear(void *obj);

/// @brief Snapshot all current keys in bucket-walk order.
/// @param obj ConcurrentMap handle; null yields an empty snapshot.
/// @return New owning sequence of copied key strings; order is unspecified.
void *rt_concmap_keys(void *obj);

/// @brief Snapshot all current values in bucket-walk order.
/// @param obj ConcurrentMap handle; null yields an empty snapshot.
/// @return New owning sequence containing retained value references; order is unspecified.
void *rt_concmap_values(void *obj);

#ifdef __cplusplus
}
#endif
