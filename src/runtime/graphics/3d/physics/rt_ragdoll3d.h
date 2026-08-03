//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_ragdoll3d.h
// Purpose: Zanna.Graphics3D.Ragdoll3D — auto-built capsule-body + 6-DoF-joint
//   ragdoll rigs from a Skeleton3D, with animation handoff (velocity seeding),
//   per-step palette write-back, blend-out to animation, and powered PD drive
//   toward the animated pose.
// Key invariants:
//   - Rig bodies/joints only exist in a physics world between Activate and
//     Deactivate; the builder itself registers nothing.
//   - Palette write-back runs between animation update and scene sync (the
//     Game3D step calls rt_ragdoll3d_step there); raw users call it manually.
// Ownership/Lifetime:
//   - GC-managed handle retaining its skeleton, bodies, joints, and (while
//     active or blending) the world/controller/node references.
// Links: rt_physics3d.h, rt_joints3d.h, rt_animcontroller3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares skeleton-derived physics ragdolls and animation synchronization.
///
/// Configuration is applied before activation and may rebuild an inactive lazy rig. Activation
/// registers retained capsule bodies and joints in a world; deactivation removes them and may
/// retain animation objects temporarily while blending back to the live pose.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Build a ragdoll rig description from a skeleton (bodies build lazily).
/// @param skeleton Borrowed `Skeleton3D` retained for the ragdoll's lifetime.
/// @return Owned `Ragdoll3D`, or null after trapping on invalid input or allocation failure.
void *rt_ragdoll3d_from_skeleton(void *skeleton);

/// @brief Total mass distributed across rig bodies (default 70).
/// @param ragdoll Borrowed `Ragdoll3D` handle.
/// @return Configured positive mass in kilograms, or zero for an invalid handle.
double rt_ragdoll3d_get_total_mass(void *ragdoll);

/// @brief Set the distributed total mass (rebuilds an inactive rig).
/// @param ragdoll Borrowed inactive `Ragdoll3D` handle.
/// @param mass Positive finite total mass in kilograms no greater than 1e9; invalid values ignored.
void rt_ragdoll3d_set_total_mass(void *ragdoll, double mass);

/// @brief Capsule radius as a fraction of bone length (default 0.22).
/// @param ragdoll Borrowed `Ragdoll3D` handle.
/// @return Configured positive radius scale, or zero for an invalid handle.
double rt_ragdoll3d_get_radius_scale(void *ragdoll);

/// @brief Set the capsule radius scale (rebuilds an inactive rig).
/// @param ragdoll Borrowed inactive `Ragdoll3D` handle.
/// @param scale Positive finite radius scale no greater than 2.
void rt_ragdoll3d_set_radius_scale(void *ragdoll, double scale);

/// @brief Minimum bone length that receives its own body (default 0.12).
/// @param ragdoll Borrowed `Ragdoll3D` handle.
/// @return Positive selection threshold in model units, or zero for an invalid handle.
double rt_ragdoll3d_get_min_bone_length(void *ragdoll);

/// @brief Set the minimum bodied bone length (rebuilds an inactive rig).
/// @param ragdoll Borrowed inactive `Ragdoll3D` handle.
/// @param length Positive finite threshold in model units no greater than 1e9.
void rt_ragdoll3d_set_min_bone_length(void *ragdoll, double length);

/// @brief Number of rig bodies (builds the rig on first query).
/// @param ragdoll Borrowed `Ragdoll3D` handle.
/// @return Built capsule-body count, or zero when validation or rig construction fails.
int64_t rt_ragdoll3d_get_body_count(void *ragdoll);

/// @brief True between Activate and Deactivate.
/// @param ragdoll Borrowed `Ragdoll3D` handle.
/// @return `1` while bodies and joints are registered in a world; otherwise `0`.
int8_t rt_ragdoll3d_get_active(void *ragdoll);

/// @brief Override a bone's joint limits (swing/twist degrees) before Activate.
/// @param ragdoll Borrowed inactive `Ragdoll3D` handle.
/// @param bone_name Borrowed skeleton bone name that must map to a rig body.
/// @param swing_deg Swing limit clamped to `[0, 180]` degrees.
/// @param twist_deg Twist limit clamped to `[0, 180]` degrees.
void rt_ragdoll3d_set_joint_limits(void *ragdoll,
                                   rt_string bone_name,
                                   double swing_deg,
                                   double twist_deg);
/// @brief Hand off from animation: pose bodies from the controller's current
///   pose (velocities from the previous palette), add bodies + joints to
///   @p world, and start palette write-back. Node supplies the world transform.
/// @param ragdoll Borrowed inactive `Ragdoll3D` handle.
/// @param world Borrowed `World3D` retained while the rig is active.
/// @param controller Borrowed `AnimController3D` for the same skeleton, retained through
///        active/blending states.
/// @param node Borrowed `SceneNode3D` retained through active/blending states.
void rt_ragdoll3d_activate(void *ragdoll, void *world, void *controller, void *node);

/// @brief Remove the rig from the world and blend the palette back to live
///   animation over @p blend_seconds.
/// @param ragdoll Borrowed active `Ragdoll3D` handle.
/// @param blend_seconds Positive finite blend duration clamped to 1e9 seconds, or a non-positive
///        value for immediate animation-object release.
void rt_ragdoll3d_deactivate(void *ragdoll, double blend_seconds);

/// @brief Drive masked joints toward the animated pose with proportional-derivative impulses.
/// @details Mask bit N selects skeleton bone N, not the compacted rig slot; indices above 63 map
///          through the low six bits. Unbodied bones are ignored.
/// @param ragdoll Borrowed `Ragdoll3D` handle.
/// @param bone_mask Skeleton-bone bit mask; `-1` selects every representable bit and zero disables
///        all bones.
/// @param stiffness Non-negative finite drive gain clamped to 100; zero disables powered drive.
void rt_ragdoll3d_set_powered(void *ragdoll, int64_t bone_mask, double stiffness);

/// @brief Per-step sync: powered drive + palette write-back + node root-follow
///   (active), or blend-out progression (deactivating). Game3D calls this
///   between the physics step and scene sync; raw users call it manually.
/// @param ragdoll Borrowed `Ragdoll3D` handle.
/// @param dt Positive finite elapsed time in seconds; invalid/non-positive values are ignored.
void rt_ragdoll3d_step(void *ragdoll, double dt);

/// @brief Borrowed rig body for a bone name (NULL when unmapped).
/// @param ragdoll Borrowed `Ragdoll3D` handle.
/// @param bone_name Borrowed skeleton bone name.
/// @return Borrowed `Body3D`, or null for an unknown, unbodied, or unbuildable bone.
void *rt_ragdoll3d_get_body(void *ragdoll, rt_string bone_name);

#ifdef __cplusplus
}
#endif
