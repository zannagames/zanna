//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_map.h
// Purpose: String-keyed hash map providing O(1) average insertion, lookup, removal, and iteration
// for the Zanna.Collections.Map runtime class.
//
// Key invariants:
//   - Keys are copied by the map; embedded NUL bytes are part of key identity.
//   - Values are retained on insertion and released on removal or overwrite.
//   - All operations are O(1) average-case using hashed buckets with
//     separate chaining (O(n) worst case within a chain).
//   - rt_map_get returns NULL for keys not present.
//
// Ownership/Lifetime:
//   - Map objects are GC-managed (rt_obj_new_i64) with a runtime finalizer;
//     callers must not free them directly.
//   - Values are retained while stored in the map.
//
// Error conventions:
//   - Allocation failure → returns NULL
//   - Key not found (Get) → returns NULL
//   - Removal not found (Remove) → returns 0
//
// Links: src/runtime/collections/rt_map.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the runtime's retained-value string Map API.
///
/// Map copies complete key byte strings and retains opaque mapped values.
/// Generic getters return borrowed pointers; key and value enumeration returns
/// owning snapshots. Typed wrappers box primitive values on insertion and
/// perform explicit numeric, boolean, or string conversion on retrieval.
///
/// Null strings denote the empty key. Map objects are runtime-managed opaque
/// handles and are not safe for unsynchronized concurrent mutation.

#pragma once

#include <stdint.h>

#include "rt_collection_ids.h"

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty map.
/// @return New runtime-managed Map, or NULL on allocation failure.
void *rt_map_new(void);

/// @brief Get number of entries in map.
/// @param obj Map pointer.
/// @return Entry count.
int64_t rt_map_len(void *obj);

/// @brief Check if map is empty.
/// @param obj Map pointer.
/// @return 1 if empty, 0 otherwise.
int8_t rt_map_is_empty(void *obj);

/// @brief Set or update a key-value pair.
/// @param obj Map pointer.
/// @param key String key (will be copied).
/// @param value Object value (will be retained).
void rt_map_set(void *obj, rt_string key, void *value);

/// @brief Get value for key.
/// @param obj Map pointer.
/// @param key String key.
/// @return Value pointer or NULL if not found.
void *rt_map_get(void *obj, rt_string key);

/// @brief Get value for key or return a default when missing.
/// @details Does not mutate the map: missing keys do not create new entries.
/// @param obj Map pointer.
/// @param key String key.
/// @param default_value Value to return when @p key is not present.
/// @return Existing value when present; otherwise @p default_value.
void *rt_map_get_or(void *obj, rt_string key, void *default_value);

/// @brief Check if key exists.
/// @param obj Map pointer.
/// @param key String key.
/// @return 1 if key exists, 0 otherwise.
int8_t rt_map_has(void *obj, rt_string key);

/// @brief Insert @p value for @p key only when the key is missing.
/// @details When @p key is already present, leaves the map unchanged and returns 0.
/// @param obj Map pointer.
/// @param key String key (will be copied when inserted).
/// @param value Object value (will be retained when inserted).
/// @return 1 when an entry was inserted, 0 otherwise.
int8_t rt_map_set_if_missing(void *obj, rt_string key, void *value);

/// @brief Remove entry by key.
/// @param obj Map pointer.
/// @param key String key.
/// @return 1 if removed, 0 if key not found.
int8_t rt_map_remove(void *obj, rt_string key);

/// @brief Remove all entries from map.
/// @param obj Map pointer.
void rt_map_clear(void *obj);

/// @brief Compact the bucket table to the minimum capacity for current entries.
/// @details Unlike @ref rt_map_clear and @ref rt_map_remove, this operation may
///          allocate. It preserves every key/value pair and leaves the old
///          table unchanged if replacement allocation fails.
/// @param obj Map pointer.
/// @return 1 when already compact or successfully trimmed; 0 after a trap.
int8_t rt_map_trim(void *obj);

/// @brief Get all keys as a Seq.
/// @param obj Map pointer.
/// @return New Seq containing all keys as strings.
void *rt_map_keys(void *obj);

/// @brief Get all values as a Seq.
/// @param obj Map pointer.
/// @return New Seq containing all values.
void *rt_map_values(void *obj);

//=========================================================================
// Typed Accessors (box/unbox wrappers)
//=========================================================================

/// @brief Set an integer value (boxes automatically).
/// @param obj Map handle, or NULL for a no-op.
/// @param key Key string; NULL denotes the empty key.
/// @param value Integer to box and store.
void rt_map_set_int(void *obj, rt_string key, int64_t value);

/// @brief Get an integer value (unboxes, returns 0 if missing).
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Numeric value converted to integer, or zero if absent.
int64_t rt_map_get_int(void *obj, rt_string key);

/// @brief Get an integer value with default (unboxes, returns def if missing).
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @param def Fallback for absence or non-numeric values.
/// @return Converted integer or @p def.
int64_t rt_map_get_int_or(void *obj, rt_string key, int64_t def);

/// @brief Set a float value (boxes automatically).
/// @param obj Map handle, or NULL for a no-op.
/// @param key Key string; NULL denotes the empty key.
/// @param value Floating-point value to box and store.
void rt_map_set_float(void *obj, rt_string key, double value);

/// @brief Get a float value (unboxes, returns 0.0 if missing).
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Numeric value converted to double, or 0.0 if absent.
double rt_map_get_float(void *obj, rt_string key);

/// @brief Get a float value with default (unboxes, returns def if missing).
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @param def Fallback for absence or non-numeric values.
/// @return Converted double or @p def.
double rt_map_get_float_or(void *obj, rt_string key, double def);

/// @brief Set a boolean value (boxes automatically).
/// @param obj Map handle, or NULL for a no-op.
/// @param key Key string; NULL denotes the empty key.
/// @param value Boolean value to normalize, box, and store.
void rt_map_set_bool(void *obj, rt_string key, int8_t value);

/// @brief Get a boolean value (unboxes, returns false if missing).
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Boolean or numeric truth value, or zero if absent.
int8_t rt_map_get_bool(void *obj, rt_string key);

/// @brief Get a boolean value with default (unboxes, returns def if missing).
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @param def Fallback for absence or unsupported stored types.
/// @return Converted truth value or @p def.
int8_t rt_map_get_bool_or(void *obj, rt_string key, int8_t def);

/// @brief Set a string value (wraps as object).
/// @param obj Map handle, or NULL for a no-op.
/// @param key Key string; NULL denotes the empty key.
/// @param value Runtime string to retain; may be NULL.
void rt_map_set_str(void *obj, rt_string key, rt_string value);

/// @brief Get a string value (returns empty string if missing).
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Caller-owned runtime string; non-string stored values trap.
rt_string rt_map_get_str(void *obj, rt_string key);

/// @brief Get an optional string value (returns NULL if missing).
/// @param obj Map handle, or NULL.
/// @param key Key string; NULL denotes the empty key.
/// @return Caller-owned runtime string, or NULL if absent.
rt_string rt_map_get_opt_str(void *obj, rt_string key);

/// @brief Create a shallow copy of the map.
/// @param obj Source Map pointer.
/// @return New map with same key-value pairs.
void *rt_map_clone(void *obj);

#ifdef __cplusplus
}
#endif
