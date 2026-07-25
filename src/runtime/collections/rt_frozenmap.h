//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_frozenmap.h
// Purpose: Immutable string-keyed map created from existing Map or parallel key/value Seqs,
// providing O(1) average lookup with guaranteed read-only semantics after construction.
//
// Key invariants:
//   - Once created, the map cannot be modified; there are no put/remove operations.
//   - Keys are byte-length-aware strings; embedded NUL bytes are part of key identity.
//   - Lookup is O(1) average using the same hash strategy as the mutable Map.
//   - Constructed from parallel key/value Seqs, as empty, or by merging maps.
//   - rt_frozenmap_has returns 1 if key exists, 0 otherwise.
//
// Ownership/Lifetime:
//   - FrozenMap retains its keys and values; source collections are not consumed.
//   - FrozenMap objects are GC-managed (rt_obj_new_i64) with a runtime finalizer;
//     callers must not free them directly.
//
// Links: src/runtime/collections/rt_frozenmap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares immutable string-keyed map operations.
///
/// FrozenMap construction retains non-null key strings and opaque values in an
/// open-addressed table. Duplicate source keys use the final corresponding
/// value. No mutation API is exposed after construction, making completed maps
/// safe for concurrent read-only access.
///
/// Getter results are borrowed pointers. Key and value enumeration returns new
/// owning Seqs that retain their elements. Null map handles act as empty maps;
/// null key handles are skipped during construction and never match lookup.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a frozen map from parallel key and value Seqs.
/// @param keys Seq of raw or boxed string keys.
/// @param values Seq of corresponding object values.
/// @return New FrozenMap built from the shorter Seq length. Null extracted keys
///         are skipped and duplicate keys use the last value.
void *rt_frozenmap_from_seqs(void *keys, void *values);

/// @brief Create an empty frozen map.
/// @return New runtime-managed empty FrozenMap.
void *rt_frozenmap_empty(void);

/// @brief Get number of entries.
/// @param obj FrozenMap handle, or NULL.
/// @return Entry count, or zero for NULL.
int64_t rt_frozenmap_len(void *obj);

/// @brief Check if map is empty.
/// @param obj FrozenMap handle, or NULL.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_frozenmap_is_empty(void *obj);

/// @brief Get value for key.
/// @param obj FrozenMap handle, or NULL.
/// @param key Non-null string key.
/// @return Borrowed value pointer, or NULL if absent.
void *rt_frozenmap_get(void *obj, rt_string key);

/// @brief Check if key exists.
/// @param obj FrozenMap handle, or NULL.
/// @param key Non-null string key.
/// @return 1 if key exists, 0 otherwise.
int8_t rt_frozenmap_has(void *obj, rt_string key);

/// @brief Get all keys as a Seq.
/// @param obj FrozenMap handle, or NULL.
/// @return New owning Seq retaining all keys in unspecified slot order.
void *rt_frozenmap_keys(void *obj);

/// @brief Get all values as a Seq.
/// @param obj FrozenMap handle, or NULL.
/// @return New owning Seq retaining values in order parallel to
///         `rt_frozenmap_keys()`.
void *rt_frozenmap_values(void *obj);

/// @brief Get value for key or return a default.
/// @param obj FrozenMap handle, or NULL.
/// @param key Non-null string key.
/// @param default_value Borrowed fallback pointer.
/// @return Borrowed mapped value, or @p default_value if absent.
void *rt_frozenmap_get_or(void *obj, rt_string key, void *default_value);

/// @brief Merge two frozen maps. Second map's values win on conflict.
/// @param obj First FrozenMap, or NULL as an empty map.
/// @param other Second FrozenMap, or NULL as an empty map.
/// @return New independent FrozenMap retaining entries from both.
void *rt_frozenmap_merge(void *obj, void *other);

/// @brief Check if two frozen maps are equal (same key-value pairs).
/// @param obj First FrozenMap, or NULL as an empty map.
/// @param other Second FrozenMap, or NULL as an empty map.
/// @return 1 if equal, 0 otherwise.
int8_t rt_frozenmap_equals(void *obj, void *other);

#ifdef __cplusplus
}
#endif
