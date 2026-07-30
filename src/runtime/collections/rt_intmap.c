//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_intmap.c
// Purpose: Implements an integer-keyed hash map (IntMap) mapping int64 keys to
//   arbitrary object values. Uses a mix-hash on the integer key to distribute
//   it into a hash table with separate chaining. Supports get, put, remove,
//   contains, and key/value enumeration. Typical uses: entity ID lookup tables,
//   sparse index-to-object mappings, and cache tables keyed by integer handle.
//
// Key invariants:
//   - Backed by a hash table with initial capacity MAP_INITIAL_CAPACITY (16)
//     buckets and separate chaining.
//   - Resizes (doubles) when count/capacity exceeds 75% (MAP_LOAD_FACTOR 3/4).
//   - Integer keys are hashed over their in-memory bytes with FNV-1a, and the
//     cached hash is reused during lookup and rehashing.
//   - Values are retained on insert (rt_obj_retain_maybe), released on
//     remove/overwrite/finalize, and exposed to the cycle collector through
//     the map's GC traversal callback.
//   - All operations are O(1) average case; O(n) worst case.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - IntMap objects are GC-managed (rt_obj_new_i64). The bucket array and all
//     entry nodes are freed by the GC finalizer.
//
// Links: src/runtime/collections/rt_intmap.h (public API),
//        src/runtime/collections/rt_map.h (string-keyed map counterpart)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a retained-value hash map keyed by signed 64-bit integers.
///
/// IntMap stores keys directly without boxing and uses separately chained
/// buckets with cached FNV-1a hashes. Insertions grow the bucket table before
/// exceeding the shared three-quarter load threshold, while explicit trimming
/// transactionally selects the smallest permitted capacity for current entries.
///
/// Values are retained on insertion, traced by the runtime collector, and
/// released on overwrite or removal. Getter results are borrowed pointers;
/// enumeration returns owning Seqs that retain boxed keys or mapped values.
/// Mutation is unsynchronized apart from the runtime GC mutator protocol.

#include "rt_intmap.h"

#include "rt_box.h"
#include "rt_collection_ids.h"
#include "rt_collection_ownership.h"
#include "rt_gc.h"
#include "rt_hash_table_util.h"
#include "rt_hash_util.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Install a non-local recovery target for the current thread.
/// @param buf Jump buffer that receives control after a runtime trap.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Remove the current thread's active legacy recovery target.
void rt_trap_clear_recovery(void);
/// @brief Borrow the most recent runtime trap diagnostic.
/// @return Null-terminated diagnostic text, or NULL when unavailable.
const char *rt_trap_get_error(void);

/// Initial number of buckets.
#define MAP_INITIAL_CAPACITY 16

/// @brief Entry in the integer-keyed hash map (collision chain node).
typedef struct rt_intmap_entry {
    int64_t key;                  ///< Integer key.
    uint64_t hash;                ///< Cached mixed hash used by lookup and rehash.
    void *value;                  ///< Retained reference to the value object.
    struct rt_intmap_entry *next; ///< Next entry in collision chain (or NULL).
} rt_intmap_entry;

/// @brief IntMap (integer-to-object dictionary) implementation structure.
typedef struct rt_intmap_impl {
    void **vptr;               ///< Vtable pointer placeholder (for OOP compatibility).
    rt_intmap_entry **buckets; ///< Array of bucket heads (collision chain pointers).
    size_t capacity;           ///< Number of buckets in the hash table.
    size_t count;              ///< Number of key-value pairs currently in the IntMap.
} rt_intmap_impl;

/// @brief Checked cast of an opaque handle to the IntMap implementation.
/// @details Traps with @p what if @p obj is NULL or not an IntMap.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used on validation failure.
/// @return The validated implementation pointer, or NULL if a returning trap
///         handler resumes after failed validation.
static rt_intmap_impl *as_intmap(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_INTMAP_CLASS_ID, sizeof(rt_intmap_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_intmap_impl *)obj;
}

/// @brief Find an entry matching the given key in a collision chain.
/// @param head Head of the collision chain.
/// @param hash Cached hash of @p key.
/// @param key Integer key to search for.
/// @return Matching entry or NULL.
static rt_intmap_entry *find_entry(rt_intmap_entry *head, uint64_t hash, int64_t key) {
    for (rt_intmap_entry *e = head; e; e = e->next) {
        if (e->hash == hash && e->key == key)
            return e;
    }
    return NULL;
}

/// @brief Free an entry and release its value reference.
/// @param entry Entry to free (NULL is a no-op).
static void free_entry(rt_intmap_entry *entry) {
    if (!entry)
        return;
    void *value = entry->value;
    rt_free(entry);
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Release one runtime object and finalize it at zero.
/// @param obj Runtime-managed object, or NULL.
static void intmap_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Destroy one detached chain after its table/count change is visible.
/// @param entries Detached IntMap entries to consume.
static void intmap_destroy_entries(rt_intmap_entry *entries) {
    while (entries) {
        rt_intmap_entry *next = entries->next;
        free_entry(entries);
        entries = next;
    }
}

/// @brief Detach all entries while preserving the reusable bucket array.
/// @param map Live IntMap whose entries are transferred.
/// @return One chain owning every formerly published entry.
static rt_intmap_entry *intmap_detach_entries(rt_intmap_impl *map) {
    rt_intmap_entry *detached = NULL;
    for (size_t i = 0; i < map->capacity; ++i) {
        rt_intmap_entry *entry = map->buckets[i];
        map->buckets[i] = NULL;
        while (entry) {
            rt_intmap_entry *next = entry->next;
            entry->next = detached;
            detached = entry;
            entry = next;
        }
    }
    map->count = 0;
    return detached;
}

/// @brief Save an active trap diagnostic before clearing its recovery frame.
/// @param buffer Destination for the bounded diagnostic copy.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when no active diagnostic is available.
static void intmap_save_trap(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Box and append one key, consuming the partial snapshot on failure.
/// @param seq Partial owning key snapshot.
/// @param key Integer key to box.
/// @return One after publication; zero after cleanup and a propagated trap.
static int intmap_append_key_or_release_seq(void *seq, int64_t key) {
    void *volatile boxed = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        intmap_save_trap(saved_error, sizeof(saved_error), "IntMap.Keys: snapshot append failed");
        rt_trap_clear_recovery();
        intmap_release_object((void *)boxed);
        intmap_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    boxed = rt_box_i64(key);
    if (!boxed)
        rt_trap("IntMap.Keys: key allocation failed");
    rt_seq_push(seq, (void *)boxed);
    intmap_release_object((void *)boxed);
    boxed = NULL;
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Append one retained value, consuming the partial snapshot on failure.
/// @param seq Partial owning value snapshot.
/// @param value Borrowed mapped value; may be NULL.
/// @return One after publication; zero after cleanup and a propagated trap.
static int intmap_append_value_or_release_seq(void *seq, void *value) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        intmap_save_trap(saved_error, sizeof(saved_error), "IntMap.Values: snapshot append failed");
        rt_trap_clear_recovery();
        intmap_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    rt_seq_push(seq, value);
    rt_trap_clear_recovery();
    return 1;
}

/// @brief GC traversal: visit every stored value across all bucket chains.
/// @param obj IntMap object to traverse.
/// @param visitor Runtime callback invoked for each retained value.
/// @param ctx Opaque visitor context forwarded unchanged.
static void rt_intmap_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_intmap_impl *map = as_intmap(obj, "IntMap: invalid IntMap object");
    if (!map)
        return;
    if (!map->buckets || map->capacity == 0)
        return;
    for (size_t i = 0; i < map->capacity; ++i) {
        for (rt_intmap_entry *entry = map->buckets[i]; entry; entry = entry->next)
            visitor(entry->value, ctx);
    }
}

/// @brief Finalizer callback invoked when an IntMap is garbage collected.
/// @param obj Pointer to the IntMap object being finalized (NULL is a no-op).
static void rt_intmap_finalize(void *obj) {
    if (!obj)
        return;
    rt_intmap_impl *map = as_intmap(obj, "IntMap: invalid IntMap object");
    if (!map)
        return;
    rt_intmap_entry **buckets = map->buckets;
    size_t capacity = map->capacity;
    map->buckets = NULL;
    map->capacity = 0;
    map->count = 0;
    if (buckets) {
        for (size_t i = 0; i < capacity; ++i)
            intmap_destroy_entries(buckets[i]);
    }
    rt_free(buckets);
}

/// @brief Resize the hash table and rehash all entries.
/// @param map IntMap to resize.
/// @param new_capacity New number of buckets.
/// @return 1 when the replacement table was installed; 0 after trapping on
///         allocation overflow or allocation failure.
static int map_resize(rt_intmap_impl *map, size_t new_capacity) {
    if (new_capacity == 0 || new_capacity > SIZE_MAX / sizeof(rt_intmap_entry *) ||
        new_capacity * sizeof(rt_intmap_entry *) > (size_t)INT64_MAX) {
        rt_trap("IntMap: allocation size overflow");
        return 0;
    }
    rt_intmap_entry **new_buckets =
        (rt_intmap_entry **)rt_alloc((int64_t)(new_capacity * sizeof(rt_intmap_entry *)));
    if (!new_buckets) {
        rt_trap("IntMap: memory allocation failed");
        return 0;
    }

    // Rehash all entries
    for (size_t i = 0; i < map->capacity; ++i) {
        rt_intmap_entry *entry = map->buckets[i];
        while (entry) {
            rt_intmap_entry *next = entry->next;
            size_t idx = entry->hash % new_capacity;
            entry->next = new_buckets[idx];
            new_buckets[idx] = entry;
            entry = next;
        }
    }

    rt_free(map->buckets);
    map->buckets = new_buckets;
    map->capacity = new_capacity;
    return 1;
}

/// @brief Check if resize is needed and perform it.
/// @param map IntMap to potentially resize.
/// @param next_count Entry count after the pending insertion.
/// @return 1 if no resize was needed or resizing succeeded; 0 if resizing trapped.
static int maybe_resize_for_count(rt_intmap_impl *map, size_t next_count) {
    if (rt_hash_table_exceeds_load(next_count, map->capacity)) {
        size_t new_capacity = 0;
        if (!rt_hash_table_double_capacity(map->capacity, &new_capacity)) {
            rt_trap("IntMap: capacity overflow");
            return 0;
        }
        return map_resize(map, new_capacity);
    }
    return 1;
}

/// @brief Create a new empty IntMap.
/// @return New runtime-managed IntMap, or NULL after an allocation trap.
void *rt_intmap_new(void) {
    rt_intmap_impl *map =
        (rt_intmap_impl *)rt_obj_new_i64(RT_INTMAP_CLASS_ID, (int64_t)sizeof(rt_intmap_impl));
    if (!map) {
        rt_trap("IntMap: memory allocation failed");
        return NULL;
    }

    map->vptr = NULL;
    map->buckets =
        (rt_intmap_entry **)rt_alloc((int64_t)(MAP_INITIAL_CAPACITY * sizeof(rt_intmap_entry *)));
    if (!map->buckets) {
        if (rt_obj_release_check0(map))
            rt_obj_free(map);
        rt_trap("IntMap: memory allocation failed");
        return NULL;
    }
    map->capacity = MAP_INITIAL_CAPACITY;
    map->count = 0;
    rt_obj_set_finalizer(map, rt_intmap_finalize);
    rt_gc_track(map, rt_intmap_traverse);
    return map;
}

/// @brief Return the number of key-value pairs in the IntMap.
/// @param obj IntMap pointer (NULL returns 0).
/// @return Entry count.
int64_t rt_intmap_len(void *obj) {
    if (!obj)
        return 0;
    rt_intmap_impl *map = as_intmap(obj, "IntMap.Len: invalid IntMap object");
    return map ? (int64_t)map->count : 0;
}

/// @brief Check whether the IntMap is empty.
/// @param obj IntMap pointer (NULL returns 1).
/// @return 1 if empty, 0 otherwise.
int8_t rt_intmap_is_empty(void *obj) {
    return rt_intmap_len(obj) == 0;
}

/// @brief Set or update a key-value pair in the IntMap.
/// @param obj IntMap pointer (NULL is a no-op).
/// @param key Integer key.
/// @param value Value to store and retain; may be NULL.
/// @note Replacing a mapping retains the new value before releasing the old.
void rt_intmap_set(void *obj, int64_t key, void *value) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_intmap_impl *map = as_intmap(obj, "IntMap.Set: invalid IntMap object");
    if (!map) {
        rt_gc_mutator_exit();
        return;
    }
    if (!map->buckets || map->capacity == 0) {
        rt_gc_mutator_exit();
        return; // Bucket allocation failed
    }

    uint64_t hash = rt_fnv1a(&key, sizeof(key));
    size_t idx = hash % map->capacity;

    // Check if key already exists
    rt_intmap_entry *existing = find_entry(map->buckets[idx], hash, key);
    if (existing) {
        // Update existing entry
        void *old_value = existing->value;
        if (!rt_collection_retain_checked(value, "IntMap.Set: value retain failed")) {
            rt_gc_mutator_exit();
            return;
        }
        existing->value = value;
        rt_gc_mutator_exit();
        intmap_release_object(old_value);
        return;
    }

    if (map->count >= (size_t)INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("IntMap.Set: maximum size reached");
        return;
    }
    if (!maybe_resize_for_count(map, map->count + 1)) {
        rt_gc_mutator_exit();
        return;
    }
    idx = hash % map->capacity;

    if (!rt_collection_retain_checked(value, "IntMap.Set: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }

    // Create new entry
    rt_intmap_entry *entry = (rt_intmap_entry *)rt_alloc((int64_t)sizeof(rt_intmap_entry));
    if (!entry) {
        if (value && rt_obj_release_check0(value))
            rt_obj_free(value);
        rt_gc_mutator_exit();
        rt_trap("IntMap: memory allocation failed");
        return;
    }

    entry->key = key;
    entry->hash = hash;
    entry->value = value;

    // Insert at head of bucket chain
    entry->next = map->buckets[idx];
    map->buckets[idx] = entry;
    map->count++;
    rt_gc_mutator_exit();
}

/// @brief Retrieve the value associated with a key.
/// @param obj IntMap pointer (NULL returns NULL).
/// @param key Integer key.
/// @return Borrowed value pointer or NULL if not found. Use `rt_intmap_has()`
///         to distinguish absence from an explicitly stored NULL.
void *rt_intmap_get(void *obj, int64_t key) {
    if (!obj)
        return NULL;

    rt_intmap_impl *map = as_intmap(obj, "IntMap.Get: invalid IntMap object");
    if (!map || !map->buckets || map->capacity == 0)
        return NULL;

    uint64_t hash = rt_fnv1a(&key, sizeof(key));
    size_t idx = hash % map->capacity;

    rt_intmap_entry *entry = find_entry(map->buckets[idx], hash, key);
    return entry ? entry->value : NULL;
}

/// @brief Retrieve the value for a key, or a default if missing.
/// @param obj IntMap pointer (NULL returns default_value).
/// @param key Integer key.
/// @param default_value Fallback value when key is absent.
/// @return Borrowed existing value or the unmodified @p default_value pointer.
void *rt_intmap_get_or(void *obj, int64_t key, void *default_value) {
    if (!obj)
        return default_value;

    rt_intmap_impl *map = as_intmap(obj, "IntMap.GetOr: invalid IntMap object");
    if (!map || !map->buckets || map->capacity == 0)
        return default_value;

    uint64_t hash = rt_fnv1a(&key, sizeof(key));
    size_t idx = hash % map->capacity;

    rt_intmap_entry *entry = find_entry(map->buckets[idx], hash, key);
    return entry ? entry->value : default_value;
}

/// @brief Test whether a key exists in the IntMap.
/// @param obj IntMap pointer (NULL returns 0).
/// @param key Integer key.
/// @return 1 if present, 0 otherwise.
int8_t rt_intmap_has(void *obj, int64_t key) {
    if (!obj)
        return 0;

    rt_intmap_impl *map = as_intmap(obj, "IntMap.Has: invalid IntMap object");
    if (!map || !map->buckets || map->capacity == 0)
        return 0;

    uint64_t hash = rt_fnv1a(&key, sizeof(key));
    size_t idx = hash % map->capacity;

    return find_entry(map->buckets[idx], hash, key) ? 1 : 0;
}

/// @brief Remove the entry with the specified key.
/// @param obj IntMap pointer (NULL returns 0).
/// @param key Integer key to remove.
/// @return 1 if removed, 0 if not found.
int8_t rt_intmap_remove(void *obj, int64_t key) {
    if (!obj)
        return 0;

    rt_gc_mutator_enter();
    rt_intmap_impl *map = as_intmap(obj, "IntMap.Remove: invalid IntMap object");
    if (!map || !map->buckets || map->capacity == 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    uint64_t hash = rt_fnv1a(&key, sizeof(key));
    size_t idx = hash % map->capacity;

    rt_intmap_entry **prev_ptr = &map->buckets[idx];
    rt_intmap_entry *entry = map->buckets[idx];

    while (entry) {
        if (entry->hash == hash && entry->key == key) {
            *prev_ptr = entry->next;
            map->count--;
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

/// @brief Remove all entries from the IntMap.
/// @param obj IntMap pointer (NULL is a no-op).
/// @note Stored values are released and bucket capacity is retained.
void rt_intmap_clear(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_intmap_impl *map = as_intmap(obj, "IntMap.Clear: invalid IntMap object");
    if (!map) {
        rt_gc_mutator_exit();
        return;
    }
    if (!map->buckets || map->capacity == 0) {
        rt_gc_mutator_exit();
        return;
    }

    rt_intmap_entry *detached = intmap_detach_entries(map);
    rt_gc_mutator_exit();
    intmap_destroy_entries(detached);
}

/// @brief Compact an IntMap's bucket table without changing its entries.
/// @details Computes the smallest power-of-two table, no smaller than sixteen,
///          whose normal 75-percent threshold holds the current entry count.
///          The replacement array is allocated transactionally before any
///          cached-hash chain is relinked. A failed allocation therefore keeps
///          the original capacity and all key/value associations intact.
/// @param obj IntMap object to compact.
/// @return Non-zero when already minimal or successfully compacted; zero after
///         an invalid-handle, overflow, or allocation trap.
int8_t rt_intmap_trim(void *obj) {
    if (!obj) {
        rt_trap("IntMap.Trim: invalid IntMap object");
        return 0;
    }

    rt_gc_mutator_enter();
    rt_intmap_impl *map = as_intmap(obj, "IntMap.Trim: invalid IntMap object");
    if (!map || !map->buckets || map->capacity == 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t target_capacity = 0;
    if (!rt_hash_table_trim_capacity(map->count, MAP_INITIAL_CAPACITY, &target_capacity)) {
        rt_gc_mutator_exit();
        rt_trap("IntMap.Trim: capacity overflow");
        return 0;
    }
    if (target_capacity >= map->capacity) {
        rt_gc_mutator_exit();
        return 1;
    }

    int resized = map_resize(map, target_capacity);
    rt_gc_mutator_exit();
    return resized ? 1 : 0;
}

/// @brief Return all keys as a Seq of boxed integers.
/// @param obj IntMap pointer (NULL returns empty Seq).
/// @return New owning Seq containing newly boxed i64 keys in unspecified
///         bucket order.
void *rt_intmap_keys(void *obj) {
    void *result = rt_seq_new_owned();
    if (!result)
        return NULL;
    if (!obj)
        return result;

    rt_intmap_impl *map = as_intmap(obj, "IntMap.Keys: invalid IntMap object");
    if (!map || !map->buckets || map->capacity == 0) {
        intmap_release_object(result);
        return NULL;
    }

    // Iterate through all buckets and entries
    for (size_t i = 0; i < map->capacity; ++i) {
        rt_intmap_entry *entry = map->buckets[i];
        while (entry) {
            if (!intmap_append_key_or_release_seq(result, entry->key))
                return NULL;
            entry = entry->next;
        }
    }

    return result;
}

/// @brief Return all values as a Seq.
/// @param obj IntMap pointer (NULL returns empty Seq).
/// @return New owning Seq retaining all mapped values in unspecified bucket
///         order.
void *rt_intmap_values(void *obj) {
    void *result = rt_seq_new_owned();
    if (!result)
        return NULL;
    if (!obj)
        return result;

    rt_intmap_impl *map = as_intmap(obj, "IntMap.Values: invalid IntMap object");
    if (!map || !map->buckets || map->capacity == 0) {
        intmap_release_object(result);
        return NULL;
    }

    // Iterate through all buckets and entries
    for (size_t i = 0; i < map->capacity; ++i) {
        rt_intmap_entry *entry = map->buckets[i];
        while (entry) {
            if (!intmap_append_value_or_release_seq(result, entry->value))
                return NULL;
            entry = entry->next;
        }
    }

    return result;
}
