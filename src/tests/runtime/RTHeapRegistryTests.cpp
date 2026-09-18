//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTHeapRegistryTests.cpp
// Purpose: Verify the lock-free live-payload registry: lookups agree with the
//          registered set across table growth and tombstones, the retained-slot
//          class checks read the header exactly, and concurrent readers never
//          see a false negative while other threads allocate and free.
// Key invariants:
//   - A registered payload is reported live by every reader entry point.
//   - A freed payload is reported dead without its header being read.
//   - Readers running against allocator churn on other threads never miss a
//     payload that was live before they started.
// Ownership/Lifetime:
//   - Every object the test allocates is released before main returns.
// Links: src/runtime/core/rt_heap.c, src/runtime/oop/rt_object.c
//
//===----------------------------------------------------------------------===//

#include "rt_heap.h"
#include "rt_object.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int64_t kClassA = 7001;
constexpr int64_t kClassB = 7002;
constexpr int64_t kPayloadBytes = 48;

void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

void free_object(void *obj) {
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

//=============================================================================
// Lookups agree with the registered set across growth and tombstones
//=============================================================================
void test_lookup_matches_registry() {
    printf("Testing lock-free lookups across growth:\n");
    const int count = 6000; /* forces several table doublings from 256 slots */
    std::vector<void *> objs;
    objs.reserve(count);
    for (int i = 0; i < count; i++) {
        void *o = rt_obj_new_i64(kClassA, kPayloadBytes);
        assert(o != nullptr);
        objs.push_back(o);
    }

    bool all_live = true;
    bool retained_agrees = true;
    bool info_agrees = true;
    bool min_bytes_enforced = true;
    for (void *o : objs) {
        rt_heap_info_t info;
        all_live = all_live && rt_heap_is_payload(o) == 1 && rt_obj_class_id(o) == kClassA &&
                   rt_obj_is_instance(o, kClassA, kPayloadBytes) == 1 &&
                   rt_obj_is_instance(o, kClassB, kPayloadBytes) == 0;
        retained_agrees = retained_agrees && rt_obj_class_id_retained(o) == kClassA &&
                          rt_obj_is_instance_retained(o, kClassA, kPayloadBytes) == 1 &&
                          rt_obj_is_instance_retained(o, kClassB, kPayloadBytes) == 0;
        info_agrees = info_agrees && rt_heap_get_info(o, &info) == 1 &&
                      (rt_heap_kind_t)info.kind == RT_HEAP_OBJECT && info.class_id == kClassA;
        min_bytes_enforced = min_bytes_enforced &&
                             rt_obj_is_instance(o, kClassA, kPayloadBytes + 1) == 0 &&
                             rt_obj_is_instance_retained(o, kClassA, kPayloadBytes + 1) == 0;
    }
    test_result("every registered object is live on every reader path", all_live);
    test_result("retained-slot checks agree with the registry", retained_agrees);
    test_result("get_info reports kind and class without a lock", info_agrees);
    test_result("minimum payload size is enforced by both variants", min_bytes_enforced);

    /* Free every other object: tombstones in the probe chains, addresses that
       must now read as dead without any header access. */
    std::vector<void *> freed;
    for (size_t i = 0; i < objs.size(); i += 2) {
        freed.push_back(objs[i]);
        free_object(objs[i]);
        objs[i] = nullptr;
    }
    bool dead_reported = true;
    for (void *f : freed)
        dead_reported = dead_reported && rt_heap_is_payload(f) == 0 && rt_obj_class_id(f) == 0 &&
                        rt_obj_is_instance(f, kClassA, kPayloadBytes) == 0;
    test_result("freed payloads read as dead through the registry", dead_reported);

    bool survivors_live = true;
    for (void *o : objs) {
        if (!o)
            continue;
        survivors_live = survivors_live && rt_obj_class_id(o) == kClassA &&
                         rt_obj_class_id_retained(o) == kClassA;
    }
    test_result("survivors stay live past their neighbours' tombstones", survivors_live);

    int on_stack = 0;
    test_result("stack addresses are not payloads",
                rt_heap_is_payload(&on_stack) == 0 && rt_obj_class_id(&on_stack) == 0 &&
                    rt_obj_is_instance(&on_stack, kClassA, 1) == 0);
    test_result("null is rejected by the retained variants",
                rt_obj_class_id_retained(nullptr) == 0 &&
                    rt_obj_is_instance_retained(nullptr, kClassA, 1) == 0);

    for (void *o : objs)
        if (o)
            free_object(o);
    printf("\n");
}

//=============================================================================
// Concurrent readers never miss a live payload while other threads churn
//=============================================================================
void test_concurrent_readers_during_churn() {
    printf("Testing concurrent readers against allocator churn:\n");
    const int live_count = 2000;
    std::vector<void *> live;
    live.reserve(live_count);
    for (int i = 0; i < live_count; i++) {
        void *o = rt_obj_new_i64(kClassA, kPayloadBytes);
        assert(o != nullptr);
        live.push_back(o);
    }

    std::atomic<bool> stop{false};
    std::atomic<long> false_negatives{0};
    std::atomic<long> checks{0};
    std::atomic<long> churned{0};

    auto reader = [&]() {
        long local_checks = 0;
        long local_misses = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            for (void *o : live) {
                rt_heap_info_t info;
                bool ok = rt_obj_is_instance(o, kClassA, kPayloadBytes) == 1 &&
                          rt_obj_class_id(o) == kClassA && rt_heap_is_payload(o) == 1 &&
                          rt_heap_get_info(o, &info) == 1 && info.class_id == kClassA;
                if (!ok)
                    local_misses++;
                local_checks++;
            }
        }
        checks.fetch_add(local_checks);
        false_negatives.fetch_add(local_misses);
    };

    auto writer = [&]() {
        long local = 0;
        std::vector<void *> batch;
        batch.reserve(512);
        while (!stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 512; i++) {
                void *o = rt_obj_new_i64(kClassB, 32);
                assert(o != nullptr);
                batch.push_back(o);
            }
            for (void *o : batch)
                free_object(o);
            local += (long)batch.size();
            batch.clear();
        }
        churned.fetch_add(local);
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++)
        threads.emplace_back(reader);
    for (int i = 0; i < 2; i++)
        threads.emplace_back(writer);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true);
    for (auto &t : threads)
        t.join();

    printf("  checks=%ld churned=%ld misses=%ld\n",
           checks.load(),
           churned.load(),
           false_negatives.load());
    test_result("readers performed work", checks.load() > 0);
    test_result("writers churned the registry", churned.load() > 0);
    test_result("no live payload was ever reported dead", false_negatives.load() == 0);

    for (void *o : live)
        free_object(o);
    printf("\n");
}

} // namespace

int main() {
    printf("=== rt_heap registry tests ===\n\n");
    test_lookup_matches_registry();
    test_concurrent_readers_during_churn();
    printf("All registry tests passed.\n");
    return 0;
}
