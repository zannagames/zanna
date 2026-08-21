//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_animcontroller3d.cpp
// Purpose: Unit tests for AnimController3D state playback, events, root motion,
//   masked overlay layers, and IKSolver3D numeric/storage robustness.
//
// Key invariants:
//   - Controller/blend-tree poses remain bounded across private storage growth.
//   - Events, transitions, root motion, and overlays are deterministic.
// Ownership/Lifetime:
//   - Runtime objects are test-process managed and released by the runtime heap.
// Links: rt_animcontroller3d.h, rt_blendtree3d.h, rt_iksolver3d.h,
//   rt_skeleton3d.h
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_animcontroller3d.h"
#include "rt_blendtree3d.h"
#include "rt_box.h"
#include "rt_iksolver3d.h"
#include "rt_seq.h"
#include "rt_seq_internal.h"
#include "rt_skeleton3d.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_quat_new(double x, double y, double z, double w);
extern void *rt_mat4_identity(void);
extern void *rt_mat4_translate(double tx, double ty, double tz);
extern void *rt_mat4_rotate_z(double angle);
extern void *rt_mat4_rotate_y(double angle);
extern void *rt_mat4_mul(void *a, void *b);
extern double rt_mat4_get(void *m, int64_t row, int64_t col);
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern rt_string rt_const_cstr(const char *s);
extern const char *rt_string_cstr(rt_string s);
}

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (fabs((double)(a) - (double)(b)) > (eps)) {                                             \
            fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", msg, (double)(a), (double)(b));    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

struct AnimController3DTestPrefix {
    void *vptr;
    void *skeleton;
    void *states;
    int32_t state_count;
    int32_t state_capacity;
    uint64_t *state_name_hashes;
    int32_t *state_name_indices;
    int32_t state_name_index_capacity;
    int8_t state_name_index_dirty;
    void *transitions;
    int32_t transition_count;
    int32_t transition_capacity;
    void *events;
    int32_t event_count;
    int32_t event_capacity;
};

struct AnimController3DStateTestLayout {
    char name[64];
    void *animation;
    float speed;
    int8_t looping;
};

struct AnimController3DLayerTestLayout {
    void *player;
    int32_t current_state;
    int32_t previous_state;
    float transition_time;
    float transition_duration;
    int8_t transitioning;
    int8_t additive;
    float weight;
    int32_t mask_root_bone;
    int32_t mask_bone_count_seen;
    uint8_t mask_bits[1024]; /* VGFX3D_MAX_SKELETON_BONES */
};

struct AnimController3DTestLayout {
    void *vptr;
    void *skeleton;
    void *states;
    int32_t state_count;
    int32_t state_capacity;
    uint64_t *state_name_hashes;
    int32_t *state_name_indices;
    int32_t state_name_index_capacity;
    int8_t state_name_index_dirty;
    void *transitions;
    int32_t transition_count;
    int32_t transition_capacity;
    void *events;
    int32_t event_count;
    int32_t event_capacity;
    AnimController3DLayerTestLayout layers[4];
    void *blend_tree;
    void *ik_solvers[4];
    int32_t ik_solver_count;
    float *final_palette;
    float *final_globals;
    float *prev_final_palette;
    int8_t has_prev_final_palette;
    double root_motion_delta[3];
    double root_motion_rotation[4];
    double animation_lod_distance;
    double animation_lod_rate_hz;
    double animation_lod_accum;
    int32_t animation_lod_max_bones;
    int32_t root_motion_bone;
};

struct BlendTree3DSampleTest {
    double x;
    double y;
    int64_t blend_index;
};

struct BlendTree3DTestLayout {
    void *vptr;
    void *blend;
    int32_t dimensions;
    int32_t sample_count;
    double param_x;
    double param_y;
    BlendTree3DSampleTest samples[16];
    int32_t blend_mode_2d;
    int32_t tris[64 * 3];
    int32_t tri_count;
    int8_t tris_dirty;
};

struct IKSolver3DTestPrefix {
    void *vptr;
    void *skeleton;
    int32_t kind;
    int32_t chain_count;
    int32_t chain[32];
    float target[3];
    float pole[3];
    int8_t has_pole;
    float ground_normal[3];
    int8_t has_ground_normal;
    float weight;
    float *solved_locals;
    float *solved_globals;
    float *owned_solved_locals;
    float *owned_solved_globals;
    int32_t pose_bone_capacity;
    int32_t chain_bone_capacity;
};

static void fill_identity_pose(float *pose, int32_t bone_count) {
    for (int32_t bone = 0; bone < bone_count; bone++) {
        float *m = &pose[bone * 16];
        memset(m, 0, 16 * sizeof(float));
        m[0] = 1.0f;
        m[5] = 1.0f;
        m[10] = 1.0f;
        m[15] = 1.0f;
    }
}

static void expect_retained_probe_untouched(void *probe, const char *msg) {
    EXPECT_TRUE(rt_obj_release_check0(probe) == 0, msg);
    if (rt_obj_release_check0(probe))
        rt_obj_free(probe);
}

static void *make_anim(const char *name,
                       int64_t bone_index,
                       double x0,
                       double y0,
                       double z0,
                       double x1,
                       double y1,
                       double z1) {
    void *anim = rt_animation3d_new(rt_const_cstr(name), 1.0);
    void *pos0 = rt_vec3_new(x0, y0, z0);
    void *pos1 = rt_vec3_new(x1, y1, z1);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_set_looping(anim, 1);
    rt_animation3d_add_keyframe(anim, bone_index, 0.0, pos0, rot, scl);
    rt_animation3d_add_keyframe(anim, bone_index, 1.0, pos1, rot, scl);
    return anim;
}

static void test_controller_state_flow() {
    void *skel = rt_skeleton3d_new();
    void *controller;
    void *idle;
    void *walk;
    void *delta;
    void *root_mat;
    const char *event_name;

    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_add_bone(skel, rt_const_cstr("arm"), 0, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    idle = make_anim("idle", 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    walk = make_anim("walk", 0, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0);

    controller = rt_anim_controller3d_new(skel);
    EXPECT_TRUE(controller != nullptr, "AnimController3D.New returns non-null");
    EXPECT_TRUE(rt_anim_controller3d_add_state(controller, rt_const_cstr("idle"), idle) == 0,
                "AddState idle returns index 0");
    EXPECT_TRUE(rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), walk) == 1,
                "AddState walk returns index 1");
    EXPECT_TRUE(rt_anim_controller3d_add_transition(
                    controller, rt_const_cstr("idle"), rt_const_cstr("walk"), 0.25) == 1,
                "AddTransition idle->walk succeeds");
    rt_anim_controller3d_add_event(controller, rt_const_cstr("walk"), 0.5, rt_const_cstr("step"));

    EXPECT_TRUE(rt_anim_controller3d_play(controller, rt_const_cstr("idle")) == 1,
                "Play idle succeeds");
    EXPECT_TRUE(
        strcmp(rt_string_cstr(rt_anim_controller3d_get_current_state(controller)), "idle") == 0,
        "CurrentState = idle");

    EXPECT_TRUE(rt_anim_controller3d_play(controller, rt_const_cstr("walk")) == 1,
                "Play walk succeeds");
    EXPECT_TRUE(rt_anim_controller3d_get_is_transitioning(controller) == 1,
                "Play walk uses default transition");
    EXPECT_TRUE(
        strcmp(rt_string_cstr(rt_anim_controller3d_get_previous_state(controller)), "idle") == 0,
        "PreviousState = idle");

    rt_anim_controller3d_set_root_motion_bone(controller, 0);
    rt_anim_controller3d_update(controller, 0.5);
    root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_TRUE(root_mat != nullptr, "GetBoneMatrix(root) returns matrix");
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3), 5.0, 0.1, "Root motion sampled at x=5");

    delta = rt_anim_controller3d_consume_root_motion(controller);
    EXPECT_NEAR(rt_vec3_x(delta), 5.0, 0.1, "ConsumeRootMotion returns root x delta");
    EXPECT_NEAR(rt_vec3_y(delta), 0.0, 0.01, "ConsumeRootMotion returns root y delta");

    delta = rt_anim_controller3d_get_root_motion_delta(controller);
    EXPECT_NEAR(rt_vec3_x(delta), 0.0, 0.01, "RootMotionDelta clears after consume");

    event_name = rt_string_cstr(rt_anim_controller3d_poll_event(controller));
    EXPECT_TRUE(event_name && strcmp(event_name, "step") == 0, "PollEvent returns queued step");
    EXPECT_TRUE(strcmp(rt_string_cstr(rt_anim_controller3d_poll_event(controller)), "") == 0,
                "PollEvent returns empty string when queue drained");
}

static void test_controller_root_motion_disabled_and_loop_wrap() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *walk = make_anim("walk", 0, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), walk);

    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_update(controller, 0.5);
    void *delta = rt_anim_controller3d_consume_root_motion(controller);
    EXPECT_NEAR(rt_vec3_x(delta), 0.0, 0.01, "Root motion is disabled by default");

    rt_anim_controller3d_set_root_motion_bone(controller, 0);
    rt_anim_controller3d_update(controller, 0.4);
    delta = rt_anim_controller3d_consume_root_motion(controller);
    EXPECT_NEAR(rt_vec3_x(delta), 4.0, 0.1, "Root motion accumulates after enabling a bone");

    rt_anim_controller3d_update(controller, 0.2);
    delta = rt_anim_controller3d_consume_root_motion(controller);
    EXPECT_NEAR(rt_vec3_x(delta), 2.0, 0.1, "Looping root motion preserves forward wrap delta");

    rt_anim_controller3d_set_root_motion_bone(controller, -1);
    rt_anim_controller3d_update(controller, 0.25);
    delta = rt_anim_controller3d_consume_root_motion(controller);
    EXPECT_NEAR(rt_vec3_x(delta), 0.0, 0.01, "SetRootMotionBone(-1) disables root motion");
}

static void test_controller_crossfade_preserves_source_speed() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *run = make_anim("run", 0, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0);
    void *idle = make_anim("idle", 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("run"), run);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("idle"), idle);
    rt_anim_controller3d_set_state_speed(controller, rt_const_cstr("run"), 2.0);
    rt_anim_controller3d_set_state_speed(controller, rt_const_cstr("idle"), 0.0);
    rt_anim_controller3d_set_state_looping(controller, rt_const_cstr("run"), 0);

    rt_anim_controller3d_play(controller, rt_const_cstr("run"));
    rt_anim_controller3d_update(controller, 0.25);
    rt_anim_controller3d_crossfade(controller, rt_const_cstr("idle"), 1.0);
    rt_anim_controller3d_update(controller, 0.25);

    void *root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3),
                7.5,
                0.2,
                "Crossfade source clip continues with source state speed");
}

static void test_controller_masked_layer() {
    void *skel = rt_skeleton3d_new();
    void *controller;
    void *walk;
    void *wave;
    void *root_mat;
    void *arm_mat;
    int64_t arm_bone;

    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    arm_bone = rt_skeleton3d_add_bone(skel, rt_const_cstr("arm"), 0, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    walk = make_anim("walk", 0, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0);
    wave = make_anim("wave", arm_bone, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0);

    controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), walk);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("wave"), wave);
    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_set_layer_mask(controller, 1, arm_bone);
    rt_anim_controller3d_set_layer_weight(controller, 1, 1.0);
    EXPECT_TRUE(rt_anim_controller3d_play_layer(controller, 1, rt_const_cstr("wave")) == 1,
                "PlayLayer(layer=1,wave) succeeds");

    rt_anim_controller3d_update(controller, 0.5);
    root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    arm_mat = rt_anim_controller3d_get_bone_matrix(controller, arm_bone);

    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3), 5.0, 0.1, "Masked layer preserves base root x");
    EXPECT_NEAR(rt_mat4_get(root_mat, 1, 3), 0.0, 0.01, "Masked layer does not affect root y");
    EXPECT_NEAR(rt_mat4_get(arm_mat, 0, 3),
                5.0,
                0.1,
                "Masked layer keeps base parent motion on child world matrix");
    EXPECT_NEAR(rt_mat4_get(arm_mat, 1, 3), 1.0, 0.1, "Masked layer drives arm y");

    rt_anim_controller3d_set_layer_weight(controller, 1, NAN);
    rt_anim_controller3d_update(controller, 0.1);
    arm_mat = rt_anim_controller3d_get_bone_matrix(controller, arm_bone);
    EXPECT_TRUE(std::isfinite(rt_mat4_get(arm_mat, 1, 3)),
                "AnimController3D converts NaN layer weights to finite output");

    rt_anim_controller3d_stop_layer(controller, 1);
    EXPECT_TRUE(rt_anim_controller3d_crossfade_layer(controller, 1, rt_const_cstr("wave"), 0.2) ==
                    1,
                "CrossfadeLayer succeeds");
}

static void test_controller_true_additive_layer_uses_bind_pose_delta() {
    void *skel_replace = rt_skeleton3d_new();
    void *skel_additive = rt_skeleton3d_new();
    int64_t replace_arm;
    int64_t additive_arm;
    void *replace_controller;
    void *additive_controller;
    void *base_replace;
    void *raise_replace;
    void *base_additive;
    void *raise_additive;
    void *reach_additive;
    void *replace_arm_mat;
    void *additive_arm_mat;

    rt_skeleton3d_add_bone(skel_replace, rt_const_cstr("root"), -1, rt_mat4_identity());
    replace_arm = rt_skeleton3d_add_bone(
        skel_replace, rt_const_cstr("arm"), 0, rt_mat4_translate(0.0, 1.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel_replace);

    rt_skeleton3d_add_bone(skel_additive, rt_const_cstr("root"), -1, rt_mat4_identity());
    additive_arm = rt_skeleton3d_add_bone(
        skel_additive, rt_const_cstr("arm"), 0, rt_mat4_translate(0.0, 1.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel_additive);

    base_replace = make_anim("base", replace_arm, 0.0, 2.0, 0.0, 0.0, 2.0, 0.0);
    raise_replace = make_anim("raise", replace_arm, 0.0, 3.0, 0.0, 0.0, 3.0, 0.0);
    base_additive = make_anim("base", additive_arm, 0.0, 2.0, 0.0, 0.0, 2.0, 0.0);
    raise_additive = make_anim("raise", additive_arm, 0.0, 3.0, 0.0, 0.0, 3.0, 0.0);
    reach_additive = make_anim("reach", additive_arm, 0.0, 5.0, 0.0, 0.0, 5.0, 0.0);

    replace_controller = rt_anim_controller3d_new(skel_replace);
    rt_anim_controller3d_add_state(replace_controller, rt_const_cstr("base"), base_replace);
    rt_anim_controller3d_add_state(replace_controller, rt_const_cstr("raise"), raise_replace);
    rt_anim_controller3d_play(replace_controller, rt_const_cstr("base"));
    rt_anim_controller3d_set_layer_mask(replace_controller, 1, replace_arm);
    rt_anim_controller3d_set_layer_weight(replace_controller, 1, 1.0);
    EXPECT_TRUE(rt_anim_controller3d_play_layer(replace_controller, 1, rt_const_cstr("raise")) == 1,
                "PlayLayer remains a masked replace overlay");
    rt_anim_controller3d_update(replace_controller, 0.0);
    replace_arm_mat = rt_anim_controller3d_get_bone_matrix(replace_controller, replace_arm);

    additive_controller = rt_anim_controller3d_new(skel_additive);
    rt_anim_controller3d_add_state(additive_controller, rt_const_cstr("base"), base_additive);
    rt_anim_controller3d_add_state(additive_controller, rt_const_cstr("raise"), raise_additive);
    rt_anim_controller3d_add_state(additive_controller, rt_const_cstr("reach"), reach_additive);
    rt_anim_controller3d_play(additive_controller, rt_const_cstr("base"));
    rt_anim_controller3d_set_layer_mask(additive_controller, 1, additive_arm);
    rt_anim_controller3d_set_layer_weight(additive_controller, 1, 1.0);
    EXPECT_TRUE(rt_anim_controller3d_play_layer_additive(
                    additive_controller, 1, rt_const_cstr("raise")) == 1,
                "PlayLayerAdditive enables true bind-pose delta composition");
    EXPECT_TRUE(rt_anim_controller3d_play_layer_additive(
                    additive_controller, 0, rt_const_cstr("raise")) == 0,
                "PlayLayerAdditive rejects the base layer");
    rt_anim_controller3d_update(additive_controller, 0.0);
    additive_arm_mat = rt_anim_controller3d_get_bone_matrix(additive_controller, additive_arm);

    EXPECT_NEAR(
        rt_mat4_get(replace_arm_mat, 1, 3), 3.0, 0.1, "PlayLayer replaces the masked local pose");
    EXPECT_NEAR(rt_mat4_get(additive_arm_mat, 1, 3),
                4.0,
                0.1,
                "PlayLayerAdditive adds overlay minus bind pose onto the base pose");
    EXPECT_TRUE(rt_anim_controller3d_crossfade_layer_additive(
                    additive_controller, 0, rt_const_cstr("reach"), 1.0) == 0,
                "CrossfadeLayerAdditive rejects the base layer");
    EXPECT_TRUE(rt_anim_controller3d_crossfade_layer_additive(
                    additive_controller, 1, rt_const_cstr("reach"), 1.0) == 1,
                "CrossfadeLayerAdditive blends true additive overlay layers");
    rt_anim_controller3d_update(additive_controller, 0.5);
    additive_arm_mat = rt_anim_controller3d_get_bone_matrix(additive_controller, additive_arm);
    EXPECT_NEAR(rt_mat4_get(additive_arm_mat, 1, 3),
                5.0,
                0.15,
                "CrossfadeLayerAdditive blends halfway between additive deltas");
    rt_anim_controller3d_update(additive_controller, 0.5);
    additive_arm_mat = rt_anim_controller3d_get_bone_matrix(additive_controller, additive_arm);
    EXPECT_NEAR(rt_mat4_get(additive_arm_mat, 1, 3),
                6.0,
                0.15,
                "CrossfadeLayerAdditive reaches the target additive delta");
}

static void test_controller_blend_tree_drives_base_pose() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *idle = make_anim("idle", 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    void *lean = make_anim("lean", 0, 10.0, 0.0, 0.0, 10.0, 0.0, 0.0);
    void *tree = rt_blend_tree3d_new_1d(skel);
    rt_blend_tree3d_add_sample(tree, idle, 0.0, 0.0);
    rt_blend_tree3d_add_sample(tree, lean, 1.0, 0.0);
    rt_blend_tree3d_set_param(tree, 0.5, 0.0);

    void *controller = rt_anim_controller3d_new(skel);
    EXPECT_TRUE(rt_anim_controller3d_set_blend_tree(controller, tree) != 0,
                "AnimController3D.SetBlendTree accepts a compatible tree");
    rt_anim_controller3d_update(controller, 0.0);
    void *root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3),
                5.0,
                0.1,
                "AnimController3D.SetBlendTree drives the base pose from blend weights");

    void *other_skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(other_skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(other_skel);
    void *other_pose = make_anim("other", 0, 20.0, 0.0, 0.0, 20.0, 0.0, 0.0);
    void *other_tree = rt_blend_tree3d_new_1d(other_skel);
    rt_blend_tree3d_add_sample(other_tree, other_pose, 0.0, 0.0);
    EXPECT_TRUE(rt_anim_controller3d_set_blend_tree(controller, other_tree) == 0,
                "AnimController3D.SetBlendTree rejects a tree bound to a different skeleton");
    rt_anim_controller3d_update(controller, 0.0);
    root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3),
                5.0,
                0.1,
                "Rejected BlendTree does not replace the active controller tree");

    EXPECT_TRUE(rt_anim_controller3d_set_blend_tree(controller, skel) == 0,
                "AnimController3D.SetBlendTree rejects non-tree handles");
    EXPECT_TRUE(rt_anim_controller3d_set_blend_tree(controller, nullptr) != 0,
                "AnimController3D.SetBlendTree clears on NULL");
    rt_anim_controller3d_update(controller, 0.0);
    root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3), 0.0, 0.1, "Clearing BlendTree restores bind pose");
}

static void test_controller_play_refreshes_final_pose_immediately() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *pose = make_anim("pose", 0, 7.0, 0.0, 0.0, 7.0, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("pose"), pose);
    EXPECT_TRUE(rt_anim_controller3d_play(controller, rt_const_cstr("pose")) != 0,
                "AnimController3D.Play starts a valid state");
    void *root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3),
                7.0,
                0.1,
                "AnimController3D.Play refreshes the final pose before the next Update");
}

static void test_controller_blend_tree_root_motion_uses_final_pose() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *move = make_anim("move", 0, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0);
    void *tree = rt_blend_tree3d_new_1d(skel);
    rt_blend_tree3d_add_sample(tree, move, 0.0, 0.0);
    rt_blend_tree3d_set_param(tree, 0.0, 0.0);

    void *controller = rt_anim_controller3d_new(skel);
    EXPECT_TRUE(rt_anim_controller3d_set_blend_tree(controller, tree) != 0,
                "AnimController3D.SetBlendTree accepts a moving tree");
    rt_anim_controller3d_set_root_motion_bone(controller, 0);
    rt_anim_controller3d_update(controller, 0.5);
    void *delta = rt_anim_controller3d_consume_root_motion(controller);
    EXPECT_NEAR(rt_vec3_x(delta),
                5.0,
                0.1,
                "AnimController3D root motion follows BlendTree final pose movement");
}

static void test_controller_private_skeleton_growth_stays_in_bounds() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    void *controller = rt_anim_controller3d_new(skel);
    void *tree = rt_blend_tree3d_new_1d(skel);
    EXPECT_TRUE(controller != nullptr, "AnimController3D growth fixture creates controller");
    EXPECT_TRUE(tree != nullptr, "BlendTree3D growth fixture creates blend tree before growth");
    if (!controller || !tree)
        return;

    auto *skel_view = static_cast<rt_skeleton3d *>(skel);
    auto *grown =
        static_cast<vgfx3d_bone_t *>(std::realloc(skel_view->bones, 2 * sizeof(vgfx3d_bone_t)));
    EXPECT_TRUE(grown != nullptr, "AnimController3D growth fixture extends skeleton storage");
    if (!grown)
        return;
    skel_view->bones = grown;
    skel_view->owned_bones = grown;
    std::memset(&skel_view->bones[1], 0, sizeof(vgfx3d_bone_t));
    skel_view->bones[1].name = rt_const_cstr("late");
    skel_view->bones[1].parent_index = 0;
    fill_identity_pose(skel_view->bones[1].bind_pose_local, 1);
    fill_identity_pose(skel_view->bones[1].inverse_bind, 1);
    skel_view->bone_capacity = 2;
    skel_view->bone_count = 2;
    skel_view->owned_bone_capacity = 2;
    skel_view->initialized_bone_count = 2;

    void *late = make_anim("late", 1, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0);
    EXPECT_TRUE(rt_anim_controller3d_add_state(controller, rt_const_cstr("late"), late) == 0,
                "AnimController3D accepts an animation for a late private bone");
    EXPECT_TRUE(rt_anim_controller3d_play(controller, rt_const_cstr("late")) == 1,
                "AnimController3D plays the late-bone animation");
    rt_anim_controller3d_update(controller, 0.5);

    void *late_mat = rt_anim_controller3d_get_bone_matrix(controller, 1);
    EXPECT_TRUE(late_mat != nullptr, "AnimController3D returns the late bone matrix");
    EXPECT_NEAR(rt_mat4_get(late_mat, 1, 3),
                1.0,
                0.1,
                "AnimController3D updates palettes within capacity after skeleton growth");

    EXPECT_TRUE(rt_blend_tree3d_add_sample(tree, late, 0.0, 0.0) == 0,
                "BlendTree3D accepts an animation for a late private bone");
    rt_blend_tree3d_update(tree, 0.5);
    int32_t blend_bones = 0;
    const float *locals =
        rt_anim_blend3d_get_local_transform_data(rt_blend_tree3d_get_blend(tree), &blend_bones);
    EXPECT_TRUE(locals != nullptr && blend_bones == 2,
                "BlendTree3D reports the grown skeleton bone count");
    EXPECT_NEAR(locals ? locals[1 * 16 + 7] : 0.0f,
                1.0,
                0.1,
                "BlendTree3D updates blend buffers within capacity after skeleton growth");
}

static void test_skeletal_state_registration_rejects_out_of_range_clip_bones() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *bad = make_anim("bad", 1, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    void *blend = rt_anim_blend3d_new(skel);
    void *tree = rt_blend_tree3d_new_1d(skel);

    EXPECT_TRUE(rt_anim_controller3d_add_state(controller, rt_const_cstr("bad"), bad) == -1,
                "AnimController3D rejects clips targeting bones outside its skeleton");
    EXPECT_TRUE(rt_anim_controller3d_get_state_count(controller) == 0,
                "AnimController3D keeps invalid clips out of the state table");
    EXPECT_TRUE(rt_anim_blend3d_add_state(blend, rt_const_cstr("bad"), bad) == -1,
                "AnimBlend3D rejects clips targeting bones outside its skeleton");
    EXPECT_TRUE(rt_anim_blend3d_state_count(blend) == 0,
                "AnimBlend3D keeps invalid clips out of the blend state table");
    EXPECT_TRUE(rt_blend_tree3d_add_sample(tree, bad, 0.0, 0.0) == -1,
                "BlendTree3D rejects samples whose clips cannot drive the bound skeleton");
}

static void test_two_bone_ik_pole_vector() {
    void *skel = rt_skeleton3d_new();
    int64_t root = rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t knee =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("knee"), root, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t foot =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("foot"), knee, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    void *controller = rt_anim_controller3d_new(skel);
    void *solver = rt_ik_solver3d_two_bone(skel, root, knee, foot);
    rt_ik_solver3d_set_target(solver, rt_vec3_new(1.0, 1.0, 0.0));
    rt_ik_solver3d_set_weight(solver, 1.0);

    /* Pole toward +Z swings the bent knee toward +Z. */
    rt_ik_solver3d_set_pole(solver, rt_vec3_new(0.0, 0.0, 1.0));
    rt_ik_solver3d_solve(solver);
    rt_anim_controller3d_set_ik_solver(controller, solver);
    double knee_z_pos = rt_mat4_get(rt_anim_controller3d_get_bone_matrix(controller, knee), 2, 3);
    EXPECT_TRUE(knee_z_pos > 0.1, "TwoBone IK pole +Z bends the knee toward +Z");

    /* Flipping the pole to -Z mirrors the bend. */
    rt_ik_solver3d_set_pole(solver, rt_vec3_new(0.0, 0.0, -1.0));
    rt_ik_solver3d_solve(solver);
    rt_anim_controller3d_set_ik_solver(controller, solver);
    double knee_z_neg = rt_mat4_get(rt_anim_controller3d_get_bone_matrix(controller, knee), 2, 3);
    EXPECT_TRUE(knee_z_neg < -0.1, "TwoBone IK pole -Z bends the knee toward -Z");

    /* The pole only changes the bend plane — the foot still reaches the target. */
    void *foot_mat = rt_anim_controller3d_get_bone_matrix(controller, foot);
    EXPECT_NEAR(rt_mat4_get(foot_mat, 0, 3), 1.0, 0.05, "Pole keeps the foot on target x");
    EXPECT_NEAR(rt_mat4_get(foot_mat, 1, 3), 1.0, 0.05, "Pole keeps the foot on target y");
}

/// Regression for translation-only IK: solving must write chain-bone
/// ROTATIONS (aim/swing toward the solved child joint), not just joint
/// translations — skinned vertices follow bone rotations, so without this a
/// bent limb shears its mesh instead of articulating.
static void test_two_bone_ik_bends_bone_rotations() {
    void *skel = rt_skeleton3d_new();
    int64_t root = rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t knee =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("knee"), root, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t foot =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("foot"), knee, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    void *controller = rt_anim_controller3d_new(skel);
    void *solver = rt_ik_solver3d_two_bone(skel, root, knee, foot);
    rt_ik_solver3d_set_target(solver, rt_vec3_new(1.0, 1.0, 0.0));
    rt_ik_solver3d_set_weight(solver, 1.0);
    rt_ik_solver3d_solve(solver);
    rt_anim_controller3d_set_ik_solver(controller, solver);

    void *root_mat = rt_anim_controller3d_get_bone_matrix(controller, root);
    void *knee_mat = rt_anim_controller3d_get_bone_matrix(controller, knee);
    void *foot_mat = rt_anim_controller3d_get_bone_matrix(controller, foot);

    /* Root bone's local +X (its bind-pose child direction) must now AIM at
     * the solved knee joint, i.e. the root carries a real rotation. */
    double ax = rt_mat4_get(root_mat, 0, 0);
    double ay = rt_mat4_get(root_mat, 1, 0);
    double az = rt_mat4_get(root_mat, 2, 0);
    double kx = rt_mat4_get(knee_mat, 0, 3) - rt_mat4_get(root_mat, 0, 3);
    double ky = rt_mat4_get(knee_mat, 1, 3) - rt_mat4_get(root_mat, 1, 3);
    double kz = rt_mat4_get(knee_mat, 2, 3) - rt_mat4_get(root_mat, 2, 3);
    double alen = std::sqrt(ax * ax + ay * ay + az * az);
    double klen = std::sqrt(kx * kx + ky * ky + kz * kz);
    EXPECT_TRUE(alen > 1e-6 && klen > 1e-6,
                "IK bend: root axis and knee offset are non-degenerate");
    double align = (ax * kx + ay * ky + az * kz) / (alen * klen);
    EXPECT_TRUE(align > 0.99, "IK writes a root rotation that aims at the solved knee");

    /* Knee bone's local +X must aim at the foot joint the same way. */
    double bx = rt_mat4_get(knee_mat, 0, 0);
    double by = rt_mat4_get(knee_mat, 1, 0);
    double bz = rt_mat4_get(knee_mat, 2, 0);
    double fx = rt_mat4_get(foot_mat, 0, 3) - rt_mat4_get(knee_mat, 0, 3);
    double fy = rt_mat4_get(foot_mat, 1, 3) - rt_mat4_get(knee_mat, 1, 3);
    double fz = rt_mat4_get(foot_mat, 2, 3) - rt_mat4_get(knee_mat, 2, 3);
    double blen = std::sqrt(bx * bx + by * by + bz * bz);
    double flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    EXPECT_TRUE(blen > 1e-6 && flen > 1e-6,
                "IK bend: knee axis and foot offset are non-degenerate");
    double align_knee = (bx * fx + by * fy + bz * fz) / (blen * flen);
    EXPECT_TRUE(align_knee > 0.99, "IK writes a knee rotation that aims at the solved foot");
    /* The knee->foot segment leaves the bind axis (+X), so the knee bone must
     * carry a REAL rotation — translation-only IK left it at bind (by == 0). */
    EXPECT_TRUE(std::fabs(by) > 0.5, "IK knee rotation is a genuine bend (not bind pose)");

    /* And the end effector still reaches the target. */
    EXPECT_NEAR(rt_mat4_get(foot_mat, 0, 3), 1.0, 0.05, "IK bend keeps the foot on target x");
    EXPECT_NEAR(rt_mat4_get(foot_mat, 1, 3), 1.0, 0.05, "IK bend keeps the foot on target y");
}

static void test_controller_ik_solver_drives_end_effector() {
    void *skel = rt_skeleton3d_new();
    int64_t root = rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t knee =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("knee"), root, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t foot =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("foot"), knee, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    void *controller = rt_anim_controller3d_new(skel);
    void *solver = rt_ik_solver3d_two_bone(skel, root, knee, foot);
    EXPECT_TRUE(solver != nullptr, "IKSolver3D.TwoBone creates a parented solver");
    rt_ik_solver3d_set_target(solver, rt_vec3_new(1.0, 1.0, 0.0));
    rt_ik_solver3d_set_weight(solver, 1.0);
    rt_ik_solver3d_solve(solver);

    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, solver) == 1,
                "AnimController3D.SetIKSolver accepts a compatible solver");
    void *foot_mat = rt_anim_controller3d_get_bone_matrix(controller, foot);
    EXPECT_NEAR(rt_mat4_get(foot_mat, 0, 3), 1.0, 0.03, "TwoBone IK reaches target x");
    EXPECT_NEAR(rt_mat4_get(foot_mat, 1, 3), 1.0, 0.03, "TwoBone IK reaches target y");
    EXPECT_NEAR(rt_mat4_get(foot_mat, 2, 3), 0.0, 0.03, "TwoBone IK preserves target z");

    rt_ik_solver3d_set_weight(solver, 0.5);
    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, solver) == 1,
                "AnimController3D.SetIKSolver accepts weight changes on the same solver");
    foot_mat = rt_anim_controller3d_get_bone_matrix(controller, foot);
    EXPECT_NEAR(rt_mat4_get(foot_mat, 0, 3), 1.5, 0.05, "IK weight blends end-effector x");
    EXPECT_NEAR(rt_mat4_get(foot_mat, 1, 3), 0.5, 0.05, "IK weight blends end-effector y");

    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, skel) == 0,
                "AnimController3D.SetIKSolver rejects non-solver handles");
    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, nullptr) == 1,
                "AnimController3D.SetIKSolver clears with NULL");
    foot_mat = rt_anim_controller3d_get_bone_matrix(controller, foot);
    EXPECT_NEAR(rt_mat4_get(foot_mat, 0, 3), 2.0, 0.01, "Clearing IK restores bind-pose foot x");
    EXPECT_NEAR(rt_mat4_get(foot_mat, 1, 3), 0.0, 0.01, "Clearing IK restores bind-pose foot y");
}

static void test_controller_ordered_ik_solver_stack() {
    void *skel = rt_skeleton3d_new();
    int64_t root =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t lu = rt_skeleton3d_add_bone(
        skel, rt_const_cstr("left_upper"), root, rt_mat4_translate(-1.0, 0.0, 0.0));
    int64_t ll = rt_skeleton3d_add_bone(
        skel, rt_const_cstr("left_lower"), lu, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t lh = rt_skeleton3d_add_bone(
        skel, rt_const_cstr("left_hand"), ll, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t ru = rt_skeleton3d_add_bone(
        skel, rt_const_cstr("right_upper"), root, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t rl = rt_skeleton3d_add_bone(
        skel, rt_const_cstr("right_lower"), ru, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t rh = rt_skeleton3d_add_bone(
        skel, rt_const_cstr("right_hand"), rl, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    void *controller = rt_anim_controller3d_new(skel);
    void *left = rt_ik_solver3d_two_bone(skel, lu, ll, lh);
    void *right = rt_ik_solver3d_two_bone(skel, ru, rl, rh);
    rt_ik_solver3d_set_target(left, rt_vec3_new(-1.0, 1.0, 0.0));
    rt_ik_solver3d_set_target(right, rt_vec3_new(1.0, 1.0, 0.0));
    rt_ik_solver3d_set_weight(left, 1.0);
    rt_ik_solver3d_set_weight(right, 1.0);

    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, left) == 1,
                "SetIKSolver installs the first ordered constraint");
    EXPECT_TRUE(rt_anim_controller3d_add_ik_solver(controller, right) == 1,
                "AddIKSolver appends a compatible independent constraint");
    EXPECT_TRUE(rt_anim_controller3d_add_ik_solver(controller, right) == 1,
                "AddIKSolver is idempotent for an existing solver");
    void *lm = rt_anim_controller3d_get_bone_matrix(controller, lh);
    void *rm = rt_anim_controller3d_get_bone_matrix(controller, rh);
    EXPECT_NEAR(rt_mat4_get(lm, 0, 3), -1.0, 0.05,
                "ordered IK stack solves the left hand x");
    EXPECT_NEAR(rt_mat4_get(lm, 1, 3), 1.0, 0.05,
                "ordered IK stack solves the left hand y");
    EXPECT_NEAR(rt_mat4_get(rm, 0, 3), 1.0, 0.05,
                "ordered IK stack solves the right hand x");
    EXPECT_NEAR(rt_mat4_get(rm, 1, 3), 1.0, 0.05,
                "ordered IK stack solves the right hand y");

    EXPECT_TRUE(rt_anim_controller3d_add_ik_solver(controller, skel) == 0,
                "AddIKSolver rejects wrong-class handles");
    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, nullptr) == 1,
                "SetIKSolver(NULL) clears the whole ordered stack");
    lm = rt_anim_controller3d_get_bone_matrix(controller, lh);
    rm = rt_anim_controller3d_get_bone_matrix(controller, rh);
    EXPECT_NEAR(rt_mat4_get(lm, 0, 3), 1.0, 0.01,
                "clearing the IK stack restores the left bind pose");
    EXPECT_NEAR(rt_mat4_get(rm, 0, 3), 3.0, 0.01,
                "clearing the IK stack restores the right bind pose");
}

static void test_two_bone_ik_foot_aligns_to_ground_normal() {
    /* The knee is given a 90-degree bind rotation so the foot's parent has a non-identity world
     * rotation. That exercises the foot-orientation's local/world conversion: a version that
     * forgot the parent inverse would only align correctly under an identity parent. */
    const double half_pi = 1.5707963267948966;
    void *skel = rt_skeleton3d_new();
    int64_t root = rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    void *knee_bind = rt_mat4_mul(rt_mat4_translate(1.0, 0.0, 0.0), rt_mat4_rotate_z(half_pi));
    int64_t knee = rt_skeleton3d_add_bone(skel, rt_const_cstr("knee"), root, knee_bind);
    int64_t foot =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("foot"), knee, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    void *controller = rt_anim_controller3d_new(skel);
    void *solver = rt_ik_solver3d_two_bone(skel, root, knee, foot);
    rt_ik_solver3d_set_target(solver, rt_vec3_new(1.5, 0.5, 0.0));
    rt_ik_solver3d_set_weight(solver, 1.0);
    /* A slope normal tilted away from world up. */
    double nx = 0.5, ny = 1.0, nz = 0.0;
    double nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
    nx /= nlen;
    ny /= nlen;
    nz /= nlen;
    rt_ik_solver3d_set_ground_normal(solver, rt_vec3_new(nx, ny, nz));
    rt_ik_solver3d_solve(solver);
    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, solver) == 1,
                "AnimController3D accepts the foot-orientation IK solver");

    /* The foot's world up axis (global matrix column 1) should align with the ground normal,
     * regardless of the rotated knee parent. */
    void *foot_mat = rt_anim_controller3d_get_bone_matrix(controller, foot);
    EXPECT_NEAR(rt_mat4_get(foot_mat, 0, 1), nx, 0.05, "Foot IK aligns foot up.x to ground normal");
    EXPECT_NEAR(rt_mat4_get(foot_mat, 1, 1), ny, 0.05, "Foot IK aligns foot up.y to ground normal");
    EXPECT_NEAR(rt_mat4_get(foot_mat, 2, 1), nz, 0.05, "Foot IK aligns foot up.z to ground normal");
}

static void test_ik_solver_look_at_and_fabrik_factories() {
    void *look_skel = rt_skeleton3d_new();
    int64_t look_bone =
        rt_skeleton3d_add_bone(look_skel, rt_const_cstr("head"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(look_skel);
    void *look_controller = rt_anim_controller3d_new(look_skel);
    void *look_solver = rt_ik_solver3d_look_at(look_skel, look_bone);
    EXPECT_TRUE(look_solver != nullptr, "IKSolver3D.LookAt creates a solver");
    rt_ik_solver3d_set_target(look_solver, rt_vec3_new(1.0, 0.0, 0.0));
    rt_ik_solver3d_set_weight(look_solver, 1.0);
    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(look_controller, look_solver) == 1,
                "AnimController3D.SetIKSolver accepts LookAt solvers");
    void *head_mat = rt_anim_controller3d_get_bone_matrix(look_controller, look_bone);
    EXPECT_NEAR(rt_mat4_get(head_mat, 0, 2), 1.0, 0.03, "LookAt rotates local +Z toward target x");
    EXPECT_NEAR(rt_mat4_get(head_mat, 1, 2), 0.0, 0.03, "LookAt keeps target y near zero");

    void *skel = rt_skeleton3d_new();
    int64_t b0 = rt_skeleton3d_add_bone(skel, rt_const_cstr("b0"), -1, rt_mat4_identity());
    int64_t b1 =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("b1"), b0, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t b2 =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("b2"), b1, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t b3 =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("b3"), b2, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);
    void *chain = rt_seq_new_owned();
    rt_seq_push(chain, rt_box_i64(b0));
    rt_seq_push(chain, rt_box_i64(b1));
    rt_seq_push(chain, rt_box_i64(b2));
    rt_seq_push(chain, rt_box_i64(b3));
    void *fabrik = rt_ik_solver3d_fabrik(skel, chain);
    EXPECT_TRUE(fabrik != nullptr, "IKSolver3D.FABRIK creates a chain solver");
    rt_ik_solver3d_set_target(fabrik, rt_vec3_new(0.0, 2.0, 0.0));
    void *controller = rt_anim_controller3d_new(skel);
    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, fabrik) == 1,
                "AnimController3D.SetIKSolver accepts FABRIK solvers");
    void *end_mat = rt_anim_controller3d_get_bone_matrix(controller, b3);
    EXPECT_NEAR(rt_mat4_get(end_mat, 0, 3), 0.0, 0.08, "FABRIK reaches target x");
    EXPECT_NEAR(rt_mat4_get(end_mat, 1, 3), 2.0, 0.08, "FABRIK reaches target y");

    auto *fabrik_layout = reinterpret_cast<IKSolver3DTestPrefix *>(fabrik);
    float fabrik_locals[4 * 16];
    float fabrik_globals[4 * 16];
    fill_identity_pose(fabrik_locals, 4);
    fill_identity_pose(fabrik_globals, 4);
    fabrik_layout->chain_count = 3;
    EXPECT_TRUE(rt_ik_solver3d_apply_to_pose(fabrik, fabrik_locals, fabrik_globals, 4) == 0,
                "IKSolver3D rejects a shortened FABRIK chain instead of changing algorithms");
    fabrik_layout->chain_count = fabrik_layout->chain_bone_capacity;

    EXPECT_TRUE(rt_ik_solver3d_two_bone(skel, b0, b2, b1) == nullptr,
                "IKSolver3D.TwoBone rejects non-parented chains");
}

static void test_ik_solver_repairs_parent_space_inputs_and_owned_pose_storage() {
    const double half_pi = 1.5707963267948966;
    void *skel = rt_skeleton3d_new();
    int64_t root =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_rotate_y(half_pi));
    int64_t aim = rt_skeleton3d_add_bone(skel, rt_const_cstr("aim"), root, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    void *controller = rt_anim_controller3d_new(skel);
    void *solver = rt_ik_solver3d_look_at(skel, aim);
    rt_ik_solver3d_set_target(solver, rt_vec3_new(0.0, 0.0, 10.0));
    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, solver) == 1,
                "IKSolver3D parent-space fixture attaches to the controller");
    void *aim_matrix = rt_anim_controller3d_get_bone_matrix(controller, aim);
    EXPECT_NEAR(rt_mat4_get(aim_matrix, 0, 2),
                0.0,
                0.03,
                "LookAt removes a rotated parent's X-facing contribution");
    EXPECT_NEAR(rt_mat4_get(aim_matrix, 2, 2),
                1.0,
                0.03,
                "LookAt converts its desired global rotation into parent-local space");

    EXPECT_TRUE(rt_ik_solver3d_fabrik(skel, skel) == nullptr,
                "IKSolver3D.FABRIK rejects a non-Seq chain without trapping");
    void *bad_chain = rt_seq_new_owned();
    rt_seq_push(bad_chain, rt_box_f64(0.0));
    rt_seq_push(bad_chain, rt_box_i64(1));
    EXPECT_TRUE(rt_ik_solver3d_fabrik(skel, bad_chain) == nullptr,
                "IKSolver3D.FABRIK rejects non-Integer chain entries without trapping");
    auto *bad_chain_layout = reinterpret_cast<rt_seq_impl *>(bad_chain);
    int64_t saved_capacity = bad_chain_layout->cap;
    bad_chain_layout->cap = 1;
    EXPECT_TRUE(rt_ik_solver3d_fabrik(skel, bad_chain) == nullptr,
                "IKSolver3D.FABRIK rejects inconsistent Seq bounds without indexing storage");
    bad_chain_layout->cap = saved_capacity;

    auto *layout = reinterpret_cast<IKSolver3DTestPrefix *>(solver);
    float foreign_locals[16];
    float foreign_globals[16];
    fill_identity_pose(foreign_locals, 1);
    fill_identity_pose(foreign_globals, 1);
    foreign_locals[3] = 123.0f;
    foreign_globals[7] = 456.0f;
    float *owned_locals = layout->owned_solved_locals;
    float *owned_globals = layout->owned_solved_globals;
    layout->solved_locals = foreign_locals;
    layout->solved_globals = foreign_globals;
    layout->target[0] = std::numeric_limits<float>::infinity();
    layout->pole[1] = std::numeric_limits<float>::quiet_NaN();
    layout->ground_normal[0] = 0.0f;
    layout->ground_normal[1] = 0.0f;
    layout->ground_normal[2] = 0.0f;
    layout->has_pole = -7;
    layout->has_ground_normal = 42;
    layout->weight = std::numeric_limits<float>::infinity();
    rt_ik_solver3d_solve(solver);
    EXPECT_TRUE(layout->solved_locals == owned_locals && layout->solved_globals == owned_globals,
                "IKSolver3D restores both pose mirrors from owned allocation identities");
    EXPECT_NEAR(foreign_locals[3],
                123.0,
                0.0,
                "IKSolver3D never writes through a corrupt local-pose mirror");
    EXPECT_NEAR(foreign_globals[7],
                456.0,
                0.0,
                "IKSolver3D never writes through a corrupt global-pose mirror");
    EXPECT_TRUE(std::isfinite(layout->target[0]) && std::isfinite(layout->pole[1]) &&
                    layout->has_pole == 1 && layout->has_ground_normal == 1,
                "IKSolver3D canonicalizes retained goal lanes and optional-goal flags");
    EXPECT_NEAR(layout->ground_normal[1],
                1.0,
                1e-6,
                "IKSolver3D repairs a degenerate retained ground normal to world up");
    EXPECT_NEAR(layout->weight, 0.0, 0.0, "IKSolver3D repairs a nonfinite retained weight");

    float locals[2 * 16];
    float globals[2 * 16];
    fill_identity_pose(locals, 2);
    fill_identity_pose(globals, 2);
    layout->kind = INT32_MAX;
    EXPECT_TRUE(rt_ik_solver3d_apply_to_pose(solver, locals, globals, 2) == 0,
                "IKSolver3D rejects a corrupted algorithm kind instead of falling through");

    void *finalized_solver = rt_ik_solver3d_look_at(skel, aim);
    auto *finalized_layout = reinterpret_cast<IKSolver3DTestPrefix *>(finalized_solver);
    finalized_layout->solved_locals = foreign_locals;
    finalized_layout->solved_globals = foreign_globals;
    if (rt_obj_release_check0(finalized_solver))
        rt_obj_free(finalized_solver);
    EXPECT_NEAR(foreign_locals[3],
                123.0,
                0.0,
                "IKSolver3D finalization frees owned identities, not corrupt mirrors");
}

static void test_controller_animation_lod_throttles_updates_deterministically() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *walk = make_anim("walk", 0, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), walk);
    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_set_animation_lod(controller, 50.0, 2.0);

    for (int i = 0; i < 4; i++)
        rt_anim_controller3d_update(controller, 0.1);
    void *root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3),
                0.0,
                0.01,
                "SetAnimationLOD holds pose until the configured update interval elapses");
    EXPECT_NEAR(rt_anim_controller3d_get_state_time(controller),
                0.0,
                0.01,
                "SetAnimationLOD defers state time while accumulating sub-interval deltas");

    rt_anim_controller3d_update(controller, 0.1);
    root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3),
                5.0,
                0.1,
                "SetAnimationLOD applies accumulated time at the target update rate");
    EXPECT_NEAR(rt_anim_controller3d_get_state_time(controller),
                0.5,
                0.01,
                "SetAnimationLOD advances state time by the accumulated delta");

    rt_anim_controller3d_set_animation_lod(controller, 0.0, 0.0);
    rt_anim_controller3d_update(controller, 0.1);
    root_mat = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3),
                6.0,
                0.1,
                "SetAnimationLOD disables throttling for non-positive inputs");
}

static void test_controller_events_cover_full_loops_and_reverse() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *walk = make_anim("walk", 0, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), walk);
    rt_anim_controller3d_add_event(controller, rt_const_cstr("walk"), 0.5, rt_const_cstr("step"));

    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_update(controller, 1.0);
    EXPECT_TRUE(strcmp(rt_string_cstr(rt_anim_controller3d_poll_event(controller)), "step") == 0,
                "AnimController3D fires events crossed by an exact full loop");

    rt_anim_controller3d_update(controller, 2.25);
    EXPECT_TRUE(strcmp(rt_string_cstr(rt_anim_controller3d_poll_event(controller)), "step") == 0,
                "AnimController3D fires looping events after multi-loop updates");

    rt_anim_controller3d_set_state_speed(controller, rt_const_cstr("walk"), -1.0);
    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_update(controller, 0.75);
    EXPECT_TRUE(strcmp(rt_string_cstr(rt_anim_controller3d_poll_event(controller)), "step") == 0,
                "AnimController3D fires looping events during reverse wrap");

    rt_anim_controller3d_set_state_speed(controller, rt_const_cstr("walk"), NAN);
    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_update(controller, 0.5);
    EXPECT_TRUE(
        std::isfinite(rt_mat4_get(rt_anim_controller3d_get_bone_matrix(controller, 0), 0, 3)),
        "AnimController3D sanitizes non-finite state speeds");
}

static void test_controller_rejects_wrong_animation_handles() {
    void *skel = rt_skeleton3d_new();
    void *controller;

    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    controller = rt_anim_controller3d_new(skel);

    EXPECT_TRUE(rt_anim_controller3d_add_state(controller, rt_const_cstr("bad"), skel) == -1,
                "AnimController3D.AddState rejects non-Animation3D handles");
    EXPECT_TRUE(rt_anim_controller3d_get_state_count(controller) == 0,
                "Rejected AnimController3D state does not change StateCount");
    rt_animation3d_set_looping(skel, 1);
    EXPECT_TRUE(rt_animation3d_get_looping(skel) == 0,
                "Animation3D getters/setters reject non-Animation3D handles");
}

static void test_controller_rejects_wrong_string_handles() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *move = make_anim("move", 0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    void *wrong_name = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_name);
    rt_string fake_name = reinterpret_cast<rt_string>(wrong_name);

    EXPECT_TRUE(rt_anim_controller3d_add_state(controller, fake_name, move) == -1,
                "AnimController3D.AddState rejects wrong-class string handles");
    EXPECT_TRUE(rt_anim_controller3d_get_state_count(controller) == 0,
                "wrong-class state names are not inserted");
    EXPECT_TRUE(rt_anim_controller3d_add_state(controller, rt_const_cstr("move"), move) == 0,
                "control state fixture inserts a valid state");
    EXPECT_TRUE(rt_anim_controller3d_play(controller, fake_name) == 0,
                "AnimController3D.Play rejects wrong-class string handles");
    EXPECT_TRUE(rt_anim_controller3d_crossfade(controller, fake_name, 0.1) == 0,
                "AnimController3D.Crossfade rejects wrong-class string handles");
    EXPECT_TRUE(
        rt_anim_controller3d_add_transition(controller, fake_name, rt_const_cstr("move"), 0.1) == 0,
        "AnimController3D.AddTransition rejects wrong-class source names");
    EXPECT_TRUE(
        rt_anim_controller3d_add_transition(controller, rt_const_cstr("move"), fake_name, 0.1) == 0,
        "AnimController3D.AddTransition rejects wrong-class target names");
    rt_anim_controller3d_add_event(controller, rt_const_cstr("move"), 0.0, fake_name);
    EXPECT_TRUE(rt_anim_controller3d_play(controller, rt_const_cstr("move")) == 1,
                "valid state still plays after rejected fake names");
    EXPECT_TRUE(strcmp(rt_string_cstr(rt_anim_controller3d_poll_event(controller)), "") == 0,
                "wrong-class event names are not queued");
    EXPECT_TRUE(rt_anim_controller3d_is_state_playing(controller, fake_name) == 0,
                "IsStatePlaying rejects wrong-class string handles");
    rt_anim_controller3d_set_state_speed(controller, fake_name, 2.0);
    rt_anim_controller3d_set_state_looping(controller, fake_name, 0);
    expect_retained_probe_untouched(
        wrong_name, "AnimController3D string-name guards do not release wrong-class handles");
}

static void test_controller_long_state_names_use_canonical_lookup() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *clip = make_anim("long", 0, 0.0, 0.0, 0.0, 4.0, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    char long_name[128];
    std::memset(long_name, 'b', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';

    EXPECT_TRUE(rt_anim_controller3d_add_state(controller, rt_const_cstr(long_name), clip) == 0,
                "AnimController3D accepts a long state name");
    EXPECT_TRUE(rt_anim_controller3d_play(controller, rt_const_cstr(long_name)) != 0,
                "AnimController3D.Play canonicalizes long names");
    EXPECT_TRUE(rt_anim_controller3d_is_state_playing(controller, rt_const_cstr(long_name)) != 0,
                "AnimController3D.IsStatePlaying canonicalizes long names");
    rt_anim_controller3d_update(controller, 0.5);
    EXPECT_NEAR(rt_mat4_get(rt_anim_controller3d_get_bone_matrix(controller, 0), 0, 3),
                2.0,
                0.1,
                "AnimController3D long-name state drives animation");
}

static void test_controller_bone_count_lod_freezes_distal_bones() {
    /* 4-bone chain, identity binds: root(0) - b1(1) - b2(2) - foot(3). */
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t b1 = rt_skeleton3d_add_bone(skel, rt_const_cstr("b1"), 0, rt_mat4_identity());
    int64_t b2 = rt_skeleton3d_add_bone(skel, rt_const_cstr("b2"), (int64_t)b1, rt_mat4_identity());
    int64_t foot =
        rt_skeleton3d_add_bone(skel, rt_const_cstr("foot"), (int64_t)b2, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    /* LOD off: animating only the foot moves it (local x -> 1 at the t=0.5 midpoint). */
    void *c1 = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(
        c1, rt_const_cstr("kick"), make_anim("kick", foot, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0));
    rt_anim_controller3d_play(c1, rt_const_cstr("kick"));
    rt_anim_controller3d_update(c1, 0.5);
    EXPECT_NEAR(rt_mat4_get(rt_anim_controller3d_get_bone_matrix(c1, foot), 0, 3),
                1.0,
                0.1,
                "Bone LOD off: the distal foot animates");

    /* LOD freezing bones >= foot: the foot stops adding local animation (returns to bind x=0). */
    void *c2 = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(
        c2, rt_const_cstr("kick"), make_anim("kick", foot, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0));
    rt_anim_controller3d_play(c2, rt_const_cstr("kick"));
    rt_anim_controller3d_set_bone_lod(c2, foot);
    rt_anim_controller3d_update(c2, 0.5);
    EXPECT_NEAR(rt_mat4_get(rt_anim_controller3d_get_bone_matrix(c2, foot), 0, 3),
                0.0,
                0.05,
                "Bone LOD freezes the distal foot to its bind-pose local");

    /* Frozen bones still follow animated ancestors: animate b1, freeze from b2 on, foot follows. */
    void *c3 = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(
        c3, rt_const_cstr("shift"), make_anim("shift", b1, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0));
    rt_anim_controller3d_play(c3, rt_const_cstr("shift"));
    rt_anim_controller3d_set_bone_lod(c3, b2);
    rt_anim_controller3d_update(c3, 0.5);
    EXPECT_NEAR(rt_mat4_get(rt_anim_controller3d_get_bone_matrix(c3, foot), 0, 3),
                1.0,
                0.1,
                "Bone LOD: frozen bones still follow their animated ancestors");
}

static void test_cubic_spline_keyframe_playback() {
    /* Two keys both at x=0 with symmetric Hermite tangents (+4 out / -4 in):
     * linear playback stays at 0; cubic playback arcs to x=1 at the midpoint
     * (h10*4 + h11*(-4) = 0.125*4 + 0.125*4 = 1). */
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    void *anim = make_anim("arc", 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    const float out_tan[3] = {4.0f, 0.0f, 0.0f};
    const float in_tan[3] = {-4.0f, 0.0f, 0.0f};
    rt_animation3d_set_keyframe_tangents(
        anim, 0, 0.0, out_tan, out_tan, nullptr, nullptr, nullptr, nullptr);
    rt_animation3d_set_keyframe_tangents(
        anim, 0, 1.0, in_tan, in_tan, nullptr, nullptr, nullptr, nullptr);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("arc"), anim);
    rt_anim_controller3d_play(controller, rt_const_cstr("arc"));
    rt_anim_controller3d_update(controller, 0.5);
    EXPECT_NEAR(rt_mat4_get(rt_anim_controller3d_get_bone_matrix(controller, 0), 0, 3),
                1.0,
                0.001,
                "CUBICSPLINE keyframe tangents drive Hermite playback");
    rt_anim_controller3d_update(controller, 0.25);
    /* t = 0.75: h10 = 0.75^3 - 2*0.5625 + 0.75 = -0.140625? compute: 0.421875 - 1.125
     * + 0.75 = 0.046875; h11 = 0.421875 - 0.5625 = -0.140625.
     * x = 0.046875*4 + (-0.140625)*(-4) = 0.1875 + 0.5625 = 0.75. */
    EXPECT_NEAR(rt_mat4_get(rt_anim_controller3d_get_bone_matrix(controller, 0), 0, 3),
                0.75,
                0.001,
                "Hermite playback tracks the spline off-center too");
}

static void test_animation_objects_repair_corrupt_private_counts() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    void *move = make_anim("move", 0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0);

    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("move"), move);
    rt_anim_controller3d_add_event(controller, rt_const_cstr("move"), 0.0, rt_const_cstr("enter"));
    auto *controller_bits = reinterpret_cast<AnimController3DTestPrefix *>(controller);
    controller_bits->state_count = INT32_MAX;
    controller_bits->state_capacity = 1;
    controller_bits->state_name_index_capacity = 3;
    controller_bits->state_name_index_dirty = 0;
    controller_bits->transition_count = INT32_MAX;
    controller_bits->transition_capacity = 0;
    controller_bits->event_count = INT32_MAX;
    controller_bits->event_capacity = 1;

    EXPECT_TRUE(rt_anim_controller3d_get_state_count(controller) == 1,
                "AnimController3D clamps a corrupted state count to the live state table");
    EXPECT_TRUE(rt_anim_controller3d_play(controller, rt_const_cstr("move")) == 1,
                "AnimController3D can still find a state after count repair");
    EXPECT_TRUE(strcmp(rt_string_cstr(rt_anim_controller3d_poll_event(controller)), "enter") == 0,
                "AnimController3D processes only live events after count repair");
    int32_t palette_bones = -1;
    EXPECT_TRUE(rt_anim_controller3d_get_final_palette_data(controller, &palette_bones) != nullptr,
                "AnimController3D still exposes palette data after count repair");
    EXPECT_TRUE(palette_bones == 1, "AnimController3D palette bone count is repaired");
    auto *controller_layout = reinterpret_cast<AnimController3DTestLayout *>(controller);
    controller_layout->layers[0].transitioning = -9;
    EXPECT_TRUE(rt_anim_controller3d_get_is_transitioning(controller) == 1,
                "AnimController3D transition getter normalizes corrupt private flags");
    rt_anim_controller3d_set_animation_lod(controller, 10.0, 10.0);
    controller_layout->animation_lod_accum = NAN;
    rt_anim_controller3d_update(controller, 0.1);
    void *lod_root = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_TRUE(std::isfinite(controller_layout->animation_lod_accum),
                "AnimController3D repairs corrupt animation LOD accumulator");
    EXPECT_NEAR(rt_mat4_get(lod_root, 0, 3),
                0.2,
                0.05,
                "AnimController3D still advances a throttled update after LOD accumulator repair");
    controller_layout->state_count = 0;
    controller_layout->transition_count = INT32_MAX;
    controller_layout->transition_capacity = 0;
    controller_layout->layers[0].current_state = 123;
    controller_layout->layers[0].previous_state = 456;
    controller_layout->layers[0].transitioning = 1;
    controller_layout->layers[0].transition_time = NAN;
    controller_layout->layers[0].transition_duration = INFINITY;
    controller_layout->layers[1].current_state = 123;
    controller_layout->layers[1].weight = NAN;
    rt_anim_controller3d_update(controller, 0.0);
    void *repaired_root = rt_anim_controller3d_get_bone_matrix(controller, 0);
    EXPECT_TRUE(controller_layout->layers[0].current_state == -1 &&
                    controller_layout->layers[0].previous_state == -1 &&
                    controller_layout->layers[0].transitioning == 0,
                "AnimController3D clears layer state that points past the repaired state table");
    EXPECT_NEAR(rt_mat4_get(repaired_root, 0, 3),
                0.0,
                1e-6,
                "AnimController3D falls back to bind pose when the active layer state is invalid");

    void *tree = rt_blend_tree3d_new_1d(skel);
    rt_blend_tree3d_add_sample(tree, move, 0.0, 0.0);
    auto *tree_bits = reinterpret_cast<BlendTree3DTestLayout *>(tree);
    tree_bits->sample_count = INT32_MAX;
    EXPECT_TRUE(rt_blend_tree3d_get_sample_count(tree) == 1,
                "BlendTree3D ignores never-added sample slots after count repair");
    rt_blend_tree3d_update(tree, 0.5);
    int32_t blend_bones = -1;
    const float *locals =
        rt_anim_blend3d_get_local_transform_data(rt_blend_tree3d_get_blend(tree), &blend_bones);
    EXPECT_TRUE(locals != nullptr && blend_bones == 1,
                "BlendTree3D keeps the underlying blend pose valid after count repair");
    EXPECT_NEAR(locals ? locals[3] : 0.0f,
                1.0,
                0.1,
                "BlendTree3D repaired sample count still drives the live sample");
    tree_bits->sample_count = 1;
    tree_bits->samples[0].blend_index = INT32_MAX;
    EXPECT_TRUE(rt_blend_tree3d_get_sample_count(tree) == 0,
                "BlendTree3D drops samples whose AnimBlend3D state index is no longer live");

    void *bad_coord_tree = rt_blend_tree3d_new_1d(skel);
    rt_blend_tree3d_add_sample(bad_coord_tree, move, 0.0, 0.0);
    rt_blend_tree3d_add_sample(bad_coord_tree, move, 1.0, 0.0);
    auto *bad_coord_bits = reinterpret_cast<BlendTree3DTestLayout *>(bad_coord_tree);
    bad_coord_bits->samples[0].x = NAN;
    bad_coord_bits->samples[1].x = NAN;
    rt_blend_tree3d_set_param(bad_coord_tree, 0.5, 0.0);
    rt_blend_tree3d_update(bad_coord_tree, 0.5);
    int32_t corrupt_coord_bones = -1;
    const float *corrupt_coord_locals = rt_anim_blend3d_get_local_transform_data(
        rt_blend_tree3d_get_blend(bad_coord_tree), &corrupt_coord_bones);
    EXPECT_TRUE(corrupt_coord_locals != nullptr && corrupt_coord_bones == 1 &&
                    std::isfinite(corrupt_coord_locals[3]),
                "BlendTree3D falls back safely when all 1D sample coordinates are corrupt");
    rt_blend_tree3d_set_blend_mode(bad_coord_tree, 1);
    EXPECT_TRUE(bad_coord_bits->blend_mode_2d == 0 &&
                    rt_blend_tree3d_get_blend_mode(bad_coord_tree) == 0,
                "BlendTree3D ignores 2D mode changes on a 1D tree");
    bad_coord_bits->samples[0].x = 0.0;
    bad_coord_bits->samples[1].x = 1.0;
    rt_blend_tree3d_set_param(bad_coord_tree, 1.0, 0.0);
    bad_coord_bits->param_x = NAN;
    EXPECT_TRUE(rt_blend_tree3d_get_sample_count(bad_coord_tree) == 2,
                "BlendTree3D count getter retains live samples while repairing parameters");
    EXPECT_NEAR(rt_anim_blend3d_get_weight(rt_blend_tree3d_get_blend(bad_coord_tree), 0),
                1.0,
                1e-12,
                "BlendTree3D getter repair refreshes the first backend weight");
    EXPECT_NEAR(rt_anim_blend3d_get_weight(rt_blend_tree3d_get_blend(bad_coord_tree), 1),
                0.0,
                1e-12,
                "BlendTree3D getter repair clears the stale second backend weight");

    void *tree_2d = rt_blend_tree3d_new_2d(skel);
    rt_blend_tree3d_add_sample(tree_2d, move, 0.0, 0.0);
    rt_blend_tree3d_add_sample(tree_2d, move, 1.0, 0.0);
    rt_blend_tree3d_add_sample(tree_2d, move, 0.0, 1.0);
    auto *tree_2d_bits = reinterpret_cast<BlendTree3DTestLayout *>(tree_2d);
    tree_2d_bits->tri_count = INT32_MAX;
    tree_2d_bits->tris_dirty = 0;
    rt_blend_tree3d_set_param(tree_2d, 0.25, 0.25);
    EXPECT_TRUE(tree_2d_bits->tri_count >= 0 && tree_2d_bits->tri_count <= 64 &&
                    tree_2d_bits->tris_dirty == 0,
                "BlendTree3D bounds a corrupt triangulation count before rebuilding it");

    tree_2d_bits->tri_count = 1;
    tree_2d_bits->tris_dirty = 0;
    tree_2d_bits->tris[0] = INT32_MAX;
    tree_2d_bits->tris[1] = -1;
    tree_2d_bits->tris[2] = 0;
    rt_blend_tree3d_set_param(tree_2d, 0.3, 0.2);
    EXPECT_TRUE(tree_2d_bits->tri_count >= 0 && tree_2d_bits->tri_count <= 64,
                "BlendTree3D rejects cached triangle indexes outside the sample table");
    for (int32_t t = 0; t < tree_2d_bits->tri_count; t++) {
        EXPECT_TRUE(tree_2d_bits->tris[t * 3] >= 0 && tree_2d_bits->tris[t * 3] < 3 &&
                        tree_2d_bits->tris[t * 3 + 1] >= 0 && tree_2d_bits->tris[t * 3 + 1] < 3 &&
                        tree_2d_bits->tris[t * 3 + 2] >= 0 && tree_2d_bits->tris[t * 3 + 2] < 3,
                    "BlendTree3D rebuilt triangles contain only live sample indexes");
    }

    tree_2d_bits->tris_dirty = -7;
    tree_2d_bits->param_x = NAN;
    tree_2d_bits->param_y = INFINITY;
    tree_2d_bits->samples[0].x = NAN;
    tree_2d_bits->samples[1].y = INFINITY;
    rt_blend_tree3d_update(tree_2d, 0.0);
    EXPECT_TRUE(std::isfinite(tree_2d_bits->param_x) && std::isfinite(tree_2d_bits->param_y) &&
                    std::isfinite(tree_2d_bits->samples[0].x) &&
                    std::isfinite(tree_2d_bits->samples[1].y) &&
                    (tree_2d_bits->tris_dirty == 0 || tree_2d_bits->tris_dirty == 1),
                "BlendTree3D repairs parameters, sample coordinates, and cache flags together");

    tree_2d_bits->samples[0].x = 0.0;
    tree_2d_bits->samples[0].y = 0.0;
    tree_2d_bits->samples[1].x = 1.0;
    tree_2d_bits->samples[1].y = 0.0;
    tree_2d_bits->samples[2].x = 0.0;
    tree_2d_bits->samples[2].y = 1.0;
    tree_2d_bits->tris_dirty = 1;
    rt_blend_tree3d_set_param(tree_2d, -5e-10, 0.25);
    double weight_sum = 0.0;
    for (int64_t state = 0; state < 3; state++) {
        double weight = rt_anim_blend3d_get_weight(rt_blend_tree3d_get_blend(tree_2d), state);
        EXPECT_TRUE(std::isfinite(weight) && weight >= 0.0,
                    "BlendTree3D publishes finite non-negative barycentric weights");
        weight_sum += weight;
    }
    EXPECT_NEAR(weight_sum,
                1.0,
                1e-12,
                "BlendTree3D renormalizes barycentric weights after tolerance clamping");

    void *ik_skel = rt_skeleton3d_new();
    int64_t root = rt_skeleton3d_add_bone(ik_skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t mid = rt_skeleton3d_add_bone(ik_skel, rt_const_cstr("mid"), root, rt_mat4_identity());
    int64_t end = rt_skeleton3d_add_bone(ik_skel, rt_const_cstr("end"), mid, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(ik_skel);
    void *solver = rt_ik_solver3d_two_bone(ik_skel, root, mid, end);
    auto *solver_bits = reinterpret_cast<IKSolver3DTestPrefix *>(solver);
    solver_bits->chain_count = INT32_MAX;
    float locals_pose[3 * 16];
    float globals_pose[3 * 16];
    fill_identity_pose(locals_pose, 3);
    fill_identity_pose(globals_pose, 3);
    EXPECT_TRUE(rt_ik_solver3d_apply_to_pose(solver, locals_pose, globals_pose, INT32_MAX) == 0,
                "IKSolver3D rejects a corrupted chain count before walking the fixed chain");
}

static void test_anim_controller_private_refs_clear_wrong_class_without_release() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    void *move = make_anim("move", 0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);

    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("move"), move);
    auto *layout = reinterpret_cast<AnimController3DTestLayout *>(controller);

    void *wrong_blend_tree = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_blend_tree);
    layout->blend_tree = wrong_blend_tree;
    EXPECT_TRUE(rt_anim_controller3d_set_blend_tree(controller, nullptr) == 1,
                "AnimController3D clears a corrupted BlendTree3D slot");
    EXPECT_TRUE(layout->blend_tree == nullptr, "AnimController3D nulls the corrupted blend tree");
    expect_retained_probe_untouched(
        wrong_blend_tree, "AnimController3D does not release wrong-class blend tree slots");

    void *wrong_ik = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_ik);
    layout->ik_solvers[0] = wrong_ik;
    layout->ik_solver_count = 1;
    EXPECT_TRUE(rt_anim_controller3d_set_ik_solver(controller, nullptr) == 1,
                "AnimController3D clears a corrupted IKSolver3D slot");
    EXPECT_TRUE(layout->ik_solvers[0] == nullptr && layout->ik_solver_count == 0,
                "AnimController3D nulls the corrupted IK solver stack");
    expect_retained_probe_untouched(
        wrong_ik, "AnimController3D does not release wrong-class IK solver slots");

    void *controller_bad_skeleton = rt_anim_controller3d_new(skel);
    auto *bad_skeleton_layout =
        reinterpret_cast<AnimController3DTestLayout *>(controller_bad_skeleton);
    void *wrong_controller_skeleton = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_controller_skeleton);
    if (bad_skeleton_layout->skeleton && rt_obj_release_check0(bad_skeleton_layout->skeleton))
        rt_obj_free(bad_skeleton_layout->skeleton);
    bad_skeleton_layout->skeleton = wrong_controller_skeleton;
    int32_t bad_palette_bones = -1;
    EXPECT_TRUE(rt_anim_controller3d_get_final_palette_data(controller_bad_skeleton,
                                                            &bad_palette_bones) == nullptr,
                "AnimController3D palette getter hides wrong-class skeleton slots");
    EXPECT_TRUE(bad_palette_bones == 0,
                "AnimController3D palette getter clears bone count for invalid skeleton slots");
    EXPECT_TRUE(rt_anim_controller3d_get_skeleton(controller_bad_skeleton) == nullptr,
                "AnimController3D skeleton getter hides wrong-class skeleton slots");
    EXPECT_TRUE(bad_skeleton_layout->skeleton == nullptr,
                "AnimController3D clears wrong-class skeleton slots on public read");
    if (rt_obj_release_check0(controller_bad_skeleton))
        rt_obj_free(controller_bad_skeleton);
    expect_retained_probe_untouched(
        wrong_controller_skeleton,
        "AnimController3D public skeleton getters do not release wrong-class skeleton slots");

    void *controller_finalizer = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller_finalizer, rt_const_cstr("move"), move);
    auto *finalizer_layout = reinterpret_cast<AnimController3DTestLayout *>(controller_finalizer);
    auto *states = reinterpret_cast<AnimController3DStateTestLayout *>(finalizer_layout->states);

    void *wrong_skeleton = rt_obj_new_i64(0, 8);
    void *wrong_animation = rt_obj_new_i64(0, 8);
    void *wrong_player = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_skeleton);
    rt_obj_retain_maybe(wrong_animation);
    rt_obj_retain_maybe(wrong_player);

    if (finalizer_layout->skeleton && rt_obj_release_check0(finalizer_layout->skeleton))
        rt_obj_free(finalizer_layout->skeleton);
    finalizer_layout->skeleton = wrong_skeleton;
    if (states && states[0].animation && rt_obj_release_check0(states[0].animation))
        rt_obj_free(states[0].animation);
    if (states)
        states[0].animation = wrong_animation;
    if (finalizer_layout->layers[0].player &&
        rt_obj_release_check0(finalizer_layout->layers[0].player))
        rt_obj_free(finalizer_layout->layers[0].player);
    finalizer_layout->layers[0].player = wrong_player;

    if (rt_obj_release_check0(controller_finalizer))
        rt_obj_free(controller_finalizer);
    expect_retained_probe_untouched(
        wrong_skeleton, "AnimController3D finalizer does not release wrong-class skeleton slots");
    expect_retained_probe_untouched(
        wrong_animation, "AnimController3D finalizer does not release wrong-class animation slots");
    expect_retained_probe_untouched(
        wrong_player, "AnimController3D finalizer does not release wrong-class player slots");

    void *tree_finalizer = rt_blend_tree3d_new_1d(skel);
    auto *tree_layout = reinterpret_cast<BlendTree3DTestLayout *>(tree_finalizer);
    void *wrong_blend = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_blend);
    if (tree_layout->blend && rt_obj_release_check0(tree_layout->blend))
        rt_obj_free(tree_layout->blend);
    tree_layout->blend = wrong_blend;
    if (rt_obj_release_check0(tree_finalizer))
        rt_obj_free(tree_finalizer);
    expect_retained_probe_untouched(
        wrong_blend, "BlendTree3D finalizer does not release wrong-class AnimBlend3D slots");

    void *solver_finalizer = rt_ik_solver3d_look_at(skel, 0);
    auto *solver_layout = reinterpret_cast<IKSolver3DTestPrefix *>(solver_finalizer);
    void *wrong_solver_skeleton = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_solver_skeleton);
    if (solver_layout->skeleton && rt_obj_release_check0(solver_layout->skeleton))
        rt_obj_free(solver_layout->skeleton);
    solver_layout->skeleton = wrong_solver_skeleton;
    EXPECT_TRUE(rt_ik_solver3d_get_skeleton(solver_finalizer) == nullptr,
                "IKSolver3D hides wrong-class retained skeleton slots");
    if (rt_obj_release_check0(solver_finalizer))
        rt_obj_free(solver_finalizer);
    expect_retained_probe_untouched(
        wrong_solver_skeleton, "IKSolver3D finalizer does not release wrong-class skeleton slots");

    void *player_finalizer = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player_finalizer, move);
    rt_anim_player3d_crossfade(player_finalizer, move, 0.25);
    auto *player_layout = reinterpret_cast<rt_anim_player3d *>(player_finalizer);
    void *wrong_player_skeleton = rt_obj_new_i64(0, 8);
    void *wrong_player_current = rt_obj_new_i64(0, 8);
    void *wrong_player_crossfade = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_player_skeleton);
    rt_obj_retain_maybe(wrong_player_current);
    rt_obj_retain_maybe(wrong_player_crossfade);
    player_layout->skeleton = reinterpret_cast<rt_skeleton3d *>(wrong_player_skeleton);
    player_layout->current = reinterpret_cast<rt_animation3d *>(wrong_player_current);
    player_layout->crossfade_from = reinterpret_cast<rt_animation3d *>(wrong_player_crossfade);
    EXPECT_TRUE(rt_anim_player3d_get_bone_matrix(player_finalizer, 0) != nullptr,
                "AnimPlayer3D repairs corrupted retained-reference mirrors");
    rt_anim_player3d_update(player_finalizer, 0.1);
    rt_anim_player3d_stop(player_finalizer);
    if (rt_obj_release_check0(player_finalizer))
        rt_obj_free(player_finalizer);
    expect_retained_probe_untouched(
        wrong_player_skeleton,
        "AnimPlayer3D finalizer does not release wrong-class skeleton slots");
    expect_retained_probe_untouched(
        wrong_player_current, "AnimPlayer3D finalizer does not release wrong-class current clips");
    expect_retained_probe_untouched(wrong_player_crossfade,
                                    "AnimPlayer3D does not release wrong-class crossfade clips");

    void *blend_with_bad_skeleton = rt_anim_blend3d_new(skel);
    rt_anim_blend3d_add_state(blend_with_bad_skeleton, rt_const_cstr("move"), move);
    auto *bad_skel_blend_layout = reinterpret_cast<rt_anim_blend3d *>(blend_with_bad_skeleton);
    void *wrong_blend_skeleton = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_blend_skeleton);
    bad_skel_blend_layout->skeleton = reinterpret_cast<rt_skeleton3d *>(wrong_blend_skeleton);
    int32_t bad_blend_bones = 1;
    EXPECT_TRUE(rt_anim_blend3d_get_skeleton(blend_with_bad_skeleton) == skel,
                "AnimBlend3D repairs its corrupted skeleton mirror");
    EXPECT_TRUE(rt_anim_blend3d_get_local_transform_data(blend_with_bad_skeleton,
                                                         &bad_blend_bones) != nullptr &&
                    bad_blend_bones == 1,
                "AnimBlend3D local transform queries use repaired owned storage");
    rt_anim_blend3d_update(blend_with_bad_skeleton, 0.1);
    if (rt_obj_release_check0(blend_with_bad_skeleton))
        rt_obj_free(blend_with_bad_skeleton);
    expect_retained_probe_untouched(
        wrong_blend_skeleton, "AnimBlend3D finalizer does not release wrong-class skeleton slots");

    void *blend_with_bad_state = rt_anim_blend3d_new(skel);
    rt_anim_blend3d_add_state(blend_with_bad_state, rt_const_cstr("move"), move);
    rt_anim_blend3d_set_weight(blend_with_bad_state, 0, 1.0);
    auto *bad_state_blend_layout = reinterpret_cast<rt_anim_blend3d *>(blend_with_bad_state);
    void *wrong_state_animation = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_state_animation);
    bad_state_blend_layout->states[0].animation =
        reinterpret_cast<rt_animation3d *>(wrong_state_animation);
    rt_anim_blend3d_update(blend_with_bad_state, 0.1);
    EXPECT_TRUE(bad_state_blend_layout->states[0].animation == move,
                "AnimBlend3D update repairs wrong-class state mirrors");
    if (rt_obj_release_check0(blend_with_bad_state))
        rt_obj_free(blend_with_bad_state);
    expect_retained_probe_untouched(
        wrong_state_animation, "AnimBlend3D finalizer does not release wrong-class state clips");
}

int main() {
    test_controller_state_flow();
    test_controller_root_motion_disabled_and_loop_wrap();
    test_controller_crossfade_preserves_source_speed();
    test_controller_masked_layer();
    test_controller_true_additive_layer_uses_bind_pose_delta();
    test_controller_blend_tree_drives_base_pose();
    test_controller_play_refreshes_final_pose_immediately();
    test_controller_blend_tree_root_motion_uses_final_pose();
    test_controller_private_skeleton_growth_stays_in_bounds();
    test_skeletal_state_registration_rejects_out_of_range_clip_bones();
    test_two_bone_ik_pole_vector();
    test_two_bone_ik_bends_bone_rotations();
    test_controller_ik_solver_drives_end_effector();
    test_controller_ordered_ik_solver_stack();
    test_two_bone_ik_foot_aligns_to_ground_normal();
    test_ik_solver_look_at_and_fabrik_factories();
    test_ik_solver_repairs_parent_space_inputs_and_owned_pose_storage();
    test_controller_animation_lod_throttles_updates_deterministically();
    test_controller_bone_count_lod_freezes_distal_bones();
    test_controller_events_cover_full_loops_and_reverse();
    test_controller_rejects_wrong_animation_handles();
    test_controller_rejects_wrong_string_handles();
    test_controller_long_state_names_use_canonical_lookup();
    test_cubic_spline_keyframe_playback();
    test_animation_objects_repair_corrupt_private_counts();
    test_anim_controller_private_refs_clear_wrong_class_without_release();

    printf("AnimController3D tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
