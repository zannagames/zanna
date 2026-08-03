//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_facial.c
// Purpose: Zanna.Game3D.LipSync3D — amplitude-envelope lip sync (per-voice RMS
//   metering → morph weights with a soft-knee curve), a seeded procedural
//   blink layer, and conversational gaze sugar over LookAt IK. The pragmatic
//   tier: envelope mouths + blinks + eye contact at conversation camera
//   distance; phoneme visemes are out of scope by design.
// Key invariants:
//   - Envelope follower: 0.04 s attack / 0.12 s release (snappy open, soft
//     close); weight = envelope^0.7 (soft knee), scaled per bound shape.
//   - Blink randomness is a per-component seeded LCG — replays are identical.
//   - Blink is additive-max composed with lip-sync weights on the same shape
//     (never fights authored blends).
// Ownership/Lifetime:
//   - Entity retains the component; the backref is cleared at entity teardown.
//     The component retains its morph handle, gaze solver, and target vec.
// Links: rt_audio.h, rt_morphtarget3d.h, rt_iksolver3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements envelope lip sync, deterministic blinking, and eased gaze.
/// @details LipSync3D attaches one facial-animation component to an Entity3D.
///          It maps metered or injected amplitude through an attack/release
///          envelope and soft-knee curve into up to four morph bindings, layers
///          a seeded triangular blink envelope by maximum composition, and
///          eases a LookAt IK solver toward or away from a sampled gaze target.

#include "rt_animcontroller3d.h"
#include "rt_audio.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_iksolver3d.h"
#include "rt_morphtarget3d.h"
#include "rt_object.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include <math.h>
#include <string.h>

#define GAME3D_LS_ATTACK_SECONDS 0.04
#define GAME3D_LS_RELEASE_SECONDS 0.12
#define GAME3D_LS_KNEE_GAMMA 0.7
#define GAME3D_LS_BLINK_SECONDS 0.24 /* full close+open envelope */
#define GAME3D_LS_GAZE_EASE_RATE 4.0

/// @brief Clamp the private mouth-shape count to its fixed backing-array extent.
/// @param lipsync Component whose private count is inspected.
/// @return Safe shape count in `[0, RT_GAME3D_LS_MAX_SHAPES]`.
static int32_t game3d_lipsync_shape_count(const rt_game3d_lipsync *lipsync) {
    if (!lipsync || lipsync->shape_count <= 0)
        return 0;
    return lipsync->shape_count > RT_GAME3D_LS_MAX_SHAPES ? RT_GAME3D_LS_MAX_SHAPES
                                                          : lipsync->shape_count;
}

//=========================================================================
// Lifecycle
//=========================================================================

/// @brief GC finalizer: release retained references.
/// @param obj LipSync3D storage being finalized; NULL is ignored.
static void game3d_lipsync_finalize(void *obj) {
    rt_game3d_lipsync *lipsync = (rt_game3d_lipsync *)obj;
    if (!lipsync)
        return;
    if (lipsync->voice_id >= 0)
        rt_voice_enable_metering(lipsync->voice_id, 0);
    lipsync->voice_id = -1;
    lipsync->driving = 0;
    game3d_release_ref(&lipsync->morph);
    game3d_release_ref(&lipsync->gaze_solver);
    game3d_release_ref(&lipsync->gaze_target);
    for (int32_t i = 0; i < RT_GAME3D_LS_MAX_SHAPES; ++i) {
        if (lipsync->shapes[i].name_interned) {
            rt_string_unref(lipsync->shapes[i].name_interned);
            lipsync->shapes[i].name_interned = NULL;
        }
    }
    if (lipsync->blink_interned) {
        rt_string_unref(lipsync->blink_interned);
        lipsync->blink_interned = NULL;
    }
}

/// @brief Create a facial component for @p entity_obj (attach stores it).
/// @details Replaces any prior component after clearing its entity back-reference.
///          The entity retains the new component while the returned creation
///          reference remains owned by the caller.
/// @param entity_obj Entity3D that receives the component.
/// @return A newly allocated LipSync3D, or NULL after validation or allocation failure.
void *rt_game3d_lipsync_new(void *entity_obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(entity_obj, "Game3D.LipSync3D.New: invalid entity");
    if (!entity)
        return NULL;
    rt_game3d_lipsync *lipsync = (rt_game3d_lipsync *)rt_obj_new_i64(RT_G3D_GAME3D_LIPSYNC_CLASS_ID,
                                                                     (int64_t)sizeof(*lipsync));
    if (!lipsync) {
        rt_trap("Game3D.LipSync3D.New: allocation failed");
        return NULL;
    }
    memset(lipsync, 0, sizeof(*lipsync));
    rt_obj_set_finalizer(lipsync, game3d_lipsync_finalize);
    lipsync->entity = entity;
    lipsync->voice_id = -1;
    lipsync->blink_min_interval = 2.0;
    lipsync->blink_max_interval = 6.0;
    lipsync->blink_seed = 0x9E3779B97F4A7C15ull;
    /* Entity retains the component (one per entity; replace releases). */
    rt_game3d_lipsync *previous = (rt_game3d_lipsync *)rt_g3d_checked_or_null(
        entity->lipsync, RT_G3D_GAME3D_LIPSYNC_CLASS_ID);
    if (previous && previous != lipsync)
        previous->entity = NULL;
    game3d_assign_typed_ref(&entity->lipsync, lipsync, RT_G3D_GAME3D_LIPSYNC_CLASS_ID);
    /* Return with the creation reference intact (+1 for the caller), matching every
     * sibling component constructor (footsteps/interactable/interactor). Dropping it
     * here left the object owned solely by entity->lipsync, so a caller releasing the
     * expected owned reference would free it out from under the entity. */
    return lipsync;
}

/// @brief Entity accessor for the attached LipSync3D (NULL when none).
/// @param obj Entity3D runtime handle.
/// @return The validated retained LipSync3D pointer, or NULL.
void *rt_game3d_entity_get_lipsync(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_lipSync: invalid entity");
    return entity ? rt_g3d_checked_or_null(entity->lipsync, RT_G3D_GAME3D_LIPSYNC_CLASS_ID) : NULL;
}

//=========================================================================
// Bindings
//=========================================================================

/// @brief Fluent: bind the MorphTarget3D the shape bindings drive.
/// @param obj LipSync3D runtime handle.
/// @param morph MorphTarget3D to retain, or NULL to detach the current target.
/// @return @p obj for fluent chaining; wrong runtime classes trap without mutation.
void *rt_game3d_lipsync_bind_morph(void *obj, void *morph) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.bindMorph: invalid lip sync");
    if (morph && !rt_g3d_has_class(morph, RT_G3D_MORPHTARGET3D_CLASS_ID)) {
        rt_trap("Game3D.LipSync3D.bindMorph: value must be MorphTarget3D");
        return obj;
    }
    if (lipsync)
        game3d_assign_ref(&lipsync->morph, morph);
    return obj;
}

/// @brief Fluent: bind a mouth shape (up to 4) with a per-shape weight scale.
/// @param obj LipSync3D runtime handle.
/// @param shape_name Non-empty morph-target name retained for allocation-free ticks.
/// @param weight_scale Per-shape multiplier finite-clamped to [0, 4].
/// @return @p obj for fluent chaining; capacity and name errors trap without
///         adding a binding.
void *rt_game3d_lipsync_bind_mouth_shape(void *obj, rt_string shape_name, double weight_scale) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.bindMouthShape: invalid lip sync");
    if (!lipsync)
        return obj;
    lipsync->shape_count = game3d_lipsync_shape_count(lipsync);
    if (lipsync->shape_count >= RT_GAME3D_LS_MAX_SHAPES) {
        rt_trap("Game3D.LipSync3D.bindMouthShape: shape limit reached (4)");
        return obj;
    }
    const char *name = shape_name ? rt_string_cstr(shape_name) : NULL;
    if (!name || !name[0]) {
        rt_trap("Game3D.LipSync3D.bindMouthShape: shape name must be non-empty");
        return obj;
    }
    rt_game3d_ls_shape *shape = &lipsync->shapes[lipsync->shape_count];
    if (shape->name_interned)
        rt_string_unref(shape->name_interned);
    memset(shape, 0, sizeof(*shape));
    strncpy(shape->name, name, RT_GAME3D_DLG_NAME_MAX - 1);
    shape->name[RT_GAME3D_DLG_NAME_MAX - 1] = '\0';
    shape->scale = game3d_clamp(game3d_finite_or(weight_scale, 1.0), 0.0, 4.0);
    shape->name_interned = rt_string_ref(shape_name); /* reused each tick; no per-frame alloc */
    if (!shape->name_interned) {
        memset(shape, 0, sizeof(*shape));
        return obj;
    }
    lipsync->shape_count++;
    return obj;
}

/// @brief Drive from a playing voice: enables metering and tracks its level.
/// @param obj LipSync3D runtime handle.
/// @param voice_id Audio voice identifier; negative values disable driving.
void rt_game3d_lipsync_drive(void *obj, int64_t voice_id) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.drive: invalid lip sync");
    if (!lipsync)
        return;
    if (lipsync->voice_id >= 0 && lipsync->voice_id != voice_id)
        rt_voice_enable_metering(lipsync->voice_id, 0);
    lipsync->voice_id = voice_id;
    lipsync->driving = voice_id >= 0 ? 1 : 0;
    if (voice_id >= 0)
        rt_voice_enable_metering(voice_id, 1);
}

/// @brief Drive from an explicit level (0..1) — Dialogue3D/tests feed this
///   directly when no metered voice is available.
/// @details The injected value raises the current envelope immediately; later
///          ticks release it toward zero because no voice remains attached.
/// @param obj LipSync3D runtime handle.
/// @param level Raw amplitude finite-clamped to [0, 1]. Zero clears driving.
void rt_game3d_lipsync_drive_level(void *obj, double level) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.driveLevel: invalid lip sync");
    if (!lipsync)
        return;
    if (lipsync->voice_id >= 0)
        rt_voice_enable_metering(lipsync->voice_id, 0);
    lipsync->voice_id = -1;
    lipsync->driving = 1;
    /* Immediate injection: the tick smooths from this raw level. */
    double target = game3d_clamp(game3d_finite_or(level, 0.0), 0.0, 1.0);
    lipsync->envelope = game3d_clamp(game3d_finite_or(lipsync->envelope, 0.0), 0.0, 1.0);
    lipsync->envelope = target > lipsync->envelope ? target : lipsync->envelope;
    if (target <= 0.0)
        lipsync->driving = 0;
}

/// @brief Manual release: eases the mouth closed.
/// @param obj LipSync3D runtime handle.
/// @post Metering is disabled for the attached voice and subsequent ticks use
///       the release time constant to approach zero.
void rt_game3d_lipsync_stop(void *obj) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.stop: invalid lip sync");
    if (!lipsync)
        return;
    if (lipsync->voice_id >= 0)
        rt_voice_enable_metering(lipsync->voice_id, 0);
    lipsync->voice_id = -1;
    lipsync->driving = 0;
}

/// @brief Configure the procedural blink layer (seeded; deterministic).
/// @param obj LipSync3D runtime handle.
/// @param enabled Non-zero to schedule blink envelopes.
/// @param shape_name Morph-target name to retain for blink output; NULL clears it.
/// @param min_interval Requested positive minimum seconds between blinks.
/// @param max_interval Requested positive maximum seconds, normalized not to
///                     fall below the sanitized minimum.
void rt_game3d_lipsync_set_blink(
    void *obj, int8_t enabled, rt_string shape_name, double min_interval, double max_interval) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.setBlink: invalid lip sync");
    if (!lipsync)
        return;
    lipsync->blink_enabled = enabled ? 1 : 0;
    const char *name = shape_name ? rt_string_cstr(shape_name) : NULL;
    lipsync->blink_shape[0] = '\0';
    if (lipsync->blink_interned) {
        rt_string_unref(lipsync->blink_interned);
        lipsync->blink_interned = NULL;
    }
    if (name) {
        strncpy(lipsync->blink_shape, name, RT_GAME3D_DLG_NAME_MAX - 1);
        lipsync->blink_shape[RT_GAME3D_DLG_NAME_MAX - 1] = '\0';
        lipsync->blink_interned = rt_string_ref(shape_name); /* reused each tick */
    }
    lipsync->blink_min_interval = game3d_positive_clamped_or(min_interval, 2.0, 60.0);
    lipsync->blink_max_interval =
        game3d_positive_clamped_or(max_interval, lipsync->blink_min_interval, 120.0);
    if (lipsync->blink_max_interval < lipsync->blink_min_interval)
        lipsync->blink_max_interval = lipsync->blink_min_interval;
    lipsync->blink_timer = 0.0; /* schedule on the next tick */
    lipsync->blink_phase = 0.0;
}

/// @brief Deterministic LCG step for blink intervals.
/// @param lipsync Component whose private seed is advanced.
/// @return A deterministic pseudo-random value in the inclusive range [0, 1].
static double game3d_lipsync_random01(rt_game3d_lipsync *lipsync) {
    lipsync->blink_seed = lipsync->blink_seed * 6364136223846793005ull + 1442695040888963407ull;
    return (double)((lipsync->blink_seed >> 33) & 0x7FFFFFFF) / 2147483647.0;
}

/// @brief Gaze sugar: ease a LookAt IK solver toward the target (NULL clears).
/// @details Vec3 targets are copied directly; Entity3D targets are sampled once
///          at world position plus a default eye-height offset. The existing
///          solver and target reference are retained while NULL merely eases
///          solver weight back to zero.
/// @param obj LipSync3D runtime handle.
/// @param target Vec3, Entity3D, or NULL. Other runtime classes trap.
void rt_game3d_lipsync_set_gaze(void *obj, void *target) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.setGaze: invalid lip sync");
    if (!lipsync)
        return;
    if (!target) {
        lipsync->gaze_active = 0;
        return;
    }
    double pos[3] = {0.0, 0.0, 0.0};
    if (rt_g3d_is_vec3(target)) {
        if (!game3d_read_vec3(target, pos, "Game3D.LipSync3D.setGaze: invalid Vec3 target"))
            return;
    } else if (rt_g3d_has_class(target, RT_G3D_GAME3D_ENTITY_CLASS_ID)) {
        rt_game3d_entity *entity = (rt_game3d_entity *)target;
        if (!game3d_entity_alive_or_record(entity) ||
            !game3d_entity_world_position_components(entity, pos))
            return;
        pos[1] = game3d_clamp_abs_or(
            pos[1] + 1.6, pos[1], RT_GAME3D_COORD_ABS_MAX); /* eye-height default */
    } else {
        rt_trap("Game3D.LipSync3D.setGaze: target must be Entity3D, Vec3, or null");
        return;
    }
    if (!lipsync->gaze_solver) {
        /* Head bone must be bound first (bindHeadBone). */
        return;
    }
    void *vec = rt_vec3_new(pos[0], pos[1], pos[2]);
    rt_ik_solver3d_set_target(lipsync->gaze_solver, vec);
    game3d_assign_ref(&lipsync->gaze_target, vec);
    game3d_release_ref(&vec);
    lipsync->gaze_active = 1;
}

/// @brief Fluent: create the gaze LookAt solver on the entity's animator for a
///   named head bone and install it on the controller.
/// @param obj LipSync3D runtime handle.
/// @param bone_name Runtime string naming a bone in the attached animator skeleton.
/// @return @p obj for fluent chaining; missing animation prerequisites or an
///         unknown bone leave the prior solver unchanged.
void *rt_game3d_lipsync_bind_head_bone(void *obj, rt_string bone_name) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.bindHeadBone: invalid lip sync");
    if (!lipsync)
        return obj;
    rt_game3d_entity *entity = lipsync->entity;
    void *animator = entity ? game3d_entity_anim_ref(entity) : NULL;
    void *controller = animator ? rt_game3d_animator_get_controller(animator) : NULL;
    void *skeleton = controller ? rt_anim_controller3d_get_skeleton(controller) : NULL;
    if (!skeleton) {
        rt_trap("Game3D.LipSync3D.bindHeadBone: entity needs an animator with a skeleton");
        return obj;
    }
    int64_t bone = rt_skeleton3d_find_bone(skeleton, bone_name);
    if (bone < 0) {
        rt_trap("Game3D.LipSync3D.bindHeadBone: unknown bone name");
        return obj;
    }
    void *solver = rt_ik_solver3d_look_at(skeleton, bone);
    if (!solver)
        return obj;
    game3d_assign_ref(&lipsync->gaze_solver, solver);
    game3d_release_ref(&solver);
    rt_ik_solver3d_set_weight(lipsync->gaze_solver, 0.0);
    (void)rt_anim_controller3d_set_ik_solver(controller, lipsync->gaze_solver);
    return obj;
}

//=========================================================================
// Properties
//=========================================================================

/// @brief Report whether metered or injected lip-sync driving is active.
/// @param obj LipSync3D runtime handle.
/// @return Non-zero while driving, otherwise zero.
int8_t rt_game3d_lipsync_get_driving(void *obj) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.get_driving: invalid lip sync");
    return lipsync ? lipsync->driving : 0;
}

/// @brief Return the current smoothed mouth envelope.
/// @param obj LipSync3D runtime handle.
/// @return The current nominal [0, 1] envelope, or zero when invalid.
double rt_game3d_lipsync_get_level(void *obj) {
    rt_game3d_lipsync *lipsync =
        game3d_lipsync_checked(obj, "Game3D.LipSync3D.get_level: invalid lip sync");
    return lipsync ? lipsync->envelope : 0.0;
}

//=========================================================================
// Per-step tick (called by the world after ragdoll sync, before scene sync)
//=========================================================================

/// @brief Advance one component: envelope, morph weights, blink, gaze ease.
/// @param lipsync Component to advance; must be valid.
/// @param dt Sanitized positive simulation step in seconds.
static void game3d_lipsync_tick_one(rt_game3d_lipsync *lipsync, double dt) {
    /* Envelope follower toward the metered (or injected) level. */
    lipsync->envelope = game3d_clamp(game3d_finite_or(lipsync->envelope, 0.0), 0.0, 1.0);
    double target = 0.0;
    if (lipsync->driving && lipsync->voice_id >= 0) {
        if (rt_voice_is_playing(lipsync->voice_id)) {
            target = game3d_clamp(rt_voice_get_level(lipsync->voice_id) * 2.83, 0.0, 1.0);
        } else {
            rt_voice_enable_metering(lipsync->voice_id, 0);
            lipsync->voice_id = -1;
            lipsync->driving = 0;
        }
    } else if (lipsync->driving && lipsync->voice_id < 0) {
        /* DriveLevel is an impulse: the injected envelope is retained, but the
         * driving flag must clear so callers observe the ensuing release. */
        lipsync->driving = 0;
    }
    double tau = target > lipsync->envelope ? GAME3D_LS_ATTACK_SECONDS : GAME3D_LS_RELEASE_SECONDS;
    double alpha = 1.0 - exp(-dt / tau);
    lipsync->envelope += (target - lipsync->envelope) * alpha;
    if (!lipsync->driving && lipsync->voice_id < 0 && target <= 0.0 && lipsync->envelope < 1e-4)
        lipsync->envelope = 0.0;

    double weight = pow(game3d_clamp(lipsync->envelope, 0.0, 1.0), GAME3D_LS_KNEE_GAMMA);

    /* Blink layer: seeded intervals, additive-max with same-shape lip sync. */
    double blink_weight = 0.0;
    if (lipsync->blink_enabled) {
        if (lipsync->blink_phase > 0.0) {
            double phase_before = lipsync->blink_phase;
            lipsync->blink_phase -= dt;
            if (lipsync->blink_phase < 0.0)
                lipsync->blink_phase = 0.0;
            double elapsed_before = GAME3D_LS_BLINK_SECONDS - phase_before;
            double elapsed_after = GAME3D_LS_BLINK_SECONDS - lipsync->blink_phase;
            double elapsed = elapsed_after;
            if (elapsed_before < GAME3D_LS_BLINK_SECONDS * 0.5 &&
                elapsed_after > GAME3D_LS_BLINK_SECONDS * 0.5)
                elapsed = GAME3D_LS_BLINK_SECONDS * 0.5;
            double t = elapsed / GAME3D_LS_BLINK_SECONDS;
            blink_weight = t < 0.5 ? t * 2.0 : (1.0 - t) * 2.0; /* triangle close/open */
        } else {
            lipsync->blink_timer -= dt;
            if (lipsync->blink_timer <= 0.0) {
                double range = lipsync->blink_max_interval - lipsync->blink_min_interval;
                lipsync->blink_timer =
                    lipsync->blink_min_interval + game3d_lipsync_random01(lipsync) * range;
                lipsync->blink_phase = GAME3D_LS_BLINK_SECONDS;
            }
        }
    }

    void *morph = rt_g3d_checked_or_null(lipsync->morph, RT_G3D_MORPHTARGET3D_CLASS_ID);
    if (morph) {
        int32_t shape_count = game3d_lipsync_shape_count(lipsync);
        for (int32_t i = 0; i < shape_count; ++i) {
            double shape_weight = weight * lipsync->shapes[i].scale;
            if (lipsync->blink_enabled && lipsync->blink_shape[0] &&
                strcmp(lipsync->shapes[i].name, lipsync->blink_shape) == 0 &&
                blink_weight > shape_weight)
                shape_weight = blink_weight; /* additive-max composition */
            if (lipsync->shapes[i].name_interned)
                rt_morphtarget3d_set_weight_by_name(
                    morph, lipsync->shapes[i].name_interned, shape_weight);
        }
        if (lipsync->blink_enabled && lipsync->blink_shape[0]) {
            int bound = 0;
            for (int32_t i = 0; i < shape_count; ++i)
                if (strcmp(lipsync->shapes[i].name, lipsync->blink_shape) == 0)
                    bound = 1;
            if (!bound && lipsync->blink_interned)
                rt_morphtarget3d_set_weight_by_name(morph, lipsync->blink_interned, blink_weight);
        }
    }

    /* Gaze ease. */
    if (lipsync->gaze_solver) {
        double gaze_target = lipsync->gaze_active ? 1.0 : 0.0;
        double ease = 1.0 - exp(-GAME3D_LS_GAZE_EASE_RATE * dt);
        lipsync->gaze_weight += (gaze_target - lipsync->gaze_weight) * ease;
        rt_ik_solver3d_set_weight(lipsync->gaze_solver, lipsync->gaze_weight);
    }
}

/// @brief World facial pass. See internal header.
/// @details Iterates the bounded live entity registry after ragdoll sync and
///          advances each attached, correctly typed LipSync3D exactly once.
/// @param world World3D whose spawned entities are scanned; NULL is ignored.
/// @param dt Frame duration in seconds, sanitized before component updates.
void game3d_world_facial_tick(rt_game3d_world *world, double dt) {
    if (!world)
        return;
    dt = game3d_clamp_dt(dt);
    int32_t entity_count = world->entity_count;
    if (!world->entities || entity_count < 0)
        entity_count = 0;
    if (entity_count > world->entity_capacity)
        entity_count = world->entity_capacity > 0 ? world->entity_capacity : 0;
    for (int32_t i = 0; i < entity_count; ++i) {
        rt_game3d_entity *entity = world->entities[i];
        if (!entity || !entity->alive || !entity->spawned || !entity->lipsync)
            continue;
        rt_game3d_lipsync *lipsync = (rt_game3d_lipsync *)rt_g3d_checked_or_null(
            entity->lipsync, RT_G3D_GAME3D_LIPSYNC_CLASS_ID);
        if (lipsync)
            game3d_lipsync_tick_one(lipsync, dt);
    }
}
