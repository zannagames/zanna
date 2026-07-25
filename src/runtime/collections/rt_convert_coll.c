//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_convert_coll.c
// Purpose: Implements conversion functions between Zanna collection types.
//   Provides direct conversions among common sequence, set, map-view, stack,
//   queue, deque, bag, and ring representations.
//
// Key invariants:
//   - All conversions iterate the source using the source collection's public
//     API (e.g., rt_seq_len + rt_seq_get for Seq, rt_stack_peek + pop for
//     Stack). No internal structure of the source is accessed directly.
//   - Conversion creates a new destination collection and pushes/inserts each
//     element from the source in iteration order.
//   - NULL source arguments return an empty (newly allocated) destination
//     collection rather than NULL, so callers can always use the result.
//   - Elements are not deep-copied; the destination holds the same object
//     pointers as the source. Shared elements follow GC ownership rules.
//   - Not thread-safe; do not convert a collection that is being modified
//     concurrently.
//
// Ownership/Lifetime:
//   - Returned collection objects are GC-managed. The caller receives a new
//     GC root; no manual free is needed.
//
// Links: src/runtime/collections/rt_convert_coll.h (public API),
//        src/runtime/collections/rt_seq.h, rt_list.h, rt_set.h, rt_stack.h,
//        rt_queue.h, rt_ring.h, rt_deque.h (source/destination types)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements collection-to-collection conversion helpers.
///
/// Every conversion allocates a destination and leaves the source unchanged.
/// Ordered sources retain their documented traversal order; unordered sources
/// retain only their source-defined enumeration order. Pointer-valued elements
/// are shared under the destination collection's retain/release conventions,
/// while Bag conversions copy string values according to the Bag API.
///
/// Stack and Queue conversions traverse clones so even partially completed
/// work cannot consume the source. Null sources produce newly allocated empty
/// destinations. Variadic constructors consume exactly @p count object
/// pointers when the count is positive.

#include "rt_convert_coll.h"

#include "rt_box.h"
#include "rt_bag.h"
#include "rt_deque.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_queue.h"
#include "rt_ring.h"
#include "rt_seq.h"
#include "rt_set.h"
#include "rt_stack.h"
#include "rt_string.h"
#include <stdarg.h>
#include <stdlib.h>

/// @brief Drop one GC reference to a transient @p obj and free it at zero.
/// @param obj Temporary runtime object reference, or NULL for a no-op.
static void release_temp_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Allocate an owned (refcounted) seq for conversion results.
/// @return A new Seq configured to own its elements, or NULL on allocation
///         failure.
static void *new_owned_seq(void) {
    return rt_seq_new_owned();
}

//=============================================================================
// Seq Conversions
//=============================================================================

/// @brief Seq to list.
/// @param seq Source Seq, or NULL.
/// @return A new List containing source elements in index order, or an empty
///         List for NULL.
void *rt_seq_to_list(void *seq) {
    void *list = rt_list_new();
    if (!seq || !list)
        return list;

    int64_t len = rt_seq_len(seq);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_seq_get(seq, i);
        rt_list_push(list, elem);
    }
    return list;
}

/// @brief Seq to set.
/// @param seq Source Seq, or NULL.
/// @return A new Set populated in index order; duplicates collapse according
///         to Set equality semantics.
void *rt_seq_to_set(void *seq) {
    void *set = rt_set_new();
    if (!seq || !set)
        return set;

    int64_t len = rt_seq_len(seq);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_seq_get(seq, i);
        rt_set_add(set, elem);
    }
    return set;
}

/// @brief Seq to stack.
/// @param seq Source Seq, or NULL.
/// @return A new owning Stack whose bottom is element zero and whose top is
///         the final Seq element.
void *rt_seq_to_stack(void *seq) {
    void *stack = rt_stack_new();
    if (!seq || !stack)
        return stack;
    rt_stack_set_owns_elements(stack, 1);

    int64_t len = rt_seq_len(seq);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_seq_get(seq, i);
        rt_stack_push(stack, elem);
    }
    return stack;
}

/// @brief Seq to queue.
/// @param seq Source Seq, or NULL.
/// @return A new owning Queue whose front is element zero.
void *rt_seq_to_queue(void *seq) {
    void *queue = rt_queue_new();
    if (!seq || !queue)
        return queue;
    rt_queue_set_owns_elements(queue, 1);

    int64_t len = rt_seq_len(seq);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_seq_get(seq, i);
        rt_queue_push(queue, elem);
    }
    return queue;
}

/// @brief Seq to deque.
/// @param seq Source Seq, or NULL.
/// @return A new Deque containing elements from front to back in Seq order.
void *rt_seq_to_deque(void *seq) {
    void *deque = rt_deque_new();
    if (!seq || !deque)
        return deque;

    int64_t len = rt_seq_len(seq);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_seq_get(seq, i);
        rt_deque_push_back(deque, elem);
    }
    return deque;
}

/// @brief Seq to bag.
/// @param seq Source Seq of raw or boxed runtime strings, or NULL.
/// @return A new Bag containing unique copied string values.
/// @note Non-string elements trap through `rt_seq_get_str()`.
void *rt_seq_to_bag(void *seq) {
    void *bag = rt_bag_new();
    if (!seq || !bag)
        return bag;

    int64_t len = rt_seq_len(seq);
    for (int64_t i = 0; i < len; i++) {
        rt_string elem = rt_seq_get_str(seq, i);
        rt_bag_add(bag, elem);
        rt_str_release_maybe(elem);
    }
    return bag;
}

//=============================================================================
// List Conversions
//=============================================================================

/// @brief List to seq.
/// @param list Source List, or NULL.
/// @return A new owning Seq containing List elements in index order.
void *rt_list_to_seq(void *list) {
    void *seq = new_owned_seq();
    if (!list || !seq)
        return seq;

    int64_t len = rt_list_len(list);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_list_get(list, i);
        rt_seq_push(seq, elem);
        release_temp_obj(elem);
    }
    return seq;
}

/// @brief List to set.
/// @param list Source List, or NULL.
/// @return A new Set containing the List's unique elements.
void *rt_list_to_set(void *list) {
    void *set = rt_set_new();
    if (!list || !set)
        return set;

    int64_t len = rt_list_len(list);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_list_get(list, i);
        rt_set_add(set, elem);
        release_temp_obj(elem);
    }
    return set;
}

/// @brief List to stack.
/// @param list Source List, or NULL.
/// @return A new owning Stack with the first List element at the bottom.
void *rt_list_to_stack(void *list) {
    void *stack = rt_stack_new();
    if (!list || !stack)
        return stack;
    rt_stack_set_owns_elements(stack, 1);

    int64_t len = rt_list_len(list);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_list_get(list, i);
        rt_stack_push(stack, elem);
        release_temp_obj(elem);
    }
    return stack;
}

/// @brief List to queue.
/// @param list Source List, or NULL.
/// @return A new owning Queue with the first List element at the front.
void *rt_list_to_queue(void *list) {
    void *queue = rt_queue_new();
    if (!list || !queue)
        return queue;
    rt_queue_set_owns_elements(queue, 1);

    int64_t len = rt_list_len(list);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_list_get(list, i);
        rt_queue_push(queue, elem);
        release_temp_obj(elem);
    }
    return queue;
}

//=============================================================================
// Set Conversions
//=============================================================================

/// @brief Set to seq.
/// @param set Source Set, or NULL.
/// @return A new owning Seq in the Set's unspecified enumeration order.
void *rt_set_to_seq(void *set) {
    if (!set)
        return new_owned_seq();
    // rt_set_items already returns a Seq
    return rt_set_items(set);
}

/// @brief Set to list.
/// @param set Source Set, or NULL.
/// @return A new List in the Set's unspecified enumeration order.
void *rt_set_to_list(void *set) {
    void *seq = rt_set_to_seq(set);
    void *list = rt_seq_to_list(seq);
    if (rt_obj_release_check0(seq))
        rt_obj_free(seq);
    return list;
}

//=============================================================================
// Stack Conversions
//=============================================================================

/// @brief Stack to seq.
/// @param stack Source Stack, or NULL.
/// @return A new owning Seq ordered from stack bottom to top.
/// @note The source is cloned before traversal and is never popped directly.
void *rt_stack_to_seq(void *stack) {
    void *seq = new_owned_seq();
    if (!stack || !seq)
        return seq;

    int64_t len = rt_stack_len(stack);
    if (len <= 0)
        return seq;
    if ((uint64_t)len > SIZE_MAX / sizeof(void *)) {
        rt_trap("stack_to_seq: size overflow");
        return seq;
    }

    void *clone = rt_stack_clone(stack);
    if (!clone)
        return seq;
    int8_t pop_returns_owned_refs = rt_stack_owns_elements(clone);

    // Pop a clone so conversion never mutates the source stack on traps.
    void **temp = malloc(sizeof(void *) * (size_t)len);
    if (!temp) {
        release_temp_obj(clone);
        return seq;
    }

    int64_t count = 0;
    while (!rt_stack_is_empty(clone)) {
        temp[count++] = rt_stack_pop(clone);
    }

    // Add to seq in reverse order (bottom to top)
    for (int64_t i = count - 1; i >= 0; i--) {
        rt_seq_push(seq, temp[i]);
        if (pop_returns_owned_refs)
            release_temp_obj(temp[i]);
    }

    free(temp);
    release_temp_obj(clone);
    return seq;
}

/// @brief Stack to list.
/// @param stack Source Stack, or NULL.
/// @return A new List ordered from stack bottom to top.
void *rt_stack_to_list(void *stack) {
    void *seq = rt_stack_to_seq(stack);
    void *list = rt_seq_to_list(seq);
    if (rt_obj_release_check0(seq))
        rt_obj_free(seq);
    return list;
}

//=============================================================================
// Queue Conversions
//=============================================================================

/// @brief Queue to seq.
/// @param queue Source Queue, or NULL.
/// @return A new owning Seq ordered from queue front to back.
/// @note The source is cloned before traversal and is never popped directly.
void *rt_queue_to_seq(void *queue) {
    void *seq = new_owned_seq();
    if (!queue || !seq)
        return seq;

    int64_t len = rt_queue_len(queue);
    if (len <= 0)
        return seq;
    if ((uint64_t)len > SIZE_MAX / sizeof(void *)) {
        rt_trap("queue_to_seq: size overflow");
        return seq;
    }

    void *clone = rt_queue_clone(queue);
    if (!clone)
        return seq;
    int8_t pop_returns_owned_refs = rt_queue_owns_elements(clone);

    while (!rt_queue_is_empty(clone)) {
        void *elem = rt_queue_pop(clone);
        rt_seq_push(seq, elem);
        if (pop_returns_owned_refs)
            release_temp_obj(elem);
    }

    release_temp_obj(clone);
    return seq;
}

/// @brief Queue to list.
/// @param queue Source Queue, or NULL.
/// @return A new List ordered from queue front to back.
void *rt_queue_to_list(void *queue) {
    void *seq = rt_queue_to_seq(queue);
    void *list = rt_seq_to_list(seq);
    if (rt_obj_release_check0(seq))
        rt_obj_free(seq);
    return list;
}

//=============================================================================
// Deque Conversions
//=============================================================================

/// @brief Deque to seq.
/// @param deque Source Deque, or NULL.
/// @return A new owning Seq ordered from deque front to back.
void *rt_deque_to_seq(void *deque) {
    void *seq = new_owned_seq();
    if (!deque || !seq)
        return seq;

    int64_t len = rt_deque_len(deque);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_deque_get(deque, i);
        rt_seq_push(seq, elem);
        release_temp_obj(elem);
    }
    return seq;
}

/// @brief Deque to list.
/// @param deque Source Deque, or NULL.
/// @return A new List ordered from deque front to back.
void *rt_deque_to_list(void *deque) {
    void *seq = rt_deque_to_seq(deque);
    void *list = rt_seq_to_list(seq);
    if (rt_obj_release_check0(seq))
        rt_obj_free(seq);
    return list;
}

//=============================================================================
// Map Conversions
//=============================================================================

/// @brief Map keys to seq.
/// @param map Source Map, or NULL.
/// @return A new owning Seq of keys in Map enumeration order.
void *rt_map_keys_to_seq(void *map) {
    if (!map)
        return new_owned_seq();
    // rt_map_keys already returns a Seq
    return rt_map_keys(map);
}

/// @brief Map values to seq.
/// @param map Source Map, or NULL.
/// @return A new owning Seq of values corresponding to Map enumeration order.
void *rt_map_values_to_seq(void *map) {
    if (!map)
        return new_owned_seq();
    // rt_map_values already returns a Seq
    return rt_map_values(map);
}

//=============================================================================
// Bag Conversions
//=============================================================================

/// @brief Bag to seq.
/// @param bag Source Bag, or NULL.
/// @return A new owning Seq of copied strings in unspecified Bag order.
void *rt_bag_to_seq(void *bag) {
    if (!bag)
        return new_owned_seq();
    // Bag stores unique strings, rt_bag_items already returns a Seq
    return rt_bag_items(bag);
}

/// @brief Bag to set.
/// @param bag Source Bag, or NULL.
/// @return A new Set of boxed string values in unspecified Bag order.
void *rt_bag_to_set(void *bag) {
    void *set = rt_set_new();
    if (!bag || !set)
        return set;

    // Get unique elements
    void *items = rt_bag_items(bag);
    if (!items)
        return set;

    int64_t len = rt_seq_len(items);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_seq_get(items, i); // borrowed from the snapshot Seq
        // Bag items are raw runtime strings; box them so Set membership uses
        // boxed-string value equality like every other Set entry (VDOC-102).
        void *entry = elem;
        if (rt_string_is_handle(elem))
            entry = rt_box_str((rt_string)elem);
        rt_set_add(set, entry); // Set retains its own reference
        if (entry != elem && rt_obj_release_check0(entry))
            rt_obj_free(entry);
    }
    // Release the intermediate Seq from rt_bag_items
    if (rt_obj_release_check0(items))
        rt_obj_free(items);
    return set;
}

//=============================================================================
// Ring Conversions
//=============================================================================

/// @brief Ring to seq.
/// @param ring Source Ring, or NULL.
/// @return A new owning Seq in logical Ring index order.
void *rt_ring_to_seq(void *ring) {
    void *seq = new_owned_seq();
    if (!ring || !seq)
        return seq;

    int64_t len = rt_ring_len(ring);
    for (int64_t i = 0; i < len; i++) {
        void *elem = rt_ring_get(ring, i);
        rt_seq_push(seq, elem);
    }
    return seq;
}

//=============================================================================
// Utility Functions
//=============================================================================

/// @brief Seq of.
/// @param count Number of following object pointers; non-positive counts
///        produce an empty Seq.
/// @param ... Exactly @p count element pointers.
/// @return A new Seq containing arguments in call order.
void *rt_seq_of(int64_t count, ...) {
    void *seq = rt_seq_new();
    if (!seq || count <= 0)
        return seq;

    va_list args;
    va_start(args, count);
    for (int64_t i = 0; i < count; i++) {
        void *elem = va_arg(args, void *);
        rt_seq_push(seq, elem);
    }
    va_end(args);

    return seq;
}

/// @brief List of.
/// @param count Number of following object pointers; non-positive counts
///        produce an empty List.
/// @param ... Exactly @p count element pointers.
/// @return A new List containing arguments in call order.
void *rt_list_of(int64_t count, ...) {
    void *list = rt_list_new();
    if (!list || count <= 0)
        return list;

    va_list args;
    va_start(args, count);
    for (int64_t i = 0; i < count; i++) {
        void *elem = va_arg(args, void *);
        rt_list_push(list, elem);
    }
    va_end(args);

    return list;
}

/// @brief Set of.
/// @param count Number of following object pointers; non-positive counts
///        produce an empty Set.
/// @param ... Exactly @p count element pointers.
/// @return A new Set populated in call order, with duplicates collapsed by Set
///         equality semantics.
void *rt_set_of(int64_t count, ...) {
    void *set = rt_set_new();
    if (!set || count <= 0)
        return set;

    va_list args;
    va_start(args, count);
    for (int64_t i = 0; i < count; i++) {
        void *elem = va_arg(args, void *);
        rt_set_add(set, elem);
    }
    va_end(args);

    return set;
}
