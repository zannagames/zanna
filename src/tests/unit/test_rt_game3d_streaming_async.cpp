//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d_streaming_async.cpp
// Purpose: Unit tests for worker-backed WorldStream3D streaming — async/blocking
//   residency parity, staging-error recovery, cancellation drops, commit-budget
//   pacing, velocity prefetch with teleport reset, and stall telemetry.
// Key invariants:
//   - Async mode reaches the same resident set blocking mode does (order-gated
//     nearest-first commits), it just spreads the work across updates.
//   - Worker staging failures are recoverable and never trap.
// Ownership/Lifetime:
//   - Test-created runtime handles rely on production GC conventions.
// Links: src/runtime/graphics/3d/rt_game3d_streaming.inc
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_g3d_commit_queue.h"
#include "rt_game3d.h"
#include "rt_game3d_diagnostics.h"
#include "rt_object.h"
#include "rt_scene3d.h"
#include "rt_string.h"
#include "rt_time.h"
#include "rt_vec3.h"

#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;
static int g_tests_passed = 0;
static int g_tests_total = 0;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

#define TEST(name)                                                                                 \
    do {                                                                                           \
        ++g_tests_total;                                                                           \
        std::printf("  [%d] %s... ", g_tests_total, name);                                         \
    } while (0)

#define PASS()                                                                                     \
    do {                                                                                           \
        ++g_tests_passed;                                                                          \
        std::printf("ok\n");                                                                       \
        return true;                                                                               \
    } while (0)

#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        std::printf("FAIL: %s\n", msg);                                                            \
        return false;                                                                              \
    } while (0)

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond))                                                                               \
            FAIL(msg);                                                                             \
    } while (0)

#define EXPECT_EQ_INT(actual, expected, msg)                                                       \
    do {                                                                                           \
        const long long got_ = (long long)(actual);                                                \
        const long long want_ = (long long)(expected);                                             \
        if (got_ != want_) {                                                                       \
            std::printf("FAIL: %s (got %lld, expected %lld)\n", msg, got_, want_);                 \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

bool write_text_file(const char *path, const char *text) {
    std::FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    const size_t len = std::strlen(text);
    const bool ok = len == 0 || std::fwrite(text, 1, len, f) == len;
    std::fclose(f);
    return ok;
}

bool write_cell_scene(const char *path, const char *marker_name) {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    rt_scene_node3d_set_name(node, rt_const_cstr(marker_name));
    rt_scene3d_add(scene, node);
    bool ok = rt_scene3d_save(scene, rt_const_cstr(path)) == 1;
    if (rt_obj_release_check0(node))
        rt_obj_free(node);
    if (rt_obj_release_check0(scene))
        rt_obj_free(scene);
    return ok;
}

void set_center(void *stream, double x, double z) {
    void *v = rt_vec3_new(x, 0.0, z);
    rt_game3d_world_stream_set_center(stream, v);
    if (rt_obj_release_check0(v))
        rt_obj_free(v);
}

/// Drive updates until the stream settles (pending drains) or the bound is hit.
void quiesce(void *stream, int max_iters = 400) {
    for (int i = 0; i < max_iters; ++i) {
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        if (rt_game3d_world_stream_get_pending_request_count(stream) == 0)
            return;
        rt_sleep_ms(1);
    }
}

//=========================================================================
// Async residency parity
//=========================================================================

bool test_async_parity_with_blocking() {
    TEST("async streaming reaches the same resident set as blocking mode");

    const char *near_path = "/tmp/zanna_g3d_async_near.vscn";
    const char *far_path = "/tmp/zanna_g3d_async_far.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_async_cells.json";
    EXPECT_TRUE(write_cell_scene(near_path, "async_near_marker"), "near fixture saves");
    EXPECT_TRUE(write_cell_scene(far_path, "async_far_marker"), "far fixture saves");

    char manifest[1024];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":["
                  "{\"name\":\"near\",\"path\":\"%s\",\"center\":[0,0,0],\"radius\":8,"
                  "\"bytes\":65536},"
                  "{\"name\":\"far\",\"path\":\"%s\",\"center\":[1000,0,0],\"radius\":8,"
                  "\"bytes\":65536}]}",
                  near_path,
                  far_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Async Stream Parity"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_async_streaming(stream),
                  1,
                  "worker-backed streaming is the default");
    set_center(stream, 0.0, 0.0);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    quiesce(stream);

    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_cell_count(stream), 1, "near cell becomes resident");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("async_near_marker")) != nullptr,
                "near cell subtree attaches");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("async_far_marker")) == nullptr,
                "far cell stays unloaded");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_bytes(stream) > 0,
                "resident bytes are measured");

    /* Cross to the far cell: async swap must land on the blocking end state. */
    set_center(stream, 1000.0, 0.0);
    quiesce(stream);
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_cell_count(stream), 1, "far swap keeps one resident");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("async_far_marker")) != nullptr,
                "far cell subtree attaches after the swap");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("async_near_marker")) == nullptr,
                "near cell unloads after the swap");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

//=========================================================================
// Commit-wrapper allocation failure
//=========================================================================

bool test_enqueue_allocation_failure_uses_emergency_handoff() {
    TEST("async streaming survives a commit-wrapper allocation failure");

    const char *cell_path = "/tmp/zanna_g3d_async_oom.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_async_oom_cells.json";
    EXPECT_TRUE(write_cell_scene(cell_path, "async_oom_marker"), "fixture saves");

    char manifest[512];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"cell\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Async Stream Enqueue OOM"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    set_center(stream, 0.0, 0.0);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);

    /* The worker has already retained the stream when it reaches this allocation. The
     * allocation-free emergency handoff must preserve both that retain and its staged payload. */
    rt_g3d_commit_queue_test_fail_next_allocations(1);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    quiesce(stream);
    rt_g3d_commit_queue_test_fail_next_allocations(0);

    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "emergency-handoff cell becomes resident");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("async_oom_marker")) != nullptr,
                "emergency-handoff payload attaches");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_pending_request_count(stream),
                  0,
                  "failed wrapper leaves no retained pending job");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

//=========================================================================
// Staging errors are recoverable
//=========================================================================

bool test_corrupt_cell_is_skipped() {
    TEST("corrupt staged cell is skipped with a diagnostic and no trap");

    const char *good_path = "/tmp/zanna_g3d_async_good.vscn";
    const char *bad_path = "/tmp/zanna_g3d_async_bad.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_async_bad_cells.json";
    EXPECT_TRUE(write_cell_scene(good_path, "async_good_marker"), "good fixture saves");
    EXPECT_TRUE(write_text_file(bad_path, "this is not a vscn payload"), "bad fixture writes");

    char manifest[1024];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":["
                  "{\"name\":\"bad\",\"path\":\"%s\",\"center\":[0,0,0],\"radius\":8,"
                  "\"bytes\":65536},"
                  "{\"name\":\"good\",\"path\":\"%s\",\"center\":[4,0,0],\"radius\":8,"
                  "\"bytes\":65536}]}",
                  bad_path,
                  good_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    const int64_t errors_before = rt_game3d_diagnostics_get_stream_staging_errors();

    void *world = rt_game3d_world_new(rt_const_cstr("Async Stream Corrupt"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    set_center(stream, 0.0, 0.0);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    quiesce(stream);
    /* The bad cell holds a reload cooldown (counted pending); keep updating until the
     * healthy sibling lands. */
    for (int i = 0; i < 400; ++i) {
        if (rt_game3d_world_find_node(world, rt_const_cstr("async_good_marker")) != nullptr)
            break;
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        rt_sleep_ms(1);
    }

    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("async_good_marker")) != nullptr,
                "healthy sibling cell still loads");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "only the healthy cell is resident");
    EXPECT_TRUE(rt_game3d_diagnostics_get_stream_staging_errors() > errors_before,
                "StreamStagingErrors counted the corrupt payload");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

//=========================================================================
// Cancellation
//=========================================================================

bool test_cancelled_stage_is_dropped() {
    TEST("staged payload for a no-longer-desired cell is dropped without leaking");

    const char *cell_path = "/tmp/zanna_g3d_async_cancel.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_async_cancel_cells.json";
    EXPECT_TRUE(write_cell_scene(cell_path, "async_cancel_marker"), "fixture saves");

    char manifest[512];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"target\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    const int64_t dropped_before = rt_game3d_diagnostics_get_stream_stale_stages_dropped();

    void *world = rt_game3d_world_new(rt_const_cstr("Async Stream Cancel"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_prefetch_lookahead(stream, 0.0); /* isolate cancellation */
    rt_game3d_world_stream_set_commit_budget(stream, 0);        /* hold commits */
    set_center(stream, 0.0, 0.0);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    /* Nothing resident yet: this baseline is the mounted manifest's own accounting. */
    const int64_t baseline_bytes = rt_game3d_world_stream_get_resident_bytes(stream);

    /* Let the worker stage the payload while commits are held... */
    for (int i = 0; i < 200; ++i) {
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        rt_sleep_ms(1);
    }
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  0,
                  "zero commit budget held the staged cell out of residency");

    /* ...then reverse away so the cell is no longer desired. */
    set_center(stream, 5000.0, 0.0);
    rt_game3d_world_stream_set_commit_budget(stream, -1);
    quiesce(stream);

    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  0,
                  "reversed center never commits the cancelled cell");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("async_cancel_marker")) == nullptr,
                "cancelled cell subtree never spawns");
    EXPECT_TRUE(rt_game3d_diagnostics_get_stream_stale_stages_dropped() > dropped_before,
                "stale-stage drop was counted");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_bytes(stream),
                  baseline_bytes,
                  "resident bytes return to the mounted-manifest baseline");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

//=========================================================================
// Commit budget pacing
//=========================================================================

bool test_zero_commit_budget_holds_then_releases() {
    TEST("zero commit budget holds staged commits and releases on restore");

    const char *cell_path = "/tmp/zanna_g3d_async_budget.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_async_budget_cells.json";
    EXPECT_TRUE(write_cell_scene(cell_path, "async_budget_marker"), "fixture saves");

    char manifest[512];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"cell\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Async Stream Budget"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_commit_budget(stream, 0);
    set_center(stream, 0.0, 0.0);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));

    for (int i = 0; i < 200; ++i) {
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        rt_sleep_ms(1);
    }
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_cell_count(stream), 0, "held cell is not resident");
    EXPECT_TRUE(rt_game3d_world_stream_get_pending_request_count(stream) > 0,
                "held cell stays pending");

    rt_game3d_world_stream_set_commit_budget(stream, -1);
    quiesce(stream);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "restored budget commits the held cell");
    EXPECT_TRUE(rt_game3d_world_stream_get_stream_stall_ms(stream) >= 0.0, "stall telemetry reads");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

//=========================================================================
// Prefetch
//=========================================================================

bool test_prefetch_stages_ahead_and_teleport_resets() {
    TEST("constant velocity prefetches the next cell; teleports do not");

    const char *ahead_path = "/tmp/zanna_g3d_async_ahead.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_async_prefetch_cells.json";
    EXPECT_TRUE(write_cell_scene(ahead_path, "async_ahead_marker"), "fixture saves");

    /* Cell sits at x=200; load radius 64 means it becomes desired at x>=128. */
    char manifest[512];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"ahead\",\"path\":\"%s\",\"center\":[200,0,0],"
                  "\"radius\":8,\"bytes\":65536}]}",
                  ahead_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Async Stream Prefetch"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_prefetch_lookahead(stream, 2.0);
    set_center(stream, 0.0, 0.0);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));

    /* Move at 30 u/s: the 2 s lookahead reaches the cell (x+60+64+8 >= 200) around
     * x >= 68 — long before the load radius crossing at x >= 128. */
    bool prefetched_before_crossing = false;
    double x = 0.0;
    for (int i = 0; i < 300 && x < 126.0; ++i) {
        x += 30.0 / 60.0;
        set_center(stream, x, 0.0);
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        if (rt_game3d_world_stream_get_prefetched_cell_count(stream) > 0)
            prefetched_before_crossing = true;
        if ((i & 7) == 0)
            rt_sleep_ms(1);
    }
    EXPECT_TRUE(prefetched_before_crossing,
                "cell staged from prefetch before crossing the load radius");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);

    /* Teleport: a fresh stream jumped straight past the same distance must not
     * prefetch along the phantom jump vector. */
    void *world2 = rt_game3d_world_new(rt_const_cstr("Async Stream Teleport"), 80, 60);
    void *stream2 = rt_game3d_world_get_stream(world2);
    rt_game3d_world_stream_set_prefetch_lookahead(stream2, 2.0);
    set_center(stream2, -1000.0, 0.0);
    rt_game3d_world_stream_set_radii(stream2, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream2, rt_const_cstr(manifest_path));
    rt_game3d_world_stream_update(stream2, 1.0 / 60.0);
    set_center(stream2, 0.0, 0.0); /* 1000-unit jump > unload radius = teleport */
    rt_game3d_world_stream_update(stream2, 1.0 / 60.0);
    rt_game3d_world_stream_update(stream2, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_prefetched_cell_count(stream2),
                  0,
                  "teleport does not prefetch along the jump");

    rt_game3d_world_destroy(world2);
    if (rt_obj_release_check0(world2))
        rt_obj_free(world2);
    PASS();
}

//=========================================================================
// Blocking fallback
//=========================================================================

bool test_blocking_fallback_is_synchronous() {
    TEST("setAsyncStreaming(false) restores single-update inline loads");

    const char *cell_path = "/tmp/zanna_g3d_async_block.vscn";
    const char *manifest_path = "/tmp/zanna_g3d_async_block_cells.json";
    EXPECT_TRUE(write_cell_scene(cell_path, "async_block_marker"), "fixture saves");

    char manifest[512];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"cell\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Blocking Stream"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream, 0);
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_async_streaming(stream), 0, "blocking mode reads back");
    set_center(stream, 0.0, 0.0);
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "blocking mode loads within one update");

    rt_game3d_world_destroy(world);
    if (rt_obj_release_check0(world))
        rt_obj_free(world);
    PASS();
}

} // namespace

int main() {
    std::printf("Game3D async streaming tests\n");
    bool ok = true;
    ok &= test_async_parity_with_blocking();
    ok &= test_enqueue_allocation_failure_uses_emergency_handoff();
    ok &= test_corrupt_cell_is_skipped();
    ok &= test_cancelled_stage_is_dropped();
    ok &= test_zero_commit_budget_holds_then_releases();
    ok &= test_prefetch_stages_ahead_and_teleport_resets();
    ok &= test_blocking_fallback_is_synchronous();
    std::printf("\nAsync streaming tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
