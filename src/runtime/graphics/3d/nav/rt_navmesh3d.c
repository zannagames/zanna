//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_navmesh3d.c
// Purpose: 3D navigation mesh with slope-filtered walkable triangles, adjacency
//   graph, A* pathfinding, and position snapping.
//
// Key invariants:
//   - Phase 1: copy vertices from source Mesh3D.
//   - Phase 2: filter triangles by face normal (walkable = normal.y > cos(slope)).
//   - Phase 3: build adjacency via edge-keyed hash table (shared edges
//     cross-link the two triangles that own them).
//   - A*: binary min-heap on f = g + h, centroid-to-centroid edge cost.
//   - Up to four A* queries own independent retained workspaces on one mesh.
//   - Off-mesh nearest-point queries expand through the retained XZ grid and
//     enter an exact linear fallback only after a fixed work budget.
//   - String-pulling smooths waypoints through portal midpoints; falls back
//     to centroids when portal extraction fails.
//   - FindPath returns a Path3D with waypoints through portals.
//
// Ownership/Lifetime:
//   - NavMesh3D is GC-managed; finalizer frees geometry, indices, and every
//     retained query-workspace array.
//   - The source mesh is borrowed during Build only — not retained.
//
// Links: rt_navmesh3d.h, rt_path3d.h, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_navmesh3d.c
 * @brief Defines NavMesh3D storage and assembles its build, bake, and query implementations.
 *
 * A NavMesh3D owns copied navigation geometry, filtered triangle topology, off-mesh links, dynamic
 * obstacles, area metadata, an XZ query grid, voxel-bake state, and a bounded pool of independent
 * A* workspaces. The implementation is split into focused include units that share these private
 * types: rt_navmesh3d_build.inc, rt_navmesh3d_bake.inc, and rt_navmesh3d_query.inc.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_navmesh3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_file_stdio.h"
#include "rt_game3d_diagnostics.h"
#include "rt_graphics3d_ids.h"
#include "rt_mat4.h"
#include "rt_option.h"
#include "rt_path3d.h"
#include "rt_platform.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_threads.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NAVMESH3D_COORD_ABS_MAX 1000000000.0
#define NAVMESH3D_AGENT_DIM_MAX 1000000.0
#define NAVMESH3D_CELL_SIZE_MIN 0.001
#define NAVMESH3D_CELL_SIZE_MAX 1000000.0
#define NAVMESH3D_TILE_SIZE_MAX 1000000000.0
#define NAVMESH3D_TILE_INDEX_ABS_MAX 1000000000LL
#define NAVMESH3D_MAX_TILES_PER_REFLAG 65536LL
#define NAVMESH3D_IMPORT_MAX_RECORDS (1 << 24)
#define NAVMESH3D_IMPORT_MAX_STRING_BYTES (1u << 20)
#define NAVMESH3D_VOXEL_MAX_DIM 512LL
#define NAVMESH3D_VOXEL_MIN_CELLS 16384LL
#define NAVMESH3D_VOXEL_ABS_MAX_CELLS 262144LL
#define NAVMESH3D_VOXEL_TARGET_BYTES (64u * 1024u * 1024u)
#define NAVMESH3D_QUERY_GRID_MAX_DIM 512
#define NAVMESH3D_QUERY_GRID_MAX_CELLS 262144u
#define NAVMESH3D_QUERY_GRID_MAX_REFERENCES (8u * 1024u * 1024u)
#define NAVMESH3D_PATH_WORKSPACE_COUNT 4
#define NAVMESH3D_SAMPLE_MAX_GRID_CELLS 4096
#define NAVMESH3D_SAMPLE_MAX_GRID_REFERENCES 131072

static int8_t g_navmesh3d_test_force_query_grid_alloc_failure = 0;
static int8_t g_navmesh3d_test_force_adjacency_alloc_failure = 0;

/// @brief Enable or disable deterministic query-grid allocation failure injection.
/// @details This test-only hook fails the next query-grid staging attempt before any allocation or
///   live-state mutation. It is process-global and must be reset by the calling test. The hook is
///   intentionally absent from the scripting surface.
/// @param enabled Nonzero to force failure; zero to restore normal allocation behavior.
void rt_navmesh3d_test_set_query_grid_alloc_failure(int8_t enabled) {
    g_navmesh3d_test_force_query_grid_alloc_failure = enabled ? 1 : 0;
}

/// @brief Enable or disable deterministic triangle-adjacency allocation failure injection.
/// @details This test-only hook fails adjacency staging before any allocation or live-neighbor
///   mutation. It is process-global and must be reset by the calling test. The hook is
///   intentionally absent from the scripting surface.
/// @param enabled Nonzero to force failure; zero to restore normal allocation behavior.
void rt_navmesh3d_test_set_adjacency_alloc_failure(int8_t enabled) {
    g_navmesh3d_test_force_adjacency_alloc_failure = enabled ? 1 : 0;
}

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_path3d_new(void);
extern void rt_path3d_add_point(void *path, void *pos);
extern void rt_canvas3d_draw_line3d(void *obj, void *from, void *to, int64_t color);
extern rt_string rt_string_from_bytes(const char *bytes, size_t len);

typedef struct {
    float position[3];
} nav_vertex_t;

typedef struct {
    int32_t v[3];
    int32_t neighbors[3]; /* adjacent tri per edge, -1 = boundary */
    float centroid[3];
    float normal[3];
    int8_t blocked;  /* 1 = carved out by a dynamic obstacle; kept in the array but skipped by every
                        query */
    int32_t area_id; /* 0 = default, >0 indexes nm->area_names[id-1] */
    float traversal_cost; /* >=1 multiplier used by A* */
} nav_triangle_t;

typedef struct {
    float from[3];
    float to[3];
    int32_t from_tri;
    int32_t to_tri;
    int8_t bidirectional;
    int64_t state_flags;
    float traversal_cost; /* >=1 multiplier over endpoint distance */
    rt_string kind;       /* optional authored traversal kind/state name */
} nav_offmesh_link_t;

/// @brief Packed directed off-mesh edge reference used by the per-triangle adjacency cache.
/// @details The high bits store the index into `offmesh_links`; bit 0 is set when the edge is the
///   reverse direction of a bidirectional link. This keeps the A* hot loop cache compact.
typedef int32_t nav_offmesh_edge_ref_t;

typedef struct {
    float min[3];
    float max[3];
} nav_obstacle_t;

/// @brief One entry in the reusable A* open-set heap.
typedef struct {
    int32_t tri;
    float f;
} navmesh3d_heap_entry_t;

typedef struct {
    float left[3];
    float right[3];
} navmesh3d_portal_t;

/// @brief Independently owned mutable storage for one in-flight A* query.
/// @details A NavMesh3D retains a small fixed pool of these slots. A query exclusively claims one
///   slot before growing or writing its arrays, so searches on the same immutable topology can
///   overlap without sharing mutable costs, parents, closed flags, or heap entries.
typedef struct {
    float *g_cost;
    int32_t *parent;
    int8_t *closed;
    navmesh3d_heap_entry_t *heap;
    int32_t *corridor;
    navmesh3d_portal_t *portals;
    int32_t triangle_capacity;
    int32_t heap_capacity;
    int32_t corridor_capacity;
    int32_t portal_capacity;
    volatile int in_use;
} navmesh3d_path_workspace_t;

typedef struct {
    void *vptr;
    nav_vertex_t *vertices;
    int32_t vertex_count;
    nav_triangle_t *source_triangles;
    int32_t source_triangle_count;
    nav_triangle_t *triangles;
    int32_t triangle_count;
    nav_offmesh_link_t *offmesh_links;
    int32_t offmesh_link_count;
    int32_t offmesh_link_capacity;
    int32_t *offmesh_adjacency_starts;
    nav_offmesh_edge_ref_t *offmesh_adjacency_edges;
    int32_t offmesh_adjacency_edge_count;
    rt_string *area_names;
    int32_t area_name_count;
    int32_t area_name_capacity;
    nav_obstacle_t *obstacles;
    int32_t obstacle_count;
    int32_t obstacle_capacity;
    double last_path_cost;
    /* Bounded pool of independently reusable A* workspaces. Queries serialize only when every
     * slot is occupied; up to NAVMESH3D_PATH_WORKSPACE_COUNT searches otherwise overlap. */
    navmesh3d_path_workspace_t path_workspaces[NAVMESH3D_PATH_WORKSPACE_COUNT];
    void *path_workspace_gate;
    volatile int path_active_queries;
    volatile int path_peak_active_queries;
    double agent_radius;
    double agent_height;
    double max_slope;              /* degrees */
    int8_t skip_portal_width_gate; /* voxel bake enforces clearance via erosion, not portal width */
    /* A* heuristic policy: 0 = strict (drops to Dijkstra when any off-mesh link
     * exists, guaranteeing optimal paths), 1 = always Euclidean (Recast-style —
     * much faster on large meshes with links, paths may be slightly suboptimal
     * when a link shortcut beats straight-line distance). */
    int8_t heuristic_mode;
    /* >0 => tiled bake: RebuildTile / tiled AddObstacle re-flag one tile's triangles in place
     * (O(tile), no adjacency/grid rebuild). 0 => non-tiled: obstacle edits whole-mesh refilter.
     * Tile (tx,tz) covers world XZ [qgrid_min + t*tile_size, qgrid_min + (t+1)*tile_size). */
    double tile_size;
    /* Uniform XZ grid index over `triangles` for fast point location (find_tri), rebuilt whenever
     * apply_slope_filter rebuilds the triangle set. Empty (qgrid_nx == 0) => linear-scan fallback.
     * CSR layout: qgrid_starts[c]..qgrid_starts[c+1] indexes qgrid_tris for cell c. */
    double qgrid_min_x;
    double qgrid_min_z;
    double qgrid_inv_cell;
    int32_t qgrid_nx;
    int32_t qgrid_nz;
    int32_t *qgrid_starts;
    int32_t *qgrid_tris;
    int64_t qgrid_fallback_count;
    volatile int sample_last_triangle_probes;
    volatile int sample_last_used_fallback;
    /* Nonzero after a complete triangle/query representation has first been published. A rebuild
     * allocation failure can then distinguish an initial no-index fallback from a mutation that
     * must retain the prior representation bit-for-bit. */
    uint64_t topology_revision;
    double voxel_min_x;
    double voxel_min_z;
    double voxel_cell_size;
    int32_t voxel_nx;
    int32_t voxel_nz;
    float *voxel_cell_height;
    uint8_t *voxel_cell_walkable;
    int32_t *voxel_corner_vertices;
} rt_navmesh3d;

/// @brief Initialize the fair counting gate that maps callers onto fixed A* workspaces.
static int navmesh3d_init_path_workspace_gate(rt_navmesh3d *nm) {
    if (!nm)
        return 0;
    nm->path_workspace_gate = rt_gate_new(NAVMESH3D_PATH_WORKSPACE_COUNT);
    return nm->path_workspace_gate != NULL;
}

/// @brief Record that a NavMesh3D query-grid build could not publish a new acceleration index.
/// @details The per-object counter is incremented when a valid object is available and the shared
///   Game3D diagnostic counter is always updated. Correct queries continue through either the
///   retained prior grid or the bounded linear-scan fallback.
/// @param nm Navigation mesh associated with the fallback, or NULL for a global-only diagnostic.
static void navmesh3d_record_query_grid_fallback(rt_navmesh3d *nm) {
    if (nm) {
        if (nm->qgrid_fallback_count < 0)
            nm->qgrid_fallback_count = 1;
        else if (nm->qgrid_fallback_count < INT64_MAX)
            nm->qgrid_fallback_count++;
    }
    rt_game3d_diag_record_nav_grid_fallback();
}

// clang-format off
#include "rt_navmesh3d_build.inc"
#include "rt_navmesh3d_bake.inc"
#include "rt_navmesh3d_query.inc"
// clang-format on
#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
