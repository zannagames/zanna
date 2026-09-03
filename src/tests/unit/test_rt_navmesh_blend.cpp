//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_navmesh_blend.cpp
// Purpose: Unit tests for NavMesh3D indexed queries/concurrent A* workspaces,
//   AnimBlend3D, and BlendTree3D.
//
// Key invariants:
//   - Indexed sampling remains exact while bounding local triangle probes.
//   - Independent path queries overlap safely without sharing mutable arrays.
// Ownership/Lifetime:
//   - Worker threads borrow a retained navmesh for the duration of each stress case.
//   - Runtime fixture objects remain owned by the test process.
//
// Links: rt_navmesh3d.h, rt_skeleton3d.h, rt_blendtree3d.h
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt.hpp"
#include "rt_blendtree3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_internal.h"
#include "rt_navmesh3d.h"
#include "rt_option.h"
#include "rt_path3d.h"
#include "rt_scene3d.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
#include <atomic>
#include <cassert>
#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_mesh3d_new_plane(double sx, double sz);
extern void *rt_mesh3d_new(void);
extern void rt_mesh3d_add_vertex(
    void *obj, double x, double y, double z, double nx, double ny, double nz, double u, double v);
extern void rt_mesh3d_add_triangle(void *obj, int64_t v0, int64_t v1, int64_t v2);
extern void *rt_mesh3d_new_box(double sx, double sy, double sz);
extern int64_t rt_path3d_get_point_count(void *path);
extern const char *rt_string_cstr(rt_string s);
extern void *rt_mat4_identity(void);
extern void *rt_skeleton3d_new(void);
extern int64_t rt_skeleton3d_add_bone(void *s, rt_string n, int64_t p, void *m);
extern void rt_skeleton3d_compute_inverse_bind(void *s);
extern void *rt_animation3d_new(rt_string name, double duration);
extern rt_string rt_const_cstr(const char *s);
extern int64_t rt_navmesh3d_copy_path_points(void *navmesh,
                                             void *from,
                                             void *to,
                                             double **out_points_xyz);
extern int64_t rt_navmesh3d_test_get_last_path_touched_count(void *navmesh);
extern int64_t rt_navmesh3d_test_get_last_path_heap_peak(void *navmesh);
extern int64_t rt_navmesh3d_test_get_path_transient_allocation_count(void *navmesh);
extern void rt_navmesh3d_test_reset_path_transient_allocation_count(void *navmesh);
extern int32_t rt_path3d_append_xyz_batch_internal(void *path,
                                                   const double *points_xyz,
                                                   int64_t point_count);
extern void rt_path3d_test_set_coordinate_alloc_failure(int8_t enabled);
}

static int tests_passed = 0;
static int tests_run = 0;
static std::jmp_buf trap_jmp;
static const char *last_trap = nullptr;
static bool expect_trap = false;

extern "C" void vm_trap(const char *msg) {
    last_trap = msg;
    if (expect_trap)
        std::longjmp(trap_jmp, 1);
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

template <typename Fn> static bool expect_trap_contains(Fn &&fn, const char *needle) {
    last_trap = nullptr;
    expect_trap = true;
    if (setjmp(trap_jmp) == 0) {
        fn();
        expect_trap = false;
        return false;
    }
    expect_trap = false;
    return last_trap && (!needle || std::strstr(last_trap, needle) != nullptr);
}

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (fabs((double)(a) - (double)(b)) > (eps)) {                                             \
            fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", msg, (double)(a), (double)(b));    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

/// @brief Build a dense, connected square grid navmesh for query-scaling regressions.
/// @details The helper emits shared vertices and two consistently upward-facing triangles per
///   cell. Its predictable topology makes an edge-local SamplePosition query touch only a few
///   spatial-index cells while a corner-to-corner A* query remains long enough for worker overlap.
/// @param cells Number of unit cells on each XZ axis; must be positive.
/// @return A newly built NavMesh3D handle, or NULL when mesh/navmesh construction fails.
static void *make_dense_grid_navmesh(int cells) {
    if (cells <= 0)
        return nullptr;
    void *mesh = rt_mesh3d_new();
    if (!mesh)
        return nullptr;
    double half = (double)cells * 0.5;
    for (int z = 0; z <= cells; z++) {
        for (int x = 0; x <= cells; x++) {
            rt_mesh3d_add_vertex(mesh,
                                 (double)x - half,
                                 0.0,
                                 (double)z - half,
                                 0.0,
                                 1.0,
                                 0.0,
                                 (double)x / (double)cells,
                                 (double)z / (double)cells);
        }
    }
    int64_t stride = (int64_t)cells + 1;
    for (int z = 0; z < cells; z++) {
        for (int x = 0; x < cells; x++) {
            int64_t a = (int64_t)z * stride + x;
            int64_t b = a + 1;
            int64_t d = a + stride;
            int64_t c = d + 1;
            rt_mesh3d_add_triangle(mesh, a, c, b);
            rt_mesh3d_add_triangle(mesh, a, d, c);
        }
    }
    return rt_navmesh3d_build(mesh, 0.2, 1.8);
}

typedef struct {
    float from[3];
    float to[3];
    int32_t from_tri;
    int32_t to_tri;
    int8_t bidirectional;
    int64_t state_flags;
    float traversal_cost;
    rt_string kind;
} NavMeshOffmeshLinkTestLayout;

typedef struct {
    float min[3];
    float max[3];
} NavMeshObstacleTestLayout;

typedef struct {
    int32_t tri;
    int32_t link;
} NavMeshOffmeshEdgeRefTestLayout;

typedef struct {
    int32_t v[3];
    int32_t neighbors[3];
    float centroid[3];
    float normal[3];
    int8_t blocked;
    int32_t area_id;
    float traversal_cost;
} NavMeshTriangleTestLayout;

typedef struct {
    void *vptr;
    void *vertices;
    int32_t vertex_count;
    void *source_triangles;
    int32_t source_triangle_count;
    void *triangles;
    int32_t triangle_count;
    NavMeshOffmeshLinkTestLayout *offmesh_links;
    int32_t offmesh_link_count;
    int32_t offmesh_link_capacity;
    int32_t *offmesh_adjacency_starts;
    NavMeshOffmeshEdgeRefTestLayout *offmesh_adjacency_edges;
    int32_t offmesh_adjacency_edge_count;
    volatile int offmesh_adjacency_state;
    int8_t offmesh_adjacency_degraded;
    int64_t offmesh_adjacency_fallback_count;
    rt_string *area_names;
    int32_t area_name_count;
    int32_t area_name_capacity;
    NavMeshObstacleTestLayout *obstacles;
    int32_t obstacle_count;
    int32_t obstacle_capacity;
} NavMesh3DTestLayout;

/*==========================================================================
 * NavMesh3D tests
 *=========================================================================*/

static void test_navmesh_build_plane() {
    /* A plane mesh is flat (normal = 0,1,0), so all triangles should be walkable */
    void *plane = rt_mesh3d_new_plane(10.0, 10.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);
    EXPECT_TRUE(nm != nullptr, "NavMesh built from plane");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) > 0, "Plane has walkable triangles");
}

static void test_navmesh_rejects_all_degenerate_geometry() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.5, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    auto *mesh_view = static_cast<rt_mesh3d *>(mesh);
    mesh_view->vertices[2].pos[0] = 2.0f;
    mesh_view->vertices[2].pos[2] = 0.0f;
    EXPECT_TRUE(rt_navmesh3d_build(mesh, 0.4, 1.8) == nullptr,
                "NavMesh rejects a source containing no non-degenerate triangles");
}

static void test_navmesh_is_walkable() {
    void *plane = rt_mesh3d_new_plane(10.0, 10.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);

    /* Center of plane should be walkable */
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, center) != 0, "Center of plane is walkable");
}

static void test_navmesh_not_walkable() {
    void *plane = rt_mesh3d_new_plane(10.0, 10.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);

    /* Far away point should not be walkable */
    void *far_pt = rt_vec3_new(100.0, 0.0, 100.0);
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, far_pt) == 0, "Far point is not walkable");
}

static void test_navmesh_sample_position() {
    void *plane = rt_mesh3d_new_plane(10.0, 10.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);

    void *pt = rt_vec3_new(2.0, 5.0, 2.0); /* above the plane */
    void *snapped = rt_navmesh3d_sample_position(nm, pt);
    /* Y should be snapped to surface (near 0) */
    EXPECT_NEAR(rt_vec3_y(snapped), 0.0, 0.5, "Sampled Y ≈ 0 (plane surface)");
}

/// @brief Verify off-mesh sampling uses an expanding spatial-index neighborhood.
/// @details A query just outside a dense grid has a nearest point in its boundary cell. The exact
///   result must therefore require a small, sublinear fraction of the mesh's triangle references
///   and must not enter the bounded whole-mesh fallback.
static void test_navmesh_sample_position_uses_sublinear_local_probe() {
    constexpr int cells = 96;
    void *nm = make_dense_grid_navmesh(cells);
    void *query = rt_vec3_new((double)cells * 0.5 + 0.25, 3.0, 0.125);
    void *sample = rt_navmesh3d_sample_position(nm, query);
    int64_t triangle_count = rt_navmesh3d_get_triangle_count(nm);
    int64_t probes = rt_navmesh3d_test_get_last_sample_probe_count(nm);

    EXPECT_TRUE(nm != nullptr && sample != nullptr,
                "NavMesh indexed sampling fixture builds successfully");
    EXPECT_NEAR(rt_vec3_x(sample),
                (double)cells * 0.5,
                0.001,
                "NavMesh indexed sampling finds the nearest boundary point");
    EXPECT_NEAR(rt_vec3_y(sample), 0.0, 0.001, "NavMesh indexed sampling preserves surface Y");
    EXPECT_TRUE(rt_navmesh3d_test_get_last_sample_used_fallback(nm) == 0,
                "NavMesh local off-mesh sampling stays on the expanding-grid path");
    EXPECT_TRUE(probes > 0 && triangle_count > 0 && probes < triangle_count / 8,
                "NavMesh local off-mesh sampling probes a sublinear triangle fraction");
    EXPECT_TRUE(probes <= triangle_count,
                "NavMesh indexed sampling tests each replicated triangle at most once");
}

static void test_navmesh_find_path() {
    void *plane = rt_mesh3d_new_plane(10.0, 10.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);

    void *from = rt_vec3_new(-3.0, 0.0, -3.0);
    void *to = rt_vec3_new(3.0, 0.0, 3.0);
    void *path = rt_navmesh3d_find_path(nm, from, to);
    EXPECT_TRUE(path != nullptr, "Path found on plane");
    void *path_option = rt_navmesh3d_find_path_option(nm, from, to);
    EXPECT_TRUE(rt_option_is_some(path_option) == 1, "FindPathOption returns Some on plane");
    EXPECT_TRUE(rt_path3d_get_point_count(rt_option_unwrap(path_option)) >= 2,
                "FindPathOption unwraps a path with at least 2 points");
    if (path) {
        EXPECT_TRUE(rt_path3d_get_point_count(path) >= 2, "Path has at least 2 points");
    }
}

static void test_navmesh_find_path_from_shared_edge() {
    void *plane = rt_mesh3d_new_plane(20.0, 20.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);
    void *from = rt_vec3_new(1.909190416, 0.0, 1.909190416);
    void *to = rt_vec3_new(4.0, 0.0, 4.0);
    void *path = rt_navmesh3d_find_path(nm, from, to);

    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, from) != 0,
                "NavMesh treats points on shared triangle edges as walkable");
    EXPECT_TRUE(path != nullptr, "NavMesh finds a path from a shared triangle edge");
    if (path)
        EXPECT_TRUE(rt_path3d_get_point_count(path) >= 2, "Shared-edge path includes endpoints");

    double *points = nullptr;
    int64_t point_count = rt_navmesh3d_copy_path_points(nm, from, to, &points);
    EXPECT_TRUE(point_count >= 2 && points != nullptr,
                "Shared-edge path exposes its packed waypoint representation");
    if (point_count >= 2 && points) {
        EXPECT_NEAR(points[0],
                    1.909190416,
                    1e-12,
                    "NavMesh preserves the requested double-precision start endpoint");
        EXPECT_NEAR(points[(point_count - 1) * 3],
                    4.0,
                    1e-12,
                    "NavMesh preserves the requested double-precision destination endpoint");
    }
    std::free(points);
}

static void test_navmesh_rejects_out_of_domain_query_instead_of_clamping() {
    void *mesh = rt_mesh3d_new();
    const double x0 = 999999872.0;
    const double x1 = 1000000000.0;
    rt_mesh3d_add_vertex(mesh, x0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, x1, 0.0, -1.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, x1, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0);
    rt_mesh3d_add_vertex(mesh, x0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 2, 1);
    rt_mesh3d_add_triangle(mesh, 0, 3, 2);
    void *nm = rt_navmesh3d_build(mesh, 0.2, 1.8);
    void *outside = rt_vec3_new(2000000000.0, 0.0, 0.0);
    void *inside = rt_vec3_new(x0, 0.0, 0.0);
    double *points = nullptr;
    EXPECT_TRUE(rt_navmesh3d_copy_path_points(nm, outside, inside, &points) == 0 && !points,
                "NavMesh rejects out-of-domain points instead of querying a clamped alias");
}

static void test_navmesh_astar_touches_only_frontier_nodes() {
    void *nm = make_dense_grid_navmesh(64);
    void *from = rt_vec3_new(-31.0, 0.0, -31.0);
    void *to = rt_vec3_new(-20.0, 0.0, -20.0);
    double *points = nullptr;
    int64_t point_count = rt_navmesh3d_copy_path_points(nm, from, to, &points);
    int64_t touched = rt_navmesh3d_test_get_last_path_touched_count(nm);
    int64_t heap_peak = rt_navmesh3d_test_get_last_path_heap_peak(nm);
    EXPECT_TRUE(point_count >= 2 && points != nullptr, "Dense-grid A* path succeeds");
    EXPECT_TRUE(touched > 0 && touched < rt_navmesh3d_get_triangle_count(nm),
                "A* initializes only nodes reached by the search frontier");
    EXPECT_TRUE(heap_peak > 0 && heap_peak <= touched,
                "Indexed A* heap contains at most one entry per touched node");
    std::free(points);
}

static void test_path3d_bulk_append_is_transactional() {
    double points[17 * 3] = {};
    for (int i = 0; i < 17; ++i)
        points[i * 3] = (double)i;
    void *path = rt_path3d_new();
    rt_path3d_test_set_coordinate_alloc_failure(1);
    EXPECT_TRUE(rt_path3d_append_xyz_batch_internal(path, points, 17) == 0,
                "Path3D bulk append reports staged allocation failure");
    EXPECT_TRUE(rt_path3d_get_point_count(path) == 0,
                "Path3D bulk append leaves the destination unchanged on failure");
    rt_path3d_test_set_coordinate_alloc_failure(0);
    EXPECT_TRUE(rt_path3d_append_xyz_batch_internal(path, points, 17) == 1 &&
                    rt_path3d_get_point_count(path) == 17,
                "Path3D bulk append publishes every point after one successful reserve");
}

static void test_navmesh_find_path_adopts_reconstructed_storage() {
    void *nm = make_dense_grid_navmesh(16);
    void *from = rt_vec3_new(-7.0, 0.0, -7.0);
    void *to = rt_vec3_new(7.0, 0.0, 7.0);
    rt_path3d_test_set_coordinate_alloc_failure(1);
    void *path = rt_navmesh3d_find_path(nm, from, to);
    rt_path3d_test_set_coordinate_alloc_failure(0);
    EXPECT_TRUE(
        path != nullptr && rt_path3d_get_point_count(path) >= 2,
        "NavMesh FindPath transfers reconstructed points without a second coordinate allocation");
}

static void test_navmesh_path_workspace_reuse_is_concurrent_safe() {
    /* Keep each search alive across scheduler quanta so the peak-concurrency
     * assertion remains meaningful on oversubscribed CI workers. */
    constexpr int cells = 256;
    void *nm = make_dense_grid_navmesh(cells);
    void *from = rt_vec3_new(-31.0, 0.0, -31.0);
    void *to = rt_vec3_new(31.0, 0.0, 31.0);
    std::atomic<bool> ok{true};
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    constexpr int worker_count = 8;
    std::thread workers[worker_count];
    rt_navmesh3d_test_reset_path_peak_concurrency(nm);
    rt_navmesh3d_test_reset_path_transient_allocation_count(nm);
    for (int worker = 0; worker < worker_count; worker++) {
        workers[worker] = std::thread([&]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int iteration = 0; iteration < 12; iteration++) {
                double *points = nullptr;
                int64_t count = rt_navmesh3d_copy_path_points(nm, from, to, &points);
                if (count < 2 || !points || !std::isfinite(points[0]) ||
                    !std::isfinite(points[(count - 1) * 3 + 2]))
                    ok.store(false, std::memory_order_relaxed);
                std::free(points);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != worker_count)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto &worker : workers)
        worker.join();
    EXPECT_TRUE(ok.load(std::memory_order_relaxed),
                "NavMesh A* workspace pool is reusable across concurrent queries");
    EXPECT_TRUE(rt_navmesh3d_test_get_path_peak_concurrency(nm) > 4,
                "NavMesh retained workspaces let more than four searches overlap in flight");
    EXPECT_TRUE(rt_navmesh3d_test_get_path_transient_allocation_count(nm) == 0,
                "NavMesh eight-way query bursts perform no transient workspace allocations");
    EXPECT_TRUE(rt_navmesh3d_test_get_ready_path_scratch_count(nm) >= 2,
                "NavMesh concurrent searches retain per-workspace corridor and portal scratch");
    EXPECT_TRUE(rt_navmesh3d_test_get_path_workspace_permits(nm) == worker_count,
                "NavMesh returns every blocking workspace permit after concurrent searches");
    EXPECT_TRUE(std::isfinite(rt_navmesh3d_get_last_path_cost(nm)),
                "NavMesh LastPathCost remains atomically readable during workspace reuse");
}

static void test_navmesh_box_slope_filter() {
    /* A box has vertical walls which should be excluded by slope filter */
    void *box = rt_mesh3d_new_box(5.0, 5.0, 5.0);
    void *nm = rt_navmesh3d_build(box, 0.4, 1.8);

    int64_t tri_count = rt_navmesh3d_get_triangle_count(nm);
    /* Box has 12 triangles total (6 faces × 2 tris each).
     * Only top face (2 tris) should be walkable (normal pointing up).
     * Bottom face also has upward normal from the inside, so could be 2-4 walkable. */
    EXPECT_TRUE(tri_count > 0 && tri_count < 12, "Box: not all triangles walkable (slope filter)");
}

/// @brief Verify extreme finite voxel dimensions coarsen safely without integer wraparound.
/// @details The production sizing helper must return an exact bounded product for enormous spans
///          and tiny requested cells, while rejecting non-finite input without mutating outputs.
static void test_navmesh_voxel_grid_extreme_arithmetic_is_bounded() {
    int32_t nx = -1;
    int32_t nz = -1;
    uint64_t cell_count = UINT64_MAX;
    double effective_cell = -1.0;

    EXPECT_TRUE(rt_navmesh3d_test_voxel_grid_dimensions(
                    1.0e300, 5.0e299, 1.0e-300, &nx, &nz, &cell_count, &effective_cell) != 0,
                "NavMesh voxel sizing accepts extreme finite spans without narrowing overflow");
    EXPECT_TRUE(nx > 0 && nx <= 512 && nz > 0 && nz <= 512,
                "NavMesh voxel sizing keeps both dimensions inside the configured bound");
    EXPECT_TRUE(cell_count == static_cast<uint64_t>(nx) * static_cast<uint64_t>(nz) &&
                    cell_count <= 262144u,
                "NavMesh voxel sizing returns the exact checked dimension product");
    EXPECT_TRUE(
        std::isfinite(effective_cell) && effective_cell > 1.0e290,
        "NavMesh voxel sizing coarsens a tiny request instead of allocating undersized storage");

    nx = 17;
    nz = 19;
    cell_count = 23;
    effective_cell = 29.0;
    EXPECT_TRUE(rt_navmesh3d_test_voxel_grid_dimensions(
                    INFINITY, 1.0, 0.1, &nx, &nz, &cell_count, &effective_cell) == 0,
                "NavMesh voxel sizing rejects non-finite extents");
    EXPECT_TRUE(nx == 17 && nz == 19 && cell_count == 23 && effective_cell == 29.0,
                "NavMesh voxel sizing leaves outputs untouched on failure");
}

/// @brief Build a two-island navmesh fixture whose second triangle is initially slope-filtered.
/// @param out_slope_height Optional destination for the authored steep-island height.
/// @return New NavMesh3D containing only the flat island at the default slope limit.
static void *make_navmesh_transaction_fixture(double *out_slope_height) {
    const double slope_height = std::sqrt(12.0);
    void *mesh = rt_mesh3d_new();

    /* Flat island: counter-clockwise in XZ so the computed face normal points upward. */
    rt_mesh3d_add_vertex(mesh, -3.0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, -1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0);
    rt_mesh3d_add_vertex(mesh, -1.0, 0.0, -1.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);

    /* A disconnected 60-degree island is excluded at the default 45 degrees and admitted at 80. */
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 3.0, slope_height, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0);
    rt_mesh3d_add_vertex(mesh, 3.0, slope_height, -1.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_triangle(mesh, 3, 4, 5);
    if (out_slope_height)
        *out_slope_height = slope_height;
    return rt_navmesh3d_build(mesh, 0.4, 1.8);
}

/// @brief Verify adjacency/grid rebuild failures preserve the last complete navmesh state.
/// @details Fault injection admits a formerly filtered island and then fails each staged derived
///          allocation. Baseline path bytes, walkability, and triangle counts must remain exact
///          until a later allocation-successful rebuild commits all state together.
static void test_navmesh_rebuild_allocation_failure_is_atomic() {
    double slope_height = 0.0;
    void *nm = make_navmesh_transaction_fixture(&slope_height);
    void *flat_from = rt_vec3_new(-2.5, 0.0, -0.75);
    void *flat_to = rt_vec3_new(-1.25, 0.0, -0.75);
    void *steep_center = rt_vec3_new(7.0 / 3.0, slope_height * 2.0 / 3.0, -1.0 / 3.0);
    double *baseline_path = nullptr;
    int64_t baseline_count = rt_navmesh3d_copy_path_points(nm, flat_from, flat_to, &baseline_path);

    EXPECT_TRUE(nm != nullptr && rt_navmesh3d_get_triangle_count(nm) == 1,
                "NavMesh transaction fixture initially publishes only the flat triangle");
    EXPECT_TRUE(baseline_count >= 2 && baseline_path != nullptr,
                "NavMesh transaction fixture has a stable baseline query");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, steep_center) == 0,
                "NavMesh transaction fixture initially filters the steep island");

    rt_navmesh3d_test_set_adjacency_alloc_failure(1);
    rt_navmesh3d_set_max_slope(nm, 80.0);
    rt_navmesh3d_test_set_adjacency_alloc_failure(0);
    double *after_adjacency_failure = nullptr;
    int64_t adjacency_count =
        rt_navmesh3d_copy_path_points(nm, flat_from, flat_to, &after_adjacency_failure);
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == 1 &&
                    rt_navmesh3d_is_walkable(nm, steep_center) == 0,
                "NavMesh adjacency allocation failure retains the prior walkable topology");
    EXPECT_TRUE(adjacency_count == baseline_count && after_adjacency_failure && baseline_path &&
                    std::memcmp(after_adjacency_failure,
                                baseline_path,
                                static_cast<size_t>(baseline_count) * 3u * sizeof(double)) == 0,
                "NavMesh adjacency allocation failure keeps path queries bit-identical");
    EXPECT_TRUE(rt_navmesh3d_check_query_grid_parity(nm) != 0,
                "NavMesh adjacency allocation failure leaves the prior query grid usable");

    rt_navmesh3d_test_set_query_grid_alloc_failure(1);
    rt_navmesh3d_set_max_slope(nm, 80.0);
    rt_navmesh3d_test_set_query_grid_alloc_failure(0);
    double *after_grid_failure = nullptr;
    int64_t grid_count = rt_navmesh3d_copy_path_points(nm, flat_from, flat_to, &after_grid_failure);
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == 1 &&
                    rt_navmesh3d_is_walkable(nm, steep_center) == 0,
                "NavMesh query-grid allocation failure retains the prior walkable topology");
    EXPECT_TRUE(grid_count == baseline_count && after_grid_failure && baseline_path &&
                    std::memcmp(after_grid_failure,
                                baseline_path,
                                static_cast<size_t>(baseline_count) * 3u * sizeof(double)) == 0,
                "NavMesh query-grid allocation failure keeps path queries bit-identical");
    EXPECT_TRUE(rt_navmesh3d_check_query_grid_parity(nm) != 0,
                "NavMesh query-grid allocation failure keeps the previous index usable");

    rt_navmesh3d_set_max_slope(nm, 80.0);
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == 2 &&
                    rt_navmesh3d_is_walkable(nm, steep_center) != 0,
                "NavMesh refilter publishes the requested topology after allocation recovers");

    std::free(baseline_path);
    std::free(after_adjacency_failure);
    std::free(after_grid_failure);
}

static void test_navmesh_adjacency_edge_hash() {
    /* Build from plane — should produce 2 triangles that are adjacent (share an edge) */
    void *plane = rt_mesh3d_new_plane(10.0, 10.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);
    EXPECT_TRUE(nm != nullptr, "NavMesh adjacency: builds from plane");
    int64_t tc = rt_navmesh3d_get_triangle_count(nm);
    EXPECT_TRUE(tc == 2, "NavMesh adjacency: plane has 2 walkable triangles");
    /* If adjacency works correctly, pathfinding between opposite corners should succeed
     * (requires traversing from triangle 0 to triangle 1 via shared edge) */
    void *from = rt_vec3_new(-4.0, 0.0, -4.0);
    void *to = rt_vec3_new(4.0, 0.0, 4.0);
    void *path = rt_navmesh3d_find_path(nm, from, to);
    EXPECT_TRUE(path != nullptr, "NavMesh adjacency: path found across edge-hash adjacency");
}

static void test_navmesh_rejects_non_manifold_edges() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, -1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.5, 1.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.5, 1.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, -2.0, 0.0, 1.0, 0.0, 0.5, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    rt_mesh3d_add_triangle(mesh, 1, 0, 3);
    rt_mesh3d_add_triangle(mesh, 0, 1, 4);

    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_navmesh3d_build(mesh, 0.4, 1.8); }, "non-manifold"),
        "NavMesh rejects non-manifold edge ownership");
}

static void test_navmesh_large_mesh() {
    /* Build from a box (12 triangles) — verifies edge hash handles > 2 triangles */
    void *box = rt_mesh3d_new_box(10.0, 0.5, 10.0); /* flat-ish box */
    void *nm = rt_navmesh3d_build(box, 0.4, 1.8);
    EXPECT_TRUE(nm != nullptr, "NavMesh large: builds from box");
    int64_t tc = rt_navmesh3d_get_triangle_count(nm);
    EXPECT_TRUE(tc > 0, "NavMesh large: has walkable triangles");
}

static void *make_narrow_corridor_navmesh(double agent_radius) {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, -3.0, 0.0, -0.3, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, -0.3, 0.0, 1.0, 0.0, 0.5, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.3, 0.0, 1.0, 0.0, 0.5, 1.0);
    rt_mesh3d_add_vertex(mesh, -3.0, 0.0, 0.3, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_vertex(mesh, 3.0, 0.0, -0.3, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 3.0, 0.0, 0.3, 0.0, 1.0, 0.0, 1.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 2, 1);
    rt_mesh3d_add_triangle(mesh, 0, 3, 2);
    rt_mesh3d_add_triangle(mesh, 1, 5, 4);
    rt_mesh3d_add_triangle(mesh, 1, 2, 5);
    return rt_navmesh3d_build(mesh, agent_radius, 1.8);
}

static void test_navmesh_agent_radius_blocks_narrow_portals() {
    void *small_agent = make_narrow_corridor_navmesh(0.2);
    void *wide_agent = make_narrow_corridor_navmesh(0.4);
    void *from = rt_vec3_new(-2.0, 0.0, 0.0);
    void *to = rt_vec3_new(2.0, 0.0, 0.0);

    EXPECT_TRUE(small_agent != nullptr && wide_agent != nullptr,
                "NavMesh radius corridors: meshes build");
    EXPECT_TRUE(rt_navmesh3d_find_path(small_agent, from, to) != nullptr,
                "NavMesh radius corridors: small agent traverses a wide-enough portal");
    EXPECT_TRUE(rt_navmesh3d_find_path(wide_agent, from, to) == nullptr,
                "NavMesh radius corridors: wide agent cannot traverse a narrow portal");
}

static void *make_two_island_navmesh() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, -4.0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, -2.0, 0.0, -1.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, -2.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0);
    rt_mesh3d_add_vertex(mesh, -4.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_vertex(mesh, 2.0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 4.0, 0.0, -1.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 4.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0);
    rt_mesh3d_add_vertex(mesh, 2.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 2, 1);
    rt_mesh3d_add_triangle(mesh, 0, 3, 2);
    rt_mesh3d_add_triangle(mesh, 4, 6, 5);
    rt_mesh3d_add_triangle(mesh, 4, 7, 6);
    return rt_navmesh3d_build(mesh, 0.4, 1.8);
}

static void test_navmesh_offmesh_links_bridge_islands() {
    void *nm = make_two_island_navmesh();
    void *from = rt_vec3_new(-3.0, 0.0, 0.0);
    void *to = rt_vec3_new(3.0, 0.0, 0.0);
    void *link_from = rt_vec3_new(-2.5, 0.0, 0.0);
    void *link_to = rt_vec3_new(2.5, 0.0, 0.0);

    EXPECT_TRUE(nm != nullptr, "NavMesh off-mesh: island mesh builds");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == 4, "NavMesh off-mesh: islands are walkable");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) == nullptr,
                "NavMesh off-mesh: disconnected islands have no path before link");
    EXPECT_TRUE(rt_navmesh3d_get_offmesh_link_count(nm) == 0,
                "NavMesh off-mesh: new mesh starts with no links");
    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(nm, link_from, link_to, 1) != 0,
                "NavMesh off-mesh: AddOffMeshLink accepts endpoints on walkable islands");
    EXPECT_TRUE(rt_navmesh3d_get_offmesh_link_count(nm) == 1,
                "NavMesh off-mesh: OffMeshLinkCount tracks authored links");
    auto *view = static_cast<NavMesh3DTestLayout *>(nm);
    EXPECT_TRUE(view->offmesh_adjacency_state == 1 && view->offmesh_adjacency_starts == nullptr,
                "NavMesh off-mesh: link authoring invalidates adjacency without rebuilding it");

    void *path = rt_navmesh3d_find_path(nm, from, to);
    EXPECT_TRUE(path != nullptr, "NavMesh off-mesh: bidirectional link bridges islands");
    EXPECT_TRUE(view->offmesh_adjacency_state == 0 && view->offmesh_adjacency_starts != nullptr,
                "NavMesh off-mesh: first path query lazily publishes adjacency once");
    if (path)
        EXPECT_TRUE(rt_path3d_get_point_count(path) >= 4,
                    "NavMesh off-mesh: path includes endpoint and link waypoints");

    int32_t from_tri = view->offmesh_links[0].from_tri;
    int32_t saved_begin = view->offmesh_adjacency_starts[from_tri];
    view->offmesh_adjacency_starts[from_tri] = -1;
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) != nullptr,
                "NavMesh off-mesh: invalid cached adjacency range falls back to link scan");
    view->offmesh_adjacency_starts[from_tri] = saved_begin;
}

static void test_navmesh_offmesh_links_validate_and_direct() {
    void *nm = make_two_island_navmesh();
    void *left = rt_vec3_new(-3.0, 0.0, 0.0);
    void *right = rt_vec3_new(3.0, 0.0, 0.0);
    void *bad = rt_vec3_new(100.0, 0.0, 0.0);

    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(nm, left, bad, 1) == 0,
                "NavMesh off-mesh: rejects endpoints outside walkable polygons");
    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(nullptr, left, right, 1) == 0,
                "NavMesh off-mesh: rejects NULL navmesh");
    EXPECT_TRUE(rt_navmesh3d_get_offmesh_link_count(left) == 0,
                "NavMesh off-mesh: accessors reject non-navmesh handles");
    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(nm, left, right, 0) != 0,
                "NavMesh off-mesh: one-way link can be authored");

    void *forward = rt_navmesh3d_find_path(nm, left, right);
    void *reverse = rt_navmesh3d_find_path(nm, right, left);
    EXPECT_TRUE(forward != nullptr, "NavMesh off-mesh: one-way link traverses forward");
    EXPECT_TRUE(reverse == nullptr, "NavMesh off-mesh: one-way link blocks reverse traversal");
}

static void test_navmesh_offmesh_link_metadata_affects_cost() {
    void *nm = make_two_island_navmesh();
    void *left = rt_vec3_new(-3.0, 0.0, 0.0);
    void *right = rt_vec3_new(3.0, 0.0, 0.0);
    rt_string jump = rt_const_cstr("jump");

    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(nm, left, right, 1) != 0,
                "NavMesh off-mesh metadata: link can be authored");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, left, right) != nullptr,
                "NavMesh off-mesh metadata: baseline linked path exists");
    double base_cost = rt_navmesh3d_get_last_path_cost(nm);
    EXPECT_TRUE(rt_navmesh3d_set_offmesh_link_metadata(nm, 0, jump, 4.0, 7) != 0,
                "NavMesh off-mesh metadata: metadata setter accepts valid link");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_navmesh3d_get_offmesh_link_kind(nm, 0)), "jump") == 0,
                "NavMesh off-mesh metadata: kind is stored");
    EXPECT_NEAR(rt_navmesh3d_get_offmesh_link_traversal_cost(nm, 0),
                4.0,
                0.001,
                "NavMesh off-mesh metadata: traversal cost is stored");
    EXPECT_TRUE(rt_navmesh3d_get_offmesh_link_state(nm, 0) == 7,
                "NavMesh off-mesh metadata: state flags are stored");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, left, right) != nullptr,
                "NavMesh off-mesh metadata: high-cost linked path still exists");
    EXPECT_TRUE(rt_navmesh3d_get_last_path_cost(nm) > base_cost * 3.0,
                "NavMesh off-mesh metadata: link traversal cost contributes to A* cost");
    EXPECT_TRUE(rt_navmesh3d_set_offmesh_link_metadata(nm, 3, jump, 2.0, 0) == 0,
                "NavMesh off-mesh metadata: invalid link index is rejected");
    EXPECT_TRUE(rt_navmesh3d_get_offmesh_link_state(nm, 3) == 0,
                "NavMesh off-mesh metadata: invalid getter returns neutral state");
}

static void test_navmesh_offmesh_links_keep_astar_heuristic_admissible() {
    void *mesh = rt_mesh3d_new();
    for (int tri = 0; tri < 4; tri++) {
        double x = (double)tri * 10.0;
        int64_t base = tri * 3;
        rt_mesh3d_add_vertex(mesh, x - 0.4, 0.0, -0.4, 0.0, 1.0, 0.0, 0.0, 0.0);
        rt_mesh3d_add_vertex(mesh, x, 0.0, 0.4, 0.0, 1.0, 0.0, 0.5, 1.0);
        rt_mesh3d_add_vertex(mesh, x + 0.4, 0.0, -0.4, 0.0, 1.0, 0.0, 1.0, 0.0);
        rt_mesh3d_add_triangle(mesh, base, base + 1, base + 2);
    }
    void *nm = rt_navmesh3d_build(mesh, 0.1, 1.8);
    void *link_from = rt_vec3_new(20.0, 0.0, 0.0);
    void *link_to = rt_vec3_new(30.0, 0.0, 0.0);
    EXPECT_TRUE(nm != nullptr && rt_navmesh3d_get_triangle_count(nm) == 4,
                "NavMesh admissibility fixture builds four isolated polygons");
    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(nm, link_from, link_to, 0) != 0,
                "NavMesh admissibility fixture adds its directed jump");

    auto *view = static_cast<NavMesh3DTestLayout *>(nm);
    auto *triangles = static_cast<NavMeshTriangleTestLayout *>(view->triangles);
    for (int i = 0; i < 4; i++)
        for (int edge = 0; edge < 3; edge++)
            triangles[i].neighbors[edge] = -1;
    triangles[0].neighbors[0] = 1; /* expensive direct route */
    triangles[0].neighbors[1] = 2; /* cheap route to the jump */
    triangles[3].neighbors[0] = 1; /* cheap route from jump landing to goal */
    triangles[0].centroid[0] = 0.0f;
    triangles[1].centroid[0] = 100.0f;
    triangles[2].centroid[0] = -1.0f;
    triangles[3].centroid[0] = 99.0f;
    for (int i = 0; i < 4; i++) {
        triangles[i].centroid[1] = 0.0f;
        triangles[i].centroid[2] = 0.0f;
    }
    /* Keep resolved polygon ids but model a one-unit authored jump. The optimal cost is
     * 1 (start->jump) + 1 (jump) + 1 (landing->goal), versus 100 directly. */
    view->offmesh_links[0].from[0] = 0.0f;
    view->offmesh_links[0].from[1] = 0.0f;
    view->offmesh_links[0].from[2] = 0.0f;
    view->offmesh_links[0].to[0] = 1.0f;
    view->offmesh_links[0].to[1] = 0.0f;
    view->offmesh_links[0].to[2] = 0.0f;

    void *from = rt_vec3_new(0.0, 0.0, 0.0);
    void *to = rt_vec3_new(10.0, 0.0, 0.0);
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) != nullptr,
                "NavMesh finds a path in the off-mesh admissibility fixture");
    EXPECT_NEAR(rt_navmesh3d_get_last_path_cost(nm),
                3.0,
                0.001,
                "A* does not terminate on a suboptimal goal when off-mesh links exist");
}

static void *make_navmesh_obstacle_strip() {
    void *mesh = rt_mesh3d_new();
    for (int i = 0; i <= 4; i++) {
        double x = -4.0 + (double)i * 2.0;
        rt_mesh3d_add_vertex(mesh, x, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0);
        rt_mesh3d_add_vertex(mesh, x, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    }
    for (int i = 0; i < 4; i++) {
        int64_t bl = i * 2;
        int64_t tl = i * 2 + 1;
        int64_t br = (i + 1) * 2;
        int64_t tr = (i + 1) * 2 + 1;
        rt_mesh3d_add_triangle(mesh, bl, tr, br);
        rt_mesh3d_add_triangle(mesh, bl, tl, tr);
    }
    return rt_navmesh3d_build(mesh, 0.25, 1.8);
}

static void test_navmesh_obstacle_carving_uses_triangle_footprint() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 10.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 10.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 2, 1);
    void *nm = rt_navmesh3d_build(mesh, 0.25, 1.8);
    void *outside_min = rt_vec3_new(9.0, -0.2, 9.0);
    void *outside_max = rt_vec3_new(9.5, 1.0, 9.5);
    void *inside_min = rt_vec3_new(1.0, -0.2, 1.0);
    void *inside_max = rt_vec3_new(1.5, 1.0, 1.5);

    EXPECT_TRUE(nm != nullptr, "NavMesh obstacle exact: single triangle builds");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == 1,
                "NavMesh obstacle exact: triangle starts walkable");
    EXPECT_TRUE(rt_navmesh3d_add_obstacle(nm, outside_min, outside_max) != 0,
                "NavMesh obstacle exact: AABB-overlap-only obstacle is accepted");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == 1,
                "NavMesh obstacle exact: obstacle outside triangle footprint does not carve");
    EXPECT_TRUE(rt_navmesh3d_update_obstacle(nm, 0, inside_min, inside_max) != 0,
                "NavMesh obstacle exact: obstacle can move into triangle footprint");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == 0,
                "NavMesh obstacle exact: obstacle inside triangle footprint carves polygon");
}

static void test_navmesh_add_obstacle_carves_walkable_triangles() {
    void *nm = make_navmesh_obstacle_strip();
    void *from = rt_vec3_new(-3.0, 0.0, 0.0);
    void *to = rt_vec3_new(3.0, 0.0, 0.0);
    void *blocked = rt_vec3_new(0.0, 0.0, 0.0);
    void *min_v = rt_vec3_new(-0.8, -0.2, -1.2);
    void *max_v = rt_vec3_new(0.8, 1.0, 1.2);
    void *far_min = rt_vec3_new(10.0, -0.2, 10.0);
    void *far_max = rt_vec3_new(12.0, 1.0, 12.0);
    void *bad_min = rt_vec3_new(NAN, 0.0, 0.0);
    int64_t before_count = rt_navmesh3d_get_triangle_count(nm);

    EXPECT_TRUE(nm != nullptr, "NavMesh obstacle: strip mesh builds");
    EXPECT_TRUE(before_count == 8, "NavMesh obstacle: strip starts with eight walkable triangles");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) != nullptr,
                "NavMesh obstacle: strip paths before carving");
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(nm) == 0,
                "NavMesh obstacle: new mesh starts with no obstacles");
    EXPECT_TRUE(rt_navmesh3d_add_obstacle(nm, bad_min, max_v) == 0,
                "NavMesh obstacle: rejects non-finite obstacle bounds");
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(nm) == 0,
                "NavMesh obstacle: rejected obstacle does not increment telemetry");
    EXPECT_TRUE(rt_navmesh3d_add_obstacle(nm, min_v, max_v) != 0,
                "NavMesh obstacle: AddObstacle accepts finite AABB bounds");
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(nm) == 1,
                "NavMesh obstacle: ObstacleCount tracks authored obstacles");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) < before_count,
                "NavMesh obstacle: obstacle removes overlapping walkable triangles");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, blocked) == 0,
                "NavMesh obstacle: carved obstacle center is not walkable");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) == nullptr,
                "NavMesh obstacle: carved strip no longer has a corridor path");
    EXPECT_TRUE(rt_navmesh3d_update_obstacle(nm, 0, bad_min, far_max) == 0,
                "NavMesh obstacle: UpdateObstacle rejects non-finite bounds");
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(nm) == 1,
                "NavMesh obstacle: rejected update keeps authored obstacle");
    EXPECT_TRUE(rt_navmesh3d_update_obstacle(nm, 0, far_min, far_max) != 0,
                "NavMesh obstacle: UpdateObstacle moves a carved obstacle");
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(nm) == 1,
                "NavMesh obstacle: UpdateObstacle preserves obstacle count");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == before_count,
                "NavMesh obstacle: moving obstacle out of bounds restores triangles");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, blocked) != 0,
                "NavMesh obstacle: moved obstacle restores walkability");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) != nullptr,
                "NavMesh obstacle: moved obstacle restores path corridor");
    EXPECT_TRUE(rt_navmesh3d_update_obstacle(nm, 0, min_v, max_v) != 0,
                "NavMesh obstacle: UpdateObstacle can carve again");
    EXPECT_TRUE(rt_navmesh3d_remove_obstacle(nm, -1) == 0,
                "NavMesh obstacle: RemoveObstacle rejects negative indices");
    EXPECT_TRUE(rt_navmesh3d_remove_obstacle(nm, 1) == 0,
                "NavMesh obstacle: RemoveObstacle rejects out-of-range indices");
    EXPECT_TRUE(rt_navmesh3d_remove_obstacle(nm, 0) != 0,
                "NavMesh obstacle: RemoveObstacle removes authored obstacles");
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(nm) == 0,
                "NavMesh obstacle: RemoveObstacle updates obstacle count");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == before_count,
                "NavMesh obstacle: removing obstacle restores walkable triangles");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) != nullptr,
                "NavMesh obstacle: removing obstacle restores path corridor");
    EXPECT_TRUE(rt_navmesh3d_add_obstacle(from, min_v, max_v) == 0,
                "NavMesh obstacle: rejects non-navmesh handles");
}

static void test_navmesh_area_metadata_and_traversal_costs() {
    void *plane = rt_mesh3d_new_plane(10.0, 10.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);
    void *from = rt_vec3_new(-4.0, 0.0, 0.0);
    void *to = rt_vec3_new(4.0, 0.0, 0.0);
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    void *min_v = rt_vec3_new(-5.0, -0.2, -5.0);
    void *max_v = rt_vec3_new(5.0, 1.0, 5.0);
    rt_string mud = rt_const_cstr("mud");

    EXPECT_TRUE(nm != nullptr, "NavMesh area: plane builds");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) != nullptr,
                "NavMesh area: baseline path exists");
    double base_cost = rt_navmesh3d_get_last_path_cost(nm);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_navmesh3d_get_area(nm, center)), "default") == 0,
                "NavMesh area: default area name is reported");
    EXPECT_NEAR(rt_navmesh3d_get_traversal_cost(nm, center),
                1.0,
                0.001,
                "NavMesh area: default traversal cost is one");
    EXPECT_TRUE(rt_navmesh3d_set_area(nm, min_v, max_v, mud, 3.0) != 0,
                "NavMesh area: SetArea accepts an authored area and cost");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_navmesh3d_get_area(nm, center)), "mud") == 0,
                "NavMesh area: authored area name is reported");
    EXPECT_NEAR(rt_navmesh3d_get_traversal_cost(nm, center),
                3.0,
                0.001,
                "NavMesh area: authored traversal cost is reported");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) != nullptr,
                "NavMesh area: path remains available after metadata assignment");
    EXPECT_TRUE(rt_navmesh3d_get_last_path_cost(nm) > base_cost * 2.5,
                "NavMesh area: polygon traversal cost contributes to A* cost");
    char area_name[32];
    for (int i = 0; i < 12; ++i) {
        std::snprintf(area_name, sizeof(area_name), "growth-area-%d", i);
        EXPECT_TRUE(
            rt_navmesh3d_set_area(nm, min_v, max_v, rt_const_cstr(area_name), 1.0 + (double)i) != 0,
            "NavMesh area: intern table grows without losing the assigned area");
    }
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_navmesh3d_get_area(nm, center)), "growth-area-11") ==
                    0,
                "NavMesh area: grown intern table resolves the latest area");
    EXPECT_TRUE(rt_navmesh3d_set_area(nm, from, max_v, nullptr, 2.0) == 0,
                "NavMesh area: invalid area string is rejected");
}

static void test_navmesh_metadata_updates_are_transactional_and_length_exact() {
    void *nm = rt_navmesh3d_build(rt_mesh3d_new_plane(10.0, 10.0), 0.4, 1.8);
    auto *view = static_cast<NavMesh3DTestLayout *>(nm);
    int32_t area_count = view->area_name_count;
    EXPECT_TRUE(rt_navmesh3d_set_area(nm,
                                      rt_vec3_new(100.0, 0.0, 100.0),
                                      rt_vec3_new(101.0, 1.0, 101.0),
                                      rt_const_cstr("unused"),
                                      2.0) == 0 &&
                    view->area_name_count == area_count,
                "SetArea does not intern a name when no source triangle is touched");

    const char area_bytes[] = {'m', 'u', 'd', '\0', 'x'};
    rt_string area = rt_string_from_bytes(area_bytes, sizeof(area_bytes));
    EXPECT_TRUE(rt_navmesh3d_set_area(
                    nm, rt_vec3_new(-5.0, -1.0, -5.0), rt_vec3_new(5.0, 1.0, 5.0), area, 2.0) == 1,
                "NavMesh accepts a length-carrying area name with an embedded NUL");
    rt_string_unref(area);
    rt_string path = rt_const_cstr("/tmp/zanna_navmesh_embedded_nul.vnav");
    EXPECT_TRUE(rt_navmesh3d_export(nm, path) == 1, "NavMesh exports exact-length area names");
    void *imported = rt_navmesh3d_import(path);
    rt_string loaded_area = rt_navmesh3d_get_area(imported, rt_vec3_new(0.0, 0.0, 0.0));
    EXPECT_TRUE(imported && rt_str_len(loaded_area) == (int64_t)sizeof(area_bytes) &&
                    std::memcmp(rt_string_cstr(loaded_area), area_bytes, sizeof(area_bytes)) == 0,
                "NavMesh import/export preserves every area-name byte");

    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(
                    nm, rt_vec3_new(-4.0, 0.0, -4.0), rt_vec3_new(4.0, 0.0, 4.0), 1) == 1,
                "NavMesh metadata fixture adds an off-mesh link");
    rt_string kind = rt_string_from_bytes("jump", 4);
    EXPECT_TRUE(rt_navmesh3d_set_offmesh_link_metadata(nm, 0, kind, 2.0, 7) == 1,
                "NavMesh metadata fixture assigns a link kind");
    rt_string_unref(kind);
    rt_string sole_owned_kind = view->offmesh_links[0].kind;
    EXPECT_TRUE(rt_navmesh3d_set_offmesh_link_metadata(nm, 0, sole_owned_kind, 3.0, 9) == 1 &&
                    rt_str_len(view->offmesh_links[0].kind) == 4 &&
                    std::memcmp(rt_string_cstr(view->offmesh_links[0].kind), "jump", 4) == 0,
                "Link metadata retains a same-handle replacement before releasing the old slot");
}

static void *make_scene_bake_fixture() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *floor = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_plane(6.0, 6.0);

    rt_scene_node3d_set_position(parent, 2.0, 0.0, -3.0);
    rt_scene_node3d_set_position(floor, 1.0, 0.0, 2.0);
    rt_scene_node3d_set_mesh(floor, mesh);
    rt_scene_node3d_add_child(parent, floor);
    rt_scene3d_add(scene, parent);
    return scene;
}

static void test_navmesh_bake_scene_flattens_transformed_nodes() {
    void *scene = make_scene_bake_fixture();
    void *nm = rt_navmesh3d_bake(scene, 0.4, 1.8, 45.0, 0.3);
    void *center = rt_vec3_new(3.0, 0.0, -1.0);
    void *outside = rt_vec3_new(-1.0, 0.0, 3.0);
    void *from = rt_vec3_new(1.0, 0.0, -3.0);
    void *to = rt_vec3_new(5.0, 0.0, 1.0);

    EXPECT_TRUE(nm != nullptr, "NavMesh Bake: scene with transformed mesh bakes");
    /* The voxel baker emits a shared-corner grid mesh (a quad per walkable cell), so the triangle
     * count reflects the cell_size grid resolution, not the 2 input plane triangles. A 6x6 plane at
     * cell_size 0.3, eroded by the 0.4 agent radius, yields a ~17x17 walkable-cell grid. */
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) > 100,
                "NavMesh Bake: voxelizes into a grid decoupled from input triangle density");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, center) != 0,
                "NavMesh Bake: world-space transformed center is walkable");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, outside) == 0,
                "NavMesh Bake: points outside transformed world bounds are not walkable");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) != nullptr,
                "NavMesh Bake: pathfinding works across baked scene geometry");
    EXPECT_TRUE(rt_navmesh3d_bake(rt_mesh3d_new_plane(2.0, 2.0), 0.4, 1.8, 45.0, 0.3) == nullptr,
                "NavMesh Bake: rejects non-SceneGraph handles");
}

static void test_navmesh_bake_tiled_and_rebuild_tile_baseline() {
    void *scene = make_scene_bake_fixture();
    void *nm = rt_navmesh3d_bake_tiled(scene, 8.0, 0.4, 1.8, 45.0, 0.3);
    void *center = rt_vec3_new(3.0, 0.0, -1.0);

    EXPECT_TRUE(nm != nullptr, "NavMesh BakeTiled: scene bakes through tiled API");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) > 100,
                "NavMesh BakeTiled: voxelizes the full scene into a navmesh grid");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, center) != 0,
                "NavMesh BakeTiled: transformed geometry is walkable");
    EXPECT_TRUE(rt_navmesh3d_rebuild_tile(nm, 0, 0) != 0,
                "NavMesh RebuildTile: tiled mesh re-carves a tile in place");
    EXPECT_TRUE(rt_navmesh3d_rebuild_tile(scene, 0, 0) == 0,
                "NavMesh RebuildTile: rejects non-navmesh handles");
}

/* R-NAV-002: RebuildTile re-carves only its own tile, not the whole mesh. We inject an obstacle
 * WITHOUT re-flagging (a deliberately stale tiled mesh), then show that rebuilding a far-away tile
 * leaves the obstacle's cell walkable, while rebuilding the obstacle's own tile carves it. The two
 * outcomes are indistinguishable for a whole-mesh refilter — only a genuinely tile-local rebuild
 * can leave one tile stale while updating another. */
static void test_navmesh_rebuild_tile_is_tile_local() {
    void *scene = make_scene_bake_fixture();
    void *nm = rt_navmesh3d_bake_tiled(scene, 2.0, 0.4, 1.8, 45.0, 0.3);
    EXPECT_TRUE(nm != nullptr, "RebuildTile local: tiled bake succeeds");
    int64_t base = rt_navmesh3d_get_triangle_count(nm);
    EXPECT_TRUE(base > 100, "RebuildTile local: baked grid has many walkable triangles");

    /* The transformed plane center is walkable (same point the bake-fixture test checks). */
    void *p = rt_vec3_new(3.0, 0.0, -1.0);
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, p) != 0,
                "RebuildTile local: target cell starts walkable");

    int64_t ptx = 0, ptz = 0;
    EXPECT_TRUE(rt_navmesh3d_test_tile_of_point(nm, 3.0, -1.0, &ptx, &ptz) != 0,
                "RebuildTile local: tiled mesh maps a point to a tile");

    /* Inject an obstacle over the target cell with NO re-flag — the mesh is now stale. */
    void *omin = rt_vec3_new(2.6, -0.5, -1.4);
    void *omax = rt_vec3_new(3.4, 0.5, -0.6);
    EXPECT_TRUE(rt_navmesh3d_test_inject_obstacle(nm, omin, omax) != 0,
                "RebuildTile local: obstacle injected");
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(nm) == 1,
                "RebuildTile local: injected obstacle is tracked");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, p) != 0,
                "RebuildTile local: cell still walkable before any tile is rebuilt (stale)");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == base,
                "RebuildTile local: injecting without rebuild leaves the walkable count unchanged");

    /* Rebuild a far-away tile: must NOT carve the obstacle's cell. */
    EXPECT_TRUE(rt_navmesh3d_rebuild_tile(nm, ptx + 5, ptz + 5) != 0,
                "RebuildTile local: rebuilding a far tile succeeds");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, p) != 0,
                "RebuildTile local: far-tile rebuild leaves the obstacle's tile untouched");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) == base,
                "RebuildTile local: far-tile rebuild does not carve the obstacle");

    /* Rebuild the obstacle's own tile: now the cell is carved and the walkable count drops. */
    EXPECT_TRUE(rt_navmesh3d_rebuild_tile(nm, ptx, ptz) != 0,
                "RebuildTile local: rebuilding the obstacle's tile succeeds");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, p) == 0,
                "RebuildTile local: rebuilding the obstacle's own tile carves the cell");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) < base,
                "RebuildTile local: tile rebuild reduces the walkable triangle count");
}

static void test_navmesh_rebuild_tile_refreshes_retained_geometry_source() {
    void *scene = make_scene_bake_fixture();
    void *nm = rt_navmesh3d_bake_tiled(scene, 2.0, 0.4, 1.8, 45.0, 0.3);
    void *p = rt_vec3_new(3.0, 0.0, -1.0);
    void *p_raised = rt_vec3_new(3.0, 2.0, -1.0);
    int64_t tx = 0, tz = 0;
    EXPECT_TRUE(nm != nullptr, "RebuildTile source: tiled bake succeeds");
    EXPECT_TRUE(rt_navmesh3d_test_tile_of_point(nm, 3.0, -1.0, &tx, &tz) != 0,
                "RebuildTile source: target point maps to a tile");
    void *sample0 = rt_navmesh3d_sample_position(nm, p);
    EXPECT_NEAR(rt_vec3_y(sample0), 0.0, 0.05, "RebuildTile source: tile starts at base height");

    EXPECT_TRUE(rt_navmesh3d_test_set_tile_source(nm, tx, tz, 2.0, 1) != 0,
                "RebuildTile source: retained tile geometry source can be edited");
    void *stale = rt_navmesh3d_sample_position(nm, p);
    EXPECT_NEAR(
        rt_vec3_y(stale), 0.0, 0.05, "RebuildTile source: source edit stays stale before rebuild");
    EXPECT_TRUE(rt_navmesh3d_rebuild_tile(nm, tx + 5, tz + 5) != 0,
                "RebuildTile source: far tile rebuild succeeds");
    void *far = rt_navmesh3d_sample_position(nm, p);
    EXPECT_NEAR(
        rt_vec3_y(far), 0.0, 0.05, "RebuildTile source: far tile rebuild leaves edited tile stale");

    EXPECT_TRUE(rt_navmesh3d_rebuild_tile(nm, tx, tz) != 0,
                "RebuildTile source: own tile rebuild succeeds");
    void *raised = rt_navmesh3d_sample_position(nm, p);
    EXPECT_TRUE(rt_vec3_y(raised) > 1.5,
                "RebuildTile source: own tile rebuild refreshes retained geometry height");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, p_raised) != 0,
                "RebuildTile source: raised tile remains walkable when source says walkable");

    int64_t before_block = rt_navmesh3d_get_triangle_count(nm);
    EXPECT_TRUE(rt_navmesh3d_test_set_tile_source(nm, tx, tz, 2.0, 0) != 0,
                "RebuildTile source: retained tile geometry can become unwalkable");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, p_raised) != 0,
                "RebuildTile source: unwalkable source edit is stale before own tile rebuild");
    EXPECT_TRUE(rt_navmesh3d_rebuild_tile(nm, tx, tz) != 0,
                "RebuildTile source: own tile rebuild applies unwalkable source edit");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm, p_raised) == 0,
                "RebuildTile source: own tile rebuild removes edited source geometry");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm) < before_block,
                "RebuildTile source: source geometry removal drops walkable triangle count");
}

/* A narrow plank: 1.0 wide (x) x 10.0 long (z), centered at origin. */
static void *make_narrow_plank_scene() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_plane(1.0, 10.0);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene3d_add(scene, node);
    return scene;
}

static void test_navmesh_bake_agent_radius_erodes_narrow_corridor() {
    /* The voxel baker erodes the walkable set by the agent radius. On a 1.0-wide plank an agent of
     * radius 0.2 (clearance 0.4 < 1.0) keeps an inset walkable strip, while an agent of radius 0.6
     * (clearance 1.2 > 1.0) cannot fit, so the whole plank erodes away and the bake yields no
     * navmesh. The pre-voxel passthrough baker ignored agent_radius, so the wide-agent NULL is a
     * genuine fail-before/pass-after for R-NAV-003. */
    void *narrow = make_narrow_plank_scene();
    void *nm_small = rt_navmesh3d_bake(narrow, 0.2, 1.8, 45.0, 0.1);
    EXPECT_TRUE(nm_small != nullptr, "NavMesh erosion: small agent keeps the plank walkable");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nm_small) > 0,
                "NavMesh erosion: small-agent navmesh has walkable triangles");
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    void *near_edge = rt_vec3_new(0.45, 0.0, 0.0);
    void *path_from = rt_vec3_new(0.0, 0.0, -4.0);
    void *path_to = rt_vec3_new(0.0, 0.0, 4.0);
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm_small, center) != 0,
                "NavMesh erosion: plank center stays walkable for the small agent");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nm_small, near_edge) == 0,
                "NavMesh erosion: the agent-radius margin near the plank edge is eroded");
    EXPECT_TRUE(rt_navmesh3d_find_path(nm_small, path_from, path_to) != nullptr,
                "NavMesh erosion: small agent paths along the eroded corridor");

    void *wide = make_narrow_plank_scene();
    void *nm_wide = rt_navmesh3d_bake(wide, 0.6, 1.8, 45.0, 0.1);
    EXPECT_TRUE(nm_wide == nullptr,
                "NavMesh erosion: an agent wider than the corridor erodes it away entirely");
}

static void test_navmesh_query_grid_matches_linear_scan() {
    /* A voxel bake builds the spatial query grid that accelerates point location. Verify the
     * grid-backed find-triangle agrees with a brute-force linear scan at every sampled point, so
     * the acceleration is exact (no popping / missed snaps for agents querying the navmesh). */
    void *scene = make_scene_bake_fixture();
    void *nm = rt_navmesh3d_bake(scene, 0.4, 1.8, 45.0, 0.3);
    EXPECT_TRUE(nm != nullptr, "NavMesh query grid: scene bakes with a spatial index");
    EXPECT_TRUE(rt_navmesh3d_check_query_grid_parity(nm) != 0,
                "NavMesh query grid: grid point location matches the linear scan everywhere");
}

/*==========================================================================
 * AnimBlend3D tests
 *=========================================================================*/

static void test_blend_create() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, nullptr, -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *blend = rt_anim_blend3d_new(skel);
    EXPECT_TRUE(blend != nullptr, "AnimBlend3D created");
    EXPECT_TRUE(rt_anim_blend3d_state_count(blend) == 0, "Starts with 0 states");
}

static void test_blend_add_state() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, nullptr, -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *blend = rt_anim_blend3d_new(skel);

    void *anim = rt_animation3d_new(nullptr, 1.0);
    int64_t idx = rt_anim_blend3d_add_state(blend, nullptr, anim);
    EXPECT_TRUE(idx == 0, "First state index = 0");
    EXPECT_TRUE(rt_anim_blend3d_state_count(blend) == 1, "State count = 1");
}

static void test_blend_weight() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, nullptr, -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *blend = rt_anim_blend3d_new(skel);
    void *anim = rt_animation3d_new(nullptr, 1.0);
    rt_anim_blend3d_add_state(blend, nullptr, anim);

    rt_anim_blend3d_set_weight(blend, 0, 0.75);
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 0), 0.75, 0.01, "Weight set to 0.75");
}

static void test_blend_weight_sanitizes_inputs() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *blend = rt_anim_blend3d_new(skel);
    void *anim = rt_animation3d_new(rt_const_cstr("walk"), 1.0);
    rt_anim_blend3d_add_state(blend, rt_const_cstr("walk"), anim);

    rt_anim_blend3d_set_weight(blend, 0, -2.0);
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 0),
                0.0,
                0.01,
                "AnimBlend3D clamps negative weights to zero");
    rt_anim_blend3d_set_weight(blend, 0, 2.0);
    EXPECT_NEAR(
        rt_anim_blend3d_get_weight(blend, 0), 1.0, 0.01, "AnimBlend3D clamps high weights to one");
    rt_anim_blend3d_set_weight(blend, 0, NAN);
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 0),
                0.0,
                0.01,
                "AnimBlend3D converts NaN weights to zero");
    rt_anim_blend3d_set_weight_by_name(blend, rt_const_cstr("walk"), 0.25);
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 0),
                0.25,
                0.01,
                "AnimBlend3D clamps named weights through the same path");
    EXPECT_TRUE(rt_anim_blend3d_state_count(skel) == 0,
                "AnimBlend3D accessors reject non-blender handles");
}

static void test_blend_update_no_crash() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, nullptr, -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *blend = rt_anim_blend3d_new(skel);
    void *anim = rt_animation3d_new(nullptr, 1.0);
    rt_anim_blend3d_add_state(blend, nullptr, anim);
    rt_anim_blend3d_set_weight(blend, 0, 1.0);

    /* Should not crash */
    rt_anim_blend3d_update(blend, 0.016);
    rt_anim_blend3d_update(blend, 0.016);
    EXPECT_TRUE(1, "AnimBlend3D update runs without crash");
}

/*==========================================================================
 * BlendTree3D tests
 *=========================================================================*/

static void *make_blendtree_test_skeleton() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    return skel;
}

static void *make_blendtree_test_animation(const char *name) {
    return rt_animation3d_new(rt_const_cstr(name), 1.0);
}

static void test_blendtree_1d_weights() {
    void *skel = make_blendtree_test_skeleton();
    void *tree = rt_blend_tree3d_new_1d(skel);
    void *blend = rt_blend_tree3d_get_blend(tree);

    EXPECT_TRUE(tree != nullptr, "BlendTree3D.New1D creates a tree");
    EXPECT_TRUE(blend != nullptr, "BlendTree3D exposes an internal AnimBlend3D");
    EXPECT_TRUE(rt_blend_tree3d_add_sample(tree, make_blendtree_test_animation("idle"), 0.0, 0.0) ==
                    0,
                "BlendTree3D.AddSample returns first sample index");
    EXPECT_TRUE(rt_blend_tree3d_add_sample(tree, make_blendtree_test_animation("run"), 1.0, 0.0) ==
                    1,
                "BlendTree3D.AddSample returns second sample index");
    EXPECT_TRUE(rt_blend_tree3d_get_sample_count(tree) == 2,
                "BlendTree3D.SampleCount tracks samples");

    rt_blend_tree3d_set_param(tree, 0.25, 0.0);
    rt_blend_tree3d_update(tree, 0.0);
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 0),
                0.75,
                0.001,
                "BlendTree3D 1D lower sample weight is linear");
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 1),
                0.25,
                0.001,
                "BlendTree3D 1D upper sample weight is linear");

    rt_blend_tree3d_set_param(tree, -2.0, 0.0);
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 0),
                1.0,
                0.001,
                "BlendTree3D 1D clamps below range to first sample");
    rt_blend_tree3d_set_param(tree, 2.0, 0.0);
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 1),
                1.0,
                0.001,
                "BlendTree3D 1D clamps above range to last sample");
}

static void test_blendtree_2d_weights() {
    void *skel = make_blendtree_test_skeleton();
    void *tree = rt_blend_tree3d_new_2d(skel);
    void *blend = rt_blend_tree3d_get_blend(tree);

    EXPECT_TRUE(tree != nullptr, "BlendTree3D.New2D creates a tree");
    rt_blend_tree3d_add_sample(tree, make_blendtree_test_animation("center"), 0.0, 0.0);
    rt_blend_tree3d_add_sample(tree, make_blendtree_test_animation("right"), 1.0, 0.0);
    rt_blend_tree3d_add_sample(tree, make_blendtree_test_animation("up"), 0.0, 1.0);
    EXPECT_TRUE(rt_blend_tree3d_get_sample_count(tree) == 3, "BlendTree3D 2D stores all samples");

    rt_blend_tree3d_set_param(tree, 1.0, 0.0);
    rt_blend_tree3d_update(tree, 0.0);
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 0),
                0.0,
                0.001,
                "BlendTree3D 2D exact coordinate clears other samples");
    EXPECT_NEAR(rt_anim_blend3d_get_weight(blend, 1),
                1.0,
                0.001,
                "BlendTree3D 2D exact coordinate selects matching sample");

    rt_blend_tree3d_set_param(tree, 0.5, 0.0);
    rt_blend_tree3d_update(tree, 0.0);
    double w0 = rt_anim_blend3d_get_weight(blend, 0);
    double w1 = rt_anim_blend3d_get_weight(blend, 1);
    double w2 = rt_anim_blend3d_get_weight(blend, 2);
    EXPECT_NEAR(w0 + w1 + w2, 1.0, 0.001, "BlendTree3D 2D weights stay normalized");
    EXPECT_TRUE(w0 > w2 && w1 > w2, "BlendTree3D 2D weights favor nearer samples");
}

static void test_blendtree_rejects_bad_handles() {
    void *skel = make_blendtree_test_skeleton();
    void *tree = rt_blend_tree3d_new_1d(skel);

    EXPECT_TRUE(rt_blend_tree3d_new_1d(nullptr) == nullptr, "BlendTree3D rejects NULL skeletons");
    EXPECT_TRUE(rt_blend_tree3d_add_sample(tree, skel, 0.0, 0.0) == -1,
                "BlendTree3D rejects non-animation samples");
    EXPECT_TRUE(rt_blend_tree3d_get_sample_count(skel) == 0,
                "BlendTree3D accessors reject non-tree handles");
}

static void test_navmesh_export_import_roundtrip() {
    /* Build a navmesh, export it to a file, re-import it, and confirm the reconstructed mesh
     * preserves pathfinding geometry plus the v2 metadata sections. */
    void *plane = rt_mesh3d_new_plane(20.0, 20.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);
    EXPECT_TRUE(nm != nullptr, "source navmesh builds");
    int64_t tri_count = rt_navmesh3d_get_triangle_count(nm);
    EXPECT_TRUE(tri_count > 0, "source navmesh has walkable triangles");
    void *bounds_min = rt_vec3_new(-10.0, -0.1, -10.0);
    void *bounds_max = rt_vec3_new(10.0, 0.1, 10.0);
    EXPECT_TRUE(rt_navmesh3d_set_area(nm, bounds_min, bounds_max, rt_const_cstr("mud"), 2.5) == 1,
                "source navmesh stores area metadata before export");
    void *link_from = rt_vec3_new(-8.0, 0.0, -8.0);
    void *link_to = rt_vec3_new(8.0, 0.0, 8.0);
    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(nm, link_from, link_to, 1) == 1,
                "source navmesh stores an off-mesh link before export");
    EXPECT_TRUE(rt_navmesh3d_set_offmesh_link_metadata(nm, 0, rt_const_cstr("jump"), 3.5, 77) == 1,
                "source navmesh stores off-mesh link metadata before export");
    EXPECT_TRUE(rt_navmesh3d_add_obstacle(
                    nm, rt_vec3_new(40.0, -1.0, 40.0), rt_vec3_new(41.0, 1.0, 41.0)) == 1,
                "source navmesh stores an obstacle before export");

    rt_string path = rt_const_cstr("/tmp/zanna_navmesh_export_roundtrip.vnav");
    EXPECT_TRUE(rt_navmesh3d_export(nm, path) == 1, "navmesh exports to a file");
    {
        FILE *f = std::fopen("/tmp/zanna_navmesh_export_roundtrip.vnav", "rb");
        char magic[8] = {};
        EXPECT_TRUE(f != nullptr, "exported navmesh file opens for magic check");
        EXPECT_TRUE(f && std::fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
                        std::memcmp(magic, "VNAVMSH2", sizeof(magic)) == 0,
                    "navmesh export writes the version-2 magic");
        if (f)
            std::fclose(f);
    }

    void *imported = rt_navmesh3d_import(path);
    EXPECT_TRUE(imported != nullptr, "navmesh re-imports from the exported file");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(imported) == tri_count,
                "imported navmesh preserves the triangle count");
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_navmesh3d_get_area(imported, center)), "mud") == 0,
                "imported navmesh preserves area labels");
    EXPECT_NEAR(rt_navmesh3d_get_traversal_cost(imported, center),
                2.5,
                0.001,
                "imported navmesh preserves traversal costs");
    EXPECT_TRUE(rt_navmesh3d_get_offmesh_link_count(imported) == 1,
                "imported navmesh preserves off-mesh link count");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_navmesh3d_get_offmesh_link_kind(imported, 0)), "jump") == 0,
        "imported navmesh preserves off-mesh link kind");
    EXPECT_NEAR(rt_navmesh3d_get_offmesh_link_traversal_cost(imported, 0),
                3.5,
                0.001,
                "imported navmesh preserves off-mesh link traversal cost");
    EXPECT_TRUE(rt_navmesh3d_get_offmesh_link_state(imported, 0) == 77,
                "imported navmesh preserves off-mesh link state");
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(imported) == 1,
                "imported navmesh preserves obstacle count");
    EXPECT_TRUE(rt_navmesh3d_remove_obstacle(imported, 0) == 1,
                "imported navmesh keeps enough source data for obstacle edits");

    /* The imported navmesh is immediately path-queryable. */
    void *from = rt_vec3_new(-8.0, 0.0, -8.0);
    void *to = rt_vec3_new(8.0, 0.0, 8.0);
    EXPECT_TRUE(rt_navmesh3d_find_path(nm, from, to) != nullptr, "source navmesh resolves a path");
    EXPECT_TRUE(rt_navmesh3d_find_path(imported, from, to) != nullptr,
                "imported navmesh resolves the same path");

    void *blocked_nm = rt_navmesh3d_build(rt_mesh3d_new_plane(20.0, 20.0), 0.4, 1.8);
    EXPECT_TRUE(blocked_nm != nullptr, "blocked-state source navmesh builds");
    EXPECT_TRUE(rt_navmesh3d_add_obstacle(
                    blocked_nm, rt_vec3_new(-20.0, -1.0, -20.0), rt_vec3_new(20.0, 1.0, 20.0)) == 1,
                "source navmesh can export carved blocked state");
    rt_string blocked_path = rt_const_cstr("/tmp/zanna_navmesh_export_blocked.vnav");
    EXPECT_TRUE(rt_navmesh3d_export(blocked_nm, blocked_path) == 1,
                "blocked navmesh exports to a file");
    void *blocked_imported = rt_navmesh3d_import(blocked_path);
    EXPECT_TRUE(blocked_imported != nullptr, "blocked navmesh re-imports");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(blocked_imported, center) == 0,
                "imported navmesh preserves carved blocked triangles");

    {
        FILE *f = std::fopen("/tmp/zanna_navmesh_export_roundtrip.vnav", "ab");
        EXPECT_TRUE(f != nullptr, "exported navmesh opens for corrupt trailing-data probe");
        if (f) {
            unsigned char trailing = 0x7f;
            std::fwrite(&trailing, 1, 1, f);
            std::fclose(f);
        }
    }
    EXPECT_TRUE(rt_navmesh3d_import(path) == nullptr,
                "importing a navmesh with trailing bytes returns null");

    /* A missing/corrupt file is recoverable: import returns null without trapping. */
    EXPECT_TRUE(rt_navmesh3d_import(rt_const_cstr("/tmp/zanna_navmesh_no_such_file.vnav")) ==
                    nullptr,
                "importing a missing file returns null");
}

static void test_navmesh_getters_clamp_corrupt_private_counts() {
    void *plane = rt_mesh3d_new_plane(20.0, 20.0);
    void *nm = rt_navmesh3d_build(plane, 0.4, 1.8);
    EXPECT_TRUE(nm != nullptr, "NavMesh corrupt getters: source navmesh builds");
    void *link_from = rt_vec3_new(-8.0, 0.0, -8.0);
    void *link_to = rt_vec3_new(8.0, 0.0, 8.0);
    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(nm, link_from, link_to, 1) == 1,
                "NavMesh corrupt getters: source stores an off-mesh link");
    EXPECT_TRUE(rt_navmesh3d_set_offmesh_link_metadata(nm, 0, rt_const_cstr("jump"), 3.5, 77) == 1,
                "NavMesh corrupt getters: source stores off-mesh metadata");
    EXPECT_TRUE(rt_navmesh3d_add_obstacle(
                    nm, rt_vec3_new(40.0, -1.0, 40.0), rt_vec3_new(41.0, 1.0, 41.0)) == 1,
                "NavMesh corrupt getters: source stores an obstacle");

    auto *view = static_cast<NavMesh3DTestLayout *>(nm);
    int32_t saved_triangle_count = view->triangle_count;
    int32_t saved_link_count = view->offmesh_link_count;
    int32_t saved_link_capacity = view->offmesh_link_capacity;
    int32_t saved_obstacle_count = view->obstacle_count;
    int32_t saved_obstacle_capacity = view->obstacle_capacity;
    rt_string saved_kind = view->offmesh_links[0].kind;

    view->triangle_count = INT32_MAX;
    int64_t clamped_triangles = rt_navmesh3d_get_triangle_count(nm);
    EXPECT_TRUE(clamped_triangles > 0 && clamped_triangles <= view->source_triangle_count,
                "NavMesh corrupt getters: triangle count caps at retained source count");
    view->triangle_count = saved_triangle_count;

    view->offmesh_link_count = INT32_MAX;
    view->offmesh_link_capacity = 1;
    EXPECT_TRUE(rt_navmesh3d_get_offmesh_link_count(nm) == 1,
                "NavMesh corrupt getters: off-mesh count caps to capacity");
    {
        double segment_from[3] = {-8.0, 0.0, -8.0};
        double segment_to[3] = {8.0, 0.0, 8.0};
        EXPECT_TRUE(rt_navmesh3d_segment_is_offmesh_link(nm, segment_from, segment_to, nullptr) ==
                        1,
                    "NavMesh corrupt getters: segment lookup caps the authored-link scan");
    }
    EXPECT_TRUE(rt_navmesh3d_get_offmesh_link_state(nm, 1) == 0,
                "NavMesh corrupt getters: off-mesh state rejects capped-out index");
    EXPECT_TRUE(rt_navmesh3d_set_offmesh_link_metadata(nm, 1, rt_const_cstr("bad"), 1.0, 1) == 0,
                "NavMesh corrupt getters: off-mesh setter rejects capped-out index");
    EXPECT_TRUE(rt_navmesh3d_add_offmesh_link(nm, link_from, link_to, 1) == 0,
                "NavMesh corrupt getters: append rejects inconsistent off-mesh storage");
    view->offmesh_link_count = saved_link_count;
    view->offmesh_link_capacity = saved_link_capacity;

    view->offmesh_links[0].kind = reinterpret_cast<rt_string>(plane);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_navmesh3d_get_offmesh_link_kind(nm, 0)), "") == 0,
                "NavMesh corrupt getters: wrong-class off-mesh kind returns empty string");
    view->offmesh_links[0].kind = saved_kind;

    view->obstacle_count = INT32_MAX;
    view->obstacle_capacity = 1;
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(nm) == 1,
                "NavMesh corrupt getters: obstacle count caps to capacity");
    void *obstacle_min = rt_vec3_new(42.0, -1.0, 42.0);
    void *obstacle_max = rt_vec3_new(43.0, 1.0, 43.0);
    EXPECT_TRUE(rt_navmesh3d_add_obstacle(nm, obstacle_min, obstacle_max) == 0,
                "NavMesh corrupt getters: obstacle append rejects inconsistent storage");
    EXPECT_TRUE(rt_navmesh3d_remove_obstacle(nm, 0) == 0,
                "NavMesh corrupt getters: obstacle removal rejects inconsistent storage");
    EXPECT_TRUE(rt_navmesh3d_update_obstacle(nm, 0, obstacle_min, obstacle_max) == 0,
                "NavMesh corrupt getters: obstacle update rejects inconsistent storage");
    EXPECT_TRUE(rt_navmesh3d_test_inject_obstacle(nm, obstacle_min, obstacle_max) == 0,
                "NavMesh corrupt getters: test injection rejects inconsistent storage");
    view->obstacle_count = -5;
    EXPECT_TRUE(rt_navmesh3d_get_obstacle_count(nm) == 0,
                "NavMesh corrupt getters: negative obstacle count reads as zero");
    view->obstacle_count = saved_obstacle_count;
    view->obstacle_capacity = saved_obstacle_capacity;
}

static void test_skeleton_alias_table_growth() {
    void *skeleton = rt_skeleton3d_new();
    EXPECT_TRUE(skeleton != nullptr, "Skeleton alias growth: skeleton allocates");
    char external[32];
    char local[32];
    for (int i = 0; i < 24; ++i) {
        std::snprintf(external, sizeof(external), "external-%d", i);
        std::snprintf(local, sizeof(local), "local-%d", i);
        rt_skeleton3d_set_bone_alias(skeleton, rt_const_cstr(external), rt_const_cstr(local));
    }
    EXPECT_TRUE(rt_skeleton3d_get_alias_count(skeleton) == 24,
                "Skeleton alias growth: geometric growth preserves every alias");
}

int main() {
    /* NavMesh3D */
    test_navmesh_build_plane();
    test_navmesh_rejects_all_degenerate_geometry();
    test_navmesh_is_walkable();
    test_navmesh_not_walkable();
    test_navmesh_sample_position();
    test_navmesh_sample_position_uses_sublinear_local_probe();
    test_navmesh_find_path();
    test_navmesh_find_path_from_shared_edge();
    test_navmesh_rejects_out_of_domain_query_instead_of_clamping();
    test_navmesh_astar_touches_only_frontier_nodes();
    test_path3d_bulk_append_is_transactional();
    test_navmesh_find_path_adopts_reconstructed_storage();
    test_navmesh_path_workspace_reuse_is_concurrent_safe();
    test_navmesh_box_slope_filter();
    test_navmesh_voxel_grid_extreme_arithmetic_is_bounded();
    test_navmesh_rebuild_allocation_failure_is_atomic();
    test_navmesh_adjacency_edge_hash();
    test_navmesh_rejects_non_manifold_edges();
    test_navmesh_large_mesh();
    test_navmesh_agent_radius_blocks_narrow_portals();
    test_navmesh_offmesh_links_bridge_islands();
    test_navmesh_offmesh_links_validate_and_direct();
    test_navmesh_offmesh_link_metadata_affects_cost();
    test_navmesh_offmesh_links_keep_astar_heuristic_admissible();
    test_navmesh_obstacle_carving_uses_triangle_footprint();
    test_navmesh_add_obstacle_carves_walkable_triangles();
    test_navmesh_area_metadata_and_traversal_costs();
    test_navmesh_metadata_updates_are_transactional_and_length_exact();
    test_navmesh_bake_scene_flattens_transformed_nodes();
    test_navmesh_bake_tiled_and_rebuild_tile_baseline();
    test_navmesh_rebuild_tile_is_tile_local();
    test_navmesh_rebuild_tile_refreshes_retained_geometry_source();
    test_navmesh_bake_agent_radius_erodes_narrow_corridor();
    test_navmesh_query_grid_matches_linear_scan();
    test_navmesh_export_import_roundtrip();
    test_navmesh_getters_clamp_corrupt_private_counts();

    /* AnimBlend3D */
    test_skeleton_alias_table_growth();
    test_blend_create();
    test_blend_add_state();
    test_blend_weight();
    test_blend_weight_sanitizes_inputs();
    test_blend_update_no_crash();

    /* BlendTree3D */
    test_blendtree_1d_weights();
    test_blendtree_2d_weights();
    test_blendtree_rejects_bad_handles();

    printf("NavMesh3D+AnimBlend3D+BlendTree3D tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
