//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_physics2d_joint.h
/// @brief Declares Physics2D distance, spring, hinge, and rope constraints plus
///        circle-body construction.
///
/// @details Joint constructors retain two distinct body endpoints. A world
/// retains a registered joint in its growable solver list, and registration
/// requires both endpoints to belong to that world. Distance, hinge, and rope
/// joints use iterative position and velocity projection; spring joints apply
/// bounded Hooke/damping impulses. Physics2D has no angular body state.
///
// File: src/runtime/graphics/2d/rt_physics2d_joint.h
// Purpose: Joint/constraint types for the 2D physics engine: DistanceJoint,
//   SpringJoint, HingeJoint, RopeJoint, and circle collision shapes.
//
// Key invariants:
//   - Joints connect two bodies and are solved iteratively per world step.
//   - World joint storage starts with PH_MAX_JOINTS slots and grows on demand.
//   - Circle bodies use radius for collision (distance-based, no SAT).
//   - Joint type is immutable after creation.
//
// Ownership/Lifetime:
//   - Joint objects are GC-managed (rt_obj_new_i64).
//   - World retains joints; removing releases the reference.
//
// Links: src/runtime/graphics/2d/rt_physics2d_joint.c,
//        src/runtime/graphics/2d/rt_physics2d.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @name Joint type constants
/// @{
#define RT_JOINT_DISTANCE 0
#define RT_JOINT_SPRING 1
#define RT_JOINT_HINGE 2
#define RT_JOINT_ROPE 3
/// @}

//=========================================================================
// Distance Joint
//=========================================================================

/// @brief Create a fixed-length distance constraint between two bodies (rigid rod-like behavior).
/// @param body_a First distinct Physics2D.Body endpoint.
/// @param body_b Second distinct Physics2D.Body endpoint.
/// @param length Requested nonnegative center-to-center rest length; invalid
///               values become zero.
/// @return A new active joint retaining both bodies, or `NULL` for invalid
///         input or allocation failure.
void *rt_physics2d_distance_joint_new(void *body_a, void *body_b, double length);
/// @brief Read the target distance.
/// @param joint Opaque Physics2D.Joint handle.
/// @return The stored target length, or `0.0` for invalid input.
double rt_physics2d_distance_joint_get_length(void *joint);
/// @brief Change the target distance at runtime.
/// @param joint Opaque Physics2D.Joint handle to update.
/// @param length Requested nonnegative length; invalid values become zero.
void rt_physics2d_distance_joint_set_length(void *joint, double length);

//=========================================================================
// Spring Joint
//=========================================================================

/// @brief Create a Hooke's-law spring between two bodies (rest length + stiffness + damping).
/// @param body_a First distinct Physics2D.Body endpoint.
/// @param body_b Second distinct Physics2D.Body endpoint.
/// @param rest_length Requested nonnegative center-to-center rest length.
/// @param stiffness Requested nonnegative Hooke coefficient.
/// @param damping Requested nonnegative axis-velocity damping coefficient.
/// @return A new active joint retaining both bodies, or `NULL` for invalid
///         input or allocation failure.
void *rt_physics2d_spring_joint_new(
    void *body_a, void *body_b, double rest_length, double stiffness, double damping);
/// @brief Read the spring stiffness coefficient.
/// @param joint Opaque Physics2D.Joint handle.
/// @return The stored stiffness, or `0.0` for invalid input.
double rt_physics2d_spring_joint_get_stiffness(void *joint);
/// @brief Set the spring stiffness coefficient.
/// @param joint Opaque Physics2D.Joint handle to update.
/// @param stiffness Requested nonnegative coefficient; invalid values become zero.
void rt_physics2d_spring_joint_set_stiffness(void *joint, double stiffness);
/// @brief Read the spring damping coefficient.
/// @param joint Opaque Physics2D.Joint handle.
/// @return The stored damping coefficient, or `0.0` for invalid input.
double rt_physics2d_spring_joint_get_damping(void *joint);
/// @brief Set the spring damping coefficient.
/// @param joint Opaque Physics2D.Joint handle to update.
/// @param damping Requested nonnegative coefficient; invalid values become zero.
void rt_physics2d_spring_joint_set_damping(void *joint, double damping);

//=========================================================================
// Hinge Joint
//=========================================================================

/// @brief Create a hinge constraint pinning two body-relative points together.
/// @details With no angular state, the hinge constrains linear center offsets
///          so both stored anchor positions coincide.
/// @param body_a First distinct Physics2D.Body endpoint.
/// @param body_b Second distinct Physics2D.Body endpoint.
/// @param anchor_x Initial world anchor X; invalid input uses body A's center.
/// @param anchor_y Initial world anchor Y; invalid input uses body A's center.
/// @return A new active joint retaining both bodies, or `NULL` for invalid
///         input or allocation failure.
void *rt_physics2d_hinge_joint_new(void *body_a, void *body_b, double anchor_x, double anchor_y);
/// @brief Read the angle of the center-to-center vector from body A to body B.
/// @param joint Opaque Physics2D.Joint handle.
/// @return `atan2(center_b.y - center_a.y, center_b.x - center_a.x)` in
///         `[-pi, pi]`, or `0.0` for invalid input.
double rt_physics2d_hinge_joint_get_angle(void *joint);

//=========================================================================
// Rope Joint
//=========================================================================

/// @brief Create a rope constraint that allows free movement up to @p max_length apart.
/// Unlike DistanceJoint, bodies can be closer than max_length without being pulled together.
/// @param body_a First distinct Physics2D.Body endpoint.
/// @param body_b Second distinct Physics2D.Body endpoint.
/// @param max_length Requested nonnegative maximum center separation.
/// @return A new active joint retaining both bodies, or `NULL` for invalid
///         input or allocation failure.
void *rt_physics2d_rope_joint_new(void *body_a, void *body_b, double max_length);
/// @brief Read the maximum allowed separation distance.
/// @param joint Opaque Physics2D.Joint handle.
/// @return The stored maximum length, or `0.0` for invalid input.
double rt_physics2d_rope_joint_get_max_length(void *joint);
/// @brief Set the maximum allowed separation distance.
/// @param joint Opaque Physics2D.Joint handle to update.
/// @param max_length Requested nonnegative maximum; invalid values become zero.
void rt_physics2d_rope_joint_set_max_length(void *joint, double max_length);

//=========================================================================
// Joint Common
//=========================================================================

/// @brief Get the first body in the joint.
/// @param joint Opaque Physics2D.Joint handle.
/// @return The borrowed first body handle, or `NULL` for invalid input.
void *rt_physics2d_joint_get_body_a(void *joint);
/// @brief Get the second body in the joint.
/// @param joint Opaque Physics2D.Joint handle.
/// @return The borrowed second body handle, or `NULL` for invalid input.
void *rt_physics2d_joint_get_body_b(void *joint);
/// @brief Get the joint type (one of RT_JOINT_DISTANCE/SPRING/HINGE/ROPE).
/// @param joint Opaque Physics2D.Joint handle.
/// @return The immutable `RT_JOINT_*` value, or `-1` for invalid input.
int64_t rt_physics2d_joint_get_type(void *joint);
/// @brief Return the joint's active solver flag.
/// @details Constructors initialize the flag to active before world
///          registration; RemoveJoint clears it and AddJoint sets it again.
/// @param joint Opaque Physics2D.Joint handle.
/// @return `1` when active, otherwise `0`; invalid input returns `0`.
int8_t rt_physics2d_joint_is_active(void *joint);

//=========================================================================
// World Joint Management
//=========================================================================

/// @brief Add a joint to the world's solver list (no-op if already present; storage grows).
/// @details Both retained endpoints must already belong to @p world. The world
///          retains the joint and sets its active flag.
/// @param world Opaque destination Physics2D.World handle.
/// @param joint Opaque Physics2D.Joint handle to register.
void rt_physics2d_world_add_joint(void *world, void *joint);
/// @brief Remove a joint from the world's solver list.
/// @details Clears the active flag, releases the world's retained reference,
///          and compacts the list without preserving order.
/// @param world Opaque source Physics2D.World handle.
/// @param joint Opaque Physics2D.Joint handle; absence is a no-op.
void rt_physics2d_world_remove_joint(void *world, void *joint);
/// @brief Number of joints currently registered in the world.
/// @param world Opaque Physics2D.World handle.
/// @return The registered joint count, or `0` for invalid input.
int64_t rt_physics2d_world_joint_count(void *world);

//=========================================================================
// Circle Bodies
//=========================================================================

/// @brief Create a circular body at (cx, cy). Any positive radius is preserved;
/// non-positive or non-finite radius falls back to 1.0.
/// @param cx Initial center X; non-finite values become zero.
/// @param cy Initial center Y; non-finite values become zero.
/// @param radius Positive radius in world units.
/// @param mass Positive dynamic mass, or a nonpositive/non-finite value for a
///             static circle.
/// @return A new circle-body handle, or `NULL` if allocation fails.
void *rt_physics2d_circle_body_new(double cx, double cy, double radius, double mass);
/// @brief Get the body's radius (0 for non-circle bodies).
/// @param body Opaque Physics2D.Body handle.
/// @return The stored radius, or `0.0` for an AABB or invalid input.
double rt_physics2d_body_radius(void *body);
/// @brief True if the body was created via `_circle_body_new` (uses circle collision).
/// @param body Opaque Physics2D.Body handle.
/// @return `1` for a circle body and `0` for an AABB or invalid input.
int8_t rt_physics2d_body_is_circle(void *body);

#ifdef __cplusplus
}
#endif
