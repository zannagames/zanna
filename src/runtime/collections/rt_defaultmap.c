//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_defaultmap.c
// Purpose: Implements a string-keyed hash map with a configurable default value
//   returned when a key is not present. DefaultMap behaves like a regular Map
//   for Set/Remove/Contains, but Get returns the default_value (set at creation)
//   instead of NULL for missing keys. Useful for counters, accumulators, and
//   lookup tables where a "zero" or sentinel default is needed.
//
// Key invariants:
//   - Backed by a hash table with initial capacity 16 buckets and separate
//     chaining using the runtime keyed hash.
//   - Resizes (doubles) when count/capacity exceeds 75%.
//   - Get returns default_value (NOT a copy) for missing keys; callers must
//     not mutate the returned default object.
//   - The default_value pointer is retained at construction time and released
//     by the finalizer.
//   - Each entry owns a heap-copied key string; values are retained by the map
//     and released on removal or finalization.
//   - All operations are O(1) average case; O(n) worst case.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - DefaultMap objects are GC-managed (rt_obj_new_i64). The bucket array
//     and all entry nodes are freed by the GC finalizer.
//
// Links: src/runtime/collections/rt_defaultmap.h (public API),
//        src/runtime/collections/rt_map.h (standard map without default value)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a string-keyed map with a configured fallback value.
///
/// DefaultMap stores copied key bytes and retained opaque runtime values in a
/// separately chained, keyed-hash table. Lookup returns a stored value when an
/// explicit mapping exists and otherwise returns the constructor-supplied
/// default. Both results are borrowed pointers rather than new references.
///
/// The default and stored values participate in runtime tracing and reference
/// management. Null strings denote the empty key, while embedded NUL bytes
/// remain part of key identity. Objects are runtime-managed, and mutation is
/// guarded by the runtime GC mutator protocol but is not otherwise synchronized.

#include "rt_defaultmap.h"
#include "rt_collection_ids.h"
#include "rt_gc.h"
#include "rt_hash_util.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// Installs a non-local recovery target for runtime traps.
void rt_trap_set_recovery(jmp_buf *buf);
/// Clears the current runtime trap recovery target.
void rt_trap_clear_recovery(void);
/// Returns the current runtime trap error message.
const char *rt_trap_get_error(void);

// ---------------------------------------------------------------------------
// Internal structure
// ---------------------------------------------------------------------------

/// @brief One owned key and retained value in a bucket collision chain.
typedef struct rt_dm_entry {
    char *key;
    size_t key_len;
    void *value;
    struct rt_dm_entry *next;
} rt_dm_entry;

/// @brief Internal state of a DefaultMap runtime object.
typedef struct {
    void *vptr;
    rt_dm_entry **buckets;
    int64_t capacity;
    int64_t count;
    void *default_value;
} rt_defaultmap_impl;

/// @brief Checked cast of an opaque handle to the DefaultMap implementation.
/// @details Traps with @p what if @p obj is NULL or not a DefaultMap.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used on validation failure.
/// @return The validated implementation pointer, or NULL if a returning trap
///         handler resumes after failed validation.
static rt_defaultmap_impl *as_defaultmap(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_DEFAULTMAP_CLASS_ID, sizeof(rt_defaultmap_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_defaultmap_impl *)obj;
}

// ---------------------------------------------------------------------------
// Hash helper
// ---------------------------------------------------------------------------

/// @brief Per-process keyed hash of @p len bytes of @p key.
/// @param key Key bytes; NULL is treated as an empty buffer.
/// @param len Number of bytes to hash.
/// @return Keyed 64-bit hash value.
static uint64_t dm_hash(const char *key, size_t len) {
    return rt_keyed_hash_bytes(key ? key : "", len);
}

/// @brief Borrow the byte buffer + length of a key string (empty "" if null).
/// @param key Runtime string to inspect; NULL denotes the empty key.
/// @param what Diagnostic raised for a stale or forged nonnull handle.
/// @param out_data Receives borrowed key bytes.
/// @param out_len Receives the usable byte length.
/// @return Nonzero for a null or live string handle; otherwise zero after
///         trapping. Null continues to denote the empty key.
static int dm_key_data(rt_string key, const char *what, const char **out_data, size_t *out_len) {
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

/// @brief Drop one GC reference to a stored value and free it at zero.
/// @param value Retained runtime value, or NULL for a no-op.
static void dm_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Destroy a detached entry chain, including retained values.
/// @details Callers unlink the complete chain and publish the corresponding
///          count change before entering this callback-capable cleanup. A
///          value finalizer may therefore re-enter the map without observing
///          or freeing any node in @p entries.
/// @param entries Detached entry list to consume.
static void dm_destroy_entries(rt_dm_entry *entries) {
    while (entries) {
        rt_dm_entry *next = entries->next;
        free(entries->key);
        void *value = entries->value;
        free(entries);
        dm_release_value(value);
        entries = next;
    }
}

/// @brief Atomically detach every entry while preserving the bucket array.
/// @param m Live DefaultMap whose entries are transferred to the return value.
/// @return Singly linked list owning all formerly published entries.
static rt_dm_entry *dm_detach_entries(rt_defaultmap_impl *m) {
    rt_dm_entry *detached = NULL;
    for (int64_t i = 0; i < m->capacity; i++) {
        rt_dm_entry *entry = m->buckets[i];
        m->buckets[i] = NULL;
        while (entry) {
            rt_dm_entry *next = entry->next;
            entry->next = detached;
            detached = entry;
            entry = next;
        }
    }
    m->count = 0;
    return detached;
}

/// @brief Copies the current trap message into a stable local buffer.
/// @param buffer Destination character buffer.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Message used when the runtime has no non-empty trap text.
static void defaultmap_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Retain a value while converting a non-local retain trap into a status.
/// @param value Runtime value to retain, or NULL for no action.
/// @param error Caller-owned buffer receiving the original diagnostic.
/// @param error_size Capacity of @p error in bytes.
/// @param fallback Diagnostic used when the trap subsystem has no message.
/// @return Nonzero after a successful retain; otherwise zero after clearing
///         the helper's recovery frame. The caller still owns all cleanup.
static int defaultmap_try_retain(void *value,
                                 char *error,
                                 size_t error_size,
                                 const char *fallback) {
    if (!value)
        return 1;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        defaultmap_save_trap_error(error, error_size, fallback);
        rt_trap_clear_recovery();
        return 0;
    }

    rt_obj_retain_maybe(value);
    rt_trap_clear_recovery();
    return 1;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

/// @brief Double the bucket array and rehash all entries into it.
/// @details No-op past the INT64_MAX/2 cap; traps on alloc overflow/OOM.
/// @param m DefaultMap whose bucket table is to grow.
/// @return 1 after installing the larger table, or 0 when capacity cannot
///         double or allocation fails. The original table remains installed.
static int dm_resize(rt_defaultmap_impl *m) {
    if (m->capacity > INT64_MAX / 2) {
        rt_trap("DefaultMap: capacity overflow during resize");
        return 0;
    }
    int64_t new_cap = m->capacity * 2;
    if ((uint64_t)new_cap > SIZE_MAX / sizeof(rt_dm_entry *)) {
        rt_trap("DefaultMap: allocation size overflow");
        return 0;
    }
    rt_dm_entry **new_buckets = (rt_dm_entry **)calloc((size_t)new_cap, sizeof(rt_dm_entry *));
    if (!new_buckets) {
        rt_trap("DefaultMap: memory allocation failed");
        return 0;
    }

    for (int64_t i = 0; i < m->capacity; i++) {
        rt_dm_entry *e = m->buckets[i];
        while (e) {
            rt_dm_entry *next = e->next;
            uint64_t idx = dm_hash(e->key, e->key_len) % (uint64_t)new_cap;
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }

    free(m->buckets);
    m->buckets = new_buckets;
    m->capacity = new_cap;
    return 1;
}

/// @brief True when the load factor reaches 3/4 (count*4 >= capacity*3).
/// @param m DefaultMap whose occupancy is inspected.
/// @return Non-zero when a new-key insertion should first grow the table.
static int dm_should_resize(rt_defaultmap_impl *m) {
    return m->count >= m->capacity - m->capacity / 4;
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

/// @brief GC finalizer: free every entry (key + released value), the bucket
///        array, and release the stored default value.
/// @param obj DefaultMap object being finalized, or NULL for a no-op.
static void defaultmap_finalizer(void *obj) {
    if (!obj)
        return;
    rt_defaultmap_impl *m = as_defaultmap(obj, "DefaultMap: invalid DefaultMap object");
    if (!m)
        return;

    // Invalidate the map before any retained value can run a finalizer. A
    // callback that holds a non-owning map pointer then observes a closed,
    // empty object instead of a half-destroyed table.
    rt_dm_entry **buckets = m->buckets;
    int64_t capacity = m->capacity;
    void *default_value = m->default_value;
    m->buckets = NULL;
    m->capacity = 0;
    m->count = 0;
    m->default_value = NULL;

    if (buckets) {
        for (int64_t i = 0; i < capacity; i++)
            dm_destroy_entries(buckets[i]);
    }
    free(buckets);
    dm_release_value(default_value);
}

/// @brief GC traversal: visit the default value and every stored value so
///        the collector can trace reachable objects.
/// @param obj DefaultMap object to traverse.
/// @param visitor Runtime callback invoked for each referenced value.
/// @param ctx Opaque visitor context forwarded unchanged.
static void defaultmap_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_defaultmap_impl *m = as_defaultmap(obj, "DefaultMap: invalid DefaultMap object");
    if (!m)
        return;
    visitor(m->default_value, ctx);
    if (!m->buckets)
        return;
    for (int64_t i = 0; i < m->capacity; i++) {
        for (rt_dm_entry *e = m->buckets[i]; e; e = e->next)
            visitor(e->value, ctx);
    }
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/// @brief Creates an empty DefaultMap with a fixed fallback value.
/// @param default_value Runtime value returned for absent keys; may be NULL.
///        A non-null value is retained for the map's lifetime.
/// @return A new runtime-managed DefaultMap, or NULL after an allocation or
///         default-retention trap.
void *rt_defaultmap_new(void *default_value) {
    rt_defaultmap_impl *m =
        (rt_defaultmap_impl *)rt_obj_new_i64(RT_DEFAULTMAP_CLASS_ID, sizeof(rt_defaultmap_impl));
    if (!m) {
        rt_trap("DefaultMap: memory allocation failed");
        return NULL;
    }
    m->capacity = 16;
    m->count = 0;
    m->default_value = NULL;
    m->buckets = (rt_dm_entry **)calloc(16, sizeof(rt_dm_entry *));
    if (!m->buckets) {
        if (rt_obj_release_check0(m))
            rt_obj_free(m);
        rt_trap("DefaultMap: memory allocation failed");
        return NULL;
    }
    rt_obj_set_finalizer(m, defaultmap_finalizer);
    char retain_error[256] = {0};
    if (!defaultmap_try_retain(default_value,
                               retain_error,
                               sizeof(retain_error),
                               "DefaultMap: default retain failed")) {
        if (rt_obj_release_check0(m))
            rt_obj_free(m);
        rt_trap(retain_error);
        return NULL;
    }
    m->default_value = default_value;
    rt_gc_track(m, defaultmap_traverse);
    return m;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

/// @brief Return the number of key-value pairs in the default map.
/// @param map DefaultMap handle, or NULL to query an empty map.
/// @return Number of explicit mappings, or zero when @p map is NULL.
int64_t rt_defaultmap_len(void *map) {
    if (!map)
        return 0;
    rt_defaultmap_impl *impl = as_defaultmap(map, "DefaultMap.Len: invalid DefaultMap object");
    return impl ? impl->count : 0;
}

// ---------------------------------------------------------------------------
// Get (returns default if missing)
// ---------------------------------------------------------------------------

/// @brief Looks up an explicit mapping or returns the configured default.
/// @param map DefaultMap handle, or NULL.
/// @param key Key to find; NULL denotes the empty string.
/// @return Borrowed stored value when the key exists, borrowed default value
///         when absent, or NULL when @p map is NULL.
/// @warning The returned pointer is owned by the map and must not be released
///          as a newly acquired reference.
void *rt_defaultmap_get(void *map, rt_string key) {
    if (!map)
        return NULL;
    rt_defaultmap_impl *m = as_defaultmap(map, "DefaultMap.Get: invalid DefaultMap object");
    if (!m || !m->buckets || m->capacity <= 0)
        return NULL;

    size_t klen = 0;
    const char *kstr = NULL;
    if (!dm_key_data(key, "DefaultMap.Get: invalid key", &kstr, &klen))
        return NULL;

    uint64_t idx = dm_hash(kstr, klen) % (uint64_t)m->capacity;
    rt_dm_entry *e = m->buckets[idx];
    while (e) {
        if (e->key_len == klen && memcmp(e->key, kstr, klen) == 0)
            return e->value;
        e = e->next;
    }
    return m->default_value;
}

// ---------------------------------------------------------------------------
// Set
// ---------------------------------------------------------------------------

/// @brief Insert or update a key-value pair in the default map.
/// @details If the key already exists, the old value is released and replaced
///          with the new one. The key bytes are copied and the value is
///          retained by the map.
/// @param map DefaultMap handle, or NULL for a no-op.
/// @param key Key to copy; NULL denotes the empty string.
/// @param value Runtime value to retain, or NULL to store an explicit null.
/// @note Allocation and retention failures raise a runtime trap.
void rt_defaultmap_set(void *map, rt_string key, void *value) {
    if (!map)
        return;
    rt_gc_mutator_enter();
    rt_defaultmap_impl *m = as_defaultmap(map, "DefaultMap.Set: invalid DefaultMap object");
    if (!m) {
        rt_gc_mutator_exit();
        return;
    }
    if (!m->buckets || m->capacity <= 0 || m->count < 0) {
        rt_gc_mutator_exit();
        return;
    }

    size_t klen = 0;
    const char *kstr = NULL;
    if (!dm_key_data(key, "DefaultMap.Set: invalid key", &kstr, &klen)) {
        rt_gc_mutator_exit();
        return;
    }

    uint64_t idx = dm_hash(kstr, klen) % (uint64_t)m->capacity;
    rt_dm_entry *e = m->buckets[idx];
    while (e) {
        if (e->key_len == klen && memcmp(e->key, kstr, klen) == 0) {
            char retain_error[256] = {0};
            if (!defaultmap_try_retain(value,
                                       retain_error,
                                       sizeof(retain_error),
                                       "DefaultMap.Set: value retain failed")) {
                rt_gc_mutator_exit();
                rt_trap(retain_error);
                return;
            }
            void *old_value = e->value;
            e->value = value;
            rt_gc_mutator_exit();
            dm_release_value(old_value);
            return;
        }
        e = e->next;
    }

    if (m->count == INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("DefaultMap.Set: maximum size reached");
        return;
    }

    // Resize check
    if (dm_should_resize(m)) {
        if (!dm_resize(m)) {
            rt_gc_mutator_exit();
            return;
        }
        idx = dm_hash(kstr, klen) % (uint64_t)m->capacity;
    }

    char retain_error[256] = {0};
    if (!defaultmap_try_retain(
            value, retain_error, sizeof(retain_error), "DefaultMap.Set: value retain failed")) {
        rt_gc_mutator_exit();
        rt_trap(retain_error);
        return;
    }

    // New entry
    rt_dm_entry *ne = (rt_dm_entry *)calloc(1, sizeof(rt_dm_entry));
    if (!ne) {
        dm_release_value(value);
        rt_gc_mutator_exit();
        rt_trap("rt_defaultmap: memory allocation failed");
        return;
    }
    if (klen == SIZE_MAX) {
        dm_release_value(value);
        free(ne);
        rt_gc_mutator_exit();
        rt_trap("rt_defaultmap: key allocation overflow");
        return;
    }
    ne->key = (char *)malloc(klen + 1);
    if (!ne->key) {
        dm_release_value(value);
        free(ne);
        rt_gc_mutator_exit();
        rt_trap("rt_defaultmap: memory allocation failed");
        return;
    }
    memcpy(ne->key, kstr, klen);
    ne->key[klen] = '\0';
    ne->key_len = klen;
    ne->value = value;
    ne->next = m->buckets[idx];
    m->buckets[idx] = ne;
    m->count++;
    rt_gc_mutator_exit();
}

// ---------------------------------------------------------------------------
// Has / Remove
// ---------------------------------------------------------------------------

/// @brief Check whether a key exists in the default map.
/// @details Hashes the key with the runtime keyed hash, indexes into the bucket array, and
///          walks the separate-chaining linked list comparing by key length
///          and content. Returns 1 if found, 0 otherwise.
/// @param map DefaultMap handle, or NULL to query an empty map.
/// @param key Key to test; NULL denotes the empty string.
/// @return 1 for an explicit mapping, even when its stored value is NULL;
///         otherwise 0.
int8_t rt_defaultmap_has(void *map, rt_string key) {
    if (!map)
        return 0;
    rt_defaultmap_impl *m = as_defaultmap(map, "DefaultMap.Has: invalid DefaultMap object");
    if (!m || !m->buckets || m->capacity <= 0)
        return 0;

    size_t klen = 0;
    const char *kstr = NULL;
    if (!dm_key_data(key, "DefaultMap.Has: invalid key", &kstr, &klen))
        return 0;

    uint64_t idx = dm_hash(kstr, klen) % (uint64_t)m->capacity;
    rt_dm_entry *e = m->buckets[idx];
    while (e) {
        if (e->key_len == klen && memcmp(e->key, kstr, klen) == 0)
            return 1;
        e = e->next;
    }
    return 0;
}

/// @brief Remove a key-value pair from the default map.
/// @details Walks the bucket's chain using a pointer-to-pointer technique to
///          unlink the entry. Releases the value reference (if non-null) and
///          frees the key string and entry node.
/// @param map DefaultMap handle, or NULL for a no-op.
/// @param key Key to remove; NULL denotes the empty string.
/// @return 1 if a mapping was removed; otherwise 0.
int8_t rt_defaultmap_remove(void *map, rt_string key) {
    if (!map)
        return 0;
    rt_gc_mutator_enter();
    rt_defaultmap_impl *m = as_defaultmap(map, "DefaultMap.Remove: invalid DefaultMap object");
    if (!m) {
        rt_gc_mutator_exit();
        return 0;
    }
    if (!m->buckets || m->capacity <= 0 || m->count <= 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t klen = 0;
    const char *kstr = NULL;
    if (!dm_key_data(key, "DefaultMap.Remove: invalid key", &kstr, &klen)) {
        rt_gc_mutator_exit();
        return 0;
    }

    uint64_t idx = dm_hash(kstr, klen) % (uint64_t)m->capacity;
    rt_dm_entry **pp = &m->buckets[idx];
    while (*pp) {
        rt_dm_entry *e = *pp;
        if (e->key_len == klen && memcmp(e->key, kstr, klen) == 0) {
            *pp = e->next;
            m->count--;
            char *owned_key = e->key;
            void *owned_value = e->value;
            free(e);
            rt_gc_mutator_exit();
            free(owned_key);
            dm_release_value(owned_value);
            return 1;
        }
        pp = &e->next;
    }
    rt_gc_mutator_exit();
    return 0;
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

/// @brief Copies every explicitly stored key into a Seq snapshot.
/// @param map DefaultMap handle, or NULL to enumerate an empty map.
/// @return A new owning Seq of newly created strings in unspecified bucket
///         order.
void *rt_defaultmap_keys(void *map) {
    void *volatile seq = NULL;
    rt_string volatile key_copy = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        defaultmap_save_trap_error(
            saved_error, sizeof(saved_error), "DefaultMap.Keys: snapshot allocation failed");
        rt_trap_clear_recovery();
        if (key_copy)
            rt_str_release_maybe((rt_string)key_copy);
        dm_release_value((void *)seq);
        rt_trap(saved_error);
        return NULL;
    }

    rt_defaultmap_impl *m =
        map ? as_defaultmap(map, "DefaultMap.Keys: invalid DefaultMap object") : NULL;
    if (m && (!m->buckets || m->capacity <= 0 || m->count < 0)) {
        rt_trap("DefaultMap.Keys: finalized or corrupt DefaultMap object");
        rt_trap_clear_recovery();
        return NULL;
    }
    int64_t initial_capacity = m && m->count > 0 ? m->count : 1;
    seq = rt_seq_with_capacity_owned(initial_capacity);
    if (!seq) {
        rt_trap_clear_recovery();
        return NULL;
    }
    if (!m) {
        rt_trap_clear_recovery();
        return (void *)seq;
    }

    for (int64_t i = 0; i < m->capacity; i++) {
        rt_dm_entry *e = m->buckets[i];
        while (e) {
            key_copy = rt_string_from_bytes(e->key, e->key_len);
            if (!key_copy)
                rt_trap("DefaultMap.Keys: key allocation failed");
            int64_t old_len = rt_seq_len((void *)seq);
            rt_seq_push_raw((void *)seq, (void *)key_copy);
            if (old_len < 0 || old_len >= INT64_MAX || rt_seq_len((void *)seq) != old_len + 1)
                rt_trap("DefaultMap.Keys: snapshot append failed");
            key_copy = NULL;
            e = e->next;
        }
    }
    rt_trap_clear_recovery();
    return (void *)seq;
}

// ---------------------------------------------------------------------------
// Get default / Clear
// ---------------------------------------------------------------------------

/// @brief Returns the configured fallback value.
/// @param map DefaultMap handle, or NULL.
/// @return Borrowed default value, which may itself be NULL.
/// @warning The result remains owned by the map.
void *rt_defaultmap_get_default(void *map) {
    if (!map)
        return NULL;
    rt_defaultmap_impl *impl =
        as_defaultmap(map, "DefaultMap.GetDefault: invalid DefaultMap object");
    return impl ? impl->default_value : NULL;
}

/// @brief Remove all entries from the default map.
/// @details Releases all retained key and value references and resets the
///          entry count to zero. The backing array is retained at its current
///          capacity to avoid re-allocation on subsequent inserts.
/// @param map DefaultMap handle, or NULL for a no-op.
/// @note The configured default remains retained and unchanged.
void rt_defaultmap_clear(void *map) {
    if (!map)
        return;
    rt_gc_mutator_enter();
    rt_defaultmap_impl *m = as_defaultmap(map, "DefaultMap.Clear: invalid DefaultMap object");
    if (!m) {
        rt_gc_mutator_exit();
        return;
    }
    if (!m->buckets || m->capacity <= 0) {
        rt_gc_mutator_exit();
        return;
    }

    rt_dm_entry *detached = dm_detach_entries(m);
    rt_gc_mutator_exit();
    dm_destroy_entries(detached);
}

/// @brief Check whether the default map contains no entries.
/// @details Equivalent to checking if len == 0.
/// @param map DefaultMap handle, or NULL to test an empty map.
/// @return 1 if there are no explicit mappings; otherwise 0.
int8_t rt_defaultmap_is_empty(void *map) {
    if (!map)
        return 1;
    rt_defaultmap_impl *impl = as_defaultmap(map, "DefaultMap.IsEmpty: invalid DefaultMap object");
    return !impl || impl->count == 0 ? 1 : 0;
}
