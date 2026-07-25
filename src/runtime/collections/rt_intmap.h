//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_intmap.h
// Purpose: Integer-keyed hash map mapping int64_t keys to object pointer values, providing O(1)
// average insertion, lookup, and removal.
//
// Key invariants:
//   - Keys are int64_t stored directly without boxing.
//   - Values are opaque object pointers; the map retains them.
//   - All operations are O(1) average-case.
//   - rt_intmap_get returns NULL for keys not present.
//
// Ownership/Lifetime:
//   - IntMap objects are runtime-managed and participate in GC traversal.
//   - Values are retained by the map; caller manages external references separately.
//
// Links: src/runtime/collections/rt_intmap.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a retained-value map keyed by signed 64-bit integers.
///
/// IntMap stores integer keys directly and associates them with retained opaque
/// runtime values. Lookup, insertion, and removal are average constant-time.
/// Explicit trimming can reduce unused bucket capacity without changing entries.
///
/// Getter results are borrowed. Key enumeration returns newly boxed integers;
/// value enumeration returns a Seq retaining mapped pointers. IntMap objects
/// are runtime-managed opaque handles and are not safe for unsynchronized
/// concurrent mutation.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty integer-keyed map.
/// @return New runtime-managed IntMap, or NULL after an allocation trap.
void *rt_intmap_new(void);

/// @brief Get number of entries in intmap.
/// @param obj IntMap handle, or NULL.
/// @return Entry count, or zero for NULL.
int64_t rt_intmap_len(void *obj);

/// @brief Check if intmap is empty.
/// @param obj IntMap handle, or NULL.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_intmap_is_empty(void *obj);

/// @brief Set or update a key-value pair.
/// @param obj IntMap handle, or NULL for a no-op.
/// @param key Integer key.
/// @param value Object value to retain; may be NULL.
void rt_intmap_set(void *obj, int64_t key, void *value);

/// @brief Get value for key.
/// @param obj IntMap handle, or NULL.
/// @param key Integer key.
/// @return Borrowed value pointer or NULL if absent.
void *rt_intmap_get(void *obj, int64_t key);

/// @brief Get value for key or return a default when missing.
/// @param obj IntMap handle, or NULL.
/// @param key Integer key.
/// @param default_value Value to return when key is not present.
/// @return Borrowed existing value when present; otherwise @p default_value.
void *rt_intmap_get_or(void *obj, int64_t key, void *default_value);

/// @brief Check if key exists.
/// @param obj IntMap handle, or NULL.
/// @param key Integer key.
/// @return 1 if key exists, 0 otherwise.
int8_t rt_intmap_has(void *obj, int64_t key);

/// @brief Remove entry by key.
/// @param obj IntMap handle, or NULL.
/// @param key Integer key.
/// @return 1 if removed, 0 if key not found.
int8_t rt_intmap_remove(void *obj, int64_t key);

/// @brief Remove all entries from intmap.
/// @param obj IntMap handle, or NULL for a no-op.
/// @note Stored values are released and bucket capacity is retained.
void rt_intmap_clear(void *obj);

/// @brief Compact the bucket table to the minimum capacity for current entries.
/// @details Allocates and installs a smaller table transactionally. Existing
///          keys and retained values remain unchanged if allocation fails.
/// @param obj Non-null IntMap handle.
/// @return 1 when already compact or successfully trimmed; 0 after a trap.
int8_t rt_intmap_trim(void *obj);

/// @brief Get all keys as a Seq of boxed integers.
/// @param obj IntMap handle, or NULL.
/// @return New owning Seq containing newly boxed i64 keys in unspecified order.
void *rt_intmap_keys(void *obj);

/// @brief Get all values as a Seq.
/// @param obj IntMap handle, or NULL.
/// @return New owning Seq retaining mapped values in unspecified order.
void *rt_intmap_values(void *obj);

#ifdef __cplusplus
}
#endif
