//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/3d/rt_g3d_commit_queue.h
// Purpose: Internal main-thread commit queue for Graphics3D/Game3D worker jobs.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the concurrent Graphics3D worker-to-main-thread commit queue.
/// @details Successfully enqueued payloads transfer to a FIFO that either invokes their commit on
///   the main thread or invokes optional cancellation during teardown; drains support independent
///   item-count and opaque-cost budgets.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Main-thread callback invoked for a successfully drained commit.
/// @param user_data Opaque payload transferred by the producer.
typedef void (*rt_g3d_commit_fn)(void *user_data);

/// @brief Cleanup callback invoked when successfully queued work is discarded before draining.
/// @param user_data Opaque payload whose ownership must be released.
typedef void (*rt_g3d_commit_cancel_fn)(void *user_data);

/// @brief Default opaque cost assigned to ordinary commit work.
#define RT_G3D_COMMIT_COST_UNIT 1ull
/// @brief Sentinel disabling cost-based drain limits.
#define RT_G3D_COMMIT_COST_UNLIMITED UINT64_MAX

/// @brief Create an internal FIFO queue for worker-produced main-thread commits.
/// @return A new opaque queue handle, or `NULL` when queue allocation fails.
void *rt_g3d_commit_queue_new(void);

/// @brief Atomically close the queue to new producer submissions without destroying it.
/// @details This is the first half of concurrent teardown: call Close while producers may still
/// hold the handle, join or otherwise stop those producers, then call Free. Already queued work
/// remains owned by the queue and is drained or cancelled normally.
/// @param queue Queue to close; ignored when null or already closed.
void rt_g3d_commit_queue_close(void *queue);

/// @brief Free the queue, discarding any pending commits without running them.
/// @details Free is the reclamation half of teardown and must not overlap any operation that may
/// dereference the handle. When producers can race shutdown, Close the queue first, stop/join all
/// producers, and only then call Free.
/// @param queue Queue to close and destroy; pending items receive optional cancellation callbacks.
void rt_g3d_commit_queue_free(void *queue);

/// @brief Enqueue a commit callback. The callback runs only when drained.
/// @param queue Queue receiving the item.
/// @param fn Non-null main-thread callback.
/// @param user_data Opaque payload whose ownership transfers only on success.
/// @return Non-zero when queued; zero for invalid input, shutdown, or allocation failure.
int8_t rt_g3d_commit_queue_enqueue(void *queue, rt_g3d_commit_fn fn, void *user_data);

/// @brief Enqueue a commit callback with an estimated main-thread cost.
/// @details The cost is an opaque budget unit; use RT_G3D_COMMIT_COST_UNIT for default work and
/// decoded-byte counts for asset uploads that should honor upload budgets.
/// @param queue Queue receiving the item.
/// @param fn Non-null main-thread callback.
/// @param user_data Opaque payload whose ownership transfers only on success.
/// @param cost Estimated main-thread work used by budgeted drains.
/// @return Non-zero when queued; zero for invalid input, shutdown, or allocation failure.
int8_t rt_g3d_commit_queue_enqueue_cost(void *queue,
                                        rt_g3d_commit_fn fn,
                                        void *user_data,
                                        uint64_t cost);

/// @brief Enqueue a commit callback with an ownership cleanup callback for discarded work.
/// @details `cancel_fn` runs only if the queue is closed/freed before `fn` drains. This lets worker
///          jobs hand retained runtime handles or malloc-owned staging buffers to the queue without
///          leaking them during shutdown. A successfully drained item never calls `cancel_fn`.
/// @param queue Queue receiving the item.
/// @param fn Non-null main-thread callback.
/// @param user_data Opaque payload whose ownership transfers only on success.
/// @param cost Estimated main-thread work used by budgeted drains.
/// @param cancel_fn Optional cleanup for successfully queued work discarded during teardown.
/// @return Non-zero when queued. On zero, ownership remains with the caller.
int8_t rt_g3d_commit_queue_enqueue_cost_cancel(void *queue,
                                               rt_g3d_commit_fn fn,
                                               void *user_data,
                                               uint64_t cost,
                                               rt_g3d_commit_cancel_fn cancel_fn);

/// @brief Force the next @p count enqueue-wrapper allocations to fail in tests.
/// @details This process-global fault-injection hook is internal to the runtime test surface.
///          Passing zero disables it. It does not affect queue or payload allocations.
/// @param count Number of subsequent wrapper allocations to suppress; non-positive input disables
///   injection.
void rt_g3d_commit_queue_test_fail_next_allocations(int32_t count);

/// @brief Drain up to @p max_items callbacks on the main thread.
/// @details `max_items <= 0` drains until the queue is empty.
/// @param queue Queue to drain.
/// @param max_items Maximum callback count, or a non-positive value for no item limit.
/// @return Number of callbacks run.
int64_t rt_g3d_commit_queue_drain(void *queue, int64_t max_items);

/// @brief Drain callbacks up to both an item count and cost budget.
/// @details `max_items <= 0` means no item limit. `max_cost == 0` drains only zero-cost work,
/// which lets callers pause positive-cost uploads without starving bookkeeping callbacks.
/// `max_cost == UINT64_MAX` means no cost limit. If the first pending item is larger than a
/// positive budget, it drains alone so oversized assets cannot deadlock the queue. This is a
/// single-consumer, main-thread drain; worker threads may enqueue concurrently but must not drain.
/// @param queue Queue to drain.
/// @param max_items Maximum callback count, or a non-positive value for no item limit.
/// @param max_cost Maximum cumulative opaque cost, including documented sentinel behavior.
/// @return Number of callbacks run.
int64_t rt_g3d_commit_queue_drain_budget(void *queue, int64_t max_items, uint64_t max_cost);

/// @brief Approximate number of pending commits; concurrent enqueue/drain can change it instantly.
/// @param queue Queue to query.
/// @return A non-negative approximate item count, or zero for invalid input.
int64_t rt_g3d_commit_queue_pending(void *queue);

/// @brief Total number of callbacks accepted by the queue.
/// @param queue Queue to query.
/// @return The non-negative monotonic submitted count, or zero for invalid input.
int64_t rt_g3d_commit_queue_submitted(void *queue);

/// @brief Total number of callbacks run by `rt_g3d_commit_queue_drain`.
/// @param queue Queue to query.
/// @return The non-negative monotonic drained count, or zero for invalid input.
int64_t rt_g3d_commit_queue_drained(void *queue);

#ifdef __cplusplus
}
#endif
