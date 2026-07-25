//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_pqueue.h
// Purpose: Runtime-backed priority queue (heap) for Zanna.Collections.Heap, supporting min-heap and
// max-heap ordering with push/pop/peek operations.
//
// Key invariants:
//   - Default is a min-heap; rt_pqueue_new_max creates a max-heap.
//   - rt_pqueue_peek returns the top element without removing it.
//   - Pop on empty queue traps immediately.
//   - Heap property is maintained after every push and pop operation.
//
// Ownership/Lifetime:
//   - Heap objects are runtime/GC-managed opaque pointers.
//   - Elements stored in the heap are retained while queued. Pop transfers the
//     retained value to the caller; peek returns an additional retained handle.
//
// Error conventions:
//   - Allocation failure → runtime trap, with NULL fallback return
//   - Pop/Peek on empty → rt_trap()
//
// Links: src/runtime/collections/rt_pqueue.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for the integer-priority runtime Heap collection.
/// @details The queue is a binary min-heap by default and can instead be
///          configured as a max-heap. Values are retained independently of
///          their signed priorities. Equal-priority entries are not stable
///          because no insertion sequence is stored.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty min-heap.
/// @return New runtime-managed Heap with smallest-priority-first ordering.
void *rt_pqueue_new(void);

/// @brief Create a new empty heap with specified ordering.
/// @param is_max Nonzero for largest-priority-first ordering; zero for
///               smallest-priority-first.
/// @return New runtime-managed Heap, or `NULL` after an allocation trap.
void *rt_pqueue_new_max(int8_t is_max);

/// @brief Get the number of elements in the heap.
/// @param obj Opaque Heap handle, or `NULL`.
/// @return Number of queued entries, or 0 for `NULL`.
int64_t rt_pqueue_len(void *obj);

/// @brief Check if the heap is empty.
/// @param obj Opaque Heap handle, or `NULL`.
/// @return 1 if empty, otherwise 0; `NULL` is empty.
int8_t rt_pqueue_is_empty(void *obj);

/// @brief Check if the heap is a max-heap.
/// @param obj Opaque Heap handle, or `NULL`.
/// @return 1 for a max-heap, 0 for a min-heap; `NULL` reports 0.
int8_t rt_pqueue_is_max(void *obj);

/// @brief Add an element with priority to the heap.
/// @details Retains @p val and restores heap order in O(log n). Equal-priority
///          entries have unspecified relative removal order.
/// @param obj Non-null Heap to mutate.
/// @param priority Signed integer ordering key.
/// @param val Runtime value retained while queued; may be `NULL`.
void rt_pqueue_push(void *obj, int64_t priority, void *val);

/// @brief Remove and return the highest priority element.
/// @details Removes the minimum priority from a min-heap or maximum priority
///          from a max-heap in O(log n). The stored retain transfers to the
///          caller. Null/empty heaps trap.
/// @param obj Non-null Heap.
/// @return Caller-owned transferred value, which may be a stored `NULL`.
void *rt_pqueue_pop(void *obj);

/// @brief Return the highest priority element without removing it.
/// @details Runs in O(1) and retains the result for the caller. Null/empty
///          heaps trap.
/// @param obj Non-null Heap.
/// @return Caller-owned retained top value, which may be `NULL`.
void *rt_pqueue_peek(void *obj);

/// @brief Try to remove and return the highest priority element.
/// @details Transfers the stored retain on success. A null result is ambiguous
///          with a queued null value; use @ref rt_pqueue_try_pop_option when
///          that distinction matters.
/// @param obj Heap handle, or `NULL`.
/// @return Caller-owned transferred value, or `NULL` if unavailable/null.
void *rt_pqueue_try_pop(void *obj);

/// @brief Try to remove the highest priority element as an Option.
/// @details Returns `None` when the heap is empty and `Some(value)` when an
///          element is removed. A stored NULL value is represented as
///          `Some(NULL)`, unlike @ref rt_pqueue_try_pop.
/// @param obj Heap handle, or `NULL`.
/// @return New runtime-managed Zanna.Option object.
void *rt_pqueue_try_pop_option(void *obj);

/// @brief Try to return the highest priority element without removing it.
/// @details Retains the result on success. A null result is ambiguous with a
///          queued null value; use @ref rt_pqueue_try_peek_option when that
///          distinction matters.
/// @param obj Heap handle, or `NULL`.
/// @return Caller-owned retained value, or `NULL` if unavailable/null.
void *rt_pqueue_try_peek(void *obj);

/// @brief Try to peek the highest priority element as an Option.
/// @details Returns `None` when the heap is empty and `Some(value)` when an
///          element exists. A stored NULL value is represented as `Some(NULL)`.
/// @param obj Heap handle, or `NULL`.
/// @return New runtime-managed Zanna.Option object.
void *rt_pqueue_try_peek_option(void *obj);

/// @brief Remove all elements from the heap.
/// @details Releases all queued values while preserving capacity and min/max
///          mode. A null heap is a no-op.
/// @param obj Heap handle, or `NULL`.
void rt_pqueue_clear(void *obj);

/// @brief Convert heap to Seq in priority order (works on an internal copy;
///        the original heap is preserved).
/// @details Produces values without priorities: ascending priority for a
///          min-heap and descending for a max-heap. Equal-priority ordering is
///          unspecified. A null heap traps.
/// @param obj Source Heap.
/// @return New runtime-managed owning Seq containing retained values.
void *rt_pqueue_to_seq(void *obj);

#ifdef __cplusplus
}
#endif
