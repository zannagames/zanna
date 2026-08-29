//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_iter.c
// Purpose: Implements a unified stateful iterator that works across all Zanna
//   collection types (Seq, List, Ring, Deque, Map, Set, Stack). Iterators wrap
//   a collection pointer and a current position index; Next() advances the
//   position and returns the element, HasNext() checks bounds.
//
// Key invariants:
//   - For indexed GC-managed collections (Seq, List, Ring), the iterator retains
//     a reference to the source collection and iterates by index directly.
//   - For unindexed or malloc-managed collections (Deque, Map, Set, Stack), the
//     iterator snapshots the collection into a Seq at creation time and iterates
//     the snapshot. This means mutations to the source after iterator creation
//     are NOT visible.
//   - The `len` field is cached at creation from the source length; it does not
//     update if the source is mutated after the iterator is created.
//   - Calling Next() when HasNext() returns 0 returns NULL and does not advance
//     past the end (pos stays at len).
//   - The iterator holds a retained reference to the source/snapshot Seq; the
//     finalizer (iter_finalizer) releases it when the iterator is collected.
//
// Ownership/Lifetime:
//   - Iterator objects are GC-managed (rt_obj_new_i64). The iter_finalizer
//     releases the source/snapshot reference when the iterator is collected.
//
// Links: src/runtime/collections/rt_iter.h (public API),
//        src/runtime/collections/rt_seq.h, rt_list.h, rt_ring.h, rt_deque.h,
//        rt_map.h, rt_set.h, rt_stack.h (iterable collections)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a unified stateful iterator over runtime collections.
///
/// Seq, List, and Ring iterators retain their sources and dispatch indexed
/// reads directly, while Deque, Map, Set, and Stack factories capture owning
/// Seq snapshots. Every iterator caches its traversal length at construction.
/// Snapshot iterators are isolated from later source mutation; structurally
/// mutating a live source during iteration is unsupported.
///
/// Next and peek return retained references owned by the caller. Exhaustion is
/// represented by NULL, so callers iterating collections that may contain NULL
/// should use `rt_iter_has_next()` to distinguish a present null value.
/// Iterator objects are runtime-managed and expose their retained source to GC.

#include "rt_iter.h"

#include "rt_array_obj.h"
#include "rt_collection_ids.h"
#include "rt_collection_ownership.h"
#include "rt_deque.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_list_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_ring.h"
#include "rt_ring_internal.h"
#include "rt_seq.h"
#include "rt_seq_internal.h"
#include "rt_set.h"
#include "rt_stack.h"
#include "rt_trap.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Identifies the indexed accessor used for an iterator's source.
typedef enum {
    ITER_SEQ,
    ITER_LIST,
    ITER_RING,
    ITER_SNAPSHOT ///< Backed by a captured Seq snapshot (for Deque, Map, Set, Stack)
} iter_kind;

/// @brief Internal iterator state with a retained live source or owned snapshot.
typedef struct {
    void *vptr;
    void *source; ///< Retained reference to the original collection or snapshot Seq
    iter_kind kind;
    int64_t pos; ///< Current position (next element to return)
    int64_t len; ///< Cached length at creation time
} rt_iter_impl;

/// @brief Checked cast of an opaque handle to the Iterator implementation.
/// @details Traps with @p what if @p obj is NULL or not an Iterator.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used on validation failure.
/// @return The validated iterator pointer, or NULL if a returning trap handler
///         resumes after failed validation.
static rt_iter_impl *as_iter(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_ITERATOR_CLASS_ID, sizeof(rt_iter_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_iter_impl *)obj;
}

/// @brief Drop one GC reference to a transient @p obj and free it at zero.
/// @param obj Temporary retained runtime object, or NULL for a no-op.
static void release_temp_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Save the current trap diagnostic before removing local recovery.
/// @param buffer Caller-owned destination.
/// @param buffer_size Capacity including the terminator.
/// @param fallback Text used when no trap message is available.
static void iter_save_trap(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief GC finalizer: release the iterator's retained source collection.
/// @param obj Iterator object being finalized, or NULL for a no-op.
static void iter_finalizer(void *obj) {
    rt_iter_impl *it = obj ? as_iter(obj, "Iterator: invalid Iterator object") : NULL;
    if (it && it->source) {
        void *source = it->source;
        it->source = NULL;
        if (rt_obj_release_check0(source))
            rt_obj_free(source);
    }
}

/// @brief GC traversal: expose the retained source edge so iterator/source
///        reference cycles are visible to the cycle collector (VDOC-094).
/// @param obj Iterator object to traverse.
/// @param visitor Runtime callback invoked for the retained source.
/// @param ctx Opaque visitor context forwarded unchanged.
static void iter_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_iter_impl *it = (rt_iter_impl *)obj;
    if (it->source)
        visitor(it->source, ctx);
}

/// @brief Creates an iterator that retains a live indexed source.
/// @param source Non-null runtime-managed collection.
/// @param kind Indexed accessor to use for @p source.
/// @param len Cached traversal length.
/// @return New runtime-managed iterator, or NULL for a null source or after an
///         allocation trap.
static rt_iter_impl *make_iter(void *source, iter_kind kind, int64_t len) {
    if (!source)
        return NULL;

    rt_iter_impl *volatile it = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        iter_save_trap(saved_error, sizeof(saved_error), "Iterator: construction failed");
        rt_trap_clear_recovery();
        release_temp_obj((void *)it);
        rt_trap(saved_error);
        return NULL;
    }

    it = (rt_iter_impl *)rt_obj_new_i64(RT_ITERATOR_CLASS_ID, (int64_t)sizeof(rt_iter_impl));
    if (!it) {
        rt_trap_clear_recovery();
        rt_trap("Iterator: allocation failed");
        return NULL;
    }
    ((rt_iter_impl *)it)->vptr = NULL;
    ((rt_iter_impl *)it)->source = NULL;
    ((rt_iter_impl *)it)->kind = kind;
    ((rt_iter_impl *)it)->pos = 0;
    ((rt_iter_impl *)it)->len = len;
    rt_obj_set_finalizer((void *)it, iter_finalizer);
    rt_gc_track((void *)it, iter_traverse);
    if (!rt_collection_retain_checked(source, "Iterator: source retain failed")) {
        rt_trap_clear_recovery();
        release_temp_obj((void *)it);
        return NULL;
    }
    ((rt_iter_impl *)it)->source = source;
    rt_trap_clear_recovery();
    return (rt_iter_impl *)it;
}

/// @brief Creates an iterator that takes ownership of a Seq snapshot.
/// @param snapshot Non-null newly created Seq whose creation reference transfers
///        to the iterator.
/// @param len Cached number of snapshot elements.
/// @return New runtime-managed iterator, or NULL after releasing an unusable
///         snapshot.
static rt_iter_impl *make_iter_snapshot(void *snapshot, int64_t len) {
    if (!snapshot)
        return NULL;

    rt_iter_impl *volatile it = NULL;
    void *volatile owned_snapshot = snapshot;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        iter_save_trap(saved_error, sizeof(saved_error), "Iterator: construction failed");
        rt_trap_clear_recovery();
        release_temp_obj((void *)it);
        release_temp_obj((void *)owned_snapshot);
        rt_trap(saved_error);
        return NULL;
    }

    it = (rt_iter_impl *)rt_obj_new_i64(RT_ITERATOR_CLASS_ID, (int64_t)sizeof(rt_iter_impl));
    if (!it) {
        rt_trap_clear_recovery();
        release_temp_obj((void *)owned_snapshot);
        rt_trap("Iterator: allocation failed");
        return NULL;
    }
    ((rt_iter_impl *)it)->vptr = NULL;
    ((rt_iter_impl *)it)->source = NULL;
    ((rt_iter_impl *)it)->kind = ITER_SNAPSHOT;
    ((rt_iter_impl *)it)->pos = 0;
    ((rt_iter_impl *)it)->len = len;
    rt_obj_set_finalizer((void *)it, iter_finalizer);
    rt_gc_track((void *)it, iter_traverse);
    ((rt_iter_impl *)it)->source = (void *)owned_snapshot;
    owned_snapshot = NULL;
    rt_trap_clear_recovery();
    return (rt_iter_impl *)it;
}

//=============================================================================
// Factory functions
//
// Each `_from_*` builds an iterator over an existing collection. SEQ, LIST,
// and RING support live (in-place) iteration — the iterator stores the
// collection by reference. DEQUE, MAP, SET, and STACK are *snapshotted* into
// a fresh Seq because their underlying storage either isn't GC-managed or
// doesn't support indexed access; the snapshot freezes the state at iter
// creation time.
//=============================================================================

/// @brief Build a live iterator over a Seq.
/// @param seq Non-null Seq to retain.
/// @return New iterator with the Seq's current length cached, or NULL.
/// @note Element replacements are observed, but structural mutation during
///       iteration is unsupported.
void *rt_iter_from_seq(void *seq) {
    if (!seq)
        return NULL;
    if (!rt_seq_internal_is_valid(seq)) {
        rt_trap("Iterator.FromSeq: invalid Seq object");
        return NULL;
    }
    return make_iter(seq, ITER_SEQ, rt_seq_len(seq));
}

/// @brief Build a live iterator over a List.
/// @param list Non-null List to retain.
/// @return New iterator with the List's current length cached, or NULL.
/// @note Structural mutation during iteration is unsupported.
void *rt_iter_from_list(void *list) {
    if (!list)
        return NULL;
    if (!rt_list_internal_is_valid(list)) {
        rt_trap("Iterator.FromList: invalid List object");
        return NULL;
    }
    return make_iter(list, ITER_LIST, rt_list_len(list));
}

/// @brief Snapshot a Deque into an iterator. Later mutations of the deque are NOT seen.
/// @param deque Non-null Deque to snapshot in front-to-back order.
/// @return New snapshot iterator, or NULL if snapshot construction fails.
void *rt_iter_from_deque(void *deque) {
    if (!deque)
        return NULL;

    void *volatile clone = NULL;
    void *volatile snapshot = NULL;
    void *volatile transferred = NULL;
    int64_t len = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        iter_save_trap(saved_error, sizeof(saved_error), "Iterator: deque snapshot failed");
        rt_trap_clear_recovery();
        release_temp_obj((void *)transferred);
        release_temp_obj((void *)snapshot);
        release_temp_obj((void *)clone);
        rt_trap(saved_error);
        return NULL;
    }

    clone = rt_deque_clone(deque);
    if (!clone) {
        rt_trap_clear_recovery();
        return NULL;
    }
    len = rt_deque_len((void *)clone);
    snapshot = rt_seq_with_capacity_owned(len > 0 ? len : 1);
    if (!snapshot) {
        rt_trap_clear_recovery();
        release_temp_obj((void *)clone);
        return NULL;
    }
    while (rt_deque_len((void *)clone) > 0) {
        transferred = rt_deque_pop_front((void *)clone);
        rt_seq_push_raw((void *)snapshot, (void *)transferred);
        transferred = NULL;
    }
    release_temp_obj((void *)clone);
    clone = NULL;
    rt_trap_clear_recovery();

    void *owned_snapshot = (void *)snapshot;
    snapshot = NULL;
    return make_iter_snapshot(owned_snapshot, len);
}

/// @brief Build a live iterator over a Ring.
/// @param ring Non-null Ring to retain.
/// @return New iterator with the Ring's current length cached, or NULL.
/// @note Structural mutation during iteration is unsupported.
void *rt_iter_from_ring(void *ring) {
    if (!ring)
        return NULL;
    if (!rt_ring_internal_is_valid(ring)) {
        rt_trap("Iterator.FromRing: invalid Ring object");
        return NULL;
    }
    return make_iter(ring, ITER_RING, rt_ring_len(ring));
}

/// @brief Snapshot the keys of a Map into an iterator (insertion order).
/// @param map Non-null Map to snapshot.
/// @return New snapshot iterator over retained key strings, or NULL.
void *rt_iter_from_map_keys(void *map) {
    void *keys;
    if (!map)
        return NULL;
    keys = rt_map_keys(map);
    if (!keys)
        return NULL;
    return make_iter_snapshot(keys, rt_seq_len(keys));
}

/// @brief Snapshot the values of a Map into an iterator (insertion order).
/// @param map Non-null Map to snapshot.
/// @return New snapshot iterator over retained values, or NULL.
void *rt_iter_from_map_values(void *map) {
    void *values;
    if (!map)
        return NULL;
    values = rt_map_values(map);
    if (!values)
        return NULL;
    return make_iter_snapshot(values, rt_seq_len(values));
}

/// @brief Snapshot a Set into an iterator. Order is the set's internal hashing order — not
/// guaranteed stable across versions. Mutations of the source set after iter creation are not seen.
/// @param set Non-null Set to snapshot.
/// @return New snapshot iterator, or NULL if snapshot construction fails.
void *rt_iter_from_set(void *set) {
    void *items;
    if (!set)
        return NULL;
    items = rt_set_items(set);
    if (!items)
        return NULL;
    return make_iter_snapshot(items, rt_seq_len(items));
}

/// @brief Snapshot a Stack into bottom-to-top iteration order without changing the source.
/// @param stack Non-null Stack to snapshot.
/// @return New snapshot iterator, or NULL after a size or allocation failure.
void *rt_iter_from_stack(void *stack) {
    if (!stack)
        return NULL;

    void *volatile clone = NULL;
    void *volatile snapshot = NULL;
    void **volatile items = NULL;
    void *volatile transferred = NULL;
    volatile int transferred_owned = 0;
    volatile int clone_owns_elements = 0;
    int64_t len = 0;
    volatile int64_t count = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        iter_save_trap(saved_error, sizeof(saved_error), "Iterator: stack snapshot failed");
        rt_trap_clear_recovery();
        if (transferred_owned)
            release_temp_obj((void *)transferred);
        if (clone_owns_elements && items) {
            for (int64_t i = 0; i < count; ++i)
                release_temp_obj(((void **)items)[i]);
        }
        free((void *)items);
        release_temp_obj((void *)snapshot);
        release_temp_obj((void *)clone);
        rt_trap(saved_error);
        return NULL;
    }

    clone = rt_stack_clone(stack);
    if (!clone) {
        rt_trap_clear_recovery();
        return NULL;
    }
    clone_owns_elements = rt_stack_owns_elements((void *)clone) ? 1 : 0;
    len = rt_stack_len((void *)clone);
    snapshot = rt_seq_with_capacity_owned(len > 0 ? len : 1);
    if (!snapshot) {
        rt_trap_clear_recovery();
        release_temp_obj((void *)clone);
        return NULL;
    }
    if ((uint64_t)len > SIZE_MAX / sizeof(void *)) {
        rt_trap("Iterator: stack snapshot too large");
        rt_trap_clear_recovery();
        release_temp_obj((void *)snapshot);
        release_temp_obj((void *)clone);
        return NULL;
    }
    if (len > 0) {
        items = (void **)calloc((size_t)len, sizeof(void *));
        if (!items) {
            rt_trap("Iterator: allocation failed");
            rt_trap_clear_recovery();
            release_temp_obj((void *)snapshot);
            release_temp_obj((void *)clone);
            return NULL;
        }
    }

    while (count < len) {
        ((void **)items)[count] = rt_stack_pop((void *)clone);
        count++;
    }
    for (int64_t i = count; i > 0; --i) {
        transferred = ((void **)items)[i - 1];
        ((void **)items)[i - 1] = NULL;
        transferred_owned = clone_owns_elements;
        if (!clone_owns_elements &&
            !rt_collection_retain_checked((void *)transferred,
                                          "Iterator: stack element retain failed")) {
            rt_trap_clear_recovery();
            if (clone_owns_elements) {
                for (int64_t j = 0; j < i - 1; ++j)
                    release_temp_obj(((void **)items)[j]);
            }
            free((void *)items);
            release_temp_obj((void *)snapshot);
            release_temp_obj((void *)clone);
            return NULL;
        }
        transferred_owned = 1;
        rt_seq_push_raw((void *)snapshot, (void *)transferred);
        transferred = NULL;
        transferred_owned = 0;
    }

    free((void *)items);
    items = NULL;
    release_temp_obj((void *)clone);
    clone = NULL;
    rt_trap_clear_recovery();

    void *owned_snapshot = (void *)snapshot;
    snapshot = NULL;
    return make_iter_snapshot(owned_snapshot, len);
}

//=============================================================================
// Core iteration
//=============================================================================

/// @brief Validate an iterator's cached cursor bounds.
/// @param it Iterator payload to inspect.
/// @return Nonzero when `0 <= pos <= len`; otherwise zero after trapping.
static int iter_cursor_valid(const rt_iter_impl *it) {
    if (!it || it->pos < 0 || it->len < 0 || it->pos > it->len) {
        rt_trap("Iterator: corrupted cursor state");
        return 0;
    }
    return 1;
}

/// @brief Fetch a borrowed element from the iterator's retained source.
/// @details Uses private sized layouts so a legitimate null slot remains
///          distinguishable from source corruption or unsupported structural
///          mutation. The caller decides whether and when to retain the result.
/// @param it Iterator whose source is accessed.
/// @param idx Zero-based source index.
/// @param out Receives the borrowed element, including a legitimate null.
/// @return Nonzero on success; zero after trapping.
static int iter_get_borrowed(rt_iter_impl *it, int64_t idx, void **out) {
    if (out)
        *out = NULL;
    if (!it || !out || idx < 0) {
        rt_trap("Iterator: invalid indexed access");
        return 0;
    }

    switch (it->kind) {
        case ITER_SEQ:
        case ITER_SNAPSHOT: {
            if (!rt_seq_internal_is_valid(it->source)) {
                rt_trap("Iterator: invalid Seq source");
                return 0;
            }
            rt_seq_impl *seq = (rt_seq_impl *)it->source;
            if (idx >= seq->len || !seq->items) {
                rt_trap("Iterator: Seq source was structurally modified");
                return 0;
            }
            *out = seq->items[idx];
            return 1;
        }
        case ITER_LIST: {
            if (!rt_list_internal_is_valid(it->source)) {
                rt_trap("Iterator: invalid List source");
                return 0;
            }
            rt_list_impl *list = (rt_list_impl *)it->source;
            size_t current_len = rt_arr_obj_len(list->arr);
            if ((uint64_t)idx >= (uint64_t)current_len || !list->arr) {
                rt_trap("Iterator: List source was structurally modified");
                return 0;
            }
            *out = list->arr[(size_t)idx];
            return 1;
        }
        case ITER_RING: {
            if (!rt_ring_internal_is_valid(it->source)) {
                rt_trap("Iterator: invalid Ring source");
                return 0;
            }
            rt_ring_impl *ring = (rt_ring_impl *)it->source;
            if ((uint64_t)idx >= (uint64_t)ring->count || !ring->items || ring->capacity == 0 ||
                ring->count > ring->capacity || ring->head >= ring->capacity) {
                rt_trap("Iterator: Ring source was structurally modified");
                return 0;
            }
            size_t logical = (size_t)idx;
            size_t until_wrap = ring->capacity - ring->head;
            size_t physical = logical < until_wrap ? ring->head + logical : logical - until_wrap;
            *out = ring->items[physical];
            return 1;
        }
    }
    rt_trap("Iterator: invalid source kind");
    return 0;
}

/// @brief Check whether the iterator has more elements.
/// @details Returns true if the current position is before the end.
/// @param iter Iterator handle, or NULL.
/// @return 1 when another cached position remains; otherwise 0.
int8_t rt_iter_has_next(void *iter) {
    if (!iter)
        return 0;
    rt_iter_impl *it = as_iter(iter, "Iterator.HasNext: invalid Iterator object");
    if (!it || !iter_cursor_valid(it))
        return 0;
    return (it->pos < it->len) ? 1 : 0;
}

/// @brief Return the current element and advance the cursor. Returns NULL when exhausted.
/// Pair with `_has_next` to drive while-loop iteration.
/// @param iter Iterator handle, or NULL.
/// @return Current element as a retained caller-owned reference, or NULL when
///         exhausted. The cursor advances by one on success.
void *rt_iter_next(void *iter) {
    if (!iter)
        return NULL;
    rt_iter_impl *it = as_iter(iter, "Iterator.Next: invalid Iterator object");
    if (!it || !iter_cursor_valid(it))
        return NULL;
    if (it->pos >= it->len)
        return NULL;

    void *elem = NULL;
    if (!iter_get_borrowed(it, it->pos, &elem))
        return NULL;
    if (!rt_collection_retain_checked(elem, "Iterator.Next: result retain failed"))
        return NULL;
    it->pos++;
    return elem;
}

/// @brief Look at the current element without advancing the cursor.
/// @param iter Iterator handle, or NULL.
/// @return Current element as a retained caller-owned reference, or NULL when
///         exhausted.
void *rt_iter_peek(void *iter) {
    if (!iter)
        return NULL;
    rt_iter_impl *it = as_iter(iter, "Iterator.Peek: invalid Iterator object");
    if (!it || !iter_cursor_valid(it))
        return NULL;
    if (it->pos >= it->len)
        return NULL;

    void *elem = NULL;
    if (!iter_get_borrowed(it, it->pos, &elem))
        return NULL;
    if (!rt_collection_retain_checked(elem, "Iterator.Peek: result retain failed"))
        return NULL;
    return elem;
}

/// @brief Reset the iterator to the beginning of the collection.
/// @param iter Iterator handle, or NULL for a no-op.
void rt_iter_reset(void *iter) {
    if (!iter)
        return;
    rt_iter_impl *it = as_iter(iter, "Iterator.Reset: invalid Iterator object");
    if (it)
        it->pos = 0;
}

/// @brief Return the current position (0-based index) of the iterator.
/// @param iter Iterator object pointer; returns 0 if NULL.
/// @return Current index within the underlying collection.
int64_t rt_iter_index(void *iter) {
    if (!iter)
        return 0;
    rt_iter_impl *it = as_iter(iter, "Iterator.Index: invalid Iterator object");
    return it ? it->pos : 0;
}

/// @brief Return the total number of elements in the iterable collection.
/// @param iter Iterator handle, or NULL.
/// @return Length cached when the iterator was created, or zero for NULL.
int64_t rt_iter_count(void *iter) {
    if (!iter)
        return 0;
    rt_iter_impl *it = as_iter(iter, "Iterator.Count: invalid Iterator object");
    return it ? it->len : 0;
}

/// @brief Drain the remaining iterator elements into a fresh Seq. Advances the cursor to end.
/// @param iter Iterator handle, or NULL.
/// @return A new owning Seq containing elements from the current position to
///         the cached end. NULL produces an empty Seq.
void *rt_iter_to_seq(void *iter) {
    if (!iter)
        return rt_seq_with_capacity_owned(1);

    rt_iter_impl *it = as_iter(iter, "Iterator.ToSeq: invalid Iterator object");
    if (!it || !iter_cursor_valid(it))
        return NULL;

    int64_t start = it->pos;
    int64_t remaining = it->len - start;
    void *volatile seq = NULL;
    void *volatile retained = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        iter_save_trap(saved_error, sizeof(saved_error), "Iterator.ToSeq: snapshot failed");
        rt_trap_clear_recovery();
        release_temp_obj((void *)retained);
        release_temp_obj((void *)seq);
        it->pos = start;
        rt_trap(saved_error);
        return NULL;
    }

    seq = rt_seq_with_capacity_owned(remaining > 0 ? remaining : 1);
    if (!seq) {
        rt_trap_clear_recovery();
        return NULL;
    }
    while (it->pos < it->len) {
        void *borrowed = NULL;
        if (!iter_get_borrowed(it, it->pos, &borrowed)) {
            rt_trap_clear_recovery();
            release_temp_obj((void *)seq);
            it->pos = start;
            return NULL;
        }
        if (!rt_collection_retain_checked(borrowed, "Iterator.ToSeq: element retain failed")) {
            rt_trap_clear_recovery();
            release_temp_obj((void *)seq);
            it->pos = start;
            return NULL;
        }
        retained = borrowed;
        rt_seq_push_raw((void *)seq, (void *)retained);
        retained = NULL;
        it->pos++;
    }
    rt_trap_clear_recovery();
    return (void *)seq;
}

/// @brief Advance the iterator by up to @p n positions, returning the count skipped.
/// @details Moves the cursor forward without returning the intermediate elements.
///          Stops at the end of the collection if fewer than @p n elements remain.
/// @param iter Iterator object pointer; returns 0 if NULL.
/// @param n Maximum number of elements to skip (must be positive).
/// @return Number of elements actually skipped.
int64_t rt_iter_skip(void *iter, int64_t n) {
    if (!iter || n <= 0)
        return 0;
    rt_iter_impl *it = as_iter(iter, "Iterator.Skip: invalid Iterator object");
    if (!it || !iter_cursor_valid(it))
        return 0;
    int64_t remaining = it->len - it->pos;
    int64_t skipped = (n < remaining) ? n : remaining;
    it->pos += skipped;
    return skipped;
}
