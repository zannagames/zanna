//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_multimap.h
// Purpose: String-keyed multimap supporting multiple values per key, returning all values for a key
// as a Seq, with O(1) average access.
//
// Key invariants:
//   - Each key maps to a sequence of values; duplicate keys are supported.
//   - Keys are copied by byte length; embedded NUL bytes are part of key identity.
//   - rt_multimap_get returns an owning snapshot Seq of all values for the key (empty Seq if
//   absent).
//   - Values are retained when added and in returned snapshots; released when removed.
//   - Removing a key removes all its associated values.
//
// Ownership/Lifetime:
//   - MultiMap objects are runtime/GC-managed.
//   - String keys are copied internally. Values are retained by per-key owning
//     sequences and by any returned snapshots.
//
// Links: src/runtime/collections/rt_multimap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for the runtime string-keyed MultiMap collection.
/// @details A MultiMap associates each copied byte-string key with an
///          insertion-ordered sequence of values. Duplicate and `NULL` values
///          are preserved, `Len` counts all values, `KeyCount` counts distinct
///          keys, and read APIs never expose the internal per-key sequence.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty MultiMap.
/// @details The runtime manages the object lifetime and traverses every
///          internal value sequence for garbage collection.
/// @return New runtime-managed MultiMap, or `NULL` after an allocation trap.
void *rt_multimap_new(void);

/// @brief Get total number of values across all keys.
/// @details Duplicate and `NULL` values each contribute one. An invalid
///          non-null handle traps.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @return Aggregate value count, or 0 for `NULL`.
int64_t rt_multimap_len(void *obj);

/// @brief Get number of distinct keys.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @return Unique byte-string key count, or 0 for `NULL`.
int64_t rt_multimap_key_count(void *obj);

/// @brief Check if multimap is empty.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @return 1 when no values are stored, otherwise 0; `NULL` is empty.
int8_t rt_multimap_is_empty(void *obj);

/// @brief Add a value to the given key's list.
/// @details Appends in insertion order and preserves duplicate values. A new
///          key is copied by complete byte length, including embedded NUL
///          bytes. A null map is a no-op.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @param value Runtime object retained by the map; may be `NULL`.
void rt_multimap_put(void *obj, rt_string key, void *value);

/// @brief Get all values for a key as a Seq.
/// @details The returned owning snapshot preserves insertion order,
///          independently retains its values, and may be mutated without
///          changing the MultiMap.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return New caller-owned `Seq`; empty when the map or key is absent.
void *rt_multimap_get(void *obj, rt_string key);

/// @brief Get the first value for a key.
/// @details Returns the earliest inserted value with a caller-owned retain.
///          Because stored `NULL` values are valid, `NULL` is ambiguous
///          between absence and a null first value.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return Retained first value, or `NULL` when unavailable/null-valued.
void *rt_multimap_get_first(void *obj, rt_string key);

/// @brief Check if key exists.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return 1 if the exact byte-string key exists, otherwise 0.
int8_t rt_multimap_has(void *obj, rt_string key);

/// @brief Get count of values for a key.
/// @details Duplicate and `NULL` values are counted independently.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return Number of values for that key, or 0 when absent.
int64_t rt_multimap_count_for(void *obj, rt_string key);

/// @brief Remove all values for a key.
/// @details Releases the key's complete owning value sequence and copied key;
///          there is no single-value removal operation.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return 1 if key existed and was removed, 0 if not found.
int8_t rt_multimap_remove_all(void *obj, rt_string key);

/// @brief Remove all entries from multimap.
/// @details Releases all copied keys and retained values but keeps the bucket
///          allocation for later reuse. A null map is a no-op.
/// @param obj Opaque MultiMap handle, or `NULL`.
void rt_multimap_clear(void *obj);

/// @brief Get all keys as a Seq.
/// @details Keys are copied into an owning result sequence. Order follows
///          bucket iteration and is not stable or insertion ordered.
/// @param obj Opaque MultiMap handle, or `NULL`.
/// @return New caller-owned `Seq` containing every distinct key.
void *rt_multimap_keys(void *obj);

#ifdef __cplusplus
}
#endif
