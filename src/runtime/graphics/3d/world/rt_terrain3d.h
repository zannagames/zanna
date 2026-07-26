//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_terrain3d.h
// Purpose: Heightmap-based terrain with chunked rendering, bilinear height
//   queries, and normal computation for physics/AI.
//
// Key invariants:
//   - Heights stored as float array (width * depth).
//   - Chunks are 16x16 quads, lazily built as Mesh3D objects.
//   - GetHeightAt uses bilinear interpolation between grid points.
//
// Links: rt_canvas3d.h, rt_mesh3d, vgfx3d_frustum.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_terrain3d.h
 * @brief Declares the chunked heightmap-terrain runtime API.
 *
 * Terrain3D owns a sampled height grid, lazily generated chunk meshes, LOD state, splat layers,
 * and authored holes. Queries expose interpolated surface data while draw functions submit
 * conservative, frustum-culled chunk geometry through Canvas3D.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a heightmap terrain of (width × depth) grid samples.
/// @param width Number of height samples along the local x axis.
/// @param depth Number of height samples along the local z axis.
/// @return New GC-managed Terrain3D handle, or NULL for invalid dimensions or allocation failure.
void *rt_terrain3d_new(int64_t width, int64_t depth);

/// @brief Procedurally generate the heightmap from a Perlin noise instance.
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param perlin Perlin generator handle used to sample each grid point.
/// @param scale Input-coordinate frequency scale.
/// @param octaves Number of noise octaves.
/// @param persistence Amplitude multiplier applied between octaves.
void rt_terrain3d_generate_perlin(
    void *terrain, void *perlin, double scale, int64_t octaves, double persistence);

/// @brief Load the heightmap from a Pixels grayscale image (R channel = height).
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param pixels Pixels heightmap whose red channel supplies normalized heights.
void rt_terrain3d_set_heightmap(void *terrain, void *pixels);

/// @brief Bind a Material3D applied to all terrain chunks.
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param material Material3D handle to retain, or NULL to clear the custom material.
void rt_terrain3d_set_material(void *terrain, void *material);

/// @brief Set per-axis world-space scale (sx along X, sy along Y/height, sz along Z).
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param sx World-space spacing between x-axis height samples.
/// @param sy Multiplier converting normalized samples to world-space height.
/// @param sz World-space spacing between z-axis height samples.
void rt_terrain3d_set_scale(void *terrain, double sx, double sy, double sz);

/// @brief Bind a 4-channel splat map (RGBA selects which of 4 layer textures shows at each texel).
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param pixels Pixels handle containing weights for layers 0 through 3, or NULL to clear it.
void rt_terrain3d_set_splat_map(void *terrain, void *pixels);

/// @brief Set splat map @p index: 0 weights layers 0-3, 1 weights layers 4-7.
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param index Splat-map bank index, either 0 or 1.
/// @param pixels RGBA Pixels weight map to retain, or NULL to clear that bank.
void rt_terrain3d_set_splat_map_at(void *terrain, int64_t index, void *pixels);

/// @brief Carve a rectangular hole (terrain-local units); returns its index or -1.
/// @param terrain Terrain3D handle; invalid handles are rejected.
/// @param x Local x coordinate of the rectangle origin.
/// @param z Local z coordinate of the rectangle origin.
/// @param width Rectangle extent along x.
/// @param depth Rectangle extent along z.
/// @return Zero-based authored-hole index, or -1 when the request cannot be stored.
int64_t rt_terrain3d_set_hole(void *terrain, double x, double z, double width, double depth);

/// @brief Remove the hole at @p index; 1 on success.
/// @param terrain Terrain3D handle; invalid handles are rejected.
/// @param index Zero-based authored-hole index.
/// @return 1 when a hole was removed, otherwise 0.
int8_t rt_terrain3d_remove_hole(void *terrain, int64_t index);

/// @brief Remove every authored hole.
/// @param terrain Terrain3D handle; invalid handles are ignored.
void rt_terrain3d_clear_holes(void *terrain);

/// @brief Number of authored holes.
/// @param terrain Terrain3D handle.
/// @return Current authored-hole count, or zero for an invalid handle.
int64_t rt_terrain3d_get_hole_count(void *terrain);

/// @brief Internal: borrow the rasterized hole bitmask (bit per cell) or NULL.
/// @param terrain Terrain3D handle whose hole mask is requested.
/// @param out_cells_x Optional output receiving the number of mask cells along x.
/// @param out_cells_z Optional output receiving the number of mask cells along z.
/// @return Borrowed packed cell mask, or NULL when no rasterized mask is available.
const uint8_t *rt_terrain3d_get_hole_mask_raw(void *terrain,
                                              int32_t *out_cells_x,
                                              int32_t *out_cells_z);

/// @brief Configure a slope-band rule (degrees) generating splat weights for @p layer.
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param layer Target texture layer from 0 through 7.
/// @param min_slope_deg Lower slope angle for the band, in degrees.
/// @param max_slope_deg Upper slope angle for the band, in degrees.
/// @param sharpness Transition sharpness at band boundaries.
void rt_terrain3d_set_slope_layer(
    void *terrain, int64_t layer, double min_slope_deg, double max_slope_deg, double sharpness);

/// @brief Configure a height-band rule (world Y) generating splat weights for @p layer.
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param layer Target texture layer from 0 through 7.
/// @param min_y Lower world-space height for the band.
/// @param max_y Upper world-space height for the band.
/// @param sharpness Transition sharpness at band boundaries.
void rt_terrain3d_set_height_layer(
    void *terrain, int64_t layer, double min_y, double max_y, double sharpness);

/// @brief Regenerate both splat maps from the configured slope/height rules.
/// @param terrain Terrain3D handle; invalid handles are ignored.
void rt_terrain3d_rebuild_splat_weights(void *terrain);

/// @brief Internal: borrow splat map @p index (0 or 1), or NULL.
/// @param terrain Terrain3D handle.
/// @param index Splat-map bank index, either 0 or 1.
/// @return Borrowed Pixels handle for the requested bank, or NULL when unavailable.
void *rt_terrain3d_get_splat_map_raw(void *terrain, int64_t index);

/// @brief Bind a texture for one of the eight splat layers (@p layer in 0..7).
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param layer Texture layer index from 0 through 7.
/// @param pixels Pixels texture to retain, or NULL to clear the layer.
void rt_terrain3d_set_layer_texture(void *terrain, int64_t layer, void *pixels);

/// @brief Set the UV tiling scale of one splat layer.
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param layer Texture layer index from 0 through 7.
/// @param scale UV tiling multiplier for the selected layer.
void rt_terrain3d_set_layer_scale(void *terrain, int64_t layer, double scale);

/// @brief Bilinearly interpolate the terrain height at world-space (x, z).
/// @param terrain Terrain3D handle.
/// @param x World-space x coordinate in the terrain's local frame.
/// @param z World-space z coordinate in the terrain's local frame.
/// @return Interpolated world-space height with out-of-grid coordinates clamped to the nearest
/// edge, or zero when the query is invalid.
double rt_terrain3d_get_height_at(void *terrain, double x, double z);

/// @brief Compute the surface normal at world-space (x, z) as a Vec3.
/// @param terrain Terrain3D handle.
/// @param x World-space x coordinate in the terrain's local frame.
/// @param z World-space z coordinate in the terrain's local frame.
/// @return New GC-managed Vec3 surface normal, or NULL when the query cannot be evaluated.
void *rt_terrain3d_get_normal_at(void *terrain, double x, double z);
/// @brief Configure level-of-detail switch distances (chunks past @p far_dist render as low-poly).
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param near_dist Distance at which chunks transition from LOD0 to LOD1.
/// @param far_dist Distance at which chunks transition from LOD1 to LOD2.
void rt_terrain3d_set_lod_distances(void *terrain, double near_dist, double far_dist);

/// @brief Configure the LOD hysteresis band that prevents threshold-edge chunk flicker.
/// @details The distance is measured in world units. Positive values make a chunk keep its current
///   LOD until the camera moves beyond a threshold plus/minus the band; zero restores immediate
///   threshold switching. The runtime clamps the band to a safe fraction of the configured
///   near/far LOD interval.
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param distance Requested nonnegative hysteresis width in world units.
void rt_terrain3d_set_lod_hysteresis(void *terrain, double distance);

/// @brief Set the depth of skirts dropped from chunk edges (hides cracks between LOD seams).
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param depth Nonnegative skirt depth in world units; zero disables skirts.
void rt_terrain3d_set_skirt_depth(void *terrain, double depth);

/// @brief Enable or disable Terrain3D participation in Canvas3D CPU occlusion testing/writes.
/// @details Disabled by default because terrain chunk AABBs are not solid occluders and can hide
///   visible floor triangles at the horizon. Frustum culling remains active either way.
/// @param terrain Terrain3D handle; invalid handles are ignored.
/// @param enabled Nonzero to opt in to CPU occlusion participation, zero to disable it.
void rt_terrain3d_set_cpu_occlusion(void *terrain, int8_t enabled);

/// @brief Return whether Terrain3D CPU occlusion participation is enabled.
/// @param terrain Terrain3D handle.
/// @return 1 when CPU occlusion is enabled, otherwise 0.
int8_t rt_terrain3d_get_cpu_occlusion(void *terrain);

/// @brief Number of chunks in the most recent terrain draw attempt.
/// @param terrain Terrain3D handle.
/// @return Total chunk count recorded by the latest draw, or zero for an invalid handle.
int64_t rt_terrain3d_get_last_chunk_count(void *terrain);

/// @brief Number of chunks submitted to Canvas3D by the most recent terrain draw.
/// @param terrain Terrain3D handle.
/// @return Submitted chunk count from the latest draw, or zero for an invalid handle.
int64_t rt_terrain3d_get_last_drawn_chunk_count(void *terrain);

/// @brief Number of chunks rejected by Terrain3D's conservative frustum pre-pass.
/// @param terrain Terrain3D handle.
/// @return Frustum-culled chunk count from the latest draw, or zero for an invalid handle.
int64_t rt_terrain3d_get_last_frustum_culled_chunk_count(void *terrain);

/// @brief Number of chunks whose selected LOD was missing and required fallback or skipped.
/// @param terrain Terrain3D handle.
/// @return Missing-LOD chunk count from the latest draw, or zero for an invalid handle.
int64_t rt_terrain3d_get_last_missing_lod_count(void *terrain);

/// @brief Number of chunks whose selected LOD was clamped to maintain neighbor seam safety.
/// @param terrain Terrain3D handle.
/// @return Seam-clamped chunk count from the latest draw, or zero for an invalid handle.
int64_t rt_terrain3d_get_last_lod_clamped_chunk_count(void *terrain);

/// @brief Number of chunks drawn at LOD0 during the most recent terrain draw.
/// @param terrain Terrain3D handle.
/// @return Latest LOD0 draw count, or zero for an invalid handle.
int64_t rt_terrain3d_get_last_lod0_chunk_count(void *terrain);

/// @brief Number of chunks drawn at LOD1 during the most recent terrain draw.
/// @param terrain Terrain3D handle.
/// @return Latest LOD1 draw count, or zero for an invalid handle.
int64_t rt_terrain3d_get_last_lod1_chunk_count(void *terrain);

/// @brief Number of chunks drawn at LOD2 during the most recent terrain draw.
/// @param terrain Terrain3D handle.
/// @return Latest LOD2 draw count, or zero for an invalid handle.
int64_t rt_terrain3d_get_last_lod2_chunk_count(void *terrain);

/// @brief Internal edge identifiers used by streaming terrain seam stitching.
enum {
    RT_TERRAIN3D_EDGE_WEST = 0,
    RT_TERRAIN3D_EDGE_EAST = 1,
    RT_TERRAIN3D_EDGE_NORTH = 2,
    RT_TERRAIN3D_EDGE_SOUTH = 3,
};

/// @brief Internal helper: average two terrain border edges in world-height space.
/// @param terrain Terrain3D handle whose selected edge is modified.
/// @param edge Edge identifier from `RT_TERRAIN3D_EDGE_*`.
/// @param neighbor Neighboring Terrain3D handle supplying the opposite samples.
/// @param neighbor_edge Edge identifier on @p neighbor.
/// @return Number of border sample pairs changed, or zero when the request is invalid or already
/// seamless.
int64_t rt_terrain3d_stitch_edge(void *terrain,
                                 int64_t edge,
                                 void *neighbor,
                                 int64_t neighbor_edge);

/// @brief Internal helper: build a Pixels heightmap from the terrain's current height grid.
/// @param terrain Terrain3D handle.
/// @return New Pixels object containing the current normalized height grid, or NULL on failure.
void *rt_terrain3d_build_heightmap_pixels(void *terrain);

/// @brief Internal helper: build a Mesh3D approximation of the terrain for nav baking.
/// @param terrain Terrain3D handle.
/// @param step Positive sample stride controlling navigation-mesh resolution.
/// @return New Mesh3D approximation excluding authored holes, or NULL on invalid input or failure.
void *rt_terrain3d_build_nav_mesh(void *terrain, int64_t step);

/// @brief Render @p terrain (LOD-selected, frustum-culled chunks) onto the
///        3D canvas.
/// @param canvas Canvas3D handle receiving terrain draw commands.
/// @param terrain Terrain3D handle to render at the origin.
void rt_canvas3d_draw_terrain(void *canvas, void *terrain);

/// @brief Internal helper: render @p terrain translated into world space.
/// @param canvas Canvas3D handle receiving terrain draw commands.
/// @param terrain Terrain3D handle to render.
/// @param x World-space x translation.
/// @param y World-space y translation.
/// @param z World-space z translation.
void rt_canvas3d_draw_terrain_at(void *canvas, void *terrain, double x, double y, double z);

#ifdef __cplusplus
}
#endif
