//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTConcMapTests.cpp
// Purpose: Tests for Zanna.Threads.ConcurrentMap thread-safe hash map.
// Key invariants: Concurrent mutation preserves map contents, cached hashes,
//                 ownership balance, GC-visible managed edges, and rejects a
//                 forged string key without aliasing it to the empty key.
// Ownership/Lifetime: Each test owns its map and inserted managed values and
//                     joins all worker threads before releasing shared state.
// Links: src/runtime/threads/rt_concmap.c,
//        docs/adr/0133-runtime-concurrency-and-collection-hardening.md
//
//===----------------------------------------------------------------------===//

#include "rt_array_obj.h"
#include "rt_concmap.h"
#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern "C" void rt_trap_set_recovery(jmp_buf *buf);
extern "C" void rt_trap_clear_recovery(void);

static bool g_return_traps = false;
static const char *g_last_returning_trap = nullptr;

extern "C" void vm_trap(const char *msg) {
    if (g_return_traps) {
        g_last_returning_trap = msg;
        return;
    }
    rt_abort(msg);
}

static rt_string make_str(const char *s) {
    return rt_const_cstr(s);
}

static void *new_obj() {
    void *p = rt_obj_new_i64(0, 8);
    assert(p != nullptr);
    return p;
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

static std::atomic<int> g_reentrant_value_finalized{0};
static void *g_reentrant_map = nullptr;

static void reentrant_map_value_finalizer(void *obj) {
    (void)obj;
    g_reentrant_value_finalized.fetch_add(1, std::memory_order_acq_rel);
    if (g_reentrant_map)
        (void)rt_concmap_len(g_reentrant_map);
}

// ============================================================================
// Basic operations
// ============================================================================

static void test_new() {
    void *m = rt_concmap_new();
    assert(m != nullptr);
    assert(rt_concmap_len(m) == 0);
    assert(rt_concmap_is_empty(m) == 1);
    printf("test_new: PASSED\n");
}

static void test_map_cycle_is_collected() {
    void *m = rt_concmap_new();
    void **array = rt_arr_obj_new(1);
    assert(m != nullptr);
    assert(array != nullptr);
    assert(rt_gc_is_tracked(m) == 1);

    rt_arr_obj_put(array, 0, m);
    rt_concmap_set(m, make_str("cycle"), array);
    if (rt_obj_release_check0(array))
        rt_obj_free(array);
    if (rt_obj_release_check0(m))
        rt_obj_free(m);

    assert(rt_gc_collect() >= 2);
    assert(rt_gc_is_tracked(m) == 0);
    assert(rt_gc_is_tracked(array) == 0);
    printf("test_map_cycle_is_collected: PASSED\n");
}

static void test_set_get() {
    void *m = rt_concmap_new();
    void *val = new_obj();
    rt_concmap_set(m, make_str("hello"), val);

    assert(rt_concmap_len(m) == 1);
    assert(rt_concmap_is_empty(m) == 0);

    void *result = rt_concmap_get(m, make_str("hello"));
    assert(result == val);

    printf("test_set_get: PASSED\n");
}

static void test_set_retain_overflow_cleans_up() {
    void *m = rt_concmap_new();
    rt_heap_hdr_t *map_hdr = rt_heap_hdr(m);
    void *val = new_obj();
    rt_heap_hdr_t *val_hdr = rt_heap_hdr(val);
    val_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    assert(expect_trap([&]() { rt_concmap_set(m, make_str("overflow"), val); }));
    assert(map_hdr->refcnt == 1);
    assert(rt_concmap_len(m) == 0);

    val_hdr->refcnt = 1;
    printf("test_set_retain_overflow_cleans_up: PASSED\n");
}

static void test_get_missing() {
    void *m = rt_concmap_new();
    void *result = rt_concmap_get(m, make_str("missing"));
    assert(result == nullptr);
    printf("test_get_missing: PASSED\n");
}

static void test_get_or() {
    void *m = rt_concmap_new();
    void *def = new_obj();

    void *result = rt_concmap_get_or(m, make_str("missing"), def);
    assert(result == def);

    void *val = new_obj();
    rt_concmap_set(m, make_str("key"), val);
    result = rt_concmap_get_or(m, make_str("key"), def);
    assert(result == val);

    printf("test_get_or: PASSED\n");
}

static void test_get_retain_overflow_unlocks_map() {
    void *m = rt_concmap_new();
    void *val = new_obj();
    rt_concmap_set(m, make_str("key"), val);

    rt_heap_hdr_t *val_hdr = rt_heap_hdr(val);
    val_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    assert(expect_trap([&]() { (void)rt_concmap_get(m, make_str("key")); }));
    assert(rt_concmap_len(m) == 1);

    val_hdr->refcnt = 2;
    printf("test_get_retain_overflow_unlocks_map: PASSED\n");
}

static void test_get_or_retain_overflow_unlocks_map() {
    void *m = rt_concmap_new();
    void *val = new_obj();
    void *def = new_obj();
    rt_concmap_set(m, make_str("key"), val);

    rt_heap_hdr_t *val_hdr = rt_heap_hdr(val);
    val_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    assert(expect_trap([&]() { (void)rt_concmap_get_or(m, make_str("key"), def); }));
    assert(rt_concmap_len(m) == 1);

    val_hdr->refcnt = 2;
    printf("test_get_or_retain_overflow_unlocks_map: PASSED\n");
}

static void test_has() {
    void *m = rt_concmap_new();
    assert(rt_concmap_has(m, make_str("key")) == 0);

    rt_concmap_set(m, make_str("key"), new_obj());
    assert(rt_concmap_has(m, make_str("key")) == 1);
    assert(rt_concmap_has(m, make_str("other")) == 0);

    printf("test_has: PASSED\n");
}

static void test_update() {
    void *m = rt_concmap_new();
    void *v1 = new_obj();
    void *v2 = new_obj();
    rt_concmap_set(m, make_str("key"), v1);
    rt_concmap_set(m, make_str("key"), v2);

    assert(rt_concmap_len(m) == 1);
    void *result = rt_concmap_get(m, make_str("key"));
    assert(result == v2);

    printf("test_update: PASSED\n");
}

static void test_remove() {
    void *m = rt_concmap_new();
    rt_concmap_set(m, make_str("key"), new_obj());
    assert(rt_concmap_len(m) == 1);

    int8_t removed = rt_concmap_remove(m, make_str("key"));
    assert(removed == 1);
    assert(rt_concmap_len(m) == 0);
    assert(rt_concmap_has(m, make_str("key")) == 0);

    removed = rt_concmap_remove(m, make_str("key"));
    assert(removed == 0);

    printf("test_remove: PASSED\n");
}

static void test_remove_releases_value_after_unlock() {
    void *m = rt_concmap_new();
    void *val = new_obj();
    rt_obj_set_finalizer(val, reentrant_map_value_finalizer);
    g_reentrant_value_finalized.store(0, std::memory_order_release);
    g_reentrant_map = m;

    rt_concmap_set(m, make_str("key"), val);
    if (rt_obj_release_check0(val))
        rt_obj_free(val);

    assert(rt_concmap_remove(m, make_str("key")) == 1);
    assert(g_reentrant_value_finalized.load(std::memory_order_acquire) == 1);
    g_reentrant_map = nullptr;

    printf("test_remove_releases_value_after_unlock: PASSED\n");
}

static void test_clear() {
    void *m = rt_concmap_new();
    rt_concmap_set(m, make_str("a"), new_obj());
    rt_concmap_set(m, make_str("b"), new_obj());
    rt_concmap_set(m, make_str("c"), new_obj());
    assert(rt_concmap_len(m) == 3);

    rt_concmap_clear(m);
    assert(rt_concmap_len(m) == 0);
    assert(rt_concmap_is_empty(m) == 1);

    printf("test_clear: PASSED\n");
}

static void test_clear_releases_values_after_unlock() {
    void *m = rt_concmap_new();
    void *val = new_obj();
    rt_obj_set_finalizer(val, reentrant_map_value_finalizer);
    g_reentrant_value_finalized.store(0, std::memory_order_release);
    g_reentrant_map = m;

    rt_concmap_set(m, make_str("key"), val);
    if (rt_obj_release_check0(val))
        rt_obj_free(val);

    rt_concmap_clear(m);
    assert(g_reentrant_value_finalized.load(std::memory_order_acquire) == 1);
    g_reentrant_map = nullptr;

    printf("test_clear_releases_values_after_unlock: PASSED\n");
}

static void test_set_if_missing() {
    void *m = rt_concmap_new();
    void *v1 = new_obj();
    void *v2 = new_obj();

    int8_t inserted = rt_concmap_set_if_missing(m, make_str("key"), v1);
    assert(inserted == 1);
    assert(rt_concmap_get(m, make_str("key")) == v1);

    inserted = rt_concmap_set_if_missing(m, make_str("key"), v2);
    assert(inserted == 0);
    assert(rt_concmap_get(m, make_str("key")) == v1);

    printf("test_set_if_missing: PASSED\n");
}

static void test_set_if_missing_retain_overflow_cleans_up() {
    void *m = rt_concmap_new();
    rt_heap_hdr_t *map_hdr = rt_heap_hdr(m);
    void *val = new_obj();
    rt_heap_hdr_t *val_hdr = rt_heap_hdr(val);
    val_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    assert(expect_trap([&]() { (void)rt_concmap_set_if_missing(m, make_str("key"), val); }));
    assert(map_hdr->refcnt == 1);
    assert(rt_concmap_len(m) == 0);

    val_hdr->refcnt = 1;
    printf("test_set_if_missing_retain_overflow_cleans_up: PASSED\n");
}

static void test_embedded_nul_keys_are_distinct() {
    void *m = rt_concmap_new();
    const char key1_bytes[3] = {'a', '\0', '1'};
    const char key2_bytes[3] = {'a', '\0', '2'};
    rt_string key1 = rt_string_from_bytes(key1_bytes, 3);
    rt_string key2 = rt_string_from_bytes(key2_bytes, 3);
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_concmap_set(m, key1, v1);
    rt_concmap_set(m, key2, v2);

    assert(rt_concmap_len(m) == 2);
    assert(rt_concmap_has(m, key1) == 1);
    assert(rt_concmap_has(m, key2) == 1);
    assert(rt_concmap_get(m, key1) == v1);
    assert(rt_concmap_get(m, key2) == v2);

    assert(rt_concmap_remove(m, key1) == 1);
    assert(rt_concmap_has(m, key1) == 0);
    assert(rt_concmap_has(m, key2) == 1);
    assert(rt_concmap_get(m, key2) == v2);

    printf("test_embedded_nul_keys_are_distinct: PASSED\n");
}

static void test_keys_values() {
    void *m = rt_concmap_new();
    rt_concmap_set(m, make_str("a"), new_obj());
    rt_concmap_set(m, make_str("b"), new_obj());

    void *keys = rt_concmap_keys(m);
    assert(rt_seq_len(keys) == 2);

    void *values = rt_concmap_values(m);
    assert(rt_seq_len(values) == 2);

    printf("test_keys_values: PASSED\n");
}

static void test_values_retain_overflow_unlocks_map() {
    void *m = rt_concmap_new();
    void *val = new_obj();
    rt_concmap_set(m, make_str("key"), val);

    rt_heap_hdr_t *val_hdr = rt_heap_hdr(val);
    val_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    assert(expect_trap([&]() { (void)rt_concmap_values(m); }));
    assert(rt_concmap_len(m) == 1);

    val_hdr->refcnt = 2;
    printf("test_values_retain_overflow_unlocks_map: PASSED\n");
}

static void test_snapshot_values_survive_clear() {
    void *m = rt_concmap_new();
    void *val = new_obj();
    rt_concmap_set(m, make_str("keep"), val);

    void *keys = rt_concmap_keys(m);
    void *values = rt_concmap_values(m);
    rt_concmap_clear(m);

    assert(rt_seq_len(keys) == 1);
    assert(strcmp(rt_string_cstr((rt_string)rt_seq_get(keys, 0)), "keep") == 0);
    assert(rt_seq_len(values) == 1);
    assert(rt_seq_get(values, 0) == val);
    assert(rt_obj_class_id(rt_seq_get(values, 0)) == 0);

    printf("test_snapshot_values_survive_clear: PASSED\n");
}

static void test_get_survives_remove() {
    void *m = rt_concmap_new();
    void *val = new_obj();
    rt_concmap_set(m, make_str("temp"), val);

    void *result = rt_concmap_get(m, make_str("temp"));
    assert(result == val);
    assert(rt_concmap_remove(m, make_str("temp")) == 1);
    assert(rt_obj_class_id(result) == 0);

    printf("test_get_survives_remove: PASSED\n");
}

static void test_many_entries() {
    void *m = rt_concmap_new();
    char buf[32];
    void *vals[100];

    /* Insert enough to trigger resize */
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof(buf), "key_%d", i);
        vals[i] = new_obj();
        rt_concmap_set(m, make_str(buf), vals[i]);
    }
    assert(rt_concmap_len(m) == 100);

    /* Verify all retrievable */
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof(buf), "key_%d", i);
        void *val = rt_concmap_get(m, make_str(buf));
        assert(val == vals[i]);
    }

    printf("test_many_entries: PASSED\n");
}

// ============================================================================
// Concurrency tests
// ============================================================================

static void test_concurrent_writes() {
    void *m = rt_concmap_new();
    const int N = 100;
    const int T = 4;

    auto worker = [&](int thread_id) {
        char buf[64];
        for (int i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "t%d_key_%d", thread_id, i);
            void *val = rt_obj_new_i64(0, 8);
            rt_concmap_set(m, rt_const_cstr(buf), val);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < T; t++) {
        threads.emplace_back(worker, t);
    }
    for (auto &th : threads) {
        th.join();
    }

    assert(rt_concmap_len(m) == N * T);
    printf("test_concurrent_writes: PASSED\n");
}

static void test_concurrent_read_write() {
    void *m = rt_concmap_new();

    /* Pre-populate */
    for (int i = 0; i < 50; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key_%d", i);
        rt_concmap_set(m, rt_const_cstr(buf), new_obj());
    }

    /* Readers and writers running concurrently */
    auto writer = [&]() {
        for (int i = 50; i < 100; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key_%d", i);
            rt_concmap_set(m, rt_const_cstr(buf), rt_obj_new_i64(0, 8));
        }
    };

    auto reader = [&]() {
        for (int i = 0; i < 50; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key_%d", i);
            /* Value may or may not be present during writes, but should not crash */
            (void)rt_concmap_get(m, rt_const_cstr(buf));
        }
    };

    std::thread w(writer);
    std::thread r1(reader);
    std::thread r2(reader);

    w.join();
    r1.join();
    r2.join();

    assert(rt_concmap_len(m) == 100);
    printf("test_concurrent_read_write: PASSED\n");
}

static void test_concurrent_set_if_missing() {
    void *m = rt_concmap_new();
    const int T = 4;

    /* All threads try to set the same key — only one should succeed */
    auto worker = [&](int thread_id) {
        void *val = rt_obj_new_i64(0, 8);
        rt_concmap_set_if_missing(m, rt_const_cstr("shared_key"), val);
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < T; t++) {
        threads.emplace_back(worker, t);
    }
    for (auto &th : threads) {
        th.join();
    }

    assert(rt_concmap_len(m) == 1);
    printf("test_concurrent_set_if_missing: PASSED\n");
}

static void test_invalid_key_returning_trap_does_not_mutate_empty_key() {
    void *m = rt_concmap_new();
    rt_string empty_key = make_str("");
    void *value = new_obj();
    rt_concmap_set(m, empty_key, value);
    if (rt_obj_release_check0(value))
        rt_obj_free(value);

    auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
    g_return_traps = true;
    g_last_returning_trap = nullptr;
    rt_concmap_set(m, forged, nullptr);
    assert(g_last_returning_trap != nullptr);
    assert(rt_concmap_len(m) == 1);

    g_last_returning_trap = nullptr;
    assert(rt_concmap_get(m, forged) == nullptr);
    assert(g_last_returning_trap != nullptr);

    g_last_returning_trap = nullptr;
    assert(rt_concmap_remove(m, forged) == 0);
    assert(g_last_returning_trap != nullptr);
    g_return_traps = false;

    void *stored = rt_concmap_get(m, empty_key);
    assert(stored == value);
    if (rt_obj_release_check0(stored))
        rt_obj_free(stored);
    rt_string_unref(empty_key);
    if (rt_obj_release_check0(m))
        rt_obj_free(m);
    printf("test_invalid_key_returning_trap_does_not_mutate_empty_key: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== ConcurrentMap Tests ===\n\n");

    /* Basic operations */
    test_new();
    test_map_cycle_is_collected();
    test_set_get();
    test_set_retain_overflow_cleans_up();
    test_get_missing();
    test_get_or();
    test_get_retain_overflow_unlocks_map();
    test_get_or_retain_overflow_unlocks_map();
    test_has();
    test_update();
    test_remove();
    test_remove_releases_value_after_unlock();
    test_clear();
    test_clear_releases_values_after_unlock();
    test_set_if_missing();
    test_set_if_missing_retain_overflow_cleans_up();
    test_embedded_nul_keys_are_distinct();
    test_keys_values();
    test_values_retain_overflow_unlocks_map();
    test_snapshot_values_survive_clear();
    test_get_survives_remove();
    test_many_entries();

    /* Concurrency */
    test_concurrent_writes();
    test_concurrent_read_write();
    test_concurrent_set_if_missing();
    test_invalid_key_returning_trap_does_not_mutate_empty_key();

    printf("\nAll ConcurrentMap tests passed!\n");
    return 0;
}
