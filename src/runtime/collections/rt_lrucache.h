//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_lrucache.h
// Purpose: String-keyed LRU (Least Recently Used) cache evicting the least-recently-accessed entry
// when capacity is exceeded, providing O(1) average get/put operations.
//
// Key invariants:
//   - Keys are byte-length-aware strings; embedded NUL bytes are part of key identity.
//   - The least-recently-used entry is evicted when capacity is exceeded.
//   - Get and Put count as use and update recency; Has is a read-only hash
//     lookup and does not promote the entry.
//   - A capacity of 0 disables eviction (the cache grows unbounded).
//
// Ownership/Lifetime:
//   - Cache objects are runtime-managed and participate in GC traversal.
//   - String keys are copied into the cache. Values are retained by the cache.
//
// Links: src/runtime/collections/rt_lrucache.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a retained-value least-recently-used cache.
///
/// LRUCache combines byte-string lookup with a most-recently-used to
/// least-recently-used ordering. Put and get promote entries, while peek, has,
/// and enumeration do not. Bounded caches evict the tail only after a new
/// entry has been prepared successfully; zero capacity disables eviction.
///
/// Keys are copied and values retained. Get and peek return borrowed pointers;
/// key/value snapshots own retained results. LRUCache objects are
/// runtime-managed and are not safe for unsynchronized concurrent mutation.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new LRU cache with the given maximum capacity.
/// @param capacity Maximum entries; zero creates an unbounded cache and
///        negative values trap.
/// @return New runtime-managed cache, or NULL after construction fails.
void *rt_lrucache_new(int64_t capacity);

/// @brief Get number of entries currently in the cache.
/// @param obj Cache handle, or NULL.
/// @return Entry count, or zero for NULL.
int64_t rt_lrucache_len(void *obj);

/// @brief Get the maximum capacity of the cache.
/// @param obj Cache handle, or NULL.
/// @return Fixed eviction limit; zero means unbounded or NULL.
int64_t rt_lrucache_cap(void *obj);

/// @brief Check if cache is empty.
/// @param obj Cache handle, or NULL.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_lrucache_is_empty(void *obj);

/// @brief Insert or update a key-value pair. Promotes to most-recently-used.
/// @param obj Cache handle, or NULL for a no-op.
/// @param key String key to copy; NULL denotes the empty key.
/// @param value Object value to retain; may be NULL.
/// @note If cache is at capacity and key is new, evicts the LRU entry.
void rt_lrucache_put(void *obj, rt_string key, void *value);

/// @brief Get value for key. Promotes the entry to most-recently-used.
/// @param obj Cache handle, or NULL.
/// @param key String key; NULL denotes the empty key.
/// @return Borrowed value pointer or NULL if absent.
void *rt_lrucache_get(void *obj, rt_string key);

/// @brief Get value for key without promoting it in the LRU order.
/// @param obj Cache handle, or NULL.
/// @param key String key; NULL denotes the empty key.
/// @return Borrowed value pointer or NULL if absent.
void *rt_lrucache_peek(void *obj, rt_string key);

/// @brief Check if key exists in the cache.
/// @param obj Cache handle, or NULL.
/// @param key String key; NULL denotes the empty key.
/// @return 1 if explicitly cached, including with a NULL value; otherwise 0.
int8_t rt_lrucache_has(void *obj, rt_string key);

/// @brief Remove entry by key.
/// @param obj Cache handle, or NULL.
/// @param key String key; NULL denotes the empty key.
/// @return 1 if removed, 0 if key not found.
int8_t rt_lrucache_remove(void *obj, rt_string key);

/// @brief Remove the least-recently-used entry.
/// @param obj Cache handle, or NULL.
/// @return 1 if an entry was removed, 0 if cache was empty.
int8_t rt_lrucache_remove_oldest(void *obj);

/// @brief Remove all entries from cache.
/// @param obj Cache handle, or NULL for a no-op.
/// @note The configured limit and hash bucket count are retained.
void rt_lrucache_clear(void *obj);

/// @brief Get all keys as a Seq, ordered from most to least recently used.
/// @param obj Cache handle, or NULL.
/// @return New owning Seq containing copied key strings.
void *rt_lrucache_keys(void *obj);

/// @brief Get all values as a Seq, ordered from most to least recently used.
/// @param obj Cache handle, or NULL.
/// @return New owning Seq retaining all values.
void *rt_lrucache_values(void *obj);

#ifdef __cplusplus
}
#endif
