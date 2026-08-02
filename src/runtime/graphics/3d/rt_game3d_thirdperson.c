//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_thirdperson.c
// Purpose: Zanna.Game3D.ThirdPersonController — an over-the-shoulder camera on a
//   collision-aware spring arm that also drives an optional CharacterController3D
//   camera-relatively. Update (pre-physics) consumes look input and drives the
//   character; LateUpdate (post-sync) sweeps the camera boom against world
//   collision, applies aim-mode blending, and optionally fades occluding meshes.
// Key invariants:
//   - The controller never writes entity transforms directly; the character
//     controller owns motion (avoids fighting SyncMode).
//   - Boom pull-in is instant (camera never clips); release is damped.
//   - Occluder-fade material instances are always restored on detach, target
//     change, disable, and finalization — no leaked material clones.
// Ownership/Lifetime:
//   - GC-managed handle; finalizer restores faded materials and releases the
//     retained world/target/character/lock references.
// Links: misc/plans/thirdpersonupgrade/01-thirdperson-controller.md,
//   rt_game3d_internal.h, rt_physics3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements collision-aware over-the-shoulder camera and character control.
/// @details ThirdPersonController separates pre-physics input and character
/// driving from post-sync spring-arm placement, blends aim framing, optionally
/// follows TargetLock3D, and manages reversible per-node material instances for
/// occlusion fading.

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_trap.h"
#include "rt_vec2.h"
#include "rt_vec3.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

//=========================================================================
// Internal helpers
//=========================================================================

/// @brief Validate dense occluder-fade bookkeeping before traversal or growth.
/// @param controller Borrowed third-person controller payload.
/// @return Nonzero when count, capacity, and backing storage agree.
static int game3d_thirdperson_fade_storage_valid(
    const rt_game3d_thirdperson_controller *controller) {
    if (!controller)
        return 0;
    if (!controller->fades)
        return controller->fade_count == 0 && controller->fade_capacity == 0 &&
               controller->fade_storage_capacity == 0 && controller->fade_storage_cookie == 0;
    return controller->fade_storage_capacity > 0 &&
           controller->fade_storage_cookie ==
               game3d_thirdperson_fade_storage_cookie_value(controller->fades,
                                                            controller->fade_storage_capacity) &&
           controller->fade_capacity == controller->fade_storage_capacity &&
           controller->fade_count >= 0 &&
           controller->fade_count <= controller->fade_storage_capacity;
}

/// @brief Return whether the raw fade allocation has a trusted ownership marker.
/// @param controller Borrowed third-person controller payload.
/// @return Nonzero only when `fades` may safely be traversed and freed.
static int game3d_thirdperson_fade_storage_owned(
    const rt_game3d_thirdperson_controller *controller) {
    return controller && controller->fades && controller->fade_storage_capacity > 0 &&
           controller->fade_storage_cookie ==
               game3d_thirdperson_fade_storage_cookie_value(controller->fades,
                                                            controller->fade_storage_capacity);
}

/// @brief Release a retained private slot only when its complete payload remains valid.
/// @details Invalid same-class or wrong-class values are treated as unowned corruption sentinels.
/// @param[in,out] slot Address of the retained slot.
/// @param class_id Required runtime class identifier.
/// @param payload_size Minimum complete payload size.
static void game3d_thirdperson_release_instance_ref(void **slot,
                                                    int64_t class_id,
                                                    size_t payload_size) {
    if (!slot || !*slot)
        return;
    if (!rt_obj_is_instance(*slot, class_id, payload_size)) {
        *slot = NULL;
        return;
    }
    game3d_release_ref(slot);
}

/// @brief Transactionally replace a complete retained private object slot.
/// @param[in,out] slot Address of the retained slot.
/// @param value Borrowed complete replacement, or NULL.
/// @param class_id Required runtime class identifier.
/// @param payload_size Minimum complete payload size.
static void game3d_thirdperson_assign_instance_ref(void **slot,
                                                   void *value,
                                                   int64_t class_id,
                                                   size_t payload_size) {
    if (!slot || *slot == value || (value && !rt_obj_is_instance(value, class_id, payload_size)))
        return;
    rt_obj_retain_maybe(value);
    game3d_thirdperson_release_instance_ref(slot, class_id, payload_size);
    *slot = value;
}

/// @brief Return the controller's target Entity3D when still alive, else NULL.
/// @param controller Borrowed third-person controller payload.
/// @return Borrowed live target entity, or `NULL` when absent, stale, or invalid.
static rt_game3d_entity *game3d_thirdperson_target_ref(
    rt_game3d_thirdperson_controller *controller) {
    rt_game3d_entity *entity = controller && rt_obj_is_instance(controller->target,
                                                                RT_G3D_GAME3D_ENTITY_CLASS_ID,
                                                                sizeof(rt_game3d_entity))
                                   ? (rt_game3d_entity *)controller->target
                                   : NULL;
    if (!entity)
        return NULL;
    if (game3d_entity_alive_or_record(entity))
        return entity;
    game3d_thirdperson_release_instance_ref(
        &controller->target, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    return NULL;
}

/// @brief Return the controller's CharacterController3D slot when valid, else NULL.
/// @param controller Borrowed third-person controller payload.
/// @return Borrowed CharacterController3D handle, or `NULL` when absent or type-mismatched.
static void *game3d_thirdperson_character_ref(rt_game3d_thirdperson_controller *controller) {
    return controller && rt_obj_is_instance(controller->character,
                                            RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID,
                                            sizeof(rt_game3d_character_controller))
               ? controller->character
               : NULL;
}

/// @brief Return the controller's TargetLock3D slot when complete, else NULL.
/// @param controller Borrowed third-person controller payload.
/// @return Borrowed complete TargetLock3D handle, or NULL.
static rt_game3d_targetlock *game3d_thirdperson_lock_ref(
    rt_game3d_thirdperson_controller *controller) {
    return controller && rt_obj_is_instance(controller->lock,
                                            RT_G3D_GAME3D_TARGETLOCK_CLASS_ID,
                                            sizeof(rt_game3d_targetlock))
               ? (rt_game3d_targetlock *)controller->lock
               : NULL;
}

/// @brief Return the controller's retained live World3D slot when complete.
/// @param controller Borrowed third-person controller payload.
/// @return Borrowed live World3D payload, or NULL for detached/corrupt state.
static rt_game3d_world *game3d_thirdperson_world_ref(rt_game3d_thirdperson_controller *controller) {
    rt_game3d_world *world = controller && rt_obj_is_instance(controller->world,
                                                              RT_G3D_GAME3D_WORLD_CLASS_ID,
                                                              sizeof(rt_game3d_world))
                                 ? (rt_game3d_world *)controller->world
                                 : NULL;
    if (!world || !world->destroyed)
        return world;
    game3d_thirdperson_release_instance_ref(
        &controller->world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world));
    return NULL;
}

/// @brief Wrap yaw into [-180, 180) so orbit state never grows unbounded.
/// @param yaw Candidate angle in degrees.
/// @return Finite coterminal angle in `[-180, 180)`, or zero for non-finite input.
static double game3d_thirdperson_wrap_yaw(double yaw) {
    if (!isfinite(yaw))
        return 0.0;
    yaw = fmod(yaw + 180.0, 360.0);
    if (yaw < 0.0)
        yaw += 360.0;
    return yaw - 180.0;
}

/// @brief Repair private third-person state before any public field access.
/// @param controller Complete controller payload; NULL is ignored.
void game3d_thirdperson_repair_state(rt_game3d_thirdperson_controller *controller) {
    if (!controller)
        return;
    if (controller->world && !rt_obj_is_instance(controller->world,
                                                 RT_G3D_GAME3D_WORLD_CLASS_ID,
                                                 sizeof(rt_game3d_world)))
        controller->world = NULL;
    if (controller->target && !rt_obj_is_instance(controller->target,
                                                  RT_G3D_GAME3D_ENTITY_CLASS_ID,
                                                  sizeof(rt_game3d_entity)))
        controller->target = NULL;
    if (controller->character && !rt_obj_is_instance(controller->character,
                                                     RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID,
                                                     sizeof(rt_game3d_character_controller)))
        controller->character = NULL;
    if (controller->lock && !rt_obj_is_instance(controller->lock,
                                                RT_G3D_GAME3D_TARGETLOCK_CLASS_ID,
                                                sizeof(rt_game3d_targetlock)))
        controller->lock = NULL;

    controller->min_distance = game3d_nonnegative_clamped_or(
        controller->min_distance, RT_GAME3D_TP_DEFAULT_MIN_DISTANCE, RT_GAME3D_COORD_ABS_MAX);
    controller->max_distance = game3d_positive_clamped_or(
        controller->max_distance, RT_GAME3D_TP_DEFAULT_MAX_DISTANCE, RT_GAME3D_COORD_ABS_MAX);
    if (controller->max_distance < controller->min_distance)
        controller->max_distance = controller->min_distance;
    controller->distance =
        game3d_clamp(game3d_finite_or(controller->distance, RT_GAME3D_TP_DEFAULT_DISTANCE),
                     controller->min_distance,
                     controller->max_distance);
    controller->yaw = game3d_thirdperson_wrap_yaw(controller->yaw);
    controller->pitch_min = game3d_clamp(
        game3d_finite_or(controller->pitch_min, RT_GAME3D_TP_DEFAULT_PITCH_MIN), -89.0, 89.0);
    controller->pitch_max = game3d_clamp(
        game3d_finite_or(controller->pitch_max, RT_GAME3D_TP_DEFAULT_PITCH_MAX), -89.0, 89.0);
    if (controller->pitch_max < controller->pitch_min) {
        controller->pitch_min = RT_GAME3D_TP_DEFAULT_PITCH_MIN;
        controller->pitch_max = RT_GAME3D_TP_DEFAULT_PITCH_MAX;
    }
    controller->pitch = game3d_clamp(
        game3d_finite_or(controller->pitch, 0.0), controller->pitch_min, controller->pitch_max);
    for (int i = 0; i < 3; ++i) {
        controller->shoulder_offset[i] = game3d_clamp_coord_or(controller->shoulder_offset[i], 0.0);
        controller->aim_shoulder_offset[i] =
            game3d_clamp_coord_or(controller->aim_shoulder_offset[i], 0.0);
    }
    controller->pivot_height =
        game3d_clamp_coord_or(controller->pivot_height, RT_GAME3D_TP_DEFAULT_PIVOT_HEIGHT);
    controller->damping = game3d_nonnegative_clamped_or(
        controller->damping, RT_GAME3D_DEFAULT_FOLLOW_DAMPING, RT_GAME3D_DAMPING_MAX);
    controller->collision_radius = game3d_positive_clamped_or(controller->collision_radius,
                                                              RT_GAME3D_TP_DEFAULT_COLLISION_RADIUS,
                                                              RT_GAME3D_SCALE_ABS_MAX);
    controller->occlusion_fade = controller->occlusion_fade ? 1 : 0;
    controller->aiming = controller->aiming ? 1 : 0;
    controller->aim_blend = game3d_clamp(game3d_finite_or(controller->aim_blend, 0.0), 0.0, 1.0);
    controller->aim_distance = game3d_positive_clamped_or(
        controller->aim_distance, RT_GAME3D_TP_DEFAULT_AIM_DISTANCE, RT_GAME3D_COORD_ABS_MAX);
    controller->aim_fov = game3d_clamp(
        game3d_finite_or(controller->aim_fov, RT_GAME3D_TP_DEFAULT_AIM_FOV), 1.0, 179.0);
    controller->base_fov_valid = controller->base_fov_valid ? 1 : 0;
    if (controller->base_fov_valid && !isfinite(controller->base_fov)) {
        controller->base_fov = 0.0;
        controller->base_fov_valid = 0;
    } else if (controller->base_fov_valid) {
        controller->base_fov = game3d_clamp(controller->base_fov, 1.0, 179.0);
    }
    controller->current_distance =
        game3d_clamp(game3d_finite_or(controller->current_distance, controller->distance),
                     controller->min_distance,
                     controller->max_distance);
    if (!game3d_thirdperson_fade_storage_valid(controller))
        game3d_thirdperson_reset_fades(controller);
}

/// @brief Camera-forward unit vector from orbit yaw/pitch (degrees).
/// @details Yaw 0 looks down -Z; yaw 90 looks down -X (matches the character-drive
///   parity contract in plan 01). Positive pitch raises the camera above the pivot
///   looking down (orbit-style pitch, like OrbitController).
/// @param yaw_deg Orbit yaw in degrees.
/// @param pitch_deg Orbit pitch in degrees.
/// @param[out] out_forward Required three-component unit-direction destination.
static void game3d_thirdperson_forward(double yaw_deg, double pitch_deg, double out_forward[3]) {
    double yaw = yaw_deg * (RT_GAME3D_PI / 180.0);
    double pitch = pitch_deg * (RT_GAME3D_PI / 180.0);
    double cp = cos(pitch);
    out_forward[0] = -sin(yaw) * cp;
    out_forward[1] = -sin(pitch);
    out_forward[2] = -cos(yaw) * cp;
}

/// @brief Restore one fade entry's original material and release its refs.
/// @param entry Fade bookkeeping entry to clear; `NULL` is ignored.
static void game3d_thirdperson_fade_entry_release(rt_game3d_tp_fade_entry *entry) {
    if (!entry)
        return;
    void *node =
        rt_obj_is_instance(entry->node, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d))
            ? entry->node
            : NULL;
    void *original = rt_obj_is_instance(entry->original_material,
                                        RT_G3D_MATERIAL3D_CLASS_ID,
                                        sizeof(rt_material3d))
                         ? entry->original_material
                         : NULL;
    void *fade_material =
        rt_obj_is_instance(entry->fade_material, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d))
            ? entry->fade_material
            : NULL;
    if (node && original && fade_material && rt_scene_node3d_get_material(node) == fade_material)
        rt_scene_node3d_set_material(node, original);
    game3d_thirdperson_release_instance_ref(
        &entry->node, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d));
    game3d_thirdperson_release_instance_ref(
        &entry->original_material, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d));
    game3d_thirdperson_release_instance_ref(
        &entry->fade_material, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d));
    memset(entry, 0, sizeof(*entry));
}

/// @brief Restore all faded occluder materials and drop the bookkeeping array.
///   Shared with the world-detach path (rt_game3d_controllers.c). See internal header.
/// @param controller Third-person controller payload whose fade state is reset.
void game3d_thirdperson_reset_fades(rt_game3d_thirdperson_controller *controller) {
    if (!controller)
        return;
    int storage_is_owned = game3d_thirdperson_fade_storage_owned(controller);
    int32_t fade_count =
        game3d_thirdperson_fade_storage_valid(controller) ? controller->fade_count : 0;
    for (int32_t i = 0; i < fade_count; ++i)
        game3d_thirdperson_fade_entry_release(&controller->fades[i]);
    if (storage_is_owned)
        free(controller->fades);
    controller->fades = NULL;
    controller->fade_count = 0;
    controller->fade_capacity = 0;
    controller->fade_storage_capacity = 0;
    controller->fade_storage_cookie = 0;
}

/// @brief Find an existing fade entry for @p node, or -1.
/// @param controller Borrowed controller containing the fade array.
/// @param node Borrowed SceneNode3D handle to locate.
/// @return Zero-based fade entry index, or `-1` when absent.
static int32_t game3d_thirdperson_fade_find(const rt_game3d_thirdperson_controller *controller,
                                            const void *node) {
    if (!node || !game3d_thirdperson_fade_storage_valid(controller))
        return -1;
    for (int32_t i = 0; i < controller->fade_count; ++i)
        if (controller->fades[i].node == node)
            return i;
    return -1;
}

/// @brief Begin fading @p node: clone its material into a blend-mode instance.
/// @param controller Controller whose owned fade array may grow.
/// @param node Borrowed SceneNode3D handle whose material is instanced.
/// @return Index of the new entry, or -1 on failure (no material, alloc failure).
static int32_t game3d_thirdperson_fade_begin(rt_game3d_thirdperson_controller *controller,
                                             void *node) {
    if (!game3d_thirdperson_fade_storage_valid(controller) ||
        !rt_obj_is_instance(node, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d)))
        return -1;
    void *original = rt_scene_node3d_get_material(node);
    if (!rt_obj_is_instance(original, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d)))
        return -1;
    void *clone = rt_material3d_make_instance(original);
    if (!rt_obj_is_instance(clone, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d)))
        return -1;
    if (controller->fade_count >= controller->fade_capacity) {
        if (controller->fade_count == INT32_MAX) {
            game3d_release_ref(&clone);
            rt_trap("Game3D.ThirdPersonController: occluder fade limit exceeded");
            return -1;
        }
        int32_t new_cap = 0;
        if (!game3d_checked_capacity_i32(controller->fade_capacity,
                                         controller->fade_count + 1,
                                         4,
                                         sizeof(*controller->fades),
                                         &new_cap)) {
            game3d_release_ref(&clone);
            rt_trap("Game3D.ThirdPersonController: occluder fade capacity overflow");
            return -1;
        }
        rt_game3d_tp_fade_entry *grown =
            (rt_game3d_tp_fade_entry *)realloc(controller->fades, (size_t)new_cap * sizeof(*grown));
        if (!grown) {
            game3d_release_ref(&clone);
            return -1;
        }
        controller->fades = grown;
        controller->fade_capacity = new_cap;
        controller->fade_storage_capacity = new_cap;
        controller->fade_storage_cookie =
            game3d_thirdperson_fade_storage_cookie_value(grown, new_cap);
    }
    rt_game3d_tp_fade_entry *entry = &controller->fades[controller->fade_count];
    memset(entry, 0, sizeof(*entry));
    game3d_thirdperson_assign_instance_ref(
        &entry->node, node, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d));
    game3d_thirdperson_assign_instance_ref(
        &entry->original_material, original, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d));
    entry->fade_material = clone; /* transfer make_instance ownership */
    entry->original_alpha =
        game3d_clamp(game3d_finite_or(rt_material3d_get_alpha(original), 1.0), 0.0, 1.0);
    entry->alpha = entry->original_alpha;
    rt_material3d_set_alpha_mode(clone, RT_MATERIAL3D_ALPHA_MODE_BLEND);
    rt_scene_node3d_set_material(node, clone);
    return controller->fade_count++;
}

/// @brief Per-late-update occluder-fade pass: raycast pivot→eye, fade hit meshes
///   toward RT_GAME3D_TP_FADE_ALPHA, restore meshes that stopped occluding.
/// @param controller Controller owning fade bookkeeping.
/// @param world Borrowed world providing physics and body lookup.
/// @param target Borrowed camera target excluded from fading.
/// @param pivot Three-component camera pivot.
/// @param eye Three-component resolved camera position.
/// @param dt Candidate frame delta used for exponential alpha blending.
static void game3d_thirdperson_update_fades(rt_game3d_thirdperson_controller *controller,
                                            rt_game3d_world *world,
                                            rt_game3d_entity *target,
                                            const double pivot[3],
                                            const double eye[3],
                                            double dt) {
    if (!game3d_thirdperson_fade_storage_valid(controller)) {
        rt_trap("Game3D.ThirdPersonController: corrupt occluder fade storage");
        game3d_thirdperson_reset_fades(controller);
        return;
    }
    for (int32_t i = 0; i < controller->fade_count; ++i)
        controller->fades[i].occluding = 0;

    double dir[3] = {eye[0] - pivot[0], eye[1] - pivot[1], eye[2] - pivot[2]};
    double len = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (isfinite(len) && len > 1e-6 && world->physics) {
        /* All layers on purpose: fadeable props are typically excluded from the
         * boom CollisionMask (no pull-in), yet must still fade when they sit
         * between the pivot and the camera. The raw query writes borrowed body
         * pointers into a stack buffer, so this per-frame pass allocates
         * nothing (no Vec3 handles, no boxed hit list). */
        enum { GAME3D_TP_FADE_QUERY_MAX = 64 };

        void *bodies[GAME3D_TP_FADE_QUERY_MAX];
        double ndir[3] = {dir[0] / len, dir[1] / len, dir[2] / len};
        int32_t hit_count = rt_world3d_raycast_all_bodies_raw(
            world->physics, pivot, ndir, len, -1, bodies, GAME3D_TP_FADE_QUERY_MAX);
        if (hit_count < 0)
            hit_count = 0;
        else if (hit_count > GAME3D_TP_FADE_QUERY_MAX)
            hit_count = GAME3D_TP_FADE_QUERY_MAX;
        for (int32_t h = 0; h < hit_count; ++h) {
            rt_game3d_entity *entity = game3d_world_find_entity_by_body(world, bodies[h]);
            if (!entity || entity == target || !game3d_entity_alive_or_record(entity))
                continue;
            void *node = game3d_entity_node_ref(entity);
            if (!node)
                continue;
            int32_t index = game3d_thirdperson_fade_find(controller, node);
            if (index < 0)
                index = game3d_thirdperson_fade_begin(controller, node);
            if (index >= 0)
                controller->fades[index].occluding = 1;
        }
    }

    /* Animate alphas; restore and compact entries that finished releasing. */
    double blend = 1.0 - exp(-RT_GAME3D_TP_FADE_RATE * game3d_clamp_controller_dt(dt));
    int32_t write = 0;
    for (int32_t i = 0; i < controller->fade_count; ++i) {
        rt_game3d_tp_fade_entry *entry = &controller->fades[i];
        void *node =
            rt_obj_is_instance(entry->node, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d))
                ? entry->node
                : NULL;
        if (!node) {
            /* Node died (despawn/world teardown): drop the clone without touching it. */
            game3d_thirdperson_fade_entry_release(entry);
            continue;
        }
        void *fade_material = rt_obj_is_instance(entry->fade_material,
                                                 RT_G3D_MATERIAL3D_CLASS_ID,
                                                 sizeof(rt_material3d))
                                  ? entry->fade_material
                                  : NULL;
        if (!fade_material) {
            game3d_thirdperson_fade_entry_release(entry);
            continue;
        }
        double target_alpha = entry->occluding ? RT_GAME3D_TP_FADE_ALPHA : entry->original_alpha;
        entry->alpha += (target_alpha - entry->alpha) * blend;
        if (!entry->occluding && fabs(entry->alpha - entry->original_alpha) < 0.01) {
            game3d_thirdperson_fade_entry_release(entry);
            continue;
        }
        rt_material3d_set_alpha(fade_material, entry->alpha);
        if (rt_scene_node3d_get_material(node) != fade_material)
            rt_scene_node3d_set_material(node, fade_material);
        if (write != i)
            controller->fades[write] = *entry;
        ++write;
    }
    controller->fade_count = write;
}

/// @brief GC finalizer: restore faded materials, release retained references.
/// @param obj Finalized ThirdPersonController payload; `NULL` is ignored.
static void game3d_thirdperson_controller_finalize(void *obj) {
    rt_game3d_thirdperson_controller *controller = (rt_game3d_thirdperson_controller *)obj;
    if (!controller)
        return;
    game3d_thirdperson_reset_fades(controller);
    game3d_thirdperson_release_instance_ref(
        &controller->world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world));
    game3d_thirdperson_release_instance_ref(
        &controller->target, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    game3d_thirdperson_release_instance_ref(&controller->character,
                                            RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID,
                                            sizeof(rt_game3d_character_controller));
    game3d_thirdperson_release_instance_ref(
        &controller->lock, RT_G3D_GAME3D_TARGETLOCK_CLASS_ID, sizeof(rt_game3d_targetlock));
}

//=========================================================================
// Construction and properties
//=========================================================================

/// @brief Create a third-person spring-arm controller orbiting @p target_entity.
///   Defaults: distance 4 (0.75..8), pivot 1.5, shoulder (0.35,0,0), pitch -60..75,
///   damping 12, boom radius 0.25, mask all, fade off, aim 1.6/45°. See header.
/// @param world_obj Borrowed live World3D handle retained by the controller.
/// @param target_entity Borrowed live Entity3D handle retained as the orbit target.
/// @return New GC-managed ThirdPersonController handle, or `NULL` after validation/allocation
/// failure.
void *rt_game3d_thirdperson_controller_new(void *world_obj, void *target_entity) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.ThirdPersonController.New: invalid world");
    rt_game3d_entity *entity = game3d_entity_checked(
        target_entity, "Game3D.ThirdPersonController.New: target must be Entity3D");
    if (!world || !entity)
        return NULL;
    if (!game3d_entity_validate_controller_world(
            entity, world, "Game3D.ThirdPersonController.New: target belongs to another world"))
        return NULL;
    rt_game3d_thirdperson_controller *controller =
        (rt_game3d_thirdperson_controller *)rt_obj_new_i64(RT_G3D_GAME3D_THIRDPERSON_CLASS_ID,
                                                           (int64_t)sizeof(*controller));
    if (!controller) {
        rt_trap("Game3D.ThirdPersonController.New: allocation failed");
        return NULL;
    }
    memset(controller, 0, sizeof(*controller));
    rt_obj_set_finalizer(controller, game3d_thirdperson_controller_finalize);
    game3d_thirdperson_assign_instance_ref(
        &controller->world, world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world));
    game3d_thirdperson_assign_instance_ref(
        &controller->target, entity, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    controller->distance = RT_GAME3D_TP_DEFAULT_DISTANCE;
    controller->min_distance = RT_GAME3D_TP_DEFAULT_MIN_DISTANCE;
    controller->max_distance = RT_GAME3D_TP_DEFAULT_MAX_DISTANCE;
    controller->shoulder_offset[0] = RT_GAME3D_TP_DEFAULT_SHOULDER_X;
    controller->aim_shoulder_offset[0] = RT_GAME3D_TP_DEFAULT_SHOULDER_X;
    controller->pivot_height = RT_GAME3D_TP_DEFAULT_PIVOT_HEIGHT;
    controller->pitch_min = RT_GAME3D_TP_DEFAULT_PITCH_MIN;
    controller->pitch_max = RT_GAME3D_TP_DEFAULT_PITCH_MAX;
    controller->damping = RT_GAME3D_DEFAULT_FOLLOW_DAMPING;
    controller->collision_radius = RT_GAME3D_TP_DEFAULT_COLLISION_RADIUS;
    controller->collision_mask = -1;
    controller->aim_distance = RT_GAME3D_TP_DEFAULT_AIM_DISTANCE;
    controller->aim_fov = RT_GAME3D_TP_DEFAULT_AIM_FOV;
    controller->current_distance = controller->distance;
    return controller;
}

/// @brief Get the orbited target entity (NULL if none/stale).
/// @param obj Borrowed ThirdPersonController handle.
/// @return Borrowed live target Entity3D handle, or `NULL`.
void *rt_game3d_thirdperson_controller_get_target(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_target: invalid controller");
    return game3d_thirdperson_target_ref(controller);
}

/// @brief Set the orbited target entity (validated when non-NULL).
/// @param obj Borrowed ThirdPersonController handle.
/// @param target_entity Borrowed same-world Entity3D to retain, or `NULL` to detach.
void rt_game3d_thirdperson_controller_set_target(void *obj, void *target_entity) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_target: invalid controller");
    if (!controller)
        return;
    rt_game3d_entity *target =
        target_entity
            ? game3d_entity_checked(
                  target_entity, "Game3D.ThirdPersonController.set_target: target must be Entity3D")
            : NULL;
    if (target_entity && !target)
        return;
    if (target) {
        rt_game3d_world *world = game3d_thirdperson_world_ref(controller);
        if (!game3d_entity_validate_controller_world(
                target,
                world,
                "Game3D.ThirdPersonController.set_target: target belongs to another world"))
            return;
    }
    if (controller->target != target) {
        game3d_thirdperson_reset_fades(controller);
        game3d_thirdperson_assign_instance_ref(
            &controller->target, target, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity));
    }
}

/// @brief Get the optional CharacterController3D drive slot (NULL if none/invalid).
/// @param obj Borrowed ThirdPersonController handle.
/// @return Borrowed CharacterController3D handle, or `NULL`.
void *rt_game3d_thirdperson_controller_get_character(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_character: invalid controller");
    return game3d_thirdperson_character_ref(controller);
}

/// @brief Set the CharacterController3D drive slot; traps on wrong class or world.
/// @param obj Borrowed ThirdPersonController handle.
/// @param character_controller Borrowed same-world CharacterController3D to retain, or `NULL`.
void rt_game3d_thirdperson_controller_set_character(void *obj, void *character_controller) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_character: invalid controller");
    if (!controller)
        return;
    rt_game3d_character_controller *character =
        character_controller ? game3d_character_controller_checked(
                                   character_controller,
                                   "Game3D.ThirdPersonController.set_character: invalid character")
                             : NULL;
    if (character_controller && !character)
        return;
    if (character) {
        rt_game3d_world *world = game3d_thirdperson_world_ref(controller);
        if (!game3d_character_controller_validate_world(
                character,
                world,
                "Game3D.ThirdPersonController.set_character: character belongs to another world"))
            return;
    }
    game3d_thirdperson_assign_instance_ref(&controller->character,
                                           character,
                                           RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID,
                                           sizeof(rt_game3d_character_controller));
}

/// @brief Get the desired (pre-collision) boom length.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Desired boom length in world units, or zero when invalid.
double rt_game3d_thirdperson_controller_get_distance(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_distance: invalid controller");
    return controller ? controller->distance : 0.0;
}

/// @brief Set the desired boom length (clamped to [min, max]).
/// @param obj Borrowed ThirdPersonController handle.
/// @param distance Requested world-space boom length.
void rt_game3d_thirdperson_controller_set_distance(void *obj, double distance) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_distance: invalid controller");
    if (controller)
        controller->distance =
            game3d_clamp(game3d_finite_or(distance, RT_GAME3D_TP_DEFAULT_DISTANCE),
                         controller->min_distance,
                         controller->max_distance);
}

/// @brief Get the boom pull-in floor.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Minimum boom length in world units, or zero when invalid.
double rt_game3d_thirdperson_controller_get_min_distance(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_minDistance: invalid controller");
    return controller ? controller->min_distance : 0.0;
}

/// @brief Set the boom pull-in floor (non-negative, ≤ max).
/// @param obj Borrowed ThirdPersonController handle.
/// @param min_distance Requested non-negative minimum; also raises the desired distance if needed.
void rt_game3d_thirdperson_controller_set_min_distance(void *obj, double min_distance) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_minDistance: invalid controller");
    if (controller) {
        controller->min_distance =
            game3d_clamp(game3d_finite_or(min_distance, RT_GAME3D_TP_DEFAULT_MIN_DISTANCE),
                         0.0,
                         controller->max_distance);
        if (controller->distance < controller->min_distance)
            controller->distance = controller->min_distance;
    }
}

/// @brief Get the boom length ceiling.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Maximum boom length in world units, or zero when invalid.
double rt_game3d_thirdperson_controller_get_max_distance(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_maxDistance: invalid controller");
    return controller ? controller->max_distance : 0.0;
}

/// @brief Set the boom length ceiling (≥ min).
/// @param obj Borrowed ThirdPersonController handle.
/// @param max_distance Requested maximum; also lowers the desired distance if needed.
void rt_game3d_thirdperson_controller_set_max_distance(void *obj, double max_distance) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_maxDistance: invalid controller");
    if (controller) {
        controller->max_distance =
            game3d_clamp(game3d_finite_or(max_distance, RT_GAME3D_TP_DEFAULT_MAX_DISTANCE),
                         controller->min_distance,
                         RT_GAME3D_COORD_ABS_MAX);
        if (controller->distance > controller->max_distance)
            controller->distance = controller->max_distance;
    }
}

/// @brief Get the local-space shoulder offset as a Vec3.
/// @param obj Borrowed ThirdPersonController handle.
/// @return New GC-managed Vec3 containing the free-camera shoulder offset, or `NULL` when invalid.
void *rt_game3d_thirdperson_controller_get_shoulder_offset(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_shoulderOffset: invalid controller");
    if (!controller)
        return NULL;
    return rt_vec3_new(controller->shoulder_offset[0],
                       controller->shoulder_offset[1],
                       controller->shoulder_offset[2]);
}

/// @brief Set the local-space shoulder offset; traps on a non-Vec3.
/// @param obj Borrowed ThirdPersonController handle.
/// @param offset Borrowed Vec3 whose sanitized components also seed the aim offset.
void rt_game3d_thirdperson_controller_set_shoulder_offset(void *obj, void *offset) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_shoulderOffset: invalid controller");
    if (!rt_g3d_is_vec3(offset)) {
        rt_trap("Game3D.ThirdPersonController.set_shoulderOffset: offset must be Vec3");
        return;
    }
    if (controller) {
        controller->shoulder_offset[0] = game3d_clamp_coord_or(rt_vec3_x(offset), 0.0);
        controller->shoulder_offset[1] = game3d_clamp_coord_or(rt_vec3_y(offset), 0.0);
        controller->shoulder_offset[2] = game3d_clamp_coord_or(rt_vec3_z(offset), 0.0);
        /* Aim shoulder tracks the free shoulder until a dedicated setter exists. */
        controller->aim_shoulder_offset[0] = controller->shoulder_offset[0];
        controller->aim_shoulder_offset[1] = controller->shoulder_offset[1];
        controller->aim_shoulder_offset[2] = controller->shoulder_offset[2];
    }
}

/// @brief Get the pivot height above the target entity origin.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Stored world-space pivot height, or zero when invalid.
double rt_game3d_thirdperson_controller_get_pivot_height(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_pivotHeight: invalid controller");
    return controller ? controller->pivot_height : 0.0;
}

/// @brief Set the pivot height above the target entity origin.
/// @param obj Borrowed ThirdPersonController handle.
/// @param height Candidate coordinate-bounded pivot height.
void rt_game3d_thirdperson_controller_set_pivot_height(void *obj, double height) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_pivotHeight: invalid controller");
    if (controller)
        controller->pivot_height = game3d_clamp_coord_or(height, RT_GAME3D_TP_DEFAULT_PIVOT_HEIGHT);
}

/// @brief Get the boom-release smoothing factor.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Sanitized non-negative damping factor, or zero when invalid.
double rt_game3d_thirdperson_controller_get_damping(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_damping: invalid controller");
    return controller ? game3d_nonnegative_clamped_or(controller->damping,
                                                      RT_GAME3D_DEFAULT_FOLLOW_DAMPING,
                                                      RT_GAME3D_DAMPING_MAX)
                      : 0.0;
}

/// @brief Set the boom-release smoothing factor (negatives reset to the default).
/// @param obj Borrowed ThirdPersonController handle.
/// @param damping Requested exponential damping factor.
void rt_game3d_thirdperson_controller_set_damping(void *obj, double damping) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_damping: invalid controller");
    if (controller)
        controller->damping = game3d_nonnegative_clamped_or(
            damping, RT_GAME3D_DEFAULT_FOLLOW_DAMPING, RT_GAME3D_DAMPING_MAX);
}

/// @brief Get the camera orbit yaw in degrees.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Stored wrapped yaw in degrees, or zero when invalid.
double rt_game3d_thirdperson_controller_get_yaw(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_yaw: invalid controller");
    return controller ? controller->yaw : 0.0;
}

/// @brief Set the camera orbit yaw in degrees (wrapped to [-180, 180)).
/// @param obj Borrowed ThirdPersonController handle.
/// @param yaw Candidate yaw in degrees.
void rt_game3d_thirdperson_controller_set_yaw(void *obj, double yaw) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_yaw: invalid controller");
    if (controller)
        controller->yaw = game3d_thirdperson_wrap_yaw(game3d_finite_or(yaw, 0.0));
}

/// @brief Get the camera orbit pitch in degrees (positive = above the pivot).
/// @param obj Borrowed ThirdPersonController handle.
/// @return Stored clamped pitch in degrees, or zero when invalid.
double rt_game3d_thirdperson_controller_get_pitch(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_pitch: invalid controller");
    return controller ? controller->pitch : 0.0;
}

/// @brief Set the camera orbit pitch in degrees (clamped to [PitchMin, PitchMax]).
/// @param obj Borrowed ThirdPersonController handle.
/// @param pitch Candidate orbit pitch in degrees.
void rt_game3d_thirdperson_controller_set_pitch(void *obj, double pitch) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_pitch: invalid controller");
    if (controller)
        controller->pitch = game3d_clamp(
            game3d_finite_or(pitch, 0.0), controller->pitch_min, controller->pitch_max);
}

/// @brief Get the boom sweep sphere radius.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Positive collision radius in world units, or zero when invalid.
double rt_game3d_thirdperson_controller_get_collision_radius(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_collisionRadius: invalid controller");
    return controller ? controller->collision_radius : 0.0;
}

/// @brief Set the boom sweep sphere radius (positive; non-finite resets default).
/// @param obj Borrowed ThirdPersonController handle.
/// @param radius Requested positive sphere radius.
void rt_game3d_thirdperson_controller_set_collision_radius(void *obj, double radius) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_collisionRadius: invalid controller");
    if (controller)
        controller->collision_radius = game3d_positive_clamped_or(
            radius, RT_GAME3D_TP_DEFAULT_COLLISION_RADIUS, RT_GAME3D_SCALE_ABS_MAX);
}

/// @brief Get the boom collision layer mask.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Raw collision-layer mask, or zero when invalid.
int64_t rt_game3d_thirdperson_controller_get_collision_mask(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_collisionMask: invalid controller");
    return controller ? controller->collision_mask : 0;
}

/// @brief Set the boom collision layer mask (exclude character/projectile layers).
/// @param obj Borrowed ThirdPersonController handle.
/// @param mask Raw physics collision-layer bit mask.
void rt_game3d_thirdperson_controller_set_collision_mask(void *obj, int64_t mask) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_collisionMask: invalid controller");
    if (controller)
        controller->collision_mask = mask;
}

/// @brief Get whether occluder fading is enabled.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Nonzero when occluder material fading is enabled.
int8_t rt_game3d_thirdperson_controller_get_occlusion_fade(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_occlusionFade: invalid controller");
    return controller ? controller->occlusion_fade : 0;
}

/// @brief Enable/disable occluder fading; disabling restores faded materials now.
/// @param obj Borrowed ThirdPersonController handle.
/// @param enabled Nonzero to enable the late-update fade pass.
void rt_game3d_thirdperson_controller_set_occlusion_fade(void *obj, int8_t enabled) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_occlusionFade: invalid controller");
    if (controller) {
        controller->occlusion_fade = enabled ? 1 : 0;
        if (!controller->occlusion_fade)
            game3d_thirdperson_reset_fades(controller);
    }
}

/// @brief Get the aim-mode request flag.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Nonzero when aim mode is requested.
int8_t rt_game3d_thirdperson_controller_get_aiming(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_aiming: invalid controller");
    return controller ? controller->aiming : 0;
}

/// @brief Set the aim-mode request flag (blend animates in Update).
/// @param obj Borrowed ThirdPersonController handle.
/// @param aiming Nonzero to blend toward aim framing.
void rt_game3d_thirdperson_controller_set_aiming(void *obj, int8_t aiming) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_aiming: invalid controller");
    if (controller)
        controller->aiming = aiming ? 1 : 0;
}

/// @brief Get the aim-mode boom length.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Stored aim boom length in world units, or zero when invalid.
double rt_game3d_thirdperson_controller_get_aim_distance(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_aimDistance: invalid controller");
    return controller ? controller->aim_distance : 0.0;
}

/// @brief Set the aim-mode boom length.
/// @param obj Borrowed ThirdPersonController handle.
/// @param distance Requested positive aim boom length.
void rt_game3d_thirdperson_controller_set_aim_distance(void *obj, double distance) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_aimDistance: invalid controller");
    if (controller)
        controller->aim_distance = game3d_positive_clamped_or(
            distance, RT_GAME3D_TP_DEFAULT_AIM_DISTANCE, RT_GAME3D_COORD_ABS_MAX);
}

/// @brief Get the aim-mode camera FOV in degrees.
/// @param obj Borrowed ThirdPersonController handle.
/// @return Stored aim FOV in degrees, or zero when invalid.
double rt_game3d_thirdperson_controller_get_aim_fov(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_aimFov: invalid controller");
    return controller ? controller->aim_fov : 0.0;
}

/// @brief Set the aim-mode camera FOV in degrees (1..179).
/// @param obj Borrowed ThirdPersonController handle.
/// @param fov Requested vertical field of view in degrees.
void rt_game3d_thirdperson_controller_set_aim_fov(void *obj, double fov) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_aimFov: invalid controller");
    if (controller)
        controller->aim_fov =
            game3d_clamp(game3d_finite_or(fov, RT_GAME3D_TP_DEFAULT_AIM_FOV), 1.0, 179.0);
}

/// @brief Get the installed TargetLock3D framing source (NULL if none).
/// @param obj Borrowed ThirdPersonController handle.
/// @return Borrowed TargetLock3D handle, or `NULL` when absent or invalid.
void *rt_game3d_thirdperson_controller_get_lock_target(void *obj) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.get_lockTarget: invalid controller");
    return game3d_thirdperson_lock_ref(controller);
}

/// @brief Install a TargetLock3D framing source (NULL to clear); traps on wrong class.
/// @param obj Borrowed ThirdPersonController handle.
/// @param lock Borrowed TargetLock3D handle to retain, or `NULL` to clear.
void rt_game3d_thirdperson_controller_set_lock_target(void *obj, void *lock) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.set_lockTarget: invalid controller");
    if (!controller)
        return;
    rt_game3d_targetlock *target_lock =
        lock ? game3d_targetlock_checked(
                   lock, "Game3D.ThirdPersonController.set_lockTarget: value must be TargetLock3D")
             : NULL;
    if (lock && !target_lock)
        return;
    rt_game3d_world *controller_world = game3d_thirdperson_world_ref(controller);
    rt_game3d_world *lock_world = target_lock && rt_obj_is_instance(target_lock->world,
                                                                    RT_G3D_GAME3D_WORLD_CLASS_ID,
                                                                    sizeof(rt_game3d_world))
                                      ? (rt_game3d_world *)target_lock->world
                                      : NULL;
    if (target_lock && controller_world && lock_world != controller_world) {
        rt_trap("Game3D.ThirdPersonController.set_lockTarget: lock belongs to another world");
        return;
    }
    game3d_thirdperson_assign_instance_ref(&controller->lock,
                                           target_lock,
                                           RT_G3D_GAME3D_TARGETLOCK_CLASS_ID,
                                           sizeof(rt_game3d_targetlock));
}

//=========================================================================
// Frame hooks
//=========================================================================

/// @brief Pre-physics update: consume look input into yaw/pitch, advance the aim
///   blend, and drive the optional character camera-relatively (yaw basis).
/// @param obj Borrowed ThirdPersonController handle.
/// @param world_obj Borrowed live World3D handle expected to own the controller.
/// @param dt Candidate simulation delta, sanitized before aim and movement integration.
void rt_game3d_thirdperson_controller_update(void *obj, void *world_obj, double dt) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.update: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.ThirdPersonController.update: invalid world");
    rt_game3d_input *input = world && rt_obj_is_instance(world->input,
                                                         RT_G3D_GAME3D_INPUT_CLASS_ID,
                                                         sizeof(rt_game3d_input))
                                 ? (rt_game3d_input *)world->input
                                 : NULL;
    if (!controller || !world || !input)
        return;
    if (!game3d_camera_controller_validate_world(
            controller,
            world,
            "Game3D.ThirdPersonController.update: controller belongs to another world"))
        return;
    dt = game3d_clamp_controller_dt(dt);
    if (dt <= 0.0)
        return;

    /* Lock maintenance: tick the installed TargetLock3D once per world step. */
    rt_game3d_targetlock *lock = game3d_thirdperson_lock_ref(controller);
    int8_t lock_engaged = 0;
    if (lock && (!lock->world || lock->world == world)) {
        rt_game3d_targetlock_update(lock, dt);
        lock_engaged = rt_game3d_targetlock_get_target(lock) != NULL;
    }

    /* Look input: Input3D.lookAxis merges mouse + pad with sensitivity applied.
     * Ignored while lock framing owns yaw/pitch (Cycle gestures are game-side). */
    if (!lock_engaged) {
        void *look = rt_game3d_input_look_axis(input);
        if (look) {
            double look_x = game3d_finite_or(rt_vec2_x(look), 0.0);
            double look_y = game3d_finite_or(rt_vec2_y(look), 0.0);
            controller->yaw = game3d_thirdperson_wrap_yaw(controller->yaw - look_x);
            controller->pitch = game3d_clamp(
                controller->pitch + look_y, controller->pitch_min, controller->pitch_max);
            game3d_release_ref(&look);
        }
    }

    /* Aim blend: linear ramp toward the requested state. */
    double aim_target = controller->aiming ? 1.0 : 0.0;
    double step = RT_GAME3D_TP_AIM_BLEND_RATE * dt;
    if (controller->aim_blend < aim_target)
        controller->aim_blend = game3d_clamp(controller->aim_blend + step, 0.0, aim_target);
    else if (controller->aim_blend > aim_target)
        controller->aim_blend = game3d_clamp(controller->aim_blend - step, aim_target, 1.0);

    /* Camera-relative character drive along the yaw basis (pitch ignored on purpose). */
    void *character_controller = game3d_thirdperson_character_ref(controller);
    if (character_controller) {
        rt_game3d_character_controller *character = game3d_character_controller_checked(
            character_controller, "Game3D.ThirdPersonController.update: invalid character");
        if (!game3d_character_controller_validate_world(
                character,
                world,
                "Game3D.ThirdPersonController.update: character belongs to another world"))
            return;
        double forward[3];
        game3d_thirdperson_forward(controller->yaw, 0.0, forward);
        double fx = forward[0];
        double fz = forward[2];
        /* right = cross(forward, up) for the planar basis. */
        double rx = -fz;
        double rz = fx;
        game3d_character_controller_drive(character, input, fx, fz, rx, rz, dt);
    }
}

/// @brief Post-sync late update: spring-arm the camera behind the target with a
///   sphere-swept boom (instant pull-in, damped release), apply aim FOV blending,
///   and run the optional occluder-fade pass.
/// @param obj Borrowed ThirdPersonController handle.
/// @param world_obj Borrowed live World3D handle expected to own the controller.
/// @param dt Candidate frame delta used for release damping and occlusion fades.
void rt_game3d_thirdperson_controller_late_update(void *obj, void *world_obj, double dt) {
    rt_game3d_thirdperson_controller *controller = game3d_thirdperson_controller_checked(
        obj, "Game3D.ThirdPersonController.lateUpdate: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.ThirdPersonController.lateUpdate: invalid world");
    void *camera = world ? rt_camera3d_checked_or_stack(world->camera) : NULL;
    if (!controller || !world || !camera)
        return;
    if (!game3d_camera_controller_validate_world(
            controller,
            world,
            "Game3D.ThirdPersonController.lateUpdate: controller belongs to another world"))
        return;
    rt_game3d_entity *target = game3d_thirdperson_target_ref(controller);
    if (!target) {
        game3d_thirdperson_reset_fades(controller);
        return;
    }
    if (!game3d_thirdperson_fade_storage_valid(controller)) {
        rt_trap("Game3D.ThirdPersonController.lateUpdate: corrupt occluder fade storage");
        game3d_thirdperson_reset_fades(controller);
        return;
    }
    dt = game3d_clamp_controller_dt(dt);
    if (dt <= 0.0)
        return;

    double target_pos[3];
    if (!game3d_entity_world_position_components(target, target_pos))
        return;

    double blend = game3d_clamp(controller->aim_blend, 0.0, 1.0);

    /* Lock-on framing: while a lock target is engaged, yaw/pitch ease toward the
     * player→target bearing instead of following look input; the look point is
     * pulled 40% toward the locked target. Resuming free look continues from the
     * framed angles (no snap). */
    double locked_pivot[3] = {0.0, 0.0, 0.0};
    int8_t lock_engaged = 0;
    {
        rt_game3d_targetlock *lock = game3d_thirdperson_lock_ref(controller);
        if (lock && lock->world && lock->world != world)
            lock = NULL;
        rt_game3d_entity *locked = lock && rt_obj_is_instance(lock->target,
                                                              RT_G3D_GAME3D_ENTITY_CLASS_ID,
                                                              sizeof(rt_game3d_entity))
                                       ? (rt_game3d_entity *)lock->target
                                       : NULL;
        if (locked && game3d_entity_alive_or_record(locked) &&
            game3d_entity_world_position_components(locked, locked_pivot)) {
            lock_engaged = 1;
            locked_pivot[1] += controller->pivot_height;
            double dx = locked_pivot[0] - target_pos[0];
            double dz = locked_pivot[2] - target_pos[2];
            double planar = sqrt(dx * dx + dz * dz);
            if (planar > 1e-6) {
                /* forward = (-sin(yaw), -cos(yaw)) ⇒ bearing yaw = atan2(-dx, -dz). */
                double bearing_yaw = atan2(-dx, -dz) * (180.0 / RT_GAME3D_PI);
                double dy = (target_pos[1] + controller->pivot_height) - locked_pivot[1];
                double pitch_target =
                    game3d_clamp(atan2(dy, planar) * (180.0 / RT_GAME3D_PI) + 12.0,
                                 controller->pitch_min,
                                 controller->pitch_max);
                double ease = 1.0 - exp(-8.0 * dt);
                double yaw_delta = bearing_yaw - controller->yaw;
                while (yaw_delta > 180.0)
                    yaw_delta -= 360.0;
                while (yaw_delta < -180.0)
                    yaw_delta += 360.0;
                controller->yaw = game3d_thirdperson_wrap_yaw(controller->yaw + yaw_delta * ease);
                controller->pitch =
                    game3d_clamp(controller->pitch + (pitch_target - controller->pitch) * ease,
                                 controller->pitch_min,
                                 controller->pitch_max);
            }
        }
    }

    /* Pivot: entity origin + pivot height + yaw-rotated shoulder offset. */
    double fwd_planar[3];
    game3d_thirdperson_forward(controller->yaw, 0.0, fwd_planar);
    double right_x = -fwd_planar[2];
    double right_z = fwd_planar[0];
    double shoulder[3];
    for (int i = 0; i < 3; ++i)
        shoulder[i] = controller->shoulder_offset[i] +
                      (controller->aim_shoulder_offset[i] - controller->shoulder_offset[i]) * blend;
    double pivot[3];
    pivot[0] = game3d_clamp_coord_or(
        target_pos[0] + right_x * shoulder[0] + fwd_planar[0] * shoulder[2], 0.0);
    pivot[1] = game3d_clamp_coord_or(target_pos[1] + controller->pivot_height + shoulder[1], 0.0);
    pivot[2] = game3d_clamp_coord_or(
        target_pos[2] + right_z * shoulder[0] + fwd_planar[2] * shoulder[2], 0.0);

    /* Desired boom vector from orbit yaw/pitch. */
    double forward[3];
    game3d_thirdperson_forward(controller->yaw, controller->pitch, forward);
    double want = controller->distance + (controller->aim_distance - controller->distance) * blend;
    want = game3d_clamp(want, controller->min_distance, controller->max_distance);

    /* Boom sweep: pull in on hit (instant), release smoothly. */
    double allowed = want;
    if (world->physics && want > 1e-9) {
        void *center = rt_vec3_new(pivot[0], pivot[1], pivot[2]);
        void *delta = rt_vec3_new(-forward[0] * want, -forward[1] * want, -forward[2] * want);
        void *hit = rt_world3d_sweep_sphere(world->physics,
                                            center,
                                            controller->collision_radius,
                                            delta,
                                            controller->collision_mask);
        if (hit) {
            if (rt_physics_hit3d_get_started_penetrating(hit)) {
                allowed = controller->min_distance;
            } else {
                double fraction = game3d_clamp(rt_physics_hit3d_get_fraction(hit), 0.0, 1.0);
                allowed = fraction * want - RT_GAME3D_TP_BOOM_SKIN;
            }
            allowed = game3d_clamp(allowed, controller->min_distance, want);
        }
        game3d_release_ref(&hit);
        game3d_release_ref(&delta);
        game3d_release_ref(&center);
    }

    double current = controller->current_distance;
    if (!isfinite(current) || current <= 0.0)
        current = allowed;
    if (allowed < current) {
        current = allowed; /* never clip: instant pull-in */
    } else {
        double damping = game3d_nonnegative_clamped_or(
            controller->damping, RT_GAME3D_DEFAULT_FOLLOW_DAMPING, RT_GAME3D_DAMPING_MAX);
        double alpha = damping <= 0.0 ? 1.0 : 1.0 - exp(-damping * dt);
        current += (allowed - current) * game3d_clamp(alpha, 0.0, 1.0);
    }
    controller->current_distance = current;

    double eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = game3d_clamp_coord_or(pivot[i] - forward[i] * current, pivot[i]);

    double look[3] = {pivot[0], pivot[1], pivot[2]};
    if (lock_engaged)
        for (int i = 0; i < 3; ++i)
            look[i] =
                game3d_clamp_coord_or(pivot[i] + (locked_pivot[i] - pivot[i]) * 0.4, pivot[i]);

    rt_camera3d_look_at_components(
        camera, eye[0], eye[1], eye[2], look[0], look[1], look[2], 0.0, 1.0, 0.0);

    /* Aim FOV: capture the base FOV when the blend engages, restore when it ends. */
    if (blend > 0.0) {
        if (!controller->base_fov_valid) {
            controller->base_fov = rt_camera3d_get_fov(camera);
            controller->base_fov_valid = 1;
        }
        rt_camera3d_set_fov(
            camera, controller->base_fov + (controller->aim_fov - controller->base_fov) * blend);
    } else if (controller->base_fov_valid) {
        rt_camera3d_set_fov(camera, controller->base_fov);
        controller->base_fov_valid = 0;
    }

    /* Occluder fade pass. */
    if (controller->occlusion_fade)
        game3d_thirdperson_update_fades(controller, world, target, pivot, eye, dt);
    else if (controller->fade_count > 0)
        game3d_thirdperson_reset_fades(controller);
}
