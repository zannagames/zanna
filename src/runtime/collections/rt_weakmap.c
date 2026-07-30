//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_weakmap.c
/// @file
/// @brief Implements the GC-managed string-keyed WeakMap collection.
///
// Purpose: Implements a string-keyed map that holds zeroing weak references
//   to its values. Keys are retained rt_string handles; values are tracked by
//   rt_weakref so freeing a target automatically makes subsequent lookups
//   return NULL.
//
// Key invariants:
//   - Open-addressing hash table with initial capacity WM_INITIAL_CAP (16).
//   - The runtime keyed hash covers raw key bytes, so embedded NUL bytes are part of keys.
//   - Load factor is kept below roughly 70% by doubling and rehashing.
//   - Values are stored as rt_weakref handles and are never strongly retained.
//     Runtime-managed objects and strings are zeroed on final release.
//   - `count` tracks occupied slots; public Len counts only live weak targets.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - WeakMap objects are GC-managed (rt_obj_new_i64).
//   - Entry keys are retained and released with the entry.
//   - Weak reference handles are owned by the map and freed on removal/clear.
//   - Get promotes a live weak target and returns an owning reference.
//     Keys() returns an owning Seq that retains the live entries' immutable
//     strings independently of subsequent map mutation.
//
// Links: src/runtime/collections/rt_weakmap.h (public API),
//        src/runtime/core/rt_gc.h (zeroing weak references)
//
//===----------------------------------------------------------------------===//

#include "rt_weakmap.h"

#include "rt_collection_ids.h"
#include "rt_gc.h"
#include "rt_hash_util.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Initial open-addressing table capacity.
#define WM_INITIAL_CAP 16
/// @brief Probe result used when a valid table has no free slot.
#define WM_SLOT_FULL INT64_C(-1)
/// @brief Probe result used after a validation/invariant trap.
#define WM_SLOT_ERROR INT64_C(-2)

/// @brief Installs @p buf as the current thread's non-local trap recovery target.
/// @param buf Jump buffer that receives control when a runtime trap is raised.
void rt_trap_set_recovery(jmp_buf *buf);

/// @brief Removes the current thread's trap recovery target.
void rt_trap_clear_recovery(void);

/// @brief Returns the diagnostic text associated with the latest runtime trap.
/// @return Borrowed null-terminated diagnostic text, if available.
const char *rt_trap_get_error(void);

/// @brief One open-addressing slot containing an owned key and weakref handle.
typedef struct {
    rt_string key;
    rt_weakref *value_ref;
    int8_t occupied;
} wm_entry;

/// @brief Runtime object payload for the WeakMap hash table.
///
/// `count` includes occupied entries whose weak target has already died;
/// public size queries scan the table and report only live targets.
typedef struct {
    wm_entry *entries;
    int64_t capacity;
    int64_t count;
} rt_weakmap_data;

/// @brief Checked cast of an opaque handle to the WeakMap implementation;
///        traps with @p what if @p obj is NULL or not a WeakMap.
/// @param obj Opaque runtime object to validate.
/// @param what Diagnostic raised when validation fails.
/// @return Validated map payload, or `NULL` after raising a trap.
static rt_weakmap_data *as_weakmap(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_WEAKMAP_CLASS_ID, sizeof(rt_weakmap_data))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_weakmap_data *)obj;
}

/// @brief Validate a key and borrow its byte buffer and length.
/// @param key Key to inspect; null continues to denote the empty key.
/// @param what Diagnostic raised for a forged or stale nonnull handle.
/// @param out_data Receives borrowed key bytes.
/// @param out_len Receives the byte count of the returned data.
/// @return Nonzero for null or a live string handle; otherwise zero after
///         trapping.
static int wm_key_data(rt_string key, const char *what, const char **out_data, size_t *out_len) {
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
    const char *data = rt_string_cstr(key);
    if (!data) {
        rt_trap(what);
        return 0;
    }
    *out_data = data;
    *out_len = (size_t)len;
    return 1;
}

/// @brief Per-process keyed hash of @p len bytes of @p data.
/// @param data Byte buffer to hash; null is accepted for a zero-length key.
/// @param len Number of significant bytes, including embedded null bytes.
/// @return Keyed 64-bit hash value.
static uint64_t wm_hash_bytes(const char *data, size_t len) {
    return rt_keyed_hash_bytes(data ? data : "", len);
}

/// @brief True iff an occupied @p entry's key byte-matches @p key/@p key_len.
/// @param entry Slot whose retained string key is compared.
/// @param key Query-key bytes.
/// @param key_len Query-key length in bytes.
/// @return One for an occupied exact byte match, zero for a mismatch/empty
///         slot, or negative after a retained-key validation trap.
static int wm_entry_matches(const wm_entry *entry, const char *key, size_t key_len) {
    if (!entry->occupied)
        return 0;
    const char *entry_key = "";
    size_t entry_len = 0;
    if (!wm_key_data(entry->key, "WeakMap: invalid retained key", &entry_key, &entry_len))
        return -1;
    return entry_len == key_len && memcmp(entry_key, key, key_len) == 0 ? 1 : 0;
}

/// @brief True iff @p entry is occupied AND its weak value reference is still
///        alive (the referent has not been collected).
/// @param entry Slot to inspect.
/// @return One for a live referent, zero for an empty/dead slot, or negative
///         after a retained-weakref validation trap.
static int wm_entry_alive(const wm_entry *entry) {
    if (!entry->occupied)
        return 0;
    if (!entry->value_ref || !rt_weakref_is_handle(entry->value_ref)) {
        rt_trap("WeakMap: invalid retained weak reference");
        return -1;
    }
    return rt_weakref_alive(entry->value_ref) ? 1 : 0;
}

/// @brief Release an occupied entry: drop the key string + free the weakref,
///        then mark the slot empty.
/// @param entry Slot to clear; null and unoccupied slots are ignored.
static void wm_release_entry(wm_entry *entry) {
    if (!entry || !entry->occupied)
        return;
    rt_str_release_maybe(entry->key);
    rt_weakref_free(entry->value_ref);
    entry->key = NULL;
    entry->value_ref = NULL;
    entry->occupied = 0;
}

/// @brief Allocate a zeroed entry table of @p capacity slots; traps on
///        overflow/OOM.
/// @param capacity Positive number of slots to allocate.
/// @return New zero-initialized table, or `NULL` after raising a trap.
static wm_entry *wm_alloc_entries(int64_t capacity) {
    if (capacity <= 0 || (uint64_t)capacity > SIZE_MAX / sizeof(wm_entry)) {
        rt_trap("WeakMap: allocation size overflow");
        return NULL;
    }
    wm_entry *entries = (wm_entry *)calloc((size_t)capacity, sizeof(wm_entry));
    if (!entries) {
        rt_trap("WeakMap: memory allocation failed");
        return NULL;
    }
    return entries;
}

/// @brief True when the occupied load has reached seven tenths.
/// @details Computes `count * 10 >= capacity * 7` without overflowing either
///          signed or unsigned integer arithmetic.
/// @param count Current number of occupied slots.
/// @param capacity Current positive table capacity.
/// @return Nonzero when a new insertion should grow the table.
static int wm_should_grow(int64_t count, int64_t capacity) {
    if (count < 0 || capacity <= 0)
        return 1;
    uint64_t cap = (uint64_t)capacity;
    uint64_t threshold = (cap / UINT64_C(10)) * UINT64_C(7) +
                         (((cap % UINT64_C(10)) * UINT64_C(7)) + UINT64_C(9)) / UINT64_C(10);
    return (uint64_t)count >= threshold;
}

/// @brief Linear-probe for @p key: returns the matching slot or the first
///        free slot.
/// @param data Initialized map payload with positive capacity.
/// @param key Query-key bytes.
/// @param key_len Query-key length in bytes.
/// @return Matching/free slot, `WM_SLOT_FULL`, or `WM_SLOT_ERROR` after a
///         validation/invariant trap.
static int64_t wm_find_slot(rt_weakmap_data *data, const char *key, size_t key_len) {
    if (!data || !data->entries || data->capacity <= 0) {
        rt_trap("WeakMap: invalid hash table");
        return WM_SLOT_ERROR;
    }
    uint64_t h = wm_hash_bytes(key, key_len);
    uint64_t capacity = (uint64_t)data->capacity;
    uint64_t mask = capacity - UINT64_C(1);
    if ((capacity & mask) != 0) {
        rt_trap("WeakMap: invalid hash table capacity");
        return WM_SLOT_ERROR;
    }
    uint64_t idx = h & mask;
    for (int64_t i = 0; i < data->capacity; i++) {
        int64_t slot = (int64_t)((idx + (uint64_t)i) & mask);
        if (!data->entries[slot].occupied)
            return slot;
        int matches = wm_entry_matches(&data->entries[slot], key, key_len);
        if (matches < 0)
            return WM_SLOT_ERROR;
        if (matches)
            return slot;
    }
    return WM_SLOT_FULL;
}

/// @brief Re-inserts an occupied entry while rebuilding a probe cluster/table.
///
/// Ownership of the entry's key and weakref handle moves as-is; neither is
/// retained or copied. Growth and compaction call this only for live entries,
/// while removal may move a dead occupied entry to preserve probe reachability.
///
/// @param data Destination table.
/// @param entry Occupied entry whose resources transfer into @p data.
/// @return Nonzero after copying the entry into the destination, otherwise
///         zero after trapping.
static int wm_move_entry(rt_weakmap_data *data, wm_entry entry) {
    const char *key = "";
    size_t key_len = 0;
    if (!wm_key_data(entry.key, "WeakMap: invalid retained key", &key, &key_len))
        return 0;
    int64_t slot = wm_find_slot(data, key, key_len);
    if (slot == WM_SLOT_ERROR)
        return 0;
    if (slot == WM_SLOT_FULL) {
        rt_trap("WeakMap: rehash failed");
        return 0;
    }
    data->entries[slot] = entry;
    data->count++;
    return 1;
}

/// @brief Double the table; live entries are carried over and dead (collected)
///        weak entries are dropped during the rehash. Traps on overflow/OOM.
/// @param data Map payload whose table is replaced on success.
/// @return Nonzero after successful growth; zero after an overflow/allocation trap.
static int wm_grow(rt_weakmap_data *data) {
    int64_t old_cap = data->capacity;
    wm_entry *old_entries = data->entries;

    if (old_cap > INT64_MAX / 2) {
        rt_trap("WeakMap: capacity overflow");
        return 0;
    }
    int64_t new_cap = old_cap * 2;
    wm_entry *new_entries = wm_alloc_entries(new_cap);
    if (!new_entries)
        return 0;

    rt_weakmap_data replacement = {
        .entries = new_entries,
        .capacity = new_cap,
        .count = 0,
    };

    for (int64_t i = 0; i < old_cap; i++) {
        if (!old_entries[i].occupied)
            continue;
        int alive = wm_entry_alive(&old_entries[i]);
        if (alive < 0) {
            for (int64_t j = 0; j < i; j++) {
                if (old_entries[j].occupied == 2)
                    old_entries[j].occupied = 1;
            }
            free(new_entries);
            return 0;
        }
        if (alive) {
            if (!wm_move_entry(&replacement, old_entries[i])) {
                for (int64_t j = 0; j < i; j++) {
                    if (old_entries[j].occupied == 2)
                        old_entries[j].occupied = 1;
                }
                free(new_entries);
                return 0;
            }
            old_entries[i].occupied = 2;
        }
    }

    *data = replacement;
    for (int64_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied == 1)
            wm_release_entry(&old_entries[i]);
    }
    free(old_entries);
    return 1;
}

/// @brief Release one owning runtime object reference.
/// @param obj Nullable runtime-managed object.
static void wm_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Save the active runtime diagnostic into bounded local storage.
/// @param output Destination character buffer.
/// @param capacity Size of @p output in bytes.
/// @param fallback Text used when the trap subsystem has no diagnostic.
static void wm_save_trap(char *output, size_t capacity, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(output, capacity, "%s", error && error[0] ? error : fallback);
}

/// @brief Append a retained key to an owning snapshot and clean up on failure.
/// @details A sequence retain/growth trap consumes the partial snapshot before
///          re-raising the original diagnostic.
/// @param seq Owning result sequence, consumed on failure.
/// @param key Borrowed live string key to append.
/// @return Nonzero on success, otherwise zero after cleanup and retrapping.
static int wm_push_key_or_release_seq(void *seq, rt_string key) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        wm_save_trap(saved_error, sizeof(saved_error), "WeakMap.Keys: snapshot append failed");
        rt_trap_clear_recovery();
        wm_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    rt_seq_push(seq, key);
    rt_trap_clear_recovery();
    return 1;
}

/// @brief GC finalizer: release every occupied entry (key + weakref) and
///        free the entry table.
/// @param obj WeakMap instance being finalized; null is ignored.
static void weakmap_finalizer(void *obj) {
    if (!obj)
        return;
    rt_weakmap_data *data = as_weakmap(obj, "WeakMap: invalid WeakMap object");
    if (!data || !data->entries)
        return;
    for (int64_t i = 0; i < data->capacity; i++)
        wm_release_entry(&data->entries[i]);
    free(data->entries);
    data->entries = NULL;
    data->capacity = 0;
    data->count = 0;
}

/// @brief Creates an empty WeakMap with the initial hash-table capacity.
/// @return New GC-managed WeakMap, or `NULL` after an allocation trap.
/// @note The map owns only keys and weakref handles; it never retains values.
void *rt_weakmap_new(void) {
    void *volatile obj = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        wm_save_trap(saved_error, sizeof(saved_error), "WeakMap: memory allocation failed");
        rt_trap_clear_recovery();
        wm_release_object((void *)obj);
        rt_trap(saved_error);
        return NULL;
    }

    obj = rt_obj_new_i64(RT_WEAKMAP_CLASS_ID, sizeof(rt_weakmap_data));
    if (!obj) {
        rt_trap_clear_recovery();
        rt_trap("WeakMap: memory allocation failed");
        return NULL;
    }
    rt_weakmap_data *data = (rt_weakmap_data *)(void *)obj;
    data->entries = wm_alloc_entries(WM_INITIAL_CAP);
    if (!data->entries) {
        rt_trap_clear_recovery();
        wm_release_object((void *)obj);
        return NULL;
    }
    data->capacity = WM_INITIAL_CAP;
    data->count = 0;
    rt_obj_set_finalizer((void *)obj, weakmap_finalizer);
    rt_trap_clear_recovery();
    return (void *)obj;
}

/// @brief Counts entries whose weak referents are currently live.
/// @param map WeakMap to scan; null returns zero.
/// @return Number of live entries, excluding occupied slots with dead targets.
/// @note Runs in O(capacity) and does not physically remove dead entries.
int64_t rt_weakmap_len(void *map) {
    if (!map)
        return 0;
    rt_weakmap_data *data = as_weakmap(map, "WeakMap.Len: invalid WeakMap object");
    if (!data || !data->entries)
        return 0;
    int64_t live = 0;
    for (int64_t i = 0; i < data->capacity; i++) {
        int alive = wm_entry_alive(&data->entries[i]);
        if (alive < 0)
            return 0;
        if (alive)
            live++;
    }
    return live;
}

/// @brief Tests whether the map currently exposes any live weak value.
/// @param map WeakMap to scan; null is considered empty.
/// @return One when the live-entry count is zero, otherwise zero.
/// @note Stops at the first live entry; worst-case O(capacity).
int8_t rt_weakmap_is_empty(void *map) {
    if (!map)
        return 1;
    rt_weakmap_data *data = as_weakmap(map, "WeakMap.IsEmpty: invalid WeakMap object");
    if (!data || !data->entries)
        return 1;
    for (int64_t i = 0; i < data->capacity; i++) {
        int alive = wm_entry_alive(&data->entries[i]);
        if (alive < 0)
            return 1;
        if (alive)
            return 0;
    }
    return 1;
}

/// @brief Inserts or replaces a key's zeroing weak value reference.
///
/// Null keys normalize to the retained runtime empty string. Replacing an
/// occupied key swaps weakref handles without changing the retained key.
/// At approximately 70% occupied load, the table doubles and drops dead entries.
///
/// @param map WeakMap to mutate; null is ignored.
/// @param key Length-aware string key; null denotes the empty key.
/// @param value Nullable runtime-managed weak target; never strongly retained.
/// @note Weakref/key setup is trap-protected so a failed new insertion releases
///       any resources acquired before re-raising the diagnostic.
void rt_weakmap_set(void *map, rt_string key, void *value) {
    if (!map)
        return;
    rt_weakmap_data *data = as_weakmap(map, "WeakMap.Set: invalid WeakMap object");
    if (!data || !data->entries)
        return;

    const char *key_data = "";
    size_t key_len = 0;
    if (!wm_key_data(key, "WeakMap.Set: invalid key", &key_data, &key_len))
        return;

    int64_t slot = wm_find_slot(data, key_data, key_len);
    if (slot == WM_SLOT_ERROR)
        return;
    if (slot >= 0 && data->entries[slot].occupied) {
        if (!data->entries[slot].value_ref ||
            !rt_weakref_is_handle(data->entries[slot].value_ref)) {
            rt_trap("WeakMap.Set: invalid retained weak reference");
            return;
        }
        rt_weakref *new_ref = rt_weakref_new(value);
        if (!new_ref)
            return;
        rt_weakref *old_ref = data->entries[slot].value_ref;
        data->entries[slot].value_ref = new_ref;
        rt_weakref_free(old_ref);
        return;
    }

    if (slot == WM_SLOT_FULL || wm_should_grow(data->count, data->capacity)) {
        if (!wm_grow(data))
            return;
        slot = wm_find_slot(data, key_data, key_len);
    }
    if (slot == WM_SLOT_ERROR)
        return;
    if (slot == WM_SLOT_FULL) {
        rt_trap("WeakMap: insertion failed");
        return;
    }

    rt_string stored_key = key ? key : rt_str_empty();
    if (!stored_key)
        return;
    rt_weakref *volatile new_ref = NULL;
    volatile int key_retained = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "WeakMap: insertion failed");
        rt_trap_clear_recovery();
        if (key_retained)
            rt_str_release_maybe(stored_key);
        rt_weakref_free((rt_weakref *)new_ref);
        rt_trap(saved_error);
        return;
    }
    new_ref = rt_weakref_new(value);
    if (!new_ref) {
        rt_trap_clear_recovery();
        return;
    }
    rt_obj_retain_maybe(stored_key);
    key_retained = stored_key ? 1 : 0;
    rt_trap_clear_recovery();

    data->entries[slot].key = stored_key;
    data->entries[slot].value_ref = (rt_weakref *)new_ref;
    data->entries[slot].occupied = 1;
    data->count++;
}

/// @brief Looks up the current referent for a string key.
/// @param map WeakMap to query; null returns `NULL`.
/// @param key Length-aware string key; null denotes the empty key.
/// @return Retained live target, or `NULL` if the key is absent or collected.
/// @note The caller owns a nonnull result and must release it.
void *rt_weakmap_get(void *map, rt_string key) {
    if (!map)
        return NULL;
    rt_weakmap_data *data = as_weakmap(map, "WeakMap.Get: invalid WeakMap object");
    if (!data || !data->entries)
        return NULL;
    const char *key_data = "";
    size_t key_len = 0;
    if (!wm_key_data(key, "WeakMap.Get: invalid key", &key_data, &key_len))
        return NULL;
    int64_t slot = wm_find_slot(data, key_data, key_len);
    if (slot < 0 || !data->entries[slot].occupied)
        return NULL;
    return rt_weakref_get(data->entries[slot].value_ref);
}

/// @brief Tests whether a key maps to a currently live referent.
/// @param map WeakMap to query; null returns false.
/// @param key Length-aware string key; null denotes the empty key.
/// @return One for an occupied matching slot with a live target, otherwise zero.
int8_t rt_weakmap_has(void *map, rt_string key) {
    if (!map)
        return 0;
    rt_weakmap_data *data = as_weakmap(map, "WeakMap.Has: invalid WeakMap object");
    if (!data || !data->entries)
        return 0;
    const char *key_data = "";
    size_t key_len = 0;
    if (!wm_key_data(key, "WeakMap.Has: invalid key", &key_data, &key_len))
        return 0;
    int64_t slot = wm_find_slot(data, key_data, key_len);
    if (slot < 0)
        return 0;
    int alive = wm_entry_alive(&data->entries[slot]);
    return alive > 0 ? 1 : 0;
}

/// @brief Removes an occupied key and repairs its linear-probe cluster.
///
/// The retained key and weakref handle are released. Subsequent occupied slots
/// are removed and reinserted so lookups are not cut off by the new empty slot.
///
/// @param map WeakMap to mutate; null returns false.
/// @param key Length-aware string key; null denotes the empty key.
/// @return One if an occupied entry was removed, even if its target was already
///         dead; zero if the key was absent.
int8_t rt_weakmap_remove(void *map, rt_string key) {
    if (!map)
        return 0;
    rt_weakmap_data *data = as_weakmap(map, "WeakMap.Remove: invalid WeakMap object");
    if (!data || !data->entries)
        return 0;
    const char *key_data = "";
    size_t key_len = 0;
    if (!wm_key_data(key, "WeakMap.Remove: invalid key", &key_data, &key_len))
        return 0;
    int64_t slot = wm_find_slot(data, key_data, key_len);
    if (slot < 0 || !data->entries[slot].occupied)
        return 0;

    wm_release_entry(&data->entries[slot]);
    data->count--;

    int64_t next = (slot + 1) % data->capacity;
    while (data->entries[next].occupied) {
        wm_entry tmp = data->entries[next];
        data->entries[next].key = NULL;
        data->entries[next].value_ref = NULL;
        data->entries[next].occupied = 0;
        data->count--;
        if (!wm_move_entry(data, tmp)) {
            data->entries[next] = tmp;
            data->count++;
            return 1;
        }
        next = (next + 1) % data->capacity;
    }

    return 1;
}

/// @brief Creates a snapshot of keys whose weak targets are currently live.
/// @param map WeakMap to scan; null produces an empty sequence.
/// @return New owning Seq retaining each live entry's string key.
/// @note Iteration follows hash-table slot order and is not sorted.
void *rt_weakmap_keys(void *map) {
    if (!map)
        return rt_seq_with_capacity_owned(1);
    rt_weakmap_data *data = as_weakmap(map, "WeakMap.Keys: invalid WeakMap object");
    if (!data)
        return NULL;
    if (!data->entries || data->capacity <= 0 || data->count < 0 || data->count > data->capacity) {
        rt_trap("WeakMap.Keys: corrupt WeakMap object");
        return NULL;
    }

    int64_t live = 0;
    for (int64_t i = 0; i < data->capacity; i++) {
        int alive = wm_entry_alive(&data->entries[i]);
        if (alive < 0)
            return NULL;
        if (alive)
            live++;
    }

    void *seq = rt_seq_with_capacity_owned(live > 0 ? live : 1);
    if (!seq)
        return NULL;
    for (int64_t i = 0; i < data->capacity; i++) {
        int alive = wm_entry_alive(&data->entries[i]);
        if (alive < 0) {
            wm_release_object(seq);
            return NULL;
        }
        if (alive && !wm_push_key_or_release_seq(seq, data->entries[i].key))
            return NULL;
    }
    return seq;
}

/// @brief Removes every occupied slot while preserving table capacity.
/// @param map WeakMap to clear; null is ignored.
/// @note Releases all retained keys and weakref handles but never releases
///       target objects, which were not strongly owned.
void rt_weakmap_clear(void *map) {
    if (!map)
        return;
    rt_weakmap_data *data = as_weakmap(map, "WeakMap.Clear: invalid WeakMap object");
    if (!data || !data->entries)
        return;
    for (int64_t i = 0; i < data->capacity; i++)
        wm_release_entry(&data->entries[i]);
    data->count = 0;
}

/// @brief Rebuilds the table without entries whose weak targets have died.
/// @param map WeakMap to compact; null returns zero.
/// @return Number of entries actually omitted by the authoritative rebuild.
/// @note Live key/weakref ownership moves without extra retains. Capacity is
///       unchanged, and no rebuild occurs when there are no dead entries.
int64_t rt_weakmap_compact(void *map) {
    if (!map)
        return 0;
    rt_weakmap_data *data = as_weakmap(map, "WeakMap.Compact: invalid WeakMap object");
    if (!data || !data->entries)
        return 0;

    int64_t removed = 0;
    for (int64_t i = 0; i < data->capacity; i++) {
        int alive = wm_entry_alive(&data->entries[i]);
        if (alive < 0)
            return 0;
        if (data->entries[i].occupied && !alive)
            removed++;
    }
    if (removed == 0)
        return 0;

    wm_entry *old_entries = data->entries;
    int64_t old_cap = data->capacity;
    wm_entry *new_entries = wm_alloc_entries(old_cap);
    if (!new_entries)
        return 0;
    rt_weakmap_data replacement = {
        .entries = new_entries,
        .capacity = old_cap,
        .count = 0,
    };

    int64_t rebuilt_removed = 0;
    for (int64_t i = 0; i < old_cap; i++) {
        if (!old_entries[i].occupied)
            continue;
        int alive = wm_entry_alive(&old_entries[i]);
        if (alive < 0) {
            for (int64_t j = 0; j < i; j++) {
                if (old_entries[j].occupied == 2)
                    old_entries[j].occupied = 1;
            }
            free(new_entries);
            return 0;
        }
        if (alive) {
            if (!wm_move_entry(&replacement, old_entries[i])) {
                for (int64_t j = 0; j < i; j++) {
                    if (old_entries[j].occupied == 2)
                        old_entries[j].occupied = 1;
                }
                free(new_entries);
                return 0;
            }
            old_entries[i].occupied = 2;
        } else
            rebuilt_removed++;
    }

    *data = replacement;
    for (int64_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied == 1)
            wm_release_entry(&old_entries[i]);
    }
    free(old_entries);
    return rebuilt_removed;
}
