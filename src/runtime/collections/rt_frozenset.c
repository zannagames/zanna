//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_frozenset.c
// Purpose: Implements an immutable string set (FrozenSet) built once from a Seq
//   of strings. After construction no elements can be added or removed. Uses
//   open-addressing with the runtime keyed hash for O(1) average-case membership tests.
//   Typical uses: constant keyword tables, stop-word lists, and any membership
//   query where the set contents are known at build time.
//
// Key invariants:
//   - Open-addressing hash table; slot array is sized to 2× the element count
//     at construction so load factor stays below 50%.
//   - Slot key == NULL indicates an empty slot; no tombstones needed since the
//     set is immutable after build.
//   - Runtime keyed hash over the raw string bytes; linear probing on collision.
//   - Keys are stored as retained rt_string references (not copied); the
//     FrozenSet keeps references to prevent GC collection of key strings.
//   - Contains returns 1 if the string is present, 0 otherwise.
//   - Safe for concurrent read-only access after construction completes.
//
// Ownership/Lifetime:
//   - FrozenSet objects are GC-managed (rt_obj_new_i64). The slots array is
//     freed by the GC finalizer (frozenset_finalizer).
//
// Links: src/runtime/collections/rt_frozenset.h (public API),
//        src/runtime/collections/rt_set.h (mutable set counterpart)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements an immutable, open-addressed set of runtime strings.
///
/// FrozenSet construction retains non-null strings extracted from a Seq,
/// collapses byte-equal duplicates, and sizes a power-of-two slot array for a
/// load factor below one half. A null slot key marks unused storage, so null
/// elements are skipped rather than representing the empty string.
///
/// Membership and set algebra are read-only with respect to their operands.
/// Algebra operations allocate independent FrozenSets, and item enumeration
/// returns an owning Seq that retains each string. Completed FrozenSets are
/// safe for concurrent read-only access.

#include "rt_frozenset.h"

#include "rt_box.h"
#include "rt_collection_ids.h"
#include "rt_hash_util.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq_internal.h"
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

// --- Helper: extract string from seq element (may be boxed) ---

/// @brief Validate and coerce a Seq element to an rt_string.
/// @param elem Raw runtime string handle or boxed string value.
/// @param out Receives the extracted string or NULL for a null element/string.
/// @param owned Set to 1 if @p out owns an unboxed retain the caller must
///              release; otherwise zero.
/// @return One for a valid raw/boxed/null representation; zero after trapping.
static int fs_extract_str(void *elem, rt_string *out, int *owned) {
    *out = NULL;
    *owned = 0;
    if (!elem)
        return 1;
    if (rt_string_is_handle(elem)) {
        *out = (rt_string)elem;
        return 1;
    }
    if (rt_box_type(elem) != RT_BOX_STR) {
        rt_trap("FrozenSet: element is not a string");
        return 0;
    }
    if (!rt_box_try_to_str(elem, out)) {
        rt_trap("FrozenSet: invalid boxed string element");
        return 0;
    }
    *owned = *out ? 1 : 0;
    return 1;
}

// --- Hash table entry (open addressing) ---

/// @brief One open-addressing slot; NULL marks an unused slot.
typedef struct {
    rt_string key; // NULL = empty slot
} fs_slot;

/// @brief Internal FrozenSet object and its immutable slot table.
typedef struct {
    void *vptr;
    int64_t count;
    int64_t capacity;
    fs_slot *slots;
} rt_frozenset_impl;

/// @brief Checked cast of an opaque handle to the FrozenSet implementation.
/// @details Traps with @p what if @p obj is NULL or not a FrozenSet.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used on validation failure.
/// @return The validated implementation pointer, or NULL if a returning trap
///         handler resumes after failed validation.
static rt_frozenset_impl *as_frozenset(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_FROZENSET_CLASS_ID, sizeof(rt_frozenset_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_frozenset_impl *)obj;
}

// --- Keyed hash ---

/// @brief Per-process keyed hash of @p len bytes of @p data.
/// @param data Key bytes; NULL is treated as an empty buffer.
/// @param len Number of bytes to hash; non-positive values hash no bytes.
/// @return Keyed 64-bit hash value.
static uint64_t fs_hash(const char *data, int64_t len) {
    return rt_keyed_hash_bytes(data ? data : "", len > 0 ? (size_t)len : 0);
}

/// @brief Keyed hash of an rt_string's bytes (empty string for NULL).
/// @param s Runtime string to hash.
/// @return Keyed 64-bit hash of the accessible byte sequence.
static uint64_t fs_str_hash(rt_string s) {
    if (!s)
        return fs_hash("", 0);
    int64_t len = rt_str_len(s);
    const char *cstr = rt_string_cstr(s);
    return fs_hash(cstr ? cstr : "", len > 0 && cstr ? len : 0);
}

// --- Internal helpers ---

/// @brief Borrow the byte buffer + length of a key string (empty "" if null).
/// @param key Runtime string to inspect.
/// @param out_len Receives the usable byte length.
/// @return Borrowed bytes, or a stable empty string for a null, empty, or
///         inaccessible runtime string.
static const char *fs_key_data(rt_string key, int64_t *out_len) {
    if (!key) {
        *out_len = 0;
        return "";
    }
    int64_t len = rt_str_len(key);
    if (len <= 0) {
        *out_len = 0;
        return "";
    }
    const char *data = rt_string_cstr(key);
    if (!data) {
        *out_len = 0;
        return "";
    }
    *out_len = len;
    return data;
}

/// @brief Byte-exact equality test between stored @p key and @p data/@p len.
/// @param key Stored runtime string.
/// @param data Candidate bytes.
/// @param len Number of candidate bytes.
/// @return 1 for equal lengths and contents; otherwise 0.
static int8_t fs_key_equals(rt_string key, const char *data, int64_t len) {
    int64_t key_len = 0;
    const char *key_data = fs_key_data(key, &key_len);
    return key_len == len && memcmp(key_data, data, (size_t)len) == 0 ? 1 : 0;
}

/// @brief Release one temporary runtime object and finalize it at zero.
/// @param obj Runtime-managed object, or NULL for a no-op.
static void fs_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copies the active trap message into a stable local buffer.
/// @param buffer Destination character buffer.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Text used when no non-empty runtime error is available.
static void fs_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief GC finalizer: unref every occupied slot's key and free the slots.
/// @param obj FrozenSet object being finalized, or NULL for a no-op.
static void fs_finalizer(void *obj) {
    if (!obj)
        return;
    rt_frozenset_impl *fs = as_frozenset(obj, "FrozenSet: invalid FrozenSet object");
    if (!fs)
        return;
    if (fs->slots) {
        for (int64_t i = 0; i < fs->capacity; i++) {
            if (fs->slots[i].key)
                rt_string_unref(fs->slots[i].key);
        }
        free(fs->slots);
        fs->slots = NULL;
    }
    fs->capacity = 0;
    fs->count = 0;
}

/// @brief Smallest power of two >= @p n, floor 16 (capacity is a power of
///        two so probing can mask instead of modulo). Traps on overflow.
/// @param n Minimum requested capacity.
/// @return Power-of-two capacity of at least 16, or zero after an overflow trap.
static int64_t fs_next_pow2(int64_t n) {
    int64_t p = 16;
    while (p < n) {
        if (p > INT64_MAX / 2) {
            rt_trap("FrozenSet: capacity overflow");
            return 0;
        }
        p *= 2;
    }
    return p;
}

/// @brief Allocate a FrozenSet sized for @p count entries (~50% load factor,
///        power-of-two capacity). Installs the finalizer; traps on
///        overflow/OOM.
/// @param count Maximum source-element count used to size the table.
/// @return Newly allocated empty FrozenSet implementation, or NULL after a
///         size or allocation trap.
static rt_frozenset_impl *fs_alloc(int64_t count) {
    // Use ~50% load factor for good probe performance
    int64_t needed = 8;
    if (count >= 4) {
        if (count > INT64_MAX / 2) {
            rt_trap("FrozenSet: capacity overflow");
            return NULL;
        }
        needed = count * 2;
    }
    int64_t cap = fs_next_pow2(needed);
    if (cap <= 0)
        return NULL;
    if ((uint64_t)cap > SIZE_MAX / sizeof(fs_slot)) {
        rt_trap("FrozenSet: allocation size overflow");
        return NULL;
    }
    rt_frozenset_impl *fs =
        (rt_frozenset_impl *)rt_obj_new_i64(RT_FROZENSET_CLASS_ID, sizeof(rt_frozenset_impl));
    if (!fs) {
        rt_trap("FrozenSet: memory allocation failed");
        return NULL;
    }
    fs->vptr = NULL;
    fs->count = 0;
    fs->capacity = cap;
    fs->slots = (fs_slot *)calloc((size_t)cap, sizeof(fs_slot));
    if (!fs->slots) {
        if (rt_obj_release_check0(fs))
            rt_obj_free(fs);
        rt_trap("rt_frozenset: memory allocation failed");
        return NULL;
    }
    rt_obj_set_finalizer(fs, fs_finalizer);
    return fs;
}

/// @brief Linear-probe insert during construction; ignores duplicates.
/// @param fs FrozenSet under construction.
/// @param key Non-null string reference to retain if newly inserted.
/// @return 1 if @p key was newly added, 0 if it was already present, or -1
///         after a returning key-retain trap.
static int8_t fs_insert(rt_frozenset_impl *fs, rt_string key) {
    uint64_t h = fs_str_hash(key);
    int64_t mask = fs->capacity - 1;
    int64_t idx = (int64_t)(h & (uint64_t)mask);
    int64_t key_len = 0;
    const char *key_data = fs_key_data(key, &key_len);

    for (int64_t i = 0; i < fs->capacity; i++) {
        int64_t slot = (idx + i) & mask;
        if (!fs->slots[slot].key) {
            if (!rt_string_ref(key))
                return -1;
            fs->slots[slot].key = key;
            fs->count++;
            return 1;
        }
        if (fs_key_equals(fs->slots[slot].key, key_data, key_len))
            return 0; // duplicate
    }
    return 0;
}

/// @brief Linear-probe membership test for @p key (0 if set is empty/absent).
/// @param fs FrozenSet implementation to search.
/// @param key String whose byte sequence is matched.
/// @return 1 if present; otherwise 0.
static int8_t fs_contains(rt_frozenset_impl *fs, rt_string key) {
    if (!fs || fs->count == 0)
        return 0;
    uint64_t h = fs_str_hash(key);
    int64_t mask = fs->capacity - 1;
    int64_t idx = (int64_t)(h & (uint64_t)mask);
    int64_t key_len = 0;
    const char *key_data = fs_key_data(key, &key_len);

    for (int64_t i = 0; i < fs->capacity; i++) {
        int64_t slot = (idx + i) & mask;
        if (!fs->slots[slot].key)
            return 0;
        if (fs_key_equals(fs->slots[slot].key, key_data, key_len))
            return 1;
    }
    return 0;
}

// --- Public API ---

/// @brief Build an immutable set from a Seq of strings (duplicates collapse). Internal storage
/// is an open-addressed hash table sized for the entry count. Use mutable Set elsewhere.
/// @param items Seq of raw or boxed string values, or NULL.
/// @return A new runtime-managed FrozenSet; NULL input produces an empty set.
/// @note Null extracted elements are skipped.
void *rt_frozenset_from_seq(void *items) {
    if (!items)
        return (void *)fs_alloc(0);
    if (!rt_seq_internal_is_valid(items)) {
        rt_trap("FrozenSet: invalid items Seq");
        return NULL;
    }

    int64_t n = rt_seq_len(items);
    rt_frozenset_impl *fs = fs_alloc(n);
    if (!fs)
        return NULL;

    volatile rt_string pending_element = NULL;
    volatile int pending_element_owned = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        fs_save_trap_error(
            saved_error, sizeof(saved_error), "FrozenSet: element extraction failed");
        rt_trap_clear_recovery();
        if (pending_element_owned)
            rt_str_release_maybe((rt_string)pending_element);
        fs_release_object(fs);
        rt_trap(saved_error);
        return NULL;
    }

    for (int64_t i = 0; i < n; i++) {
        int owned = 0;
        rt_string elem = NULL;
        if (!fs_extract_str(rt_seq_get(items, i), &elem, &owned)) {
            rt_trap_clear_recovery();
            fs_release_object(fs);
            return NULL;
        }
        pending_element = elem;
        pending_element_owned = owned;
        int8_t inserted = elem ? fs_insert(fs, elem) : 0;
        if (owned)
            rt_str_release_maybe(elem);
        pending_element = NULL;
        pending_element_owned = 0;
        if (inserted < 0) {
            rt_trap_clear_recovery();
            fs_release_object(fs);
            return NULL;
        }
    }
    rt_trap_clear_recovery();
    return (void *)fs;
}

/// @brief Construct an empty frozen set.
/// @return A new runtime-managed empty FrozenSet.
void *rt_frozenset_empty(void) {
    return (void *)fs_alloc(0);
}

/// @brief Return the number of elements in the frozen (immutable) set.
/// @param obj FrozenSet handle, or NULL to query an empty set.
/// @return Element count, or zero for NULL.
int64_t rt_frozenset_len(void *obj) {
    if (!obj)
        return 0;
    rt_frozenset_impl *set = as_frozenset(obj, "FrozenSet.Len: invalid FrozenSet object");
    return set ? set->count : 0;
}

/// @brief Check whether the frozen set has no elements.
/// @param obj FrozenSet handle, or NULL to test an empty set.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_frozenset_is_empty(void *obj) {
    return rt_frozenset_len(obj) == 0 ? 1 : 0;
}

/// @brief Check whether an element exists in the frozen set.
/// @details Uses hash-based lookup on the immutable backing array.
/// @param obj FrozenSet handle, or NULL.
/// @param elem Non-null runtime string to find.
/// @return 1 if present; otherwise 0.
int8_t rt_frozenset_has(void *obj, rt_string elem) {
    if (!obj)
        return 0;
    rt_frozenset_impl *fs = as_frozenset(obj, "FrozenSet.Has: invalid FrozenSet object");
    if (!elem)
        return 0;
    if (!fs)
        return 0;
    if (!rt_string_is_handle(elem)) {
        rt_trap("FrozenSet.Has: invalid element");
        return 0;
    }
    return fs_contains(fs, elem);
}

/// @brief Return a Seq of every element in the set (slot-iteration order, not insertion order).
/// @param obj FrozenSet handle, or NULL to enumerate an empty set.
/// @return A new owning Seq retaining all strings in unspecified slot order.
void *rt_frozenset_items(void *obj) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;

    rt_frozenset_impl *fs = as_frozenset(obj, "FrozenSet.Items: invalid FrozenSet object");
    if (!fs)
        return seq;
    for (int64_t i = 0; i < fs->capacity; i++) {
        if (fs->slots[i].key)
            rt_seq_push(seq, fs->slots[i].key);
    }
    return seq;
}

/// @brief Return a fresh frozen set containing every element from either operand. Duplicates
/// (elements present in both) appear once. Implemented by zipping into a Seq then re-freezing.
/// @param obj First FrozenSet, or NULL as an empty set.
/// @param other Second FrozenSet, or NULL as an empty set.
/// @return A new independent FrozenSet containing the union.
void *rt_frozenset_union(void *obj, void *other) {
    // Collect all elements from both sets
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;

    if (obj) {
        rt_frozenset_impl *a = as_frozenset(obj, "FrozenSet.Union: invalid FrozenSet object");
        if (!a) {
            fs_release_object(seq);
            return NULL;
        }
        for (int64_t i = 0; i < a->capacity; i++) {
            if (a->slots[i].key)
                rt_seq_push(seq, a->slots[i].key);
        }
    }
    if (other) {
        rt_frozenset_impl *b = as_frozenset(other, "FrozenSet.Union: invalid FrozenSet object");
        if (!b) {
            fs_release_object(seq);
            return NULL;
        }
        for (int64_t i = 0; i < b->capacity; i++) {
            if (b->slots[i].key)
                rt_seq_push(seq, b->slots[i].key);
        }
    }

    void *result = rt_frozenset_from_seq(seq);
    fs_release_object(seq);
    return result;
}

/// @brief Return a fresh frozen set containing only elements present in both operands.
/// Returns an empty set if either operand is NULL.
/// @param obj First FrozenSet, or NULL as an empty set.
/// @param other Second FrozenSet, or NULL as an empty set.
/// @return A new independent FrozenSet containing the intersection.
void *rt_frozenset_intersect(void *obj, void *other) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_frozenset_impl *a =
        obj ? as_frozenset(obj, "FrozenSet.Intersect: invalid FrozenSet object") : NULL;
    rt_frozenset_impl *b =
        other ? as_frozenset(other, "FrozenSet.Intersect: invalid FrozenSet object") : NULL;
    if ((obj && !a) || (other && !b)) {
        fs_release_object(seq);
        return NULL;
    }
    if (!a || !b) {
        void *result = rt_frozenset_from_seq(seq);
        fs_release_object(seq);
        return result;
    }

    for (int64_t i = 0; i < a->capacity; i++) {
        if (a->slots[i].key && fs_contains(b, a->slots[i].key))
            rt_seq_push(seq, a->slots[i].key);
    }

    void *result = rt_frozenset_from_seq(seq);
    fs_release_object(seq);
    return result;
}

/// @brief Return a fresh frozen set containing elements present in `obj` but not in `other`.
/// `obj - other` set semantics. NULL `other` returns a copy of `obj`.
/// @param obj Source FrozenSet, or NULL as an empty set.
/// @param other Exclusion FrozenSet, or NULL to exclude nothing.
/// @return A new independent FrozenSet containing the difference.
void *rt_frozenset_diff(void *obj, void *other) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    if (!obj) {
        void *result = rt_frozenset_from_seq(seq);
        fs_release_object(seq);
        return result;
    }

    rt_frozenset_impl *a = as_frozenset(obj, "FrozenSet.Diff: invalid FrozenSet object");
    rt_frozenset_impl *b =
        other ? as_frozenset(other, "FrozenSet.Diff: invalid FrozenSet object") : NULL;
    if (!a || (other && !b)) {
        fs_release_object(seq);
        return NULL;
    }

    for (int64_t i = 0; i < a->capacity; i++) {
        if (a->slots[i].key) {
            if (!b || !fs_contains(b, a->slots[i].key))
                rt_seq_push(seq, a->slots[i].key);
        }
    }

    void *result = rt_frozenset_from_seq(seq);
    fs_release_object(seq);
    return result;
}

/// @brief Check whether this frozen set is a subset of another.
/// @details Every element in this set must also be present in the other.
/// @param obj Candidate subset, or NULL as an empty set.
/// @param other Candidate superset, or NULL as an empty set.
/// @return 1 if every element of @p obj occurs in @p other; otherwise 0.
int8_t rt_frozenset_is_subset(void *obj, void *other) {
    if (!obj)
        return 1; // empty set is subset of everything
    rt_frozenset_impl *a = as_frozenset(obj, "FrozenSet.IsSubset: invalid FrozenSet object");
    if (!a)
        return 0;
    if (!other)
        return a->count == 0 ? 1 : 0;
    rt_frozenset_impl *b = as_frozenset(other, "FrozenSet.IsSubset: invalid FrozenSet object");
    if (!b)
        return 0;

    for (int64_t i = 0; i < a->capacity; i++) {
        if (a->slots[i].key && !fs_contains(b, a->slots[i].key))
            return 0;
    }
    return 1;
}

/// @brief Compare two frozen sets for structural equality.
/// @details Two frozen sets are equal when they contain the same elements.
/// @param obj First FrozenSet, or NULL as an empty set.
/// @param other Second FrozenSet, or NULL as an empty set.
/// @return 1 if both contain identical byte-string elements; otherwise 0.
int8_t rt_frozenset_equals(void *obj, void *other) {
    int64_t la = rt_frozenset_len(obj);
    int64_t lb = rt_frozenset_len(other);
    if (la != lb)
        return 0;
    return rt_frozenset_is_subset(obj, other);
}
