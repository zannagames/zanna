//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_defaultmap.h
// Purpose: String-keyed map that returns a configured default value for missing keys instead of
// NULL, providing safe access without explicit missing-key checks.
//
// Key invariants:
//   - rt_defaultmap_get returns the configured default for missing keys; the
//     result can still be NULL when the default is NULL or a stored value is
//     NULL.
//   - NULL keys are treated as the empty key; embedded NUL bytes are part of key identity.
//   - The default value is set at creation time and cannot be changed.
//   - Values are retained in the map; the default value is also retained.
//   - All operations are O(1) average-case.
//
// Ownership/Lifetime:
//   - DefaultMap objects are GC-managed opaque pointers.
//   - Values stored in the map are retained; callers should not free them while stored.
//
// Links: src/runtime/collections/rt_defaultmap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a string-keyed map with a configured fallback value.
///
/// Lookup distinguishes explicit presence from fallback behavior:
/// `rt_defaultmap_has()` reports whether a key is stored, while
/// `rt_defaultmap_get()` returns either that stored value or the fixed
/// constructor default. Both stored values and the default are retained and
/// traced by the map; getter results are borrowed pointers.
///
/// Key bytes are copied in full, including embedded NUL bytes, and a null
/// string denotes the empty key. DefaultMap objects are runtime-managed opaque
/// handles and are not safe for unsynchronized concurrent mutation.

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new default map with a default value.
/// @param default_value Value returned for missing keys; may be NULL and is
///        retained by the map.
/// @return A new runtime-managed DefaultMap, or NULL after construction fails.
void *rt_defaultmap_new(void *default_value);

/// @brief Get the number of entries.
/// @param map DefaultMap handle, or NULL to query an empty map.
/// @return Number of explicit mappings, or zero for NULL.
int64_t rt_defaultmap_len(void *map);

/// @brief Get value by key (returns default if missing).
/// @param map DefaultMap handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Borrowed stored value, borrowed default when absent, or NULL for a
///         null map.
void *rt_defaultmap_get(void *map, rt_string key);

/// @brief Set a key-value pair.
/// @param map DefaultMap handle, or NULL for a no-op.
/// @param key Key string to copy; NULL denotes the empty key.
/// @param value Value to retain, or NULL to store an explicit null mapping.
void rt_defaultmap_set(void *map, rt_string key, void *value);

/// @brief Check if a key exists (explicitly set, not just default).
/// @param map DefaultMap handle, or NULL to query an empty map.
/// @param key Key string; NULL denotes the empty key.
/// @return 1 if explicitly set, including to NULL; otherwise 0.
int8_t rt_defaultmap_has(void *map, rt_string key);

/// @brief Remove a key-value pair.
/// @param map DefaultMap handle, or NULL for a no-op.
/// @param key Key string; NULL denotes the empty key.
/// @return 1 if removed, 0 if not found.
int8_t rt_defaultmap_remove(void *map, rt_string key);

/// @brief Get all keys.
/// @param map DefaultMap handle, or NULL to enumerate an empty map.
/// @return New owning Seq of copied key strings in unspecified order.
void *rt_defaultmap_keys(void *map);

/// @brief Get the default value.
/// @param map DefaultMap handle, or NULL.
/// @return Borrowed default value, which may be NULL.
void *rt_defaultmap_get_default(void *map);

/// @brief Clear all entries.
/// @param map DefaultMap handle, or NULL for a no-op.
/// @note The configured default and bucket capacity are retained.
void rt_defaultmap_clear(void *map);

/// @brief Check if the map is empty.
/// @param map DefaultMap handle, or NULL to test an empty map.
/// @return 1 if no explicit mappings exist; otherwise 0.
int8_t rt_defaultmap_is_empty(void *map);

#ifdef __cplusplus
}
#endif
