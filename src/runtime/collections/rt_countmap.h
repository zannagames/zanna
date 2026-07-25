//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_countmap.h
// Purpose: Frequency counting map from string keys to non-negative integer counts, supporting
// increment, decrement, get, top-N queries, and iteration over counted items.
//
// Key invariants:
//   - Counts are always >= 0; decrementing a count to 0 removes the entry.
//   - Keys are copied by byte length; embedded NUL bytes are part of key identity.
//   - Count and total overflow trap instead of wrapping.
//   - rt_countmap_get returns 0 for keys not present.
//   - Keyed operations are O(1) average-case; rt_countmap_most_common sorts
//     and is O(n log n).
//   - rt_countmap_most_common returns up to N keys with highest counts.
//
// Ownership/Lifetime:
//   - CountMap objects are GC-managed (rt_obj_new_i64) with a runtime
//     finalizer; callers must not free them directly.
//   - String keys are copied internally; caller retains ownership of input strings.
//
// Links: src/runtime/collections/rt_countmap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a string-keyed frequency-counting map.
///
/// CountMap stores only positive signed 64-bit counts and maintains their sum.
/// Keys are copied using complete runtime byte lengths, so embedded NUL bytes
/// participate in identity and a null string denotes the empty key. Assigning
/// or decrementing to zero removes the mapping.
///
/// Snapshot operations return owning Seqs of newly created key strings.
/// CountMap objects are runtime-managed opaque handles. Invalid non-null
/// handles and arithmetic, capacity, or allocation failures raise runtime
/// traps; null handles follow the per-operation behavior below.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty CountMap.
/// @return A new runtime-managed CountMap, or NULL after an allocation trap.
void *rt_countmap_new(void);

/// @brief Get number of distinct keys.
/// @param obj CountMap handle, or NULL to query an empty map.
/// @return Number of keys with count greater than zero.
int64_t rt_countmap_len(void *obj);

/// @brief Check if countmap is empty.
/// @param obj CountMap handle, or NULL to test an empty map.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_countmap_is_empty(void *obj);

/// @brief Increment count for key by 1.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key String key; NULL denotes the empty string.
/// @return Post-increment count, or zero after a failed operation.
int64_t rt_countmap_inc(void *obj, rt_string key);

/// @brief Increment count for key by a specific amount.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key String key; NULL denotes the empty string.
/// @param n Non-negative amount. Zero performs a lookup; negative values trap.
/// @return Post-operation count.
int64_t rt_countmap_inc_by(void *obj, rt_string key, int64_t n);

/// @brief Decrement count for key by 1. Removes entry if count reaches 0.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key String key; NULL denotes the empty string.
/// @return New count, or zero when absent or removed.
int64_t rt_countmap_dec(void *obj, rt_string key);

/// @brief Get current count for key.
/// @param obj CountMap handle, or NULL to query an empty map.
/// @param key String key; NULL denotes the empty string.
/// @return Count for key, or 0 if not present.
int64_t rt_countmap_get(void *obj, rt_string key);

/// @brief Set count for key directly.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key String key; NULL denotes the empty string.
/// @param count New count; non-positive values remove the entry.
void rt_countmap_set(void *obj, rt_string key, int64_t count);

/// @brief Check if key has a count > 0.
/// @param obj CountMap handle, or NULL to query an empty map.
/// @param key String key; NULL denotes the empty string.
/// @return 1 if key exists, 0 otherwise.
int8_t rt_countmap_has(void *obj, rt_string key);

/// @brief Get total of all counts.
/// @param obj CountMap handle, or NULL to query an empty map.
/// @return Sum of all counts, or zero for NULL.
int64_t rt_countmap_total(void *obj);

/// @brief Get all keys as a Seq.
/// @param obj CountMap handle, or NULL to enumerate an empty map.
/// @return New owning Seq of copied keys in unspecified order.
void *rt_countmap_keys(void *obj);

/// @brief Get top N keys by count (descending).
/// @param obj CountMap handle, or NULL to rank an empty map.
/// @param n Maximum number of keys; non-positive values produce an empty Seq.
/// @return New owning Seq of copied keys sorted by descending count.
/// @note Equal-count keys have unspecified relative order.
void *rt_countmap_most_common(void *obj, int64_t n);

/// @brief Remove a key entirely.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key String key; NULL denotes the empty string.
/// @return 1 if removed, 0 if not found.
int8_t rt_countmap_remove(void *obj, rt_string key);

/// @brief Remove all entries while retaining bucket capacity.
/// @param obj CountMap handle, or NULL for a no-op.
void rt_countmap_clear(void *obj);

#ifdef __cplusplus
}
#endif
