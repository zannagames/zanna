//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_treemap.c
/// @file
/// @brief Implements the GC-managed, sorted string-keyed TreeMap collection.
///
// Purpose: Implements a sorted string-keyed map (TreeMap) backed by a
//   dynamically-resizing sorted array with binary search. Keys are maintained
//   in ascending lexicographic order at all times, supporting ordered iteration
//   and range queries (Floor, Ceiling, First, Last) not available in the
//   unordered Map.
//
// Key invariants:
//   - Entries are sorted by length-aware unsigned byte order at all times.
//     Embedded null bytes participate in comparison; a null key is normalized
//     to the zero-length key.
//   - Binary search provides O(log n) lookup, Floor, and Ceiling queries.
//   - Insertion uses binary search to find the insertion point, then memmove
//     to shift the suffix right: O(n) per insert.
//   - Removal uses binary search to find the entry, then memmove to shift the
//     suffix left: O(n) per remove.
//   - Capacity doubles when the array is full (starting from 8 entries).
//   - Each entry stores a heap-copied key string (owned) and a void* value
//     (retained on insertion, released on removal/finalization).
//   - Floor(k): largest key <= k; Ceiling(k): smallest key >= k; both O(log n).
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - TreeMap objects are GC-managed (rt_obj_new_i64). The entries array and
//     all heap-copied key strings are freed by the GC finalizer (treemap_finalizer).
//   - Stored values are retained by the map and released when replaced,
//     removed, cleared, or finalized. Lookup returns a borrowed value.
//   - Key-navigation calls construct result strings. Keys() and Values()
//     return new owning Seq snapshots whose elements remain valid independently
//     of later mutations to the map.
//
// Links: src/runtime/collections/rt_treemap.h (public API),
//        src/runtime/collections/rt_map.h (unordered hash map counterpart)
//
//===----------------------------------------------------------------------===//

#include "rt_treemap.h"

#include "rt_collection_ids.h"
#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Installs @p buf as the current thread's non-local trap recovery target.
/// @param buf Jump buffer that receives control when a runtime trap is raised.
void rt_trap_set_recovery(jmp_buf *buf);

/// @brief Removes the current thread's trap recovery target.
void rt_trap_clear_recovery(void);

/// @brief Returns the diagnostic text associated with the most recent runtime trap.
/// @return Borrowed null-terminated error text, or an empty/null result when unavailable.
const char *rt_trap_get_error(void);

/// @brief Initial capacity for the entries array when first allocation occurs.
///
/// Starting with 8 entries provides a reasonable balance between memory
/// efficiency for small maps and reducing reallocation frequency.
#define TREEMAP_INITIAL_CAPACITY 8

/// @brief A single key-value entry in the TreeMap.
///
/// Each entry owns a copy of the key string and retains a reference to
/// the value. Entries are stored in an array sorted by key to enable
/// binary search lookup.
typedef struct {
    char *key;     ///< Owned copy of key string (heap-allocated, null-terminated).
    size_t keylen; ///< Length of key in bytes (excluding null terminator).
    void *value;   ///< Retained value pointer (reference count incremented).
} treemap_entry;

/// @brief Internal implementation structure for the TreeMap container.
///
/// TreeMap maintains entries in a dynamically-sized array that is always
/// kept sorted by key. This enables O(log n) lookup via binary search
/// at the cost of O(n) insertion and deletion (due to array shifting).
///
/// **Invariants:**
/// - entries[i].key < entries[i+1].key for all valid i (lexicographic order)
/// - count <= capacity
/// - All keys are non-NULL and null-terminated
/// - All values have their reference counts incremented
typedef struct {
    void **vptr;            ///< Vtable pointer placeholder (for OOP compatibility).
    treemap_entry *entries; ///< Sorted array of entries (NULL if capacity == 0).
    size_t capacity;        ///< Allocated size of entries array.
    size_t count;           ///< Number of entries currently stored.
} treemap_impl;

/// @brief Checked cast of an opaque handle to the TreeMap implementation;
///        traps with @p what if @p obj is NULL or not a TreeMap.
/// @param obj Opaque runtime object to validate.
/// @param what Diagnostic message used if validation fails.
/// @return The validated implementation pointer, or `NULL` after raising a trap.
static treemap_impl *as_treemap(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_TREEMAP_CLASS_ID, sizeof(treemap_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (treemap_impl *)obj;
}

/// @brief Extracts raw key data and length from an rt_string.
///
/// Converts a Zanna string to a C string pointer and computes its length.
/// A null string denotes the zero-length key. A forged or stale nonnull handle
/// traps and is never normalized to a legitimate empty key.
///
/// @param key The Zanna string to extract data from.
/// @param what Diagnostic raised for a stale or forged nonnull handle.
/// @param out_data Receives borrowed key bytes.
/// @param out_len Output parameter that receives the key length in bytes.
///
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

/// @brief Compares two keys lexicographically.
///
/// Performs a byte-by-byte comparison of two keys, returning a value
/// indicating their relative order. This establishes the total ordering
/// used to maintain sorted entries.
///
/// @param k1 First key data.
/// @param len1 Length of first key.
/// @param k2 Second key data.
/// @param len2 Length of second key.
///
/// @return Negative if k1 < k2, zero if k1 == k2, positive if k1 > k2.
static int key_compare(const char *k1, size_t len1, const char *k2, size_t len2) {
    size_t minlen = len1 < len2 ? len1 : len2;
    int cmp = memcmp(k1, k2, minlen);
    if (cmp != 0)
        return cmp;
    if (len1 < len2)
        return -1;
    if (len1 > len2)
        return 1;
    return 0;
}

/// @brief Searches for a key using binary search.
///
/// Performs binary search on the sorted entries array to find the position
/// of a key. If the key exists, returns its index. If not, returns the
/// index where it should be inserted to maintain sorted order.
///
/// **Algorithm:** Standard binary search with O(log n) comparisons.
///
/// @param tm The TreeMap to search.
/// @param key The key data to search for.
/// @param keylen Length of the key in bytes.
/// @param found Output parameter set to true if exact match found, false otherwise.
///
/// @return The index of the key if found, or the insertion point if not found.
///         The insertion point is the index where the key would be inserted
///         to maintain sorted order.
static size_t binary_search(treemap_impl *tm, const char *key, size_t keylen, bool *found) {
    *found = false;
    if (tm->count == 0)
        return 0;

    size_t lo = 0;
    size_t hi = tm->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        treemap_entry *e = &tm->entries[mid];
        int cmp = key_compare(key, keylen, e->key, e->keylen);
        if (cmp < 0) {
            hi = mid;
        } else if (cmp > 0) {
            lo = mid + 1;
        } else {
            *found = true;
            return mid;
        }
    }
    return lo;
}

/// @brief Ensures the entries array has capacity for at least one more entry.
///
/// If the array is full (count == capacity), doubles the capacity using
/// realloc. For the first allocation (capacity == 0), allocates
/// TREEMAP_INITIAL_CAPACITY entries.
///
/// @param tm The TreeMap to grow if needed.
///
/// @return Nonzero when capacity is available; zero after trapping for
///         capacity, allocation-size, or memory-allocation failure.
static int ensure_capacity(treemap_impl *tm) {
    if (tm->count < tm->capacity)
        return 1;

    if (tm->capacity > SIZE_MAX / 2) {
        rt_trap("TreeMap: capacity overflow");
        return 0;
    }
    size_t new_cap = tm->capacity == 0 ? TREEMAP_INITIAL_CAPACITY : tm->capacity * 2;
    if (new_cap > SIZE_MAX / sizeof(treemap_entry)) {
        rt_trap("TreeMap: allocation size overflow");
        return 0;
    }
    treemap_entry *new_entries =
        (treemap_entry *)realloc(tm->entries, new_cap * sizeof(treemap_entry));
    if (!new_entries) {
        rt_trap("TreeMap: memory allocation failed");
        return 0;
    }
    tm->entries = new_entries;
    tm->capacity = new_cap;
    return 1;
}

/// @brief Frees an entry's key and releases its value.
///
/// Called when removing an entry from the TreeMap. Frees the key string
/// that was allocated when the entry was created, and releases the
/// reference to the value (potentially freeing it if this was the last
/// reference).
///
/// @param e The entry to clean up.
/// @note The entry itself is not cleared; callers must overwrite or zero it
///       before treating the slot as reusable.
static void free_entry_contents(treemap_entry *e) {
    free(e->key);
    if (e->value && rt_obj_release_check0(e->value))
        rt_obj_free(e->value);
}

/// @brief Releases one retained runtime object and frees it if its count reaches zero.
/// @param obj Nullable runtime object reference to release.
static void treemap_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copies the active trap diagnostic into a caller-owned fixed buffer.
/// @param buffer Destination buffer for a null-terminated message.
/// @param buffer_size Size of @p buffer in bytes.
/// @param fallback Message used when the trap subsystem has no nonempty diagnostic.
static void treemap_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Appends a copied key string to a snapshot sequence with trap cleanup.
///
/// If string construction or sequence insertion traps, this helper releases
/// the temporary string and the entire partially constructed sequence before
/// re-raising the saved diagnostic.
///
/// @param seq Owning sequence being populated; consumed on a trapped failure.
/// @param key Key bytes to copy into a runtime string.
/// @param keylen Number of bytes available at @p key.
/// @return Nonzero after append; zero after releasing @p seq and retrapping.
static int treemap_push_key_or_release_seq(void *seq, const char *key, size_t keylen) {
    rt_string volatile str = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        treemap_save_trap_error(
            saved_error, sizeof(saved_error), "TreeMap.Keys: snapshot append failed");
        rt_trap_clear_recovery();
        if (str)
            rt_str_release_maybe((rt_string)str);
        treemap_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    str = rt_string_from_bytes(key, keylen);
    if (!str)
        rt_trap("TreeMap.Keys: string allocation failed");
    rt_seq_push(seq, (void *)str);
    rt_str_release_maybe((rt_string)str);
    str = NULL;
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Appends a value to an owning snapshot sequence with trap cleanup.
///
/// The sequence retains @p value through `rt_seq_push`. If insertion traps,
/// the entire partially constructed sequence is released before the saved
/// diagnostic is re-raised.
///
/// @param seq Owning sequence being populated; consumed on a trapped failure.
/// @param value Nullable borrowed value to append and retain.
/// @return Nonzero after append; zero after releasing @p seq and retrapping.
static int treemap_push_value_or_release_seq(void *seq, void *value) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        treemap_save_trap_error(
            saved_error, sizeof(saved_error), "TreeMap.Values: snapshot append failed");
        rt_trap_clear_recovery();
        treemap_release_object(seq);
        rt_trap(saved_error);
        return 0;
    }

    rt_seq_push(seq, value);
    rt_trap_clear_recovery();
    return 1;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief GC finalizer: free each entry's contents (key + released value),
///        then free the sorted entries array.
/// @param obj TreeMap instance being finalized; a null pointer is ignored.
static void treemap_finalizer(void *obj) {
    if (!obj)
        return;
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return;
    if (tm->entries) {
        for (size_t i = 0; i < tm->count; i++)
            free_entry_contents(&tm->entries[i]);
        free(tm->entries);
        tm->entries = NULL;
    }
    tm->count = 0;
    tm->capacity = 0;
}

/// @brief GC traversal: visit every stored value in sorted-key order.
/// @param obj TreeMap instance whose outgoing references are being traced.
/// @param visitor Callback invoked once for each nullable stored value.
/// @param ctx Opaque context forwarded unchanged to @p visitor.
static void treemap_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return;
    for (size_t i = 0; i < tm->count; i++)
        visitor(tm->entries[i].value, ctx);
}

/// @brief Creates a new empty TreeMap.
///
/// Allocates the GC-managed object but defers allocation of the sorted entry
/// array until the first insertion.
///
/// @return New empty TreeMap, or `NULL` after trapping if allocation fails.
/// @note The returned object is tracked by the garbage collector and must not
///       be freed directly by the caller.
/// @see rt_treemap_set
/// @see rt_treemap_keys
void *rt_treemap_new(void) {
    treemap_impl *tm =
        (treemap_impl *)rt_obj_new_i64(RT_TREEMAP_CLASS_ID, (int64_t)sizeof(treemap_impl));
    if (!tm) {
        rt_trap("TreeMap: memory allocation failed");
        return NULL;
    }

    tm->vptr = NULL;
    tm->entries = NULL;
    tm->capacity = 0;
    tm->count = 0;

    rt_obj_set_finalizer(tm, treemap_finalizer);
    rt_gc_track(tm, treemap_traverse);
    return tm;
}

/// @brief Returns the number of key-value pairs in the TreeMap.
///
/// @param obj Pointer to a TreeMap object.
///
/// @return The number of entries in the map.
///
/// @note O(1) time complexity.
/// @note A null handle is treated as an empty map; a non-TreeMap handle traps.
int64_t rt_treemap_len(void *obj) {
    if (!obj)
        return 0;
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return 0;
    return (int64_t)tm->count;
}

/// @brief Checks whether the TreeMap contains no entries.
///
/// @param obj Pointer to a TreeMap object.
///
/// @return 1 (true) if the TreeMap is empty, 0 (false) otherwise.
///
/// @note O(1) time complexity.
/// @note A null handle is treated as empty; a non-TreeMap handle traps.
int8_t rt_treemap_is_empty(void *obj) {
    if (!obj)
        return 1;
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return 1;
    return tm->count == 0 ? 1 : 0;
}

/// @brief Sets or updates a key-value pair in the TreeMap.
///
/// If the key already exists, updates its value (releasing the old value
/// and retaining the new one). If the key doesn't exist, inserts a new
/// entry at the correct sorted position.
///
/// **Insertion maintains sorted order:**
/// ```
/// Before: [alpha, charlie, delta]
/// Set("bravo", val)
/// After:  [alpha, bravo, charlie, delta]
/// ```
///
/// @param obj Pointer to a TreeMap object.
/// @param key The key string. Its bytes are copied for a new entry; a null
///            string denotes the empty key.
/// @param value The value to associate with the key. May be NULL.
///              The TreeMap retains a reference to this value.
///
/// @note O(log n) for lookup + O(n) for insertion (array shifting).
/// @note Updating an existing key is O(log n). Passing a null map is a no-op.
/// @note Traps for an invalid object or if capacity/key allocation fails.
///
/// @see rt_treemap_get For retrieving values by key
void rt_treemap_set(void *obj, rt_string key, void *value) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm) {
        rt_gc_mutator_exit();
        return;
    }

    size_t keylen = 0;
    const char *keydata = NULL;
    if (!get_key_data(key, "TreeMap.Set: invalid key", &keydata, &keylen)) {
        rt_gc_mutator_exit();
        return;
    }

    bool found;
    size_t idx = binary_search(tm, keydata, keylen, &found);

    if (found) {
        // Update existing entry
        treemap_entry *e = &tm->entries[idx];
        // Retain new value
        rt_obj_retain_maybe(value);
        // Release old value after retaining in case the pointer is unchanged.
        void *old_value = e->value;
        e->value = value;
        if (old_value && rt_obj_release_check0(old_value))
            rt_obj_free(old_value);
    } else {
        // Insert new entry
        if (tm->count == SIZE_MAX) {
            rt_gc_mutator_exit();
            rt_trap("TreeMap.Set: maximum size reached");
            return;
        }
        if (!ensure_capacity(tm)) {
            rt_gc_mutator_exit();
            return;
        }

        if (value)
            rt_obj_retain_maybe(value);

        if (keylen == SIZE_MAX) {
            if (value && rt_obj_release_check0(value))
                rt_obj_free(value);
            rt_gc_mutator_exit();
            rt_trap("TreeMap: key allocation overflow");
            return;
        }
        char *key_copy = (char *)malloc(keylen + 1);
        if (!key_copy) {
            if (value && rt_obj_release_check0(value))
                rt_obj_free(value);
            rt_gc_mutator_exit();
            rt_trap("TreeMap: memory allocation failed");
            return;
        }
        memcpy(key_copy, keydata, keylen);
        key_copy[keylen] = '\0';

        // Make room by shifting entries
        if (idx < tm->count) {
            memmove(&tm->entries[idx + 1],
                    &tm->entries[idx],
                    (tm->count - idx) * sizeof(treemap_entry));
        }

        // Create new entry
        treemap_entry *e = &tm->entries[idx];
        e->key = key_copy;
        e->keylen = keylen;
        e->value = value;

        tm->count++;
    }
    rt_gc_mutator_exit();
}

/// @brief Retrieves the value associated with a key.
///
/// Performs binary search to find the key and returns its associated value.
///
/// @param obj Pointer to a TreeMap object.
/// @param key The key to look up; a null string denotes the empty key.
///
/// @return Borrowed value associated with the key, or `NULL` if the key is
///         absent, its stored value is null, or @p obj is null.
///
/// @note O(log n) time complexity (binary search).
/// @note A nonnull object of the wrong runtime class raises a trap.
///
/// @see rt_treemap_has For checking if a key exists
/// @see rt_treemap_set For storing key-value pairs
void *rt_treemap_get(void *obj, rt_string key) {
    if (!obj)
        return NULL;
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return NULL;

    size_t keylen = 0;
    const char *keydata = NULL;
    if (!get_key_data(key, "TreeMap.Get: invalid key", &keydata, &keylen))
        return NULL;

    bool found;
    size_t idx = binary_search(tm, keydata, keylen, &found);

    if (found)
        return tm->entries[idx].value;
    return NULL;
}

/// @brief Checks whether a key exists in the TreeMap.
///
/// @param obj Pointer to a TreeMap object.
/// @param key The key to check for; a null string denotes the empty key.
///
/// @return 1 (true) if the key exists, 0 (false) otherwise.
///
/// @note O(log n) time complexity (binary search).
/// @note A null map returns false; a non-TreeMap handle raises a trap.
int8_t rt_treemap_has(void *obj, rt_string key) {
    if (!obj)
        return 0;
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return 0;

    size_t keylen = 0;
    const char *keydata = NULL;
    if (!get_key_data(key, "TreeMap.Has: invalid key", &keydata, &keylen))
        return 0;

    bool found;
    binary_search(tm, keydata, keylen, &found);

    return found ? 1 : 0;
}

/// @brief Removes a key-value pair from the TreeMap.
///
/// If the key exists, removes the entry, frees the key copy, releases
/// the value reference, and shifts remaining entries to maintain sorted order.
///
/// @param obj Pointer to a TreeMap object.
/// @param key The key to remove; a null string denotes the empty key.
///
/// @return 1 (true) if the key was found and removed, 0 (false) if not found.
///
/// @note O(log n) for lookup + O(n) for removal (array shifting).
/// @note Removal releases the map's retained value reference. A null map
///       returns false; a non-TreeMap handle raises a trap.
int8_t rt_treemap_remove(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_gc_mutator_enter();
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t keylen = 0;
    const char *keydata = NULL;
    if (!get_key_data(key, "TreeMap.Remove: invalid key", &keydata, &keylen)) {
        rt_gc_mutator_exit();
        return 0;
    }

    bool found;
    size_t idx = binary_search(tm, keydata, keylen, &found);

    if (!found) {
        rt_gc_mutator_exit();
        return 0;
    }

    treemap_entry removed = tm->entries[idx];

    // Shift remaining entries
    if (idx < tm->count - 1) {
        memmove(&tm->entries[idx],
                &tm->entries[idx + 1],
                (tm->count - idx - 1) * sizeof(treemap_entry));
    }

    tm->count--;
    memset(&tm->entries[tm->count], 0, sizeof(treemap_entry));
    free_entry_contents(&removed);
    rt_gc_mutator_exit();
    return 1;
}

/// @brief Removes all key-value pairs from the TreeMap.
///
/// Frees all key copies and releases all value references. The entries
/// array capacity remains allocated for potential reuse.
///
/// @param obj Pointer to a TreeMap object.
///
/// @note O(n) time complexity where n is the number of entries.
/// @note The allocated entry capacity is retained for reuse. A null map is
///       ignored; a non-TreeMap handle raises a trap.
void rt_treemap_clear(void *obj) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm) {
        rt_gc_mutator_exit();
        return;
    }

    size_t count = tm->count;
    tm->count = 0;
    for (size_t i = 0; i < count; i++) {
        free_entry_contents(&tm->entries[i]);
        memset(&tm->entries[i], 0, sizeof(treemap_entry));
    }
    rt_gc_mutator_exit();
}

/// @brief Returns all keys in the TreeMap as a Seq, in sorted order.
///
/// Creates a new Seq containing all keys from the TreeMap. Because the
/// TreeMap maintains sorted order internally, the keys in the returned
/// Seq are already in lexicographic order.
///
/// **Usage example:**
/// ```
/// map.Set("charlie", v1)
/// map.Set("alpha", v2)
/// map.Set("bravo", v3)
/// Dim keys = map.Keys()
/// ' keys = ["alpha", "bravo", "charlie"]
/// ```
///
/// @param obj Pointer to a TreeMap object.
///
/// @return A new owning Seq containing independent key strings in sorted
///         order, or `NULL` if sequence allocation fails.
///
/// @note O(n) time complexity where n is the number of entries.
/// @note A null map produces an empty sequence. Snapshot construction releases
///       the partial sequence before propagating an append trap.
///
/// @see rt_treemap_values For retrieving values
void *rt_treemap_keys(void *obj) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return seq;

    for (size_t i = 0; i < tm->count; i++) {
        if (!treemap_push_key_or_release_seq(seq, tm->entries[i].key, tm->entries[i].keylen))
            return NULL;
    }

    return seq;
}

/// @brief Returns all values in the TreeMap as a Seq, in key-sorted order.
///
/// Creates a new Seq containing all values from the TreeMap. Values appear
/// in the same order as their corresponding keys (sorted lexicographically).
///
/// @param obj Pointer to a TreeMap object.
///
/// @return A new owning Seq containing retained value references in key-sorted
///         order, or `NULL` if sequence allocation fails.
///
/// @note O(n) time complexity where n is the number of entries.
/// @note A null map produces an empty sequence. Snapshot construction releases
///       the partial sequence before propagating an append trap.
///
/// @see rt_treemap_keys For retrieving keys
void *rt_treemap_values(void *obj) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return seq;

    for (size_t i = 0; i < tm->count; i++) {
        if (!treemap_push_value_or_release_seq(seq, tm->entries[i].value))
            return NULL;
    }

    return seq;
}

/// @brief Returns the smallest (first) key in the TreeMap.
///
/// Because keys are stored in sorted order, this returns the lexicographically
/// smallest key, which is the first entry in the sorted array.
///
/// **Usage example:**
/// ```
/// map.Set("charlie", v1)
/// map.Set("alpha", v2)
/// map.Set("bravo", v3)
/// Print map.First()    ' Outputs: "alpha"
/// ```
///
/// @param obj Pointer to a TreeMap object.
///
/// @return Newly constructed smallest key, or the runtime empty-string
///         constant when the map is null or empty.
///
/// @note O(1) time complexity.
/// @note A non-TreeMap handle raises a trap. String allocation may also trap.
///
/// @see rt_treemap_last For the largest key
rt_string rt_treemap_first(void *obj) {
    if (!obj)
        return rt_const_cstr("");
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return rt_const_cstr("");

    if (tm->count == 0)
        return rt_const_cstr("");

    return rt_string_from_bytes(tm->entries[0].key, tm->entries[0].keylen);
}

/// @brief Returns the largest (last) key in the TreeMap.
///
/// Because keys are stored in sorted order, this returns the lexicographically
/// largest key, which is the last entry in the sorted array.
///
/// **Usage example:**
/// ```
/// map.Set("charlie", v1)
/// map.Set("alpha", v2)
/// map.Set("bravo", v3)
/// Print map.Last()     ' Outputs: "charlie"
/// ```
///
/// @param obj Pointer to a TreeMap object.
///
/// @return Newly constructed largest key, or the runtime empty-string
///         constant when the map is null or empty.
///
/// @note O(1) time complexity.
/// @note A non-TreeMap handle raises a trap. String allocation may also trap.
///
/// @see rt_treemap_first For the smallest key
rt_string rt_treemap_last(void *obj) {
    if (!obj)
        return rt_const_cstr("");
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return rt_const_cstr("");

    if (tm->count == 0)
        return rt_const_cstr("");

    size_t last = tm->count - 1;
    return rt_string_from_bytes(tm->entries[last].key, tm->entries[last].keylen);
}

/// @brief Returns the largest key less than or equal to the given key.
///
/// Performs a "floor" operation: finds the greatest key in the TreeMap
/// that is less than or equal to the specified key. If the key exists,
/// returns it. If not, returns the next smaller key.
///
/// **Usage example:**
/// ```
/// map.Set("apple", v1)
/// map.Set("cherry", v2)
/// map.Set("grape", v3)
///
/// Print map.Floor("cherry")    ' Outputs: "cherry" (exact match)
/// Print map.Floor("date")      ' Outputs: "cherry" (next smaller)
/// Print map.Floor("aardvark")  ' Outputs: "" (nothing smaller)
/// ```
///
/// @param obj Pointer to a TreeMap object.
/// @param key The key to find the floor for; a null string denotes the empty key.
///
/// @return Newly constructed floor key, or the runtime empty-string constant
///         if no key is less than or equal to @p key.
///
/// @note O(log n) time complexity (binary search).
/// @note The empty string is also a valid stored key, so callers cannot use
///       the returned contents alone to distinguish it from “no result.”
///
/// @see rt_treemap_ceil For finding the smallest key >= a given key
rt_string rt_treemap_floor(void *obj, rt_string key) {
    if (!obj)
        return rt_const_cstr("");
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return rt_const_cstr("");

    if (tm->count == 0)
        return rt_const_cstr("");

    size_t keylen = 0;
    const char *keydata = NULL;
    if (!get_key_data(key, "TreeMap.Floor: invalid key", &keydata, &keylen))
        return NULL;

    bool found;
    size_t idx = binary_search(tm, keydata, keylen, &found);

    if (found) {
        // Exact match
        return rt_string_from_bytes(tm->entries[idx].key, tm->entries[idx].keylen);
    }

    // idx is insertion point - floor is the previous entry
    if (idx == 0)
        return rt_const_cstr(""); // No key <= given key

    return rt_string_from_bytes(tm->entries[idx - 1].key, tm->entries[idx - 1].keylen);
}

/// @brief Returns the smallest key greater than or equal to the given key.
///
/// Performs a "ceiling" operation: finds the smallest key in the TreeMap
/// that is greater than or equal to the specified key. If the key exists,
/// returns it. If not, returns the next larger key.
///
/// **Usage example:**
/// ```
/// map.Set("apple", v1)
/// map.Set("cherry", v2)
/// map.Set("grape", v3)
///
/// Print map.Ceil("cherry")     ' Outputs: "cherry" (exact match)
/// Print map.Ceil("date")       ' Outputs: "grape" (next larger)
/// Print map.Ceil("zebra")      ' Outputs: "" (nothing larger)
/// ```
///
/// @param obj Pointer to a TreeMap object.
/// @param key The key to find the ceiling for; a null string denotes the empty key.
///
/// @return Newly constructed ceiling key, or the runtime empty-string constant
///         if no key is greater than or equal to @p key.
///
/// @note O(log n) time complexity (binary search).
/// @note The empty string is also a valid stored key, so callers cannot use
///       the returned contents alone to distinguish it from “no result.”
///
/// @see rt_treemap_floor For finding the largest key <= a given key
rt_string rt_treemap_ceil(void *obj, rt_string key) {
    if (!obj)
        return rt_const_cstr("");
    treemap_impl *tm = as_treemap(obj, "TreeMap: invalid TreeMap object");
    if (!tm)
        return rt_const_cstr("");

    if (tm->count == 0)
        return rt_const_cstr("");

    size_t keylen = 0;
    const char *keydata = NULL;
    if (!get_key_data(key, "TreeMap.Ceil: invalid key", &keydata, &keylen))
        return NULL;

    bool found;
    size_t idx = binary_search(tm, keydata, keylen, &found);

    if (found) {
        // Exact match
        return rt_string_from_bytes(tm->entries[idx].key, tm->entries[idx].keylen);
    }

    // idx is insertion point - ceiling is the entry at idx (if exists)
    if (idx >= tm->count)
        return rt_const_cstr(""); // No key >= given key

    return rt_string_from_bytes(tm->entries[idx].key, tm->entries[idx].keylen);
}
