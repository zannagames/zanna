//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTThreadPoolTests.cpp
// Purpose: Validate asynchronous thread-pool execution, shutdown, ownership,
//          construction rollback, and GC traversal.
// Key invariants: One shutdown caller completes cleanup, task arguments have
//                 balanced ownership, and worker self-finalization cannot leak.
// Ownership/Lifetime: Each test waits for or shuts down all workers before
//                     releasing the pool and any separately retained arguments.
// Links: src/runtime/threads/rt_threadpool.c, docs/zannalib/threads.md,
//        docs/adr/0133-runtime-concurrency-and-collection-hardening.md
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_heap.h"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <setjmp.h>
#include <thread>

extern "C" {
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_threadpool.h"
#include "rt_threads.h"

/// @brief Vm_trap.
void vm_trap(const char *msg) {
    fprintf(stderr, "TRAP: %s\n", msg);
    rt_abort(msg);
}

void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);
}

//=============================================================================
// Shared test helpers (use C++ atomic since SafeI64 not on Windows)
//=============================================================================

static std::atomic<int64_t> g_counter{0};
static std::atomic<int> g_cycle_task_started{0};
static std::atomic<int> g_cycle_task_release{0};
static std::atomic<int> g_shutdown_task_started{0};
static std::atomic<int> g_shutdown_task_release{0};
static std::atomic<int> g_fallback_task_started{0};
static std::atomic<int> g_fallback_task_release{0};

static void init_counter() {
    g_counter.store(0);
}

extern "C" {
static void increment_task(void *arg) {
    (void)arg;
    g_counter.fetch_add(1);
}

static void slow_task(void *arg) {
    (void)arg;
    rt_thread_sleep(50);
    g_counter.fetch_add(1);
}

static void trap_task(void *arg) {
    (void)arg;
    g_counter.fetch_add(1);
    rt_trap("pool task trap");
}

/// @brief Hold the sole worker so a following owned task remains in the queue.
/// @details The pool-cycle regression releases this task only after synchronous collection
///          has returned, proving the collector never waits for a worker while holding its
///          exclusive managed-graph barrier.
static void hold_cycle_worker(void *arg) {
    (void)arg;
    g_cycle_task_started.store(1, std::memory_order_release);
    while (!g_cycle_task_release.load(std::memory_order_acquire))
        std::this_thread::yield();
}

/// @brief Block a pool worker until the concurrent-shutdown test releases it.
/// @param arg Unused.
static void hold_shutdown_worker(void *arg) {
    (void)arg;
    g_shutdown_task_started.store(1, std::memory_order_release);
    while (!g_shutdown_task_release.load(std::memory_order_acquire))
        std::this_thread::yield();
}

/// @brief Hold a worker until the worker-exit cleanup fallback is armed.
/// @param arg Unused.
static void hold_fallback_worker(void *arg) {
    (void)arg;
    g_fallback_task_started.store(1, std::memory_order_release);
    while (!g_fallback_task_release.load(std::memory_order_acquire))
        std::this_thread::yield();
}

static void shutdown_pool(void *arg) {
    rt_threadpool_shutdown(arg);
}

static void shutdown_pool_now(void *arg) {
    rt_threadpool_shutdown_now(arg);
}
}

static int call_traps(void (*fn)(void *), void *arg) {
    jmp_buf recovery;
    volatile int trapped = 0;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        fn(arg);
    } else {
        trapped = 1;
    }
    rt_trap_clear_recovery();
    return trapped;
}

template <typename Fn> static bool expect_trap(Fn fn) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        fn();
        rt_trap_clear_recovery();
        return false;
    }
    rt_trap_clear_recovery();
    return true;
}

struct PoolTaskMirror {
    void (*callback)(void *);
    void *arg;
    int8_t owns_arg;
    PoolTaskMirror *next;
};

struct PoolWorkerMirror {
    void *thread;
    void *pool;
};

struct PoolMirror {
    void *monitor;
    PoolTaskMirror *queue_head;
    PoolTaskMirror *queue_tail;
    PoolWorkerMirror *workers;
    int64_t worker_count;
    int64_t pending_count;
    int64_t active_count;
    int64_t error_count;
    char last_error[512];
    int shutdown;
    int shutdown_now;
    int8_t shutdown_joining;
    int8_t shutdown_complete;
    int cleanup_scheduled;
};

//=============================================================================
// Creation and properties
//=============================================================================

static void test_new() {
    void *pool = rt_threadpool_new(4);
    assert(pool != NULL);
    assert(rt_threadpool_get_size(pool) == 4);
    assert(rt_threadpool_get_pending(pool) == 0);
    assert(rt_threadpool_get_is_shutdown(pool) == 0);
    rt_threadpool_shutdown(pool);
}

static void test_new_clamp_min() {
    void *pool = rt_threadpool_new(0);
    assert(pool != NULL);
    assert(rt_threadpool_get_size(pool) == 1); // Clamped to 1
    rt_threadpool_shutdown(pool);
}

static void test_new_clamp_negative() {
    void *pool = rt_threadpool_new(-5);
    assert(pool != NULL);
    assert(rt_threadpool_get_size(pool) == 1); // Clamped to 1
    rt_threadpool_shutdown(pool);
}

//=============================================================================
// Task submission and execution
//=============================================================================

static void test_submit_and_wait() {
    init_counter();
    void *pool = rt_threadpool_new(2);

    int i;
    for (i = 0; i < 10; i++) {
        int8_t ok = rt_threadpool_submit(pool, (void *)increment_task, NULL);
        assert(ok == 1);
    }

    rt_threadpool_wait(pool);
    assert(g_counter.load() == 10);

    rt_threadpool_shutdown(pool);
}

static void test_submit_after_shutdown() {
    void *pool = rt_threadpool_new(2);
    rt_threadpool_shutdown(pool);

    assert(rt_threadpool_get_is_shutdown(pool) == 1);
    assert(rt_threadpool_submit(pool, (void *)increment_task, NULL) == 0);
}

static void test_submit_null_callback() {
    void *pool = rt_threadpool_new(2);
    assert(rt_threadpool_submit(pool, NULL, NULL) == 0);
    rt_threadpool_shutdown(pool);
}

//=============================================================================
// Wait with timeout
//=============================================================================

static void test_wait_for_success() {
    init_counter();
    void *pool = rt_threadpool_new(2);

    int i;
    for (i = 0; i < 5; i++)
        rt_threadpool_submit(pool, (void *)increment_task, NULL);

    int8_t done = rt_threadpool_wait_for(pool, 5000);
    assert(done == 1);
    assert(g_counter.load() == 5);

    rt_threadpool_shutdown(pool);
}

static void test_wait_for_immediate_check() {
    void *pool = rt_threadpool_new(2);
    PoolMirror *state = (PoolMirror *)pool;

    // Idle-worker monitor contention must not be mistaken for outstanding work.
    rt_monitor_enter(state->monitor);
    std::atomic<int> done{-1};
    std::thread poller([&]() { done.store(rt_threadpool_wait_for(pool, 0)); });
    poller.join();
    rt_monitor_exit(state->monitor);
    assert(done.load() == 1);

    rt_threadpool_shutdown(pool);
}

static void test_wait_for_huge_timeout_pending_task() {
    init_counter();
    void *pool = rt_threadpool_new(1);
    rt_threadpool_submit(pool, (void *)slow_task, NULL);

    int8_t done = rt_threadpool_wait_for(pool, INT64_MAX);
    assert(done == 1);
    assert(g_counter.load() == 1);

    rt_threadpool_shutdown(pool);
}

static void test_wait_for_timeout_budget() {
    init_counter();
    void *pool = rt_threadpool_new(1);
    rt_threadpool_submit(pool, (void *)slow_task, NULL);

    auto start = std::chrono::steady_clock::now();
    int8_t done = rt_threadpool_wait_for(pool, 20);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    assert(done == 0);
    assert(elapsed >= 10);
    assert(elapsed < 150);

    rt_threadpool_wait(pool);
    rt_threadpool_shutdown(pool);
}

static void test_task_trap_does_not_hang_wait() {
    init_counter();
    void *pool = rt_threadpool_new(1);

    assert(rt_threadpool_submit(pool, (void *)trap_task, NULL) == 1);
    assert(rt_threadpool_submit(pool, (void *)increment_task, NULL) == 1);

    jmp_buf recovery;
    int trapped = 0;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_threadpool_wait_for(pool, 1000);
    } else {
        trapped = 1;
    }
    rt_trap_clear_recovery();

    assert(trapped == 1);
    assert(g_counter.load() == 2);
    assert(rt_threadpool_get_pending(pool) == 0);
    assert(rt_threadpool_get_active(pool) == 0);

    assert(call_traps(shutdown_pool, pool) == 0);
}

static void test_task_trap_error_is_reported_once() {
    init_counter();
    void *pool = rt_threadpool_new(1);

    assert(rt_threadpool_submit(pool, (void *)trap_task, NULL) == 1);

    jmp_buf recovery;
    int trapped = 0;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_threadpool_wait_for(pool, 1000);
    } else {
        trapped = 1;
    }
    rt_trap_clear_recovery();
    assert(trapped == 1);

    trapped = 0;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_threadpool_wait(pool);
    } else {
        trapped = 1;
    }
    rt_trap_clear_recovery();
    assert(trapped == 0);

    assert(call_traps(shutdown_pool, pool) == 0);
}

static void test_submit_pending_count_overflow_traps() {
    void *pool = rt_threadpool_new(1);
    PoolMirror *state = (PoolMirror *)pool;
    rt_monitor_enter(state->monitor);
    state->pending_count = INT64_MAX;
    rt_monitor_exit(state->monitor);

    assert(expect_trap([&]() { (void)rt_threadpool_submit(pool, (void *)increment_task, NULL); }));

    rt_monitor_enter(state->monitor);
    state->pending_count = 0;
    rt_monitor_exit(state->monitor);
    rt_threadpool_shutdown(pool);
}

static void test_error_count_saturates_and_reports_task_trap() {
    init_counter();
    void *pool = rt_threadpool_new(1);
    PoolMirror *state = (PoolMirror *)pool;
    rt_monitor_enter(state->monitor);
    state->error_count = INT64_MAX;
    rt_monitor_exit(state->monitor);

    assert(rt_threadpool_submit(pool, (void *)trap_task, NULL) == 1);
    assert(expect_trap([&]() { (void)rt_threadpool_wait_for(pool, 1000); }));

    rt_monitor_enter(state->monitor);
    assert(state->error_count == 0);
    rt_monitor_exit(state->monitor);
    assert(call_traps(shutdown_pool, pool) == 0);
}

//=============================================================================
// Shutdown modes
//=============================================================================

static void test_graceful_shutdown() {
    init_counter();
    void *pool = rt_threadpool_new(2);

    int i;
    for (i = 0; i < 5; i++)
        rt_threadpool_submit(pool, (void *)slow_task, NULL);

    // Graceful: waits for pending tasks to finish
    rt_threadpool_shutdown(pool);
    assert(rt_threadpool_get_is_shutdown(pool) == 1);
    assert(g_counter.load() == 5);
}

static void test_shutdown_now() {
    init_counter();
    void *pool = rt_threadpool_new(1); // 1 worker

    // Submit many slow tasks
    int i;
    for (i = 0; i < 20; i++)
        rt_threadpool_submit(pool, (void *)slow_task, NULL);

    // Immediate shutdown discards queue
    rt_threadpool_shutdown_now(pool);
    assert(rt_threadpool_get_is_shutdown(pool) == 1);
    // Not all 20 should have completed
    assert(g_counter.load() < 20);
}

static void test_shutdown_surfaces_task_trap() {
    init_counter();
    void *pool = rt_threadpool_new(1);

    assert(rt_threadpool_submit(pool, (void *)trap_task, NULL) == 1);
    assert(call_traps(shutdown_pool, pool) == 1);
    assert(g_counter.load() == 1);
    assert(rt_threadpool_get_is_shutdown(pool) == 1);
}

static void test_shutdown_now_surfaces_task_trap() {
    init_counter();
    void *pool = rt_threadpool_new(1);

    assert(rt_threadpool_submit(pool, (void *)trap_task, NULL) == 1);
    for (int waited = 0; waited < 1000 && g_counter.load() == 0; waited++)
        rt_thread_sleep(1);
    assert(g_counter.load() == 1);

    assert(call_traps(shutdown_pool_now, pool) == 1);
    assert(rt_threadpool_get_is_shutdown(pool) == 1);
}

static void test_shutdown_is_idempotent() {
    init_counter();
    void *pool = rt_threadpool_new(2);
    assert(rt_threadpool_submit(pool, (void *)increment_task, NULL) == 1);
    rt_threadpool_shutdown(pool);
    assert(g_counter.load() == 1);
    assert(rt_threadpool_get_is_shutdown(pool) == 1);
    rt_threadpool_shutdown(pool);
    rt_threadpool_shutdown_now(pool);
}

/// @brief Verify every concurrent Shutdown caller waits for worker termination.
/// @details One worker is held inside a task while the first shutdown caller
///          steals its handle. A second caller must wait on the pool's
///          completion condition instead of observing empty handle slots and
///          returning early. Both calls complete after the task is released.
static void test_concurrent_shutdown_waits_for_join_owner() {
    g_shutdown_task_started.store(0, std::memory_order_relaxed);
    g_shutdown_task_release.store(0, std::memory_order_relaxed);
    void *pool = rt_threadpool_new(1);
    PoolMirror *state = (PoolMirror *)pool;
    assert(rt_threadpool_submit(pool, (void *)hold_shutdown_worker, nullptr) == 1);
    for (int waited = 0; waited < 5000 && !g_shutdown_task_started.load(std::memory_order_acquire);
         ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(g_shutdown_task_started.load(std::memory_order_acquire) == 1);

    std::atomic<int> first_returned{0};
    std::atomic<int> second_returned{0};
    std::thread first([&]() {
        rt_threadpool_shutdown(pool);
        first_returned.store(1, std::memory_order_release);
    });

    bool join_claimed = false;
    for (int waited = 0; waited < 5000 && !join_claimed; ++waited) {
        rt_monitor_enter(state->monitor);
        join_claimed = state->shutdown_joining != 0;
        rt_monitor_exit(state->monitor);
        if (!join_claimed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(join_claimed);

    std::thread second([&]() {
        rt_threadpool_shutdown(pool);
        second_returned.store(1, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert(first_returned.load(std::memory_order_acquire) == 0);
    assert(second_returned.load(std::memory_order_acquire) == 0);

    g_shutdown_task_release.store(1, std::memory_order_release);
    first.join();
    second.join();
    assert(first_returned.load(std::memory_order_acquire) == 1);
    assert(second_returned.load(std::memory_order_acquire) == 1);
    assert(state->shutdown_complete == 1);

    if (rt_obj_release_check0(pool))
        rt_obj_free(pool);
}

//=============================================================================
// Null safety
//=============================================================================

static void test_null_safety() {
    assert(rt_threadpool_get_size(NULL) == 0);
    assert(rt_threadpool_get_pending(NULL) == 0);
    assert(rt_threadpool_get_active(NULL) == 0);
    assert(rt_threadpool_get_is_shutdown(NULL) == 1);
    assert(rt_threadpool_submit(NULL, (void *)increment_task, NULL) == 0);
    assert(rt_threadpool_wait_for(NULL, 100) == 1);

    /// @brief Rt_threadpool_wait.
    rt_threadpool_wait(NULL);         // Should not crash
                                      /// @brief Rt_threadpool_shutdown.
    rt_threadpool_shutdown(NULL);     // Should not crash
                                      /// @brief Rt_threadpool_shutdown_now.
    rt_threadpool_shutdown_now(NULL); // Should not crash
}

//=============================================================================
// Concurrent stress test
//=============================================================================

static void test_concurrent_submitters() {
    init_counter();
    void *pool = rt_threadpool_new(4);
    const int TASKS_PER_THREAD = 25;
    const int NUM_THREADS = 4;

    std::thread threads[NUM_THREADS];
    int i;
    for (i = 0; i < NUM_THREADS; i++) {
        threads[i] = std::thread([pool]() {
            for (int j = 0; j < TASKS_PER_THREAD; j++)
                rt_threadpool_submit(pool, (void *)increment_task, NULL);
        });
    }

    for (i = 0; i < NUM_THREADS; i++)
        threads[i].join();

    rt_threadpool_wait(pool);
    assert(g_counter.load() == NUM_THREADS * TASKS_PER_THREAD);

    rt_threadpool_shutdown(pool);
}

/// @brief Main.
static void owned_noop_task(void *arg) {
    (void)arg;
}

static void test_submit_owned_releases_on_run_and_discard() {
    // VDOC-128: SubmitOwned retains the runtime-managed argument on
    // acceptance and releases it after the callback runs OR when ShutdownNow
    // discards the queued task, so the argument's refcount is balanced in
    // both paths.
    void *arg = rt_box_i64(42);
    size_t base = rt_heap_hdr(arg)->refcnt;

    // Path 1: task runs to completion.
    void *pool = rt_threadpool_new(1);
    assert(rt_threadpool_submit_owned(pool, (void *)owned_noop_task, arg) == 1);
    rt_threadpool_wait(pool);
    assert(rt_heap_hdr(arg)->refcnt == base);
    rt_threadpool_shutdown(pool);

    // Path 2: queued task is discarded by ShutdownNow.
    void *pool2 = rt_threadpool_new(1);
    // Occupy the single worker so the owned task stays queued.
    rt_threadpool_submit(pool2, (void *)slow_task, NULL);
    rt_threadpool_submit(pool2, (void *)slow_task, NULL);
    assert(rt_threadpool_submit_owned(pool2, (void *)owned_noop_task, arg) == 1);
    rt_threadpool_shutdown_now(pool2);
    assert(rt_heap_hdr(arg)->refcnt == base);
}

/// @brief Verify a queued owned argument may form a cycle back to its ThreadPool.
/// @details A blocked first task keeps an owned `pool` argument queued. After the caller drops
///          its reference, cycle collection must return without joining the blocked worker,
///          resurrect the pool for deferred cleanup, and eventually clear the weak observer
///          once the worker is released and the queued self-edge is discarded.
static void test_submit_owned_pool_cycle_is_cleaned_without_collector_deadlock() {
    g_cycle_task_started.store(0, std::memory_order_relaxed);
    g_cycle_task_release.store(0, std::memory_order_relaxed);

    void *pool = rt_threadpool_new(1);
    assert(pool != nullptr);
    assert(rt_threadpool_submit(pool, (void *)hold_cycle_worker, nullptr) == 1);
    for (int waited = 0; waited < 5000 && !g_cycle_task_started.load(std::memory_order_acquire);
         ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(g_cycle_task_started.load(std::memory_order_acquire) == 1);

    assert(rt_threadpool_submit_owned(pool, (void *)owned_noop_task, pool) == 1);
    rt_weakref *weak = rt_weakref_new(pool);
    assert(weak != nullptr);
    assert(rt_obj_release_check0(pool) == 0);

    int64_t passes = rt_gc_pass_count();
    assert(rt_gc_collect() == 0);
    assert(rt_gc_pass_count() > passes);
    g_cycle_task_release.store(1, std::memory_order_release);

    bool cleared = false;
    for (int waited = 0; waited < 5000; ++waited) {
        if (!rt_weakref_alive(weak)) {
            cleared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!cleared) {
        void *live_pool = rt_weakref_get(weak);
        if (live_pool) {
            rt_threadpool_shutdown_now(live_pool);
            if (rt_obj_release_check0(live_pool))
                rt_obj_free(live_pool);
        }
    }
    assert(cleared);
    rt_weakref_free(weak);
}

/// @brief Verify worker-exit cleanup reclaims a resurrected pool without self-joining.
/// @details This white-box regression directly arms cleanup state 2, the state
///          selected when a finalizer cannot create its detached cleanup
///          thread. The exiting worker must consume the sole resurrection
///          reference, skip joining its own self-retained handle, and clear a
///          weak observer without an external shutdown call.
static void test_worker_exit_cleanup_fallback_reclaims_pool() {
    g_fallback_task_started.store(0, std::memory_order_relaxed);
    g_fallback_task_release.store(0, std::memory_order_relaxed);

    void *pool = rt_threadpool_new(1);
    assert(pool != nullptr);
    assert(rt_threadpool_submit_fn(pool, hold_fallback_worker, nullptr) == 1);
    for (int waited = 0; waited < 5000 && !g_fallback_task_started.load(std::memory_order_acquire);
         ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(g_fallback_task_started.load(std::memory_order_acquire) == 1);

    rt_weakref *weak = rt_weakref_new(pool);
    assert(weak != nullptr);
    PoolMirror *state = (PoolMirror *)pool;
    rt_monitor_enter(state->monitor);
    state->cleanup_scheduled = 2;
    state->shutdown_now = 1;
    state->shutdown = 1;
    rt_monitor_pause_all(state->monitor);
    rt_monitor_exit(state->monitor);

    /* The worker-exit fallback owns and consumes the caller-visible reference
       as the stand-in resurrection reference; the test must not release it. */
    g_fallback_task_release.store(1, std::memory_order_release);

    bool cleared = false;
    for (int waited = 0; waited < 5000; ++waited) {
        if (!rt_weakref_alive(weak)) {
            cleared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(cleared);
    rt_weakref_free(weak);
}

int main() {
    test_submit_owned_releases_on_run_and_discard();
    test_submit_owned_pool_cycle_is_cleaned_without_collector_deadlock();
    test_worker_exit_cleanup_fallback_reclaims_pool();
    test_new();
    test_new_clamp_min();
    test_new_clamp_negative();
    test_submit_and_wait();
    test_submit_after_shutdown();
    test_submit_null_callback();
    test_wait_for_success();
    test_wait_for_immediate_check();
    test_wait_for_huge_timeout_pending_task();
    test_wait_for_timeout_budget();
    test_task_trap_does_not_hang_wait();
    test_task_trap_error_is_reported_once();
    test_submit_pending_count_overflow_traps();
    test_error_count_saturates_and_reports_task_trap();
    test_graceful_shutdown();
    test_shutdown_now();
    test_shutdown_surfaces_task_trap();
    test_shutdown_now_surfaces_task_trap();
    test_shutdown_is_idempotent();
    test_concurrent_shutdown_waits_for_join_owner();
    test_null_safety();
    test_concurrent_submitters();

    printf("ThreadPool tests: all passed\n");
    return 0;
}
