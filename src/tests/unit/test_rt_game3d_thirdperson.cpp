//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d_thirdperson.cpp
// Purpose: Unit tests for the third-person upgrade runtime surface —
//   ThirdPersonController spring-arm/boom collision/aim/occluder fade,
//   TargetLock3D acquisition/cycling/auto-release, Character3D crouch/
//   dynamic-push/moving-platform/slide upgrades, and the Physics3DWorld
//   traversal probes (ProbeLedge/ProbeVault/ProbeClearance).
// Key invariants:
//   - Deterministic: every scenario uses fixed steps and synthetic input only.
//   - Replays of identical worlds produce byte-identical trajectories.
// Ownership/Lifetime:
//   - Test-created runtime handles rely on production GC conventions.
// Links: misc/plans/thirdpersonupgrade/01..04, rt_game3d.h, rt_physics3d.h
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_collider3d.h"
#include "rt_game3d.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_quat.h"
#include "rt_scene3d.h"
#include "rt_string.h"
#include "rt_vec3.h"

#include <cmath>
#include <csetjmp>
#include <cstdint>
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

#define EXPECT_NEAR(actual, expected, eps, msg)                                                    \
    do {                                                                                           \
        const double got_ = (double)(actual);                                                      \
        const double want_ = (double)(expected);                                                   \
        if (std::fabs(got_ - want_) > (eps)) {                                                     \
            std::printf("FAIL: %s (got %.6f, expected %.6f)\n", msg, got_, want_);                 \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

/// @brief Spawn a static AABB wall entity; returns the entity handle.
void *spawn_static_box(
    void *world, double cx, double cy, double cz, double hx, double hy, double hz) {
    void *entity = rt_game3d_entity_new();
    void *body = rt_body3d_new_aabb(hx, hy, hz, 0.0);
    rt_game3d_entity_attach_body(entity, body);
    rt_game3d_entity_set_position(entity, cx, cy, cz);
    rt_game3d_world_spawn(world, entity);
    return entity;
}

/// @brief Spawn a dynamic AABB crate entity; returns the entity handle.
///   A non-zero @p layer overrides the body's collision layer (lock-on tests).
void *spawn_dynamic_box(
    void *world, double cx, double cy, double cz, double half, double mass, int64_t layer = 0) {
    void *entity = rt_game3d_entity_new();
    void *body = rt_body3d_new_aabb(half, half, half, mass);
    if (layer != 0)
        rt_body3d_set_collision_layer(body, layer);
    rt_game3d_entity_attach_body(entity, body);
    rt_game3d_entity_set_position(entity, cx, cy, cz);
    rt_game3d_world_spawn(world, entity);
    return entity;
}

double camera_distance_to(void *camera, double px, double py, double pz) {
    void *pos = rt_camera3d_get_position(camera);
    double dx = rt_vec3_x(pos) - px;
    double dy = rt_vec3_y(pos) - py;
    double dz = rt_vec3_z(pos) - pz;
    if (rt_obj_release_check0(pos))
        rt_obj_free(pos);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

//=========================================================================
// Plan 01 — ThirdPersonController
//=========================================================================

bool test_thirdperson_defaults_and_orbit() {
    TEST("ThirdPersonController defaults and free orbit keep boom length");
    void *world = rt_game3d_world_new(rt_const_cstr("TP Orbit"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(player, 0.0, 0.0, 0.0);
    rt_game3d_world_spawn(world, player);
    void *controller = rt_game3d_thirdperson_controller_new(world, player);
    EXPECT_TRUE(controller != nullptr, "ThirdPersonController.New returns an object");
    EXPECT_NEAR(rt_game3d_thirdperson_controller_get_distance(controller),
                4.0,
                1e-9,
                "default distance is 4");
    EXPECT_NEAR(rt_game3d_thirdperson_controller_get_min_distance(controller),
                0.75,
                1e-9,
                "default min distance is 0.75");
    EXPECT_NEAR(rt_game3d_thirdperson_controller_get_max_distance(controller),
                8.0,
                1e-9,
                "default max distance is 8");
    EXPECT_NEAR(rt_game3d_thirdperson_controller_get_pivot_height(controller),
                1.5,
                1e-9,
                "default pivot height is 1.5");
    rt_game3d_thirdperson_controller_set_pitch(controller, -1000.0);
    EXPECT_NEAR(rt_game3d_thirdperson_controller_get_pitch(controller),
                -60.0,
                1e-9,
                "pitch clamps to the default floor");
    rt_game3d_thirdperson_controller_set_pitch(controller, 1000.0);
    EXPECT_NEAR(rt_game3d_thirdperson_controller_get_pitch(controller),
                75.0,
                1e-9,
                "pitch clamps to the default ceiling");
    rt_game3d_thirdperson_controller_set_pitch(controller, 0.0);
    rt_game3d_world_set_camera_controller(world, controller);

    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);

    /* Pivot = entity origin + pivot height + yaw-rotated shoulder (0.35 right). */
    void *camera = rt_game3d_world_get_camera(world);
    double boom = camera_distance_to(camera, 0.35, 1.5, 0.0);
    EXPECT_NEAR(boom, 4.0, 0.01, "free boom settles at the configured distance");
    void *cam_pos = rt_camera3d_get_position(camera);
    EXPECT_TRUE(rt_vec3_z(cam_pos) > 3.5, "yaw 0 places the camera on +Z behind the pivot");
    if (rt_obj_release_check0(cam_pos))
        rt_obj_free(cam_pos);
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_thirdperson_boom_pull_in_and_release() {
    TEST("ThirdPersonController boom sweeps in against walls, never clips");
    void *world = rt_game3d_world_new(rt_const_cstr("TP Boom"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(player, 0.0, 0.0, 0.0);
    rt_game3d_world_spawn(world, player);
    /* Wall crossing the boom path: spans z in [2.25, 2.75]. */
    spawn_static_box(world, 0.35, 1.5, 2.5, 3.0, 3.0, 0.25);
    void *controller = rt_game3d_thirdperson_controller_new(world, player);
    rt_game3d_world_set_camera_controller(world, controller);

    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *camera = rt_game3d_world_get_camera(world);
    void *cam_pos = rt_camera3d_get_position(camera);
    double cam_z = rt_vec3_z(cam_pos);
    if (rt_obj_release_check0(cam_pos))
        rt_obj_free(cam_pos);
    EXPECT_TRUE(cam_z < 2.25, "camera never sits behind the wall plane");
    EXPECT_TRUE(cam_z > 1.0, "camera pulled in but did not collapse to the pivot");

    rt_game3d_world_destroy(world);
    PASS();
}

bool test_thirdperson_penetrating_start_snaps_to_min() {
    TEST("ThirdPersonController boom snaps to MinDistance when starting inside geometry");
    void *world = rt_game3d_world_new(rt_const_cstr("TP Penetrating"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(player, 0.0, 0.0, 0.0);
    rt_game3d_world_spawn(world, player);
    /* Box fully engulfing pivot and boom path. */
    spawn_static_box(world, 0.0, 1.5, 2.0, 6.0, 4.0, 6.0);
    void *controller = rt_game3d_thirdperson_controller_new(world, player);
    rt_game3d_world_set_camera_controller(world, controller);
    for (int i = 0; i < 10; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *camera = rt_game3d_world_get_camera(world);
    double boom = camera_distance_to(camera, 0.35, 1.5, 0.0);
    EXPECT_NEAR(boom, 0.75, 0.05, "penetrating start clamps the boom to MinDistance");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_thirdperson_character_drive_yaw_basis() {
    TEST("ThirdPersonController drives the character camera-relatively (yaw 90 -> -X)");
    double first_run_x = 0.0;
    double first_run_z = 0.0;
    for (int run = 0; run < 2; ++run) {
        void *world = rt_game3d_world_new(rt_const_cstr("TP Drive"), 64, 48);
        void *canvas = rt_game3d_world_get_canvas(world);
        /* Ground plane: top face at y = 0. */
        spawn_static_box(world, 0.0, -0.5, 0.0, 50.0, 0.5, 50.0);
        void *player = rt_game3d_entity_new();
        rt_game3d_entity_set_position(player, 0.0, 0.9, 0.0);
        rt_game3d_world_spawn(world, player);
        void *char_controller = rt_game3d_character_controller_new(world, player, 0.3, 1.8, 70.0);
        void *controller = rt_game3d_thirdperson_controller_new(world, player);
        rt_game3d_thirdperson_controller_set_character(controller, char_controller);
        rt_game3d_thirdperson_controller_set_yaw(controller, 90.0);
        rt_game3d_world_set_camera_controller(world, controller);

        rt_canvas3d_push_synthetic_key(canvas, rt_game3d_key_w(), 1);
        rt_game3d_world_run_frames_only(world, 30, 1.0 / 60.0);
        rt_canvas3d_clear_synthetic_input(canvas);

        void *pos = rt_game3d_entity_world_position(player);
        double px = rt_vec3_x(pos);
        double pz = rt_vec3_z(pos);
        if (rt_obj_release_check0(pos))
            rt_obj_free(pos);
        if (run == 0) {
            first_run_x = px;
            first_run_z = pz;
            EXPECT_TRUE(px < -0.5, "forward input under yaw 90 moves the player along -X");
            EXPECT_TRUE(std::fabs(pz) < 0.05, "yaw-90 drive stays on the X axis");
            EXPECT_TRUE(rt_game3d_character_controller_grounded(char_controller),
                        "player stays grounded on the plane");
        } else {
            EXPECT_TRUE(std::memcmp(&first_run_x, &px, sizeof(double)) == 0 &&
                            std::memcmp(&first_run_z, &pz, sizeof(double)) == 0,
                        "deterministic replay is byte-identical");
        }
        rt_game3d_world_destroy(world);
    }
    PASS();
}

bool test_thirdperson_aim_blend() {
    TEST("ThirdPersonController aim mode converges distance and FOV, then restores");
    void *world = rt_game3d_world_new(rt_const_cstr("TP Aim"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(player, 0.0, 0.0, 0.0);
    rt_game3d_world_spawn(world, player);
    void *controller = rt_game3d_thirdperson_controller_new(world, player);
    rt_game3d_world_set_camera_controller(world, controller);
    void *camera = rt_game3d_world_get_camera(world);
    double base_fov = rt_camera3d_get_fov(camera);

    rt_game3d_thirdperson_controller_set_aiming(controller, 1);
    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    double aim_boom = camera_distance_to(camera, 0.35, 1.5, 0.0);
    EXPECT_NEAR(aim_boom, 1.6, 0.05, "aim mode pulls the boom to AimDistance within 0.5 s");
    EXPECT_NEAR(rt_camera3d_get_fov(camera), 45.0, 0.5, "aim mode converges the camera FOV");

    rt_game3d_thirdperson_controller_set_aiming(controller, 0);
    for (int i = 0; i < 60; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    double free_boom = camera_distance_to(camera, 0.35, 1.5, 0.0);
    EXPECT_NEAR(free_boom, 4.0, 0.1, "leaving aim mode releases the boom");
    EXPECT_NEAR(rt_camera3d_get_fov(camera), base_fov, 1e-6, "leaving aim mode restores the FOV");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_thirdperson_occluder_fade_and_restore() {
    TEST("ThirdPersonController fades occluding meshes and restores their materials");
    void *world = rt_game3d_world_new(rt_const_cstr("TP Fade"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(player, 0.0, 0.0, 0.0);
    rt_game3d_world_spawn(world, player);

    /* Occluder: rendered wall with a body crossing the pivot->camera ray.
     * It lives on a layer excluded from the boom CollisionMask (a fadeable
     * prop), so the camera stays put and the wall occludes the view. */
    void *wall = rt_game3d_entity_new();
    void *wall_body = rt_body3d_new_aabb(3.0, 3.0, 0.25, 0.0);
    rt_game3d_entity_attach_body(wall, wall_body);
    void *mesh = rt_mesh3d_new_box(6.0, 6.0, 0.5);
    void *material = rt_material3d_new_color(0.8, 0.1, 0.1);
    rt_game3d_entity_set_mesh(wall, mesh);
    rt_game3d_entity_set_material(wall, material);
    rt_game3d_entity_set_position(wall, 0.35, 1.5, 2.5);
    rt_game3d_world_spawn(world, wall);
    void *node = rt_game3d_entity_get_node(wall);
    void *original_material = rt_scene_node3d_get_material(node);
    EXPECT_TRUE(original_material != nullptr, "wall node has a bound material");

    void *controller = rt_game3d_thirdperson_controller_new(world, player);
    rt_game3d_thirdperson_controller_set_occlusion_fade(controller, 1);
    rt_game3d_thirdperson_controller_set_collision_mask(controller, INT64_C(1) << 0);
    rt_game3d_world_set_camera_controller(world, controller);

    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *faded = rt_scene_node3d_get_material(node);
    EXPECT_TRUE(faded != nullptr && faded != original_material,
                "occluding wall swaps to a faded material instance");
    EXPECT_TRUE(rt_material3d_get_alpha(faded) < 0.6,
                "faded clone alpha animates toward the fade target");

    void *replacement_target = rt_game3d_entity_new();
    rt_game3d_entity_set_position(replacement_target, 0.0, 0.0, 0.0);
    rt_game3d_world_spawn(world, replacement_target);
    rt_game3d_thirdperson_controller_set_target(controller, replacement_target);
    EXPECT_TRUE(rt_scene_node3d_get_material(node) == original_material,
                "changing the camera target immediately restores faded materials");
    rt_game3d_thirdperson_controller_set_target(controller, player);
    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_scene_node3d_get_material(node) != original_material,
                "returning to the occluded target can establish a fresh fade");

    /* Move the wall's collision body clear of the boom ray (the ray tests the
     * physics world, so the body is the source of truth) and let the fade
     * release. */
    rt_body3d_set_position(wall_body, 30.0, 1.5, 2.5);
    for (int i = 0; i < 90; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_scene_node3d_get_material(node) == original_material,
                "cleared occluder restores the original material handle");
    rt_game3d_world_destroy(world);
    PASS();
}

//=========================================================================
// Plan 02 — TargetLock3D
//=========================================================================

struct LockFixture {
    void *world = nullptr;
    void *player = nullptr;
    void *lock = nullptr;
    void *center = nullptr;
    void *left = nullptr;
    void *right = nullptr;
};

/// @brief Build a lock scenario: player at origin, camera looking down -Z,
///   three targetable crates ahead.
LockFixture lock_fixture_new(const char *title) {
    LockFixture fx;
    fx.world = rt_game3d_world_new(rt_const_cstr(title), 64, 48);
    rt_game3d_world_set_gravity(fx.world, 0.0, 0.0, 0.0);
    fx.player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(fx.player, 0.0, 1.0, 0.0);
    rt_game3d_world_spawn(fx.world, fx.player);
    void *camera = rt_game3d_world_get_camera(fx.world);
    void *eye = rt_vec3_new(0.0, 1.0, 6.0);
    void *look = rt_vec3_new(0.0, 1.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    rt_camera3d_look_at(camera, eye, look, up);
    if (rt_obj_release_check0(up))
        rt_obj_free(up);
    if (rt_obj_release_check0(look))
        rt_obj_free(look);
    if (rt_obj_release_check0(eye))
        rt_obj_free(eye);
    /* Targetables live on a dedicated layer bit; scenery keeps the default
     * layer so the candidate mask filters it out (walls still block LoS at
     * full mask). Layer policy is entity-owned: attachBody copies the entity
     * layer onto the body, so the entity API is the right place to set it. */
    fx.center = spawn_dynamic_box(fx.world, 0.0, 1.0, -5.0, 0.4, 1.0);
    fx.left = spawn_dynamic_box(fx.world, -3.0, 1.0, -4.0, 0.4, 1.0);
    fx.right = spawn_dynamic_box(fx.world, 3.0, 1.0, -4.0, 0.4, 1.0);
    rt_game3d_entity_set_layer(fx.center, INT64_C(1) << 3);
    rt_game3d_entity_set_layer(fx.left, INT64_C(1) << 3);
    rt_game3d_entity_set_layer(fx.right, INT64_C(1) << 3);
    fx.lock = rt_game3d_targetlock_new(fx.world, fx.player);
    rt_game3d_targetlock_set_candidate_mask(fx.lock, INT64_C(1) << 3);
    return fx;
}

bool test_targetlock_acquire_prefers_near_center() {
    TEST("TargetLock3D acquires the near-center candidate");
    LockFixture fx = lock_fixture_new("Lock Acquire");
    /* Far center target competes on angle but loses on distance. */
    spawn_dynamic_box(fx.world, 0.0, 1.0, -15.0, 0.4, 1.0);
    EXPECT_TRUE(rt_game3d_targetlock_acquire(fx.lock) != 0, "Acquire finds a target");
    EXPECT_TRUE(rt_game3d_targetlock_get_target(fx.lock) == fx.center,
                "near-center target wins the angle-weighted score");
    EXPECT_TRUE(rt_game3d_targetlock_just_acquired(fx.lock) != 0,
                "JustAcquired fires on acquisition");
    rt_game3d_targetlock_update(fx.lock, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_targetlock_just_acquired(fx.lock) == 0,
                "JustAcquired clears on the next update");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_targetlock_los_gating() {
    TEST("TargetLock3D line-of-sight gating skips walled candidates");
    LockFixture fx = lock_fixture_new("Lock LoS");
    /* Wall between the player and the center target. */
    spawn_static_box(fx.world, 0.0, 1.0, -2.5, 1.2, 2.0, 0.2);
    EXPECT_TRUE(rt_game3d_targetlock_acquire(fx.lock) != 0, "Acquire still finds a target");
    void *chosen = rt_game3d_targetlock_get_target(fx.lock);
    EXPECT_TRUE(chosen == fx.left || chosen == fx.right,
                "walled center candidate is skipped under RequireLineOfSight");
    rt_game3d_targetlock_set_require_los(fx.lock, 0);
    rt_game3d_targetlock_clear(fx.lock);
    EXPECT_TRUE(rt_game3d_targetlock_acquire(fx.lock) != 0, "Acquire works without LoS");
    EXPECT_TRUE(rt_game3d_targetlock_get_target(fx.lock) == fx.center,
                "disabling LoS re-admits the walled candidate");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_targetlock_cycle_direction() {
    TEST("TargetLock3D Cycle picks the nearest candidate in camera-yaw order");
    LockFixture fx = lock_fixture_new("Lock Cycle");
    EXPECT_TRUE(rt_game3d_targetlock_acquire(fx.lock) != 0, "Acquire locks the center");
    EXPECT_TRUE(rt_game3d_targetlock_get_target(fx.lock) == fx.center, "center first");
    EXPECT_TRUE(rt_game3d_targetlock_cycle(fx.lock, 1) != 0, "Cycle(+1) changes target");
    EXPECT_TRUE(rt_game3d_targetlock_get_target(fx.lock) == fx.right,
                "Cycle(+1) selects the right-hand target");
    EXPECT_TRUE(rt_game3d_targetlock_cycle(fx.lock, -1) != 0, "Cycle(-1) changes target");
    EXPECT_TRUE(rt_game3d_targetlock_get_target(fx.lock) == fx.center,
                "Cycle(-1) returns to the center target");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_targetlock_break_distance_release() {
    TEST("TargetLock3D auto-releases past BreakDistance with a one-shot JustLost");
    LockFixture fx = lock_fixture_new("Lock Break");
    EXPECT_TRUE(rt_game3d_targetlock_acquire(fx.lock) != 0, "Acquire locks the center");
    rt_game3d_entity_set_position(fx.center, 0.0, 1.0, -100.0);
    rt_game3d_targetlock_update(fx.lock, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_targetlock_get_target(fx.lock) == nullptr,
                "target beyond BreakDistance is released");
    EXPECT_TRUE(rt_game3d_targetlock_just_lost(fx.lock) != 0, "JustLost fires on release");
    rt_game3d_targetlock_update(fx.lock, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_targetlock_just_lost(fx.lock) == 0, "JustLost clears next update");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_targetlock_framing_converges() {
    TEST("ThirdPersonController lock framing converges yaw onto the target bearing");
    LockFixture fx = lock_fixture_new("Lock Framing");
    void *controller = rt_game3d_thirdperson_controller_new(fx.world, fx.player);
    rt_game3d_thirdperson_controller_set_lock_target(controller, fx.lock);
    rt_game3d_world_set_camera_controller(fx.world, controller);
    /* Lock the right-hand target: bearing from player (0,1,0) to (3,1,-4). */
    rt_game3d_targetlock_set_require_los(fx.lock, 0);
    EXPECT_TRUE(rt_game3d_targetlock_acquire(fx.lock) != 0, "Acquire locks a target");
    rt_game3d_targetlock_cycle(fx.lock, 1);
    EXPECT_TRUE(rt_game3d_targetlock_get_target(fx.lock) == fx.right, "right target locked");
    for (int i = 0; i < 90; ++i)
        rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    /* forward = (-sin(yaw), -cos(yaw)) => bearing yaw = atan2(-3, 4) = -36.87 deg. */
    double yaw = rt_game3d_thirdperson_controller_get_yaw(controller);
    EXPECT_NEAR(yaw,
                std::atan2(-3.0, 4.0) * 180.0 / 3.14159265358979323846,
                2.0,
                "framed yaw eases onto the player->target bearing");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_targetlock_locked_move_bias() {
    TEST("TargetLock3D LockedMoveBias bends the move vector toward the target");
    LockFixture fx = lock_fixture_new("Lock Bias");
    rt_game3d_targetlock_set_require_los(fx.lock, 0);
    rt_game3d_targetlock_acquire(fx.lock);
    EXPECT_TRUE(rt_game3d_targetlock_get_target(fx.lock) == fx.center, "center locked");
    /* Move 20 degrees off the bearing to the center target (bearing = -Z). */
    double off = 20.0 * 3.14159265358979323846 / 180.0;
    void *move = rt_vec3_new(std::sin(off) * -1.0, 0.0, std::cos(off) * -1.0);
    void *biased = rt_game3d_targetlock_locked_move_bias(fx.lock, move);
    EXPECT_TRUE(biased != nullptr, "LockedMoveBias returns a vector");
    /* Bearing is exactly -Z; the biased vector should rotate up to 12 deg toward it. */
    double biased_angle =
        std::atan2(rt_vec3_x(biased), rt_vec3_z(biased)) * 180.0 / 3.14159265358979323846;
    double raw_angle =
        std::atan2(rt_vec3_x(move), rt_vec3_z(move)) * 180.0 / 3.14159265358979323846;
    EXPECT_TRUE(std::fabs(biased_angle - 180.0) < std::fabs(raw_angle - 180.0) - 5.0 ||
                    std::fabs(std::fabs(biased_angle) - 180.0) <
                        std::fabs(std::fabs(raw_angle) - 180.0) - 5.0,
                "biased move vector is meaningfully closer to the target bearing");
    if (rt_obj_release_check0(biased))
        rt_obj_free(biased);
    if (rt_obj_release_check0(move))
        rt_obj_free(move);
    rt_game3d_world_destroy(fx.world);
    PASS();
}

//=========================================================================
// Plan 03 — Character3D upgrades
//=========================================================================

bool test_character_push_and_block_dynamics() {
    TEST("Character3D pushes light crates, is walled by heavy ones, compat flag ghosts");
    /* Raw physics world: ground plane + controller + crate. */
    for (int scenario = 0; scenario < 3; ++scenario) {
        void *world = rt_world3d_new(0.0, -20.0, 0.0);
        void *ground = rt_body3d_new_aabb(50.0, 0.5, 50.0, 0.0);
        rt_body3d_set_position(ground, 0.0, -0.5, 0.0);
        rt_world3d_add(world, ground);
        double crate_mass = scenario == 1 ? 1000.0 : 1.0;
        void *crate = rt_body3d_new_aabb(0.3, 0.3, 0.3, crate_mass);
        rt_body3d_set_position(crate, 0.0, 0.3, -1.2);
        rt_world3d_add(world, crate);
        void *ctrl = rt_character3d_new(0.3, 1.8, 70.0);
        rt_character3d_set_world(ctrl, world);
        rt_character3d_set_position(ctrl, 0.0, 0.91, 0.0);
        if (scenario == 2)
            rt_character3d_set_collide_dynamic(ctrl, 0);

        void *velocity = rt_vec3_new(0.0, -1.0, -2.5);
        for (int i = 0; i < 60; ++i) {
            rt_character3d_move(ctrl, velocity, 1.0 / 60.0);
            rt_world3d_step(world, 1.0 / 60.0);
        }
        if (rt_obj_release_check0(velocity))
            rt_obj_free(velocity);

        void *ctrl_pos = rt_character3d_get_position(ctrl);
        double ctrl_z = rt_vec3_z(ctrl_pos);
        if (rt_obj_release_check0(ctrl_pos))
            rt_obj_free(ctrl_pos);
        void *crate_pos = rt_body3d_get_position(crate);
        double crate_z = rt_vec3_z(crate_pos);
        if (rt_obj_release_check0(crate_pos))
            rt_obj_free(crate_pos);

        if (scenario == 0) {
            EXPECT_TRUE(crate_z < -1.5, "1 kg crate is pushed ahead of the walking controller");
            EXPECT_TRUE(ctrl_z < -0.8, "controller advances while pushing the light crate");
        } else if (scenario == 1) {
            EXPECT_TRUE(crate_z > -1.35, "1000 kg crate barely moves");
            EXPECT_TRUE(ctrl_z > -0.75, "controller is walled by the heavy crate");
        } else {
            EXPECT_TRUE(ctrl_z < -1.5, "CollideDynamic=false restores legacy ghost-through");
        }
    }
    PASS();
}

bool test_character_crouch_and_try_stand() {
    TEST("Character3D crouch shrinks in place; standing under a ceiling fails");
    void *world = rt_world3d_new(0.0, -20.0, 0.0);
    void *ground = rt_body3d_new_aabb(50.0, 0.5, 50.0, 0.0);
    rt_body3d_set_position(ground, 0.0, -0.5, 0.0);
    rt_world3d_add(world, ground);
    /* Ceiling slab: underside at y = 1.2, only over the origin. */
    void *ceiling = rt_body3d_new_aabb(2.0, 0.25, 2.0, 0.0);
    rt_body3d_set_position(ceiling, 0.0, 1.45, 0.0);
    rt_world3d_add(world, ceiling);

    void *ctrl = rt_character3d_new(0.3, 1.8, 70.0);
    rt_character3d_set_world(ctrl, world);
    rt_character3d_set_position(ctrl, 0.0, 0.91, 0.0);
    EXPECT_NEAR(rt_character3d_get_height(ctrl), 1.8, 1e-9, "initial capsule height");

    EXPECT_TRUE(rt_character3d_try_set_height(ctrl, 0.9) != 0, "crouch always succeeds");
    EXPECT_NEAR(rt_character3d_get_height(ctrl), 0.9, 1e-9, "crouched capsule height");
    void *pos = rt_character3d_get_position(ctrl);
    EXPECT_NEAR(rt_vec3_y(pos) - 0.45, 0.01, 0.05, "crouch keeps the feet planted");
    if (rt_obj_release_check0(pos))
        rt_obj_free(pos);

    EXPECT_TRUE(rt_character3d_try_set_height(ctrl, 1.8) == 0,
                "standing under a 1.2 m ceiling is rejected");
    EXPECT_NEAR(rt_character3d_get_height(ctrl), 0.9, 1e-9, "failed stand keeps crouch height");

    /* Walk clear of the ceiling and stand. */
    rt_character3d_set_position(ctrl, 6.0, 0.46, 0.0);
    EXPECT_TRUE(rt_character3d_try_set_height(ctrl, 1.8) != 0, "standing succeeds in the open");
    EXPECT_NEAR(rt_character3d_get_height(ctrl), 1.8, 1e-9, "stood back to full height");
    PASS();
}

bool test_character_rides_kinematic_platform() {
    TEST("Character3D rides a moving kinematic platform");
    void *world = rt_world3d_new(0.0, -20.0, 0.0);
    void *platform = rt_body3d_new_aabb(2.0, 0.25, 2.0, 1.0);
    rt_body3d_set_kinematic(platform, 1);
    rt_body3d_set_position(platform, 0.0, -0.25, 0.0);
    rt_body3d_set_velocity(platform, 2.0, 0.0, 0.0);
    rt_world3d_add(world, platform);

    void *ctrl = rt_character3d_new(0.3, 1.8, 70.0);
    rt_character3d_set_world(ctrl, world);
    rt_character3d_set_position(ctrl, 0.0, 0.91, 0.0);

    void *velocity = rt_vec3_new(0.0, -1.0, 0.0);
    for (int i = 0; i < 60; ++i) {
        rt_character3d_move(ctrl, velocity, 1.0 / 60.0);
        rt_world3d_step(world, 1.0 / 60.0);
    }
    if (rt_obj_release_check0(velocity))
        rt_obj_free(velocity);

    EXPECT_TRUE(rt_character3d_get_ground_body(ctrl) == platform,
                "GetGroundBody exposes the platform under the controller");
    void *pos = rt_character3d_get_position(ctrl);
    double x = rt_vec3_x(pos);
    if (rt_obj_release_check0(pos))
        rt_obj_free(pos);
    EXPECT_TRUE(x > 1.5, "idle controller tracks the platform motion");

    /* RidePlatforms off: controller stops tracking. */
    rt_character3d_set_ride_platforms(ctrl, 0);
    void *pos_before = rt_character3d_get_position(ctrl);
    double x_before = rt_vec3_x(pos_before);
    if (rt_obj_release_check0(pos_before))
        rt_obj_free(pos_before);
    void *velocity2 = rt_vec3_new(0.0, -1.0, 0.0);
    for (int i = 0; i < 30; ++i) {
        rt_character3d_move(ctrl, velocity2, 1.0 / 60.0);
        rt_world3d_step(world, 1.0 / 60.0);
    }
    if (rt_obj_release_check0(velocity2))
        rt_obj_free(velocity2);
    void *pos_after = rt_character3d_get_position(ctrl);
    double x_after = rt_vec3_x(pos_after);
    if (rt_obj_release_check0(pos_after))
        rt_obj_free(pos_after);
    EXPECT_TRUE(std::fabs(x_after - x_before) < 0.2, "RidePlatforms=false stops platform tracking");
    PASS();
}

bool test_character_steep_slope_slides() {
    TEST("Character3D slides on slopes past the limit and reports IsSliding");
    for (int scenario = 0; scenario < 2; ++scenario) {
        double tilt_deg = scenario == 0 ? 70.0 : 30.0;
        void *world = rt_world3d_new(0.0, -20.0, 0.0);
        void *slope = rt_body3d_new_aabb(6.0, 0.5, 6.0, 0.0);
        void *axis = rt_vec3_new(1.0, 0.0, 0.0);
        void *quat = rt_quat_from_axis_angle(axis, tilt_deg * 3.14159265358979323846 / 180.0);
        rt_body3d_set_position(slope, 0.0, 0.0, 0.0);
        rt_body3d_set_orientation(slope, quat);
        rt_world3d_add(world, slope);
        if (rt_obj_release_check0(quat))
            rt_obj_free(quat);
        if (rt_obj_release_check0(axis))
            rt_obj_free(axis);

        void *ctrl = rt_character3d_new(0.3, 1.8, 70.0);
        rt_character3d_set_world(ctrl, world);
        rt_character3d_set_position(ctrl, 0.0, 3.0, 0.0);

        double start_y = 3.0;
        void *velocity = rt_vec3_new(0.0, -4.0, 0.0);
        for (int i = 0; i < 90; ++i) {
            rt_character3d_move(ctrl, velocity, 1.0 / 60.0);
            rt_world3d_step(world, 1.0 / 60.0);
        }
        if (rt_obj_release_check0(velocity))
            rt_obj_free(velocity);

        void *pos = rt_character3d_get_position(ctrl);
        double end_y = rt_vec3_y(pos);
        if (rt_obj_release_check0(pos))
            rt_obj_free(pos);
        (void)start_y;
        (void)end_y;
        if (scenario == 0) {
            EXPECT_TRUE(rt_character3d_is_grounded(ctrl) == 0,
                        "70 deg slope is not walkable ground");
            EXPECT_TRUE(rt_character3d_is_sliding(ctrl) != 0, "70 deg slope reports sliding");
        } else {
            EXPECT_TRUE(rt_character3d_is_grounded(ctrl) != 0, "30 deg slope is walkable");
            EXPECT_TRUE(rt_character3d_is_sliding(ctrl) == 0, "30 deg slope is not sliding");
        }
    }
    PASS();
}

//=========================================================================
// Plan 04 — Traversal probes
//=========================================================================

bool test_probe_clearance() {
    TEST("Physics3DWorld.ProbeClearance detects open space and blockers");
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *box = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    rt_body3d_set_position(box, 0.0, 1.0, 0.0);
    rt_world3d_add(world, box);
    void *open_pos = rt_vec3_new(5.0, 1.0, 0.0);
    void *blocked_pos = rt_vec3_new(0.0, 1.0, 0.0);
    EXPECT_TRUE(rt_world3d_probe_clearance(world, open_pos, 0.3, 1.8, -1) != 0,
                "open space has clearance");
    EXPECT_TRUE(rt_world3d_probe_clearance(world, blocked_pos, 0.3, 1.8, -1) == 0,
                "space inside a box has no clearance");
    if (rt_obj_release_check0(blocked_pos))
        rt_obj_free(blocked_pos);
    if (rt_obj_release_check0(open_pos))
        rt_obj_free(open_pos);
    PASS();
}

bool test_probe_ledge_and_mask() {
    TEST("Physics3DWorld.ProbeLedge finds a 1 m wall top with standing room");
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    /* Wall: spans y 0..1, z 1.25..1.75. */
    void *wall = rt_body3d_new_aabb(2.0, 0.5, 0.25, 0.0);
    rt_body3d_set_position(wall, 0.0, 0.5, 1.5);
    rt_world3d_add(world, wall);

    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *forward = rt_vec3_new(0.0, 0.0, 1.0);
    void *hit = rt_world3d_probe_ledge(world, origin, forward, 0.3, 2.0, 1.5, -1);
    EXPECT_TRUE(hit != nullptr, "ledge probe finds the wall top");
    EXPECT_NEAR(rt_ledge_hit3d_get_height(hit), 1.0, 0.1, "ledge height matches the wall");
    void *surface = rt_ledge_hit3d_get_surface_normal(hit);
    EXPECT_TRUE(surface != nullptr && rt_vec3_y(surface) > 0.9, "ledge top is upward-facing");
    if (surface && rt_obj_release_check0(surface))
        rt_obj_free(surface);
    EXPECT_TRUE(rt_ledge_hit3d_get_has_standing_room(hit) != 0,
                "open wall top reports standing room");
    EXPECT_TRUE(rt_ledge_hit3d_get_has_landing(hit) == 0, "ledge probes have no landing");
    if (hit && rt_obj_release_check0(hit))
        rt_obj_free(hit);

    /* Mask exclusion: wall on layer bit 4, probe mask bit 1. */
    rt_body3d_set_collision_layer(wall, 1 << 2);
    void *masked = rt_world3d_probe_ledge(world, origin, forward, 0.3, 2.0, 1.5, 1 << 0);
    EXPECT_TRUE(masked == nullptr, "mask-excluded wall yields no ledge");
    if (masked && rt_obj_release_check0(masked))
        rt_obj_free(masked);

    /* Too tall: 1 m budget cannot reach a 1 m wall top from y=0 with margin. */
    void *short_hit = rt_world3d_probe_ledge(world, origin, forward, 0.3, 0.5, 1.5, -1);
    EXPECT_TRUE(short_hit == nullptr, "insufficient maxHeight yields no ledge");
    if (short_hit && rt_obj_release_check0(short_hit))
        rt_obj_free(short_hit);

    if (rt_obj_release_check0(forward))
        rt_obj_free(forward);
    if (rt_obj_release_check0(origin))
        rt_obj_free(origin);
    PASS();
}

bool test_probe_vault_thin_vs_thick() {
    TEST("Physics3DWorld.ProbeVault accepts thin walls with far landings only");
    for (int scenario = 0; scenario < 2; ++scenario) {
        void *world = rt_world3d_new(0.0, 0.0, 0.0);
        /* Ground plane top at y = 0 everywhere. */
        void *ground = rt_body3d_new_aabb(50.0, 0.5, 50.0, 0.0);
        rt_body3d_set_position(ground, 0.0, -0.5, 0.0);
        rt_world3d_add(world, ground);
        /* Wall: thin (0.4) or thick (3.0), 1 m tall, front face at z = 1.3. */
        double half_thickness = scenario == 0 ? 0.2 : 1.5;
        void *wall = rt_body3d_new_aabb(2.0, 0.5, half_thickness, 0.0);
        rt_body3d_set_position(wall, 0.0, 0.5, 1.3 + half_thickness);
        rt_world3d_add(world, wall);

        /* Foot-level origin on the ground plane. */
        void *origin = rt_vec3_new(0.0, 0.0, 0.0);
        void *forward = rt_vec3_new(0.0, 0.0, 1.0);
        void *hit = rt_world3d_probe_vault(world, origin, forward, 0.3, 2.0, 0.8, -1);
        if (scenario == 0) {
            EXPECT_TRUE(hit != nullptr, "thin wall with a far-side floor is vaultable");
            void *landing = rt_ledge_hit3d_get_landing_point(hit);
            EXPECT_TRUE(landing != nullptr, "vault result carries a landing point");
            EXPECT_NEAR(rt_vec3_y(landing), 0.0, 0.35, "landing sits at ground level");
            if (landing && rt_obj_release_check0(landing))
                rt_obj_free(landing);
        } else {
            EXPECT_TRUE(hit == nullptr, "thick wall is not vaultable");
        }
        if (hit && rt_obj_release_check0(hit))
            rt_obj_free(hit);
        if (rt_obj_release_check0(forward))
            rt_obj_free(forward);
        if (rt_obj_release_check0(origin))
            rt_obj_free(origin);
    }
    PASS();
}

bool test_character_probe_sugar() {
    TEST("CharacterController3D probe sugar defaults from the character pose");
    void *world = rt_game3d_world_new(rt_const_cstr("TP Probe Sugar"), 64, 48);
    /* Ground + a 1 m wall one unit ahead on -Z (identity facing). */
    spawn_static_box(world, 0.0, -0.5, 0.0, 50.0, 0.5, 50.0);
    spawn_static_box(world, 0.0, 0.5, -1.0, 2.0, 0.5, 0.2);
    void *player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(player, 0.0, 0.91, 0.0);
    rt_game3d_world_spawn(world, player);
    void *char_controller = rt_game3d_character_controller_new(world, player, 0.3, 1.8, 70.0);
    void *hit = rt_game3d_character_controller_probe_ledge(char_controller, 2.0);
    EXPECT_TRUE(hit != nullptr, "probeLedge sugar finds the wall ahead of the facing");
    if (hit && rt_obj_release_check0(hit))
        rt_obj_free(hit);
    rt_game3d_world_destroy(world);
    PASS();
}

} // namespace

int main() {
    std::printf("Game3D third-person upgrade tests\n");
    bool ok = true;
    ok = test_thirdperson_defaults_and_orbit() && ok;
    ok = test_thirdperson_boom_pull_in_and_release() && ok;
    ok = test_thirdperson_penetrating_start_snaps_to_min() && ok;
    ok = test_thirdperson_character_drive_yaw_basis() && ok;
    ok = test_thirdperson_aim_blend() && ok;
    ok = test_thirdperson_occluder_fade_and_restore() && ok;
    ok = test_targetlock_acquire_prefers_near_center() && ok;
    ok = test_targetlock_los_gating() && ok;
    ok = test_targetlock_cycle_direction() && ok;
    ok = test_targetlock_break_distance_release() && ok;
    ok = test_targetlock_framing_converges() && ok;
    ok = test_targetlock_locked_move_bias() && ok;
    ok = test_character_push_and_block_dynamics() && ok;
    ok = test_character_crouch_and_try_stand() && ok;
    ok = test_character_rides_kinematic_platform() && ok;
    ok = test_character_steep_slope_slides() && ok;
    ok = test_probe_clearance() && ok;
    ok = test_probe_ledge_and_mask() && ok;
    ok = test_probe_vault_thin_vs_thick() && ok;
    ok = test_character_probe_sugar() && ok;
    std::printf("\nThird-person upgrade tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
