//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_set.c
// Purpose: Implements a generic hash set supporting heterogeneous element types.
//   Uses content-aware hashing and equality: boxed integers, floats, booleans,
//   and strings are compared by value; non-boxed objects fall back to pointer
//   identity. Supports add, remove, contains, intersection, union, and
//   difference operations.
//
// Key invariants:
//   - Backed by a hash table with initial capacity SET_INITIAL_CAPACITY (16)
//     and separate chaining.
//   - Resizes (doubles) at 75% load factor (SET_LOAD_FACTOR 3/4).
//   - Hash dispatch: RT_ELEM_BOX elements use content hash (FNV-1a for strings,
//     bit-mix for integers/floats); all other elements use pointer address.
//   - Equality dispatch matches the hash dispatch to ensure correctness.
//   - Elements are retained on insertion and released on removal/finalization.
//   - Contains, add, and remove are O(1) average case; O(n) worst case.
//   - Set algebra (union, intersection, difference) iterates all buckets: O(n+m).
//   - Not thread-safe; external synchronization required for concurrent access.
//
// Ownership/Lifetime:
//   - Set objects are GC-managed (rt_obj_new_i64). The bucket array and all
//     entry nodes are freed by the GC finalizer (set_finalizer).
//
// Links: src/runtime/collections/rt_set.h (public API),
//        src/runtime/rt_box.h (boxed value type inspection)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime heterogeneous mutable hash Set.
/// @details Hashing and equality delegate to the shared boxed-value helpers so
///          boxed scalars/strings use value semantics and ordinary objects use
///          identity semantics. Each unique stored value is retained and
///          visited by the collector.

#include "rt_set.h"

#include "rt_box.h"
#include "rt_collection_ids.h"
#include "rt_collection_ownership.h"
#include "rt_gc.h"
#include "rt_object.h"
#include "rt_seq.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "rt_trap.h"

/// Initial number of buckets.
#define SET_INITIAL_CAPACITY 16

/// Load factor threshold for resizing (0.75 = 75%).
#define SET_LOAD_FACTOR_NUM 3
#define SET_LOAD_FACTOR_DEN 4

/// @brief Entry in the hash set (collision chain node).
typedef struct rt_set_entry {
    void *elem;                ///< Element pointer (retained).
    struct rt_set_entry *next; ///< Next entry in collision chain (or NULL).
} rt_set_entry;

/// @brief Set implementation structure.
typedef struct rt_set_impl {
    void **vptr;            ///< Vtable pointer placeholder (for OOP compatibility).
    rt_set_entry **buckets; ///< Array of bucket heads (collision chain pointers).
    size_t capacity;        ///< Number of buckets in the hash table.
    size_t count;           ///< Number of elements currently in the Set.
} rt_set_impl;

/// @brief Release one temporary runtime object reference.
static void set_release_temp(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copy the current trap diagnostic for cleanup and rethrow.
static void set_save_trap(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Checked cast of an opaque handle to the Set implementation;
///        traps with @p what if @p obj is NULL or not a Set.
/// @param obj Opaque runtime handle to validate.
/// @param what Diagnostic emitted by the trap subsystem on failure.
/// @return Validated Set implementation, or `NULL` after trapping.
static rt_set_impl *as_set(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_SET_CLASS_ID, sizeof(rt_set_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_set_impl *)obj;
}

/// @brief Find an entry in a bucket's collision chain using content equality.
/// @param head First collision-chain node, or `NULL`.
/// @param elem Candidate value; may be `NULL`.
/// @return Borrowed matching entry, or `NULL` when absent.
static rt_set_entry *find_entry(rt_set_entry *head, void *elem) {
    for (rt_set_entry *e = head; e; e = e->next) {
        if (rt_box_equal(e->elem, elem))
            return e;
    }
    return NULL;
}

/// @brief Return whether a validated Set has usable bucket storage.
static int set_storage_valid(const rt_set_impl *set) {
    return set && set->buckets && set->capacity > 0 &&
           set->capacity <= SIZE_MAX / sizeof(rt_set_entry *);
}

/// @brief Look up an element in a validated Set implementation.
static int set_contains_impl(const rt_set_impl *set, void *elem) {
    size_t index = rt_box_hash(elem) % set->capacity;
    return find_entry(set->buckets[index], elem) ? 1 : 0;
}

/// @brief Copy one retained element into a result, accepting a duplicate.
/// @return Nonzero when the result contains @p elem after the operation.
static int set_add_copy_checked(void *result, void *elem) {
    int64_t before = rt_set_len(result);
    if (rt_set_add(result, elem))
        return before >= 0 && before < INT64_MAX && rt_set_len(result) == before + 1;
    return rt_set_len(result) == before && rt_set_has(result, elem);
}

typedef enum set_build_op {
    SET_BUILD_COPY,
    SET_BUILD_UNION,
    SET_BUILD_INTERSECT,
    SET_BUILD_DIFF,
} set_build_op;

/// @brief Build one Set algebra result with all-or-nothing retained ownership.
static void *set_build_result(rt_set_impl *left,
                              rt_set_impl *right,
                              set_build_op op,
                              const char *diagnostic) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        set_save_trap(saved_error, sizeof(saved_error), diagnostic);
        rt_trap_clear_recovery();
        set_release_temp((void *)result);
        rt_trap(saved_error);
        return NULL;
    }

    result = rt_set_new();
    if (!result) {
        rt_trap_clear_recovery();
        return NULL;
    }

    rt_set_impl *scan = left;
    rt_set_impl *membership = right;
    if (op == SET_BUILD_INTERSECT) {
        if (!left || !right) {
            scan = NULL;
            membership = NULL;
        } else if (right->count < left->count) {
            scan = right;
            membership = left;
        }
    }

    if (scan) {
        for (size_t index = 0; index < scan->capacity; ++index) {
            for (rt_set_entry *entry = scan->buckets[index]; entry; entry = entry->next) {
                int include =
                    op == SET_BUILD_COPY || op == SET_BUILD_UNION ||
                    (op == SET_BUILD_INTERSECT && set_contains_impl(membership, entry->elem)) ||
                    (op == SET_BUILD_DIFF &&
                     (!membership || !set_contains_impl(membership, entry->elem)));
                if (include && !set_add_copy_checked((void *)result, entry->elem))
                    goto returning_failure;
            }
        }
    }

    if (op == SET_BUILD_UNION && right) {
        for (size_t index = 0; index < right->capacity; ++index) {
            for (rt_set_entry *entry = right->buckets[index]; entry; entry = entry->next) {
                if (!set_add_copy_checked((void *)result, entry->elem))
                    goto returning_failure;
            }
        }
    }

    rt_trap_clear_recovery();
    return (void *)result;

returning_failure: {
    char saved_error[256];
    set_save_trap(saved_error, sizeof(saved_error), diagnostic);
    rt_trap_clear_recovery();
    set_release_temp((void *)result);
    rt_trap(saved_error);
    return NULL;
}
}

/// @brief GC traversal: visit every stored element across all bucket chains.
/// @param obj Set whose retained values are to be traced.
/// @param visitor Collector callback invoked once per entry.
/// @param ctx Opaque collector context forwarded unchanged.
static void rt_set_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_set_impl *set = as_set(obj, "Set: invalid Set object");
    if (!set_storage_valid(set))
        return;
    for (size_t i = 0; i < set->capacity; ++i) {
        for (rt_set_entry *e = set->buckets[i]; e; e = e->next)
            visitor(e->elem, ctx);
    }
}

/// @brief Resize the hash table when load factor is exceeded.
/// @details Allocates a doubled bucket array before relinking entries and
///          publishing it. Entry/value ownership and count do not change.
/// @param set Set whose live bucket array has reached the load threshold.
/// @return 1 after publication, or 0 after an overflow/allocation trap.
static int resize_set(rt_set_impl *set) {
    if (set->capacity > SIZE_MAX / 2) {
        rt_trap("Set: capacity overflow");
        return 0;
    }
    size_t new_capacity = set->capacity * 2;
    if (new_capacity > SIZE_MAX / sizeof(rt_set_entry *)) {
        rt_trap("Set: allocation size overflow");
        return 0;
    }
    rt_set_entry **new_buckets = calloc(new_capacity, sizeof(rt_set_entry *));
    if (!new_buckets) {
        rt_trap("Set: memory allocation failed");
        return 0;
    }

    // Rehash all entries
    for (size_t i = 0; i < set->capacity; ++i) {
        rt_set_entry *e = set->buckets[i];
        while (e) {
            rt_set_entry *next = e->next;
            size_t new_idx = rt_box_hash(e->elem) % new_capacity;
            e->next = new_buckets[new_idx];
            new_buckets[new_idx] = e;
            e = next;
        }
    }

    free(set->buckets);
    set->buckets = new_buckets;
    set->capacity = new_capacity;
    return 1;
}

/// @brief Finalizer callback invoked when a Set is garbage collected.
/// @details Clears every retained element/entry, frees the bucket allocation,
///          and marks the implementation as finalized.
/// @param obj Set object being finalized; `NULL` is ignored.
static void rt_set_finalize(void *obj) {
    if (!obj)
        return;
    rt_set_impl *set = as_set(obj, "Set: invalid Set object");
    if (!set)
        return;
    if (!set_storage_valid(set))
        return;
    rt_set_clear(obj);
    free(set->buckets);
    set->buckets = NULL;
    set->capacity = 0;
    set->count = 0;
}

/// @brief Construct an empty mutable set. Internal storage is hashed buckets
/// with separate chaining;
/// resizes when load factor exceeds 0.75. Elements are reference-counted via Box hash/equality.
/// @return New runtime-managed Set, or `NULL` on allocation failure.
void *rt_set_new(void) {
    rt_set_impl *set = (rt_set_impl *)rt_obj_new_i64(RT_SET_CLASS_ID, (int64_t)sizeof(rt_set_impl));
    if (!set)
        return NULL;

    set->vptr = NULL; // Placeholder for OOP vtable
    set->capacity = SET_INITIAL_CAPACITY;
    set->count = 0;
    set->buckets = calloc(SET_INITIAL_CAPACITY, sizeof(rt_set_entry *));

    if (!set->buckets) {
        if (rt_obj_release_check0(set))
            rt_obj_free(set);
        rt_trap("Set.New: memory allocation failed");
        return NULL;
    }

    rt_obj_set_finalizer(set, rt_set_finalize);
    rt_gc_track(set, rt_set_traverse);
    return set;
}

/// @brief Number of elements currently in the set.
/// @param obj Set handle, or `NULL`.
/// @return Unique-element count, or 0 for `NULL`.
int64_t rt_set_len(void *obj) {
    if (!obj)
        return 0;
    rt_set_impl *set = as_set(obj, "Set: invalid Set object");
    if (!set || set->count > (size_t)INT64_MAX)
        return 0;
    return (int64_t)set->count;
}

/// @brief Returns 1 if the set has no elements (or for a NULL handle).
/// @param obj Set handle, or `NULL`.
/// @return 1 when empty, otherwise 0.
int8_t rt_set_is_empty(void *obj) {
    if (!obj)
        return 1;
    rt_set_impl *set = as_set(obj, "Set: invalid Set object");
    return set && set->count == 0 ? 1 : 0;
}

/// @brief Insert `elem` into the set. Element is retained on success. Returns 1 if newly added,
/// 0 if already present (the existing entry keeps its retain count). Triggers resize at high load.
/// @details Shared boxed hashing/equality makes equal boxed values idempotent
///          even when their pointers differ. `NULL` is a valid set element. A
///          null Set handle reports no insertion.
/// @param obj Set to mutate, or `NULL`.
/// @param elem Value to retain if not already equal to a stored element.
/// @return 1 when a new entry is published, otherwise 0.
int8_t rt_set_add(void *obj, void *elem) {
    if (!obj)
        return 0;
    rt_gc_mutator_enter();
    rt_set_impl *set = as_set(obj, "Set.Add: invalid Set object");
    if (!set || !set->buckets || set->capacity == 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t idx = rt_box_hash(elem) % set->capacity;
    if (find_entry(set->buckets[idx], elem)) {
        rt_gc_mutator_exit();
        return 0; // Already exists
    }

    if (set->count == SIZE_MAX || set->count >= (size_t)INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Set.Add: maximum size reached");
        return 0;
    }

    // Check load factor and resize if needed
    if ((long double)set->count * (long double)SET_LOAD_FACTOR_DEN >=
        (long double)set->capacity * (long double)SET_LOAD_FACTOR_NUM) {
        if (!resize_set(set)) {
            rt_gc_mutator_exit();
            return 0;
        }
    }

    idx = rt_box_hash(elem) % set->capacity;

    if (!rt_collection_retain_checked(elem, "Set.Add: value retain failed")) {
        rt_gc_mutator_exit();
        return 0;
    }

    // Create new entry
    rt_set_entry *entry = malloc(sizeof(rt_set_entry));
    if (!entry) {
        if (elem && rt_obj_release_check0(elem))
            rt_obj_free(elem);
        rt_gc_mutator_exit();
        rt_trap("Set.Add: memory allocation failed");
        return 0;
    }

    entry->elem = elem;
    entry->next = set->buckets[idx];
    set->buckets[idx] = entry;
    set->count++;

    rt_gc_mutator_exit();
    return 1;
}

/// @brief Remove `elem`. Releases the element. Returns 1 if removed, 0 if not present.
/// @details Removes the equal stored entry, which need not be pointer-identical
///          to @p elem when it is a boxed value.
/// @param obj Set to mutate, or `NULL`.
/// @param elem Value used for shared hash/equality lookup.
/// @return 1 if an entry was removed, otherwise 0.
int8_t rt_set_remove(void *obj, void *elem) {
    if (!obj)
        return 0;
    rt_gc_mutator_enter();
    rt_set_impl *set = as_set(obj, "Set.Remove: invalid Set object");
    if (!set || !set->buckets || set->capacity == 0) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t idx = rt_box_hash(elem) % set->capacity;

    rt_set_entry *prev = NULL;
    for (rt_set_entry *e = set->buckets[idx]; e; prev = e, e = e->next) {
        if (rt_box_equal(e->elem, elem)) {
            // Remove from chain
            if (prev)
                prev->next = e->next;
            else
                set->buckets[idx] = e->next;

            // Release and free
            if (e->elem && rt_obj_release_check0(e->elem))
                rt_obj_free(e->elem);
            free(e);
            set->count--;
            rt_gc_mutator_exit();
            return 1;
        }
    }

    rt_gc_mutator_exit();
    return 0;
}

/// @brief Returns 1 if `elem` is in the set, 0 otherwise. O(1) average.
/// @param obj Set handle, or `NULL`.
/// @param elem Value used for shared hash/equality lookup.
/// @return 1 if an equal element is stored, otherwise 0.
int8_t rt_set_has(void *obj, void *elem) {
    if (!obj)
        return 0;
    rt_set_impl *set = as_set(obj, "Set.Has: invalid Set object");
    if (!set || !set->buckets || set->capacity == 0)
        return 0;

    size_t idx = rt_box_hash(elem) % set->capacity;
    return find_entry(set->buckets[idx], elem) ? 1 : 0;
}

/// @brief Remove every element (releases each). Bucket array is preserved for re-use.
/// @param obj Set to clear, or `NULL` for a no-op.
void rt_set_clear(void *obj) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_set_impl *set = as_set(obj, "Set.Clear: invalid Set object");
    if (!set_storage_valid(set)) {
        rt_gc_mutator_exit();
        return;
    }

    for (size_t i = 0; i < set->capacity; ++i) {
        rt_set_entry *e = set->buckets[i];
        set->buckets[i] = NULL;
        while (e) {
            rt_set_entry *next = e->next;
            if (e->elem && rt_obj_release_check0(e->elem))
                rt_obj_free(e->elem);
            free(e);
            e = next;
        }
    }
    set->count = 0;
    rt_gc_mutator_exit();
}

/// @brief Return a Seq containing all elements in bucket-iteration order (not insertion order).
/// @details The fresh owning `Seq` independently retains every element.
///          Ordering may change after resize and is not a stable API contract.
///          A null Set returns an empty owning sequence.
/// @param obj Set handle, or `NULL`.
/// @return New runtime-managed owning Seq.
void *rt_set_items(void *obj) {
    if (!obj)
        return rt_seq_new_owned();

    rt_set_impl *set = as_set(obj, "Set.Items: invalid Set object");
    if (!set || !set->buckets || set->capacity == 0)
        return NULL;
    if (set->count > (size_t)INT64_MAX) {
        rt_trap("Set.Items: snapshot is too large");
        return NULL;
    }

    void *volatile seq = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        set_save_trap(saved_error, sizeof(saved_error), "Set.Items: snapshot failed");
        rt_trap_clear_recovery();
        set_release_temp((void *)seq);
        rt_trap(saved_error);
        return NULL;
    }

    seq = rt_seq_with_capacity_owned(set->count > 0 ? (int64_t)set->count : 1);
    if (!seq) {
        rt_trap_clear_recovery();
        return NULL;
    }
    for (size_t i = 0; i < set->capacity; ++i) {
        for (rt_set_entry *e = set->buckets[i]; e; e = e->next) {
            int64_t before = rt_seq_len((void *)seq);
            rt_seq_push((void *)seq, e->elem);
            if (before < 0 || before >= INT64_MAX || rt_seq_len((void *)seq) != before + 1) {
                char saved_error[256];
                set_save_trap(saved_error, sizeof(saved_error), "Set.Items: append failed");
                rt_trap_clear_recovery();
                set_release_temp((void *)seq);
                rt_trap(saved_error);
                return NULL;
            }
        }
    }

    rt_trap_clear_recovery();
    return (void *)seq;
}

/// @brief Return a fresh set containing every element from either operand. Result is mutable.
/// @details Null operands are treated as empty. The result independently
///          retains each unique element under shared equality.
/// @param obj First Set operand, or `NULL`.
/// @param other Second Set operand, or `NULL`.
/// @return New runtime-managed Set, or `NULL` on allocation failure.
void *rt_set_union(void *obj, void *other) {
    rt_set_impl *left = obj ? as_set(obj, "Set.Union: invalid Set object") : NULL;
    if (obj && !set_storage_valid(left))
        return NULL;
    rt_set_impl *right = other ? as_set(other, "Set.Union: invalid Set object") : NULL;
    if (other && !set_storage_valid(right))
        return NULL;
    return set_build_result(left, right, SET_BUILD_UNION, "Set.Union: construction failed");
}

/// @brief Return a fresh set containing only elements present in both operands.
/// @details A null operand denotes the empty set. Membership uses shared
///          boxed-value equality and retained result entries are independent.
/// @param obj First Set operand, or `NULL`.
/// @param other Second Set operand, or `NULL`.
/// @return New runtime-managed intersection Set.
void *rt_set_intersect(void *obj, void *other) {
    rt_set_impl *left = obj ? as_set(obj, "Set.Intersect: invalid Set object") : NULL;
    if (obj && !set_storage_valid(left))
        return NULL;
    rt_set_impl *right = other ? as_set(other, "Set.Intersect: invalid Set object") : NULL;
    if (other && !set_storage_valid(right))
        return NULL;
    return set_build_result(left, right, SET_BUILD_INTERSECT, "Set.Intersect: construction failed");
}

/// @brief Return a fresh set containing elements in `obj` but not in `other`.
/// @details A null first operand denotes empty; a null second operand causes a
///          retained shallow copy of the first.
/// @param obj Minuend Set, or `NULL`.
/// @param other Subtrahend Set, or `NULL`.
/// @return New runtime-managed difference Set.
void *rt_set_diff(void *obj, void *other) {
    rt_set_impl *left = obj ? as_set(obj, "Set.Diff: invalid Set object") : NULL;
    if (obj && !set_storage_valid(left))
        return NULL;
    rt_set_impl *right = other ? as_set(other, "Set.Diff: invalid Set object") : NULL;
    if (other && !set_storage_valid(right))
        return NULL;
    return set_build_result(left, right, SET_BUILD_DIFF, "Set.Diff: construction failed");
}

/// @brief Returns 1 if every element of `obj` is also in `other`. Empty set is subset of anything.
/// @details Null handles represent empty operands for this relation.
/// @param obj Candidate subset Set, or `NULL`.
/// @param other Candidate superset Set, or `NULL`.
/// @return 1 when the subset relation holds, otherwise 0.
int8_t rt_set_is_subset(void *obj, void *other) {
    rt_set_impl *left = obj ? as_set(obj, "Set.IsSubset: invalid Set object") : NULL;
    if (obj && !set_storage_valid(left))
        return 0;
    rt_set_impl *right = other ? as_set(other, "Set.IsSubset: invalid Set object") : NULL;
    if (other && !set_storage_valid(right))
        return 0;
    if (!left || left->count == 0)
        return 1;
    if (!right)
        return 0;
    for (size_t index = 0; index < left->capacity; ++index) {
        for (rt_set_entry *entry = left->buckets[index]; entry; entry = entry->next) {
            if (!set_contains_impl(right, entry->elem))
                return 0;
        }
    }
    return 1;
}

/// @brief Returns 1 if every element of `other` is also in `obj`. Inverse of `_is_subset`.
/// @param obj Candidate superset Set, or `NULL`.
/// @param other Candidate subset Set, or `NULL`.
/// @return 1 when the superset relation holds, otherwise 0.
int8_t rt_set_is_superset(void *obj, void *other) {
    return rt_set_is_subset(other, obj);
}

/// @brief Returns 1 if the two sets share no elements (intersection is empty).
/// @details A null operand represents an empty Set and is disjoint from every
///          valid Set.
/// @param obj First Set, or `NULL`.
/// @param other Second Set, or `NULL`.
/// @return 1 when there is no shared equal element, otherwise 0.
int8_t rt_set_is_disjoint(void *obj, void *other) {
    rt_set_impl *left = obj ? as_set(obj, "Set.IsDisjoint: invalid Set object") : NULL;
    if (obj && !set_storage_valid(left))
        return 0;
    rt_set_impl *right = other ? as_set(other, "Set.IsDisjoint: invalid Set object") : NULL;
    if (other && !set_storage_valid(right))
        return 0;
    if (!left || !right)
        return 1;

    rt_set_impl *scan = left->count <= right->count ? left : right;
    rt_set_impl *membership = scan == left ? right : left;
    for (size_t index = 0; index < scan->capacity; ++index) {
        for (rt_set_entry *entry = scan->buckets[index]; entry; entry = entry->next) {
            if (set_contains_impl(membership, entry->elem))
                return 0;
        }
    }
    return 1;
}

/// @brief Create a shallow copy of the set.
///
/// Allocates a new Set and adds all elements from the source set.
/// Elements are shared (not deep-copied) between the original and clone.
///
/// @param obj Source Set pointer (may be NULL).
/// @return New Set containing the same elements, or empty set if NULL.
/// @details The bucket/entry structure is independent and every element is
///          retained again, but element objects themselves are not cloned.
void *rt_set_clone(void *obj) {
    if (!obj)
        return rt_set_new();

    rt_set_impl *set = as_set(obj, "Set.Clone: invalid Set object");
    if (!set_storage_valid(set))
        return NULL;
    return set_build_result(set, NULL, SET_BUILD_COPY, "Set.Clone: construction failed");
}
