//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_countmap.c
// Purpose: Implements a frequency-counting map from string keys to int64 counts.
//   Provides increment, decrement, set, get, and "top-N by count" operations.
//   Maintains a running total of all counts for efficient average/total queries.
//   Typical uses: word frequency, event tallying, histogram building, and any
//   scenario where distinct string occurrences need to be counted.
//
// Key invariants:
//   - Backed by a hash table with initial capacity CM_INITIAL_CAPACITY (16)
//     buckets and separate chaining with FNV-1a hashing.
//   - Resizes (doubles) when distinct-key count/capacity exceeds 75%.
//   - `total` tracks the sum of all stored counts and is updated atomically
//     with every increment, decrement, and set operation.
//   - Decrementing to zero removes the entry (count and total adjusted).
//   - Removing an entry (count set to 0 via Remove) frees the entry node and
//     adjusts both `count` (distinct keys) and `total`.
//   - All operations are O(1) average case; O(n) worst case due to chaining.
//   - Not thread-safe; external synchronization required for concurrent access.
//
// Ownership/Lifetime:
//   - CountMap objects are GC-managed (rt_obj_new_i64). The bucket array and
//     all entry nodes are freed by the GC finalizer.
//
// Links: src/runtime/collections/rt_countmap.h (public API),
//        src/runtime/collections/rt_hash_util.h (FNV-1a hash macro)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a hash-based frequency map from strings to counts.
///
/// CountMap owns copied key bytes and associates each distinct key with a
/// strictly positive signed 64-bit count. A running total permits constant-time
/// aggregation. Decrementing or assigning a non-positive count removes the
/// mapping, so stored entries never represent zero.
///
/// Keys use their complete runtime byte lengths, including embedded NUL bytes;
/// a null string denotes the empty key. Snapshot and ranking operations return
/// owning Seqs of newly created strings. CountMap objects are runtime-managed,
/// and mutation is unsynchronized.

#include "rt_countmap.h"

#include "rt_collection_ids.h"
#include "rt_hash_table_util.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Initial number of hash buckets.
#define CM_INITIAL_CAPACITY 16
#include "rt_hash_util.h"

/// @brief One owned key/count entry in a bucket collision chain.
typedef struct rt_cm_entry {
    char *key;
    size_t key_len;
    int64_t count;
    struct rt_cm_entry *next;
} rt_cm_entry;

/// @brief Internal CountMap state and aggregate counters.
typedef struct rt_countmap_impl {
    void **vptr;
    rt_cm_entry **buckets;
    size_t capacity;
    size_t count;  // distinct keys
    int64_t total; // sum of all counts
} rt_countmap_impl;

/// @brief Checked cast of an opaque handle to the CountMap implementation.
/// @details Traps with @p what if @p obj is NULL or not a CountMap.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used on validation failure.
/// @return The validated implementation pointer, or NULL if a returning trap
///         handler resumes after failed validation.
static rt_countmap_impl *as_countmap(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_COUNTMAP_CLASS_ID, sizeof(rt_countmap_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_countmap_impl *)obj;
}

/// @brief Borrow the byte buffer + length of an rt_string (empty "" if null).
/// @param s Runtime string to inspect; NULL denotes an empty key.
/// @param what Diagnostic raised for a stale or forged nonnull handle.
/// @param out_data Receives borrowed key bytes.
/// @param out_len Receives the usable byte length.
/// @return Nonzero for a null or live string handle; otherwise zero after
///         trapping. Null continues to denote the empty key.
static int get_str_data(rt_string s, const char *what, const char **out_data, size_t *out_len) {
    *out_data = "";
    *out_len = 0;
    if (!s) {
        return 1;
    }
    if (!rt_string_is_handle(s)) {
        rt_trap(what);
        return 0;
    }
    int64_t len = rt_str_len(s);
    if (len <= 0)
        return 1;
    const char *cstr = rt_string_cstr(s);
    if (!cstr) {
        rt_trap(what);
        return 0;
    }
    *out_data = cstr;
    *out_len = (size_t)len;
    return 1;
}

/// @brief Linear scan of a bucket chain for an exact key match (NULL if none).
/// @param head First entry in the bucket chain.
/// @param key Key bytes to compare.
/// @param key_len Number of bytes in @p key.
/// @return Matching entry, or NULL if no chain entry has the same byte string.
static rt_cm_entry *find_entry(rt_cm_entry *head, const char *key, size_t key_len) {
    for (rt_cm_entry *e = head; e; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

/// @brief Free a chain entry and its owned key buffer (NULL-safe).
/// @param entry Entry to destroy, or NULL for a no-op.
static void free_entry(rt_cm_entry *entry) {
    if (!entry)
        return;
    free(entry->key);
    free(entry);
}

/// @brief Release a runtime-managed object reference at zero.
/// @param obj Nullable runtime object to release.
static void countmap_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copy the active trap diagnostic into stable local storage.
/// @param output Destination buffer.
/// @param capacity Size of @p output including its terminator.
/// @param fallback Diagnostic used when no current trap text exists.
static void countmap_save_trap(char *output, size_t capacity, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(output, capacity, "%s", error && error[0] ? error : fallback);
}

/// @brief Copy and transfer a key into an owning snapshot.
/// @details On a trap, releases the temporary string, partial sequence, and
///          optional native scratch allocation before re-raising.
/// @param seq Owning Seq to append to; consumed on failure.
/// @param key Key bytes to copy.
/// @param key_len Number of bytes in @p key.
/// @param scratch Optional native allocation consumed only on failure.
/// @param fallback Diagnostic used when no active trap message exists.
/// @return One after a checked append; zero after cleanup and retrapping.
static int countmap_append_key_or_release(
    void *seq, const char *key, size_t key_len, void *scratch, const char *fallback) {
    rt_string volatile copy = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        countmap_save_trap(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        if (copy)
            rt_str_release_maybe((rt_string)copy);
        free(scratch);
        countmap_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    copy = rt_string_from_bytes(key, key_len);
    if (!copy)
        rt_trap(fallback);
    int64_t old_len = rt_seq_len(seq);
    rt_seq_push_raw(seq, (void *)copy);
    if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len(seq) != old_len + 1)
        rt_trap(fallback);
    copy = NULL;
    rt_trap_clear_recovery();
    return 1;
}

/// @brief GC finalizer: free every chain entry, the bucket array, and reset.
/// @param obj CountMap object being finalized, or NULL for a no-op.
static void countmap_finalizer(void *obj) {
    if (!obj)
        return;
    rt_countmap_impl *cm = as_countmap(obj, "CountMap: invalid CountMap object");
    if (!cm)
        return;

    if (cm->buckets) {
        for (size_t i = 0; i < cm->capacity; ++i) {
            rt_cm_entry *e = cm->buckets[i];
            while (e) {
                rt_cm_entry *next = e->next;
                free_entry(e);
                e = next;
            }
        }
    }
    free(cm->buckets);
    cm->buckets = NULL;
    cm->capacity = 0;
    cm->count = 0;
    cm->total = 0;
}

/// @brief Double the bucket array and rehash all entries into it.
/// @details No-op past the SIZE_MAX/2 cap; traps on allocation overflow/OOM.
/// @param cm CountMap whose bucket table is to grow.
/// @return 1 after installing the larger table, or 0 when capacity cannot
///         double or allocation fails. The original table remains installed.
static int resize(rt_countmap_impl *cm) {
    if (cm->capacity > SIZE_MAX / 2) {
        rt_trap("CountMap: capacity overflow during resize");
        return 0;
    }
    size_t new_cap = cm->capacity * 2;
    if (new_cap > SIZE_MAX / sizeof(rt_cm_entry *)) {
        rt_trap("CountMap: allocation size overflow");
        return 0;
    }
    rt_cm_entry **new_buckets = (rt_cm_entry **)calloc(new_cap, sizeof(rt_cm_entry *));
    if (!new_buckets) {
        rt_trap("CountMap: memory allocation failed");
        return 0;
    }

    for (size_t i = 0; i < cm->capacity; ++i) {
        rt_cm_entry *e = cm->buckets[i];
        while (e) {
            rt_cm_entry *next = e->next;
            uint64_t h = rt_fnv1a(e->key, e->key_len);
            size_t idx = (size_t)(h % new_cap);
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }

    free(cm->buckets);
    cm->buckets = new_buckets;
    cm->capacity = new_cap;
    return 1;
}

/// @brief True when the load factor (count/capacity) reaches the grow
///        threshold CM_LOAD_FACTOR_NUM/CM_LOAD_FACTOR_DEN.
/// @param cm CountMap whose current occupancy is inspected.
/// @return Non-zero when the next new-key insertion should first grow the
///         bucket table.
static int should_resize(rt_countmap_impl *cm) {
    return cm->count == SIZE_MAX || rt_hash_table_exceeds_load(cm->count + 1, cm->capacity);
}

/// @brief Construct an empty count map (string → int64). Designed for tally workloads —
/// histogram counting, frequency tables, vote totals. Internal storage is a chained hash table.
/// @return A new runtime-managed CountMap, or NULL after reporting an
///         allocation trap if construction cannot complete.
void *rt_countmap_new(void) {
    rt_countmap_impl *cm =
        (rt_countmap_impl *)rt_obj_new_i64(RT_COUNTMAP_CLASS_ID, sizeof(rt_countmap_impl));
    if (!cm) {
        rt_trap("CountMap: memory allocation failed");
        return NULL;
    }

    cm->vptr = NULL;
    cm->capacity = CM_INITIAL_CAPACITY;
    cm->count = 0;
    cm->total = 0;
    cm->buckets = (rt_cm_entry **)calloc(CM_INITIAL_CAPACITY, sizeof(rt_cm_entry *));
    if (!cm->buckets) {
        if (rt_obj_release_check0(cm))
            rt_obj_free(cm);
        rt_trap("CountMap: memory allocation failed");
        return NULL;
    }

    rt_obj_set_finalizer(cm, countmap_finalizer);
    return cm;
}

/// @brief Returns the number of distinct keys with positive counts.
/// @param obj CountMap handle, or NULL to query an empty map.
/// @return Distinct-key count, or zero when @p obj is NULL.
int64_t rt_countmap_len(void *obj) {
    if (!obj)
        return 0;
    rt_countmap_impl *map = as_countmap(obj, "CountMap.Len: invalid CountMap object");
    return map ? (int64_t)map->count : 0;
}

/// @brief Tests whether a CountMap contains no stored keys.
/// @param obj CountMap handle, or NULL to test an empty map.
/// @return 1 when empty or NULL; otherwise 0.
int8_t rt_countmap_is_empty(void *obj) {
    return rt_countmap_len(obj) == 0 ? 1 : 0;
}

/// @brief Increments one key's count by one.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key Key to increment; NULL denotes the empty string.
/// @return The post-increment count, or zero if @p obj is NULL or the
///         operation cannot complete after a trap.
int64_t rt_countmap_inc(void *obj, rt_string key) {
    return rt_countmap_inc_by(obj, key, 1);
}

/// @brief Adds a non-negative amount to one key's count.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key Key to update; NULL denotes the empty string.
/// @param n Amount to add. Zero performs a lookup without inserting; negative
///        values raise a runtime trap.
/// @return The key's post-operation count. Overflow or allocation failures
///         trap and leave the prior mapping unchanged.
int64_t rt_countmap_inc_by(void *obj, rt_string key, int64_t n) {
    if (!obj)
        return 0;
    // The return is always the post-operation count (VDOC-099): a negative
    // amount traps (use Decrement to go down), and zero is a lookup no-op.
    if (n < 0) {
        rt_trap("CountMap.IncrementBy: amount must be >= 0");
        return 0;
    }
    rt_countmap_impl *cm = as_countmap(obj, "CountMap.IncBy: invalid CountMap object");
    if (!cm)
        return 0;
    if (cm->capacity == 0) {
        rt_trap("CountMap: finalized CountMap object");
        return 0;
    }

    size_t klen = 0;
    const char *kdata = NULL;
    if (!get_str_data(key, "CountMap.IncrementBy: invalid key", &kdata, &klen))
        return 0;

    uint64_t h = rt_fnv1a(kdata, klen);
    size_t idx = (size_t)(h % cm->capacity);

    rt_cm_entry *e = find_entry(cm->buckets[idx], kdata, klen);
    if (n == 0)
        return e ? e->count : 0;
    if (e) {
        if (n > INT64_MAX - e->count || n > INT64_MAX - cm->total) {
            rt_trap("CountMap: count overflow");
            return e->count;
        }
        e->count += n;
        cm->total += n;
        return e->count;
    }

    if (n > INT64_MAX - cm->total) {
        rt_trap("CountMap: total overflow");
        return 0;
    }
    if (cm->count >= (size_t)INT64_MAX) {
        rt_trap("CountMap: length overflow");
        return 0;
    }

    // New entry. Reject deterministic arithmetic limits before performing a
    // fallible representation-only resize.
    if (should_resize(cm)) {
        if (!resize(cm))
            return 0;
        idx = (size_t)(h % cm->capacity);
    }

    e = (rt_cm_entry *)malloc(sizeof(rt_cm_entry));
    if (!e) {
        rt_trap("CountMap: memory allocation failed");
        return 0;
    }
    if (klen == SIZE_MAX) {
        free(e);
        rt_trap("CountMap: key allocation overflow");
        return 0;
    }
    e->key = (char *)malloc(klen + 1);
    if (!e->key) {
        free(e);
        rt_trap("CountMap: key allocation failed");
        return 0;
    }
    memcpy(e->key, kdata, klen);
    e->key[klen] = '\0';
    e->key_len = klen;
    e->count = n;
    e->next = cm->buckets[idx];
    cm->buckets[idx] = e;
    cm->count++;
    cm->total += n;
    return e->count;
}

/// @brief Decrements one key and removes it when its count reaches zero.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key Key to decrement; NULL denotes the empty string.
/// @return The post-decrement count, or zero if the key was absent, removed,
///         or @p obj is NULL.
int64_t rt_countmap_dec(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_countmap_impl *cm = as_countmap(obj, "CountMap.Dec: invalid CountMap object");
    if (!cm)
        return 0;
    if (cm->capacity == 0)
        return 0;

    size_t klen = 0;
    const char *kdata = NULL;
    if (!get_str_data(key, "CountMap.Decrement: invalid key", &kdata, &klen))
        return 0;

    uint64_t h = rt_fnv1a(kdata, klen);
    size_t idx = (size_t)(h % cm->capacity);

    rt_cm_entry **pp = &cm->buckets[idx];
    while (*pp) {
        rt_cm_entry *e = *pp;
        if (e->key_len == klen && memcmp(e->key, kdata, klen) == 0) {
            if (e->count <= 0 || cm->total <= 0) {
                rt_trap("CountMap: total invariant violation");
                return 0;
            }
            e->count--;
            cm->total--;
            if (e->count <= 0) {
                *pp = e->next;
                free_entry(e);
                cm->count--;
                return 0;
            }
            return e->count;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

/// @brief Returns the current count associated with a key.
/// @param obj CountMap handle, or NULL to query an empty map.
/// @param key Key to find; NULL denotes the empty string.
/// @return Positive stored count, or zero when absent or @p obj is NULL.
int64_t rt_countmap_get(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_countmap_impl *cm = as_countmap(obj, "CountMap.Get: invalid CountMap object");
    if (!cm)
        return 0;
    if (cm->capacity == 0)
        return 0;

    size_t klen = 0;
    const char *kdata = NULL;
    if (!get_str_data(key, "CountMap.Get: invalid key", &kdata, &klen))
        return 0;

    uint64_t h = rt_fnv1a(kdata, klen);
    size_t idx = (size_t)(h % cm->capacity);

    rt_cm_entry *e = find_entry(cm->buckets[idx], kdata, klen);
    return e ? e->count : 0;
}

/// @brief Assigns a key's count directly.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key Key to update; NULL denotes the empty string.
/// @param count New count. Positive values insert or replace; zero and
///        negative values remove an existing mapping.
/// @note Total overflow and allocation failures trap without installing a new
///       or increased count.
void rt_countmap_set(void *obj, rt_string key, int64_t count) {
    if (!obj)
        return;
    rt_countmap_impl *cm = as_countmap(obj, "CountMap.Set: invalid CountMap object");
    if (!cm)
        return;
    if (cm->capacity == 0) {
        rt_trap("CountMap: finalized CountMap object");
        return;
    }

    size_t klen = 0;
    const char *kdata = NULL;
    if (!get_str_data(key, "CountMap.Set: invalid key", &kdata, &klen))
        return;

    uint64_t h = rt_fnv1a(kdata, klen);
    size_t idx = (size_t)(h % cm->capacity);

    // Remove if count <= 0
    if (count <= 0) {
        rt_cm_entry **pp = &cm->buckets[idx];
        while (*pp) {
            rt_cm_entry *e = *pp;
            if (e->key_len == klen && memcmp(e->key, kdata, klen) == 0) {
                *pp = e->next;
                cm->total -= e->count;
                free_entry(e);
                cm->count--;
                return;
            }
            pp = &(*pp)->next;
        }
        return;
    }

    rt_cm_entry *e = find_entry(cm->buckets[idx], kdata, klen);
    if (e) {
        if (count > e->count) {
            int64_t delta = count - e->count;
            if (delta > INT64_MAX - cm->total) {
                rt_trap("CountMap: total overflow");
                return;
            }
            cm->total += delta;
        } else {
            cm->total -= (e->count - count);
        }
        e->count = count;
        return;
    }

    if (count > INT64_MAX - cm->total) {
        rt_trap("CountMap: total overflow");
        return;
    }
    if (cm->count >= (size_t)INT64_MAX) {
        rt_trap("CountMap: length overflow");
        return;
    }

    // New entry. Preserve the old representation when the requested count
    // cannot be represented regardless of allocation success.
    if (should_resize(cm)) {
        if (!resize(cm))
            return;
        idx = (size_t)(h % cm->capacity);
    }

    e = (rt_cm_entry *)malloc(sizeof(rt_cm_entry));
    if (!e) {
        rt_trap("CountMap: memory allocation failed");
        return;
    }
    if (klen == SIZE_MAX) {
        free(e);
        rt_trap("CountMap: key allocation overflow");
        return;
    }
    e->key = (char *)malloc(klen + 1);
    if (!e->key) {
        free(e);
        rt_trap("CountMap: key allocation failed");
        return;
    }
    memcpy(e->key, kdata, klen);
    e->key[klen] = '\0';
    e->key_len = klen;
    e->count = count;
    e->next = cm->buckets[idx];
    cm->buckets[idx] = e;
    cm->count++;
    cm->total += count;
}

/// @brief Tests whether a key has a positive stored count.
/// @param obj CountMap handle, or NULL to query an empty map.
/// @param key Key to find; NULL denotes the empty string.
/// @return 1 if present; otherwise 0.
int8_t rt_countmap_has(void *obj, rt_string key) {
    return rt_countmap_get(obj, key) > 0 ? 1 : 0;
}

/// @brief Returns the sum of all stored counts.
/// @param obj CountMap handle, or NULL to query an empty map.
/// @return Aggregate count, or zero when @p obj is NULL.
int64_t rt_countmap_total(void *obj) {
    if (!obj)
        return 0;
    rt_countmap_impl *cm = as_countmap(obj, "CountMap.Total: invalid CountMap object");
    return cm ? cm->total : 0;
}

/// @brief Return a Seq of every key currently stored. Snapshot, not a live view.
/// @param obj CountMap handle, or NULL to enumerate an empty map.
/// @return A new owning Seq of newly created key strings in unspecified
///         bucket order, or NULL if Seq allocation fails.
void *rt_countmap_keys(void *obj) {
    rt_countmap_impl *cm = NULL;
    if (obj) {
        cm = as_countmap(obj, "CountMap.Keys: invalid CountMap object");
        if (!cm)
            return NULL;
        if (!cm->buckets || cm->capacity == 0 || cm->count > (size_t)INT64_MAX) {
            rt_trap("CountMap.Keys: corrupt CountMap object");
            return NULL;
        }
    }

    int64_t capacity = cm && cm->count > 0 ? (int64_t)cm->count : 1;
    void *seq = rt_seq_with_capacity_owned(capacity);
    if (!seq)
        return NULL;
    if (!cm)
        return seq;

    size_t appended = 0;
    for (size_t i = 0; i < cm->capacity; ++i) {
        for (rt_cm_entry *e = cm->buckets[i]; e; e = e->next) {
            if (!countmap_append_key_or_release(
                    seq, e->key, e->key_len, NULL, "CountMap.Keys: snapshot append failed"))
                return NULL;
            appended++;
        }
    }
    if (appended != cm->count) {
        countmap_release_object(seq);
        rt_trap("CountMap.Keys: count invariant violation");
        return NULL;
    }
    return seq;
}

/// @brief qsort comparator ordering entry pointers by count, descending
///        (used by the "most common" / ranked-tally accessors).
/// @param a Pointer to the first entry pointer.
/// @param b Pointer to the second entry pointer.
/// @return Negative if @p a has a greater count, positive if @p b has a
///         greater count, or zero for a tie.
static int cmp_entries_desc(const void *a, const void *b) {
    const rt_cm_entry *ea = *(const rt_cm_entry *const *)a;
    const rt_cm_entry *eb = *(const rt_cm_entry *const *)b;
    if (ea->count > eb->count)
        return -1;
    if (ea->count < eb->count)
        return 1;
    return 0;
}

/// @brief Return the top-`n` most-frequent keys as a Seq, sorted descending by count. Returns
/// every key if `n` exceeds the map size. Useful for top-N reports / leaderboards.
/// @param obj CountMap handle, or NULL to rank an empty map.
/// @param n Maximum number of keys; non-positive values produce an empty Seq.
/// @return A new owning Seq of newly created key strings, or NULL if initial
///         Seq allocation fails.
/// @note Equal-count keys have unspecified relative order.
void *rt_countmap_most_common(void *obj, int64_t n) {
    rt_countmap_impl *cm = NULL;
    if (obj) {
        cm = as_countmap(obj, "CountMap.MostCommon: invalid CountMap object");
        if (!cm)
            return NULL;
        if (!cm->buckets || cm->capacity == 0 || cm->count > (size_t)INT64_MAX) {
            rt_trap("CountMap.MostCommon: corrupt CountMap object");
            return NULL;
        }
    }
    if (!cm || n <= 0 || cm->count == 0)
        return rt_seq_with_capacity_owned(1);

    size_t limit = (uint64_t)n < (uint64_t)cm->count ? (size_t)n : cm->count;
    void *seq = rt_seq_with_capacity_owned((int64_t)limit);
    if (!seq)
        return NULL;

    // Collect all entries into a flat array
    if (cm->count > SIZE_MAX / sizeof(rt_cm_entry *)) {
        countmap_release_object(seq);
        rt_trap("CountMap.MostCommon: allocation size overflow");
        return NULL;
    }
    rt_cm_entry **entries = (rt_cm_entry **)malloc(cm->count * sizeof(rt_cm_entry *));
    if (!entries) {
        countmap_release_object(seq);
        rt_trap("CountMap.MostCommon: memory allocation failed");
        return NULL;
    }

    size_t idx = 0;
    for (size_t i = 0; i < cm->capacity; ++i) {
        for (rt_cm_entry *e = cm->buckets[i]; e; e = e->next) {
            if (idx >= cm->count) {
                free(entries);
                countmap_release_object(seq);
                rt_trap("CountMap.MostCommon: count invariant violation");
                return NULL;
            }
            entries[idx++] = e;
        }
    }
    if (idx != cm->count) {
        free(entries);
        countmap_release_object(seq);
        rt_trap("CountMap.MostCommon: count invariant violation");
        return NULL;
    }

    // Sort by count descending
    qsort(entries, cm->count, sizeof(rt_cm_entry *), cmp_entries_desc);

    for (size_t i = 0; i < limit; ++i) {
        if (!countmap_append_key_or_release(seq,
                                            entries[i]->key,
                                            entries[i]->key_len,
                                            entries,
                                            "CountMap.MostCommon: snapshot append failed"))
            return NULL;
    }

    free(entries);
    return seq;
}

/// @brief Removes a key and subtracts its complete count from the total.
/// @param obj CountMap handle, or NULL for a no-op.
/// @param key Key to remove; NULL denotes the empty string.
/// @return 1 if a mapping was removed; otherwise 0.
int8_t rt_countmap_remove(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_countmap_impl *cm = as_countmap(obj, "CountMap.Remove: invalid CountMap object");
    if (!cm || !cm->buckets || cm->capacity == 0)
        return 0;

    size_t klen = 0;
    const char *kdata = NULL;
    if (!get_str_data(key, "CountMap.Remove: invalid key", &kdata, &klen))
        return 0;

    uint64_t h = rt_fnv1a(kdata, klen);
    size_t idx = (size_t)(h % cm->capacity);

    rt_cm_entry **pp = &cm->buckets[idx];
    while (*pp) {
        rt_cm_entry *e = *pp;
        if (e->key_len == klen && memcmp(e->key, kdata, klen) == 0) {
            *pp = e->next;
            cm->total -= e->count;
            free_entry(e);
            cm->count--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

/// @brief Removes every mapping while retaining the bucket table for reuse.
/// @param obj CountMap handle, or NULL for a no-op.
void rt_countmap_clear(void *obj) {
    if (!obj)
        return;
    rt_countmap_impl *cm = as_countmap(obj, "CountMap.Clear: invalid CountMap object");
    if (!cm || !cm->buckets)
        return;

    for (size_t i = 0; i < cm->capacity; ++i) {
        rt_cm_entry *e = cm->buckets[i];
        while (e) {
            rt_cm_entry *next = e->next;
            free_entry(e);
            e = next;
        }
        cm->buckets[i] = NULL;
    }
    cm->count = 0;
    cm->total = 0;
}
