//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_navagent3d.cpp
// Purpose: Unit tests for NavAgent3D path following, bindings, warp state, and
//   deterministic snapshot/solve/apply crowd batches.
//
// Key invariants:
//   - Reversing batch input order cannot change published navigation results.
//   - Character/node bindings preserve the agent's authoritative position contract.
// Ownership/Lifetime:
//   - Test fixtures own temporary meshes/navmeshes/agents for process duration.
// Links: rt_navagent3d.h, rt_navmesh3d.h, rt_physics3d.h
//
//===----------------------------------------------------------------------===//

#include "rt_mat4.h"
#include "rt_navagent3d.h"
#include "rt_navmesh3d.h"
#include "rt_physics3d.h"
#include "rt_scene3d.h"

#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_mesh3d_new_plane(double sx, double sz);
extern void *rt_mesh3d_new(void);
extern void rt_mesh3d_add_vertex(
    void *obj, double x, double y, double z, double nx, double ny, double nz, double u, double v);
extern void rt_mesh3d_add_triangle(void *obj, int64_t i0, int64_t i1, int64_t i2);
extern void *rt_navmesh3d_find_path(void *navmesh, void *from, void *to);
extern double rt_path3d_get_length(void *path);
extern int64_t rt_path3d_get_point_count(void *path);
extern void *rt_path3d_get_position_at(void *path, double t);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
}

struct NavAgent3DTestLayout {
    void *vptr;
    void *navmesh;
    void *bound_character;
    void *bound_node;
    double radius;
    double height;
    double avoidance_radius;
    double position[3];
    double velocity[3];
    double desired_velocity[3];
    double target[3];
    double stopping_distance;
    double desired_speed;
    double remaining_distance;
    double repath_interval;
    double repath_accum;
    double *path_points_xyz;
    NavAgent3DTestLayout **avoidance_neighbors;
    int32_t path_point_count;
    int32_t path_index;
    int32_t avoidance_neighbor_capacity;
    int8_t has_target;
    int8_t has_path;
    int8_t auto_repath;
    int8_t avoidance_enabled;
    /* Byte-layout mirror of rt_navagent3d: keep the steering-pause flag in the
     * same position as the runtime struct. */
    int8_t stopped;
    NavAgent3DTestLayout *registry_next;
    NavAgent3DTestLayout *grid_next;
    int32_t grid_cx;
    int32_t grid_cz;
    int8_t in_grid;
    uint64_t stable_order;
    double *owned_path_points_xyz;
    double *owned_path_suffix_lengths;
    int32_t path_allocation_capacity;
    NavAgent3DTestLayout **owned_avoidance_neighbors;
    int32_t avoidance_allocation_capacity;
};

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (std::fabs((double)(a) - (double)(b)) > (eps)) {                                        \
            std::fprintf(                                                                          \
                stderr, "FAIL: %s (got %f, expected %f)\n", msg, (double)(a), (double)(b));        \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

static void get_node_world_position(void *node, double out_pos[3]) {
    void *world = rt_scene_node3d_get_world_matrix(node);
    void *pt = rt_mat4_transform_point(world, rt_vec3_new(0.0, 0.0, 0.0));
    out_pos[0] = pt ? rt_vec3_x(pt) : 0.0;
    out_pos[1] = pt ? rt_vec3_y(pt) : 0.0;
    out_pos[2] = pt ? rt_vec3_z(pt) : 0.0;
}

static void test_navagent_bound_node_reaches_target_in_world_space() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    double world[3];
    void *pos;

    rt_scene_node3d_set_position(parent, 2.0, 0.0, 1.0);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);
    rt_navagent3d_bind_node(agent, child);
    rt_navagent3d_warp(agent, rt_vec3_new(0.0, 0.0, 0.0));
    rt_navagent3d_set_desired_speed(agent, 6.0);
    rt_navagent3d_set_stopping_distance(agent, 0.2);
    rt_navagent3d_set_target(agent, rt_vec3_new(4.0, 0.0, 4.0));

    for (int i = 0; i < 80; i++)
        rt_navagent3d_update(agent, 0.05);

    get_node_world_position(child, world);
    pos = rt_navagent3d_get_position(agent);
    EXPECT_NEAR(world[0], 4.0, 0.35, "NavAgent3D drives bound SceneNode in world X");
    EXPECT_NEAR(world[2], 4.0, 0.35, "NavAgent3D drives bound SceneNode in world Z");
    EXPECT_NEAR(rt_vec3_x(pos), world[0], 0.01, "NavAgent3D position matches bound node world X");
    EXPECT_NEAR(rt_navagent3d_get_remaining_distance(agent),
                0.0,
                0.3,
                "NavAgent3D remaining distance reaches zero near the target");
    EXPECT_TRUE(rt_navagent3d_get_has_path(agent) == 0,
                "NavAgent3D clears active path state after arriving");
}

static void test_navagent_bound_character_moves_controller() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *world = rt_world3d_new(0.0, -9.8, 0.0);
    void *character = rt_character3d_new(0.4, 1.8, 80.0);
    void *pos;
    void *vel;

    rt_character3d_set_world(character, world);
    rt_navagent3d_bind_character(agent, character);
    rt_navagent3d_set_desired_speed(agent, 4.0);
    rt_navagent3d_set_target(agent, rt_vec3_new(3.0, 0.0, 0.0));

    for (int i = 0; i < 20; i++)
        rt_navagent3d_update(agent, 0.1);

    pos = rt_character3d_get_position(character);
    vel = rt_navagent3d_get_velocity(agent);
    EXPECT_TRUE(rt_vec3_x(pos) > 1.5, "NavAgent3D advances a bound Character3D");
    EXPECT_TRUE(rt_vec3_x(vel) >= 0.0, "NavAgent3D exposes actual velocity after character motion");
    EXPECT_TRUE(rt_navagent3d_get_has_path(agent) == 0 ||
                    rt_navagent3d_get_remaining_distance(agent) < 1.5,
                "NavAgent3D reduces remaining distance while driving Character3D");
}

static void test_navagent_warp_resets_motion_and_rebuilds_path() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *pos;
    void *vel;

    rt_navagent3d_set_target(agent, rt_vec3_new(5.0, 0.0, 0.0));
    rt_navagent3d_update(agent, 0.1);
    rt_navagent3d_warp(agent, rt_vec3_new(-3.0, 0.0, 0.0));
    pos = rt_navagent3d_get_position(agent);
    vel = rt_navagent3d_get_velocity(agent);
    EXPECT_NEAR(
        rt_vec3_x(pos), -3.0, 0.2, "NavAgent3D.Warp moves immediately to the snapped position");
    EXPECT_NEAR(rt_vec3_x(vel), 0.0, 0.001, "NavAgent3D.Warp clears the previous frame velocity");
    EXPECT_TRUE(rt_navagent3d_get_has_path(agent) != 0,
                "NavAgent3D.Warp rebuilds a live path when a target is still active");

    rt_navagent3d_update(agent, 0.1);
    EXPECT_TRUE(rt_vec3_x(rt_navagent3d_get_desired_velocity(agent)) > 0.0,
                "NavAgent3D resumes steering toward the target after Warp");
}

static void test_navagent_character_binding_updates_bound_node() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *world = rt_world3d_new(0.0, -9.8, 0.0);
    void *character = rt_character3d_new(0.4, 1.8, 80.0);
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    double world_pos[3];
    void *char_pos;

    rt_character3d_set_world(character, world);
    rt_scene_node3d_set_position(parent, 1.0, 0.0, -2.0);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);
    rt_navagent3d_bind_character(agent, character);
    rt_navagent3d_bind_node(agent, child);
    rt_navagent3d_set_target(agent, rt_vec3_new(2.0, 0.0, 2.0));

    for (int i = 0; i < 15; i++)
        rt_navagent3d_update(agent, 0.1);

    get_node_world_position(child, world_pos);
    char_pos = rt_character3d_get_position(character);
    EXPECT_NEAR(world_pos[0],
                rt_vec3_x(char_pos),
                0.01,
                "NavAgent3D mirrors bound Character3D X into the bound SceneNode");
    EXPECT_NEAR(world_pos[2],
                rt_vec3_z(char_pos),
                0.01,
                "NavAgent3D mirrors bound Character3D Z into the bound SceneNode");
}

static void test_navagent_avoidance_properties() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.5, 1.8);
    void *agent = rt_navagent3d_new(navmesh, 0.5, 1.8);

    EXPECT_TRUE(rt_navagent3d_get_avoidance_enabled(agent) == 0,
                "NavAgent3D.AvoidanceEnabled defaults to false");
    EXPECT_NEAR(rt_navagent3d_get_avoidance_radius(agent),
                0.5,
                0.001,
                "NavAgent3D.AvoidanceRadius defaults to the agent radius");

    rt_navagent3d_set_avoidance_enabled(agent, 1);
    EXPECT_TRUE(rt_navagent3d_get_avoidance_enabled(agent) == 1,
                "NavAgent3D.AvoidanceEnabled can be enabled");

    rt_navagent3d_set_avoidance_radius(agent, 1.25);
    EXPECT_NEAR(rt_navagent3d_get_avoidance_radius(agent),
                1.25,
                0.001,
                "NavAgent3D.AvoidanceRadius stores positive values");

    rt_navagent3d_set_avoidance_radius(agent, -2.0);
    EXPECT_NEAR(rt_navagent3d_get_avoidance_radius(agent),
                0.0,
                0.001,
                "NavAgent3D.AvoidanceRadius clamps negative values to zero");

    rt_navagent3d_set_avoidance_radius(agent, std::numeric_limits<double>::infinity());
    EXPECT_NEAR(rt_navagent3d_get_avoidance_radius(agent),
                0.0,
                0.001,
                "NavAgent3D.AvoidanceRadius clamps non-finite values to zero");

    rt_navagent3d_set_avoidance_enabled(agent, 0);
    EXPECT_TRUE(rt_navagent3d_get_avoidance_enabled(agent) == 0,
                "NavAgent3D.AvoidanceEnabled can be disabled");
}

static void test_navagent_local_avoidance_reduces_head_on_velocity() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *plain_a = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *plain_b = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *avoid_a = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *avoid_b = rt_navagent3d_new(navmesh, 0.4, 1.8);
    double plain_ax;
    double plain_bx;
    double avoid_ax;
    double avoid_bx;
    double avoid_az;
    double avoid_bz;

    rt_navagent3d_set_desired_speed(plain_a, 2.0);
    rt_navagent3d_set_desired_speed(plain_b, 2.0);
    rt_navagent3d_set_stopping_distance(plain_a, 0.05);
    rt_navagent3d_set_stopping_distance(plain_b, 0.05);
    rt_navagent3d_warp(plain_a, rt_vec3_new(-0.4, 0.0, 0.0));
    rt_navagent3d_warp(plain_b, rt_vec3_new(0.4, 0.0, 0.0));
    rt_navagent3d_set_target(plain_a, rt_vec3_new(4.0, 0.0, 0.0));
    rt_navagent3d_set_target(plain_b, rt_vec3_new(-4.0, 0.0, 0.0));
    rt_navagent3d_update(plain_a, 0.1);
    rt_navagent3d_update(plain_b, 0.1);
    plain_ax = rt_vec3_x(rt_navagent3d_get_desired_velocity(plain_a));
    plain_bx = rt_vec3_x(rt_navagent3d_get_desired_velocity(plain_b));

    rt_navagent3d_set_desired_speed(avoid_a, 2.0);
    rt_navagent3d_set_desired_speed(avoid_b, 2.0);
    rt_navagent3d_set_stopping_distance(avoid_a, 0.05);
    rt_navagent3d_set_stopping_distance(avoid_b, 0.05);
    rt_navagent3d_set_avoidance_enabled(avoid_a, 1);
    rt_navagent3d_set_avoidance_enabled(avoid_b, 1);
    rt_navagent3d_set_avoidance_radius(avoid_a, 0.8);
    rt_navagent3d_set_avoidance_radius(avoid_b, 0.8);
    rt_navagent3d_warp(avoid_a, rt_vec3_new(-1.2, 0.0, 0.0));
    rt_navagent3d_warp(avoid_b, rt_vec3_new(1.2, 0.0, 0.0));
    rt_navagent3d_set_target(avoid_a, rt_vec3_new(4.0, 0.0, 0.0));
    rt_navagent3d_set_target(avoid_b, rt_vec3_new(-4.0, 0.0, 0.0));
    rt_navagent3d_update(avoid_a, 0.1);
    rt_navagent3d_update(avoid_b, 0.1);
    avoid_ax = rt_vec3_x(rt_navagent3d_get_desired_velocity(avoid_a));
    avoid_bx = rt_vec3_x(rt_navagent3d_get_desired_velocity(avoid_b));
    avoid_az = rt_vec3_z(rt_navagent3d_get_desired_velocity(avoid_a));
    avoid_bz = rt_vec3_z(rt_navagent3d_get_desired_velocity(avoid_b));

    EXPECT_TRUE(plain_ax > 1.0, "NavAgent3D baseline agent steers toward its target");
    EXPECT_TRUE(plain_bx < -1.0, "NavAgent3D baseline peer steers toward its target");
    EXPECT_TRUE(std::fabs(avoid_ax) <= std::fabs(plain_ax) + 0.01,
                "NavAgent3D avoidance keeps agent within preferred forward speed");
    EXPECT_TRUE(std::fabs(avoid_bx) <= std::fabs(plain_bx) + 0.01,
                "NavAgent3D avoidance keeps peer within preferred forward speed");
    EXPECT_TRUE(std::fabs(avoid_az) > 0.25,
                "NavAgent3D RVO avoidance adds lateral passing velocity");
    EXPECT_TRUE(std::fabs(avoid_az) + std::fabs(avoid_bz) > 0.25,
                "NavAgent3D reciprocal avoidance creates a collision-free passing side");
}

static void test_navagent_avoidance_breaks_head_on_deadlock() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *a = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *b = rt_navagent3d_new(navmesh, 0.4, 1.8);
    rt_navagent3d_set_desired_speed(a, 2.0);
    rt_navagent3d_set_desired_speed(b, 2.0);
    rt_navagent3d_set_stopping_distance(a, 0.05);
    rt_navagent3d_set_stopping_distance(b, 0.05);
    rt_navagent3d_set_avoidance_enabled(a, 1);
    rt_navagent3d_set_avoidance_enabled(b, 1);
    rt_navagent3d_set_avoidance_radius(a, 0.5);
    rt_navagent3d_set_avoidance_radius(b, 0.5);
    /* Start on opposite sides heading to each other's start: a perfectly symmetric head-on. */
    rt_navagent3d_warp(a, rt_vec3_new(-3.0, 0.0, 0.0));
    rt_navagent3d_warp(b, rt_vec3_new(3.0, 0.0, 0.0));
    rt_navagent3d_set_target(a, rt_vec3_new(3.0, 0.0, 0.0));
    rt_navagent3d_set_target(b, rt_vec3_new(-3.0, 0.0, 0.0));

    double min_dist = 1e9;
    for (int i = 0; i < 200; ++i) {
        rt_navagent3d_update(a, 1.0 / 30.0);
        rt_navagent3d_update(b, 1.0 / 30.0);
        void *pa = rt_navagent3d_get_position(a);
        void *pb = rt_navagent3d_get_position(b);
        double dx = rt_vec3_x(pa) - rt_vec3_x(pb);
        double dz = rt_vec3_z(pa) - rt_vec3_z(pb);
        double d = std::sqrt(dx * dx + dz * dz);
        if (d < min_dist)
            min_dist = d;
    }
    void *pa = rt_navagent3d_get_position(a);
    void *pb = rt_navagent3d_get_position(b);
    /* Without a tie-break the symmetric head-on stalls both agents at x~0; the perpendicular
     * tie-break lets them veer to opposite sides and cross past each other to their swapped goals.
     */
    EXPECT_TRUE(rt_vec3_x(pa) > 1.0,
                "Head-on agent A crosses past the meeting point (no deadlock)");
    EXPECT_TRUE(rt_vec3_x(pb) < -1.0,
                "Head-on agent B crosses past the meeting point (no deadlock)");
    EXPECT_TRUE(min_dist > 0.4, "Head-on agents avoid deep interpenetration while passing");
}

static void test_navagent_crowd_multiple_pairs_cross() {
    /* Several head-on pairs in well-separated Z lanes exercise the avoidance with many simultaneous
     * agents in the shared registry. Lanes are far enough apart not to interfere, so each pair
     * independently resolves its head-on; all agents reach their swapped goals. */
    void *mesh = rt_mesh3d_new_plane(40.0, 40.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    const int pairs = 3;
    void *as[3];
    void *bs[3];
    for (int p = 0; p < pairs; ++p) {
        double z = (double)(p - 1) * 5.0; /* lanes z = -5, 0, +5 */
        as[p] = rt_navagent3d_new(navmesh, 0.4, 1.8);
        bs[p] = rt_navagent3d_new(navmesh, 0.4, 1.8);
        rt_navagent3d_set_desired_speed(as[p], 2.0);
        rt_navagent3d_set_desired_speed(bs[p], 2.0);
        rt_navagent3d_set_stopping_distance(as[p], 0.05);
        rt_navagent3d_set_stopping_distance(bs[p], 0.05);
        rt_navagent3d_set_avoidance_enabled(as[p], 1);
        rt_navagent3d_set_avoidance_enabled(bs[p], 1);
        rt_navagent3d_set_avoidance_radius(as[p], 0.5);
        rt_navagent3d_set_avoidance_radius(bs[p], 0.5);
        rt_navagent3d_warp(as[p], rt_vec3_new(-3.0, 0.0, z));
        rt_navagent3d_warp(bs[p], rt_vec3_new(3.0, 0.0, z));
        rt_navagent3d_set_target(as[p], rt_vec3_new(3.0, 0.0, z));
        rt_navagent3d_set_target(bs[p], rt_vec3_new(-3.0, 0.0, z));
    }
    for (int i = 0; i < 250; ++i) {
        for (int p = 0; p < pairs; ++p) {
            rt_navagent3d_update(as[p], 1.0 / 30.0);
            rt_navagent3d_update(bs[p], 1.0 / 30.0);
        }
    }
    for (int p = 0; p < pairs; ++p) {
        EXPECT_TRUE(rt_vec3_x(rt_navagent3d_get_position(as[p])) > 1.0,
                    "Crowd: each lane's agent A crosses to its goal");
        EXPECT_TRUE(rt_vec3_x(rt_navagent3d_get_position(bs[p])) < -1.0,
                    "Crowd: each lane's agent B crosses to its goal");
    }
}

static void test_navagent_avoidance_grid_matches_full_scan() {
    /* O(N) peer-culling correctness: populate the shared registry with many agents spread across
     * several spatial-grid cells, give them crossing targets so they converge into shared cells,
     * and assert the grid-based avoidance query yields the same steering as a brute-force full
     * registry scan at every checked frame. A missed neighbor would diverge the two. */
    void *mesh = rt_mesh3d_new_plane(60.0, 60.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    const int n = 24;
    void *agents[24];
    for (int i = 0; i < n; ++i) {
        double x = (double)((i % 6) * 4) - 10.0; /* spread across X cells */
        double z = (double)((i / 6) * 4) - 6.0;  /* and Z cells */
        agents[i] = rt_navagent3d_new(navmesh, 0.4, 1.8);
        rt_navagent3d_set_desired_speed(agents[i], 2.0);
        rt_navagent3d_set_avoidance_enabled(agents[i], 1);
        rt_navagent3d_set_avoidance_radius(agents[i], 0.6);
        rt_navagent3d_warp(agents[i], rt_vec3_new(x, 0.0, z));
        rt_navagent3d_set_target(agents[i],
                                 rt_vec3_new(-x, 0.0, -z)); /* cross through the middle */
    }
    EXPECT_TRUE(rt_navagent3d_check_avoidance_grid_parity() == 1,
                "NavAgent grid avoidance matches full-scan at rest");
    for (int step = 0; step < 60; ++step) {
        for (int i = 0; i < n; ++i)
            rt_navagent3d_update(agents[i], 1.0 / 30.0);
        if (step % 15 == 0)
            EXPECT_TRUE(rt_navagent3d_check_avoidance_grid_parity() == 1,
                        "NavAgent grid avoidance matches full-scan during crowd motion");
    }
    EXPECT_TRUE(rt_navagent3d_check_avoidance_grid_parity() == 1,
                "NavAgent grid avoidance matches full-scan after crowd converges");
}

static void test_navagent_agent_count_perf_target() {
    void *mesh = rt_mesh3d_new_plane(240.0, 240.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.35, 1.8);
    const int lanes = 100;
    const int pairs_per_lane = 1;
    const int pair_count = lanes * pairs_per_lane;
    const int agent_count = pair_count * 2;
    const int frames = 180;
    void *agents[agent_count];
    double start_x[agent_count];
    double min_pair_distance = 1.0e9;
    int idx = 0;

    for (int lane = 0; lane < lanes; ++lane) {
        double z = ((double)lane - ((double)lanes - 1.0) * 0.5) * 2.0;
        for (int col = 0; col < pairs_per_lane; ++col) {
            double offset = (double)col * 0.75;
            double left_start = -10.0 - offset;
            double right_start = 10.0 + offset;
            double left_goal = 10.0 - offset;
            double right_goal = -10.0 + offset;
            void *left = rt_navagent3d_new(navmesh, 0.35, 1.8);
            void *right = rt_navagent3d_new(navmesh, 0.35, 1.8);
            rt_navagent3d_set_desired_speed(left, 4.0);
            rt_navagent3d_set_desired_speed(right, 4.0);
            rt_navagent3d_set_stopping_distance(left, 0.2);
            rt_navagent3d_set_stopping_distance(right, 0.2);
            rt_navagent3d_set_avoidance_enabled(left, 1);
            rt_navagent3d_set_avoidance_enabled(right, 1);
            rt_navagent3d_set_avoidance_radius(left, 0.7);
            rt_navagent3d_set_avoidance_radius(right, 0.7);
            rt_navagent3d_warp(left, rt_vec3_new(left_start, 0.0, z));
            rt_navagent3d_warp(right, rt_vec3_new(right_start, 0.0, z));
            rt_navagent3d_set_target(left, rt_vec3_new(left_goal, 0.0, z));
            rt_navagent3d_set_target(right, rt_vec3_new(right_goal, 0.0, z));
            agents[idx] = left;
            start_x[idx] = left_start;
            idx++;
            agents[idx] = right;
            start_x[idx] = right_start;
            idx++;
        }
    }

    auto update_begin = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        for (int i = 0; i < agent_count; ++i)
            rt_navagent3d_update(agents[i], 1.0 / 30.0);
        if (frame % 20 == 0) {
            for (int i = 0; i < agent_count; ++i) {
                void *pa = rt_navagent3d_get_position(agents[i]);
                for (int j = i + 1; j < agent_count; ++j) {
                    void *pb = rt_navagent3d_get_position(agents[j]);
                    double dx = rt_vec3_x(pa) - rt_vec3_x(pb);
                    double dz = rt_vec3_z(pa) - rt_vec3_z(pb);
                    double d = std::sqrt(dx * dx + dz * dz);
                    if (d < min_pair_distance)
                        min_pair_distance = d;
                }
            }
        }
    }
    auto update_end = std::chrono::steady_clock::now();

    int crossed = 0;
    for (int i = 0; i < agent_count; ++i) {
        void *pos = rt_navagent3d_get_position(agents[i]);
        double x = rt_vec3_x(pos);
        if ((start_x[i] < 0.0 && x > 0.0) || (start_x[i] > 0.0 && x < 0.0))
            crossed++;
    }
    long long update_us =
        (long long)std::chrono::duration_cast<std::chrono::microseconds>(update_end - update_begin)
            .count();
    std::printf("NAVAGENT_CROWD_TARGET: agents=%d frames=%d update_us=%lld "
                "min_pair_distance=%.3f crossed=%d\n",
                agent_count,
                frames,
                update_us,
                min_pair_distance,
                crossed);

    EXPECT_TRUE(agent_count >= 200, "NavAgent crowd target uses hundreds of agents");
    EXPECT_TRUE(crossed >= agent_count * 3 / 4,
                "NavAgent crowd target: most agents cross through the crowd");
    EXPECT_TRUE(min_pair_distance > 0.25,
                "NavAgent crowd target: RVO keeps a bounded minimum pair distance");
    EXPECT_TRUE(rt_navagent3d_check_avoidance_grid_parity() == 1,
                "NavAgent crowd target: grid and full-scan RVO agree after the fixture");
}

static void test_navagent_rejects_wrong_handle_types() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *bad_node = rt_scene_node3d_new();
    void *pos;

    EXPECT_TRUE(rt_navagent3d_new(bad_node, 0.4, 1.8) == nullptr,
                "NavAgent3D.New rejects non-NavMesh3D handles");

    rt_navagent3d_bind_node(agent, navmesh);
    rt_navagent3d_set_target(agent, agent);
    EXPECT_TRUE(rt_navagent3d_get_has_path(agent) == 0,
                "NavAgent3D.SetTarget rejects non-Vec3 handles");

    rt_navagent3d_warp(agent, agent);
    pos = rt_navagent3d_get_position(agent);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.001, "NavAgent3D.Warp rejects non-Vec3 X");
    EXPECT_NEAR(rt_vec3_z(pos), 0.0, 0.001, "NavAgent3D.Warp rejects non-Vec3 Z");
}

static void test_navagent_repairs_wrong_class_private_bindings() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent_obj = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *wrong_mesh = rt_mesh3d_new_plane(1.0, 1.0);
    auto *agent = (NavAgent3DTestLayout *)agent_obj;
    EXPECT_TRUE(agent != nullptr, "NavAgent3D.New creates an agent for binding repair test");

    agent->navmesh = wrong_mesh;
    agent->bound_character = wrong_mesh;
    agent->bound_node = wrong_mesh;
    rt_navagent3d_set_target(agent_obj,
                             rt_vec3_new(std::numeric_limits<double>::infinity(), 0.0, -1.0e300));
    rt_navagent3d_update(agent_obj, 0.1);

    EXPECT_TRUE(agent->navmesh == nullptr, "NavAgent3D drops wrong-class private navmesh slots");
    EXPECT_TRUE(agent->bound_character == nullptr,
                "NavAgent3D drops wrong-class private character slots");
    EXPECT_TRUE(agent->bound_node == nullptr, "NavAgent3D drops wrong-class private node slots");
    EXPECT_TRUE(std::isfinite(agent->target[0]) && std::isfinite(agent->target[2]),
                "NavAgent3D clamps extreme private target coordinates through SetTarget");
}

static void test_navagent_getters_sanitize_corrupt_private_state() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent_obj = rt_navagent3d_new(navmesh, 0.4, 1.8);
    auto *agent = (NavAgent3DTestLayout *)agent_obj;
    EXPECT_TRUE(agent != nullptr, "NavAgent3D.New creates an agent for corrupt getter test");

    agent->position[0] = std::numeric_limits<double>::infinity();
    agent->position[1] = -std::numeric_limits<double>::infinity();
    agent->position[2] = std::numeric_limits<double>::quiet_NaN();
    agent->radius = std::numeric_limits<double>::quiet_NaN();
    agent->height = std::numeric_limits<double>::infinity();
    agent->avoidance_radius = -std::numeric_limits<double>::infinity();
    agent->velocity[0] = std::numeric_limits<double>::infinity();
    agent->desired_velocity[1] = std::numeric_limits<double>::quiet_NaN();
    agent->target[2] = -std::numeric_limits<double>::infinity();
    agent->remaining_distance = std::numeric_limits<double>::infinity();
    agent->repath_interval = 0.0;
    agent->repath_accum = std::numeric_limits<double>::infinity();
    agent->has_target = -9;
    agent->has_path = -7;
    agent->auto_repath = -3;
    agent->avoidance_enabled = 42;

    void *pos = rt_navagent3d_get_position(agent_obj);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(pos)) && std::isfinite(rt_vec3_y(pos)) &&
                    std::isfinite(rt_vec3_z(pos)),
                "NavAgent3D.GetPosition returns finite coordinates for corrupt private state");
    EXPECT_TRUE(rt_navagent3d_get_has_path(agent_obj) == 0,
                "NavAgent3D clears HasPath when no owned corner storage exists");
    EXPECT_TRUE(rt_navagent3d_get_auto_repath(agent_obj) == 1,
                "NavAgent3D.AutoRepath getter normalizes corrupt nonzero flags");
    EXPECT_TRUE(rt_navagent3d_get_avoidance_enabled(agent_obj) == 1,
                "NavAgent3D.AvoidanceEnabled getter normalizes corrupt nonzero flags");
    EXPECT_TRUE(std::isfinite(agent->radius) && agent->radius > 0.0 &&
                    std::isfinite(agent->height) && agent->height > 0.0 &&
                    std::isfinite(agent->avoidance_radius) && agent->avoidance_radius >= 0.0,
                "NavAgent3D persists repaired dimensions and avoidance radius");
    EXPECT_TRUE(std::isfinite(agent->velocity[0]) && std::isfinite(agent->desired_velocity[1]) &&
                    std::isfinite(agent->target[2]),
                "NavAgent3D persists repaired motion and target vectors");
    EXPECT_TRUE(agent->has_target == 1 && agent->repath_interval == 0.25 &&
                    agent->repath_accum <= agent->repath_interval,
                "NavAgent3D canonicalizes target and repath timer state");
    EXPECT_NEAR(agent->remaining_distance,
                0.0,
                0.0,
                "NavAgent3D clears remaining distance without owned path storage");
}

static void test_navagent_repairs_owned_path_and_neighbor_storage() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent_obj = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *peer_obj = rt_navagent3d_new(navmesh, 0.4, 1.8);
    auto *agent = reinterpret_cast<NavAgent3DTestLayout *>(agent_obj);
    auto *peer = reinterpret_cast<NavAgent3DTestLayout *>(peer_obj);

    rt_navagent3d_set_target(agent_obj, rt_vec3_new(5.0, 0.0, 0.0));
    EXPECT_TRUE(agent->owned_path_points_xyz != nullptr &&
                    agent->owned_path_suffix_lengths != nullptr &&
                    agent->path_allocation_capacity >= 1,
                "NavAgent3D path rebuild publishes owned corners and suffix distances together");
    double *owned_points = agent->owned_path_points_xyz;
    double fake_points[3] = {99.0, 99.0, 99.0};
    agent->path_points_xyz = fake_points;
    agent->path_point_count = INT32_MAX;
    agent->path_index = INT32_MIN;
    agent->has_path = -7;
    EXPECT_TRUE(rt_navagent3d_get_has_path(agent_obj) == 1,
                "NavAgent3D retains a live path after repairing corrupt logical metadata");
    EXPECT_TRUE(agent->path_points_xyz == owned_points &&
                    agent->path_point_count == agent->path_allocation_capacity &&
                    agent->path_index == 0,
                "NavAgent3D restores the path pointer and clamps count/index to allocation bounds");
    rt_navagent3d_update(agent_obj, 0.05);
    EXPECT_TRUE(std::isfinite(rt_navagent3d_get_remaining_distance(agent_obj)),
                "NavAgent3D uses its suffix cache safely after path metadata repair");

    rt_navagent3d_set_avoidance_enabled(agent_obj, 1);
    rt_navagent3d_set_avoidance_enabled(peer_obj, 1);
    rt_navagent3d_warp(agent_obj, rt_vec3_new(-1.0, 0.0, 0.0));
    rt_navagent3d_warp(peer_obj, rt_vec3_new(1.0, 0.0, 0.0));
    rt_navagent3d_set_target(agent_obj, rt_vec3_new(4.0, 0.0, 0.0));
    rt_navagent3d_set_target(peer_obj, rt_vec3_new(-4.0, 0.0, 0.0));
    rt_navagent3d_update(agent_obj, 0.05);
    EXPECT_TRUE(agent->owned_avoidance_neighbors != nullptr &&
                    agent->avoidance_allocation_capacity > 0,
                "NavAgent3D grows reusable owned avoidance-neighbor storage");
    auto **owned_neighbors = agent->owned_avoidance_neighbors;
    NavAgent3DTestLayout *fake_neighbors[1] = {peer};
    agent->avoidance_neighbors = fake_neighbors;
    agent->avoidance_neighbor_capacity = INT32_MAX;
    rt_navagent3d_update(agent_obj, 0.05);
    EXPECT_TRUE(agent->avoidance_neighbors == owned_neighbors &&
                    agent->avoidance_neighbor_capacity == agent->avoidance_allocation_capacity,
                "NavAgent3D restores neighbor scratch mirrors before collection/growth");

    agent->path_points_xyz = fake_points;
    agent->avoidance_neighbors = fake_neighbors;
    if (rt_obj_release_check0(agent_obj))
        rt_obj_free(agent_obj);
    EXPECT_NEAR(fake_points[0],
                99.0,
                0.0,
                "NavAgent3D finalization frees owned path storage, not a corrupt mirror");
    EXPECT_TRUE(fake_neighbors[0] == peer,
                "NavAgent3D finalization leaves corrupt neighbor mirrors unowned");
}

static void test_navagent_singular_parent_world_write_is_exact_or_rejected() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    double local_x = 0.0;
    double local_y = 0.0;
    double local_z = 0.0;

    rt_scene_node3d_set_position(parent, 2.0, 0.0, 0.0);
    rt_scene_node3d_set_scale(parent, 0.0, 1.0, 1.0);
    rt_scene_node3d_set_position(child, 1.0, 0.0, 0.0);
    rt_scene_node3d_add_child(parent, child);
    rt_navagent3d_bind_node(agent, child);
    rt_navagent3d_warp(agent, rt_vec3_new(5.0, 0.0, 0.0));
    EXPECT_TRUE(rt_scene_node3d_get_position_components(child, &local_x, &local_y, &local_z) == 1,
                "SceneNode3D exposes the singular-parent fixture's local position");
    EXPECT_NEAR(local_x,
                1.0,
                0.0,
                "NavAgent3D leaves local state unchanged when exact world conversion is singular");
}

static void test_navagent_invalid_handle_numeric_getters_return_sentinel() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *agent = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *not_an_agent = rt_scene_node3d_new();

    // Invalid handles (null and wrong-class) return the -1 sentinel, distinguishable from a
    // legitimate 0 (e.g. "no path" / "at goal"), which the lenient getters used to be confused
    // with.
    EXPECT_NEAR(rt_navagent3d_get_remaining_distance(nullptr),
                -1.0,
                0.0,
                "NavAgent3D.RemainingDistance returns -1 for a null handle");
    EXPECT_NEAR(rt_navagent3d_get_stopping_distance(nullptr),
                -1.0,
                0.0,
                "NavAgent3D.StoppingDistance returns -1 for a null handle");
    EXPECT_NEAR(rt_navagent3d_get_desired_speed(nullptr),
                -1.0,
                0.0,
                "NavAgent3D.DesiredSpeed returns -1 for a null handle");
    EXPECT_NEAR(rt_navagent3d_get_avoidance_radius(nullptr),
                -1.0,
                0.0,
                "NavAgent3D.AvoidanceRadius returns -1 for a null handle");
    EXPECT_NEAR(rt_navagent3d_get_remaining_distance(not_an_agent),
                -1.0,
                0.0,
                "NavAgent3D.RemainingDistance returns -1 for a wrong-class handle");

    // A valid agent never returns the sentinel; every numeric getter stays >= 0.
    EXPECT_TRUE(rt_navagent3d_get_remaining_distance(agent) >= 0.0,
                "NavAgent3D.RemainingDistance is non-negative for a valid agent");
    EXPECT_TRUE(rt_navagent3d_get_stopping_distance(agent) >= 0.0,
                "NavAgent3D.StoppingDistance is non-negative for a valid agent");
    EXPECT_TRUE(rt_navagent3d_get_desired_speed(agent) >= 0.0,
                "NavAgent3D.DesiredSpeed is non-negative for a valid agent");
    EXPECT_TRUE(rt_navagent3d_get_avoidance_radius(agent) >= 0.0,
                "NavAgent3D.AvoidanceRadius is non-negative for a valid agent");
}

/// @brief Verify batch avoidance is invariant to caller iteration order.
/// @details The same two-agent head-on state is advanced once in creation order, reset exactly,
///   and advanced again with the input handles reversed. Position, actual velocity, and solved
///   desired velocity must match lane-for-lane, while a nonzero lateral result confirms the test
///   exercised avoidance rather than only straight-line integration.
static void test_navagent_batch_update_is_order_independent() {
    void *mesh = rt_mesh3d_new_plane(20.0, 20.0);
    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    void *a = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *b = rt_navagent3d_new(navmesh, 0.4, 1.8);
    void *forward[2] = {a, b};
    void *reverse[2] = {b, a};
    for (void *agent : forward) {
        rt_navagent3d_set_desired_speed(agent, 2.0);
        rt_navagent3d_set_stopping_distance(agent, 0.05);
        rt_navagent3d_set_avoidance_enabled(agent, 1);
        rt_navagent3d_set_avoidance_radius(agent, 0.8);
    }

    auto reset_pair = [&]() {
        rt_navagent3d_clear_target(a);
        rt_navagent3d_clear_target(b);
        rt_navagent3d_warp(a, rt_vec3_new(-1.2, 0.0, 0.0));
        rt_navagent3d_warp(b, rt_vec3_new(1.2, 0.0, 0.0));
        rt_navagent3d_set_target(a, rt_vec3_new(4.0, 0.0, 0.0));
        rt_navagent3d_set_target(b, rt_vec3_new(-4.0, 0.0, 0.0));
    };
    reset_pair();
    EXPECT_TRUE(rt_navagent3d_update_batch(forward, 2, 0.1) == 2,
                "NavAgent3D batch processes both unique valid handles");
    double first_position[2][2];
    double first_velocity[2][2];
    double first_desired[2][2];
    for (int i = 0; i < 2; i++) {
        void *position = rt_navagent3d_get_position(forward[i]);
        void *velocity = rt_navagent3d_get_velocity(forward[i]);
        void *desired = rt_navagent3d_get_desired_velocity(forward[i]);
        first_position[i][0] = rt_vec3_x(position);
        first_position[i][1] = rt_vec3_z(position);
        first_velocity[i][0] = rt_vec3_x(velocity);
        first_velocity[i][1] = rt_vec3_z(velocity);
        first_desired[i][0] = rt_vec3_x(desired);
        first_desired[i][1] = rt_vec3_z(desired);
    }
    EXPECT_TRUE(std::fabs(first_desired[0][1]) > 0.1 || std::fabs(first_desired[1][1]) > 0.1,
                "NavAgent3D batch fixture exercises lateral avoidance");

    reset_pair();
    EXPECT_TRUE(rt_navagent3d_update_batch(reverse, 2, 0.1) == 2,
                "NavAgent3D batch accepts reversed caller iteration order");
    for (int i = 0; i < 2; i++) {
        void *position = rt_navagent3d_get_position(forward[i]);
        void *velocity = rt_navagent3d_get_velocity(forward[i]);
        void *desired = rt_navagent3d_get_desired_velocity(forward[i]);
        EXPECT_NEAR(rt_vec3_x(position),
                    first_position[i][0],
                    1e-12,
                    "NavAgent3D batch X position is caller-order invariant");
        EXPECT_NEAR(rt_vec3_z(position),
                    first_position[i][1],
                    1e-12,
                    "NavAgent3D batch Z position is caller-order invariant");
        EXPECT_NEAR(rt_vec3_x(velocity),
                    first_velocity[i][0],
                    1e-12,
                    "NavAgent3D batch X velocity is caller-order invariant");
        EXPECT_NEAR(rt_vec3_z(velocity),
                    first_velocity[i][1],
                    1e-12,
                    "NavAgent3D batch Z velocity is caller-order invariant");
        EXPECT_NEAR(rt_vec3_x(desired),
                    first_desired[i][0],
                    1e-12,
                    "NavAgent3D batch desired X is caller-order invariant");
        EXPECT_NEAR(rt_vec3_z(desired),
                    first_desired[i][1],
                    1e-12,
                    "NavAgent3D batch desired Z is caller-order invariant");
    }
}

/// Funnel regression: an L-shaped corridor must string-pull a tight path that
/// hugs the inside corner and never leaves the walkable area. A wrong-side
/// left/right assignment in the portal ordering would emit a corner on the far
/// wall — visible here as excess length or an out-of-corridor sample.
static void test_navmesh_funnel_hugs_l_corridor_corner() {
    void *mesh = rt_mesh3d_new();
    /* Corridor legs (y=0, width 2): leg A x[0,2] z[0,8]; leg B x[2,8] z[6,8].
     * Shared edge (2,6)-(2,8) uses shared vertex indices for adjacency. */
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0, 1, 0, 0, 0); /* v0 */
    rt_mesh3d_add_vertex(mesh, 2.0, 0.0, 0.0, 0, 1, 0, 0, 0); /* v1 */
    rt_mesh3d_add_vertex(mesh, 2.0, 0.0, 6.0, 0, 1, 0, 0, 0); /* v2 */
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 6.0, 0, 1, 0, 0, 0); /* v3 */
    rt_mesh3d_add_vertex(mesh, 2.0, 0.0, 8.0, 0, 1, 0, 0, 0); /* v4 */
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 8.0, 0, 1, 0, 0, 0); /* v5 */
    rt_mesh3d_add_vertex(mesh, 8.0, 0.0, 6.0, 0, 1, 0, 0, 0); /* v6 */
    rt_mesh3d_add_vertex(mesh, 8.0, 0.0, 8.0, 0, 1, 0, 0, 0); /* v7 */
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    rt_mesh3d_add_triangle(mesh, 0, 2, 3);
    rt_mesh3d_add_triangle(mesh, 3, 2, 4);
    rt_mesh3d_add_triangle(mesh, 3, 4, 5);
    rt_mesh3d_add_triangle(mesh, 2, 6, 7);
    rt_mesh3d_add_triangle(mesh, 2, 7, 4);
    void *navmesh = rt_navmesh3d_build(mesh, 0.1, 1.8);
    EXPECT_TRUE(navmesh != nullptr, "L-corridor navmesh builds");
    if (!navmesh)
        return;
    void *path =
        rt_navmesh3d_find_path(navmesh, rt_vec3_new(1.0, 0.0, 1.0), rt_vec3_new(7.0, 0.0, 7.0));
    EXPECT_TRUE(path != nullptr, "L-corridor path found");
    if (!path)
        return;
    double length = rt_path3d_get_length(path);
    /* Straight-line lower bound sqrt(72)=8.49; corner-hugging optimum ~10.2;
     * a wrong-side corner or centroid chain lands well above 11.5. */
    EXPECT_TRUE(length > 8.4, "funnel path respects the corner (not a wall clip)");
    EXPECT_TRUE(length < 11.5, "funnel path stays near the corner-hugging optimum");
    for (int i = 0; i <= 32; i++) {
        double t = (double)i / 32.0;
        void *pt = rt_path3d_get_position_at(path, t);
        double x = pt ? rt_vec3_x(pt) : 0.0;
        double z = pt ? rt_vec3_z(pt) : 0.0;
        int in_leg_a = (x >= -0.15 && x <= 2.15 && z >= -0.15 && z <= 8.15);
        int in_leg_b = (x >= 1.85 && x <= 8.15 && z >= 5.85 && z <= 8.15);
        EXPECT_TRUE(in_leg_a || in_leg_b, "funnel path sample stays inside the corridor");
    }
}

int main() {
    test_navmesh_funnel_hugs_l_corridor_corner();
    test_navagent_bound_node_reaches_target_in_world_space();
    test_navagent_bound_character_moves_controller();
    test_navagent_warp_resets_motion_and_rebuilds_path();
    test_navagent_character_binding_updates_bound_node();
    test_navagent_avoidance_properties();
    test_navagent_batch_update_is_order_independent();
    test_navagent_local_avoidance_reduces_head_on_velocity();
    test_navagent_avoidance_breaks_head_on_deadlock();
    test_navagent_crowd_multiple_pairs_cross();
    test_navagent_avoidance_grid_matches_full_scan();
    test_navagent_agent_count_perf_target();
    test_navagent_rejects_wrong_handle_types();
    test_navagent_repairs_wrong_class_private_bindings();
    test_navagent_getters_sanitize_corrupt_private_state();
    test_navagent_repairs_owned_path_and_neighbor_storage();
    test_navagent_singular_parent_world_write_is_exact_or_rejected();
    test_navagent_invalid_handle_numeric_getters_return_sentinel();

    std::printf("NavAgent3D tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
