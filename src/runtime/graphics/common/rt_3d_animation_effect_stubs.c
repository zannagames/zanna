//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/common/rt_3d_animation_effect_stubs.c
// Purpose: Graphics-disabled animation controller, decal, sprite, and atlas entry points.
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Stateful graphics APIs fail through the shared unavailable-backend policy.
// Ownership/Lifetime: Stub entry points allocate no graphics resources or retained handles.
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h,
//        src/runtime/graphics/common/rt_3d_asset_stubs.c
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"

/* AnimBlend3D stubs */

/// @brief Stub for `AnimBlend3D.New` — would normally create a
///        weighted-blend animation tree for the given Skeleton3D.
///        Distinct from `AnimController3D` (which switches between
///        discrete states); blends combine multiple animations
///        simultaneously with per-state weights.
///
/// Silent stub returning NULL.
///
/// @param s Skeleton3D handle (ignored).
///
/// @return `NULL`.
void *rt_anim_blend3d_new(void *s) {
    (void)s;
    return NULL;
}

/// @brief Stub for `AnimBlend3D.AddState` — register a named state
///        backed by an Animation3D. Returns the assigned state index.
///
/// Silent stub returning `-1`.
///
/// @param b AnimBlend3D handle (ignored).
/// @param n State name (ignored).
/// @param a Animation3D handle (ignored).
///
/// @return `-1`.
int64_t rt_anim_blend3d_add_state(void *b, rt_string n, void *a) {
    (void)b;
    (void)n;
    (void)a;
    return -1;
}

/// @brief Stub for `AnimBlend3D.SetWeight` — set the contribution of
///        state `s` to the final pose. Weights across all states should
///        sum to 1.0 for normalized blending; the renderer doesn't
///        enforce this.
///
/// Silent no-op stub.
///
/// @param b AnimBlend3D handle (ignored).
/// @param s State index from `AddState` (ignored).
/// @param w Blend weight 0..1 (ignored).
void rt_anim_blend3d_set_weight(void *b, int64_t s, double w) {
    (void)b;
    (void)s;
    (void)w;
}

/// @brief Stub for `AnimBlend3D.SetWeightByName` — convenience wrapper
///        around `SetWeight` that looks up the state index by name.
///
/// Silent no-op stub.
///
/// @param b AnimBlend3D handle (ignored).
/// @param n State name (ignored).
/// @param w Blend weight 0..1 (ignored).
void rt_anim_blend3d_set_weight_by_name(void *b, rt_string n, double w) {
    (void)b;
    (void)n;
    (void)w;
}

/// @brief Stub for `AnimBlend3D.Weight` — get the current blend weight
///        of state `s`.
///
/// Silent stub returning `0.0`.
///
/// @param b AnimBlend3D handle (ignored).
/// @param s State index (ignored).
///
/// @return `0.0`.
double rt_anim_blend3d_get_weight(void *b, int64_t s) {
    (void)b;
    (void)s;
    return 0.0;
}

/// @brief Stub for `AnimBlend3D.SetSpeed` — per-state playback speed
///        multiplier. Each state in the blend tree advances independently
///        at its own rate; useful for blending walk/run cycles whose
///        natural durations differ.
///
/// Silent no-op stub.
///
/// @param b  AnimBlend3D handle (ignored).
/// @param s  State index (ignored).
/// @param sp Speed multiplier (ignored).
void rt_anim_blend3d_set_speed(void *b, int64_t s, double sp) {
    (void)b;
    (void)s;
    (void)sp;
}

/// @brief Stub for `AnimBlend3D.Update` — advance every state's playback
///        clock by `dt` (scaled by the state's per-state speed) and
///        recompute the blended pose.
///
/// Silent no-op stub.
///
/// @param b  AnimBlend3D handle (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_anim_blend3d_update(void *b, double dt) {
    (void)b;
    (void)dt;
}

/// @brief Stub for `AnimBlend3D.StateCount` — number of registered
///        blend states.
///
/// Silent stub returning `0`.
///
/// @param b AnimBlend3D handle (ignored).
///
/// @return `0`.
int64_t rt_anim_blend3d_state_count(void *b) {
    (void)b;
    return 0;
}

/* BlendTree3D stubs */

/// @brief Return no 1D blend tree in a graphics-disabled build.
/// @param skeleton Skeleton3D handle (ignored).
/// @return Always NULL.
void *rt_blend_tree3d_new_1d(void *skeleton) {
    (void)skeleton;
    return NULL;
}

/// @brief Return no 2D blend tree in a graphics-disabled build.
/// @param skeleton Skeleton3D handle (ignored).
/// @return Always NULL.
void *rt_blend_tree3d_new_2d(void *skeleton) {
    (void)skeleton;
    return NULL;
}

/// @brief Reject adding an animation sample to an unavailable blend tree.
/// @param tree BlendTree3D handle (ignored).
/// @param animation Animation3D handle (ignored).
/// @param x Sample x coordinate (ignored).
/// @param y Sample y coordinate (ignored).
/// @return Always `-1`.
/// @details Silent stub: graphics-disabled builds cannot retain blend samples.
int64_t rt_blend_tree3d_add_sample(void *tree, void *animation, double x, double y) {
    (void)tree;
    (void)animation;
    (void)x;
    (void)y;
    return -1;
}

/// @brief Ignore a blend-tree parameter update.
/// @param tree BlendTree3D handle (ignored).
/// @param x Blend parameter x coordinate (ignored).
/// @param y Blend parameter y coordinate (ignored).
/// @details No-op stub: graphics-disabled builds have no blend state to update.
void rt_blend_tree3d_set_param(void *tree, double x, double y) {
    (void)tree;
    (void)x;
    (void)y;
}

/// @brief Ignore a blend-tree time update.
/// @param tree BlendTree3D handle (ignored).
/// @param dt Elapsed time (ignored).
/// @details No-op stub: graphics-disabled builds have no blend clock to advance.
void rt_blend_tree3d_update(void *tree, double dt) {
    (void)tree;
    (void)dt;
}

/// @brief Return the neutral blend-tree sample count.
/// @param tree BlendTree3D handle (ignored).
/// @return Always `0`.
int64_t rt_blend_tree3d_get_sample_count(void *tree) {
    (void)tree;
    return 0;
}

/// @brief Return no computed blend pose.
/// @param tree BlendTree3D handle (ignored).
/// @return Always NULL.
void *rt_blend_tree3d_get_blend(void *tree) {
    (void)tree;
    return NULL;
}

/* IKSolver3D stubs */

/// @brief Return no two-bone IK solver in a graphics-disabled build.
/// @param skeleton Skeleton3D handle (ignored).
/// @param root Root bone index (ignored).
/// @param mid Middle bone index (ignored).
/// @param end End-effector bone index (ignored).
/// @return Always NULL.
void *rt_ik_solver3d_two_bone(void *skeleton, int64_t root, int64_t mid, int64_t end) {
    (void)skeleton;
    (void)root;
    (void)mid;
    (void)end;
    return NULL;
}

/// @brief Return no look-at IK solver.
/// @param skeleton Skeleton3D handle (ignored).
/// @param bone Driven bone index (ignored).
/// @return Always NULL.
void *rt_ik_solver3d_look_at(void *skeleton, int64_t bone) {
    (void)skeleton;
    (void)bone;
    return NULL;
}

/// @brief Return no FABRIK IK solver.
/// @param skeleton Skeleton3D handle (ignored).
/// @param chain Bone-index chain (ignored).
/// @return Always NULL.
void *rt_ik_solver3d_fabrik(void *skeleton, void *chain) {
    (void)skeleton;
    (void)chain;
    return NULL;
}

/// @brief Ignore an IK target update.
/// @param solver IKSolver3D handle (ignored).
/// @param target Vec3 or Transform target (ignored).
void rt_ik_solver3d_set_target(void *solver, void *target) {
    (void)solver;
    (void)target;
}

/// @brief Ignore an IK blend-weight update.
/// @param solver IKSolver3D handle (ignored).
/// @param weight Requested solver weight (ignored).
void rt_ik_solver3d_set_weight(void *solver, double weight) {
    (void)solver;
    (void)weight;
}

/// @brief Ignore an IK pole-vector update.
/// @param solver IKSolver3D handle (ignored).
/// @param pole Vec3 pole target (ignored).
void rt_ik_solver3d_set_pole(void *solver, void *pole) {
    (void)solver;
    (void)pole;
}

/// @brief Ignore an IK solve request.
/// @param solver IKSolver3D handle (ignored).
void rt_ik_solver3d_solve(void *solver) {
    (void)solver;
}

/// @brief Return no skeleton from an unavailable IK solver.
/// @param solver IKSolver3D handle (ignored).
/// @return Always NULL.
void *rt_ik_solver3d_get_skeleton(void *solver) {
    (void)solver;
    return NULL;
}

/// @brief Report that an unavailable IK solver cannot modify a pose.
/// @param solver IKSolver3D handle (ignored).
/// @param locals Local-pose matrix buffer (ignored).
/// @param globals Global-pose matrix buffer (ignored).
/// @param bone_count Number of bones in both buffers (ignored).
/// @return Always `0`.
int8_t rt_ik_solver3d_apply_to_pose(void *solver,
                                    float *locals,
                                    float *globals,
                                    int32_t bone_count) {
    (void)solver;
    (void)locals;
    (void)globals;
    (void)bone_count;
    return 0;
}

/* AnimController3D stubs */

/// @brief Stub for `AnimController3D.New` — would normally create a
///        named-state animation controller bound to the given Skeleton3D.
///        States hold Animation3D references; transitions define crossfade
///        durations between states.
///
/// Trapping stub: controllers are bound to scene nodes via `BindAnimator`
/// and would crash later if NULL.
///
/// @param s Skeleton3D handle (ignored).
///
/// @return Never returns normally.
void *rt_anim_controller3d_new(void *s) {
    (void)s;
    rt_graphics_unavailable_("AnimController3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `AnimController3D.AddState` — register a named state
///        backed by an Animation3D. Returns the assigned state index, or
///        `-1` on failure (duplicate name, NULL animation).
///
/// Silent stub returning `-1`.
///
/// @param c AnimController3D handle (ignored).
/// @param n State name (ignored).
/// @param a Animation3D handle (ignored).
///
/// @return `-1`.
int64_t rt_anim_controller3d_add_state(void *c, rt_string n, void *a) {
    (void)c;
    (void)n;
    (void)a;
    return -1;
}

/// @brief Stub for `AnimController3D.AddTransition` — define a named
///        transition between states `f` and `t` with crossfade duration
///        `d` seconds. Used so `Crossfade(name)` knows how long to blend.
///
/// Silent stub returning `0` (failure).
///
/// @param c AnimController3D handle (ignored).
/// @param f From-state name (ignored).
/// @param t To-state name (ignored).
/// @param d Crossfade duration in seconds (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_add_transition(void *c, rt_string f, rt_string t, double d) {
    (void)c;
    (void)f;
    (void)t;
    (void)d;
    return 0;
}

/// @brief Stub for `AnimController3D.Play` — switch immediately to the
///        named state (no crossfade, instant pose snap). Returns 1 on
///        success, 0 if the state name is unknown.
///
/// Silent stub returning `0`.
///
/// @param c AnimController3D handle (ignored).
/// @param n State name (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_play(void *c, rt_string n) {
    (void)c;
    (void)n;
    return 0;
}

/// @brief Stub for `AnimController3D.Crossfade` — blend into the named
///        state over `d` seconds. The previous state continues to drive
///        the pose (with diminishing weight) until the blend completes.
///
/// Silent stub returning `0`.
///
/// @param c AnimController3D handle (ignored).
/// @param n Target state name (ignored).
/// @param d Crossfade duration in seconds (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_crossfade(void *c, rt_string n, double d) {
    (void)c;
    (void)n;
    (void)d;
    return 0;
}

/// @brief Stub for `AnimController3D.Stop` — halt animation playback.
///        The skeleton freezes at the current pose.
///
/// Silent no-op stub.
///
/// @param c AnimController3D handle (ignored).
void rt_anim_controller3d_stop(void *c) {
    (void)c;
}

/// @brief Stub for `AnimController3D.Update` — advance the controller
///        by `dt` seconds: progress the active state's animation,
///        advance crossfade blend, fire event-frame callbacks.
///
/// Silent no-op stub.
///
/// @param c  AnimController3D handle (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_anim_controller3d_update(void *c, double dt) {
    (void)c;
    (void)dt;
}

/// @brief Stub for `AnimController3D.CurrentState` — get the name of
///        the state currently driving the pose. During a crossfade this
///        is the destination state.
///
/// Silent stub returning NULL.
///
/// @param c AnimController3D handle (ignored).
///
/// @return `NULL`.
rt_string rt_anim_controller3d_get_current_state(void *c) {
    (void)c;
    return NULL;
}

/// @brief Stub for `AnimController3D.PreviousState` — get the name of
///        the state that was previously active (the source of the most
///        recent crossfade).
///
/// Silent stub returning NULL.
///
/// @param c AnimController3D handle (ignored).
///
/// @return `NULL`.
rt_string rt_anim_controller3d_get_previous_state(void *c) {
    (void)c;
    return NULL;
}

/// @brief Stub for `AnimController3D.IsTransitioning` — true while a
///        crossfade is in progress (between `Crossfade` start and the
///        end of its duration).
///
/// Silent stub returning `0`.
///
/// @param c AnimController3D handle (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_get_is_transitioning(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `AnimController3D.StateCount` — number of registered
///        states.
///
/// Silent stub returning `0`.
///
/// @param c AnimController3D handle (ignored).
///
/// @return `0`.
int64_t rt_anim_controller3d_get_state_count(void *c) {
    (void)c;
    return 0;
}

/// @brief Stub for `AnimController3D.SetStateSpeed` — per-state playback
///        speed multiplier. `1.0` is normal; values <1 slow the state
///        down, >1 speed it up.
///
/// Silent no-op stub.
///
/// @param c AnimController3D handle (ignored).
/// @param n State name (ignored).
/// @param s Speed multiplier (ignored).
void rt_anim_controller3d_set_state_speed(void *c, rt_string n, double s) {
    (void)c;
    (void)n;
    (void)s;
}

/// @brief Stub for `AnimController3D.SetStateLooping` — per-state
///        looping flag. Disabled states play once and stop at the last
///        frame; enabled states wrap around.
///
/// Silent no-op stub.
///
/// @param c AnimController3D handle (ignored).
/// @param n State name (ignored).
/// @param l Non-zero to enable looping (ignored).
void rt_anim_controller3d_set_state_looping(void *c, rt_string n, int8_t l) {
    (void)c;
    (void)n;
    (void)l;
}

/// @brief Stub for `AnimController3D.SetAnimationLOD` — configure a
///        lower update rate for distant or low-priority controllers.
///
/// Silent no-op stub.
///
/// @param c AnimController3D handle (ignored).
/// @param d Distance marker in world units (ignored).
/// @param r Update rate in Hz (ignored).
void rt_anim_controller3d_set_animation_lod(void *c, double d, double r) {
    (void)c;
    (void)d;
    (void)r;
}

/// @brief Stub for `AnimController3D.SetBlendTree` — would normally use a
///        BlendTree3D as the controller's base pose source.
///
/// Silent stub returning `0`.
///
/// @param c AnimController3D handle (ignored).
/// @param t BlendTree3D handle (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_set_blend_tree(void *c, void *t) {
    (void)c;
    (void)t;
    return 0;
}

/// @brief Stub for `AnimController3D.SetIKSolver` — would normally apply an
///        IKSolver3D after controller layers and before skinning.
///
/// Silent stub returning `0`.
///
/// @param c AnimController3D handle (ignored).
/// @param s IKSolver3D handle to apply (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_set_ik_solver(void *c, void *s) {
    (void)c;
    (void)s;
    return 0;
}

/// @brief Stub for `AnimController3D.AddIKSolver`.
int8_t rt_anim_controller3d_add_ik_solver(void *c, void *s) {
    (void)c;
    (void)s;
    return 0;
}

/// @brief Stub for `AnimController3D.AddEvent` — register a tagged
///        event frame `e` at time `t` within state `s`. When playback
///        crosses time `t`, the event is queued for `PollEvent`.
///
/// Silent no-op stub. Used for triggering footstep SFX, weapon-swing
/// hit windows, particle spawns synced to animation.
///
/// @param c AnimController3D handle (ignored).
/// @param s State name (ignored).
/// @param t Event time within state in seconds (ignored).
/// @param e Event tag string (ignored).
void rt_anim_controller3d_add_event(void *c, rt_string s, double t, rt_string e) {
    (void)c;
    (void)s;
    (void)t;
    (void)e;
}

/// @brief Stub for `AnimController3D.PollEvent` — dequeue the next
///        pending event tag, or NULL if no events have fired since the
///        last call. Drains one event at a time so callers can loop.
///
/// Silent stub returning NULL.
///
/// @param c AnimController3D handle (ignored).
///
/// @return `NULL`.
rt_string rt_anim_controller3d_poll_event(void *c) {
    (void)c;
    return NULL;
}

/// @brief Stub for `AnimController3D.SetRootMotionBone` — designate a
///        bone whose displacement is tracked separately as "root motion"
///        rather than being applied to the rendered pose. Use for
///        animation-driven character locomotion.
///
/// Silent no-op stub.
///
/// @param c AnimController3D handle (ignored).
/// @param b Bone index in the bound skeleton (ignored).
void rt_anim_controller3d_set_root_motion_bone(void *c, int64_t b) {
    (void)c;
    (void)b;
}

/// @brief Stub for `AnimController3D.RootMotionDelta` — get the
///        accumulated root-motion translation since the last
///        `ConsumeRootMotion` call as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param c AnimController3D handle (ignored).
///
/// @return `NULL`.
void *rt_anim_controller3d_get_root_motion_delta(void *c) {
    (void)c;
    return NULL;
}

/// @brief Stub for `AnimController3D.ConsumeRootMotion` — read and
///        zero the root-motion accumulator in one operation. Pattern:
///        gameplay code calls this once per tick to translate the
///        character body, then continues using the bone-driven pose.
///
/// Silent stub returning NULL.
///
/// @param c AnimController3D handle (ignored).
///
/// @return `NULL`.
void *rt_anim_controller3d_consume_root_motion(void *c) {
    (void)c;
    return NULL;
}

/// @brief Stub for `AnimController3D.SetLayerWeight` — per-layer blend
///        weight, 0..1. Layers compose additively so layer 0 is typically
///        "full body" at weight 1.0 and additional layers are partial-
///        body overlays (e.g. upper-body shoot pose blended over locomotion).
///
/// Silent no-op stub.
///
/// @param c AnimController3D handle (ignored).
/// @param l Layer index, 0..LayerCount-1 (ignored).
/// @param w Layer weight, 0..1 (ignored).
void rt_anim_controller3d_set_layer_weight(void *c, int64_t l, double w) {
    (void)c;
    (void)l;
    (void)w;
}

/// @brief Stub for `AnimController3D.SetLayerMask` — per-layer bone
///        bitmask. Only bones whose index is set in `b` are affected
///        by this layer — useful for upper-body / lower-body splits.
///
/// Silent no-op stub.
///
/// @param c AnimController3D handle (ignored).
/// @param l Layer index (ignored).
/// @param b Bitmask of affected bone indices (ignored).
void rt_anim_controller3d_set_layer_mask(void *c, int64_t l, int64_t b) {
    (void)c;
    (void)l;
    (void)b;
}

/// @brief Stub for `AnimController3D.PlayLayer` — instantly switch
///        layer `l` to state `s`. Per-layer `Play` allows independent
///        upper-body and lower-body animations.
///
/// Silent stub returning `0` (failure).
///
/// @param c AnimController3D handle (ignored).
/// @param l Layer index, 0..LayerCount-1 (ignored).
/// @param s State name (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_play_layer(void *c, int64_t l, rt_string s) {
    (void)c;
    (void)l;
    (void)s;
    return 0;
}

/// @brief Stub for `AnimController3D.PlayLayerAdditive` — instantly switch
///        layer `l` to state `s` and compose it as a bind-pose delta.
///
/// Silent stub returning `0` (failure).
///
/// @param c AnimController3D handle (ignored).
/// @param l Layer index (ignored).
/// @param s State name (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_play_layer_additive(void *c, int64_t l, rt_string s) {
    (void)c;
    (void)l;
    (void)s;
    return 0;
}

/// @brief Stub for `AnimController3D.CrossfadeLayer` — blend layer `l`
///        toward state `s` over `d` seconds. Each layer maintains its
///        own crossfade clock independent of other layers.
///
/// Silent stub returning `0`.
///
/// @param c AnimController3D handle (ignored).
/// @param l Layer index (ignored).
/// @param s Target state name (ignored).
/// @param d Crossfade duration in seconds (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_crossfade_layer(void *c, int64_t l, rt_string s, double d) {
    (void)c;
    (void)l;
    (void)s;
    (void)d;
    return 0;
}

/// @brief Stub for `AnimController3D.CrossfadeLayerAdditive` — blend layer `l`
///        toward state `s` over `d` seconds and compose it as a bind-pose delta.
///
/// Silent stub returning `0`.
///
/// @param c AnimController3D handle (ignored).
/// @param l Layer index (ignored).
/// @param s Target state name (ignored).
/// @param d Crossfade duration in seconds (ignored).
///
/// @return `0`.
int8_t rt_anim_controller3d_crossfade_layer_additive(void *c, int64_t l, rt_string s, double d) {
    (void)c;
    (void)l;
    (void)s;
    (void)d;
    return 0;
}

/// @brief Stub for `AnimController3D.StopLayer` — halt animation in
///        the given layer. Bones masked into this layer freeze at their
///        current pose contribution.
///
/// Silent no-op stub.
///
/// @param c AnimController3D handle (ignored).
/// @param l Layer index (ignored).
void rt_anim_controller3d_stop_layer(void *c, int64_t l) {
    (void)c;
    (void)l;
}

/// @brief Stub for `AnimController3D.BoneMatrix(i)` — get the world-
///        space matrix for bone `i` after blending all active layers.
///        Used by the renderer to compute final per-vertex skinning.
///
/// Silent stub returning NULL.
///
/// @param c AnimController3D handle (ignored).
/// @param i Bone index (ignored).
///
/// @return `NULL`.
void *rt_anim_controller3d_get_bone_matrix(void *c, int64_t i) {
    (void)c;
    (void)i;
    return NULL;
}

/// @brief Stub for the C accessor returning the controller's final
///        bone palette (column-major float matrices, one per bone) used
///        by the GPU vertex shader for skinning.
///
/// Silent stub: writes `0` to `*bone_count` and returns NULL.
///
/// @param c          AnimController3D handle (ignored).
/// @param bone_count Out-param receiving the bone count; defaults to `0`.
///
/// @return `NULL`.
const float *rt_anim_controller3d_get_final_palette_data(void *c, int32_t *bone_count) {
    (void)c;
    if (bone_count)
        *bone_count = 0;
    return NULL;
}

/// @brief Stub for the C accessor returning the previous-frame bone
///        palette (used by motion-blur post-FX to compute per-vertex
///        motion vectors).
///
/// Silent stub: writes `0` to `*bone_count` and returns NULL.
///
/// @param c          AnimController3D handle (ignored).
/// @param bone_count Out-param receiving the bone count; defaults to `0`.
///
/// @return `NULL`.
const float *rt_anim_controller3d_get_previous_palette_data(void *c, int32_t *bone_count) {
    (void)c;
    if (bone_count)
        *bone_count = 0;
    return NULL;
}

/* Decal3D stubs */

/// @brief Stub for `Decal3D.New` — would normally create a projected
///        texture decal at world position `p` with normal `n`, world-space
///        size `s`, and texture `t`. Used for bullet holes, paint splats,
///        AOE indicators.
///
/// Silent stub returning NULL.
///
/// @param p Vec3 world-space position (ignored).
/// @param n Vec3 surface normal (must be normalized) (ignored).
/// @param s Decal world-space size (ignored).
/// @param t Pixels handle for the decal texture (ignored).
///
/// @return `NULL`.
void *rt_decal3d_new(void *p, void *n, double s, void *t) {
    (void)p;
    (void)n;
    (void)s;
    (void)t;
    return NULL;
}

/// @brief Stub for `Decal3D.SetLifetime` — set the decal's remaining
///        lifetime in seconds. After expiry the decal is no longer rendered
///        and `IsExpired` returns true.
///
/// Silent no-op stub. `s = 0` means infinite (persistent decal).
///
/// @param d Decal3D handle (ignored).
/// @param s Lifetime in seconds (ignored).
void rt_decal3d_set_lifetime(void *d, double s) {
    (void)d;
    (void)s;
}

/// @brief Stub for `Decal3D.Update` — advance the decal's age by `dt`
///        seconds. Should be called once per frame.
///
/// Silent no-op stub.
///
/// @param d  Decal3D handle (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_decal3d_update(void *d, double dt) {
    (void)d;
    (void)dt;
}

/// @brief Stub for `Decal3D.IsExpired` — true once the decal's lifetime
///        has elapsed.
///
/// Silent stub returning `1` (expired) so caller-driven cleanup loops
/// don't accidentally retain decal handles forever in the headless build.
///
/// @param d Decal3D handle (ignored).
///
/// @return `1`.
int8_t rt_decal3d_is_expired(void *d) {
    (void)d;
    return 1;
}

/* Sprite3D stubs */

/// @brief Stub for `Sprite3D.New` — would normally create a 3D billboard
///        sprite (always camera-facing) bound to the given Pixels texture.
///
/// Silent stub returning NULL.
///
/// @param t Pixels handle for the sprite texture (ignored).
///
/// @return `NULL`.
void *rt_sprite3d_new(void *t) {
    (void)t;
    return NULL;
}

/// @brief Stub for `Sprite3D.SetPosition` — set the sprite's world-space
///        position.
///
/// Silent no-op stub.
///
/// @param s Sprite3D handle (ignored).
/// @param x World x (ignored).
/// @param y World y (ignored).
/// @param z World z (ignored).
void rt_sprite3d_set_position(void *s, double x, double y, double z) {
    (void)s;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Stub for `Sprite3D.SetScale` — set the sprite's world-space
///        size in world units.
///
/// Silent no-op stub.
///
/// @param s Sprite3D handle (ignored).
/// @param w Width in world units (ignored).
/// @param h Height in world units (ignored).
void rt_sprite3d_set_scale(void *s, double w, double h) {
    (void)s;
    (void)w;
    (void)h;
}

/// @brief Stub for `Sprite3D.SetAnchor` — set the normalized anchor point
///        within the sprite quad. `(0.5, 0.5)` is centered; `(0.5, 1.0)` is
///        bottom-center (good for ground-anchored billboards).
///
/// Silent no-op stub.
///
/// @param s  Sprite3D handle (ignored).
/// @param ax Horizontal anchor, 0..1 (ignored).
/// @param ay Vertical anchor, 0..1 (ignored).
void rt_sprite3d_set_anchor(void *s, double ax, double ay) {
    (void)s;
    (void)ax;
    (void)ay;
}

/// @brief Stub for `Sprite3D.SetFrame` — select a sub-rectangle of the
///        bound texture as the visible frame (sprite-sheet animation).
///
/// Silent no-op stub.
///
/// @param s  Sprite3D handle (ignored).
/// @param fx Frame top-left x in texture pixels (ignored).
/// @param fy Frame top-left y in texture pixels (ignored).
/// @param fw Frame width in texture pixels (ignored).
/// @param fh Frame height in texture pixels (ignored).
void rt_sprite3d_set_frame(void *s, int64_t fx, int64_t fy, int64_t fw, int64_t fh) {
    (void)s;
    (void)fx;
    (void)fy;
    (void)fw;
    (void)fh;
}

/* TextureAtlas3D stubs (F4) */

/// @brief Stub for `TextureAtlas3D.New` — would normally allocate a
///        `(w x h)` atlas surface with a packing strategy (skyline / shelf)
///        ready to receive sub-textures via `Add`.
///
/// Silent stub returning NULL.
///
/// @param w Atlas width in pixels (ignored).
/// @param h Atlas height in pixels (ignored).
///
/// @return `NULL`.
void *rt_texatlas3d_new(int64_t w, int64_t h) {
    (void)w;
    (void)h;
    return NULL;
}

/// @brief Stub for `TextureAtlas3D.Add` — pack a Pixels surface into the
///        atlas at the next available position. Returns an integer ID
///        used by `GetUVRect` to locate the sub-region later, or `-1` on
///        pack failure.
///
/// Silent stub returning `-1` (atlas full).
///
/// @param a Atlas handle (ignored).
/// @param p Pixels handle for the sub-texture (ignored).
///
/// @return `-1`.
int64_t rt_texatlas3d_add(void *a, void *p) {
    (void)a;
    (void)p;
    return -1;
}

/// @brief Stub for `TextureAtlas3D.Texture` — get the underlying Pixels
///        surface for binding to materials.
///
/// Silent stub returning NULL.
///
/// @param a Atlas handle (ignored).
///
/// @return `NULL`.
void *rt_texatlas3d_get_texture(void *a) {
    (void)a;
    return NULL;
}

/// @brief Stub for `TextureAtlas3D.GetUVRect(id)` — write the UV
///        coordinates `(u0, v0, u1, v1)` for sub-texture `id` to the
///        out-parameters.
///
/// Silent stub: writes the full-atlas defaults `(0, 0, 1, 1)` to non-NULL
/// out-params so callers get a usable (full-coverage) result rather than
/// an uninitialized read.
///
/// @param a  Atlas handle (ignored).
/// @param id Sub-texture id from `Add`, or invalid (ignored).
/// @param u0 Out-param: top-left u; defaults to `0`.
/// @param v0 Out-param: top-left v; defaults to `0`.
/// @param u1 Out-param: bottom-right u; defaults to `1`.
/// @param v1 Out-param: bottom-right v; defaults to `1`.
void rt_texatlas3d_get_uv_rect(
    void *a, int64_t id, double *u0, double *v0, double *u1, double *v1) {
    (void)a;
    (void)id;
    if (u0)
        *u0 = 0;
    if (v0)
        *v0 = 0;
    if (u1)
        *u1 = 1;
    if (v1)
        *v1 = 1;
}

/// @brief Stub for `Particles3D.set_Seed`. Silent no-op stub.
///
/// @param obj  Particles3D handle (ignored).
/// @param seed Deterministic random seed (ignored).
void rt_particles3d_set_seed(void *obj, int64_t seed) {
    (void)obj;
    (void)seed;
}

/// @brief Stub for `Particles3D.get_Seed`. Silent stub returning 0.
///
/// @param obj Particles3D handle (ignored).
///
/// @return `0`.
int64_t rt_particles3d_get_seed(void *obj) {
    (void)obj;
    return 0;
}
