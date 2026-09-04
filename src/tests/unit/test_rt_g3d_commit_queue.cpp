//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_g3d_commit_queue.cpp
// Purpose: Unit tests for the internal Graphics3D main-thread commit queue.
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_concqueue.h"
#include "rt_g3d_commit_queue.h"
#include "rt_platform.h"
#include "rt_threadpool.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>

extern "C" void vm_trap(const char *msg) {
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

extern "C" void rt_trap_set_recovery(jmp_buf *buf);
extern "C" void rt_trap_clear_recovery(void);
extern "C" const char *rt_trap_get_error(void);

namespace {
struct CommitContext {
    int64_t values[64];
    int64_t count;
    int64_t sum;
};

struct CommitRecord {
    CommitContext *ctx;
    int64_t value;
};

struct WorkerArg {
    void *queue;
    CommitRecord *record;
};

struct WorkerDrainArg {
    void *queue;
};

struct CloseRaceArg {
    void *queue;
    volatile int phase;
    volatile int accepted;
    volatile int rejected;
    volatile int cancelled;
};

struct CommitQueueLayout {
    void *head;
    void *tail;
    void *free_items;
    void *blocks;
    volatile int gate;
    volatile int closed;
    int64_t pending;
    volatile int64_t submitted;
    volatile int64_t drained;
    uint64_t block_allocations;
};

static void expect_true(bool cond, const char *message) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

extern "C" void record_commit(void *user_data) {
    expect_true(rt_is_main_thread() != 0, "commit callback must run on main thread");
    CommitRecord *record = (CommitRecord *)user_data;
    CommitContext *ctx = record->ctx;
    ctx->values[ctx->count++] = record->value;
    ctx->sum += record->value;
}

extern "C" void enqueue_from_worker(void *user_data) {
    WorkerArg *arg = (WorkerArg *)user_data;
    expect_true(rt_is_main_thread() == 0, "worker enqueue should run off main thread");
    expect_true(rt_g3d_commit_queue_enqueue(arg->queue, record_commit, arg->record) != 0,
                "worker enqueue should succeed");
}

extern "C" void drain_from_worker(void *user_data) {
    WorkerDrainArg *arg = (WorkerDrainArg *)user_data;
    expect_true(rt_is_main_thread() == 0, "worker drain probe should run off main thread");
    (void)rt_g3d_commit_queue_drain(arg->queue, 0);
}

extern "C" void count_cancelled_commit(void *user_data) {
    CloseRaceArg *arg = (CloseRaceArg *)user_data;
    rt_atomic_fetch_add_i32(&arg->cancelled, 1, __ATOMIC_RELAXED);
}

extern "C" void enqueue_across_close(void *user_data) {
    CloseRaceArg *arg = (CloseRaceArg *)user_data;
    for (int attempt = 0; attempt < 1024; ++attempt) {
        int8_t accepted = rt_g3d_commit_queue_enqueue_cost_cancel(
            arg->queue, record_commit, arg, 1, count_cancelled_commit);
        rt_atomic_fetch_add_i32(accepted ? &arg->accepted : &arg->rejected, 1, __ATOMIC_RELAXED);
    }
    rt_atomic_store_i32(&arg->phase, 1, __ATOMIC_RELEASE);
    while (rt_atomic_load_i32(&arg->phase, __ATOMIC_ACQUIRE) < 2)
        rt_atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (int attempt = 0; attempt < 1024; ++attempt) {
        int8_t accepted = rt_g3d_commit_queue_enqueue_cost_cancel(
            arg->queue, record_commit, arg, 1, count_cancelled_commit);
        rt_atomic_fetch_add_i32(accepted ? &arg->accepted : &arg->rejected, 1, __ATOMIC_RELAXED);
    }
    rt_atomic_store_i32(&arg->phase, 3, __ATOMIC_RELEASE);
}

static bool wait_for_worker_trap(void *pool, const char *needle) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_threadpool_wait(pool);
        rt_trap_clear_recovery();
        return false;
    }

    const char *message = rt_trap_get_error();
    bool matched = message && needle && std::strstr(message, needle) != nullptr;
    rt_trap_clear_recovery();
    return matched;
}

static void test_fifo_drain_budget() {
    void *queue = rt_g3d_commit_queue_new();
    expect_true(queue != nullptr, "queue should be created");

    CommitContext ctx = {};
    CommitRecord records[4] = {{&ctx, 10}, {&ctx, 20}, {&ctx, 30}, {&ctx, 40}};
    expect_true(rt_g3d_commit_queue_enqueue(queue, record_commit, &records[0]) != 0,
                "enqueue 10 should succeed");
    expect_true(rt_g3d_commit_queue_enqueue(queue, record_commit, &records[1]) != 0,
                "enqueue 20 should succeed");
    expect_true(rt_g3d_commit_queue_enqueue(queue, record_commit, &records[2]) != 0,
                "enqueue 30 should succeed");
    expect_true(rt_g3d_commit_queue_pending(queue) == 3, "pending count after enqueue");
    expect_true(rt_g3d_commit_queue_submitted(queue) == 3, "submitted count after enqueue");

    expect_true(rt_g3d_commit_queue_drain(queue, 2) == 2, "budgeted drain count");
    expect_true(ctx.count == 2, "two callbacks should run");
    expect_true(ctx.values[0] == 10 && ctx.values[1] == 20, "callbacks should drain FIFO");
    expect_true(rt_g3d_commit_queue_pending(queue) == 1, "one item should remain pending");
    expect_true(rt_g3d_commit_queue_drained(queue) == 2, "drained telemetry after budget");

    expect_true(rt_g3d_commit_queue_enqueue(queue, record_commit, &records[3]) != 0,
                "enqueue after partial drain should succeed");
    expect_true(rt_g3d_commit_queue_drain(queue, 0) == 2, "unbounded drain count");
    expect_true(ctx.count == 4, "all callbacks should run");
    expect_true(ctx.values[2] == 30 && ctx.values[3] == 40, "remaining callbacks keep FIFO");
    expect_true(ctx.sum == 100, "commit callback sum");
    expect_true(rt_g3d_commit_queue_pending(queue) == 0, "queue should be empty");
    expect_true(rt_g3d_commit_queue_drained(queue) == 4, "drained telemetry final");

    rt_g3d_commit_queue_free(queue);
}

static void test_worker_enqueue_main_thread_drain() {
    void *queue = rt_g3d_commit_queue_new();
    void *pool = rt_threadpool_new(4);
    expect_true(queue != nullptr, "queue should be created for worker test");
    expect_true(pool != nullptr, "pool should be created for worker test");

    CommitContext ctx = {};
    CommitRecord records[16] = {};
    WorkerArg args[16] = {};
    int64_t expected_sum = 0;
    for (int64_t i = 0; i < 16; ++i) {
        records[i] = {&ctx, i + 1};
        args[i] = {queue, &records[i]};
        expected_sum += i + 1;
        expect_true(rt_threadpool_submit(pool, (void *)&enqueue_from_worker, &args[i]) != 0,
                    "worker enqueue task should submit");
    }

    rt_threadpool_wait(pool);
    expect_true(ctx.count == 0, "worker enqueue must not run commits directly");
    expect_true(rt_g3d_commit_queue_pending(queue) == 16, "all worker commits should be pending");
    expect_true(rt_g3d_commit_queue_submitted(queue) == 16,
                "submitted telemetry should include worker commits");

    expect_true(rt_g3d_commit_queue_drain(queue, 0) == 16, "main thread drains worker commits");
    expect_true(ctx.count == 16, "all worker commits should run on drain");
    expect_true(ctx.sum == expected_sum, "worker commit sum should match");
    expect_true(rt_g3d_commit_queue_pending(queue) == 0, "queue empty after worker drain");

    rt_threadpool_shutdown(pool);
    rt_g3d_commit_queue_free(queue);
}

static void test_worker_drain_is_rejected() {
    void *queue = rt_g3d_commit_queue_new();
    void *pool = rt_threadpool_new(1);
    expect_true(queue != nullptr, "queue should be created for worker drain test");
    expect_true(pool != nullptr, "pool should be created for worker drain test");

    CommitContext ctx = {};
    CommitRecord record = {&ctx, 99};
    expect_true(rt_g3d_commit_queue_enqueue(queue, record_commit, &record) != 0,
                "main-thread setup enqueue should succeed");

    WorkerDrainArg arg = {queue};
    expect_true(rt_threadpool_submit(pool, (void *)&drain_from_worker, &arg) != 0,
                "worker drain task should submit");
    expect_true(wait_for_worker_trap(pool, "non-main thread"),
                "worker drain should trap with main-thread diagnostic");

    expect_true(ctx.count == 0, "worker drain must not run commit callbacks");
    expect_true(rt_g3d_commit_queue_pending(queue) == 1,
                "commit should remain pending after rejected worker drain");
    expect_true(rt_g3d_commit_queue_drained(queue) == 0,
                "rejected worker drain must not advance drain telemetry");

    expect_true(rt_g3d_commit_queue_drain(queue, 0) == 1,
                "main thread should still drain rejected worker item");
    expect_true(ctx.count == 1 && ctx.values[0] == 99, "main thread should run preserved commit");
    expect_true(rt_g3d_commit_queue_pending(queue) == 0, "queue empty after main-thread drain");

    rt_threadpool_shutdown(pool);
    rt_g3d_commit_queue_free(queue);
}

static void test_cost_budget_drain() {
    void *queue = rt_g3d_commit_queue_new();
    expect_true(queue != nullptr, "queue should be created for cost-budget test");

    CommitContext ctx = {};
    CommitRecord records[6] = {
        {&ctx, 10}, {&ctx, 20}, {&ctx, 30}, {&ctx, 40}, {&ctx, 50}, {&ctx, 60}};
    expect_true(rt_g3d_commit_queue_enqueue_cost(queue, record_commit, &records[0], 10) != 0,
                "enqueue cost 10 should succeed");
    expect_true(rt_g3d_commit_queue_enqueue_cost(queue, record_commit, &records[1], 15) != 0,
                "enqueue cost 15 should succeed");
    expect_true(rt_g3d_commit_queue_enqueue_cost(queue, record_commit, &records[2], 0) != 0,
                "enqueue cost 0 should succeed");
    expect_true(rt_g3d_commit_queue_enqueue_cost(queue, record_commit, &records[3], 40) != 0,
                "enqueue oversized cost should succeed");

    expect_true(rt_g3d_commit_queue_drain_budget(queue, 0, 20) == 1,
                "cost budget drains only the prefix within budget");
    expect_true(ctx.count == 1 && ctx.values[0] == 10, "first cost-budget drain runs FIFO head");
    expect_true(rt_g3d_commit_queue_pending(queue) == 3,
                "cost budget leaves over-budget tail pending");

    expect_true(rt_g3d_commit_queue_drain_budget(queue, 0, 20) == 2,
                "zero-cost callbacks can share remaining budget");
    expect_true(ctx.count == 3 && ctx.values[1] == 20 && ctx.values[2] == 30,
                "second cost-budget drain preserves FIFO");

    expect_true(rt_g3d_commit_queue_drain_budget(queue, 0, 20) == 1,
                "oversized first item drains alone for liveness");
    expect_true(ctx.count == 4 && ctx.values[3] == 40, "oversized item runs after prior budget");
    expect_true(rt_g3d_commit_queue_pending(queue) == 0, "queue empty after oversized drain");

    expect_true(rt_g3d_commit_queue_enqueue_cost(queue, record_commit, &records[4], 0) != 0,
                "enqueue zero-cost item should succeed");
    expect_true(rt_g3d_commit_queue_enqueue_cost(queue, record_commit, &records[5], 1) != 0,
                "enqueue positive-cost item should succeed");
    expect_true(rt_g3d_commit_queue_drain_budget(queue, 0, 0) == 1,
                "zero budget drains zero-cost work only");
    expect_true(ctx.count == 5 && ctx.values[4] == 50, "zero budget ran the zero-cost callback");
    expect_true(rt_g3d_commit_queue_pending(queue) == 1,
                "zero budget leaves positive-cost work pending");

    expect_true(rt_g3d_commit_queue_drain_budget(queue, 1, 100) == 1,
                "item budget still applies with cost budget");
    expect_true(ctx.count == 6 && ctx.values[5] == 60,
                "final positive-cost callback runs when budget allows");

    rt_g3d_commit_queue_free(queue);
}

static void test_closed_queue_rejects_enqueue_without_trap() {
    void *queue = rt_g3d_commit_queue_new();
    expect_true(queue != nullptr, "queue should be created for closed enqueue test");

    rt_g3d_commit_queue_close(queue);

    CommitContext ctx = {};
    CommitRecord record = {&ctx, 7};
    expect_true(rt_g3d_commit_queue_enqueue(queue, record_commit, &record) == 0,
                "enqueue on a closed backing queue should fail gracefully");
    expect_true(rt_g3d_commit_queue_pending(queue) == 0,
                "closed backing queue should not retain a pending item");
    expect_true(rt_g3d_commit_queue_submitted(queue) == 0,
                "closed backing queue should not advance submit telemetry");

    rt_g3d_commit_queue_free(queue);
}

static void test_drained_item_storage_is_reused() {
    void *queue = rt_g3d_commit_queue_new();
    expect_true(queue != nullptr, "queue should be created for storage reuse test");

    CommitContext ctx = {};
    CommitRecord records[64] = {};
    for (int iteration = 0; iteration < 100; ++iteration) {
        for (int index = 0; index < 64; ++index) {
            records[index] = {&ctx, 0};
            expect_true(rt_g3d_commit_queue_enqueue(queue, record_commit, &records[index]) != 0,
                        "pooled enqueue should succeed");
        }
        expect_true(rt_g3d_commit_queue_drain(queue, 0) == 64,
                    "each pooled batch should drain completely");
        ctx.count = 0;
    }

    CommitQueueLayout *layout = (CommitQueueLayout *)queue;
    expect_true(layout->block_allocations == 1,
                "repeated drained batches should reuse one allocation block");
    rt_g3d_commit_queue_free(queue);
}

static void test_wrapper_allocation_failure_preserves_caller_ownership() {
    void *queue = rt_g3d_commit_queue_new();
    expect_true(queue != nullptr, "queue should be created for allocation-failure test");

    CommitContext ctx = {};
    CommitRecord record = {&ctx, 13};
    rt_g3d_commit_queue_test_fail_next_allocations(1);
    expect_true(rt_g3d_commit_queue_enqueue(queue, record_commit, &record) == 0,
                "wrapper allocation failure should be reported without trapping");
    rt_g3d_commit_queue_test_fail_next_allocations(0);
    expect_true(rt_g3d_commit_queue_pending(queue) == 0,
                "failed allocation should not publish a pending item");
    expect_true(rt_g3d_commit_queue_submitted(queue) == 0,
                "failed allocation should not advance submit telemetry");
    expect_true(ctx.count == 0, "failed allocation must not run or consume the caller payload");

    expect_true(rt_g3d_commit_queue_enqueue(queue, record_commit, &record) != 0,
                "caller should be able to retry the same payload after allocation failure");
    expect_true(rt_g3d_commit_queue_drain(queue, 0) == 1, "retried payload should drain normally");
    expect_true(ctx.count == 1 && ctx.values[0] == 13,
                "retried payload should remain intact after allocation failure");

    rt_g3d_commit_queue_free(queue);
}

static void test_close_then_join_prevents_enqueue_free_race() {
    void *queue = rt_g3d_commit_queue_new();
    void *pool = rt_threadpool_new(1);
    expect_true(queue != nullptr, "queue should be created for close race test");
    expect_true(pool != nullptr, "pool should be created for close race test");

    CloseRaceArg arg = {queue, 0, 0, 0, 0};
    expect_true(rt_threadpool_submit(pool, (void *)&enqueue_across_close, &arg) != 0,
                "close-race producer should submit");
    while (rt_atomic_load_i32(&arg.phase, __ATOMIC_ACQUIRE) < 1)
        rt_atomic_thread_fence(__ATOMIC_ACQUIRE);
    rt_g3d_commit_queue_close(queue);
    rt_atomic_store_i32(&arg.phase, 2, __ATOMIC_RELEASE);
    rt_threadpool_wait(pool);

    expect_true(arg.accepted == 1024,
                "all submissions before the close publication transfer ownership");
    expect_true(arg.rejected == 1024,
                "all producer submissions after close retain caller ownership");
    rt_threadpool_shutdown(pool);
    rt_g3d_commit_queue_free(queue);
    expect_true(arg.cancelled == arg.accepted,
                "free after producer join cancels every accepted pending payload exactly once");
}
} // namespace

int main() {
    rt_set_main_thread();
    test_fifo_drain_budget();
    test_worker_enqueue_main_thread_drain();
    test_worker_drain_is_rejected();
    test_cost_budget_drain();
    test_closed_queue_rejects_enqueue_without_trap();
    test_drained_item_storage_is_reused();
    test_wrapper_allocation_failure_preserves_caller_ownership();
    test_close_then_join_prevents_enqueue_free_race();
    std::printf("Graphics3D commit queue tests: all passed\n");
    return 0;
}
