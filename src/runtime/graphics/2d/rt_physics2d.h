//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_physics2d.h
/// @brief Declares the public 2D rigid-body world, body, contact, and analytic
///        projectile APIs.
///
/// @details Physics worlds simulate non-rotating AABB and circle bodies in
/// positive-Y-down coordinates. Worlds retain registered bodies, expose
/// per-step contact records as borrowed views, and bound large public
/// timesteps through substepping. Projectile2D provides an independent
/// closed-form trajectory helper and is not registered with a physics world.
///
// File: src/runtime/graphics/2d/rt_physics2d.h
// Purpose: 2D physics engine for game entities providing AABB/circle rigid-body simulation,
// impulse-based collision resolution, bitmask collision filtering, and a simple world step loop.
//
// Key invariants:
//   - Bodies are AABB or circle shapes; no rotational physics.
//   - Fast bodies use shape-aware swept collision checks.
//   - World body storage starts with PH_MAX_BODIES slots and grows on demand.
//   - Bodies with mass == 0.0 are static (immovable).
//   - Collision filtering: bodies collide only when (A.layer & B.mask) != 0 AND (B.layer & A.mask)
//   != 0.
//   - Collision layer/mask values are 64-bit bitmasks; the default mask is all bits set.
//
// Ownership/Lifetime:
//   - World objects are GC-managed; body handles are reference-counted.
//   - Adding a body gives the world its own retained reference. Removing the
//     body releases that reference; the caller's reference is unchanged.
//   - Contact body accessors return borrowed handles valid only while that
//     contact list remains current.
//
// Links: src/runtime/graphics/2d/rt_physics2d.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_physics2d_joint.h"
#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Physics World
//=========================================================================

/// @brief Create a new 2D physics world.
/// @details Non-finite gravity components become zero. Initial body, joint, and
///          contact reservations are allocated before the world is returned.
/// @param gravity_x Horizontal acceleration in world units per second squared.
/// @param gravity_y Vertical acceleration; positive values point downward.
/// @return A new opaque world handle, or `NULL` after allocation failure.
void *rt_physics2d_world_new(double gravity_x, double gravity_y);

/// @brief Step the physics simulation forward. Clears previous contacts first; non-finite and
/// non-positive dt values then no-op.
/// @details Positive time is capped at eight seconds and split into at most
///          eight substeps of no more than one second. Accumulated forces are
///          snapshotted and reapplied to every substep so their total impulse
///          remains proportional to the requested bounded timestep.
/// @param world Opaque world handle to advance.
/// @param dt Requested elapsed time in seconds.
void rt_physics2d_world_step(void *world, double dt);

/// @brief Add a body to the world. Duplicate adds are ignored; storage grows on demand.
/// @details The world retains the body. A body is intended to belong to only
///          one world, so remove it from any prior world before adding it here.
/// @param world Opaque destination world handle.
/// @param body Opaque body handle retained on successful insertion.
void rt_physics2d_world_add(void *world, void *body);

/// @brief Remove a body from the world.
/// @details Referencing joints and current contacts are removed first, then the
///          world's body reference is released. Array order is not preserved.
/// @param world Opaque source world handle.
/// @param body Opaque body handle to detach; absence is a no-op.
void rt_physics2d_world_remove(void *world, void *body);

/// @brief Get the number of bodies in the world.
/// @param world Opaque world handle to query.
/// @return The retained body count, or `0` for invalid input.
int64_t rt_physics2d_world_body_count(void *world);

/// @brief Set world gravity.
/// @details Non-finite components are independently replaced with zero.
/// @param world Opaque world handle to update.
/// @param gx Horizontal acceleration.
/// @param gy Vertical acceleration; positive values point downward.
void rt_physics2d_world_set_gravity(void *world, double gx, double gy);

/// @brief Number of contacts detected during the most recent world step.
/// @details The list is cleared at the start of every Step() and whenever a
///          body is removed. Contacts from every substep of one public Step()
///          remain queryable and may include the same pair more than once.
/// @param world Opaque world handle to query.
/// @return The current contact-record count, or `0` for invalid input.
int64_t rt_physics2d_world_contact_count(void *world);

/// @brief Return whether contacts were omitted from the most recent world step.
/// @details Contact storage starts with PH_MAX_CONTACTS slots and grows on demand. If allocation
///          fails while recording a contact, ContactCount returns the partial count and this query
///          returns 1 until the next step or explicit contact clear caused by body removal.
/// @param world World handle.
/// @return 1 if the last contact generation could not grow the contact list, otherwise 0.
int8_t rt_physics2d_world_contact_overflowed(void *world);

/// @brief Body A for a contact from the most recent world step, or NULL if index is invalid.
/// @param world Opaque world handle containing the contact list.
/// @param index Zero-based contact index.
/// @return A borrowed body handle retained by the current contact, or `NULL`.
void *rt_physics2d_world_contact_body_a(void *world, int64_t index);

/// @brief Body B for a contact from the most recent world step, or NULL if index is invalid.
/// @param world Opaque world handle containing the contact list.
/// @param index Zero-based contact index.
/// @return A borrowed body handle retained by the current contact, or `NULL`.
void *rt_physics2d_world_contact_body_b(void *world, int64_t index);

/// @brief Contact normal X component from A toward B for the most recent world step.
/// @param world Opaque world handle containing the contact list.
/// @param index Zero-based contact index.
/// @return The normal X component, or `0.0` for invalid input.
double rt_physics2d_world_contact_nx(void *world, int64_t index);

/// @brief Contact normal Y component from A toward B for the most recent world step.
/// @param world Opaque world handle containing the contact list.
/// @param index Zero-based contact index.
/// @return The normal Y component, or `0.0` for invalid input.
double rt_physics2d_world_contact_ny(void *world, int64_t index);

/// @brief Contact penetration depth for overlap contacts, 0 for swept contacts.
/// @param world Opaque world handle containing the contact list.
/// @param index Zero-based contact index.
/// @return The nonnegative penetration depth, or `0.0` for invalid input.
double rt_physics2d_world_contact_depth(void *world, int64_t index);

//=========================================================================
// Rigid Body
//=========================================================================

/// @brief Create a new rigid body (AABB shape). Invalid size clamps to 1; invalid mass is static.
/// @param x Initial top-left X; non-finite values become zero.
/// @param y Initial top-left Y; non-finite values become zero.
/// @param w Width; non-finite or nonpositive values become one.
/// @param h Height; non-finite or nonpositive values become one.
/// @param mass Positive dynamic mass, or a nonpositive/non-finite value for a
///             static body.
/// @return A new opaque body handle, or `NULL` after allocation failure.
void *rt_physics2d_body_new(double x, double y, double w, double h, double mass);

/// @brief Get the top-left X of an AABB body or center X of a circle body.
/// @param body Opaque body handle.
/// @return The stored X coordinate, or `0.0` for invalid input.
double rt_physics2d_body_x(void *body);

/// @brief Get the top-left Y of an AABB body or center Y of a circle body.
/// @param body Opaque body handle.
/// @return The stored Y coordinate, or `0.0` for invalid input.
double rt_physics2d_body_y(void *body);

/// @brief Get body X from the start of the most recent integration substep.
/// @param body Opaque body handle.
/// @return The previous top-left/center X coordinate, or `0.0` for invalid input.
double rt_physics2d_body_prev_x(void *body);

/// @brief Get body Y from the start of the most recent integration substep.
/// @param body Opaque body handle.
/// @return The previous top-left/center Y coordinate, or `0.0` for invalid input.
double rt_physics2d_body_prev_y(void *body);

/// @brief Get the stored AABB width; this is not a circle diameter.
/// @param body Opaque body handle.
/// @return The stored width, or `0.0` for invalid input.
double rt_physics2d_body_w(void *body);

/// @brief Get the stored AABB height; this is not a circle diameter.
/// @param body Opaque body handle.
/// @return The stored height, or `0.0` for invalid input.
double rt_physics2d_body_h(void *body);

/// @brief Get body X velocity.
/// @param body Opaque body handle.
/// @return Horizontal velocity in world units per second, or `0.0` if invalid.
double rt_physics2d_body_vx(void *body);

/// @brief Get body Y velocity.
/// @param body Opaque body handle.
/// @return Vertical velocity in world units per second, or `0.0` if invalid.
double rt_physics2d_body_vy(void *body);

/// @brief Set body position.
/// @details Updates current and previous coordinates together, so no swept
///          motion is generated. Non-finite coordinates are ignored.
/// @param body Opaque body handle to update.
/// @param x New top-left X for an AABB or center X for a circle.
/// @param y New top-left Y for an AABB or center Y for a circle.
void rt_physics2d_body_set_pos(void *body, double x, double y);

/// @brief Set body velocity. Static bodies ignore velocity writes.
/// @param body Opaque body handle to update.
/// @param vx Finite horizontal velocity.
/// @param vy Finite vertical velocity.
void rt_physics2d_body_set_vel(void *body, double vx, double vy);

/// @brief Apply a force to the body (accumulated until next step).
/// @details Static bodies and non-finite inputs are ignored.
/// @param body Opaque body handle to update.
/// @param fx Horizontal force contribution.
/// @param fy Vertical force contribution.
void rt_physics2d_body_apply_force(void *body, double fx, double fy);

/// @brief Apply an impulse (instant velocity change).
/// @details Adds `impulse * inverse_mass` to velocity. Static bodies and
///          non-finite inputs are ignored.
/// @param body Opaque body handle to update.
/// @param ix Horizontal impulse.
/// @param iy Vertical impulse.
void rt_physics2d_body_apply_impulse(void *body, double ix, double iy);

/// @brief Get restitution (bounciness).
/// @param body Opaque body handle.
/// @return The coefficient in `[0, 1]`, or `0.0` for invalid input.
double rt_physics2d_body_restitution(void *body);
/// @brief Set the body's restitution (bounciness), clamped to 0-1.
/// @param body Opaque body handle to update.
/// @param r Requested coefficient; non-finite values become zero.
void rt_physics2d_body_set_restitution(void *body, double r);

/// @brief Get the friction coefficient.
/// @param body Opaque body handle.
/// @return The coefficient in `[0, 1]`, or `0.0` for invalid input.
double rt_physics2d_body_friction(void *body);
/// @brief Set the body's friction coefficient, clamped to 0-1.
/// @param body Opaque body handle to update.
/// @param f Requested coefficient; non-finite values become zero.
void rt_physics2d_body_set_friction(void *body, double f);

/// @brief Check if body is static (mass == 0).
/// @param body Opaque body handle.
/// @return `1` for a static body and `0` for dynamic or invalid input.
int8_t rt_physics2d_body_is_static(void *body);

/// @brief Get body mass.
/// @param body Opaque body handle.
/// @return Positive dynamic mass, or `0.0` for static or invalid input.
double rt_physics2d_body_mass(void *body);

/// @brief Get collision layer bitmask.
/// @param body Opaque body handle.
/// @return The stored 64-bit membership mask, or `0` for invalid input.
int64_t rt_physics2d_body_collision_layer(void *body);

/// @brief Set collision layer bitmask.
/// @param body Opaque body handle to update.
/// @param layer Signed 64-bit membership mask stored verbatim.
void rt_physics2d_body_set_collision_layer(void *body, int64_t layer);

/// @brief Get collision mask bitmask.
/// @param body Opaque body handle.
/// @return The stored 64-bit selection mask, or `0` for invalid input.
int64_t rt_physics2d_body_collision_mask(void *body);

/// @brief Set collision mask bitmask.
/// @param body Opaque body handle to update.
/// @param mask Signed 64-bit selection mask stored verbatim.
void rt_physics2d_body_set_collision_mask(void *body, int64_t mask);

//=========================================================================
// Projectile2D
//=========================================================================

/// @brief Create a projectile from initial position (p0), velocity (v0) and
///        constant gravity (g).
/// @details Non-finite components become zero. Drag is initially disabled and
///          the ground threshold is initially positive infinity.
/// @param p0x Initial horizontal position.
/// @param p0y Initial vertical position.
/// @param v0x Initial horizontal velocity.
/// @param v0y Initial vertical velocity.
/// @param gx Constant horizontal acceleration.
/// @param gy Constant vertical acceleration in positive-Y-down coordinates.
/// @return A new Projectile2D handle, or `NULL` after allocation failure.
void *rt_projectile2d_new(double p0x, double p0y, double v0x, double v0y, double gx, double gy);
/// @brief Set linear air-drag (0 = none); affects the closed-form solution.
/// @param p Opaque Projectile2D handle to update.
/// @param drag Positive finite coefficient, or any other value to disable drag.
void rt_projectile2d_set_drag(void *p, double drag);
/// @brief Set the ground plane Y used for landing detection (default +inf).
/// @details Non-finite input restores positive infinity and disables landing.
/// @param p Opaque Projectile2D handle to update.
/// @param y Ground threshold in positive-Y-down coordinates.
void rt_projectile2d_set_ground_y(void *p, double y);
/// @brief Reset elapsed time and landed state back to launch.
/// @details Launch parameters, drag, and ground threshold are preserved.
/// @param p Opaque Projectile2D handle to reset.
void rt_projectile2d_reset(void *p);
/// @brief Advance the simulation clock by @p dt seconds (updates landed state).
/// @details Nonpositive/non-finite deltas and already-landed projectiles are
///          ignored. The elapsed time is not clamped to the exact impact time.
/// @param p Opaque Projectile2D handle to advance.
/// @param dt Positive finite elapsed time.
void rt_projectile2d_advance(void *p, double dt);
/// @brief X position at absolute time @p t seconds since launch.
/// @param p Opaque Projectile2D handle.
/// @param t Absolute time since launch; nonpositive/non-finite values select the
///          initial position.
/// @return The analytic X position, or `0.0` for invalid input.
double rt_projectile2d_x_at(void *p, double t);
/// @brief Y position at absolute time @p t seconds since launch.
/// @param p Opaque Projectile2D handle.
/// @param t Absolute time since launch; nonpositive/non-finite values select the
///          initial position.
/// @return The analytic Y position, or `0.0` for invalid input.
double rt_projectile2d_y_at(void *p, double t);
/// @brief X velocity at absolute time @p t seconds since launch.
/// @param p Opaque Projectile2D handle.
/// @param t Absolute time since launch; nonpositive/non-finite values select the
///          initial velocity.
/// @return The analytic X velocity, or `0.0` for invalid input.
double rt_projectile2d_vx_at(void *p, double t);
/// @brief Y velocity at absolute time @p t seconds since launch.
/// @param p Opaque Projectile2D handle.
/// @param t Absolute time since launch; nonpositive/non-finite values select the
///          initial velocity.
/// @return The analytic Y velocity, or `0.0` for invalid input.
double rt_projectile2d_vy_at(void *p, double t);
/// @brief Whether the projectile has reached the ground plane.
/// @param p Opaque Projectile2D handle.
/// @return The landed flag latched by Advance, or `0` for invalid input.
int8_t rt_projectile2d_has_landed(void *p);
/// @brief Total elapsed simulated time (seconds) since launch.
/// @param p Opaque Projectile2D handle.
/// @return Accumulated elapsed time, or `0.0` for invalid input.
double rt_projectile2d_total_time(void *p);
/// @brief Estimate an absolute time since launch at the ground threshold.
/// @details Returns positive infinity when ground detection is disabled or no
///          supported crossing is found. This is not remaining time relative
///          to TotalTime.
/// @param p Opaque Projectile2D handle.
/// @return A nonnegative absolute launch time, or positive infinity.
double rt_projectile2d_time_to_ground(void *p);

#ifdef __cplusplus
}
#endif
