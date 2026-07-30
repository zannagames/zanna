//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_multimap.c
// Purpose: Implements a string-keyed multimap where each key can map to
//   multiple values. Internally backed by a hash table where each bucket entry
//   holds a Seq of values for that key. Supports add (appending to a key's
//   list), get (returning the Seq for a key), and removeAll (removing an
//   entire key and its Seq); there is no single-value removal operation.
//
// Key invariants:
//   - Backed by a hash table with initial capacity MM_INITIAL_CAPACITY (16)
//     buckets and separate chaining using a per-process keyed hash.
//   - Resizes (doubles) when key_count/capacity exceeds 75%.
//   - `key_count` tracks distinct keys; `total_count` tracks total values added.
//   - Each published bucket entry owns a non-empty Seq; adding a value appends
//     to that Seq, including duplicate and NULL values.
//   - Entries are removed only as a whole by RemoveAll or Clear; there is no
//     single-value removal operation.
//   - Getting a non-existent key returns an empty Seq (not NULL) so callers can
//     always iterate the result without a null-check.
//   - Key lookup and insertion are O(1) average case. Snapshot creation and
//     removal are linear in the number of values retained by the selected key.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - MultiMap objects are GC-managed (rt_obj_new_i64). The bucket array, entry
//     nodes, and per-key Seq objects are freed by the GC finalizer.
//   - Keys are copied by byte length. Each owning Seq retains its values; a
//     returned snapshot independently retains the same values.
//
// Links: src/runtime/collections/rt_multimap.h (public API),
//        src/runtime/collections/rt_seq.h (value list backing type)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime's string-keyed multi-valued hash map.
/// @details Each distinct byte-string key owns an insertion-ordered `Seq` of
///          values. Repeated puts preserve duplicates, total length counts
///          values rather than keys, and read operations return independent
///          owning snapshots instead of exposing internal storage. The table
///          uses separate chaining and doubles before a new key would exceed a
///          75-percent load factor.

#include "rt_multimap.h"

#include "rt_collection_ids.h"
#include "rt_collection_ownership.h"
#include "rt_gc.h"
#include "rt_hash_table_util.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Number of buckets allocated for a newly constructed MultiMap.
#define MM_INITIAL_CAPACITY 16
#include "rt_hash_util.h"

/// @brief Install a non-local trap recovery target for the current thread.
/// @param buf Jump buffer that receives control when a runtime trap occurs.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Remove the current thread's non-local trap recovery target.
void rt_trap_clear_recovery(void);
/// @brief Borrow the current thread's most recently recorded trap message.
/// @return NUL-terminated diagnostic text, or `NULL` when none is available.
const char *rt_trap_get_error(void);

/// @brief One separately chained hash-table entry for a copied string key.
/// @details `values` is an owning runtime `Seq`; `next` links entries that
///          hash to the same bucket. The key buffer includes a convenience
///          trailing NUL, but `key_len` defines identity and permits embedded
///          NUL bytes.
typedef struct rt_mm_entry {
    char *key;
    size_t key_len;
    void *values; // Seq of values for this key
    struct rt_mm_entry *next;
} rt_mm_entry;

/// @brief GC-managed implementation payload for the public MultiMap object.
/// @details `key_count` counts chain entries while `total_count` counts every
///          stored value, including duplicates and `NULL`.
typedef struct rt_multimap_impl {
    void **vptr;
    rt_mm_entry **buckets;
    size_t capacity;
    size_t key_count;
    size_t total_count;
    int8_t finalizing;
} rt_multimap_impl;

/// @brief Checked cast of an opaque handle to the MultiMap implementation.
/// @details Traps with @p what if @p obj is NULL or not a MultiMap.
/// @param obj Opaque runtime object to validate.
/// @param what Diagnostic passed to the trap subsystem on type failure.
/// @return The validated implementation pointer, or `NULL` after trapping.
static rt_multimap_impl *as_multimap(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_MULTIMAP_CLASS_ID, sizeof(rt_multimap_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_multimap_impl *)obj;
}

/// @brief Borrow the byte buffer and byte length of a runtime key string.
/// @details A null handle denotes the empty byte string. Forged and stale
///          nonnull handles trap and cannot address a legitimate empty key.
/// @param key Runtime string key to inspect; `NULL` denotes the empty key.
/// @param what Diagnostic raised for a stale or forged nonnull handle.
/// @param out_data Receives borrowed key bytes.
/// @param out_len Receives the number of identity bytes in the returned key.
/// @return Nonzero for a null or live string handle; otherwise zero after
///         trapping.
static int get_key_data(rt_string key, const char *what, const char **out_data, size_t *out_len) {
    *out_data = "";
    *out_len = 0;
    if (!key) {
        return 1;
    }
    if (!rt_string_is_handle(key)) {
        rt_trap(what);
        return 0;
    }
    int64_t len = rt_str_len(key);
    if (len <= 0)
        return 1;
    const char *cstr = rt_string_cstr(key);
    if (!cstr) {
        rt_trap(what);
        return 0;
    }
    *out_data = cstr;
    *out_len = (size_t)len;
    return 1;
}

/// @brief Per-process keyed hash for attacker-controlled multimap keys.
/// @param key Borrowed key bytes.
/// @param key_len Number of identity bytes.
/// @return Process-keyed 64-bit hash.
static uint64_t mm_hash(const char *key, size_t key_len) {
    return rt_keyed_hash_bytes(key ? key : "", key_len);
}

/// @brief Find an exact byte-string key in one collision chain.
/// @param head First entry in the bucket chain, or `NULL`.
/// @param key Key bytes to compare.
/// @param key_len Number of bytes in @p key.
/// @return The borrowed matching entry, or `NULL` when the chain lacks the key.
static rt_mm_entry *find_entry(rt_mm_entry *head, const char *key, size_t key_len) {
    for (rt_mm_entry *e = head; e; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

/// @brief Release one runtime object reference and finalize it at zero.
/// @param obj Runtime-managed object, or `NULL` for a no-op.
static void mm_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Destroy an unpublished or detached chain entry.
/// @details Frees the copied key, releases the entry's owning value `Seq`, and
///          then frees the entry node itself.
/// @param entry Entry to destroy, or `NULL` for a no-op.
static void free_entry(rt_mm_entry *entry) {
    if (!entry)
        return;
    void *values = entry->values;
    entry->values = NULL;
    free(entry->key);
    entry->key = NULL;
    free(entry);
    mm_release_object(values);
}

/// @brief Copy the active trap diagnostic into bounded local storage.
/// @details Uses @p fallback when the trap subsystem has no non-empty message,
///          allowing recovery paths to clear the trap frame before retrapping.
/// @param buffer Destination buffer for the saved NUL-terminated diagnostic.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Diagnostic to use when no active trap text is available.
static void mm_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Publish an empty table and collect every old entry into one list.
/// @details All buckets and both counts are cleared before any callback-capable
///          release. Rewriting the detached collision links requires no
///          allocation and lets callback-created entries remain independent.
/// @param mm Valid MultiMap with a live bucket array.
/// @return Head of a detached list containing every formerly published entry.
static rt_mm_entry *mm_detach_all_entries(rt_multimap_impl *mm) {
    rt_mm_entry *detached = NULL;
    for (size_t i = 0; i < mm->capacity; ++i) {
        rt_mm_entry *entry = mm->buckets[i];
        mm->buckets[i] = NULL;
        while (entry) {
            rt_mm_entry *next = entry->next;
            entry->next = detached;
            detached = entry;
            entry = next;
        }
    }
    mm->key_count = 0;
    mm->total_count = 0;
    return detached;
}

/// @brief Consume detached entries despite trapping per-key Seq finalizers.
/// @details Native entry/key ownership is reclaimed before releasing each
///          value Seq. Cleanup resumes at the following entry after a
///          non-local trap and reports the first diagnostic after draining all.
/// @param head Detached entry list, or NULL.
/// @param error Buffer receiving the first trapped diagnostic.
/// @param error_size Capacity of @p error including its terminator.
/// @return Nonzero when at least one entry cleanup trapped.
static int mm_destroy_entries(rt_mm_entry *head, char *error, size_t error_size) {
    rt_mm_entry *volatile cursor = head;
    volatile int trapped = 0;
    jmp_buf recovery;

    rt_gc_mutator_enter();
    rt_trap_set_recovery(&recovery);
    for (;;) {
        if (setjmp(recovery) != 0) {
            rt_gc_mutator_enter();
            if (!trapped)
                mm_save_trap_error(error, error_size, "MultiMap: value finalizer cleanup failed");
            trapped = 1;
        }
        rt_mm_entry *entry = (rt_mm_entry *)cursor;
        if (!entry)
            break;
        cursor = entry->next;
        free_entry(entry);
    }
    rt_trap_clear_recovery();
    rt_gc_mutator_exit();
    return trapped ? 1 : 0;
}

/// @brief Append the first value to a not-yet-published entry transactionally.
/// @details Catches a trap from the owning `Seq` retain/growth path, destroys
///          the entire partial entry, restores the diagnostic, and retraps.
/// @param entry Fully allocated entry whose owning `Seq` is initially empty.
/// @param value Value to append and retain; `NULL` is a valid stored value.
/// @return 1 after the append succeeds, or 0 after cleanup and a propagated
///         trap.
static int mm_push_value_or_release_entry(rt_mm_entry *entry, void *value) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        mm_save_trap_error(saved_error, sizeof(saved_error), "MultiMap: value retain failed");
        rt_trap_clear_recovery();
        free_entry(entry);
        rt_trap(saved_error);
        return 0;
    }

    int64_t old_len = rt_seq_len(entry->values);
    rt_seq_push(entry->values, value);
    if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len(entry->values) != old_len + 1)
        rt_trap("MultiMap: value append failed");
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Append to a published value sequence without changing map metadata
///        after a failed retain or allocation.
/// @details The entry and its owning `Seq` remain published and unchanged when
///          `Seq.Push` traps. The original diagnostic is propagated after the
///          local recovery frame is removed.
/// @param seq Published owning `Seq` belonging to an existing entry.
/// @param value Value to append and retain; `NULL` is valid.
/// @return Nonzero after append; zero after propagating a trapped failure.
static int mm_push_value_checked(void *seq, void *value) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        mm_save_trap_error(saved_error, sizeof(saved_error), "MultiMap: value retain failed");
        rt_trap_clear_recovery();
        rt_trap(saved_error);
        return 0;
    }

    int64_t old_len = rt_seq_len(seq);
    rt_seq_push(seq, value);
    if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len(seq) != old_len + 1)
        rt_trap("MultiMap: value append failed");
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Append a value to a result snapshot or release the snapshot on trap.
/// @details This helper provides failure-atomic construction for Get results:
///          if `Seq.Push` traps, every value retained so far is released with
///          @p seq before the original diagnostic is propagated.
/// @param seq Owning result `Seq` under construction.
/// @param value Borrowed value to append and retain; may be `NULL`.
/// @param fallback Diagnostic used when the trapped operation supplied none.
/// @return Nonzero after append; zero after releasing @p seq and retrapping.
static int mm_push_value_or_release_seq(void *seq, void *value, const char *fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        mm_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        mm_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    int64_t old_len = rt_seq_len(seq);
    rt_seq_push(seq, value);
    if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len(seq) != old_len + 1)
        rt_trap(fallback);
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Copy one stored key into an owning key-snapshot `Seq`.
/// @details Both string construction and `Seq.Push` are trap-protected. A
///          failure releases the temporary string and the entire partial
///          result before propagating the saved diagnostic.
/// @param seq Owning key `Seq` under construction.
/// @param key Borrowed key bytes from an internal entry.
/// @param key_len Number of bytes to copy, including any embedded NUL bytes
///                in the key identity.
/// @return Nonzero after append; zero after releasing @p seq and retrapping.
static int mm_append_key_or_release_seq(void *seq, const char *key, size_t key_len) {
    volatile rt_string key_copy = NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        mm_save_trap_error(saved_error, sizeof(saved_error), "MultiMap.Keys: failed to copy key");
        rt_trap_clear_recovery();
        if (key_copy)
            rt_str_release_maybe((rt_string)key_copy);
        mm_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    key_copy = rt_string_from_bytes(key, key_len);
    if (!key_copy)
        rt_trap("MultiMap.Keys: failed to copy key");
    int64_t old_len = rt_seq_len(seq);
    rt_seq_push_raw(seq, (void *)key_copy);
    if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len(seq) != old_len + 1)
        rt_trap("MultiMap.Keys: failed to append key");
    key_copy = NULL;
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Rehash all entries into a fresh @p new_cap bucket array.
/// @details Entry and value ownership is unchanged; only collision links and
///          the bucket array are replaced. Traps on allocation overflow or
///          exhaustion and leaves the original table intact.
/// @param mm MultiMap whose existing entries are to be redistributed.
/// @param new_cap Nonzero target bucket count.
/// @return 1 after publication of the new table, or 0 after trapping.
static int mm_resize(rt_multimap_impl *mm, size_t new_cap) {
    if (new_cap > SIZE_MAX / sizeof(rt_mm_entry *)) {
        rt_trap("MultiMap: allocation size overflow");
        return 0;
    }
    rt_mm_entry **new_buckets = (rt_mm_entry **)calloc(new_cap, sizeof(rt_mm_entry *));
    if (!new_buckets) {
        rt_trap("MultiMap: memory allocation failed");
        return 0;
    }
    for (size_t i = 0; i < mm->capacity; ++i) {
        rt_mm_entry *e = mm->buckets[i];
        while (e) {
            rt_mm_entry *next = e->next;
            uint64_t h = mm_hash(e->key, e->key_len);
            size_t idx = h % new_cap;
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }
    free(mm->buckets);
    mm->buckets = new_buckets;
    mm->capacity = new_cap;
    return 1;
}

/// @brief Double the bucket array once the key load factor exceeds the
///        MM_LOAD_FACTOR_NUM/MM_LOAD_FACTOR_DEN threshold (capped).
/// @param mm MultiMap about to receive one previously absent key.
/// @return 1 when the existing or resized table can accept the key, or 0 after
///         a capacity/allocation trap.
static int maybe_resize_for_insert(rt_multimap_impl *mm) {
    if (mm->key_count == SIZE_MAX) {
        rt_trap("MultiMap: length overflow");
        return 0;
    }
    size_t next_count = mm->key_count + 1;
    if (rt_hash_table_exceeds_load(next_count, mm->capacity)) {
        if (mm->capacity > SIZE_MAX / 2) {
            rt_trap("MultiMap: capacity overflow");
            return 0;
        }
        return mm_resize(mm, mm->capacity * 2);
    }
    return 1;
}

/// @brief GC finalizer: detach and destroy all entries, then free the table.
/// @param obj MultiMap object being finalized; `NULL` is ignored.
static void rt_multimap_finalize(void *obj) {
    if (!obj)
        return;
    rt_multimap_impl *mm = as_multimap(obj, "MultiMap: invalid MultiMap object");
    if (!mm)
        return;
    if (!mm->buckets)
        return;

    mm->finalizing = 1;
    rt_mm_entry *entries = mm_detach_all_entries(mm);
    rt_mm_entry **buckets = mm->buckets;
    size_t capacity = mm->capacity;
    mm->buckets = NULL;
    mm->capacity = 0;

    char cleanup_error[256] = {0};
    int cleanup_trapped = mm_destroy_entries(entries, cleanup_error, sizeof(cleanup_error));

    rt_heap_info_t info;
    int resurrected = rt_heap_get_info(obj, &info) && info.refcnt != 0;
    if (resurrected && buckets && capacity > 0) {
        memset(buckets, 0, capacity * sizeof(rt_mm_entry *));
        mm->buckets = buckets;
        mm->capacity = capacity;
        buckets = NULL;
        rt_obj_set_finalizer(mm, rt_multimap_finalize);
    }
    mm->finalizing = 0;
    free(buckets);

    if (cleanup_trapped)
        rt_trap(cleanup_error[0] ? cleanup_error : "MultiMap: value finalizer cleanup failed");
}

/// @brief Visit every runtime object directly retained by a MultiMap.
/// @details The copied keys and chain nodes are native allocations, so only
///          each entry's owning value `Seq` participates in GC traversal.
/// @param obj MultiMap object to traverse; `NULL` is ignored.
/// @param visitor Collector callback invoked once per internal value `Seq`.
/// @param ctx Opaque collector context forwarded unchanged to @p visitor.
static void rt_multimap_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_multimap_impl *mm = as_multimap(obj, "MultiMap: invalid MultiMap object");
    if (!mm)
        return;
    if (!mm->buckets || mm->capacity == 0)
        return;
    for (size_t i = 0; i < mm->capacity; ++i) {
        for (rt_mm_entry *entry = mm->buckets[i]; entry; entry = entry->next)
            visitor(entry->values, ctx);
    }
}

/// @brief Construct an empty string-to-values MultiMap.
/// @details Allocates the initial chained hash table, installs the native
///          finalizer, and registers value-`Seq` traversal with the collector.
///          The returned object begins with zero distinct keys and zero values.
/// @return New runtime-managed MultiMap object, or `NULL` after an allocation
///         trap.
void *rt_multimap_new(void) {
    rt_multimap_impl *mm =
        (rt_multimap_impl *)rt_obj_new_i64(RT_MULTIMAP_CLASS_ID, (int64_t)sizeof(rt_multimap_impl));
    if (!mm) {
        rt_trap("MultiMap: memory allocation failed");
        return NULL;
    }
    mm->vptr = NULL;
    mm->buckets = (rt_mm_entry **)calloc(MM_INITIAL_CAPACITY, sizeof(rt_mm_entry *));
    if (!mm->buckets) {
        if (rt_obj_release_check0(mm))
            rt_obj_free(mm);
        rt_trap("MultiMap: memory allocation failed");
        return NULL;
    }
    mm->capacity = MM_INITIAL_CAPACITY;
    mm->key_count = 0;
    mm->total_count = 0;
    mm->finalizing = 0;
    rt_obj_set_finalizer(mm, rt_multimap_finalize);
    rt_gc_track(mm, rt_multimap_traverse);
    return mm;
}

/// @brief Return the total number of values across all keys in the multimap.
/// @details Duplicate and `NULL` values each contribute one. A null handle is
///          treated as an empty map; an invalid non-null handle traps.
/// @param obj Opaque MultiMap object, or `NULL`.
/// @return Aggregate value count, or 0 for `NULL`.
int64_t rt_multimap_len(void *obj) {
    if (!obj)
        return 0;
    rt_multimap_impl *map = as_multimap(obj, "MultiMap.Len: invalid MultiMap object");
    return map ? (int64_t)map->total_count : 0;
}

/// @brief Return the number of distinct keys in the multimap.
/// @details The count is independent of the number of values stored for each
///          key. A null handle is treated as an empty map.
/// @param obj Opaque MultiMap object, or `NULL`.
/// @return Number of unique byte-string keys, or 0 for `NULL`.
int64_t rt_multimap_key_count(void *obj) {
    if (!obj)
        return 0;
    rt_multimap_impl *map = as_multimap(obj, "MultiMap.KeyCount: invalid MultiMap object");
    return map ? (int64_t)map->key_count : 0;
}

/// @brief Check whether the multimap has no entries.
/// @details Emptiness is defined by aggregate value count. A null handle is
///          therefore empty.
/// @param obj Opaque MultiMap object, or `NULL`.
/// @return 1 when the map stores no values, otherwise 0.
int8_t rt_multimap_is_empty(void *obj) {
    return rt_multimap_len(obj) == 0;
}

/// @brief Add a value under a key in the multimap.
/// @details Normalizes a null key to the empty byte string. For a new key, the
///          bytes are copied and a private owning `Seq` is created before the
///          entry is published. For an existing key, @p value is appended in
///          insertion order. Duplicates and `NULL` values are preserved. A
///          null map is a no-op; invalid or finalized non-null handles trap.
/// @param obj Opaque MultiMap object, or `NULL` for a no-op.
/// @param key Runtime string key; `NULL` identifies the empty key.
/// @param value Runtime value retained by the internal owning `Seq`; may be
///              `NULL`.
void rt_multimap_put(void *obj, rt_string key, void *value) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_multimap_impl *mm = as_multimap(obj, "MultiMap.Put: invalid MultiMap object");
    if (!mm) {
        rt_gc_mutator_exit();
        return;
    }
    if (mm->finalizing) {
        rt_gc_mutator_exit();
        return;
    }
    if (!mm->buckets || mm->capacity == 0) {
        rt_gc_mutator_exit();
        rt_trap("MultiMap: finalized MultiMap object");
        return;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "MultiMap.Put: invalid key", &key_data, &key_len)) {
        rt_gc_mutator_exit();
        return;
    }
    uint64_t hash = mm_hash(key_data, key_len);
    size_t idx = hash % mm->capacity;

    rt_mm_entry *existing = find_entry(mm->buckets[idx], key_data, key_len);
    if (existing) {
        if (mm->total_count >= (size_t)INT64_MAX) {
            rt_gc_mutator_exit();
            rt_trap("MultiMap: length overflow");
            return;
        }
        if (!mm_push_value_checked(existing->values, value)) {
            rt_gc_mutator_exit();
            return;
        }
        mm->total_count++;
        rt_gc_mutator_exit();
        return;
    }

    // New key
    if (mm->key_count >= (size_t)INT64_MAX || mm->total_count >= (size_t)INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("MultiMap: length overflow");
        return;
    }
    if (!maybe_resize_for_insert(mm)) {
        rt_gc_mutator_exit();
        return;
    }
    idx = hash % mm->capacity;

    void *values = rt_seq_with_capacity_owned(1);
    if (!values) {
        rt_gc_mutator_exit();
        return;
    }
    rt_mm_entry *entry = (rt_mm_entry *)malloc(sizeof(rt_mm_entry));
    if (!entry) {
        mm_release_object(values);
        rt_gc_mutator_exit();
        rt_trap("MultiMap: memory allocation failed");
        return;
    }
    if (key_len == SIZE_MAX) {
        mm_release_object(values);
        free(entry);
        rt_gc_mutator_exit();
        rt_trap("MultiMap: key allocation overflow");
        return;
    }
    entry->key = (char *)malloc(key_len + 1);
    if (!entry->key) {
        mm_release_object(values);
        free(entry);
        rt_gc_mutator_exit();
        rt_trap("MultiMap: key allocation failed");
        return;
    }
    memcpy(entry->key, key_data, key_len);
    entry->key[key_len] = '\0';
    entry->key_len = key_len;

    entry->values = values;
    entry->next = NULL;
    // The Seq object itself starts with refcount=1; do not retain it again.
    if (!mm_push_value_or_release_entry(entry, value)) {
        rt_gc_mutator_exit();
        return;
    }

    entry->next = mm->buckets[idx];
    mm->buckets[idx] = entry;
    mm->key_count++;
    mm->total_count++;
    rt_gc_mutator_exit();
}

/// @brief Snapshot all values stored under a key.
/// @details Returns a fresh owning `Seq` in per-key insertion order. The
///          snapshot independently retains every non-null value and may be
///          mutated without changing the MultiMap. A null map, finalized map,
///          or absent key produces an empty owning `Seq`.
/// @param obj Opaque MultiMap object, or `NULL`.
/// @param key Runtime string key; `NULL` identifies the empty key.
/// @return New caller-owned `Seq`, never `NULL` unless allocation traps.
void *rt_multimap_get(void *obj, rt_string key) {
    if (!obj)
        return rt_seq_with_capacity_owned(1);
    rt_multimap_impl *mm = as_multimap(obj, "MultiMap.Get: invalid MultiMap object");
    if (!mm)
        return NULL;
    if (!mm->buckets || mm->capacity == 0)
        return rt_seq_with_capacity_owned(1);

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "MultiMap.Get: invalid key", &key_data, &key_len))
        return NULL;
    uint64_t hash = mm_hash(key_data, key_len);
    size_t idx = hash % mm->capacity;

    rt_mm_entry *entry = find_entry(mm->buckets[idx], key_data, key_len);
    if (!entry)
        return rt_seq_with_capacity_owned(1);

    // Return a copy of the values Seq
    int64_t len = rt_seq_len(entry->values);
    if (len < 0)
        return NULL;
    void *result = rt_seq_with_capacity_owned(len > 0 ? len : 1);
    if (!result)
        return NULL;
    for (int64_t i = 0; i < len; ++i) {
        void *value = rt_seq_get(entry->values, i);
        if (!mm_push_value_or_release_seq(result, value, "MultiMap.Get: failed to copy values"))
            return NULL;
    }
    return result;
}

/// @brief Return the earliest value inserted under a key.
/// @details The result is retained for the caller before the GC mutator region
///          closes. Because `NULL` is a valid stored value, a null result
///          cannot distinguish an absent key from a first value of `NULL`.
/// @param obj Opaque MultiMap object, or `NULL`.
/// @param key Runtime string key; `NULL` identifies the empty key.
/// @return Caller-owned retained first value, or `NULL` when unavailable or
///         when the stored first value is itself `NULL`.
void *rt_multimap_get_first(void *obj, rt_string key) {
    if (!obj)
        return NULL;
    rt_gc_mutator_enter();
    rt_multimap_impl *mm = as_multimap(obj, "MultiMap.GetFirst: invalid MultiMap object");
    if (!mm || !mm->buckets || mm->capacity == 0) {
        rt_gc_mutator_exit();
        return NULL;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "MultiMap.GetFirst: invalid key", &key_data, &key_len)) {
        rt_gc_mutator_exit();
        return NULL;
    }
    uint64_t hash = mm_hash(key_data, key_len);
    size_t idx = hash % mm->capacity;

    rt_mm_entry *entry = find_entry(mm->buckets[idx], key_data, key_len);
    if (!entry || rt_seq_len(entry->values) == 0) {
        rt_gc_mutator_exit();
        return NULL;
    }
    void *value = rt_seq_get(entry->values, 0);
    if (!rt_collection_retain_checked(value, "MultiMap.GetFirst: result retain failed")) {
        rt_gc_mutator_exit();
        return NULL;
    }
    rt_gc_mutator_exit();
    return value;
}

/// @brief Check whether a key exists in the multimap.
/// @details Key identity uses the complete runtime-string byte length. A null
///          map or finalized map reports absence.
/// @param obj Opaque MultiMap object, or `NULL`.
/// @param key Runtime string key; `NULL` identifies the empty key.
/// @return 1 when an entry exists for @p key, otherwise 0.
int8_t rt_multimap_has(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_multimap_impl *mm = as_multimap(obj, "MultiMap.Has: invalid MultiMap object");
    if (!mm || !mm->buckets || mm->capacity == 0)
        return 0;

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "MultiMap.Has: invalid key", &key_data, &key_len))
        return 0;
    uint64_t hash = mm_hash(key_data, key_len);
    size_t idx = hash % mm->capacity;
    return find_entry(mm->buckets[idx], key_data, key_len) ? 1 : 0;
}

/// @brief Return the number of values associated with a specific key.
/// @details Counts duplicates and `NULL` values independently.
/// @param obj Opaque MultiMap object, or `NULL`.
/// @param key Runtime string key; `NULL` identifies the empty key.
/// @return Number of values stored under the key, or 0 when the map/key is
///         absent.
int64_t rt_multimap_count_for(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_multimap_impl *mm = as_multimap(obj, "MultiMap.CountFor: invalid MultiMap object");
    if (!mm || !mm->buckets || mm->capacity == 0)
        return 0;

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "MultiMap.CountFor: invalid key", &key_data, &key_len))
        return 0;
    uint64_t hash = mm_hash(key_data, key_len);
    size_t idx = hash % mm->capacity;

    rt_mm_entry *entry = find_entry(mm->buckets[idx], key_data, key_len);
    return entry ? rt_seq_len(entry->values) : 0;
}

/// @brief Remove all values associated with a key from the multimap.
/// @details Unlinks the complete entry, subtracts its `Seq` length from the
///          aggregate count, releases every retained value, and frees the
///          copied key. Other keys and bucket capacity are unchanged.
/// @param obj Opaque MultiMap object, or `NULL`.
/// @param key Runtime string key; `NULL` identifies the empty key.
/// @return 1 if the key existed and was removed, otherwise 0.
int8_t rt_multimap_remove_all(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_gc_mutator_enter();
    rt_multimap_impl *mm = as_multimap(obj, "MultiMap.RemoveAll: invalid MultiMap object");
    if (!mm || !mm->buckets || mm->capacity == 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, "MultiMap.RemoveAll: invalid key", &key_data, &key_len)) {
        rt_gc_mutator_exit();
        return 0;
    }
    uint64_t hash = mm_hash(key_data, key_len);
    size_t idx = hash % mm->capacity;

    rt_mm_entry **prev_ptr = &mm->buckets[idx];
    rt_mm_entry *entry = mm->buckets[idx];
    while (entry) {
        if (entry->key_len == key_len && memcmp(entry->key, key_data, key_len) == 0) {
            *prev_ptr = entry->next;
            mm->total_count -= (size_t)rt_seq_len(entry->values);
            mm->key_count--;
            rt_gc_mutator_exit();
            free_entry(entry);
            return 1;
        }
        prev_ptr = &entry->next;
        entry = entry->next;
    }
    rt_gc_mutator_exit();
    return 0;
}

/// @brief Remove all entries from the multimap.
/// @details Releases all per-key value snapshots and copied keys while
///          retaining the current bucket allocation for reuse. Resets both
///          distinct-key and aggregate-value counts to zero. A null map is a
///          no-op.
/// @param obj Opaque MultiMap object, or `NULL`.
void rt_multimap_clear(void *obj) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_multimap_impl *mm = as_multimap(obj, "MultiMap.Clear: invalid MultiMap object");
    if (!mm || !mm->buckets) {
        rt_gc_mutator_exit();
        return;
    }
    if (mm->finalizing) {
        rt_gc_mutator_exit();
        return;
    }

    rt_mm_entry *entries = mm_detach_all_entries(mm);
    rt_gc_mutator_exit();

    char cleanup_error[256] = {0};
    if (mm_destroy_entries(entries, cleanup_error, sizeof(cleanup_error)))
        rt_trap(cleanup_error[0] ? cleanup_error
                                 : "MultiMap.Clear: value finalizer cleanup failed");
}

/// @brief Return a Seq of every distinct key in the multimap (bucket-iteration order).
/// @details Each key is copied into a new runtime string retained by the
///          owning result `Seq`. Ordering reflects current bucket and chain
///          layout and is not an insertion-order or stable-order contract. A
///          null map returns an empty owning `Seq`.
/// @param obj Opaque MultiMap object, or `NULL`.
/// @return New caller-owned `Seq` of copied string handles.
void *rt_multimap_keys(void *obj) {
    rt_multimap_impl *mm = NULL;
    if (obj) {
        mm = as_multimap(obj, "MultiMap.Keys: invalid MultiMap object");
        if (!mm)
            return NULL;
    }

    int64_t initial_capacity = mm && mm->key_count > 0 ? (int64_t)mm->key_count : 1;
    void *result = rt_seq_with_capacity_owned(initial_capacity);
    if (!result)
        return NULL;
    if (!mm || !mm->buckets)
        return result;
    for (size_t i = 0; i < mm->capacity; ++i) {
        for (rt_mm_entry *e = mm->buckets[i]; e; e = e->next) {
            if (!mm_append_key_or_release_seq(result, e->key, e->key_len))
                return NULL;
        }
    }
    return result;
}
