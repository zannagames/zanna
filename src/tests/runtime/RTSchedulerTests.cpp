//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTSchedulerTests.cpp
// Purpose: Validate synchronized scheduling, replacement, polling,
//          generations, ownership rollback, and invalid-name handling.
// Key invariants:
//   - Equal byte spans replace one task while embedded NUL names remain distinct.
//   - Poll transfers retained names and concurrent mutation preserves list state.
//   - A returning invalid-name trap cannot alias a forged handle to an empty name.
// Ownership/Lifetime:
//   - Focused regression resources are explicitly released; scheduler-owned
//     names transfer or release through the runtime paths under test.
// Links: src/runtime/threads/rt_scheduler.c,
//        docs/adr/0133-runtime-concurrency-and-collection-hardening.md
//
//===----------------------------------------------------------------------===//

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_scheduler.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static bool g_return_traps = false;
static const char *g_last_returning_trap = nullptr;

extern "C" {
void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);

void vm_trap(const char *msg) {
    if (g_return_traps) {
        g_last_returning_trap = msg;
        return;
    }
    rt_abort(msg);
}
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

static void test_new_scheduler() {
    void *s = rt_scheduler_new();
    assert(s != NULL);
    assert(rt_scheduler_pending(s) == 0);
}

static void test_schedule_and_pending() {
    void *s = rt_scheduler_new();
    rt_string t1 = rt_string_from_bytes("task1", 5);
    rt_string t2 = rt_string_from_bytes("task2", 5);

    /// @brief Rt_scheduler_schedule.
    rt_scheduler_schedule(s, t1, 1000); // 1 second from now
    assert(rt_scheduler_pending(s) == 1);

    rt_scheduler_schedule(s, t2, 2000);
    assert(rt_scheduler_pending(s) == 2);
}

static void test_cancel() {
    void *s = rt_scheduler_new();
    rt_string t1 = rt_string_from_bytes("task1", 5);

    rt_scheduler_schedule(s, t1, 1000);
    assert(rt_scheduler_pending(s) == 1);

    int8_t cancelled = rt_scheduler_cancel(s, t1);
    assert(cancelled == 1);
    assert(rt_scheduler_pending(s) == 0);

    // Cancel non-existent
    cancelled = rt_scheduler_cancel(s, t1);
    assert(cancelled == 0);
}

static void test_is_due_not_ready() {
    void *s = rt_scheduler_new();
    rt_string t1 = rt_string_from_bytes("task1", 5);

    /// @brief Rt_scheduler_schedule.
    rt_scheduler_schedule(s, t1, 5000);      // 5 seconds from now
    assert(rt_scheduler_is_due(s, t1) == 0); // not due yet

    // Non-existent task
    rt_string missing = rt_string_from_bytes("nope", 4);
    assert(rt_scheduler_is_due(s, missing) == 0);
}

static void test_immediate_due() {
    void *s = rt_scheduler_new();
    rt_string t1 = rt_string_from_bytes("now", 3);

    /// @brief Rt_scheduler_schedule.
    rt_scheduler_schedule(s, t1, 0); // due immediately
    // Small sleep to ensure the monotonic clock advances
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(rt_scheduler_is_due(s, t1) == 1);
}

static void test_poll_returns_due() {
    void *s = rt_scheduler_new();
    rt_string t1 = rt_string_from_bytes("fast", 4);
    rt_string t2 = rt_string_from_bytes("slow", 4);

    /// @brief Rt_scheduler_schedule.
    rt_scheduler_schedule(s, t1, 0);     // due immediately
                                         /// @brief Rt_scheduler_schedule.
    rt_scheduler_schedule(s, t2, 60000); // due in 60 seconds

    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    void *due = rt_scheduler_poll(s);
    assert(due != NULL);

    int64_t count = rt_seq_len(due);
    assert(count == 1);

    // The fast task should be returned
    rt_string name = (rt_string)rt_seq_get(due, 0);
    assert(strcmp(rt_string_cstr(name), "fast") == 0);

    // Fast task removed, slow task still pending
    assert(rt_scheduler_pending(s) == 1);
}

static void test_clear() {
    void *s = rt_scheduler_new();
    rt_string t1 = rt_string_from_bytes("a", 1);
    rt_string t2 = rt_string_from_bytes("b", 1);

    rt_scheduler_schedule(s, t1, 100);
    rt_scheduler_schedule(s, t2, 200);
    assert(rt_scheduler_pending(s) == 2);

    rt_scheduler_clear(s);
    assert(rt_scheduler_pending(s) == 0);
}

static void test_duplicate_name_replaces() {
    void *s = rt_scheduler_new();
    rt_string t1 = rt_string_from_bytes("task", 4);

    rt_scheduler_schedule(s, t1, 1000);
    assert(rt_scheduler_pending(s) == 1);

    // Schedule again with same name - should replace
    rt_scheduler_schedule(s, t1, 2000);
    assert(rt_scheduler_pending(s) == 1);
}

static void test_schedule_name_retain_overflow_drops_scheduler_reference() {
    void *s = rt_scheduler_new();
    rt_string name = rt_string_from_bytes("overflow", 8);
    rt_heap_hdr_t *sched_hdr = rt_heap_hdr(s);
    size_t sched_refcnt = sched_hdr->refcnt;

    name->literal_refs = RT_HEAP_MAX_MORTAL_REFCNT;
    assert(expect_trap([&]() { rt_scheduler_schedule(s, name, 0); }));
    assert(sched_hdr->refcnt == sched_refcnt);

    name->literal_refs = 1;
    rt_string_unref(name);
    if (rt_obj_release_check0(s))
        rt_obj_free(s);
}

static void test_embedded_nul_names_are_distinct() {
    void *s = rt_scheduler_new();
    const char name1_bytes[3] = {'a', '\0', '1'};
    const char name2_bytes[3] = {'a', '\0', '2'};
    rt_string name1 = rt_string_from_bytes(name1_bytes, 3);
    rt_string name2 = rt_string_from_bytes(name2_bytes, 3);

    rt_scheduler_schedule(s, name1, 0);
    rt_scheduler_schedule(s, name2, 60000);
    assert(rt_scheduler_pending(s) == 2);
    assert(rt_scheduler_is_due(s, name1) == 1);
    assert(rt_scheduler_is_due(s, name2) == 0);

    assert(rt_scheduler_cancel(s, name1) == 1);
    assert(rt_scheduler_pending(s) == 1);
    assert(rt_scheduler_cancel(s, name1) == 0);
    assert(rt_scheduler_cancel(s, name2) == 1);
    assert(rt_scheduler_pending(s) == 0);
}

static void test_embedded_nul_poll_preserves_name() {
    void *s = rt_scheduler_new();
    const char name_bytes[3] = {'x', '\0', 'z'};
    rt_string name = rt_string_from_bytes(name_bytes, 3);

    rt_scheduler_schedule(s, name, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    void *due = rt_scheduler_poll(s);
    assert(rt_seq_len(due) == 1);
    const char *got = rt_string_cstr((rt_string)rt_seq_get(due, 0));
    assert(got[0] == 'x');
    assert(got[1] == '\0');
    assert(got[2] == 'z');
}

static void test_concurrent_schedule_cancel() {
    void *s = rt_scheduler_new();
    constexpr int kThreads = 4;
    constexpr int kTasksPerThread = 50;

    std::vector<std::thread> schedulers;
    for (int tid = 0; tid < kThreads; ++tid) {
        schedulers.emplace_back([=]() {
            for (int i = 0; i < kTasksPerThread; ++i) {
                std::string name = "task-" + std::to_string(tid) + "-" + std::to_string(i);
                rt_string rt_name = rt_string_from_bytes(name.data(), (int64_t)name.size());
                rt_scheduler_schedule(s, rt_name, 60000);
            }
        });
    }
    for (auto &t : schedulers)
        t.join();

    assert(rt_scheduler_pending(s) == kThreads * kTasksPerThread);

    std::vector<std::thread> cancellers;
    for (int tid = 0; tid < kThreads; ++tid) {
        cancellers.emplace_back([=]() {
            for (int i = 0; i < kTasksPerThread; i += 2) {
                std::string name = "task-" + std::to_string(tid) + "-" + std::to_string(i);
                rt_string rt_name = rt_string_from_bytes(name.data(), (int64_t)name.size());
                assert(rt_scheduler_cancel(s, rt_name) == 1);
            }
        });
    }
    for (auto &t : cancellers)
        t.join();

    assert(rt_scheduler_pending(s) == kThreads * (kTasksPerThread / 2));
}

static void test_null_safety() {
    assert(rt_scheduler_pending(NULL) == 0);
    /// @brief Rt_scheduler_clear.
    rt_scheduler_clear(NULL); // should not crash
    assert(rt_scheduler_generation_of(NULL, NULL) == -1);
    assert(rt_scheduler_is_due_gen(NULL, NULL, 0) == 0);
}

static void test_schedule_gen_and_generation_of() {
    void *s = rt_scheduler_new();
    rt_string t = rt_string_from_bytes("gen", 3);

    rt_scheduler_schedule_gen(s, t, 5000, 7); // not due for 5s, tagged generation 7
    assert(rt_scheduler_pending(s) == 1);
    assert(rt_scheduler_generation_of(s, t) == 7);
    assert(rt_scheduler_is_due_gen(s, t, 7) == 0); // tagged correctly but not due yet

    rt_string missing = rt_string_from_bytes("nope", 4);
    assert(rt_scheduler_generation_of(s, missing) == -1); // unscheduled name
}

static void test_is_due_gen_matches_generation() {
    void *s = rt_scheduler_new();
    rt_string t = rt_string_from_bytes("now", 3);

    rt_scheduler_schedule_gen(s, t, 0, 7); // due immediately, generation 7
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(rt_scheduler_is_due(s, t) == 1);        // generation-agnostic
    assert(rt_scheduler_is_due_gen(s, t, 7) == 1); // matching generation fires
    assert(rt_scheduler_is_due_gen(s, t, 8) == 0); // due, but wrong generation
}

static void test_generation_supersession() {
    void *s = rt_scheduler_new();
    rt_string t = rt_string_from_bytes("diag", 4);

    rt_scheduler_schedule_gen(s, t, 0, 5); // revision 5
    rt_scheduler_schedule_gen(s, t, 0, 6); // revision 6 supersedes revision 5
    assert(rt_scheduler_pending(s) == 1);  // replaced, not duplicated

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(rt_scheduler_generation_of(s, t) == 6);
    assert(rt_scheduler_is_due_gen(s, t, 5) == 0); // stale revision is discarded
    assert(rt_scheduler_is_due_gen(s, t, 6) == 1); // current revision fires
}

static void test_plain_schedule_is_generation_zero() {
    void *s = rt_scheduler_new();
    rt_string t = rt_string_from_bytes("plain", 5);

    rt_scheduler_schedule(s, t, 5000);
    assert(rt_scheduler_generation_of(s, t) == 0); // plain Schedule records generation 0

    // A plain Schedule after a ScheduleGen resets the generation back to 0.
    rt_scheduler_schedule_gen(s, t, 5000, 9);
    assert(rt_scheduler_generation_of(s, t) == 9);
    rt_scheduler_schedule(s, t, 5000);
    assert(rt_scheduler_generation_of(s, t) == 0);
}

/// @brief Main.
static void test_generation_of_option_disambiguates() {
    // VDOC-130: generation -1 is valid data; the Option form separates it
    // from absence.
    void *sched = rt_scheduler_new();
    rt_string name = rt_string_from_bytes("task", 4);

    void *missing = rt_scheduler_generation_of_option(sched, name);
    assert(rt_option_is_none(missing) == 1);

    rt_scheduler_schedule_gen(sched, name, 1000, -1);
    void *live = rt_scheduler_generation_of_option(sched, name);
    assert(rt_option_is_some(live) == 1);
    assert(rt_option_unwrap_i64(live) == -1);

    // The legacy accessor still returns -1 for both cases (documented).
    assert(rt_scheduler_generation_of(sched, name) == -1);
    rt_string_unref(name);
}

static void test_invalid_name_returning_trap_does_not_alias_empty_name() {
    void *sched = rt_scheduler_new();
    rt_string empty_name = rt_string_from_bytes("", 0);
    rt_scheduler_schedule_gen(sched, empty_name, 1000, 42);
    assert(rt_scheduler_pending(sched) == 1);

    auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
    g_return_traps = true;

    g_last_returning_trap = nullptr;
    rt_scheduler_schedule_gen(sched, forged, 0, 99);
    assert(g_last_returning_trap != nullptr);
    assert(rt_scheduler_pending(sched) == 1);

    g_last_returning_trap = nullptr;
    assert(rt_scheduler_cancel(sched, forged) == 0);
    assert(g_last_returning_trap != nullptr);

    g_last_returning_trap = nullptr;
    assert(rt_scheduler_generation_of(sched, forged) == -1);
    assert(g_last_returning_trap != nullptr);

    g_return_traps = false;
    assert(rt_scheduler_generation_of(sched, empty_name) == 42);

    rt_string_unref(empty_name);
    if (rt_obj_release_check0(sched))
        rt_obj_free(sched);
}

int main() {
    test_generation_of_option_disambiguates();
    test_new_scheduler();
    test_schedule_and_pending();
    test_cancel();
    test_is_due_not_ready();
    test_immediate_due();
    test_poll_returns_due();
    test_clear();
    test_duplicate_name_replaces();
    test_schedule_name_retain_overflow_drops_scheduler_reference();
    test_embedded_nul_names_are_distinct();
    test_embedded_nul_poll_preserves_name();
    test_concurrent_schedule_cancel();
    test_null_safety();
    test_schedule_gen_and_generation_of();
    test_is_due_gen_matches_generation();
    test_generation_supersession();
    test_plain_schedule_is_generation_zero();
    test_invalid_name_returning_trap_does_not_alias_empty_name();
    return 0;
}
