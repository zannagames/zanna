//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_concqueue.h
// Purpose: Thread-safe concurrent queue with blocking and non-blocking dequeue operations, using a
// mutex and condition variable for producer/consumer synchronization.
//
// Key invariants:
//   - FIFO ordering; mutex-protected for thread safety.
//   - rt_concqueue_dequeue blocks on a condition variable when the queue is empty.
//   - rt_concqueue_try_dequeue returns NULL immediately if the queue is empty.
//   - rt_concqueue_close wakes all blocked dequeue callers with NULL once the
//     queue is empty and rejects future enqueue attempts.
//   - The strict enqueue traps on node-allocation failure or a closed queue;
//     the internal status-returning enqueue leaves caller ownership unchanged.
//
// Ownership/Lifetime:
//   - ConcQueue objects are heap-allocated; caller is responsible for lifetime management.
//   - Enqueued values are retained; dequeued and peeked values are returned
//     with a stable retained reference.
//
// Links: src/runtime/threads/rt_concqueue.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Unbounded concurrent FIFO queue C ABI.
/// @details Enqueue retains runtime-managed values. Successful dequeue
///          operations transfer that stored reference, while Peek creates an
///          additional retained reference. Closing rejects producers, wakes
///          consumers, and still permits queued items to drain.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create an empty queue with platform mutex and condition state.
/// @return New runtime-managed ConcurrentQueue, or null after a construction trap.
void *rt_concqueue_new(void);

/// @brief Read the queued-node count under the queue mutex.
/// @param obj ConcurrentQueue handle; null reports zero.
/// @return Exact count at the instant of the locked read.
int64_t rt_concqueue_len(void *obj);

/// @brief Test whether the locked queue count is zero.
/// @param obj ConcurrentQueue handle; null is treated as empty.
/// @return 1 when empty, otherwise 0.
int8_t rt_concqueue_is_empty(void *obj);

/// @brief Check whether the queue has been closed.
/// @param obj ConcurrentQueue handle.
/// @return 1 if closed; null is treated as closed.
int8_t rt_concqueue_get_is_closed(void *obj);

/// @brief Add item to back of queue (thread-safe).
/// @details Null items are accepted. Allocation, retain, count-overflow, and
///          closed-queue failures trap without consuming the caller's reference.
/// @param obj ConcurrentQueue handle; null is ignored.
/// @param item Runtime-managed value to retain, or null.
void rt_concqueue_enqueue(void *obj, void *item);

/// @brief Try to add an item without trapping for node-allocation failure or a closed queue.
/// @details The item is retained only on success. Null items are rejected;
///          invalid non-null queue handles and retain failures may still trap.
/// @param obj ConcurrentQueue handle.
/// @param item Nonnull runtime-managed value retained on success.
/// @return 1 when enqueued, or 0 for null input, allocation/count failure, or close.
int8_t rt_concqueue_try_enqueue(void *obj, void *item);

/// @brief Remove item from front of queue (non-blocking).
/// @param obj ConcurrentQueue handle; null behaves as empty.
/// @return Transferred oldest value, or null for empty or a queued null.
void *rt_concqueue_try_dequeue(void *obj);

/// @brief Remove item from front of queue as an Option (non-blocking).
/// @details Returns `None` when the queue is empty and `Some(value)` when an
///          item is removed. A queued NULL value is represented as
///          `Some(NULL)`, unlike @ref rt_concqueue_try_dequeue.
/// @param obj ConcurrentQueue handle.
/// @return Runtime-managed `Some(value)` or `None`.
void *rt_concqueue_try_dequeue_option(void *obj);

/// @brief Remove item from front of queue (blocking).
/// @details Waits through spurious wakeups until an item appears or the queue
///          becomes closed and empty.
/// @param obj ConcurrentQueue handle; null behaves as closed and empty.
/// @return Transferred oldest value, or null for close/empty or a queued null.
void *rt_concqueue_dequeue(void *obj);

/// @brief Remove item with timeout.
/// @details Nonpositive timeouts use the nonblocking dequeue. POSIX prefers
///          monotonic deadlines; Windows uses its monotonic tick counter.
/// @param obj ConcurrentQueue handle; null behaves as empty.
/// @param timeout_ms Condition-wait timeout in milliseconds.
/// @return Transferred oldest value, or null for timeout, close, or a null item.
void *rt_concqueue_dequeue_timeout(void *obj, int64_t timeout_ms);

/// @brief Peek at front item without removing (non-blocking).
/// @param obj ConcurrentQueue handle; null behaves as empty.
/// @return Newly retained front value, or null for empty or a null-valued head.
void *rt_concqueue_peek(void *obj);

/// @brief Detach all queued nodes atomically and release their values after unlocking.
/// @param obj ConcurrentQueue handle; null is ignored.
void rt_concqueue_clear(void *obj);

/// @brief Close the queue and wake blocked dequeuers.
/// @details Once closed, dequeue operations return NULL when the queue becomes
///          empty and strict enqueue attempts trap. Closing is idempotent.
/// @param obj ConcurrentQueue handle; null is ignored.
void rt_concqueue_close(void *obj);

#ifdef __cplusplus
}
#endif
