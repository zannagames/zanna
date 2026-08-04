//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_behavior.c
// Purpose: Zanna.Game3D.Behavior3D — composable preset behaviors that drive an
//   Entity3D each simulation step without per-entity script callbacks: spin,
//   orbit, sine float, face-target, chase (direct steer or via NavAgent3D),
//   follow-path, and lifetime despawn.
//
// Key invariants:
//   - A behavior is a flag set plus per-preset parameters; presets apply in a
//     fixed order (lifetime, path, chase, orbit, sine, spin, face) so composed
//     behaviors are deterministic.
//   - Behaviors mutate entities only through existing public entity/node/nav
//     APIs — no new mutation paths.
//   - Ticked from the world simulation step BEFORE physics and binding sync so
//     bodies and sockets see the behavior's writes in the same step.
//
// Ownership/Lifetime:
//   - Behavior3D is GC-managed; it retains its Path3D / NavAgent3D / target
//     Entity3D references and releases them in its finalizer.
//   - Entities retain an attached behavior via their `behavior` slot.
//
// Links: rt_game3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic, composable preset behaviors for Game3D entities.
/// @details Behavior3D stores a fixed flag set and parameters for spin, orbit, sine motion,
///   targeting, chase, path following, navigation, and lifetime. Presets execute in a stable order
///   before physics and binding synchronization so same-step consumers observe their mutations.

#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_navagent3d.h"
#include "rt_path3d.h"
#include "rt_scene3d.h"
#include "rt_trap.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_quat_new(double x, double y, double z, double w);
extern int8_t rt_navagent3d_get_has_target(void *agent);

#define BHV3D_PI 3.14159265358979323846
#define BHV3D_TAU (2.0 * BHV3D_PI)

enum {
    BHV3D_SPIN = 1 << 0,
    BHV3D_ORBIT = 1 << 1,
    BHV3D_SINE_FLOAT = 1 << 2,
    BHV3D_FACE_TARGET = 1 << 3,
    BHV3D_FOLLOW_PATH = 1 << 4,
    BHV3D_CHASE = 1 << 5,
    BHV3D_LIFETIME = 1 << 6,
    BHV3D_DESPAWN_TARGET = 1 << 7,
    BHV3D_ALL = BHV3D_SPIN | BHV3D_ORBIT | BHV3D_SINE_FLOAT | BHV3D_FACE_TARGET |
                BHV3D_FOLLOW_PATH | BHV3D_CHASE | BHV3D_LIFETIME | BHV3D_DESPAWN_TARGET,
};

/// @brief Behavior3D payload: preset flag set plus per-preset parameters and
///   retained collaborator references.
typedef struct rt_game3d_behavior {
    void *vptr;
    uint32_t flags;

    /* Spin */
    double spin_axis[3];
    double spin_deg_per_sec;
    double spin_angle;

    /* Orbit */
    double orbit_center[3];
    double orbit_radius;
    double orbit_deg_per_sec;
    double orbit_angle;

    /* Sine float */
    double sine_amplitude;
    double sine_speed;
    double sine_phase;
    double sine_base_y;
    int8_t sine_base_captured;

    /* Face target / chase */
    void *target_entity; /* retained Entity3D */
    double chase_speed;
    double chase_range;

    /* Follow path */
    void *path; /* retained Path3D */
    double path_speed;
    double path_distance;
    int8_t path_loop;

    /* Chase via navigation */
    void *nav_agent; /* retained NavAgent3D */
    double nav_target[3];
    int8_t nav_target_valid;

    /* Lifetime */
    double lifetime_remaining;

    /* Despawn target (C-internal trap/test preset): despawned once on the
     * next update — exercises cross-entity registry compaction mid-sweep. */
    void *despawn_target; /* retained Entity3D */
} rt_game3d_behavior;

/// @brief Normalize a finite vector without overflow from squaring large lanes.
/// @param x Input/output X component.
/// @param y Input/output Y component.
/// @param z Input/output Z component.
/// @return Nonzero for a usable non-zero vector, otherwise zero without writes.
static int behavior_normalize3(double *x, double *y, double *z) {
    double length;
    if (!x || !y || !z || !isfinite(*x) || !isfinite(*y) || !isfinite(*z))
        return 0;
    length = hypot(hypot(*x, *y), *z);
    if (!isfinite(length) || length < 1e-9)
        return 0;
    *x /= length;
    *y /= length;
    *z /= length;
    return 1;
}

/// @brief Advance and wrap an angular accumulator without allowing corrupt state to propagate.
/// @param phase Current angle in radians.
/// @param radians_per_second Bounded signed angular rate.
/// @param dt Bounded non-negative step duration.
/// @return Finite angle in `(-2*pi, 2*pi)`.
static double behavior_advance_phase(double phase, double radians_per_second, double dt) {
    if (!isfinite(phase))
        phase = 0.0;
    radians_per_second = game3d_clamp_abs_or(
        radians_per_second, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX * (BHV3D_PI / 180.0));
    dt = game3d_clamp_controller_dt(dt);
    phase = fmod(phase + radians_per_second * dt, BHV3D_TAU);
    return isfinite(phase) ? phase : 0.0;
}

/// @brief Write a local quaternion from raw components without allocating Vec3/Quat boxes.
/// @param node Scene node receiving the local orientation.
/// @param qx Quaternion X component.
/// @param qy Quaternion Y component.
/// @param qz Quaternion Z component.
/// @param qw Quaternion scalar component.
static void behavior_set_local_rotation(void *node, double qx, double qy, double qz, double qw) {
    double position[3];
    double scale[3];
    if (!rt_scene_node3d_get_position_components(node, &position[0], &position[1], &position[2]) ||
        !rt_scene_node3d_get_scale_components(node, &scale[0], &scale[1], &scale[2]))
        return;
    rt_scene_node3d_set_transform(
        node, position[0], position[1], position[2], qx, qy, qz, qw, scale[0], scale[1], scale[2]);
}

/// @brief Class-checked cast to a Behavior3D (traps with @p method on mismatch).
/// @param obj Candidate Behavior3D handle.
/// @param method Trap message emitted when @p obj has the wrong runtime class.
/// @return The validated behavior, or NULL after reporting an invalid handle.
static rt_game3d_behavior *game3d_behavior_checked(void *obj, const char *method) {
    rt_game3d_behavior *behavior =
        (rt_game3d_behavior *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_BEHAVIOR3D_CLASS_ID);
    if (!behavior)
        rt_trap(method);
    return behavior;
}

/// @brief GC finalizer: release retained collaborator references.
/// @param obj Behavior3D allocation being finalized.
static void game3d_behavior_finalize(void *obj) {
    rt_game3d_behavior *behavior = (rt_game3d_behavior *)obj;
    if (!behavior)
        return;
    game3d_release_ref(&behavior->target_entity);
    game3d_release_ref(&behavior->path);
    game3d_release_ref(&behavior->nav_agent);
    game3d_release_ref(&behavior->despawn_target);
}

/// @brief Create an empty behavior; add presets with the fluent Add* methods.
/// @return A new zero-initialized Behavior3D, or NULL when allocation fails.
void *rt_game3d_behavior_new(void) {
    rt_game3d_behavior *behavior = (rt_game3d_behavior *)rt_obj_new_i64(
        RT_G3D_GAME3D_BEHAVIOR3D_CLASS_ID, (int64_t)sizeof(rt_game3d_behavior));
    if (!behavior) {
        rt_trap("Game3D.Behavior3D.New: memory allocation failed");
        return NULL;
    }
    memset(&behavior->flags, 0, sizeof(*behavior) - offsetof(rt_game3d_behavior, flags));
    behavior->vptr = NULL;
    rt_obj_set_finalizer(behavior, game3d_behavior_finalize);
    return behavior;
}

/// @brief Fluent: continuous rotation about an axis at @p deg_per_sec.
/// @param obj Behavior3D to configure.
/// @param axis_x X component of the non-zero rotation axis.
/// @param axis_y Y component of the non-zero rotation axis.
/// @param axis_z Z component of the non-zero rotation axis.
/// @param deg_per_sec Signed angular velocity in degrees per second.
/// @return @p obj for fluent chaining.
void *rt_game3d_behavior_add_spin(
    void *obj, double axis_x, double axis_y, double axis_z, double deg_per_sec) {
    rt_game3d_behavior *behavior =
        game3d_behavior_checked(obj, "Game3D.Behavior3D.addSpin: invalid behavior");
    if (!behavior)
        return obj;
    if (!isfinite(axis_x) || !isfinite(axis_y) || !isfinite(axis_z) || !isfinite(deg_per_sec)) {
        rt_trap("Game3D.Behavior3D.addSpin: parameters must be finite");
        return obj;
    }
    if (!behavior_normalize3(&axis_x, &axis_y, &axis_z)) {
        rt_trap("Game3D.Behavior3D.addSpin: axis must be non-zero");
        return obj;
    }
    behavior->spin_axis[0] = axis_x;
    behavior->spin_axis[1] = axis_y;
    behavior->spin_axis[2] = axis_z;
    behavior->spin_deg_per_sec = game3d_clamp_abs_or(deg_per_sec, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
    behavior->spin_angle = 0.0;
    behavior->flags |= BHV3D_SPIN;
    return obj;
}

/// @brief Fluent: circular orbit in the XZ plane around a world-space center.
/// @param obj Behavior3D to configure.
/// @param center_x World-space X coordinate of the orbit center.
/// @param center_y World-space Y coordinate retained throughout the orbit.
/// @param center_z World-space Z coordinate of the orbit center.
/// @param radius Non-negative orbit radius in world units.
/// @param deg_per_sec Signed angular velocity in degrees per second.
/// @return @p obj for fluent chaining.
void *rt_game3d_behavior_add_orbit(void *obj,
                                   double center_x,
                                   double center_y,
                                   double center_z,
                                   double radius,
                                   double deg_per_sec) {
    rt_game3d_behavior *behavior =
        game3d_behavior_checked(obj, "Game3D.Behavior3D.addOrbit: invalid behavior");
    if (!behavior)
        return obj;
    if (!isfinite(center_x) || !isfinite(center_y) || !isfinite(center_z) || !isfinite(radius) ||
        !isfinite(deg_per_sec) || radius < 0.0) {
        rt_trap("Game3D.Behavior3D.addOrbit: parameters must be finite (radius >= 0)");
        return obj;
    }
    behavior->orbit_center[0] = game3d_clamp_coord_or(center_x, 0.0);
    behavior->orbit_center[1] = game3d_clamp_coord_or(center_y, 0.0);
    behavior->orbit_center[2] = game3d_clamp_coord_or(center_z, 0.0);
    behavior->orbit_radius = game3d_nonnegative_clamped_or(radius, 0.0, RT_GAME3D_COORD_ABS_MAX);
    behavior->orbit_deg_per_sec =
        game3d_clamp_abs_or(deg_per_sec, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
    behavior->orbit_angle = 0.0;
    behavior->flags |= BHV3D_ORBIT;
    return obj;
}

/// @brief Fluent: vertical sine bobbing around the entity's height at attach time.
/// @param obj Behavior3D to configure.
/// @param amplitude Signed vertical displacement amplitude in local units.
/// @param speed Phase advance in radians per second.
/// @return @p obj for fluent chaining.
void *rt_game3d_behavior_add_sine_float(void *obj, double amplitude, double speed) {
    rt_game3d_behavior *behavior =
        game3d_behavior_checked(obj, "Game3D.Behavior3D.addSineFloat: invalid behavior");
    if (!behavior)
        return obj;
    if (!isfinite(amplitude) || !isfinite(speed)) {
        rt_trap("Game3D.Behavior3D.addSineFloat: parameters must be finite");
        return obj;
    }
    behavior->sine_amplitude = game3d_clamp_abs_or(amplitude, 0.0, RT_GAME3D_COORD_ABS_MAX);
    behavior->sine_speed = game3d_clamp_abs_or(speed, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX);
    behavior->sine_phase = 0.0;
    behavior->sine_base_captured = 0;
    behavior->flags |= BHV3D_SINE_FLOAT;
    return obj;
}

/// @brief Fluent: yaw the entity so its forward (-Z) axis points at the target entity.
/// @param obj Behavior3D to configure.
/// @param target_entity Entity3D whose live world position drives the yaw.
/// @return @p obj for fluent chaining.
void *rt_game3d_behavior_add_face_target(void *obj, void *target_entity) {
    rt_game3d_behavior *behavior =
        game3d_behavior_checked(obj, "Game3D.Behavior3D.addFaceTarget: invalid behavior");
    if (!behavior)
        return obj;
    if (!target_entity || !rt_g3d_has_class(target_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID)) {
        rt_trap("Game3D.Behavior3D.addFaceTarget: target must be an Entity3D");
        return obj;
    }
    game3d_assign_ref(&behavior->target_entity, target_entity);
    behavior->flags |= BHV3D_FACE_TARGET;
    return obj;
}

/// @brief C-internal fluent preset: despawn @p target_entity on this
///   behavior's next update (one-shot).
/// @details Not part of the script-visible registry. Exists so tests (and
///   trap-volume style C fixtures) can exercise cross-entity registry
///   compaction while an entity sweep is in flight — the failure mode the
///   stamped sweep in game3d_world_sweep_entities guards against.
/// @param obj Behavior3D to configure.
/// @param target_entity Entity3D to despawn once on the next behavior update.
/// @return @p obj for fluent chaining.
void *rt_game3d_behavior_add_despawn_target_internal(void *obj, void *target_entity) {
    rt_game3d_behavior *behavior =
        game3d_behavior_checked(obj, "Game3D.Behavior3D.addDespawnTarget: invalid behavior");
    if (!behavior)
        return obj;
    if (!target_entity || !rt_g3d_has_class(target_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID)) {
        rt_trap("Game3D.Behavior3D.addDespawnTarget: target must be an Entity3D");
        return obj;
    }
    game3d_assign_ref(&behavior->despawn_target, target_entity);
    behavior->flags |= BHV3D_DESPAWN_TARGET;
    return obj;
}

/// @brief Fluent: move toward the target entity, stopping inside @p range.
/// @details With a bound NavAgent3D (SetNavAgent) the chase routes through the
///   navigation mesh; otherwise the entity steers straight toward the target
///   in the XZ plane at @p speed.
/// @param obj Behavior3D to configure.
/// @param target_entity Entity3D whose live world position is chased.
/// @param speed Non-negative movement speed in world units per second.
/// @param range Non-negative stopping radius around the target.
/// @return @p obj for fluent chaining.
void *rt_game3d_behavior_add_chase(void *obj, void *target_entity, double speed, double range) {
    rt_game3d_behavior *behavior =
        game3d_behavior_checked(obj, "Game3D.Behavior3D.addChase: invalid behavior");
    if (!behavior)
        return obj;
    if (!target_entity || !rt_g3d_has_class(target_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID)) {
        rt_trap("Game3D.Behavior3D.addChase: target must be an Entity3D");
        return obj;
    }
    if (!isfinite(speed) || !isfinite(range) || speed < 0.0 || range < 0.0) {
        rt_trap("Game3D.Behavior3D.addChase: speed and range must be finite and >= 0");
        return obj;
    }
    game3d_assign_ref(&behavior->target_entity, target_entity);
    behavior->chase_speed =
        game3d_nonnegative_clamped_or(speed, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX);
    behavior->chase_range = game3d_nonnegative_clamped_or(range, 0.0, RT_GAME3D_COORD_ABS_MAX);
    behavior->flags |= BHV3D_CHASE;
    return obj;
}

/// @brief Fluent: follow a Path3D at constant speed (looping or one-shot).
/// @param obj Behavior3D to configure.
/// @param path Path3D retained as the world-space trajectory.
/// @param speed Non-negative distance advanced per simulation second.
/// @param loop Non-zero to wrap at the path length; zero to stop at the end.
/// @return @p obj for fluent chaining.
void *rt_game3d_behavior_add_follow_path(void *obj, void *path, double speed, int8_t loop) {
    rt_game3d_behavior *behavior =
        game3d_behavior_checked(obj, "Game3D.Behavior3D.addFollowPath: invalid behavior");
    if (!behavior)
        return obj;
    if (!path || !rt_g3d_has_class(path, RT_G3D_PATH3D_CLASS_ID)) {
        rt_trap("Game3D.Behavior3D.addFollowPath: path must be a Path3D");
        return obj;
    }
    if (!isfinite(speed) || speed < 0.0) {
        rt_trap("Game3D.Behavior3D.addFollowPath: speed must be finite and >= 0");
        return obj;
    }
    game3d_assign_ref(&behavior->path, path);
    behavior->path_speed =
        game3d_nonnegative_clamped_or(speed, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX);
    behavior->path_distance = 0.0;
    behavior->path_loop = loop ? 1 : 0;
    behavior->flags |= BHV3D_FOLLOW_PATH;
    return obj;
}

/// @brief Fluent: despawn the entity after @p seconds of simulation time.
/// @param obj Behavior3D to configure.
/// @param seconds Non-negative simulated lifetime.
/// @return @p obj for fluent chaining.
void *rt_game3d_behavior_add_lifetime(void *obj, double seconds) {
    rt_game3d_behavior *behavior =
        game3d_behavior_checked(obj, "Game3D.Behavior3D.addLifetime: invalid behavior");
    if (!behavior)
        return obj;
    if (!isfinite(seconds) || seconds < 0.0) {
        rt_trap("Game3D.Behavior3D.addLifetime: seconds must be finite and >= 0");
        return obj;
    }
    behavior->lifetime_remaining =
        game3d_nonnegative_clamped_or(seconds, 0.0, RT_GAME3D_COORD_ABS_MAX);
    behavior->flags |= BHV3D_LIFETIME;
    return obj;
}

/// @brief Fluent: route chase movement through a NavAgent3D instead of direct steering.
/// @param obj Behavior3D to configure.
/// @param agent NavAgent3D to retain, or NULL to restore direct XZ steering.
/// @return @p obj for fluent chaining.
void *rt_game3d_behavior_set_nav_agent(void *obj, void *agent) {
    rt_game3d_behavior *behavior =
        game3d_behavior_checked(obj, "Game3D.Behavior3D.setNavAgent: invalid behavior");
    if (!behavior)
        return obj;
    if (agent && !rt_g3d_has_class(agent, RT_G3D_NAVAGENT3D_CLASS_ID)) {
        rt_trap("Game3D.Behavior3D.setNavAgent: expected NavAgent3D or null");
        return obj;
    }
    game3d_assign_ref(&behavior->nav_agent, agent);
    behavior->nav_target_valid = 0;
    return obj;
}

/// @brief Read the target entity's world position (returns 0 when unavailable).
/// @param behavior Behavior3D containing the retained target entity.
/// @param out Three-component output receiving the live world position.
/// @return 1 when a live target node supplied a position, or 0 otherwise.
static int behavior_target_world_pos(rt_game3d_behavior *behavior, double out[3]) {
    rt_game3d_entity *target = (rt_game3d_entity *)rt_g3d_checked_or_null(
        behavior->target_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    void *node = target && target->alive ? game3d_entity_node_ref(target) : NULL;
    if (!node)
        return 0;
    if (!rt_scene_node3d_get_world_position_components(node, &out[0], &out[1], &out[2]))
        return 0;
    out[0] = game3d_clamp_coord_or(out[0], 0.0);
    out[1] = game3d_clamp_coord_or(out[1], 0.0);
    out[2] = game3d_clamp_coord_or(out[2], 0.0);
    return 1;
}

/// @brief Advance one behavior for one entity by @p dt seconds.
/// @details Preset order is fixed: lifetime, follow-path, chase, orbit, sine
///   float, spin, face-target. Position-writing presets are exclusive in
///   practice (path/chase/orbit all set position; last writer wins in that
///   fixed order), while spin/face write rotation only.
/// @param behavior_obj Behavior3D containing the presets to advance.
/// @param entity_obj Live Entity3D receiving the resulting transforms or despawn.
/// @param dt Non-negative finite simulation interval in seconds.
void rt_game3d_behavior_update(void *behavior_obj, void *entity_obj, double dt) {
    rt_game3d_behavior *behavior = (rt_game3d_behavior *)rt_g3d_checked_or_null(
        behavior_obj, RT_G3D_GAME3D_BEHAVIOR3D_CLASS_ID);
    rt_game3d_entity *entity =
        (rt_game3d_entity *)rt_g3d_checked_or_null(entity_obj, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    void *node;

    if (!behavior || !entity || !entity->alive || !isfinite(dt) || dt < 0.0)
        return;
    dt = game3d_clamp_controller_dt(dt);
    behavior->flags &= (uint32_t)BHV3D_ALL;
    node = game3d_entity_node_ref(entity);
    if (!node)
        return;

    if (behavior->flags & BHV3D_DESPAWN_TARGET) {
        void *victim =
            rt_g3d_checked_or_null(behavior->despawn_target, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        behavior->flags &= ~(uint32_t)BHV3D_DESPAWN_TARGET;
        if (victim) {
            rt_game3d_entity *victim_entity = (rt_game3d_entity *)victim;
            if (victim_entity->spawned && victim_entity->world)
                rt_game3d_world_despawn(victim_entity->world, victim);
        }
        game3d_release_ref(&behavior->despawn_target);
    }

    if (behavior->flags & BHV3D_LIFETIME) {
        behavior->lifetime_remaining = game3d_nonnegative_clamped_or(
            behavior->lifetime_remaining, 0.0, RT_GAME3D_COORD_ABS_MAX);
        behavior->lifetime_remaining -= dt;
        if (behavior->lifetime_remaining <= 0.0) {
            if (entity->spawned && entity->world)
                rt_game3d_world_despawn(entity->world, entity_obj);
            return;
        }
    }

    if (behavior->flags & BHV3D_FOLLOW_PATH) {
        void *path = rt_g3d_checked_or_null(behavior->path, RT_G3D_PATH3D_CLASS_ID);
        double length = path ? rt_path3d_get_length(path) : 0.0;
        if (path && isfinite(length) && length > 1e-9) {
            double normalized_distance;
            double world_pos[3];
            behavior->path_speed = game3d_nonnegative_clamped_or(
                behavior->path_speed, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX);
            behavior->path_distance = game3d_nonnegative_clamped_or(
                behavior->path_distance, 0.0, RT_GAME3D_COORD_ABS_MAX);
            behavior->path_distance += behavior->path_speed * dt;
            if (behavior->path_loop) {
                behavior->path_distance = fmod(behavior->path_distance, length);
            } else if (behavior->path_distance > length) {
                behavior->path_distance = length;
            }
            if (!isfinite(behavior->path_distance) || behavior->path_distance < 0.0)
                behavior->path_distance = 0.0;
            normalized_distance = behavior->path_distance / length;
            /* The internal raw evaluator maps normalized arc length to the
             * spline parameter, preserving world-units-per-second semantics
             * without allocating a temporary Vec3 every simulation step. */
            rt_path3d_eval_spline_raw(path, normalized_distance, world_pos, NULL);
            world_pos[0] = game3d_clamp_coord_or(world_pos[0], 0.0);
            world_pos[1] = game3d_clamp_coord_or(world_pos[1], 0.0);
            world_pos[2] = game3d_clamp_coord_or(world_pos[2], 0.0);
            game3d_set_node_world_position(node, world_pos);
        }
    }

    if (behavior->flags & BHV3D_CHASE) {
        double target[3];
        if (behavior_target_world_pos(behavior, target)) {
            void *agent = rt_g3d_checked_or_null(behavior->nav_agent, RT_G3D_NAVAGENT3D_CLASS_ID);
            if (agent) {
                double dx = target[0] - behavior->nav_target[0];
                double dy = target[1] - behavior->nav_target[1];
                double dz = target[2] - behavior->nav_target[2];
                if (!behavior->nav_target_valid || !rt_navagent3d_get_has_target(agent) ||
                    fabs(dx) > 1e-6 || fabs(dy) > 1e-6 || fabs(dz) > 1e-6) {
                    void *target_vec = rt_vec3_new(target[0], target[1], target[2]);
                    if (target_vec) {
                        rt_navagent3d_set_target(agent, target_vec);
                        behavior->nav_target[0] = target[0];
                        behavior->nav_target[1] = target[1];
                        behavior->nav_target[2] = target[2];
                        behavior->nav_target_valid = 1;
                    }
                    game3d_release_ref(&target_vec);
                }
            } else {
                double pos[3];
                behavior->nav_target_valid = 0;
                if (rt_scene_node3d_get_world_position_components(
                        node, &pos[0], &pos[1], &pos[2])) {
                    pos[0] = game3d_clamp_coord_or(pos[0], 0.0);
                    pos[1] = game3d_clamp_coord_or(pos[1], 0.0);
                    pos[2] = game3d_clamp_coord_or(pos[2], 0.0);
                    double dx = target[0] - pos[0];
                    double dz = target[2] - pos[2];
                    double dist = hypot(dx, dz);
                    behavior->chase_range = game3d_nonnegative_clamped_or(
                        behavior->chase_range, 0.0, RT_GAME3D_COORD_ABS_MAX);
                    behavior->chase_speed = game3d_nonnegative_clamped_or(
                        behavior->chase_speed, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX);
                    if (dist > behavior->chase_range && dist > 1e-9) {
                        double step = behavior->chase_speed * dt;
                        if (step > dist - behavior->chase_range)
                            step = dist - behavior->chase_range;
                        /* Step in world space (pos is world) and write through the
                         * parent frame; the old code mixed a local read with a world
                         * delta, drifting parented entities. */
                        double world_pos[3] = {
                            game3d_clamp_coord_or(pos[0] + dx / dist * step, pos[0]),
                            pos[1],
                            game3d_clamp_coord_or(pos[2] + dz / dist * step, pos[2])};
                        game3d_set_node_world_position(node, world_pos);
                    }
                }
            }
        }
    }

    if (behavior->flags & BHV3D_ORBIT) {
        behavior->orbit_center[0] = game3d_clamp_coord_or(behavior->orbit_center[0], 0.0);
        behavior->orbit_center[1] = game3d_clamp_coord_or(behavior->orbit_center[1], 0.0);
        behavior->orbit_center[2] = game3d_clamp_coord_or(behavior->orbit_center[2], 0.0);
        behavior->orbit_radius =
            game3d_nonnegative_clamped_or(behavior->orbit_radius, 0.0, RT_GAME3D_COORD_ABS_MAX);
        behavior->orbit_deg_per_sec =
            game3d_clamp_abs_or(behavior->orbit_deg_per_sec, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
        behavior->orbit_angle = behavior_advance_phase(
            behavior->orbit_angle, behavior->orbit_deg_per_sec * (BHV3D_PI / 180.0), dt);
        /* orbit_center is world-space; write through the parent frame. */
        double world_pos[3] = {
            game3d_clamp_coord_or(behavior->orbit_center[0] +
                                      cos(behavior->orbit_angle) * behavior->orbit_radius,
                                  behavior->orbit_center[0]),
            behavior->orbit_center[1],
            game3d_clamp_coord_or(behavior->orbit_center[2] +
                                      sin(behavior->orbit_angle) * behavior->orbit_radius,
                                  behavior->orbit_center[2])};
        game3d_set_node_world_position(node, world_pos);
    }

    if (behavior->flags & BHV3D_SINE_FLOAT) {
        double local[3];
        if (rt_scene_node3d_get_position_components(node, &local[0], &local[1], &local[2])) {
            if (!behavior->sine_base_captured) {
                behavior->sine_base_y = game3d_clamp_coord_or(local[1], 0.0);
                behavior->sine_base_captured = 1;
            }
            behavior->sine_base_y = game3d_clamp_coord_or(behavior->sine_base_y, local[1]);
            behavior->sine_amplitude =
                game3d_clamp_abs_or(behavior->sine_amplitude, 0.0, RT_GAME3D_COORD_ABS_MAX);
            behavior->sine_speed =
                game3d_clamp_abs_or(behavior->sine_speed, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX);
            behavior->sine_phase =
                behavior_advance_phase(behavior->sine_phase, behavior->sine_speed, dt);
            rt_scene_node3d_set_position(
                node,
                game3d_clamp_coord_or(local[0], 0.0),
                game3d_clamp_coord_or(behavior->sine_base_y +
                                          sin(behavior->sine_phase) * behavior->sine_amplitude,
                                      behavior->sine_base_y),
                game3d_clamp_coord_or(local[2], 0.0));
        }
    }

    if (behavior->flags & BHV3D_SPIN) {
        double half_angle;
        double sine;
        if (!behavior_normalize3(
                &behavior->spin_axis[0], &behavior->spin_axis[1], &behavior->spin_axis[2])) {
            behavior->spin_axis[0] = 0.0;
            behavior->spin_axis[1] = 1.0;
            behavior->spin_axis[2] = 0.0;
        }
        behavior->spin_deg_per_sec =
            game3d_clamp_abs_or(behavior->spin_deg_per_sec, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
        behavior->spin_angle = behavior_advance_phase(
            behavior->spin_angle, behavior->spin_deg_per_sec * (BHV3D_PI / 180.0), dt);
        half_angle = behavior->spin_angle * 0.5;
        sine = sin(half_angle);
        behavior_set_local_rotation(node,
                                    behavior->spin_axis[0] * sine,
                                    behavior->spin_axis[1] * sine,
                                    behavior->spin_axis[2] * sine,
                                    cos(half_angle));
    }

    if (behavior->flags & BHV3D_FACE_TARGET) {
        double target[3];
        double pos[3];
        if (behavior_target_world_pos(behavior, target) &&
            rt_scene_node3d_get_world_position_components(node, &pos[0], &pos[1], &pos[2])) {
            pos[0] = game3d_clamp_coord_or(pos[0], 0.0);
            pos[2] = game3d_clamp_coord_or(pos[2], 0.0);
            double dx = target[0] - pos[0];
            double dz = target[2] - pos[2];
            if (hypot(dx, dz) > 1e-6) {
                /* Identity forward is -Z; yaw about +Y so -Z points at the target. */
                double yaw = atan2(-dx, -dz);
                void *quat = rt_quat_new(0.0, sin(yaw * 0.5), 0.0, cos(yaw * 0.5));
                if (quat)
                    game3d_set_node_world_rotation(node, quat); /* world yaw → parent-local */
                game3d_release_ref(&quat);
            }
        }
    }
}
