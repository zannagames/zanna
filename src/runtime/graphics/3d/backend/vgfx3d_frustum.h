//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_frustum.h
// Purpose: View frustum extraction and AABB/sphere intersection tests for
//   culling objects outside the camera's view volume.
//
// Key invariants:
//   - Planes are extracted from the combined VP matrix (Gribb-Hartmann method).
//   - Plane normals point INWARD (positive side = inside frustum).
//   - All planes are normalized after extraction.
//   - Test returns: 0=outside, 1=intersecting, 2=fully inside.
//
// Links: plans/3d/13-frustum-culling.md, rt_scene3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_frustum.h
 * @brief Declares conservative frustum extraction, volume classification, and AABB utilities.
 *
 * Extracted planes use the Gribb-Hartmann convention with inward-facing unit normals. Culling
 * helpers return outside, intersecting, or fully-inside classifications and intentionally treat
 * invalid frusta or bounds as intersecting so malformed numeric data cannot hide geometry.
 */
#pragma once

#include <stdint.h>

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Six frustum planes in Ax+By+Cz+D=0 form (normalized).
/// Order: 0=left, 1=right, 2=bottom, 3=top, 4=near, 5=far.
typedef struct {
    /// Inward-facing normalized plane equations stored as `[A, B, C, D]`.
    float planes[6][4];  /* [A, B, C, D] per plane */
    /// One after all planes were extracted and normalized; zero requests conservative culling.
    int8_t planes_valid; /* 1 once extract normalizes all six planes; 0 = conservative */
} vgfx3d_frustum_t;

/// @brief Extract frustum planes from a row-major View*Projection matrix.
/// @param f Caller-owned frustum receiving six normalized planes and their validity state.
/// @param vp Borrowed 16-element row-major view-projection matrix. Null or invalid input produces
///           a conservative frustum.
void vgfx3d_frustum_extract(vgfx3d_frustum_t *f, const float vp[16]);

/// @brief Test an axis-aligned bounding box against the frustum.
/// @param f Borrowed frustum.
/// @param min Borrowed minimum AABB corner.
/// @param max Borrowed maximum AABB corner.
/// @return 0 = fully outside, 1 = intersecting, 2 = fully inside.
int vgfx3d_frustum_test_aabb(const vgfx3d_frustum_t *f, const float min[3], const float max[3]);

/// @brief Test an AABB after expanding it outward by @p pad world/render units.
/// @details Terrain and horizon-adjacent chunks need culling to be slightly more conservative
///   than rasterization because a one-ULP CPU bounds shrink can otherwise reject visible triangles.
///   Non-finite, negative, and zero padding falls back to the ordinary AABB test.
/// @param f Borrowed frustum.
/// @param min Borrowed first AABB corner; per-axis ordering is canonicalized when padding applies.
/// @param max Borrowed opposite AABB corner.
/// @param pad Finite positive symmetric expansion in world or render units.
/// @return 0 = fully outside, 1 = intersecting, 2 = fully inside.
int vgfx3d_frustum_test_aabb_padded(const vgfx3d_frustum_t *f,
                                    const float min[3],
                                    const float max[3],
                                    float pad);

/// @brief Test a bounding sphere against the frustum.
/// @param f Borrowed frustum.
/// @param center Borrowed sphere center in the frustum's coordinate space.
/// @param radius Finite non-negative sphere radius.
/// @return 0 = fully outside, 1 = intersecting, 2 = fully inside.
int vgfx3d_frustum_test_sphere(const vgfx3d_frustum_t *f, const float center[3], float radius);

/// @brief Transform an object-space AABB by a 4x4 row-major matrix,
///        producing a new world-space AABB that encloses the transformed box.
/// @details This legacy wrapper writes zero bounds when checked transformation fails.
/// @param obj_min Borrowed first object-space AABB corner.
/// @param obj_max Borrowed opposite object-space AABB corner.
/// @param world_matrix Borrowed 16-element row-major affine transform.
/// @param out_min Receives the transformed minimum, or zero on failure.
/// @param out_max Receives the transformed maximum, or zero on failure.
void vgfx3d_transform_aabb(const float obj_min[3],
                           const float obj_max[3],
                           const double world_matrix[16],
                           float out_min[3],
                           float out_max[3]);
/// @brief Checked AABB transform; returns 0 instead of collapsing invalid output to zero.
/// @details Callers that make visibility decisions should prefer this form and draw
///   conservatively when it fails.
/// @param obj_min Borrowed first object-space AABB corner.
/// @param obj_max Borrowed opposite object-space AABB corner; ordering is canonicalized.
/// @param world_matrix Borrowed 16-element row-major affine transform.
/// @param out_min Receives the conservatively rounded world-space minimum corner.
/// @param out_max Receives the conservatively rounded world-space maximum corner.
/// @return 1 on success, otherwise 0; output values are not meaningful on failure.
int vgfx3d_transform_aabb_checked(const float obj_min[3],
                                  const float obj_max[3],
                                  const double world_matrix[16],
                                  float out_min[3],
                                  float out_max[3]);

/// @brief Compute the AABB of a mesh's vertices.
/// @details Positions are read as the first three floats of each strided record. Empty input
///          produces zero bounds, non-finite positions are skipped, and an array with no finite
///          positions produces a deliberately large conservative box.
/// @param vertices Borrowed byte-addressable vertex array.
/// @param vertex_count Number of strided vertex records.
/// @param vertex_stride Byte distance between consecutive position slots.
/// @param out_min Receives the minimum finite position on each axis.
/// @param out_max Receives the maximum finite position on each axis.
void vgfx3d_compute_mesh_aabb(const void *vertices,
                              uint32_t vertex_count,
                              uint32_t vertex_stride,
                              float out_min[3],
                              float out_max[3]);

#endif /* ZANNA_ENABLE_GRAPHICS */
