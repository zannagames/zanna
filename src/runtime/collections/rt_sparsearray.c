//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_sparsearray.c
// Purpose: Implements a sparse integer-indexed array using open-addressing
//   hash map semantics (int64_t key -> void* value). Only indices that have
//   been explicitly set occupy memory; unset indices return NULL. Suitable for
//   large, sparsely populated index spaces where most indices are absent.
//
// Key invariants:
//   - Open-addressing hash table with 64-bit mix hash on the integer key to
//     avoid clustering from sequential indices.
//   - Initial capacity is 16; probing is linear (index + i) modulo capacity.
//   - The table doubles before an insertion at or above 70% load. Capacity is
//     always a power of two
//     (required for bitmask indexing: slot = hash & (capacity - 1)).
//   - `occupied` distinguishes an unused slot from any signed integer key.
//   - Get returns NULL for missing indices; no error is raised.
//   - Set with NULL value removes the entry (slot becomes unoccupied).
//   - Tombstones are not used; removal backward-reinserts the following probe
//     cluster so early termination at an empty slot remains correct.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - SparseArray objects are GC-managed (rt_obj_new_i64). The slots array is
//     freed by the GC finalizer (sa_finalizer), which also releases retained
//     value references via rt_obj_release_check0.
//
// Links: src/runtime/collections/rt_sparsearray.h (public API),
//        src/runtime/collections/rt_intmap.h (similar integer-keyed map)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime sparse signed-index array.
/// @details Only non-null values occupy the power-of-two linear-probe table.
///          Stored values are retained and GC-traced; reads return borrowed
///          pointers, while index/value enumerations build owning snapshots in
///          current physical slot order.

#include "rt_sparsearray.h"

#include "rt_box.h"
#include "rt_collection_ids.h"
#include "rt_collection_ownership.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
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

// --- Open addressing hash map: int64_t -> void* ---

/// @brief One physical slot in the open-addressed table.
/// @details Null values are never stored by the public API; `occupied` exists
///          so every signed key, including zero, remains representable.
typedef struct {
    int64_t key;
    void *value;
    int8_t occupied;
} sa_slot;

/// @brief GC-managed SparseArray implementation payload.
typedef struct {
    void *vptr;
    int64_t count;
    int64_t capacity;
    sa_slot *slots;
} rt_sparse_impl;

/// @brief Checked cast of an opaque handle to the SparseArray implementation;
///        traps with @p what if @p obj is NULL or not a SparseArray.
/// @param obj Opaque runtime handle to validate.
/// @param what Diagnostic emitted by the trap subsystem on failure.
/// @return Validated SparseArray implementation, or `NULL` after trapping.
static rt_sparse_impl *as_sparse(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_SPARSEARRAY_CLASS_ID, sizeof(rt_sparse_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_sparse_impl *)obj;
}

// --- Hash function for int64 ---

/// @brief 64-bit integer mix hash (Murmur3 fmix64) for sparse-array keys.
/// @param key Arbitrary signed array index.
/// @return Mixed unsigned hash suitable for power-of-two masking.
static uint64_t sa_hash(int64_t key) {
    uint64_t k = (uint64_t)key;
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

// --- Internal helpers ---

/// @brief Drop one GC reference to a stored value and free it at zero.
/// @param value Runtime object reference, or `NULL` for a no-op.
static void sa_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Save an active trap diagnostic before clearing its recovery frame.
/// @param buffer Destination for the bounded diagnostic copy.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when no active diagnostic is available.
static void sa_save_trap(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Box and append one sparse index, consuming the snapshot on failure.
/// @param seq Partial owning index snapshot.
/// @param index Sparse index to box.
/// @return One after publication; zero after cleanup and a propagated trap.
static int sa_append_index_or_release_seq(void *seq, int64_t index) {
    void *volatile boxed = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        sa_save_trap(
            saved_error, sizeof(saved_error), "SparseArray.Indices: snapshot append failed");
        rt_trap_clear_recovery();
        sa_release_value((void *)boxed);
        sa_release_value(seq);
        rt_trap(saved_error);
        return 0;
    }

    boxed = rt_box_i64(index);
    if (!boxed)
        rt_trap("SparseArray.Indices: index allocation failed");
    rt_seq_push(seq, (void *)boxed);
    sa_release_value((void *)boxed);
    boxed = NULL;
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Append one retained value, consuming the snapshot on failure.
/// @param seq Partial owning value snapshot.
/// @param value Borrowed stored value.
/// @return One after publication; zero after cleanup and a propagated trap.
static int sa_append_value_or_release_seq(void *seq, void *value) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        sa_save_trap(
            saved_error, sizeof(saved_error), "SparseArray.Values: snapshot append failed");
        rt_trap_clear_recovery();
        sa_release_value(seq);
        rt_trap(saved_error);
        return 0;
    }

    rt_seq_push(seq, value);
    rt_trap_clear_recovery();
    return 1;
}

/// @brief GC finalizer: release every occupied slot's value, free the slots.
/// @param obj SparseArray object being finalized; `NULL` is ignored.
static void sa_finalizer(void *obj) {
    if (!obj)
        return;
    rt_sparse_impl *sa = as_sparse(obj, "SparseArray: invalid SparseArray object");
    if (!sa)
        return;
    if (sa->slots) {
        for (int64_t i = 0; i < sa->capacity; i++) {
            if (sa->slots[i].occupied) {
                void *value = sa->slots[i].value;
                sa->slots[i].occupied = 0;
                sa->slots[i].value = NULL;
                sa_release_value(value);
            }
        }
        free(sa->slots);
        sa->slots = NULL;
    }
    sa->capacity = 0;
    sa->count = 0;
}

/// @brief GC traversal: visit the value of every occupied slot.
/// @param obj SparseArray whose retained values are to be traced.
/// @param visitor Collector callback invoked in physical slot order.
/// @param ctx Opaque collector context forwarded unchanged.
static void sa_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_sparse_impl *sa = as_sparse(obj, "SparseArray: invalid SparseArray object");
    if (!sa || !sa->slots)
        return;
    for (int64_t i = 0; i < sa->capacity; i++) {
        if (sa->slots[i].occupied)
            visitor(sa->slots[i].value, ctx);
    }
}

/// @brief Double and rehash the power-of-two slot table.
/// @param sa SparseArray whose load threshold has been reached.
/// @return 1 after publishing the larger table, or 0 after trapping.
static int sa_grow(rt_sparse_impl *sa);

/// @brief Open-addressed (linear-probe) insert/update of @p key→@p value;
///        retains the value, releasing a replaced one. Assumes spare capacity.
/// @details Replacement retains before release, so assigning the identical
///          object is safe.
/// @param sa SparseArray with at least one reachable free slot.
/// @param key Signed integer index.
/// @param value Non-null runtime value to retain.
/// @return One after insertion/replacement; zero after a returning retain or
///         internal-capacity trap.
static int sa_insert_internal(rt_sparse_impl *sa, int64_t key, void *value) {
    uint64_t h = sa_hash(key);
    int64_t mask = sa->capacity - 1;
    int64_t idx = (int64_t)(h & (uint64_t)mask);

    for (int64_t i = 0; i < sa->capacity; i++) {
        int64_t slot = (idx + i) & mask;
        if (!sa->slots[slot].occupied) {
            if (!rt_collection_retain_checked(value, "SparseArray.Set: value retain failed"))
                return 0;
            sa->slots[slot].key = key;
            sa->slots[slot].value = value;
            sa->slots[slot].occupied = 1;
            sa->count++;
            return 1;
        }
        if (sa->slots[slot].key == key) {
            // Update value
            if (!rt_collection_retain_checked(value, "SparseArray.Set: value retain failed"))
                return 0;
            void *old_value = sa->slots[slot].value;
            sa->slots[slot].value = value;
            sa_release_value(old_value);
            return 1;
        }
    }
    rt_trap("SparseArray.Set: no insertion slot");
    return 0;
}

/// @brief Double the slot table and re-insert occupied entries (ownership
///        transferred, no extra retain). Traps on overflow/OOM.
/// @details Allocation succeeds before the implementation publishes the new
///          table. Rehashing only moves existing retained pointers; logical
///          count and ownership are preserved.
/// @param sa SparseArray to grow.
/// @return 1 after rehashing, or 0 after an overflow/allocation trap.
static int sa_grow(rt_sparse_impl *sa) {
    int64_t old_cap = sa->capacity;
    sa_slot *old_slots = sa->slots;

    if (old_cap > INT64_MAX / 2) {
        rt_trap("SparseArray: capacity overflow");
        return 0;
    }
    int64_t new_cap = old_cap * 2;
    if ((uint64_t)new_cap > SIZE_MAX / sizeof(sa_slot)) {
        rt_trap("SparseArray: allocation size overflow");
        return 0;
    }
    sa_slot *new_slots = (sa_slot *)calloc((size_t)new_cap, sizeof(sa_slot));
    if (!new_slots) {
        rt_trap("SparseArray: memory allocation failed");
        return 0;
    }
    sa->capacity = new_cap;
    sa->slots = new_slots;
    sa->count = 0;

    for (int64_t i = 0; i < old_cap; i++) {
        if (old_slots[i].occupied) {
            // Re-insert without retain (we already hold a ref)
            uint64_t h = sa_hash(old_slots[i].key);
            int64_t mask = sa->capacity - 1;
            int64_t idx = (int64_t)(h & (uint64_t)mask);

            for (int64_t j = 0; j < sa->capacity; j++) {
                int64_t slot = (idx + j) & mask;
                if (!sa->slots[slot].occupied) {
                    sa->slots[slot].key = old_slots[i].key;
                    sa->slots[slot].value = old_slots[i].value;
                    sa->slots[slot].occupied = 1;
                    sa->count++;
                    break;
                }
            }
        }
    }
    free(old_slots);
    return 1;
}

/// @brief Linear-probe lookup of @p key; returns its slot or NULL if absent.
/// @details Search terminates at the first unused slot, which is valid because
///          deletion closes probe-chain gaps by reinsertion.
/// @param sa SparseArray to search.
/// @param key Signed index to locate.
/// @return Borrowed occupied slot, or `NULL` when absent.
static sa_slot *sa_find(rt_sparse_impl *sa, int64_t key) {
    if (!sa || sa->count == 0)
        return NULL;
    uint64_t h = sa_hash(key);
    int64_t mask = sa->capacity - 1;
    int64_t idx = (int64_t)(h & (uint64_t)mask);

    for (int64_t i = 0; i < sa->capacity; i++) {
        int64_t slot = (idx + i) & mask;
        if (!sa->slots[slot].occupied)
            return NULL;
        if (sa->slots[slot].key == key)
            return &sa->slots[slot];
    }
    return NULL;
}

// --- Public API ---

/// @brief Construct a sparse array (int64 → value). Open-addressed hash with linear probing,
/// capacity grows past 70% load. Suitable when most indices in a logical range are unused
/// (e.g., entity IDs scattered across a wide ID space).
/// @return New runtime-managed SparseArray, or `NULL` after an allocation
///         trap.
void *rt_sparse_new(void) {
    rt_sparse_impl *sa =
        (rt_sparse_impl *)rt_obj_new_i64(RT_SPARSEARRAY_CLASS_ID, sizeof(rt_sparse_impl));
    if (!sa) {
        rt_trap("SparseArray: memory allocation failed");
        return NULL;
    }
    sa->count = 0;
    sa->capacity = 16;
    sa->slots = (sa_slot *)calloc(16, sizeof(sa_slot));
    if (!sa->slots) {
        if (rt_obj_release_check0(sa))
            rt_obj_free(sa);
        rt_trap("SparseArray: memory allocation failed");
        return NULL;
    }
    rt_obj_set_finalizer(sa, sa_finalizer);
    rt_gc_track(sa, sa_traverse);
    return (void *)sa;
}

/// @brief Return the number of populated entries in the sparse array.
/// @param obj SparseArray handle, or `NULL`.
/// @return Count of occupied non-null entries, or 0 for `NULL`.
int64_t rt_sparse_len(void *obj) {
    if (!obj)
        return 0;
    rt_sparse_impl *sparse = as_sparse(obj, "SparseArray.Len: invalid SparseArray object");
    return sparse ? sparse->count : 0;
}

/// @brief Read the value stored at `index`, or NULL if absent.
/// @details Creates no retain; the returned value remains owned by the
///          SparseArray and is invalidated by replacement/removal/clear.
/// @param obj SparseArray handle, or `NULL`.
/// @param index Arbitrary signed index.
/// @return Borrowed stored value, or `NULL` when absent.
void *rt_sparse_get(void *obj, int64_t index) {
    if (!obj)
        return NULL;
    sa_slot *s = sa_find(as_sparse(obj, "SparseArray.Get: invalid SparseArray object"), index);
    return s ? s->value : NULL;
}

/// @brief Set the value at a sparse index, growing the table if needed.
/// @details Uses open addressing with linear probing. Grows the table when
///          load factor exceeds 70% to maintain O(1) amortized access.
///          A non-null value is retained; replacing a value releases the old
///          one. Passing `NULL` removes @p index.
/// @param obj SparseArray handle, or `NULL` for a no-op.
/// @param index Sparse key to store at.
/// @param value Value to retain, or `NULL` to remove the entry.
void rt_sparse_set(void *obj, int64_t index, void *value) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_sparse_impl *sa = as_sparse(obj, "SparseArray.Set: invalid SparseArray object");
    if (!sa) {
        rt_gc_mutator_exit();
        return;
    }

    if (!value) {
        rt_sparse_remove(obj, index);
        rt_gc_mutator_exit();
        return;
    }

    // Check load factor (> 70%)
    if ((long double)sa->count * 10.0L >= (long double)sa->capacity * 7.0L && !sa_grow(sa)) {
        rt_gc_mutator_exit();
        return;
    }

    if (!sa_insert_internal(sa, index, value)) {
        rt_gc_mutator_exit();
        return;
    }
    rt_gc_mutator_exit();
}

/// @brief Check whether a value exists at the given sparse index.
/// @param obj SparseArray handle, or `NULL`.
/// @param index Sparse key to look up.
/// @return 1 if the index is occupied, 0 otherwise.
int8_t rt_sparse_has(void *obj, int64_t index) {
    if (!obj)
        return 0;
    return sa_find(as_sparse(obj, "SparseArray.Has: invalid SparseArray object"), index) != NULL
               ? 1
               : 0;
}

/// @brief Remove the value at a sparse index, rehashing displaced neighbors.
/// @details Uses backward-shift deletion: after clearing the slot, any
///          subsequent occupied slots that were displaced by probing are
///          re-inserted to maintain correct lookup behavior.
/// @param obj Sparse array object pointer; returns 0 if NULL.
/// @param index Sparse key to remove.
/// @return 1 if the entry was found and removed, 0 otherwise.
int8_t rt_sparse_remove(void *obj, int64_t index) {
    if (!obj)
        return 0;
    rt_gc_mutator_enter();
    rt_sparse_impl *sa = as_sparse(obj, "SparseArray.Remove: invalid SparseArray object");
    sa_slot *s = sa_find(sa, index);
    if (!s) {
        rt_gc_mutator_exit();
        return 0;
    }

    void *removed_value = s->value;
    s->value = NULL;
    s->occupied = 0;
    sa->count--;

    // Rehash subsequent entries that may have been displaced
    int64_t mask = sa->capacity - 1;
    int64_t pos = (int64_t)(s - sa->slots);
    int64_t next = (pos + 1) & mask;

    while (sa->slots[next].occupied) {
        sa_slot tmp = sa->slots[next];
        sa->slots[next].occupied = 0;
        sa->count--;
        // Re-insert without ref counting (already held)
        uint64_t h = sa_hash(tmp.key);
        int64_t idx = (int64_t)(h & (uint64_t)mask);
        for (int64_t i = 0; i < sa->capacity; i++) {
            int64_t slot = (idx + i) & mask;
            if (!sa->slots[slot].occupied) {
                sa->slots[slot] = tmp;
                sa->count++;
                break;
            }
        }
        next = (next + 1) & mask;
    }

    sa_release_value(removed_value);
    rt_gc_mutator_exit();
    return 1;
}

/// @brief Return a Seq of every populated index as boxed i64 values.
/// Slot-iteration order, not insertion order. Snapshot at call time.
/// @details The owning result retains each fresh boxed index. Ordering may
///          change after growth or deletion.
/// @param obj SparseArray handle, or `NULL`.
/// @return New runtime-managed owning Seq of boxed indices.
void *rt_sparse_indices(void *obj) {
    if (!obj)
        return rt_seq_new_owned();
    rt_sparse_impl *sa = as_sparse(obj, "SparseArray.Indices: invalid SparseArray object");
    if (!sa || !sa->slots || sa->capacity <= 0)
        return NULL;
    void *seq = rt_seq_with_capacity_owned(sa->count > 0 ? sa->count : 1);
    if (!seq)
        return NULL;
    for (int64_t i = 0; i < sa->capacity; i++) {
        if (sa->slots[i].occupied) {
            if (!sa_append_index_or_release_seq(seq, sa->slots[i].key))
                return NULL;
        }
    }
    return seq;
}

/// @brief Return a Seq of every stored value (parallel to `_indices` order).
/// @details The owning snapshot independently retains all values. A values
///          snapshot and indices snapshot correspond positionally only when
///          no intervening mutation changes the table.
/// @param obj SparseArray handle, or `NULL`.
/// @return New runtime-managed owning Seq of values.
void *rt_sparse_values(void *obj) {
    if (!obj)
        return rt_seq_new_owned();
    rt_sparse_impl *sa = as_sparse(obj, "SparseArray.Values: invalid SparseArray object");
    if (!sa || !sa->slots || sa->capacity <= 0)
        return NULL;
    void *seq = rt_seq_with_capacity_owned(sa->count > 0 ? sa->count : 1);
    if (!seq)
        return NULL;
    for (int64_t i = 0; i < sa->capacity; i++) {
        if (sa->slots[i].occupied && !sa_append_value_or_release_seq(seq, sa->slots[i].value))
            return NULL;
    }
    return seq;
}

/// @brief Remove all entries from the sparse array.
/// @details Releases all value references and marks all slots as unoccupied
///          while preserving the current capacity for reuse.
/// @param obj SparseArray handle, or `NULL` for a no-op.
void rt_sparse_clear(void *obj) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_sparse_impl *sa = as_sparse(obj, "SparseArray.Clear: invalid SparseArray object");
    if (!sa) {
        rt_gc_mutator_exit();
        return;
    }
    for (int64_t i = 0; i < sa->capacity; i++) {
        if (sa->slots[i].occupied) {
            void *value = sa->slots[i].value;
            sa->slots[i].occupied = 0;
            sa->slots[i].value = NULL;
            sa_release_value(value);
        }
    }
    sa->count = 0;
    rt_gc_mutator_exit();
}
