//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_concmap.c
// Purpose: Implements a thread-safe concurrent hash map for the
//          Zanna.Threads.ConcMap class. Uses FNV-1a hashing with separate
//          chaining and a single global mutex protecting all operations.
//          Supports Get, Set, Delete, ContainsKey, Keys, Values, and Count.
//
// Key invariants:
//   - All public operations acquire the mutex before reading or modifying state.
//   - Initial capacity is CM_INITIAL_CAPACITY (16); rehashes at load > 3/4.
//   - Rehash doubles capacity and redistributes all entries.
//   - Keys are rt_string values compared by content, hashed with FNV-1a.
//   - Stored values are void* object references; the map retains them.
//   - Deletion releases the retained value reference for the removed entry.
//
// Ownership/Lifetime:
//   - The map retains references to all stored values (via rt_object_retain).
//   - Keys are copied into the chain nodes; the map owns the copies.
//   - The finalizer releases all retained value references and frees all nodes.
//
// Links: src/runtime/threads/rt_concmap.h (public API),
//        src/runtime/rt_hash_util.h (FNV-1a hash helper)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Mutex-protected, string-keyed concurrent hash map implementation.
/// @details Entries use FNV-1a hashing and separate chaining. Key bytes are
///          copied, values are retained, and GC graph mutations are bracketed
///          so collector traversal can inspect the table without taking its
///          platform mutex.

#include "rt_concmap.h"

#include "rt_gc.h"
#include "rt_hash_util.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_internal.h"
#include "rt_threads.h"
#include "rt_trap.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#define CM_INITIAL_CAPACITY 16
#define CM_LOAD_FACTOR_NUM 3
#define CM_LOAD_FACTOR_DEN 4

/// @brief Install a trap-recovery jump target for the current thread.
/// @param buf Recovery buffer to install.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Clear the current thread's trap-recovery target.
void rt_trap_clear_recovery(void);
/// @brief Read the current thread's most recent trap message.
/// @return Borrowed diagnostic text, or null.
const char *rt_trap_get_error(void);

//=============================================================================
// Internal types
//=============================================================================

/// @brief One separately chained map entry with an owned key copy.
typedef struct cm_entry {
    char *key;
    size_t key_len;
    uint64_t hash;
    void *value;
    struct cm_entry *next;
} cm_entry;

/// @brief Runtime payload and platform mutex for a concurrent map.
typedef struct {
    void *vptr;
    cm_entry **buckets;
    size_t capacity;
    size_t count;
#if defined(_WIN32)
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
} rt_concmap_impl;

//=============================================================================
// Platform abstraction
//=============================================================================

#if defined(_WIN32)
#define CM_LOCK(cm) EnterCriticalSection(&(cm)->mutex)
#define CM_UNLOCK(cm) LeaveCriticalSection(&(cm)->mutex)
#else
#define CM_LOCK(cm) pthread_mutex_lock(&(cm)->mutex)
#define CM_UNLOCK(cm) pthread_mutex_unlock(&(cm)->mutex)
#endif

//=============================================================================
// Internal helpers
//=============================================================================

/// @brief Extract a borrowed (data, byte_length) view of a validated string key.
/// @details Used by every Get/Set/ContainsKey/Delete entry point so the
///          hash and compare paths can operate on (data, len) pairs
///          uniformly. Hashing uses the full byte length (not strlen) so
///          embedded NULs in keys remain distinct. Null denotes the empty key;
///          a forged handle traps and returns failure instead of aliasing it.
/// @param key Runtime string key, or null for the empty key.
/// @param out_data Receives borrowed key bytes.
/// @param out_len Receives the key length in bytes.
/// @return One on success, otherwise zero after a recoverable trap.
static int get_key_data(rt_string key, const char **out_data, size_t *out_len) {
    *out_data = "";
    *out_len = 0;
    if (!key) {
        return 1;
    }
    if (!rt_string_is_handle(key)) {
        rt_trap("ConcurrentMap: invalid string key");
        return 0;
    }
    const char *cstr = rt_string_cstr(key);
    if (!cstr)
        return 0;
    *out_data = cstr;
    *out_len = (size_t)rt_string_len_bytes(key);
    return 1;
}

/// @brief Linear-search a separate-chaining bucket for an entry whose key matches (key, key_len).
/// @details Bucket chains are short on average (load factor ≤ 3/4 enforced
///          via maybe_resize), so linear traversal is the right choice.
///          memcmp on the full byte length means embedded NULs don't
///          collapse keys together.
/// @param head First entry in the bucket chain, or null.
/// @param key Candidate key bytes.
/// @param key_len Candidate byte length.
/// @param hash Precomputed FNV-1a hash.
/// @return Pointer to the matching entry, or NULL if not found.
static cm_entry *find_entry(cm_entry *head, const char *key, size_t key_len, uint64_t hash) {
    cm_entry *e = head;
    while (e) {
        if (e->hash == hash && e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
        e = e->next;
    }
    return NULL;
}

/// @brief Validate-and-cast an opaque ConcurrentMap handle to its impl.
/// @details Standard pattern: NULL @p obj traps when @p trap_on_null is
///          set, otherwise returns NULL; wrong-class id always traps.
/// @param obj Candidate ConcurrentMap handle.
/// @param trap_on_null Nonzero to trap on a null handle.
/// @return Validated map payload, or null for an accepted null/trap path.
static rt_concmap_impl *concmap_require(void *obj, int8_t trap_on_null) {
    if (!obj) {
        if (trap_on_null)
            rt_trap("ConcurrentMap: null object");
        return NULL;
    }
    if (!rt_obj_is_instance(obj, RT_CONCMAP_CLASS_ID, sizeof(rt_concmap_impl))) {
        rt_trap("ConcurrentMap: invalid object");
        return NULL;
    }
    return (rt_concmap_impl *)obj;
}

/// @brief Retain a validated map receiver without allowing trap fallthrough.
/// @param obj Validated managed ConcurrentMap payload.
/// @return One when the receiver is live and retained, otherwise zero after trapping.
static int8_t concmap_retain_receiver(void *obj) {
    int32_t retained = rt_heap_try_retain_live(obj);
    if (retained == 1)
        return 1;
    rt_trap(retained < 0    ? "ConcurrentMap: receiver refcount overflow"
            : retained == 2 ? "ConcurrentMap: receiver has an immortal refcount"
                            : "ConcurrentMap: receiver is no longer live");
    return 0;
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Runtime-managed object reference, or null.
static void concmap_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Snapshot the current trap error message into @p buffer (or
///        @p fallback if none) so it survives lock cleanup before re-raise.
/// @param buffer Destination character array.
/// @param buffer_size Destination capacity including its terminator.
/// @param fallback Required message when no trap diagnostic is available.
static void concmap_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Release the runtime reference carried by a stored map @p value.
/// @param value Retained runtime value, or null.
static void release_retained_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Free an entry's heap storage (its key copy and the entry struct)
///        WITHOUT touching its value reference (see free_entry for that).
/// @param e Entry to destroy, or null.
static void free_entry_storage(cm_entry *e) {
    if (e) {
        free(e->key);
        free(e);
    }
}

/// @brief Free a single entry — its key copy, its retained value (refcount-aware), and the entry
/// itself.
/// @details Used by Delete and bucket clear paths. The key is a freshly
///          malloc'd copy so we own it; the value carries a runtime ref
///          that we release. NULL @p e is a no-op.
/// @param e Entry to destroy, or null.
static void free_entry(cm_entry *e) {
    if (e) {
        free(e->key);
        release_retained_value(e->value);
        free(e);
    }
}

/// @brief Retain a runtime reference on @p value for storage in @p entry,
///        trap-recovering on failure: if the retain traps, @p entry is freed
///        and the saved error (or @p fallback) is re-raised so the map never
///        keeps a half-constructed entry.
/// @param obj Retained map reference released during trap cleanup.
/// @param entry Prepared entry whose storage is freed on failure.
/// @param value Runtime value to retain, or null.
/// @param fallback Diagnostic used when the captured trap has no message.
/// @return 1 on success; the failure path cleans up and re-raises the trap.
static int8_t retain_value_or_free_entry(void *obj,
                                         cm_entry *entry,
                                         void *value,
                                         const char *fallback) {
    jmp_buf recovery;
    cm_entry *volatile entry_for_cleanup = entry;
    void *volatile obj_for_cleanup = obj;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        concmap_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        free_entry_storage((cm_entry *)entry_for_cleanup);
        concmap_release_object((void *)obj_for_cleanup);
        rt_trap(saved_error);
        return 0;
    }
    rt_obj_retain_maybe(value);
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Double the bucket array and redistribute every entry to the new modulus.
/// @details Triggered by maybe_resize when load factor exceeds the
///          threshold. Walks each old bucket chain, recomputes the FNV-1a
///          hash modulo the new capacity, and links the entry into its
///          new bucket. Refuses to grow past `SIZE_MAX / 2` to avoid the
///          `capacity * 2` overflow. On allocation failure leaves the map
///          untouched at the old capacity (callers continue to function;
///          only the load factor degrades).
/// @param cm Locked map to grow; invalid or unrepresentable capacities are ignored.
static void cm_resize(rt_concmap_impl *cm) {
    if (!cm || cm->capacity == 0 || cm->capacity > SIZE_MAX / 2)
        return;
    size_t new_cap = cm->capacity * 2;
    if (new_cap > SIZE_MAX / sizeof(cm_entry *))
        return;
    cm_entry **new_buckets = (cm_entry **)calloc(new_cap, sizeof(cm_entry *));
    if (!new_buckets)
        return;

    for (size_t i = 0; i < cm->capacity; i++) {
        cm_entry *e = cm->buckets[i];
        while (e) {
            cm_entry *next = e->next;
            size_t idx = (size_t)(e->hash % new_cap);
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }
    free(cm->buckets);
    cm->buckets = new_buckets;
    cm->capacity = new_cap;
}

/// @brief Trigger a resize when the entry count crosses the load-factor threshold.
/// @details Threshold is `capacity * CM_LOAD_FACTOR_NUM / CM_LOAD_FACTOR_DEN`
///          (currently 3/4). The threshold is computed in two pieces so the
///          intermediate `capacity * NUM` doesn't overflow size_t for
///          large maps. Caller must hold the map mutex.
/// @param cm Locked map whose post-insertion load is inspected.
static void maybe_resize(rt_concmap_impl *cm) {
    size_t threshold =
        (cm->capacity / CM_LOAD_FACTOR_DEN) * CM_LOAD_FACTOR_NUM +
        ((cm->capacity % CM_LOAD_FACTOR_DEN) * CM_LOAD_FACTOR_NUM) / CM_LOAD_FACTOR_DEN;
    if (cm->count > threshold) {
        cm_resize(cm);
    }
}

/// @brief Enumerate every strong managed value owned by a ConcurrentMap.
/// @details All bucket/link/value publications use the collector mutator
///          barrier. Exclusive collection can therefore traverse the raw
///          bucket chains without taking the map mutex, avoiding lock inversion
///          with a writer paused at the graph barrier.
/// @param obj ConcurrentMap payload being traversed.
/// @param visitor Collector visitor invoked for each non-null stored value.
/// @param ctx Opaque collector context forwarded to @p visitor.
static void cm_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    rt_concmap_impl *cm = (rt_concmap_impl *)obj;
    if (!cm || !visitor || !cm->buckets)
        return;
    for (size_t i = 0; i < cm->capacity; ++i) {
        for (cm_entry *entry = cm->buckets[i]; entry; entry = entry->next) {
            if (entry->value)
                visitor(entry->value, ctx);
        }
    }
}

/// @brief Publish a prepared entry while holding the ConcurrentMap mutex.
/// @details The caller has retained the value and verified that the key is
///          absent. Insertion and any resulting rehash occur within one graph
///          mutation so collector traversal sees a complete old or new table.
/// @param cm Locked map receiving the entry.
/// @param entry Fully initialized entry whose ownership transfers to @p cm.
static void cm_publish_entry_locked(rt_concmap_impl *cm, cm_entry *entry) {
    rt_gc_mutator_enter();
    size_t idx = (size_t)(entry->hash % cm->capacity);
    entry->next = cm->buckets[idx];
    cm->buckets[idx] = entry;
    cm->count++;
    maybe_resize(cm);
    rt_gc_mutator_exit();
}

/// @brief Replace an entry value while holding the ConcurrentMap mutex.
/// @details No reference count is changed here: the caller has retained the
///          new value and releases the returned old ownership after unlocking.
/// @param entry Existing locked entry to update.
/// @param value Already retained replacement value, possibly null.
/// @return Previous retained value whose ownership transfers to the caller.
static void *cm_replace_value_locked(cm_entry *entry, void *value) {
    rt_gc_mutator_enter();
    void *old_value = entry->value;
    entry->value = value;
    rt_gc_mutator_exit();
    return old_value;
}

/// @brief Free a detached list of entries after it is no longer map-reachable.
/// @param entries Head of a detached entry list, or null.
static void free_entry_list(cm_entry *entries) {
    while (entries) {
        cm_entry *next = entries->next;
        free_entry(entries);
        entries = next;
    }
}

/// @brief Detach every bucket chain and reset the count to zero.
/// @details Caller must hold the map mutex. The returned list is no longer
///          reachable from the map and may be freed after unlocking.
/// @param cm Locked map to empty.
/// @return Head of one combined detached entry list, or null.
static cm_entry *cm_detach_entries_unlocked(rt_concmap_impl *cm) {
    rt_gc_mutator_enter();
    cm_entry *entries = NULL;
    for (size_t i = 0; i < cm->capacity; i++) {
        cm_entry *e = cm->buckets[i];
        if (e) {
            cm_entry *tail = e;
            while (tail->next)
                tail = tail->next;
            tail->next = entries;
            entries = e;
        }
        cm->buckets[i] = NULL;
    }
    cm->count = 0;
    rt_gc_mutator_exit();
    return entries;
}

/// @brief GC finalizer for a `ConcMap` — detaches entries under the lock,
///        then releases retained values outside the mutex.
/// @param obj ConcurrentMap payload being finalized.
static void cm_finalizer(void *obj) {
    rt_concmap_impl *cm = (rt_concmap_impl *)obj;
    CM_LOCK(cm);
    cm_entry *entries = cm_detach_entries_unlocked(cm);
    cm_entry **buckets = cm->buckets;
    cm->buckets = NULL;
    cm->capacity = 0;
    CM_UNLOCK(cm);

    free_entry_list(entries);
    free(buckets);

#if defined(_WIN32)
    DeleteCriticalSection(&cm->mutex);
#else
    pthread_mutex_destroy(&cm->mutex);
#endif
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Create an empty concurrent map with 16 initial buckets.
/// @return New runtime-managed map, or null after an allocation, mutex, or GC
///         tracking trap.
void *rt_concmap_new(void) {
    rt_concmap_impl *cm =
        (rt_concmap_impl *)rt_obj_new_i64(RT_CONCMAP_CLASS_ID, (int64_t)sizeof(rt_concmap_impl));
    if (!cm) {
        rt_trap("ConcurrentMap: memory allocation failed");
        return NULL;
    }
    cm->vptr = NULL;
    cm->capacity = CM_INITIAL_CAPACITY;
    cm->count = 0;
    cm->buckets = (cm_entry **)calloc(CM_INITIAL_CAPACITY, sizeof(cm_entry *));
    if (!cm->buckets) {
        if (rt_obj_release_check0(cm))
            rt_obj_free(cm);
        rt_trap("ConcurrentMap: memory allocation failed");
        return NULL;
    }

#if defined(_WIN32)
    if (!InitializeCriticalSectionEx(&cm->mutex, 0, 0)) {
        free(cm->buckets);
        cm->buckets = NULL;
        concmap_release_object(cm);
        rt_trap("ConcurrentMap: mutex initialization failed");
        return NULL;
    }
#else
    if (pthread_mutex_init(&cm->mutex, NULL) != 0) {
        free(cm->buckets);
        cm->buckets = NULL;
        if (rt_obj_release_check0(cm))
            rt_obj_free(cm);
        rt_trap("ConcurrentMap: mutex initialization failed");
        return NULL;
    }
#endif

    rt_obj_set_finalizer(cm, cm_finalizer);

    jmp_buf recovery;
    rt_concmap_impl *volatile cm_for_cleanup = cm;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        concmap_save_trap_error(
            saved_error, sizeof(saved_error), "ConcurrentMap: GC tracking failed");
        rt_trap_clear_recovery();
        concmap_release_object((void *)cm_for_cleanup);
        rt_trap(saved_error);
        return NULL;
    }
    rt_gc_track(cm, cm_traverse);
    if (!rt_gc_is_tracked(cm)) {
        rt_trap("ConcurrentMap: GC tracking failed");
        return NULL;
    }
    rt_trap_clear_recovery();
    return cm;
}

/// @brief Read the exact entry count while holding the map mutex.
/// @param obj ConcurrentMap handle; null reports zero.
/// @return Entry count at the instant of the locked read.
int64_t rt_concmap_len(void *obj) {
    if (!obj)
        return 0;
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm)
        return 0;
    if (!concmap_retain_receiver(obj))
        return 0;
    CM_LOCK(cm);
    int64_t len = (int64_t)cm->count;
    CM_UNLOCK(cm);
    concmap_release_object(obj);
    return len;
}

/// @brief Test whether the locked entry count is zero.
/// @param obj ConcurrentMap handle; null is treated as empty.
/// @return 1 when empty, otherwise 0.
int8_t rt_concmap_is_empty(void *obj) {
    return rt_concmap_len(obj) == 0 ? 1 : 0;
}

/// @brief Atomically insert or replace a string-keyed value.
/// @details The complete key byte span is copied and @p value is retained. On
///          replacement the old value is released after unlocking. A null key
///          denotes the empty string and a null value is permitted.
/// @param obj ConcurrentMap handle; null is ignored.
/// @param key String key to copy, or null for the empty key.
/// @param value Runtime value to retain, or null.
void rt_concmap_set(void *obj, rt_string key, void *value) {
    if (!obj)
        return;
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm)
        return;
    if (!concmap_retain_receiver(obj))
        return;
    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, &key_data, &key_len)) {
        concmap_release_object(obj);
        return;
    }
    uint64_t h = rt_fnv1a(key_data, key_len);

    if (key_len == SIZE_MAX) {
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.Set: key too large");
        return;
    }

    cm_entry *new_entry = (cm_entry *)malloc(sizeof(cm_entry));
    if (!new_entry) {
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.Set: memory allocation failed");
        return;
    }
    new_entry->key = (char *)malloc(key_len + 1);
    if (!new_entry->key) {
        free(new_entry);
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.Set: memory allocation failed");
        return;
    }
    memcpy(new_entry->key, key_data, key_len);
    new_entry->key[key_len] = '\0';
    new_entry->key_len = key_len;
    new_entry->hash = h;
    new_entry->value = value;
    new_entry->next = NULL;
    if (!retain_value_or_free_entry(
            obj, new_entry, value, "ConcurrentMap.Set: value retain failed"))
        return;

    void *old_value = NULL;
    CM_LOCK(cm);

    size_t idx = (size_t)(h % cm->capacity);
    cm_entry *existing = find_entry(cm->buckets[idx], key_data, key_len, h);

    if (existing) {
        /* Update existing entry. */
        old_value = cm_replace_value_locked(existing, value);
        CM_UNLOCK(cm);
        free(new_entry->key);
        free(new_entry);
        concmap_release_object(obj);
        release_retained_value(old_value);
        return;
    }

    /* Insert new entry. */
    if (cm->count == SIZE_MAX) {
        CM_UNLOCK(cm);
        free_entry(new_entry);
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.Set: count overflow");
        return;
    }
    cm_publish_entry_locked(cm, new_entry);
    CM_UNLOCK(cm);
    concmap_release_object(obj);
}

/// @brief Look up a value by string key. Returns a freshly-retained reference (caller releases)
/// or NULL if absent. The string key is hashed via FNV-1a and the bucket is scanned linearly
/// (collision chaining); thread-safe via the map's mutex.
/// @param obj ConcurrentMap handle; null behaves as a miss.
/// @param key String key, or null for the empty key.
/// @return Retained stored value, or null for an absent or null-valued entry.
void *rt_concmap_get(void *obj, rt_string key) {
    if (!obj)
        return NULL;
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm)
        return NULL;
    if (!concmap_retain_receiver(obj))
        return NULL;
    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, &key_data, &key_len)) {
        concmap_release_object(obj);
        return NULL;
    }
    uint64_t h = rt_fnv1a(key_data, key_len);

    CM_LOCK(cm);
    size_t idx = (size_t)(h % cm->capacity);
    cm_entry *e = find_entry(cm->buckets[idx], key_data, key_len, h);
    void *result = e ? e->value : NULL;
    if (result) {
        rt_concmap_impl *volatile locked_cm = cm;
        void *volatile obj_for_cleanup = obj;
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) != 0) {
            char saved_error[256];
            concmap_save_trap_error(
                saved_error, sizeof(saved_error), "ConcurrentMap.Get: value retain failed");
            rt_trap_clear_recovery();
            CM_UNLOCK((rt_concmap_impl *)locked_cm);
            concmap_release_object((void *)obj_for_cleanup);
            rt_trap(saved_error);
            return NULL;
        }
        rt_obj_retain_maybe(result);
        rt_trap_clear_recovery();
    }
    CM_UNLOCK(cm);
    concmap_release_object(obj);
    return result;
}

/// @brief Look up a value, returning `default_value` if missing. Only the found value is
/// retained; `default_value`'s lifetime is the caller's responsibility (passed-through unchanged).
/// @param obj ConcurrentMap handle; null returns @p default_value.
/// @param key String key, or null for the empty key.
/// @param default_value Borrowed fallback returned unchanged on a miss.
/// @return Retained stored value on a hit, otherwise borrowed @p default_value.
void *rt_concmap_get_or(void *obj, rt_string key, void *default_value) {
    if (!obj) {
        rt_obj_retain_maybe(default_value); /* uniform owned result (ADR 0314) */
        return default_value;
    }
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm) {
        rt_obj_retain_maybe(default_value); /* uniform owned result (ADR 0314) */
        return default_value;
    }
    if (!concmap_retain_receiver(obj)) {
        rt_obj_retain_maybe(default_value); /* uniform owned result (ADR 0314) */
        return default_value;
    }
    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, &key_data, &key_len)) {
        concmap_release_object(obj);
        rt_obj_retain_maybe(default_value); /* uniform owned result (ADR 0314) */
        return default_value;
    }
    uint64_t h = rt_fnv1a(key_data, key_len);

    CM_LOCK(cm);
    size_t idx = (size_t)(h % cm->capacity);
    cm_entry *e = find_entry(cm->buckets[idx], key_data, key_len, h);
    void *result = e ? e->value : default_value;
    if (e && result) {
        rt_concmap_impl *volatile locked_cm = cm;
        void *volatile obj_for_cleanup = obj;
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) != 0) {
            char saved_error[256];
            concmap_save_trap_error(
                saved_error, sizeof(saved_error), "ConcurrentMap.GetOr: value retain failed");
            rt_trap_clear_recovery();
            CM_UNLOCK((rt_concmap_impl *)locked_cm);
            concmap_release_object((void *)obj_for_cleanup);
            rt_trap(saved_error);
            return NULL;
        }
        rt_obj_retain_maybe(result);
        rt_trap_clear_recovery();
    }
    if (!(e && result))
        rt_obj_retain_maybe(result); /* a miss hands back the retained default */
    CM_UNLOCK(cm);
    concmap_release_object(obj);
    return result;
}

/// @brief Test whether a key exists, including when its stored value is null.
/// @param obj ConcurrentMap handle; null behaves as empty.
/// @param key String key, or null for the empty key.
/// @return 1 when an entry exists, otherwise 0.
int8_t rt_concmap_has(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm)
        return 0;
    if (!concmap_retain_receiver(obj))
        return 0;
    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, &key_data, &key_len)) {
        concmap_release_object(obj);
        return 0;
    }
    uint64_t h = rt_fnv1a(key_data, key_len);

    CM_LOCK(cm);
    size_t idx = (size_t)(h % cm->capacity);
    cm_entry *e = find_entry(cm->buckets[idx], key_data, key_len, h);
    int8_t found = e ? 1 : 0;
    CM_UNLOCK(cm);
    concmap_release_object(obj);
    return found;
}

/// @brief Atomically insert a key only when no matching entry exists.
/// @details A prepared key copy and retained value are discarded if another
///          entry already owns the key when the mutex is acquired.
/// @param obj ConcurrentMap handle; null returns failure.
/// @param key String key to copy, or null for the empty key.
/// @param value Runtime value to retain on insertion, or null.
/// @return 1 when inserted, or 0 when the key already exists or input is null.
int8_t rt_concmap_set_if_missing(void *obj, rt_string key, void *value) {
    if (!obj)
        return 0;
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm)
        return 0;
    if (!concmap_retain_receiver(obj))
        return 0;
    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, &key_data, &key_len)) {
        concmap_release_object(obj);
        return 0;
    }
    uint64_t h = rt_fnv1a(key_data, key_len);

    if (key_len == SIZE_MAX) {
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.SetIfMissing: key too large");
        return 0;
    }

    cm_entry *e = (cm_entry *)malloc(sizeof(cm_entry));
    if (!e) {
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.SetIfMissing: memory allocation failed");
        return 0;
    }
    e->key = (char *)malloc(key_len + 1);
    if (!e->key) {
        free(e);
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.SetIfMissing: memory allocation failed");
        return 0;
    }
    memcpy(e->key, key_data, key_len);
    e->key[key_len] = '\0';
    e->key_len = key_len;
    e->hash = h;
    e->value = value;
    e->next = NULL;
    if (!retain_value_or_free_entry(
            obj, e, value, "ConcurrentMap.SetIfMissing: value retain failed"))
        return 0;

    CM_LOCK(cm);

    size_t idx = (size_t)(h % cm->capacity);
    cm_entry *existing = find_entry(cm->buckets[idx], key_data, key_len, h);
    if (existing) {
        CM_UNLOCK(cm);
        free(e->key);
        free(e);
        concmap_release_object(obj);
        release_retained_value(value);
        return 0;
    }

    if (cm->count == SIZE_MAX) {
        CM_UNLOCK(cm);
        free_entry(e);
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.SetIfMissing: count overflow");
        return 0;
    }
    cm_publish_entry_locked(cm, e);
    CM_UNLOCK(cm);
    concmap_release_object(obj);
    return 1;
}

/// @brief Atomically detach and destroy a matching entry.
/// @details The table edge is removed under the mutex and GC barrier; the key
///          storage and retained value are released after unlocking.
/// @param obj ConcurrentMap handle; null behaves as empty.
/// @param key String key, or null for the empty key.
/// @return 1 when removed, otherwise 0.
int8_t rt_concmap_remove(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm)
        return 0;
    if (!concmap_retain_receiver(obj))
        return 0;
    size_t key_len = 0;
    const char *key_data = NULL;
    if (!get_key_data(key, &key_data, &key_len)) {
        concmap_release_object(obj);
        return 0;
    }
    uint64_t h = rt_fnv1a(key_data, key_len);

    CM_LOCK(cm);
    size_t idx = (size_t)(h % cm->capacity);
    cm_entry **prev = &cm->buckets[idx];
    cm_entry *e = cm->buckets[idx];

    while (e) {
        if (e->hash == h && e->key_len == key_len && memcmp(e->key, key_data, key_len) == 0) {
            rt_gc_mutator_enter();
            *prev = e->next;
            cm->count--;
            e->next = NULL;
            rt_gc_mutator_exit();
            CM_UNLOCK(cm);
            free_entry(e);
            concmap_release_object(obj);
            return 1;
        }
        prev = &e->next;
        e = e->next;
    }

    CM_UNLOCK(cm);
    concmap_release_object(obj);
    return 0;
}

/// @brief Atomically detach all entries and release them after unlocking.
/// @param obj ConcurrentMap handle; null is ignored.
void rt_concmap_clear(void *obj) {
    if (!obj)
        return;
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm)
        return;
    if (!concmap_retain_receiver(obj))
        return;
    CM_LOCK(cm);
    cm_entry *entries = cm_detach_entries_unlocked(cm);
    CM_UNLOCK(cm);
    free_entry_list(entries);
    concmap_release_object(obj);
}

/// @brief Return a snapshot Seq containing every key currently in the map. The Seq owns its
/// element strings (`set_owns_elements(1)`) so caller release frees them. Order is bucket-walk
/// order — NOT insertion order; treat it as unordered.
/// @param obj ConcurrentMap handle; null yields an empty snapshot.
/// @return New owning sequence of copied key strings.
void *rt_concmap_keys(void *obj) {
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm)
        return seq;
    if (!concmap_retain_receiver(obj))
        return seq;

    size_t snapshot_count = 0;
    size_t total_bytes = 0;
    char **keys = NULL;
    size_t *lens = NULL;
    char *bytes = NULL;

    CM_LOCK(cm);
    snapshot_count = cm->count;
    for (size_t i = 0; i < cm->capacity; i++) {
        cm_entry *e = cm->buckets[i];
        while (e) {
            if (e->key_len > SIZE_MAX - total_bytes) {
                CM_UNLOCK(cm);
                concmap_release_object(obj);
                rt_trap("ConcurrentMap.Keys: allocation size overflow");
                return seq;
            }
            total_bytes += e->key_len;
            e = e->next;
        }
    }

    if (snapshot_count == 0) {
        CM_UNLOCK(cm);
        concmap_release_object(obj);
        return seq;
    }

    if (snapshot_count > SIZE_MAX / sizeof(char *) || snapshot_count > SIZE_MAX / sizeof(size_t)) {
        CM_UNLOCK(cm);
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.Keys: allocation size overflow");
        return seq;
    }

    keys = (char **)calloc(snapshot_count, sizeof(char *));
    lens = (size_t *)calloc(snapshot_count, sizeof(size_t));
    bytes = (char *)malloc(total_bytes ? total_bytes : 1);
    if (!keys || !lens || !bytes) {
        CM_UNLOCK(cm);
        free(keys);
        free(lens);
        free(bytes);
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.Keys: memory allocation failed");
        return seq;
    }

    size_t copied = 0;
    size_t offset = 0;
    for (size_t i = 0; i < cm->capacity && copied < snapshot_count; i++) {
        cm_entry *e = cm->buckets[i];
        while (e && copied < snapshot_count) {
            keys[copied] = bytes + offset;
            lens[copied] = e->key_len;
            memcpy(keys[copied], e->key, e->key_len);
            offset += e->key_len;
            copied++;
            e = e->next;
        }
    }
    CM_UNLOCK(cm);
    concmap_release_object(obj);

    rt_string volatile current_key = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        concmap_save_trap_error(
            saved_error, sizeof(saved_error), "ConcurrentMap.Keys: key copy failed");
        rt_trap_clear_recovery();
        if (current_key)
            rt_str_release_maybe((rt_string)current_key);
        free(keys);
        free(lens);
        free(bytes);
        concmap_release_object(seq);
        rt_trap(saved_error);
        return rt_seq_new_owned();
    }
    for (size_t i = 0; i < copied; i++) {
        current_key = rt_string_from_bytes(keys[i], lens[i]);
        rt_seq_push_raw(seq, (void *)current_key);
        current_key = NULL;
    }
    rt_trap_clear_recovery();

    free(keys);
    free(lens);
    free(bytes);
    return seq;
}

/// @brief Return a snapshot Seq of all values. Like `_keys`, the Seq owns its elements (so the
/// values get released when the Seq is) — implies a retain on each map value at snapshot time.
/// Bucket-walk order; not insertion order.
/// @param obj ConcurrentMap handle; null yields an empty snapshot.
/// @return New owning sequence of retained values in bucket-walk order.
void *rt_concmap_values(void *obj) {
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;
    rt_concmap_impl *cm = concmap_require(obj, 0);
    if (!cm)
        return seq;
    if (!concmap_retain_receiver(obj))
        return seq;

    size_t snapshot_count = 0;
    void **volatile values = NULL;
    CM_LOCK(cm);
    snapshot_count = cm->count;

    if (snapshot_count == 0) {
        CM_UNLOCK(cm);
        concmap_release_object(obj);
        return seq;
    }

    if (snapshot_count > SIZE_MAX / sizeof(void *)) {
        CM_UNLOCK(cm);
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.Values: allocation size overflow");
        return seq;
    }

    values = (void **)calloc(snapshot_count, sizeof(void *));
    if (!values) {
        CM_UNLOCK(cm);
        concmap_release_object(obj);
        rt_trap("ConcurrentMap.Values: memory allocation failed");
        return seq;
    }

    size_t volatile copied = 0;
    rt_concmap_impl *volatile locked_cm = cm;
    void *volatile obj_for_cleanup = obj;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        concmap_save_trap_error(
            saved_error, sizeof(saved_error), "ConcurrentMap.Values: value retain failed");
        rt_trap_clear_recovery();
        CM_UNLOCK((rt_concmap_impl *)locked_cm);
        for (size_t i = 0; i < copied; i++)
            release_retained_value(values[i]);
        free((void **)values);
        concmap_release_object((void *)obj_for_cleanup);
        rt_trap(saved_error);
        return seq;
    }
    for (size_t i = 0; i < cm->capacity && copied < snapshot_count; i++) {
        cm_entry *e = cm->buckets[i];
        while (e && copied < snapshot_count) {
            values[copied] = e->value;
            if (values[copied])
                rt_obj_retain_maybe(values[copied]);
            copied++;
            e = e->next;
        }
    }
    rt_trap_clear_recovery();
    CM_UNLOCK(cm);
    concmap_release_object(obj);

    size_t volatile transferred = 0;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        concmap_save_trap_error(
            saved_error, sizeof(saved_error), "ConcurrentMap.Values: snapshot append failed");
        rt_trap_clear_recovery();
        for (size_t i = transferred; i < copied; i++)
            release_retained_value(values[i]);
        free((void **)values);
        concmap_release_object(seq);
        rt_trap(saved_error);
        return rt_seq_new_owned();
    }
    for (transferred = 0; transferred < copied; transferred++)
        rt_seq_push_raw(seq, values[transferred]);
    rt_trap_clear_recovery();

    free(values);
    return seq;
}
