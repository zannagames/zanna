//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d_step_contract.cpp
// Purpose: Pin the World3D dual step contract (ZB-9): the documented manual
//   loop Update() -> StepSimulation(DeltaTime) applies time scale, hit-stop
//   decay, and the frame/elapsed counters exactly once per frame, while
//   standalone StepSimulation keeps its historical unscaled semantics.
// Key invariants:
//   - Combined-loop frames never square TimeScale, never double-decay
//     hit-stop, and never double-count frame/elapsed/unscaled.
//   - A paused/hit-stopped combined frame is a pure re-render (dt 0), not a
//     default-length step.
// Ownership/Lifetime:
//   - Test-created runtime handles rely on production GC conventions.
// Links: src/runtime/graphics/3d/rt_game3d.c (tick / step_simulation),
//        docs/zannalib/graphics/game3d.md (manual loop contract)
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_game3d.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_string.h"
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

#define EXPECT_NEAR(actual, expected, eps, msg)                                                    \
    do {                                                                                           \
        const double got_ = (double)(actual);                                                      \
        const double want_ = (double)(expected);                                                   \
        if (std::fabs(got_ - want_) > (eps)) {                                                     \
            std::printf("FAIL: %s (got %.9f, expected %.9f)\n", msg, got_, want_);                 \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

void *falling_world_new(void **out_world, const char *title) {
    void *world = rt_game3d_world_new(rt_const_cstr(title), 64, 48);
    void *crate = rt_game3d_entity_new();
    void *body = rt_body3d_new_aabb(0.3, 0.3, 0.3, 1.0);
    rt_game3d_entity_attach_body(crate, body);
    rt_game3d_entity_set_position(crate, 0.0, 20.0, 0.0);
    rt_game3d_world_spawn(world, crate);
    *out_world = world;
    return crate;
}

double entity_y(void *entity) {
    void *pos = rt_game3d_entity_world_position(entity);
    double y = rt_vec3_y(pos);
    if (rt_obj_release_check0(pos))
        rt_obj_free(pos);
    return y;
}

/* One documented manual-loop frame: Update() then StepSimulation(DeltaTime). */
bool combined_frame(void *world) {
    if (!rt_game3d_world_tick(world))
        return false;
    rt_game3d_world_step_simulation(world, rt_game3d_world_get_dt(world));
    return true;
}

bool test_combined_counters_single_count() {
    TEST("combined loop: TimeScale applied once, counters advance once");
    void *world = nullptr;
    falling_world_new(&world, "Step Contract Scale");
    rt_game3d_world_set_time_scale(world, 0.5);

    /* Warm one frame so the canvas clock is live. */
    EXPECT_TRUE(combined_frame(world), "warm frame ticks");

    double elapsed_before = rt_game3d_world_get_elapsed(world);
    double unscaled_before = rt_game3d_world_get_unscaled_elapsed(world);
    long long frame_before = rt_game3d_world_get_frame(world);

    EXPECT_TRUE(rt_game3d_world_tick(world), "tick succeeds");
    double dt = rt_game3d_world_get_dt(world);           /* scaled once by tick */
    double real_dt = rt_game3d_world_get_unscaled_dt(world);
    EXPECT_NEAR(dt, real_dt * 0.5, 1e-12, "tick applies TimeScale exactly once");
    rt_game3d_world_step_simulation(world, dt);

    EXPECT_NEAR(rt_game3d_world_get_elapsed(world) - elapsed_before,
                dt,
                1e-12,
                "elapsed advances by the scaled dt exactly once (no re-scale, no re-add)");
    EXPECT_NEAR(rt_game3d_world_get_unscaled_elapsed(world) - unscaled_before,
                real_dt,
                1e-12,
                "unscaled elapsed advances once");
    EXPECT_EQ_INT(rt_game3d_world_get_frame(world) - frame_before,
                  1,
                  "frame advances once per combined frame");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_combined_simulation_advances() {
    TEST("combined loop: physics/animation actually advance (the live-path guarantee)");
    void *world = nullptr;
    void *crate = falling_world_new(&world, "Step Contract Falls");
    double y0 = entity_y(crate);
    for (int i = 0; i < 20; ++i)
        EXPECT_TRUE(combined_frame(world), "combined frame runs");
    EXPECT_TRUE(entity_y(crate) < y0 - 1e-6,
                "a body falls across combined Update+StepSimulation frames");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_combined_pause_is_pure_rerender() {
    TEST("combined loop: paused frame is a pure re-render, not a default step");
    void *world = nullptr;
    void *crate = falling_world_new(&world, "Step Contract Pause");
    for (int i = 0; i < 10; ++i)
        EXPECT_TRUE(combined_frame(world), "warm frames run");
    rt_game3d_world_set_paused(world, 1);
    double y = entity_y(crate);
    double elapsed = rt_game3d_world_get_elapsed(world);
    for (int i = 0; i < 30; ++i)
        EXPECT_TRUE(combined_frame(world), "paused frames still tick");
    EXPECT_NEAR(entity_y(crate), y, 1e-12, "paused combined frames freeze the body");
    EXPECT_NEAR(rt_game3d_world_get_elapsed(world), elapsed, 1e-12,
                "paused combined frames do not advance sim time");
    rt_game3d_world_set_paused(world, 0);
    EXPECT_TRUE(combined_frame(world), "unpaused frame runs");
    EXPECT_TRUE(entity_y(crate) < y, "unpausing resumes the simulation");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_combined_hitstop_decays_in_real_time() {
    TEST("combined loop: hit-stop expires after >= its real-time window (single decay)");
    void *world = nullptr;
    void *crate = falling_world_new(&world, "Step Contract HitStop");
    for (int i = 0; i < 10; ++i)
        EXPECT_TRUE(combined_frame(world), "warm frames run");
    const double window = 0.12;
    rt_game3d_world_hit_stop(world, window);
    double y_frozen = entity_y(crate);
    double real_accum = 0.0;
    bool resumed = false;
    for (int i = 0; i < 100000; ++i) {
        EXPECT_TRUE(combined_frame(world), "hit-stop frame ticks");
        double frame_real = rt_game3d_world_get_unscaled_dt(world);
        if (entity_y(crate) < y_frozen - 1e-9) {
            /* Resumed. Under single decay this needs >= window real seconds
             * (minus one frame of granularity); double decay resumes at
             * window/2 and fails the bound. */
            EXPECT_TRUE(real_accum + frame_real + 1e-9 >= window - frame_real,
                        "hit-stop lasted its full real-time window");
            resumed = true;
            break;
        }
        real_accum += frame_real;
    }
    EXPECT_TRUE(resumed, "hit-stop eventually expires");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_standalone_semantics_unchanged() {
    TEST("standalone StepSimulation keeps historical unscaled semantics");
    void *world = nullptr;
    void *crate = falling_world_new(&world, "Step Contract Standalone");
    rt_game3d_world_set_time_scale(world, 0.5);
    double elapsed0 = rt_game3d_world_get_elapsed(world);
    long long frame0 = rt_game3d_world_get_frame(world);
    for (int i = 0; i < 60; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_NEAR(rt_game3d_world_get_elapsed(world) - elapsed0,
                0.5,
                1e-9,
                "standalone still scales the unscaled step (60 frames @ 1/60 * 0.5)");
    EXPECT_EQ_INT(rt_game3d_world_get_frame(world) - frame0,
                  60,
                  "standalone still advances frame per call");
    EXPECT_TRUE(entity_y(crate) < 20.0, "standalone still simulates");
    rt_game3d_world_destroy(world);
    PASS();
}

} // namespace

int main() {
    std::printf("Game3D step-contract (ZB-9) tests\n");
    bool ok = true;
    ok = test_combined_counters_single_count() && ok;
    ok = test_combined_simulation_advances() && ok;
    ok = test_combined_pause_is_pure_rerender() && ok;
    ok = test_combined_hitstop_decays_in_real_time() && ok;
    ok = test_standalone_semantics_unchanged() && ok;
    std::printf("Step-contract tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok ? 0 : 1;
}
