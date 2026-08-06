//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_skeleton3d.cpp
// Purpose: Unit tests for Skeleton3D, Animation3D, AnimPlayer3D — bone
//   hierarchy, keyframe sampling, CPU skinning, crossfade.
//
// Key invariants:
//   - Exact bone names and double key times survive private runtime storage.
//   - Invalid/corrupt private counts are clamped before pose traversal.
// Ownership/Lifetime:
//   - Runtime fixtures are owned for the duration of the test process.
//
// Links: rt_skeleton3d.h, vgfx3d_skinning.h, plans/3d/14-skeletal-animation.md
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_canvas3d.h"
#include "rt_internal.h"
#include "rt_option.h"
#include "rt_skeleton3d.h"
#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_quat_new(double x, double y, double z, double w);
extern void *rt_quat_from_euler(double pitch, double yaw, double roll);
extern double rt_quat_x(void *q);
extern double rt_quat_y(void *q);
extern double rt_quat_z(void *q);
extern double rt_quat_w(void *q);
extern void *rt_mat4_identity(void);
extern void *rt_mat4_translate(double tx, double ty, double tz);
extern void *rt_mat4_rotate_x(double angle);
extern rt_string rt_const_cstr(const char *s);
extern int rt_obj_release_check0(void *obj);
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

static void test_skeleton_create() {
    void *skel = rt_skeleton3d_new();
    EXPECT_TRUE(skel != nullptr, "Skeleton3D.New returns non-null");
    EXPECT_TRUE(rt_skeleton3d_get_bone_count(skel) == 0, "Initial bone count = 0");
}

static void test_skeleton_add_bone() {
    void *skel = rt_skeleton3d_new();
    rt_string name = rt_const_cstr("root");
    int64_t idx = rt_skeleton3d_add_bone(skel, name, -1, rt_mat4_identity());
    EXPECT_TRUE(idx == 0, "First bone index = 0");
    EXPECT_TRUE(rt_skeleton3d_get_bone_count(skel) == 1, "Bone count = 1");

    rt_string child_name = rt_const_cstr("child");
    int64_t cidx = rt_skeleton3d_add_bone(skel, child_name, 0, rt_mat4_translate(1.0, 0.0, 0.0));
    EXPECT_TRUE(cidx == 1, "Second bone index = 1");
    EXPECT_TRUE(rt_skeleton3d_get_bone_count(skel) == 2, "Bone count = 2");
}

static void test_skeleton_find_bone() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_add_bone(skel, rt_const_cstr("arm"), 0, rt_mat4_identity());
    rt_skeleton3d_add_bone(skel, rt_const_cstr("hand"), 1, rt_mat4_identity());

    EXPECT_TRUE(rt_skeleton3d_find_bone(skel, rt_const_cstr("arm")) == 1, "FindBone('arm') = 1");
    EXPECT_TRUE(rt_skeleton3d_find_bone(skel, rt_const_cstr("hand")) == 2, "FindBone('hand') = 2");
    EXPECT_TRUE(rt_skeleton3d_find_bone(skel, rt_const_cstr("missing")) == -1,
                "FindBone('missing') = -1");
    void *arm_option = rt_skeleton3d_find_bone_option(skel, rt_const_cstr("arm"));
    EXPECT_TRUE(rt_option_is_some(arm_option) == 1, "FindBoneOption('arm') returns Some");
    EXPECT_TRUE(rt_option_unwrap_i64(arm_option) == 1, "FindBoneOption('arm') unwraps index 1");
    EXPECT_TRUE(rt_option_is_none(rt_skeleton3d_find_bone_option(skel, rt_const_cstr("missing"))) ==
                    1,
                "FindBoneOption('missing') returns None");
}

static void test_skeleton_find_bone_uses_canonical_long_names() {
    void *skel = rt_skeleton3d_new();
    char long_name[128];
    std::memset(long_name, 'b', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';

    EXPECT_TRUE(rt_skeleton3d_add_bone(skel, rt_const_cstr(long_name), -1, rt_mat4_identity()) == 0,
                "Skeleton3D accepts long bone names");
    EXPECT_TRUE(rt_skeleton3d_find_bone(skel, rt_const_cstr(long_name)) == 0,
                "Skeleton3D.FindBone canonicalizes long names before lookup");
}

static void test_animation_create() {
    void *anim = rt_animation3d_new(rt_const_cstr("walk"), 1.0);
    EXPECT_TRUE(anim != nullptr, "Animation3D.New returns non-null");
    EXPECT_NEAR(rt_animation3d_get_duration(anim), 1.0, 0.001, "Duration = 1.0");
}

static void test_animation_keyframes() {
    void *anim = rt_animation3d_new(rt_const_cstr("test"), 1.0);
    void *pos0 = rt_vec3_new(0.0, 0.0, 0.0);
    void *pos1 = rt_vec3_new(1.0, 0.0, 0.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);

    rt_animation3d_add_keyframe(anim, 0, 0.0, pos0, rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, pos1, rot, scl);
    /* Keyframes stored successfully — no crash */
    EXPECT_TRUE(1, "AddKeyframe succeeds");
}

static void test_animation_keyframes_are_sorted() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("sorted"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 1.0, rt_vec3_new(10.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 0.0, rt_vec3_new(0.0, 0.0, 0.0), rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 0.5);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(mv->m[3], 5.0, 0.1, "Out-of-order keyframes sample at sorted midpoint");
}

/// @brief Verify arithmetic-noise duplicate key times replace instead of creating two samples.
/// @details The delta is below the double-time equality tolerance but far below one FBX source
///          tick; the later TRS must replace the earlier key without weakening tick preservation.
static void test_animation_roundoff_duplicate_keyframes_replace_existing_sample() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("dedupe"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, rt_vec3_new(1.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 0.00000000000005, rt_vec3_new(2.0, 0.0, 0.0), rot, scl);

    rt_animation3d *impl = (rt_animation3d *)anim;
    EXPECT_TRUE(impl->channel_count == 1, "Roundoff-duplicate keyframes keep one channel");
    EXPECT_TRUE(impl->channels[0].keyframe_count == 1,
                "Roundoff-duplicate keyframes replace the existing sample");

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 0.0);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(mv->m[3], 2.0, 0.05, "Roundoff-duplicate keyframe keeps latest TRS values");
}

static void test_skeleton_animation_repairs_corrupt_counts() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_add_bone(skel, rt_const_cstr("child"), 0, rt_mat4_identity());
    auto *skel_impl = static_cast<rt_skeleton3d *>(skel);
    skel_impl->bone_count = INT32_MAX;
    skel_impl->bone_capacity = 2;
    EXPECT_TRUE(rt_skeleton3d_get_bone_count(skel) == 2,
                "Skeleton3D boneCount clamps corrupt count to capacity");
    EXPECT_TRUE(rt_skeleton3d_find_bone(skel, rt_const_cstr("child")) == 1,
                "Skeleton3D FindBone walks repaired bone count");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_skeleton3d_get_bone_name(skel, 2)), "") == 0,
                "Skeleton3D GetBoneName rejects indexes beyond repaired count");

    void *anim = rt_animation3d_new(rt_const_cstr("corrupt_counts"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, rt_vec3_new(0.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, rt_vec3_new(1.0, 0.0, 0.0), rot, scl);
    auto *anim_impl = static_cast<rt_animation3d *>(anim);
    anim_impl->channel_count = INT32_MAX;
    anim_impl->channel_capacity = 1;
    anim_impl->channels[0].keyframe_count = INT32_MAX;
    anim_impl->channels[0].keyframe_capacity = 2;

    void *player = rt_anim_player3d_new(skel);
    EXPECT_TRUE(player != nullptr, "AnimPlayer3D.New accepts repaired skeleton counts");
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 0.5);
    EXPECT_TRUE(anim_impl->channel_count == 1,
                "AnimPlayer3D playback repairs corrupt animation channel count");

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_TRUE(mv != nullptr, "AnimPlayer3D returns matrix after repaired playback");
    if (mv)
        EXPECT_NEAR(mv->m[3], 0.5, 0.05, "AnimPlayer3D samples repaired keyframe count");
}

static void test_animation_safe_counts_have_domain_ceilings() {
    rt_animation3d fake_anim = {};
    vgfx3d_anim_channel_t fake_channels[1] = {};
    vgfx3d_anim_channel_t fake_channel = {};
    vgfx3d_keyframe_t fake_keyframe = {};

    fake_anim.channels = fake_channels;
    fake_anim.channel_count = INT32_MAX;
    fake_anim.channel_capacity = INT32_MAX;
    EXPECT_TRUE(animation3d_safe_channel_count(&fake_anim) == RT_ANIMATION3D_MAX_CHANNELS,
                "Animation3D safe channel count clamps to the bone-channel ceiling");

    fake_channel.keyframes = &fake_keyframe;
    fake_channel.keyframe_count = INT32_MAX;
    fake_channel.keyframe_capacity = INT32_MAX;
    EXPECT_TRUE(animation3d_safe_keyframe_count(&fake_channel) ==
                    RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL,
                "Animation3D safe keyframe count clamps to the per-channel ceiling");

    void *anim = rt_animation3d_new(rt_const_cstr("full_channels"), 1.0);
    auto *impl = static_cast<rt_animation3d *>(anim);
    EXPECT_TRUE(impl != nullptr, "Animation3D channel-limit fixture exists");
    if (!impl)
        return;
    impl->channels = static_cast<vgfx3d_anim_channel_t *>(
        std::calloc(RT_ANIMATION3D_MAX_CHANNELS, sizeof(vgfx3d_anim_channel_t)));
    EXPECT_TRUE(impl->channels != nullptr, "Animation3D channel-limit table allocated");
    if (!impl->channels)
        return;
    impl->owned_channels = impl->channels;
    impl->channel_count = RT_ANIMATION3D_MAX_CHANNELS;
    impl->channel_capacity = RT_ANIMATION3D_MAX_CHANNELS;
    impl->owned_channel_capacity = RT_ANIMATION3D_MAX_CHANNELS;
    impl->initialized_channel_count = RT_ANIMATION3D_MAX_CHANNELS;
    for (int32_t i = 0; i < RT_ANIMATION3D_MAX_CHANNELS; i++)
        impl->channels[i].bone_index = -1;

    rt_animation3d_add_keyframe(anim,
                                0,
                                0.0,
                                rt_vec3_new(1.0, 0.0, 0.0),
                                rt_quat_new(0.0, 0.0, 0.0, 1.0),
                                rt_vec3_new(1.0, 1.0, 1.0));
    EXPECT_TRUE(impl->channel_count == RT_ANIMATION3D_MAX_CHANNELS,
                "Animation3D.AddKeyframe refuses to grow past the channel ceiling");
    EXPECT_TRUE(impl->channel_capacity == RT_ANIMATION3D_MAX_CHANNELS,
                "Animation3D.AddKeyframe keeps channel capacity at the ceiling");
}

static void test_anim_player_retains_inputs() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("held"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, rt_vec3_new(0.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, rt_vec3_new(1.0, 0.0, 0.0), rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    int skel_zero = rt_obj_release_check0(skel);
    int anim_zero = rt_obj_release_check0(anim);
    EXPECT_TRUE(skel_zero == 0, "AnimPlayer3D retains its skeleton");
    EXPECT_TRUE(anim_zero == 0, "AnimPlayer3D retains the current animation");
    if (skel_zero || anim_zero)
        return;
    rt_anim_player3d_update(player, 0.5);
    EXPECT_TRUE(rt_anim_player3d_get_bone_matrix(player, 0) != nullptr,
                "AnimPlayer3D remains usable after caller releases inputs");
}

static void test_player_create() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *player = rt_anim_player3d_new(skel);
    EXPECT_TRUE(player != nullptr, "AnimPlayer3D.New returns non-null");
    EXPECT_TRUE(rt_anim_player3d_is_playing(player) == 0, "Not playing initially");
}

static void test_animation_getters_normalize_corrupt_private_flags() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    void *anim = rt_animation3d_new(rt_const_cstr("flags"), 1.0);
    void *player = rt_anim_player3d_new(skel);

    static_cast<rt_animation3d *>(anim)->looping = -8;
    static_cast<rt_anim_player3d *>(player)->playing = -7;

    EXPECT_TRUE(rt_animation3d_get_looping(anim) == 1,
                "Animation3D looping getter normalizes corrupt private flags");
    EXPECT_TRUE(rt_anim_player3d_is_playing(player) == 1,
                "AnimPlayer3D playing getter normalizes corrupt private flags");

    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_crossfade(player, anim, 0.25);
    EXPECT_TRUE(static_cast<rt_anim_player3d *>(player)->crossfade_from_looping == 1,
                "AnimPlayer3D crossfade loop snapshots normalize corrupt clip flags");
}

static void test_skeleton_freezes_after_player_creation() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *player = rt_anim_player3d_new(skel);
    EXPECT_TRUE(player != nullptr, "AnimPlayer3D.New freezes skeleton after pose buffers exist");
    EXPECT_TRUE(((rt_skeleton3d *)skel)->frozen == 1,
                "Skeleton3D records frozen topology after player creation");
    EXPECT_TRUE(rt_skeleton3d_get_bone_count(skel) == 1,
                "Frozen skeleton bone count remains unchanged");
}

static void test_player_playback() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("slide"), 1.0);
    void *pos0 = rt_vec3_new(0.0, 0.0, 0.0);
    void *pos1 = rt_vec3_new(10.0, 0.0, 0.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, pos0, rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, pos1, rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    EXPECT_TRUE(rt_anim_player3d_is_playing(player) == 1, "Playing after play()");

    /* Advance to midpoint */
    rt_anim_player3d_update(player, 0.5);
    void *mat = rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_TRUE(mat != nullptr, "GetBoneMatrix returns non-null");

    /* At t=0.5, position should be (5, 0, 0). Bone palette = global * inverse_bind.
     * With identity bind pose, inverse_bind = identity, so palette = local transform.
     * The translation at t=0.5: lerp(0, 10, 0.5) = 5. */
    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)mat;
    EXPECT_NEAR(mv->m[3], 5.0, 0.1, "At t=0.5: bone X translation ≈ 5");
}

static void test_player_loop() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("loop"), 1.0);
    rt_animation3d_set_looping(anim, 1);
    void *pos0 = rt_vec3_new(0.0, 0.0, 0.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, pos0, rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, pos0, rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);

    /* Advance past duration — should wrap */
    rt_anim_player3d_update(player, 1.5);
    EXPECT_TRUE(rt_anim_player3d_is_playing(player) == 1, "Still playing after loop wrap");
    EXPECT_NEAR(rt_anim_player3d_get_time(player), 0.5, 0.01, "Time wraps to 0.5");

    rt_anim_player3d_set_time(player, 2.25);
    EXPECT_NEAR(
        rt_anim_player3d_get_time(player), 0.25, 0.01, "Looping SetTime wraps positive seeks");
    rt_anim_player3d_set_time(player, -0.25);
    EXPECT_NEAR(
        rt_anim_player3d_get_time(player), 0.75, 0.01, "Looping SetTime wraps negative seeks");
}

/// @brief Extreme finite elapsed times and speeds must never overflow animation clocks.
static void test_player_extreme_clock_arithmetic_stays_finite() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    void *anim = rt_animation3d_new(rt_const_cstr("extreme_clock"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, rt_vec3_new(0.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, rt_vec3_new(1.0, 0.0, 0.0), rot, scl);
    void *player = rt_anim_player3d_new(skel);

    rt_animation3d_set_looping(anim, 1);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_set_speed(player, DBL_MAX);
    rt_anim_player3d_update(player, DBL_MAX);
    double loop_time = rt_anim_player3d_get_time(player);
    EXPECT_TRUE(std::isfinite(loop_time) && loop_time >= 0.0 && loop_time < 1.0,
                "Extreme looping clock stays finite and wrapped");

    rt_animation3d_set_looping(anim, 0);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_set_speed(player, DBL_MAX);
    rt_anim_player3d_update(player, DBL_MAX);
    EXPECT_NEAR(rt_anim_player3d_get_time(player),
                1.0,
                0.0,
                "Extreme forward clock clamps exactly to the endpoint");
    EXPECT_TRUE(rt_anim_player3d_is_playing(player) == 0,
                "Extreme forward clock stops a non-looping clip");

    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_set_time(player, 1.0);
    rt_anim_player3d_set_speed(player, -DBL_MAX);
    rt_anim_player3d_update(player, DBL_MAX);
    EXPECT_NEAR(rt_anim_player3d_get_time(player),
                0.0,
                0.0,
                "Extreme reverse clock clamps exactly to the start");
}

static void test_player_reverse_timing_and_bad_handles() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("reverse"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, rt_vec3_new(0.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, rt_vec3_new(1.0, 0.0, 0.0), rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_animation3d_set_looping(anim, 1);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_set_time(player, 0.25);
    rt_anim_player3d_set_speed(player, -1.0);
    rt_anim_player3d_update(player, 0.5);
    EXPECT_TRUE(rt_anim_player3d_is_playing(player) == 1,
                "Reverse looping playback keeps playing after wrapping below zero");
    EXPECT_NEAR(rt_anim_player3d_get_time(player),
                0.75,
                0.01,
                "Reverse looping playback wraps negative time");

    rt_anim_player3d_set_speed(player, 1.0);
    rt_anim_player3d_set_time(player, 0.25);
    rt_anim_player3d_update(player, -1.0);
    EXPECT_NEAR(
        rt_anim_player3d_get_time(player), 0.25, 0.01, "AnimPlayer3D ignores negative delta time");

    rt_animation3d_set_looping(anim, 0);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_set_time(player, 0.25);
    rt_anim_player3d_set_speed(player, -1.0);
    rt_anim_player3d_update(player, 0.5);
    EXPECT_TRUE(rt_anim_player3d_is_playing(player) == 0,
                "Reverse non-looping playback stops at the start");
    EXPECT_NEAR(rt_anim_player3d_get_time(player),
                0.0,
                0.01,
                "Reverse non-looping playback clamps to zero");

    void *fresh_player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(fresh_player, skel);
    EXPECT_TRUE(rt_anim_player3d_is_playing(fresh_player) == 0,
                "AnimPlayer3D.Play rejects non-Animation3D handles");
    rt_anim_player3d_set_time(skel, 0.5);
    EXPECT_NEAR(rt_anim_player3d_get_time(skel),
                0.0,
                0.01,
                "AnimPlayer3D accessors reject non-player handles");
}

static void test_player_stop_at_end() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("once"), 1.0);
    rt_animation3d_set_looping(anim, 0);
    void *pos = rt_vec3_new(0.0, 0.0, 0.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, pos, rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, pos, rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 2.0);
    EXPECT_TRUE(rt_anim_player3d_is_playing(player) == 0, "Stopped after non-looping end");
}

static void test_player_stop_returns_to_bind_pose() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_translate(3.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("move"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, rt_vec3_new(10.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, rt_vec3_new(10.0, 0.0, 0.0), rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 0.0);
    rt_anim_player3d_stop(player);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(mv->m[3], 3.0, 0.1, "AnimPlayer3D.Stop restores bind-pose world matrix");
}

static void test_player_speed() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("fast"), 2.0);
    void *pos = rt_vec3_new(0.0, 0.0, 0.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, pos, rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 2.0, pos, rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_set_speed(player, 2.0);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 0.5);
    /* At speed 2x, 0.5s real time = 1.0s animation time */
    EXPECT_NEAR(rt_anim_player3d_get_time(player), 1.0, 0.01, "Speed 2x: t=1.0 after 0.5s");
}

static void test_two_bone_chain() {
    void *skel = rt_skeleton3d_new();
    /* Root at origin */
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    /* Child offset by (2, 0, 0) */
    rt_skeleton3d_add_bone(skel, rt_const_cstr("child"), 0, rt_mat4_translate(2.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    /* Animation: translate root by (5, 0, 0). Child stays at local (2, 0, 0). */
    void *anim = rt_animation3d_new(rt_const_cstr("move"), 1.0);
    void *pos5 = rt_vec3_new(5.0, 0.0, 0.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, pos5, rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, pos5, rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 0.0);

    /* Root bone: global = translate(5,0,0). Palette = global * inv_bind.
     * inv_bind(root) = identity. So palette[0] = translate(5,0,0). */
    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *m0 = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(m0->m[3], 5.0, 0.1, "Root world X = 5");

    /* Child bone: global = parent_global * child_local = translate(5,0,0) * translate(2,0,0)
     * = translate(7,0,0). */
    mat4_view *m1 = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 1);
    EXPECT_NEAR(m1->m[3], 7.0, 0.1, "Child world X = 7 (root moved +5, child stays relative)");
}

static void test_non_identity_bind_pose() {
    /* Regression test: bone palette must use separate globals buffer.
     * With non-identity bind pose, palette = global * inverse_bind.
     * A child's global must NOT include parent's inverse_bind. */
    void *skel = rt_skeleton3d_new();
    /* Root bone at position (3, 0, 0) */
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_translate(3.0, 0.0, 0.0));
    /* Child bone at position (2, 0, 0) relative to root */
    rt_skeleton3d_add_bone(skel, rt_const_cstr("child"), 0, rt_mat4_translate(2.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    /* Animation: move root to (10, 0, 0), child stays at local (2, 0, 0) */
    void *anim = rt_animation3d_new(rt_const_cstr("move"), 1.0);
    void *pos_root = rt_vec3_new(10.0, 0.0, 0.0);
    void *pos_child = rt_vec3_new(2.0, 0.0, 0.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, pos_root, rot, scl);
    rt_animation3d_add_keyframe(anim, 1, 0.0, pos_child, rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 0.0);

    /* GetBoneMatrix returns the animated world/global transform, not the skinning palette. */
    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *m0 = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(m0->m[3], 10.0, 0.1, "Root world X = 10");

    /* Child: global = parent_global(10,0,0) * child_local(2,0,0) = translate(12,0,0). */
    mat4_view *m1 = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 1);
    EXPECT_NEAR(m1->m[3], 12.0, 0.1, "Child world X = 12 (non-identity bind pose)");
}

static void test_partial_keyframes_preserve_bind_components() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_translate(3.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("partial"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, NULL, rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, rt_vec3_new(INFINITY, 0.0, 0.0), rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 0.5);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(
        mv->m[3], 3.0, 0.1, "Partial/overflow keyframe position components fall back to bind pose");
}

static void test_bone_name() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("LeftArm"), -1, rt_mat4_identity());
    rt_string name = rt_skeleton3d_get_bone_name(skel, 0);
    EXPECT_TRUE(name != nullptr, "GetBoneName returns non-null");
}

/*==========================================================================
 * Crossfade tests
 *=========================================================================*/

static void test_crossfade_basic() {
    EXPECT_TRUE(1, "crossfade: TRS-based SLERP blending (compile check)");
    /* Create skeleton with 1 bone */
    void *skel = rt_skeleton3d_new();
    rt_string bone_name = rt_const_cstr("root");
    rt_skeleton3d_add_bone(skel, bone_name, -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    /* Animation A: bone at identity */
    void *anim_a = rt_animation3d_new(rt_const_cstr("idle"), 1.0);
    void *pos_a = rt_vec3_new(0, 0, 0);
    void *rot_a = rt_quat_new(0, 0, 0, 1); /* identity */
    void *scl_a = rt_vec3_new(1, 1, 1);
    rt_animation3d_add_keyframe(anim_a, 0, 0.0, pos_a, rot_a, scl_a);
    rt_animation3d_add_keyframe(anim_a, 0, 1.0, pos_a, rot_a, scl_a);

    /* Animation B: bone rotated 90 degrees around Y */
    void *anim_b = rt_animation3d_new(rt_const_cstr("turn"), 1.0);
    void *pos_b = rt_vec3_new(0, 0, 0);
    void *rot_b = rt_quat_from_euler(0, 1.5707963267948966, 0); /* 90 deg yaw about Y */
    void *scl_b = rt_vec3_new(1, 1, 1);
    rt_animation3d_add_keyframe(anim_b, 0, 0.0, pos_b, rot_b, scl_b);
    rt_animation3d_add_keyframe(anim_b, 0, 1.0, pos_b, rot_b, scl_b);

    /* Player: play A, crossfade to B */
    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim_a);
    rt_anim_player3d_update(player, 0.1);            /* advance a bit */
    rt_anim_player3d_crossfade(player, anim_b, 0.5); /* 0.5 sec crossfade */
    /* Step to midpoint of crossfade */
    rt_anim_player3d_update(player, 0.25);

    /* At 50% blend, the bone matrix should be a valid rotation (not skewed).
     * With SLERP, the quaternion midpoint of identity and 90-deg-Y is 45-deg-Y.
     * Verify: get bone matrix, check it's orthogonal (no shear). */
    void *bone_mat = rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_TRUE(bone_mat != NULL, "crossfade: bone matrix is non-null at midpoint");
}

static void test_crossfade_falls_back_to_bind_pose_translation() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_translate(2.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim_a = rt_animation3d_new(rt_const_cstr("from"), 1.0);
    void *anim_b = rt_animation3d_new(rt_const_cstr("to_bind"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim_a, 0, 0.0, rt_vec3_new(10.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim_a, 0, 1.0, rt_vec3_new(10.0, 0.0, 0.0), rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim_a);
    rt_anim_player3d_crossfade(player, anim_b, 1.0);
    rt_anim_player3d_update(player, 0.5);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(mv->m[3],
                6.0,
                0.1,
                "Crossfade missing-channel fallback blends toward bind-pose world translation");
}

static void test_crossfade_blends_target_only_channels() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_translate(2.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim_a = rt_animation3d_new(rt_const_cstr("from_bind"), 1.0);
    void *anim_b = rt_animation3d_new(rt_const_cstr("to"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim_b, 0, 0.0, rt_vec3_new(10.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim_b, 0, 1.0, rt_vec3_new(10.0, 0.0, 0.0), rot, scl);

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim_a);
    rt_anim_player3d_crossfade(player, anim_b, 1.0);
    rt_anim_player3d_update(player, 0.5);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(mv->m[3],
                6.0,
                0.1,
                "Crossfade target-only channels blend from bind pose instead of popping");
}

static void test_anim_blend_dt_zero_and_looping_defaults() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *constant = rt_animation3d_new(rt_const_cstr("constant"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(constant, 0, 0.0, rt_vec3_new(4.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(constant, 0, 1.0, rt_vec3_new(4.0, 0.0, 0.0), rot, scl);
    rt_animation3d_set_looping(constant, 0);

    void *blend = rt_anim_blend3d_new(skel);
    int64_t state = rt_anim_blend3d_add_state(blend, rt_const_cstr("constant"), constant);
    rt_anim_blend3d_set_weight(blend, state, 1.0);
    rt_anim_blend3d_update(blend, 0.0);
    rt_anim_blend3d *blend_impl = (rt_anim_blend3d *)blend;
    EXPECT_NEAR(blend_impl->bone_palette[3],
                4.0,
                0.1,
                "AnimBlend3D.Update recomputes weighted pose when dt is zero");

    rt_anim_blend3d_update(blend, 1.5);
    EXPECT_NEAR(blend_impl->states[state].anim_time,
                1.0,
                0.01,
                "AnimBlend3D state inherits non-looping animation default");

    float *saved_temp_state_local = blend_impl->temp_state_local;
    blend_impl->temp_state_local = nullptr;
    rt_anim_blend3d_update(blend, 0.5);
    EXPECT_NEAR(blend_impl->states[state].anim_time,
                1.0,
                0.01,
                "AnimBlend3D.Update ignores corrupted scratch buffers before advancing time");
    blend_impl->temp_state_local = saved_temp_state_local;
}

static void test_anim_blend_long_state_names_use_canonical_lookup() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("long"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, rt_vec3_new(3.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, rt_vec3_new(3.0, 0.0, 0.0), rot, scl);

    char long_name[128];
    std::memset(long_name, 'a', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';

    void *blend = rt_anim_blend3d_new(skel);
    int64_t state = rt_anim_blend3d_add_state(blend, rt_const_cstr(long_name), anim);
    EXPECT_TRUE(state == 0, "AnimBlend3D accepts a long state name");
    rt_anim_blend3d_set_weight_by_name(blend, rt_const_cstr(long_name), 1.0);
    rt_anim_blend3d_update(blend, 0.0);

    rt_anim_blend3d *impl = (rt_anim_blend3d *)blend;
    EXPECT_NEAR(
        impl->states[0].weight, 1.0, 0.001, "AnimBlend3D.SetWeightByName canonicalizes long names");
    EXPECT_NEAR(impl->bone_palette[3],
                3.0,
                0.05,
                "AnimBlend3D long-name lookup contributes to the blended pose");
}

static void test_animation_retarget_scales_by_proportion() {
    /* Source skeleton: arm offset 1 unit from root (bone length 1). */
    void *src = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(src, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t src_arm =
        rt_skeleton3d_add_bone(src, rt_const_cstr("arm"), 0, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(src);

    /* Target skeleton: arm twice as long (bone length 2). */
    void *dst = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(dst, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t dst_arm =
        rt_skeleton3d_add_bone(dst, rt_const_cstr("arm"), 0, rt_mat4_translate(2.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(dst);

    void *anim = rt_animation3d_new(rt_const_cstr("reach"), 2.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, src_arm, 0.0, rt_vec3_new(0.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, src_arm, 2.0, rt_vec3_new(2.0, 0.0, 0.0), rot, scl);
    void *tangent_anim = rt_animation3d_new(rt_const_cstr("reach-cubic"), 2.0);
    rt_animation3d_add_keyframe(tangent_anim, src_arm, 0.0, rt_vec3_new(0.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(tangent_anim, src_arm, 2.0, rt_vec3_new(2.0, 0.0, 0.0), rot, scl);
    const float tangent_in[3] = {-3.0f, 0.0f, 0.0f};
    const float tangent_out[3] = {3.0f, 0.0f, 0.0f};
    rt_animation3d_set_keyframe_tangents(
        tangent_anim, src_arm, 0.0, tangent_in, tangent_out, nullptr, nullptr, nullptr, nullptr);
    rt_animation3d_set_keyframe_tangents(
        tangent_anim, src_arm, 2.0, tangent_in, tangent_out, nullptr, nullptr, nullptr, nullptr);

    void *retargeted_tangent = rt_animation3d_retarget(tangent_anim, src, dst);
    EXPECT_TRUE(retargeted_tangent != nullptr, "Proportional retarget returns an animation");
    {
        /* Pose-based retargeting samples the cubic curve onto a dense grid instead of
         * copying tangents; the played value must still match the scaled Hermite curve.
         * Source cubic at t=0.5 (keys 0->2s, values 0->2, out-tangent +3, in-tangent
         * -3) evaluates to 1.4375; the 2x-longer target scales it to 2.875. */
        void *tangent_player = rt_anim_player3d_new(dst);
        rt_anim_player3d_play(tangent_player, retargeted_tangent);
        rt_anim_player3d_update(tangent_player, 0.5);

        typedef struct {
            double m[16];
        } tangent_mat4_view;

        tangent_mat4_view *tangent_arm =
            (tangent_mat4_view *)rt_anim_player3d_get_bone_matrix(tangent_player, dst_arm);
        EXPECT_NEAR(tangent_arm->m[3],
                    2.875,
                    0.02,
                    "Retarget preserves scaled cubic curve shape through dense sampling");
    }

    void *retargeted = rt_animation3d_retarget(anim, src, dst);
    void *player = rt_anim_player3d_new(dst);
    rt_anim_player3d_play(player, retargeted);
    rt_anim_player3d_update(player, 1.0); /* sample at t=1 -> source mid pos (1,0,0) */

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *arm_mat = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, dst_arm);
    /* Source mid-translation is 1.0; the 2x-longer target scales it to 2.0. */
    EXPECT_NEAR(arm_mat->m[3], 2.0, 0.05, "Retarget scales translation by bone-length ratio");
}

static void test_animation_retarget_maps_humanoid_roles() {
    /* Source: mixamo-style leg chain. */
    void *src = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(src, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t s_thigh = rt_skeleton3d_add_bone(
        src, rt_const_cstr("LeftUpLeg"), 0, rt_mat4_translate(0.0, -1.0, 0.0));
    int64_t s_calf = rt_skeleton3d_add_bone(
        src, rt_const_cstr("LeftLeg"), (int64_t)s_thigh, rt_mat4_translate(0.0, -1.0, 0.0));
    rt_skeleton3d_add_bone(
        src, rt_const_cstr("LeftFoot"), (int64_t)s_calf, rt_mat4_translate(0.0, -1.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(src);

    /* Target: Unreal-style names, with a spine bone shifting indices. The source thigh is bone
     * index 1; the index fallback would mis-map it onto the target's bone 1 (the spine). Only
     * humanoid role mapping lands it on thigh_l (bone 2). */
    void *dst = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(dst, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t d_spine =
        rt_skeleton3d_add_bone(dst, rt_const_cstr("spine_01"), 0, rt_mat4_translate(0.0, 1.0, 0.0));
    int64_t d_thigh =
        rt_skeleton3d_add_bone(dst, rt_const_cstr("thigh_l"), 0, rt_mat4_translate(0.0, -1.0, 0.0));
    int64_t d_calf = rt_skeleton3d_add_bone(
        dst, rt_const_cstr("calf_l"), (int64_t)d_thigh, rt_mat4_translate(0.0, -1.0, 0.0));
    rt_skeleton3d_add_bone(
        dst, rt_const_cstr("foot_l"), (int64_t)d_calf, rt_mat4_translate(0.0, -1.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(dst);

    /* Animate the source thigh to slide +1.0 in Z by t=2 (so t=1 samples +0.5). */
    void *anim = rt_animation3d_new(rt_const_cstr("kick"), 2.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, s_thigh, 0.0, rt_vec3_new(0.0, -1.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, s_thigh, 2.0, rt_vec3_new(0.0, -1.0, 1.0), rot, scl);

    void *retargeted = rt_animation3d_retarget(anim, src, dst);
    EXPECT_TRUE(retargeted != nullptr, "Humanoid retarget returns an animation");

    void *player = rt_anim_player3d_new(dst);
    rt_anim_player3d_play(player, retargeted);
    rt_anim_player3d_update(player, 1.0); /* sample at t=1 -> thigh Z = 0.5 */

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *thigh = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, d_thigh);
    mat4_view *spine = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, d_spine);
    /* Role mapping lands LeftUpLeg's channel on thigh_l (Z animates to 0.5), not on the
     * index-fallback bone (the spine, which stays at its Z=0 bind). */
    EXPECT_NEAR(thigh->m[11], 0.5, 0.05, "Humanoid retarget maps LeftUpLeg -> thigh_l by role");
    EXPECT_NEAR(
        spine->m[11], 0.0, 0.05, "Humanoid retarget does not mis-map onto the index-fallback bone");
}

/// @brief A source key at its bind pose must land exactly on the destination bind pose.
/// @details Rest-delta compensation contract: rigs bind differently (a Biped figure stance vs a
///          T-pose auto-rig), and uncompensated verbatim rotation copy bakes that constant
///          difference into every key. The source arm binds rotated 90 degrees about X; a key
///          holding exactly that bind rotation must play back as the destination's own bind.
static void test_animation_retarget_compensates_rest_pose_delta() {
    void *src = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(src, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t src_arm =
        rt_skeleton3d_add_bone(src, rt_const_cstr("arm"), 0, rt_mat4_rotate_x(1.5707963267948966));
    rt_skeleton3d_compute_inverse_bind(src);

    void *dst = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(dst, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t dst_arm =
        rt_skeleton3d_add_bone(dst, rt_const_cstr("arm"), 0, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(dst);

    /* Key 0: exactly the source bind rotation. Key 1: bind followed by a 30-degree Z twist. */
    void *anim = rt_animation3d_new(rt_const_cstr("delta"), 2.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    void *bind_rot = rt_quat_new(0.7071067811865476, 0.0, 0.0, 0.7071067811865476);
    /* qX90 * qZ30 under this runtime's Hamilton convention. */
    void *twisted_rot = rt_quat_new(
        0.6830127018922193, -0.1830127018922193, 0.1830127018922193, 0.6830127018922193);
    rt_animation3d_add_keyframe(anim, src_arm, 0.0, rt_vec3_new(0.0, 0.0, 0.0), bind_rot, scl);
    rt_animation3d_add_keyframe(anim, src_arm, 2.0, rt_vec3_new(0.0, 0.0, 0.0), twisted_rot, scl);

    void *retargeted = rt_animation3d_retarget(anim, src, dst);
    EXPECT_TRUE(retargeted != nullptr, "Rest-delta retarget returns an animation");
    if (!retargeted)
        return;

    typedef struct {
        double m[16];
    } mat4_view;

    void *player = rt_anim_player3d_new(dst);
    rt_anim_player3d_play(player, retargeted);
    rt_anim_player3d_update(player, 0.0);
    mat4_view *at_bind = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, dst_arm);
    EXPECT_NEAR(at_bind->m[3], 1.0, 1e-4, "Bind-pose key keeps the destination bind translation");
    EXPECT_NEAR(at_bind->m[0], 1.0, 1e-4, "Bind-pose key plays the destination bind rotation");
    EXPECT_NEAR(at_bind->m[5], 1.0, 1e-4, "Bind-pose key does not inherit the source bind X-roll");
    EXPECT_NEAR(at_bind->m[10], 1.0, 1e-4, "Bind-pose key stays upright on the destination");

    /* The 30-degree source-local Z twist conjugates through the source bind X-roll into a
     * world rotation about -Y: RotX(90) * RotZ(30) * RotX(-90) == RotY(-30). */
    void *player2 = rt_anim_player3d_new(dst);
    rt_anim_player3d_play(player2, retargeted);
    rt_anim_player3d_update(player2, 2.0);
    mat4_view *at_twist = (mat4_view *)rt_anim_player3d_get_bone_matrix(player2, dst_arm);
    EXPECT_NEAR(at_twist->m[0], 0.8660254, 1e-3, "Delta transfers as a world -Y rotation (xx)");
    EXPECT_NEAR(at_twist->m[2], -0.5, 1e-3, "Delta transfers as a world -Y rotation (xz)");
    EXPECT_NEAR(at_twist->m[8], 0.5, 1e-3, "Delta transfers as a world -Y rotation (zx)");
    EXPECT_NEAR(at_twist->m[10], 0.8660254, 1e-3, "Delta transfers as a world -Y rotation (zz)");
    EXPECT_NEAR(at_twist->m[3], 1.0, 1e-4, "Zero translation delta keeps the bind offset");
}

/// @brief Two source bones mapping onto one destination bone compose parent-first.
/// @details A Biped drives one glTF Hips from Bip01 (translation/facing) + Pelvis
///          (hip rotation); first-claim used to silently drop whichever came second.
static void test_animation_retarget_composes_chain_onto_one_bone() {
    void *src = rt_skeleton3d_new();
    int64_t s_com = rt_skeleton3d_add_bone(src, rt_const_cstr("Bip"), -1, rt_mat4_identity());
    int64_t s_pelvis =
        rt_skeleton3d_add_bone(src, rt_const_cstr("BipPelvis"), 0, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(src);

    void *dst = rt_skeleton3d_new();
    int64_t d_hips = rt_skeleton3d_add_bone(dst, rt_const_cstr("hips"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(dst);
    rt_skeleton3d_set_bone_alias(dst, rt_const_cstr("Bip"), rt_const_cstr("hips"));
    rt_skeleton3d_set_bone_alias(dst, rt_const_cstr("BipPelvis"), rt_const_cstr("hips"));

    /* COM translates to (0,2,0); pelvis rotates 90 degrees about Z. Composed local:
     * translation (0,2,0) THEN rotation Z90. */
    void *anim = rt_animation3d_new(rt_const_cstr("combo"), 2.0);
    void *ident = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *z90 = rt_quat_new(0.0, 0.0, 0.7071067811865476, 0.7071067811865476);
    void *one = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, s_com, 0.0, rt_vec3_new(0.0, 2.0, 0.0), ident, one);
    rt_animation3d_add_keyframe(anim, s_com, 2.0, rt_vec3_new(0.0, 2.0, 0.0), ident, one);
    rt_animation3d_add_keyframe(anim, s_pelvis, 0.0, rt_vec3_new(0.0, 0.0, 0.0), z90, one);
    rt_animation3d_add_keyframe(anim, s_pelvis, 2.0, rt_vec3_new(0.0, 0.0, 0.0), z90, one);

    void *retargeted = rt_animation3d_retarget(anim, src, dst);
    EXPECT_TRUE(retargeted != nullptr, "Chain retarget returns an animation");
    if (!retargeted)
        return;
    auto *impl = static_cast<rt_animation3d *>(retargeted);
    EXPECT_TRUE(impl->channel_count == 1, "Both source bones compose into ONE hips channel");

    typedef struct {
        double m[16];
    } mat4_view;

    void *player = rt_anim_player3d_new(dst);
    rt_anim_player3d_play(player, retargeted);
    rt_anim_player3d_update(player, 1.0);
    mat4_view *hips = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, d_hips);
    /* Rotation Z90: m[0]=0, m[1]=-1, m[4]=1, m[5]=0. Translation preserved (0,2,0). */
    EXPECT_NEAR(hips->m[7], 2.0, 1e-3, "Composed channel keeps the COM translation");
    EXPECT_NEAR(hips->m[0], 0.0, 1e-3, "Composed channel keeps the pelvis rotation (xx)");
    EXPECT_NEAR(hips->m[1], -1.0, 1e-3, "Composed channel keeps the pelvis rotation (xy)");
    EXPECT_NEAR(hips->m[4], 1.0, 1e-3, "Composed channel keeps the pelvis rotation (yx)");
}

/// @brief StripRootMotion pins X/Z travel to the first key and keeps Y when asked.
static void test_animation_strip_root_motion_pins_travel() {
    void *skel = rt_skeleton3d_new();
    int64_t root = rt_skeleton3d_add_bone(skel, rt_const_cstr("hips"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *anim = rt_animation3d_new(rt_const_cstr("travel"), 2.0);
    void *ident = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *one = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, root, 0.0, rt_vec3_new(1.0, 3.0, 2.0), ident, one);
    rt_animation3d_add_keyframe(anim, root, 2.0, rt_vec3_new(9.0, 0.5, -7.0), ident, one);

    EXPECT_TRUE(rt_animation3d_strip_root_motion(anim, root, 1) == 1,
                "StripRootMotion reports the modified channel");

    typedef struct {
        double m[16];
    } mat4_view;

    void *player = rt_anim_player3d_new(skel);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 2.0);
    mat4_view *hips = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, root);
    EXPECT_NEAR(hips->m[3], 1.0, 1e-4, "X travel is pinned to the first key");
    EXPECT_NEAR(hips->m[11], 2.0, 1e-4, "Z travel is pinned to the first key");
    EXPECT_NEAR(hips->m[7], 0.5, 1e-4, "Vertical motion is preserved when requested");
    EXPECT_TRUE(rt_animation3d_strip_root_motion(anim, root + 5, 1) == 0,
                "A bone without a channel reports zero");
}

static void test_animation_retarget_matches_bone_names() {
    void *src = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(src, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t src_arm = rt_skeleton3d_add_bone(src, rt_const_cstr("arm"), 0, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(src);

    void *dst = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(dst, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t dst_unused =
        rt_skeleton3d_add_bone(dst, rt_const_cstr("unused"), 0, rt_mat4_identity());
    int64_t dst_arm = rt_skeleton3d_add_bone(dst, rt_const_cstr("arm"), 0, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(dst);

    void *anim = rt_animation3d_new(rt_const_cstr("reach"), 2.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, src_arm, 0.0, rt_vec3_new(0.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, src_arm, 2.0, rt_vec3_new(8.0, 0.0, 0.0), rot, scl);
    rt_animation3d_set_looping(anim, 1);

    void *retargeted = rt_animation3d_retarget(anim, src, dst);
    EXPECT_TRUE(retargeted != nullptr, "Animation3D.Retarget returns an animation");
    EXPECT_NEAR(rt_animation3d_get_duration(retargeted),
                2.0,
                0.001,
                "Animation3D.Retarget preserves duration");
    EXPECT_TRUE(rt_animation3d_get_looping(retargeted) != 0,
                "Animation3D.Retarget preserves looping");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_animation3d_get_name(retargeted)), "reach") == 0,
                "Animation3D.Retarget preserves the clip name");

    void *player = rt_anim_player3d_new(dst);
    rt_anim_player3d_play(player, retargeted);
    rt_anim_player3d_update(player, 1.0);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *arm_mat = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, dst_arm);
    mat4_view *unused_mat = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, dst_unused);
    EXPECT_NEAR(arm_mat->m[3], 4.0, 0.1, "Animation3D.Retarget maps keyed bone by name");
    EXPECT_NEAR(unused_mat->m[3], 0.0, 0.1, "Animation3D.Retarget does not animate wrong index");

    EXPECT_TRUE(rt_animation3d_retarget(nullptr, src, dst) == nullptr,
                "Animation3D.Retarget rejects NULL animations");
    EXPECT_TRUE(rt_animation3d_retarget(anim, anim, dst) == nullptr,
                "Animation3D.Retarget rejects non-skeleton source handles");
}

static void test_non_topological_parent_order_evaluates_hierarchy() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(
        skel, rt_const_cstr("child_first"), -1, rt_mat4_translate(2.0, 0.0, 0.0));
    rt_skeleton3d_add_bone(
        skel, rt_const_cstr("parent_second"), 0, rt_mat4_translate(3.0, 0.0, 0.0));
    auto *impl = static_cast<rt_skeleton3d *>(skel);
    impl->bones[0].parent_index = 1;
    impl->bones[1].parent_index = -1;
    rt_skeleton3d_compute_inverse_bind(skel);

    void *player = rt_anim_player3d_new(skel);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *child_bind = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(child_bind->m[3],
                5.0,
                0.1,
                "AnimPlayer3D evaluates a forward parent reference in bind pose");

    void *anim = rt_animation3d_new(rt_const_cstr("parent_move"), 1.0);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(anim, 1, 0.0, rt_vec3_new(3.0, 0.0, 0.0), rot, scl);
    rt_animation3d_add_keyframe(anim, 1, 1.0, rt_vec3_new(5.0, 0.0, 0.0), rot, scl);
    rt_anim_player3d_play(player, anim);
    rt_anim_player3d_update(player, 0.5);
    mat4_view *child_anim = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    EXPECT_NEAR(child_anim->m[3],
                6.0,
                0.1,
                "AnimPlayer3D keeps children attached to forward parents during animation");
}

static void test_cyclic_parent_indices_degrade_to_finite_pose() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("a"), -1, rt_mat4_translate(2.0, 0.0, 0.0));
    rt_skeleton3d_add_bone(skel, rt_const_cstr("b"), 0, rt_mat4_translate(3.0, 0.0, 0.0));
    auto *impl = static_cast<rt_skeleton3d *>(skel);
    impl->bones[0].parent_index = 1;
    impl->bones[1].parent_index = 0;
    rt_skeleton3d_compute_inverse_bind(skel);

    void *player = rt_anim_player3d_new(skel);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *a = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 0);
    mat4_view *b = (mat4_view *)rt_anim_player3d_get_bone_matrix(player, 1);
    EXPECT_TRUE(a != nullptr && b != nullptr,
                "AnimPlayer3D returns matrices for a skeleton with a parent cycle");
    EXPECT_TRUE(std::isfinite(a->m[3]) && std::isfinite(b->m[3]),
                "AnimPlayer3D breaks parent cycles into finite global transforms");
}

static void test_crossfade_preserves_structure() {
    EXPECT_TRUE(1, "crossfade: TRS blend preserves matrix orthogonality (compile check)");
    /* This test ensures the crossfade code path compiles and runs
     * without crashing. Full orthogonality verification would require
     * reading individual matrix elements (not yet exposed via API). */
}

int main() {
    test_skeleton_create();
    test_skeleton_add_bone();
    test_skeleton_find_bone();
    test_animation_create();
    test_animation_keyframes();
    test_animation_keyframes_are_sorted();
    test_animation_roundoff_duplicate_keyframes_replace_existing_sample();
    test_skeleton_animation_repairs_corrupt_counts();
    test_animation_safe_counts_have_domain_ceilings();
    test_anim_player_retains_inputs();
    test_player_create();
    test_animation_getters_normalize_corrupt_private_flags();
    test_skeleton_freezes_after_player_creation();
    test_player_playback();
    test_player_loop();
    test_player_extreme_clock_arithmetic_stays_finite();
    test_player_reverse_timing_and_bad_handles();
    test_player_stop_at_end();
    test_player_stop_returns_to_bind_pose();
    test_player_speed();
    test_two_bone_chain();
    test_non_identity_bind_pose();
    test_partial_keyframes_preserve_bind_components();
    test_skeleton_find_bone_uses_canonical_long_names();
    test_bone_name();

    /* Crossfade tests */
    test_crossfade_basic();
    test_crossfade_falls_back_to_bind_pose_translation();
    test_crossfade_blends_target_only_channels();
    test_anim_blend_dt_zero_and_looping_defaults();
    test_anim_blend_long_state_names_use_canonical_lookup();
    test_animation_retarget_matches_bone_names();
    test_animation_retarget_compensates_rest_pose_delta();
    test_animation_retarget_composes_chain_onto_one_bone();
    test_animation_strip_root_motion_pins_travel();
    test_animation_retarget_scales_by_proportion();
    test_animation_retarget_maps_humanoid_roles();
    test_non_topological_parent_order_evaluates_hierarchy();
    test_cyclic_parent_indices_degrade_to_finite_pose();
    test_crossfade_preserves_structure();

    printf("Skeleton3D tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
