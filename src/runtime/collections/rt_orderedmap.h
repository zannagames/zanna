//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_orderedmap.h
// Purpose: Insertion-order preserving string-keyed map where iteration yields entries in the order
// they were first inserted, with O(1) average keyed access.
//
// Key invariants:
//   - Iteration order matches the order of first insertion.
//   - NULL keys are treated as the empty key; embedded NUL bytes are part of key identity.
//   - Updating an existing key does not change its position in iteration order.
//   - Keyed operations (Get/Set/Has/Remove) are O(1) average-case; KeyAt
//     walks the insertion-order list (O(n)), and snapshots/Clear are linear.
//   - Values are retained while stored in the map.
//
// Ownership/Lifetime:
//   - OrderedMap objects are GC-managed opaque pointers.
//   - Callers should not free orderedmap objects directly.
//
// Links: src/runtime/collections/rt_orderedmap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for the insertion-ordered string-keyed map.
/// @details Keys are copied by complete byte length and occupy the order of
///          their first insertion. Replacing a value leaves that order
///          unchanged. Keyed access is hash-backed; positional access and
///          snapshots traverse the stable insertion-order list.

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty ordered map.
/// @return New runtime/GC-managed OrderedMap object.
void *rt_orderedmap_new(void);

/// @brief Get the number of entries.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @return Number of distinct keys, or 0 for `NULL`.
int64_t rt_orderedmap_len(void *map);

/// @brief Check if the map is empty.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @return 1 if empty, otherwise 0; `NULL` is treated as empty.
int8_t rt_orderedmap_is_empty(void *map);

/// @brief Insert a new association or replace an existing value.
/// @details New keys append to iteration order; replacements retain their
///          original position. Key bytes are copied on first insertion, and
///          self-replacement is safe. A null map is a no-op.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @param value Runtime object retained while stored; may be `NULL`.
void rt_orderedmap_set(void *map, rt_string key, void *value);

/// @brief Get a value by key.
/// @details Returns a borrowed pointer retained by the map. Since `NULL` may
///          be stored, call @ref rt_orderedmap_has to distinguish absence.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return Borrowed value, or `NULL` for absence/a stored null.
void *rt_orderedmap_get(void *map, rt_string key);

/// @brief Check if a key exists.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return 1 if the exact byte-string key exists, otherwise 0.
int8_t rt_orderedmap_has(void *map, rt_string key);

/// @brief Remove a key-value pair.
/// @details Frees the copied key and releases its value while preserving the
///          relative order of all surviving entries.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return 1 if removed, otherwise 0.
int8_t rt_orderedmap_remove(void *map, rt_string key);

/// @brief Get all keys in insertion order as a Seq.
/// @details Each key is copied into a fresh string retained by the owning
///          result sequence.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @return New runtime-managed owning Seq; empty for `NULL`.
void *rt_orderedmap_keys(void *map);

/// @brief Get all values in insertion order as a Seq.
/// @details The fresh owning sequence independently retains non-null values
///          and preserves null-valued entries.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @return New runtime-managed owning Seq; empty for `NULL`.
void *rt_orderedmap_values(void *map);

/// @brief Get the key at a given index (insertion order).
/// @details Walks the order list in O(index) and returns a fresh key copy.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param index Zero-based index.
/// @return New runtime-managed key string, or `NULL` if out of range.
rt_string rt_orderedmap_key_at(void *map, int64_t index);

/// @brief Clear all entries.
/// @details Releases all retained values and copied keys while retaining the
///          current bucket allocation. A null map is a no-op.
/// @param map Opaque OrderedMap handle, or `NULL`.
void rt_orderedmap_clear(void *map);

#ifdef __cplusplus
}
#endif
