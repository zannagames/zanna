//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_bimap.c
// Purpose: Implements a bidirectional string-to-string map (BiMap) using two
//   parallel hash tables: a forward table (key -> value) and an inverse table
//   (value -> key). Both directions support O(1) average-case lookup, insert,
//   and remove. The invariant that each key maps to exactly one value AND each
//   value maps to exactly one key is enforced at insert time.
//
// Key invariants:
//   - Forward and inverse tables each start with BM_INITIAL_CAPACITY (16)
//     buckets and use separate chaining with FNV-1a hashing.
//   - Inserting a (key, value) pair where the key already maps to a different
//     value removes the old pair first (old value loses its inverse mapping).
//     Similarly, if the new value already maps to a different key, that old
//     pair is removed. This preserves the bijection invariant.
//   - Entry nodes are shared between the forward and inverse chain lists to
//     avoid double allocation; each node stores both key and value strings.
//   - Both forward and inverse tables resize independently at 75% load factor.
//   - All operations are O(1) average case; O(n) worst case.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - BiMap objects are GC-managed (rt_obj_new_i64). All entry nodes, bucket
//     arrays, and copied key/value strings are freed by the GC finalizer.
//
// Links: src/runtime/collections/rt_bimap.h (public API),
//        src/runtime/collections/rt_hash_util.h (FNV-1a hash macro)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime's bijective string-to-string map.
///
/// Each logical mapping is represented by one forward-table entry and one
/// inverse-chain link that points back to that entry. This permits average
/// constant-time lookup in either direction while keeping the copied key and
/// value bytes in a single authoritative entry. Inserting a pair first
/// allocates its replacement representation, then removes any mapping that
/// conflicts by key or by value, preserving the one-to-one invariant.
///
/// BiMap handles are runtime-managed objects validated against
/// `RT_BIMAP_CLASS_ID`. Null handles have the operation-specific behavior
/// documented by the public API. The implementation is unsynchronized.

#include "rt_bimap.h"

#include "rt_collection_ids.h"
#include "rt_hash_table_util.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdlib.h>
#include <string.h>

/// Initial bucket count for both indexes.
#define BM_INITIAL_CAPACITY 16
#include "rt_hash_util.h"

/// @brief Authoritative forward-table node for one key/value mapping.
/// @details The node owns the separately allocated key and value buffers and
///          participates directly in exactly one forward collision chain.
typedef struct rt_bm_entry {
    /// Owned key bytes followed by a NUL terminator.
    char *key;
    /// Key length excluding the terminator.
    size_t key_len;
    /// Owned value bytes followed by a NUL terminator.
    char *value;
    /// Value length excluding the terminator.
    size_t value_len;
    /// Next entry in the forward collision chain.
    struct rt_bm_entry *next;
} rt_bm_entry;

/// @brief Internal state of a bidirectional map.
/// @details Forward buckets own entries; inverse chains contain non-owning
///          links to those entries. The two capacities may grow independently.
typedef struct rt_bimap_impl {
    /// Runtime object layout placeholder.
    void **vptr;
    /// Key-indexed bucket heads.
    rt_bm_entry **fwd_buckets; // key -> entry
    /// Number of forward buckets.
    size_t fwd_capacity;
    /// Number of inverse buckets.
    size_t inv_capacity;
    /// Number of bijective mappings.
    size_t count;

    // Separate chains for inverse lookups
    struct rt_bm_inv_link {
        /// Non-owning link to a forward entry.
        rt_bm_entry *entry;
        /// Next inverse collision-chain link.
        struct rt_bm_inv_link *next;
    } **inv_chains;
} rt_bimap_impl;

/// Inverse lookup collision-chain node.
typedef struct rt_bm_inv_link rt_bm_inv_link;

/// @brief Checked cast of an opaque handle to the BiMap implementation.
/// @details Traps with the @p what message if @p obj is NULL or not a BiMap.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used when validation fails.
/// @return The validated implementation pointer, or NULL if a returning trap
///         handler resumes after failed validation.
static rt_bimap_impl *as_bimap(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_BIMAP_CLASS_ID, sizeof(rt_bimap_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_bimap_impl *)obj;
}

/// @brief Borrows the byte buffer and length of a runtime string.
/// @param s String to inspect; NULL is treated as an empty string.
/// @param what Diagnostic raised for a stale or forged nonnull handle.
/// @param out_data Receives borrowed string bytes.
/// @param out_len Receives the number of usable bytes.
/// @return Nonzero for a null or live string handle; otherwise zero after
///         trapping. Null continues to denote the empty string.
static int get_str_data(rt_string s, const char *what, const char **out_data, size_t *out_len) {
    *out_data = "";
    *out_len = 0;
    if (!s)
        return 1;
    if (!rt_string_is_handle(s)) {
        rt_trap(what);
        return 0;
    }
    int64_t len = rt_str_len(s);
    if (len <= 0)
        return 1;
    const char *data = rt_string_cstr(s);
    if (!data) {
        rt_trap(what);
        return 0;
    }
    *out_data = data;
    *out_len = (size_t)len;
    return 1;
}

/// @brief Linear scan of a forward bucket chain for an exact key match.
/// @param head First entry in the forward collision chain.
/// @param key Key bytes to compare.
/// @param key_len Number of bytes in @p key.
/// @return The matching entry, or NULL when the chain has no equal key.
static rt_bm_entry *find_fwd(rt_bm_entry *head, const char *key, size_t key_len) {
    for (rt_bm_entry *e = head; e; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

/// @brief Linear scan of an inverse chain for the link whose entry has @p val.
/// @param head First link in the inverse collision chain.
/// @param val Value bytes to compare.
/// @param val_len Number of bytes in @p val.
/// @return The matching inverse link, or NULL when no linked entry has the
///         requested value.
static rt_bm_inv_link *find_inv(rt_bm_inv_link *head, const char *val, size_t val_len) {
    for (rt_bm_inv_link *l = head; l; l = l->next) {
        if (l->entry->value_len == val_len && memcmp(l->entry->value, val, val_len) == 0)
            return l;
    }
    return NULL;
}

/// @brief Unlink and free the inverse-chain node for value @p val (if present).
/// @param bm Map whose inverse index is searched.
/// @param val Value bytes identifying the link.
/// @param val_len Number of bytes in @p val.
/// @note The referenced forward entry is not freed.
static void remove_inv_link(rt_bimap_impl *bm, const char *val, size_t val_len) {
    uint64_t h = rt_fnv1a(val, val_len);
    size_t idx = (size_t)(h % bm->inv_capacity);
    rt_bm_inv_link **pp = &bm->inv_chains[idx];
    while (*pp) {
        if ((*pp)->entry->value_len == val_len && memcmp((*pp)->entry->value, val, val_len) == 0) {
            rt_bm_inv_link *old = *pp;
            *pp = old->next;
            free(old);
            return;
        }
        pp = &(*pp)->next;
    }
}

/// @brief Push @p link onto the inverse chain bucket for its entry's value.
/// @param bm Map that receives the already initialized link.
/// @param link Link whose `entry` field identifies the indexed value.
static void insert_inv_link(rt_bimap_impl *bm, rt_bm_inv_link *link) {
    rt_bm_entry *entry = link->entry;
    uint64_t h = rt_fnv1a(entry->value, entry->value_len);
    size_t idx = (size_t)(h % bm->inv_capacity);
    link->next = bm->inv_chains[idx];
    bm->inv_chains[idx] = link;
}

/// @brief Free a forward entry and its owned key/value buffers (NULL-safe).
/// @param entry Entry to destroy, or NULL for a no-op.
static void free_entry(rt_bm_entry *entry) {
    if (!entry)
        return;
    free(entry->key);
    free(entry->value);
    free(entry);
}

/// @brief GC finalizer: free all forward entries, inverse links, and the
///        bucket/chain arrays, then zero the struct fields.
/// @param obj BiMap object being finalized, or NULL for a no-op.
static void bimap_finalizer(void *obj) {
    if (!obj)
        return;
    rt_bimap_impl *bm = as_bimap(obj, "BiMap: invalid BiMap object");

    // Free forward entries
    if (bm->fwd_buckets) {
        for (size_t i = 0; i < bm->fwd_capacity; ++i) {
            rt_bm_entry *e = bm->fwd_buckets[i];
            while (e) {
                rt_bm_entry *next = e->next;
                free_entry(e);
                e = next;
            }
        }
    }
    free(bm->fwd_buckets);

    // Free inverse chains
    if (bm->inv_chains) {
        for (size_t i = 0; i < bm->inv_capacity; ++i) {
            rt_bm_inv_link *l = bm->inv_chains[i];
            while (l) {
                rt_bm_inv_link *next = l->next;
                free(l);
                l = next;
            }
        }
    }
    free(bm->inv_chains);
    bm->fwd_buckets = NULL;
    bm->inv_chains = NULL;
    bm->fwd_capacity = 0;
    bm->inv_capacity = 0;
    bm->count = 0;
}

/// @brief Double the forward bucket array and rehash all entries into it.
/// @details No-op past the SIZE_MAX/2 cap; traps on allocation overflow/OOM.
/// @param bm Map whose forward index is to grow.
/// @return 1 after installing the larger table, or 0 if capacity cannot double
///         or allocation fails. The original table remains installed on 0.
static int resize_fwd(rt_bimap_impl *bm) {
    if (bm->fwd_capacity > SIZE_MAX / 2) {
        rt_trap("BiMap: forward capacity overflow during resize");
        return 0;
    }
    size_t new_cap = bm->fwd_capacity * 2;
    if (new_cap > SIZE_MAX / sizeof(rt_bm_entry *)) {
        rt_trap("BiMap: allocation size overflow");
        return 0;
    }
    rt_bm_entry **new_buckets = (rt_bm_entry **)calloc(new_cap, sizeof(rt_bm_entry *));
    if (!new_buckets) {
        rt_trap("BiMap: memory allocation failed");
        return 0;
    }

    for (size_t i = 0; i < bm->fwd_capacity; ++i) {
        rt_bm_entry *e = bm->fwd_buckets[i];
        while (e) {
            rt_bm_entry *next = e->next;
            uint64_t h = rt_fnv1a(e->key, e->key_len);
            size_t idx = (size_t)(h % new_cap);
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }

    free(bm->fwd_buckets);
    bm->fwd_buckets = new_buckets;
    bm->fwd_capacity = new_cap;
    return 1;
}

/// @brief Double the inverse chain array and rehash all links into it.
/// @details No-op past the SIZE_MAX/2 cap; traps on allocation overflow/OOM.
/// @param bm Map whose inverse index is to grow.
/// @return 1 after installing the larger index, or 0 if capacity cannot double
///         or allocation fails. The original index remains installed on 0.
static int resize_inv(rt_bimap_impl *bm) {
    if (bm->inv_capacity > SIZE_MAX / 2) {
        rt_trap("BiMap: inverse capacity overflow during resize");
        return 0;
    }
    size_t new_cap = bm->inv_capacity * 2;
    if (new_cap > SIZE_MAX / sizeof(rt_bm_inv_link *)) {
        rt_trap("BiMap: allocation size overflow");
        return 0;
    }
    rt_bm_inv_link **new_chains = (rt_bm_inv_link **)calloc(new_cap, sizeof(rt_bm_inv_link *));
    if (!new_chains) {
        rt_trap("BiMap: memory allocation failed");
        return 0;
    }

    for (size_t i = 0; i < bm->inv_capacity; ++i) {
        rt_bm_inv_link *l = bm->inv_chains[i];
        while (l) {
            rt_bm_inv_link *next = l->next;
            uint64_t h = rt_fnv1a(l->entry->value, l->entry->value_len);
            size_t idx = (size_t)(h % new_cap);
            l->next = new_chains[idx];
            new_chains[idx] = l;
            l = next;
        }
    }

    free(bm->inv_chains);
    bm->inv_chains = new_chains;
    bm->inv_capacity = new_cap;
    return 1;
}

/// @brief Construct an empty bidirectional map (string ↔ string). Maintains forward and inverse
/// hash tables so both `_get_by_key` and `_get_by_value` are O(1) average. Useful for two-way
/// lookups (e.g., name ↔ id) that would otherwise need two parallel maps.
/// @return A new runtime-managed BiMap, or NULL after reporting an allocation
///         trap if construction cannot complete.
void *rt_bimap_new(void) {
    rt_bimap_impl *bm = (rt_bimap_impl *)rt_obj_new_i64(RT_BIMAP_CLASS_ID, sizeof(rt_bimap_impl));
    if (!bm) {
        rt_trap("BiMap: memory allocation failed");
        return NULL;
    }

    bm->vptr = NULL;
    bm->fwd_capacity = BM_INITIAL_CAPACITY;
    bm->inv_capacity = BM_INITIAL_CAPACITY;
    bm->count = 0;
    bm->fwd_buckets = (rt_bm_entry **)calloc(BM_INITIAL_CAPACITY, sizeof(rt_bm_entry *));
    bm->inv_chains = (rt_bm_inv_link **)calloc(BM_INITIAL_CAPACITY, sizeof(rt_bm_inv_link *));
    if (!bm->fwd_buckets || !bm->inv_chains) {
        free(bm->fwd_buckets);
        free(bm->inv_chains);
        if (rt_obj_release_check0(bm))
            rt_obj_free(bm);
        rt_trap("BiMap: memory allocation failed");
        return NULL;
    }

    rt_obj_set_finalizer(bm, bimap_finalizer);
    return bm;
}

/// @brief Return the number of entries in the bidirectional map.
/// @param obj BiMap handle, or NULL to query an empty map.
/// @return Number of mappings, or zero when @p obj is NULL.
/// @note Invalid non-null handles raise a runtime trap.
int64_t rt_bimap_len(void *obj) {
    if (!obj)
        return 0;
    rt_bimap_impl *map = as_bimap(obj, "BiMap.Len: invalid BiMap object");
    return map ? (int64_t)map->count : 0;
}

/// @brief Check whether the bidirectional map is empty.
/// @param obj BiMap handle, or NULL to test an empty map.
/// @return 1 if @p obj is NULL or contains no mappings; otherwise 0.
int8_t rt_bimap_is_empty(void *obj) {
    return rt_bimap_len(obj) == 0 ? 1 : 0;
}

/// @brief Insert a key-value pair with bidirectional lookup support.
/// @details Maintains two parallel hash tables so both key→value and
///          value→key lookups are O(1). If either the key or value already
///          exists, the conflicting entries are removed before the replacement
///          is linked. All replacement allocations complete before conflicts
///          are removed, so allocation failure preserves existing mappings.
/// @param obj BiMap handle; NULL makes the operation a no-op.
/// @param key Key to copy; NULL denotes the empty string.
/// @param value Value to copy; NULL denotes the empty string.
/// @note Invalid non-null handles and allocation failures raise a runtime trap.
void rt_bimap_put(void *obj, rt_string key, rt_string value) {
    if (!obj)
        return;
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.Put: invalid BiMap object");
    if (!bm || !bm->fwd_buckets || !bm->inv_chains || bm->fwd_capacity == 0 ||
        bm->inv_capacity == 0)
        return;

    size_t klen = 0;
    size_t vlen = 0;
    const char *kdata = NULL;
    const char *vdata = NULL;
    if (!get_str_data(key, "BiMap.Put: invalid key", &kdata, &klen) ||
        !get_str_data(value, "BiMap.Put: invalid value", &vdata, &vlen))
        return;
    if (bm->count == SIZE_MAX) {
        rt_trap("BiMap.Put: maximum size reached");
        return;
    }

    // Check load factor on forward table
    if (rt_hash_table_exceeds_load(bm->count + 1, bm->fwd_capacity) && !resize_fwd(bm))
        return;
    if (rt_hash_table_exceeds_load(bm->count + 1, bm->inv_capacity) && !resize_inv(bm))
        return;

    // Create entry
    rt_bm_entry *entry = (rt_bm_entry *)malloc(sizeof(rt_bm_entry));
    if (!entry) {
        rt_trap("BiMap: memory allocation failed");
        return;
    }
    if (klen == SIZE_MAX || vlen == SIZE_MAX) {
        free(entry);
        rt_trap("BiMap: string allocation overflow");
        return;
    }
    entry->key = (char *)malloc(klen + 1);
    entry->value = (char *)malloc(vlen + 1);
    if (!entry->key || !entry->value) {
        free(entry->key);
        free(entry->value);
        free(entry);
        rt_trap("BiMap: memory allocation failed");
        return;
    }
    memcpy(entry->key, kdata, klen);
    entry->key[klen] = '\0';
    entry->key_len = klen;
    memcpy(entry->value, vdata, vlen);
    entry->value[vlen] = '\0';
    entry->value_len = vlen;

    rt_bm_inv_link *inv_link = (rt_bm_inv_link *)malloc(sizeof(rt_bm_inv_link));
    if (!inv_link) {
        free_entry(entry);
        rt_trap("BiMap: memory allocation failed");
        return;
    }
    inv_link->entry = entry;
    inv_link->next = NULL;

    // All allocations for the replacement entry have succeeded. It is now safe
    // to remove any conflicting key or value mappings before committing.
    rt_bimap_remove_by_key(obj, key);
    rt_bimap_remove_by_value(obj, value);

    // Insert into forward table
    uint64_t fh = rt_fnv1a(kdata, klen);
    size_t fidx = (size_t)(fh % bm->fwd_capacity);
    entry->next = bm->fwd_buckets[fidx];
    bm->fwd_buckets[fidx] = entry;

    // Insert into inverse chain
    insert_inv_link(bm, inv_link);

    bm->count++;
}

/// @brief Look up a value by its associated key.
/// @details Returns an owned empty string if the key is not present.
/// @param obj BiMap handle, or NULL to query an empty map.
/// @param key Key to find; NULL denotes the empty string.
/// @return A newly created runtime string containing the mapped value, or an
///         owned empty string when the key is absent or @p obj is NULL.
/// @note Invalid non-null handles raise a runtime trap.
rt_string rt_bimap_get_by_key(void *obj, rt_string key) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.GetByKey: invalid BiMap object");
    if (!bm || !bm->fwd_buckets || bm->fwd_capacity == 0)
        return NULL;

    size_t klen = 0;
    const char *kdata = NULL;
    if (!get_str_data(key, "BiMap.GetByKey: invalid key", &kdata, &klen))
        return NULL;

    uint64_t h = rt_fnv1a(kdata, klen);
    size_t idx = (size_t)(h % bm->fwd_capacity);
    rt_bm_entry *e = find_fwd(bm->fwd_buckets[idx], kdata, klen);
    if (!e)
        return rt_string_from_bytes("", 0);

    return rt_string_from_bytes(e->value, e->value_len);
}

/// @brief Look up a key by its associated value (reverse lookup).
/// @details Returns an owned empty string if the value is not present in the reverse index.
/// @param obj BiMap handle, or NULL to query an empty map.
/// @param value Value to find; NULL denotes the empty string.
/// @return A newly created runtime string containing the mapped key, or an
///         owned empty string when the value is absent or @p obj is NULL.
/// @note Invalid non-null handles raise a runtime trap.
rt_string rt_bimap_get_by_value(void *obj, rt_string value) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.GetByValue: invalid BiMap object");
    if (!bm || !bm->inv_chains || bm->inv_capacity == 0)
        return NULL;

    size_t vlen = 0;
    const char *vdata = NULL;
    if (!get_str_data(value, "BiMap.GetByValue: invalid value", &vdata, &vlen))
        return NULL;

    uint64_t h = rt_fnv1a(vdata, vlen);
    size_t idx = (size_t)(h % bm->inv_capacity);
    rt_bm_inv_link *l = find_inv(bm->inv_chains[idx], vdata, vlen);
    if (!l)
        return rt_string_from_bytes("", 0);

    return rt_string_from_bytes(l->entry->key, l->entry->key_len);
}

/// @brief Check whether a key exists in the bidirectional map.
/// @param obj BiMap handle, or NULL to query an empty map.
/// @param key Key to find; NULL denotes the empty string.
/// @return 1 if the key is present; otherwise 0.
/// @note Invalid non-null handles raise a runtime trap.
int8_t rt_bimap_has_key(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.HasKey: invalid BiMap object");
    if (!bm || !bm->fwd_buckets || bm->fwd_capacity == 0)
        return 0;

    size_t klen = 0;
    const char *kdata = NULL;
    if (!get_str_data(key, "BiMap.HasKey: invalid key", &kdata, &klen))
        return 0;

    uint64_t h = rt_fnv1a(kdata, klen);
    size_t idx = (size_t)(h % bm->fwd_capacity);
    return find_fwd(bm->fwd_buckets[idx], kdata, klen) ? 1 : 0;
}

/// @brief Check whether a value exists in the reverse index.
/// @param obj BiMap handle, or NULL to query an empty map.
/// @param value Value to find; NULL denotes the empty string.
/// @return 1 if the value is present; otherwise 0.
/// @note Invalid non-null handles raise a runtime trap.
int8_t rt_bimap_has_value(void *obj, rt_string value) {
    if (!obj)
        return 0;
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.HasValue: invalid BiMap object");
    if (!bm || !bm->inv_chains || bm->inv_capacity == 0)
        return 0;

    size_t vlen = 0;
    const char *vdata = NULL;
    if (!get_str_data(value, "BiMap.HasValue: invalid value", &vdata, &vlen))
        return 0;

    uint64_t h = rt_fnv1a(vdata, vlen);
    size_t idx = (size_t)(h % bm->inv_capacity);
    return find_inv(bm->inv_chains[idx], vdata, vlen) ? 1 : 0;
}

/// @brief Remove an entry by its key, also removing the reverse mapping.
/// @details Both owned byte buffers and the two index nodes are released.
/// @param obj BiMap handle, or NULL for a no-op.
/// @param key Key identifying the mapping; NULL denotes the empty string.
/// @return 1 if a mapping was removed; otherwise 0.
/// @note Invalid non-null handles raise a runtime trap.
int8_t rt_bimap_remove_by_key(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.RemoveByKey: invalid BiMap object");
    if (!bm || !bm->fwd_buckets || !bm->inv_chains || bm->fwd_capacity == 0 ||
        bm->inv_capacity == 0)
        return 0;

    size_t klen = 0;
    const char *kdata = NULL;
    if (!get_str_data(key, "BiMap.RemoveByKey: invalid key", &kdata, &klen))
        return 0;

    uint64_t h = rt_fnv1a(kdata, klen);
    size_t idx = (size_t)(h % bm->fwd_capacity);

    rt_bm_entry **pp = &bm->fwd_buckets[idx];
    while (*pp) {
        rt_bm_entry *e = *pp;
        if (e->key_len == klen && memcmp(e->key, kdata, klen) == 0) {
            // Remove from forward chain
            *pp = e->next;
            // Remove from inverse chain
            remove_inv_link(bm, e->value, e->value_len);
            free_entry(e);
            bm->count--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

/// @brief Remove an entry by its value, also removing the forward mapping.
/// @details Both owned byte buffers and the two index nodes are released.
/// @param obj BiMap handle, or NULL for a no-op.
/// @param value Value identifying the mapping; NULL denotes the empty string.
/// @return 1 if a mapping was removed; otherwise 0.
/// @note Invalid non-null handles raise a runtime trap.
int8_t rt_bimap_remove_by_value(void *obj, rt_string value) {
    if (!obj)
        return 0;
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.RemoveByValue: invalid BiMap object");
    if (!bm || !bm->fwd_buckets || !bm->inv_chains || bm->fwd_capacity == 0 ||
        bm->inv_capacity == 0)
        return 0;

    size_t vlen = 0;
    const char *vdata = NULL;
    if (!get_str_data(value, "BiMap.RemoveByValue: invalid value", &vdata, &vlen))
        return 0;

    // Find entry via inverse lookup
    uint64_t vh = rt_fnv1a(vdata, vlen);
    size_t vidx = (size_t)(vh % bm->inv_capacity);
    rt_bm_inv_link *l = find_inv(bm->inv_chains[vidx], vdata, vlen);
    if (!l)
        return 0;

    rt_bm_entry *entry = l->entry;

    // Remove from forward chain
    uint64_t fh = rt_fnv1a(entry->key, entry->key_len);
    size_t fidx = (size_t)(fh % bm->fwd_capacity);
    rt_bm_entry **pp = &bm->fwd_buckets[fidx];
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }

    // Remove from inverse chain
    remove_inv_link(bm, entry->value, entry->value_len);
    free_entry(entry);
    bm->count--;
    return 1;
}

/// @brief Return a Seq of every key (forward-table iteration order). Snapshot of current state.
/// @param obj BiMap handle, or NULL to enumerate an empty map.
/// @return A new owning Seq containing newly created copies of all keys.
/// @note Order reflects the forward hash table and is unspecified.
/// @note Invalid non-null handles raise a runtime trap.
void *rt_bimap_keys(void *obj) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.Keys: invalid BiMap object");
    if (!bm)
        return seq;

    for (size_t i = 0; i < bm->fwd_capacity; ++i) {
        for (rt_bm_entry *e = bm->fwd_buckets[i]; e; e = e->next) {
            rt_string k = rt_string_from_bytes(e->key, e->key_len);
            rt_seq_push(seq, (void *)k);
            rt_str_release_maybe(k);
        }
    }
    return seq;
}

/// @brief Return a Seq of every value as a snapshot of current state.
/// @param obj BiMap handle, or NULL to enumerate an empty map.
/// @return A new owning Seq containing newly created copies of all values.
/// @note Order reflects the forward hash table and is unspecified.
/// @note Invalid non-null handles raise a runtime trap.
void *rt_bimap_values(void *obj) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.Values: invalid BiMap object");
    if (!bm)
        return seq;

    for (size_t i = 0; i < bm->fwd_capacity; ++i) {
        for (rt_bm_entry *e = bm->fwd_buckets[i]; e; e = e->next) {
            rt_string v = rt_string_from_bytes(e->value, e->value_len);
            rt_seq_push(seq, (void *)v);
            rt_str_release_maybe(v);
        }
    }
    return seq;
}

/// @brief Remove all entries from both forward and reverse tables.
/// @details Releases all owned strings and index nodes while retaining both
///          bucket arrays and their current capacities for reuse.
/// @param obj BiMap handle, or NULL for a no-op.
/// @note Invalid non-null handles raise a runtime trap.
void rt_bimap_clear(void *obj) {
    if (!obj)
        return;
    rt_bimap_impl *bm = as_bimap(obj, "BiMap.Clear: invalid BiMap object");
    if (!bm || !bm->fwd_buckets || !bm->inv_chains)
        return;

    for (size_t i = 0; i < bm->fwd_capacity; ++i) {
        rt_bm_entry *e = bm->fwd_buckets[i];
        while (e) {
            rt_bm_entry *next = e->next;
            free_entry(e);
            e = next;
        }
        bm->fwd_buckets[i] = NULL;
    }

    for (size_t i = 0; i < bm->inv_capacity; ++i) {
        rt_bm_inv_link *l = bm->inv_chains[i];
        while (l) {
            rt_bm_inv_link *next = l->next;
            free(l);
            l = next;
        }
        bm->inv_chains[i] = NULL;
    }

    bm->count = 0;
}
