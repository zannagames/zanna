//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_controllers.c
// Purpose: Camera/character controllers for the Zanna.Game3D layer —
//   CharacterController3D, FirstPerson, FreeFly, Orbit, and Follow controllers,
//   plus their finalizers and the entity/node sync helpers. Split out of
//   rt_game3d.c; shares private types/helpers via rt_game3d_internal.h.
// Key invariants:
//   - Camera controllers retain their active world; World3D clears that ref on
//     detach/destroy, and moving a controller detaches it from the previous world.
// Ownership/Lifetime:
//   - GC-managed handles; finalizers release retained entity/character refs.
// Links: rt_game3d_internal.h, rt_camera3d.h, rt_physics3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements character movement and the built-in Game3D camera controllers.
/// @details This translation unit owns the CharacterController3D, first-person,
///          free-fly, orbit, and follow controller implementations. It converts
///          input snapshots into bounded movement, maintains controller-to-world
///          ownership, synchronizes physics characters with scene nodes, and
///          separates pre-physics input work from post-physics camera placement.
///          All public entry points validate runtime class identities; camera and
///          entity bindings are additionally checked to prevent cross-world state
///          mutation.

#include "rt_animcontroller3d.h"
#include "rt_asset.h"
#include "rt_audio.h"
#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_collider3d.h"
#include "rt_decal3d.h"
#include "rt_g3d_commit_queue.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_gltf.h"
#include "rt_graphics3d_ids.h"
#include "rt_input.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_mat4.h"
#include "rt_model3d.h"
#include "rt_navmesh3d.h"
#include "rt_object.h"
#include "rt_parallel.h"
#include "rt_particles3d.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_postfx3d.h"
#include "rt_quat.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "rt_textureasset3d.h"
#include "rt_threadpool.h"
#include "rt_trap.h"
#include "rt_vec2.h"
#include "rt_vec3.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Validate an entity binding before a controller uses it in a world.
/// @param entity Entity candidate; NULL is accepted.
/// @param world Intended controller world; NULL suppresses owner comparison.
/// @param api_name Trap message used when a spawned entity belongs elsewhere.
/// @return Non-zero when the binding is usable, otherwise zero.
int game3d_entity_validate_controller_world(rt_game3d_entity *entity,
                                            rt_game3d_world *world,
                                            const char *api_name);

/// @brief Compute walking-controller X/Z input without Space/Shift/Ctrl vertical keys.
/// @param input Input snapshot queried for WASD and arrow-key state; may be NULL.
/// @param[out] out_x Optional destination for the normalized right/left component.
/// @param[out] out_z Optional destination for the normalized forward/back component.
static void game3d_input_planar_move_axis_components(rt_game3d_input *input,
                                                     double *out_x,
                                                     double *out_z) {
    double x = 0.0;
    double z = 0.0;
    if (game3d_input_key_down(input, rt_keyboard_key_d()) ||
        game3d_input_key_down(input, rt_keyboard_key_right()))
        x += 1.0;
    if (game3d_input_key_down(input, rt_keyboard_key_a()) ||
        game3d_input_key_down(input, rt_keyboard_key_left()))
        x -= 1.0;
    if (game3d_input_key_down(input, rt_keyboard_key_w()) ||
        game3d_input_key_down(input, rt_keyboard_key_up()))
        z += 1.0;
    if (game3d_input_key_down(input, rt_keyboard_key_s()) ||
        game3d_input_key_down(input, rt_keyboard_key_down()))
        z -= 1.0;
    game3d_normalize_xz(&x, &z, 0.0, 0.0);
    if (out_x)
        *out_x = x;
    if (out_z)
        *out_z = z;
}

/// @brief GC finalizer for a character controller: release its world, entity, and
///   underlying character references.
/// @param obj CharacterController3D storage being finalized; NULL is ignored.
static void game3d_character_controller_finalize(void *obj) {
    rt_game3d_character_controller *controller = (rt_game3d_character_controller *)obj;
    if (!controller)
        return;
    game3d_release_ref(&controller->world);
    game3d_release_ref(&controller->entity);
    game3d_release_ref(&controller->character);
}

/// @brief Return the controller's Entity3D slot only when it still has the expected class.
/// @param controller Controller whose retained entity slot is inspected; may be NULL.
/// @return The live or recorded Entity3D pointer, or NULL for a missing, stale, or
///         wrongly typed slot.
static rt_game3d_entity *game3d_character_controller_entity_ref(
    const rt_game3d_character_controller *controller) {
    rt_game3d_entity *entity = controller ? (rt_game3d_entity *)rt_g3d_checked_or_null(
                                                controller->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID)
                                          : NULL;
    return game3d_entity_alive_or_record(entity) ? entity : NULL;
}

/// @brief Return the controller's Character3D slot only when it still has the expected class.
/// @param controller Controller whose retained character slot is inspected; may be NULL.
/// @return The validated Character3D pointer, or NULL when unavailable.
static void *game3d_character_controller_character_ref(
    const rt_game3d_character_controller *controller) {
    return controller ? rt_g3d_checked_or_null(controller->character, RT_G3D_CHARACTER3D_CLASS_ID)
                      : NULL;
}

/// @brief Return the FPS controller's nested CharacterController3D when valid.
/// @param controller First-person controller to inspect; may be NULL.
/// @return The validated nested CharacterController3D pointer, or NULL.
static void *game3d_first_person_character_controller_ref(
    const rt_game3d_first_person_controller *controller) {
    return controller ? rt_g3d_checked_or_null(controller->character_controller,
                                               RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID)
                      : NULL;
}

/// @brief Return an orbit target only when it is still a Vec3 or Entity3D.
/// @param controller Orbit controller whose target slot is inspected; may be NULL.
/// @return The retained Vec3 or live/recorded Entity3D target, or NULL when invalid.
static void *game3d_orbit_controller_target_ref(const rt_game3d_orbit_controller *controller) {
    if (!controller)
        return NULL;
    if (rt_g3d_is_vec3(controller->target))
        return controller->target;
    rt_game3d_entity *entity = (rt_game3d_entity *)rt_g3d_checked_or_null(
        controller->target, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    if (game3d_entity_alive_or_record(entity))
        return entity;
    return NULL;
}

/// @brief Compute the sanitized orbit distance range, preserving min <= max.
/// @param controller Orbit controller supplying the configured bounds; NULL selects defaults.
/// @param[out] out_min Optional destination for the positive sanitized minimum.
/// @param[out] out_max Optional destination for the sanitized maximum, never below the minimum.
static void game3d_orbit_controller_distance_range(const rt_game3d_orbit_controller *controller,
                                                   double *out_min,
                                                   double *out_max) {
    double min_distance =
        controller
            ? game3d_positive_clamped_or(controller->min_distance, 1.0, RT_GAME3D_COORD_ABS_MAX)
            : 1.0;
    double max_distance =
        controller
            ? game3d_positive_clamped_or(controller->max_distance, 100.0, RT_GAME3D_COORD_ABS_MAX)
            : 100.0;
    if (max_distance < min_distance)
        max_distance = min_distance;
    if (out_min)
        *out_min = min_distance;
    if (out_max)
        *out_max = max_distance;
}

/// @brief Return a finite orbit distance within the controller's sanitized range.
/// @param controller Orbit controller to inspect; may be NULL.
/// @return The positive clamped distance, or zero for a NULL controller.
static double game3d_orbit_controller_distance_value(const rt_game3d_orbit_controller *controller) {
    double min_distance = 1.0;
    double max_distance = 100.0;
    if (!controller)
        return 0.0;
    game3d_orbit_controller_distance_range(controller, &min_distance, &max_distance);
    return game3d_clamp(
        game3d_positive_clamped_or(controller->distance, min_distance, RT_GAME3D_COORD_ABS_MAX),
        min_distance,
        max_distance);
}

/// @brief Return the follow target only when it is still an Entity3D.
/// @param controller Follow controller whose target slot is inspected; may be NULL.
/// @return The live or recorded target entity, or NULL when unavailable.
static rt_game3d_entity *game3d_follow_controller_target_ref(
    const rt_game3d_follow_controller *controller) {
    rt_game3d_entity *entity =
        controller ? (rt_game3d_entity *)rt_g3d_checked_or_null(controller->target_entity,
                                                                RT_G3D_GAME3D_ENTITY_CLASS_ID)
                   : NULL;
    return game3d_entity_alive_or_record(entity) ? entity : NULL;
}

/// @brief Return the follow offset only when it is still a Vec3.
/// @param controller Follow controller whose offset slot is inspected; may be NULL.
/// @return The validated Vec3 offset pointer, or NULL.
static void *game3d_follow_controller_offset_ref(const rt_game3d_follow_controller *controller) {
    return controller && rt_g3d_is_vec3(controller->offset) ? controller->offset : NULL;
}

/// @brief Write a world-space position into a node, converting through its
///        parent's inverse world matrix so parent transforms are preserved.
/// @param node SceneNode3D to reposition; NULL is ignored.
/// @param[in,out] world_pos Three finite-clamped world coordinates. The supplied
///                array is sanitized in place before conversion to local space.
void game3d_set_node_world_position(void *node, double world_pos[3]) {
    void *parent;
    if (!node || !world_pos)
        return;
    world_pos[0] = game3d_clamp_coord_or(world_pos[0], 0.0);
    world_pos[1] = game3d_clamp_coord_or(world_pos[1], 0.0);
    world_pos[2] = game3d_clamp_coord_or(world_pos[2], 0.0);
    parent = rt_scene_node3d_get_parent(node);
    if (!parent) {
        rt_scene_node3d_set_position(node, world_pos[0], world_pos[1], world_pos[2]);
        return;
    }
    {
        void *parent_world = rt_scene_node3d_get_world_matrix(parent);
        void *parent_inv = parent_world ? rt_mat4_try_inverse(parent_world) : NULL;
        void *world_vec = rt_vec3_new(world_pos[0], world_pos[1], world_pos[2]);
        void *local =
            (parent_inv && world_vec) ? rt_mat4_transform_point(parent_inv, world_vec) : NULL;
        if (local) {
            rt_scene_node3d_set_position(node,
                                         game3d_clamp_coord_or(rt_vec3_x(local), 0.0),
                                         game3d_clamp_coord_or(rt_vec3_y(local), 0.0),
                                         game3d_clamp_coord_or(rt_vec3_z(local), 0.0));
        } else {
            rt_scene_node3d_set_position(node, world_pos[0], world_pos[1], world_pos[2]);
        }
        game3d_release_ref(&local);
        game3d_release_ref(&world_vec);
        game3d_release_ref(&parent_inv);
        game3d_release_ref(&parent_world);
    }
}

/// @brief Write a world-space rotation into a node, converting through its parent's
///        inverse world rotation so parent orientation is preserved (world = parent * local).
/// @param node SceneNode3D to rotate; NULL is ignored.
/// @param world_quat Desired world-space quaternion; NULL is ignored and ownership
///                   remains with the caller.
void game3d_set_node_world_rotation(void *node, void *world_quat) {
    void *parent;
    if (!node || !world_quat)
        return;
    parent = rt_scene_node3d_get_parent(node);
    if (!parent) {
        rt_scene_node3d_set_rotation(node, world_quat);
        return;
    }
    {
        void *parent_rot = rt_scene_node3d_get_world_rotation(parent);
        void *parent_inv = parent_rot ? rt_quat_inverse(parent_rot) : NULL;
        void *local = parent_inv ? rt_quat_mul(parent_inv, world_quat) : NULL;
        rt_scene_node3d_set_rotation(node, local ? local : world_quat);
        game3d_release_ref(&local);
        game3d_release_ref(&parent_inv);
        game3d_release_ref(&parent_rot);
    }
}

/// @brief Push a node's current world-space position/rotation/scale into its body.
/// @param entity Entity whose node and physics body are synchronized; missing
///               components make the operation a no-op.
/// @param force Non-zero to synchronize regardless of the node sync mode.
void game3d_sync_body_from_entity_node(rt_game3d_entity *entity, int8_t force) {
    void *node = game3d_entity_node_ref(entity);
    void *body = game3d_entity_body_ref(entity);
    if (!node || !body)
        return;
    if (!force && rt_scene_node3d_get_sync_mode(node) != RT_GAME3D_SYNC_BODY_FROM_NODE)
        return;
    void *pos = rt_scene_node3d_get_world_position(node);
    void *rot = rt_scene_node3d_get_world_rotation(node);
    void *scale = rt_scene_node3d_get_world_scale(node);
    if (pos)
        rt_body3d_set_position(body,
                               game3d_clamp_coord_or(rt_vec3_x(pos), 0.0),
                               game3d_clamp_coord_or(rt_vec3_y(pos), 0.0),
                               game3d_clamp_coord_or(rt_vec3_z(pos), 0.0));
    if (rot)
        rt_body3d_set_orientation(body, rot);
    if (scale)
        rt_body3d_set_scale(body,
                            game3d_scale_or_unit(rt_vec3_x(scale)),
                            game3d_scale_or_unit(rt_vec3_y(scale)),
                            game3d_scale_or_unit(rt_vec3_z(scale)));
    game3d_release_ref(&scale);
    game3d_release_ref(&rot);
    game3d_release_ref(&pos);
}

/// @brief Copy the character's current position back onto the driven entity's node.
/// @param controller Controller whose Character3D position drives its entity node;
///                   NULL or incomplete controllers are ignored.
static void game3d_character_controller_sync_entity(rt_game3d_character_controller *controller) {
    rt_game3d_entity *entity = game3d_character_controller_entity_ref(controller);
    void *character = game3d_character_controller_character_ref(controller);
    if (!entity || !character)
        return;
    void *pos = rt_character3d_get_position(character);
    void *node = game3d_entity_node_ref(entity);
    if (node && pos) {
        double world_pos[3] = {rt_vec3_x(pos), rt_vec3_y(pos), rt_vec3_z(pos)};
        game3d_set_node_world_position(node, world_pos);
    }
    game3d_release_ref(&pos);
}

/// @brief Create a capsule character controller bound to an entity, seeding the
///   character at the entity's position and wiring it into the world physics; sane
///   defaults are substituted for non-positive radius/height/mass. See header.
/// @param world_obj World3D that owns the physics simulation and controller binding.
/// @param entity_obj Entity3D whose scene node is driven by the character.
/// @param radius Capsule radius; invalid or non-positive values select 0.3.
/// @param height Standing capsule height; invalid or non-positive values select 1.8.
/// @param mass Character mass; invalid or negative values select 70.
/// @return A newly allocated CharacterController3D, or NULL after validation or
///         allocation failure. The returned controller retains its world, entity,
///         and underlying Character3D.
void *rt_game3d_character_controller_new(
    void *world_obj, void *entity_obj, double radius, double height, double mass) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.CharacterController3D.New: invalid world");
    rt_game3d_entity *entity = game3d_entity_checked(
        entity_obj, "Game3D.CharacterController3D.New: entity must be Entity3D");
    if (!world || !entity)
        return NULL;
    if (!game3d_entity_validate_controller_world(
            entity, world, "Game3D.CharacterController3D.New: entity belongs to another world"))
        return NULL;

    radius = game3d_positive_clamped_or(radius, 0.3, RT_GAME3D_SCALE_ABS_MAX);
    height = game3d_positive_clamped_or(height, 1.8, RT_GAME3D_SCALE_ABS_MAX);
    mass = game3d_nonnegative_clamped_or(mass, 70.0, RT_GAME3D_SCALE_ABS_MAX);

    rt_game3d_character_controller *controller = (rt_game3d_character_controller *)rt_obj_new_i64(
        RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID, (int64_t)sizeof(*controller));
    if (!controller) {
        rt_trap("Game3D.CharacterController3D.New: allocation failed");
        return NULL;
    }
    memset(controller, 0, sizeof(*controller));
    rt_obj_set_finalizer(controller, game3d_character_controller_finalize);
    game3d_assign_ref(&controller->world, world);
    controller->entity = entity;
    rt_obj_retain_maybe(entity);
    controller->character = rt_character3d_new(radius, height, mass);
    if (!controller->character) {
        if (rt_obj_release_check0(controller))
            rt_obj_free(controller);
        rt_trap("Game3D.CharacterController3D.New: Character3D allocation failed");
        return NULL;
    }
    rt_character3d_set_world(controller->character, world->physics);
    controller->speed = RT_GAME3D_DEFAULT_MOVE_SPEED;
    controller->jump_speed = RT_GAME3D_DEFAULT_JUMP_SPEED;
    controller->gravity = RT_GAME3D_DEFAULT_GRAVITY;
    controller->eye_height = height * 0.45;
    controller->stand_height = height;
    controller->crouch_height = height * 0.5;
    controller->capsule_radius = radius;
    controller->crouching = 0;

    void *pos = rt_game3d_entity_world_position(entity);
    if (pos) {
        rt_character3d_set_position(controller->character,
                                    game3d_clamp_coord_or(rt_vec3_x(pos), 0.0),
                                    game3d_clamp_coord_or(rt_vec3_y(pos), 0.0),
                                    game3d_clamp_coord_or(rt_vec3_z(pos), 0.0));
        game3d_release_ref(&pos);
    }
    game3d_character_controller_sync_entity(controller);
    return controller;
}

/// @brief Get the underlying Character3D object (NULL if invalid).
/// @param obj CharacterController3D runtime handle.
/// @return The validated underlying Character3D pointer, or NULL.
void *rt_game3d_character_controller_get_character(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.get_character: invalid controller");
    return game3d_character_controller_character_ref(controller);
}

/// @brief Get the entity driven by this controller (NULL if invalid).
/// @param obj CharacterController3D runtime handle.
/// @return The live or recorded driven Entity3D pointer, or NULL.
void *rt_game3d_character_controller_get_entity(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.get_entity: invalid controller");
    return game3d_character_controller_entity_ref(controller);
}

/// @brief Get the horizontal move speed in units/sec.
/// @param obj CharacterController3D runtime handle.
/// @return The finite non-negative speed, or zero for an invalid controller.
double rt_game3d_character_controller_get_speed(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.get_speed: invalid controller");
    return controller ? game3d_nonnegative_clamped_or(controller->speed,
                                                      RT_GAME3D_DEFAULT_MOVE_SPEED,
                                                      RT_GAME3D_CONTROLLER_SPEED_MAX)
                      : 0.0;
}

/// @brief Set the horizontal move speed (negatives reset to the default).
/// @param obj CharacterController3D runtime handle.
/// @param speed Requested units per second; invalid or negative values select
///              the default and large values are capped.
void rt_game3d_character_controller_set_speed(void *obj, double speed) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.set_speed: invalid controller");
    if (controller)
        controller->speed = game3d_nonnegative_clamped_or(
            speed, RT_GAME3D_DEFAULT_MOVE_SPEED, RT_GAME3D_CONTROLLER_SPEED_MAX);
}

/// @brief Get the jump launch speed in units/sec.
/// @param obj CharacterController3D runtime handle.
/// @return The finite non-negative launch speed, or zero for an invalid controller.
double rt_game3d_character_controller_get_jump_speed(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.get_jumpSpeed: invalid controller");
    return controller ? game3d_nonnegative_clamped_or(controller->jump_speed,
                                                      RT_GAME3D_DEFAULT_JUMP_SPEED,
                                                      RT_GAME3D_CONTROLLER_SPEED_MAX)
                      : 0.0;
}

/// @brief Set the jump launch speed (negatives reset to the default).
/// @param obj CharacterController3D runtime handle.
/// @param jump_speed Requested upward launch speed; invalid or negative values
///                   select the default and large values are capped.
void rt_game3d_character_controller_set_jump_speed(void *obj, double jump_speed) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.set_jumpSpeed: invalid controller");
    if (controller)
        controller->jump_speed = game3d_nonnegative_clamped_or(
            jump_speed, RT_GAME3D_DEFAULT_JUMP_SPEED, RT_GAME3D_CONTROLLER_SPEED_MAX);
}

/// @brief Get the gravity acceleration in units/sec².
/// @param obj CharacterController3D runtime handle.
/// @return The finite bounded gravity magnitude, or zero for an invalid controller.
double rt_game3d_character_controller_get_gravity(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.get_gravity: invalid controller");
    return controller ? game3d_clamp_abs_or(controller->gravity,
                                            RT_GAME3D_DEFAULT_GRAVITY,
                                            RT_GAME3D_CONTROLLER_SPEED_MAX)
                      : 0.0;
}

/// @brief Set the downward gravity acceleration magnitude (non-finite resets to the default).
/// @param obj CharacterController3D runtime handle.
/// @param gravity Requested acceleration magnitude; non-finite input selects the
///                default and extreme values are bounded.
void rt_game3d_character_controller_set_gravity(void *obj, double gravity) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.set_gravity: invalid controller");
    if (controller)
        controller->gravity =
            game3d_clamp_abs_or(gravity, RT_GAME3D_DEFAULT_GRAVITY, RT_GAME3D_CONTROLLER_SPEED_MAX);
}

/// @brief Shared planar drive core: integrate jump/gravity against the grounded state
///   and move the wrapped Character3D along the caller-supplied normalized XZ basis.
///   See rt_game3d_internal.h.
/// @param controller Character controller whose vertical state and wrapped
///                   Character3D are advanced.
/// @param input_obj Game3D input snapshot used for planar axes and jump presses.
/// @param fx World-space X component of the desired forward basis.
/// @param fz World-space Z component of the desired forward basis.
/// @param rx World-space X component of the desired right basis.
/// @param rz World-space Z component of the desired right basis.
/// @param dt Frame duration in seconds; sanitized to the controller time-step range.
void game3d_character_controller_drive(rt_game3d_character_controller *controller,
                                       void *input_obj,
                                       double fx,
                                       double fz,
                                       double rx,
                                       double rz,
                                       double dt) {
    void *character = game3d_character_controller_character_ref(controller);
    if (!controller || !character)
        return;

    dt = game3d_clamp_dt(dt);
    controller->vertical_velocity =
        game3d_clamp_abs_or(controller->vertical_velocity, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX);
    double move_x = 0.0;
    double move_z = 0.0;
    game3d_input_planar_move_axis_components((rt_game3d_input *)input_obj, &move_x, &move_z);

    move_x = game3d_finite_or(move_x, 0.0);
    move_z = game3d_finite_or(move_z, 0.0);
    double move_len = sqrt(move_x * move_x + move_z * move_z);
    if (isfinite(move_len) && move_len > 1.0) {
        move_x /= move_len;
        move_z /= move_len;
    }

    game3d_normalize_xz(&fx, &fz, 0.0, -1.0);
    game3d_normalize_xz(&rx, &rz, 1.0, 0.0);

    int8_t grounded = rt_character3d_is_grounded(character);
    double jump_speed = game3d_nonnegative_clamped_or(
        controller->jump_speed, RT_GAME3D_DEFAULT_JUMP_SPEED, RT_GAME3D_CONTROLLER_SPEED_MAX);
    double gravity = game3d_clamp_abs_or(
        controller->gravity, RT_GAME3D_DEFAULT_GRAVITY, RT_GAME3D_CONTROLLER_SPEED_MAX);
    if (grounded) {
        if (controller->vertical_velocity < 0.0)
            controller->vertical_velocity = -0.5;
        if (rt_game3d_input_pressed(input_obj, rt_game3d_key_space()))
            controller->vertical_velocity = jump_speed;
    } else {
        controller->vertical_velocity -= gravity * dt;
        controller->vertical_velocity = game3d_clamp(controller->vertical_velocity, -100.0, 100.0);
    }

    double speed = game3d_nonnegative_clamped_or(
        controller->speed, RT_GAME3D_DEFAULT_MOVE_SPEED, RT_GAME3D_CONTROLLER_SPEED_MAX);
    double vx = (fx * move_z + rx * move_x) * speed;
    double vz = (fz * move_z + rz * move_x) * speed;
    void *velocity = rt_vec3_new(vx, controller->vertical_velocity, vz);
    rt_character3d_move(character, velocity, dt);
    game3d_release_ref(&velocity);
    if (rt_character3d_is_grounded(character) && controller->vertical_velocity < 0.0)
        controller->vertical_velocity = -0.5;
    game3d_character_controller_sync_entity(controller);
}

/// @brief Advance the character one frame from input and camera orientation.
/// @details Builds a camera-relative X/Z move vector from WASD/arrow keys
///   (clamped to unit length), integrates jump/downward gravity against the grounded
///   state, moves the Character3D by `dt` (itself clamped), and syncs the result
///   back onto the entity. Traps on invalid input or a non-Camera3D camera.
/// @param obj CharacterController3D runtime handle.
/// @param input_obj Game3D input snapshot supplying movement and jump state.
/// @param camera Camera3D whose planar forward/right axes define movement space.
/// @param dt Frame duration in seconds; sanitized before integration.
void rt_game3d_character_controller_update(void *obj, void *input_obj, void *camera, double dt) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.update: invalid controller");
    (void)game3d_input_checked(input_obj, "Game3D.CharacterController3D.update: invalid input");
    if (!rt_g3d_has_class(camera, RT_G3D_CAMERA3D_CLASS_ID)) {
        rt_trap("Game3D.CharacterController3D.update: camera must be Camera3D");
        return;
    }
    void *character = game3d_character_controller_character_ref(controller);
    if (!controller || !character)
        return;

    void *forward = rt_camera3d_get_forward(camera);
    void *right = rt_camera3d_get_right(camera);
    double fx = forward ? rt_vec3_x(forward) : 0.0;
    double fz = forward ? rt_vec3_z(forward) : -1.0;
    double rx = right ? rt_vec3_x(right) : 1.0;
    double rz = right ? rt_vec3_z(right) : 0.0;
    game3d_release_ref(&right);
    game3d_release_ref(&forward);

    game3d_character_controller_drive(controller, input_obj, fx, fz, rx, rz, dt);
}

/// @brief Teleport the character to an absolute position (NaN-scrubbed), clearing
///   vertical velocity, and sync the entity. See header.
/// @param obj CharacterController3D runtime handle.
/// @param x Desired world-space X coordinate.
/// @param y Desired world-space Y coordinate.
/// @param z Desired world-space Z coordinate.
void rt_game3d_character_controller_teleport(void *obj, double x, double y, double z) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.teleport: invalid controller");
    void *character = game3d_character_controller_character_ref(controller);
    if (!controller || !character)
        return;
    controller->vertical_velocity = 0.0;
    rt_character3d_set_position(character,
                                game3d_clamp_coord_or(x, 0.0),
                                game3d_clamp_coord_or(y, 0.0),
                                game3d_clamp_coord_or(z, 0.0));
    game3d_character_controller_sync_entity(controller);
}

/// @brief True if the character is currently standing on ground. See header.
/// @param obj CharacterController3D runtime handle.
/// @return Non-zero when the wrapped character reports grounded, otherwise zero.
int8_t rt_game3d_character_controller_grounded(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.grounded: invalid controller");
    void *character = game3d_character_controller_character_ref(controller);
    return character ? rt_character3d_is_grounded(character) : 0;
}

/// @brief Get the crouch capsule height applied by SetCrouching(true).
/// @param obj CharacterController3D runtime handle.
/// @return The configured crouch height, or zero for an invalid controller.
double rt_game3d_character_controller_get_crouch_height(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.get_crouchHeight: invalid controller");
    return controller ? controller->crouch_height : 0.0;
}

/// @brief Set the crouch capsule height (positive; applied on the next SetCrouching).
/// @param obj CharacterController3D runtime handle.
/// @param height Requested positive capsule height; invalid values fall back to
///               half the stored standing height.
void rt_game3d_character_controller_set_crouch_height(void *obj, double height) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.set_crouchHeight: invalid controller");
    if (controller)
        controller->crouch_height = game3d_positive_clamped_or(
            height, controller->stand_height * 0.5, RT_GAME3D_SCALE_ABS_MAX);
}

/// @brief Toggle crouch: true swaps to CrouchHeight (always succeeds), false tries
///   to stand back up and returns false when blocked by a ceiling (TryStand).
/// @param obj CharacterController3D runtime handle.
/// @param crouching Non-zero to request the crouched height; zero to try restoring
///                  the standing height.
/// @return Non-zero when the requested capsule height was accepted, otherwise zero.
int8_t rt_game3d_character_controller_set_crouching(void *obj, int8_t crouching) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.setCrouching: invalid controller");
    void *character = game3d_character_controller_character_ref(controller);
    if (!controller || !character)
        return 0;
    if (crouching) {
        if (rt_character3d_try_set_height(character, controller->crouch_height)) {
            controller->crouching = 1;
            game3d_character_controller_sync_entity(controller);
            return 1;
        }
        return 0;
    }
    if (rt_character3d_try_set_height(character, controller->stand_height)) {
        controller->crouching = 0;
        game3d_character_controller_sync_entity(controller);
        return 1;
    }
    return 0;
}

/// @brief True while the crouch state is engaged.
/// @param obj CharacterController3D runtime handle.
/// @return Non-zero when crouching is engaged, otherwise zero.
int8_t rt_game3d_character_controller_is_crouching(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.isCrouching: invalid controller");
    return controller ? controller->crouching : 0;
}

/// @brief Get the dynamic push impulse scale (delegates to Character3D).
/// @param obj CharacterController3D runtime handle.
/// @return The wrapped character push strength, or zero when unavailable.
double rt_game3d_character_controller_get_push_strength(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.get_pushStrength: invalid controller");
    void *character = game3d_character_controller_character_ref(controller);
    return character ? rt_character3d_get_push_strength(character) : 0.0;
}

/// @brief Set the dynamic push impulse scale (delegates to Character3D).
/// @param obj CharacterController3D runtime handle.
/// @param strength Requested push impulse scale, interpreted by Character3D.
void rt_game3d_character_controller_set_push_strength(void *obj, double strength) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.set_pushStrength: invalid controller");
    void *character = game3d_character_controller_character_ref(controller);
    if (character)
        rt_character3d_set_push_strength(character, strength);
}

/// @brief Get whether the controller rides moving platforms.
/// @param obj CharacterController3D runtime handle.
/// @return Non-zero when moving-platform riding is enabled, otherwise zero.
int8_t rt_game3d_character_controller_get_ride_platforms(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.get_ridePlatforms: invalid controller");
    void *character = game3d_character_controller_character_ref(controller);
    return character ? rt_character3d_get_ride_platforms(character) : 0;
}

/// @brief Set whether the controller rides moving platforms.
/// @param obj CharacterController3D runtime handle.
/// @param enabled Non-zero to inherit supported platform motion.
void rt_game3d_character_controller_set_ride_platforms(void *obj, int8_t enabled) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.set_ridePlatforms: invalid controller");
    void *character = game3d_character_controller_character_ref(controller);
    if (character)
        rt_character3d_set_ride_platforms(character, enabled);
}

/// @brief True while the character rests on a too-steep surface.
/// @param obj CharacterController3D runtime handle.
/// @return Non-zero when Character3D reports a sliding state, otherwise zero.
int8_t rt_game3d_character_controller_is_sliding(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.isSliding: invalid controller");
    void *character = game3d_character_controller_character_ref(controller);
    return character ? rt_character3d_is_sliding(character) : 0;
}

/// @brief Build the probe basis (physics world, origin, planar forward, radius)
///   for the traversal-probe sugar. Forward comes from the entity node's world
///   rotation applied to -Z (identity rotation faces -Z). Returns 0 when the
///   controller lacks a world, character, or entity node.
/// @param controller Character controller supplying the world, character, and
///                   driven entity orientation.
/// @param[out] out_physics Destination for the borrowed Physics3DWorld pointer.
/// @param[out] origin Three-element destination for the foot-level world origin.
/// @param[out] forward Three-element destination for the entity-facing world vector.
/// @param[out] out_radius Destination for the positive capsule radius.
/// @return Non-zero when every required output was populated, otherwise zero.
static int game3d_character_controller_probe_basis(rt_game3d_character_controller *controller,
                                                   void **out_physics,
                                                   double origin[3],
                                                   double forward[3],
                                                   double *out_radius) {
    rt_game3d_world *world =
        controller ? (rt_game3d_world *)rt_g3d_checked_or_null(controller->world,
                                                               RT_G3D_GAME3D_WORLD_CLASS_ID)
                   : NULL;
    void *character = game3d_character_controller_character_ref(controller);
    rt_game3d_entity *entity = game3d_character_controller_entity_ref(controller);
    void *node = entity ? game3d_entity_node_ref(entity) : NULL;
    if (!world || !world->physics || !character)
        return 0;
    void *pos = rt_character3d_get_position(character);
    if (!pos)
        return 0;
    /* World probes take a FOOT-level origin; the character position is the
     * capsule center, so drop by half the current height. */
    double half_height = rt_character3d_get_height(character) * 0.5;
    origin[0] = game3d_clamp_coord_or(rt_vec3_x(pos), 0.0);
    origin[1] = game3d_clamp_coord_or(rt_vec3_y(pos) - half_height, 0.0);
    origin[2] = game3d_clamp_coord_or(rt_vec3_z(pos), 0.0);
    game3d_release_ref(&pos);
    forward[0] = 0.0;
    forward[1] = 0.0;
    forward[2] = -1.0;
    if (node) {
        double qx = 0.0;
        double qy = 0.0;
        double qz = 0.0;
        double qw = 1.0;
        if (rt_scene_node3d_get_world_rotation_components(node, &qx, &qy, &qz, &qw)) {
            /* v' = v + 2*w*(q×v) + 2*(q×(q×v)) for v = (0,0,-1). */
            double cx = qy * -1.0 - qz * 0.0; /* q × v */
            double cy = qz * 0.0 - qx * -1.0;
            double cz = qx * 0.0 - qy * 0.0;
            double ccx = qy * cz - qz * cy; /* q × (q × v) */
            double ccy = qz * cx - qx * cz;
            double ccz = qx * cy - qy * cx;
            forward[0] = 0.0 + 2.0 * (qw * cx + ccx);
            forward[1] = 0.0 + 2.0 * (qw * cy + ccy);
            forward[2] = -1.0 + 2.0 * (qw * cz + ccz);
        }
    }
    *out_physics = world->physics;
    *out_radius = controller->capsule_radius > 0.0 ? controller->capsule_radius : 0.3;
    return 1;
}

/// @brief Probe for a grabbable ledge ahead of the character (see
///   Physics3DWorld.ProbeLedge). Sweep depth defaults to 1.0 unit; mask all.
/// @param obj CharacterController3D runtime handle.
/// @param max_height Maximum ledge height above the foot-level probe origin.
/// @return A probe-result object supplied by Physics3D, or NULL when the
///         controller lacks a usable probe basis or no result is produced.
void *rt_game3d_character_controller_probe_ledge(void *obj, double max_height) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.probeLedge: invalid controller");
    void *physics = NULL;
    double origin[3];
    double forward[3];
    double radius = 0.3;
    if (!game3d_character_controller_probe_basis(controller, &physics, origin, forward, &radius))
        return NULL;
    void *origin_vec = rt_vec3_new(origin[0], origin[1], origin[2]);
    void *forward_vec = rt_vec3_new(forward[0], forward[1], forward[2]);
    void *result = rt_world3d_probe_ledge(
        physics, origin_vec, forward_vec, radius, max_height, 1.0, -1);
    game3d_release_ref(&forward_vec);
    game3d_release_ref(&origin_vec);
    return result;
}

/// @brief Probe for a vaultable obstacle ahead of the character (see
///   Physics3DWorld.ProbeVault). Mask all.
/// @param obj CharacterController3D runtime handle.
/// @param max_height Maximum vault height above the foot-level origin.
/// @param max_thickness Maximum accepted obstacle depth.
/// @return A probe-result object supplied by Physics3D, or NULL when the
///         controller lacks a usable probe basis or no result is produced.
void *rt_game3d_character_controller_probe_vault(void *obj,
                                                 double max_height,
                                                 double max_thickness) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.probeVault: invalid controller");
    void *physics = NULL;
    double origin[3];
    double forward[3];
    double radius = 0.3;
    if (!game3d_character_controller_probe_basis(controller, &physics, origin, forward, &radius))
        return NULL;
    void *origin_vec = rt_vec3_new(origin[0], origin[1], origin[2]);
    void *forward_vec = rt_vec3_new(forward[0], forward[1], forward[2]);
    void *result = rt_world3d_probe_vault(
        physics, origin_vec, forward_vec, radius, max_height, max_thickness, -1);
    game3d_release_ref(&forward_vec);
    game3d_release_ref(&origin_vec);
    return result;
}

/// @brief Resolve the ground body under the character to its owning Entity3D
///   through the world registry (NULL when airborne or unmanaged).
/// @param obj CharacterController3D runtime handle.
/// @return The live or recorded entity that owns the supporting body, or NULL
///         while airborne, unmanaged, or invalid.
void *rt_game3d_character_controller_ground_entity(void *obj) {
    rt_game3d_character_controller *controller = game3d_character_controller_checked(
        obj, "Game3D.CharacterController3D.groundEntity: invalid controller");
    void *character = game3d_character_controller_character_ref(controller);
    rt_game3d_world *world =
        controller ? (rt_game3d_world *)rt_g3d_checked_or_null(controller->world,
                                                               RT_G3D_GAME3D_WORLD_CLASS_ID)
                   : NULL;
    if (!character || !world)
        return NULL;
    void *body = rt_character3d_get_ground_body(character);
    if (!body)
        return NULL;
    rt_game3d_entity *entity = game3d_world_find_entity_by_body(world, body);
    return game3d_entity_alive_or_record(entity) ? entity : NULL;
}

/// @brief True if `controller` is NULL or one of the four Game3D camera-controller
///   classes — the set accepted by World3D.SetCameraController.
/// @param controller Candidate runtime object; NULL represents no active controller.
/// @return Non-zero for NULL or a supported camera-controller class, otherwise zero.
int game3d_camera_controller_is_valid(void *controller) {
    if (!controller)
        return 1;
    int64_t cid = rt_obj_class_id(controller);
    return cid == RT_G3D_GAME3D_FIRSTPERSON_CLASS_ID || cid == RT_G3D_GAME3D_FREEFLY_CLASS_ID ||
           cid == RT_G3D_GAME3D_ORBIT_CLASS_ID || cid == RT_G3D_GAME3D_FOLLOW_CLASS_ID ||
           cid == RT_G3D_GAME3D_THIRDPERSON_CLASS_ID || cid == RT_G3D_GAME3D_RAILCAMERA_CLASS_ID;
}

/// @brief Return the world currently retained by a camera controller, or NULL.
/// @param controller Supported camera-controller runtime object; may be NULL.
/// @return The retained World3D after class validation, or NULL.
void *game3d_camera_controller_get_world_ref(void *controller) {
    void *world = NULL;
    if (!controller)
        return NULL;
    int64_t cid = rt_obj_class_id(controller);
    if (cid == RT_G3D_GAME3D_FIRSTPERSON_CLASS_ID)
        world = ((rt_game3d_first_person_controller *)controller)->world;
    else if (cid == RT_G3D_GAME3D_FREEFLY_CLASS_ID)
        world = ((rt_game3d_free_fly_controller *)controller)->world;
    else if (cid == RT_G3D_GAME3D_ORBIT_CLASS_ID)
        world = ((rt_game3d_orbit_controller *)controller)->world;
    else if (cid == RT_G3D_GAME3D_FOLLOW_CLASS_ID)
        world = ((rt_game3d_follow_controller *)controller)->world;
    else if (cid == RT_G3D_GAME3D_THIRDPERSON_CLASS_ID)
        world = ((rt_game3d_thirdperson_controller *)controller)->world;
    else if (cid == RT_G3D_GAME3D_RAILCAMERA_CLASS_ID)
        world = ((rt_game3d_rail_camera *)controller)->world;
    return rt_g3d_checked_or_null(world, RT_G3D_GAME3D_WORLD_CLASS_ID);
}

/// @brief Rebind a camera controller's retained world reference to @p world.
/// @param controller Supported camera-controller runtime object; NULL is ignored.
/// @param world New World3D reference, or NULL to detach. The slot retains a
///              valid world and releases its previous value.
void game3d_camera_controller_bind_world_ref(void *controller, void *world) {
    if (!controller)
        return;
    int64_t cid = rt_obj_class_id(controller);
    if (cid == RT_G3D_GAME3D_FIRSTPERSON_CLASS_ID) {
        game3d_assign_typed_ref(&((rt_game3d_first_person_controller *)controller)->world,
                                world,
                                RT_G3D_GAME3D_WORLD_CLASS_ID);
    } else if (cid == RT_G3D_GAME3D_FREEFLY_CLASS_ID) {
        game3d_assign_typed_ref(&((rt_game3d_free_fly_controller *)controller)->world,
                                world,
                                RT_G3D_GAME3D_WORLD_CLASS_ID);
    } else if (cid == RT_G3D_GAME3D_ORBIT_CLASS_ID) {
        game3d_assign_typed_ref(&((rt_game3d_orbit_controller *)controller)->world,
                                world,
                                RT_G3D_GAME3D_WORLD_CLASS_ID);
    } else if (cid == RT_G3D_GAME3D_FOLLOW_CLASS_ID) {
        game3d_assign_typed_ref(&((rt_game3d_follow_controller *)controller)->world,
                                world,
                                RT_G3D_GAME3D_WORLD_CLASS_ID);
    } else if (cid == RT_G3D_GAME3D_RAILCAMERA_CLASS_ID) {
        game3d_assign_typed_ref(&((rt_game3d_rail_camera *)controller)->world,
                                world,
                                RT_G3D_GAME3D_WORLD_CLASS_ID);
    } else if (cid == RT_G3D_GAME3D_THIRDPERSON_CLASS_ID) {
        rt_game3d_thirdperson_controller *third =
            (rt_game3d_thirdperson_controller *)controller;
        if (!world)
            game3d_thirdperson_reset_fades(third);
        game3d_assign_typed_ref(&third->world, world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    }
}

/// @brief Clear a camera controller's retained world reference without mutating nested controllers.
/// @param controller Supported camera-controller runtime object; NULL is ignored.
void game3d_camera_controller_clear_world_ref(void *controller) {
    game3d_camera_controller_bind_world_ref(controller, NULL);
}

/// @brief Clear a camera controller's world reference only when it still points at @p world.
/// @param controller Supported camera-controller runtime object; may be NULL.
/// @param world Expected current World3D pointer.
void game3d_camera_controller_clear_world_ref_if(void *controller, void *world) {
    if (game3d_camera_controller_get_world_ref(controller) == world)
        game3d_camera_controller_bind_world_ref(controller, NULL);
}

/// @brief Validate that a camera controller is being updated against its bound world.
/// @details Direct update calls with a different world can otherwise apply input and
///   camera changes to one World3D while retained controller state belongs to another.
///   A NULL bound world is allowed for detached controllers so explicit update calls
///   remain possible after clearing a world slot.
/// @param controller Camera controller being updated; may be detached or NULL.
/// @param world World3D supplied to the update operation; may be NULL.
/// @param api_name Trap message used for a cross-world mismatch; NULL selects
///                 a generic controller message.
/// @return Non-zero when detached or bound to @p world, otherwise zero.
int game3d_camera_controller_validate_world(void *controller,
                                            rt_game3d_world *world,
                                            const char *api_name) {
    void *bound_world = game3d_camera_controller_get_world_ref(controller);
    if (bound_world && world && bound_world != world) {
        rt_trap(api_name ? api_name
                         : "Game3D.Controller.update: controller belongs to another world");
        return 0;
    }
    return 1;
}

/// @brief Validate that a CharacterController3D's retained physics world matches @p world.
/// @details First-person controllers can wrap a separately-created character
///   controller. This prevents one world's FPS controller from moving a character
///   registered in another world's physics simulation.
/// @param controller CharacterController3D whose retained world is inspected.
/// @param world Intended update world; NULL suppresses comparison.
/// @param api_name Trap message used on mismatch; NULL selects a generic message.
/// @return Non-zero when detached or bound to @p world, otherwise zero.
int game3d_character_controller_validate_world(rt_game3d_character_controller *controller,
                                               rt_game3d_world *world,
                                               const char *api_name) {
    void *bound_world =
        controller ? rt_g3d_checked_or_null(controller->world, RT_G3D_GAME3D_WORLD_CLASS_ID) : NULL;
    if (bound_world && world && bound_world != world) {
        rt_trap(api_name ? api_name
                         : "Game3D.CharacterController3D.update: controller belongs to another "
                           "world");
        return 0;
    }
    return 1;
}

/// @brief Validate that a spawned Entity3D belongs to @p world when it already has an owner.
/// @details Unspawned entities remain assignable because they can be spawned into
///   the target world later. Spawned entities must match to avoid controllers reading
///   transforms from one World3D while writing cameras or physics state in another.
/// @param entity Entity candidate; NULL is accepted.
/// @param world Intended controller world; NULL suppresses owner comparison.
/// @param api_name Trap message used for an ownership mismatch; NULL selects a
///                 generic controller message.
/// @return Non-zero for a usable entity binding, otherwise zero.
int game3d_entity_validate_controller_world(rt_game3d_entity *entity,
                                            rt_game3d_world *world,
                                            const char *api_name) {
    if (entity && !game3d_entity_alive_or_record(entity))
        return 0;
    if (entity && entity->spawned && entity->world && world && entity->world != world) {
        rt_trap(api_name ? api_name : "Game3D.Controller: entity belongs to another world");
        return 0;
    }
    return 1;
}

/// @brief GC finalizer for the FPS controller: release its character controller.
/// @param obj FirstPersonController storage being finalized; NULL is ignored.
static void game3d_first_person_controller_finalize(void *obj) {
    rt_game3d_first_person_controller *controller = (rt_game3d_first_person_controller *)obj;
    if (controller) {
        game3d_release_ref(&controller->world);
        game3d_release_ref(&controller->character_controller);
    }
}

/// @brief Create a first-person controller bound to the world's camera. See header.
/// @param world_obj World3D whose camera and input snapshot the controller uses.
/// @return A newly allocated FirstPersonController retaining @p world_obj, or
///         NULL after validation or allocation failure.
void *rt_game3d_first_person_controller_new(void *world_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.FirstPersonController.New: invalid world");
    if (!world)
        return NULL;
    rt_game3d_first_person_controller *controller =
        (rt_game3d_first_person_controller *)rt_obj_new_i64(RT_G3D_GAME3D_FIRSTPERSON_CLASS_ID,
                                                            (int64_t)sizeof(*controller));
    if (!controller) {
        rt_trap("Game3D.FirstPersonController.New: allocation failed");
        return NULL;
    }
    memset(controller, 0, sizeof(*controller));
    rt_obj_set_finalizer(controller, game3d_first_person_controller_finalize);
    game3d_assign_ref(&controller->world, world);
    controller->speed = RT_GAME3D_DEFAULT_MOVE_SPEED;
    controller->look_sensitivity = RT_GAME3D_DEFAULT_LOOK_SENSITIVITY;
    controller->capture_mouse = 1;
    if (world->camera)
        rt_camera3d_fps_init(world->camera);
    return controller;
}

/// @brief Get the character controller driving movement (NULL if none/invalid).
/// @param obj FirstPersonController runtime handle.
/// @return The validated nested CharacterController3D pointer, or NULL.
void *rt_game3d_first_person_controller_get_character(void *obj) {
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.get_character: invalid controller");
    return game3d_first_person_character_controller_ref(controller);
}

/// @brief Set the character controller driving movement; traps on a non-CharacterController3D.
/// @param obj FirstPersonController runtime handle.
/// @param character_controller CharacterController3D to retain, or NULL to use
///                             direct camera walking. A controller bound to a
///                             different world is rejected.
void rt_game3d_first_person_controller_set_character(void *obj, void *character_controller) {
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.set_character: invalid controller");
    if (character_controller &&
        !rt_g3d_has_class(character_controller, RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID)) {
        rt_trap("Game3D.FirstPersonController.set_character: value must be CharacterController3D");
        return;
    }
    if (controller && character_controller) {
        rt_game3d_world *world = (rt_game3d_world *)rt_g3d_checked_or_null(
            controller->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
        rt_game3d_character_controller *character = game3d_character_controller_checked(
            character_controller, "Game3D.FirstPersonController.set_character: invalid character");
        if (!game3d_character_controller_validate_world(
                character,
                world,
                "Game3D.FirstPersonController.set_character: character belongs "
                "to another world"))
            return;
    }
    if (controller)
        game3d_assign_ref(&controller->character_controller, character_controller);
}

/// @brief Get the move speed in units/sec.
/// @param obj FirstPersonController runtime handle.
/// @return The finite non-negative movement speed, or zero when invalid.
double rt_game3d_first_person_controller_get_speed(void *obj) {
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.get_speed: invalid controller");
    return controller ? game3d_nonnegative_clamped_or(controller->speed,
                                                      RT_GAME3D_DEFAULT_MOVE_SPEED,
                                                      RT_GAME3D_CONTROLLER_SPEED_MAX)
                      : 0.0;
}

/// @brief Set the move speed (negatives reset to the default).
/// @param obj FirstPersonController runtime handle.
/// @param speed Requested movement speed; invalid or negative values select the
///              default and extreme values are capped.
void rt_game3d_first_person_controller_set_speed(void *obj, double speed) {
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.set_speed: invalid controller");
    if (controller)
        controller->speed = game3d_nonnegative_clamped_or(
            speed, RT_GAME3D_DEFAULT_MOVE_SPEED, RT_GAME3D_CONTROLLER_SPEED_MAX);
}

/// @brief Get the mouse-look sensitivity.
/// @param obj FirstPersonController runtime handle.
/// @return The finite non-negative sensitivity, or zero when invalid.
double rt_game3d_first_person_controller_get_look_sensitivity(void *obj) {
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.get_lookSensitivity: invalid controller");
    return controller ? game3d_nonnegative_clamped_or(controller->look_sensitivity,
                                                      RT_GAME3D_DEFAULT_LOOK_SENSITIVITY,
                                                      RT_GAME3D_LOOK_SENSITIVITY_MAX)
                      : 0.0;
}

/// @brief Set the mouse-look sensitivity (negatives reset to the default).
/// @param obj FirstPersonController runtime handle.
/// @param sensitivity Requested degrees-per-mouse-unit scale; invalid or
///                    negative values select the default.
void rt_game3d_first_person_controller_set_look_sensitivity(void *obj, double sensitivity) {
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.set_lookSensitivity: invalid controller");
    if (controller)
        controller->look_sensitivity = game3d_nonnegative_clamped_or(
            sensitivity, RT_GAME3D_DEFAULT_LOOK_SENSITIVITY, RT_GAME3D_LOOK_SENSITIVITY_MAX);
}

/// @brief Capture and hide the cursor and remember the captured state.
/// @param obj FirstPersonController runtime handle.
void rt_game3d_first_person_controller_capture_mouse(void *obj) {
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.captureMouse: invalid controller");
    if (controller) {
        controller->capture_mouse = 1;
        rt_mouse_capture();
    }
}

/// @brief Release the cursor and clear the captured state.
/// @param obj FirstPersonController runtime handle.
void rt_game3d_first_person_controller_release_mouse(void *obj) {
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.releaseMouse: invalid controller");
    if (controller) {
        controller->capture_mouse = 0;
        rt_mouse_release();
    }
}

/// @brief Update FPS look + movement for the frame.
/// @details Applies mouse-look to the camera; if a character controller is attached it
///   drives ground movement through it, otherwise it free-walks the camera directly.
///   Re-captures the cursor each frame when capture is enabled.
/// @param obj FirstPersonController runtime handle.
/// @param world_obj World3D supplying the camera and current input snapshot; it
///                  must match the retained world when one is bound.
/// @param dt Frame duration in seconds; sanitized before camera or character movement.
void rt_game3d_first_person_controller_update(void *obj, void *world_obj, double dt) {
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.update: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.FirstPersonController.update: invalid world");
    if (!controller || !world || !world->camera || !world->input)
        return;
    if (!game3d_camera_controller_validate_world(
            controller,
            world,
            "Game3D.FirstPersonController.update: controller belongs to "
            "another world"))
        return;
    dt = game3d_clamp_dt(dt);
    if (controller->capture_mouse)
        rt_mouse_capture();
    double sensitivity = game3d_nonnegative_clamped_or(controller->look_sensitivity,
                                                       RT_GAME3D_DEFAULT_LOOK_SENSITIVITY,
                                                       RT_GAME3D_LOOK_SENSITIVITY_MAX);
    double yaw = game3d_input_mouse_fdx((rt_game3d_input *)world->input) * sensitivity;
    double pitch = 0.0 - game3d_input_mouse_fdy((rt_game3d_input *)world->input) * sensitivity;
    yaw = game3d_clamp_abs_or(yaw, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
    pitch = game3d_clamp_abs_or(pitch, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
    void *character_controller = game3d_first_person_character_controller_ref(controller);
    if (character_controller) {
        rt_game3d_character_controller *character = game3d_character_controller_checked(
            character_controller, "Game3D.FirstPersonController.update: invalid character");
        if (!game3d_character_controller_validate_world(
                character,
                world,
                "Game3D.FirstPersonController.update: character belongs to "
                "another world"))
            return;
        rt_camera3d_fps_update(world->camera, yaw, pitch, 0.0, 0.0, 0.0, 0.0, dt);
        rt_game3d_character_controller_set_speed(
            character_controller,
            game3d_nonnegative_clamped_or(
                controller->speed, RT_GAME3D_DEFAULT_MOVE_SPEED, RT_GAME3D_CONTROLLER_SPEED_MAX));
        rt_game3d_character_controller_update(
            character_controller, world->input, world->camera, dt);
    } else {
        double move_x = 0.0;
        double move_y = 0.0;
        double move_z = 0.0;
        game3d_input_move_axis_components(
            (rt_game3d_input *)world->input, &move_x, &move_y, &move_z);
        double speed = game3d_nonnegative_clamped_or(
            controller->speed, RT_GAME3D_DEFAULT_MOVE_SPEED, RT_GAME3D_CONTROLLER_SPEED_MAX);
        rt_camera3d_fps_update(world->camera, yaw, pitch, move_z, move_x, move_y, speed, dt);
    }
}

/// @brief After physics, snap the camera to the character's eye height (only when a
///   character controller is attached). See header.
/// @param obj FirstPersonController runtime handle.
/// @param world_obj World3D whose camera follows the nested character.
/// @param dt Frame duration in seconds; unused because placement is an exact snap.
void rt_game3d_first_person_controller_late_update(void *obj, void *world_obj, double dt) {
    (void)dt;
    rt_game3d_first_person_controller *controller = game3d_first_person_controller_checked(
        obj, "Game3D.FirstPersonController.lateUpdate: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.FirstPersonController.lateUpdate: invalid world");
    void *character_controller = game3d_first_person_character_controller_ref(controller);
    if (!controller || !world || !world->camera || !character_controller)
        return;
    if (!game3d_camera_controller_validate_world(
            controller,
            world,
            "Game3D.FirstPersonController.lateUpdate: controller belongs to "
            "another world"))
        return;
    rt_game3d_character_controller *character = game3d_character_controller_checked(
        character_controller, "Game3D.FirstPersonController.lateUpdate: invalid character");
    if (!game3d_character_controller_validate_world(
            character,
            world,
            "Game3D.FirstPersonController.lateUpdate: character belongs to "
            "another world"))
        return;
    void *character_ref = game3d_character_controller_character_ref(character);
    if (!character || !character_ref)
        return;
    void *pos = rt_character3d_get_position(character_ref);
    if (pos) {
        /* Scale the standing eye offset by the capsule's CURRENT height so a
         * crouched character's camera drops with the capsule instead of
         * floating at standing eye level above the shrunk collider. */
        double eye_offset = character->eye_height;
        double current_height = rt_character3d_get_height(character_ref);
        if (isfinite(current_height) && current_height > 1e-9 &&
            isfinite(character->stand_height) && character->stand_height > 1e-9)
            eye_offset *= current_height / character->stand_height;
        void *eye = rt_vec3_new(game3d_clamp_coord_or(rt_vec3_x(pos), 0.0),
                                game3d_clamp_coord_or(rt_vec3_y(pos) + eye_offset, 0.0),
                                game3d_clamp_coord_or(rt_vec3_z(pos), 0.0));
        rt_camera3d_set_position(world->camera, eye);
        game3d_release_ref(&eye);
    }
    game3d_release_ref(&pos);
}

/// @brief GC finalizer for a FreeFlyController: release its retained world/camera references.
/// @param obj FreeFlyController storage being finalized; NULL is ignored.
static void game3d_free_fly_controller_finalize(void *obj) {
    rt_game3d_free_fly_controller *controller = (rt_game3d_free_fly_controller *)obj;
    if (controller)
        game3d_release_ref(&controller->world);
}

/// @brief Create a free-fly spectator controller for the world's camera. See header.
/// @param world_obj World3D whose camera and input snapshot the controller uses.
/// @return A newly allocated FreeFlyController retaining @p world_obj, or NULL
///         after validation or allocation failure.
void *rt_game3d_free_fly_controller_new(void *world_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.FreeFlyController.New: invalid world");
    if (!world)
        return NULL;
    rt_game3d_free_fly_controller *controller = (rt_game3d_free_fly_controller *)rt_obj_new_i64(
        RT_G3D_GAME3D_FREEFLY_CLASS_ID, (int64_t)sizeof(*controller));
    if (!controller) {
        rt_trap("Game3D.FreeFlyController.New: allocation failed");
        return NULL;
    }
    memset(controller, 0, sizeof(*controller));
    rt_obj_set_finalizer(controller, game3d_free_fly_controller_finalize);
    game3d_assign_ref(&controller->world, world);
    controller->speed = RT_GAME3D_DEFAULT_MOVE_SPEED;
    controller->look_sensitivity = RT_GAME3D_DEFAULT_LOOK_SENSITIVITY;
    controller->capture_mouse = 1;
    if (world->camera)
        rt_camera3d_fps_init(world->camera);
    return controller;
}

/// @brief Get the fly speed in units/sec.
/// @param obj FreeFlyController runtime handle.
/// @return The finite non-negative fly speed, or zero when invalid.
double rt_game3d_free_fly_controller_get_speed(void *obj) {
    rt_game3d_free_fly_controller *controller = game3d_free_fly_controller_checked(
        obj, "Game3D.FreeFlyController.get_speed: invalid controller");
    return controller ? game3d_nonnegative_clamped_or(controller->speed,
                                                      RT_GAME3D_DEFAULT_MOVE_SPEED,
                                                      RT_GAME3D_CONTROLLER_SPEED_MAX)
                      : 0.0;
}

/// @brief Set the fly speed (negatives reset to the default).
/// @param obj FreeFlyController runtime handle.
/// @param speed Requested movement speed; invalid or negative values select the
///              default and extreme values are capped.
void rt_game3d_free_fly_controller_set_speed(void *obj, double speed) {
    rt_game3d_free_fly_controller *controller = game3d_free_fly_controller_checked(
        obj, "Game3D.FreeFlyController.set_speed: invalid controller");
    if (controller)
        controller->speed = game3d_nonnegative_clamped_or(
            speed, RT_GAME3D_DEFAULT_MOVE_SPEED, RT_GAME3D_CONTROLLER_SPEED_MAX);
}

/// @brief Get the mouse-look sensitivity.
/// @param obj FreeFlyController runtime handle.
/// @return The finite non-negative sensitivity, or zero when invalid.
double rt_game3d_free_fly_controller_get_look_sensitivity(void *obj) {
    rt_game3d_free_fly_controller *controller = game3d_free_fly_controller_checked(
        obj, "Game3D.FreeFlyController.get_lookSensitivity: invalid controller");
    return controller ? game3d_nonnegative_clamped_or(controller->look_sensitivity,
                                                      RT_GAME3D_DEFAULT_LOOK_SENSITIVITY,
                                                      RT_GAME3D_LOOK_SENSITIVITY_MAX)
                      : 0.0;
}

/// @brief Set the mouse-look sensitivity (negatives reset to the default).
/// @param obj FreeFlyController runtime handle.
/// @param sensitivity Requested degrees-per-mouse-unit scale; invalid or
///                    negative values select the default.
void rt_game3d_free_fly_controller_set_look_sensitivity(void *obj, double sensitivity) {
    rt_game3d_free_fly_controller *controller = game3d_free_fly_controller_checked(
        obj, "Game3D.FreeFlyController.set_lookSensitivity: invalid controller");
    if (controller)
        controller->look_sensitivity = game3d_nonnegative_clamped_or(
            sensitivity, RT_GAME3D_DEFAULT_LOOK_SENSITIVITY, RT_GAME3D_LOOK_SENSITIVITY_MAX);
}

/// @brief Capture and hide the cursor and remember the captured state.
/// @param obj FreeFlyController runtime handle.
void rt_game3d_free_fly_controller_capture_mouse(void *obj) {
    rt_game3d_free_fly_controller *controller = game3d_free_fly_controller_checked(
        obj, "Game3D.FreeFlyController.captureMouse: invalid controller");
    if (controller) {
        controller->capture_mouse = 1;
        rt_mouse_capture();
    }
}

/// @brief Release the cursor and clear the captured state.
/// @param obj FreeFlyController runtime handle.
void rt_game3d_free_fly_controller_release_mouse(void *obj) {
    rt_game3d_free_fly_controller *controller = game3d_free_fly_controller_checked(
        obj, "Game3D.FreeFlyController.releaseMouse: invalid controller");
    if (controller) {
        controller->capture_mouse = 0;
        rt_mouse_release();
    }
}

/// @brief Update free-fly look and 6-DOF movement (including vertical) from input for
///   the frame; re-captures the cursor when capture is enabled.
/// @param obj FreeFlyController runtime handle.
/// @param world_obj World3D supplying the camera and current input snapshot; it
///                  must match the retained world when one is bound.
/// @param dt Frame duration in seconds; sanitized before camera integration.
void rt_game3d_free_fly_controller_update(void *obj, void *world_obj, double dt) {
    rt_game3d_free_fly_controller *controller = game3d_free_fly_controller_checked(
        obj, "Game3D.FreeFlyController.update: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.FreeFlyController.update: invalid world");
    if (!controller || !world || !world->camera || !world->input)
        return;
    if (!game3d_camera_controller_validate_world(
            controller,
            world,
            "Game3D.FreeFlyController.update: controller belongs to another "
            "world"))
        return;
    dt = game3d_clamp_dt(dt);
    if (controller->capture_mouse)
        rt_mouse_capture();
    double move_x = 0.0;
    double move_y = 0.0;
    double move_z = 0.0;
    game3d_input_move_axis_components((rt_game3d_input *)world->input, &move_x, &move_y, &move_z);
    double sensitivity = game3d_nonnegative_clamped_or(controller->look_sensitivity,
                                                       RT_GAME3D_DEFAULT_LOOK_SENSITIVITY,
                                                       RT_GAME3D_LOOK_SENSITIVITY_MAX);
    double yaw = game3d_input_mouse_fdx((rt_game3d_input *)world->input) * sensitivity;
    double pitch = 0.0 - game3d_input_mouse_fdy((rt_game3d_input *)world->input) * sensitivity;
    yaw = game3d_clamp_abs_or(yaw, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
    pitch = game3d_clamp_abs_or(pitch, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
    rt_camera3d_fps_update(world->camera,
                           yaw,
                           pitch,
                           move_z,
                           move_x,
                           move_y,
                           game3d_nonnegative_clamped_or(controller->speed,
                                                         RT_GAME3D_DEFAULT_MOVE_SPEED,
                                                         RT_GAME3D_CONTROLLER_SPEED_MAX),
                           dt);
}

/// @brief No-op late update (free-fly needs no post-physics pass); validates handles. See header.
/// @param obj FreeFlyController runtime handle.
/// @param world_obj World3D validated against the controller binding.
/// @param dt Frame duration in seconds; intentionally unused.
void rt_game3d_free_fly_controller_late_update(void *obj, void *world_obj, double dt) {
    (void)dt;
    rt_game3d_free_fly_controller *controller = game3d_free_fly_controller_checked(
        obj, "Game3D.FreeFlyController.lateUpdate: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.FreeFlyController.lateUpdate: invalid world");
    (void)game3d_camera_controller_validate_world(
        controller,
        world,
        "Game3D.FreeFlyController.lateUpdate: controller belongs to another "
        "world");
}

/// @brief GC finalizer for the orbit controller: release its target reference.
/// @param obj OrbitController storage being finalized; NULL is ignored.
static void game3d_orbit_controller_finalize(void *obj) {
    rt_game3d_orbit_controller *controller = (rt_game3d_orbit_controller *)obj;
    if (controller) {
        game3d_release_ref(&controller->world);
        game3d_release_ref(&controller->target);
    }
}

/// @brief Create an orbit controller circling the given Vec3 target; traps on a
///   non-Vec3 target. See header.
/// @param world_obj World3D whose camera and input snapshot the controller uses.
/// @param target Vec3 world position or Entity3D to orbit. Entity targets must
///               not belong to another world.
/// @return A newly allocated OrbitController retaining the world and target, or
///         NULL after validation or allocation failure.
void *rt_game3d_orbit_controller_new(void *world_obj, void *target) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.OrbitController.New: invalid world");
    rt_game3d_entity *target_entity = NULL;
    if (!rt_g3d_is_vec3(target)) {
        target_entity = game3d_entity_checked(
            target, "Game3D.OrbitController.New: target must be Vec3 or Entity3D");
        if (!target_entity)
            return NULL;
    }
    if (!world)
        return NULL;
    if (target_entity &&
        !game3d_entity_validate_controller_world(
            target_entity, world, "Game3D.OrbitController.New: target belongs to another world"))
        return NULL;
    rt_game3d_orbit_controller *controller = (rt_game3d_orbit_controller *)rt_obj_new_i64(
        RT_G3D_GAME3D_ORBIT_CLASS_ID, (int64_t)sizeof(*controller));
    if (!controller) {
        rt_trap("Game3D.OrbitController.New: allocation failed");
        return NULL;
    }
    memset(controller, 0, sizeof(*controller));
    rt_obj_set_finalizer(controller, game3d_orbit_controller_finalize);
    game3d_assign_ref(&controller->world, world);
    game3d_assign_ref(&controller->target, target);
    controller->distance = 6.0;
    controller->min_distance = 1.0;
    controller->max_distance = 100.0;
    controller->pitch = 20.0;
    controller->orbit_sensitivity = 0.25;
    controller->zoom_sensitivity = 1.0;
    return controller;
}

/// @brief Get the orbit target Vec3 or Entity3D (NULL if invalid).
/// @param obj OrbitController runtime handle.
/// @return The validated retained target, or NULL.
void *rt_game3d_orbit_controller_get_target(void *obj) {
    rt_game3d_orbit_controller *controller = game3d_orbit_controller_checked(
        obj, "Game3D.OrbitController.get_target: invalid controller");
    return game3d_orbit_controller_target_ref(controller);
}

/// @brief Set the orbit target; traps on a non-Vec3/non-Entity3D.
/// @param obj OrbitController runtime handle.
/// @param target Vec3 world position or Entity3D to retain. Entity targets
///               owned by a different world are rejected.
void rt_game3d_orbit_controller_set_target(void *obj, void *target) {
    rt_game3d_orbit_controller *controller = game3d_orbit_controller_checked(
        obj, "Game3D.OrbitController.set_target: invalid controller");
    rt_game3d_entity *target_entity = NULL;
    if (!rt_g3d_is_vec3(target)) {
        target_entity = game3d_entity_checked(
            target, "Game3D.OrbitController.set_target: target must be Vec3 or Entity3D");
        if (!target_entity)
            return;
    }
    if (controller && target_entity) {
        rt_game3d_world *world = (rt_game3d_world *)rt_g3d_checked_or_null(
            controller->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
        if (!game3d_entity_validate_controller_world(
                target_entity,
                world,
                "Game3D.OrbitController.set_target: target belongs to another world"))
            return;
    }
    if (controller)
        game3d_assign_ref(&controller->target, target);
}

/// @brief Get the orbit distance (radius) in world units.
/// @param obj OrbitController runtime handle.
/// @return The positive distance clamped to the sanitized minimum/maximum
///         range, or zero when invalid.
double rt_game3d_orbit_controller_get_distance(void *obj) {
    rt_game3d_orbit_controller *controller = game3d_orbit_controller_checked(
        obj, "Game3D.OrbitController.get_distance: invalid controller");
    return game3d_orbit_controller_distance_value(controller);
}

/// @brief Set the orbit distance, clamped to the controller's min/max range.
/// @param obj OrbitController runtime handle.
/// @param distance Requested positive orbit radius in world units.
void rt_game3d_orbit_controller_set_distance(void *obj, double distance) {
    rt_game3d_orbit_controller *controller = game3d_orbit_controller_checked(
        obj, "Game3D.OrbitController.set_distance: invalid controller");
    if (controller) {
        double min_distance = 1.0;
        double max_distance = 100.0;
        game3d_orbit_controller_distance_range(controller, &min_distance, &max_distance);
        controller->distance = game3d_clamp(
            game3d_positive_clamped_or(distance, min_distance, RT_GAME3D_COORD_ABS_MAX),
            min_distance,
            max_distance);
    }
}

/// @brief Get the horizontal orbit angle (yaw) in degrees.
/// @param obj OrbitController runtime handle.
/// @return The finite bounded yaw angle, or zero when invalid.
double rt_game3d_orbit_controller_get_yaw(void *obj) {
    rt_game3d_orbit_controller *controller =
        game3d_orbit_controller_checked(obj, "Game3D.OrbitController.get_yaw: invalid controller");
    return controller ? game3d_clamp_abs_or(controller->yaw, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX)
                      : 0.0;
}

/// @brief Set the yaw angle in degrees (non-finite resets to 0).
/// @param obj OrbitController runtime handle.
/// @param yaw Requested horizontal angle in degrees.
void rt_game3d_orbit_controller_set_yaw(void *obj, double yaw) {
    rt_game3d_orbit_controller *controller =
        game3d_orbit_controller_checked(obj, "Game3D.OrbitController.set_yaw: invalid controller");
    if (controller)
        controller->yaw = game3d_clamp_abs_or(yaw, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
}

/// @brief Get the vertical orbit angle (pitch) in degrees.
/// @param obj OrbitController runtime handle.
/// @return The pitch clamped to [-85, 85] degrees, or zero when invalid.
double rt_game3d_orbit_controller_get_pitch(void *obj) {
    rt_game3d_orbit_controller *controller = game3d_orbit_controller_checked(
        obj, "Game3D.OrbitController.get_pitch: invalid controller");
    return controller ? game3d_clamp(controller->pitch, -85.0, 85.0) : 0.0;
}

/// @brief Set the pitch angle in degrees, clamped to [-85, 85] to avoid gimbal flip.
/// @param obj OrbitController runtime handle.
/// @param pitch Requested vertical angle in degrees.
void rt_game3d_orbit_controller_set_pitch(void *obj, double pitch) {
    rt_game3d_orbit_controller *controller = game3d_orbit_controller_checked(
        obj, "Game3D.OrbitController.set_pitch: invalid controller");
    if (controller)
        controller->pitch = game3d_clamp(pitch, -85.0, 85.0);
}

/// @brief Update orbit yaw/pitch from left-drag and distance from the wheel; clamps
///   pitch and distance to their ranges. See header.
/// @param obj OrbitController runtime handle.
/// @param world_obj World3D supplying the current input snapshot; it must match
///                  the retained world when one is bound.
/// @param dt Frame duration in seconds; unused because input deltas are snapshots.
void rt_game3d_orbit_controller_update(void *obj, void *world_obj, double dt) {
    (void)dt;
    rt_game3d_orbit_controller *controller =
        game3d_orbit_controller_checked(obj, "Game3D.OrbitController.update: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.OrbitController.update: invalid world");
    if (!controller || !world || !world->input)
        return;
    if (!game3d_camera_controller_validate_world(
            controller,
            world,
            "Game3D.OrbitController.update: controller belongs to another "
            "world"))
        return;
    double orbit_sensitivity =
        game3d_nonnegative_clamped_or(controller->orbit_sensitivity, 0.25, 1000.0);
    double zoom_sensitivity =
        game3d_nonnegative_clamped_or(controller->zoom_sensitivity, 1.0, RT_GAME3D_COORD_ABS_MAX);
    double min_distance = 1.0;
    double max_distance = 100.0;
    game3d_orbit_controller_distance_range(controller, &min_distance, &max_distance);
    controller->yaw = game3d_clamp_abs_or(controller->yaw, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
    controller->pitch = game3d_clamp(controller->pitch, -85.0, 85.0);
    if (rt_game3d_input_mouse_button(world->input, rt_game3d_mouse_left())) {
        controller->yaw = game3d_clamp_abs_or(
            controller->yaw +
                (double)game3d_input_mouse_dx((rt_game3d_input *)world->input) * orbit_sensitivity,
            0.0,
            RT_GAME3D_ANGLE_DEG_ABS_MAX);
        controller->pitch -=
            (double)game3d_input_mouse_dy((rt_game3d_input *)world->input) * orbit_sensitivity;
        controller->pitch = game3d_clamp(controller->pitch, -85.0, 85.0);
    }
    controller->distance -=
        game3d_input_wheel_y_snapshot((rt_game3d_input *)world->input) * zoom_sensitivity;
    controller->distance = game3d_clamp(controller->distance, min_distance, max_distance);
}

/// @brief After physics, reposition the camera on the orbit sphere around the target. See header.
/// @param obj OrbitController runtime handle.
/// @param world_obj World3D whose camera is placed around the target.
/// @param dt Frame duration in seconds; unused because placement is absolute.
void rt_game3d_orbit_controller_late_update(void *obj, void *world_obj, double dt) {
    (void)dt;
    rt_game3d_orbit_controller *controller = game3d_orbit_controller_checked(
        obj, "Game3D.OrbitController.lateUpdate: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.OrbitController.lateUpdate: invalid world");
    void *target = game3d_orbit_controller_target_ref(controller);
    if (!game3d_camera_controller_validate_world(
            controller,
            world,
            "Game3D.OrbitController.lateUpdate: controller belongs to another "
            "world"))
        return;
    if (controller && world && world->camera && target) {
        double target_pos[3];
        if (rt_g3d_has_class(target, RT_G3D_GAME3D_ENTITY_CLASS_ID)) {
            if (!game3d_entity_world_position_components((rt_game3d_entity *)target, target_pos))
                return;
        } else if (rt_g3d_is_vec3(target)) {
            target_pos[0] = rt_vec3_x(target);
            target_pos[1] = rt_vec3_y(target);
            target_pos[2] = rt_vec3_z(target);
        } else {
            return;
        }
        target_pos[0] = game3d_clamp_coord_or(target_pos[0], 0.0);
        target_pos[1] = game3d_clamp_coord_or(target_pos[1], 0.0);
        target_pos[2] = game3d_clamp_coord_or(target_pos[2], 0.0);
        rt_camera3d_orbit_components(
            world->camera,
            target_pos[0],
            target_pos[1],
            target_pos[2],
            game3d_orbit_controller_distance_value(controller),
            game3d_clamp_abs_or(controller->yaw, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX),
            game3d_clamp(controller->pitch, -85.0, 85.0));
    }
}

/// @brief GC finalizer for the follow controller: release its target and offset.
/// @param obj FollowController storage being finalized; NULL is ignored.
static void game3d_follow_controller_finalize(void *obj) {
    rt_game3d_follow_controller *controller = (rt_game3d_follow_controller *)obj;
    if (!controller)
        return;
    game3d_release_ref(&controller->world);
    game3d_release_ref(&controller->target_entity);
    game3d_release_ref(&controller->offset);
}

/// @brief Create a follow controller chasing `target_entity` at a Vec3 `offset`; traps
///   on a bad entity or non-Vec3 offset. See header.
/// @param world_obj World3D whose camera the controller positions.
/// @param target_entity Entity3D to follow; a spawned entity must belong to
///                      @p world_obj.
/// @param offset Vec3 world-space displacement from the target to the desired camera.
/// @return A newly allocated FollowController retaining the world, target, and
///         offset, or NULL after validation or allocation failure.
void *rt_game3d_follow_controller_new(void *world_obj, void *target_entity, void *offset) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.FollowController.New: invalid world");
    if (!game3d_entity_checked(target_entity,
                               "Game3D.FollowController.New: target must be Entity3D"))
        return NULL;
    if (!rt_g3d_is_vec3(offset)) {
        rt_trap("Game3D.FollowController.New: offset must be Vec3");
        return NULL;
    }
    if (!world)
        return NULL;
    if (!game3d_entity_validate_controller_world(
            (rt_game3d_entity *)target_entity,
            world,
            "Game3D.FollowController.New: target belongs to another world"))
        return NULL;
    rt_game3d_follow_controller *controller = (rt_game3d_follow_controller *)rt_obj_new_i64(
        RT_G3D_GAME3D_FOLLOW_CLASS_ID, (int64_t)sizeof(*controller));
    if (!controller) {
        rt_trap("Game3D.FollowController.New: allocation failed");
        return NULL;
    }
    memset(controller, 0, sizeof(*controller));
    rt_obj_set_finalizer(controller, game3d_follow_controller_finalize);
    game3d_assign_ref(&controller->world, world);
    game3d_assign_ref(&controller->target_entity, target_entity);
    game3d_assign_ref(&controller->offset, offset);
    controller->damping = RT_GAME3D_DEFAULT_FOLLOW_DAMPING;
    return controller;
}

/// @brief Get the followed entity (NULL if none/invalid).
/// @param obj FollowController runtime handle.
/// @return The live or recorded retained target Entity3D, or NULL.
void *rt_game3d_follow_controller_get_target(void *obj) {
    rt_game3d_follow_controller *controller = game3d_follow_controller_checked(
        obj, "Game3D.FollowController.get_target: invalid controller");
    return game3d_follow_controller_target_ref(controller);
}

/// @brief Set the followed entity (validated when non-NULL).
/// @param obj FollowController runtime handle.
/// @param target_entity Entity3D to retain, or NULL to clear the target. A
///                      spawned entity owned by a different world is rejected.
void rt_game3d_follow_controller_set_target(void *obj, void *target_entity) {
    rt_game3d_follow_controller *controller = game3d_follow_controller_checked(
        obj, "Game3D.FollowController.set_target: invalid controller");
    if (target_entity &&
        !game3d_entity_checked(target_entity,
                               "Game3D.FollowController.set_target: target must be Entity3D"))
        return;
    if (controller && target_entity) {
        rt_game3d_world *world = (rt_game3d_world *)rt_g3d_checked_or_null(
            controller->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
        if (!game3d_entity_validate_controller_world(
                (rt_game3d_entity *)target_entity,
                world,
                "Game3D.FollowController.set_target: target belongs to another world"))
            return;
    }
    if (controller)
        game3d_assign_ref(&controller->target_entity, target_entity);
}

/// @brief Get the follow offset Vec3 (NULL if invalid).
/// @param obj FollowController runtime handle.
/// @return The validated retained Vec3 offset, or NULL.
void *rt_game3d_follow_controller_get_offset(void *obj) {
    rt_game3d_follow_controller *controller = game3d_follow_controller_checked(
        obj, "Game3D.FollowController.get_offset: invalid controller");
    return game3d_follow_controller_offset_ref(controller);
}

/// @brief Set the follow offset; traps on a non-Vec3.
/// @param obj FollowController runtime handle.
/// @param offset Vec3 displacement to retain; NULL and non-Vec3 values trap.
void rt_game3d_follow_controller_set_offset(void *obj, void *offset) {
    rt_game3d_follow_controller *controller = game3d_follow_controller_checked(
        obj, "Game3D.FollowController.set_offset: invalid controller");
    if (!rt_g3d_is_vec3(offset)) {
        rt_trap("Game3D.FollowController.set_offset: offset must be Vec3");
        return;
    }
    if (controller)
        game3d_assign_ref(&controller->offset, offset);
}

/// @brief Get the position-smoothing damping factor.
/// @param obj FollowController runtime handle.
/// @return The finite non-negative bounded damping factor, or zero when invalid.
double rt_game3d_follow_controller_get_damping(void *obj) {
    rt_game3d_follow_controller *controller = game3d_follow_controller_checked(
        obj, "Game3D.FollowController.get_damping: invalid controller");
    return controller ? game3d_nonnegative_clamped_or(controller->damping,
                                                      RT_GAME3D_DEFAULT_FOLLOW_DAMPING,
                                                      RT_GAME3D_DAMPING_MAX)
                      : 0.0;
}

/// @brief Set the damping factor (negatives reset to the default).
/// @param obj FollowController runtime handle.
/// @param damping Requested exponential damping coefficient; invalid or
///                negative values select the default.
void rt_game3d_follow_controller_set_damping(void *obj, double damping) {
    rt_game3d_follow_controller *controller = game3d_follow_controller_checked(
        obj, "Game3D.FollowController.set_damping: invalid controller");
    if (controller)
        controller->damping = game3d_nonnegative_clamped_or(
            damping, RT_GAME3D_DEFAULT_FOLLOW_DAMPING, RT_GAME3D_DAMPING_MAX);
}

/// @brief No-op pre-physics update (follow runs in late update); validates handles. See header.
/// @param obj FollowController runtime handle.
/// @param world_obj World3D validated against the controller binding.
/// @param dt Frame duration in seconds; intentionally unused.
void rt_game3d_follow_controller_update(void *obj, void *world_obj, double dt) {
    (void)dt;
    rt_game3d_follow_controller *controller =
        game3d_follow_controller_checked(obj, "Game3D.FollowController.update: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.FollowController.update: invalid world");
    (void)game3d_camera_controller_validate_world(
        controller, world, "Game3D.FollowController.update: controller belongs to another world");
}

/// @brief After physics, exponentially damp the camera toward target+offset and look at
///   the target. The damping uses a frame-rate-independent `1 - exp(-damping·dt)` blend. See
///   header.
/// @param obj FollowController runtime handle.
/// @param world_obj World3D whose camera is positioned; it must match the
///                  retained world when one is bound.
/// @param dt Frame duration in seconds; sanitized before evaluating the
///           exponential blend.
void rt_game3d_follow_controller_late_update(void *obj, void *world_obj, double dt) {
    rt_game3d_follow_controller *controller = game3d_follow_controller_checked(
        obj, "Game3D.FollowController.lateUpdate: invalid controller");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.FollowController.lateUpdate: invalid world");
    rt_game3d_entity *target_entity = game3d_follow_controller_target_ref(controller);
    void *offset = game3d_follow_controller_offset_ref(controller);
    if (!controller || !world || !world->camera || !target_entity || !offset)
        return;
    if (!game3d_camera_controller_validate_world(
            controller,
            world,
            "Game3D.FollowController.lateUpdate: controller belongs to another "
            "world"))
        return;

    dt = game3d_clamp_dt(dt);
    double target_pos[3];
    double current[3];
    if (!game3d_entity_world_position_components(target_entity, target_pos))
        return;
    if (!rt_camera3d_get_position_components(world->camera, &current[0], &current[1], &current[2]))
        return;
    double target_x =
        game3d_clamp_coord_or(target_pos[0] + game3d_clamp_coord_or(rt_vec3_x(offset), 0.0), 0.0);
    double target_y =
        game3d_clamp_coord_or(target_pos[1] + game3d_clamp_coord_or(rt_vec3_y(offset), 0.0), 0.0);
    double target_z =
        game3d_clamp_coord_or(target_pos[2] + game3d_clamp_coord_or(rt_vec3_z(offset), 0.0), 0.0);
    double damping = game3d_nonnegative_clamped_or(
        controller->damping, RT_GAME3D_DEFAULT_FOLLOW_DAMPING, RT_GAME3D_DAMPING_MAX);
    double alpha = damping <= 0.0 ? 1.0 : 1.0 - exp(0.0 - damping * dt);
    alpha = game3d_clamp(alpha, 0.0, 1.0);
    current[0] = game3d_clamp_coord_or(current[0], target_x);
    current[1] = game3d_clamp_coord_or(current[1], target_y);
    current[2] = game3d_clamp_coord_or(current[2], target_z);
    double x = game3d_clamp_coord_or(current[0] + (target_x - current[0]) * alpha, target_x);
    double y = game3d_clamp_coord_or(current[1] + (target_y - current[1]) * alpha, target_y);
    double z = game3d_clamp_coord_or(current[2] + (target_z - current[2]) * alpha, target_z);
    rt_camera3d_look_at_components(
        world->camera, x, y, z, target_pos[0], target_pos[1], target_pos[2], 0.0, 1.0, 0.0);
}
