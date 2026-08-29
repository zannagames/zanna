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
//   - Indexed conversions validate one exact private source layout and borrow
//     stable slots directly while external synchronization excludes mutation.
//   - Conversion creates a new destination collection and pushes/inserts each
//     element from the source in iteration order as an all-or-nothing
//     ownership transaction.
//   - NULL source arguments return an empty (newly allocated) destination
//     collection rather than NULL, so callers can always use the result.
//   - Elements are not deep-copied; the destination holds the same object
//     pointers as the source. Shared elements follow GC ownership rules.
//   - Not thread-safe; do not convert a collection that is being modified
//     concurrently.
//
// Ownership/Lifetime:
//   - Returned collection objects are GC-managed. The caller owns the returned
//     reference and must release it when the result is no longer needed.
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
/// Stack and Queue conversions borrow their validated native layouts without
/// mutating or cloning them. Null sources produce newly allocated empty
/// destinations. Variadic constructors consume exactly @p count object
/// pointers when the count is positive.

#include "rt_convert_coll.h"

#include "rt_array_obj.h"
#include "rt_bag.h"
#include "rt_box.h"
#include "rt_deque.h"
#include "rt_deque_internal.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_list_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_queue.h"
#include "rt_queue_internal.h"
#include "rt_ring.h"
#include "rt_ring_internal.h"
#include "rt_seq.h"
#include "rt_seq_internal.h"
#include "rt_set.h"
#include "rt_stack.h"
#include "rt_stack_internal.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
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

/// @brief Copy the active trap diagnostic before removing a recovery frame.
/// @param buffer Caller-owned diagnostic buffer.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when no current trap diagnostic is available.
static void save_conversion_trap(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Reject a source whose private layout or storage invariants are invalid.
/// @param condition Nonzero when the source is safe to inspect.
/// @param diagnostic Trap message for an invalid source.
/// @return One when valid; zero after trapping.
static int require_source(int condition, const char *diagnostic) {
    if (condition)
        return 1;
    rt_trap(diagnostic);
    return 0;
}

/// @brief Validate an indexed Seq source and return its logical length.
static int seq_view(void *obj, int64_t *out_len) {
    *out_len = 0;
    if (!require_source(rt_seq_internal_is_valid(obj), "Convert: invalid Seq source"))
        return 0;
    rt_seq_impl *seq = (rt_seq_impl *)obj;
    if (!require_source(seq->len >= 0 && seq->cap >= seq->len && seq->cap > 0 &&
                            (uint64_t)seq->cap <= SIZE_MAX / sizeof(void *) && seq->items,
                        "Convert: corrupted Seq source"))
        return 0;
    *out_len = seq->len;
    return 1;
}

/// @brief Validate an indexed List source and its managed object-array backing.
static int list_view(void *obj, int64_t *out_len) {
    *out_len = 0;
    if (!require_source(rt_list_internal_is_valid(obj), "Convert: invalid List source"))
        return 0;
    rt_list_impl *list = (rt_list_impl *)obj;
    if (!list->arr)
        return 1;

    rt_heap_info_t info;
    if (!require_source(rt_heap_get_info(list->arr, &info) && info.kind == RT_HEAP_ARRAY &&
                            info.elem_kind == RT_ELEM_OBJ && info.cap >= info.len &&
                            info.len <= (size_t)INT64_MAX,
                        "Convert: corrupted List backing array"))
        return 0;
    *out_len = (int64_t)info.len;
    return 1;
}

/// @brief Validate an indexed Stack source and return its logical length.
static int stack_view(void *obj, int64_t *out_len) {
    *out_len = 0;
    if (!require_source(rt_stack_internal_is_valid(obj), "Convert: invalid Stack source"))
        return 0;
    rt_stack_impl *stack = (rt_stack_impl *)obj;
    if (!require_source(stack->len >= 0 && stack->cap >= stack->len && stack->cap > 0 &&
                            (uint64_t)stack->cap <= SIZE_MAX / sizeof(void *) && stack->items,
                        "Convert: corrupted Stack source"))
        return 0;
    *out_len = stack->len;
    return 1;
}

/// @brief Validate an indexed Queue source and return its logical length.
static int queue_view(void *obj, int64_t *out_len) {
    *out_len = 0;
    if (!require_source(rt_queue_internal_is_valid(obj), "Convert: invalid Queue source"))
        return 0;
    rt_queue_impl *queue = (rt_queue_impl *)obj;
    int storage_valid = queue->cap > 0 && (uint64_t)queue->cap <= SIZE_MAX / sizeof(void *) &&
                        queue->len >= 0 && queue->len <= queue->cap && queue->head >= 0 &&
                        queue->head < queue->cap && queue->tail >= 0 && queue->tail < queue->cap &&
                        queue->items;
    if (storage_valid) {
        uint64_t expected_tail =
            ((uint64_t)queue->head + (uint64_t)queue->len) % (uint64_t)queue->cap;
        storage_valid = (uint64_t)queue->tail == expected_tail;
    }
    if (!require_source(storage_valid, "Convert: corrupted Queue source"))
        return 0;
    *out_len = queue->len;
    return 1;
}

/// @brief Validate an indexed Deque source and return its logical length.
static int deque_view(void *obj, int64_t *out_len) {
    *out_len = 0;
    if (!require_source(rt_deque_internal_is_valid(obj), "Convert: invalid Deque source"))
        return 0;
    rt_deque_impl *deque = (rt_deque_impl *)obj;
    if (!require_source(deque->cap > 0 && (uint64_t)deque->cap <= SIZE_MAX / sizeof(void *) &&
                            deque->len >= 0 && deque->len <= deque->cap && deque->front >= 0 &&
                            deque->front < deque->cap && deque->data,
                        "Convert: corrupted Deque source"))
        return 0;
    *out_len = deque->len;
    return 1;
}

/// @brief Validate an indexed Ring source and return its logical length.
static int ring_view(void *obj, int64_t *out_len) {
    *out_len = 0;
    if (!require_source(rt_ring_internal_is_valid(obj), "Convert: invalid Ring source"))
        return 0;
    rt_ring_impl *ring = (rt_ring_impl *)obj;
    if (!require_source(ring->capacity > 0 && ring->capacity <= SIZE_MAX / sizeof(void *) &&
                            ring->count <= ring->capacity && ring->count <= (size_t)INT64_MAX &&
                            ring->head < ring->capacity && ring->items,
                        "Convert: corrupted Ring source"))
        return 0;
    *out_len = (int64_t)ring->count;
    return 1;
}

/// @brief Borrow an indexed Seq element after @ref seq_view succeeds.
static void *seq_at(void *obj, int64_t index) {
    return ((rt_seq_impl *)obj)->items[index];
}

/// @brief Borrow an indexed List element after @ref list_view succeeds.
static void *list_at(void *obj, int64_t index) {
    return ((rt_list_impl *)obj)->arr[index];
}

/// @brief Borrow a Stack element in bottom-to-top order.
static void *stack_at(void *obj, int64_t index) {
    return ((rt_stack_impl *)obj)->items[index];
}

/// @brief Borrow a Queue element in front-to-back order.
static void *queue_at(void *obj, int64_t index) {
    rt_queue_impl *queue = (rt_queue_impl *)obj;
    uint64_t physical = ((uint64_t)queue->head + (uint64_t)index) % (uint64_t)queue->cap;
    return queue->items[physical];
}

/// @brief Borrow a Deque element in front-to-back order.
static void *deque_at(void *obj, int64_t index) {
    rt_deque_impl *deque = (rt_deque_impl *)obj;
    uint64_t physical = ((uint64_t)deque->front + (uint64_t)index) % (uint64_t)deque->cap;
    return deque->data[physical];
}

/// @brief Borrow a Ring element in logical index order.
static void *ring_at(void *obj, int64_t index) {
    rt_ring_impl *ring = (rt_ring_impl *)obj;
    size_t offset = (size_t)index;
    size_t distance_to_end = ring->capacity - ring->head;
    size_t physical = offset >= distance_to_end ? offset - distance_to_end : ring->head + offset;
    return ring->items[physical];
}

/// @brief Borrow one pointer from a native variadic staging table.
static void *native_pointer_at(void *table, int64_t index) {
    return ((void **)table)[index];
}

typedef void *(*conversion_at_fn)(void *source, int64_t index);
typedef void *(*conversion_new_fn)(int64_t source_len);
typedef int (*conversion_append_fn)(void *result, void *element);

/// @brief Allocate an owning Seq with exact known source capacity.
static void *new_seq_result(int64_t source_len) {
    return rt_seq_with_capacity_owned(source_len > 0 ? source_len : 1);
}

/// @brief Allocate a List conversion result.
static void *new_list_result(int64_t source_len) {
    (void)source_len;
    return rt_list_new();
}

/// @brief Allocate a Set conversion result.
static void *new_set_result(int64_t source_len) {
    (void)source_len;
    return rt_set_new();
}

/// @brief Allocate an owning Stack conversion result.
static void *new_stack_result(int64_t source_len) {
    (void)source_len;
    void *stack = rt_stack_new();
    if (stack)
        rt_stack_set_owns_elements(stack, 1);
    return stack;
}

/// @brief Allocate an owning Queue conversion result.
static void *new_queue_result(int64_t source_len) {
    (void)source_len;
    void *queue = rt_queue_new();
    if (queue)
        rt_queue_set_owns_elements(queue, 1);
    return queue;
}

/// @brief Allocate an exact-capacity Deque conversion result.
static void *new_deque_result(int64_t source_len) {
    return rt_deque_with_capacity(source_len > 0 ? source_len : 1);
}

/// @brief Return whether a void append advanced a linear destination by one.
static int append_advanced(int64_t before, int64_t after) {
    return before >= 0 && before < INT64_MAX && after == before + 1;
}

/// @brief Append to an owning Seq and verify returning-trap failure.
static int append_seq(void *result, void *element) {
    int64_t before = rt_seq_len(result);
    rt_seq_push(result, element);
    return append_advanced(before, rt_seq_len(result));
}

/// @brief Append to a List and verify returning-trap failure.
static int append_list(void *result, void *element) {
    int64_t before = rt_list_len(result);
    rt_list_push(result, element);
    return append_advanced(before, rt_list_len(result));
}

/// @brief Append to an owning Stack and verify returning-trap failure.
static int append_stack(void *result, void *element) {
    int64_t before = rt_stack_len(result);
    rt_stack_push(result, element);
    return append_advanced(before, rt_stack_len(result));
}

/// @brief Append to an owning Queue and verify returning-trap failure.
static int append_queue(void *result, void *element) {
    int64_t before = rt_queue_len(result);
    rt_queue_push(result, element);
    return append_advanced(before, rt_queue_len(result));
}

/// @brief Append to a Deque and verify returning-trap failure.
static int append_deque(void *result, void *element) {
    int64_t before = rt_deque_len(result);
    rt_deque_push_back(result, element);
    return append_advanced(before, rt_deque_len(result));
}

/// @brief Insert into a Set, accepting an already-present equal value.
static int append_set(void *result, void *element) {
    int64_t before = rt_set_len(result);
    if (rt_set_add(result, element))
        return append_advanced(before, rt_set_len(result));
    return rt_set_len(result) == before && rt_set_has(result, element);
}

/// @brief Insert a copied string into a Bag, accepting an existing equal key.
static int append_bag(void *result, rt_string element) {
    int64_t before = rt_bag_len(result);
    if (rt_bag_add(result, element))
        return append_advanced(before, rt_bag_len(result));
    return rt_bag_len(result) == before && rt_bag_has(result, element);
}

/// @brief Convert a validated indexed source as one retained transaction.
/// @param source Validated source object or native pointer table.
/// @param source_len Number of elements available through @p at.
/// @param at Borrowed element accessor.
/// @param make_result Destination constructor.
/// @param append Checked retaining append operation.
/// @param diagnostic Fallback failure diagnostic.
/// @return Complete new destination, or null after destroying partial state.
static void *convert_indexed(void *source,
                             int64_t source_len,
                             conversion_at_fn at,
                             conversion_new_fn make_result,
                             conversion_append_fn append,
                             const char *diagnostic) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        save_conversion_trap(saved_error, sizeof(saved_error), diagnostic);
        rt_trap_clear_recovery();
        release_temp_obj((void *)result);
        rt_trap(saved_error);
        return NULL;
    }

    result = make_result(source_len);
    if (!result) {
        rt_trap_clear_recovery();
        return NULL;
    }
    for (int64_t index = 0; index < source_len; ++index) {
        if (!append((void *)result, at(source, index))) {
            char saved_error[256];
            save_conversion_trap(saved_error, sizeof(saved_error), diagnostic);
            rt_trap_clear_recovery();
            release_temp_obj((void *)result);
            rt_trap(saved_error);
            return NULL;
        }
    }

    rt_trap_clear_recovery();
    return (void *)result;
}

/// @brief Convert and then free a native pointer staging table on every path.
static void *convert_native_pointers(void **elements,
                                     int64_t count,
                                     conversion_new_fn make_result,
                                     conversion_append_fn append,
                                     const char *diagnostic) {
    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        save_conversion_trap(saved_error, sizeof(saved_error), diagnostic);
        rt_trap_clear_recovery();
        free(elements);
        rt_trap(saved_error);
        return NULL;
    }

    result = convert_indexed(elements, count, native_pointer_at, make_result, append, diagnostic);
    rt_trap_clear_recovery();
    free(elements);
    return (void *)result;
}

/// @brief Copy exactly @p count variadic object pointers into native storage.
/// @param count Positive number of pointer arguments.
/// @param args Active variadic argument cursor.
/// @return Native pointer table, or null on overflow/allocation failure.
static void **stage_variadic_pointers(int64_t count, va_list *args) {
    if ((uint64_t)count > SIZE_MAX / sizeof(void *))
        return NULL;
    void **elements = (void **)malloc((size_t)count * sizeof(void *));
    if (!elements)
        return NULL;
    for (int64_t index = 0; index < count; ++index)
        elements[index] = va_arg(*args, void *);
    return elements;
}

//=============================================================================
// Seq Conversions
//=============================================================================

/// @brief Seq to list.
/// @param seq Source Seq, or NULL.
/// @return A new List containing source elements in index order, or an empty
///         List for NULL.
void *rt_seq_to_list(void *seq) {
    if (!seq)
        return new_list_result(0);
    int64_t len = 0;
    if (!seq_view(seq, &len))
        return NULL;
    return convert_indexed(
        seq, len, seq_at, new_list_result, append_list, "Seq.ToList: conversion failed");
}

/// @brief Seq to set.
/// @param seq Source Seq, or NULL.
/// @return A new Set populated in index order; duplicates collapse according
///         to Set equality semantics.
void *rt_seq_to_set(void *seq) {
    if (!seq)
        return new_set_result(0);
    int64_t len = 0;
    if (!seq_view(seq, &len))
        return NULL;
    return convert_indexed(
        seq, len, seq_at, new_set_result, append_set, "Seq.ToSet: conversion failed");
}

/// @brief Seq to stack.
/// @param seq Source Seq, or NULL.
/// @return A new owning Stack whose bottom is element zero and whose top is
///         the final Seq element.
void *rt_seq_to_stack(void *seq) {
    if (!seq)
        return new_stack_result(0);
    int64_t len = 0;
    if (!seq_view(seq, &len))
        return NULL;
    return convert_indexed(
        seq, len, seq_at, new_stack_result, append_stack, "Seq.ToStack: conversion failed");
}

/// @brief Seq to queue.
/// @param seq Source Seq, or NULL.
/// @return A new owning Queue whose front is element zero.
void *rt_seq_to_queue(void *seq) {
    if (!seq)
        return new_queue_result(0);
    int64_t len = 0;
    if (!seq_view(seq, &len))
        return NULL;
    return convert_indexed(
        seq, len, seq_at, new_queue_result, append_queue, "Seq.ToQueue: conversion failed");
}

/// @brief Seq to deque.
/// @param seq Source Seq, or NULL.
/// @return A new Deque containing elements from front to back in Seq order.
void *rt_seq_to_deque(void *seq) {
    if (!seq)
        return new_deque_result(0);
    int64_t len = 0;
    if (!seq_view(seq, &len))
        return NULL;
    return convert_indexed(
        seq, len, seq_at, new_deque_result, append_deque, "Seq.ToDeque: conversion failed");
}

/// @brief Seq to bag.
/// @param seq Source Seq of raw or boxed runtime strings, or NULL.
/// @return A new Bag containing unique copied string values.
/// @note Non-string elements trap through `rt_seq_get_str()`.
void *rt_seq_to_bag(void *seq) {
    if (!seq)
        return rt_bag_new();
    int64_t len = 0;
    if (!seq_view(seq, &len))
        return NULL;

    void *volatile bag = NULL;
    rt_string volatile retained = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        save_conversion_trap(saved_error, sizeof(saved_error), "Seq.ToBag: conversion failed");
        rt_trap_clear_recovery();
        rt_str_release_maybe((rt_string)retained);
        release_temp_obj((void *)bag);
        rt_trap(saved_error);
        return NULL;
    }

    bag = rt_bag_new();
    if (!bag) {
        rt_trap_clear_recovery();
        return NULL;
    }
    for (int64_t index = 0; index < len; ++index) {
        void *raw = seq_at(seq, index);
        rt_string element = NULL;
        if (rt_string_is_handle(raw)) {
            element = (rt_string)raw;
        } else if (rt_box_type(raw) == RT_BOX_STR) {
            if (!rt_box_try_to_str(raw, &element))
                goto returning_failure;
            retained = element;
        } else {
            rt_trap("Seq.ToBag: element is not a string");
            goto returning_failure;
        }

        if (!append_bag((void *)bag, element))
            goto returning_failure;
        rt_str_release_maybe((rt_string)retained);
        retained = NULL;
    }

    rt_trap_clear_recovery();
    return (void *)bag;

returning_failure:
    {
        char saved_error[256];
        save_conversion_trap(saved_error, sizeof(saved_error), "Seq.ToBag: conversion failed");
        rt_trap_clear_recovery();
        rt_str_release_maybe((rt_string)retained);
        release_temp_obj((void *)bag);
        rt_trap(saved_error);
        return NULL;
    }
}

//=============================================================================
// List Conversions
//=============================================================================

/// @brief List to seq.
/// @param list Source List, or NULL.
/// @return A new owning Seq containing List elements in index order.
void *rt_list_to_seq(void *list) {
    if (!list)
        return new_seq_result(0);
    int64_t len = 0;
    if (!list_view(list, &len))
        return NULL;
    return convert_indexed(
        list, len, list_at, new_seq_result, append_seq, "List.ToSeq: conversion failed");
}

/// @brief List to set.
/// @param list Source List, or NULL.
/// @return A new Set containing the List's unique elements.
void *rt_list_to_set(void *list) {
    if (!list)
        return new_set_result(0);
    int64_t len = 0;
    if (!list_view(list, &len))
        return NULL;
    return convert_indexed(
        list, len, list_at, new_set_result, append_set, "List.ToSet: conversion failed");
}

/// @brief List to stack.
/// @param list Source List, or NULL.
/// @return A new owning Stack with the first List element at the bottom.
void *rt_list_to_stack(void *list) {
    if (!list)
        return new_stack_result(0);
    int64_t len = 0;
    if (!list_view(list, &len))
        return NULL;
    return convert_indexed(
        list, len, list_at, new_stack_result, append_stack, "List.ToStack: conversion failed");
}

/// @brief List to queue.
/// @param list Source List, or NULL.
/// @return A new owning Queue with the first List element at the front.
void *rt_list_to_queue(void *list) {
    if (!list)
        return new_queue_result(0);
    int64_t len = 0;
    if (!list_view(list, &len))
        return NULL;
    return convert_indexed(
        list, len, list_at, new_queue_result, append_queue, "List.ToQueue: conversion failed");
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
    void *volatile seq = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        save_conversion_trap(saved_error, sizeof(saved_error), "Set.ToList: conversion failed");
        rt_trap_clear_recovery();
        release_temp_obj((void *)seq);
        rt_trap(saved_error);
        return NULL;
    }

    seq = rt_set_to_seq(set);
    if (!seq) {
        rt_trap_clear_recovery();
        return NULL;
    }
    void *list = rt_seq_to_list((void *)seq);
    rt_trap_clear_recovery();
    release_temp_obj((void *)seq);
    return list;
}

//=============================================================================
// Stack Conversions
//=============================================================================

/// @brief Stack to seq.
/// @param stack Source Stack, or NULL.
/// @return A new owning Seq ordered from stack bottom to top.
/// @note The validated source is borrowed and never mutated.
void *rt_stack_to_seq(void *stack) {
    if (!stack)
        return new_seq_result(0);
    int64_t len = 0;
    if (!stack_view(stack, &len))
        return NULL;
    return convert_indexed(
        stack, len, stack_at, new_seq_result, append_seq, "Stack.ToSeq: conversion failed");
}

/// @brief Stack to list.
/// @param stack Source Stack, or NULL.
/// @return A new List ordered from stack bottom to top.
void *rt_stack_to_list(void *stack) {
    if (!stack)
        return new_list_result(0);
    int64_t len = 0;
    if (!stack_view(stack, &len))
        return NULL;
    return convert_indexed(
        stack, len, stack_at, new_list_result, append_list, "Stack.ToList: conversion failed");
}

//=============================================================================
// Queue Conversions
//=============================================================================

/// @brief Queue to seq.
/// @param queue Source Queue, or NULL.
/// @return A new owning Seq ordered from queue front to back.
/// @note The validated source is borrowed and never mutated.
void *rt_queue_to_seq(void *queue) {
    if (!queue)
        return new_seq_result(0);
    int64_t len = 0;
    if (!queue_view(queue, &len))
        return NULL;
    return convert_indexed(
        queue, len, queue_at, new_seq_result, append_seq, "Queue.ToSeq: conversion failed");
}

/// @brief Queue to list.
/// @param queue Source Queue, or NULL.
/// @return A new List ordered from queue front to back.
void *rt_queue_to_list(void *queue) {
    if (!queue)
        return new_list_result(0);
    int64_t len = 0;
    if (!queue_view(queue, &len))
        return NULL;
    return convert_indexed(
        queue, len, queue_at, new_list_result, append_list, "Queue.ToList: conversion failed");
}

//=============================================================================
// Deque Conversions
//=============================================================================

/// @brief Deque to seq.
/// @param deque Source Deque, or NULL.
/// @return A new owning Seq ordered from deque front to back.
void *rt_deque_to_seq(void *deque) {
    if (!deque)
        return new_seq_result(0);
    int64_t len = 0;
    if (!deque_view(deque, &len))
        return NULL;
    return convert_indexed(
        deque, len, deque_at, new_seq_result, append_seq, "Deque.ToSeq: conversion failed");
}

/// @brief Deque to list.
/// @param deque Source Deque, or NULL.
/// @return A new List ordered from deque front to back.
void *rt_deque_to_list(void *deque) {
    if (!deque)
        return new_list_result(0);
    int64_t len = 0;
    if (!deque_view(deque, &len))
        return NULL;
    return convert_indexed(
        deque, len, deque_at, new_list_result, append_list, "Deque.ToList: conversion failed");
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
    if (!bag)
        return rt_set_new();

    void *volatile set = NULL;
    void *volatile items = NULL;
    void *volatile entry = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        save_conversion_trap(saved_error, sizeof(saved_error), "Bag.ToSet: conversion failed");
        rt_trap_clear_recovery();
        release_temp_obj((void *)entry);
        release_temp_obj((void *)items);
        release_temp_obj((void *)set);
        rt_trap(saved_error);
        return NULL;
    }

    items = rt_bag_items(bag);
    if (!items) {
        rt_trap_clear_recovery();
        return NULL;
    }
    int64_t len = 0;
    if (!seq_view((void *)items, &len))
        goto returning_failure;

    set = rt_set_new();
    if (!set) {
        rt_trap_clear_recovery();
        release_temp_obj((void *)items);
        return NULL;
    }
    for (int64_t index = 0; index < len; ++index) {
        void *element = seq_at((void *)items, index);
        if (!rt_string_is_handle(element)) {
            rt_trap("Bag.ToSet: item snapshot contains a non-string");
            goto returning_failure;
        }
        entry = rt_box_str((rt_string)element);
        if (!entry || !append_set((void *)set, (void *)entry))
            goto returning_failure;
        release_temp_obj((void *)entry);
        entry = NULL;
    }

    rt_trap_clear_recovery();
    release_temp_obj((void *)items);
    return (void *)set;

returning_failure:
    {
        char saved_error[256];
        save_conversion_trap(saved_error, sizeof(saved_error), "Bag.ToSet: conversion failed");
        rt_trap_clear_recovery();
        release_temp_obj((void *)entry);
        release_temp_obj((void *)items);
        release_temp_obj((void *)set);
        rt_trap(saved_error);
        return NULL;
    }
}

//=============================================================================
// Ring Conversions
//=============================================================================

/// @brief Ring to seq.
/// @param ring Source Ring, or NULL.
/// @return A new owning Seq in logical Ring index order.
void *rt_ring_to_seq(void *ring) {
    if (!ring)
        return new_seq_result(0);
    int64_t len = 0;
    if (!ring_view(ring, &len))
        return NULL;
    return convert_indexed(
        ring, len, ring_at, new_seq_result, append_seq, "Ring.ToSeq: conversion failed");
}

//=============================================================================
// Utility Functions
//=============================================================================

/// @brief Seq of.
/// @param count Number of following object pointers; non-positive counts
///        produce an empty Seq.
/// @param ... Exactly @p count element pointers.
/// @return A new owning Seq containing arguments in call order.
void *rt_seq_of(int64_t count, ...) {
    if (count <= 0)
        return new_seq_result(0);

    va_list args;
    va_start(args, count);
    void **elements = stage_variadic_pointers(count, &args);
    va_end(args);
    if (!elements) {
        rt_trap("Seq.Of: argument table allocation failed");
        return NULL;
    }
    return convert_native_pointers(
        elements, count, new_seq_result, append_seq, "Seq.Of: construction failed");
}

/// @brief List of.
/// @param count Number of following object pointers; non-positive counts
///        produce an empty List.
/// @param ... Exactly @p count element pointers.
/// @return A new List containing arguments in call order.
void *rt_list_of(int64_t count, ...) {
    if (count <= 0)
        return new_list_result(0);

    va_list args;
    va_start(args, count);
    void **elements = stage_variadic_pointers(count, &args);
    va_end(args);
    if (!elements) {
        rt_trap("List.Of: argument table allocation failed");
        return NULL;
    }
    return convert_native_pointers(
        elements, count, new_list_result, append_list, "List.Of: construction failed");
}

/// @brief Set of.
/// @param count Number of following object pointers; non-positive counts
///        produce an empty Set.
/// @param ... Exactly @p count element pointers.
/// @return A new Set populated in call order, with duplicates collapsed by Set
///         equality semantics.
void *rt_set_of(int64_t count, ...) {
    if (count <= 0)
        return new_set_result(0);

    va_list args;
    va_start(args, count);
    void **elements = stage_variadic_pointers(count, &args);
    va_end(args);
    if (!elements) {
        rt_trap("Set.Of: argument table allocation failed");
        return NULL;
    }
    return convert_native_pointers(
        elements, count, new_set_result, append_set, "Set.Of: construction failed");
}
