//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_quickhull3d.h
// Purpose: From-scratch 3D convex hull construction (quickhull) for physics
//          collider reduction. Given a point cloud, produces the exact hull
//          vertex set and triangle faces (outward CCW winding).
// Key invariants:
//   - Output vertices are a subset of the input points (no new positions).
//   - Faces wind counter-clockwise seen from outside; normals point outward.
//   - Degenerate inputs (fewer than 4 non-coplanar points) fail cleanly
//     (return 0) rather than emitting a broken hull.
// Ownership/Lifetime:
//   - Output arrays are malloc'd; the caller frees them with free().
// Links: rt_collider3d.c (NewConvexHullReduced), src/tests/unit/test_quickhull3d.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares exact 3D convex-hull construction and support-preserving cloud reduction.
///
/// Both APIs borrow packed xyz input coordinates. Hull construction transfers successful
/// allocations to the caller, while reduction writes into caller-provided storage.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Compute the convex hull of a 3D point cloud (quickhull).
/// @param points       Borrowed interleaved xyz doubles (`point_count * 3` finite entries).
/// @param point_count  Number of input points (>= 4 for a volumetric hull).
/// @param[out] out_vertices Required destination receiving malloc'd interleaved xyz hull vertices.
/// @param[out] out_vertex_count Required destination receiving the hull vertex count.
/// @param[out] out_indices Optional (may be NULL): receives malloc'd triangle
///                         indices into out_vertices (3 per face, CCW outward).
/// @param[out] out_index_count Optional destination receiving the index count (3 * faces) when
///        @p out_indices is requested.
/// @return 1 on success; 0 on degenerate input (collinear/coplanar), invalid
///         arguments, or allocation failure (no outputs are leaked).
/// @note The caller releases successful vertex and index allocations with `free()`.
int rt_quickhull3d_build(const double *points,
                         int32_t point_count,
                         double **out_vertices,
                         int32_t *out_vertex_count,
                         int32_t **out_indices,
                         int32_t *out_index_count);

/// @brief Reduce a point set to at most @p max_points while preserving spread.
/// @details Greedy farthest-point (k-center) selection seeded with the
///          extreme points on each axis — the classic support-set reduction
///          for GJK colliders. Output points are a subset of the input.
/// @param points Borrowed interleaved xyz coordinates for @p point_count points.
/// @param point_count Positive number of input points.
/// @param max_points Positive upper bound on retained points.
/// @param[out] out_points Caller-owned interleaved xyz buffer with room for at least
///        `min(point_count, max_points) * 3` doubles.
/// @return Number of points written, or 0 on invalid arguments or allocation failure.
int32_t rt_quickhull3d_reduce(const double *points,
                              int32_t point_count,
                              int32_t max_points,
                              double *out_points);

#ifdef __cplusplus
}
#endif
