//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_mesh_simplify.h
// Purpose: Quadric-error-metric mesh simplification (Mesh3D.Simplify) and the
//   one-call LOD-chain generator (SceneNode.GenerateLODs).
// Key invariants:
//   - Simplify returns a NEW mesh; attributes are never interpolated (subset
//     placement), so skinned meshes decimate safely and UV seams survive.
//   - Deterministic for identical inputs.
// Ownership/Lifetime: returned meshes are GC-managed Mesh3D objects.
// Links: rt_mesh_simplify.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_mesh_simplify.h
 * @brief Declares deterministic Mesh3D simplification and SceneNode3D LOD-chain generation.
 * @details Simplification uses subset placement so retained vertex attributes and animation
 *          side streams come from original vertices rather than interpolation. Results expose
 *          requested, achieved, and completion status so topology-constrained partial meshes
 *          remain distinguishable from exact-budget results.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decimate a mesh toward a triangle budget and return a new Mesh3D.
 *
 * The source remains unchanged. The returned mesh retains/remaps all vertex and
 * animation side streams and records whether manifold/boundary constraints allowed
 * the exact target to be reached. Use the simplification diagnostic getters below
 * to distinguish complete and valid partial results.
 *
 * @param[in] mesh Source Mesh3D handle.
 * @param target_triangles Requested triangle budget, clamped to at least one.
 * @return A new valid Mesh3D, including when the result is partial, or `NULL` on
 * invalid input/allocation failure.
 */
void *rt_mesh3d_simplify(void *mesh, int64_t target_triangles);

/**
 * @brief Refuse collapses that remove any open/attribute/material boundary vertex.
 *
 * Exact-record welding splits every UV-chart and material border into independent
 * boundary polylines; decimating the two sides to different vertex subsets opens
 * visible cracks. With this flag each classified boundary polyline survives
 * verbatim and only chart interiors decimate, so scans with dense seam networks
 * (photogrammetry) stay watertight at aggressive targets.
 */
#define RT_MESH3D_SIMPLIFY_FLAG_LOCK_BOUNDARIES INT64_C(1)

/**
 * @brief Tool-facing simplify with boundary locking and a geometric error ceiling.
 *
 * Identical to rt_mesh3d_simplify plus two offline-bake controls: @p flags selects
 * additional collapse constraints (see the flag macros above) and @p max_error_frac,
 * when positive and finite, stops collapsing once the cheapest remaining candidate's
 * accumulated quadric cost exceeds `(max_error_frac * bounding_diameter)^2`. Stopping
 * early is not an error: the result is a valid PARTIAL mesh with exact diagnostics.
 *
 * @param[in] mesh Source Mesh3D handle.
 * @param target_triangles Requested triangle budget, clamped to at least one.
 * @param flags Bitwise OR of RT_MESH3D_SIMPLIFY_FLAG_* values, or zero.
 * @param max_error_frac Error ceiling as a fraction of the bounding diameter; <= 0 disables.
 * @return A new valid Mesh3D, including when the result is partial, or `NULL` on
 * invalid input/allocation failure.
 */
void *rt_mesh3d_simplify_ex(void *mesh,
                            int64_t target_triangles,
                            int64_t flags,
                            double max_error_frac);
/**
 * @brief Return the sanitized target recorded by Mesh3D.Simplify.
 * @param[in] mesh Mesh3D receiver.
 * @return Requested triangle count, or zero for a null/non-simplified mesh.
 */
int64_t rt_mesh3d_get_simplify_requested_triangles(void *mesh);
/**
 * @brief Return the exact triangle count achieved by Mesh3D.Simplify.
 * @param[in] mesh Mesh3D receiver.
 * @return Achieved triangle count, or zero for a null/non-simplified mesh.
 */
int64_t rt_mesh3d_get_simplify_achieved_triangles(void *mesh);
/**
 * @brief Return the simplification completion status for a Mesh3D.
 * @param[in] mesh Mesh3D receiver.
 * @return `0` for not-run, `1` for complete, or `2` for a topology-constrained
 * partial result.
 */
int64_t rt_mesh3d_get_simplify_status(void *mesh);
/**
 * @brief Build one to four simplified LOD levels and enable automatic selection.
 * @details Each level applies @p ratio to the preceding triangle budget, starts its
 *          distance threshold from the source bounding radius, and doubles that threshold
 *          for successive levels.
 * @param[in,out] node SceneNode receiver whose base mesh supplies the LOD source.
 * @param levels Requested level count, clamped to `[1,4]`.
 * @param ratio Per-level triangle ratio; invalid values use `0.4`.
 */
void rt_scene_node3d_generate_lods(void *node, int64_t levels, double ratio);

#ifdef __cplusplus
}
#endif
