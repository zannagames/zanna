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

#include "rt_collection_ids.h"
#include "rt_deque.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_ring.h"
#include "rt_seq.h"
#include "rt_set.h"
#include "rt_stack.h"

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
    rt_iter_impl *it;
    if (!source)
        return NULL;
    it = (rt_iter_impl *)rt_obj_new_i64(RT_ITERATOR_CLASS_ID, (int64_t)sizeof(rt_iter_impl));
    if (!it) {
        rt_trap("Iterator: allocation failed");
        return NULL;
    }
    it->vptr = NULL;
    it->source = source;
    rt_obj_retain_maybe(source);
    it->kind = kind;
    it->pos = 0;
    it->len = len;
    rt_obj_set_finalizer(it, iter_finalizer);
    rt_gc_track(it, iter_traverse);
    return it;
}

/// @brief Creates an iterator that takes ownership of a Seq snapshot.
/// @param snapshot Non-null newly created Seq whose creation reference transfers
///        to the iterator.
/// @param len Cached number of snapshot elements.
/// @return New runtime-managed iterator, or NULL after releasing an unusable
///         snapshot.
static rt_iter_impl *make_iter_snapshot(void *snapshot, int64_t len) {
    rt_iter_impl *it;
    if (!snapshot)
        return NULL;
    it = (rt_iter_impl *)rt_obj_new_i64(RT_ITERATOR_CLASS_ID, (int64_t)sizeof(rt_iter_impl));
    if (!it) {
        /* Failed to create iterator — release the snapshot we own. */
        if (rt_obj_release_check0(snapshot))
            rt_obj_free(snapshot);
        rt_trap("Iterator: allocation failed");
        return NULL;
    }
    it->vptr = NULL;
    it->source = snapshot;
    /* No retain: we take ownership of the snapshot's creation reference. */
    it->kind = ITER_SNAPSHOT;
    it->pos = 0;
    it->len = len;
    rt_obj_set_finalizer(it, iter_finalizer);
    rt_gc_track(it, iter_traverse);
    return it;
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
    return make_iter(seq, ITER_SEQ, rt_seq_len(seq));
}

/// @brief Build a live iterator over a List.
/// @param list Non-null List to retain.
/// @return New iterator with the List's current length cached, or NULL.
/// @note Structural mutation during iteration is unsupported.
void *rt_iter_from_list(void *list) {
    if (!list)
        return NULL;
    return make_iter(list, ITER_LIST, rt_list_len(list));
}

/// @brief Snapshot a Deque into an iterator. Later mutations of the deque are NOT seen.
/// @param deque Non-null Deque to snapshot in front-to-back order.
/// @return New snapshot iterator, or NULL if snapshot construction fails.
void *rt_iter_from_deque(void *deque) {
    void *snapshot;
    int64_t len, i;
    if (!deque)
        return NULL;
    /* Snapshot all elements into an owning Seq so iterator values survive source mutations. */
    len = rt_deque_len(deque);
    snapshot = rt_seq_new();
    if (!snapshot)
        return NULL;
    rt_seq_set_owns_elements(snapshot, 1);
    for (i = 0; i < len; i++) {
        void *item = rt_deque_get(deque, i);
        rt_seq_push(snapshot, item);
        release_temp_obj(item);
    }
    return make_iter_snapshot(snapshot, len);
}

/// @brief Build a live iterator over a Ring.
/// @param ring Non-null Ring to retain.
/// @return New iterator with the Ring's current length cached, or NULL.
/// @note Structural mutation during iteration is unsupported.
void *rt_iter_from_ring(void *ring) {
    if (!ring)
        return NULL;
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
    void *snapshot;
    void *clone;
    void **items;
    int64_t len;
    int64_t count = 0;
    if (!stack)
        return NULL;
    len = rt_stack_len(stack);
    if (len <= 0)
        snapshot = rt_seq_new();
    else
        snapshot = rt_seq_with_capacity(len);
    if (!snapshot)
        return NULL;
    rt_seq_set_owns_elements(snapshot, 1);
    if (len <= 0)
        return make_iter_snapshot(snapshot, 0);
    clone = rt_stack_clone(stack);
    if (!clone) {
        if (rt_obj_release_check0(snapshot))
            rt_obj_free(snapshot);
        return NULL;
    }
    if ((uint64_t)len > SIZE_MAX / sizeof(void *)) {
        if (rt_obj_release_check0(clone))
            rt_obj_free(clone);
        if (rt_obj_release_check0(snapshot))
            rt_obj_free(snapshot);
        rt_trap("Iterator: stack snapshot too large");
        return NULL;
    }
    int8_t pop_returns_owned_refs = rt_stack_owns_elements(clone);
    items = (void **)malloc((size_t)len * sizeof(void *));
    if (!items) {
        if (rt_obj_release_check0(clone))
            rt_obj_free(clone);
        if (rt_obj_release_check0(snapshot))
            rt_obj_free(snapshot);
        rt_trap("Iterator: allocation failed");
        return NULL;
    }

    while (!rt_stack_is_empty(clone) && count < len)
        items[count++] = rt_stack_pop(clone);

    for (int64_t i = count; i > 0; --i) {
        void *item = items[i - 1];
        rt_seq_push(snapshot, item);
        if (pop_returns_owned_refs)
            release_temp_obj(item);
    }
    if (rt_obj_release_check0(clone))
        rt_obj_free(clone);
    free(items);
    return make_iter_snapshot(snapshot, rt_seq_len(snapshot));
}

//=============================================================================
// Core iteration
//=============================================================================

/// @brief Fetch element @p idx from the iterator's source, dispatching on
///        its backing collection kind (seq/list/ring/…).
/// @param it Iterator whose source is accessed.
/// @param idx Zero-based source index.
/// @return Element pointer with the source accessor's native ownership.
static void *get_element(rt_iter_impl *it, int64_t idx) {
    switch (it->kind) {
        case ITER_SEQ:
        case ITER_SNAPSHOT:
            return rt_seq_get(it->source, idx);
        case ITER_LIST:
            return rt_list_get(it->source, idx);
        case ITER_RING:
            return rt_ring_get(it->source, idx);
    }
    return NULL;
}

/// @brief Check whether the iterator has more elements.
/// @details Returns true if the current position is before the end.
/// @param iter Iterator handle, or NULL.
/// @return 1 when another cached position remains; otherwise 0.
int8_t rt_iter_has_next(void *iter) {
    rt_iter_impl *it;
    if (!iter)
        return 0;
    it = as_iter(iter, "Iterator.HasNext: invalid Iterator object");
    return (it->pos < it->len) ? 1 : 0;
}

/// @brief Return the current element and advance the cursor. Returns NULL when exhausted.
/// Pair with `_has_next` to drive while-loop iteration.
/// @param iter Iterator handle, or NULL.
/// @return Current element as a retained caller-owned reference, or NULL when
///         exhausted. The cursor advances by one on success.
void *rt_iter_next(void *iter) {
    rt_iter_impl *it;
    void *elem;
    if (!iter)
        return NULL;
    it = as_iter(iter, "Iterator.Next: invalid Iterator object");
    if (it->pos >= it->len)
        return NULL;
    elem = get_element(it, it->pos);
    it->pos++;
    if (it->kind != ITER_LIST)
        rt_obj_retain_maybe(elem);
    return elem;
}

/// @brief Look at the current element without advancing the cursor.
/// @param iter Iterator handle, or NULL.
/// @return Current element as a retained caller-owned reference, or NULL when
///         exhausted.
void *rt_iter_peek(void *iter) {
    rt_iter_impl *it;
    void *elem;
    if (!iter)
        return NULL;
    it = as_iter(iter, "Iterator.Peek: invalid Iterator object");
    if (it->pos >= it->len)
        return NULL;
    elem = get_element(it, it->pos);
    if (it->kind != ITER_LIST)
        rt_obj_retain_maybe(elem);
    return elem;
}

/// @brief Reset the iterator to the beginning of the collection.
/// @param iter Iterator handle, or NULL for a no-op.
void rt_iter_reset(void *iter) {
    if (!iter)
        return;
    as_iter(iter, "Iterator.Reset: invalid Iterator object")->pos = 0;
}

/// @brief Return the current position (0-based index) of the iterator.
/// @param iter Iterator object pointer; returns 0 if NULL.
/// @return Current index within the underlying collection.
int64_t rt_iter_index(void *iter) {
    if (!iter)
        return 0;
    return as_iter(iter, "Iterator.Index: invalid Iterator object")->pos;
}

/// @brief Return the total number of elements in the iterable collection.
/// @param iter Iterator handle, or NULL.
/// @return Length cached when the iterator was created, or zero for NULL.
int64_t rt_iter_count(void *iter) {
    if (!iter)
        return 0;
    return as_iter(iter, "Iterator.Count: invalid Iterator object")->len;
}

/// @brief Drain the remaining iterator elements into a fresh Seq. Advances the cursor to end.
/// @param iter Iterator handle, or NULL.
/// @return A new owning Seq containing elements from the current position to
///         the cached end. NULL produces an empty Seq.
void *rt_iter_to_seq(void *iter) {
    rt_iter_impl *it;
    void *seq;
    if (!iter) {
        seq = rt_seq_new();
        rt_seq_set_owns_elements(seq, 1);
        return seq;
    }
    it = as_iter(iter, "Iterator.ToSeq: invalid Iterator object");
    seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    while (it->pos < it->len) {
        void *elem = rt_iter_next(iter);
        rt_seq_push(seq, elem);
        release_temp_obj(elem);
    }
    return seq;
}

/// @brief Advance the iterator by up to @p n positions, returning the count skipped.
/// @details Moves the cursor forward without returning the intermediate elements.
///          Stops at the end of the collection if fewer than @p n elements remain.
/// @param iter Iterator object pointer; returns 0 if NULL.
/// @param n Maximum number of elements to skip (must be positive).
/// @return Number of elements actually skipped.
int64_t rt_iter_skip(void *iter, int64_t n) {
    rt_iter_impl *it;
    int64_t remaining, skipped;
    if (!iter || n <= 0)
        return 0;
    it = as_iter(iter, "Iterator.Skip: invalid Iterator object");
    remaining = it->len - it->pos;
    skipped = (n < remaining) ? n : remaining;
    it->pos += skipped;
    return skipped;
}
