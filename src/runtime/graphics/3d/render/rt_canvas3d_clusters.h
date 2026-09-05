//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_clusters.h
// Purpose: Internal declarations for clustered forward+ CPU froxel binning
//   (Plan 07). Split from rt_canvas3d_internal.h because these signatures use
//   backend types (vgfx3d_light_params_t / vgfx3d_cluster_table_t) that the
//   internal header deliberately stays free of.
// Key invariants:
//   - Engine-internal; included only by canvas TUs that already include
//     vgfx3d_backend.h and by the unit tests.
//   - Binning is deterministic and conservatively over-inclusive (see
//     rt_canvas3d_clusters.c).
// Ownership/Lifetime:
//   - Tables are POD owned by the canvas's revision-keyed ring.
// Links: rt_canvas3d_clusters.c, vgfx3d_backend.h,
//   docs/internals/graphics3d-architecture.md
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares Canvas3D's internal CPU clustered-lighting binning interface.
/// @details These declarations intentionally depend on backend light/table layouts and therefore
/// remain separate from the otherwise backend-type-free Canvas3D internal header.

#pragma once

#include "rt_canvas3d_internal.h"
#include "vgfx3d_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief World influence radius of a point/spot light (0 = none, <0 = unbounded).
/// @param intensity Finite non-negative light intensity.
/// @param attenuation Quadratic distance-falloff coefficient.
/// @return Zero for no contribution, a negative unbounded sentinel for no falloff, or a positive
/// finite radius in world units.
float canvas3d_cluster_light_radius(float intensity, float attenuation);
/// @brief Exponential Z slice for a view depth (clamped to the grid).
/// @param depth Positive view-space depth in world units.
/// @param znear Positive cluster near-plane distance.
/// @param zfar Cluster far-plane distance greater than @p znear.
/// @return Zero-based slice index clamped to `VGFX3D_CLUSTER_DIM_Z`.
int32_t canvas3d_cluster_z_slice(float depth, float znear, float zfar);
/// @brief 1 for directional/ambient light types (the global prefix), else 0.
/// @param type Flattened runtime light-type identifier.
/// @return Non-zero for a global directional or ambient light; zero for locally binned types.
int canvas3d_cluster_light_is_global(int32_t type);
/// @brief Build a froxel table for a globals-first flattened light array.
/// @details The output is always initialized. Invalid/empty input produces an empty table stamped
/// with @p lights_revision; capacity overflow selects full-light fallback deterministically.
/// @param c Borrowed canvas providing cached camera state and the cluster budget.
/// @param lights Borrowed globals-first flattened light array.
/// @param light_count Number of valid entries in @p lights.
/// @param lights_revision Revision to stamp into the output.
/// @param[out] out Caller-owned table populated in place; NULL is ignored.
void canvas3d_build_cluster_table(const rt_canvas3d *c,
                                  const vgfx3d_light_params_t *lights,
                                  int32_t light_count,
                                  uint32_t lights_revision,
                                  vgfx3d_cluster_table_t *out);
/// @brief Shader-mirror cluster index for (uv in [0,1], view depth).
/// @param u Horizontal normalized screen coordinate.
/// @param v Vertical normalized screen coordinate, with zero at the top.
/// @param depth Positive view-space depth.
/// @param znear Cluster near-plane distance.
/// @param zfar Cluster far-plane distance.
/// @return Flattened cluster index with every axis clamped to the backend grid.
int32_t canvas3d_cluster_index_for_point(float u, float v, float depth, float znear, float zfar);
/// @brief Fetch-or-build the froxel table matching a light revision.
/// @param c Canvas owning the revision-keyed table ring.
/// @param lights Borrowed globals-first flattened light array.
/// @param light_count Number of valid entries in @p lights.
/// @param revision Non-zero light revision used as the cache key.
/// @return Borrowed canvas-owned ring entry, valid until overwritten or canvas finalization; NULL
/// when clustering is off, the backend keeps the flat loop, input is invalid, or allocation fails.
const vgfx3d_cluster_table_t *canvas3d_cluster_table_for_revision(
    rt_canvas3d *c, const vgfx3d_light_params_t *lights, int32_t light_count, uint32_t revision);

#ifdef __cplusplus
}
#endif
