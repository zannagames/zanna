//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTPoolTests.cpp
// Purpose: Unit tests for rt_pool slab allocator.
// Key invariants:
//   - Pool allocates from size classes 64, 128, 256, 512
//   - Allocations > 512 fall back to malloc
//   - Freed blocks are recycled via freelist
//   - Allocated memory is zeroed
// Ownership/Lifetime:
//   - Test-only file; no production ownership
// Links: src/runtime/core/rt_pool.h, src/runtime/core/rt_pool.c
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_pool.h"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static bool g_return_pool_traps = false;
static std::string g_pool_trap_message;

extern "C" void vm_trap(const char *msg) {
    if (g_return_pool_traps) {
        g_pool_trap_message = msg ? msg : "";
        return;
    }
    fprintf(stderr, "TRAP: %s\n", msg);
    rt_abort(msg);
}

// ============================================================================
// test_alloc_64 — Allocate 64-byte block, write to it, free
// ============================================================================

static void test_alloc_64(void) {
    void *p = rt_pool_alloc(64);
    assert(p != nullptr);
    memset(p, 0xAB, 64);
    rt_pool_free(p, 64);
    printf("test_alloc_64: PASSED\n");
}

// ============================================================================
// test_alloc_128 — Allocate 128-byte block, write to it, free
// ============================================================================

static void test_alloc_128(void) {
    void *p = rt_pool_alloc(128);
    assert(p != nullptr);
    memset(p, 0xCD, 128);
    rt_pool_free(p, 128);
    printf("test_alloc_128: PASSED\n");
}

// ============================================================================
// test_alloc_256 — Allocate 256-byte block, write to it, free
// ============================================================================

static void test_alloc_256(void) {
    void *p = rt_pool_alloc(256);
    assert(p != nullptr);
    memset(p, 0xEF, 256);
    rt_pool_free(p, 256);
    printf("test_alloc_256: PASSED\n");
}

// ============================================================================
// test_alloc_512 — Allocate 512-byte block, write to it, free
// ============================================================================

static void test_alloc_512(void) {
    void *p = rt_pool_alloc(512);
    assert(p != nullptr);
    memset(p, 0x42, 512);
    rt_pool_free(p, 512);
    printf("test_alloc_512: PASSED\n");
}

// ============================================================================
// test_alloc_large — Allocate >512 bytes (falls through to malloc)
// ============================================================================

static void test_alloc_large(void) {
    void *p = rt_pool_alloc(1024);
    assert(p != nullptr);
    memset(p, 0xFF, 1024);
    rt_pool_free(p, 1024);
    printf("test_alloc_large: PASSED\n");
}

// ============================================================================
// test_zeroed — Allocated memory must be zeroed
// ============================================================================

static void test_zeroed(void) {
    void *p = rt_pool_alloc(64);
    assert(p != nullptr);

    const unsigned char *bytes = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < 64; i++) {
        assert(bytes[i] == 0 && "pool-allocated memory must be zeroed");
    }

    rt_pool_free(p, 64);
    printf("test_zeroed: PASSED\n");
}

// ============================================================================
// test_reuse — Alloc, free, alloc same size should recycle the block
// ============================================================================

static void test_reuse(void) {
    void *first = rt_pool_alloc(64);
    assert(first != nullptr);
    void *saved = first;
    rt_pool_free(first, 64);

    void *second = rt_pool_alloc(64);
    assert(second != nullptr);
    // In a single-threaded scenario the freed block should be reused.
    assert(second == saved && "pool should recycle freed block");

    // Recycled block must still be zeroed.
    const unsigned char *bytes = static_cast<const unsigned char *>(second);
    for (size_t i = 0; i < 64; i++) {
        assert(bytes[i] == 0 && "recycled block must be zeroed");
    }

    rt_pool_free(second, 64);
    printf("test_reuse: PASSED\n");
}

// ============================================================================
// test_stats — Verify rt_pool_stats reports correct counts
// ============================================================================

static void test_stats(void) {
    // Shut down pools to get a clean baseline.
    rt_pool_shutdown();

    size_t allocated = 0, free_count = 0;

    // Before any allocation, both should be zero.
    rt_pool_stats(RT_POOL_128, &allocated, &free_count);
    assert(allocated == 0);
    assert(free_count == 0);

    // Allocate one 128-byte block.
    void *p = rt_pool_alloc(128);
    assert(p != nullptr);

    rt_pool_stats(RT_POOL_128, &allocated, &free_count);
    assert(allocated == 1);
    // A slab was created with 64 blocks; one was taken, so 63 remain free.
    assert(free_count == 63);

    // Free it — allocated drops to 0, free goes up to 64.
    rt_pool_free(p, 128);
    rt_pool_stats(RT_POOL_128, &allocated, &free_count);
    assert(allocated == 0);
    assert(free_count == 64);

    printf("test_stats: PASSED\n");
}

/// @brief Verify invalid enum values cannot index before or after the pool table.
static void test_stats_reject_invalid_classes(void) {
    size_t allocated = 7;
    size_t free_count = 9;
    rt_pool_stats(static_cast<rt_pool_class_t>(-1), &allocated, &free_count);
    assert(allocated == 0);
    assert(free_count == 0);

    allocated = 7;
    free_count = 9;
    rt_pool_stats(RT_POOL_COUNT, &allocated, &free_count);
    assert(allocated == 0);
    assert(free_count == 0);

    printf("test_stats_reject_invalid_classes: PASSED\n");
}

/// @brief Stress concurrent freelist publication while sampling the free counter.
/// @details Workers keep nearly every block in one slab checked out, repeatedly
///          returning and reacquiring their final blocks. A freelist node must
///          never become visible to another pop before its counter increment;
///          otherwise an empty-list handoff can transiently wrap the statistic
///          to a value near SIZE_MAX.
static void test_live_stats_do_not_underflow(void) {
    rt_pool_shutdown();

    constexpr size_t kBlockCount = 64;
    constexpr size_t kWorkerCount = 4;
    constexpr int kIterations = 25000;
    std::vector<void *> blocks(kBlockCount, nullptr);
    for (void *&block : blocks) {
        block = rt_pool_alloc(64);
        assert(block != nullptr);
    }

    std::atomic<bool> start{false};
    std::atomic<size_t> active{kWorkerCount};
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < kWorkerCount; ++worker) {
        workers.emplace_back([&, worker] {
            void *owned = blocks[kBlockCount - kWorkerCount + worker];
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                rt_pool_free(owned, 64);
                owned = rt_pool_alloc(64);
                if (!owned) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            blocks[kBlockCount - kWorkerCount + worker] = owned;
            active.fetch_sub(1, std::memory_order_release);
        });
    }

    start.store(true, std::memory_order_release);
    while (active.load(std::memory_order_acquire) != 0) {
        size_t free_count = 0;
        rt_pool_stats(RT_POOL_64, nullptr, &free_count);
        if (free_count > SIZE_MAX / 2)
            failures.fetch_add(1, std::memory_order_relaxed);
    }

    for (auto &worker : workers)
        worker.join();
    assert(failures.load(std::memory_order_relaxed) == 0);

    for (void *block : blocks) {
        if (block)
            rt_pool_free(block, 64);
    }
    rt_pool_shutdown();
    printf("test_live_stats_do_not_underflow: PASSED\n");
}

/// @brief Verify shutdown defers slab reclamation while a block remains live.
/// @details A shutdown request must not invalidate outstanding allocations or
///          route their later release through system free. Once the last block
///          is returned, a subsequent shutdown may reclaim and reset the class.
static void test_shutdown_defers_live_slab(void) {
    rt_pool_shutdown();

    void *live = rt_pool_alloc(128);
    assert(live != nullptr);
    std::memset(live, 0x5A, 128);

    rt_pool_shutdown();

    const unsigned char *bytes = static_cast<const unsigned char *>(live);
    for (size_t i = 0; i < 128; ++i)
        assert(bytes[i] == 0x5A && "shutdown must preserve outstanding pool blocks");

    size_t allocated = 0;
    size_t free_count = 0;
    rt_pool_stats(RT_POOL_128, &allocated, &free_count);
    assert(allocated == 1);
    assert(free_count == 63);

    rt_pool_free(live, 128);
    rt_pool_shutdown();
    rt_pool_stats(RT_POOL_128, &allocated, &free_count);
    assert(allocated == 0);
    assert(free_count == 0);

    printf("test_shutdown_defers_live_slab: PASSED\n");
}

/// @brief Verify a duplicate free cannot duplicate one freelist entry or underflow statistics.
/// @details Uses the runtime's supported returning-trap contract so the second
///          release completes locally. The original free must remain the only
///          state transition, leaving exactly one free block and no allocated
///          blocks in the size class.
static void test_double_free_is_rejected(void) {
    rt_pool_shutdown();

    void *block = rt_pool_alloc(64);
    assert(block != nullptr);
    rt_pool_free(block, 64);

    g_pool_trap_message.clear();
    g_return_pool_traps = true;
    rt_pool_free(block, 64);
    g_return_pool_traps = false;

    assert(g_pool_trap_message.find("double free") != std::string::npos);
    size_t allocated = 0;
    size_t free_count = 0;
    rt_pool_stats(RT_POOL_64, &allocated, &free_count);
    assert(allocated == 0);
    assert(free_count == 64);

    rt_pool_shutdown();
    printf("test_double_free_is_rejected: PASSED\n");
}

/// @brief Verify a caller-supplied size cannot disagree with private slab ownership.
/// @details A rejected free must leave the block allocated so a caller whose trap
///          hook returns can release it with the original size.
static void test_mismatched_small_size_class_is_rejected(void) {
    struct MismatchCase {
        size_t allocated_size;
        size_t wrong_size;
        rt_pool_class_t owner_class;
    };

    constexpr MismatchCase cases[] = {
        {1, 65, RT_POOL_64},
        {65, 129, RT_POOL_128},
        {129, 257, RT_POOL_256},
        {257, 1, RT_POOL_512},
    };

    for (const MismatchCase &test_case : cases) {
        rt_pool_shutdown();
        void *block = rt_pool_alloc(test_case.allocated_size);
        assert(block != nullptr);

        g_pool_trap_message.clear();
        g_return_pool_traps = true;
        rt_pool_free(block, test_case.wrong_size);
        g_return_pool_traps = false;

        assert(g_pool_trap_message.find("size class mismatch") != std::string::npos);
        size_t allocated = 0;
        rt_pool_stats(test_case.owner_class, &allocated, nullptr);
        assert(allocated == 1);

        rt_pool_free(block, test_case.allocated_size);
        rt_pool_shutdown();
    }

    printf("test_mismatched_small_size_class_is_rejected: PASSED\n");
}

/// @brief Verify caller size cannot choose the wrong slab/system free path.
/// @details Both directions must trap before releasing or reading through the
///          wrong provenance, and the allocation must remain retryable with its
///          exact original size under a returning trap hook.
static void test_pooled_fallback_boundary_mismatch_is_rejected(void) {
    rt_pool_shutdown();

    void *pooled = rt_pool_alloc(64);
    assert(pooled != nullptr);
    g_pool_trap_message.clear();
    g_return_pool_traps = true;
    rt_pool_free(pooled, 513);
    g_return_pool_traps = false;
    assert(g_pool_trap_message.find("provenance mismatch") != std::string::npos);
    size_t pooled_allocated = 0;
    rt_pool_stats(RT_POOL_64, &pooled_allocated, nullptr);
    assert(pooled_allocated == 1);
    std::memset(pooled, 0xA5, 64);
    rt_pool_free(pooled, 64);

    void *fallback = rt_pool_alloc(513);
    assert(fallback != nullptr);
    g_pool_trap_message.clear();
    g_return_pool_traps = true;
    rt_pool_free(fallback, 64);
    g_return_pool_traps = false;
    assert(g_pool_trap_message.find("provenance mismatch") != std::string::npos);
    std::memset(fallback, 0x5A, 513);
    rt_pool_free(fallback, 513);

    rt_pool_shutdown();
    printf("test_pooled_fallback_boundary_mismatch_is_rejected: PASSED\n");
}

/// @brief Verify the documented exact-size contract within one provenance class.
/// @details Same-class slab sizes and two system-fallback sizes previously
///          reached the same release route despite disagreeing with allocation.
static void test_exact_request_size_is_enforced(void) {
    rt_pool_shutdown();

    void *pooled = rt_pool_alloc(32);
    assert(pooled != nullptr);
    g_pool_trap_message.clear();
    g_return_pool_traps = true;
    rt_pool_free(pooled, 64);
    g_return_pool_traps = false;
    assert(g_pool_trap_message.find("allocation size mismatch") != std::string::npos);
    rt_pool_free(pooled, 32);

    void *fallback = rt_pool_alloc(1024);
    assert(fallback != nullptr);
    g_pool_trap_message.clear();
    g_return_pool_traps = true;
    rt_pool_free(fallback, 2048);
    g_return_pool_traps = false;
    assert(g_pool_trap_message.find("allocation size mismatch") != std::string::npos);
    rt_pool_free(fallback, 1024);

    void *zero = rt_pool_alloc(0);
    assert(zero != nullptr);
    rt_pool_free(zero, 0);

    assert(rt_pool_alloc(SIZE_MAX) == nullptr);
    rt_pool_shutdown();
    printf("test_exact_request_size_is_enforced: PASSED\n");
}

/// @brief Stress the lock-free lifecycle epoch against allocation/free traffic.
/// @details Worker threads repeatedly touch caller-visible payloads while a
///          coordinator requests shutdown. A shutdown may reclaim only a
///          quiescent class; it must never invalidate a live payload, lose a
///          freelist node, or leave the lifecycle admission count stranded.
static void test_concurrent_shutdown_epoch(void) {
    rt_pool_shutdown();
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int iteration = 0; iteration < 4000; ++iteration) {
                size_t request = static_cast<size_t>(32 + ((worker + iteration) & 3) * 96);
                void *payload = rt_pool_alloc(request);
                if (!payload) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                unsigned char pattern = static_cast<unsigned char>(worker + 1);
                std::memset(payload, pattern, request);
                const auto *bytes = static_cast<const unsigned char *>(payload);
                if (bytes[0] != pattern || bytes[request - 1] != pattern)
                    failures.fetch_add(1, std::memory_order_relaxed);
                rt_pool_free(payload, request);
            }
        });
    }

    std::thread shutdown_thread([&] {
        start.store(true, std::memory_order_release);
        for (int iteration = 0; iteration < 128; ++iteration) {
            rt_pool_shutdown();
            std::this_thread::yield();
        }
    });

    for (auto &worker : workers)
        worker.join();
    shutdown_thread.join();
    assert(failures.load(std::memory_order_relaxed) == 0);

    rt_pool_shutdown();
    for (int class_index = 0; class_index < RT_POOL_COUNT; ++class_index) {
        size_t allocated = 1;
        size_t free_count = 1;
        rt_pool_stats(static_cast<rt_pool_class_t>(class_index), &allocated, &free_count);
        assert(allocated == 0);
        assert(free_count == 0);
    }
    printf("test_concurrent_shutdown_epoch: PASSED\n");
}

// ============================================================================
// test_many_allocs — Allocate 200+ small blocks, verify all non-NULL, free all
// ============================================================================

static void test_many_allocs(void) {
    static const int kCount = 200;
    void *ptrs[kCount];

    for (int i = 0; i < kCount; i++) {
        ptrs[i] = rt_pool_alloc(64);
        assert(ptrs[i] != nullptr);
    }

    // All pointers must be distinct.
    for (int i = 0; i < kCount; i++) {
        for (int j = i + 1; j < kCount; j++) {
            assert(ptrs[i] != ptrs[j] && "all allocations must be distinct");
        }
    }

    for (int i = 0; i < kCount; i++) {
        rt_pool_free(ptrs[i], 64);
    }

    printf("test_many_allocs: PASSED\n");
}

/// @brief Verify every pooled class preserves fundamental maximum alignment.
static void test_max_alignment(void) {
    constexpr size_t kRequests[] = {1, 64, 65, 128, 129, 256, 257, 512, 513, 1024};
    for (size_t request : kRequests) {
        void *payload = rt_pool_alloc(request);
        assert(payload != nullptr);
        assert(reinterpret_cast<uintptr_t>(payload) % alignof(std::max_align_t) == 0);
        rt_pool_free(payload, request);
    }

    printf("test_max_alignment: PASSED\n");
}

// ============================================================================
// test_null_free — rt_pool_free(NULL, size) must not crash
// ============================================================================

static void test_null_free(void) {
    rt_pool_free(nullptr, 64);
    rt_pool_free(nullptr, 128);
    rt_pool_free(nullptr, 256);
    rt_pool_free(nullptr, 512);
    rt_pool_free(nullptr, 1024);
    printf("test_null_free: PASSED\n");
}

// ============================================================================
// test_mixed_sizes — Allocate blocks of different sizes interleaved
// ============================================================================

static void test_mixed_sizes(void) {
    void *a = rt_pool_alloc(32);  // -> 64-byte class
    void *b = rt_pool_alloc(100); // -> 128-byte class
    void *c = rt_pool_alloc(200); // -> 256-byte class
    void *d = rt_pool_alloc(400); // -> 512-byte class
    void *e = rt_pool_alloc(64);  // -> 64-byte class

    assert(a != nullptr);
    assert(b != nullptr);
    assert(c != nullptr);
    assert(d != nullptr);
    assert(e != nullptr);

    // All must be distinct.
    assert(a != b);
    assert(a != c);
    assert(a != d);
    assert(b != c);
    assert(b != d);
    assert(c != d);
    assert(a != e);

    // Write different patterns to verify no overlap.
    memset(a, 0x11, 32);
    memset(b, 0x22, 100);
    memset(c, 0x33, 200);
    memset(d, 0x44, 400);
    memset(e, 0x55, 64);

    // Verify patterns are intact.
    const unsigned char *pa = static_cast<const unsigned char *>(a);
    for (size_t i = 0; i < 32; i++)
        assert(pa[i] == 0x11);

    const unsigned char *pb = static_cast<const unsigned char *>(b);
    for (size_t i = 0; i < 100; i++)
        assert(pb[i] == 0x22);

    const unsigned char *pc = static_cast<const unsigned char *>(c);
    for (size_t i = 0; i < 200; i++)
        assert(pc[i] == 0x33);

    const unsigned char *pd = static_cast<const unsigned char *>(d);
    for (size_t i = 0; i < 400; i++)
        assert(pd[i] == 0x44);

    const unsigned char *pe = static_cast<const unsigned char *>(e);
    for (size_t i = 0; i < 64; i++)
        assert(pe[i] == 0x55);

    rt_pool_free(a, 32);
    rt_pool_free(b, 100);
    rt_pool_free(c, 200);
    rt_pool_free(d, 400);
    rt_pool_free(e, 64);

    printf("test_mixed_sizes: PASSED\n");
}

// ============================================================================
// Entry point
// ============================================================================

int main(void) {
    printf("=== RT Pool Tests ===\n\n");

    test_alloc_64();
    test_alloc_128();
    test_alloc_256();
    test_alloc_512();
    test_alloc_large();
    test_zeroed();
    test_reuse();
    test_stats();
    test_stats_reject_invalid_classes();
    test_live_stats_do_not_underflow();
    test_shutdown_defers_live_slab();
    test_double_free_is_rejected();
    test_mismatched_small_size_class_is_rejected();
    test_pooled_fallback_boundary_mismatch_is_rejected();
    test_exact_request_size_is_enforced();
    test_concurrent_shutdown_epoch();
    test_many_allocs();
    test_max_alignment();
    test_null_free();
    test_mixed_sizes();

    printf("\nAll RT pool tests passed!\n");
    return 0;
}
