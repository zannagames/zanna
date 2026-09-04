//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_combat.c
// Purpose: Melee-combat foundation for Zanna.Game3D — Hitbox3D bone/entity
//   attached hit and hurt volumes with manual + animation-window activation,
//   the per-step world combat pass emitting polled HitEvent3D records, and the
//   Health3D component (damage/heal/i-frames/death lifecycle, knockback helper,
//   polled DamageEvent3D buffer).
// Key invariants:
//   - Hit volumes are combat-only: never physics bodies, never visible to
//     raycasts/sweeps; overlap uses scratch bodies through the narrow-phase.
//   - One hit per activation per victim (rehit suppression resets when the
//     hitbox goes inactive).
//   - One-shot health flags and both event buffers clear at the start of each
//     combat pass, so events emitted during or after a step survive to the
//     poll point after that step.
//   - Entity teardown NULLs component backrefs before releasing, so surviving
//     Hitbox3D/Health3D handles fail closed.
// Ownership/Lifetime:
//   - Entities own their hitboxes/health (retained slots); components hold
//     plain backrefs cleared at teardown. Event records retain their handles
//     until the buffer clears.
// Links: rt_game3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Game3D combat volumes, health state, and polled combat events.
/// @details Bone- or entity-attached hit and hurt volumes are evaluated outside Physics3D in a
///   deterministic per-world pass. Activation windows suppress repeat victims, Health3D applies
///   damage and invulnerability state, and bounded event records retain their participants.

#include "rt_animcontroller3d.h"
#include "rt_collider3d.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_scene3d.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GAME3D_COMBAT_MAX_HIT_VOLUMES 128
#define GAME3D_COMBAT_MAX_HURT_VOLUMES 512

//=========================================================================
// Small quaternion helpers (local; combat poses compose node × bone × offset)
//=========================================================================

/// @brief out = a ⊗ b (Hamilton product, xyzw layout).
/// @param a Left quaternion in XYZW component order.
/// @param b Right quaternion in XYZW component order.
/// @param out Four-component output receiving the Hamilton product.
static void combat_quat_mul(const double a[4], const double b[4], double out[4]) {
    double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

/// @brief Rotate vector @p v by quaternion @p q (xyzw).
/// @param q Rotation quaternion in XYZW component order.
/// @param v Three-component input vector.
/// @param out Three-component output receiving the rotated vector.
static void combat_quat_rotate(const double q[4], const double v[3], double out[3]) {
    double cx = q[1] * v[2] - q[2] * v[1];
    double cy = q[2] * v[0] - q[0] * v[2];
    double cz = q[0] * v[1] - q[1] * v[0];
    double ccx = q[1] * cz - q[2] * cy;
    double ccy = q[2] * cx - q[0] * cz;
    double ccz = q[0] * cy - q[1] * cx;
    out[0] = v[0] + 2.0 * (q[3] * cx + ccx);
    out[1] = v[1] + 2.0 * (q[3] * cy + ccy);
    out[2] = v[2] + 2.0 * (q[3] * cz + ccz);
}

//=========================================================================
// Hitbox3D
//=========================================================================

/// @brief Validate @p obj as a Hitbox3D handle, trapping @p method on mismatch.
/// @param obj Candidate Hitbox3D handle.
/// @param method Trap message emitted on runtime-class mismatch.
/// @return The validated hitbox, or NULL after reporting an invalid handle.
static rt_game3d_hitbox *game3d_hitbox_checked(void *obj, const char *method) {
    rt_game3d_hitbox *hitbox =
        (rt_game3d_hitbox *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_HITBOX_CLASS_ID);
    if (!hitbox)
        rt_trap(method);
    return hitbox;
}

/// @brief Validate @p obj as a Health3D handle, trapping @p method on mismatch.
/// @param obj Candidate Health3D handle.
/// @param method Trap message emitted on runtime-class mismatch.
/// @return The validated health component, or NULL after reporting an invalid handle.
static rt_game3d_health *game3d_health_checked(void *obj, const char *method) {
    rt_game3d_health *health =
        (rt_game3d_health *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_HEALTH_CLASS_ID);
    if (!health)
        rt_trap(method);
    return health;
}

/// @brief Validate one dense private combat array before traversal or append.
static int game3d_combat_array_valid(int32_t count, int32_t capacity, const void *items) {
    return count >= 0 && capacity >= 0 && count <= capacity && (capacity == 0 || items);
}

/// @brief GC finalizer for Hitbox3D: release the retained collider shape.
/// @param obj Hitbox3D allocation being finalized.
static void game3d_hitbox_finalize(void *obj) {
    rt_game3d_hitbox *hitbox = (rt_game3d_hitbox *)obj;
    if (!hitbox)
        return;
    game3d_release_typed_ref(&hitbox->collider, RT_G3D_COLLIDER3D_CLASS_ID);
}

/// @brief Append @p hitbox to @p entity's retained combat-volume array.
/// @param entity Entity3D that will own the hitbox reference.
/// @param hitbox Hitbox3D to append.
/// @return 1 when retained and appended, or 0 when the array cannot grow.
static int game3d_entity_append_hitbox(rt_game3d_entity *entity, rt_game3d_hitbox *hitbox) {
    int32_t new_cap;
    if (!entity || !hitbox ||
        !game3d_combat_array_valid(entity->hitbox_count, entity->hitbox_capacity, entity->hitboxes))
        return 0;
    if (entity->hitbox_count >= entity->hitbox_capacity) {
        if (entity->hitbox_count == INT32_MAX ||
            !game3d_checked_capacity_i32(entity->hitbox_capacity,
                                         entity->hitbox_count + 1,
                                         4,
                                         sizeof(*entity->hitboxes),
                                         &new_cap))
            return 0;
        void **grown = (void **)realloc(entity->hitboxes, (size_t)new_cap * sizeof(void *));
        if (!grown)
            return 0;
        entity->hitboxes = grown;
        entity->hitbox_capacity = new_cap;
    }
    entity->hitboxes[entity->hitbox_count] = hitbox;
    rt_obj_retain_maybe(hitbox);
    entity->hitbox_count += 1;
    return 1;
}

/// @brief Shared Hitbox3D constructor body (entity-space or bone attachment).
/// @param entity_obj Entity3D that will own and position the volume.
/// @param collider Collider3D shape retained by the volume.
/// @param bone_index Skeleton bone index, or -1 for entity-space attachment.
/// @param api_name Trap context used when validating the owner.
/// @return A newly registered Hitbox3D handle, or NULL on validation, allocation, or registration
/// failure.
static void *game3d_hitbox_new_impl(void *entity_obj,
                                    void *collider,
                                    int64_t bone_index,
                                    const char *api_name) {
    rt_game3d_entity *entity = game3d_entity_checked(entity_obj, api_name);
    if (!entity)
        return NULL;
    if (!rt_g3d_has_class(collider, RT_G3D_COLLIDER3D_CLASS_ID)) {
        rt_trap("Game3D.Hitbox3D.New: shape must be Collider3D");
        return NULL;
    }
    rt_game3d_hitbox *hitbox =
        (rt_game3d_hitbox *)rt_obj_new_i64(RT_G3D_GAME3D_HITBOX_CLASS_ID, (int64_t)sizeof(*hitbox));
    if (!hitbox) {
        rt_trap("Game3D.Hitbox3D.New: allocation failed");
        return NULL;
    }
    memset(hitbox, 0, sizeof(*hitbox));
    rt_obj_set_finalizer(hitbox, game3d_hitbox_finalize);
    hitbox->entity = entity;
    game3d_assign_typed_ref(&hitbox->collider, collider, RT_G3D_COLLIDER3D_CLASS_ID);
    hitbox->bone_index = bone_index;
    hitbox->kind = RT_GAME3D_HITBOX_KIND_HURT;
    hitbox->channel = 1;
    if (!game3d_entity_append_hitbox(entity, hitbox)) {
        hitbox->entity = NULL;
        if (rt_obj_release_check0(hitbox))
            rt_obj_free(hitbox);
        rt_trap("Game3D.Hitbox3D.New: hitbox registration failed");
        return NULL;
    }
    /* Two live references now: the entity's registration retain and the
     * constructor's, which the caller adopts. Dropping the constructor's here
     * (the pre-2026-08 behavior) left the caller's adopted reference
     * uncounted, so a short-lived script local freed the hitbox while the
     * entity's combat list still pointed at it — use-after-free volumes that
     * produced phantom hits and silent misses. */
    return hitbox;
}

/// @brief Create an entity-space combat volume registered on @p entity_obj.
///   Defaults: kind Hurt, team 0, channel 1, inactive. See header.
/// @param entity_obj Entity3D that will own and position the volume.
/// @param collider Collider3D shape retained by the hitbox.
/// @return A new Hitbox3D handle, or NULL on failure.
void *rt_game3d_hitbox_new(void *entity_obj, void *collider) {
    return game3d_hitbox_new_impl(entity_obj, collider, -1, "Game3D.Hitbox3D.New: invalid entity");
}

/// @brief Create a bone-attached combat volume; traps when the entity has no
///   animator/skeleton or the bone name is unknown. See header.
/// @param entity_obj Entity3D whose animator supplies the skeleton.
/// @param bone_name Runtime string naming the attachment bone.
/// @param collider Collider3D shape retained by the hitbox.
/// @return A new bone-attached Hitbox3D handle, or NULL on failure.
void *rt_game3d_hitbox_new_on_bone(void *entity_obj, rt_string bone_name, void *collider) {
    rt_game3d_entity *entity =
        game3d_entity_checked(entity_obj, "Game3D.Hitbox3D.NewOnBone: invalid entity");
    if (!entity)
        return NULL;
    void *animator = game3d_entity_anim_ref(entity);
    void *controller = animator ? rt_game3d_animator_get_controller(animator) : NULL;
    void *skeleton = controller ? rt_anim_controller3d_get_skeleton(controller) : NULL;
    if (!skeleton) {
        rt_trap("Game3D.Hitbox3D.NewOnBone: entity needs an animator with a skeleton");
        return NULL;
    }
    int64_t bone_index = rt_skeleton3d_find_bone(skeleton, bone_name);
    if (bone_index < 0) {
        rt_trap("Game3D.Hitbox3D.NewOnBone: unknown bone name");
        return NULL;
    }
    return game3d_hitbox_new_impl(
        entity_obj, collider, bone_index, "Game3D.Hitbox3D.NewOnBone: invalid entity");
}

/// @brief Get the volume kind (0 = Hurt, 1 = Hit).
/// @param obj Hitbox3D to query.
/// @return RT_GAME3D_HITBOX_KIND_HURT or RT_GAME3D_HITBOX_KIND_HIT.
int64_t rt_game3d_hitbox_get_kind(void *obj) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.get_kind: invalid hitbox");
    return hitbox ? hitbox->kind : 0;
}

/// @brief Set the volume kind (0 = Hurt, 1 = Hit); other values trap.
/// @param obj Hitbox3D to configure.
/// @param kind Valid RT_GAME3D_HITBOX_KIND_* value.
void rt_game3d_hitbox_set_kind(void *obj, int64_t kind) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.set_kind: invalid hitbox");
    if (!hitbox)
        return;
    if (kind != RT_GAME3D_HITBOX_KIND_HURT && kind != RT_GAME3D_HITBOX_KIND_HIT) {
        rt_trap("Game3D.Hitbox3D.set_kind: kind must be HitboxKind.Hurt or HitboxKind.Hit");
        return;
    }
    hitbox->kind = (int8_t)kind;
}

/// @brief Get the team id (same-team pairs are skipped unless friendly fire).
/// @param obj Hitbox3D to query.
/// @return The application-defined team identifier, or 0 when invalid.
int64_t rt_game3d_hitbox_get_team(void *obj) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.get_team: invalid hitbox");
    return hitbox ? hitbox->team : 0;
}

/// @brief Set the team id.
/// @param obj Hitbox3D to configure.
/// @param team Application-defined team identifier.
void rt_game3d_hitbox_set_team(void *obj, int64_t team) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.set_team: invalid hitbox");
    if (hitbox)
        hitbox->team = team;
}

/// @brief Get the channel bitmask (hit×hurt require overlapping channels).
/// @param obj Hitbox3D to query.
/// @return The configured combat-channel bit mask, or 0 when invalid.
int64_t rt_game3d_hitbox_get_channel(void *obj) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.get_channel: invalid hitbox");
    return hitbox ? hitbox->channel : 0;
}

/// @brief Set the channel bitmask.
/// @param obj Hitbox3D to configure.
/// @param channel Combat-channel bits used by pair filtering.
void rt_game3d_hitbox_set_channel(void *obj, int64_t channel) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.set_channel: invalid hitbox");
    if (hitbox)
        hitbox->channel = channel;
}

/// @brief Get the manual activation switch.
/// @param obj Hitbox3D to query.
/// @return 1 when manually active, or 0 when inactive or invalid.
int8_t rt_game3d_hitbox_get_active(void *obj) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.get_active: invalid hitbox");
    return hitbox ? hitbox->active : 0;
}

/// @brief Set the manual activation switch (scripted attacks).
/// @param obj Hitbox3D to configure.
/// @param active Non-zero to activate the volume; zero to deactivate it.
void rt_game3d_hitbox_set_active(void *obj, int8_t active) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.set_active: invalid hitbox");
    if (hitbox)
        hitbox->active = active ? 1 : 0;
}

/// @brief Get the friendly-fire flag on this attacking volume.
/// @param obj Hitbox3D to query.
/// @return 1 when same-team victims are allowed, or 0 otherwise.
int8_t rt_game3d_hitbox_get_friendly_fire(void *obj) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.get_friendlyFire: invalid hitbox");
    return hitbox ? hitbox->friendly_fire : 0;
}

/// @brief Set the friendly-fire flag (allow same-team hits from this attacker).
/// @param obj Hitbox3D to configure.
/// @param enabled Non-zero to allow same-team hits; zero to filter them.
void rt_game3d_hitbox_set_friendly_fire(void *obj, int8_t enabled) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.set_friendlyFire: invalid hitbox");
    if (hitbox)
        hitbox->friendly_fire = enabled ? 1 : 0;
}

/// @brief Fluent: bind an activation window — live while the owner's animator
///   base state is @p state_name and its time is within [t0, t1]. Up to 4.
/// @param obj Hitbox3D to configure.
/// @param state_name Non-empty runtime string naming the animator state.
/// @param t0 First endpoint of the activation interval in seconds.
/// @param t1 Second endpoint; endpoints are reordered when necessary.
/// @return @p obj for fluent chaining.
void *rt_game3d_hitbox_bind_window(void *obj, rt_string state_name, double t0, double t1) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.bindWindow: invalid hitbox");
    if (!hitbox)
        return obj;
    if (hitbox->window_count < 0 || hitbox->window_count >= RT_GAME3D_HITBOX_MAX_WINDOWS) {
        rt_trap("Game3D.Hitbox3D.bindWindow: window limit reached (4)");
        return obj;
    }
    const char *name = state_name ? rt_string_cstr(state_name) : NULL;
    if (!name || !name[0]) {
        rt_trap("Game3D.Hitbox3D.bindWindow: state name must be non-empty");
        return obj;
    }
    rt_game3d_hitbox_window *window = &hitbox->windows[hitbox->window_count];
    memset(window, 0, sizeof(*window));
    (void)game3d_utf8_copy_bounded(window->state, RT_GAME3D_HITBOX_STATE_NAME_MAX, name);
    window->t0 = game3d_finite_or(t0, 0.0);
    window->t1 = game3d_finite_or(t1, 0.0);
    if (window->t1 < window->t0) {
        double tmp = window->t0;
        window->t0 = window->t1;
        window->t1 = tmp;
    }
    hitbox->window_count += 1;
    return obj;
}

/// @brief Fluent: set the shape offset in bone/entity space.
/// @param obj Hitbox3D to configure.
/// @param x Local X offset.
/// @param y Local Y offset.
/// @param z Local Z offset.
/// @return @p obj for fluent chaining.
void *rt_game3d_hitbox_set_local_offset(void *obj, double x, double y, double z) {
    rt_game3d_hitbox *hitbox =
        game3d_hitbox_checked(obj, "Game3D.Hitbox3D.setLocalOffset: invalid hitbox");
    if (hitbox) {
        hitbox->local_offset[0] = game3d_clamp_coord_or(x, 0.0);
        hitbox->local_offset[1] = game3d_clamp_coord_or(y, 0.0);
        hitbox->local_offset[2] = game3d_clamp_coord_or(z, 0.0);
    }
    return obj;
}

/// @brief Compute a hitbox's world pose (position + orientation).
/// @param hitbox Hitbox3D whose owner, bone pose, and local offset are composed.
/// @param out_pos Three-component output receiving world position.
/// @param out_quat Four-component output receiving XYZW world orientation.
/// @return 0 when the owner entity/node is unavailable.
static int game3d_hitbox_world_pose(rt_game3d_hitbox *hitbox,
                                    double out_pos[3],
                                    double out_quat[4]) {
    rt_game3d_entity *entity = hitbox->entity;
    if (!entity || !game3d_entity_alive_or_record(entity))
        return 0;
    void *node = game3d_entity_node_ref(entity);
    if (!node)
        return 0;
    double node_pos[3] = {0.0, 0.0, 0.0};
    double node_quat[4] = {0.0, 0.0, 0.0, 1.0};
    if (!rt_scene_node3d_get_world_position_components(
            node, &node_pos[0], &node_pos[1], &node_pos[2]))
        return 0;
    (void)rt_scene_node3d_get_world_rotation_components(
        node, &node_quat[0], &node_quat[1], &node_quat[2], &node_quat[3]);

    double pose_pos[3] = {node_pos[0], node_pos[1], node_pos[2]};
    double pose_quat[4] = {node_quat[0], node_quat[1], node_quat[2], node_quat[3]};
    if (hitbox->bone_index >= 0) {
        void *animator = game3d_entity_anim_ref(entity);
        void *controller = animator ? rt_game3d_animator_get_controller(animator) : NULL;
        double bone_pos[3];
        double bone_quat[4];
        if (controller && rt_anim_controller3d_get_bone_pose(
                              controller, hitbox->bone_index, bone_pos, bone_quat)) {
            double rotated[3];
            combat_quat_rotate(node_quat, bone_pos, rotated);
            pose_pos[0] = node_pos[0] + rotated[0];
            pose_pos[1] = node_pos[1] + rotated[1];
            pose_pos[2] = node_pos[2] + rotated[2];
            combat_quat_mul(node_quat, bone_quat, pose_quat);
        }
    }
    double offset_world[3];
    combat_quat_rotate(pose_quat, hitbox->local_offset, offset_world);
    out_pos[0] = pose_pos[0] + offset_world[0];
    out_pos[1] = pose_pos[1] + offset_world[1];
    out_pos[2] = pose_pos[2] + offset_world[2];
    memcpy(out_quat, pose_quat, sizeof(pose_quat));
    return 1;
}

/// @brief True when the hitbox is live this step (manual switch or any
///   animation window matching the owner's animator base state/time).
/// @details Window liveness tests the CROSSING of [prev_sample, now] against
///   [t0, t1], not instantaneous membership: a coarse fixed step or a high
///   animation-speed multiplier can advance state_time straight over a narrow
///   authored window, which would silently whiff the attack frame-rate-
///   dependently. Loop wrap (now < prev) checks both tail [prev, end] and head
///   [0, now] spans. A window whose state was not playing at the previous
///   sample uses instantaneous membership (the prev sample belonged to another
///   state, so the crossing interval would be meaningless). Called once per
///   hitbox per combat step — it advances the stored sample.
/// @param hitbox Hitbox3D whose manual and animator-window state is sampled.
/// @return 1 when the volume is live for this combat step, or 0 otherwise.
static int game3d_hitbox_is_live(rt_game3d_hitbox *hitbox) {
    int32_t window_count;
    if (!hitbox)
        return 0;
    if (hitbox->active)
        return 1;
    window_count = hitbox->window_count;
    if (window_count <= 0 || window_count > RT_GAME3D_HITBOX_MAX_WINDOWS)
        return 0;
    rt_game3d_entity *entity = hitbox->entity;
    void *animator = entity ? game3d_entity_anim_ref(entity) : NULL;
    void *controller = animator ? rt_game3d_animator_get_controller(animator) : NULL;
    if (!controller) {
        hitbox->window_prev_valid = 0;
        return 0;
    }
    double state_time = rt_anim_controller3d_get_state_time(controller);
    double prev_time = hitbox->window_prev_valid ? hitbox->window_prev_time : state_time;
    int live = 0;
    for (int32_t i = 0; i < window_count; ++i) {
        rt_game3d_hitbox_window *window = &hitbox->windows[i];
        int playing = rt_anim_controller3d_is_state_playing_cstr(controller, window->state) ? 1 : 0;
        if (playing && !live) {
            double lo = (hitbox->window_prev_valid && hitbox->window_prev_playing[i]) ? prev_time
                                                                                      : state_time;
            if (lo <= state_time) {
                if (window->t1 >= lo && window->t0 <= state_time)
                    live = 1;
            } else {
                /* Clip looped between samples: [lo, clip_end] ∪ [0, state_time]. */
                if (window->t1 >= lo || window->t0 <= state_time)
                    live = 1;
            }
        }
        hitbox->window_prev_playing[i] = (int8_t)playing;
    }
    hitbox->window_prev_time = state_time;
    hitbox->window_prev_valid = 1;
    return live;
}

/// @brief True when @p victim was already reported for the current activation.
/// @param hitbox Attacking Hitbox3D containing the bounded victim set.
/// @param victim Candidate victim entity identity.
/// @return 1 when already recorded, or 0 otherwise.
static int game3d_hitbox_victim_seen(const rt_game3d_hitbox *hitbox,
                                     const rt_game3d_entity *victim) {
    if (!hitbox || hitbox->hit_victim_count < 0 ||
        hitbox->hit_victim_count > RT_GAME3D_HITBOX_MAX_VICTIMS)
        return 0;
    for (int32_t i = 0; i < hitbox->hit_victim_count; ++i)
        if (hitbox->hit_victims[i] == victim)
            return 1;
    return 0;
}

/// @brief Record @p victim in the rehit-suppression ring (bounded).
/// @return 1 when recorded; 0 when the ring is full. Callers must fail closed
///   (suppress the hit) on 0 — an unrecorded victim would otherwise be re-hit
///   on every subsequent combat pass of the same activation.
/// @param hitbox Attacking Hitbox3D whose bounded victim set is updated.
/// @param victim Victim entity identity to remember.
static int game3d_hitbox_remember_victim(rt_game3d_hitbox *hitbox, rt_game3d_entity *victim) {
    if (!hitbox || !victim || hitbox->hit_victim_count < 0 ||
        hitbox->hit_victim_count >= RT_GAME3D_HITBOX_MAX_VICTIMS)
        return 0;
    hitbox->hit_victims[hitbox->hit_victim_count++] = victim;
    return 1;
}

//=========================================================================
// Entity slot management (called from rt_game3d_entity.c teardown)
//=========================================================================

/// @brief Release an entity's hitbox array and health slot, clearing component
///   backrefs first so surviving handles fail closed. See internal header.
/// @param entity Entity3D whose combat component ownership is dismantled.
void game3d_entity_release_combat_slots(rt_game3d_entity *entity) {
    int32_t hitbox_count;
    if (!entity)
        return;
    hitbox_count =
        game3d_combat_array_valid(entity->hitbox_count, entity->hitbox_capacity, entity->hitboxes)
            ? entity->hitbox_count
            : 0;
    for (int32_t i = 0; i < hitbox_count; ++i) {
        rt_game3d_hitbox *hitbox = (rt_game3d_hitbox *)rt_g3d_checked_or_null(
            entity->hitboxes[i], RT_G3D_GAME3D_HITBOX_CLASS_ID);
        if (hitbox)
            hitbox->entity = NULL;
        game3d_release_typed_ref(&entity->hitboxes[i], RT_G3D_GAME3D_HITBOX_CLASS_ID);
    }
    free(entity->hitboxes);
    entity->hitboxes = NULL;
    entity->hitbox_count = 0;
    entity->hitbox_capacity = 0;
    rt_game3d_health *health =
        (rt_game3d_health *)rt_g3d_checked_or_null(entity->health, RT_G3D_GAME3D_HEALTH_CLASS_ID);
    if (health)
        health->entity = NULL;
    game3d_release_typed_ref(&entity->health, RT_G3D_GAME3D_HEALTH_CLASS_ID);
}

//=========================================================================
// Health3D
//=========================================================================

/// @brief Create a health component with @p max_hp (clamped positive).
/// @param max_hp Requested positive maximum hit points.
/// @return A new full-health Health3D component, or NULL on allocation failure.
void *rt_game3d_health_new(double max_hp) {
    rt_game3d_health *health =
        (rt_game3d_health *)rt_obj_new_i64(RT_G3D_GAME3D_HEALTH_CLASS_ID, (int64_t)sizeof(*health));
    if (!health) {
        rt_trap("Game3D.Health3D.New: allocation failed");
        return NULL;
    }
    memset(health, 0, sizeof(*health));
    health->max_hp = game3d_positive_clamped_or(max_hp, 100.0, 1e12);
    health->hp = health->max_hp;
    return health;
}

/// @brief Fluent: attach a Health3D component (one per entity; reattach replaces).
/// @param obj Entity3D that will retain the component.
/// @param health_obj Health3D to attach.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_attach_health(void *obj, void *health_obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.attachHealth: invalid entity");
    rt_game3d_health *health =
        game3d_health_checked(health_obj, "Game3D.Entity3D.attachHealth: value must be Health3D");
    if (!entity || !health)
        return obj;
    rt_game3d_health *previous =
        (rt_game3d_health *)rt_g3d_checked_or_null(entity->health, RT_G3D_GAME3D_HEALTH_CLASS_ID);
    if (previous && previous != health)
        previous->entity = NULL;
    game3d_assign_typed_ref(&entity->health, health, RT_G3D_GAME3D_HEALTH_CLASS_ID);
    health->entity = entity;
    return obj;
}

/// @brief Get the entity's Health3D component (NULL when none/stale entity).
/// @param obj Entity3D to query.
/// @return The borrowed validated Health3D handle, or NULL.
void *rt_game3d_entity_get_health(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_health: invalid entity");
    if (!entity)
        return NULL;
    return rt_g3d_checked_or_null(entity->health, RT_G3D_GAME3D_HEALTH_CLASS_ID);
}

/// @brief Get current hit points.
/// @param obj Health3D component to query.
/// @return Current hit points, or 0 when invalid.
double rt_game3d_health_get_current(void *obj) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.get_current: invalid health");
    return health ? health->hp : 0.0;
}

/// @brief Get maximum hit points.
/// @param obj Health3D component to query.
/// @return Maximum hit points, or 0 when invalid.
double rt_game3d_health_get_max(void *obj) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.get_max: invalid health");
    return health ? health->max_hp : 0.0;
}

/// @brief Set maximum hit points (clamps current downward when reduced).
/// @param obj Health3D component to configure.
/// @param max_hp Requested positive maximum hit points.
void rt_game3d_health_set_max(void *obj, double max_hp) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.set_max: invalid health");
    if (!health)
        return;
    health->max_hp = game3d_positive_clamped_or(max_hp, health->max_hp, 1e12);
    if (health->hp > health->max_hp)
        health->hp = health->max_hp;
}

/// @brief True once hp reached 0 (until Revive).
/// @param obj Health3D component to query.
/// @return 1 when death is latched, or 0 otherwise.
int8_t rt_game3d_health_is_dead(void *obj) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.get_isDead: invalid health");
    return health ? health->dead : 0;
}

/// @brief Get the i-frame duration granted per applied damage.
/// @param obj Health3D component to query.
/// @return Configured invulnerability duration in seconds, or 0 when invalid.
double rt_game3d_health_get_invuln_seconds(void *obj) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.get_invulnSeconds: invalid health");
    return health ? health->invuln_seconds : 0.0;
}

/// @brief Set the i-frame duration granted per applied damage.
/// @param obj Health3D component to configure.
/// @param seconds Non-negative duration in seconds, clamped to one hour.
void rt_game3d_health_set_invuln_seconds(void *obj, double seconds) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.set_invulnSeconds: invalid health");
    if (health)
        health->invuln_seconds = game3d_nonnegative_clamped_or(seconds, 0.0, 3600.0);
}

/// @brief True while i-frames are active.
/// @param obj Health3D component to query.
/// @return 1 while invulnerability time remains, or 0 otherwise.
int8_t rt_game3d_health_get_invulnerable(void *obj) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.get_invulnerable: invalid health");
    return health && health->invuln_remaining > 0.0 ? 1 : 0;
}

/// @brief Apply damage: returns the applied amount (0 while invulnerable/dead).
///   Grants i-frames, sets one-shot flags, emits a DamageEvent3D, and latches
///   death (with a lethal event) when hp crosses 0. See header.
/// @param obj Health3D component receiving damage.
/// @param amount Positive requested damage amount.
/// @param source_entity Optional Entity3D responsible for the damage.
/// @param tag Application-defined damage tag copied into telemetry.
/// @return Damage actually removed after clamping to remaining health, or 0 when rejected.
double rt_game3d_health_damage(void *obj, double amount, void *source_entity, int64_t tag) {
    rt_game3d_health *health = game3d_health_checked(obj, "Game3D.Health3D.Damage: invalid health");
    if (!health)
        return 0.0;
    if (source_entity && !rt_g3d_has_class(source_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID)) {
        rt_trap("Game3D.Health3D.Damage: source must be Entity3D or null");
        return 0.0;
    }
    amount = game3d_finite_or(amount, 0.0);
    if (amount <= 0.0 || health->dead || health->invuln_remaining > 0.0)
        return 0.0;
    double applied = amount > health->hp ? health->hp : amount;
    health->hp -= applied;
    health->invuln_remaining = health->invuln_seconds;
    health->just_damaged = 1;
    health->last_damage = applied;
    health->last_tag = tag;
    int8_t lethal = 0;
    if (health->hp <= 0.0) {
        health->hp = 0.0;
        health->dead = 1;
        health->just_died = 1;
        lethal = 1;
    }
    rt_game3d_entity *victim = health->entity;
    rt_game3d_world *world =
        victim
            ? (rt_game3d_world *)rt_g3d_checked_or_null(victim->world, RT_G3D_GAME3D_WORLD_CLASS_ID)
            : NULL;
    if (world)
        game3d_world_push_damage_event(
            world, victim, (rt_game3d_entity *)source_entity, applied, tag, lethal);
    return applied;
}

/// @brief Heal by @p amount (no effect while dead; clamps to max).
/// @param obj Health3D component to heal.
/// @param amount Positive hit points to restore.
void rt_game3d_health_heal(void *obj, double amount) {
    rt_game3d_health *health = game3d_health_checked(obj, "Game3D.Health3D.Heal: invalid health");
    if (!health || health->dead)
        return;
    amount = game3d_finite_or(amount, 0.0);
    if (amount <= 0.0)
        return;
    health->hp += amount;
    if (health->hp > health->max_hp)
        health->hp = health->max_hp;
}

/// @brief Clear the death latch and restore @p hp (clamped to 1..max).
/// @param obj Health3D component to revive.
/// @param hp Requested restored hit points.
void rt_game3d_health_revive(void *obj, double hp) {
    rt_game3d_health *health = game3d_health_checked(obj, "Game3D.Health3D.Revive: invalid health");
    if (!health)
        return;
    health->dead = 0;
    health->invuln_remaining = 0.0;
    hp = game3d_finite_or(hp, health->max_hp);
    health->hp = game3d_clamp(hp, 1.0, health->max_hp);
}

/// @brief One-shot: true for the step after hp crossed to 0.
/// @param obj Health3D component to query.
/// @return 1 when death occurred in the current event interval, or 0 otherwise.
int8_t rt_game3d_health_just_died(void *obj) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.JustDied: invalid health");
    return health ? health->just_died : 0;
}

/// @brief One-shot: true for the step after damage applied.
/// @param obj Health3D component to query.
/// @return 1 when damage was applied in the current event interval, or 0 otherwise.
int8_t rt_game3d_health_just_damaged(void *obj) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.JustDamaged: invalid health");
    return health ? health->just_damaged : 0;
}

/// @brief Most recent applied damage amount.
/// @param obj Health3D component to query.
/// @return The last applied amount, or 0 when unavailable.
double rt_game3d_health_last_damage(void *obj) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.LastDamage: invalid health");
    return health ? health->last_damage : 0.0;
}

/// @brief Most recent caller-supplied damage tag.
/// @param obj Health3D component to query.
/// @return The last damage tag, or 0 when unavailable.
int64_t rt_game3d_health_last_tag(void *obj) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.LastTag: invalid health");
    return health ? health->last_tag : 0;
}

/// @brief Impulse knockback on the owner's dynamic body: returns false (no-op)
///   for kinematic/static/absent bodies so gameplay can route to its character
///   controller instead. Not automatic on damage.
/// @param obj Health3D whose owning entity supplies the body.
/// @param direction Vec3 impulse direction; normalized internally.
/// @param strength Positive impulse magnitude.
/// @param point Vec3 world-space application point.
/// @return 1 when the impulse was applied, or 0 when validation or body state rejects it.
int8_t rt_game3d_health_apply_knockback(void *obj, void *direction, double strength, void *point) {
    rt_game3d_health *health =
        game3d_health_checked(obj, "Game3D.Health3D.ApplyKnockback: invalid health");
    if (!rt_g3d_is_vec3(direction) || !rt_g3d_is_vec3(point)) {
        rt_trap("Game3D.Health3D.ApplyKnockback: direction and point must be Vec3");
        return 0;
    }
    rt_game3d_entity *entity = health ? health->entity : NULL;
    if (!entity || !game3d_entity_alive_or_record(entity))
        return 0;
    void *body = game3d_entity_body_ref(entity);
    if (!body || rt_body3d_is_kinematic(body) || rt_body3d_get_mass(body) <= 0.0)
        return 0;
    strength = game3d_finite_or(strength, 0.0);
    if (strength <= 0.0)
        return 0;
    double dx = game3d_finite_or(rt_vec3_x(direction), 0.0);
    double dy = game3d_finite_or(rt_vec3_y(direction), 0.0);
    double dz = game3d_finite_or(rt_vec3_z(direction), 0.0);
    double len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 1e-9)
        return 0;
    rt_body3d_apply_impulse_at_point(body,
                                     dx / len * strength,
                                     dy / len * strength,
                                     dz / len * strength,
                                     game3d_finite_or(rt_vec3_x(point), 0.0),
                                     game3d_finite_or(rt_vec3_y(point), 0.0),
                                     game3d_finite_or(rt_vec3_z(point), 0.0));
    rt_body3d_wake(body);
    return 1;
}

//=========================================================================
// World event buffers
//=========================================================================

/// @brief Release every record in both combat buffers. See internal header.
/// @param world World3D whose retained hit and damage records are cleared.
void game3d_world_clear_combat_events(rt_game3d_world *world) {
    int32_t hit_count;
    int32_t damage_count;
    if (!world)
        return;
    hit_count = game3d_combat_array_valid(
                    world->hit_event_count, world->hit_event_capacity, world->hit_events)
                    ? world->hit_event_count
                    : 0;
    for (int32_t i = 0; i < hit_count; ++i) {
        rt_game3d_hit_event_rec *rec = &world->hit_events[i];
        game3d_release_ref(&rec->attacker);
        game3d_release_ref(&rec->victim);
        game3d_release_ref(&rec->hitbox);
        game3d_release_ref(&rec->hurtbox);
    }
    world->hit_event_count = 0;
    damage_count = game3d_combat_array_valid(world->damage_event_count,
                                             world->damage_event_capacity,
                                             world->damage_events)
                       ? world->damage_event_count
                       : 0;
    for (int32_t i = 0; i < damage_count; ++i) {
        rt_game3d_damage_event_rec *rec = &world->damage_events[i];
        game3d_release_ref(&rec->victim);
        game3d_release_ref(&rec->source);
    }
    world->damage_event_count = 0;
}

/// @brief Append a hit event record (retains all handles).
/// @param world World3D whose hit-event buffer is extended.
/// @param attacker Entity3D owning the attacking volume.
/// @param victim Entity3D owning the hurt volume.
/// @param hitbox Attacking Hitbox3D.
/// @param hurtbox Victim Hitbox3D.
/// @param point Three-component overlap witness point.
/// @param normal Three-component contact normal.
static void game3d_world_push_hit_event(rt_game3d_world *world,
                                        rt_game3d_entity *attacker,
                                        rt_game3d_entity *victim,
                                        rt_game3d_hitbox *hitbox,
                                        rt_game3d_hitbox *hurtbox,
                                        const double point[3],
                                        const double normal[3]) {
    int32_t new_cap;
    if (!world || !point || !normal ||
        !game3d_combat_array_valid(
            world->hit_event_count, world->hit_event_capacity, world->hit_events))
        return;
    if (world->hit_event_count >= world->hit_event_capacity) {
        if (world->hit_event_count == INT32_MAX ||
            !game3d_checked_capacity_i32(world->hit_event_capacity,
                                         world->hit_event_count + 1,
                                         16,
                                         sizeof(*world->hit_events),
                                         &new_cap))
            return;
        rt_game3d_hit_event_rec *grown =
            (rt_game3d_hit_event_rec *)realloc(world->hit_events, (size_t)new_cap * sizeof(*grown));
        if (!grown)
            return;
        world->hit_events = grown;
        world->hit_event_capacity = new_cap;
    }
    rt_game3d_hit_event_rec *rec = &world->hit_events[world->hit_event_count++];
    memset(rec, 0, sizeof(*rec));
    game3d_assign_ref(&rec->attacker, attacker);
    game3d_assign_ref(&rec->victim, victim);
    game3d_assign_ref(&rec->hitbox, hitbox);
    game3d_assign_ref(&rec->hurtbox, hurtbox);
    memcpy(rec->point, point, sizeof(rec->point));
    memcpy(rec->normal, normal, sizeof(rec->normal));
}

/// @brief Append a damage event record (retains handles). See internal header.
/// @param world World3D whose damage-event buffer is extended.
/// @param victim Entity3D receiving damage.
/// @param source Optional source Entity3D.
/// @param amount Applied damage amount.
/// @param tag Application-defined damage tag.
/// @param was_lethal Non-zero when this damage latched death.
void game3d_world_push_damage_event(rt_game3d_world *world,
                                    rt_game3d_entity *victim,
                                    rt_game3d_entity *source,
                                    double amount,
                                    int64_t tag,
                                    int8_t was_lethal) {
    int32_t new_cap;
    if (!world || !game3d_combat_array_valid(world->damage_event_count,
                                             world->damage_event_capacity,
                                             world->damage_events))
        return;
    if (world->damage_event_count >= world->damage_event_capacity) {
        if (world->damage_event_count == INT32_MAX ||
            !game3d_checked_capacity_i32(world->damage_event_capacity,
                                         world->damage_event_count + 1,
                                         16,
                                         sizeof(*world->damage_events),
                                         &new_cap))
            return;
        rt_game3d_damage_event_rec *grown = (rt_game3d_damage_event_rec *)realloc(
            world->damage_events, (size_t)new_cap * sizeof(*grown));
        if (!grown)
            return;
        world->damage_events = grown;
        world->damage_event_capacity = new_cap;
    }
    rt_game3d_damage_event_rec *rec = &world->damage_events[world->damage_event_count++];
    memset(rec, 0, sizeof(*rec));
    game3d_assign_ref(&rec->victim, victim);
    game3d_assign_ref(&rec->source, source);
    rec->amount = amount;
    rec->tag = tag;
    rec->was_lethal = was_lethal;
}

//=========================================================================
// Combat pass
//=========================================================================

/// @brief One posed live volume collected for the pairwise pass.
typedef struct {
    rt_game3d_hitbox *hitbox;
    rt_game3d_entity *entity;
    double pos[3];
    double quat[4];
    double aabb_min[3];
    double aabb_max[3];
} game3d_combat_volume;

/// @brief One stable X-axis broadphase entry for a posed hurt volume.
typedef struct {
    double min_x;
    int32_t original_index;
} game3d_combat_hurt_x_entry;

/// @brief Per-world combat scratch: the posed hit/hurt volume lists for one pass.
/// @details Owned by the world (lazily allocated, freed with the world's other
///   registries) instead of TU-static storage, so concurrent worlds and any
///   re-entrant combat pass each work on their own buffers.
typedef struct {
    game3d_combat_volume hit_volumes[GAME3D_COMBAT_MAX_HIT_VOLUMES];
    game3d_combat_volume hurt_volumes[GAME3D_COMBAT_MAX_HURT_VOLUMES];
    game3d_combat_hurt_x_entry hurt_x_order[GAME3D_COMBAT_MAX_HURT_VOLUMES];
    uint64_t hurt_candidate_bits[(GAME3D_COMBAT_MAX_HURT_VOLUMES + 63) / 64];
} game3d_combat_scratch;

/// @brief Return the world's combat scratch, allocating it on first use.
/// @param world World3D that owns the scratch allocation.
/// @return The per-world scratch buffers, or NULL when absent or allocation fails.
static game3d_combat_scratch *game3d_combat_scratch_for(rt_game3d_world *world) {
    if (!world)
        return NULL;
    if (!world->combat_scratch)
        world->combat_scratch = malloc(sizeof(game3d_combat_scratch));
    return (game3d_combat_scratch *)world->combat_scratch;
}

/// @brief Collect one entity's live volumes into the hit/hurt lists.
/// @param entity Live spawned Entity3D whose combat slots are scanned.
/// @param hits Fixed-capacity output array for live attack volumes.
/// @param hit_count Input/output count of collected attack volumes.
/// @param hurts Fixed-capacity output array for hurt volumes.
/// @param hurt_count Input/output count of collected hurt volumes.
static void game3d_combat_collect_entity(rt_game3d_entity *entity,
                                         game3d_combat_volume *hits,
                                         int32_t *hit_count,
                                         game3d_combat_volume *hurts,
                                         int32_t *hurt_count) {
    static const double unit_scale[3] = {1.0, 1.0, 1.0};
    if (!entity ||
        !game3d_combat_array_valid(entity->hitbox_count, entity->hitbox_capacity, entity->hitboxes))
        return;
    for (int32_t i = 0; i < entity->hitbox_count; ++i) {
        rt_game3d_hitbox *hitbox = (rt_game3d_hitbox *)rt_g3d_checked_or_null(
            entity->hitboxes[i], RT_G3D_GAME3D_HITBOX_CLASS_ID);
        if (!hitbox || !hitbox->collider)
            continue;
        if (hitbox->kind == RT_GAME3D_HITBOX_KIND_HIT) {
            int live = game3d_hitbox_is_live(hitbox);
            if (!live) {
                if (hitbox->was_live)
                    hitbox->hit_victim_count = 0; /* activation ended: new swing */
                hitbox->was_live = 0;
                continue;
            }
            hitbox->was_live = 1;
            if (*hit_count >= GAME3D_COMBAT_MAX_HIT_VOLUMES)
                continue;
            game3d_combat_volume *volume = &hits[*hit_count];
            if (!game3d_hitbox_world_pose(hitbox, volume->pos, volume->quat))
                continue;
            volume->hitbox = hitbox;
            volume->entity = entity;
            rt_collider3d_compute_world_aabb_raw(hitbox->collider,
                                                 volume->pos,
                                                 volume->quat,
                                                 unit_scale,
                                                 volume->aabb_min,
                                                 volume->aabb_max);
            *hit_count += 1;
        } else {
            if (*hurt_count >= GAME3D_COMBAT_MAX_HURT_VOLUMES)
                continue;
            game3d_combat_volume *volume = &hurts[*hurt_count];
            if (!game3d_hitbox_world_pose(hitbox, volume->pos, volume->quat))
                continue;
            volume->hitbox = hitbox;
            volume->entity = entity;
            rt_collider3d_compute_world_aabb_raw(hitbox->collider,
                                                 volume->pos,
                                                 volume->quat,
                                                 unit_scale,
                                                 volume->aabb_min,
                                                 volume->aabb_max);
            *hurt_count += 1;
        }
    }
}

/// @brief AABB overlap reject for the pairwise pass.
/// @param a First posed combat volume.
/// @param b Second posed combat volume.
/// @return 1 when all three axis intervals overlap, or 0 otherwise.
static int game3d_combat_aabb_overlap(const game3d_combat_volume *a,
                                      const game3d_combat_volume *b) {
    return a->aabb_min[0] <= b->aabb_max[0] && a->aabb_max[0] >= b->aabb_min[0] &&
           a->aabb_min[1] <= b->aabb_max[1] && a->aabb_max[1] >= b->aabb_min[1] &&
           a->aabb_min[2] <= b->aabb_max[2] && a->aabb_max[2] >= b->aabb_min[2];
}

/// @brief Order hurt-volume broadphase entries by minimum X, retaining original-order ties.
static int game3d_combat_hurt_x_compare(const void *lhs, const void *rhs) {
    const game3d_combat_hurt_x_entry *a = (const game3d_combat_hurt_x_entry *)lhs;
    const game3d_combat_hurt_x_entry *b = (const game3d_combat_hurt_x_entry *)rhs;
    if (a->min_x < b->min_x)
        return -1;
    if (a->min_x > b->min_x)
        return 1;
    return (a->original_index > b->original_index) - (a->original_index < b->original_index);
}

/// @brief Find the first set bit in a non-zero 64-bit word without compiler intrinsics.
static int32_t game3d_combat_first_set_bit(uint64_t bits) {
    int32_t bit = 0;
    while ((bits & UINT64_C(1)) == 0) {
        bits >>= 1;
        bit++;
    }
    return bit;
}

/// @brief Per-step combat pass. See internal header.
/// @details Clears prior events and one-shot flags, advances invulnerability, collects posed
///   volumes, performs broad- and narrow-phase overlap tests, and records one hit per activation.
/// @param world World3D whose live entity registry is evaluated.
/// @param dt Simulation interval in seconds, clamped to the shared step limit.
void game3d_world_update_combat(rt_game3d_world *world, double dt) {
    if (!world)
        return;
    dt = game3d_clamp_dt(dt);

    /* 1. New step: clear event buffers and one-shot flags, tick i-frames. */
    game3d_world_clear_combat_events(world);
    int32_t entity_count = world->entity_count;
    if (!world->entities || entity_count < 0)
        entity_count = 0;
    if (entity_count > world->entity_capacity)
        entity_count = world->entity_capacity > 0 ? world->entity_capacity : 0;
    int has_combat = 0;
    for (int32_t e = 0; e < entity_count; ++e) {
        rt_game3d_entity *entity = world->entities[e];
        if (!entity || !entity->alive || !entity->spawned)
            continue;
        rt_game3d_health *health = (rt_game3d_health *)rt_g3d_checked_or_null(
            entity->health, RT_G3D_GAME3D_HEALTH_CLASS_ID);
        if (health) {
            health->just_damaged = 0;
            health->just_died = 0;
            if (health->invuln_remaining > 0.0) {
                health->invuln_remaining -= dt;
                if (health->invuln_remaining < 0.0)
                    health->invuln_remaining = 0.0;
            }
        }
        if (entity->hitbox_count > 0)
            has_combat = 1;
    }
    if (!has_combat)
        return;

    /* 2. Collect live hit volumes and all hurt volumes, posed in world space. */
    game3d_combat_scratch *scratch = game3d_combat_scratch_for(world);
    if (!scratch)
        return;
    game3d_combat_volume *hit_volumes = scratch->hit_volumes;
    game3d_combat_volume *hurt_volumes = scratch->hurt_volumes;
    int32_t hit_count = 0;
    int32_t hurt_count = 0;
    for (int32_t e = 0; e < entity_count; ++e) {
        rt_game3d_entity *entity = world->entities[e];
        if (!entity || !entity->alive || !entity->spawned || entity->hitbox_count <= 0)
            continue;
        game3d_combat_collect_entity(entity, hit_volumes, &hit_count, hurt_volumes, &hurt_count);
    }
    if (hit_count <= 0 || hurt_count <= 0)
        return;

    /* 3. Sort a compact X-axis broadphase once. Candidate bits restore the original hurt-volume
     * order before narrow phase, so event ordering remains stable across spatial arrangements. */
    for (int32_t v = 0; v < hurt_count; ++v) {
        scratch->hurt_x_order[v].min_x = hurt_volumes[v].aabb_min[0];
        scratch->hurt_x_order[v].original_index = v;
    }
    qsort(scratch->hurt_x_order,
          (size_t)hurt_count,
          sizeof(scratch->hurt_x_order[0]),
          game3d_combat_hurt_x_compare);

    for (int32_t h = 0; h < hit_count; ++h) {
        game3d_combat_volume *attack = &hit_volumes[h];
        memset(scratch->hurt_candidate_bits, 0, sizeof(scratch->hurt_candidate_bits));
        for (int32_t sorted = 0; sorted < hurt_count; ++sorted) {
            int32_t v = scratch->hurt_x_order[sorted].original_index;
            if (scratch->hurt_x_order[sorted].min_x > attack->aabb_max[0])
                break;
            if (hurt_volumes[v].aabb_max[0] >= attack->aabb_min[0])
                scratch->hurt_candidate_bits[(uint32_t)v >> 6] |= UINT64_C(1)
                                                                  << ((uint32_t)v & 63u);
        }
        int32_t word_count = (hurt_count + 63) / 64;
        for (int32_t word_index = 0; word_index < word_count; ++word_index) {
            uint64_t candidates = scratch->hurt_candidate_bits[word_index];
            while (candidates) {
                int32_t v = word_index * 64 + game3d_combat_first_set_bit(candidates);
                candidates &= candidates - UINT64_C(1);
                if (v >= hurt_count)
                    break;
                game3d_combat_volume *defend = &hurt_volumes[v];
                if (attack->entity == defend->entity)
                    continue;
                if (attack->hitbox->team == defend->hitbox->team && !attack->hitbox->friendly_fire)
                    continue;
                if ((attack->hitbox->channel & defend->hitbox->channel) == 0)
                    continue;
                if (game3d_hitbox_victim_seen(attack->hitbox, defend->entity))
                    continue;
                if (!game3d_combat_aabb_overlap(attack, defend))
                    continue;
                double normal[3];
                double depth = 0.0;
                double point[3];
                if (!rt_collider3d_overlap_at_raw(attack->hitbox->collider,
                                                  attack->pos,
                                                  attack->quat,
                                                  defend->hitbox->collider,
                                                  defend->pos,
                                                  defend->quat,
                                                  normal,
                                                  &depth,
                                                  point))
                    continue;
                double safe_normal[3] = {0.0, 1.0, 0.0};
                double normal_len =
                    sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                if (isfinite(normal_len) && normal_len > 1e-9) {
                    safe_normal[0] = normal[0] / normal_len;
                    safe_normal[1] = normal[1] / normal_len;
                    safe_normal[2] = normal[2] / normal_len;
                }
                double safe_point[3] = {game3d_clamp_coord_or(point[0], attack->pos[0]),
                                        game3d_clamp_coord_or(point[1], attack->pos[1]),
                                        game3d_clamp_coord_or(point[2], attack->pos[2])};
                /* Fail closed when the suppression ring is full: dropping the hit
                 * preserves the one-hit-per-activation-per-victim invariant. */
                if (!game3d_hitbox_remember_victim(attack->hitbox, defend->entity))
                    continue;
                game3d_world_push_hit_event(world,
                                            attack->entity,
                                            defend->entity,
                                            attack->hitbox,
                                            defend->hitbox,
                                            safe_point,
                                            safe_normal);
            }
        }
    }
}

//=========================================================================
// World polling API + boxed event wrappers
//=========================================================================

/// @brief Number of hit events buffered by the most recent step.
/// @param obj World3D to query.
/// @return The buffered hit-event count, or 0 when invalid.
int64_t rt_game3d_world_hit_event_count(void *obj) {
    rt_game3d_world *world =
        game3d_world_checked(obj, "Game3D.World3D.hitEventCount: invalid world");
    return world && game3d_combat_array_valid(
                        world->hit_event_count, world->hit_event_capacity, world->hit_events)
               ? world->hit_event_count
               : 0;
}

/// @brief Validate @p obj as a HitEvent3D handle, trapping @p method on mismatch.
/// @param obj Candidate HitEvent3D handle.
/// @param method Trap message emitted on runtime-class mismatch.
/// @return The validated event, or NULL after reporting an invalid handle.
static rt_game3d_hit_event *game3d_hit_event_checked(void *obj, const char *method) {
    rt_game3d_hit_event *event =
        (rt_game3d_hit_event *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_HITEVENT_CLASS_ID);
    if (!event)
        rt_trap(method);
    return event;
}

/// @brief Validate @p obj as a DamageEvent3D handle, trapping @p method on mismatch.
/// @param obj Candidate DamageEvent3D handle.
/// @param method Trap message emitted on runtime-class mismatch.
/// @return The validated event, or NULL after reporting an invalid handle.
static rt_game3d_damage_event *game3d_damage_event_checked(void *obj, const char *method) {
    rt_game3d_damage_event *event =
        (rt_game3d_damage_event *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_DAMAGEEVENT_CLASS_ID);
    if (!event)
        rt_trap(method);
    return event;
}

/// @brief GC finalizer for a boxed HitEvent3D.
/// @param obj HitEvent3D allocation whose retained participants are released.
static void game3d_hit_event_finalize(void *obj) {
    rt_game3d_hit_event *event = (rt_game3d_hit_event *)obj;
    if (!event)
        return;
    game3d_release_ref(&event->attacker);
    game3d_release_ref(&event->victim);
    game3d_release_ref(&event->hitbox);
    game3d_release_ref(&event->hurtbox);
}

/// @brief GC finalizer for a boxed DamageEvent3D.
/// @param obj DamageEvent3D allocation whose retained participants are released.
static void game3d_damage_event_finalize(void *obj) {
    rt_game3d_damage_event *event = (rt_game3d_damage_event *)obj;
    if (!event)
        return;
    game3d_release_ref(&event->victim);
    game3d_release_ref(&event->source);
}

/// @brief Get the @p index-th buffered hit event as a boxed HitEvent3D (NULL
///   when out of range).
/// @param obj World3D containing the hit-event buffer.
/// @param index Zero-based buffered event index.
/// @return A new boxed HitEvent3D snapshot, or NULL when out of range or allocation fails.
void *rt_game3d_world_hit_event(void *obj, int64_t index) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.World3D.hitEvent: invalid world");
    if (!world ||
        !game3d_combat_array_valid(
            world->hit_event_count, world->hit_event_capacity, world->hit_events) ||
        index < 0 || index >= world->hit_event_count)
        return NULL;
    rt_game3d_hit_event_rec *rec = &world->hit_events[index];
    rt_game3d_hit_event *event = (rt_game3d_hit_event *)rt_obj_new_i64(
        RT_G3D_GAME3D_HITEVENT_CLASS_ID, (int64_t)sizeof(*event));
    if (!event) {
        rt_trap("Game3D.World3D.hitEvent: allocation failed");
        return NULL;
    }
    memset(event, 0, sizeof(*event));
    rt_obj_set_finalizer(event, game3d_hit_event_finalize);
    game3d_assign_ref(&event->attacker, rec->attacker);
    game3d_assign_ref(&event->victim, rec->victim);
    game3d_assign_ref(&event->hitbox, rec->hitbox);
    game3d_assign_ref(&event->hurtbox, rec->hurtbox);
    memcpy(event->point, rec->point, sizeof(event->point));
    memcpy(event->normal, rec->normal, sizeof(event->normal));
    return event;
}

/// @brief Clear the buffered hit and damage events without stepping.
/// @param obj World3D whose combat-event buffers are released.
void rt_game3d_world_clear_hit_events(void *obj) {
    rt_game3d_world *world =
        game3d_world_checked(obj, "Game3D.World3D.clearHitEvents: invalid world");
    if (world)
        game3d_world_clear_combat_events(world);
}

/// @brief Number of damage events buffered since the last step.
/// @param obj World3D to query.
/// @return The buffered damage-event count, or 0 when invalid.
int64_t rt_game3d_world_damage_event_count(void *obj) {
    rt_game3d_world *world =
        game3d_world_checked(obj, "Game3D.World3D.damageEventCount: invalid world");
    return world && game3d_combat_array_valid(world->damage_event_count,
                                              world->damage_event_capacity,
                                              world->damage_events)
               ? world->damage_event_count
               : 0;
}

/// @brief Get the @p index-th buffered damage event as a boxed DamageEvent3D.
/// @param obj World3D containing the damage-event buffer.
/// @param index Zero-based buffered event index.
/// @return A new boxed DamageEvent3D snapshot, or NULL when out of range or allocation fails.
void *rt_game3d_world_damage_event(void *obj, int64_t index) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.World3D.damageEvent: invalid world");
    if (!world ||
        !game3d_combat_array_valid(
            world->damage_event_count, world->damage_event_capacity, world->damage_events) ||
        index < 0 || index >= world->damage_event_count)
        return NULL;
    rt_game3d_damage_event_rec *rec = &world->damage_events[index];
    rt_game3d_damage_event *event = (rt_game3d_damage_event *)rt_obj_new_i64(
        RT_G3D_GAME3D_DAMAGEEVENT_CLASS_ID, (int64_t)sizeof(*event));
    if (!event) {
        rt_trap("Game3D.World3D.damageEvent: allocation failed");
        return NULL;
    }
    memset(event, 0, sizeof(*event));
    rt_obj_set_finalizer(event, game3d_damage_event_finalize);
    game3d_assign_ref(&event->victim, rec->victim);
    game3d_assign_ref(&event->source, rec->source);
    event->amount = rec->amount;
    event->tag = rec->tag;
    event->was_lethal = rec->was_lethal;
    return event;
}

/// @brief HitEvent3D.Attacker — attacking entity (NULL when stale).
/// @param obj HitEvent3D to query.
/// @return The borrowed live attacking Entity3D, or NULL when stale.
void *rt_game3d_hit_event_get_attacker(void *obj) {
    rt_game3d_hit_event *event =
        game3d_hit_event_checked(obj, "Game3D.HitEvent3D.get_attacker: invalid event");
    if (!event)
        return NULL;
    rt_game3d_entity *entity =
        (rt_game3d_entity *)rt_g3d_checked_or_null(event->attacker, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    return game3d_entity_alive_or_record(entity) ? entity : NULL;
}

/// @brief HitEvent3D.Victim — victim entity (NULL when stale).
/// @param obj HitEvent3D to query.
/// @return The borrowed live victim Entity3D, or NULL when stale.
void *rt_game3d_hit_event_get_victim(void *obj) {
    rt_game3d_hit_event *event =
        game3d_hit_event_checked(obj, "Game3D.HitEvent3D.get_victim: invalid event");
    if (!event)
        return NULL;
    rt_game3d_entity *entity =
        (rt_game3d_entity *)rt_g3d_checked_or_null(event->victim, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    return game3d_entity_alive_or_record(entity) ? entity : NULL;
}

/// @brief HitEvent3D.Hitbox — the attacking volume.
/// @param obj HitEvent3D to query.
/// @return The borrowed validated attacking Hitbox3D, or NULL.
void *rt_game3d_hit_event_get_hitbox(void *obj) {
    rt_game3d_hit_event *event =
        game3d_hit_event_checked(obj, "Game3D.HitEvent3D.get_hitbox: invalid event");
    return event ? rt_g3d_checked_or_null(event->hitbox, RT_G3D_GAME3D_HITBOX_CLASS_ID) : NULL;
}

/// @brief HitEvent3D.Hurtbox — the victim volume.
/// @param obj HitEvent3D to query.
/// @return The borrowed validated victim Hitbox3D, or NULL.
void *rt_game3d_hit_event_get_hurtbox(void *obj) {
    rt_game3d_hit_event *event =
        game3d_hit_event_checked(obj, "Game3D.HitEvent3D.get_hurtbox: invalid event");
    return event ? rt_g3d_checked_or_null(event->hurtbox, RT_G3D_GAME3D_HITBOX_CLASS_ID) : NULL;
}

/// @brief HitEvent3D.Point — deepest-overlap witness point (fresh Vec3).
/// @param obj HitEvent3D to query.
/// @return A new Vec3 containing the witness point, or the zero vector when invalid.
void *rt_game3d_hit_event_point(void *obj) {
    rt_game3d_hit_event *event =
        game3d_hit_event_checked(obj, "Game3D.HitEvent3D.Point: invalid event");
    if (!event)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(event->point[0], event->point[1], event->point[2]);
}

/// @brief HitEvent3D.Normal — contact normal (fresh Vec3, +Y fallback).
/// @param obj HitEvent3D to query.
/// @return A new Vec3 containing the normalized contact direction, or +Y when invalid.
void *rt_game3d_hit_event_normal(void *obj) {
    rt_game3d_hit_event *event =
        game3d_hit_event_checked(obj, "Game3D.HitEvent3D.Normal: invalid event");
    if (!event)
        return rt_vec3_new(0.0, 1.0, 0.0);
    return rt_vec3_new(event->normal[0], event->normal[1], event->normal[2]);
}

/// @brief DamageEvent3D.Victim — damaged entity (NULL when stale).
/// @param obj DamageEvent3D to query.
/// @return The borrowed live victim Entity3D, or NULL when stale.
void *rt_game3d_damage_event_get_victim(void *obj) {
    rt_game3d_damage_event *event =
        game3d_damage_event_checked(obj, "Game3D.DamageEvent3D.get_victim: invalid event");
    if (!event)
        return NULL;
    rt_game3d_entity *entity =
        (rt_game3d_entity *)rt_g3d_checked_or_null(event->victim, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    return game3d_entity_alive_or_record(entity) ? entity : NULL;
}

/// @brief DamageEvent3D.Source — damage source entity or NULL.
/// @param obj DamageEvent3D to query.
/// @return The borrowed live source Entity3D, or NULL when absent or stale.
void *rt_game3d_damage_event_get_source(void *obj) {
    rt_game3d_damage_event *event =
        game3d_damage_event_checked(obj, "Game3D.DamageEvent3D.get_source: invalid event");
    if (!event)
        return NULL;
    rt_game3d_entity *entity =
        (rt_game3d_entity *)rt_g3d_checked_or_null(event->source, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    return game3d_entity_alive_or_record(entity) ? entity : NULL;
}

/// @brief DamageEvent3D.Amount — applied damage amount.
/// @param obj DamageEvent3D to query.
/// @return The applied damage amount, or 0 when invalid.
double rt_game3d_damage_event_get_amount(void *obj) {
    rt_game3d_damage_event *event =
        game3d_damage_event_checked(obj, "Game3D.DamageEvent3D.get_amount: invalid event");
    return event ? event->amount : 0.0;
}

/// @brief DamageEvent3D.Tag — caller-supplied damage tag.
/// @param obj DamageEvent3D to query.
/// @return The application-defined tag, or 0 when invalid.
int64_t rt_game3d_damage_event_get_tag(void *obj) {
    rt_game3d_damage_event *event =
        game3d_damage_event_checked(obj, "Game3D.DamageEvent3D.get_tag: invalid event");
    return event ? event->tag : 0;
}

/// @brief DamageEvent3D.WasLethal — this damage crossed hp to 0.
/// @param obj DamageEvent3D to query.
/// @return 1 when this event latched death, or 0 otherwise.
int8_t rt_game3d_damage_event_get_was_lethal(void *obj) {
    rt_game3d_damage_event *event =
        game3d_damage_event_checked(obj, "Game3D.DamageEvent3D.get_wasLethal: invalid event");
    return event ? event->was_lethal : 0;
}

//=========================================================================
// HitboxKind constants
//=========================================================================

/// @brief HitboxKind.Hurt — damageable region volume.
/// @return The stable RT_GAME3D_HITBOX_KIND_HURT value.
int64_t rt_game3d_hitbox_kind_hurt(void) {
    return RT_GAME3D_HITBOX_KIND_HURT;
}

/// @brief HitboxKind.Hit — attack volume.
/// @return The stable RT_GAME3D_HITBOX_KIND_HIT value.
int64_t rt_game3d_hitbox_kind_hit(void) {
    return RT_GAME3D_HITBOX_KIND_HIT;
}
