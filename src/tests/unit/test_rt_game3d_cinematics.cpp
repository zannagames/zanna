//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d_cinematics.cpp
// Purpose: Unit tests for the cinematics layer — the Path3D arclength spline
//   evaluator, RailCamera3D progress/keys/look modes, the PostFX DOF focus
//   drive, and the Timeline3D sequencer (firing math, camera ownership,
//   spline moves, skip semantics, markers, overlay state).
// Key invariants:
//   - Deterministic fixed steps; replays produce identical results.
// Ownership/Lifetime:
//   - Test-created runtime handles rely on production GC conventions.
// Links: src/runtime/graphics/3d/rt_game3d_timeline.c, rt_game3d_railcamera.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_game3d.h"
#include "rt_graphics3d_ids.h"
#include "rt_heap.h"
#include "rt_object.h"
#include "rt_path3d.h"
#include "rt_postfx3d.h"
#include "rt_string.h"
#include "rt_vec3.h"

#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef struct {
    double t;
    double value;
} Game3DRailKeyTestLayout;

typedef struct {
    void *world;
    void *path;
    void *look_entity;
    void *look_point;
    void *look_path;
    double progress;
    double smoothed;
    double speed;
    double position_damping;
    int8_t key_ease;
    Game3DRailKeyTestLayout fov_keys[16];
    int32_t fov_key_count;
    Game3DRailKeyTestLayout roll_keys[16];
    int32_t roll_key_count;
} Game3DRailCameraTestLayout;

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
            std::printf("FAIL: %s (got %.6f, expected %.6f)\n", msg, got_, want_);                 \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

template <typename Fn> static bool expect_trap_contains(Fn &&fn, const char *needle) {
    g_last_trap = nullptr;
    g_expect_trap = true;
    if (setjmp(g_trap_jmp) == 0) {
        fn();
        g_expect_trap = false;
        return false;
    }
    g_expect_trap = false;
    return g_last_trap && std::strstr(g_last_trap, needle) != nullptr;
}

namespace {

void path_add(void *path, double x, double y, double z) {
    void *v = rt_vec3_new(x, y, z);
    rt_path3d_add_point(path, v);
    if (rt_obj_release_check0(v))
        rt_obj_free(v);
}

//=========================================================================
// Plan 10 — spline evaluator + rail camera + DOF
//=========================================================================

bool test_spline_evaluator_constant_speed() {
    TEST("Path3D arclength evaluation is constant-speed on uneven spacing");
    void *path = rt_path3d_new();
    /* Wildly uneven collinear spacing along +X. */
    path_add(path, 0.0, 0.0, 0.0);
    path_add(path, 0.5, 0.0, 0.0);
    path_add(path, 6.0, 0.0, 0.0);
    path_add(path, 10.0, 0.0, 0.0);

    /* Collinear control points reproduce the line. */
    double pos[3];
    double tan[3];
    rt_path3d_eval_spline_raw(path, 0.35, pos, tan);
    EXPECT_NEAR(pos[1], 0.0, 1e-9, "collinear points stay on the line (y)");
    EXPECT_NEAR(pos[2], 0.0, 1e-9, "collinear points stay on the line (z)");
    EXPECT_NEAR(tan[0], 1.0, 1e-6, "tangent points along the line");

    /* Equal t steps cover equal arclength within tolerance. */
    double prev[3];
    rt_path3d_eval_spline_raw(path, 0.0, prev, nullptr);
    double min_step = 1e30;
    double max_step = 0.0;
    for (int i = 1; i <= 20; ++i) {
        double t = (double)i / 20.0;
        rt_path3d_eval_spline_raw(path, t, pos, nullptr);
        double dx = pos[0] - prev[0];
        double dy = pos[1] - prev[1];
        double dz = pos[2] - prev[2];
        double step = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (step < min_step)
            min_step = step;
        if (step > max_step)
            max_step = step;
        std::memcpy(prev, pos, sizeof(prev));
    }
    EXPECT_TRUE(max_step - min_step < 0.06 * max_step,
                "equal t steps cover near-equal arclength (uneven spacing)");

    /* End points hit the control extremes. */
    rt_path3d_eval_spline_raw(path, 0.0, pos, nullptr);
    EXPECT_NEAR(pos[0], 0.0, 1e-9, "t=0 is the first control point");
    rt_path3d_eval_spline_raw(path, 1.0, pos, nullptr);
    EXPECT_NEAR(pos[0], 10.0, 1e-9, "t=1 is the last control point");
    PASS();
}

bool test_spline_evaluator_continuity_and_loop() {
    TEST("Spline evaluation is continuous and loop wrap matches t=0");
    void *path = rt_path3d_new();
    path_add(path, 0.0, 0.0, 0.0);
    path_add(path, 2.0, 1.0, 0.0);
    path_add(path, 4.0, -1.0, 2.0);
    path_add(path, 6.0, 0.5, 1.0);
    double prev[3];
    rt_path3d_eval_spline_raw(path, 0.0, prev, nullptr);
    for (int i = 1; i <= 200; ++i) {
        double pos[3];
        rt_path3d_eval_spline_raw(path, (double)i / 200.0, pos, nullptr);
        double dx = pos[0] - prev[0];
        double dy = pos[1] - prev[1];
        double dz = pos[2] - prev[2];
        double step = std::sqrt(dx * dx + dy * dy + dz * dz);
        EXPECT_TRUE(step < 0.2, "no discontinuity between adjacent samples");
        std::memcpy(prev, pos, sizeof(prev));
    }
    rt_path3d_set_looping(path, 1);
    double at_zero[3];
    double at_one[3];
    rt_path3d_eval_spline_raw(path, 0.0, at_zero, nullptr);
    rt_path3d_eval_spline_raw(path, 1.0, at_one, nullptr);
    EXPECT_NEAR(at_zero[0], at_one[0], 1e-6, "loop wrap continuous at t=0/1");
    PASS();
}

bool test_rail_camera_progress_and_keys() {
    TEST("RailCamera3D rides the spline with FOV keys and tangent facing");
    void *world = rt_game3d_world_new(rt_const_cstr("Rail Cam"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *path = rt_path3d_new();
    path_add(path, 0.0, 2.0, 0.0);
    path_add(path, 4.0, 2.0, 0.0);
    path_add(path, 8.0, 2.0, 0.0);
    path_add(path, 12.0, 2.0, 0.0);

    void *rail = rt_game3d_rail_camera_new(world, path);
    rt_game3d_rail_camera_add_fov_key(rail, 0.0, 70.0);
    rt_game3d_rail_camera_add_fov_key(rail, 1.0, 40.0);
    rt_game3d_world_set_camera_controller(world, rail);
    void *camera = rt_game3d_world_get_camera(world);

    rt_game3d_rail_camera_set_progress(rail, 0.5);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *cam_pos = rt_camera3d_get_position(camera);
    EXPECT_NEAR(rt_vec3_x(cam_pos), 6.0, 0.1, "camera at the spline midpoint");
    EXPECT_NEAR(rt_vec3_y(cam_pos), 2.0, 0.05, "camera keeps the path height");
    if (rt_obj_release_check0(cam_pos))
        rt_obj_free(cam_pos);
    EXPECT_NEAR(rt_camera3d_get_fov(camera), 55.0, 1.0, "FOV keys interpolate at t=0.5");

    /* Auto-advance covers the remaining arclength at the configured speed. */
    rt_game3d_rail_camera_set_speed(rail, 6.0); /* 12 units / 6 per sec = 2 s total */
    for (int i = 0; i < 60; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_NEAR(rt_game3d_rail_camera_get_progress(rail),
                1.0,
                0.01,
                "auto-advance reaches the end of the rail");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_rail_camera_repairs_corrupt_state_transactionally() {
    TEST("RailCamera3D repairs corrupt state, keys, and undersized handles");
    void *world = rt_game3d_world_new(rt_const_cstr("Rail Repair"), 64, 48);
    void *path = rt_path3d_new();
    path_add(path, 0.0, 1.0, 0.0);
    path_add(path, 10.0, 1.0, 0.0);
    void *rail = rt_game3d_rail_camera_new(world, path);
    auto *view = static_cast<Game3DRailCameraTestLayout *>(rail);

    view->progress = NAN;
    view->smoothed = INFINITY;
    view->speed = -INFINITY;
    view->position_damping = INFINITY;
    view->key_ease = 9;
    view->fov_key_count = INT32_MAX;
    view->fov_keys[0] = {0.8, 200.0};
    view->fov_keys[1] = {0.2, NAN};
    view->fov_keys[2] = {0.2, 40.0};
    view->fov_keys[3] = {-INFINITY, -5.0};
    view->roll_key_count = -100;

    EXPECT_NEAR(rt_game3d_rail_camera_get_progress(rail),
                0.0,
                0.000001,
                "non-finite progress repairs to the rail start");
    EXPECT_NEAR(view->smoothed, 0.0, 0.000001, "smoothed progress repair persists");
    EXPECT_NEAR(view->speed, 0.0, 0.000001, "negative rail speed repair persists");
    EXPECT_NEAR(view->position_damping, 0.0, 0.000001, "non-finite rail damping repair persists");
    EXPECT_EQ_INT(view->key_ease, 1, "rail key easing flag is canonicalized");
    EXPECT_EQ_INT(view->fov_key_count, 3, "rail key repair coalesces duplicate times");
    EXPECT_EQ_INT(view->roll_key_count, 0, "negative rail key count repairs to empty");
    for (int32_t i = 0; i < view->fov_key_count; ++i) {
        EXPECT_TRUE(std::isfinite(view->fov_keys[i].t) && view->fov_keys[i].t >= 0.0 &&
                        view->fov_keys[i].t <= 1.0,
                    "repaired FOV key times are finite and bounded");
        EXPECT_TRUE(std::isfinite(view->fov_keys[i].value) && view->fov_keys[i].value >= 1.0 &&
                        view->fov_keys[i].value <= 179.0,
                    "repaired FOV values are finite and bounded");
        if (i > 0)
            EXPECT_TRUE(view->fov_keys[i - 1].t < view->fov_keys[i].t,
                        "repaired FOV keys are strictly sorted");
    }
    EXPECT_NEAR(view->fov_keys[1].value,
                40.0,
                0.000001,
                "duplicate rail keys deterministically keep the last value");
    EXPECT_NEAR(view->fov_keys[3].t, 0.0, 0.000001, "unused key times are cleared");
    EXPECT_NEAR(view->fov_keys[3].value, 0.0, 0.000001, "unused key values are cleared");

    view->fov_key_count = 0;
    rt_game3d_rail_camera_add_fov_key(rail, 0.5, 50.0);
    rt_game3d_rail_camera_add_fov_key(rail, 0.5, 80.0);
    EXPECT_EQ_INT(view->fov_key_count, 1, "adding a duplicate key replaces instead of growing");
    EXPECT_NEAR(view->fov_keys[0].value,
                80.0,
                0.000001,
                "duplicate-key replacement keeps the newest value");

    rt_game3d_rail_camera_set_progress(rail, 0.25);
    rt_game3d_rail_camera_set_speed(rail, 10.0);
    rt_game3d_rail_camera_update(rail, world, 0.0);
    EXPECT_NEAR(rt_game3d_rail_camera_get_progress(rail),
                0.25,
                0.000001,
                "zero-delta rail update preserves progress");

    void *wrong_path = rt_vec3_new(1.0, 2.0, 3.0);
    size_t wrong_path_refcount = rt_heap_hdr(wrong_path)->refcnt;
    void *saved_path = view->path;
    view->path = wrong_path;
    (void)rt_game3d_rail_camera_get_speed(rail);
    EXPECT_TRUE(view->path == nullptr, "wrong-class private rail path is quarantined");
    EXPECT_TRUE(rt_heap_hdr(wrong_path)->refcnt == wrong_path_refcount,
                "quarantining an unowned wrong-class path does not release it");
    view->path = saved_path;

    void *undersized = rt_obj_new_i64(RT_G3D_GAME3D_RAILCAMERA_CLASS_ID, 1);
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_rail_camera_get_speed(undersized); },
                                     "invalid rail"),
                "undersized RailCamera3D class spoof is rejected");
    if (undersized && rt_obj_release_check0(undersized))
        rt_obj_free(undersized);
    if (wrong_path && rt_obj_release_check0(wrong_path))
        rt_obj_free(wrong_path);
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_dof_focus_drive() {
    TEST("PostFX3D.SetDofFocus mutates a live DOF effect and reports absence");
    void *chain = rt_postfx3d_new();
    EXPECT_TRUE(rt_postfx3d_set_dof_focus(chain, 5.0) == 0, "chain without DOF returns false");
    rt_postfx3d_add_dof(chain, 10.0, 2.0, 8.0);
    EXPECT_TRUE(rt_postfx3d_set_dof_focus(chain, 3.5) != 0, "chain with DOF returns true");
    PASS();
}

//=========================================================================
// Plan 09 — Timeline3D
//=========================================================================

bool test_timeline_firing_math() {
    TEST("Timeline3D point tracks fire exactly once regardless of step size");
    void *world = rt_game3d_world_new(rt_const_cstr("TL Fire"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *tl = rt_game3d_timeline_new(world);
    rt_game3d_timeline_add_marker(tl, 1.0, 42);
    rt_game3d_timeline_add_marker(tl, 0.0, 7);
    rt_game3d_timeline_add_marker(tl, 2.5, 99);
    for (int i = 0; i < 24; ++i)
        rt_game3d_timeline_add_marker(tl, 0.05 * i, 1000 + i);
    rt_game3d_world_play_timeline(world, tl);

    int fired_42 = 0;
    int fired_7 = 0;
    int fired_99 = 0;
    int fired_fillers = 0;
    /* Coarse 0.4 s steps: markers must still fire exactly once. */
    for (int i = 0; i < 10; ++i) {
        rt_game3d_world_step_simulation(world, 0.4);
        int64_t n = rt_game3d_timeline_events_fired_count(tl);
        for (int64_t e = 0; e < n; ++e) {
            int64_t id = rt_game3d_timeline_event_fired_id(tl, e);
            if (id == 42)
                ++fired_42;
            if (id == 7)
                ++fired_7;
            if (id == 99)
                ++fired_99;
            if (id >= 1000 && id < 1024)
                ++fired_fillers;
        }
    }
    EXPECT_EQ_INT(fired_7, 1, "t=0 marker fires exactly once");
    EXPECT_EQ_INT(fired_42, 1, "t=1 marker fires exactly once");
    EXPECT_EQ_INT(fired_99, 1, "t=2.5 marker fires exactly once");
    EXPECT_EQ_INT(fired_fillers, 24, "grown track storage preserves every marker");
    EXPECT_TRUE(rt_game3d_timeline_get_finished(tl) != 0, "timeline finished");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_timeline_camera_ownership() {
    TEST("Timeline3D suspends the installed controller and restores it after stop");
    void *world = rt_game3d_world_new(rt_const_cstr("TL Camera"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *camera = rt_game3d_world_get_camera(world);

    /* Installed third-person controller (would move the camera every step). */
    void *player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(player, 0.0, 0.0, 0.0);
    rt_game3d_world_spawn(world, player);
    void *controller = rt_game3d_thirdperson_controller_new(world, player);
    rt_game3d_world_set_camera_controller(world, controller);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);

    void *tl = rt_game3d_timeline_new(world);
    void *cut_pos = rt_vec3_new(50.0, 20.0, 50.0);
    void *cut_look = rt_vec3_new(0.0, 0.0, 0.0);
    rt_game3d_timeline_add_camera_cut(tl, 0.0, cut_pos, cut_look, 45.0);
    rt_game3d_timeline_add_marker(tl, 1.0, 1);
    rt_game3d_world_play_timeline(world, tl);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);

    void *cam = rt_camera3d_get_position(camera);
    EXPECT_NEAR(rt_vec3_x(cam), 50.0, 0.01, "timeline cut owns the camera");
    EXPECT_NEAR(rt_camera3d_get_fov(camera), 45.0, 0.01, "cut applies its FOV");
    if (rt_obj_release_check0(cam))
        rt_obj_free(cam);

    /* Stop: controller resumes next step (boom pulls the camera back). */
    rt_game3d_world_stop_timeline(world);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *cam2 = rt_camera3d_get_position(camera);
    EXPECT_TRUE(std::fabs(rt_vec3_x(cam2) - 50.0) > 1.0,
                "controller regains the camera after stopTimeline");
    if (rt_obj_release_check0(cam2))
        rt_obj_free(cam2);
    if (rt_obj_release_check0(cut_look))
        rt_obj_free(cut_look);
    if (rt_obj_release_check0(cut_pos))
        rt_obj_free(cut_pos);
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_timeline_spline_move_and_overlay_state() {
    TEST("Timeline3D camera move follows the path; overlay state tracks the playhead");
    void *world = rt_game3d_world_new(rt_const_cstr("TL Move"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *camera = rt_game3d_world_get_camera(world);
    void *path = rt_path3d_new();
    path_add(path, 0.0, 5.0, 10.0);
    path_add(path, 5.0, 5.0, 10.0);
    path_add(path, 10.0, 5.0, 10.0);
    path_add(path, 15.0, 5.0, 10.0);

    void *tl = rt_game3d_timeline_new(world);
    void *look = rt_vec3_new(0.0, 0.0, 0.0);
    rt_game3d_timeline_add_camera_move(tl, 0.0, 2.0, path, look, 0);
    rt_game3d_timeline_add_subtitle(tl, 0.5, 1.5, rt_const_cstr("Hello there"));
    rt_game3d_timeline_add_letterbox(tl, 0.0, 2.0, 0.12);
    rt_game3d_timeline_add_fade(tl, 1.5, 2.0, 0.0, 1.0);
    rt_game3d_world_play_timeline(world, tl);

    /* Advance to t = 1.0 (path midpoint). */
    for (int i = 0; i < 60; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *cam = rt_camera3d_get_position(camera);
    EXPECT_NEAR(rt_vec3_x(cam), 7.5, 0.2, "camera move tracks the path midpoint");
    EXPECT_NEAR(rt_vec3_y(cam), 5.0, 0.05, "camera move keeps the path height");
    if (rt_obj_release_check0(cam))
        rt_obj_free(cam);
    rt_string subtitle = rt_game3d_timeline_active_subtitle(tl);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(subtitle), "Hello there") == 0,
                "subtitle active inside its window");

    if (rt_obj_release_check0(look))
        rt_obj_free(look);
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_timeline_skip_semantics() {
    TEST("Timeline3D.skip applies end state, fires markers, sets finished");
    void *world = rt_game3d_world_new(rt_const_cstr("TL Skip"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *camera = rt_game3d_world_get_camera(world);
    void *tl = rt_game3d_timeline_new(world);
    void *p1 = rt_vec3_new(1.0, 2.0, 3.0);
    void *p2 = rt_vec3_new(9.0, 9.0, 9.0);
    void *look = rt_vec3_new(0.0, 0.0, 0.0);
    rt_game3d_timeline_add_camera_cut(tl, 0.0, p1, look, 60.0);
    rt_game3d_timeline_add_camera_cut(tl, 3.0, p2, look, 30.0);
    rt_game3d_timeline_add_marker(tl, 2.0, 5);
    rt_game3d_world_play_timeline(world, tl);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);

    rt_game3d_timeline_skip(tl);
    EXPECT_TRUE(rt_game3d_timeline_get_finished(tl) != 0, "skip finishes the timeline");
    EXPECT_TRUE(rt_game3d_timeline_just_finished(tl) != 0, "skip latches justFinished");
    void *cam = rt_camera3d_get_position(camera);
    EXPECT_NEAR(rt_vec3_x(cam), 9.0, 0.01, "skip applies the end camera cut");
    if (rt_obj_release_check0(cam))
        rt_obj_free(cam);

    /* Non-skippable timelines ignore skip(). */
    void *tl2 = rt_game3d_timeline_new(world);
    rt_game3d_timeline_add_marker(tl2, 5.0, 1);
    rt_game3d_timeline_set_skippable(tl2, 0);
    rt_game3d_world_play_timeline(world, tl2);
    rt_game3d_timeline_skip(tl2);
    EXPECT_TRUE(rt_game3d_timeline_get_finished(tl2) == 0, "non-skippable ignores skip");

    if (rt_obj_release_check0(look))
        rt_obj_free(look);
    if (rt_obj_release_check0(p2))
        rt_obj_free(p2);
    if (rt_obj_release_check0(p1))
        rt_obj_free(p1);
    rt_game3d_world_destroy(world);
    PASS();
}

} // namespace

int main() {
    std::printf("Game3D cinematics tests\n");
    bool ok = true;
    ok = test_spline_evaluator_constant_speed() && ok;
    ok = test_spline_evaluator_continuity_and_loop() && ok;
    ok = test_rail_camera_progress_and_keys() && ok;
    ok = test_rail_camera_repairs_corrupt_state_transactionally() && ok;
    ok = test_dof_focus_drive() && ok;
    ok = test_timeline_firing_math() && ok;
    ok = test_timeline_camera_ownership() && ok;
    ok = test_timeline_spline_move_and_overlay_state() && ok;
    ok = test_timeline_skip_semantics() && ok;
    std::printf("\nCinematics tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
