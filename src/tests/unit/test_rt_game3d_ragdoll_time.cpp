//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d_ragdoll_time.cpp
// Purpose: Unit tests for Ragdoll3D (builder, activation handoff, settle,
//   deactivate blend-out) and the World3D time-control layer (timeScale,
//   pause, hit-stop, unscaled counters).
// Key invariants:
//   - Deterministic fixed steps; scenario replays produce identical results.
// Ownership/Lifetime:
//   - Test-created runtime handles rely on production GC conventions.
// Links: misc/plans/thirdpersonupgrade/07-ragdoll.md,
//   misc/plans/thirdpersonupgrade/08-time-control.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_animcontroller3d.h"
#include "rt_game3d.h"
#include "rt_mat4.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_quat.h"
#include "rt_ragdoll3d.h"
#include "rt_scene3d.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
#include "rt_vec3.h"

#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

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

template <typename Fn> bool expect_trap_contains(Fn &&fn, const char *needle) {
    g_last_trap = nullptr;
    g_expect_trap = true;
    if (setjmp(g_trap_jmp) == 0) {
        fn();
        g_expect_trap = false;
        return false;
    }
    g_expect_trap = false;
    return g_last_trap && (!needle || std::strstr(g_last_trap, needle) != nullptr);
}

namespace {

/// @brief Five-bone vertical chain skeleton (root at Y=1.5, +Y links of 0.3).
void *chain_skeleton_new() {
    void *skeleton = rt_skeleton3d_new();
    const char *names[5] = {"root", "spine", "chest", "neck", "head"};
    for (int i = 0; i < 5; ++i) {
        void *bind = rt_mat4_new(1.0,
                                 0.0,
                                 0.0,
                                 0.0,
                                 0.0,
                                 1.0,
                                 0.0,
                                 i == 0 ? 1.5 : 0.3,
                                 0.0,
                                 0.0,
                                 1.0,
                                 0.0,
                                 0.0,
                                 0.0,
                                 0.0,
                                 1.0);
        rt_skeleton3d_add_bone(skeleton, rt_const_cstr(names[i]), i - 1, bind);
        if (rt_obj_release_check0(bind))
            rt_obj_free(bind);
    }
    rt_skeleton3d_compute_inverse_bind(skeleton);
    return skeleton;
}

void *translation_matrix_new(double x, double y, double z) {
    return rt_mat4_new(1.0, 0.0, 0.0, x, 0.0, 1.0, 0.0, y, 0.0, 0.0, 1.0, z, 0.0, 0.0, 0.0, 1.0);
}

void add_bone_with_matrix(void *skeleton, const char *name, int64_t parent, void *matrix) {
    rt_skeleton3d_add_bone(skeleton, rt_const_cstr(name), parent, matrix);
    if (rt_obj_release_check0(matrix))
        rt_obj_free(matrix);
}

void *branched_skeleton_new() {
    void *skeleton = rt_skeleton3d_new();
    add_bone_with_matrix(skeleton, "root", -1, translation_matrix_new(0.0, 0.0, 0.0));
    add_bone_with_matrix(skeleton, "helper", 0, translation_matrix_new(0.05, 0.0, 0.0));
    add_bone_with_matrix(skeleton, "limb", 0, translation_matrix_new(0.0, 1.0, 0.0));
    add_bone_with_matrix(skeleton, "tip", 2, translation_matrix_new(0.0, 0.5, 0.0));
    rt_skeleton3d_compute_inverse_bind(skeleton);
    return skeleton;
}

void *short_child_skeleton_new() {
    void *skeleton = rt_skeleton3d_new();
    add_bone_with_matrix(skeleton, "root", -1, translation_matrix_new(0.0, 0.0, 0.0));
    add_bone_with_matrix(skeleton, "tip", 0, translation_matrix_new(0.0, 0.02, 0.0));
    rt_skeleton3d_compute_inverse_bind(skeleton);
    return skeleton;
}

void *rotated_terminal_skeleton_new() {
    void *skeleton = rt_skeleton3d_new();
    void *root = rt_mat4_new(
        0.0, -1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
    add_bone_with_matrix(skeleton, "root", -1, root);
    rt_skeleton3d_compute_inverse_bind(skeleton);
    return skeleton;
}

//=========================================================================
// Plan 07 — Ragdoll3D
//=========================================================================

bool test_ragdoll_builder_chain() {
    TEST("Ragdoll3D builder: 5-bone chain yields 5 capsules with distributed mass");
    void *skeleton = chain_skeleton_new();
    void *ragdoll = rt_ragdoll3d_from_skeleton(skeleton);
    EXPECT_TRUE(ragdoll != nullptr, "FromSkeleton returns an object");
    EXPECT_NEAR(rt_ragdoll3d_get_total_mass(ragdoll), 70.0, 1e-9, "default total mass");
    EXPECT_EQ_INT(rt_ragdoll3d_get_body_count(ragdoll), 5, "five bodied bones");
    EXPECT_TRUE(rt_ragdoll3d_get_active(ragdoll) == 0, "inactive after build");

    double mass_sum = 0.0;
    const char *names[5] = {"root", "spine", "chest", "neck", "head"};
    for (int i = 0; i < 5; ++i) {
        void *body = rt_ragdoll3d_get_body(ragdoll, rt_const_cstr(names[i]));
        EXPECT_TRUE(body != nullptr, "every chain bone has a body");
        mass_sum += rt_body3d_get_mass(body);
    }
    EXPECT_NEAR(mass_sum, 70.0, 0.5, "masses sum to the configured total");
    void *root_body = rt_ragdoll3d_get_body(ragdoll, rt_const_cstr("root"));
    void *root_position = rt_body3d_get_position(root_body);
    EXPECT_NEAR(rt_vec3_y(root_position), 1.65, 1e-6, "root center uses Mat4 column translation");

    rt_ragdoll3d_set_total_mass(ragdoll, rt_ragdoll3d_get_total_mass(ragdoll));
    rt_ragdoll3d_set_radius_scale(ragdoll, rt_ragdoll3d_get_radius_scale(ragdoll));
    rt_ragdoll3d_set_min_bone_length(ragdoll, rt_ragdoll3d_get_min_bone_length(ragdoll));
    rt_ragdoll3d_set_total_mass(ragdoll, std::numeric_limits<double>::max());
    EXPECT_TRUE(rt_ragdoll3d_get_body(ragdoll, rt_const_cstr("root")) == root_body,
                "unchanged and out-of-range configuration preserves the built rig");
    PASS();
}

bool test_ragdoll_builder_handles_branches_short_children_and_rotated_terminals() {
    TEST("Ragdoll3D builder uses longest children and column-vector terminal bases");
    void *branched = rt_ragdoll3d_from_skeleton(branched_skeleton_new());
    void *branched_root = rt_ragdoll3d_get_body(branched, rt_const_cstr("root"));
    void *branched_pos = rt_body3d_get_position(branched_root);
    EXPECT_NEAR(
        rt_vec3_x(branched_pos), 0.0, 1e-9, "tiny helper is not selected by insertion order");
    EXPECT_NEAR(rt_vec3_y(branched_pos), 0.5, 1e-9, "root follows its farthest direct child");

    void *short_ragdoll = rt_ragdoll3d_from_skeleton(short_child_skeleton_new());
    void *short_root = rt_ragdoll3d_get_body(short_ragdoll, rt_const_cstr("root"));
    void *short_pos = rt_body3d_get_position(short_root);
    EXPECT_NEAR(rt_vec3_y(short_pos),
                0.01,
                1e-9,
                "a real short child is not replaced by the terminal fallback length");

    void *rotated_skeleton = rotated_terminal_skeleton_new();
    void *rotated_ragdoll = rt_ragdoll3d_from_skeleton(rotated_skeleton);
    void *rotated_root = rt_ragdoll3d_get_body(rotated_ragdoll, rt_const_cstr("root"));
    void *rotated_pos = rt_body3d_get_position(rotated_root);
    EXPECT_NEAR(rt_vec3_x(rotated_pos),
                -0.075,
                1e-6,
                "terminal extension reads the transformed +Y basis column");
    EXPECT_NEAR(rt_vec3_y(rotated_pos), 0.0, 1e-9, "rotated terminal center stays on its axis");

    void *rotated_controller = rt_anim_controller3d_new(rotated_skeleton);
    void *rotated_world = rt_world3d_new(0.0, 0.0, 0.0);
    void *rotated_node = rt_scene_node3d_new();
    rt_anim_controller3d_update(rotated_controller, 1.0 / 60.0);
    rt_ragdoll3d_activate(rotated_ragdoll, rotated_world, rotated_controller, rotated_node);
    rt_ragdoll3d_step(rotated_ragdoll, 1.0 / 60.0);
    void *rotated_matrix = rt_anim_controller3d_get_bone_matrix(rotated_controller, 0);
    EXPECT_NEAR(rt_mat4_get(rotated_matrix, 0, 1),
                -1.0,
                1e-6,
                "quaternion extraction preserves positive quarter-turn handedness");
    EXPECT_NEAR(rt_mat4_get(rotated_matrix, 1, 0),
                1.0,
                1e-6,
                "quaternion composition writes the row-major rotation convention");
    rt_ragdoll3d_deactivate(rotated_ragdoll, 0.0);
    PASS();
}

bool test_ragdoll_requires_matching_controller_and_follows_parented_world_motion() {
    TEST("Ragdoll3D validates controller identity and root-follows in world space");
    void *skeleton = chain_skeleton_new();
    void *ragdoll = rt_ragdoll3d_from_skeleton(skeleton);
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *node = rt_scene_node3d_new();
    void *wrong_controller = rt_anim_controller3d_new(chain_skeleton_new());
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_ragdoll3d_activate(ragdoll, world, wrong_controller, node); },
                             "ragdoll skeleton"),
        "activation rejects a controller built from another skeleton instance");
    EXPECT_EQ_INT(rt_world3d_body_count(world), 0, "mismatched activation registers no bodies");

    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    rt_scene3d_add(scene, parent);
    rt_scene_node3d_add_child(parent, node);
    rt_scene_node3d_set_position(parent, 10.0, 20.0, 0.0);
    void *quarter_turn =
        rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), 3.14159265358979323846 * 0.5);
    rt_scene_node3d_set_rotation(parent, quarter_turn);
    rt_scene_node3d_set_position(node, 1.0, 0.0, 0.0);

    void *controller = rt_anim_controller3d_new(skeleton);
    rt_anim_controller3d_update(controller, 1.0 / 60.0);
    rt_ragdoll3d_activate(ragdoll, world, controller, node);
    double before_x = 0.0, before_y = 0.0, before_z = 0.0;
    EXPECT_TRUE(
        rt_scene_node3d_get_world_position_components(node, &before_x, &before_y, &before_z) != 0,
        "parented node has a world pose");
    void *root = rt_ragdoll3d_get_body(ragdoll, rt_const_cstr("root"));
    void *body_pos = rt_body3d_get_position(root);
    rt_body3d_set_position(
        root, rt_vec3_x(body_pos) + 1.0, rt_vec3_y(body_pos), rt_vec3_z(body_pos));
    rt_ragdoll3d_step(ragdoll, 1.0 / 60.0);
    double after_x = 0.0, after_y = 0.0, after_z = 0.0;
    rt_scene_node3d_get_world_position_components(node, &after_x, &after_y, &after_z);
    EXPECT_NEAR(after_x, before_x + 1.0, 1e-9, "world-X corpse motion advances node world X");
    EXPECT_NEAR(after_y, before_y, 1e-9, "parent rotation does not rotate world motion twice");
    EXPECT_NEAR(after_z, before_z, 1e-9, "root follow preserves orthogonal world position");
    rt_ragdoll3d_deactivate(ragdoll, 0.0);
    PASS();
}

bool test_ragdoll_finalizer_unregisters_active_world_objects() {
    TEST("Ragdoll3D finalizer removes active bodies and joints from its world");
    void *skeleton = chain_skeleton_new();
    void *controller = rt_anim_controller3d_new(skeleton);
    rt_anim_controller3d_update(controller, 1.0 / 60.0);
    void *node = rt_scene_node3d_new();
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *ragdoll = rt_ragdoll3d_from_skeleton(skeleton);
    rt_ragdoll3d_activate(ragdoll, world, controller, node);
    EXPECT_TRUE(rt_world3d_body_count(world) > 0 && rt_world3d_joint_count(world) > 0,
                "active rig registered bodies and joints");
    EXPECT_TRUE(rt_obj_release_check0(ragdoll) != 0, "test owns the active ragdoll");
    rt_obj_free(ragdoll);
    EXPECT_EQ_INT(rt_world3d_body_count(world), 0, "finalizer removed every rig body");
    EXPECT_EQ_INT(rt_world3d_joint_count(world), 0, "finalizer removed every rig joint");
    PASS();
}

bool test_ragdoll_activate_settle_deactivate() {
    TEST("Ragdoll3D activates, settles onto a floor, and blends back out");
    void *skeleton = chain_skeleton_new();
    void *controller = rt_anim_controller3d_new(skeleton);
    void *clip = rt_animation3d_new(rt_const_cstr("Idle"), 1.0);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("Idle"), clip);
    rt_anim_controller3d_play(controller, rt_const_cstr("Idle"));
    rt_anim_controller3d_update(controller, 1.0 / 60.0);
    rt_anim_controller3d_update(controller, 1.0 / 60.0);

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    rt_scene3d_add(scene, node);
    void *world = rt_world3d_new(0.0, -20.0, 0.0);
    void *floor = rt_body3d_new_aabb(20.0, 0.5, 20.0, 0.0);
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0);
    rt_world3d_add(world, floor);

    void *ragdoll = rt_ragdoll3d_from_skeleton(skeleton);
    rt_ragdoll3d_activate(ragdoll, world, controller, node);
    EXPECT_TRUE(rt_ragdoll3d_get_active(ragdoll) != 0, "active after Activate");

    void *root_body = rt_ragdoll3d_get_body(ragdoll, rt_const_cstr("root"));
    EXPECT_TRUE(root_body != nullptr, "root body resolves");
    void *start_pos = rt_body3d_get_position(root_body);
    double start_y = rt_vec3_y(start_pos);
    if (rt_obj_release_check0(start_pos))
        rt_obj_free(start_pos);

    for (int i = 0; i < 300; ++i) {
        rt_world3d_step(world, 1.0 / 60.0);
        rt_ragdoll3d_step(ragdoll, 1.0 / 60.0);
    }
    void *end_pos = rt_body3d_get_position(root_body);
    double end_y = rt_vec3_y(end_pos);
    double end_x = rt_vec3_x(end_pos);
    if (rt_obj_release_check0(end_pos))
        rt_obj_free(end_pos);
    EXPECT_TRUE(std::isfinite(end_y) && std::isfinite(end_x), "no NaNs after settling");
    EXPECT_TRUE(end_y < start_y - 0.3, "chain fell under gravity");
    EXPECT_TRUE(end_y > -0.6, "chain rests on (not through) the floor");

    /* Deactivate with a blend; stepping progresses and completes the blend. */
    rt_ragdoll3d_deactivate(ragdoll, 0.5);
    EXPECT_TRUE(rt_ragdoll3d_get_active(ragdoll) == 0, "inactive after Deactivate");
    for (int i = 0; i < 60; ++i) {
        rt_anim_controller3d_update(controller, 1.0 / 60.0);
        rt_ragdoll3d_step(ragdoll, 1.0 / 60.0);
    }
    /* After the blend, the palette follows live animation again: the bind-pose
     * root bone matrix translation matches the skeleton bind (1.5). */
    void *root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_TRUE(root_mat != nullptr, "root bone matrix readable after blend");
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 0), 1.0, 1e-6, "completed blend restores exact basis");
    EXPECT_NEAR(rt_mat4_get(root_mat, 1, 3),
                1.5,
                1e-6,
                "completed blend reaches the exact animated translation");
    if (root_mat && rt_obj_release_check0(root_mat))
        rt_obj_free(root_mat);
    PASS();
}

bool test_ragdoll_game3d_sugar() {
    TEST("Entity3D.enableRagdoll builds, activates, and steps inside the world loop");
    void *world = rt_game3d_world_new(rt_const_cstr("Ragdoll Sugar"), 64, 48);
    /* Floor. */
    void *floor_entity = rt_game3d_entity_new();
    void *floor_body = rt_body3d_new_aabb(20.0, 0.5, 20.0, 0.0);
    rt_game3d_entity_attach_body(floor_entity, floor_body);
    rt_game3d_entity_set_position(floor_entity, 0.0, -0.5, 0.0);
    rt_game3d_world_spawn(world, floor_entity);

    /* Animated agent with a chain skeleton. */
    void *agent = rt_game3d_entity_new();
    rt_game3d_entity_set_position(agent, 0.0, 0.0, 0.0);
    rt_game3d_world_spawn(world, agent);
    void *skeleton = chain_skeleton_new();
    void *controller = rt_anim_controller3d_new(skeleton);
    void *clip = rt_animation3d_new(rt_const_cstr("Idle"), 1.0);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("Idle"), clip);
    rt_anim_controller3d_play(controller, rt_const_cstr("Idle"));
    rt_game3d_entity_attach_animator(agent, controller);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);

    void *ragdoll = rt_game3d_entity_enable_ragdoll(agent);
    EXPECT_TRUE(ragdoll != nullptr, "enableRagdoll returns the rig");
    EXPECT_TRUE(rt_game3d_entity_get_ragdoll(agent) == ragdoll, "rig cached on the entity");
    EXPECT_TRUE(rt_ragdoll3d_get_active(ragdoll) != 0, "rig active");

    for (int i = 0; i < 120; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *head = rt_ragdoll3d_get_body(ragdoll, rt_const_cstr("head"));
    void *head_pos = rt_body3d_get_position(head);
    EXPECT_TRUE(std::isfinite(rt_vec3_y(head_pos)), "rig stays finite in the world loop");
    EXPECT_TRUE(rt_vec3_y(head_pos) < 2.6, "head fell from its bind height");
    if (rt_obj_release_check0(head_pos))
        rt_obj_free(head_pos);

    EXPECT_TRUE(rt_game3d_entity_disable_ragdoll(agent, 0.25) != 0, "disable succeeds");
    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    rt_game3d_world_destroy(world);
    PASS();
}

//=========================================================================
// Plan 08 — Time control
//=========================================================================

/// @brief World with one falling dynamic crate entity; returns the entity.
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

bool test_time_pause_freezes_simulation() {
    TEST("World3D.Paused freezes bodies and Elapsed while UnscaledElapsed advances");
    void *world = nullptr;
    void *crate = falling_world_new(&world, "Time Pause");
    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    double y_before = entity_y(crate);
    double elapsed_before = rt_game3d_world_get_elapsed(world);
    double unscaled_before = rt_game3d_world_get_unscaled_elapsed(world);
    EXPECT_TRUE(y_before < 20.0, "crate falls while unpaused");

    rt_game3d_world_set_paused(world, 1);
    for (int i = 0; i < 60; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_NEAR(entity_y(crate), y_before, 1e-12, "paused body is frozen");
    EXPECT_NEAR(rt_game3d_world_get_elapsed(world),
                elapsed_before,
                1e-12,
                "Elapsed is frozen while paused");
    EXPECT_NEAR(rt_game3d_world_get_unscaled_elapsed(world) - unscaled_before,
                1.0,
                1e-9,
                "UnscaledElapsed advances in real time");

    rt_game3d_world_set_paused(world, 0);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_TRUE(entity_y(crate) < y_before, "unpausing resumes the simulation");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_time_scale_slows_simulation() {
    TEST("World3D.TimeScale 0.5 advances simulated time at half rate");
    void *world_full = nullptr;
    void *crate_full = falling_world_new(&world_full, "Time Full");
    void *world_half = nullptr;
    void *crate_half = falling_world_new(&world_half, "Time Half");
    rt_game3d_world_set_time_scale(world_half, 0.5);

    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(world_full, 1.0 / 60.0);
    for (int i = 0; i < 60; ++i)
        rt_game3d_world_step_simulation(world_half, 1.0 / 60.0);

    EXPECT_NEAR(rt_game3d_world_get_elapsed(world_half),
                rt_game3d_world_get_elapsed(world_full),
                1e-9,
                "half-scale world needs twice the frames for equal sim time");
    /* Same simulated duration => comparable fall distance (integration step
     * sizes differ, so allow a coarse tolerance). */
    EXPECT_NEAR(entity_y(crate_half),
                entity_y(crate_full),
                0.2,
                "fall distance tracks simulated time, not wall frames");
    rt_game3d_world_destroy(world_half);
    rt_game3d_world_destroy(world_full);
    PASS();
}

bool test_time_hitstop_latch() {
    TEST("World3D.HitStop freezes for its real-time window without stacking");
    void *world = nullptr;
    void *crate = falling_world_new(&world, "Time HitStop");
    for (int i = 0; i < 10; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    double y_frozen = entity_y(crate);

    rt_game3d_world_hit_stop(world, 0.1); /* 6 frames at 60 Hz */
    for (int i = 0; i < 6; ++i) {
        if (i == 2)
            rt_game3d_world_hit_stop(world, 0.05); /* shorter: must not extend */
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
        EXPECT_NEAR(entity_y(crate), y_frozen, 1e-12, "hit-stop frame is frozen");
    }
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_TRUE(entity_y(crate) < y_frozen, "simulation resumes after the window");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_time_scale_zero_behaves_like_pause() {
    TEST("TimeScale 0 freezes like pause and restores cleanly");
    void *world = nullptr;
    void *crate = falling_world_new(&world, "Time Zero");
    for (int i = 0; i < 10; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    double y = entity_y(crate);
    rt_game3d_world_set_time_scale(world, 0.0);
    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_NEAR(entity_y(crate), y, 1e-12, "scale 0 freezes the body");
    rt_game3d_world_set_time_scale(world, 1.0);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_TRUE(entity_y(crate) < y, "restoring scale resumes");
    rt_game3d_world_destroy(world);
    PASS();
}

} // namespace

int main() {
    std::printf("Game3D ragdoll + time-control tests\n");
    bool ok = true;
    ok = test_ragdoll_builder_chain() && ok;
    ok = test_ragdoll_builder_handles_branches_short_children_and_rotated_terminals() && ok;
    ok = test_ragdoll_requires_matching_controller_and_follows_parented_world_motion() && ok;
    ok = test_ragdoll_finalizer_unregisters_active_world_objects() && ok;
    ok = test_ragdoll_activate_settle_deactivate() && ok;
    ok = test_ragdoll_game3d_sugar() && ok;
    ok = test_time_pause_freezes_simulation() && ok;
    ok = test_time_scale_slows_simulation() && ok;
    ok = test_time_hitstop_latch() && ok;
    ok = test_time_scale_zero_behaves_like_pause() && ok;
    std::printf("\nRagdoll + time-control tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
