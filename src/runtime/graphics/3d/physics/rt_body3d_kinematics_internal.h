//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/3d/physics/rt_body3d_kinematics_internal.h
// Purpose: Private shared prefix view used by the Body3D core and joint solvers.
//
// Key invariants:
//   - This layout is implementation-only and never part of the public C ABI.
//   - rt_physics3d.c statically verifies every field offset against rt_body3d.
//
// Ownership/Lifetime:
//   - Values are borrowed views of live Body3D payloads; this type owns nothing.
//
// Links: rt_physics3d_internal.h, rt_joints3d_internal.h, rt_physics3d.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_body3d_kinematics_internal.h
 * @brief Defines the private Body3D prefix shared with constraint solvers.
 *
 * This implementation-only view must remain layout-compatible with the
 * corresponding prefix of `rt_body3d`; compile-time assertions in the physics
 * implementation verify the offsets. The structure borrows every referenced
 * value and does not define a public runtime ABI.
 */

#pragma once

/// @brief Shared read/write view of a Body3D's private kinematic prefix.
typedef struct rt_body3d_kinematics {
    ///< Reserved runtime object header slot.
    void *vptr;
    ///< World-space center-of-mass position.
    double position[3];
    ///< World-space orientation quaternion in the Body3D storage convention.
    double orientation[4];
    ///< Per-axis local-to-world scale.
    double scale[3];
    ///< World-space linear velocity.
    double velocity[3];
    ///< World-space angular velocity.
    double angular_velocity[3];
    ///< Accumulated world-space force awaiting integration.
    double force[3];
    ///< Accumulated world-space torque awaiting integration.
    double torque[3];
    ///< Body mass in simulation units.
    double mass;
    ///< Reciprocal mass, or zero for an immovable body.
    double inv_mass;
    ///< Borrowed collider slot retained to match the complete Body3D layout.
    void *collider;         /* layout filler: keeps inv_inertia at the rt_body3d offset */
    ///< Body-local inverse inertia along the principal axes.
    double inv_inertia[3];  /* body-local principal-axis inverse inertia (see rt_body3d) */
} rt_body3d_kinematics;
