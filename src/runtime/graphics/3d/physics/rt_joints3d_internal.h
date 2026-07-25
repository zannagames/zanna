//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_joints3d_internal.h
// Purpose: Shared vector/quaternion/anchor math primitives for the 3D joint
//          solvers. Split out of rt_joints3d.c so the math library and the
//          per-joint solvers (distance/spring/hinge/etc.) can live in separate
//          translation units.
//
// Key invariants:
//   - These helpers are engine-internal; the header must not be included
//     outside the physics/ directory.
//   - All clamp/sanitize helpers map non-finite input to safe defaults so the
//     solver never propagates NaN.
//   - Body access uses the private shared rt_body3d_kinematics prefix view.
//
// Ownership/Lifetime:
//   - Pure functions over caller-owned double arrays / body views; no
//     allocation or ownership transfer.
//
// Links: src/runtime/graphics/3d/physics/rt_joints3d.c (joint solvers + API),
//        src/runtime/graphics/3d/physics/rt_joints3d_math.c (definitions)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_joints3d_internal.h
 * @brief Declares sanitized math and Body3D bridges shared by joint solvers.
 *
 * These allocation-free helpers operate on caller-owned vectors, quaternions,
 * matrices, and the private Body3D kinematic prefix. They centralize finite
 * input handling, bounded corrections, anchor transforms, effective inertia,
 * and pair wake/broadphase coordination for all concrete joint types.
 */

#pragma once

#include "rt_body3d_kinematics_internal.h"

//=============================================================================
// Joint math limits
//=============================================================================

#define RT_JOINT3D_MAX_PARAM 1.0e9
#define RT_JOINT3D_MAX_FORCE 1.0e9
#define RT_JOINT3D_MAX_COORD 1.0e12
#define RT_JOINT3D_MAX_DT 1.0
#define RT_JOINT3D_TWO_PI 6.28318530717958647692

/* Body pose/velocity access uses a private prefix view whose layout is asserted to match the
 * private rt_body3d payload in rt_physics3d.c. */

/// @brief Minimal private view of a runtime Mat4 payload.
typedef struct {
    ///< Sixteen matrix elements in the runtime's established layout.
    double m[16];
} joint3d_mat4_view;

//=============================================================================
// Vector / quaternion / anchor math (defined in rt_joints3d_math.c)
//=============================================================================

/// @brief Sanitize a joint parameter to the supported non-negative range.
/// @param value Candidate parameter.
/// @return Finite value clamped to `[0, RT_JOINT3D_MAX_PARAM]`.
double joint3d_sanitize_nonnegative(double value);

/// @brief Clamp a force, impulse, or velocity correction to the solver's safe range.
/// @param value Candidate correction.
/// @return Finite signed value bounded by `RT_JOINT3D_MAX_FORCE`.
double joint3d_clamp_force(double value);

/// @brief Clamp a position coordinate or positional correction to the solver's safe range.
/// @param value Candidate coordinate.
/// @return Finite signed value bounded by `RT_JOINT3D_MAX_COORD`.
double joint3d_clamp_coord(double value);

/// @brief Sanitize a physics step duration.
/// @param dt Candidate duration in seconds.
/// @return Finite duration clamped to `[0, RT_JOINT3D_MAX_DT]`.
double joint3d_sanitize_dt(double dt);

/// @brief Test whether every component of a three-vector is finite.
/// @param v Three-component vector.
/// @return One when @p v is non-null and finite, otherwise zero.
int joint3d_vec3_all_finite(const double *v);

/// @brief Replace non-finite vector components with zero and clamp finite extremes.
/// @param v Three-component vector to sanitize in place.
void joint3d_vec3_sanitize(double *v);

/// @brief Initialize a sanitized three-component vector.
/// @param dst Destination vector.
/// @param x X component.
/// @param y Y component.
/// @param z Z component.
void joint3d_vec3_set(double *dst, double x, double y, double z);

/// @brief Subtract two vectors component-wise.
/// @param a Left-hand vector.
/// @param b Right-hand vector.
/// @param out Receives `a - b`.
void joint3d_vec3_sub(const double *a, const double *b, double *out);

/// @brief Compute a three-dimensional dot product.
/// @param a First vector.
/// @param b Second vector.
/// @return Scalar dot product.
double joint3d_vec3_dot(const double *a, const double *b);

/// @brief Compute a robust Euclidean length from three scalar components.
/// @param x X component.
/// @param y Y component.
/// @param z Z component.
/// @return Finite magnitude, or zero for unusable input.
double joint3d_len3(double x, double y, double z);

/// @brief Compute a robust Euclidean length of a three-vector.
/// @param v Three-component vector.
/// @return Finite magnitude, or zero for an unavailable vector.
double joint3d_vec3_len(const double *v);

/// @brief Normalize a three-vector in place.
/// @param v Vector to normalize.
/// @return One for a finite non-degenerate vector, otherwise zero.
int joint3d_vec3_normalize(double *v);

/// @brief Read a finite runtime Vec3 into raw storage.
/// @param obj Candidate Vec3 handle.
/// @param out Receives three sanitized components.
/// @return One for a valid finite Vec3, otherwise zero.
int joint3d_read_vec3(void *obj, double *out);

/// @brief Order each component pair so lower limits do not exceed upper limits.
/// @param min_v In/out three-component lower bounds.
/// @param max_v In/out three-component upper bounds.
void joint3d_canonicalize_limits(double *min_v, double *max_v);

/// @brief Validate and borrow the private payload of a runtime Mat4.
/// @param obj Candidate Mat4 handle.
/// @return Borrowed matrix view, or NULL for an invalid handle.
joint3d_mat4_view *joint3d_mat4_checked(void *obj);

/// @brief Read the translation component of a finite runtime Mat4.
/// @param obj Candidate Mat4 handle.
/// @param out Receives the three-component translation.
/// @return One when the matrix and translation are finite, otherwise zero.
int joint3d_read_mat4_translation(void *obj, double *out);

/// @brief Compute the XYZW Hamilton product of two quaternions.
/// @param a Left-hand quaternion.
/// @param b Right-hand quaternion.
/// @param out Receives the product.
void joint3d_quat_mul(const double *a, const double *b, double *out);

/// @brief Compute an XYZW quaternion conjugate.
/// @param q Input quaternion.
/// @param out Receives the conjugate.
void joint3d_quat_conjugate(const double *q, double *out);

/// @brief Normalize an XYZW quaternion or replace it with identity.
/// @param q Quaternion to normalize in place.
void joint3d_quat_normalize(double *q);

/// @brief Construct a quaternion from a normalized axis and angle.
/// @param axis Three-component rotation axis.
/// @param angle Rotation angle in radians.
/// @param out Receives the normalized XYZW quaternion.
void joint3d_quat_from_axis_angle(const double *axis, double angle, double *out);

/// @brief Prepend a world-axis rotation to an orientation quaternion.
/// @param orientation In/out XYZW orientation.
/// @param axis Three-component world rotation axis.
/// @param angle Rotation angle in radians.
void joint3d_quat_prepend_axis_angle(double *orientation, const double *axis, double angle);

/// @brief Convert an orientation quaternion to a shortest rotation vector.
/// @param q Input XYZW quaternion.
/// @param out Receives axis multiplied by signed angle in radians.
void joint3d_quat_to_rotation_vector(const double *q, double *out);

/// @brief Rotate a three-vector by an XYZW quaternion.
/// @param q Rotation quaternion.
/// @param v Input vector.
/// @param out Receives the rotated vector.
void joint3d_quat_rotate_vec3(const double *q, const double *v, double *out);
/// @brief Transform a body-local anchor into world space.
/// @param body Body3D kinematic pose.
/// @param local_anchor Three-component local anchor.
/// @param out Receives the world-space anchor.
void joint3d_world_anchor(const rt_body3d_kinematics *body,
                          const double *local_anchor,
                          double *out);

/// @brief Transform a world-space point into a body's local frame.
/// @param body Body3D kinematic pose.
/// @param world_point Three-component world point.
/// @param out Receives the local-space point.
void joint3d_local_from_world(const rt_body3d_kinematics *body,
                              const double *world_point,
                              double *out);

/// @brief Multiply a world-space vector by a body's world inverse-inertia tensor.
/// @param body Body3D kinematic and principal-inertia view.
/// @param v World-space vector.
/// @param out Receives the transformed vector.
void joint3d_world_inv_inertia_mul(const rt_body3d_kinematics *body,
                                   const double *v,
                                   double *out);

/// @brief Compute effective inverse inertia about a world-space axis.
/// @param body Body3D kinematic and principal-inertia view.
/// @param axis World-space axis.
/// @return Non-negative scalar `axis dot I^-1 axis`, or zero for unusable state.
double joint3d_effective_inv_inertia_about_axis(const rt_body3d_kinematics *body,
                                                const double *axis);

/// @brief Rotate a body-local axis into normalized world space.
/// @param body Body3D kinematic pose.
/// @param local_axis Three-component local axis.
/// @param out Receives a normalized world axis.
void joint3d_world_axis_from_local(const rt_body3d_kinematics *body,
                                   const double *local_axis,
                                   double *out);
/* Sleep/wake + broadphase bridge (defined in rt_physics3d.c, where the full
 * rt_body3d layout is known). Joint solvers only see the kinematics prefix
 * view, so they cannot touch motion_mode/is_sleeping/broadphase directly. */

/// @brief Gate a joint solve on pair activity and wake the pair when it runs.
/// @details Returns 0 (skip solving) when neither endpoint can drive the
///   constraint: both dynamics asleep, static anchors, and motionless
///   kinematics don't need the joint enforced this substep. When any endpoint
///   is an active driver, both dynamic endpoints are woken (so a sleeping
///   partner picks impulses up again) and 1 is returned.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @return One when the pair should be solved, otherwise zero.
int joint3d_pair_begin_solve(rt_body3d_kinematics *body_a, rt_body3d_kinematics *body_b);

/// @brief Record that a joint position-correction moved @p body, so cached
///   query-broadphase entries revalidate against the new pose.
/// @param body Body3D endpoint whose position changed.
void joint3d_mark_body_moved(rt_body3d_kinematics *body);

/// @brief Correct two world anchors toward coincidence with configurable stiffness.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param local_anchor_a Anchor expressed in body A's local frame.
/// @param local_anchor_b Anchor expressed in body B's local frame.
/// @param stiffness Correction fraction applied this solve.
void joint3d_correct_anchor_pair(rt_body3d_kinematics *body_a,
                                 rt_body3d_kinematics *body_b,
                                 const double *local_anchor_a,
                                 const double *local_anchor_b,
                                 double stiffness);

/// @brief Keep anchor separation within per-axis world-space linear limits.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param local_anchor_a Anchor expressed in body A's local frame.
/// @param local_anchor_b Anchor expressed in body B's local frame.
/// @param linear_min Three-component lower separation bounds.
/// @param linear_max Three-component upper separation bounds.
void joint3d_correct_anchor_pair_limited(rt_body3d_kinematics *body_a,
                                         rt_body3d_kinematics *body_b,
                                         const double *local_anchor_a,
                                         const double *local_anchor_b,
                                         const double *linear_min,
                                         const double *linear_max);

/// @brief Keep anchor separation within per-axis limits expressed in a quaternion frame.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param local_anchor_a Anchor expressed in body A's local frame.
/// @param local_anchor_b Anchor expressed in body B's local frame.
/// @param linear_min Three-component lower frame-space bounds.
/// @param linear_max Three-component upper frame-space bounds.
/// @param frame_quat XYZW quaternion rotating frame-space vectors to world space.
void joint3d_correct_anchor_pair_limited_frame(rt_body3d_kinematics *body_a,
                                               rt_body3d_kinematics *body_b,
                                               const double *local_anchor_a,
                                               const double *local_anchor_b,
                                               const double *linear_min,
                                               const double *linear_max,
                                               const double *frame_quat);

/// @brief Remove relative linear velocity along axes locked in a quaternion frame.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param linear_min Three-component lower frame-space bounds.
/// @param linear_max Three-component upper frame-space bounds.
/// @param frame_quat XYZW quaternion rotating frame-space axes to world space.
void joint3d_remove_relative_linear_velocity_locked_axes_frame(rt_body3d_kinematics *body_a,
                                                               rt_body3d_kinematics *body_b,
                                                               const double *linear_min,
                                                               const double *linear_max,
                                                               const double *frame_quat);

/// @brief Remove a fraction of all relative linear velocity between two bodies.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param amount Correction fraction.
void joint3d_remove_relative_linear_velocity(rt_body3d_kinematics *body_a,
                                             rt_body3d_kinematics *body_b,
                                             double amount);

/// @brief Remove relative linear velocity along world axes with equal lower and upper limits.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param linear_min Three-component lower limits.
/// @param linear_max Three-component upper limits.
void joint3d_remove_relative_linear_velocity_locked_axes(rt_body3d_kinematics *body_a,
                                                         rt_body3d_kinematics *body_b,
                                                         const double *linear_min,
                                                         const double *linear_max);

/// @brief Remove relative angular velocity perpendicular to an optional allowed axis.
/// @param body_a First Body3D endpoint.
/// @param body_b Second Body3D endpoint.
/// @param allowed_axis Optional normalized world axis whose relative spin is preserved.
void joint3d_remove_relative_angular_velocity(rt_body3d_kinematics *body_a,
                                              rt_body3d_kinematics *body_b,
                                              const double *allowed_axis);

/// @brief True if a body view is solvable (finite state, non-negative inv mass).
/// Defined in rt_joints3d.c.
/// @param body Borrowed Body3D kinematic prefix to validate.
/// @return One when solver-consumed state is finite and inverse mass is non-negative.
int joint3d_body_is_finite(const rt_body3d_kinematics *body);
