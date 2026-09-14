//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTGCChurnTests.c
// Purpose: Reproduce long-session GC lookup degradation without timing assertions.
// Key invariants:
//   - Stable live populations must retain short unsuccessful lookup paths.
//   - Collision chains and relocated entries retain all collector metadata.
// Ownership/Lifetime:
//   - Tests own and release every managed payload; GC state is reset per case.
// Links: src/runtime/core/rt_gc.c, src/tests/runtime/RTGCHashTableTests.cpp
//
//===----------------------------------------------------------------------===//

// Compile the implementation into this test only, permitting structural cost
// assertions without adding diagnostics to the runtime ABI or timing hot paths.
#include "../../runtime/core/rt_gc.c"

static void churn_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    (void)obj;
    (void)visitor;
    (void)ctx;
}

/// @brief Return the worst unsuccessful lookup length across all start buckets.
static int64_t longest_miss(void) {
    int64_t longest = 0;
    for (int64_t start = 0; start < g_gc.capacity; ++start) {
        int64_t probes = 0;
        uint64_t slot = (uint64_t)start;
        while (probes < g_gc.capacity) {
            ++probes;
            if (g_gc.entries[slot].obj == GC_EMPTY)
                break;
            slot = (slot + 1) & (uint64_t)(g_gc.capacity - 1);
        }
        if (probes > longest)
            longest = probes;
    }
    return longest;
}

/// @brief Exercise allocation churn and wraparound collisions on real heap objects.
int test_gc_churn(void) {
    enum { N = 8192, COLLISIONS = 16 };

    void *objects[N];
    void *colliders[COLLISIONS];
    int failures = 0;
    rt_gc_shutdown();
    for (int i = 0; i < N; ++i)
        objects[i] = rt_obj_new_i64(0, sizeof(void *));

    // Keep one long-lived object, but rotate through many distinct temporary
    // addresses. Retain allocations so allocator address reuse cannot mask the
    // bug. Production untracking also runs for every untracked temporary free.
    rt_gc_track(objects[0], churn_traverse);
    for (int i = 1; i < N; ++i) {
        rt_gc_track(objects[i], churn_traverse);
        rt_gc_untrack(objects[i]);
    }
    int64_t probes = longest_miss();
    printf("GC churn: live=%lld capacity=%lld worst miss=%lld (expected <=2)\n",
           (long long)g_gc.count,
           (long long)g_gc.capacity,
           (long long)probes);
    failures += g_gc.count != 1 || g_gc.capacity != 64 || probes > 2;
    failures += !rt_gc_is_tracked(objects[0]);
    rt_gc_untrack(objects[0]);
    rt_gc_shutdown();

    // Select real addresses whose initial bucket is the final slot. The
    // resulting collision chain crosses index zero; remove its head and middle.
    int found = 0;
    for (int i = 0; i < N && found < COLLISIONS; ++i) {
        if ((ptr_hash(objects[i]) & 63u) == 63u)
            colliders[found++] = objects[i];
    }
    if (found != COLLISIONS) {
        ++failures;
    } else {
        for (int i = 0; i < COLLISIONS; ++i) {
            rt_gc_track(colliders[i], churn_traverse);
            int64_t slot = find_entry(colliders[i]);
            g_gc.entries[slot].trial_rc = 100 + i;
            g_gc.entries[slot].color = 2;
            g_gc.entries[slot].survived = 7;
            g_gc.entries[slot].finalizer_epoch = 123;
        }
        rt_gc_untrack(colliders[0]);
        rt_gc_untrack(colliders[8]);
        for (int i = 0; i < COLLISIONS; ++i) {
            int64_t slot = find_entry(colliders[i]);
            if (i == 0 || i == 8) {
                failures += slot != -1;
            } else if (slot < 0) {
                ++failures;
            } else {
                gc_entry *entry = &g_gc.entries[slot];
                failures += entry->trial_rc != 100 + i || entry->color != 2 ||
                            entry->survived != 7 || entry->finalizer_epoch != 123 ||
                            entry->traverse != churn_traverse;
            }
        }
        failures += longest_miss() > COLLISIONS - 1;

        // Move the head repeatedly to previously untracked payload addresses.
        // Relocation must not leave a second source of deleted-slot buildup.
        void *moving = colliders[1];
        for (int i = 0; i < N; ++i) {
            if (rt_gc_is_tracked(objects[i]))
                continue;
            rt_gc_relocate_payload(moving, objects[i]);
            failures += rt_gc_is_tracked(moving) || !rt_gc_is_tracked(objects[i]);
            moving = objects[i];
        }
        int64_t slot = find_entry(moving);
        failures += slot < 0 || g_gc.entries[slot].trial_rc != 101 ||
                    g_gc.entries[slot].survived != 7 || g_gc.entries[slot].finalizer_epoch != 123;
        failures += longest_miss() > COLLISIONS - 1;
        failures += g_gc.count != COLLISIONS - 2 || g_gc.capacity != 64;
    }
    for (int i = 0; i < N; ++i) {
        if (rt_obj_release_check0(objects[i]))
            rt_obj_free(objects[i]);
    }
    failures += rt_gc_tracked_count() != 0;
    rt_gc_shutdown();
    return failures;
}
