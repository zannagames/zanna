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
#include "rt_error.h"
#include "rt_gc.h"
#include "rt_hash_util.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <stdlib.h>
#include <string.h>

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
/// @details Null, empty, or unreadable string handles normalize to the empty
///          key. Returned storage remains owned by @p key.
/// @param key Runtime string key; `NULL` denotes the empty key.
/// @param out_len Receives the number of key-identity bytes.
/// @return Borrowed key bytes, never `NULL`.
static const char *om_key_data(rt_string key, size_t *out_len) {
    if (!key) {
        *out_len = 0;
        return "";
    }
    int64_t len = rt_str_len(key);
    if (len <= 0) {
        *out_len = 0;
        return "";
    }
    const char *cstr = rt_string_cstr(key);
    if (!cstr) {
        *out_len = 0;
        return "";
    }
    *out_len = (size_t)len;
    return cstr;
}

/// @brief Drop one GC reference to a stored value and free it at zero.
/// @param value Runtime object being released, or `NULL` for a no-op.
static void om_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
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
    rt_om_entry *e = m->head;
    m->head = m->tail = NULL;
    m->count = 0;
    if (m->buckets)
        memset(m->buckets, 0, (size_t)m->capacity * sizeof(rt_om_entry *));
    while (e) {
        rt_om_entry *next = e->next;
        free(e->key);
        om_release_value(e->value);
        free(e);
        e = next;
    }
    free(m->buckets);
    m->buckets = NULL;
    m->capacity = 0;
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
    return as_orderedmap(map, "OrderedMap.Len: invalid OrderedMap object")->count;
}

/// @brief Check whether the ordered map has no entries.
/// @param map Opaque OrderedMap handle, or `NULL`.
/// @return 1 when no keys are stored, otherwise 0; `NULL` is empty.
int8_t rt_orderedmap_is_empty(void *map) {
    if (!map)
        return 1;
    return as_orderedmap(map, "OrderedMap.IsEmpty: invalid OrderedMap object")->count == 0 ? 1 : 0;
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

    size_t klen;
    const char *kstr = om_key_data(key, &klen);

    // Check for existing key
    rt_om_entry *existing = om_find(m, kstr, klen);
    if (existing) {
        // Update value in-place (preserves order)
        if (value)
            rt_obj_retain_maybe(value);
        void *old_value = existing->value;
        existing->value = value;
        om_release_value(old_value);
        rt_gc_mutator_exit();
        return;
    }

    // Resize if needed
    if ((long double)m->count * 4.0L >= (long double)m->capacity * 3.0L && !om_resize(m)) {
        rt_gc_mutator_exit();
        return;
    }

    if (value)
        rt_obj_retain_maybe(value);

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

    size_t klen;
    const char *kstr = om_key_data(key, &klen);

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

    size_t klen;
    const char *kstr = om_key_data(key, &klen);

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
    if (!m || m->capacity <= 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t klen;
    const char *kstr = om_key_data(key, &klen);

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

            free(e->key);
            om_release_value(e->value);
            free(e);
            m->count--;
            rt_gc_mutator_exit();
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
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    if (!map)
        return seq;
    rt_orderedmap_impl *m = as_orderedmap(map, "OrderedMap.Keys: invalid OrderedMap object");

    rt_om_entry *e = m->head;
    while (e) {
        rt_string k = rt_string_from_bytes(e->key, e->key_len);
        rt_seq_push(seq, k);
        rt_str_release_maybe(k);
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
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    if (!map)
        return seq;
    rt_orderedmap_impl *m = as_orderedmap(map, "OrderedMap.Values: invalid OrderedMap object");

    rt_om_entry *e = m->head;
    while (e) {
        if (e->value)
            rt_seq_push(seq, e->value);
        else
            rt_seq_push(seq, NULL);
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

    rt_om_entry *e = m->head;
    m->head = m->tail = NULL;
    m->count = 0;
    if (m->buckets)
        memset(m->buckets, 0, (size_t)m->capacity * sizeof(rt_om_entry *));
    while (e) {
        rt_om_entry *next = e->next;
        free(e->key);
        om_release_value(e->value);
        free(e);
        e = next;
    }
    rt_gc_mutator_exit();
}
