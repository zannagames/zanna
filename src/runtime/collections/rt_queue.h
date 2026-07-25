//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_queue.h
// Purpose: Runtime-backed FIFO queue for Zanna.Collections.Queue, providing enqueue/dequeue/peek
// with automatic growth and O(1) operations.
//
// Key invariants:
//   - FIFO ordering: enqueue at back, dequeue from front.
//   - Dequeue and peek on empty queue trap immediately.
//   - Internal ring buffer doubles capacity on overflow.
//   - By default elements are borrowed; runtime conversion helpers can enable
//     retained-element ownership before inserting values.
//
// Ownership/Lifetime:
//   - Queue objects are runtime/GC-managed opaque pointers.
//   - Owned-element queues retain on push and release on pop/clear/finalize.
//   - Borrowing queues never extend element lifetime.
//
// Error conventions:
//   - Allocation failure → rt_trap()
//   - Pop/Peek on empty → rt_trap()
//   - TryPop on empty → returns NULL
//
// Links: src/runtime/collections/rt_queue.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for the growable FIFO Queue collection.
/// @details A queue uses a circular pointer buffer and starts in borrowing
///          mode. Retained-element ownership can be enabled only while empty.
///          This choice controls GC traversal and result lifetime but not FIFO
///          order or circular-buffer behavior.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty queue with default capacity.
/// @details The queue starts in borrowing mode with capacity for sixteen
///          elements.
/// @return New runtime-managed Queue.
void *rt_queue_new(void);

/// @brief Enable or disable retained-element ownership for an empty queue.
/// @details Traps if a mode change is requested while non-empty. A null queue
///          is a no-op.
/// @param obj Queue handle, or `NULL`.
/// @param owns Nonzero to retain/trace/release elements; zero to borrow them.
void rt_queue_set_owns_elements(void *obj, int8_t owns);

/// @brief Return whether the queue retains its elements.
/// @param obj Queue handle, or `NULL`.
/// @return 1 in owning mode, otherwise 0; `NULL` reports 0.
int8_t rt_queue_owns_elements(void *obj);

/// @brief Get the number of elements in the queue.
/// @param obj Queue handle, or `NULL`.
/// @return Number of live FIFO entries, or 0 for `NULL`.
int64_t rt_queue_len(void *obj);

/// @brief Check if the queue is empty.
/// @param obj Queue handle, or `NULL`.
/// @return 1 if empty, otherwise 0; `NULL` is empty.
int8_t rt_queue_is_empty(void *obj);

/// @brief Push an element to the back of the queue.
/// @details Runs in amortized O(1), growing and linearizing the circular
///          buffer when full. Owning queues retain @p elem; borrowing queues
///          store it raw. A null queue traps.
/// @param obj Non-null Queue to mutate.
/// @param elem Value to enqueue; may be `NULL`.
void rt_queue_push(void *obj, void *elem);

/// @brief Pop and return the front element from the queue.
/// @details Traps for null/empty queues. Owning queues return a caller-retained
///          value; borrowing queues return the raw stored pointer.
/// @param obj Non-null Queue.
/// @return Removed front value, which may be a stored `NULL`.
void *rt_queue_pop(void *obj);

/// @brief Return the front element without removing it.
/// @details Traps for null/empty queues and creates no additional retain. In
///          owning mode the result is borrowed from the queue.
/// @param obj Non-null Queue.
/// @return Borrowed front value, which may be `NULL`.
void *rt_queue_peek(void *obj);

/// @brief Remove all elements from the queue.
/// @details Releases live elements in owning mode, resets the circular
///          indices, and preserves capacity/ownership mode. A null queue is a
///          no-op.
/// @param obj Queue handle, or `NULL`.
void rt_queue_clear(void *obj);

/// @brief Check if the queue contains an equal element.
/// @details Uses boxed-value equality for boxed numeric/string values and
///          identity for ordinary runtime objects.
/// @param obj Queue handle, or `NULL`.
/// @param elem Value to find; may be `NULL`.
/// @return 1 if found, 0 otherwise.
int8_t rt_queue_has(void *obj, void *elem);

/// @brief Convert the queue to a List (front-to-back order).
/// @details Implemented by the collection-conversion layer without mutating
///          the source. The result independently owns/retains its values.
/// @param obj Queue handle, or `NULL`.
/// @return New runtime-managed List; empty for `NULL`.
void *rt_queue_to_list(void *obj);

/// @brief Pop the front element, or return NULL if empty (no trap).
/// @details Ownership follows @ref rt_queue_pop. A stored null is ambiguous
///          with absence; use the Option variant when that matters.
/// @param obj Queue handle, or `NULL`.
/// @return Removed front value, or `NULL` if unavailable/null-valued.
void *rt_queue_try_pop(void *obj);

/// @brief Pop the front element as an Option.
/// @details Returns `None` when the queue is empty and `Some(value)` when an
///          element is removed. A stored NULL value is represented as
///          `Some(NULL)`, unlike @ref rt_queue_try_pop.
/// @param obj Queue handle, or `NULL`.
/// @return New runtime-managed Zanna.Option object.
void *rt_queue_try_pop_option(void *obj);

/// @brief Create a shallow copy of the queue.
/// @details Preserves front-to-back order and ownership mode. An owning clone
///          independently retains all values; a borrowing clone copies raw
///          pointers.
/// @param obj Source Queue handle, or `NULL`.
/// @return New runtime-managed clone, or empty borrowing Queue for `NULL`.
void *rt_queue_clone(void *obj);

#ifdef __cplusplus
}
#endif
