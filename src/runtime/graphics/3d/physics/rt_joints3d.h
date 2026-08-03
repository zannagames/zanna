//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_joints3d.h
// Purpose: 3D physics joint constraints.
//
// Key invariants:
//   - Joints reference two Body3D objects; if either is freed, joint is inert.
//   - Distance joints maintain a fixed separation between centers.
//   - Spring joints apply Hooke's law force toward rest length.
//   - Hinge/SixDof joints keep their authored frame anchors coincident.
//   - Rope joints only constrain bodies when separation exceeds MaxLength.
//   - Joints are solved iteratively after collision resolution in step().
//
// Ownership/Lifetime:
//   - GC-managed via rt_obj_new_i64.
//
// Links: rt_joints3d.c, rt_physics3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_joints3d.h
 * @brief Declares runtime 3D joint construction, configuration, and solving.
 *
 * The API provides distance, spring, hinge, rope, and six-degree-of-freedom
 * constraints between retained Body3D handles. Public setters configure each
 * concrete joint; internal helpers expose body membership and dispatch one
 * solver substep by `RT_JOINT_*` discriminator.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Distance joint — maintains fixed distance between two body centers */
/// @brief Create a fixed-length distance constraint between two 3D bodies.
/// @param body_a First Body3D handle.
/// @param body_b Second Body3D handle.
/// @param distance Requested non-negative target separation.
/// @return Newly allocated DistanceJoint3D handle, or NULL after validation or allocation failure.
void *rt_distance_joint3d_new(void *body_a, void *body_b, double distance);

/// @brief Read the target distance.
/// @param joint DistanceJoint3D handle to inspect.
/// @return Non-negative target distance, or zero for an invalid handle.
double rt_distance_joint3d_get_distance(void *joint);

/// @brief Change the target distance at runtime.
/// @param joint DistanceJoint3D handle to modify.
/// @param distance Requested non-negative target separation.
void rt_distance_joint3d_set_distance(void *joint, double distance);

/* Spring joint — Hooke's law force toward rest length */
/// @brief Create a 3D spring constraint with rest length, stiffness, and damping coefficients.
/// @param body_a First Body3D handle.
/// @param body_b Second Body3D handle.
/// @param rest_length Natural zero-force length.
/// @param stiffness Non-negative Hooke's-law spring constant.
/// @param damping Non-negative relative-velocity damping coefficient.
/// @return Newly allocated SpringJoint3D handle, or NULL after validation or allocation failure.
void *rt_spring_joint3d_new(
    void *body_a, void *body_b, double rest_length, double stiffness, double damping);
/// @brief Read the spring stiffness.
/// @param joint SpringJoint3D handle to inspect.
/// @return Non-negative stiffness, or zero for an invalid handle.
double rt_spring_joint3d_get_stiffness(void *joint);

/// @brief Set the spring stiffness.
/// @param joint SpringJoint3D handle to modify.
/// @param stiffness Requested non-negative spring constant.
void rt_spring_joint3d_set_stiffness(void *joint, double stiffness);

/// @brief Read the spring damping coefficient.
/// @param joint SpringJoint3D handle to inspect.
/// @return Non-negative damping coefficient, or zero for an invalid handle.
double rt_spring_joint3d_get_damping(void *joint);

/// @brief Set the spring damping coefficient.
/// @param joint SpringJoint3D handle to modify.
/// @param damping Requested non-negative damping coefficient.
void rt_spring_joint3d_set_damping(void *joint, double damping);

/// @brief Read the spring rest length.
/// @param joint SpringJoint3D handle to inspect.
/// @return Non-negative rest length, or zero for an invalid handle.
double rt_spring_joint3d_get_rest_length(void *joint);

/* Hinge joint — keeps two local anchors together while allowing twist around an axis */
/// @brief Create a hinge-like anchor constraint around a normalized world axis.
/// @param body_a First Body3D handle.
/// @param body_b Second Body3D handle.
/// @param anchor Finite Vec3 world-space pivot.
/// @param axis Nonzero finite Vec3 world-space hinge axis.
/// @return Newly allocated HingeJoint3D handle, or NULL after validation or allocation failure.
void *rt_hinge_joint3d_new(void *body_a, void *body_b, void *anchor, void *axis);

/// @brief Configure a hinge motor driving rotation about the axis toward a target
///   angular velocity (rad/s), bounded by a max-impulse strength.
/// @param joint HingeJoint3D handle to modify.
/// @param enabled Nonzero to enable motor impulses.
/// @param target_velocity Desired relative angular velocity in radians per second.
/// @param max_impulse Non-negative per-step motor strength bound.
void rt_hinge_joint3d_set_motor(void *joint,
                                int8_t enabled,
                                double target_velocity,
                                double max_impulse);
/// @brief Current signed hinge angle (radians) about the axis; 0 at creation.
/// @param joint HingeJoint3D handle to inspect.
/// @return Signed angle in radians, or zero for an invalid handle.
double rt_hinge_joint3d_get_angle(void *joint);

/// @brief Constrain the hinge to [min, max] radians; non-finite values disable the limit.
/// @param joint HingeJoint3D handle to modify.
/// @param min_angle Requested minimum signed angle in radians.
/// @param max_angle Requested maximum signed angle in radians.
void rt_hinge_joint3d_set_limits(void *joint, double min_angle, double max_angle);

/* Rope joint — enforces only a maximum center-to-center distance */
/// @brief Create a maximum-distance rope constraint between two 3D bodies.
/// @param body_a First Body3D handle.
/// @param body_b Second Body3D handle.
/// @param max_length Maximum permitted center-to-center separation.
/// @return Newly allocated RopeJoint3D handle, or NULL after validation or allocation failure.
void *rt_rope_joint3d_new(void *body_a, void *body_b, double max_length);

/// @brief Read the maximum rope length.
/// @param joint RopeJoint3D handle to inspect.
/// @return Non-negative maximum length, or zero for an invalid handle.
double rt_rope_joint3d_get_max_length(void *joint);

/// @brief Change the maximum rope length at runtime.
/// @param joint RopeJoint3D handle to modify.
/// @param max_length Requested non-negative maximum separation.
void rt_rope_joint3d_set_max_length(void *joint, double max_length);

/* SixDof joint — configurable frame-anchor constraint */
/// @brief Create a configurable joint that locks the two frame anchors together by default.
/// @param body_a First Body3D handle.
/// @param body_b Second Body3D handle.
/// @param frame_a Mat4 whose translation defines body A's local anchor.
/// @param frame_b Mat4 whose translation defines body B's local anchor.
/// @return Newly allocated SixDofJoint3D handle, or NULL after validation or allocation failure.
void *rt_sixdof_joint3d_new(void *body_a, void *body_b, void *frame_a, void *frame_b);

/// @brief Set allowed frame-anchor separation along each local/world axis.
/// @param joint SixDofJoint3D handle to modify.
/// @param min Vec3 lower bounds in body A's joint frame.
/// @param max Vec3 upper bounds in body A's joint frame.
void rt_sixdof_joint3d_set_linear_limits(void *joint, void *min, void *max);

/// @brief Set allowed relative pose angle along each SixDof joint-frame axis.
/// @param joint SixDofJoint3D handle to modify.
/// @param min Vec3 lower angular bounds in radians.
/// @param max Vec3 upper angular bounds in radians.
void rt_sixdof_joint3d_set_angular_limits(void *joint, void *min, void *max);

/// @brief Configure a linear motor driving relative velocity (Vec3 per world axis)
///   along unlocked axes, bounded by a max-impulse strength.
/// @param joint SixDofJoint3D handle to modify.
/// @param enabled Nonzero to enable the linear motor.
/// @param velocity Vec3 target relative velocity in body A's joint frame.
/// @param max_impulse Non-negative per-axis correction bound.
void rt_sixdof_joint3d_set_linear_motor(void *joint,
                                        int8_t enabled,
                                        void *velocity,
                                        double max_impulse);

/* Joint solving (called from physics world step) */
/// @brief Return the two Body3D handles retained by a joint of the supplied RT_JOINT_* type.
/// @details Used by World3D to validate that a joint only references bodies registered
///   in the same world and to purge constraints when a body is removed. Returns 0
///   if @p joint does not match @p joint_type.
/// @param joint Candidate concrete joint handle.
/// @param joint_type Expected `RT_JOINT_*` discriminator.
/// @param out_body_a Optional output receiving the first borrowed Body3D handle.
/// @param out_body_b Optional output receiving the second borrowed Body3D handle.
/// @return One when the class matches @p joint_type, otherwise zero.
int rt_joint3d_get_bodies(void *joint, int32_t joint_type, void **out_body_a, void **out_body_b);

/// @brief Solve one substep of a joint constraint (called internally from `world_step`).
/// @param joint Concrete joint handle.
/// @param joint_type Matching `RT_JOINT_*` discriminator.
/// @param dt Physics step duration in seconds.
void rt_joint3d_solve(void *joint, int32_t joint_type, double dt);

/* Joint types */
#define RT_JOINT_DISTANCE 0
#define RT_JOINT_SPRING 1
#define RT_JOINT_HINGE 2
#define RT_JOINT_ROPE 3
#define RT_JOINT_SIXDOF 4

/* Joint readback (ADR 0233). BodyA/BodyB getters return the borrowed retained
 * bodies (NULL on class mismatch); Vec3-returning getters allocate fresh
 * snapshots. */
/// @brief Borrowed first body. @param joint DistanceJoint3D handle.
void *rt_distance_joint3d_get_body_a(void *joint);
/// @brief Borrowed second body. @param joint DistanceJoint3D handle.
void *rt_distance_joint3d_get_body_b(void *joint);
/// @brief Borrowed first body. @param joint SpringJoint3D handle.
void *rt_spring_joint3d_get_body_a(void *joint);
/// @brief Borrowed second body. @param joint SpringJoint3D handle.
void *rt_spring_joint3d_get_body_b(void *joint);
/// @brief Borrowed first body. @param joint HingeJoint3D handle.
void *rt_hinge_joint3d_get_body_a(void *joint);
/// @brief Borrowed second body. @param joint HingeJoint3D handle.
void *rt_hinge_joint3d_get_body_b(void *joint);
/// @brief Borrowed first body. @param joint RopeJoint3D handle.
void *rt_rope_joint3d_get_body_a(void *joint);
/// @brief Borrowed second body. @param joint RopeJoint3D handle.
void *rt_rope_joint3d_get_body_b(void *joint);
/// @brief Borrowed first body. @param joint SixDofJoint3D handle.
void *rt_sixdof_joint3d_get_body_a(void *joint);
/// @brief Borrowed second body. @param joint SixDofJoint3D handle.
void *rt_sixdof_joint3d_get_body_b(void *joint);
/// @brief Retained motor flag. @param joint HingeJoint3D handle.
int8_t rt_hinge_joint3d_get_motor_enabled(void *joint);
/// @brief Retained motor target velocity (rad/s). @param joint HingeJoint3D handle.
double rt_hinge_joint3d_get_motor_target_velocity(void *joint);
/// @brief Retained per-step motor impulse bound. @param joint HingeJoint3D handle.
double rt_hinge_joint3d_get_motor_max_impulse(void *joint);
/// @brief Whether angle limits are active. @param joint HingeJoint3D handle.
int8_t rt_hinge_joint3d_get_limits_enabled(void *joint);
/// @brief Retained lower angle limit in radians (0 when disabled). @param joint HingeJoint3D handle.
double rt_hinge_joint3d_get_limit_min(void *joint);
/// @brief Retained upper angle limit in radians (0 when disabled). @param joint HingeJoint3D handle.
double rt_hinge_joint3d_get_limit_max(void *joint);
/// @brief Fresh Vec3 lower linear bounds. @param joint SixDofJoint3D handle.
void *rt_sixdof_joint3d_get_linear_limit_min(void *joint);
/// @brief Fresh Vec3 upper linear bounds. @param joint SixDofJoint3D handle.
void *rt_sixdof_joint3d_get_linear_limit_max(void *joint);
/// @brief Fresh Vec3 lower angular bounds (radians). @param joint SixDofJoint3D handle.
void *rt_sixdof_joint3d_get_angular_limit_min(void *joint);
/// @brief Fresh Vec3 upper angular bounds (radians). @param joint SixDofJoint3D handle.
void *rt_sixdof_joint3d_get_angular_limit_max(void *joint);
/// @brief Retained linear-motor flag. @param joint SixDofJoint3D handle.
int8_t rt_sixdof_joint3d_get_linear_motor_enabled(void *joint);
/// @brief Fresh Vec3 linear-motor target velocity. @param joint SixDofJoint3D handle.
void *rt_sixdof_joint3d_get_linear_motor_velocity(void *joint);
/// @brief Retained linear-motor impulse bound. @param joint SixDofJoint3D handle.
double rt_sixdof_joint3d_get_linear_motor_max_impulse(void *joint);

#ifdef __cplusplus
}
#endif
