//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_transform3d.h
// Purpose: Transform3D — position + quaternion rotation + scale with lazy
//   TRS matrix recomputation. Eliminates manual Mat4 composition chains.
//
// Key invariants:
//   - Dirty flag tracks when matrix needs recompute.
//   - Quaternion stored as (x,y,z,w) matching Quat convention.
//   - Matrix is row-major double[16], matching Mat4 layout.
//
// Links: rt_canvas3d_internal.h, rt_scene3d.c (build_trs_matrix)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the sanitized Transform3D TRS value and lazy matrix API.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a Transform3D at the origin with identity rotation and unit scale.
/// @return New owned Transform3D handle, or `NULL` on allocation failure.
void *rt_transform3d_new(void);
/// @brief Set the translation component (marks the matrix dirty).
/// @param xf Borrowed Transform3D handle.
/// @param x Position X.
/// @param y Position Y.
/// @param z Position Z.
void rt_transform3d_set_position(void *xf, double x, double y, double z);
/// @brief Get the translation as a Vec3.
/// @param xf Borrowed Transform3D handle.
/// @return New owned Vec3; invalid handles produce origin.
void *rt_transform3d_get_position(void *xf);
/// @brief Set the rotation component from a Quaternion.
/// @param xf Borrowed Transform3D handle.
/// @param quat Borrowed Quat normalized on assignment.
void rt_transform3d_set_rotation(void *xf, void *quat);
/// @brief Get the rotation as a Quaternion.
/// @param xf Borrowed Transform3D handle.
/// @return New owned Quat; invalid handles produce identity.
void *rt_transform3d_get_rotation(void *xf);
/// @brief Set rotation from Euler angles in degrees (pitch=X, yaw=Y, roll=Z, intrinsic order).
/// @param xf Borrowed Transform3D handle.
/// @param pitch X rotation in degrees.
/// @param yaw Y rotation in degrees.
/// @param roll Z rotation in degrees.
void rt_transform3d_set_euler(void *xf, double pitch, double yaw, double roll);
/// @brief Set the per-axis scale.
/// @param xf Borrowed Transform3D handle.
/// @param x Scale X.
/// @param y Scale Y.
/// @param z Scale Z.
void rt_transform3d_set_scale(void *xf, double x, double y, double z);
/// @brief Get the scale as a Vec3.
/// @param xf Borrowed Transform3D handle.
/// @return New owned Vec3; invalid handles produce unit scale.
void *rt_transform3d_get_scale(void *xf);
/// @brief Get the composed TRS matrix as a Mat4 (lazily recomputed when dirty).
/// @param xf Borrowed Transform3D handle.
/// @return New owned row-major Mat4; invalid handles produce identity.
void *rt_transform3d_get_matrix(void *xf);
/// @brief Add @p delta (Vec3) to the current position.
/// @param xf Borrowed Transform3D handle.
/// @param delta Borrowed displacement Vec3.
void rt_transform3d_translate(void *xf, void *delta);
/// @brief Multiply the current rotation by an axis-angle rotation (axis = Vec3, angle in radians).
/// @param xf Borrowed Transform3D handle.
/// @param axis Borrowed non-degenerate axis Vec3.
/// @param angle Finite angle in radians.
void rt_transform3d_rotate(void *xf, void *axis, double angle);
/// @brief Aim the transform's -Z forward at @p target with up vector @p up (Vec3 handles).
/// @param xf Borrowed Transform3D handle.
/// @param target Borrowed world-space target Vec3.
/// @param up Borrowed up-hint Vec3, or `NULL` for world Y.
void rt_transform3d_look_at(void *xf, void *target, void *up);

#ifdef __cplusplus
}
#endif
