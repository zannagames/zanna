//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_joints3d.c
// Purpose: 3D physics joint constraints. Distance joints maintain fixed
//   separation via positional correction. Spring joints apply Hooke's law
//   forces with damping. Both operate on Body3D position/velocity directly.
//
// Key invariants:
//   - Distance joint: positional correction pushes bodies to target distance.
//   - Spring joint: force = -stiffness * (dist - rest) - damping * rel_vel.
//   - Both handle zero-distance edge case (coincident centers).
//   - Joints with NULL or non-finite body references are no-ops.
//
// Ownership/Lifetime:
//   - GC-managed via rt_obj_new_i64.
//   - Body references are retained while the joint exists and released by the
//     joint finalizer, so worlds can keep solving after caller locals release.
//
// Links: rt_joints3d.h, rt_physics3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_joints3d.c
 * @brief Implements distance, spring, hinge, rope, and six-degree-of-freedom joints.
 *
 * Each runtime-managed joint retains two Body3D objects and applies a bounded,
 * finite-state constraint during world stepping. The solvers share body
 * validation and math helpers, distribute corrections by inverse mass or
 * effective inertia, and expose a common dispatch surface to Physics3D.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_joints3d.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_joints3d_internal.h"
#include "rt_mat4.h"
#include "rt_physics3d.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
#include "rt_trap.h"

/// @brief True if a body view is solvable: finite, non-negative inverse mass and
///        finite position/velocity. Guards the joint solver against NaN bodies.
/// @param body Borrowed Body3D kinematic prefix to validate.
/// @return One when all solver-consumed state is finite and inverse mass is non-negative.
int joint3d_body_is_finite(const rt_body3d_kinematics *body) {
    if (!body || !isfinite(body->inv_mass) || body->inv_mass < 0.0)
        return 0;
    for (int i = 0; i < 3; i++) {
        if (!isfinite(body->position[i]) || !isfinite(body->velocity[i]) ||
            !isfinite(body->orientation[i]) || !isfinite(body->angular_velocity[i]))
            return 0;
    }
    if (!isfinite(body->orientation[3]))
        return 0;
    return 1;
}

/// @brief Release the GC reference held in `*slot` (if any) and null the slot. Idempotent.
/// @param slot Address of a retained Body3D pointer slot.
static void joint3d_release_body_ref(rt_body3d_kinematics **slot) {
    rt_g3d_ref_slot_release_class((void **)slot, RT_G3D_BODY3D_CLASS_ID);
}

/*==========================================================================
 * Distance Joint
 *=========================================================================*/

typedef struct {
    void *vptr;
    rt_body3d_kinematics *body_a;
    rt_body3d_kinematics *body_b;
    double target_distance;
} rt_distance_joint3d;

/// @brief GC finalizer — release the bodies retained by this distance joint.
/// @param obj DistanceJoint3D payload being finalized.
static void distance_joint_finalizer(void *obj) {
    rt_distance_joint3d *j = (rt_distance_joint3d *)obj;
    if (!j)
        return;
    joint3d_release_body_ref(&j->body_a);
    joint3d_release_body_ref(&j->body_b);
}

/// @brief Create a distance joint that constrains two bodies to a fixed separation.
/// @details The joint applies positional correction and velocity damping each
///          physics step to maintain the target distance. Both bodies must be
///          non-null. If both are static (zero inverse mass), the joint is inert.
/// @param body_a   First body handle.
/// @param body_b   Second body handle.
/// @param distance Target separation distance in world units.
/// @return Opaque joint handle, or NULL on failure.
void *rt_distance_joint3d_new(void *body_a, void *body_b, double distance) {
    if (!rt_g3d_has_class(body_a, RT_G3D_BODY3D_CLASS_ID) ||
        !rt_g3d_has_class(body_b, RT_G3D_BODY3D_CLASS_ID)) {
        rt_trap("DistanceJoint3D.New: both bodies must be non-null");
        return NULL;
    }
    rt_distance_joint3d *j = (rt_distance_joint3d *)rt_obj_new_i64(
        RT_G3D_DISTANCEJOINT3D_CLASS_ID, (int64_t)sizeof(rt_distance_joint3d));
    if (!j) {
        rt_trap("DistanceJoint3D.New: allocation failed");
        return NULL;
    }
    j->vptr = NULL;
    j->body_a = (rt_body3d_kinematics *)body_a;
    j->body_b = (rt_body3d_kinematics *)body_b;
    rt_obj_retain_maybe(body_a);
    rt_obj_retain_maybe(body_b);
    j->target_distance = joint3d_sanitize_nonnegative(distance);
    rt_obj_set_finalizer(j, distance_joint_finalizer);
    return j;
}

/// @brief Get the target distance of a distance joint.
/// @param joint DistanceJoint3D handle to inspect.
/// @return Sanitized target separation, or zero for an invalid handle.
double rt_distance_joint3d_get_distance(void *joint) {
    rt_distance_joint3d *j =
        (rt_distance_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_DISTANCEJOINT3D_CLASS_ID);
    return j ? j->target_distance : 0;
}

/// @brief Change the target distance of a distance joint at runtime.
/// @param joint DistanceJoint3D handle to modify.
/// @param distance Requested target separation, sanitized to a non-negative finite value.
void rt_distance_joint3d_set_distance(void *joint, double distance) {
    rt_distance_joint3d *j =
        (rt_distance_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_DISTANCEJOINT3D_CLASS_ID);
    if (j)
        j->target_distance = joint3d_sanitize_nonnegative(distance);
}

/// @brief Enforce a rigid distance constraint between two bodies for one step.
/// @details Implements a two-pass hard constraint: a position correction that
///   teleports the bodies so their separation matches `target_distance`, then
///   a velocity correction that zeros out the relative velocity along the
///   constraint axis so the bodies don't immediately separate again next
///   frame. Both corrections are mass-weighted by inverse mass — infinite-mass
///   (static) bodies contribute zero to the split, so a dynamic body hitched
///   to a static one moves the full correction distance itself. `dt` is
///   currently unused because this is a position-based constraint; leaving
///   it in the signature keeps parity with `solve_spring` and reserves it
///   for future XPBD-style step-size-aware variants.
///   Early-outs: coincident bodies (no defined direction) and two static
///   bodies (both inv_mass = 0) both skip the step cleanly.
/// @param j Distance joint constraint to solve.
/// @param dt Physics step duration, reserved for future step-size-aware variants.
static void solve_distance(rt_distance_joint3d *j, double dt) {
    double delta[3];
    if (!j || !joint3d_body_is_finite(j->body_a) || !joint3d_body_is_finite(j->body_b))
        return;
    (void)dt;

    joint3d_vec3_sub(j->body_b->position, j->body_a->position, delta);
    double dx = delta[0];
    double dy = delta[1];
    double dz = delta[2];
    double dist = joint3d_len3(dx, dy, dz);

    if (!isfinite(dist) || dist < 1e-12)
        return; /* coincident — can't determine direction */

    double error = dist - j->target_distance;
    double inv_dist = 1.0 / dist;
    double nx = dx * inv_dist;
    double ny = dy * inv_dist;
    double nz = dz * inv_dist;

    double inv_sum = j->body_a->inv_mass + j->body_b->inv_mass;
    if (!isfinite(inv_sum) || inv_sum < 1e-12)
        return; /* both static */

    /* Positional correction: move each body proportional to inverse mass */
    double correction = joint3d_clamp_coord(error / inv_sum);
    if (!isfinite(correction))
        return;

    j->body_a->position[0] =
        joint3d_clamp_coord(j->body_a->position[0] + correction * j->body_a->inv_mass * nx);
    j->body_a->position[1] =
        joint3d_clamp_coord(j->body_a->position[1] + correction * j->body_a->inv_mass * ny);
    j->body_a->position[2] =
        joint3d_clamp_coord(j->body_a->position[2] + correction * j->body_a->inv_mass * nz);
    j->body_b->position[0] =
        joint3d_clamp_coord(j->body_b->position[0] - correction * j->body_b->inv_mass * nx);
    j->body_b->position[1] =
        joint3d_clamp_coord(j->body_b->position[1] - correction * j->body_b->inv_mass * ny);
    j->body_b->position[2] =
        joint3d_clamp_coord(j->body_b->position[2] - correction * j->body_b->inv_mass * nz);
    if (fabs(correction) > 1e-12) {
        if (j->body_a->inv_mass > 0.0)
            joint3d_mark_body_moved(j->body_a);
        if (j->body_b->inv_mass > 0.0)
            joint3d_mark_body_moved(j->body_b);
    }

    /* Velocity correction: remove relative velocity along constraint axis */
    double rvx = j->body_b->velocity[0] - j->body_a->velocity[0];
    double rvy = j->body_b->velocity[1] - j->body_a->velocity[1];
    double rvz = j->body_b->velocity[2] - j->body_a->velocity[2];
    double rv_along = rvx * nx + rvy * ny + rvz * nz;
    if (!isfinite(rv_along))
        return;

    double jn = joint3d_clamp_force(rv_along / inv_sum);
    if (!isfinite(jn))
        return;
    j->body_a->velocity[0] =
        joint3d_clamp_force(j->body_a->velocity[0] + jn * j->body_a->inv_mass * nx);
    j->body_a->velocity[1] =
        joint3d_clamp_force(j->body_a->velocity[1] + jn * j->body_a->inv_mass * ny);
    j->body_a->velocity[2] =
        joint3d_clamp_force(j->body_a->velocity[2] + jn * j->body_a->inv_mass * nz);
    j->body_b->velocity[0] =
        joint3d_clamp_force(j->body_b->velocity[0] - jn * j->body_b->inv_mass * nx);
    j->body_b->velocity[1] =
        joint3d_clamp_force(j->body_b->velocity[1] - jn * j->body_b->inv_mass * ny);
    j->body_b->velocity[2] =
        joint3d_clamp_force(j->body_b->velocity[2] - jn * j->body_b->inv_mass * nz);
}

/*==========================================================================
 * Spring Joint
 *=========================================================================*/

typedef struct {
    void *vptr;
    rt_body3d_kinematics *body_a;
    rt_body3d_kinematics *body_b;
    double rest_length;
    double stiffness;
    double damping;
} rt_spring_joint3d;

/// @brief GC finalizer — release the bodies retained by this spring joint.
/// @param obj SpringJoint3D payload being finalized.
static void spring_joint_finalizer(void *obj) {
    rt_spring_joint3d *j = (rt_spring_joint3d *)obj;
    if (!j)
        return;
    joint3d_release_body_ref(&j->body_a);
    joint3d_release_body_ref(&j->body_b);
}

/// @brief Create a spring joint that applies Hooke's law forces between two bodies.
/// @details Unlike the distance joint (hard constraint), the spring joint applies
///          continuous forces: F = -k*(dist - rest) + damping. This produces
///          bouncy, elastic behavior. Damping reduces oscillation over time.
/// @param body_a      First body handle.
/// @param body_b      Second body handle.
/// @param rest_length Natural length at which the spring exerts zero force.
/// @param stiffness   Spring constant k (higher = stiffer, less stretch).
/// @param damping     Velocity damping coefficient (higher = less oscillation).
/// @return Opaque joint handle, or NULL on failure.
void *rt_spring_joint3d_new(
    void *body_a, void *body_b, double rest_length, double stiffness, double damping) {
    if (!rt_g3d_has_class(body_a, RT_G3D_BODY3D_CLASS_ID) ||
        !rt_g3d_has_class(body_b, RT_G3D_BODY3D_CLASS_ID)) {
        rt_trap("SpringJoint3D.New: both bodies must be non-null");
        return NULL;
    }
    rt_spring_joint3d *j = (rt_spring_joint3d *)rt_obj_new_i64(RT_G3D_SPRINGJOINT3D_CLASS_ID,
                                                               (int64_t)sizeof(rt_spring_joint3d));
    if (!j) {
        rt_trap("SpringJoint3D.New: allocation failed");
        return NULL;
    }
    j->vptr = NULL;
    j->body_a = (rt_body3d_kinematics *)body_a;
    j->body_b = (rt_body3d_kinematics *)body_b;
    rt_obj_retain_maybe(body_a);
    rt_obj_retain_maybe(body_b);
    j->rest_length = joint3d_sanitize_nonnegative(rest_length);
    j->stiffness = joint3d_sanitize_nonnegative(stiffness);
    j->damping = joint3d_sanitize_nonnegative(damping);
    rt_obj_set_finalizer(j, spring_joint_finalizer);
    return j;
}

/// @brief Get the spring constant k.
/// @param joint SpringJoint3D handle to inspect.
/// @return Non-negative stiffness, or zero for an invalid handle.
double rt_spring_joint3d_get_stiffness(void *joint) {
    rt_spring_joint3d *j =
        (rt_spring_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_SPRINGJOINT3D_CLASS_ID);
    return j ? j->stiffness : 0;
}

/// @brief Set the spring constant k at runtime.
/// @param joint SpringJoint3D handle to modify.
/// @param stiffness Requested spring constant, sanitized to a non-negative finite value.
void rt_spring_joint3d_set_stiffness(void *joint, double stiffness) {
    rt_spring_joint3d *j =
        (rt_spring_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_SPRINGJOINT3D_CLASS_ID);
    if (j)
        j->stiffness = joint3d_sanitize_nonnegative(stiffness);
}

/// @brief Get the velocity damping coefficient.
/// @param joint SpringJoint3D handle to inspect.
/// @return Non-negative damping coefficient, or zero for an invalid handle.
double rt_spring_joint3d_get_damping(void *joint) {
    rt_spring_joint3d *j =
        (rt_spring_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_SPRINGJOINT3D_CLASS_ID);
    return j ? j->damping : 0;
}

/// @brief Set the velocity damping coefficient at runtime.
/// @param joint SpringJoint3D handle to modify.
/// @param damping Requested damping coefficient, sanitized to a non-negative finite value.
void rt_spring_joint3d_set_damping(void *joint, double damping) {
    rt_spring_joint3d *j =
        (rt_spring_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_SPRINGJOINT3D_CLASS_ID);
    if (j)
        j->damping = joint3d_sanitize_nonnegative(damping);
}

/// @brief Get the spring's natural (zero-force) length.
/// @param joint SpringJoint3D handle to inspect.
/// @return Non-negative rest length, or zero for an invalid handle.
double rt_spring_joint3d_get_rest_length(void *joint) {
    rt_spring_joint3d *j =
        (rt_spring_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_SPRINGJOINT3D_CLASS_ID);
    return j ? j->rest_length : 0;
}

/// @brief Integrate spring + damping forces into both bodies' velocities.
/// @details Applies Hooke's law `F = -k * (dist - rest)` along the axis from
///   A to B, plus a velocity-damping term `-c * (rel_vel · axis)` to bleed
///   oscillation. The two forces are added into equal-and-opposite impulses
///   of magnitude `F * dt` scaled by each body's inverse mass, preserving
///   momentum between the pair. Unlike `solve_distance`, this is a soft
///   constraint: it never corrects positions directly, so over-stiff springs
///   can oscillate or explode at the current fixed step — tune `stiffness`
///   below the stability ceiling for the caller's step frequency.
/// @param j Spring joint constraint to solve.
/// @param dt Physics step duration used to integrate force into velocity.
static void solve_spring(rt_spring_joint3d *j, double dt) {
    double delta[3];
    dt = joint3d_sanitize_dt(dt);
    if (!j || !joint3d_body_is_finite(j->body_a) || !joint3d_body_is_finite(j->body_b) || dt <= 0.0)
        return;

    joint3d_vec3_sub(j->body_b->position, j->body_a->position, delta);
    double dx = delta[0];
    double dy = delta[1];
    double dz = delta[2];
    double dist = joint3d_len3(dx, dy, dz);

    if (!isfinite(dist) || dist < 1e-12)
        return;

    double inv_dist = 1.0 / dist;
    double nx = dx * inv_dist;
    double ny = dy * inv_dist;
    double nz = dz * inv_dist;

    /* Hooke's law: F = -k * (dist - rest) */
    double displacement = dist - j->rest_length;
    double spring_force = -j->stiffness * displacement;

    /* Damping: F_damp = -c * relative_velocity_along_axis */
    double rvx = j->body_b->velocity[0] - j->body_a->velocity[0];
    double rvy = j->body_b->velocity[1] - j->body_a->velocity[1];
    double rvz = j->body_b->velocity[2] - j->body_a->velocity[2];
    double rv_along = rvx * nx + rvy * ny + rvz * nz;
    if (!isfinite(rv_along))
        return;
    double damp_force = -j->damping * rv_along;

    double total_force = joint3d_clamp_force(spring_force + damp_force);

    /* Apply force to both bodies (equal and opposite) */
    double fx = joint3d_clamp_force(total_force * nx);
    double fy = joint3d_clamp_force(total_force * ny);
    double fz = joint3d_clamp_force(total_force * nz);

    /* F = ma → a = F * inv_mass, v += a * dt */
    j->body_a->velocity[0] =
        joint3d_clamp_force(j->body_a->velocity[0] - fx * j->body_a->inv_mass * dt);
    j->body_a->velocity[1] =
        joint3d_clamp_force(j->body_a->velocity[1] - fy * j->body_a->inv_mass * dt);
    j->body_a->velocity[2] =
        joint3d_clamp_force(j->body_a->velocity[2] - fz * j->body_a->inv_mass * dt);
    j->body_b->velocity[0] =
        joint3d_clamp_force(j->body_b->velocity[0] + fx * j->body_b->inv_mass * dt);
    j->body_b->velocity[1] =
        joint3d_clamp_force(j->body_b->velocity[1] + fy * j->body_b->inv_mass * dt);
    j->body_b->velocity[2] =
        joint3d_clamp_force(j->body_b->velocity[2] + fz * j->body_b->inv_mass * dt);
}

/*==========================================================================
 * Hinge Joint
 *=========================================================================*/

typedef struct {
    void *vptr;
    rt_body3d_kinematics *body_a;
    rt_body3d_kinematics *body_b;
    double local_anchor_a[3];
    double local_anchor_b[3];
    double local_axis_a[3];
    double ref_perp_a[3]; /* a perpendicular-to-axis reference, in each body's local frame */
    double ref_perp_b[3]; /* coincident at creation, so the hinge angle starts at 0 */
    int8_t motor_enabled;
    double motor_target_velocity;
    double motor_max_impulse;
    int8_t has_limits;
    double angle_min;
    double angle_max;
} rt_hinge_joint3d;

/// @brief GC finalizer: release the hinge's two retained body references.
/// @param obj HingeJoint3D payload being finalized.
static void hinge_joint_finalizer(void *obj) {
    rt_hinge_joint3d *j = (rt_hinge_joint3d *)obj;
    if (!j)
        return;
    joint3d_release_body_ref(&j->body_a);
    joint3d_release_body_ref(&j->body_b);
}

/// @brief Create a hinge joint pinning two bodies at @p anchor and constraining rotation to @p
/// axis.
/// @details Validates the bodies/anchor/axis, then stores the anchor and (normalized) axis in each
///          body's local frame so the constraint follows the bodies as they move. Traps on bad
///          input.
/// @param body_a First Body3D handle.
/// @param body_b Second Body3D handle.
/// @param anchor Finite Vec3 world-space pivot shared at creation.
/// @param axis Nonzero finite Vec3 world-space hinge axis.
/// @return Opaque HingeJoint3D handle, or NULL on validation failure.
void *rt_hinge_joint3d_new(void *body_a, void *body_b, void *anchor, void *axis) {
    double anchor_world[3];
    double axis_world[3];
    double inv_a_rotation[4];
    if (!rt_g3d_has_class(body_a, RT_G3D_BODY3D_CLASS_ID) ||
        !rt_g3d_has_class(body_b, RT_G3D_BODY3D_CLASS_ID)) {
        rt_trap("HingeJoint3D.New: both bodies must be non-null");
        return NULL;
    }
    if (!joint3d_read_vec3(anchor, anchor_world)) {
        rt_trap("HingeJoint3D.New: anchor must be a finite Vec3");
        return NULL;
    }
    if (!joint3d_read_vec3(axis, axis_world) || !joint3d_vec3_normalize(axis_world)) {
        rt_trap("HingeJoint3D.New: axis must be a non-zero finite Vec3");
        return NULL;
    }

    rt_hinge_joint3d *j = (rt_hinge_joint3d *)rt_obj_new_i64(RT_G3D_HINGEJOINT3D_CLASS_ID,
                                                             (int64_t)sizeof(rt_hinge_joint3d));
    if (!j) {
        rt_trap("HingeJoint3D.New: allocation failed");
        return NULL;
    }
    j->vptr = NULL;
    j->body_a = (rt_body3d_kinematics *)body_a;
    j->body_b = (rt_body3d_kinematics *)body_b;
    joint3d_local_from_world(j->body_a, anchor_world, j->local_anchor_a);
    joint3d_local_from_world(j->body_b, anchor_world, j->local_anchor_b);
    joint3d_quat_conjugate(j->body_a->orientation, inv_a_rotation);
    joint3d_quat_rotate_vec3(inv_a_rotation, axis_world, j->local_axis_a);
    if (!joint3d_vec3_normalize(j->local_axis_a))
        joint3d_vec3_set(j->local_axis_a, 0.0, 1.0, 0.0);
    /* Store a perpendicular-to-axis reference in each body's local frame; both
     * map to the same world direction now, so the hinge angle starts at 0. */
    {
        double seed[3];
        double perp_world[3];
        double inv_b_rotation[4];
        double d;
        if (fabs(axis_world[0]) < 0.9)
            joint3d_vec3_set(seed, 1.0, 0.0, 0.0);
        else
            joint3d_vec3_set(seed, 0.0, 1.0, 0.0);
        d = joint3d_vec3_dot(seed, axis_world);
        perp_world[0] = seed[0] - axis_world[0] * d;
        perp_world[1] = seed[1] - axis_world[1] * d;
        perp_world[2] = seed[2] - axis_world[2] * d;
        if (!joint3d_vec3_normalize(perp_world))
            joint3d_vec3_set(perp_world, 1.0, 0.0, 0.0);
        joint3d_quat_rotate_vec3(inv_a_rotation, perp_world, j->ref_perp_a);
        joint3d_quat_conjugate(j->body_b->orientation, inv_b_rotation);
        joint3d_quat_rotate_vec3(inv_b_rotation, perp_world, j->ref_perp_b);
    }
    j->motor_enabled = 0;
    j->motor_target_velocity = 0.0;
    j->motor_max_impulse = 0.0;
    j->has_limits = 0;
    j->angle_min = 0.0;
    j->angle_max = 0.0;
    rt_obj_retain_maybe(body_a);
    rt_obj_retain_maybe(body_b);
    rt_obj_set_finalizer(j, hinge_joint_finalizer);
    return j;
}

/// @brief Current signed hinge angle (radians) between the bodies' stored
///   perpendicular references, projected onto the world hinge axis.
/// @param j Valid hinge joint whose retained bodies define the current pose.
/// @return Signed right-handed angle in radians, or zero for degenerate projected references.
static double hinge_joint_current_angle(const rt_hinge_joint3d *j) {
    double axis_world[3];
    double ra[3];
    double rb[3];
    double cross[3];
    double da;
    double db;
    joint3d_world_axis_from_local(j->body_a, j->local_axis_a, axis_world);
    joint3d_quat_rotate_vec3(j->body_a->orientation, j->ref_perp_a, ra);
    joint3d_quat_rotate_vec3(j->body_b->orientation, j->ref_perp_b, rb);
    da = joint3d_vec3_dot(ra, axis_world);
    ra[0] -= axis_world[0] * da;
    ra[1] -= axis_world[1] * da;
    ra[2] -= axis_world[2] * da;
    db = joint3d_vec3_dot(rb, axis_world);
    rb[0] -= axis_world[0] * db;
    rb[1] -= axis_world[1] * db;
    rb[2] -= axis_world[2] * db;
    if (!joint3d_vec3_normalize(ra) || !joint3d_vec3_normalize(rb))
        return 0.0;
    cross[0] = ra[1] * rb[2] - ra[2] * rb[1];
    cross[1] = ra[2] * rb[0] - ra[0] * rb[2];
    cross[2] = ra[0] * rb[1] - ra[1] * rb[0];
    return atan2(joint3d_vec3_dot(cross, axis_world), joint3d_vec3_dot(ra, rb));
}

/// @brief When the hinge angle reaches a configured limit, remove the axial
///   relative angular velocity that would carry it further past the bound, so
///   the joint stops at the limit (overrides the motor at the stop).
/// @param j Hinge joint with enabled angle limits.
/// @param axis_world Normalized current hinge axis in world space.
static void hinge_joint_apply_limits(rt_hinge_joint3d *j, const double *axis_world) {
    rt_body3d_kinematics *a = j->body_a;
    rt_body3d_kinematics *b = j->body_b;
    double angle;
    double rel[3];
    double w_rel;
    double inv_sum;
    double correction;
    if (!joint3d_body_is_finite(a) || !joint3d_body_is_finite(b))
        return;
    if (!joint3d_vec3_all_finite(a->angular_velocity) ||
        !joint3d_vec3_all_finite(b->angular_velocity))
        return;
    angle = hinge_joint_current_angle(j);
    joint3d_vec3_sub(b->angular_velocity, a->angular_velocity, rel);
    w_rel = joint3d_vec3_dot(rel, axis_world);
    if (!((angle >= j->angle_max && w_rel > 0.0) || (angle <= j->angle_min && w_rel < 0.0)))
        return;
    /* Weight by effective inverse inertia about the hinge axis, not inverse mass. */
    double w_a = joint3d_effective_inv_inertia_about_axis(a, axis_world);
    double w_b = joint3d_effective_inv_inertia_about_axis(b, axis_world);
    inv_sum = w_a + w_b;
    if (inv_sum <= 1e-9)
        return;
    correction = joint3d_clamp_force(w_rel / inv_sum); /* drive axial relative velocity to 0 */
    for (int i = 0; i < 3; i++) {
        a->angular_velocity[i] =
            joint3d_clamp_force(a->angular_velocity[i] + axis_world[i] * correction * w_a);
        b->angular_velocity[i] =
            joint3d_clamp_force(b->angular_velocity[i] - axis_world[i] * correction * w_b);
    }
}

/// @brief Drive the relative angular velocity about the hinge axis toward the
///   motor target velocity, with the per-step change bounded by
///   motor_max_impulse (the motor's strength). The inverse-inertia terms cancel
///   so an unbounded motor reaches the target in one step; the clamp models a
///   finite motor. Runs after the perpendicular-twist constraint, which leaves
///   the axial component free for the motor to drive.
/// @param j Hinge joint with enabled motor configuration.
/// @param axis_world Normalized current hinge axis in world space.
static void hinge_joint_apply_motor(rt_hinge_joint3d *j, const double *axis_world) {
    rt_body3d_kinematics *a = j->body_a;
    rt_body3d_kinematics *b = j->body_b;
    double rel[3];
    double w_rel;
    double violation;
    double inv_sum;
    double correction;
    if (!joint3d_body_is_finite(a) || !joint3d_body_is_finite(b))
        return;
    if (!joint3d_vec3_all_finite(a->angular_velocity) ||
        !joint3d_vec3_all_finite(b->angular_velocity))
        return;
    joint3d_vec3_sub(b->angular_velocity, a->angular_velocity, rel);
    w_rel = joint3d_vec3_dot(rel, axis_world);
    violation = w_rel - j->motor_target_velocity;
    /* Weight by effective inverse inertia about the hinge axis, not inverse mass. */
    double w_a = joint3d_effective_inv_inertia_about_axis(a, axis_world);
    double w_b = joint3d_effective_inv_inertia_about_axis(b, axis_world);
    inv_sum = w_a + w_b;
    if (inv_sum <= 1e-9)
        return;
    correction = joint3d_clamp_force(violation / inv_sum);
    if (correction > j->motor_max_impulse)
        correction = j->motor_max_impulse;
    else if (correction < -j->motor_max_impulse)
        correction = -j->motor_max_impulse;
    for (int i = 0; i < 3; i++) {
        a->angular_velocity[i] =
            joint3d_clamp_force(a->angular_velocity[i] + axis_world[i] * correction * w_a);
        b->angular_velocity[i] =
            joint3d_clamp_force(b->angular_velocity[i] - axis_world[i] * correction * w_b);
    }
}

/// @brief Solve one hinge constraint step: keep anchors coincident, lock off-axis rotation, apply
/// motor/limits.
/// @details Runs positional + linear-velocity anchor correction, removes relative angular velocity
///          except about the world hinge axis, then applies the optional motor and angle limits.
/// @param j Hinge joint constraint to solve.
/// @param dt Physics step duration, reserved by the position-based solver interface.
static void solve_hinge(rt_hinge_joint3d *j, double dt) {
    double axis_world[3];
    (void)dt;
    if (!j)
        return;
    joint3d_correct_anchor_pair(j->body_a, j->body_b, j->local_anchor_a, j->local_anchor_b, 1.0);
    joint3d_remove_relative_linear_velocity(j->body_a, j->body_b, 1.0);
    joint3d_world_axis_from_local(j->body_a, j->local_axis_a, axis_world);
    joint3d_remove_relative_angular_velocity(j->body_a, j->body_b, axis_world);
    if (j->motor_enabled)
        hinge_joint_apply_motor(j, axis_world);
    if (j->has_limits)
        hinge_joint_apply_limits(j, axis_world);
}

/// @brief Current signed hinge angle (radians) about the axis, measured between
///   the two bodies' stored perpendicular references. Reads 0 at creation; grows
///   right-handed about the hinge axis as body B rotates relative to body A.
/// @param joint HingeJoint3D handle to inspect.
/// @return Signed current angle in radians, or zero for an invalid handle.
double rt_hinge_joint3d_get_angle(void *joint) {
    if (!rt_g3d_has_class(joint, RT_G3D_HINGEJOINT3D_CLASS_ID))
        return 0.0;
    return hinge_joint_current_angle((const rt_hinge_joint3d *)joint);
}

/// @brief Constrain the hinge to [min, max] radians (swapped if reversed); the
///   solver stops rotation at the bounds. Non-finite limits disable the limit.
/// @param joint HingeJoint3D handle to modify.
/// @param min_angle Requested minimum signed angle in radians.
/// @param max_angle Requested maximum signed angle in radians.
void rt_hinge_joint3d_set_limits(void *joint, double min_angle, double max_angle) {
    rt_hinge_joint3d *j;
    if (!rt_g3d_has_class(joint, RT_G3D_HINGEJOINT3D_CLASS_ID))
        return;
    j = (rt_hinge_joint3d *)joint;
    if (!isfinite(min_angle) || !isfinite(max_angle)) {
        j->has_limits = 0;
        return;
    }
    if (min_angle > max_angle) {
        double tmp = min_angle;
        min_angle = max_angle;
        max_angle = tmp;
    }
    j->angle_min = min_angle;
    j->angle_max = max_angle;
    j->has_limits = 1;
}

/// @brief Enable/configure a hinge motor that drives rotation about the hinge
///   axis toward @p target_velocity (rad/s), bounded by @p max_impulse strength.
/// @param joint HingeJoint3D handle to modify.
/// @param enabled Nonzero to enable motor impulses during solving.
/// @param target_velocity Desired relative angular velocity in radians per second.
/// @param max_impulse Non-negative per-step motor strength bound.
void rt_hinge_joint3d_set_motor(void *joint,
                                int8_t enabled,
                                double target_velocity,
                                double max_impulse) {
    rt_hinge_joint3d *j;
    if (!rt_g3d_has_class(joint, RT_G3D_HINGEJOINT3D_CLASS_ID))
        return;
    j = (rt_hinge_joint3d *)joint;
    j->motor_enabled = enabled ? 1 : 0;
    j->motor_target_velocity = joint3d_clamp_force(target_velocity);
    j->motor_max_impulse = joint3d_sanitize_nonnegative(max_impulse);
}

/*==========================================================================
 * Rope Joint
 *=========================================================================*/

typedef struct {
    void *vptr;
    rt_body3d_kinematics *body_a;
    rt_body3d_kinematics *body_b;
    double max_length;
} rt_rope_joint3d;

/// @brief GC finalizer: release the rope's two retained body references.
/// @param obj RopeJoint3D payload being finalized.
static void rope_joint_finalizer(void *obj) {
    rt_rope_joint3d *j = (rt_rope_joint3d *)obj;
    if (!j)
        return;
    joint3d_release_body_ref(&j->body_a);
    joint3d_release_body_ref(&j->body_b);
}

/// @brief Create a rope joint limiting the distance between two bodies to @p max_length.
/// @details A rope only resists stretching past its length (it goes slack when closer). The length
///          is sanitized non-negative. Traps on non-body inputs or allocation failure.
/// @param body_a First Body3D handle.
/// @param body_b Second Body3D handle.
/// @param max_length Maximum permitted center-to-center separation.
/// @return Opaque RopeJoint3D handle, or NULL on failure.
void *rt_rope_joint3d_new(void *body_a, void *body_b, double max_length) {
    if (!rt_g3d_has_class(body_a, RT_G3D_BODY3D_CLASS_ID) ||
        !rt_g3d_has_class(body_b, RT_G3D_BODY3D_CLASS_ID)) {
        rt_trap("RopeJoint3D.New: both bodies must be non-null");
        return NULL;
    }
    rt_rope_joint3d *j = (rt_rope_joint3d *)rt_obj_new_i64(RT_G3D_ROPEJOINT3D_CLASS_ID,
                                                           (int64_t)sizeof(rt_rope_joint3d));
    if (!j) {
        rt_trap("RopeJoint3D.New: allocation failed");
        return NULL;
    }
    j->vptr = NULL;
    j->body_a = (rt_body3d_kinematics *)body_a;
    j->body_b = (rt_body3d_kinematics *)body_b;
    j->max_length = joint3d_sanitize_nonnegative(max_length);
    rt_obj_retain_maybe(body_a);
    rt_obj_retain_maybe(body_b);
    rt_obj_set_finalizer(j, rope_joint_finalizer);
    return j;
}

/// @brief Read the rope's maximum length (0 if the handle is invalid).
/// @param joint RopeJoint3D handle to inspect.
/// @return Non-negative maximum length, or zero for an invalid handle.
double rt_rope_joint3d_get_max_length(void *joint) {
    rt_rope_joint3d *j =
        (rt_rope_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_ROPEJOINT3D_CLASS_ID);
    return j ? j->max_length : 0.0;
}

/// @brief Set the rope's maximum length (sanitized non-negative).
/// @param joint RopeJoint3D handle to modify.
/// @param max_length Requested maximum separation.
void rt_rope_joint3d_set_max_length(void *joint, double max_length) {
    rt_rope_joint3d *j =
        (rt_rope_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_ROPEJOINT3D_CLASS_ID);
    if (j)
        j->max_length = joint3d_sanitize_nonnegative(max_length);
}

/// @brief Solve one rope constraint step: only acts when the bodies are stretched past max_length.
/// @details Projects the bodies back to the rope length and removes the separating relative
/// velocity
///          along the rope direction (inverse-mass weighted); does nothing while the rope is slack.
/// @param j Rope joint constraint to solve.
/// @param dt Physics step duration, reserved by the position-based solver interface.
static void solve_rope(rt_rope_joint3d *j, double dt) {
    double delta[3];
    double dist;
    double inv_sum;
    double n[3];
    double rel_velocity[3];
    double rel_along;
    (void)dt;
    if (!j || !joint3d_body_is_finite(j->body_a) || !joint3d_body_is_finite(j->body_b))
        return;
    joint3d_vec3_sub(j->body_b->position, j->body_a->position, delta);
    if (!joint3d_vec3_all_finite(delta))
        return;
    dist = joint3d_vec3_len(delta);
    if (!isfinite(dist) || dist <= j->max_length || dist < 1e-12)
        return;
    inv_sum = j->body_a->inv_mass + j->body_b->inv_mass;
    if (!isfinite(inv_sum) || inv_sum < 1e-12)
        return;
    n[0] = delta[0] / dist;
    n[1] = delta[1] / dist;
    n[2] = delta[2] / dist;

    double error = dist - j->max_length;
    double correction = joint3d_clamp_coord(error / inv_sum);
    for (int i = 0; i < 3; i++) {
        j->body_a->position[i] =
            joint3d_clamp_coord(j->body_a->position[i] + correction * j->body_a->inv_mass * n[i]);
        j->body_b->position[i] =
            joint3d_clamp_coord(j->body_b->position[i] - correction * j->body_b->inv_mass * n[i]);
    }
    if (fabs(correction) > 1e-12) {
        if (j->body_a->inv_mass > 0.0)
            joint3d_mark_body_moved(j->body_a);
        if (j->body_b->inv_mass > 0.0)
            joint3d_mark_body_moved(j->body_b);
    }

    joint3d_vec3_sub(j->body_b->velocity, j->body_a->velocity, rel_velocity);
    rel_along = joint3d_vec3_dot(rel_velocity, n);
    if (!isfinite(rel_along) || rel_along <= 0.0)
        return;
    double impulse = joint3d_clamp_force(rel_along / inv_sum);
    for (int i = 0; i < 3; i++) {
        j->body_a->velocity[i] =
            joint3d_clamp_force(j->body_a->velocity[i] + impulse * j->body_a->inv_mass * n[i]);
        j->body_b->velocity[i] =
            joint3d_clamp_force(j->body_b->velocity[i] - impulse * j->body_b->inv_mass * n[i]);
    }
}

/*==========================================================================
 * SixDof Joint
 *=========================================================================*/

typedef struct {
    void *vptr;
    rt_body3d_kinematics *body_a;
    rt_body3d_kinematics *body_b;
    double local_anchor_a[3];
    double local_anchor_b[3];
    double linear_min[3];
    double linear_max[3];
    double angular_min[3];
    double angular_max[3];
    double reference_relative_orientation[4];
    int8_t linear_motor_enabled;
    double linear_motor_velocity[3];
    double linear_motor_max_impulse;
} rt_sixdof_joint3d;

/// @brief GC finalizer: release the 6DOF joint's two retained body references.
/// @param obj SixDofJoint3D payload being finalized.
static void sixdof_joint_finalizer(void *obj) {
    rt_sixdof_joint3d *j = (rt_sixdof_joint3d *)obj;
    if (!j)
        return;
    joint3d_release_body_ref(&j->body_a);
    joint3d_release_body_ref(&j->body_b);
}

/// @brief Create a 6-DOF joint anchoring two bodies at the translations of @p frame_a / @p frame_b.
/// @details Reads each Mat4 frame's translation as the per-body local anchor and starts with all
/// six
///          axes locked (zero linear/angular range), to be relaxed via the set-limits calls. Traps
///          on non-body inputs or non-finite frames.
/// @param body_a First Body3D handle.
/// @param body_b Second Body3D handle.
/// @param frame_a Mat4 whose translation defines body A's local anchor.
/// @param frame_b Mat4 whose translation defines body B's local anchor.
/// @return Opaque SixDofJoint3D handle, or NULL on failure.
void *rt_sixdof_joint3d_new(void *body_a, void *body_b, void *frame_a, void *frame_b) {
    double local_anchor_a[3];
    double local_anchor_b[3];
    if (!rt_g3d_has_class(body_a, RT_G3D_BODY3D_CLASS_ID) ||
        !rt_g3d_has_class(body_b, RT_G3D_BODY3D_CLASS_ID)) {
        rt_trap("SixDofJoint3D.New: both bodies must be non-null");
        return NULL;
    }
    if (!joint3d_read_mat4_translation(frame_a, local_anchor_a) ||
        !joint3d_read_mat4_translation(frame_b, local_anchor_b)) {
        rt_trap("SixDofJoint3D.New: frames must be finite Mat4 values");
        return NULL;
    }
    rt_sixdof_joint3d *j = (rt_sixdof_joint3d *)rt_obj_new_i64(RT_G3D_SIXDOFJOINT3D_CLASS_ID,
                                                               (int64_t)sizeof(rt_sixdof_joint3d));
    if (!j) {
        rt_trap("SixDofJoint3D.New: allocation failed");
        return NULL;
    }
    j->vptr = NULL;
    j->body_a = (rt_body3d_kinematics *)body_a;
    j->body_b = (rt_body3d_kinematics *)body_b;
    memcpy(j->local_anchor_a, local_anchor_a, sizeof(j->local_anchor_a));
    memcpy(j->local_anchor_b, local_anchor_b, sizeof(j->local_anchor_b));
    joint3d_vec3_set(j->linear_min, 0.0, 0.0, 0.0);
    joint3d_vec3_set(j->linear_max, 0.0, 0.0, 0.0);
    joint3d_vec3_set(j->angular_min, 0.0, 0.0, 0.0);
    joint3d_vec3_set(j->angular_max, 0.0, 0.0, 0.0);
    {
        double inv_a[4];
        joint3d_quat_conjugate(j->body_a->orientation, inv_a);
        joint3d_quat_mul(inv_a, j->body_b->orientation, j->reference_relative_orientation);
        joint3d_quat_normalize(j->reference_relative_orientation);
    }
    j->linear_motor_enabled = 0;
    joint3d_vec3_set(j->linear_motor_velocity, 0.0, 0.0, 0.0);
    j->linear_motor_max_impulse = 0.0;
    rt_obj_retain_maybe(body_a);
    rt_obj_retain_maybe(body_b);
    rt_obj_set_finalizer(j, sixdof_joint_finalizer);
    return j;
}

/// @brief Compute body B's pose-angle delta from the SixDof creation pose in body A's frame.
/// @param j Six-degree-of-freedom joint to inspect.
/// @param out Receives the three-component rotation vector in body A's joint frame.
/// @return One when both bodies and the resulting angles are finite, otherwise zero.
static int sixdof_joint_current_pose_angles(const rt_sixdof_joint3d *j, double *out) {
    double inv_a[4];
    double rel[4];
    double inv_ref[4];
    double delta[4];
    if (!j || !out || !joint3d_body_is_finite(j->body_a) || !joint3d_body_is_finite(j->body_b))
        return 0;
    joint3d_quat_conjugate(j->body_a->orientation, inv_a);
    joint3d_quat_mul(inv_a, j->body_b->orientation, rel);
    joint3d_quat_normalize(rel);
    joint3d_quat_conjugate(j->reference_relative_orientation, inv_ref);
    joint3d_quat_mul(rel, inv_ref, delta);
    joint3d_quat_to_rotation_vector(delta, out);
    return joint3d_vec3_all_finite(out);
}

/// @brief World-space unit axis for a SixDof angular limit component.
/// @param j Six-degree-of-freedom joint defining body A's joint frame.
/// @param axis Component index from zero through two.
/// @param out Receives a normalized world-space axis or a deterministic basis fallback.
static void sixdof_joint_world_axis(const rt_sixdof_joint3d *j, int axis, double *out) {
    double local[3] = {0.0, 0.0, 0.0};
    if (!out) {
        return;
    }
    if (axis < 0 || axis > 2 || !j || !j->body_a) {
        joint3d_vec3_set(out, 1.0, 0.0, 0.0);
        return;
    }
    local[axis] = 1.0;
    joint3d_quat_rotate_vec3(j->body_a->orientation, local, out);
    if (!joint3d_vec3_normalize(out))
        joint3d_vec3_set(out, axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0);
}

/// @brief Correct relative orientation when the SixDof pose-angle exits an angular limit.
/// @param j Six-degree-of-freedom joint whose body orientations are corrected.
/// @param axis_world Normalized world-space limit axis.
/// @param violation Signed angular distance outside the permitted interval.
static void sixdof_joint_apply_pose_angle_correction(rt_sixdof_joint3d *j,
                                                     const double *axis_world,
                                                     double violation) {
    double inv_sum;
    if (!j || !axis_world || fabs(violation) < 1e-12 || !joint3d_body_is_finite(j->body_a) ||
        !joint3d_body_is_finite(j->body_b))
        return;
    /* Split the orientation correction by effective inverse inertia about the
     * limit axis, not inverse mass. */
    double w_a = joint3d_effective_inv_inertia_about_axis(j->body_a, axis_world);
    double w_b = joint3d_effective_inv_inertia_about_axis(j->body_b, axis_world);
    inv_sum = w_a + w_b;
    if (!isfinite(inv_sum) || inv_sum < 1e-12)
        return;
    violation = joint3d_clamp_force(violation);
    joint3d_quat_prepend_axis_angle(j->body_a->orientation, axis_world, violation * w_a / inv_sum);
    joint3d_quat_prepend_axis_angle(j->body_b->orientation, axis_world, -violation * w_b / inv_sum);
}

/// @brief Remove relative angular velocity that would keep driving a pose-angle outside its limit.
/// @param j Six-degree-of-freedom joint whose angular velocities are corrected.
/// @param pose_angles Per-axis pose angles already clamped to their active limits.
static void sixdof_joint_apply_pose_angle_velocity_stop(rt_sixdof_joint3d *j,
                                                        const double *pose_angles) {
    double rel[3];
    if (!j || !pose_angles || !joint3d_body_is_finite(j->body_a) ||
        !joint3d_body_is_finite(j->body_b))
        return;
    if (!joint3d_vec3_all_finite(j->body_a->angular_velocity) ||
        !joint3d_vec3_all_finite(j->body_b->angular_velocity))
        return;
    joint3d_vec3_sub(j->body_b->angular_velocity, j->body_a->angular_velocity, rel);
    if (!joint3d_vec3_all_finite(rel))
        return;
    for (int i = 0; i < 3; i++) {
        double axis_world[3];
        double rel_axis;
        double correction;
        double w_a;
        double w_b;
        double inv_sum;
        int stop = 0;
        sixdof_joint_world_axis(j, i, axis_world);
        rel_axis = joint3d_vec3_dot(rel, axis_world);
        if (!isfinite(rel_axis))
            continue;
        if (fabs(j->angular_max[i] - j->angular_min[i]) <= 1e-12) {
            stop = fabs(rel_axis) > 1e-12;
        } else if (pose_angles[i] >= j->angular_max[i] - 1e-6 && rel_axis > 0.0) {
            stop = 1;
        } else if (pose_angles[i] <= j->angular_min[i] + 1e-6 && rel_axis < 0.0) {
            stop = 1;
        }
        if (!stop)
            continue;
        /* Effective inverse inertia about this limit axis, not inverse mass. */
        w_a = joint3d_effective_inv_inertia_about_axis(j->body_a, axis_world);
        w_b = joint3d_effective_inv_inertia_about_axis(j->body_b, axis_world);
        inv_sum = w_a + w_b;
        if (!isfinite(inv_sum) || inv_sum < 1e-12)
            continue;
        correction = joint3d_clamp_force(rel_axis / inv_sum);
        for (int k = 0; k < 3; k++) {
            j->body_a->angular_velocity[k] = joint3d_clamp_force(
                j->body_a->angular_velocity[k] + axis_world[k] * correction * w_a);
            j->body_b->angular_velocity[k] = joint3d_clamp_force(
                j->body_b->angular_velocity[k] - axis_world[k] * correction * w_b);
        }
    }
}

/// @brief Enforce SixDof per-axis pose-angle limits around the creation relative orientation.
/// @param j Six-degree-of-freedom joint to constrain.
static void sixdof_joint_apply_angular_limits(rt_sixdof_joint3d *j) {
    double pose_angles[3];
    double clamped_angles[3];
    if (!sixdof_joint_current_pose_angles(j, pose_angles))
        return;
    memcpy(clamped_angles, pose_angles, sizeof(clamped_angles));
    for (int i = 0; i < 3; i++) {
        double violation = 0.0;
        double axis_world[3];
        if (pose_angles[i] < j->angular_min[i]) {
            violation = pose_angles[i] - j->angular_min[i];
            clamped_angles[i] = j->angular_min[i];
        } else if (pose_angles[i] > j->angular_max[i]) {
            violation = pose_angles[i] - j->angular_max[i];
            clamped_angles[i] = j->angular_max[i];
        }
        if (fabs(violation) <= 1e-12)
            continue;
        sixdof_joint_world_axis(j, i, axis_world);
        sixdof_joint_apply_pose_angle_correction(j, axis_world, violation);
    }
    sixdof_joint_apply_pose_angle_velocity_stop(j, clamped_angles);
}

/// @brief Drive the relative linear velocity along each *unlocked* joint-frame
///   axis toward the motor target (locked axes are held by the limit solver),
///   bounded by the motor's max-impulse strength. Powers sliders/pistons/
///   elevators. The joint frame is body A's axes — matching the angular limits
///   — so a piston authored along local X keeps driving along body A's X after
///   body A rotates.
/// @param j Six-degree-of-freedom joint with linear motor configuration.
static void sixdof_joint_apply_linear_motor(rt_sixdof_joint3d *j) {
    rt_body3d_kinematics *a = j->body_a;
    rt_body3d_kinematics *b = j->body_b;
    double rel[3];
    double rel_frame[3];
    double impulse_frame[3] = {0.0, 0.0, 0.0};
    double impulse_world[3];
    double inv_frame[4];
    double inv_sum;
    int any_driven = 0;
    if (!joint3d_body_is_finite(a) || !joint3d_body_is_finite(b))
        return;
    if (!joint3d_vec3_all_finite(a->velocity) || !joint3d_vec3_all_finite(b->velocity))
        return;
    inv_sum = a->inv_mass + b->inv_mass;
    if (inv_sum <= 1e-9)
        return;
    joint3d_vec3_sub(b->velocity, a->velocity, rel);
    joint3d_quat_conjugate(a->orientation, inv_frame);
    joint3d_quat_rotate_vec3(inv_frame, rel, rel_frame);
    if (!joint3d_vec3_all_finite(rel_frame))
        return;
    for (int i = 0; i < 3; i++) {
        double violation;
        double correction;
        if (fabs(j->linear_max[i] - j->linear_min[i]) <= 1e-12)
            continue; /* axis is locked — leave it to the limit solver */
        violation = rel_frame[i] - j->linear_motor_velocity[i];
        correction = joint3d_clamp_force(violation / inv_sum);
        if (correction > j->linear_motor_max_impulse)
            correction = j->linear_motor_max_impulse;
        else if (correction < -j->linear_motor_max_impulse)
            correction = -j->linear_motor_max_impulse;
        impulse_frame[i] = correction;
        any_driven = 1;
    }
    if (!any_driven)
        return;
    joint3d_quat_rotate_vec3(a->orientation, impulse_frame, impulse_world);
    if (!joint3d_vec3_all_finite(impulse_world))
        return;
    for (int i = 0; i < 3; i++) {
        a->velocity[i] = joint3d_clamp_force(a->velocity[i] + impulse_world[i] * a->inv_mass);
        b->velocity[i] = joint3d_clamp_force(b->velocity[i] - impulse_world[i] * b->inv_mass);
    }
}

/// @brief Enable/configure the SixDof linear motor (target relative velocity per
///   world axis, bounded by max_impulse). Non-Vec3 velocity is ignored.
/// @param joint SixDofJoint3D handle to modify.
/// @param enabled Nonzero to enable the linear motor.
/// @param velocity Vec3 target relative velocity in body A's joint frame.
/// @param max_impulse Non-negative per-axis correction bound.
void rt_sixdof_joint3d_set_linear_motor(void *joint,
                                        int8_t enabled,
                                        void *velocity,
                                        double max_impulse) {
    double vel[3];
    rt_sixdof_joint3d *j =
        (rt_sixdof_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID);
    if (!j)
        return;
    if (!joint3d_read_vec3(velocity, vel))
        return;
    j->linear_motor_enabled = enabled ? 1 : 0;
    memcpy(j->linear_motor_velocity, vel, sizeof(j->linear_motor_velocity));
    j->linear_motor_max_impulse = joint3d_sanitize_nonnegative(max_impulse);
}

/// @brief Set the joint's per-axis linear limits from two Vec3 handles.
/// @details Limits are canonicalized (min<=max); equal min/max locks that translational axis. Traps
///          on non-Vec3 inputs.
/// @param joint SixDofJoint3D handle to modify.
/// @param min_obj Vec3 lower linear bounds in body A's joint frame.
/// @param max_obj Vec3 upper linear bounds in body A's joint frame.
void rt_sixdof_joint3d_set_linear_limits(void *joint, void *min_obj, void *max_obj) {
    double min_v[3];
    double max_v[3];
    rt_sixdof_joint3d *j =
        (rt_sixdof_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID);
    if (!j)
        return;
    if (!joint3d_read_vec3(min_obj, min_v) || !joint3d_read_vec3(max_obj, max_v)) {
        rt_trap("SixDofJoint3D.SetLinearLimits: min and max must be finite Vec3 values");
        return;
    }
    joint3d_canonicalize_limits(min_v, max_v);
    memcpy(j->linear_min, min_v, sizeof(j->linear_min));
    memcpy(j->linear_max, max_v, sizeof(j->linear_max));
}

/// @brief Set the joint's per-axis angular pose limits (radians) from two Vec3 handles.
/// @details Limits are relative to the creation pose in body A's joint frame; equal min/max locks
///          that rotational axis. Traps on non-Vec3 inputs.
/// @param joint SixDofJoint3D handle to modify.
/// @param min_obj Vec3 lower angular bounds in radians.
/// @param max_obj Vec3 upper angular bounds in radians.
void rt_sixdof_joint3d_set_angular_limits(void *joint, void *min_obj, void *max_obj) {
    double min_v[3];
    double max_v[3];
    rt_sixdof_joint3d *j =
        (rt_sixdof_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID);
    if (!j)
        return;
    if (!joint3d_read_vec3(min_obj, min_v) || !joint3d_read_vec3(max_obj, max_v)) {
        rt_trap("SixDofJoint3D.SetAngularLimits: min and max must be finite Vec3 values");
        return;
    }
    joint3d_canonicalize_limits(min_v, max_v);
    memcpy(j->angular_min, min_v, sizeof(j->angular_min));
    memcpy(j->angular_max, max_v, sizeof(j->angular_max));
}

/// @brief Solve one 6DOF constraint step: enforce linear/pose-angular box limits and any motor.
/// @details Projects the anchor gap back inside the linear limits, zeroes relative velocity on
/// locked
///          linear axes, holds relative pose angles inside angular limits, then drives the motor.
/// @param j Six-degree-of-freedom joint constraint to solve.
/// @param dt Physics step duration, reserved by the position-based solver interface.
static void solve_sixdof(rt_sixdof_joint3d *j, double dt) {
    (void)dt;
    if (!j)
        return;
    /* Linear limits, locked axes, and the motor all operate in body A's joint
     * frame — the same frame the angular limits use — so constraints authored
     * against the creation pose keep tracking body A as it rotates. */
    joint3d_correct_anchor_pair_limited_frame(j->body_a,
                                              j->body_b,
                                              j->local_anchor_a,
                                              j->local_anchor_b,
                                              j->linear_min,
                                              j->linear_max,
                                              j->body_a->orientation);
    joint3d_remove_relative_linear_velocity_locked_axes_frame(
        j->body_a, j->body_b, j->linear_min, j->linear_max, j->body_a->orientation);
    sixdof_joint_apply_angular_limits(j);
    if (j->linear_motor_enabled)
        sixdof_joint_apply_linear_motor(j);
}

/*==========================================================================
 * Generic joint solver dispatch
 *=========================================================================*/

typedef struct {
    void *vptr;
    rt_body3d_kinematics *body_a;
    rt_body3d_kinematics *body_b;
} rt_joint3d_body_pair_view;

/// @brief Return the two body handles retained by a validated joint object.
/// @details Every concrete joint payload starts with `vptr`, `body_a`, and
///   `body_b`; this accessor first checks that the supplied runtime type tag
///   matches the concrete class and then reads that common prefix. It gives
///   World3D one safe place to validate same-world membership and purge joints
///   that mention a removed body.
/// @param joint Candidate concrete joint handle.
/// @param joint_type Expected `RT_JOINT_*` discriminator.
/// @param out_body_a Optional output receiving the first borrowed Body3D handle.
/// @param out_body_b Optional output receiving the second borrowed Body3D handle.
/// @return One when the handle class matches @p joint_type and outputs are populated, otherwise
/// zero.
int rt_joint3d_get_bodies(void *joint, int32_t joint_type, void **out_body_a, void **out_body_b) {
    int matches = 0;
    if (out_body_a)
        *out_body_a = NULL;
    if (out_body_b)
        *out_body_b = NULL;
    if (!joint)
        return 0;
    if (joint_type == RT_JOINT_DISTANCE)
        matches = rt_g3d_has_class(joint, RT_G3D_DISTANCEJOINT3D_CLASS_ID);
    else if (joint_type == RT_JOINT_SPRING)
        matches = rt_g3d_has_class(joint, RT_G3D_SPRINGJOINT3D_CLASS_ID);
    else if (joint_type == RT_JOINT_HINGE)
        matches = rt_g3d_has_class(joint, RT_G3D_HINGEJOINT3D_CLASS_ID);
    else if (joint_type == RT_JOINT_ROPE)
        matches = rt_g3d_has_class(joint, RT_G3D_ROPEJOINT3D_CLASS_ID);
    else if (joint_type == RT_JOINT_SIXDOF)
        matches = rt_g3d_has_class(joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID);
    if (!matches)
        return 0;
    rt_joint3d_body_pair_view *pair = (rt_joint3d_body_pair_view *)joint;
    if (out_body_a)
        *out_body_a = pair->body_a;
    if (out_body_b)
        *out_body_b = pair->body_b;
    return 1;
}

/// @brief Dispatch the constraint solver for a joint based on its type.
/// @details Called by the physics world during each step. Dispatches to
///          the concrete joint solver based on joint_type.
/// @param joint      Opaque joint handle.
/// @param joint_type RT_JOINT_* type code.
/// @param dt         Physics timestep in seconds.
void rt_joint3d_solve(void *joint, int32_t joint_type, double dt) {
    if (!joint)
        return;
    /* Skip pairs with no active driver (both asleep / static / motionless
     * kinematic); wake both dynamics otherwise so a sleeping partner rejoins
     * the solve instead of silently discarding impulses. rt_joint3d_get_bodies
     * validates the class tag before the pair prefix is trusted. */
    {
        void *gate_a = NULL;
        void *gate_b = NULL;
        if (rt_joint3d_get_bodies(joint, joint_type, &gate_a, &gate_b) &&
            !joint3d_pair_begin_solve((rt_body3d_kinematics *)gate_a,
                                      (rt_body3d_kinematics *)gate_b))
            return;
    }
    if (joint_type == RT_JOINT_DISTANCE && rt_g3d_has_class(joint, RT_G3D_DISTANCEJOINT3D_CLASS_ID))
        solve_distance((rt_distance_joint3d *)joint, dt);
    else if (joint_type == RT_JOINT_SPRING &&
             rt_g3d_has_class(joint, RT_G3D_SPRINGJOINT3D_CLASS_ID))
        solve_spring((rt_spring_joint3d *)joint, dt);
    else if (joint_type == RT_JOINT_HINGE && rt_g3d_has_class(joint, RT_G3D_HINGEJOINT3D_CLASS_ID))
        solve_hinge((rt_hinge_joint3d *)joint, dt);
    else if (joint_type == RT_JOINT_ROPE && rt_g3d_has_class(joint, RT_G3D_ROPEJOINT3D_CLASS_ID))
        solve_rope((rt_rope_joint3d *)joint, dt);
    else if (joint_type == RT_JOINT_SIXDOF &&
             rt_g3d_has_class(joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID))
        solve_sixdof((rt_sixdof_joint3d *)joint, dt);
}

/*==========================================================================
 * Joint readback (ADR 0233)
 *=========================================================================*/

/// @brief Shared borrowed-body accessor for the per-class BodyA/BodyB getters.
/// @param joint Candidate joint handle.
/// @param joint_type Expected `RT_JOINT_*` discriminator.
/// @param want_b Nonzero to select body B, zero for body A.
/// @return Borrowed Body3D handle, or NULL for a class mismatch.
static void *joint3d_body_readback(void *joint, int32_t joint_type, int want_b) {
    void *body_a = NULL;
    void *body_b = NULL;
    if (!rt_joint3d_get_bodies(joint, joint_type, &body_a, &body_b))
        return NULL;
    return want_b ? body_b : body_a;
}

/// @brief `DistanceJoint3D.BodyA` — borrowed first body. @param joint Joint handle.
void *rt_distance_joint3d_get_body_a(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_DISTANCE, 0);
}

/// @brief `DistanceJoint3D.BodyB` — borrowed second body. @param joint Joint handle.
void *rt_distance_joint3d_get_body_b(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_DISTANCE, 1);
}

/// @brief `SpringJoint3D.BodyA` — borrowed first body. @param joint Joint handle.
void *rt_spring_joint3d_get_body_a(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_SPRING, 0);
}

/// @brief `SpringJoint3D.BodyB` — borrowed second body. @param joint Joint handle.
void *rt_spring_joint3d_get_body_b(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_SPRING, 1);
}

/// @brief `HingeJoint3D.BodyA` — borrowed first body. @param joint Joint handle.
void *rt_hinge_joint3d_get_body_a(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_HINGE, 0);
}

/// @brief `HingeJoint3D.BodyB` — borrowed second body. @param joint Joint handle.
void *rt_hinge_joint3d_get_body_b(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_HINGE, 1);
}

/// @brief `RopeJoint3D.BodyA` — borrowed first body. @param joint Joint handle.
void *rt_rope_joint3d_get_body_a(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_ROPE, 0);
}

/// @brief `RopeJoint3D.BodyB` — borrowed second body. @param joint Joint handle.
void *rt_rope_joint3d_get_body_b(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_ROPE, 1);
}

/// @brief `SixDofJoint3D.BodyA` — borrowed first body. @param joint Joint handle.
void *rt_sixdof_joint3d_get_body_a(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_SIXDOF, 0);
}

/// @brief `SixDofJoint3D.BodyB` — borrowed second body. @param joint Joint handle.
void *rt_sixdof_joint3d_get_body_b(void *joint) {
    return joint3d_body_readback(joint, RT_JOINT_SIXDOF, 1);
}

/// @brief Checked hinge downcast shared by the hinge readback getters.
/// @param joint Candidate joint handle.
/// @return Typed hinge pointer, or NULL for a class mismatch.
static rt_hinge_joint3d *hinge_joint_readback_checked(void *joint) {
    if (!rt_g3d_has_class(joint, RT_G3D_HINGEJOINT3D_CLASS_ID))
        return NULL;
    return (rt_hinge_joint3d *)joint;
}

/// @brief `HingeJoint3D.MotorEnabled` — retained motor flag. @param joint Joint handle.
int8_t rt_hinge_joint3d_get_motor_enabled(void *joint) {
    rt_hinge_joint3d *j = hinge_joint_readback_checked(joint);
    return j && j->motor_enabled ? 1 : 0;
}

/// @brief `HingeJoint3D.MotorTargetVelocity` — retained target rad/s. @param joint Joint handle.
double rt_hinge_joint3d_get_motor_target_velocity(void *joint) {
    rt_hinge_joint3d *j = hinge_joint_readback_checked(joint);
    return j ? j->motor_target_velocity : 0.0;
}

/// @brief `HingeJoint3D.MotorMaxImpulse` — retained motor bound. @param joint Joint handle.
double rt_hinge_joint3d_get_motor_max_impulse(void *joint) {
    rt_hinge_joint3d *j = hinge_joint_readback_checked(joint);
    return j ? j->motor_max_impulse : 0.0;
}

/// @brief `HingeJoint3D.LimitsEnabled` — whether angle limits are active. @param joint Joint handle.
int8_t rt_hinge_joint3d_get_limits_enabled(void *joint) {
    rt_hinge_joint3d *j = hinge_joint_readback_checked(joint);
    return j && j->has_limits ? 1 : 0;
}

/// @brief `HingeJoint3D.LimitMin` — retained lower angle limit (radians). @param joint Joint handle.
double rt_hinge_joint3d_get_limit_min(void *joint) {
    rt_hinge_joint3d *j = hinge_joint_readback_checked(joint);
    return j && j->has_limits ? j->angle_min : 0.0;
}

/// @brief `HingeJoint3D.LimitMax` — retained upper angle limit (radians). @param joint Joint handle.
double rt_hinge_joint3d_get_limit_max(void *joint) {
    rt_hinge_joint3d *j = hinge_joint_readback_checked(joint);
    return j && j->has_limits ? j->angle_max : 0.0;
}

/// @brief Checked 6DOF downcast shared by the sixdof readback getters.
/// @param joint Candidate joint handle.
/// @return Typed joint pointer, or NULL for a class mismatch.
static rt_sixdof_joint3d *sixdof_joint_readback_checked(void *joint) {
    return (rt_sixdof_joint3d *)rt_g3d_checked_or_null(joint, RT_G3D_SIXDOFJOINT3D_CLASS_ID);
}

/// @brief Box a stored double triple as a fresh Vec3 (origin for NULL joints).
/// @param values Borrowed triple, or NULL.
/// @return Newly allocated Vec3 snapshot.
static void *joint3d_vec3_snapshot(const double *values) {
    if (!values)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(values[0], values[1], values[2]);
}

/// @brief `SixDofJoint3D.LinearLimitMin` — fresh Vec3 lower bounds. @param joint Joint handle.
void *rt_sixdof_joint3d_get_linear_limit_min(void *joint) {
    rt_sixdof_joint3d *j = sixdof_joint_readback_checked(joint);
    return joint3d_vec3_snapshot(j ? j->linear_min : NULL);
}

/// @brief `SixDofJoint3D.LinearLimitMax` — fresh Vec3 upper bounds. @param joint Joint handle.
void *rt_sixdof_joint3d_get_linear_limit_max(void *joint) {
    rt_sixdof_joint3d *j = sixdof_joint_readback_checked(joint);
    return joint3d_vec3_snapshot(j ? j->linear_max : NULL);
}

/// @brief `SixDofJoint3D.AngularLimitMin` — fresh Vec3 lower bounds (radians).
/// @param joint Joint handle.
void *rt_sixdof_joint3d_get_angular_limit_min(void *joint) {
    rt_sixdof_joint3d *j = sixdof_joint_readback_checked(joint);
    return joint3d_vec3_snapshot(j ? j->angular_min : NULL);
}

/// @brief `SixDofJoint3D.AngularLimitMax` — fresh Vec3 upper bounds (radians).
/// @param joint Joint handle.
void *rt_sixdof_joint3d_get_angular_limit_max(void *joint) {
    rt_sixdof_joint3d *j = sixdof_joint_readback_checked(joint);
    return joint3d_vec3_snapshot(j ? j->angular_max : NULL);
}

/// @brief `SixDofJoint3D.LinearMotorEnabled` — retained motor flag. @param joint Joint handle.
int8_t rt_sixdof_joint3d_get_linear_motor_enabled(void *joint) {
    rt_sixdof_joint3d *j = sixdof_joint_readback_checked(joint);
    return j && j->linear_motor_enabled ? 1 : 0;
}

/// @brief `SixDofJoint3D.LinearMotorVelocity` — fresh Vec3 target velocity.
/// @param joint Joint handle.
void *rt_sixdof_joint3d_get_linear_motor_velocity(void *joint) {
    rt_sixdof_joint3d *j = sixdof_joint_readback_checked(joint);
    return joint3d_vec3_snapshot(j ? j->linear_motor_velocity : NULL);
}

/// @brief `SixDofJoint3D.LinearMotorMaxImpulse` — retained motor bound. @param joint Joint handle.
double rt_sixdof_joint3d_get_linear_motor_max_impulse(void *joint) {
    rt_sixdof_joint3d *j = sixdof_joint_readback_checked(joint);
    return j ? j->linear_motor_max_impulse : 0.0;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
