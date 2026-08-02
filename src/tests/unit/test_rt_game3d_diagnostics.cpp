//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_game3d_diagnostics.cpp
// Purpose: Unit tests for Game3D degradation diagnostics counters and summary.
//
// Key invariants:
//   - Each known rare fallback path increments exactly its matching counter.
//   - Diagnostics.Summary emits non-zero counters in a stable order.
//
// Ownership/Lifetime:
//   - Test-created runtime handles are process-local and intentionally short-lived.
//   - Diagnostics counters are reset between cases to isolate assertions.
//
// Links: src/runtime/graphics/3d/rt_game3d_diagnostics.h,
//   src/il/runtime/runtime.def, docs/zannalib/graphics/game3d.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_animcontroller3d.h"
#include "rt_game3d_diagnostics.h"
#include "rt_navmesh3d.h"
#include "rt_physics3d.h"
#include "rt_skeleton3d.h"
#include "rt_sound3d.h"
#include "rt_string.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
extern void *rt_mat4_identity(void);
extern void *rt_mesh3d_new_plane(double sx, double sz);
extern void *rt_quat_new(double x, double y, double z, double w);
extern void *rt_vec3_new(double x, double y, double z);
extern rt_string rt_const_cstr(const char *s);
extern void rt_world3d_test_set_broadphase_alloc_failure(int8_t enabled);
extern void rt_sound3d_test_set_all_voices_playing(int8_t enabled);
extern void rt_navmesh3d_test_set_query_grid_alloc_failure(int8_t enabled);
}

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond))                                                                               \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

#define EXPECT_EQ_I64(actual, expected, msg)                                                       \
    do {                                                                                           \
        tests_run++;                                                                               \
        int64_t got_value = (int64_t)(actual);                                                     \
        int64_t expected_value = (int64_t)(expected);                                              \
        if (got_value != expected_value)                                                           \
            std::fprintf(stderr,                                                                   \
                         "FAIL: %s (got %lld, expected %lld)\n",                                   \
                         msg,                                                                      \
                         (long long)got_value,                                                     \
                         (long long)expected_value);                                               \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

#define EXPECT_STREQ(actual, expected, msg)                                                        \
    do {                                                                                           \
        tests_run++;                                                                               \
        const char *got_text = (actual);                                                           \
        const char *expected_text = (expected);                                                    \
        if (std::strcmp(got_text, expected_text) != 0)                                             \
            std::fprintf(                                                                          \
                stderr, "FAIL: %s (got '%s', expected '%s')\n", msg, got_text, expected_text);     \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

static void *make_single_bone_animation(const char *name) {
    void *anim = rt_animation3d_new(rt_const_cstr(name), 1.0);
    void *pos0 = rt_vec3_new(0.0, 0.0, 0.0);
    void *pos1 = rt_vec3_new(1.0, 0.0, 0.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scale = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_set_looping(anim, 1);
    rt_animation3d_add_keyframe(anim, 0, 0.0, pos0, rot, scale);
    rt_animation3d_add_keyframe(anim, 0, 1.0, pos1, rot, scale);
    return anim;
}

static void test_summary_is_stable_and_resettable() {
    rt_game3d_diagnostics_reset();
    EXPECT_EQ_I64(rt_str_len(rt_game3d_diagnostics_summary()), 0, "Summary starts empty");

    rt_game3d_diag_record_broadphase_fallback();
    rt_game3d_diag_record_ccd_clamp(2);
    rt_game3d_diag_record_anim_events_dropped(3);
    rt_game3d_diag_record_audio_voice_evicted();
    rt_game3d_diag_record_nav_grid_fallback();
    rt_game3d_diag_record_stale_entity_call();
    rt_game3d_diag_record_stale_async_load_dropped();

    EXPECT_EQ_I64(rt_game3d_diagnostics_get_broadphase_fallback_count(),
                  1,
                  "Broadphase fallback getter reports direct record");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_ccd_clamped_frames(),
                  1,
                  "CCD frame getter reports direct record");
    EXPECT_EQ_I64(
        rt_game3d_diagnostics_get_ccd_clamped_bodies(), 2, "CCD body getter reports direct record");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_anim_events_dropped(),
                  3,
                  "Anim event getter reports direct record");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_audio_voices_evicted(),
                  1,
                  "Audio eviction getter reports direct record");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_nav_grid_fallbacks(),
                  1,
                  "Nav fallback getter reports direct record");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_stale_entity_calls(),
                  1,
                  "Stale entity getter reports direct record");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_stale_async_loads_dropped(),
                  1,
                  "Stale async load getter reports direct record");
    EXPECT_STREQ(rt_string_cstr(rt_game3d_diagnostics_summary()),
                 "BroadphaseFallbackCount=1\n"
                 "CcdClampedFrames=1\n"
                 "CcdClampedBodies=2\n"
                 "AnimEventsDropped=3\n"
                 "AudioVoicesEvicted=1\n"
                 "NavGridFallbacks=1\n"
                 "StaleEntityCalls=1\n"
                 "StaleAsyncLoadsDropped=1\n",
                 "Summary emits stable non-zero counter order");

    rt_game3d_diagnostics_reset();
    EXPECT_EQ_I64(rt_str_len(rt_game3d_diagnostics_summary()), 0, "Reset clears Summary output");
}

static void test_counters_are_atomic_and_saturating() {
    rt_game3d_diagnostics_reset();
    constexpr int worker_count = 8;
    constexpr int iterations = 10000;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (int worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([] {
            for (int iteration = 0; iteration < iterations; ++iteration) {
                rt_game3d_diag_record_broadphase_fallback();
                rt_game3d_diag_record_ccd_clamp(2);
                rt_game3d_diag_record_anim_events_dropped(3);
                rt_game3d_diag_record_audio_voice_evicted();
                rt_game3d_diag_record_stale_async_load_dropped();
            }
        });
    }
    for (std::thread &worker : workers)
        worker.join();

    constexpr int64_t event_count = (int64_t)worker_count * iterations;
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_broadphase_fallback_count(),
                  event_count,
                  "Concurrent broadphase records are never lost");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_ccd_clamped_frames(),
                  event_count,
                  "Concurrent CCD frame records are never lost");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_ccd_clamped_bodies(),
                  event_count * 2,
                  "Concurrent CCD body additions are exact");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_anim_events_dropped(),
                  event_count * 3,
                  "Concurrent animation-event additions are exact");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_audio_voices_evicted(),
                  event_count,
                  "Concurrent bridged audio records are never lost");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_stale_async_loads_dropped(),
                  event_count,
                  "Concurrent streaming records are never lost");

    rt_game3d_diagnostics_reset();
    rt_game3d_diag_record_ccd_clamp(INT64_MAX);
    rt_game3d_diag_record_ccd_clamp(INT64_MAX);
    rt_game3d_diag_record_anim_events_dropped(INT64_MAX);
    rt_game3d_diag_record_anim_events_dropped(1);
    rt_game3d_diag_record_auto_instanced_draws(INT64_MAX);
    rt_game3d_diag_record_auto_instanced_draws(1);
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_ccd_clamped_bodies(),
                  INT64_MAX,
                  "CCD body counter saturates without signed overflow");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_anim_events_dropped(),
                  INT64_MAX,
                  "Animation counter saturates without signed overflow");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_auto_instanced_draws(),
                  INT64_MAX,
                  "Auto-instancing counter saturates without signed overflow");
}

static void test_physics_broadphase_fallback_counter() {
    rt_game3d_diagnostics_reset();

    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 0.5, 0.0, 0.0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);

    rt_world3d_test_set_broadphase_alloc_failure(1);
    rt_world3d_step(world, 1.0 / 60.0);
    rt_world3d_test_set_broadphase_alloc_failure(0);

    EXPECT_EQ_I64(rt_world3d_get_broadphase_fallback_count(world),
                  1,
                  "Physics3DWorld records broadphase fallback locally");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_broadphase_fallback_count(),
                  1,
                  "Diagnostics records broadphase fallback globally");
}

static void test_physics_ccd_body_counter() {
    rt_game3d_diagnostics_reset();

    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(a, -100.0, 0.0, 0.0);
    rt_body3d_set_position(b, 100.0, 0.0, 0.0);
    rt_body3d_set_velocity(a, 1000.0, 0.0, 0.0);
    rt_body3d_set_velocity(b, -1000.0, 0.0, 0.0);
    rt_body3d_set_use_ccd(a, 1);
    rt_body3d_set_use_ccd(b, 1);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);

    rt_world3d_step(world, 0.1);

    EXPECT_EQ_I64(
        rt_world3d_get_ccd_substep_clamped_count(world), 1, "CCD frame clamp count increments");
    EXPECT_EQ_I64(rt_world3d_get_last_ccd_clamped_body_count(world),
                  2,
                  "CCD last-frame affected body count increments");
    EXPECT_EQ_I64(rt_world3d_get_ccd_substep_clamped_body_count(world),
                  2,
                  "CCD affected body total increments");
    EXPECT_EQ_I64(
        rt_game3d_diagnostics_get_ccd_clamped_frames(), 1, "Diagnostics records CCD frame clamp");
    EXPECT_EQ_I64(
        rt_game3d_diagnostics_get_ccd_clamped_bodies(), 2, "Diagnostics records CCD body clamp");
}

static void test_anim_event_drop_counter() {
    rt_game3d_diagnostics_reset();

    void *skeleton = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skeleton, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skeleton);

    void *controller = rt_anim_controller3d_new(skeleton);
    void *anim = make_single_bone_animation("idle");
    rt_anim_controller3d_add_state(controller, rt_const_cstr("idle"), anim);
    for (int i = 0; i < 65; i++) {
        char name[16];
        std::snprintf(name, sizeof(name), "event%02d", i);
        rt_anim_controller3d_add_event(controller, rt_const_cstr("idle"), 0.0, rt_const_cstr(name));
    }

    rt_anim_controller3d_play(controller, rt_const_cstr("idle"));

    EXPECT_EQ_I64(rt_game3d_diagnostics_get_anim_events_dropped(),
                  1,
                  "Diagnostics records one AnimController3D event queue eviction");
}

static void test_audio_voice_eviction_counter() {
    rt_game3d_diagnostics_reset();

    rt_sound3d_test_set_all_voices_playing(1);
    const int64_t voice_table_pressure_count = 4097; // SOUND3D_MAX_VOICE_CAPACITY + 1.
    for (int64_t voice = 1; voice <= voice_table_pressure_count; voice++)
        rt_sound3d_register_voice_ex(voice, 1.0, 10.0, 100);
    rt_sound3d_test_set_all_voices_playing(0);

    EXPECT_EQ_I64(rt_game3d_diagnostics_get_audio_voices_evicted(),
                  1,
                  "Diagnostics records one Sound3D bounded voice-table eviction");
}

static void test_nav_grid_fallback_counter() {
    rt_game3d_diagnostics_reset();

    void *plane = rt_mesh3d_new_plane(8.0, 8.0);
    rt_navmesh3d_test_set_query_grid_alloc_failure(1);
    void *navmesh = rt_navmesh3d_build(plane, 0.4, 1.8);
    rt_navmesh3d_test_set_query_grid_alloc_failure(0);

    EXPECT_TRUE(navmesh != nullptr, "NavMesh3D still builds when query grid falls back");
    EXPECT_EQ_I64(rt_game3d_diagnostics_get_nav_grid_fallbacks(),
                  1,
                  "Diagnostics records one NavMesh3D query-grid fallback");
}

int main() {
    test_summary_is_stable_and_resettable();
    test_counters_are_atomic_and_saturating();
    test_physics_broadphase_fallback_counter();
    test_physics_ccd_body_counter();
    test_anim_event_drop_counter();
    test_audio_voice_eviction_counter();
    test_nav_grid_fallback_counter();

    printf("Game3D diagnostics tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
