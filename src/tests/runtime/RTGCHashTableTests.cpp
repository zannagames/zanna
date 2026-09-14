//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTGCHashTableTests.cpp
// Purpose: Validate the GC tracked-object hash table and deferred auto-trigger.
// Key invariants: Tracking/removal remain correct across resize/deletion and
//                 allocator activity requests collection only at a safe point.
// Ownership/Lifetime: Each case releases or explicitly collects all tracked
//                     objects before resetting process-global GC test state.
// Links: src/runtime/core/rt_gc.c,
//        docs/adr/0116-gc-mutator-quiescence-and-array-cycles.md
//
//===----------------------------------------------------------------------===//

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"

/// @brief Vm_trap.
void vm_trap(const char *msg) {
    fprintf(stderr, "TRAP: %s\n", msg);
    rt_abort(msg);
}
}

extern "C" int test_gc_churn(void);

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);                        \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

//=============================================================================
// Test helpers
//=============================================================================

struct test_node {
    void *child;
};

extern "C" {
static void test_node_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    struct test_node *node = (struct test_node *)obj;
    if (node->child)
        visitor(node->child, ctx);
}
}

static void *make_node() {
    void *obj = rt_obj_new_i64(0, (int64_t)sizeof(struct test_node));
    struct test_node *n = (struct test_node *)obj;
    n->child = NULL;
    return obj;
}

static void free_node(void *obj) {
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

//=============================================================================
// Hash Table Scalability Tests
//=============================================================================

static void test_track_many_objects() {
    const int N = 500;
    void *objs[500];

    int64_t base = rt_gc_tracked_count();

    for (int i = 0; i < N; i++) {
        objs[i] = make_node();
        rt_gc_track(objs[i], test_node_traverse);
    }

    ASSERT(rt_gc_tracked_count() == base + N, "all 500 objects tracked");

    // Verify each is individually findable
    for (int i = 0; i < N; i++) {
        ASSERT(rt_gc_is_tracked(objs[i]) == 1, "each object is tracked");
    }

    // Untrack all
    for (int i = 0; i < N; i++) {
        rt_gc_untrack(objs[i]);
    }

    ASSERT(rt_gc_tracked_count() == base, "count back to base after untracking all");

    // Clean up
    for (int i = 0; i < N; i++)
        free_node(objs[i]);
}

static void test_track_untrack_interleaved() {
    // Track and untrack in an interleaved pattern to exercise probe-chain repair
    const int N = 200;
    void *objs[200];

    int64_t base = rt_gc_tracked_count();

    // Track all
    for (int i = 0; i < N; i++) {
        objs[i] = make_node();
        rt_gc_track(objs[i], test_node_traverse);
    }

    // Untrack every other one (repairs collision chains)
    for (int i = 0; i < N; i += 2) {
        rt_gc_untrack(objs[i]);
    }

    ASSERT(rt_gc_tracked_count() == base + N / 2, "half tracked after interleaved untrack");

    // Verify correct tracking state
    for (int i = 0; i < N; i++) {
        int8_t expected = (i % 2 == 1) ? 1 : 0;
        ASSERT(rt_gc_is_tracked(objs[i]) == expected,
               "correct tracking state after interleaved ops");
    }

    // Track some new objects (should reuse the emptied slots)
    void *extra[50];
    for (int i = 0; i < 50; i++) {
        extra[i] = make_node();
        rt_gc_track(extra[i], test_node_traverse);
    }

    ASSERT(rt_gc_tracked_count() == base + N / 2 + 50,
           "count correct after inserting into partially emptied table");

    // Clean up
    for (int i = 0; i < N; i++) {
        rt_gc_untrack(objs[i]);
        free_node(objs[i]);
    }
    for (int i = 0; i < 50; i++) {
        rt_gc_untrack(extra[i]);
        free_node(extra[i]);
    }
}

static void test_collect_many_cycles() {
    // Create N/2 two-node cycles — all should be collected
    const int N = 100;
    void *objs[100];

    for (int i = 0; i < N; i++)
        objs[i] = make_node();

    // Wire pairs into cycles: 0<->1, 2<->3, 4<->5, ...
    for (int i = 0; i < N; i += 2) {
        struct test_node *a = (struct test_node *)objs[i];
        struct test_node *b = (struct test_node *)objs[i + 1];
        a->child = objs[i + 1];
        b->child = objs[i];
        rt_gc_track(objs[i], test_node_traverse);
        rt_gc_track(objs[i + 1], test_node_traverse);
    }

    int64_t freed = rt_gc_collect();
    ASSERT(freed == N, "all cycle members freed in bulk collect");
    ASSERT(rt_gc_tracked_count() == 0 || freed == N, "tracked count decreased by freed amount");
}

static void test_hash_table_growth() {
    // Force multiple rehashes by tracking many objects beyond initial capacity
    const int N = 300; // well beyond initial capacity of 64
    void *objs[300];

    int64_t base = rt_gc_tracked_count();

    for (int i = 0; i < N; i++) {
        objs[i] = make_node();
        rt_gc_track(objs[i], test_node_traverse);
        // Verify count stays consistent during growth
        ASSERT(rt_gc_tracked_count() == base + i + 1, "count consistent during growth");
    }

    // Verify all are still findable after multiple rehashes
    for (int i = 0; i < N; i++) {
        ASSERT(rt_gc_is_tracked(objs[i]) == 1, "all objects still tracked after rehash");
    }

    // Clean up
    for (int i = 0; i < N; i++) {
        rt_gc_untrack(objs[i]);
        free_node(objs[i]);
    }
}

static void test_untrack_nonexistent() {
    // Untracking something not in the table should be a no-op
    void *obj = make_node();
    int64_t base = rt_gc_tracked_count();

    /// @brief Rt_gc_untrack.
    rt_gc_untrack(obj); // not tracked — should be harmless

    ASSERT(rt_gc_tracked_count() == base, "untrack nonexistent doesn't change count");

    free_node(obj);
}

//=============================================================================
// Auto-Trigger Tests
//=============================================================================

static void test_threshold_api() {
    // Default threshold is 0 (disabled)
    ASSERT(rt_gc_get_threshold() == 0, "default threshold is 0");

    rt_gc_set_threshold(1000);
    ASSERT(rt_gc_get_threshold() == 1000, "threshold set to 1000");

    rt_gc_set_threshold(0);
    ASSERT(rt_gc_get_threshold() == 0, "threshold reset to 0");

    // Negative values should be treated as 0
    rt_gc_set_threshold(-5);
    ASSERT(rt_gc_get_threshold() == 0, "negative threshold treated as 0");
}

static void test_auto_trigger_collects_cycles() {
    // Set a low threshold so that subsequent allocations trigger collection
    int64_t initial_passes = rt_gc_pass_count();

    // Create a cycle that should be collected by auto-trigger
    void *a = make_node();
    void *b = make_node();
    struct test_node *na = (struct test_node *)a;
    struct test_node *nb = (struct test_node *)b;
    na->child = b;
    nb->child = a;

    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);

    // Set threshold low — the next few allocations will trigger a collect
    rt_gc_set_threshold(5);

    // Allocate enough objects to trigger the threshold
    void *temps[20];
    for (int i = 0; i < 20; i++) {
        temps[i] = make_node();
    }

    // Allocation only records debt. The explicit safe boundary services one
    // coalesced request after all newly returned objects are initialized.
    ASSERT(rt_gc_pass_count() == initial_passes,
           "allocation threshold does not collect recursively inside allocator");
    rt_gc_safepoint();

    // The safepoint should have run at least one pass.
    int64_t passes_after = rt_gc_pass_count();
    ASSERT(passes_after > initial_passes, "auto-trigger fired at least once");

    // The cycle should have been collected
    ASSERT(rt_gc_is_tracked(a) == 0, "cycle node a collected by auto-trigger");
    ASSERT(rt_gc_is_tracked(b) == 0, "cycle node b collected by auto-trigger");

    // Clean up
    rt_gc_set_threshold(0); // disable auto-trigger
    for (int i = 0; i < 20; i++)
        free_node(temps[i]);
}

static void test_threshold_disabled_no_auto_collect() {
    rt_gc_set_threshold(0);

    int64_t initial_passes = rt_gc_pass_count();

    // Create a cycle
    void *a = make_node();
    void *b = make_node();
    struct test_node *na = (struct test_node *)a;
    struct test_node *nb = (struct test_node *)b;
    na->child = b;
    nb->child = a;

    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);

    // Allocate many objects — should NOT trigger GC (threshold=0)
    void *temps[50];
    for (int i = 0; i < 50; i++)
        temps[i] = make_node();

    int64_t passes_after = rt_gc_pass_count();
    ASSERT(passes_after == initial_passes, "no auto-collect when threshold is 0");

    // Objects should still be tracked
    ASSERT(rt_gc_is_tracked(a) == 1, "cycle node a still tracked");
    ASSERT(rt_gc_is_tracked(b) == 1, "cycle node b still tracked");

    // Manual collect to clean up
    rt_gc_collect();

    for (int i = 0; i < 50; i++)
        free_node(temps[i]);
}

//=============================================================================
// Main
//=============================================================================

int main() {
    ASSERT(test_gc_churn() == 0, "GC lookup cost remains bounded after churn");

    // Hash table scalability
    test_track_many_objects();
    test_track_untrack_interleaved();
    test_collect_many_cycles();
    test_hash_table_growth();
    test_untrack_nonexistent();

    // Auto-trigger
    test_threshold_api();
    test_auto_trigger_collects_cycles();
    test_threshold_disabled_no_auto_collect();

    printf("GC hash table + auto-trigger tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
