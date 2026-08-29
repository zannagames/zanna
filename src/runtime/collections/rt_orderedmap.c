//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_orderedmap.c
// Purpose: Implements an insertion-ordered string-keyed map that preserves the
//   order in which keys were first inserted. Combines a hash table for O(1)
//   key lookup with a doubly-linked list maintaining insertion order. Iteration
//   always visits entries in the order they were inserted, not hash order.
//
// Key invariants:
//   - Hash table starts at capacity 16 and resizes (doubles) at 75% load.
//   - Each entry node belongs to both a hash bucket chain (hash_next) and the
//     insertion-order doubly-linked list (prev/next). head = first inserted,
//     tail = last inserted.
//   - Updating an existing key's value preserves its position in the insertion
//     order; the node is not moved to the tail.
//   - Removing an entry unlinks it from both the bucket chain and the list.
//   - Key strings are heap-copied into entry nodes; the OrderedMap is
//     independent of the source rt_string lifetime.
//   - Values are retained on insertion and released on removal/finalization.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - OrderedMap objects are GC-managed (rt_obj_new_i64). The bucket array and
//     all entry nodes are freed by the GC finalizer (orderedmap_finalizer).
//
// Links: src/runtime/collections/rt_orderedmap.h (public API),
//        src/runtime/collections/rt_map.h (unordered map counterpart)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime insertion-ordered string map.
/// @details Each entry participates in both a keyed-hash collision chain and a
///          doubly linked first-insertion-order list. Keyed operations use the
///          hash table, while snapshots and positional access walk the list so
///          resizing never changes observable order.

#include "rt_orderedmap.h"
#include "rt_collection_ids.h"
#include "rt_collection_ownership.h"
#include "rt_error.h"
#include "rt_gc.h"
#include "rt_hash_table_util.h"
#include "rt_hash_util.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"

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

// ---------------------------------------------------------------------------
// Internal structure: doubly-linked list + hash table
// ---------------------------------------------------------------------------

/// @brief One entry shared by a hash bucket and the insertion-order list.
/// @details `hash_next` is independent of `prev`/`next`. The copied key has a
///          trailing NUL for convenience, but `key_len` defines identity and
///          preserves embedded NUL bytes.
typedef struct rt_om_entry {
    char *key;
    size_t key_len;
    void *value;
    struct rt_om_entry *hash_next; // Hash chain
    struct rt_om_entry *prev;      // Insertion order
    struct rt_om_entry *next;      // Insertion order
} rt_om_entry;

/// @brief GC-managed OrderedMap payload.
/// @details `head` and `tail` delimit first-insertion order; `buckets` provides
///          average constant-time lookup over the same @p count entries.
typedef struct {
    void *vptr;
    rt_om_entry **buckets;
    int64_t capacity;
    int64_t count;
    rt_om_entry *head; // First inserted
    rt_om_entry *tail; // Last inserted
    int8_t finalizing; // Owner teardown rejects callback-driven mutation
} rt_orderedmap_impl;

/// @brief Checked cast of an opaque handle to the OrderedMap implementation.
/// @details Raises a runtime-error trap with @p what if @p obj is NULL or
///          not an OrderedMap.
/// @param obj Opaque runtime object to validate.
/// @param what Diagnostic attached to the runtime-error trap on failure.
/// @return Validated OrderedMap implementation, or `NULL` after trapping.
static rt_orderedmap_impl *as_orderedmap(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_ORDEREDMAP_CLASS_ID, sizeof(rt_orderedmap_impl))) {
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR, Err_RuntimeError, -1, what);
        return NULL;
    }
    return (rt_orderedmap_impl *)obj;
}

// ---------------------------------------------------------------------------
// Hash helpers
// ---------------------------------------------------------------------------

/// @brief Per-process keyed hash of @p len bytes of @p key.
/// @details A null byte pointer is normalized to the empty string. The keyed
///          hash protects bucket selection without affecting iteration order.
/// @param key Borrowed key bytes.
/// @param len Number of identity bytes to hash.
/// @return Process-keyed 64-bit hash.
static uint64_t om_hash(const char *key, size_t len) {
    return rt_keyed_hash_bytes(key ? key : "", len);
}

/// @brief Borrow the complete byte representation of a runtime string key.
/// @details Null denotes the empty key. A forged or stale nonnull handle traps
///          and is never normalized to a valid empty-key lookup.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @param what Diagnostic raised for a stale or forged nonnull handle.
/// @param out_data Receives borrowed key bytes.
/// @param out_len Receives the number of key-identity bytes.
/// @return Nonzero for a null or live string handle; otherwise zero after
///         trapping.
static int om_key_data(rt_string key, const char *what, const char **out_data, size_t *out_len) {
    *out_data = "";
    *out_len = 0;
    if (!key) {
        return 1;
    }
    if (!rt_string_is_handle(key)) {
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR, Err_RuntimeError, -1, what);
        return 0;
    }
    int64_t len = rt_str_len(key);
    if (len <= 0)
        return 1;
    const char *cstr = rt_string_cstr(key);
    if (!cstr) {
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR, Err_RuntimeError, -1, what);
        return 0;
    }
    *out_data = cstr;
    *out_len = (size_t)len;
    return 1;
}

/// @brief Drop one GC reference to a stored value and free it at zero.
/// @param value Runtime object being released, or `NULL` for a no-op.
static void om_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Save the active trap diagnostic before clearing a recovery frame.
/// @param buffer Destination for the bounded diagnostic copy.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when the trap subsystem has no text.
static void om_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Consume a detached insertion-order list despite trapping finalizers.
/// @details Each native node and key is reclaimed before its value is released,
///          so callback re-entry cannot observe or free the node again. When a
///          value finalizer traps non-locally, cleanup resumes at the following
///          node and the first diagnostic is returned after the whole list has
///          been drained.
/// @param head Detached list head, or NULL.
/// @param error Buffer receiving the first trapped diagnostic.
/// @param error_size Capacity of @p error including its terminator.
/// @return Nonzero when at least one value cleanup trapped.
static int om_destroy_entries(rt_om_entry *head, char *error, size_t error_size) {
    rt_om_entry *volatile cursor = head;
    volatile int trapped = 0;
    jmp_buf recovery;

    rt_gc_mutator_enter();
    rt_trap_set_recovery(&recovery);
    for (;;) {
        if (RT_SETJMP(recovery) != 0) {
            // Trap dispatch unwinds managed mutation scopes before longjmp.
            rt_gc_mutator_enter();
            if (!trapped)
                om_save_trap_error(error, error_size, "OrderedMap: value finalizer cleanup failed");
            trapped = 1;
        }

        rt_om_entry *entry = (rt_om_entry *)cursor;
        if (!entry)
            break;
        cursor = entry->next;
        void *value = entry->value;
        entry->value = NULL;
        free(entry->key);
        entry->key = NULL;
        free(entry);
        om_release_value(value);
    }
    rt_trap_clear_recovery();
    rt_gc_mutator_exit();
    return trapped ? 1 : 0;
}

/// @brief Append a copied key to an owning snapshot, consuming it on failure.
/// @param seq Partial owning snapshot.
/// @param key Borrowed key bytes.
/// @param key_len Number of key bytes.
/// @return One after append; zero after consuming @p seq and retrapping.
static int om_append_key_or_release_seq(void *seq, const char *key, size_t key_len) {
    volatile rt_string key_copy = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        om_save_trap_error(
            saved_error, sizeof(saved_error), "OrderedMap.Keys: snapshot append failed");
        rt_trap_clear_recovery();
        if (key_copy)
            rt_str_release_maybe((rt_string)key_copy);
        om_release_value(seq);
        rt_trap(saved_error);
        return 0;
    }

    key_copy = rt_string_from_bytes(key, key_len);
    if (!key_copy)
        rt_trap("OrderedMap.Keys: key allocation failed");
    int64_t old_len = rt_seq_len(seq);
    rt_seq_push_raw(seq, (void *)key_copy);
    if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len(seq) != old_len + 1)
        rt_trap("OrderedMap.Keys: snapshot append failed");
    key_copy = NULL;
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Append a retained value to an owning snapshot, consuming it on failure.
/// @param seq Partial owning snapshot.
/// @param value Borrowed stored value; may be NULL.
/// @return One after append; zero after consuming @p seq and retrapping.
static int om_append_value_or_release_seq(void *seq, void *value) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        om_save_trap_error(
            saved_error, sizeof(saved_error), "OrderedMap.Values: snapshot append failed");
        rt_trap_clear_recovery();
        om_release_value(seq);
        rt_trap(saved_error);
        return 0;
    }

    int64_t old_len = rt_seq_len(seq);
    rt_seq_push(seq, value);
    if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len(seq) != old_len + 1)
        rt_trap("OrderedMap.Values: snapshot append failed");
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Find an exact byte-string key through its collision chain.
/// @param m OrderedMap with a live nonzero-capacity bucket array.
/// @param key Borrowed key bytes.
/// @param len Number of identity bytes in @p key.
/// @return Borrowed matching entry, or `NULL` when absent.
static rt_om_entry *om_find(rt_orderedmap_impl *m, const char *key, size_t len) {
    uint64_t idx = om_hash(key, len) % (uint64_t)m->capacity;
    rt_om_entry *e = m->buckets[idx];
    while (e) {
        if (e->key_len == len && memcmp(e->key, key, len) == 0)
            return e;
        e = e->hash_next;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

/// @brief Double the bucket array and rehash entries, preserving insertion
///        order by re-linking via the head→next list. Traps on overflow/OOM.
/// @details Allocation completes before publication, so allocation failure
///          leaves the original buckets and links intact. Only `hash_next`
///          links change; `prev`/`next`, `head`, and `tail` remain untouched.
/// @param m OrderedMap whose bucket load reached the resize threshold.
/// @return 1 after publishing the doubled table, or 0 after trapping.
static int om_resize(rt_orderedmap_impl *m) {
    // Guard against integer overflow before doubling.
    if (m->capacity > INT64_MAX / 2) {
        rt_trap_raise_kind(
            RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "OrderedMap: capacity overflow during resize");
        return 0;
    }

    int64_t new_cap = m->capacity * 2;
    if ((uint64_t)new_cap > SIZE_MAX / sizeof(rt_om_entry *)) {
        rt_trap_raise_kind(RT_TRAP_KIND_OVERFLOW,
                           Err_Overflow,
                           -1,
                           "OrderedMap: allocation size overflow during resize");
        return 0;
    }
    rt_om_entry **new_buckets = (rt_om_entry **)calloc((size_t)new_cap, sizeof(rt_om_entry *));
    if (!new_buckets) {
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "OrderedMap: memory allocation failed during resize");
        return 0;
    }

    // Re-hash all entries via insertion-order list
    rt_om_entry *e = m->head;
    while (e) {
        uint64_t idx = om_hash(e->key, e->key_len) % (uint64_t)new_cap;
        e->hash_next = new_buckets[idx];
        new_buckets[idx] = e;
        e = e->next;
    }

    free(m->buckets);
    m->buckets = new_buckets;
    m->capacity = new_cap;
    return 1;
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

/// @brief GC finalizer: walk the insertion-order list freeing each entry
///        (key + released value), then free the bucket array.
/// @param obj OrderedMap object being finalized; `NULL` is ignored.
static void orderedmap_finalizer(void *obj) {
    if (!obj)
        return;
    rt_orderedmap_impl *m = as_orderedmap(obj, "OrderedMap: invalid OrderedMap object");
    if (!m)
        return;

    rt_om_entry *entries = m->head;
    rt_om_entry **buckets = m->buckets;
    int64_t capacity = m->capacity;
    m->finalizing = 1;
    m->buckets = NULL;
    m->capacity = 0;
    m->head = m->tail = NULL;
    m->count = 0;

    char cleanup_error[256] = {0};
    int cleanup_trapped = om_destroy_entries(entries, cleanup_error, sizeof(cleanup_error));

    rt_heap_info_t info;
    int resurrected = rt_heap_get_info(obj, &info) && info.refcnt != 0;
    if (resurrected && buckets && capacity > 0) {
        memset(buckets, 0, (size_t)capacity * sizeof(rt_om_entry *));
        m->buckets = buckets;
        m->capacity = capacity;
        buckets = NULL;
        // Finalizer slots are one-shot. A resurrected owner must re-arm its
        // cleanup before it can be reused and released a second time.
        rt_obj_set_finalizer(m, orderedmap_finalizer);
    }
    m->finalizing = 0;
    free(buckets);

    if (cleanup_trapped)
        rt_trap(cleanup_error[0] ? cleanup_error : "OrderedMap: value finalizer cleanup failed");
}

/// @brief GC traversal: visit every stored value in insertion order.
/// @details Native entry nodes and copied key buffers are not runtime objects
///          and therefore are not visited.
/// @param obj OrderedMap whose retained values are to be traced.
/// @param visitor Collector callback invoked for every stored value.
/// @param ctx Opaque collector context forwarded unchanged.
static void orderedmap_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_orderedmap_impl *m = as_orderedmap(obj, "OrderedMap: invalid OrderedMap object");
    if (!m)
        return;
    for (rt_om_entry *e = m->head; e; e = e->next)
        visitor(e->value, ctx);
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/// @brief Construct an empty insertion-ordered string map.
/// @details Allocates sixteen initial buckets, installs the native finalizer,
///          and registers stored-value traversal with the collector.
/// @return New runtime-managed OrderedMap, or `NULL` after an allocation trap.
void *rt_orderedmap_new(void) {
    rt_orderedmap_impl *m =
        (rt_orderedmap_impl *)rt_obj_new_i64(RT_ORDEREDMAP_CLASS_ID, sizeof(rt_orderedmap_impl));
    if (!m) {
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "OrderedMap: memory allocation failed");
        return NULL;
    }
    m->capacity = 16;
    m->count = 0;
    m->buckets = (rt_om_entry **)calloc(16, sizeof(rt_om_entry *));
    if (!m->buckets) {
        if (rt_obj_release_check0(m))
            rt_obj_free(m);
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "OrderedMap: memory allocation failed");
        return NULL;
    }
    m->head = m->tail = NULL;
    m->finalizing = 0;
    rt_obj_set_finalizer(m, orderedmap_finalizer);
    rt_gc_track(m, orderedmap_traverse);
    return m;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

/// @brief Return the number of entries in the ordered map.
/// @details A null map is treated as empty; an invalid non-null handle traps.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @return Number of distinct keys, or 0 for `NULL`.
int64_t rt_orderedmap_len(void *map) {
    if (!map)
        return 0;
    rt_orderedmap_impl *impl = as_orderedmap(map, "OrderedMap.Len: invalid OrderedMap object");
    return impl ? impl->count : 0;
}

/// @brief Check whether the ordered map has no entries.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @return 1 when no keys are stored, otherwise 0; `NULL` is empty.
int8_t rt_orderedmap_is_empty(void *map) {
    if (!map)
        return 1;
    rt_orderedmap_impl *impl = as_orderedmap(map, "OrderedMap.IsEmpty: invalid OrderedMap object");
    return !impl || impl->count == 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Set
// ---------------------------------------------------------------------------

/// @brief Insert or update a key-value pair, preserving insertion order.
/// @details New keys are appended to the end of the order. Updating an
///          existing key retains the replacement before releasing the old
///          value, making self-replacement safe, and keeps the node in place.
///          Null keys identify the empty byte string and null values are valid.
///          A new key is copied before publication. A null map is a no-op.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @param value Runtime object retained while stored; may be `NULL`.
void rt_orderedmap_set(void *map, rt_string key, void *value) {
    if (!map)
        return;
    rt_gc_mutator_enter();
    rt_orderedmap_impl *m = as_orderedmap(map, "OrderedMap.Set: invalid OrderedMap object");
    if (!m) {
        rt_gc_mutator_exit();
        return;
    }

    if (m->finalizing) {
        rt_gc_mutator_exit();
        return;
    }
    if (!m->buckets || m->capacity <= 0) {
        rt_gc_mutator_exit();
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "OrderedMap.Set: unavailable bucket table");
        return;
    }

    size_t klen = 0;
    const char *kstr = NULL;
    if (!om_key_data(key, "OrderedMap.Set: invalid key", &kstr, &klen)) {
        rt_gc_mutator_exit();
        return;
    }

    // Check for existing key
    rt_om_entry *existing = om_find(m, kstr, klen);
    if (existing) {
        // Update value in-place (preserves order)
        if (!rt_collection_retain_checked(value, "OrderedMap.Set: value retain failed")) {
            rt_gc_mutator_exit();
            return;
        }
        void *old_value = existing->value;
        existing->value = value;
        rt_gc_mutator_exit();
        om_release_value(old_value);
        return;
    }

    if (m->count == INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap_raise_kind(
            RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "OrderedMap.Set: maximum size reached");
        return;
    }

    // Resize if needed
    if (rt_hash_table_exceeds_load((size_t)m->count + 1, (size_t)m->capacity) && !om_resize(m)) {
        rt_gc_mutator_exit();
        return;
    }

    if (!rt_collection_retain_checked(value, "OrderedMap.Set: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }

    // Create new entry
    rt_om_entry *e = (rt_om_entry *)calloc(1, sizeof(rt_om_entry));
    if (!e) {
        om_release_value(value);
        rt_gc_mutator_exit();
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "OrderedMap: entry allocation failed");
        return;
    }
    if (klen == SIZE_MAX) {
        om_release_value(value);
        free(e);
        rt_gc_mutator_exit();
        rt_trap_raise_kind(
            RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "OrderedMap: key allocation overflow");
        return;
    }
    e->key = (char *)malloc(klen + 1);
    if (!e->key) {
        om_release_value(value);
        free(e);
        rt_gc_mutator_exit();
        rt_trap_raise_kind(
            RT_TRAP_KIND_RUNTIME_ERROR, Err_RuntimeError, -1, "OrderedMap: key allocation failed");
        return;
    }
    memcpy(e->key, kstr, klen);
    e->key[klen] = '\0';
    e->key_len = klen;
    e->value = value;

    // Add to hash chain
    uint64_t idx = om_hash(kstr, klen) % (uint64_t)m->capacity;
    e->hash_next = m->buckets[idx];
    m->buckets[idx] = e;

    // Add to insertion-order list (tail)
    e->prev = m->tail;
    e->next = NULL;
    if (m->tail)
        m->tail->next = e;
    else
        m->head = e;
    m->tail = e;

    m->count++;
    rt_gc_mutator_exit();
}

// ---------------------------------------------------------------------------
// Get / Has
// ---------------------------------------------------------------------------

/// @brief Borrow the value associated with an exact key.
/// @details The map retains ownership; the pointer remains valid only while
///          that association remains stored. Because `NULL` is a valid value,
///          use @ref rt_orderedmap_has to distinguish a missing key. A null map
///          returns `NULL`.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return Borrowed stored value, or `NULL` for absence/a stored null.
void *rt_orderedmap_get(void *map, rt_string key) {
    if (!map)
        return NULL;
    rt_orderedmap_impl *m = as_orderedmap(map, "OrderedMap.Get: invalid OrderedMap object");
    if (!m || !m->buckets || m->capacity <= 0)
        return NULL;

    size_t klen = 0;
    const char *kstr = NULL;
    if (!om_key_data(key, "OrderedMap.Get: invalid key", &kstr, &klen))
        return NULL;

    rt_om_entry *e = om_find(m, kstr, klen);
    return e ? e->value : NULL;
}

/// @brief Check whether a key exists in the ordered map.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return 1 when the exact byte-string key exists, otherwise 0.
int8_t rt_orderedmap_has(void *map, rt_string key) {
    if (!map)
        return 0;
    rt_orderedmap_impl *m = as_orderedmap(map, "OrderedMap.Has: invalid OrderedMap object");
    if (!m || !m->buckets || m->capacity <= 0)
        return 0;

    size_t klen = 0;
    const char *kstr = NULL;
    if (!om_key_data(key, "OrderedMap.Has: invalid key", &kstr, &klen))
        return 0;

    return om_find(m, kstr, klen) != NULL ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

/// @brief Remove a key-value pair from the ordered map.
/// @details The entry is removed from both the hash table and the
///          insertion-order linked list. Its copied key is freed and its
///          stored value is released. Surviving entries keep their relative
///          order and the bucket allocation is retained.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @return 1 when the key was removed, otherwise 0.
int8_t rt_orderedmap_remove(void *map, rt_string key) {
    if (!map)
        return 0;
    rt_gc_mutator_enter();
    rt_orderedmap_impl *m = as_orderedmap(map, "OrderedMap.Remove: invalid OrderedMap object");
    if (!m || m->finalizing || !m->buckets || m->capacity <= 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t klen = 0;
    const char *kstr = NULL;
    if (!om_key_data(key, "OrderedMap.Remove: invalid key", &kstr, &klen)) {
        rt_gc_mutator_exit();
        return 0;
    }

    uint64_t idx = om_hash(kstr, klen) % (uint64_t)m->capacity;

    // Remove from hash chain
    rt_om_entry **pp = &m->buckets[idx];
    while (*pp) {
        rt_om_entry *e = *pp;
        if (e->key_len == klen && memcmp(e->key, kstr, klen) == 0) {
            *pp = e->hash_next;

            // Remove from insertion-order list
            if (e->prev)
                e->prev->next = e->next;
            else
                m->head = e->next;
            if (e->next)
                e->next->prev = e->prev;
            else
                m->tail = e->prev;

            m->count--;
            void *value = e->value;
            e->value = NULL;
            free(e->key);
            e->key = NULL;
            free(e);
            rt_gc_mutator_exit();
            om_release_value(value);
            return 1;
        }
        pp = &e->hash_next;
    }
    rt_gc_mutator_exit();
    return 0;
}

// ---------------------------------------------------------------------------
// Keys / Values
// ---------------------------------------------------------------------------

/// @brief Snapshot every key in first-insertion order.
/// @details Each key is copied by its complete byte length into a fresh
///          runtime string retained by the owning result `Seq`. A null map
///          returns an empty owning sequence.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @return New runtime-managed owning `Seq` of copied keys.
void *rt_orderedmap_keys(void *map) {
    rt_orderedmap_impl *m = NULL;
    if (map) {
        m = as_orderedmap(map, "OrderedMap.Keys: invalid OrderedMap object");
        if (!m)
            return NULL;
    }

    int64_t initial_capacity = m && m->count > 0 ? m->count : 1;
    void *seq = rt_seq_with_capacity_owned(initial_capacity);
    if (!seq)
        return NULL;
    if (!m)
        return seq;

    rt_om_entry *e = m->head;
    while (e) {
        if (!om_append_key_or_release_seq(seq, e->key, e->key_len))
            return NULL;
        e = e->next;
    }
    return seq;
}

/// @brief Snapshot every value in first-insertion order.
/// @details The fresh owning `Seq` independently retains non-null values and
///          preserves stored `NULL` entries. Mutating the snapshot does not
///          change the map. A null map returns an empty sequence.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @return New runtime-managed owning `Seq` of stored values.
void *rt_orderedmap_values(void *map) {
    rt_orderedmap_impl *m = NULL;
    if (map) {
        m = as_orderedmap(map, "OrderedMap.Values: invalid OrderedMap object");
        if (!m)
            return NULL;
    }

    int64_t initial_capacity = m && m->count > 0 ? m->count : 1;
    void *seq = rt_seq_with_capacity_owned(initial_capacity);
    if (!seq)
        return NULL;
    if (!m)
        return seq;

    rt_om_entry *e = m->head;
    while (e) {
        if (!om_append_value_or_release_seq(seq, e->value))
            return NULL;
        e = e->next;
    }
    return seq;
}

/// @brief Return the key at the given insertion-order index.
/// @details Walks the insertion-order linked list in O(index) time and copies
///          the complete key bytes into a fresh runtime string.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @param index Zero-based position in insertion order.
/// @return New runtime-managed key string, or `NULL` when @p map is null or
///         @p index is out of range.
rt_string rt_orderedmap_key_at(void *map, int64_t index) {
    if (!map)
        return NULL;
    rt_orderedmap_impl *m = as_orderedmap(map, "OrderedMap.KeyAt: invalid OrderedMap object");
    if (!m)
        return NULL;

    if (index < 0 || index >= m->count)
        return NULL;

    rt_om_entry *e = m->head;
    for (int64_t i = 0; i < index; i++)
        e = e->next;

    return rt_string_from_bytes(e->key, e->key_len);
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

/// @brief Remove all entries from the ordered map.
/// @details Releases all retained references and resets both the hash
///          table and the insertion-order list. The current bucket allocation
///          is zeroed and retained for reuse. A null map is a no-op.
/// @param map Opaque OrderedMap handle, or `NULL`.
void rt_orderedmap_clear(void *map) {
    if (!map)
        return;
    rt_gc_mutator_enter();
    rt_orderedmap_impl *m = as_orderedmap(map, "OrderedMap.Clear: invalid OrderedMap object");
    if (!m) {
        rt_gc_mutator_exit();
        return;
    }
    if (m->finalizing) {
        rt_gc_mutator_exit();
        return;
    }

    rt_om_entry *entries = m->head;
    m->head = m->tail = NULL;
    m->count = 0;
    if (m->buckets)
        memset(m->buckets, 0, (size_t)m->capacity * sizeof(rt_om_entry *));
    rt_gc_mutator_exit();

    char cleanup_error[256] = {0};
    if (om_destroy_entries(entries, cleanup_error, sizeof(cleanup_error)))
        rt_trap(cleanup_error[0] ? cleanup_error
                                 : "OrderedMap.Clear: value finalizer cleanup failed");
}
