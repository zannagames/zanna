//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTChannelTests.cpp
// Purpose: Validate bounded-channel transfer, timeout, ownership, and GC behavior.
// Key invariants:
//   - A successful receive transfers exactly one owned value; probes consume none.
//   - Timed operations obey one total deadline and channels expose all managed
//     edges needed for cycle collection.
// Ownership/Lifetime:
//   - Tests release channel/value objects after worker threads join. Trap cases
//     retain ownership until their explicit recovery cleanup completes.
// Links: src/runtime/threads/rt_channel.c, docs/zannalib/threads.md,
//        docs/adr/0133-runtime-concurrency-and-collection-hardening.md
//
//===----------------------------------------------------------------------===//

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static bool g_return_traps = false;
static std::string g_last_returning_trap;

extern "C" {
#include "rt_array_obj.h"
#include "rt_channel.h"
#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_threads.h"
#include "rt_trap.h"

void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);

/// @brief Vm_trap.
void vm_trap(const char *msg) {
    if (g_return_traps) {
        g_last_returning_trap = msg ? msg : "";
        return;
    }
    fprintf(stderr, "TRAP: %s\n", msg);
    rt_abort(msg);
}
}

static void *make_obj() {
    return rt_obj_new_i64(0, 8);
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

static std::atomic<int> g_discard_finalizer_count{0};
static void *g_discard_finalizer_channel = NULL;
static int g_channel_alloc_fail_countdown = 0;

/// @brief Fail one selected managed allocation in Channel ownership tests.
/// @details The hook delegates every other request to the runtime allocator,
///          isolating the Option allocation that follows a successful receive.
/// @param bytes Requested allocation size.
/// @param next Default managed allocator.
/// @return Allocated storage, or NULL at the selected failure boundary.
static void *channel_fail_countdown_alloc(int64_t bytes, void *(*next)(int64_t)) {
    if (g_channel_alloc_fail_countdown > 0 && --g_channel_alloc_fail_countdown == 0)
        return nullptr;
    return next(bytes);
}

static void reentrant_channel_discard_finalizer(void *obj) {
    (void)obj;
    g_discard_finalizer_count.fetch_add(1, std::memory_order_acq_rel);
    if (g_discard_finalizer_channel)
        (void)rt_channel_get_len(g_discard_finalizer_channel);
}

struct ChannelMirror {
    void *monitor;
    void **buffer;
    int64_t capacity;
    int64_t count;
    int64_t head;
    int64_t tail;
    int64_t waiting_senders;
    int64_t waiting_receivers;
    int64_t sync_epoch;
    int64_t sync_acked_epoch;
    int8_t closed;
};

//=============================================================================
// Creation and properties
//=============================================================================

static void test_new_buffered() {
    void *ch = rt_channel_new(10);
    assert(ch != NULL);
    assert(rt_channel_get_cap(ch) == 10);
    assert(rt_channel_get_len(ch) == 0);
    assert(rt_channel_get_is_empty(ch) == 1);
    assert(rt_channel_get_is_full(ch) == 0);
    assert(rt_channel_get_is_closed(ch) == 0);
    rt_channel_close(ch);
}

static void test_new_synchronous() {
    void *ch = rt_channel_new(0);
    assert(ch != NULL);
    assert(rt_channel_get_cap(ch) == 0);
    assert(rt_channel_get_len(ch) == 0);
    assert(rt_channel_get_is_empty(ch) == 1);
    // Synchronous channels report full
    assert(rt_channel_get_is_full(ch) == 1);
    rt_channel_close(ch);
}

static void test_new_negative_capacity() {
    void *ch = rt_channel_new(-5);
    assert(ch != NULL);
    assert(rt_channel_get_cap(ch) == 0); // Clamped to 0
    rt_channel_close(ch);
}

//=============================================================================
// Buffered send/recv (single-threaded via try_ variants)
//=============================================================================

static void test_try_send_recv() {
    void *ch = rt_channel_new(5);
    void *a = make_obj();
    void *b = make_obj();
    void *c = make_obj();

    assert(rt_channel_try_send(ch, a) == 1);
    assert(rt_channel_try_send(ch, b) == 1);
    assert(rt_channel_try_send(ch, c) == 1);
    assert(rt_channel_get_len(ch) == 3);
    assert(rt_channel_get_is_empty(ch) == 0);

    void *out = NULL;
    assert(rt_channel_try_recv(ch, &out) == 1);
    assert(out == a);

    assert(rt_channel_try_recv(ch, &out) == 1);
    assert(out == b);

    assert(rt_channel_try_recv(ch, &out) == 1);
    assert(out == c);

    assert(rt_channel_get_len(ch) == 0);
    assert(rt_channel_get_is_empty(ch) == 1);
    rt_channel_close(ch);
}

static void test_try_recv_empty() {
    void *ch = rt_channel_new(5);
    void *sentinel = make_obj();
    void *out = sentinel;
    assert(rt_channel_try_recv(ch, &out) == 0);
    assert(out == NULL);
    rt_channel_close(ch);
}

static void test_try_recv_null_out_does_not_consume() {
    void *ch = rt_channel_new(2);
    void *a = make_obj();
    void *b = make_obj();

    assert(rt_channel_try_send(ch, a) == 1);
    assert(rt_channel_try_send(ch, b) == 1);
    assert(rt_channel_get_len(ch) == 2);

    assert(rt_channel_try_recv(ch, NULL) == 1);
    assert(rt_channel_get_len(ch) == 2);

    void *out = NULL;
    assert(rt_channel_try_recv(ch, &out) == 1);
    assert(out == a);
    assert(rt_channel_get_len(ch) == 1);

    rt_channel_close(ch);
}

static void test_try_send_full() {
    void *ch = rt_channel_new(2);
    void *a = make_obj();
    void *b = make_obj();
    void *c = make_obj();

    assert(rt_channel_try_send(ch, a) == 1);
    assert(rt_channel_try_send(ch, b) == 1);
    assert(rt_channel_get_is_full(ch) == 1);
    rt_heap_hdr_t *c_hdr = rt_heap_hdr(c);
    c_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    assert(rt_channel_try_send(ch, c) == 0); // Full
    c_hdr->refcnt = 1;

    rt_channel_close(ch);
}

static void test_fifo_order() {
    void *ch = rt_channel_new(10);
    void *items[5];
    int i;
    for (i = 0; i < 5; i++) {
        items[i] = make_obj();
        rt_channel_try_send(ch, items[i]);
    }

    for (i = 0; i < 5; i++) {
        void *out = NULL;
        assert(rt_channel_try_recv(ch, &out) == 1);
        assert(out == items[i]);
    }
    rt_channel_close(ch);
}

//=============================================================================
// Close semantics
//=============================================================================

static void test_close_prevents_send() {
    void *ch = rt_channel_new(5);
    rt_channel_close(ch);

    assert(rt_channel_get_is_closed(ch) == 1);
    void *item = make_obj();
    rt_heap_hdr_t *hdr = rt_heap_hdr(item);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    assert(rt_channel_try_send(ch, item) == 0);
    hdr->refcnt = 1;
}

static void test_close_allows_drain() {
    void *ch = rt_channel_new(5);
    void *a = make_obj();
    rt_channel_try_send(ch, a);
    rt_channel_close(ch);

    // Can still recv remaining items
    void *out = NULL;
    assert(rt_channel_try_recv(ch, &out) == 1);
    assert(out == a);

    // Empty now
    out = a;
    assert(rt_channel_try_recv(ch, &out) == 0);
    assert(out == NULL);
}

static void test_double_close() {
    void *ch = rt_channel_new(5);
    rt_channel_close(ch);
    /// @brief Rt_channel_close.
    rt_channel_close(ch); // Should not crash
    assert(rt_channel_get_is_closed(ch) == 1);
}

//=============================================================================
// Timed operations
//=============================================================================

static void test_recv_for_timeout() {
    void *ch = rt_channel_new(5);
    void *sentinel = make_obj();
    void *out = sentinel;
    // Should timeout quickly on empty channel
    assert(rt_channel_recv_for(ch, &out, 10) == 0);
    assert(out == NULL);
    rt_channel_close(ch);
}

static void test_recv_for_closed_clears_out() {
    void *ch = rt_channel_new(5);
    void *sentinel = make_obj();
    void *out = sentinel;
    rt_channel_close(ch);

    assert(rt_channel_recv_for(ch, &out, 10) == 0);
    assert(out == NULL);
}

static void test_recv_for_immediate() {
    void *ch = rt_channel_new(5);
    void *a = make_obj();
    rt_channel_try_send(ch, a);

    void *out = NULL;
    assert(rt_channel_recv_for(ch, &out, 100) == 1);
    assert(out == a);
    rt_channel_close(ch);
}

static void test_huge_timeout_immediate_paths() {
    void *ch = rt_channel_new(1);
    void *a = make_obj();

    assert(rt_channel_send_for(ch, a, INT64_MAX) == 1);

    void *out = NULL;
    assert(rt_channel_recv_for(ch, &out, INT64_MAX) == 1);
    assert(out == a);
    rt_channel_close(ch);
}

static void test_managed_value_wrappers() {
    void *ch = rt_channel_new(2);
    void *a = make_obj();
    void *b = make_obj();

    assert(rt_channel_try_recv_val(ch) == NULL);
    void *empty = rt_channel_try_recv_option(ch);
    assert(rt_option_is_none(empty) == 1);
    assert(rt_channel_try_send(ch, a) == 1);
    assert(rt_channel_try_recv_val(ch) == a);

    assert(rt_channel_try_send(ch, NULL) == 1);
    void *null_value = rt_channel_try_recv_option(ch);
    assert(rt_option_is_some(null_value) == 1);
    assert(rt_option_unwrap(null_value) == NULL);

    assert(rt_channel_recv_for_val(ch, 5) == NULL);
    assert(rt_channel_try_send(ch, b) == 1);
    assert(rt_channel_recv_for_val(ch, 100) == b);

    rt_channel_close(ch);
}

static void test_send_for_timeout() {
    void *ch = rt_channel_new(1);
    void *a = make_obj();
    void *b = make_obj();

    assert(rt_channel_send_for(ch, a, 100) == 1); // Space available
    assert(rt_channel_send_for(ch, b, 10) == 0);  // Full, should timeout
    rt_channel_close(ch);
}

static void test_send_for_zero_ms() {
    void *ch = rt_channel_new(1);
    void *a = make_obj();

    // ms <= 0 degrades to try_send
    assert(rt_channel_send_for(ch, a, 0) == 1);
    assert(rt_channel_send_for(ch, make_obj(), 0) == 0); // Full
    rt_channel_close(ch);
}

static void test_send_retain_overflow_does_not_lock_channel() {
    void *ch = rt_channel_new(1);
    void *item = make_obj();
    rt_heap_hdr_t *hdr = rt_heap_hdr(item);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    assert(expect_trap([&]() { rt_channel_send(ch, item); }));
    assert(rt_channel_get_len(ch) == 0);
    hdr->refcnt = 1;

    rt_channel_close(ch);
}

static void test_try_send_retain_overflow_does_not_lock_channel() {
    void *ch = rt_channel_new(1);
    void *item = make_obj();
    rt_heap_hdr_t *hdr = rt_heap_hdr(item);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    assert(expect_trap([&]() { (void)rt_channel_try_send(ch, item); }));
    assert(rt_channel_get_len(ch) == 0);
    hdr->refcnt = 1;

    assert(rt_channel_try_send(ch, NULL) == 1);
    assert(rt_channel_get_len(ch) == 1);
    void *out = (void *)0x1;
    assert(rt_channel_try_recv(ch, &out) == 1);
    assert(out == NULL);

    rt_channel_close(ch);
}

static void test_send_for_retain_overflow_does_not_lock_channel() {
    void *ch = rt_channel_new(1);
    void *item = make_obj();
    rt_heap_hdr_t *hdr = rt_heap_hdr(item);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    assert(expect_trap([&]() { (void)rt_channel_send_for(ch, item, 10); }));
    assert(rt_channel_get_len(ch) == 0);
    hdr->refcnt = 1;

    rt_channel_close(ch);
}

static void test_recv_for_discard_releases_after_unlock() {
    void *ch = rt_channel_new(1);
    void *item = make_obj();
    rt_obj_set_finalizer(item, reentrant_channel_discard_finalizer);

    g_discard_finalizer_count.store(0, std::memory_order_release);
    g_discard_finalizer_channel = ch;

    assert(rt_channel_try_send(ch, item) == 1);
    if (rt_obj_release_check0(item))
        rt_obj_free(item);

    assert(rt_channel_recv_for_discard(ch, 100) == 1);
    assert(g_discard_finalizer_count.load(std::memory_order_acquire) == 1);

    g_discard_finalizer_channel = NULL;
    rt_channel_close(ch);
}

static void test_recv_for_null_out_is_non_consuming_probe() {
    void *ch = rt_channel_new(1);
    void *item = make_obj();

    assert(rt_channel_try_send(ch, item) == 1);
    assert(rt_channel_recv_for(ch, NULL, 100) == 1);
    assert(rt_channel_get_len(ch) == 1);

    void *out = NULL;
    assert(rt_channel_try_recv(ch, &out) == 1);
    assert(out == item);
    rt_channel_close(ch);
}

static void test_try_recv_option_allocation_failure_releases_transfer() {
    void *ch = rt_channel_new(1);
    void *item = make_obj();
    rt_obj_set_finalizer(item, reentrant_channel_discard_finalizer);
    g_discard_finalizer_count.store(0, std::memory_order_release);
    g_discard_finalizer_channel = ch;

    assert(rt_channel_try_send(ch, item) == 1);
    if (rt_obj_release_check0(item))
        rt_obj_free(item);

    g_channel_alloc_fail_countdown = 1;
    rt_set_alloc_hook(channel_fail_countdown_alloc);
    bool trapped = expect_trap([&]() { (void)rt_channel_try_recv_option(ch); });
    rt_set_alloc_hook(nullptr);
    g_channel_alloc_fail_countdown = 0;

    assert(trapped);
    assert(g_discard_finalizer_count.load(std::memory_order_acquire) == 1);
    assert(rt_channel_get_len(ch) == 0);
    assert(rt_channel_try_send(ch, NULL) == 1);

    g_discard_finalizer_channel = NULL;
    rt_channel_close(ch);
}

static void test_channel_cycle_is_collected() {
    void *ch = rt_channel_new(1);
    void **array = rt_arr_obj_new(1);
    assert(ch != NULL);
    assert(array != NULL);
    assert(rt_gc_is_tracked(ch) == 1);
    assert(rt_gc_is_tracked(array) == 1);

    rt_arr_obj_put(array, 0, ch);
    assert(rt_channel_try_send(ch, array) == 1);
    if (rt_obj_release_check0(array))
        rt_obj_free(array);
    if (rt_obj_release_check0(ch))
        rt_obj_free(ch);

    assert(rt_gc_collect() >= 2);
    assert(rt_gc_is_tracked(ch) == 0);
    assert(rt_gc_is_tracked(array) == 0);
}

static void test_timed_operations_include_monitor_acquisition() {
    void *ch = rt_channel_new(1);
    void *item = make_obj();
    ChannelMirror *state = (ChannelMirror *)ch;
    std::atomic<int> acquired{0};

    std::thread holder([&]() {
        rt_monitor_enter(state->monitor);
        acquired.store(1, std::memory_order_release);
        rt_thread_sleep(220);
        rt_monitor_exit(state->monitor);
    });
    while (acquired.load(std::memory_order_acquire) == 0)
        rt_thread_sleep(1);

    auto start = std::chrono::steady_clock::now();
    assert(rt_channel_send_for(ch, item, 40) == 0);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    assert(elapsed >= 20);
    assert(elapsed < 180);
    holder.join();

    acquired.store(0, std::memory_order_release);
    std::thread second_holder([&]() {
        rt_monitor_enter(state->monitor);
        acquired.store(1, std::memory_order_release);
        rt_thread_sleep(220);
        rt_monitor_exit(state->monitor);
    });
    while (acquired.load(std::memory_order_acquire) == 0)
        rt_thread_sleep(1);

    void *out = nullptr;
    start = std::chrono::steady_clock::now();
    assert(rt_channel_recv_for(ch, &out, 40) == 0);
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    assert(out == nullptr);
    assert(elapsed >= 20);
    assert(elapsed < 180);
    second_holder.join();

    if (rt_obj_release_check0(item))
        rt_obj_free(item);
    rt_channel_close(ch);
}

static void test_sender_wait_count_overflow_traps() {
    void *sync = rt_channel_new(0);
    ChannelMirror *sync_state = (ChannelMirror *)sync;
    sync_state->waiting_senders = INT64_MAX;
    assert(expect_trap([&]() { rt_channel_send(sync, NULL); }));
    sync_state->waiting_senders = 0;
    rt_channel_close(sync);

    void *buffered = rt_channel_new(1);
    ChannelMirror *buffered_state = (ChannelMirror *)buffered;
    buffered_state->waiting_senders = INT64_MAX;
    assert(expect_trap([&]() { (void)rt_channel_send_for(buffered, NULL, 10); }));
    buffered_state->waiting_senders = 0;
    rt_channel_close(buffered);
}

static void test_receiver_wait_count_overflow_traps() {
    void *sync = rt_channel_new(0);
    ChannelMirror *sync_state = (ChannelMirror *)sync;
    sync_state->waiting_receivers = INT64_MAX;
    assert(expect_trap([&]() { (void)rt_channel_recv(sync); }));
    sync_state->waiting_receivers = 0;
    rt_channel_close(sync);

    void *buffered = rt_channel_new(1);
    ChannelMirror *buffered_state = (ChannelMirror *)buffered;
    buffered_state->waiting_receivers = INT64_MAX;
    void *out = NULL;
    assert(expect_trap([&]() { (void)rt_channel_recv_for(buffered, &out, 10); }));
    buffered_state->waiting_receivers = 0;
    rt_channel_close(buffered);
}

static void test_sync_epoch_overflow_traps() {
    void *ch = rt_channel_new(0);
    ChannelMirror *state = (ChannelMirror *)ch;
    state->waiting_receivers = 1;
    state->sync_epoch = INT64_MAX;

    assert(expect_trap([&]() { (void)rt_channel_try_send(ch, NULL); }));

    state->waiting_receivers = 0;
    state->sync_epoch = 0;
    rt_channel_close(ch);
}

//=============================================================================
// Null safety
//=============================================================================

static void test_null_safety() {
    assert(rt_channel_get_len(NULL) == 0);
    assert(rt_channel_get_cap(NULL) == 0);
    assert(rt_channel_get_is_closed(NULL) == 1);
    assert(rt_channel_get_is_empty(NULL) == 1);
    assert(rt_channel_get_is_full(NULL) == 0);
    assert(rt_channel_try_send(NULL, make_obj()) == 0);

    void *out = NULL;
    out = make_obj();
    assert(rt_channel_try_recv(NULL, &out) == 0);
    assert(out == NULL);
    out = make_obj();
    assert(rt_channel_recv_for(NULL, &out, 10) == 0);
    assert(out == NULL);
    assert(rt_channel_send_for(NULL, make_obj(), 10) == 0);

    /// @brief Rt_channel_close.
    rt_channel_close(NULL); // Should not crash
}

//=============================================================================
// Multi-threaded tests
//=============================================================================

static void test_producer_consumer() {
    void *ch = rt_channel_new(10);
    const int N = 50;
    void *items[50];
    int i;
    for (i = 0; i < N; i++)
        items[i] = make_obj();

    // Producer thread
    std::thread producer([ch, &items, N]() {
        for (int j = 0; j < N; j++)
            rt_channel_send(ch, items[j]);
    });

    // Consumer on main thread
    std::vector<void *> received;
    for (i = 0; i < N; i++) {
        void *out = NULL;
        int8_t ok = rt_channel_recv_for(ch, &out, 5000);
        assert(ok == 1);
        received.push_back(out);
    }

    producer.join();
    assert((int)received.size() == N);
    // Verify FIFO order
    for (i = 0; i < N; i++)
        assert(received[i] == items[i]);

    rt_channel_close(ch);
}

static void test_close_wakes_receiver() {
    void *ch = rt_channel_new(5);

    /// @brief Closer.
    std::thread closer([ch]() {
        rt_thread_sleep(50);
        rt_channel_close(ch);
    });

    // Blocking recv should return NULL when channel is closed
    void *result = rt_channel_recv(ch);
    assert(result == NULL);

    closer.join();
}

static void test_synchronous_channel() {
    void *ch = rt_channel_new(0); // Synchronous
    void *item = make_obj();

    // Need sender and receiver on separate threads for synchronous channel
    void *received = NULL;

    std::thread receiver([ch, &received]() { received = rt_channel_recv(ch); });

    // Give receiver time to start waiting
    rt_thread_sleep(20);
    rt_channel_send(ch, item);

    receiver.join();
    assert(received == item);
    rt_channel_close(ch);
}

static void test_synchronous_multi_sender_order() {
    void *ch = rt_channel_new(0);
    void *a = make_obj();
    void *b = make_obj();
    void *received[2] = {NULL, NULL};

    std::thread receiver([&]() {
        rt_thread_sleep(30);
        received[0] = rt_channel_recv(ch);
        rt_thread_sleep(30);
        received[1] = rt_channel_recv(ch);
    });

    std::thread sender1([&]() { rt_channel_send(ch, a); });
    rt_thread_sleep(5);
    std::thread sender2([&]() { rt_channel_send(ch, b); });

    sender1.join();
    sender2.join();
    receiver.join();

    assert(received[0] == a);
    assert(received[1] == b);
    rt_channel_close(ch);
}

static void test_synchronous_send_for_timeout_budget() {
    void *ch = rt_channel_new(0);
    void *item = make_obj();

    auto start = std::chrono::steady_clock::now();
    assert(rt_channel_send_for(ch, item, 40) == 0);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    assert(elapsed >= 30);
    assert(elapsed < 150);
    rt_channel_close(ch);
}

static void test_synchronous_probe_ignores_waiting_sender() {
    void *ch = rt_channel_new(0);
    void *item = make_obj();

    std::thread sender([&]() { rt_channel_send(ch, item); });

    rt_thread_sleep(20);
    assert(rt_channel_try_recv(ch, NULL) == 0);

    void *out = NULL;
    assert(rt_channel_recv_for(ch, &out, 1000) == 1);
    assert(out == item);

    sender.join();
    rt_channel_close(ch);
}

static void test_synchronous_is_full_honors_waiting_receiver() {
    void *ch = rt_channel_new(0);
    void *item = make_obj();
    void *received = NULL;
    std::atomic<int> receiver_started{0};

    std::thread receiver([&]() {
        receiver_started.store(1, std::memory_order_release);
        received = rt_channel_recv(ch);
    });

    while (receiver_started.load(std::memory_order_acquire) == 0)
        rt_thread_sleep(1);
    rt_thread_sleep(20);

    assert(rt_channel_get_is_full(ch) == 0);
    assert(rt_channel_send_for(ch, item, 1000) == 1);

    receiver.join();
    assert(received == item);
    rt_channel_close(ch);
}

static void test_synchronous_try_send_handoffs_to_waiting_receiver() {
    void *ch = rt_channel_new(0);
    void *item = make_obj();
    void *received = NULL;
    std::atomic<int> receiver_started{0};

    std::thread receiver([&]() {
        receiver_started.store(1, std::memory_order_release);
        received = rt_channel_recv(ch);
    });

    while (receiver_started.load(std::memory_order_acquire) == 0)
        rt_thread_sleep(1);
    rt_thread_sleep(20);

    assert(rt_channel_try_send(ch, item) == 1);
    receiver.join();
    assert(received == item);
    assert(rt_channel_get_len(ch) == 0);
    rt_channel_close(ch);
}

static void test_synchronous_try_recv_is_strictly_nonblocking() {
    void *ch = rt_channel_new(0);
    void *item = make_obj();

    std::thread sender([&]() { rt_channel_send(ch, item); });

    rt_thread_sleep(20);
    void *out = NULL;
    int8_t ok = rt_channel_try_recv(ch, &out);
    assert(ok == 0);
    assert(out == NULL);

    assert(rt_channel_recv_for(ch, &out, 1000) == 1);
    assert(out == item);

    sender.join();
    rt_channel_close(ch);
}

static void test_receiver_retain_overflow_stops_operation() {
    void *ch = rt_channel_new(1);
    rt_heap_hdr_t *hdr = rt_heap_hdr(ch);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    g_last_returning_trap.clear();
    g_return_traps = true;
    assert(rt_channel_get_len(ch) == 0);
    g_return_traps = false;

    assert(g_last_returning_trap.find("receiver refcount overflow") != std::string::npos);
    assert(hdr->refcnt == RT_HEAP_MAX_MORTAL_REFCNT);
    hdr->refcnt = 1;
    if (rt_obj_release_check0(ch))
        rt_obj_free(ch);
}

/// @brief Main.
int main() {
    test_new_buffered();
    test_new_synchronous();
    test_new_negative_capacity();
    test_try_send_recv();
    test_try_recv_empty();
    test_try_recv_null_out_does_not_consume();
    test_try_send_full();
    test_fifo_order();
    test_close_prevents_send();
    test_close_allows_drain();
    test_double_close();
    test_recv_for_timeout();
    test_recv_for_closed_clears_out();
    test_recv_for_immediate();
    test_huge_timeout_immediate_paths();
    test_managed_value_wrappers();
    test_send_for_timeout();
    test_send_for_zero_ms();
    test_send_retain_overflow_does_not_lock_channel();
    test_try_send_retain_overflow_does_not_lock_channel();
    test_send_for_retain_overflow_does_not_lock_channel();
    test_recv_for_discard_releases_after_unlock();
    test_recv_for_null_out_is_non_consuming_probe();
    test_try_recv_option_allocation_failure_releases_transfer();
    test_channel_cycle_is_collected();
    test_timed_operations_include_monitor_acquisition();
    test_sender_wait_count_overflow_traps();
    test_receiver_wait_count_overflow_traps();
    test_sync_epoch_overflow_traps();
    test_null_safety();
    test_producer_consumer();
    test_close_wakes_receiver();
    test_synchronous_channel();
    test_synchronous_multi_sender_order();
    test_synchronous_send_for_timeout_budget();
    test_synchronous_probe_ignores_waiting_sender();
    test_synchronous_is_full_honors_waiting_receiver();
    test_synchronous_try_send_handoffs_to_waiting_receiver();
    test_synchronous_try_recv_is_strictly_nonblocking();
    test_receiver_retain_overflow_stops_operation();

    printf("Channel tests: all passed\n");
    return 0;
}
