//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_navmesh3d.h
// Purpose: 3D navigation mesh — slope-filtered walkable triangles from level
//   geometry, A* pathfinding on triangle adjacency graph, position snapping.
//
// Key invariants:
//   - Built from Mesh3D by filtering triangles whose normal.y > cos(max_slope).
//   - Adjacency: triangles sharing 2 vertices are neighbors.
//   - A* uses centroid-to-centroid distance weighted by polygon traversal cost.
//   - FindPath returns a Path3D with waypoints through triangle centroids.
//   - Read-only path queries lease independent retained workspaces and can overlap.
//   - SamplePosition expands through the spatial index with an exact bounded fallback.
//
// Ownership/Lifetime:
//   - NavMesh3D owns baked polygons, adjacency, grid state, and query workspaces.
//   - Returned Path3D/query values are GC-managed and do not borrow workspace arrays.
//
// Links: rt_path3d.h, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_navmesh3d.h
 * @brief Declares NavMesh3D baking, mutation, pathfinding, sampling, and diagnostics.
 *
 * NavMesh3D copies source geometry into owned walkable topology, supports transactional slope and
 * obstacle updates, stores authored area and off-mesh-link metadata, and answers overlapping path
 * and nearest-point queries through independent retained workspaces. Returned runtime objects are
 * GC-managed; explicitly copied point arrays transfer ownership to the caller.
 */
#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Bake a navigation mesh from a source @p mesh for an agent of the
///        given radius/height.
/// @param mesh Borrowed Mesh3D source geometry.
/// @param agent_radius Requested non-negative agent radius.
/// @param agent_height Requested non-negative agent height.
/// @return New GC-managed NavMesh3D handle, or null for invalid input, empty geometry, or failure.
void *rt_navmesh3d_build(void *mesh, double agent_radius, double agent_height);
/// @brief Bake a navigation mesh from all Mesh3D nodes in a Scene3D.
/// @param scene Borrowed Scene3D whose mesh-node geometry is collected in world space.
/// @param agent_radius Requested non-negative agent radius.
/// @param agent_height Requested non-negative agent height.
/// @param max_slope Maximum walkable face slope in degrees.
/// @param cell_size Requested voxel cell edge length.
/// @return New GC-managed baked NavMesh3D, or null for invalid input or bake failure.
void *rt_navmesh3d_bake(
    void *scene, double agent_radius, double agent_height, double max_slope, double cell_size);
/// @brief Bake a scene navmesh using the tiled API shape. Current baseline bakes the full scene.
/// @param scene Borrowed Scene3D whose mesh-node geometry is collected.
/// @param tile_size Positive world-space tile edge length used for localized rebuilds.
/// @param agent_radius Requested non-negative agent radius.
/// @param agent_height Requested non-negative agent height.
/// @param max_slope Maximum walkable face slope in degrees.
/// @param cell_size Requested voxel cell edge length.
/// @return New GC-managed tiled NavMesh3D, or null for invalid input or bake failure.
void *rt_navmesh3d_bake_tiled(void *scene,
                              double tile_size,
                              double agent_radius,
                              double agent_height,
                              double max_slope,
                              double cell_size);
/// @brief Find a path between Vec3 @p from and @p to.
/// @param navmesh Opaque NavMesh3D handle.
/// @param from Borrowed Vec3 start position.
/// @param to Borrowed Vec3 goal position.
/// @return New GC-managed Path3D point list, or null when invalid or unreachable.
void *rt_navmesh3d_find_path(void *navmesh, void *from, void *to);
/// @brief Find a path between Vec3 @p from and @p to as Some(Path3D), or None if unreachable.
/// @param navmesh Opaque NavMesh3D handle.
/// @param from Borrowed Vec3 start position.
/// @param to Borrowed Vec3 goal position.
/// @return Runtime option containing a GC-managed Path3D, or None.
void *rt_navmesh3d_find_path_option(void *navmesh, void *from, void *to);
/// @brief Serialize a baked navmesh to a "VNAVMSH2" binary file.
/// @details Records vertices, source/current triangles, traversal costs, blocked flags, area
///          labels, off-mesh links, obstacles, and agent params. Voxel heightfield source arrays
///          are runtime-derived and are not serialized.
/// @param navmesh Opaque baked NavMesh3D handle.
/// @param path Runtime string naming the destination file.
/// @return 1 after a complete write, otherwise 0 for invalid input or I/O failure.
int8_t rt_navmesh3d_export(void *navmesh, rt_string path);
/// @brief Reconstruct a path-queryable navmesh from "VNAVMSH2" or legacy "VNAVMSH1".
/// @param path Runtime string naming the source file.
/// @return Imported navmesh, or NULL on a missing/corrupt/trailing-data file.
void *rt_navmesh3d_import(rt_string path);
/// @brief Snap a Vec3 @p point onto the nearest walkable navmesh location.
/// @param navmesh Opaque NavMesh3D handle.
/// @param point Borrowed Vec3 query position.
/// @return New GC-managed Vec3 at the nearest walkable location. Invalid handles produce the
///         origin, and a valid mesh without triangles returns the sanitized input position.
void *rt_navmesh3d_sample_position(void *navmesh, void *point);
/// @brief Whether a Vec3 @p point lies on a walkable navmesh polygon.
/// @param navmesh Opaque NavMesh3D handle.
/// @param point Borrowed Vec3 query position.
/// @return 1 when the point resolves to a walkable triangle, otherwise 0.
int8_t rt_navmesh3d_is_walkable(void *navmesh, void *point);
/// @brief Number of triangles in the baked navmesh.
/// @param navmesh Opaque NavMesh3D handle.
/// @return Non-negative current walkable-triangle count, or zero for an invalid handle.
int64_t rt_navmesh3d_get_triangle_count(void *navmesh);
/// @brief Cost of the most recent successful FindPath/CopyPathPoints query.
/// @param navmesh Opaque NavMesh3D handle.
/// @return Latest path traversal cost, or zero when unavailable.
double rt_navmesh3d_get_last_path_cost(void *navmesh);
/// @brief Add an authored off-mesh traversal link between two walkable points.
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param from Borrowed Vec3 start endpoint.
/// @param to Borrowed Vec3 destination endpoint.
/// @param bidirectional Non-zero to permit traversal in both directions.
/// @return 1 on success, otherwise 0 for invalid endpoints, allocation failure, or mutation error.
int8_t rt_navmesh3d_add_offmesh_link(void *navmesh, void *from, void *to, int8_t bidirectional);
/// @brief Tile edge length for O(tile) rebuilds (0 for Build-constructed meshes).
/// @param navmesh Opaque NavMesh3D handle.
/// @return Positive tile edge length for tiled bakes, otherwise zero.
double rt_navmesh3d_get_tile_size(void *navmesh);
/// @brief True when segment (a -> b) matches a registered off-mesh link
///        (either direction for bidirectional links); optionally reports the
///        link's authored kind string (borrowed).
/// @param navmesh Opaque NavMesh3D handle.
/// @param a Borrowed three-double segment start.
/// @param b Borrowed three-double segment end.
/// @param out_kind Optional output receiving a borrowed link-kind string on a match.
/// @return 1 when the directed segment matches an authored link, otherwise 0.
int8_t rt_navmesh3d_segment_is_offmesh_link(void *navmesh,
                                            const double *a,
                                            const double *b,
                                            rt_string *out_kind);
/// @brief Number of authored off-mesh traversal links.
/// @param navmesh Opaque NavMesh3D handle.
/// @return Non-negative authored-link count, or zero for an invalid handle.
int64_t rt_navmesh3d_get_offmesh_link_count(void *navmesh);
/// @brief Attach kind/cost/state metadata to an authored off-mesh link by index.
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param index Zero-based authored link index.
/// @param kind Runtime string describing the traversal kind.
/// @param traversal_cost Positive traversal-cost multiplier.
/// @param state_flags Authored gameplay state bitset.
/// @return 1 when metadata is applied, otherwise 0.
int8_t rt_navmesh3d_set_offmesh_link_metadata(
    void *navmesh, int64_t index, rt_string kind, double traversal_cost, int64_t state_flags);
/// @brief Read off-mesh link metadata by index.
/// @param navmesh Opaque NavMesh3D handle.
/// @param index Zero-based authored link index.
/// @return Retained runtime kind string, or the shared empty string for invalid input/no kind.
rt_string rt_navmesh3d_get_offmesh_link_kind(void *navmesh, int64_t index);
/// @brief Read the traversal-cost metadata of an authored off-mesh link by index.
/// @param navmesh Opaque NavMesh3D handle.
/// @param index Zero-based authored link index.
/// @return Positive traversal-cost multiplier, or zero for invalid input.
double rt_navmesh3d_get_offmesh_link_traversal_cost(void *navmesh, int64_t index);
/// @brief Read the state-flag metadata of an authored off-mesh link by index.
/// @param navmesh Opaque NavMesh3D handle.
/// @param index Zero-based authored link index.
/// @return Authored state bitset, or zero for invalid input.
int64_t rt_navmesh3d_get_offmesh_link_state(void *navmesh, int64_t index);
/// @brief Add a coarse AABB obstacle that removes overlapping walkable triangles.
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param min Borrowed Vec3 first obstacle corner.
/// @param max Borrowed Vec3 opposite obstacle corner.
/// @return 1 when the obstacle is appended and affected topology is reflagged, otherwise 0.
int8_t rt_navmesh3d_add_obstacle(void *navmesh, void *min, void *max);
/// @brief Remove an authored coarse AABB obstacle by index and refilter.
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param index Zero-based obstacle index.
/// @return 1 when the obstacle is removed and topology updated, otherwise 0.
int8_t rt_navmesh3d_remove_obstacle(void *navmesh, int64_t index);
/// @brief Update an authored coarse AABB obstacle by index and refilter.
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param index Zero-based obstacle index.
/// @param min Borrowed Vec3 replacement minimum corner.
/// @param max Borrowed Vec3 replacement maximum corner.
/// @return 1 when the obstacle and affected topology are updated, otherwise 0.
int8_t rt_navmesh3d_update_obstacle(void *navmesh, int64_t index, void *min, void *max);
/// @brief Number of authored coarse AABB obstacles.
/// @param navmesh Opaque NavMesh3D handle.
/// @return Non-negative obstacle count, or zero for an invalid handle.
int64_t rt_navmesh3d_get_obstacle_count(void *navmesh);
/// @brief Assign nav area and traversal cost metadata to polygons overlapping an AABB volume.
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param min Borrowed Vec3 volume minimum corner.
/// @param max Borrowed Vec3 volume maximum corner.
/// @param area Runtime string naming the navigation area.
/// @param traversal_cost Positive A* traversal-cost multiplier.
/// @return 1 when one or more source polygons are updated, otherwise 0.
int8_t rt_navmesh3d_set_area(
    void *navmesh, void *min, void *max, rt_string area, double traversal_cost);
/// @brief Read nav area and traversal cost metadata at a walkable position.
/// @param navmesh Opaque NavMesh3D handle.
/// @param point Borrowed Vec3 query position.
/// @return Retained runtime area-name string, or the shared empty string when no area is found.
rt_string rt_navmesh3d_get_area(void *navmesh, void *point);
/// @brief Read the traversal-cost metadata at a walkable position.
/// @param navmesh Opaque NavMesh3D handle.
/// @param point Borrowed Vec3 query position.
/// @return Positive polygon traversal-cost multiplier, or zero when the query is invalid.
double rt_navmesh3d_get_traversal_cost(void *navmesh, void *point);
/// @brief Re-carve a single tile of a tiled bake in place (O(tile): no adjacency/grid rebuild).
///        Falls back to a whole-mesh refilter for non-tiled meshes. Tile (tx,tz) covers world XZ
///        [meshMin + t*tile_size, meshMin + (t+1)*tile_size).
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param tile_x Signed X tile coordinate.
/// @param tile_z Signed Z tile coordinate.
/// @return 1 when the requested tile or fallback topology is rebuilt, otherwise 0.
int8_t rt_navmesh3d_rebuild_tile(void *navmesh, int64_t tile_x, int64_t tile_z);
/// @brief Set the maximum walkable slope (degrees) and transactionally rebuild derived topology.
/// @details If staging adjacency or the spatial query index fails, the previous slope, triangles,
///          neighbor graph, and query results remain published unchanged.
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param degrees Requested maximum slope in degrees.
void rt_navmesh3d_set_max_slope(void *navmesh, double degrees);
/// @brief Set the A* heuristic policy: 0 = strict/optimal (Dijkstra once any
///        off-mesh link exists), 1 = always Euclidean (faster, near-optimal).
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param mode Zero for strict optimal behavior, non-zero for always-Euclidean behavior.
void rt_navmesh3d_set_heuristic_mode(void *navmesh, int64_t mode);
/// @brief Current A* heuristic policy (0 strict, 1 euclidean).
/// @param navmesh Opaque NavMesh3D handle.
/// @return Zero for strict mode or one for always-Euclidean mode; invalid handles return zero.
int64_t rt_navmesh3d_get_heuristic_mode(void *navmesh);
/// @brief Draw the navmesh polygons as a debug overlay onto @p canvas.
/// @param navmesh Opaque NavMesh3D handle.
/// @param canvas Borrowed Canvas3D destination.
void rt_navmesh3d_debug_draw(void *navmesh, void *canvas);

/* Runtime integration helper used by NavAgent3D. Returns malloc'd xyz triples. */
/// @brief Compute a path from @p from to @p to and copy it into a freshly
///        malloc'd flat xyz array.
/// @param navmesh Opaque NavMesh3D handle.
/// @param from Borrowed Vec3 start position.
/// @param to Borrowed Vec3 goal position.
/// @param out_points_xyz Receives an owned flat `[x,y,z,...]` allocation on success.
/// @return Positive point count on success, otherwise zero. The caller frees the published array.
int64_t rt_navmesh3d_copy_path_points(void *navmesh, void *from, void *to, double **out_points_xyz);

/// @brief Test-only: verify the spatial query-grid point location matches a brute-force linear
///        scan over a sample lattice spanning the navmesh bounds.
/// @param navmesh Opaque NavMesh3D handle.
/// @return 1 if all samples agree or the navmesh has no grid, otherwise 0.
int8_t rt_navmesh3d_check_query_grid_parity(void *navmesh);

/// @brief Test-only: reset the peak count of concurrently active A* searches.
/// @details Call while no path query is running. The hook does not alter query results or become
///          part of the scripting surface.
/// @param navmesh NavMesh3D handle whose overlap watermark is reset.
void rt_navmesh3d_test_reset_path_peak_concurrency(void *navmesh);
/// @brief Test-only: read the peak number of A* queries that owned distinct workspace slots.
/// @param navmesh NavMesh3D handle to inspect.
/// @return Peak overlapping query count, or zero for an invalid handle.
int64_t rt_navmesh3d_test_get_path_peak_concurrency(void *navmesh);

/// @brief Return the number of path workspaces retaining corridor and portal scratch buffers.
int64_t rt_navmesh3d_test_get_ready_path_scratch_count(void *navmesh);

/// @brief Return the number of currently available path-workspace gate permits.
int64_t rt_navmesh3d_test_get_path_workspace_permits(void *navmesh);
/// @brief Test-only: read triangle-reference probes from the latest SamplePosition query.
/// @details Duplicate cell references and bounded-fallback probes are included conservatively.
/// @param navmesh NavMesh3D handle to inspect.
/// @return Non-negative triangle probe count, or zero for an invalid handle.
int64_t rt_navmesh3d_test_get_last_sample_probe_count(void *navmesh);
/// @brief Test-only: whether the latest SamplePosition query required its bounded linear fallback.
/// @param navmesh NavMesh3D handle to inspect.
/// @return One after fallback, otherwise zero.
int8_t rt_navmesh3d_test_get_last_sample_used_fallback(void *navmesh);

/// @brief Test-only: append an obstacle WITHOUT re-flagging any tile, leaving the tiled navmesh in
///        a deliberately stale state so a subsequent RebuildTile can be shown to affect exactly one
///        tile.
/// @param navmesh Opaque mutable NavMesh3D handle.
/// @param min Borrowed Vec3 obstacle minimum.
/// @param max Borrowed Vec3 obstacle maximum.
/// @return 1 on success, otherwise 0 for invalid input or allocation failure.
int8_t rt_navmesh3d_test_inject_obstacle(void *navmesh, void *min, void *max);
/// @brief Test-only: map a world XZ position to its tile coordinates.
/// @param navmesh Opaque NavMesh3D handle.
/// @param px World X coordinate.
/// @param pz World Z coordinate.
/// @param out_tx Receives the signed X tile coordinate.
/// @param out_tz Receives the signed Z tile coordinate.
/// @return 1 with outputs written for a valid tiled mesh, otherwise 0.
int8_t rt_navmesh3d_test_tile_of_point(
    void *navmesh, double px, double pz, int64_t *out_tx, int64_t *out_tz);
/// @brief Test-only: edit retained tiled voxel source for one tile without rebuilding it.
/// @param navmesh Opaque mutable tiled NavMesh3D handle.
/// @param tile_x Signed X tile coordinate.
/// @param tile_z Signed Z tile coordinate.
/// @param height Replacement source height for cells in the tile.
/// @param walkable Non-zero to mark edited cells walkable.
/// @return 1 when retained voxel source is updated, otherwise 0.
int8_t rt_navmesh3d_test_set_tile_source(
    void *navmesh, int64_t tile_x, int64_t tile_z, double height, int8_t walkable);

/// @brief Test-only: enable deterministic query-grid staging failure before live mutation.
/// @details The process-global hook is not part of the scripting surface. Callers must restore zero
///          after the assertion so later navigation builds use normal allocation behavior.
/// @param enabled Nonzero to force staging failure; zero to disable injection.
void rt_navmesh3d_test_set_query_grid_alloc_failure(int8_t enabled);

/// @brief Test-only: enable deterministic adjacency staging failure before live mutation.
/// @details The process-global hook is not part of the scripting surface. Callers must restore zero
///          after the assertion so later navigation builds use normal allocation behavior.
/// @param enabled Nonzero to force staging failure; zero to disable injection.
void rt_navmesh3d_test_set_adjacency_alloc_failure(int8_t enabled);

/// @brief Test-only: calculate bounded voxel-grid dimensions without allocating grid storage.
/// @details Uses production sanitization, coarsening, and checked product logic. Every output is
///          written only on success; the entry point is not part of the scripting surface.
/// @param span_x Nonnegative finite X extent.
/// @param span_z Nonnegative finite Z extent.
/// @param requested_cell_size Requested cell size before sanitization/coarsening.
/// @param out_nx Receives the bounded X dimension.
/// @param out_nz Receives the bounded Z dimension.
/// @param out_cell_count Receives the checked product of both dimensions.
/// @param out_effective_cell_size Receives the final coarsened cell size.
/// @return 1 on success, or 0 for invalid inputs/unrepresentable arithmetic.
int8_t rt_navmesh3d_test_voxel_grid_dimensions(double span_x,
                                               double span_z,
                                               double requested_cell_size,
                                               int32_t *out_nx,
                                               int32_t *out_nz,
                                               uint64_t *out_cell_count,
                                               double *out_effective_cell_size);

#ifdef __cplusplus
}
#endif
