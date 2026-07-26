//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_vegetation3d.h
// Purpose: Instanced vegetation rendering — grass blades, foliage, etc.
//   Cross-billboard geometry with wind animation and distance LOD.
//
// Key invariants:
//   - Blade mesh is two perpendicular quads (cross-billboard, 8 verts).
//   - Instances stored as float[16] transforms (same as InstanceBatch3D).
//   - Wind applied as Y-axis shear per-frame before rendering.
//   - LOD: blades thinned by distance, culled beyond far threshold.
//
// Links: rt_canvas3d.h, rt_terrain3d.h, rt_instbatch3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_vegetation3d.h
 * @brief Declares deterministic terrain-scattered, instanced Vegetation3D rendering.
 *
 * A vegetation system owns shared cross-billboard geometry, authored scatter and wind settings,
 * population buffers, and a camera-dependent visible transform batch.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a vegetation system that uses @p blade_texture for each grass quad.
/// @param blade_texture Optional Pixels texture retained by the generated blade material.
/// @return New GC-managed Vegetation3D handle, or NULL on invalid texture or allocation failure.
void *rt_vegetation3d_new(void *blade_texture);

/// @brief Bind a Pixels density map (R channel modulates per-tile blade count).
/// @param veg Vegetation3D handle; invalid handles are ignored.
/// @param pixels Nonempty Pixels density map to retain, or NULL for uniform density.
void rt_vegetation3d_set_density_map(void *veg, void *pixels);

/// @brief Configure wind animation parameters (speed = waving frequency, strength = max bend,
/// turbulence = noise factor).
/// @param veg Vegetation3D handle; invalid handles are ignored.
/// @param speed Signed temporal wave speed.
/// @param strength Nonnegative maximum bend in world units.
/// @param turbulence Nonnegative spatial phase variation.
void rt_vegetation3d_set_wind_params(void *veg, double speed, double strength, double turbulence);

/// @brief Set near (full density) and far (cull) LOD distances in world units.
/// @param veg Vegetation3D handle; invalid handles are ignored.
/// @param near_dist Full-density radius.
/// @param far_dist Hard-cull radius, sanitized to remain greater than @p near_dist.
void rt_vegetation3d_set_lod_distances(void *veg, double near_dist, double far_dist);

/// @brief Set per-blade dimensions: average width, average height, and ±variation factor.
/// @param veg Vegetation3D handle; invalid handles are ignored.
/// @param width Positive cross-billboard width.
/// @param height Positive cross-billboard height.
/// @param variation Per-instance scale variation clamped to `[0, 1]`.
void rt_vegetation3d_set_blade_size(void *veg, double width, double height, double variation);

/// @brief Set the deterministic scatter seed used by subsequent Populate calls.
/// @param veg Vegetation3D handle; invalid handles are ignored.
/// @param seed User seed mixed into the nonzero 32-bit scatter state.
void rt_vegetation3d_set_seed(void *veg, int64_t seed);

/// @brief Spawn @p count blades scattered across the terrain (sampled using density map if set).
/// @param veg Vegetation3D handle; invalid handles are ignored.
/// @param terrain Terrain3D handle supplying extents and sampled heights.
/// @param count Maximum candidate count; zero clears the existing population.
void rt_vegetation3d_populate(void *veg, void *terrain, int64_t count);

/// @brief Advance wind animation by @p dt and apply distance-based culling against the camera.
/// @param veg Vegetation3D handle; invalid handles are ignored.
/// @param dt Nonnegative finite simulation delta, clamped to one second.
/// @param camX Camera world-space x coordinate.
/// @param camY Camera world-space y coordinate; currently unused.
/// @param camZ Camera world-space z coordinate.
void rt_vegetation3d_update(void *veg, double dt, double camX, double camY, double camZ);

/// @brief Render all visible blades as a single instanced draw call.
/// @param canvas Canvas3D handle receiving the instanced batch.
/// @param veg Vegetation3D handle supplying shared geometry and visible transforms.
void rt_canvas3d_draw_vegetation(void *canvas, void *veg);

#ifdef __cplusplus
}
#endif
