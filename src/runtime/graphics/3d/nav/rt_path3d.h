//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_path3d.h
// Purpose: 3D Catmull-Rom spline path for camera dollies, patrol routes,
//   missile trajectories. Evaluate position/direction at t in [0,1].
//
// Key invariants:
//   - Minimum 2 control points for evaluation.
//   - Catmull-Rom: curve passes through all control points.
//   - Arc length computed numerically (cached, dirty flag).
//
// Links: rt_spline.c (2D version), rt_transform3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_path3d.h
 * @brief Declares the runtime C interface for three-dimensional spline paths.
 *
 * Path3D is a runtime-managed sequence of Vec3 control points. The public
 * evaluators expose the historical uniformly parameterized curve, while the
 * raw helper supplies a constant-speed centripetal evaluation for internal
 * camera and timeline consumers.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create an empty 3D spline path.
/// @return Newly allocated runtime-managed Path3D handle, or NULL on allocation failure.
void *rt_path3d_new(void);

/// @brief Append a Vec3 control point @p pos to the path.
/// @param path Path3D handle to modify.
/// @param pos Vec3 whose sanitized coordinates are copied into the path.
void rt_path3d_add_point(void *path, void *pos);

/// @brief Sample the interpolated position at normalized parameter @p t
///        ([0,1] over the whole path).
/// @param path Path3D handle to evaluate.
/// @param t Normalized parameter, wrapped for loops and clamped for open paths.
/// @return Newly allocated Vec3 containing the interpolated position.
void *rt_path3d_get_position_at(void *path, double t);

/// @brief Sample the (normalized) tangent/direction at parameter @p t.
/// @param path Path3D handle to evaluate.
/// @param t Normalized parameter, wrapped for loops and clamped for open paths.
/// @return Newly allocated unit-direction Vec3, or the zero vector for a degenerate path.
void *rt_path3d_get_direction_at(void *path, double t);

/// @brief Total arc length of the path in world units.
/// @param path Path3D handle to measure.
/// @return Cached or recomputed finite curve length, or zero for an invalid or degenerate path.
double rt_path3d_get_length(void *path);

/// @brief Number of control points.
/// @details The query repairs pointer/count metadata in constant time without
///   rescanning control-point contents.
/// @param path Path3D handle to inspect.
/// @return Non-negative control-point count, or zero for an invalid handle.
int64_t rt_path3d_get_point_count(void *path);

/// @brief Set whether the path wraps from the last point back to the first.
/// @param path Path3D handle to modify.
/// @param loop Nonzero to enable the closing segment and normalized-parameter wrapping.
void rt_path3d_set_looping(void *path, int8_t loop);
/// @brief Read whether the path loops back to its first point (ADR 0227).
/// @param path Path3D receiver.
/// @return Nonzero for looping paths, or 0 for invalid handles.
int8_t rt_path3d_get_looping(void *path);

/// @brief Remove all control points.
/// @details Retains coordinate capacity for reuse and releases the derived
///   constant-speed lookup table.
/// @param path Path3D handle to clear while retaining its allocated coordinate storage.
void rt_path3d_clear(void *path);

/// @brief Internal: arclength-normalized spline evaluation (constant-speed t in
///        [0,1] along the same Catmull-Rom curve GetPositionAt samples). Writes
///        the position and, when non-NULL, the unit tangent.
/// @param path Path3D handle to evaluate.
/// @param t Normalized arc-length parameter.
/// @param pos_out Required three-component output receiving the position.
/// @param tan_out Optional three-component output receiving the unit tangent.
void rt_path3d_eval_spline_raw(void *path, double t, double *pos_out, double *tan_out);

#ifdef __cplusplus
}
#endif
