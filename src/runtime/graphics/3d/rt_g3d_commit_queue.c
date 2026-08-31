//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/3d/rt_g3d_commit_queue.c
// Purpose: Internal main-thread commit queue for Graphics3D/Game3D worker jobs.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the concurrent Graphics3D worker-to-main-thread commit queue.
/// @details Producers enqueue callbacks with ownership cleanup and cost metadata; the main thread
///   drains FIFO work under count and cost budgets, while teardown cancels payloads that never ran
///   and atomic counters expose approximate progress.

#include "rt_g3d_commit_queue.h"

#include "rt_concqueue.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_trap.h"

#include <stdlib.h>

/// @brief One queued commit: the callback, its user data, and its budget cost.
typedef struct rt_g3d_commit_item {
    rt_g3d_commit_fn fn;
    rt_g3d_commit_cancel_fn cancel_fn;
    void *user_data;
    uint64_t cost;
} rt_g3d_commit_item;

/// @brief Queue state: a concurrent FIFO of items plus submit/drain counters.
typedef struct rt_g3d_commit_queue {
    void *items;
    int64_t submitted;
    int64_t drained;
} rt_g3d_commit_queue;

/// @brief Test-only countdown for deterministic enqueue-wrapper allocation failures.
static volatile int g_rt_g3d_commit_queue_fail_allocations;

/// @brief Consume one requested allocation failure without racing concurrent producers.
/// @return 1 when this allocation attempt must fail, or 0 when normal allocation may proceed.
static int rt_g3d_commit_queue_should_fail_allocation(void) {
    int remaining = rt_atomic_load_i32(&g_rt_g3d_commit_queue_fail_allocations, __ATOMIC_ACQUIRE);
    while (remaining > 0) {
        int expected = remaining;
        if (rt_atomic_compare_exchange_i32(&g_rt_g3d_commit_queue_fail_allocations,
                                           &expected,
                                           remaining - 1,
                                           __ATOMIC_ACQ_REL,
                                           __ATOMIC_ACQUIRE))
            return 1;
        remaining = expected;
    }
    return 0;
}

/// @brief Configure deterministic allocation failure injection for unit tests.
/// @param count Number of subsequent item-wrapper allocations to suppress; non-positive input
///   clears failure injection.
void rt_g3d_commit_queue_test_fail_next_allocations(int32_t count) {
    rt_atomic_store_i32(
        &g_rt_g3d_commit_queue_fail_allocations, count > 0 ? count : 0, __ATOMIC_RELEASE);
}

/// @brief Allocate a commit queue wrapping a fresh concurrent FIFO.
/// @return Opaque queue handle, or NULL if either allocation fails.
void *rt_g3d_commit_queue_new(void) {
    rt_g3d_commit_queue *queue = (rt_g3d_commit_queue *)calloc(1, sizeof(rt_g3d_commit_queue));
    if (!queue)
        return NULL;
    queue->items = rt_concqueue_new();
    if (!queue->items) {
        free(queue);
        return NULL;
    }
    return queue;
}

/// @brief Close the producer side of a commit queue while preserving its allocation.
/// @details `rt_concqueue_close` provides the atomic publish point: racing enqueue attempts either
/// complete before closure or fail without transferring payload ownership. Keeping the wrapper
/// alive lets callers join every producer before the separate reclamation step.
void rt_g3d_commit_queue_close(void *obj) {
    rt_g3d_commit_queue *queue = (rt_g3d_commit_queue *)obj;
    if (queue && queue->items)
        rt_concqueue_close(queue->items);
}

/// @brief Drain and free every pending item without running its callback.
/// @details Used during teardown so worker-produced commits still in the queue are
///          reclaimed rather than leaked; the callbacks are intentionally skipped.
/// @param queue Queue whose pending payloads receive their optional cancellation callbacks.
static void rt_g3d_commit_queue_discard_pending(rt_g3d_commit_queue *queue) {
    if (!queue || !queue->items)
        return;
    for (;;) {
        rt_g3d_commit_item *item = (rt_g3d_commit_item *)rt_concqueue_try_dequeue(queue->items);
        if (!item)
            break;
        if (item->cancel_fn)
            item->cancel_fn(item->user_data);
        free(item);
    }
}

/// @brief Close the queue, discard any pending commits, and free all backing memory.
/// @param obj Queue handle to destroy; ignored when `NULL`.
void rt_g3d_commit_queue_free(void *obj) {
    rt_g3d_commit_queue *queue = (rt_g3d_commit_queue *)obj;
    if (!queue)
        return;
    if (queue->items) {
        rt_g3d_commit_queue_close(queue);
        rt_g3d_commit_queue_discard_pending(queue);
        if (rt_obj_release_check0(queue->items))
            rt_obj_free(queue->items);
        queue->items = NULL;
    }
    free(queue);
}

/// @brief Enqueue a commit callback tagged with a main-thread cost estimate and cleanup hook.
/// @details If allocation fails, ownership remains with the caller and zero is returned. If the
///          queue is later closed before the item drains, `cancel_fn` receives `user_data` so
///          payload ownership can be released.
/// @param obj Commit queue receiving the item.
/// @param fn Non-null main-thread callback invoked with @p user_data after dequeue.
/// @param user_data Opaque callback payload; ownership transfers only when enqueue succeeds.
/// @param cost Estimated main-thread cost used by budgeted drains.
/// @param cancel_fn Optional cleanup invoked for successfully queued payloads discarded at
///   teardown.
/// @return 1 when queued; 0 for invalid input, a closed queue, or allocation failure.
int8_t rt_g3d_commit_queue_enqueue_cost_cancel(void *obj,
                                               rt_g3d_commit_fn fn,
                                               void *user_data,
                                               uint64_t cost,
                                               rt_g3d_commit_cancel_fn cancel_fn) {
    rt_g3d_commit_queue *queue = (rt_g3d_commit_queue *)obj;
    if (!queue || !queue->items || !fn)
        return 0;
    if (rt_concqueue_get_is_closed(queue->items))
        return 0;
    rt_g3d_commit_item *item = NULL;
    if (!rt_g3d_commit_queue_should_fail_allocation())
        item = (rt_g3d_commit_item *)malloc(sizeof(rt_g3d_commit_item));
    if (!item)
        return 0;
    item->fn = fn;
    item->cancel_fn = cancel_fn;
    item->user_data = user_data;
    item->cost = cost;
    if (!rt_concqueue_try_enqueue(queue->items, item)) {
        free(item);
        return 0;
    }
    rt_atomic_fetch_add_i64(&queue->submitted, 1, __ATOMIC_RELAXED);
    return 1;
}

/// @brief Enqueue a commit callback tagged with a main-thread cost estimate.
/// @param obj Commit queue receiving the item.
/// @param fn Non-null main-thread callback.
/// @param user_data Opaque callback payload; ownership transfers only when enqueue succeeds.
/// @param cost Estimated main-thread cost used by budgeted drains.
/// @return 1 when queued, or 0 when validation, closure, or allocation prevents enqueue.
int8_t rt_g3d_commit_queue_enqueue_cost(void *obj,
                                        rt_g3d_commit_fn fn,
                                        void *user_data,
                                        uint64_t cost) {
    return rt_g3d_commit_queue_enqueue_cost_cancel(obj, fn, user_data, cost, NULL);
}

/// @brief Enqueue a commit callback with the default unit cost (1).
/// @param obj Commit queue receiving the item.
/// @param fn Non-null main-thread callback.
/// @param user_data Opaque callback payload; ownership transfers only when enqueue succeeds.
/// @return 1 when queued, or 0 when validation, closure, or allocation prevents enqueue.
int8_t rt_g3d_commit_queue_enqueue(void *obj, rt_g3d_commit_fn fn, void *user_data) {
    return rt_g3d_commit_queue_enqueue_cost(obj, fn, user_data, RT_G3D_COMMIT_COST_UNIT);
}

/// @brief Saturating unsigned add — clamps to UINT64_MAX instead of overflowing.
/// @details Keeps the running cost budget monotonic even when per-item costs sum
///          past 64 bits, so the drain loop's budget comparison never wraps around.
/// @param a First non-negative cost.
/// @param b Second non-negative cost.
/// @return The mathematical sum when representable, otherwise `UINT64_MAX`.
static uint64_t rt_g3d_commit_queue_cost_add(uint64_t a, uint64_t b) {
    if (UINT64_MAX - a < b)
        return UINT64_MAX;
    return a + b;
}

/// @brief Run pending commits on the main thread under item-count and cost budgets.
/// @details Peeks each item's cost before dequeuing, so a commit runs only when it
///          fits the remaining cost budget. A zero budget drains only zero-cost work; an unlimited
///          budget disables cost limiting. The first item always runs even if it exceeds a positive
///          budget (the `count > 0` guard), so an oversized asset commit cannot stall the queue
///          forever. Asserts it is called on the main thread.
/// @param obj Commit queue to drain.
/// @param max_items Maximum callbacks to run; non-positive values impose no count limit.
/// @param max_cost Maximum cumulative cost, or `RT_G3D_COMMIT_COST_UNLIMITED` for no cost limit.
/// @return Number of commits actually run.
int64_t rt_g3d_commit_queue_drain_budget(void *obj, int64_t max_items, uint64_t max_cost) {
    rt_g3d_commit_queue *queue = (rt_g3d_commit_queue *)obj;
    int no_cost_limit;
    if (!queue || !queue->items)
        return 0;
    RT_ASSERT_MAIN_THREAD();

    int64_t count = 0;
    uint64_t cost = 0;
    no_cost_limit = (max_cost == UINT64_MAX);
    while (max_items <= 0 || count < max_items) {
        rt_g3d_commit_item *peek = (rt_g3d_commit_item *)rt_concqueue_peek(queue->items);
        if (!peek)
            break;
        uint64_t item_cost = peek->cost;
        if (!no_cost_limit && item_cost > 0) {
            uint64_t next_cost = rt_g3d_commit_queue_cost_add(cost, item_cost);
            if (next_cost > max_cost) {
                if (count > 0 || max_cost == 0)
                    break;
            }
        }
        rt_g3d_commit_item *item = (rt_g3d_commit_item *)rt_concqueue_try_dequeue(queue->items);
        if (!item)
            break;
        rt_g3d_commit_fn fn = item->fn;
        void *user_data = item->user_data;
        item_cost = item->cost;
        free(item);
        if (fn) {
            fn(user_data);
            count++;
            cost = rt_g3d_commit_queue_cost_add(cost, item_cost);
        }
    }
    if (count > 0)
        rt_atomic_fetch_add_i64(&queue->drained, count, __ATOMIC_RELAXED);
    return count;
}

/// @brief Drain up to @p max_items commits with no cost budget.
/// @param obj Commit queue to drain on the main thread.
/// @param max_items Maximum callbacks to run; non-positive values drain until empty.
/// @return Number of callbacks run.
int64_t rt_g3d_commit_queue_drain(void *obj, int64_t max_items) {
    return rt_g3d_commit_queue_drain_budget(obj, max_items, RT_G3D_COMMIT_COST_UNLIMITED);
}

/// @brief Approximate count of commits still waiting to run.
/// @param obj Commit queue to query.
/// @return A non-negative approximate queue length, or zero for an invalid queue.
int64_t rt_g3d_commit_queue_pending(void *obj) {
    rt_g3d_commit_queue *queue = (rt_g3d_commit_queue *)obj;
    if (!queue || !queue->items)
        return 0;
    int64_t pending = rt_concqueue_len(queue->items);
    return pending > 0 ? pending : 0;
}

/// @brief Total commits ever accepted by the queue (monotonic counter).
/// @param obj Commit queue to query.
/// @return The non-negative submitted count, or zero for an invalid queue.
int64_t rt_g3d_commit_queue_submitted(void *obj) {
    rt_g3d_commit_queue *queue = (rt_g3d_commit_queue *)obj;
    if (!queue)
        return 0;
    int64_t submitted = rt_atomic_load_i64(&queue->submitted, __ATOMIC_RELAXED);
    return submitted > 0 ? submitted : 0;
}

/// @brief Total commits ever run by a drain call (monotonic counter).
/// @param obj Commit queue to query.
/// @return The non-negative drained count, or zero for an invalid queue.
int64_t rt_g3d_commit_queue_drained(void *obj) {
    rt_g3d_commit_queue *queue = (rt_g3d_commit_queue *)obj;
    if (!queue)
        return 0;
    int64_t drained = rt_atomic_load_i64(&queue->drained, __ATOMIC_RELAXED);
    return drained > 0 ? drained : 0;
}
