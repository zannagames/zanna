//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_physics3d.cpp
// Purpose: Unit tests for Physics3D — world step, body creation, collision
//   detection, impulse response, collision layers, character controller.
//
// Key invariants:
//   - Physics3D runtime defaults remain stable and deterministic.
//   - Contact, joint, and fixed-step behavior are validated through the C ABI.
//
// Ownership/Lifetime:
//   - Tests allocate runtime objects through GC-managed constructors.
//   - Test process lifetime owns intentionally leaked handles until exit.
//
// Links: rt_physics3d.h
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt.hpp"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_collider3d.h"
#include "rt_internal.h"
#include "rt_joints3d.h"
#include "rt_mat4.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_physics3d_internal.h"
#include "rt_pixels.h"
#include "rt_quat.h"
#include "rt_transform3d.h"
#include <cassert>
#include <chrono>
#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern void *rt_mesh3d_new(void);
extern void rt_mesh3d_add_vertex(
    void *obj, double x, double y, double z, double nx, double ny, double nz, double u, double v);
extern void rt_mesh3d_add_triangle(void *obj, int64_t i0, int64_t i1, int64_t i2);
}

static int tests_passed = 0;
static int tests_run = 0;
static const double TEST_PI = 3.14159265358979323846;

typedef struct {
    void *vptr;
    double bounds_min[3];
    double bounds_max[3];
    void **tracked_bodies;
    int8_t *was_inside;
    int8_t *is_inside;
    uint32_t *seen_stamp;
    int32_t tracked_count;
    int32_t tracked_capacity;
    uint32_t update_stamp;
    int32_t enter_count;
    int32_t exit_count;
} Trigger3DTestLayout;

typedef struct {
    void *vptr;
    void *body_a;
    void *body_b;
    double target_distance;
    void *owned_body_a;
    void *owned_body_b;
} DistanceJoint3DTestLayout;

typedef struct {
    void *vptr;
    void *body_a;
    void *body_b;
    double rest_length;
    double stiffness;
    double damping;
    void *owned_body_a;
    void *owned_body_b;
} SpringJoint3DTestLayout;

typedef struct {
    void *vptr;
    void *body_a;
    void *body_b;
    double local_anchor_a[3];
    double local_anchor_b[3];
    double local_axis_a[3];
    double ref_perp_a[3];
    double ref_perp_b[3];
    int8_t motor_enabled;
    double motor_target_velocity;
    double motor_max_impulse;
    int8_t has_limits;
    double angle_min;
    double angle_max;
    void *owned_body_a;
    void *owned_body_b;
} HingeJoint3DTestLayout;

typedef struct {
    void *vptr;
    void *body_a;
    void *body_b;
    double max_length;
    void *owned_body_a;
    void *owned_body_b;
} RopeJoint3DTestLayout;

typedef struct {
    void *vptr;
    void *body_a;
    void *body_b;
    double local_anchor_a[3];
    double local_anchor_b[3];
    double linear_min[3];
    double linear_max[3];
    double angular_min[3];
    double angular_max[3];
    double reference_relative_orientation[4];
    int8_t linear_motor_enabled;
    double linear_motor_velocity[3];
    double linear_motor_max_impulse;
    void *owned_body_a;
    void *owned_body_b;
} SixDofJoint3DTestLayout;

typedef struct {
    void *vptr;
    rt_body3d *body;
    rt_world3d *world;
    double step_height;
    double slope_limit_cos;
    int8_t is_grounded;
    int8_t was_grounded;
    int8_t is_sliding;
    int8_t collide_dynamic;
    int8_t ride_platforms;
    double push_strength;
    rt_body3d *ground_body;
    rt_body3d *pushed_body;
    rt_body3d **move_candidates;
    int32_t move_candidate_count;
    int32_t move_candidate_capacity;
    int8_t move_candidates_active;
} Character3DTestLayout;

namespace {
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    rt_abort(msg);
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

template <typename Fn> static bool expect_trap_contains(Fn &&fn, const char *needle) {
    g_last_trap = nullptr;
    g_expect_trap = true;
    if (setjmp(g_trap_jmp) == 0) {
        fn();
        g_expect_trap = false;
        return false;
    }
    g_expect_trap = false;
    return g_last_trap && (!needle || std::strstr(g_last_trap, needle) != nullptr);
}

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        const double actual_value = (double)(a);                                                   \
        const double expected_value = (double)(b);                                                 \
        tests_run++;                                                                               \
        if (!std::isfinite(actual_value) || !std::isfinite(expected_value) ||                      \
            fabs(actual_value - expected_value) > (eps)) {                                         \
            fprintf(                                                                               \
                stderr, "FAIL: %s (got %f, expected %f)\n", msg, actual_value, expected_value);    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

static double quat_rotation_component(void *q, int axis) {
    double v[3] = {rt_quat_x(q), rt_quat_y(q), rt_quat_z(q)};
    double w = rt_quat_w(q);
    double v_len;
    double angle;
    if (w < 0.0) {
        v[0] = -v[0];
        v[1] = -v[1];
        v[2] = -v[2];
        w = -w;
    }
    v_len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (v_len < 1e-12)
        return 2.0 * v[axis];
    angle = 2.0 * atan2(v_len, w);
    return v[axis] * angle / v_len;
}

typedef struct {
    double position[3];
    double rotation[4];
    double scale[3];
} ColliderChildTestLayout;

typedef struct {
    void *vptr;
    int32_t type;
    int8_t static_only;
    double half_extents[3];
    double radius;
    double height;
    void *mesh;
    float *heightfield_heights;
    uint8_t *heightfield_holes;
    int32_t heightfield_width;
    int32_t heightfield_depth;
    double heightfield_scale[3];
    double heightfield_min;
    double heightfield_max;
    void **children;
    ColliderChildTestLayout *child_transforms;
    int32_t child_count;
    int32_t child_capacity;
} Collider3DTestLayout;

typedef struct {
    double local_anchor[3];
    double radius;
    double suspension_rest;
    double stiffness;
    double damping;
    int8_t steers;
    int8_t driven;
    int8_t in_contact;
    double travel;
    double contact_load;
    double contact_point[3];
} VehicleWheelTestLayout;

typedef struct {
    void *vptr;
    void *world;
    void *chassis;
    VehicleWheelTestLayout wheels[8];
    int32_t wheel_count;
    double throttle;
    double brake;
    double steer;
    double drive_force;
    double brake_force;
    double max_steer_rad;
    double long_grip;
    double lat_grip;
    int64_t collision_mask;
} VehicleTestLayout;

static int64_t encode_height16(uint16_t value) {
    return ((int64_t)((value >> 8) & 0xFF) << 24) | ((int64_t)(value & 0xFF) << 16) | 0xFF;
}

static void *make_tetra_mesh(int sign) {
    void *mesh = rt_mesh3d_new();
    double s = sign >= 0 ? 1.0 : -1.0;
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, s, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, s, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, s, 0.0, 1.0, 0.0, 1.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 2, 1);
    rt_mesh3d_add_triangle(mesh, 0, 1, 3);
    rt_mesh3d_add_triangle(mesh, 0, 3, 2);
    rt_mesh3d_add_triangle(mesh, 1, 2, 3);
    return mesh;
}

static void *make_subdivided_plane_mesh(int segments, double size) {
    void *mesh = rt_mesh3d_new();
    double half = size * 0.5;
    for (int z = 0; z <= segments; ++z) {
        double pz = -half + size * (double)z / (double)segments;
        for (int x = 0; x <= segments; ++x) {
            double px = -half + size * (double)x / (double)segments;
            rt_mesh3d_add_vertex(mesh,
                                 px,
                                 0.0,
                                 pz,
                                 0.0,
                                 1.0,
                                 0.0,
                                 (double)x / (double)segments,
                                 (double)z / (double)segments);
        }
    }
    for (int z = 0; z < segments; ++z) {
        for (int x = 0; x < segments; ++x) {
            int i00 = z * (segments + 1) + x;
            int i10 = i00 + 1;
            int i01 = i00 + (segments + 1);
            int i11 = i01 + 1;
            rt_mesh3d_add_triangle(mesh, i00, i01, i10);
            rt_mesh3d_add_triangle(mesh, i10, i01, i11);
        }
    }
    return mesh;
}

/*==========================================================================
 * Body creation tests
 *=========================================================================*/

static void test_body_new_aabb() {
    void *b = rt_body3d_new_aabb(1.0, 2.0, 3.0, 10.0);
    EXPECT_TRUE(b != nullptr, "AABB body created");
    EXPECT_NEAR(rt_body3d_get_mass(b), 10.0, 0.01, "AABB mass = 10");
    EXPECT_NEAR(rt_body3d_get_restitution(b), 0.3, 0.01, "Default restitution = 0.3");
    EXPECT_NEAR(rt_body3d_get_friction(b), 0.5, 0.01, "Default friction = 0.5");
    EXPECT_TRUE(rt_body3d_is_static(b) == 0, "Dynamic body not static");
}

static void test_body_new_sphere() {
    void *b = rt_body3d_new_sphere(2.5, 5.0);
    EXPECT_TRUE(b != nullptr, "Sphere body created");
    EXPECT_NEAR(rt_body3d_get_mass(b), 5.0, 0.01, "Sphere mass = 5");
}

static void test_body_new_capsule() {
    void *b = rt_body3d_new_capsule(0.5, 2.0, 8.0);
    EXPECT_TRUE(b != nullptr, "Capsule body created");
    EXPECT_NEAR(rt_body3d_get_mass(b), 8.0, 0.01, "Capsule mass = 8");
}

static void test_body_static_zero_mass() {
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    EXPECT_TRUE(rt_body3d_is_static(b) != 0, "Zero-mass body is static");
}

static void test_body_new_and_set_collider() {
    void *body = rt_body3d_new(2.0);
    void *collider = rt_collider3d_new_box(1.0, 0.5, 1.0);
    EXPECT_TRUE(body != nullptr, "Generic body created");
    EXPECT_TRUE(collider != nullptr, "Collider created");
    rt_body3d_set_collider(body, collider);
    EXPECT_TRUE(rt_body3d_get_collider(body) == collider, "Collider setter stores collider");
}

static void test_wrapper_constructors_assign_colliders() {
    void *box = rt_body3d_new_aabb(1.0, 2.0, 3.0, 1.0);
    void *sphere = rt_body3d_new_sphere(1.5, 1.0);
    void *capsule = rt_body3d_new_capsule(0.5, 2.0, 1.0);
    EXPECT_TRUE(rt_collider3d_get_type(rt_body3d_get_collider(box)) == RT_COLLIDER3D_TYPE_BOX,
                "AABB wrapper assigns box collider");
    EXPECT_TRUE(rt_collider3d_get_type(rt_body3d_get_collider(sphere)) == RT_COLLIDER3D_TYPE_SPHERE,
                "Sphere wrapper assigns sphere collider");
    EXPECT_TRUE(rt_collider3d_get_type(rt_body3d_get_collider(capsule)) ==
                    RT_COLLIDER3D_TYPE_CAPSULE,
                "Capsule wrapper assigns capsule collider");
}

static void test_collider_constructors_sanitize_nonfinite_dimensions() {
    double half_extents[3];
    double mn[3], mx[3];
    void *box = rt_collider3d_new_box(NAN, -2.0, INFINITY);
    rt_collider3d_get_box_half_extents_raw(box, half_extents);
    EXPECT_NEAR(
        half_extents[0], 1.0, 0.001, "Box collider NaN half extent falls back to unit extent");
    EXPECT_NEAR(half_extents[1], 2.0, 0.001, "Box collider negative half extent becomes positive");
    EXPECT_NEAR(
        half_extents[2], 1.0, 0.001, "Box collider infinite half extent falls back to unit extent");

    void *sphere = rt_collider3d_new_sphere(NAN);
    EXPECT_NEAR(rt_collider3d_get_radius_raw(sphere),
                1.0,
                0.001,
                "Sphere collider NaN radius falls back to unit radius");

    void *capsule = rt_collider3d_new_capsule(INFINITY, NAN);
    EXPECT_NEAR(rt_collider3d_get_radius_raw(capsule),
                1.0,
                0.001,
                "Capsule infinite radius falls back to unit radius");
    EXPECT_NEAR(rt_collider3d_get_height_raw(capsule),
                2.0,
                0.001,
                "Capsule NaN height falls back to diameter");

    void *pixels = rt_pixels_new(2, 2);
    rt_pixels_set(pixels, 0, 0, encode_height16(0));
    rt_pixels_set(pixels, 1, 0, encode_height16(65535));
    rt_pixels_set(pixels, 0, 1, encode_height16(0));
    rt_pixels_set(pixels, 1, 1, encode_height16(65535));
    void *heightfield = rt_collider3d_new_heightfield(pixels, NAN, INFINITY, -2.0);
    rt_collider3d_get_local_bounds_raw(heightfield, mn, mx);
    EXPECT_NEAR(mx[0], 0.5, 0.001, "Heightfield NaN X scale falls back to unit scale");
    EXPECT_NEAR(mx[1], 1.0, 0.001, "Heightfield infinite Y scale falls back to unit scale");
    EXPECT_NEAR(mx[2], 1.0, 0.001, "Heightfield negative Z scale uses absolute value");
}

static void test_collider_getters_sanitize_corrupt_private_state() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mesh_collider = rt_collider3d_new_mesh(mesh);
    auto *mesh_view = static_cast<Collider3DTestLayout *>(mesh_collider);
    void *saved_mesh = mesh_view->mesh;
    int32_t saved_type = mesh_view->type;

    mesh_view->type = 99;
    EXPECT_TRUE(rt_collider3d_get_type(mesh_collider) == -1,
                "Collider3D type getter rejects corrupt private type tags");
    mesh_view->type = saved_type;
    mesh_view->static_only = -5;
    EXPECT_TRUE(rt_collider3d_is_static_only_raw(mesh_collider) == 1,
                "Collider3D static-only getter normalizes corrupt private flags");

    mesh_view->mesh = rt_vec3_new(0.0, 0.0, 0.0);
    EXPECT_TRUE(rt_collider3d_get_mesh_raw(mesh_collider) == nullptr,
                "Collider3D mesh getter rejects wrong-class private mesh refs");
    mesh_view->mesh = saved_mesh;

    void *compound = rt_collider3d_new_compound();
    void *child = rt_collider3d_new_box(0.5, 0.5, 0.5);
    void *xf = rt_transform3d_new();
    rt_transform3d_set_position(xf, 2.0, 0.0, 0.0);
    rt_collider3d_add_child(compound, child, xf);
    auto *compound_view = static_cast<Collider3DTestLayout *>(compound);
    int32_t saved_child_count = compound_view->child_count;
    int32_t saved_child_capacity = compound_view->child_capacity;
    void *saved_child = compound_view->children[0];
    ColliderChildTestLayout saved_transform = compound_view->child_transforms[0];

    compound_view->child_count = INT32_MAX;
    compound_view->child_capacity = 1;
    EXPECT_TRUE(rt_collider3d_get_child_count_raw(compound) == 1,
                "Collider3D child count caps corrupt private counts to capacity");
    EXPECT_TRUE(rt_collider3d_get_child_raw(compound, 1) == nullptr,
                "Collider3D child getter rejects capped-out private indices");
    compound_view->children[0] = rt_vec3_new(0.0, 0.0, 0.0);
    EXPECT_TRUE(rt_collider3d_get_child_raw(compound, 0) == nullptr,
                "Collider3D child getter rejects wrong-class child refs");
    compound_view->children[0] = saved_child;

    compound_view->child_transforms[0].rotation[0] = NAN;
    compound_view->child_transforms[0].scale[0] = NAN;
    double pos[3], rot[4], scale[3];
    rt_collider3d_get_child_transform_raw(compound, 0, pos, rot, scale);
    EXPECT_NEAR(rot[0], 0.0, 0.001, "Collider3D child transform repairs corrupt rotation x");
    EXPECT_NEAR(rot[3], 1.0, 0.001, "Collider3D child transform repairs corrupt rotation w");
    EXPECT_NEAR(scale[0], 1.0, 0.001, "Collider3D child transform repairs corrupt scale");
    compound_view->child_transforms[0] = saved_transform;

    compound_view->child_count = -4;
    EXPECT_TRUE(rt_collider3d_get_child_count_raw(compound) == 0,
                "Collider3D child count treats negative private counts as zero");
    compound_view->child_count = saved_child_count;
    compound_view->child_capacity = saved_child_capacity;

    void *pixels = rt_pixels_new(2, 2);
    rt_pixels_set(pixels, 0, 0, encode_height16(0));
    rt_pixels_set(pixels, 1, 0, encode_height16(65535));
    rt_pixels_set(pixels, 0, 1, encode_height16(0));
    rt_pixels_set(pixels, 1, 1, encode_height16(65535));
    void *heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 1.0, 1.0);
    auto *heightfield_view = static_cast<Collider3DTestLayout *>(heightfield);
    int32_t saved_width = heightfield_view->heightfield_width;
    int32_t saved_depth = heightfield_view->heightfield_depth;
    heightfield_view->heightfield_width = -1;
    int32_t width = 0, depth = 0;
    double heightfield_scale[3] = {};
    EXPECT_TRUE(
        rt_collider3d_get_heightfield_info_raw(heightfield, &width, &depth, heightfield_scale) == 0,
        "Collider3D heightfield info rejects corrupt private dimensions");
    heightfield_view->heightfield_width = saved_width;
    heightfield_view->heightfield_depth = saved_depth;
}

static void test_mesh_collider_attaches_to_static_body() {
    void *mesh = rt_mesh3d_new_box(4.0, 1.0, 4.0);
    void *mesh_collider = rt_collider3d_new_mesh(mesh);
    void *body = rt_body3d_new(0.0);
    EXPECT_TRUE(body != nullptr, "Static body for mesh collider created");
    rt_body3d_set_collider(body, mesh_collider);
    EXPECT_TRUE(rt_body3d_get_collider(body) == mesh_collider, "Static body accepts mesh collider");
}

/*==========================================================================
 * Property accessors
 *=========================================================================*/

static void test_body_position() {
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_position(b, 3.0, 5.0, 7.0);
    void *pos = rt_body3d_get_position(b);
    EXPECT_NEAR(rt_vec3_x(pos), 3.0, 0.01, "Position X = 3");
    EXPECT_NEAR(rt_vec3_y(pos), 5.0, 0.01, "Position Y = 5");
    EXPECT_NEAR(rt_vec3_z(pos), 7.0, 0.01, "Position Z = 7");
}

static void test_body_scale_affects_queries() {
    void *world = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    rt_body3d_set_scale(body, 3.0, 1.0, 1.0);
    rt_world3d_add(world, body);
    void *scale = rt_body3d_get_scale(body);
    EXPECT_NEAR(rt_vec3_x(scale), 3.0, 0.001, "Body scale X round-trips");
    EXPECT_NEAR(rt_vec3_y(scale), 1.0, 0.001, "Body scale Y round-trips");
    EXPECT_NEAR(rt_vec3_z(scale), 1.0, 0.001, "Body scale Z round-trips");

    void *center = rt_vec3_new(2.5, 0.0, 0.0);
    void *hits = rt_world3d_overlap_sphere(world, center, 0.1, -1);
    EXPECT_TRUE(hits != nullptr, "scaled body overlap returns a hit list");
    EXPECT_TRUE(rt_physics_hit_list3d_get_count(hits) == 1,
                "scaled body AABB participates in broadphase queries");
}

static void test_body_velocity() {
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_velocity(b, 1.0, 2.0, 3.0);
    void *vel = rt_body3d_get_velocity(b);
    EXPECT_NEAR(rt_vec3_x(vel), 1.0, 0.01, "Velocity X = 1");
    EXPECT_NEAR(rt_vec3_y(vel), 2.0, 0.01, "Velocity Y = 2");
    EXPECT_NEAR(rt_vec3_z(vel), 3.0, 0.01, "Velocity Z = 3");
}

static void test_body_far_origin_integrates_sub_float_delta() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *body = rt_body3d_new_aabb(0.25, 0.25, 0.25, 1.0);
    constexpr double kBase = 1000000000.0;

    rt_body3d_set_position(body, kBase, 0.0, 0.0);
    rt_body3d_set_velocity(body, 0.125, 0.0, 0.0);
    rt_world3d_add(world, body);
    rt_world3d_step(world, 1.0);

    void *pos = rt_body3d_get_position(body);
    EXPECT_NEAR(rt_vec3_x(pos),
                kBase + 0.125,
                1e-9,
                "Physics body state preserves sub-float deltas at far origin");
}

static void test_body_collision_layer_mask() {
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_collision_layer(b, 4);
    rt_body3d_set_collision_mask(b, 6);
    EXPECT_TRUE(rt_body3d_get_collision_layer(b) == 4, "Collision layer = 4");
    EXPECT_TRUE(rt_body3d_get_collision_mask(b) == 6, "Collision mask = 6");
    EXPECT_TRUE(expect_trap_contains([&] { rt_body3d_set_collision_layer(b, 0); }, "positive"),
                "Collision layer 0 traps");
    EXPECT_TRUE(expect_trap_contains([&] { rt_body3d_set_collision_layer(b, -1); }, "positive"),
                "Collision layer negative traps");
    EXPECT_TRUE(rt_body3d_get_collision_layer(b) == 4, "Invalid collision layer leaves value");
}

static void test_body_material_coefficients_are_sanitized() {
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);

    rt_body3d_set_restitution(b, -1.0);
    EXPECT_NEAR(rt_body3d_get_restitution(b), 0.0, 0.001, "Negative restitution clamps to zero");
    rt_body3d_set_restitution(b, 2.0);
    EXPECT_NEAR(rt_body3d_get_restitution(b), 1.0, 0.001, "Restitution clamps to one");
    rt_body3d_set_restitution(b, NAN);
    EXPECT_NEAR(rt_body3d_get_restitution(b), 0.0, 0.001, "NaN restitution clamps to zero");

    rt_body3d_set_friction(b, -1.0);
    EXPECT_NEAR(rt_body3d_get_friction(b), 0.0, 0.001, "Negative friction clamps to zero");
    rt_body3d_set_friction(b, NAN);
    EXPECT_NEAR(rt_body3d_get_friction(b), 0.0, 0.001, "NaN friction clamps to zero");
    rt_body3d_set_friction(b, 2.5);
    EXPECT_NEAR(rt_body3d_get_friction(b), 2.5, 0.001, "Finite friction is preserved");
}

static void test_body_getters_sanitize_corrupt_private_state() {
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d *view = static_cast<rt_body3d *>(b);
    void *saved_collider = view->collider;
    void *wrong_ref = rt_vec3_new(0.0, 0.0, 0.0);

    view->collider = wrong_ref;
    EXPECT_TRUE(rt_body3d_get_collider(b) == nullptr,
                "Body3D collider getter rejects wrong-class private refs");
    rt_body3d_set_kinematic(b, 1);
    EXPECT_TRUE(rt_body3d_is_kinematic(b) != 0,
                "Body3D kinematic setter ignores wrong-class private collider slots");
    view->collider = saved_collider;
    rt_body3d_set_kinematic(b, 0);

    view->restitution = INFINITY;
    view->friction = NAN;
    view->linear_damping = -8.0;
    view->angular_damping = INFINITY;
    view->mass = NAN;
    view->collision_layer = -4;
    EXPECT_NEAR(rt_body3d_get_restitution(b),
                1.0,
                0.001,
                "Body3D restitution getter clamps corrupt private state");
    EXPECT_NEAR(rt_body3d_get_friction(b),
                0.0,
                0.001,
                "Body3D friction getter sanitizes corrupt private state");
    EXPECT_NEAR(rt_body3d_get_linear_damping(b),
                0.0,
                0.001,
                "Body3D linear damping getter sanitizes corrupt private state");
    EXPECT_NEAR(rt_body3d_get_angular_damping(b),
                0.0,
                0.001,
                "Body3D angular damping getter sanitizes corrupt private state");
    EXPECT_NEAR(
        rt_body3d_get_mass(b), 0.0, 0.001, "Body3D mass getter sanitizes corrupt private state");
    EXPECT_TRUE(rt_body3d_get_collision_layer(b) == 1,
                "Body3D collision layer getter repairs corrupt private state");

    view->motion_mode = PH3D_MODE_DYNAMIC;
    view->is_static = 42;
    view->is_kinematic = -7;
    EXPECT_TRUE(rt_body3d_is_static(b) == 0, "Body3D static getter follows sanitized motion mode");
    EXPECT_TRUE(rt_body3d_is_kinematic(b) == 0,
                "Body3D kinematic getter follows sanitized motion mode");
    view->motion_mode = PH3D_MODE_STATIC;
    view->is_static = 0;
    EXPECT_TRUE(rt_body3d_is_static(b) != 0,
                "Body3D static getter does not trust stale private flags");
    view->motion_mode = PH3D_MODE_DYNAMIC;

    view->is_trigger = -1;
    view->can_sleep = 99;
    view->is_sleeping = -2;
    view->use_ccd = 7;
    view->is_grounded = -8;
    EXPECT_TRUE(rt_body3d_is_trigger(b) == 1, "Body3D trigger getter normalizes to 0/1");
    EXPECT_TRUE(rt_body3d_can_sleep(b) == 1, "Body3D canSleep getter normalizes to 0/1");
    EXPECT_TRUE(rt_body3d_is_sleeping(b) == 1, "Body3D sleeping getter normalizes to 0/1");
    EXPECT_TRUE(rt_body3d_get_use_ccd(b) == 1, "Body3D CCD getter normalizes to 0/1");
    EXPECT_TRUE(rt_body3d_is_grounded(b) == 1, "Body3D grounded getter normalizes to 0/1");
}

static void test_body_sanitizes_nonfinite_motion_state() {
    void *static_body = rt_body3d_new(NAN);
    EXPECT_NEAR(rt_body3d_get_mass(static_body), 0.0, 0.001, "NaN mass becomes static zero mass");
    EXPECT_TRUE(rt_body3d_is_static(static_body) != 0, "NaN mass body is static");

    void *b = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_position(b, NAN, 2.0, INFINITY);
    void *pos = rt_body3d_get_position(b);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.001, "Body non-finite position X falls back to 0");
    EXPECT_NEAR(rt_vec3_y(pos), 2.0, 0.001, "Body finite position Y is preserved");
    EXPECT_NEAR(rt_vec3_z(pos), 0.0, 0.001, "Body non-finite position Z falls back to 0");

    rt_body3d_set_velocity(b, NAN, 3.0, INFINITY);
    rt_body3d_apply_impulse(b, NAN, 2.0, INFINITY);
    void *vel = rt_body3d_get_velocity(b);
    EXPECT_NEAR(rt_vec3_x(vel), 0.0, 0.001, "Body non-finite velocity X falls back to 0");
    EXPECT_NEAR(rt_vec3_y(vel), 5.0, 0.001, "Body finite velocity and impulse are applied");
    EXPECT_NEAR(rt_vec3_z(vel), 0.0, 0.001, "Body non-finite velocity Z falls back to 0");

    rt_body3d_set_angular_velocity(b, INFINITY, 4.0, NAN);
    rt_body3d_apply_angular_impulse(b, INFINITY, 1.0, NAN);
    void *ang = rt_body3d_get_angular_velocity(b);
    EXPECT_NEAR(rt_vec3_x(ang), 0.0, 0.001, "Body non-finite angular velocity X falls back to 0");
    EXPECT_TRUE(rt_vec3_y(ang) > 4.0, "Body finite angular impulse updates Y angular velocity");
    EXPECT_NEAR(rt_vec3_z(ang), 0.0, 0.001, "Body non-finite angular velocity Z falls back to 0");

    rt_body3d_set_orientation(b, rt_quat_new(NAN, 0.0, 0.0, 0.0));
    EXPECT_NEAR(rt_quat_w(rt_body3d_get_orientation(b)),
                1.0,
                0.001,
                "Body invalid quaternion resets to identity");
    EXPECT_TRUE(expect_trap_contains([&] { rt_body3d_set_orientation(b, b); }, "Quat"),
                "Body.SetOrientation traps on non-Quat handles");
    EXPECT_NEAR(rt_quat_w(rt_body3d_get_orientation(b)), 1.0, 0.001, "Non-Quat leaves orientation");

    rt_body3d_set_linear_damping(b, INFINITY);
    rt_body3d_set_angular_damping(b, NAN);
    EXPECT_NEAR(
        rt_body3d_get_linear_damping(b), 0.0, 0.001, "Body non-finite linear damping clamps");
    EXPECT_NEAR(
        rt_body3d_get_angular_damping(b), 0.0, 0.001, "Body non-finite angular damping clamps");
}

static void test_body_extreme_motion_state_saturates() {
    const double state_max = 1000000000000.0;
    void *world = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_position(body, 1.0e300, -1.0e300, 0.0);
    void *pos = rt_body3d_get_position(body);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(pos)) && fabs(rt_vec3_x(pos)) <= state_max,
                "Extreme position X saturates to finite state bound");
    EXPECT_TRUE(std::isfinite(rt_vec3_y(pos)) && fabs(rt_vec3_y(pos)) <= state_max,
                "Extreme position Y saturates to finite state bound");

    rt_body3d_set_velocity(body, 1.0e300, 0.0, 0.0);
    rt_body3d_apply_impulse(body, 1.0e300, -1.0e300, 0.0);
    rt_body3d_apply_force(body, 1.0e300, 1.0e300, 0.0);
    rt_world3d_add(world, body);
    rt_world3d_step(world, 0.1);

    void *vel = rt_body3d_get_velocity(body);
    pos = rt_body3d_get_position(body);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(vel)) && fabs(rt_vec3_x(vel)) <= state_max,
                "Extreme velocity X remains finite after impulse/force integration");
    EXPECT_TRUE(std::isfinite(rt_vec3_y(vel)) && fabs(rt_vec3_y(vel)) <= state_max,
                "Extreme velocity Y remains finite after impulse/force integration");
    EXPECT_TRUE(std::isfinite(rt_vec3_x(pos)) && fabs(rt_vec3_x(pos)) <= state_max,
                "Integrated position X remains finite after extreme motion");
    EXPECT_TRUE(std::isfinite(rt_vec3_y(pos)) && fabs(rt_vec3_y(pos)) <= state_max,
                "Integrated position Y remains finite after extreme motion");
}

static void test_body_extreme_scale_and_offcenter_inputs_remain_finite() {
    const double state_max = 1000000000000.0;
    const double param_max = 1000000000.0;
    void *world = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);

    rt_body3d_set_scale(body, 1.0e300, NAN, 0.0);
    void *scale = rt_body3d_get_scale(body);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(scale)) && fabs(rt_vec3_x(scale)) <= param_max,
                "Extreme scale X clamps to finite parameter bound");
    EXPECT_NEAR(rt_vec3_y(scale), 1.0, 0.001, "NaN scale Y falls back to unit scale");
    EXPECT_NEAR(rt_vec3_z(scale), 1.0, 0.001, "Zero scale Z falls back to unit scale");

    rt_body3d_apply_force_at_point(body, 1.0e300, 1.0e300, 0.0, -1.0e300, 1.0e300, 0.0);
    rt_body3d_apply_impulse_at_point(body, -1.0e300, 0.0, 1.0e300, 1.0e300, 0.0, -1.0e300);
    rt_world3d_add(world, body);
    rt_world3d_step(world, 1.0 / 60.0);

    void *vel = rt_body3d_get_velocity(body);
    void *ang = rt_body3d_get_angular_velocity(body);
    void *pos = rt_body3d_get_position(body);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(vel)) && fabs(rt_vec3_x(vel)) <= state_max,
                "Extreme off-center impulse keeps velocity finite");
    EXPECT_TRUE(std::isfinite(rt_vec3_y(ang)) && fabs(rt_vec3_y(ang)) <= state_max,
                "Extreme off-center impulse keeps angular velocity finite");
    EXPECT_TRUE(std::isfinite(rt_vec3_x(pos)) && fabs(rt_vec3_x(pos)) <= state_max,
                "Extreme scaled body keeps integrated position finite");
}

static void test_world_pathological_step_and_rebase_are_bounded() {
    const double state_max = 1000000000000.0;
    void *world = rt_world3d_new(INFINITY, NAN, 0.0);
    void *body = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_velocity(body, 2.0, 0.0, 0.0);
    rt_world3d_add(world, body);

    rt_world3d_step(world, 1.0e300);
    void *pos = rt_body3d_get_position(body);
    EXPECT_NEAR(rt_vec3_x(pos), 2.0, 0.001, "Pathological Step dt is capped to one second");
    EXPECT_TRUE(std::isfinite(rt_vec3_y(pos)), "Non-finite gravity is sanitized before stepping");

    rt_world3d_rebase_origin(world, 1.0e300, -1.0e300, NAN);
    pos = rt_body3d_get_position(body);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(pos)) && fabs(rt_vec3_x(pos)) <= state_max,
                "Pathological origin rebase keeps X finite");
    EXPECT_TRUE(std::isfinite(rt_vec3_y(pos)) && fabs(rt_vec3_y(pos)) <= state_max,
                "Pathological origin rebase keeps Y finite");
}

static void test_body_trigger() {
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    EXPECT_TRUE(rt_body3d_is_trigger(b) == 0, "Default: not trigger");
    rt_body3d_set_trigger(b, 1);
    EXPECT_TRUE(rt_body3d_is_trigger(b) != 0, "Set trigger = true");
}

static void test_body_orientation_roundtrip() {
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    void *q = rt_quat_from_axis_angle(rt_vec3_new(0.0, 1.0, 0.0), TEST_PI * 0.5);
    rt_body3d_set_orientation(b, q);
    void *out = rt_body3d_get_orientation(b);
    EXPECT_NEAR(rt_quat_y(out), rt_quat_y(q), 0.001, "Orientation Y round-trips");
    EXPECT_NEAR(rt_quat_w(out), rt_quat_w(q), 0.001, "Orientation W round-trips");
}

static void test_body_torque_updates_angular_velocity_and_orientation() {
    void *w = rt_world3d_new(0, 0, 0);
    void *b = rt_body3d_new_sphere(1.0, 2.0);
    rt_world3d_add(w, b);
    rt_body3d_apply_torque(b, 0.0, 4.0, 0.0);
    rt_world3d_step(w, 1.0);

    void *ang = rt_body3d_get_angular_velocity(b);
    void *q = rt_body3d_get_orientation(b);
    EXPECT_TRUE(rt_vec3_y(ang) > 1.5, "Torque increases angular velocity");
    EXPECT_TRUE(fabs(rt_quat_w(q)) < 0.95, "Torque integration changes orientation");
}

static void test_body_angular_damping() {
    void *w = rt_world3d_new(0, 0, 0);
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_angular_velocity(b, 0.0, 4.0, 0.0);
    rt_body3d_set_angular_damping(b, 1.0);
    rt_world3d_add(w, b);

    rt_world3d_step(w, 1.0);
    {
        void *ang = rt_body3d_get_angular_velocity(b);
        EXPECT_TRUE(rt_vec3_y(ang) < 4.0, "Angular damping reduces angular velocity");
    }
}

static void test_body_sleep_and_wake() {
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    EXPECT_TRUE(rt_body3d_is_sleeping(b) == 0, "Dynamic body starts awake");
    rt_body3d_sleep(b);
    EXPECT_TRUE(rt_body3d_is_sleeping(b) != 0, "Sleep() marks body sleeping");
    rt_body3d_wake(b);
    EXPECT_TRUE(rt_body3d_is_sleeping(b) == 0, "Wake() clears sleeping state");
}

static void test_body_auto_sleep() {
    void *w = rt_world3d_new(0, 0, 0);
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    rt_world3d_add(w, b);
    for (int i = 0; i < 40; i++)
        rt_world3d_step(w, 1.0 / 60.0);
    EXPECT_TRUE(rt_body3d_is_sleeping(b) != 0, "Idle dynamic body auto-sleeps");
}

static void test_kinematic_body_ignores_gravity_but_integrates_velocity() {
    void *w = rt_world3d_new(0, -9.81, 0);
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_kinematic(b, 1);
    rt_body3d_set_velocity(b, 2.0, 0.0, 0.0);
    rt_body3d_set_angular_velocity(b, 0.0, 2.0, 0.0);
    rt_world3d_add(w, b);
    rt_world3d_step(w, 1.0);

    {
        void *pos = rt_body3d_get_position(b);
        void *q = rt_body3d_get_orientation(b);
        EXPECT_NEAR(rt_vec3_x(pos), 2.0, 0.05, "Kinematic body advances from explicit velocity");
        EXPECT_NEAR(rt_vec3_y(pos), 0.0, 0.05, "Kinematic body ignores gravity");
        EXPECT_TRUE(fabs(rt_quat_w(q)) < 0.95, "Kinematic angular velocity updates orientation");
    }
}

static void test_ccd_prevents_fast_sphere_tunneling() {
    void *w = rt_world3d_new(0, 0, 0);
    void *wall = rt_body3d_new_aabb(0.1, 2.0, 2.0, 0.0);
    void *sphere = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(wall, 5.0, 0.0, 0.0);
    rt_body3d_set_position(sphere, 0.0, 0.0, 0.0);
    rt_body3d_set_velocity(sphere, 100.0, 0.0, 0.0);
    rt_body3d_set_use_ccd(sphere, 1);
    rt_world3d_add(w, wall);
    rt_world3d_add(w, sphere);

    rt_world3d_step(w, 0.1);
    {
        void *pos = rt_body3d_get_position(sphere);
        EXPECT_TRUE(rt_vec3_x(pos) < 5.6, "CCD body does not tunnel through thin wall");
    }

    void *diag_world = rt_world3d_new(0, 0, 0);
    void *fast = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_velocity(fast, 1000.0, 0.0, 0.0);
    rt_body3d_set_use_ccd(fast, 1);
    rt_world3d_add(diag_world, fast);
    rt_world3d_step(diag_world, 0.1);
    EXPECT_TRUE(rt_world3d_get_last_ccd_requested_substeps(diag_world) >
                    rt_world3d_get_last_ccd_substeps(diag_world),
                "CCD diagnostics retain unclamped substep demand");
    EXPECT_TRUE(rt_world3d_get_last_ccd_substeps(diag_world) == PH3D_MAX_CCD_SUBSTEPS,
                "CCD diagnostics report capped substeps");
    EXPECT_TRUE(rt_world3d_get_ccd_substep_clamped_count(diag_world) == 1,
                "CCD diagnostics count clamp events");

    void *huge_world = rt_world3d_new(0, 0, 0);
    void *huge = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_velocity(huge, 1.0e300, 0.0, 0.0);
    rt_body3d_set_use_ccd(huge, 1);
    rt_world3d_add(huge_world, huge);
    rt_world3d_step(huge_world, 0.1);
    EXPECT_TRUE(rt_world3d_get_last_ccd_requested_substeps(huge_world) >=
                    rt_world3d_get_last_ccd_substeps(huge_world),
                "CCD diagnostics stay ordered for saturated extreme speeds");
    EXPECT_TRUE(rt_world3d_get_last_ccd_substeps(huge_world) == PH3D_MAX_CCD_SUBSTEPS,
                "CCD substeps remain capped for saturated extreme speeds");
    EXPECT_TRUE(rt_world3d_get_ccd_substep_clamped_count(huge_world) == 1,
                "CCD clamp diagnostics count saturated extreme speeds");
}

/// @brief Verify that a fast rotated box's complete swept volume participates in CCD.
/// @details The target lies inside the moving box's rotated Y/Z silhouette but
///   outside the old max-half-extent swept sphere. A half-diagonal proxy catches
///   the target, whereas the former proxy allowed the box corner to tunnel.
static void test_ccd_proxy_encloses_rotated_box_corners() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *target = rt_body3d_new_aabb(0.05, 0.05, 0.05, 0.0);
    void *moving = rt_body3d_new_aabb(0.25, 1.0, 1.0, 1.0);
    void *rotation = rt_quat_from_axis_angle(rt_vec3_new(1.0, 0.0, 0.0), TEST_PI * 0.25);
    rt_body3d_set_position(target, 5.0, 1.2, 0.0);
    rt_body3d_set_orientation(moving, rotation);
    rt_body3d_set_velocity(moving, 100.0, 0.0, 0.0);
    rt_body3d_set_restitution(moving, 0.0);
    rt_body3d_set_use_ccd(moving, 1);
    rt_world3d_add(world, target);
    rt_world3d_add(world, moving);

    rt_world3d_step(world, 0.1);

    EXPECT_TRUE(rt_vec3_x(rt_body3d_get_position(moving)) < 5.0,
                "CCD proxy encloses rotated box corners instead of tunneling");
    EXPECT_TRUE(static_cast<rt_world3d *>(world)->ccd_toi_count > 0,
                "Rotated box corner collision is resolved as a swept time of impact");
}

struct CcdMaterialResult {
    double normal_velocity;
    double tangent_velocity;
};

/// @brief Run one high-speed wall impact with explicit target-collider material overrides.
/// @details The moving sphere deliberately keeps identical body material in
///   every run. Only the static target collider's restitution and friction are
///   varied, proving that swept TOI contacts retain both material participants.
/// @param target_restitution Target collider restitution override in `[0, 1]`.
/// @param target_friction Target collider friction override in `[0, +inf)`.
/// @param tangent_speed Initial velocity tangent to the wall.
/// @return Post-step normal and tangent components of the moving sphere velocity.
static CcdMaterialResult run_ccd_material_impact(double target_restitution,
                                                 double target_friction,
                                                 double tangent_speed) {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *wall = rt_body3d_new_aabb(0.05, 5.0, 5.0, 0.0);
    void *sphere = rt_body3d_new_sphere(0.5, 1.0);
    void *wall_collider = rt_body3d_get_collider(wall);
    rt_body3d_set_position(wall, 5.0, 0.0, 0.0);
    rt_body3d_set_velocity(sphere, 100.0, tangent_speed, 0.0);
    rt_body3d_set_restitution(sphere, 1.0);
    rt_body3d_set_friction(sphere, 1.0);
    rt_collider3d_set_restitution(wall_collider, target_restitution);
    rt_collider3d_set_friction(wall_collider, target_friction);
    rt_body3d_set_use_ccd(sphere, 1);
    rt_world3d_add(world, wall);
    rt_world3d_add(world, sphere);

    rt_world3d_step(world, 0.1);

    void *velocity = rt_body3d_get_velocity(sphere);
    return {rt_vec3_x(velocity), rt_vec3_y(velocity)};
}

/// @brief Verify swept contacts use both colliders' restitution and friction.
/// @details Restitution uses the ordinary minimum combine and friction uses the
///   ordinary geometric-mean combine. These comparisons fail if CCD reflects
///   from only the moving body's material or preserves tangent velocity blindly.
static void test_ccd_toi_uses_target_collider_material() {
    CcdMaterialResult inelastic = run_ccd_material_impact(0.0, 0.0, 0.0);
    CcdMaterialResult elastic = run_ccd_material_impact(1.0, 0.0, 0.0);
    CcdMaterialResult frictionless = run_ccd_material_impact(0.0, 0.0, 20.0);
    CcdMaterialResult gripping = run_ccd_material_impact(0.0, 1.0, 20.0);

    EXPECT_TRUE(inelastic.normal_velocity > -5.0,
                "Zero target restitution prevents an artificial CCD rebound");
    EXPECT_TRUE(elastic.normal_velocity < -50.0,
                "Unit target restitution produces the ordinary elastic CCD rebound");
    EXPECT_TRUE(fabs(gripping.tangent_velocity) + 1.0 < fabs(frictionless.tangent_velocity),
                "Target collider friction reduces tangent speed during the CCD impact");
}

/// @brief Verify one fast CCD body does not globally subdivide unrelated integration.
/// @details Two worlds integrate identical slow bodies for the same frame; one
///   world also contains a distant high-speed CCD sphere. The slow trajectory is
///   bit-identical and both worlds report one integration pass, while only the
///   mixed world reports multiple local CCD segments.
static void test_ccd_segmentation_is_body_local() {
    void *baseline_world = rt_world3d_new(0.0, -9.81, 0.0);
    void *mixed_world = rt_world3d_new(0.0, -9.81, 0.0);
    void *baseline_slow = rt_body3d_new_sphere(0.5, 1.0);
    void *mixed_slow = rt_body3d_new_sphere(0.5, 1.0);
    void *bullet = rt_body3d_new_sphere(0.25, 1.0);
    rt_body3d_set_position(baseline_slow, 0.0, 10.0, 0.0);
    rt_body3d_set_position(mixed_slow, 0.0, 10.0, 0.0);
    rt_body3d_set_velocity(baseline_slow, 1.25, 0.0, -0.5);
    rt_body3d_set_velocity(mixed_slow, 1.25, 0.0, -0.5);
    rt_body3d_set_linear_damping(baseline_slow, 0.7);
    rt_body3d_set_linear_damping(mixed_slow, 0.7);
    rt_body3d_set_position(bullet, 0.0, 1000.0, 1000.0);
    rt_body3d_set_velocity(bullet, 1000.0, 0.0, 0.0);
    rt_body3d_set_use_ccd(bullet, 1);
    rt_world3d_add(baseline_world, baseline_slow);
    rt_world3d_add(mixed_world, mixed_slow);
    rt_world3d_add(mixed_world, bullet);

    rt_world3d_step(baseline_world, 0.1);
    rt_world3d_step(mixed_world, 0.1);

    void *baseline_position = rt_body3d_get_position(baseline_slow);
    void *mixed_position = rt_body3d_get_position(mixed_slow);
    void *baseline_velocity = rt_body3d_get_velocity(baseline_slow);
    void *mixed_velocity = rt_body3d_get_velocity(mixed_slow);
    EXPECT_NEAR(rt_vec3_x(mixed_position),
                rt_vec3_x(baseline_position),
                0.0,
                "A distant CCD bullet does not alter slow-body X integration");
    EXPECT_NEAR(rt_vec3_y(mixed_position),
                rt_vec3_y(baseline_position),
                0.0,
                "A distant CCD bullet does not alter slow-body Y integration");
    EXPECT_NEAR(rt_vec3_z(mixed_position),
                rt_vec3_z(baseline_position),
                0.0,
                "A distant CCD bullet does not alter slow-body Z integration");
    EXPECT_NEAR(rt_vec3_x(mixed_velocity),
                rt_vec3_x(baseline_velocity),
                0.0,
                "A distant CCD bullet does not alter slow-body damping");
    EXPECT_TRUE(static_cast<rt_world3d *>(baseline_world)->last_integration_pass_count == 1 &&
                    static_cast<rt_world3d *>(mixed_world)->last_integration_pass_count == 1,
                "CCD segmentation never multiplies the world integration pass");
    EXPECT_TRUE(rt_world3d_get_last_ccd_substeps(mixed_world) > 1,
                "Fast CCD motion is still divided into body-local segments");
}

/*==========================================================================
 * World tests
 *=========================================================================*/

static void test_world_create() {
    void *w = rt_world3d_new(0, -9.81, 0);
    EXPECT_TRUE(w != nullptr, "World created");
    EXPECT_TRUE(rt_world3d_body_count(w) == 0, "World starts empty");
}

static void test_world_add_remove() {
    void *w = rt_world3d_new(0, -9.81, 0);
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    EXPECT_TRUE(rt_world3d_try_add(w, b) != 0, "TryAdd returns true for a valid body");
    EXPECT_TRUE(rt_world3d_body_count(w) == 1, "Body count = 1 after add");
    EXPECT_TRUE(rt_world3d_contains_body(w, b) != 0, "World contains added body");
    EXPECT_TRUE(rt_world3d_try_add(w, b) != 0, "TryAdd returns true for an existing body");
    EXPECT_TRUE(rt_world3d_body_count(w) == 1, "TryAdd keeps duplicate body count stable");
    EXPECT_TRUE(rt_world3d_try_add(w, w) == 0, "TryAdd returns false for invalid body handles");
    rt_world3d_remove(w, b);
    EXPECT_TRUE(rt_world3d_body_count(w) == 0, "Body count = 0 after remove");
    EXPECT_TRUE(rt_world3d_contains_body(w, b) == 0, "World no longer contains removed body");
}

static void test_world_remove_purges_contacts_for_body() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 0.5, 0.0, 0.0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_event_count(world) == 1,
                "World records contact before removal");

    rt_world3d_remove(world, b);
    EXPECT_TRUE(rt_world3d_get_collision_event_count(world) == 0,
                "World removal purges contacts mentioning the removed body");
    EXPECT_TRUE(rt_world3d_get_enter_event_count(world) == 0,
                "World removal purges enter events mentioning the removed body");
}

static void test_world_rejects_duplicate_body_adds() {
    void *w = rt_world3d_new(0, 0, 0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    rt_world3d_add(w, b);
    rt_world3d_add(w, b);
    EXPECT_TRUE(rt_world3d_body_count(w) == 1, "World ignores duplicate body registration");

    rt_world3d_step(w, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_count(w) == 0,
                "Duplicate body registration cannot self-collide");
}

static void test_world_body_storage_grows_past_initial_capacity() {
    void *w = rt_world3d_new(0, 0, 0);
    for (int i = 0; i < 300; i++) {
        void *b = rt_body3d_new_sphere(0.25, 0.0);
        rt_body3d_set_position(b, (double)i * 2.0, 0.0, 0.0);
        rt_world3d_add(w, b);
    }
    EXPECT_TRUE(rt_world3d_body_count(w) == 300, "World body storage grows past 256 bodies");
}

static void test_world_sparse_body_count_step_stress() {
    void *w = rt_world3d_new(0, 0, 0);
    for (int i = 0; i < 320; i++) {
        void *b = rt_body3d_new_sphere(0.25, 0.0);
        double x = (double)(i % 40) * 3.0;
        double z = (double)(i / 40) * 3.0;
        rt_body3d_set_position(b, x, 0.0, z);
        rt_world3d_add(w, b);
    }

    void *mover = rt_body3d_new_sphere(0.2, 1.0);
    rt_body3d_set_can_sleep(mover, 0);
    rt_body3d_set_position(mover, -10.0, 0.0, -10.0);
    rt_body3d_set_velocity(mover, 2.0, 0.0, 0.0);
    rt_world3d_add(w, mover);

    for (int i = 0; i < 16; i++)
        rt_world3d_step(w, 1.0 / 60.0);

    void *pos = rt_body3d_get_position(mover);
    EXPECT_TRUE(rt_world3d_body_count(w) == 321,
                "World step stress keeps every sparse body registered");
    EXPECT_TRUE(rt_world3d_get_collision_count(w) == 0,
                "World step stress keeps sparse bodies non-overlapping");
    EXPECT_TRUE(rt_vec3_x(pos) > -9.5, "World step stress integrates the dynamic body");
}

static void test_world_contact_storage_grows_past_initial_capacity() {
    void *w = rt_world3d_new(0, 0, 0);
    void *volume = rt_body3d_new_aabb(1000.0, 1000.0, 1000.0, 0.0);
    rt_body3d_set_trigger(volume, 1);
    rt_world3d_add(w, volume);
    for (int i = 0; i < 160; i++) {
        void *b = rt_body3d_new_sphere(0.25, 1.0);
        rt_body3d_set_position(b, -240.0 + (double)i * 3.0, 0.0, 0.0);
        rt_body3d_set_trigger(b, 1);
        rt_world3d_add(w, b);
    }
    rt_world3d_step(w, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_count(w) >= 160,
                "World contact storage grows past 128 contacts");
    EXPECT_TRUE(rt_world3d_get_enter_event_count(w) >= 160,
                "World enter-event storage grows with contacts");
}

static void test_world_broadphase_rejects_separated_bodies() {
    void *w = rt_world3d_new(0, 0, 0);
    for (int i = 0; i < 300; i++) {
        void *b = rt_body3d_new_sphere(0.25, 1.0);
        rt_body3d_set_position(b, (double)i * 4.0, 0.0, 0.0);
        rt_body3d_set_trigger(b, 1);
        rt_world3d_add(w, b);
    }
    rt_world3d_step(w, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_count(w) == 0,
                "Sweep-and-prune broadphase keeps separated bodies out of narrowphase");
}

static void test_world_broadphase_keeps_legacy_cached_primitives() {
    void *w = rt_world3d_new(0, 0, 0);
    rt_body3d *a = (rt_body3d *)rt_body3d_new(1.0);
    rt_body3d *b = (rt_body3d *)rt_body3d_new(1.0);
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    void *hits;

    a->shape = PH3D_SHAPE_AABB;
    b->shape = PH3D_SHAPE_AABB;
    a->half_extents[0] = a->half_extents[1] = a->half_extents[2] = 1.0;
    b->half_extents[0] = b->half_extents[1] = b->half_extents[2] = 1.0;
    rt_body3d_set_position(b, 1.5, 0.0, 0.0);
    body3d_touch_broadphase(a);
    body3d_touch_broadphase(b);

    rt_world3d_add(w, a);
    rt_world3d_add(w, b);
    rt_world3d_step(w, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_count(w) == 1,
                "Legacy cached primitive bodies participate in SAP collision detection");

    hits = rt_world3d_overlap_sphere(w, center, 2.0, -1);
    EXPECT_TRUE(rt_physics_hit_list3d_get_total_count(hits) >= 1,
                "Legacy cached primitive bodies participate in cached overlap queries");
}

static void test_gravity_integration() {
    void *w = rt_world3d_new(0, -10.0, 0);
    void *b = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    rt_body3d_set_position(b, 0, 10, 0);
    rt_world3d_add(w, b);

    /* Step 1 second: velocity += gravity*dt = -10, pos += velocity*dt = 10 + (-10)*1 = 0 */
    rt_world3d_step(w, 1.0);

    void *vel = rt_body3d_get_velocity(b);
    EXPECT_NEAR(rt_vec3_y(vel), -10.0, 0.1, "After 1s: velocity Y = -10");

    void *pos = rt_body3d_get_position(b);
    EXPECT_NEAR(rt_vec3_y(pos), 0.0, 0.1, "After 1s: position Y = 0");
}

static void test_force_application() {
    void *w = rt_world3d_new(0, 0, 0); /* no gravity */
    void *b = rt_body3d_new_aabb(0.5, 0.5, 0.5, 2.0);
    rt_body3d_set_position(b, 0, 0, 0);
    rt_world3d_add(w, b);

    /* Apply force of 10N in X. a = F/m = 5 m/s². Over 1s: v = 5, x = 5 */
    rt_body3d_apply_force(b, 10.0, 0, 0);
    rt_world3d_step(w, 1.0);

    void *vel = rt_body3d_get_velocity(b);
    EXPECT_NEAR(rt_vec3_x(vel), 5.0, 0.01, "Force: velocity X = F/m = 5");

    void *pos = rt_body3d_get_position(b);
    EXPECT_NEAR(rt_vec3_x(pos), 5.0, 0.01, "Force: position X = 5");
}

static void test_impulse_application() {
    void *b = rt_body3d_new_aabb(0.5, 0.5, 0.5, 2.0);
    rt_body3d_apply_impulse(b, 10.0, 0, 0);
    void *vel = rt_body3d_get_velocity(b);
    /* impulse changes velocity by impulse * inv_mass = 10 * 0.5 = 5 */
    EXPECT_NEAR(rt_vec3_x(vel), 5.0, 0.01, "Impulse: velocity X = impulse/mass = 5");
}

static void test_impulse_at_point_adds_spin() {
    void *b = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    rt_body3d_set_position(b, 0.0, 0.0, 0.0);
    /* +X impulse applied at a point offset +Z from the center: linear +X plus a
     * torque about Y (r x impulse) — an off-center hit spins the body. */
    rt_body3d_apply_impulse_at_point(b, 1.0, 0.0, 0.0, 0.0, 0.0, 0.5);
    void *vel = rt_body3d_get_velocity(b);
    void *ang = rt_body3d_get_angular_velocity(b);
    EXPECT_NEAR(rt_vec3_x(vel), 1.0, 0.05, "Off-center impulse still adds linear velocity");
    EXPECT_TRUE(fabs(rt_vec3_y(ang)) > 0.2, "Off-center impulse adds angular velocity (spin)");
    /* A central impulse adds no spin, by contrast. */
    void *b2 = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    rt_body3d_apply_impulse_at_point(b2, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    void *ang2 = rt_body3d_get_angular_velocity(b2);
    EXPECT_NEAR(rt_vec3_y(ang2), 0.0, 0.001, "Central impulse adds no spin");
}

static void test_force_at_point_adds_spin() {
    void *w = rt_world3d_new(0, 0, 0);
    void *box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    rt_body3d_set_position(box, 0.0, 0.0, 0.0);
    rt_world3d_add(w, box);
    /* Force is cleared each step, so re-apply before each. +X force at a +Z point
     * builds linear +X velocity plus a spin about Y. */
    for (int i = 0; i < 5; i++) {
        rt_body3d_apply_force_at_point(box, 1.0, 0.0, 0.0, 0.0, 0.0, 0.5);
        rt_world3d_step(w, 1.0 / 60.0);
    }
    void *vel = rt_body3d_get_velocity(box);
    void *ang = rt_body3d_get_angular_velocity(box);
    EXPECT_TRUE(rt_vec3_x(vel) > 0.01, "Off-center force builds linear velocity");
    EXPECT_TRUE(fabs(rt_vec3_y(ang)) > 0.01, "Off-center force builds angular velocity (spin)");
}

/// @brief Verify public angular impulses and accumulated torques both use world inertia.
/// @details A +90-degree Z rotation maps world X onto the box's local Y principal
///   axis. For half-extents `(1, 2, 3)` and mass 2, `I_y^-1` is exactly 0.15;
///   the old component-wise world multiplication incorrectly used `I_x^-1`.
static void test_rotated_anisotropic_body_uses_world_inverse_inertia() {
    constexpr double kQuarterTurn = 0.70710678118654752440;
    void *impulse_body = rt_body3d_new_aabb(1.0, 2.0, 3.0, 2.0);
    rt_body3d_set_orientation(impulse_body, rt_quat_new(0.0, 0.0, kQuarterTurn, kQuarterTurn));
    rt_body3d_apply_angular_impulse(impulse_body, 1.0, 0.0, 0.0);
    void *impulse_omega = rt_body3d_get_angular_velocity(impulse_body);
    EXPECT_NEAR(rt_vec3_x(impulse_omega),
                0.15,
                1e-10,
                "Rotated anisotropic angular impulse uses local-Y inverse inertia in world X");
    EXPECT_NEAR(rt_vec3_y(impulse_omega),
                0.0,
                1e-10,
                "Rotated angular impulse does not leak onto the orthogonal world axis");

    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *torque_body = rt_body3d_new_aabb(1.0, 2.0, 3.0, 2.0);
    rt_body3d_set_orientation(torque_body, rt_quat_new(0.0, 0.0, kQuarterTurn, kQuarterTurn));
    rt_body3d_set_angular_damping(torque_body, 0.0);
    rt_world3d_add(world, torque_body);
    rt_body3d_apply_torque(torque_body, 1.0, 0.0, 0.0);
    rt_world3d_step(world, 0.25);
    void *torque_omega = rt_body3d_get_angular_velocity(torque_body);
    EXPECT_NEAR(rt_vec3_x(torque_omega),
                0.15 * 0.25,
                1e-10,
                "Rotated anisotropic torque uses the shared world inverse-inertia transform");
}

/// @brief Verify exponential linear and angular damping are invariant to step partitioning.
static void test_exponential_damping_is_substep_partition_invariant() {
    void *single_world = rt_world3d_new(0.0, 0.0, 0.0);
    void *split_world = rt_world3d_new(0.0, 0.0, 0.0);
    void *single = rt_body3d_new_sphere(0.5, 1.0);
    void *split = rt_body3d_new_sphere(0.5, 1.0);
    void *bodies[] = {single, split};
    for (void *body : bodies) {
        rt_body3d_set_velocity(body, 8.0, -4.0, 2.0);
        rt_body3d_set_angular_velocity(body, 3.0, -2.0, 1.0);
        rt_body3d_set_linear_damping(body, 2.0);
        rt_body3d_set_angular_damping(body, 3.0);
        rt_body3d_set_can_sleep(body, 0);
    }
    rt_world3d_add(single_world, single);
    rt_world3d_add(split_world, split);

    rt_world3d_step(single_world, 0.5);
    for (int i = 0; i < 10; ++i)
        rt_world3d_step(split_world, 0.05);

    void *single_velocity = rt_body3d_get_velocity(single);
    void *split_velocity = rt_body3d_get_velocity(split);
    void *single_angular = rt_body3d_get_angular_velocity(single);
    void *split_angular = rt_body3d_get_angular_velocity(split);
    EXPECT_NEAR(rt_vec3_x(single_velocity),
                rt_vec3_x(split_velocity),
                1e-10,
                "Linear exponential damping matches across equal elapsed time");
    EXPECT_NEAR(rt_vec3_y(single_velocity),
                rt_vec3_y(split_velocity),
                1e-10,
                "Linear exponential damping is component-independent");
    EXPECT_NEAR(rt_vec3_x(single_angular),
                rt_vec3_x(split_angular),
                1e-10,
                "Angular exponential damping matches across equal elapsed time");
    EXPECT_NEAR(rt_vec3_z(single_angular),
                rt_vec3_z(split_angular),
                1e-10,
                "Angular exponential damping is component-independent");
}

/*==========================================================================
 * Collision tests
 *=========================================================================*/

static void test_collision_aabb_overlap() {
    void *w = rt_world3d_new(0, 0, 0); /* no gravity */
    void *a = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_position(a, 0, 0, 0);
    rt_body3d_set_position(b, 1.5, 0, 0); /* overlapping by 0.5 in X */

    rt_world3d_add(w, a);
    rt_world3d_add(w, b);
    rt_world3d_step(w, 0.016);

    /* After collision resolution, bodies should be pushed apart */
    void *pa = rt_body3d_get_position(a);
    void *pb = rt_body3d_get_position(b);
    double gap = rt_vec3_x(pb) - rt_vec3_x(pa);
    EXPECT_TRUE(gap > 1.5, "AABB collision pushes bodies apart");
}

static void test_collision_layer_filtering() {
    void *w = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_position(a, 0, 0, 0);
    rt_body3d_set_position(b, 1.5, 0, 0);

    /* Put them on different layers that don't match each other's masks */
    rt_body3d_set_collision_layer(a, 1);
    rt_body3d_set_collision_mask(a, 1); /* a only collides with layer 1 */
    rt_body3d_set_collision_layer(b, 2);
    rt_body3d_set_collision_mask(b, 2); /* b only collides with layer 2 */

    rt_world3d_add(w, a);
    rt_world3d_add(w, b);
    rt_world3d_step(w, 0.016);

    /* Bodies should NOT be pushed apart — layers don't match */
    void *pa = rt_body3d_get_position(a);
    void *pb = rt_body3d_get_position(b);
    double gap = rt_vec3_x(pb) - rt_vec3_x(pa);
    EXPECT_NEAR(gap, 1.5, 0.01, "Filtered layers: no collision push");
}

static void test_trigger_no_push() {
    void *w = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_position(a, 0, 0, 0);
    rt_body3d_set_position(b, 1.5, 0, 0);
    rt_body3d_set_trigger(a, 1); /* a is a trigger — overlap only */

    rt_world3d_add(w, a);
    rt_world3d_add(w, b);
    rt_world3d_step(w, 0.016);

    void *pa = rt_body3d_get_position(a);
    void *pb = rt_body3d_get_position(b);
    double gap = rt_vec3_x(pb) - rt_vec3_x(pa);
    EXPECT_NEAR(gap, 1.5, 0.01, "Trigger body: no collision push");
}

static void test_ground_detection() {
    void *w = rt_world3d_new(0, -10.0, 0);
    /* Floor: static AABB at y=0, half-extents (10,0.5,10) */
    void *floor = rt_body3d_new_aabb(10.0, 0.5, 10.0, 0.0);
    rt_body3d_set_position(floor, 0, -0.5, 0); /* top surface at y=0 */

    /* Dynamic box sitting on top of floor */
    void *box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    rt_body3d_set_position(box, 0, 0.5, 0); /* bottom at y=0, touching floor top */

    rt_world3d_add(w, floor);
    rt_world3d_add(w, box);

    /* Step a few frames to let gravity pull box down and collision resolve */
    for (int i = 0; i < 10; i++)
        rt_world3d_step(w, 0.016);

    EXPECT_TRUE(rt_body3d_is_grounded(box) != 0, "Box on floor is grounded");
}

/* Drop a stack of unit boxes onto a static floor and settle it for 600 fixed
 * steps. Writes each box's final center Y into out_y[] and its position drift
 * over the last 20 steps into out_drift[]. Exercises the warm-started
 * sequential-impulse solver's resting stability (AC-008). */
static void run_box_stack(int count, double *out_y, double *out_drift) {
    void *w = rt_world3d_new(0.0, -9.8, 0.0);
    /* Stacking is the canonical hard case for sequential-impulse solvers:
     * support must propagate up the stack, one contact per Gauss-Seidel sweep.
     * Games tune SolverIterations for stacks; use a realistic stacking budget. */
    rt_world3d_set_solver_iterations(w, 20);
    void *floor = rt_body3d_new_aabb(20.0, 0.5, 20.0, 0.0);
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0); /* top surface at y=0 */
    rt_world3d_add(w, floor);

    void *boxes[16];
    for (int i = 0; i < count; ++i) {
        boxes[i] = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0); /* unit cube, mass 1 */
        /* Spawn the stack in resting contact (box i bottom on box i-1 top). */
        rt_body3d_set_position(boxes[i], 0.0, 0.5 + (double)i, 0.0);
        rt_world3d_add(w, boxes[i]);
    }

    double mid_y[16];
    for (int step = 0; step < 600; ++step) {
        rt_world3d_step(w, 1.0 / 60.0);
        if (step == 579) {
            for (int i = 0; i < count; ++i)
                mid_y[i] = rt_vec3_y(rt_body3d_get_position(boxes[i]));
        }
    }
    for (int i = 0; i < count; ++i) {
        out_y[i] = rt_vec3_y(rt_body3d_get_position(boxes[i]));
        out_drift[i] = fabs(out_y[i] - mid_y[i]);
    }
}

static void test_box_stack_rests_stably() {
    const int count = 5;
    double y1[5];
    double drift1[5];
    double y2[5];
    double drift2[5];
    run_box_stack(count, y1, drift1);
    run_box_stack(count, y2, drift2); /* identical second run for determinism */

    for (int i = 0; i < count; ++i) {
        double expected = 0.5 + (double)i; /* touching rest heights: 0.5, 1.5, ... */
        /* Rests near the ideal touching height — not fallen through, not
         * exploded. The tolerance absorbs the small per-contact penetration slop
         * accumulating down the loaded stack. */
        EXPECT_NEAR(y1[i], expected, 0.15, "Box stack: box rests near its touching height");
        /* Settled: negligible drift over the final 20 steps (no sink / no jitter). */
        EXPECT_TRUE(drift1[i] < 0.01, "Box stack: box has settled to rest (no drift)");
        /* Determinism: an identical run reproduces the resting state exactly. */
        EXPECT_NEAR(y1[i], y2[i], 1e-9, "Box stack: resting state is deterministic");
    }
    /* Stack stays ordered and separated (no box sinks through its neighbour). */
    for (int i = 1; i < count; ++i)
        EXPECT_TRUE(y1[i] > y1[i - 1] + 0.85, "Box stack: boxes stay separated and ordered");
}

static double vec3_handle_len(void *v) {
    double x = rt_vec3_x(v);
    double y = rt_vec3_y(v);
    double z = rt_vec3_z(v);
    return sqrt(x * x + y * y + z * z);
}

static void test_ten_box_tower_stacking_regression() {
    const int count = 10;
    const double fixed_dt = 1.0 / 60.0;
    void *world = rt_world3d_new(0.0, -9.8, 0.0);
    void *floor = rt_body3d_new_aabb(20.0, 0.5, 20.0, 0.0);
    void *boxes[count];
    double start_x[count];
    double start_z[count];
    int64_t fixed_steps = 0;

    /* A ten-box tower is a high-stack case; cover the new per-world solver
     * budget without changing the default tuning for simpler scenes. */
    rt_world3d_set_solver_iterations(world, 32);
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0);
    rt_world3d_add(world, floor);
    for (int i = 0; i < count; ++i) {
        boxes[i] = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
        start_x[i] = 0.0;
        start_z[i] = 0.0;
        rt_body3d_set_position(boxes[i], start_x[i], 0.5 + (double)i, start_z[i]);
        rt_world3d_add(world, boxes[i]);
    }

    for (int step = 0; step < 600; ++step)
        fixed_steps += rt_world3d_step_fixed(world, fixed_dt, fixed_dt, 1);
    EXPECT_TRUE(fixed_steps == 600, "10-box tower: StepFixed runs every fixed step");

    for (int i = 0; i < count; ++i) {
        void *pos = rt_body3d_get_position(boxes[i]);
        void *vel = rt_body3d_get_velocity(boxes[i]);
        void *ang = rt_body3d_get_angular_velocity(boxes[i]);
        double dx = rt_vec3_x(pos) - start_x[i];
        double dz = rt_vec3_z(pos) - start_z[i];
        double horizontal_drift = sqrt(dx * dx + dz * dz);
        double linear_speed = vec3_handle_len(vel);
        double angular_speed = vec3_handle_len(ang);
        int sleeping_or_slow =
            rt_body3d_is_sleeping(boxes[i]) != 0 || (linear_speed <= 0.05 && angular_speed <= 0.05);
        EXPECT_TRUE(horizontal_drift <= 0.1,
                    "10-box tower: box stays within 0.1m horizontal drift");
        EXPECT_TRUE(sleeping_or_slow, "10-box tower: box is sleeping or low velocity");
    }
}

static void test_world_solver_island_batches_resting_pile_target() {
    const int piles = 32;
    const int height = 8;
    void *tops[piles];
    void *world = rt_world3d_new(0.0, -9.8, 0.0);
    void *floor = rt_body3d_new_aabb(64.0, 0.5, 32.0, 0.0);
    rt_world3d_set_solver_iterations(world, 20);
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0);
    rt_world3d_add(world, floor);

    for (int pile = 0; pile < piles; ++pile) {
        double x = (double)(pile % 8) * 3.0;
        double z = (double)(pile / 8) * 3.0;
        for (int level = 0; level < height; ++level) {
            void *box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
            rt_body3d_set_position(box, x, 0.5 + (double)level, z);
            rt_world3d_add(world, box);
            if (level == height - 1)
                tops[pile] = box;
        }
    }

    auto first_begin = std::chrono::steady_clock::now();
    rt_world3d_step(world, 1.0 / 60.0);
    auto first_end = std::chrono::steady_clock::now();
    int64_t first_islands = rt_world3d_get_last_solver_island_count(world);
    int64_t first_active = rt_world3d_get_last_solver_active_body_count(world);
    int64_t first_contacts = rt_world3d_get_last_solver_contact_count(world);

    EXPECT_TRUE(rt_world3d_body_count(world) == 1 + piles * height,
                "Solver island target: fixture keeps the expected body count");
    EXPECT_TRUE(first_islands == piles,
                "Solver island target: separated resting piles batch as independent islands");
    EXPECT_TRUE(first_active == piles * height,
                "Solver island target: all dynamic pile bodies are scheduled");
    EXPECT_TRUE(first_contacts >= piles * height,
                "Solver island target: floor and stacked-box contacts are scheduled");

    auto settle_begin = std::chrono::steady_clock::now();
    for (int step = 0; step < 120; ++step)
        rt_world3d_step(world, 1.0 / 60.0);
    auto settle_end = std::chrono::steady_clock::now();

    double min_top_y = 1e300;
    for (int pile = 0; pile < piles; ++pile) {
        double y = rt_vec3_y(rt_body3d_get_position(tops[pile]));
        if (y < min_top_y)
            min_top_y = y;
    }
    EXPECT_TRUE(min_top_y > (double)height - 1.25,
                "Solver island target: 256-body resting pile does not collapse");

    long long first_us =
        (long long)std::chrono::duration_cast<std::chrono::microseconds>(first_end - first_begin)
            .count();
    long long settle_us =
        (long long)std::chrono::duration_cast<std::chrono::microseconds>(settle_end - settle_begin)
            .count();
    std::printf("PHYSICS_ISLAND_BATCH_TARGET: bodies=%d piles=%d height=%d islands=%lld "
                "active_bodies=%lld contacts=%lld first_step_us=%lld settle_steps=120 "
                "settle_us=%lld min_top_y=%.3f\n",
                1 + piles * height,
                piles,
                height,
                (long long)first_islands,
                (long long)first_active,
                (long long)first_contacts,
                first_us,
                settle_us,
                min_top_y);
}

/*==========================================================================
 * Character controller tests
 *=========================================================================*/

static void test_character_create() {
    void *c = rt_character3d_new(0.5, 2.0, 80.0);
    EXPECT_TRUE(c != nullptr, "Character controller created");
    EXPECT_NEAR(rt_character3d_get_step_height(c), 0.3, 0.01, "Default step height = 0.3");
}

static void test_character_position() {
    void *c = rt_character3d_new(0.5, 2.0, 80.0);
    rt_character3d_set_position(c, 1.0, 2.0, 3.0);
    void *pos = rt_character3d_get_position(c);
    EXPECT_NEAR(rt_vec3_x(pos), 1.0, 0.01, "Character pos X = 1");
    EXPECT_NEAR(rt_vec3_y(pos), 2.0, 0.01, "Character pos Y = 2");
    EXPECT_NEAR(rt_vec3_z(pos), 3.0, 0.01, "Character pos Z = 3");
}

static void test_character_step_height() {
    void *c = rt_character3d_new(0.5, 2.0, 80.0);
    rt_character3d_set_step_height(c, 0.5);
    EXPECT_NEAR(rt_character3d_get_step_height(c), 0.5, 0.01, "Step height set to 0.5");
}

static void test_character_sanitizes_motion_config() {
    void *c = rt_character3d_new(0.5, 2.0, 80.0);
    rt_character3d_set_step_height(c, NAN);
    EXPECT_NEAR(rt_character3d_get_step_height(c), 0.0, 0.001, "NaN step height clamps to zero");
    rt_character3d_set_step_height(c, -1.0);
    EXPECT_NEAR(
        rt_character3d_get_step_height(c), 0.0, 0.001, "Negative step height clamps to zero");
    rt_character3d_set_step_height(c, 1.0e300);
    EXPECT_TRUE(rt_character3d_get_step_height(c) <= 100.0,
                "Extreme finite step height clamps to controller bound");

    rt_character3d_set_position(c, 1.0, 2.0, 3.0);
    rt_character3d_move(c, rt_vec3_new(NAN, 8.0, INFINITY), 1.0);
    void *pos = rt_character3d_get_position(c);
    EXPECT_NEAR(rt_vec3_x(pos), 1.0, 0.001, "invalid velocity leaves Character3D X unchanged");
    EXPECT_NEAR(rt_vec3_y(pos), 2.0, 0.001, "invalid velocity rejects finite sibling lanes");
    EXPECT_NEAR(rt_vec3_z(pos), 3.0, 0.001, "invalid velocity leaves Character3D Z unchanged");
    rt_character3d_move(c, rt_vec3_new(1.0, 0.0, 0.0), NAN);
    pos = rt_character3d_get_position(c);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(pos)) && std::isfinite(rt_vec3_y(pos)) &&
                    std::isfinite(rt_vec3_z(pos)),
                "Character move ignores non-finite dt");
    double before_x = rt_vec3_x(pos);
    rt_character3d_move(c, c, 1.0);
    pos = rt_character3d_get_position(c);
    EXPECT_NEAR(rt_vec3_x(pos), before_x, 0.001, "Character3D.Move rejects non-Vec3 handles");

    rt_character3d_move(c, rt_vec3_new(1.0e300, 1.0e300, -1.0e300), 1.0e300);
    pos = rt_character3d_get_position(c);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(pos)) && std::isfinite(rt_vec3_y(pos)) &&
                    std::isfinite(rt_vec3_z(pos)),
                "Character move clamps extreme finite velocity and dt");
}

static void test_character_world_binding() {
    void *w = rt_world3d_new(0, -9.81, 0);
    void *c = rt_character3d_new(0.5, 2.0, 80.0);
    rt_character3d_set_world(c, w);
    EXPECT_TRUE(rt_character3d_get_world(c) == w, "Character world getter returns bound world");
    rt_character3d_set_world(c, NULL);
    EXPECT_TRUE(rt_character3d_get_world(c) == nullptr, "Character world can be cleared");
}

static void test_character_repairs_private_refs_and_clears_stale_grounding() {
    void *world_a = rt_world3d_new(0, 0, 0);
    void *world_b = rt_world3d_new(0, 0, 0);
    void *floor = rt_body3d_new_aabb(10.0, 0.5, 10.0, 0.0);
    void *character = rt_character3d_new(0.5, 2.0, 80.0);
    Character3DTestLayout *view = (Character3DTestLayout *)character;
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0);
    rt_world3d_add(world_a, floor);
    rt_character3d_set_world(character, world_a);
    rt_character3d_set_position(character, 0.0, 1.0, 0.0);
    rt_character3d_move(character, rt_vec3_new(0.0, -1.0, 0.0), 0.1);
    EXPECT_TRUE(rt_character3d_is_grounded(character) != 0,
                "character establishes a support before state-transition checks");

    rt_character3d_set_position(character, 0.0, 10.0, 0.0);
    EXPECT_TRUE(rt_character3d_is_grounded(character) == 0 &&
                    rt_character3d_get_ground_body(character) == nullptr,
                "teleport clears stale grounded and supporting-body state");

    rt_character3d_set_position(character, 0.0, 1.0, 0.0);
    rt_character3d_move(character, rt_vec3_new(0.0, -1.0, 0.0), 0.1);
    EXPECT_TRUE(rt_character3d_get_ground_body(character) == floor,
                "character reacquires support before world replacement");
    rt_character3d_set_world(character, world_b);
    EXPECT_TRUE(rt_character3d_is_grounded(character) == 0 &&
                    rt_character3d_get_ground_body(character) == nullptr,
                "changing worlds clears support retained from the old world");

    rt_body3d *saved_body = view->body;
    rt_world3d *saved_world = view->world;
    void *wrong_class = rt_vec3_new(4.0, 5.0, 6.0);
    view->body = (rt_body3d *)wrong_class;
    void *position = rt_character3d_get_position(character);
    EXPECT_NEAR(
        rt_vec3_x(position), 0.0, 0.001, "wrong-class private body returns a neutral position");
    view->body = saved_body;
    view->world = (rt_world3d *)wrong_class;
    EXPECT_TRUE(rt_character3d_get_world(character) == nullptr,
                "wrong-class private world is not exposed");
    view->world = saved_world;
    view->ground_body = (rt_body3d *)wrong_class;
    EXPECT_TRUE(rt_character3d_get_ground_body(character) == nullptr,
                "wrong-class private ground body is discarded");
}

static void test_character_skips_wrong_class_world_slots() {
    void *world = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_sphere(0.5, 0.0);
    void *character = rt_character3d_new(0.5, 2.0, 80.0);
    rt_world3d_add(world, body);
    rt_character3d_set_world(character, world);
    rt_world3d *world_view = (rt_world3d *)world;
    rt_body3d *saved = world_view->bodies[0];
    world_view->bodies[0] = (rt_body3d *)rt_vec3_new(0.0, 0.0, 0.0);
    rt_character3d_move(character, rt_vec3_new(1.0, 0.0, 0.0), 0.1);
    void *position = rt_character3d_get_position(character);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(position)) && std::isfinite(rt_vec3_y(position)) &&
                    std::isfinite(rt_vec3_z(position)),
                "character direct scan skips a wrong-class world body slot");
    world_view->bodies[0] = saved;
}

static void test_character_sweeps_platform_motion_and_reports_it_in_velocity() {
    void *world = rt_world3d_new(0, 0, 0);
    void *platform = rt_body3d_new_aabb(2.0, 0.25, 2.0, 1.0);
    void *wall = rt_body3d_new_aabb(0.1, 2.0, 2.0, 0.0);
    void *character = rt_character3d_new(0.3, 1.8, 70.0);
    Character3DTestLayout *view = (Character3DTestLayout *)character;
    rt_body3d_set_kinematic(platform, 1);
    rt_body3d_set_position(platform, 0.0, -0.25, 0.0);
    rt_body3d_set_velocity(platform, 20.0, 0.0, 0.0);
    rt_body3d_set_position(wall, 1.0, 1.0, 0.0);
    rt_world3d_add(world, platform);
    rt_world3d_add(world, wall);
    rt_character3d_set_world(character, world);
    rt_character3d_set_position(character, 0.0, 0.91, 0.0);
    rt_character3d_move(character, rt_vec3_new(0.0, -1.0, 0.0), 0.05);
    EXPECT_TRUE(rt_character3d_get_ground_body(character) == platform,
                "character establishes the moving platform as support");

    rt_character3d_move(character, rt_vec3_new(0.0, 0.0, 0.0), 0.1);
    void *position = rt_character3d_get_position(character);
    void *achieved_velocity = rt_body3d_get_velocity(view->body);
    EXPECT_TRUE(rt_vec3_x(position) < 0.7,
                "swept platform displacement cannot teleport the character through a wall");
    EXPECT_TRUE(rt_vec3_x(achieved_velocity) > 0.0,
                "body velocity includes achieved moving-platform displacement");

    double before_remove = rt_vec3_x(position);
    rt_world3d_remove(world, platform);
    rt_character3d_move(character, rt_vec3_new(0.0, 0.0, 0.0), 0.1);
    position = rt_character3d_get_position(character);
    EXPECT_NEAR(rt_vec3_x(position),
                before_remove,
                0.001,
                "removed supporting platforms no longer displace the character");
    EXPECT_TRUE(rt_character3d_get_ground_body(character) == nullptr,
                "removed supporting platforms are cleared from public ground state");
}

static void test_character_slide_against_wall() {
    void *w = rt_world3d_new(0, 0, 0);
    void *floor = rt_body3d_new_aabb(10.0, 0.5, 10.0, 0.0);
    void *wall = rt_body3d_new_aabb(0.5, 2.0, 2.0, 0.0);
    void *c = rt_character3d_new(0.5, 2.0, 80.0);
    void *v = rt_vec3_new(3.0, 0.0, 0.0);
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0);
    rt_body3d_set_position(wall, 2.0, 1.5, 0.0);
    rt_world3d_add(w, floor);
    rt_world3d_add(w, wall);
    rt_character3d_set_world(c, w);
    rt_character3d_set_position(c, 0.0, 1.0, 0.0);

    rt_character3d_move(c, v, 1.0);

    {
        void *pos = rt_character3d_get_position(c);
        EXPECT_TRUE(rt_vec3_x(pos) <= 1.05, "Character stops before wall instead of tunneling");
        EXPECT_TRUE(rt_character3d_is_grounded(c) != 0, "Character remains grounded while sliding");
    }
}

static void test_character_step_up() {
    void *w = rt_world3d_new(0, 0, 0);
    void *floor = rt_body3d_new_aabb(10.0, 0.5, 10.0, 0.0);
    void *step = rt_body3d_new_aabb(0.5, 0.125, 0.5, 0.0);
    void *c = rt_character3d_new(0.5, 2.0, 80.0);
    void *v = rt_vec3_new(2.0, 0.0, 0.0);
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0);
    rt_body3d_set_position(step, 1.2, 0.125, 0.0);
    rt_world3d_add(w, floor);
    rt_world3d_add(w, step);
    rt_character3d_set_world(c, w);
    rt_character3d_set_position(c, 0.0, 1.0, 0.0);

    rt_character3d_move(c, v, 1.0);

    {
        void *pos = rt_character3d_get_position(c);
        EXPECT_TRUE(rt_vec3_x(pos) > 1.0, "Character steps forward across low obstacle");
        EXPECT_TRUE(rt_vec3_y(pos) > 1.1, "Character steps up onto walkable obstacle");
    }
}

static void test_character_crosses_uneven_walkable_heightfield() {
    constexpr int kWidth = 25;
    constexpr int kDepth = 5;
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *pixels = rt_pixels_new(kWidth, kDepth);

    /* A shallow uphill grade with low alternating undulations reproduces the
     * cell-boundary contacts that used to consume sustained horizontal input.
     * Every local slope is far below the default 45-degree walkable limit. */
    for (int64_t z = 0; z < kDepth; ++z) {
        for (int64_t x = 0; x < kWidth; ++x) {
            double grade = 0.18 + (double)x * 0.012;
            double ripple = ((x + z) & 1) != 0 ? 0.006 : -0.006;
            double height = grade + ripple;
            uint16_t encoded = (uint16_t)(height * 65535.0);
            rt_pixels_set(pixels, x, z, encode_height16(encoded));
        }
    }

    void *heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 1.0, 1.0);
    void *terrain = rt_body3d_new(0.0);
    rt_body3d_set_collider(terrain, heightfield);
    rt_world3d_add(world, terrain);

    void *character = rt_character3d_new(0.35, 1.8, 70.0);
    rt_character3d_set_world(character, world);
    rt_character3d_set_step_height(character, 0.3);
    rt_character3d_set_position(character, -10.5, 1.13, 0.0);

    void *velocity = rt_vec3_new(2.0, -3.0, 0.0);
    for (int frame = 0; frame < 360; ++frame)
        rt_character3d_move(character, velocity, 1.0 / 60.0);

    void *position = rt_character3d_get_position(character);
    EXPECT_TRUE(rt_vec3_x(position) > 0.5,
                "Character sustains forward motion across an uneven walkable heightfield");
    EXPECT_TRUE(rt_vec3_y(position) > 1.2,
                "Character follows the rising heightfield instead of remaining below it");
    EXPECT_TRUE(rt_character3d_is_grounded(character) != 0,
                "Character remains grounded after crossing uneven walkable terrain");
}

/*==========================================================================
 * Trigger3D tests
 *=========================================================================*/

/*==========================================================================
 * Sphere-sphere collision tests
 *=========================================================================*/

static void test_sphere_sphere_collision() {
    /* Two spheres overlapping — should produce radial normal */
    void *world = rt_world3d_new(0, 0, 0);
    void *s1 = rt_body3d_new_sphere(1.0, 1.0);
    void *s2 = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_position(s1, 0, 0, 0);
    rt_body3d_set_position(s2, 1.5, 0, 0); /* overlap: sum_r=2 > dist=1.5 */
    rt_world3d_add(world, s1);
    rt_world3d_add(world, s2);
    rt_world3d_step(world, 1.0 / 60.0);

    /* After collision, s2 should have moved right (positive X) — radial pushout */
    void *pos2 = rt_body3d_get_position(s2);
    EXPECT_TRUE(rt_vec3_x(pos2) > 1.5, "sphere-sphere: s2 pushed right after collision");
    /* Collision event should have been recorded with radial normal */
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "sphere-sphere: 1 collision event recorded");
}

static void test_sphere_sphere_no_overlap() {
    /* Two spheres not overlapping — no collision */
    void *world = rt_world3d_new(0, 0, 0);
    void *s1 = rt_body3d_new_sphere(1.0, 1.0);
    void *s2 = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_position(s1, 0, 0, 0);
    rt_body3d_set_position(s2, 3.0, 0, 0); /* no overlap: sum_r=2 < dist=3 */
    rt_world3d_add(world, s1);
    rt_world3d_add(world, s2);
    rt_world3d_step(world, 1.0 / 60.0);

    /* Collision count should be 0 */
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 0,
                "sphere-sphere: no collision when separated");
}

static void test_aabb_sphere_collision() {
    /* AABB and sphere overlapping */
    void *world = rt_world3d_new(0, 0, 0);
    void *box = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *sph = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_position(box, 0, 0, 0);
    rt_body3d_set_position(sph, 1.5, 0, 0); /* overlap: box extends to 1.0, sphere from 0.5 */
    rt_world3d_add(world, box);
    rt_world3d_add(world, sph);
    rt_world3d_step(world, 1.0 / 60.0);

    EXPECT_TRUE(rt_world3d_get_collision_count(world) > 0, "aabb-sphere: collision detected");
}

static void test_capsule_aabb_collision() {
    void *world = rt_world3d_new(0, 0, 0);
    void *capsule = rt_body3d_new_capsule(0.5, 2.0, 1.0);
    void *wall = rt_body3d_new_aabb(0.5, 2.0, 2.0, 0.0);
    rt_body3d_set_position(capsule, 1.2, 1.0, 0.0);
    rt_body3d_set_position(wall, 2.0, 1.0, 0.0);
    rt_world3d_add(world, capsule);
    rt_world3d_add(world, wall);
    rt_world3d_step(world, 1.0 / 60.0);

    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1, "capsule-aabb: collision recorded");
    {
        void *pos = rt_body3d_get_position(capsule);
        EXPECT_TRUE(rt_vec3_x(pos) < 1.2, "capsule-aabb: capsule pushed out of wall");
    }
}

static void test_rotated_capsule_aabb_collision() {
    void *world = rt_world3d_new(0, 0, 0);
    void *capsule = rt_body3d_new_capsule(0.25, 4.0, 1.0);
    void *wall = rt_body3d_new_aabb(0.25, 0.5, 0.5, 0.0);
    void *q = rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), TEST_PI * 0.5);
    rt_body3d_set_orientation(capsule, q);
    rt_body3d_set_position(capsule, 0.0, 0.0, 0.0);
    rt_body3d_set_position(wall, 2.1, 0.0, 0.0);
    rt_world3d_add(world, capsule);
    rt_world3d_add(world, wall);
    rt_world3d_step(world, 1.0 / 60.0);

    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "rotated capsule-aabb: oriented capsule axis participates in collision");
}

static void test_offcenter_contact_generates_angular_velocity() {
    void *world = rt_world3d_new(0, 0, 0);
    void *capsule = rt_body3d_new_capsule(0.2, 1.2, 1.8);
    void *sphere = rt_body3d_new_sphere(0.22, 3.0);
    rt_body3d_set_position(capsule, 0.0, 0.6, 0.0);
    rt_body3d_set_position(sphere, -0.35, 0.9, 0.0);
    rt_body3d_set_velocity(sphere, 5.0, 0.0, 0.0);
    rt_world3d_add(world, capsule);
    rt_world3d_add(world, sphere);

    rt_world3d_step(world, 1.0 / 60.0);

    {
        void *ang = rt_body3d_get_angular_velocity(capsule);
        double angular_speed =
            sqrt(rt_vec3_x(ang) * rt_vec3_x(ang) + rt_vec3_y(ang) * rt_vec3_y(ang) +
                 rt_vec3_z(ang) * rt_vec3_z(ang));
        EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                    "off-center contact: collision recorded");
        EXPECT_TRUE(angular_speed > 0.1,
                    "off-center contact: angular velocity generated from contact point");
    }
}

static void test_large_floor_contact_point_stays_near_dynamic_body() {
    void *world = rt_world3d_new(0, 0, 0);
    void *floor = rt_body3d_new_aabb(100.0, 0.05, 100.0, 0.0);
    void *sphere = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_position(floor, 0.0, -0.05, 0.0);
    rt_body3d_set_position(sphere, 10.0, 0.95, 20.0);
    rt_world3d_add(world, floor);
    rt_world3d_add(world, sphere);

    rt_world3d_step(world, 1.0 / 60.0);

    {
        void *event = rt_world3d_get_collision_event(world, 0);
        void *point = rt_collision_event3d_get_contact_point(event, 0);
        EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                    "floor contact point: collision recorded");
        EXPECT_NEAR(rt_vec3_x(point), 10.0, 0.05, "floor contact point: x remains near sphere");
        EXPECT_NEAR(rt_vec3_z(point), 20.0, 0.05, "floor contact point: z remains near sphere");
    }
}

static void test_convex_hull_collider_blocks_sphere() {
    void *world = rt_world3d_new(0, 0, 0);
    void *mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
    void *collider = rt_collider3d_new_convex_hull(mesh);
    void *hull_body = rt_body3d_new(0.0);
    void *sphere = rt_body3d_new_sphere(0.6, 1.0);
    rt_body3d_set_collider(hull_body, collider);
    rt_body3d_set_position(hull_body, 0.0, 0.0, 0.0);
    rt_body3d_set_position(sphere, 1.3, 0.0, 0.0);
    rt_world3d_add(world, hull_body);
    rt_world3d_add(world, sphere);
    rt_world3d_step(world, 0.016);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) > 0,
                "convex hull: collision detected against sphere");
}

static void test_convex_hull_gjk_detects_contained_sphere() {
    void *world = rt_world3d_new(0, 0, 0);
    void *mesh = make_tetra_mesh(1);
    void *collider = rt_collider3d_new_convex_hull(mesh);
    void *hull_body = rt_body3d_new(0.0);
    void *sphere = rt_body3d_new_sphere(0.05, 1.0);
    rt_body3d_set_collider(hull_body, collider);
    rt_body3d_set_position(hull_body, 0.0, 0.0, 0.0);
    rt_body3d_set_position(sphere, 0.25, 0.25, 0.25);
    rt_world3d_add(world, hull_body);
    rt_world3d_add(world, sphere);

    rt_world3d_step(world, 0.016);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "convex hull GJK detects a simple collider fully contained by the hull");
}

static void test_convex_hull_gjk_rejects_aabb_false_positive() {
    void *world = rt_world3d_new(0, 0, 0);
    void *mesh_a = make_tetra_mesh(1);
    void *mesh_b = make_tetra_mesh(-1);
    void *collider_a = rt_collider3d_new_convex_hull(mesh_a);
    void *collider_b = rt_collider3d_new_convex_hull(mesh_b);
    void *body_a = rt_body3d_new(0.0);
    void *body_b = rt_body3d_new(1.0);
    rt_body3d_set_collider(body_a, collider_a);
    rt_body3d_set_collider(body_b, collider_b);
    rt_body3d_set_position(body_a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(body_b, 0.8, 0.8, 0.8);
    rt_world3d_add(world, body_a);
    rt_world3d_add(world, body_b);

    rt_world3d_step(world, 0.016);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 0,
                "convex hull GJK rejects separated hulls with overlapping AABBs");
}

static void test_unsupported_mesh_pair_rejects_aabb_false_positive() {
    void *mesh_a = make_tetra_mesh(1);
    void *mesh_b = make_tetra_mesh(-1);
    void *body_a_obj = rt_body3d_new(0.0);
    void *body_b_obj = rt_body3d_new(0.0);
    rt_body3d *body_a = static_cast<rt_body3d *>(body_a_obj);
    rt_body3d *body_b = static_cast<rt_body3d *>(body_b_obj);
    double normal[3] = {0.0, 0.0, 0.0};
    double point[3] = {0.0, 0.0, 0.0};
    double depth = 0.0;

    rt_body3d_set_collider(body_a, rt_collider3d_new_mesh(mesh_a));
    rt_body3d_set_collider(body_b, rt_collider3d_new_mesh(mesh_b));
    rt_body3d_set_position(body_a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(body_b, 0.8, 0.8, 0.8);

    EXPECT_TRUE(test_collision(
                    body_a, body_b, normal, &depth, point, nullptr, nullptr, nullptr, nullptr) == 0,
                "Unsupported mesh-mesh pair does not turn overlapping AABBs into a contact");
}

static void test_convex_hull_gjk_detects_overlapping_hulls() {
    void *world = rt_world3d_new(0, 0, 0);
    void *mesh_a = make_tetra_mesh(1);
    void *mesh_b = make_tetra_mesh(-1);
    void *collider_a = rt_collider3d_new_convex_hull(mesh_a);
    void *collider_b = rt_collider3d_new_convex_hull(mesh_b);
    void *body_a = rt_body3d_new(0.0);
    void *body_b = rt_body3d_new(1.0);
    rt_body3d_set_collider(body_a, collider_a);
    rt_body3d_set_collider(body_b, collider_b);
    rt_body3d_set_position(body_a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(body_b, 0.4, 0.4, 0.4);
    rt_world3d_add(world, body_a);
    rt_world3d_add(world, body_b);

    rt_world3d_step(world, 0.016);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "convex hull GJK/EPA detects overlapping hulls");
    if (rt_world3d_get_collision_count(world) > 0) {
        void *normal = rt_world3d_get_collision_normal(world, 0);
        EXPECT_TRUE(normal != nullptr, "convex hull GJK/EPA reports a contact normal");
        EXPECT_TRUE(rt_world3d_get_collision_depth(world, 0) >= 0.0,
                    "convex hull GJK/EPA reports finite penetration depth");
    }
}

static void test_convex_hull_gjk_handles_box_capsule_and_simple_edge_cases() {
    {
        void *world = rt_world3d_new(0, 0, 0);
        void *tetra = make_tetra_mesh(1);
        void *hull_collider = rt_collider3d_new_convex_hull(tetra);
        void *hull_body = rt_body3d_new(0.0);
        void *box = rt_body3d_new_aabb(0.05, 0.05, 0.05, 1.0);
        rt_body3d_set_collider(hull_body, hull_collider);
        rt_body3d_set_position(hull_body, 0.0, 0.0, 0.0);
        rt_body3d_set_position(box, 0.8, 0.8, 0.8);
        rt_world3d_add(world, hull_body);
        rt_world3d_add(world, box);
        rt_world3d_step(world, 0.016);
        EXPECT_TRUE(rt_world3d_get_collision_count(world) == 0,
                    "convex hull GJK rejects a box outside a tetrahedron but inside its AABB");
    }

    {
        void *world = rt_world3d_new(0, 0, 0);
        void *mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
        void *hull_collider = rt_collider3d_new_convex_hull(mesh);
        void *hull_body = rt_body3d_new(0.0);
        void *box = rt_body3d_new_aabb(0.25, 0.35, 0.25, 1.0);
        rt_body3d_set_collider(hull_body, hull_collider);
        rt_body3d_set_position(hull_body, 0.0, 0.0, 0.0);
        rt_body3d_set_position(box, -1.15, 0.0, 0.0);
        rt_world3d_add(world, box);
        rt_world3d_add(world, hull_body);
        rt_world3d_step(world, 0.016);
        EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                    "convex hull GJK detects the simple-vs-hull box branch");
        if (rt_world3d_get_collision_count(world) > 0) {
            void *normal = rt_world3d_get_collision_normal(world, 0);
            EXPECT_TRUE(normal != nullptr && std::isfinite(rt_vec3_x(normal)) &&
                            std::isfinite(rt_vec3_y(normal)) && std::isfinite(rt_vec3_z(normal)),
                        "convex hull GJK reports a finite box contact normal");
            EXPECT_TRUE(rt_world3d_get_collision_depth(world, 0) > 0.0,
                        "convex hull GJK reports positive box penetration");
        }
    }

    {
        void *world = rt_world3d_new(0, 0, 0);
        void *mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
        void *hull_collider = rt_collider3d_new_convex_hull(mesh);
        void *hull_body = rt_body3d_new(0.0);
        void *capsule = rt_body3d_new_capsule(0.25, 1.2, 1.0);
        rt_body3d_set_collider(hull_body, hull_collider);
        rt_body3d_set_position(hull_body, 0.0, 0.0, 0.0);
        rt_body3d_set_position(capsule, 1.15, 0.0, 0.0);
        rt_world3d_add(world, hull_body);
        rt_world3d_add(world, capsule);
        rt_world3d_step(world, 0.016);
        EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                    "convex hull GJK detects hull-vs-capsule contact");
        EXPECT_TRUE(rt_world3d_get_collision_depth(world, 0) > 0.0,
                    "convex hull GJK reports positive capsule penetration");
    }
}

static void test_convex_hull_gjk_perf_target() {
    const int pairs_per_shape = 8;
    const int shape_kinds = 4;
    const int pair_count = pairs_per_shape * shape_kinds;
    void *world = rt_world3d_new(0, 0, 0);
    void *hull_mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
    void *hull_collider = rt_collider3d_new_convex_hull(hull_mesh);
    void *small_hull_mesh = rt_mesh3d_new_box(0.8, 0.8, 0.8);
    void *small_hull_collider = rt_collider3d_new_convex_hull(small_hull_mesh);

    for (int i = 0; i < pair_count; ++i) {
        int kind = i / pairs_per_shape;
        double x = (double)i * 4.0;
        void *static_hull = rt_body3d_new(0.0);
        void *dynamic_body = nullptr;
        rt_body3d_set_collider(static_hull, hull_collider);
        rt_body3d_set_position(static_hull, x, 0.0, 0.0);
        rt_world3d_add(world, static_hull);

        if (kind == 0) {
            dynamic_body = rt_body3d_new_sphere(0.35, 1.0);
            rt_body3d_set_position(dynamic_body, x + 1.2, 0.0, 0.0);
        } else if (kind == 1) {
            dynamic_body = rt_body3d_new_capsule(0.25, 1.2, 1.0);
            rt_body3d_set_position(dynamic_body, x + 1.15, 0.0, 0.0);
        } else if (kind == 2) {
            dynamic_body = rt_body3d_new_aabb(0.25, 0.35, 0.25, 1.0);
            rt_body3d_set_position(dynamic_body, x + 1.15, 0.0, 0.0);
        } else {
            dynamic_body = rt_body3d_new(1.0);
            rt_body3d_set_collider(dynamic_body, small_hull_collider);
            rt_body3d_set_position(dynamic_body, x + 0.8, 0.0, 0.0);
        }
        rt_world3d_add(world, dynamic_body);
    }

    auto step_begin = std::chrono::steady_clock::now();
    rt_world3d_step(world, 1.0 / 60.0);
    auto step_end = std::chrono::steady_clock::now();

    EXPECT_TRUE(rt_world3d_get_collision_count(world) == pair_count,
                "convex hull GJK target: each isolated convex pair reports one contact");

    long long step_us =
        (long long)std::chrono::duration_cast<std::chrono::microseconds>(step_end - step_begin)
            .count();
    std::printf("PHYSICS_CONVEX_GJK_TARGET: pairs=%d contacts=%lld spheres=%d capsules=%d "
                "boxes=%d hulls=%d step_us=%lld\n",
                pair_count,
                (long long)rt_world3d_get_collision_count(world),
                pairs_per_shape,
                pairs_per_shape,
                pairs_per_shape,
                pairs_per_shape,
                step_us);
}

static void test_mesh_collider_blocks_falling_sphere() {
    void *world = rt_world3d_new(0, -9.81, 0);
    void *mesh = rt_mesh3d_new_box(8.0, 1.0, 8.0);
    void *collider = rt_collider3d_new_mesh(mesh);
    void *floor_body = rt_body3d_new(0.0);
    void *sphere = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_collider(floor_body, collider);
    rt_body3d_set_position(floor_body, 0.0, -0.5, 0.0);
    rt_body3d_set_position(sphere, 0.0, 2.0, 0.0);
    rt_world3d_add(world, floor_body);
    rt_world3d_add(world, sphere);
    for (int i = 0; i < 90; ++i)
        rt_world3d_step(world, 1.0 / 60.0);
    {
        void *pos = rt_body3d_get_position(sphere);
        EXPECT_TRUE(rt_vec3_y(pos) > -0.2, "mesh collider: sphere stays above floor");
    }
}

static void test_mesh_collider_narrowphase_builds_bvh_for_sphere_and_box() {
    {
        void *world = rt_world3d_new(0, 0, 0);
        void *mesh = rt_mesh3d_new_box(8.0, 1.0, 8.0);
        rt_mesh3d *mesh_impl = (rt_mesh3d *)mesh;
        void *collider = rt_collider3d_new_mesh(mesh);
        void *floor_body = rt_body3d_new(0.0);
        void *sphere = rt_body3d_new_sphere(0.75, 1.0);
        rt_body3d_set_collider(floor_body, collider);
        rt_body3d_set_position(floor_body, 0.0, -0.5, 0.0);
        rt_body3d_set_position(sphere, 0.0, 0.25, 0.0);
        rt_world3d_add(world, floor_body);
        rt_world3d_add(world, sphere);
        rt_world3d_step(world, 0.016);
        EXPECT_TRUE(rt_world3d_get_collision_count(world) > 0,
                    "mesh narrow phase: sphere overlap reports contact");
        EXPECT_TRUE(mesh_impl->physics_bvh_node_count > 0,
                    "mesh narrow phase: sphere path builds physics BVH");
        EXPECT_TRUE(mesh_impl->physics_bvh_revision == mesh_impl->geometry_revision,
                    "mesh narrow phase: sphere path marks BVH revision current");
    }

    {
        void *world = rt_world3d_new(0, 0, 0);
        void *mesh = rt_mesh3d_new_box(8.0, 1.0, 8.0);
        rt_mesh3d *mesh_impl = (rt_mesh3d *)mesh;
        void *collider = rt_collider3d_new_mesh(mesh);
        void *floor_body = rt_body3d_new(0.0);
        void *box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
        rt_body3d_set_collider(floor_body, collider);
        rt_body3d_set_position(floor_body, 0.0, -0.5, 0.0);
        rt_body3d_set_position(box, 0.0, 0.25, 0.0);
        rt_world3d_add(world, floor_body);
        rt_world3d_add(world, box);
        rt_world3d_step(world, 0.016);
        EXPECT_TRUE(rt_world3d_get_collision_count(world) > 0,
                    "mesh narrow phase: box overlap reports contact");
        EXPECT_TRUE(mesh_impl->physics_bvh_node_count > 0,
                    "mesh narrow phase: box path builds physics BVH");
        EXPECT_TRUE(mesh_impl->physics_bvh_revision == mesh_impl->geometry_revision,
                    "mesh narrow phase: box path marks BVH revision current");
    }
}

static void test_mesh_convex_hull_bvh_and_body_broadphase_target() {
    const int tile_count = 16;
    const int columns = 4;
    const int segments = 16;
    const double tile_size = 2.0;
    const double spacing = 4.0;
    const int target = 5;
    rt_mesh3d *meshes[tile_count];
    double tile_x[tile_count];
    double tile_z[tile_count];
    void *world = rt_world3d_new(0, 0, 0);

    for (int i = 0; i < tile_count; ++i) {
        int col = i % columns;
        int row = i / columns;
        tile_x[i] = ((double)col - 1.5) * spacing;
        tile_z[i] = ((double)row - 1.5) * spacing;
        void *mesh = make_subdivided_plane_mesh(segments, tile_size);
        void *collider = rt_collider3d_new_mesh(mesh);
        void *body = rt_body3d_new(0.0);
        meshes[i] = (rt_mesh3d *)mesh;
        rt_body3d_set_collider(body, collider);
        rt_body3d_set_position(body, tile_x[i], 0.0, tile_z[i]);
        rt_world3d_add(world, body);
    }

    void *hull_mesh = rt_mesh3d_new_box(0.8, 0.8, 0.8);
    void *hull_collider = rt_collider3d_new_convex_hull(hull_mesh);
    void *hull_body = rt_body3d_new(1.0);
    rt_body3d_set_collider(hull_body, hull_collider);
    rt_body3d_set_position(hull_body, tile_x[target], 0.2, tile_z[target]);
    rt_world3d_add(world, hull_body);

    auto step_begin = std::chrono::steady_clock::now();
    rt_world3d_step(world, 1.0 / 60.0);
    auto step_end = std::chrono::steady_clock::now();

    int built_bvhs = 0;
    for (int i = 0; i < tile_count; ++i) {
        if (meshes[i]->physics_bvh_node_count > 0)
            built_bvhs++;
    }

    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "mesh convex hull target: only the overlapping tile collides");
    EXPECT_TRUE(meshes[target]->physics_bvh_node_count > 0,
                "mesh convex hull target: overlapping tile builds its triangle BVH");
    EXPECT_TRUE(built_bvhs == 1,
                "mesh convex hull target: body broadphase skips distant mesh BVHs");

    long long step_us =
        (long long)std::chrono::duration_cast<std::chrono::microseconds>(step_end - step_begin)
            .count();
    std::printf("PHYSICS_MESH_BVH_TARGET: tiles=%d triangles_per_tile=%d built_mesh_bvhs=%d "
                "collisions=%lld step_us=%lld\n",
                tile_count,
                segments * segments * 2,
                built_bvhs,
                (long long)rt_world3d_get_collision_count(world),
                step_us);
}

static void test_compound_collider_child_transform_affects_contact() {
    void *world = rt_world3d_new(0, 0, 0);
    void *compound = rt_collider3d_new_compound();
    void *child = rt_collider3d_new_box(0.5, 0.5, 0.5);
    void *xf = rt_transform3d_new();
    void *compound_body = rt_body3d_new(0.0);
    void *sphere = rt_body3d_new_sphere(0.45, 1.0);
    rt_transform3d_set_position(xf, 2.0, 0.0, 0.0);
    rt_collider3d_add_child(compound, child, xf);
    rt_body3d_set_collider(compound_body, compound);
    rt_body3d_set_position(sphere, 2.2, 0.0, 0.0);
    rt_world3d_add(world, compound_body);
    rt_world3d_add(world, sphere);
    rt_world3d_step(world, 0.016);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) > 0,
                "compound collider: child offset affects contacts");
}

static void test_compound_collider_collects_multiple_leaf_contacts() {
    void *world = rt_world3d_new(0, 0, 0);
    void *compound = rt_collider3d_new_compound();
    void *left = rt_collider3d_new_box(0.45, 0.2, 0.45);
    void *right = rt_collider3d_new_box(0.45, 0.2, 0.45);
    void *left_xf = rt_transform3d_new();
    void *right_xf = rt_transform3d_new();
    void *compound_body = rt_body3d_new(0.0);
    void *bridge = rt_body3d_new_aabb(1.4, 0.2, 0.45, 1.0);

    rt_transform3d_set_position(left_xf, -0.75, 0.0, 0.0);
    rt_transform3d_set_position(right_xf, 0.75, 0.0, 0.0);
    rt_collider3d_add_child(compound, left, left_xf);
    rt_collider3d_add_child(compound, right, right_xf);
    rt_body3d_set_collider(compound_body, compound);
    rt_body3d_set_position(bridge, 0.0, 0.3, 0.0);
    rt_body3d_set_trigger(bridge, 1);
    rt_world3d_add(world, compound_body);
    rt_world3d_add(world, bridge);
    rt_world3d_step(world, 1.0 / 60.0);

    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "Compound multi-contact fixture produces one body-pair contact");
    void *event = rt_world3d_get_collision_event(world, 0);
    int64_t contact_count = rt_collision_event3d_get_contact_count(event);
    EXPECT_TRUE(contact_count >= 2,
                "Compound body-pair manifold includes more than its deepest child contact");
    double min_x = INFINITY;
    double max_x = -INFINITY;
    for (int64_t i = 0; i < contact_count; ++i) {
        void *point = rt_collision_event3d_get_contact_point(event, i);
        if (!point)
            continue;
        min_x = fmin(min_x, rt_vec3_x(point));
        max_x = fmax(max_x, rt_vec3_x(point));
    }
    EXPECT_TRUE(min_x < -0.2 && max_x > 0.2,
                "Compound manifold retains contacts from both separated child shapes");
}

static void test_compound_collider_rejects_transitive_cycle() {
    void *a = rt_collider3d_new_compound();
    void *b = rt_collider3d_new_compound();
    rt_collider3d_add_child(a, b, NULL);
    EXPECT_TRUE(rt_collider3d_get_child_count_raw(a) == 1,
                "Compound cycle test starts with one child");
    EXPECT_TRUE(expect_trap_contains([&] { rt_collider3d_add_child(b, a, NULL); }, "cycle"),
                "Compound collider rejects transitive cycles");
    EXPECT_TRUE(rt_collider3d_get_child_count_raw(b) == 0,
                "Rejected compound cycle does not mutate child list");
}

static void test_compound_collider_rejects_excessive_nesting() {
    void *leaf = rt_collider3d_new_sphere(0.5);
    void *root = leaf;
    for (int depth = 0; depth < RT_COLLIDER3D_MAX_COMPOUND_DEPTH; depth++) {
        void *parent = rt_collider3d_new_compound();
        rt_collider3d_add_child(parent, root, nullptr);
        root = parent;
    }
    void *too_deep = rt_collider3d_new_compound();
    EXPECT_TRUE(expect_trap_contains([&] { rt_collider3d_add_child(too_deep, root, nullptr); },
                                     "64 levels"),
                "Compound collider rejects trees that could exhaust recursive physics paths");
    EXPECT_TRUE(rt_collider3d_get_child_count_raw(too_deep) == 0,
                "Rejected excessive nesting leaves the target compound unchanged");
}

static void test_heightfield_collider_supports_ground_contact() {
    void *world = rt_world3d_new(0, 0, 0);
    void *pixels = rt_pixels_new(4, 4);
    void *heightfield;
    void *terrain_body;
    void *sphere;
    for (int64_t z = 0; z < 4; ++z) {
        for (int64_t x = 0; x < 4; ++x) {
            uint16_t h = (uint16_t)(32768 + x * 4096);
            rt_pixels_set(pixels, x, z, encode_height16(h));
        }
    }
    heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 2.0, 1.0);
    terrain_body = rt_body3d_new(0.0);
    sphere = rt_body3d_new_sphere(0.35, 1.0);
    rt_body3d_set_collider(terrain_body, heightfield);
    rt_body3d_set_position(sphere, 0.8, 1.1, 0.0);
    rt_world3d_add(world, terrain_body);
    rt_world3d_add(world, sphere);
    rt_world3d_step(world, 0.016);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) > 0,
                "heightfield collider: contact detected");
}

static void test_heightfield_box_samples_bottom_edges() {
    void *world = rt_world3d_new(0, 0, 0);
    void *pixels = rt_pixels_new(3, 3);
    for (int64_t z = 0; z < 3; ++z) {
        for (int64_t x = 0; x < 3; ++x)
            rt_pixels_set(pixels, x, z, encode_height16(0));
    }
    rt_pixels_set(pixels, 2, 1, encode_height16(65535));

    void *heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 1.0, 1.0);
    void *terrain_body = rt_body3d_new(0.0);
    void *box = rt_body3d_new_aabb(1.0, 0.25, 1.0, 1.0);
    rt_body3d_set_collider(terrain_body, heightfield);
    rt_body3d_set_position(box, 0.0, 0.5, 0.0);
    rt_world3d_add(world, terrain_body);
    rt_world3d_add(world, box);
    rt_world3d_step(world, 0.016);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) > 0,
                "heightfield box contact detects edge ridge");
}

/*==========================================================================
 * Collision event queue tests
 *=========================================================================*/

static void test_collision_event_count() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_position(a, 0, 0, 0);
    rt_body3d_set_position(b, 0.5, 0, 0); /* overlapping */
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_step(world, 1.0 / 60.0);

    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1, "collision event: 1 contact recorded");
}

static void test_collision_event_bodies() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_position(a, 0, 0, 0);
    rt_body3d_set_position(b, 0.5, 0, 0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_step(world, 1.0 / 60.0);

    void *body_a = rt_world3d_get_collision_body_a(world, 0);
    void *body_b = rt_world3d_get_collision_body_b(world, 0);
    EXPECT_TRUE(body_a == a, "collision event: body A matches");
    EXPECT_TRUE(body_b == b, "collision event: body B matches");

    void *normal = rt_world3d_get_collision_normal(world, 0);
    EXPECT_TRUE(normal != nullptr, "collision event: normal is non-null");

    double depth = rt_world3d_get_collision_depth(world, 0);
    EXPECT_TRUE(depth > 0, "collision event: depth > 0");

    void *event0 = rt_world3d_get_collision_event(world, 0);
    void *event1 = rt_world3d_get_collision_event(world, 0);
    EXPECT_TRUE(event0 != nullptr && event0 == event1,
                "collision event objects are cached per frame");
}

static void test_physics_world_event_hit_getters_sanitize_corrupt_private_state() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_position(a, 0, 0, 0);
    rt_body3d_set_position(b, 0.5, 0, 0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_step(world, 1.0 / 60.0);

    auto *world_view = static_cast<rt_world3d *>(world);
    int32_t saved_body_count = world_view->body_count;
    int32_t saved_contact_count = world_view->contact_count;
    int32_t saved_joint_count = world_view->joint_count;
    rt_body3d *saved_contact_body_a = world_view->contacts[0].body_a;

    world_view->body_count = INT32_MAX;
    EXPECT_TRUE(rt_world3d_body_count(world) == 2,
                "World3D body count ignores corrupt private tail count");
    EXPECT_TRUE(rt_world3d_contains_body(world, b) != 0,
                "World3D contains uses bounded private body count");
    world_view->body_count = saved_body_count;

    world_view->joint_count = INT32_MAX;
    EXPECT_TRUE(rt_world3d_joint_count(world) == 0,
                "World3D joint count ignores corrupt private tail count");
    world_view->joint_count = saved_joint_count;

    world_view->contact_count = INT32_MAX;
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "World3D collision count ignores corrupt private tail count");
    EXPECT_TRUE(rt_world3d_get_collision_body_a(world, 1) == nullptr,
                "World3D collision body getter rejects capped-out indices");
    world_view->contacts[0].body_a = static_cast<rt_body3d *>(rt_vec3_new(0.0, 0.0, 0.0));
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 0,
                "World3D collision count rejects wrong-class contact bodies");
    world_view->contacts[0].body_a = saved_contact_body_a;
    world_view->contact_count = saved_contact_count;

    world_view->last_solver_island_count = -4;
    world_view->last_solver_active_body_count = INT32_MAX;
    world_view->last_solver_contact_count = INT32_MAX;
    world_view->last_ccd_requested_substeps = INT32_MAX;
    world_view->last_ccd_substeps = INT32_MAX;
    world_view->ccd_substep_clamped_count = -9;
    EXPECT_TRUE(rt_world3d_get_last_solver_island_count(world) == 0,
                "World3D solver island telemetry treats negative private state as zero");
    EXPECT_TRUE(rt_world3d_get_last_solver_active_body_count(world) == 2,
                "World3D active-body telemetry caps at live body count");
    EXPECT_TRUE(rt_world3d_get_last_solver_contact_count(world) == 1,
                "World3D contact telemetry caps at live contact count");
    EXPECT_TRUE(rt_world3d_get_last_ccd_requested_substeps(world) == INT32_MAX,
                "World3D requested CCD telemetry preserves saturated unclamped demand");
    EXPECT_TRUE(rt_world3d_get_last_ccd_substeps(world) == PH3D_MAX_CCD_SUBSTEPS,
                "World3D CCD substeps cap at runtime maximum");
    EXPECT_TRUE(rt_world3d_get_ccd_substep_clamped_count(world) == 0,
                "World3D CCD clamp telemetry treats negative private state as zero");

    void *event = rt_world3d_get_collision_event(world, 0);
    EXPECT_TRUE(event != nullptr, "World3D corrupt getter test gets a collision event");
    auto *event_view = static_cast<rt_collision_event3d_obj *>(event);
    rt_body3d *saved_event_body_a = event_view->body_a;
    void *saved_event_collider_a = event_view->collider_a;
    int32_t saved_event_contact_count = event_view->contact_count;
    int8_t saved_event_trigger = event_view->is_trigger;
    event_view->body_a = static_cast<rt_body3d *>(rt_vec3_new(0.0, 0.0, 0.0));
    event_view->collider_a = rt_vec3_new(0.0, 0.0, 0.0);
    event_view->contact_count = INT32_MAX;
    event_view->is_trigger = -2;
    EXPECT_TRUE(rt_collision_event3d_get_body_a(event) == nullptr,
                "CollisionEvent3D body getter rejects wrong-class private refs");
    EXPECT_TRUE(rt_collision_event3d_get_collider_a(event) == nullptr,
                "CollisionEvent3D collider getter rejects wrong-class private refs");
    EXPECT_TRUE(rt_collision_event3d_get_contact_count(event) == PH3D_MAX_MANIFOLD_POINTS,
                "CollisionEvent3D contact count caps at manifold capacity");
    EXPECT_TRUE(rt_collision_event3d_get_contact(event, PH3D_MAX_MANIFOLD_POINTS) == nullptr,
                "CollisionEvent3D contact getter rejects capped-out indices");
    EXPECT_TRUE(rt_collision_event3d_get_is_trigger(event) == 1,
                "CollisionEvent3D trigger getter normalizes corrupt private flags");
    event_view->body_a = saved_event_body_a;
    event_view->collider_a = saved_event_collider_a;
    event_view->contact_count = saved_event_contact_count;
    event_view->is_trigger = saved_event_trigger;

    void *hits = rt_world3d_overlap_sphere(world, rt_vec3_new(0.0, 0.0, 0.0), 2.0, -1);
    EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) > 0,
                "World3D corrupt getter test gets overlap hits");
    auto *list_view = static_cast<rt_physics_hit_list3d_obj *>(hits);
    int64_t saved_hit_count = list_view->count;
    int64_t saved_hit_capacity = list_view->capacity;
    int64_t saved_hit_total = list_view->total_count;
    int8_t saved_hit_truncated = list_view->truncated;
    void *saved_hit_item0 = list_view->items[0];
    list_view->count = INT64_MAX;
    list_view->total_count = -5;
    list_view->truncated = -1;
    EXPECT_TRUE(rt_physics_hit_list3d_get_count(hits) == saved_hit_capacity,
                "PhysicsHitList3D count caps at private capacity");
    EXPECT_TRUE(rt_physics_hit_list3d_get_total_count(hits) == saved_hit_capacity,
                "PhysicsHitList3D total count is never below stored count");
    EXPECT_TRUE(rt_physics_hit_list3d_get_truncated(hits) == 1,
                "PhysicsHitList3D truncated getter normalizes corrupt private flags");
    list_view->items[0] = rt_vec3_new(0.0, 0.0, 0.0);
    EXPECT_TRUE(rt_physics_hit_list3d_get(hits, 0) == nullptr,
                "PhysicsHitList3D item getter rejects wrong-class private refs");
    list_view->items[0] = saved_hit_item0;
    list_view->count = saved_hit_count;
    list_view->capacity = saved_hit_capacity;
    list_view->total_count = saved_hit_total;
    list_view->truncated = saved_hit_truncated;

    void *hit = rt_physics_hit_list3d_get(hits, 0);
    auto *hit_view = static_cast<rt_physics_hit3d_obj *>(hit);
    rt_body3d *saved_hit_body = hit_view->body;
    void *saved_hit_collider = hit_view->collider;
    int8_t saved_started = hit_view->started_penetrating;
    int8_t saved_is_trigger = hit_view->is_trigger;
    hit_view->body = static_cast<rt_body3d *>(rt_vec3_new(0.0, 0.0, 0.0));
    hit_view->collider = rt_vec3_new(0.0, 0.0, 0.0);
    hit_view->started_penetrating = -3;
    hit_view->is_trigger = -4;
    EXPECT_TRUE(rt_physics_hit3d_get_body(hit) == nullptr,
                "PhysicsHit3D body getter rejects wrong-class private refs");
    EXPECT_TRUE(rt_physics_hit3d_get_collider(hit) == nullptr,
                "PhysicsHit3D collider getter rejects wrong-class private refs");
    EXPECT_TRUE(rt_physics_hit3d_get_started_penetrating(hit) == 1,
                "PhysicsHit3D started-penetrating getter normalizes corrupt private flags");
    EXPECT_TRUE(rt_physics_hit3d_get_is_trigger(hit) == 1,
                "PhysicsHit3D trigger getter normalizes corrupt private flags");
    hit_view->body = saved_hit_body;
    hit_view->collider = saved_hit_collider;
    hit_view->started_penetrating = saved_started;
    hit_view->is_trigger = saved_is_trigger;
}

static void test_world_raycast_returns_nearest_hit() {
    void *world = rt_world3d_new(0, 0, 0);
    void *near_box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *far_box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    rt_body3d_set_position(near_box, 5.0, 0.0, 0.0);
    rt_body3d_set_position(far_box, 8.0, 0.0, 0.0);
    rt_world3d_add(world, near_box);
    rt_world3d_add(world, far_box);

    {
        void *hit = rt_world3d_raycast(world, origin, dir, 20.0, 1);
        EXPECT_TRUE(hit != nullptr, "raycast: hit returned");
        EXPECT_TRUE(rt_physics_hit3d_get_body(hit) == near_box, "raycast: nearest body returned");
        EXPECT_TRUE(rt_physics_hit3d_get_distance(hit) > 4.0 &&
                        rt_physics_hit3d_get_distance(hit) < 5.5,
                    "raycast: distance near expected surface");
        EXPECT_TRUE(rt_physics_hit3d_get_started_penetrating(hit) == 0,
                    "raycast: not starting in penetration");
    }
}

static void test_world_raycast_all_sorted() {
    void *world = rt_world3d_new(0, 0, 0);
    void *near_box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *far_box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    rt_body3d_set_position(near_box, 4.0, 0.0, 0.0);
    rt_body3d_set_position(far_box, 7.0, 0.0, 0.0);
    rt_world3d_add(world, near_box);
    rt_world3d_add(world, far_box);

    {
        void *hits = rt_world3d_raycast_all(world, origin, dir, 20.0, 1);
        EXPECT_TRUE(hits != nullptr, "raycast all: list returned");
        EXPECT_TRUE(rt_physics_hit_list3d_get_count(hits) == 2, "raycast all: two hits");
        auto *list = static_cast<rt_physics_hit_list3d_obj *>(hits);
        EXPECT_TRUE(list->raw_hits != nullptr && list->items[0] == nullptr &&
                        list->items[1] == nullptr,
                    "raycast all: raw hits are packed and managed boxes start lazy");
        {
            void *hit0 = rt_physics_hit_list3d_get(hits, 0);
            void *hit1 = rt_physics_hit_list3d_get(hits, 1);
            EXPECT_TRUE(list->items[0] == hit0 && list->items[1] == hit1,
                        "raycast all: Get materializes each managed hit at most once");
            EXPECT_TRUE(rt_physics_hit3d_get_body(hit0) == near_box, "raycast all: hit0 is near");
            EXPECT_TRUE(rt_physics_hit3d_get_body(hit1) == far_box, "raycast all: hit1 is far");
            EXPECT_TRUE(rt_physics_hit3d_get_distance(hit0) < rt_physics_hit3d_get_distance(hit1),
                        "raycast all: hits sorted by distance");
        }
    }
}

static void test_world_overlap_hit_list_reports_truncation() {
    void *world = rt_world3d_new(0, 0, 0);
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    for (int i = 0; i < 260; i++) {
        void *body = rt_body3d_new_sphere(0.1, 0.0);
        double x = ((double)(i % 26) - 13.0) * 0.25;
        double z = (double)(i / 26) * 0.25;
        rt_body3d_set_position(body, x, 0.0, z);
        EXPECT_TRUE(rt_world3d_try_add(world, body) != 0,
                    "TryAdd succeeds while filling overlap set");
    }

    void *hits = rt_world3d_overlap_sphere(world, center, 100.0, 1);
    EXPECT_TRUE(hits != nullptr, "OverlapSphere returns a hit list");
    EXPECT_TRUE(rt_physics_hit_list3d_get_count(hits) == 256, "OverlapSphere stores bounded hits");
    EXPECT_TRUE(rt_physics_hit_list3d_get_total_count(hits) == 260,
                "OverlapSphere reports total matching hits");
    EXPECT_TRUE(rt_physics_hit_list3d_get_truncated(hits) != 0,
                "OverlapSphere marks truncated hit lists");

    rt_world3d_set_max_query_hits(world, 4096);
    void *expanded_hits = rt_world3d_overlap_sphere(world, center, 100.0, 1);
    EXPECT_TRUE(expanded_hits != nullptr, "OverlapSphere returns an expanded hit list");
    EXPECT_TRUE(rt_physics_hit_list3d_get_count(expanded_hits) == 260,
                "OverlapSphere exposes hits above the legacy 256-item limit");
    EXPECT_TRUE(rt_physics_hit_list3d_get_total_count(expanded_hits) == 260,
                "Expanded OverlapSphere preserves its total hit count");
    EXPECT_TRUE(rt_physics_hit_list3d_get_truncated(expanded_hits) == 0,
                "Expanded OverlapSphere is not marked truncated");
    EXPECT_TRUE(rt_physics_hit_list3d_get(expanded_hits, 259) != nullptr,
                "Expanded OverlapSphere exposes its final boxed hit");
}

static void test_world_sweep_sphere_reports_started_penetrating() {
    void *world = rt_world3d_new(0, 0, 0);
    void *wall = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    void *center = rt_vec3_new(0.25, 0.0, 0.0);
    void *delta = rt_vec3_new(2.0, 0.0, 0.0);
    rt_body3d_set_position(wall, 0.0, 0.0, 0.0);
    rt_world3d_add(world, wall);

    {
        void *hit = rt_world3d_sweep_sphere(world, center, 0.5, delta, 1);
        EXPECT_TRUE(hit != nullptr, "sweep sphere: hit returned");
        EXPECT_TRUE(rt_physics_hit3d_get_started_penetrating(hit) != 0,
                    "sweep sphere: started penetrating reported");
        EXPECT_NEAR(rt_physics_hit3d_get_fraction(hit), 0.0, 1e-6, "sweep sphere: fraction zero");
    }
}

static void test_world_overlap_queries_honor_mask() {
    void *world = rt_world3d_new(0, 0, 0);
    void *body_a = rt_body3d_new_sphere(0.5, 0.0);
    void *body_b = rt_body3d_new_sphere(0.5, 0.0);
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    void *minv = rt_vec3_new(-1.0, -1.0, -1.0);
    void *maxv = rt_vec3_new(1.0, 1.0, 1.0);
    rt_body3d_set_position(body_a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(body_b, 0.0, 0.0, 0.0);
    rt_body3d_set_collision_layer(body_a, 1);
    rt_body3d_set_collision_layer(body_b, 2);
    rt_world3d_add(world, body_a);
    rt_world3d_add(world, body_b);

    {
        void *hits1 = rt_world3d_overlap_sphere(world, center, 1.0, 1);
        void *hits2 = rt_world3d_overlap_aabb(world, minv, maxv, 2);
        EXPECT_TRUE(hits1 != nullptr, "overlap sphere: list returned");
        EXPECT_TRUE(rt_physics_hit_list3d_get_count(hits1) == 1, "overlap sphere: one masked hit");
        EXPECT_TRUE(rt_physics_hit3d_get_body(rt_physics_hit_list3d_get(hits1, 0)) == body_a,
                    "overlap sphere: layer-1 body returned");
        EXPECT_TRUE(hits2 != nullptr, "overlap aabb: list returned");
        EXPECT_TRUE(rt_physics_hit_list3d_get_count(hits2) == 1, "overlap aabb: one masked hit");
        EXPECT_TRUE(rt_physics_hit3d_get_body(rt_physics_hit_list3d_get(hits2, 0)) == body_b,
                    "overlap aabb: layer-2 body returned");
    }
}

static void test_world_overlap_queries_reject_nonfinite_inputs() {
    void *world = rt_world3d_new(0, 0, 0);
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    void *bad_center = rt_vec3_new(NAN, 0.0, 0.0);
    void *minv = rt_vec3_new(-1.0, -1.0, -1.0);
    void *bad_maxv = rt_vec3_new(INFINITY, 1.0, 1.0);

    EXPECT_TRUE(rt_world3d_overlap_sphere(world, center, NAN, 1) == nullptr,
                "OverlapSphere rejects non-finite radius");
    EXPECT_TRUE(rt_world3d_overlap_sphere(world, bad_center, 1.0, 1) == nullptr,
                "OverlapSphere rejects non-finite center");
    EXPECT_TRUE(rt_world3d_overlap_aabb(world, minv, bad_maxv, 1) == nullptr,
                "OverlapAabb rejects non-finite bounds");
}

static void test_world_queries_clamp_extreme_finite_inputs() {
    void *world = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    rt_world3d_add(world, body);

    void *origin = rt_vec3_new(-10.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0e300, 0.0, 0.0);
    void *hit = rt_world3d_raycast(world, origin, dir, 1.0e300, 1);
    EXPECT_TRUE(hit != nullptr, "Raycast clamps extreme finite direction and distance");
    EXPECT_TRUE(std::isfinite(rt_physics_hit3d_get_distance(hit)),
                "Raycast extreme finite hit distance is finite");

    void *minv = rt_vec3_new(-1.0e300, -1.0e300, -1.0e300);
    void *maxv = rt_vec3_new(1.0e300, 1.0e300, 1.0e300);
    void *hits = rt_world3d_overlap_aabb(world, minv, maxv, 1);
    EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) == 1,
                "OverlapAabb clamps extreme finite bounds");

    void *delta = rt_vec3_new(1.0e300, 0.0, 0.0);
    hit = rt_world3d_sweep_sphere(world, origin, 0.25, delta, 1);
    EXPECT_TRUE(hit != nullptr, "SweepSphere clamps extreme finite delta");
    EXPECT_TRUE(std::isfinite(rt_physics_hit3d_get_fraction(hit)) &&
                    rt_physics_hit3d_get_fraction(hit) >= 0.0 &&
                    rt_physics_hit3d_get_fraction(hit) <= 1.0,
                "SweepSphere extreme finite hit fraction is clamped");
}

static void test_world_query_broadphase_cache_invalidates_after_body_move() {
    void *world = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    rt_body3d_set_position(body, 0.0, 0.0, 0.0);
    rt_world3d_add(world, body);

    void *hits = rt_world3d_overlap_sphere(world, center, 0.25, -1);
    EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) == 1,
                "query broadphase initially sees the body");

    rt_body3d_set_position(body, 10.0, 0.0, 0.0);
    hits = rt_world3d_overlap_sphere(world, center, 0.25, -1);
    EXPECT_TRUE(hits == nullptr || rt_physics_hit_list3d_get_count(hits) == 0,
                "query broadphase cache invalidates after body movement");
}

static void test_world_query_broadphase_cache_invalidates_after_compound_mutation() {
    void *world = rt_world3d_new(0, 0, 0);
    void *compound = rt_collider3d_new_compound();
    void *body = rt_body3d_new(0.0);
    void *origin_child = rt_collider3d_new_box(0.5, 0.5, 0.5);
    void *far_child = rt_collider3d_new_box(0.5, 0.5, 0.5);
    void *far_transform = rt_transform3d_new();
    void *far_center = rt_vec3_new(10.0, 0.0, 0.0);
    rt_collider3d_add_child(compound, origin_child, nullptr);
    rt_body3d_set_collider(body, compound);
    rt_world3d_add(world, body);

    void *hits = rt_world3d_overlap_sphere(world, far_center, 0.25, -1);
    EXPECT_TRUE(hits == nullptr || rt_physics_hit_list3d_get_count(hits) == 0,
                "query broadphase initially excludes the future compound child");

    rt_transform3d_set_position(far_transform, 10.0, 0.0, 0.0);
    rt_collider3d_add_child(compound, far_child, far_transform);
    hits = rt_world3d_overlap_sphere(world, far_center, 0.25, -1);
    EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) == 1,
                "query broadphase invalidates after attached compound mutation");
}

static void test_world_query_broadphase_cache_invalidates_after_mesh_mutation() {
    void *world = rt_world3d_new(0, 0, 0);
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    void *collider = rt_collider3d_new_mesh(mesh);
    void *body = rt_body3d_new(0.0);
    void *far_center = rt_vec3_new(10.25, 0.25, 0.0);
    rt_body3d_set_collider(body, collider);
    rt_world3d_add(world, body);

    void *hits = rt_world3d_overlap_sphere(world, far_center, 0.1, -1);
    EXPECT_TRUE(hits == nullptr || rt_physics_hit_list3d_get_count(hits) == 0,
                "query broadphase initially excludes future mesh geometry");

    rt_mesh3d_add_vertex(mesh, 10.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 11.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 10.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 3, 4, 5);
    hits = rt_world3d_overlap_sphere(world, far_center, 0.1, -1);
    EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) == 1,
                "query broadphase invalidates after collider mesh mutation");
}

static void test_world_rebase_origin_shifts_body_contact_and_query_state() {
    void *world = rt_world3d_new(0, 0, 0);
    void *query_body = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    void *a = rt_body3d_new_sphere(1.0, 1.0);
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    rt_body3d_set_position(query_body, 1000.0, 50.0, 0.0);
    rt_body3d_set_position(a, 1004.0, 0.0, 0.0);
    rt_body3d_set_position(b, 1005.5, 0.0, 0.0);
    rt_world3d_add(world, query_body);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);

    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_event_count(world) >= 1,
                "RebaseOrigin setup records an overlapping contact");
    void *event = rt_world3d_get_collision_event(world, 0);
    void *point = rt_collision_event3d_get_contact_point(event, 0);
    double before_x = rt_vec3_x(point);

    void *far_center = rt_vec3_new(1000.0, 50.0, 0.0);
    void *hits = rt_world3d_overlap_sphere(world, far_center, 0.25, -1);
    EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) == 1,
                "RebaseOrigin setup warms the query broadphase at the far origin");

    rt_world3d_rebase_origin(world, 1000.0, 0.0, 0.0);

    void *query_pos = rt_body3d_get_position(query_body);
    EXPECT_NEAR(rt_vec3_x(query_pos), 0.0, 0.000001, "RebaseOrigin shifts body X");
    EXPECT_NEAR(rt_vec3_y(query_pos), 50.0, 0.000001, "RebaseOrigin preserves body Y");

    void *near_center = rt_vec3_new(0.0, 50.0, 0.0);
    hits = rt_world3d_overlap_sphere(world, near_center, 0.25, -1);
    EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) == 1,
                "RebaseOrigin invalidates query broadphase caches");

    event = rt_world3d_get_collision_event(world, 0);
    point = rt_collision_event3d_get_contact_point(event, 0);
    EXPECT_NEAR(
        rt_vec3_x(point), before_x - 1000.0, 0.000001, "RebaseOrigin shifts cached contact points");
}

static void test_collision_events_enter_stay_exit() {
    void *world = rt_world3d_new(0, 0, 0);
    void *floor = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    void *box = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_position(floor, 0.0, 0.0, 0.0);
    rt_body3d_set_position(box, 0.5, 0.0, 0.0);
    rt_world3d_add(world, floor);
    rt_world3d_add(world, box);

    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_enter_event_count(world) == 1,
                "collision events: first step enters");
    EXPECT_TRUE(rt_world3d_get_stay_event_count(world) == 0,
                "collision events: no stay on first step");
    EXPECT_TRUE(rt_world3d_get_exit_event_count(world) == 0,
                "collision events: no exit on first step");

    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_enter_event_count(world) == 0, "collision events: no re-enter");
    EXPECT_TRUE(rt_world3d_get_stay_event_count(world) == 1,
                "collision events: stay on second step");

    rt_body3d_set_position(box, 5.0, 0.0, 0.0);
    rt_body3d_set_velocity(box, 0.0, 0.0, 0.0);
    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_exit_event_count(world) == 1,
                "collision events: exit when separated");
}

static void test_query_mask_zero_matches_no_layers() {
    void *world = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_sphere(0.5, 0.0);
    void *origin = rt_vec3_new(-2.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    void *center = rt_vec3_new(0.0, 0.0, 0.0);
    void *delta = rt_vec3_new(2.0, 0.0, 0.0);
    void *minv = rt_vec3_new(-1.0, -1.0, -1.0);
    void *maxv = rt_vec3_new(1.0, 1.0, 1.0);
    rt_body3d_set_position(body, 0.0, 0.0, 0.0);
    rt_world3d_add(world, body);

    EXPECT_TRUE(rt_world3d_raycast(world, origin, dir, 10.0, 0) == nullptr,
                "LayerMask.None raycast returns no hit");
    void *ray_hits = rt_world3d_raycast_all(world, origin, dir, 10.0, 0);
    void *sphere_hits = rt_world3d_overlap_sphere(world, center, 1.0, 0);
    void *aabb_hits = rt_world3d_overlap_aabb(world, minv, maxv, 0);
    EXPECT_TRUE(ray_hits != nullptr && rt_physics_hit_list3d_get_count(ray_hits) == 0,
                "LayerMask.None raycast all returns an empty list");
    EXPECT_TRUE(sphere_hits != nullptr && rt_physics_hit_list3d_get_count(sphere_hits) == 0,
                "LayerMask.None overlap sphere returns an empty list");
    EXPECT_TRUE(aabb_hits != nullptr && rt_physics_hit_list3d_get_count(aabb_hits) == 0,
                "LayerMask.None overlap aabb returns an empty list");
    EXPECT_TRUE(rt_world3d_sweep_sphere(world, origin, 0.25, delta, 0) == nullptr,
                "LayerMask.None sphere sweep returns no hit");
}

static void test_kinematic_static_trigger_contacts_are_reported() {
    void *world = rt_world3d_new(0, 0, 0);
    void *trigger = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    void *body = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_trigger(trigger, 1);
    rt_body3d_set_kinematic(body, 1);
    rt_body3d_set_position(trigger, 0.0, 0.0, 0.0);
    rt_body3d_set_position(body, 0.5, 0.0, 0.0);
    rt_world3d_add(world, trigger);
    rt_world3d_add(world, body);

    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "kinematic-static trigger contact is reported");
    EXPECT_TRUE(rt_world3d_get_enter_event_count(world) == 1,
                "kinematic-static trigger contact emits enter");
}

static void test_contact_identity_survives_broadphase_order_flip() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *b = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_trigger(a, 1);
    rt_body3d_set_trigger(b, 1);
    rt_body3d_set_kinematic(a, 1);
    rt_body3d_set_kinematic(b, 1);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 0.5, 0.0, 0.0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);

    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_enter_event_count(world) == 1,
                "order flip setup enters on first frame");

    rt_body3d_set_position(a, 1.0, 0.0, 0.0);
    rt_body3d_set_position(b, 0.5, 0.0, 0.0);
    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_enter_event_count(world) == 0,
                "order-independent contact identity prevents false re-enter");
    EXPECT_TRUE(rt_world3d_get_stay_event_count(world) == 1,
                "order-independent contact identity emits stay after sort reversal");
    EXPECT_TRUE(rt_world3d_get_exit_event_count(world) == 0,
                "order-independent contact identity prevents false exit");
}

static void test_ccd_substep_contact_generates_frame_event() {
    void *world = rt_world3d_new(0, 0, 0);
    void *trigger = rt_body3d_new_aabb(0.05, 1.0, 1.0, 0.0);
    void *sphere = rt_body3d_new_sphere(0.1, 1.0);
    rt_body3d_set_trigger(trigger, 1);
    rt_body3d_set_position(trigger, 0.0, 0.0, 0.0);
    rt_body3d_set_position(sphere, -2.0, 0.0, 0.0);
    rt_body3d_set_velocity(sphere, 40.0, 0.0, 0.0);
    rt_body3d_set_use_ccd(sphere, 1);
    rt_world3d_add(world, trigger);
    rt_world3d_add(world, sphere);

    rt_world3d_step(world, 0.1);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "CCD substep-only contact is retained for the frame");
    EXPECT_TRUE(rt_world3d_get_enter_event_count(world) == 1,
                "CCD substep-only contact emits enter");
}

static void test_collision_event_surface_and_trigger_flag() {
    void *world = rt_world3d_new(0, 0, 0);
    void *trigger = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    void *body = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    rt_body3d_set_trigger(trigger, 1);
    rt_body3d_set_position(trigger, 0.0, 0.0, 0.0);
    rt_body3d_set_position(body, 0.5, 0.0, 0.0);
    rt_world3d_add(world, trigger);
    rt_world3d_add(world, body);
    rt_world3d_step(world, 1.0 / 60.0);

    {
        void *event = rt_world3d_get_collision_event(world, 0);
        EXPECT_TRUE(event != nullptr, "collision event: event object returned");
        EXPECT_TRUE(rt_collision_event3d_get_is_trigger(event) != 0,
                    "collision event: trigger flag propagated");
        EXPECT_TRUE(rt_collision_event3d_get_contact_count(event) > 1,
                    "collision event: AABB pair exposes manifold points");
        EXPECT_NEAR(rt_collision_event3d_get_normal_impulse(event),
                    0.0,
                    1e-9,
                    "collision event: trigger has zero impulse");
        {
            void *contact = rt_collision_event3d_get_contact(event, 0);
            void *contact1 = rt_collision_event3d_get_contact(event, 1);
            EXPECT_TRUE(contact != nullptr, "collision event: contact object returned");
            EXPECT_TRUE(contact1 != nullptr, "collision event: second manifold contact returned");
            EXPECT_TRUE(rt_contact_point3d_get_point(contact) != nullptr,
                        "collision event: contact point object returned");
            EXPECT_TRUE(rt_collision_event3d_get_contact_separation(event, 0) < 0.0,
                        "collision event: separation negative while penetrating");
        }
    }
}

static void test_rotated_box_box_exposes_clipped_manifold() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_aabb(1.0, 0.5, 0.75, 0.0);
    void *b = rt_body3d_new_aabb(1.0, 0.5, 0.75, 1.0);
    double angle = 0.40;
    void *q = rt_quat_from_axis_angle(rt_vec3_new(0.0, 1.0, 0.0), angle);
    void *x_axis = rt_quat_rotate_vec3(q, rt_vec3_new(1.0, 0.0, 0.0));
    double nx = rt_vec3_x(x_axis);
    double nz = rt_vec3_z(x_axis);
    rt_body3d_set_orientation(a, q);
    rt_body3d_set_orientation(b, q);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, nx * 1.85, 0.0, nz * 1.85);
    rt_body3d_set_trigger(b, 1);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);

    rt_world3d_step(world, 1.0 / 60.0);

    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "rotated box manifold: OBB face pair collides");
    {
        void *event = rt_world3d_get_collision_event(world, 0);
        int64_t contact_count = rt_collision_event3d_get_contact_count(event);
        EXPECT_TRUE(contact_count >= 4,
                    "rotated box manifold: clipped face contact exposes four points");
        for (int64_t i = 0; i < contact_count; ++i) {
            void *point = rt_collision_event3d_get_contact_point(event, i);
            void *normal = rt_collision_event3d_get_contact_normal(event, i);
            EXPECT_TRUE(point != nullptr, "rotated box manifold: contact point returned");
            EXPECT_TRUE(normal != nullptr, "rotated box manifold: contact normal returned");
            EXPECT_TRUE(rt_collision_event3d_get_contact_separation(event, i) < 0.0,
                        "rotated box manifold: clipped contact separation is penetrating");
        }
        {
            void *normal = rt_collision_event3d_get_contact_normal(event, 0);
            EXPECT_TRUE(fabs(rt_vec3_y(normal)) < 0.25,
                        "rotated box manifold: face normal is not the old axis-aligned Y fallback");
            EXPECT_TRUE(fabs(rt_vec3_x(normal)) > 0.75,
                        "rotated box manifold: face normal follows the rotated box axis");
            EXPECT_TRUE(fabs(rt_vec3_z(normal)) > 0.20,
                        "rotated box manifold: face normal preserves yaw rotation");
        }
    }
}

/*==========================================================================
 * Narrowphase correctness probes (2026-07 third review pass)
 *=========================================================================*/

/// Property probe for the OBB SAT edge-axis depth: translating B along the
/// reported A->B normal by the reported depth must separate every colliding
/// rotated-box pair. The pre-fix code left edge-cross penetrations in
/// |A_i x B_j|-scaled units (systematically underestimated), so pushed-out
/// pairs stayed overlapping in edge-dominant configurations.
static void test_obb_edge_axis_depth_resolves_separation() {
    void *a = rt_body3d_new_aabb(1.0, 0.6, 0.8, 1.0);
    void *b = rt_body3d_new_aabb(0.9, 0.7, 0.5, 1.0);
    void *qa = rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), 0.7853981633974483);
    const double axes[4][3] = {
        {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.6, 0.8, 0.0}, {0.577, 0.577, 0.577}};
    const double offsets[4][3] = {
        {1.4, 1.0, 0.2}, {0.9, 1.3, 0.6}, {1.6, 0.4, 0.9}, {1.1, 1.1, 1.1}};
    int colliding_configs = 0;
    rt_body3d_set_orientation(a, qa);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    for (int ai = 0; ai < 4; ++ai) {
        for (int oi = 0; oi < 4; ++oi) {
            void *axis = rt_vec3_new(axes[ai][0], axes[ai][1], axes[ai][2]);
            void *qb = rt_quat_from_axis_angle(axis, 0.9);
            double normal[3];
            double depth = 0.0;
            double point[3];
            rt_body3d_set_orientation(b, qb);
            rt_body3d_set_position(b, offsets[oi][0], offsets[oi][1], offsets[oi][2]);
            if (!test_collision((rt_body3d *)a,
                                (rt_body3d *)b,
                                normal,
                                &depth,
                                point,
                                nullptr,
                                nullptr,
                                nullptr,
                                nullptr))
                continue;
            colliding_configs++;
            {
                double resolved_normal[3];
                double resolved_depth = 0.0;
                double resolved_point[3];
                rt_body3d_set_position(b,
                                       offsets[oi][0] + normal[0] * (depth + 1e-7),
                                       offsets[oi][1] + normal[1] * (depth + 1e-7),
                                       offsets[oi][2] + normal[2] * (depth + 1e-7));
                int still = test_collision((rt_body3d *)a,
                                           (rt_body3d *)b,
                                           resolved_normal,
                                           &resolved_depth,
                                           resolved_point,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr);
                EXPECT_TRUE(!still || resolved_depth <= 1e-6,
                            "OBB SAT: translating by reported depth along the normal separates");
            }
        }
    }
    EXPECT_TRUE(colliding_configs >= 3, "OBB SAT probe exercised overlapping configurations");
}

/// An upside-down (180-degree rolled) box resting on a flat heightfield must still
/// produce a contact: the pre-fix probe sampled the box's LOCAL -Y face, which sits
/// on top after the roll, so the box sank a full height into terrain contact-free.
static void test_heightfield_box_upside_down_still_contacts() {
    void *pixels = rt_pixels_new(3, 3);
    for (int64_t z = 0; z < 3; ++z) {
        for (int64_t x = 0; x < 3; ++x)
            rt_pixels_set(pixels, x, z, encode_height16(0));
    }
    void *heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 1.0, 1.0);
    void *terrain_body = rt_body3d_new(0.0);
    void *box = rt_body3d_new_aabb(0.3, 0.2, 0.3, 1.0);
    void *flip = rt_quat_from_axis_angle(rt_vec3_new(1.0, 0.0, 0.0), TEST_PI);
    double normal[3];
    double depth = 0.0;
    double point[3];
    rt_body3d_set_collider(terrain_body, heightfield);
    rt_body3d_set_position(terrain_body, 0.0, 0.0, 0.0);
    rt_body3d_set_orientation(box, flip);
    rt_body3d_set_position(box, 0.0, 0.15, 0.0);
    EXPECT_TRUE(test_collision((rt_body3d *)terrain_body,
                               (rt_body3d *)box,
                               normal,
                               &depth,
                               point,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr) != 0,
                "heightfield: upside-down box still generates a contact");
    EXPECT_TRUE(depth > 0.02 && depth < 0.09,
                "heightfield: upside-down box depth is the true 0.05");
    EXPECT_TRUE(normal[1] > 0.9, "heightfield: upside-down box contact normal points up");
}

/// On a 45-degree ramp the vertical probe penetration must be projected onto the
/// surface normal (factor cos ~ 0.707); the pre-fix depth used the raw vertical
/// measure, over-ejecting bodies on steep slopes.
static void test_heightfield_slope_depth_projected_onto_normal() {
    void *pixels = rt_pixels_new(2, 2);
    rt_pixels_set(pixels, 0, 0, encode_height16(0));
    rt_pixels_set(pixels, 1, 0, encode_height16(0));
    rt_pixels_set(pixels, 0, 1, encode_height16(65535));
    rt_pixels_set(pixels, 1, 1, encode_height16(65535));
    void *heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 1.0, 1.0);
    void *terrain_body = rt_body3d_new(0.0);
    void *sphere = rt_body3d_new_sphere(0.3, 1.0);
    double normal[3];
    double depth = 0.0;
    double point[3];
    rt_body3d_set_collider(terrain_body, heightfield);
    /* Field spans x,z in [-0.5, 0.5]; surface height at z=0 is 0.5 (mid-ramp).
     * Sphere bottom at 0.45 -> vertical penetration 0.05, slope cos = 1/sqrt(2). */
    rt_body3d_set_position(sphere, 0.0, 0.75, 0.0);
    EXPECT_TRUE(test_collision((rt_body3d *)terrain_body,
                               (rt_body3d *)sphere,
                               normal,
                               &depth,
                               point,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr) != 0,
                "heightfield slope: sphere contact detected");
    EXPECT_TRUE(depth > 0.02 && depth < 0.045,
                "heightfield slope: depth projected onto the 45-degree normal (~0.035, not 0.05)");
    EXPECT_TRUE(normal[1] > 0.6 && normal[1] < 0.8, "heightfield slope: normal tilted ~45 degrees");
}

/// Friction must be direction-independent: a box sliding diagonally decelerates at
/// the same rate as one sliding along a tangent axis. The pre-fix per-axis clamps
/// formed a SQUARE cone that granted diagonal sliders up to sqrt(2)x friction.
static void test_friction_is_direction_independent() {
    void *world = rt_world3d_new(0.0, -10.0, 0.0);
    void *ground = rt_body3d_new_aabb(80.0, 1.0, 80.0, 0.0);
    void *axis_box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    void *diag_box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    const double speed = 4.0;
    const double inv_sqrt2 = 0.7071067811865476;
    rt_body3d_set_position(ground, 0.0, -1.0, 0.0);
    rt_body3d_set_friction(ground, 1.0);
    rt_body3d_set_position(axis_box, -30.0, 0.5, -30.0);
    rt_body3d_set_friction(axis_box, 1.0);
    rt_body3d_set_velocity(axis_box, speed, 0.0, 0.0);
    rt_body3d_set_position(diag_box, 30.0, 0.5, 30.0);
    rt_body3d_set_friction(diag_box, 1.0);
    rt_body3d_set_velocity(diag_box, speed * inv_sqrt2, 0.0, speed * inv_sqrt2);
    rt_world3d_add(world, ground);
    rt_world3d_add(world, axis_box);
    rt_world3d_add(world, diag_box);
    for (int i = 0; i < 90; ++i)
        rt_world3d_step(world, 1.0 / 60.0);
    {
        void *axis_pos = rt_body3d_get_position(axis_box);
        void *diag_pos = rt_body3d_get_position(diag_box);
        double ax = rt_vec3_x(axis_pos) + 30.0;
        double dx = rt_vec3_x(diag_pos) - 30.0;
        double dz = rt_vec3_z(diag_pos) - 30.0;
        double axis_dist = fabs(ax);
        double diag_dist = sqrt(dx * dx + dz * dz);
        EXPECT_TRUE(axis_dist > 0.05, "friction cone: axis slider actually moved");
        EXPECT_NEAR(diag_dist,
                    axis_dist,
                    axis_dist * 0.05 + 0.01,
                    "friction cone: diagonal slide distance matches axis slide");
    }
}

/// A long horizontal capsule lying across a narrow one-cell ridge must contact it:
/// the pre-fix path probed a fixed 5 samples along the axis, so a ridge between
/// samples was skipped entirely.
static void test_long_capsule_catches_narrow_ridge() {
    void *pixels = rt_pixels_new(25, 3);
    for (int64_t z = 0; z < 3; ++z) {
        for (int64_t x = 0; x < 25; ++x)
            rt_pixels_set(pixels, x, z, encode_height16(0));
    }
    for (int64_t z = 0; z < 3; ++z)
        rt_pixels_set(pixels, 14, z, encode_height16(65535));
    void *heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 1.0, 1.0);
    void *terrain_body = rt_body3d_new(0.0);
    void *capsule = rt_body3d_new_capsule(0.2, 20.0, 1.0);
    void *lie = rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), TEST_PI * 0.5);
    double normal[3];
    double depth = 0.0;
    double point[3];
    rt_body3d_set_collider(terrain_body, heightfield);
    rt_body3d_set_orientation(capsule, lie);
    /* Ridge column ix=14 -> local x = 2; capsule center x=0.4 keeps the old five
     * fixed samples (spaced 4.9 apart) outside the penetrating window around the
     * ridge peak while adaptive radius/2 spacing lands several samples in it. */
    rt_body3d_set_position(capsule, 0.4, 1.1, 0.0);
    EXPECT_TRUE(test_collision((rt_body3d *)terrain_body,
                               (rt_body3d *)capsule,
                               normal,
                               &depth,
                               point,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr) != 0,
                "heightfield: long capsule contacts a one-cell ridge between coarse samples");
    EXPECT_TRUE(depth > 0.005, "heightfield: ridge contact has usable depth");
}

/// A box resting on a flat heightfield must expose a multi-point support manifold:
/// the heightfield narrow phase reports one deepest point, and pre-fix nothing
/// expanded it, so crates on terrain balanced (and rocked) on a single contact.
static void test_box_on_heightfield_gets_support_manifold() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *pixels = rt_pixels_new(5, 5);
    for (int64_t z = 0; z < 5; ++z) {
        for (int64_t x = 0; x < 5; ++x)
            rt_pixels_set(pixels, x, z, encode_height16(0));
    }
    void *heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 1.0, 1.0);
    void *terrain_body = rt_body3d_new(0.0);
    void *box = rt_body3d_new_aabb(0.4, 0.2, 0.4, 1.0);
    rt_body3d_set_collider(terrain_body, heightfield);
    rt_body3d_set_position(box, 0.0, 0.17, 0.0);
    rt_world3d_add(world, terrain_body);
    rt_world3d_add(world, box);
    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "heightfield support manifold: contact reported");
    {
        void *event = rt_world3d_get_collision_event(world, 0);
        int64_t contact_count = rt_collision_event3d_get_contact_count(event);
        EXPECT_TRUE(contact_count >= 3,
                    "heightfield support manifold: box rests on a multi-point patch");
    }
}

/// A box resting on a flat triangle-mesh floor must likewise get a support patch.
static void test_box_on_mesh_floor_gets_support_manifold() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, -3.0, 0.0, -3.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 3.0, 0.0, -3.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 3.0, 0.0, 3.0, 0.0, 1.0, 0.0, 1.0, 1.0);
    rt_mesh3d_add_vertex(mesh, -3.0, 0.0, 3.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 2, 1);
    rt_mesh3d_add_triangle(mesh, 0, 3, 2);
    void *mesh_collider = rt_collider3d_new_mesh(mesh);
    void *floor_body = rt_body3d_new(0.0);
    void *box = rt_body3d_new_aabb(0.4, 0.2, 0.4, 1.0);
    rt_body3d_set_collider(floor_body, mesh_collider);
    rt_body3d_set_position(box, 0.5, 0.17, 0.5);
    rt_world3d_add(world, floor_body);
    rt_world3d_add(world, box);
    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "mesh support manifold: contact reported");
    {
        void *event = rt_world3d_get_collision_event(world, 0);
        int64_t contact_count = rt_collision_event3d_get_contact_count(event);
        EXPECT_TRUE(contact_count >= 3, "mesh support manifold: box rests on a multi-point patch");
    }
}

/// Collider-backed capsules must keep their lengthwise two-point manifold on
/// heightfields (the body shape cache mirrors the capsule collider).
static void test_capsule_on_heightfield_gets_lengthwise_manifold() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *pixels = rt_pixels_new(9, 3);
    for (int64_t z = 0; z < 3; ++z) {
        for (int64_t x = 0; x < 9; ++x)
            rt_pixels_set(pixels, x, z, encode_height16(0));
    }
    void *heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 1.0, 1.0);
    void *terrain_body = rt_body3d_new(0.0);
    void *capsule = rt_body3d_new_capsule(0.25, 3.0, 1.0);
    void *lie = rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), TEST_PI * 0.5);
    rt_body3d_set_collider(terrain_body, heightfield);
    rt_body3d_set_orientation(capsule, lie);
    rt_body3d_set_position(capsule, 0.0, 0.2, 0.0);
    rt_world3d_add(world, terrain_body);
    rt_world3d_add(world, capsule);
    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) == 1,
                "capsule heightfield manifold: contact reported");
    {
        void *event = rt_world3d_get_collision_event(world, 0);
        int64_t contact_count = rt_collision_event3d_get_contact_count(event);
        EXPECT_TRUE(contact_count >= 2,
                    "capsule heightfield manifold: lengthwise rest has two points");
    }
}

/// Long rays across large heightfields must not be cut off by an iteration cap:
/// the pre-fix march stopped after 2048 half-cell steps (~1024 cells) and
/// reported a MISS even though terrain lay ahead.
static void test_heightfield_long_raycast_hits_distant_terrain() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *pixels = rt_pixels_new(4097, 2);
    for (int64_t z = 0; z < 2; ++z) {
        for (int64_t x = 0; x < 4097; ++x)
            rt_pixels_set(pixels, x, z, encode_height16(0));
    }
    void *heightfield = rt_collider3d_new_heightfield(pixels, 1.0, 1.0, 1.0);
    void *terrain_body = rt_body3d_new(0.0);
    rt_body3d_set_collider(terrain_body, heightfield);
    rt_world3d_add(world, terrain_body);
    {
        double len = sqrt(2000.0 * 2000.0 + 4.0 * 4.0);
        void *origin = rt_vec3_new(-1500.0, 4.0, 0.0);
        void *dir = rt_vec3_new(2000.0 / len, -4.0 / len, 0.0);
        void *hit = rt_world3d_raycast(world, origin, dir, 3000.0, 1);
        EXPECT_TRUE(hit != nullptr, "heightfield raycast: distant ground hit is found");
        if (hit) {
            void *point = rt_physics_hit3d_get_point(hit);
            EXPECT_NEAR(rt_vec3_x(point), 500.0, 1.5, "heightfield raycast: hit lands at x=500");
            EXPECT_NEAR(
                rt_vec3_y(point), 0.0, 0.05, "heightfield raycast: hit lands on the ground");
        }
    }
}

/// Two fast CCD spheres approaching head-on must not pass through each other:
/// the pre-fix sweep only tested static/kinematic targets, so dynamic-vs-dynamic
/// pairs tunneled whenever the relative displacement per substep exceeded their
/// combined radii.
static void test_ccd_dynamic_pair_does_not_tunnel() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *left = rt_body3d_new_sphere(0.5, 1.0);
    void *right = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(left, -20.0, 0.0, 0.0);
    rt_body3d_set_position(right, 20.0, 0.0, 0.0);
    rt_body3d_set_velocity(left, 400.0, 0.0, 0.0);
    rt_body3d_set_velocity(right, -400.0, 0.0, 0.0);
    rt_body3d_set_use_ccd(left, 1);
    rt_body3d_set_use_ccd(right, 1);
    rt_world3d_add(world, left);
    rt_world3d_add(world, right);
    for (int i = 0; i < 12; ++i)
        rt_world3d_step(world, 1.0 / 60.0);
    {
        void *left_pos = rt_body3d_get_position(left);
        void *right_pos = rt_body3d_get_position(right);
        EXPECT_TRUE(rt_vec3_x(left_pos) < rt_vec3_x(right_pos) + 0.5,
                    "dynamic CCD: head-on spheres did not swap sides (no tunneling)");
    }
}

/// A box dropped tilted onto flat ground must settle without residual penetration:
/// the angular positional correction lets it rotate out of corner overlap instead
/// of only translating along the deepest contact normal.
static void test_tilted_box_settles_without_penetration() {
    void *world = rt_world3d_new(0.0, -10.0, 0.0);
    void *ground = rt_body3d_new_aabb(20.0, 1.0, 20.0, 0.0);
    void *box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    void *tilt = rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), 0.25);
    rt_body3d_set_position(ground, 0.0, -1.0, 0.0);
    rt_body3d_set_orientation(box, tilt);
    rt_body3d_set_position(box, 0.0, 0.9, 0.0);
    rt_world3d_add(world, ground);
    rt_world3d_add(world, box);
    for (int i = 0; i < 240; ++i)
        rt_world3d_step(world, 1.0 / 60.0);
    {
        double normal[3];
        double depth = 0.0;
        double point[3];
        void *pos = rt_body3d_get_position(box);
        int overlapping = test_collision((rt_body3d *)ground,
                                         (rt_body3d *)box,
                                         normal,
                                         &depth,
                                         point,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr);
        EXPECT_TRUE(rt_vec3_y(pos) > 0.2 && rt_vec3_y(pos) < 0.8,
                    "tilted box settles onto the ground (not ejected, not sunk)");
        EXPECT_TRUE(!overlapping || depth < 0.02,
                    "tilted box settles without residual penetration");
    }
}

/*==========================================================================
 * Trigger3D tests
 *=========================================================================*/

static void test_trigger_create() {
    void *t = rt_trigger3d_new(-1, -1, -1, 1, 1, 1);
    EXPECT_TRUE(t != nullptr, "Trigger3D created");
}

static void test_trigger_contains_inside() {
    void *t = rt_trigger3d_new(-2, -2, -2, 2, 2, 2);
    void *p = rt_vec3_new(0, 0, 0);
    EXPECT_TRUE(rt_trigger3d_contains(t, p) != 0, "Origin inside [-2,2] trigger");
}

static void test_trigger_contains_outside() {
    void *t = rt_trigger3d_new(-1, -1, -1, 1, 1, 1);
    void *p = rt_vec3_new(5, 0, 0);
    EXPECT_TRUE(rt_trigger3d_contains(t, p) == 0, "Point at (5,0,0) outside [-1,1] trigger");
    EXPECT_TRUE(rt_trigger3d_contains(t, t) == 0, "Trigger3D.Contains rejects non-Vec3 handles");
}

static void test_trigger_enter_detection() {
    void *w = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_aabb(0.1, 0.1, 0.1, 1.0);
    rt_body3d_set_position(body, 5, 0, 0);
    rt_world3d_add(w, body);

    void *t = rt_trigger3d_new(-1, -1, -1, 1, 1, 1);

    /* Frame 1: body outside */
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(t) == 0, "Frame 1: no enter");

    /* Move body inside the trigger */
    rt_body3d_set_position(body, 0, 0, 0);

    /* Frame 2: body now inside — should detect enter */
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(t) == 1, "Frame 2: enter detected");
    EXPECT_TRUE(rt_trigger3d_get_exit_count(t) == 0, "Frame 2: no exit");
}

static void test_trigger_exit_detection() {
    void *w = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_aabb(0.1, 0.1, 0.1, 1.0);
    rt_body3d_set_position(body, 0, 0, 0);
    rt_world3d_add(w, body);

    void *t = rt_trigger3d_new(-1, -1, -1, 1, 1, 1);

    /* Frame 1: body inside */
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(t) == 1, "Frame 1: initial enter");

    /* Move body outside */
    rt_body3d_set_position(body, 5, 0, 0);

    /* Frame 2: body now outside — should detect exit */
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_exit_count(t) == 1, "Frame 2: exit detected");
    EXPECT_TRUE(rt_trigger3d_get_enter_count(t) == 0, "Frame 2: no re-enter");
}

static void test_trigger_multiple_bodies() {
    void *w = rt_world3d_new(0, 0, 0);
    void *b1 = rt_body3d_new_aabb(0.1, 0.1, 0.1, 1.0);
    void *b2 = rt_body3d_new_aabb(0.1, 0.1, 0.1, 1.0);
    rt_body3d_set_position(b1, 5, 0, 0);
    rt_body3d_set_position(b2, 5, 0, 0);
    rt_world3d_add(w, b1);
    rt_world3d_add(w, b2);

    void *t = rt_trigger3d_new(-1, -1, -1, 1, 1, 1);

    /* Frame 1: both outside */
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(t) == 0, "Frame 1: both outside");

    /* Move only b1 inside */
    rt_body3d_set_position(b1, 0, 0, 0);
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(t) == 1, "Frame 2: one enters");

    /* Move b2 inside too */
    rt_body3d_set_position(b2, 0, 0, 0);
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(t) == 1, "Frame 3: second body enters");
}

static void test_trigger_set_bounds() {
    void *t = rt_trigger3d_new(0, 0, 0, 1, 1, 1);
    void *p = rt_vec3_new(5, 5, 5);
    EXPECT_TRUE(rt_trigger3d_contains(t, p) == 0, "Before resize: outside");

    rt_trigger3d_set_bounds(t, 0, 0, 0, 10, 10, 10);
    EXPECT_TRUE(rt_trigger3d_contains(t, p) != 0, "After resize: inside");

    rt_trigger3d_set_bounds(t, -1.0e300, -1.0e300, -1.0e300, 1.0e300, 1.0e300, 1.0e300);
    EXPECT_TRUE(rt_trigger3d_contains(t, p) != 0, "Trigger3D clamps extreme finite bounds");
    EXPECT_TRUE(rt_trigger3d_contains(t, rt_vec3_new(NAN, 0.0, 0.0)) == 0,
                "Trigger3D.Contains rejects non-finite points");
}

/*==========================================================================
 * Joint tests
 *=========================================================================*/

static void test_distance_joint_create() {
    void *a = rt_body3d_new_sphere(1.0, 1.0);
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    void *j = rt_distance_joint3d_new(a, b, 5.0);
    EXPECT_TRUE(j != nullptr, "DistanceJoint3D.New creates joint");
    EXPECT_NEAR(rt_distance_joint3d_get_distance(j), 5.0, 0.01, "DistanceJoint3D distance = 5.0");
}

static void test_joints_retain_bodies_and_sanitize_parameters() {
    void *a = rt_body3d_new_sphere(1.0, 1.0);
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    void *distance = rt_distance_joint3d_new(a, b, INFINITY);
    int a_reached_zero = rt_obj_release_check0(a);
    int b_reached_zero = rt_obj_release_check0(b);

    EXPECT_TRUE(a_reached_zero == 0, "DistanceJoint3D retains body A");
    EXPECT_TRUE(b_reached_zero == 0, "DistanceJoint3D retains body B");
    EXPECT_NEAR(rt_distance_joint3d_get_distance(distance),
                0.0,
                0.01,
                "DistanceJoint3D converts infinite distances to zero");

    void *c = rt_body3d_new_sphere(1.0, 1.0);
    void *d = rt_body3d_new_sphere(1.0, 1.0);
    void *spring = rt_spring_joint3d_new(c, d, NAN, INFINITY, -1.0);
    int c_reached_zero = rt_obj_release_check0(c);
    int d_reached_zero = rt_obj_release_check0(d);
    EXPECT_TRUE(c_reached_zero == 0, "SpringJoint3D retains body A");
    EXPECT_TRUE(d_reached_zero == 0, "SpringJoint3D retains body B");
    EXPECT_NEAR(rt_spring_joint3d_get_rest_length(spring),
                0.0,
                0.01,
                "SpringJoint3D converts NaN rest length to zero");
    EXPECT_NEAR(rt_spring_joint3d_get_stiffness(spring),
                0.0,
                0.01,
                "SpringJoint3D converts infinite stiffness to zero");
    EXPECT_NEAR(rt_spring_joint3d_get_damping(spring),
                0.0,
                0.01,
                "SpringJoint3D converts negative damping to zero");
    rt_spring_joint3d_set_stiffness(spring, 1.0e100);
    rt_spring_joint3d_set_damping(spring, 1.0e100);
    EXPECT_NEAR(
        rt_spring_joint3d_get_stiffness(spring), 1.0e9, 1.0, "SpringJoint3D clamps huge stiffness");
    EXPECT_NEAR(
        rt_spring_joint3d_get_damping(spring), 1.0e9, 1.0, "SpringJoint3D clamps huge damping");

    void *e = rt_body3d_new_sphere(1.0, 1.0);
    void *f = rt_body3d_new_sphere(1.0, 1.0);
    void *rope = rt_rope_joint3d_new(e, f, INFINITY);
    int e_reached_zero = rt_obj_release_check0(e);
    int f_reached_zero = rt_obj_release_check0(f);
    EXPECT_TRUE(e_reached_zero == 0, "RopeJoint3D retains body A");
    EXPECT_TRUE(f_reached_zero == 0, "RopeJoint3D retains body B");
    EXPECT_NEAR(rt_rope_joint3d_get_max_length(rope),
                0.0,
                0.01,
                "RopeJoint3D converts infinite max length to zero");
    rt_rope_joint3d_set_max_length(rope, 1.0e100);
    EXPECT_NEAR(
        rt_rope_joint3d_get_max_length(rope), 1.0e9, 1.0, "RopeJoint3D clamps huge max length");
}

static void test_joint_readbacks_repair_corrupt_private_state() {
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    void *wrong = rt_vec3_new(9.0, 8.0, 7.0);
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *axis = rt_vec3_new(0.0, 1.0, 0.0);
    void *frame = rt_mat4_identity();
    auto *distance = static_cast<DistanceJoint3DTestLayout *>(rt_distance_joint3d_new(a, b, 2.0));
    auto *spring =
        static_cast<SpringJoint3DTestLayout *>(rt_spring_joint3d_new(a, b, 2.0, 3.0, 4.0));
    auto *hinge = static_cast<HingeJoint3DTestLayout *>(rt_hinge_joint3d_new(a, b, origin, axis));
    auto *rope = static_cast<RopeJoint3DTestLayout *>(rt_rope_joint3d_new(a, b, 2.0));
    auto *six = static_cast<SixDofJoint3DTestLayout *>(rt_sixdof_joint3d_new(a, b, frame, frame));
    assert(a && b && wrong && origin && axis && frame && distance && spring && hinge && rope &&
           six);

    distance->body_a = wrong;
    distance->body_b = wrong;
    spring->body_a = wrong;
    spring->body_b = wrong;
    hinge->body_a = wrong;
    hinge->body_b = wrong;
    rope->body_a = wrong;
    rope->body_b = wrong;
    six->body_a = wrong;
    six->body_b = wrong;
    EXPECT_TRUE(rt_distance_joint3d_get_body_a(distance) == a,
                "DistanceJoint3D.BodyA repairs its retained body mirror");
    EXPECT_TRUE(rt_distance_joint3d_get_body_b(distance) == b,
                "DistanceJoint3D.BodyB repairs its retained body mirror");
    EXPECT_TRUE(rt_spring_joint3d_get_body_a(spring) == a,
                "SpringJoint3D.BodyA repairs its retained body mirror");
    EXPECT_TRUE(rt_spring_joint3d_get_body_b(spring) == b,
                "SpringJoint3D.BodyB repairs its retained body mirror");
    EXPECT_TRUE(rt_hinge_joint3d_get_body_a(hinge) == a,
                "HingeJoint3D.BodyA repairs its retained body mirror");
    EXPECT_TRUE(rt_hinge_joint3d_get_body_b(hinge) == b,
                "HingeJoint3D.BodyB repairs its retained body mirror");
    EXPECT_TRUE(rt_rope_joint3d_get_body_a(rope) == a,
                "RopeJoint3D.BodyA repairs its retained body mirror");
    EXPECT_TRUE(rt_rope_joint3d_get_body_b(rope) == b,
                "RopeJoint3D.BodyB repairs its retained body mirror");
    EXPECT_TRUE(rt_sixdof_joint3d_get_body_a(six) == a,
                "SixDofJoint3D.BodyA repairs its retained body mirror");
    EXPECT_TRUE(rt_sixdof_joint3d_get_body_b(six) == b,
                "SixDofJoint3D.BodyB repairs its retained body mirror");

    distance->target_distance = NAN;
    spring->rest_length = NAN;
    spring->stiffness = INFINITY;
    spring->damping = -INFINITY;
    rope->max_length = NAN;
    EXPECT_NEAR(rt_distance_joint3d_get_distance(distance),
                0.0,
                0.0,
                "DistanceJoint3D.Distance repairs non-finite retained state");
    EXPECT_NEAR(rt_spring_joint3d_get_rest_length(spring),
                0.0,
                0.0,
                "SpringJoint3D.RestLength repairs non-finite retained state");
    EXPECT_NEAR(rt_spring_joint3d_get_stiffness(spring),
                0.0,
                0.0,
                "SpringJoint3D.Stiffness repairs non-finite retained state");
    EXPECT_NEAR(rt_spring_joint3d_get_damping(spring),
                0.0,
                0.0,
                "SpringJoint3D.Damping repairs non-finite retained state");
    EXPECT_NEAR(rt_rope_joint3d_get_max_length(rope),
                0.0,
                0.0,
                "RopeJoint3D.MaxLength repairs non-finite retained state");

    hinge->motor_enabled = -7;
    hinge->motor_target_velocity = INFINITY;
    hinge->motor_max_impulse = -INFINITY;
    hinge->has_limits = -3;
    hinge->angle_min = NAN;
    hinge->angle_max = INFINITY;
    EXPECT_TRUE(rt_hinge_joint3d_get_motor_enabled(hinge) == 1 && hinge->motor_enabled == 1,
                "HingeJoint3D.MotorEnabled persists a canonical Boolean");
    EXPECT_NEAR(rt_hinge_joint3d_get_motor_target_velocity(hinge),
                0.0,
                0.0,
                "HingeJoint3D.MotorTargetVelocity repairs non-finite state");
    EXPECT_NEAR(rt_hinge_joint3d_get_motor_max_impulse(hinge),
                0.0,
                0.0,
                "HingeJoint3D.MotorMaxImpulse repairs non-finite state");
    EXPECT_TRUE(rt_hinge_joint3d_get_limits_enabled(hinge) == 0 && hinge->has_limits == 0,
                "HingeJoint3D disables corrupt non-finite retained limits");
    EXPECT_NEAR(rt_hinge_joint3d_get_limit_min(hinge),
                0.0,
                0.0,
                "HingeJoint3D.LimitMin hides disabled corrupt limits");
    EXPECT_NEAR(rt_hinge_joint3d_get_limit_max(hinge),
                0.0,
                0.0,
                "HingeJoint3D.LimitMax hides disabled corrupt limits");

    six->linear_min[0] = NAN;
    six->linear_min[1] = 5.0;
    six->linear_min[2] = -INFINITY;
    six->linear_max[0] = INFINITY;
    six->linear_max[1] = -5.0;
    six->linear_max[2] = NAN;
    six->angular_min[0] = NAN;
    six->angular_min[1] = 4.0;
    six->angular_min[2] = -INFINITY;
    six->angular_max[0] = INFINITY;
    six->angular_max[1] = -4.0;
    six->angular_max[2] = NAN;
    six->linear_motor_enabled = -4;
    six->linear_motor_velocity[0] = NAN;
    six->linear_motor_velocity[1] = INFINITY;
    six->linear_motor_velocity[2] = -INFINITY;
    six->linear_motor_max_impulse = INFINITY;
    void *linear_min = rt_sixdof_joint3d_get_linear_limit_min(six);
    void *linear_max = rt_sixdof_joint3d_get_linear_limit_max(six);
    void *angular_min = rt_sixdof_joint3d_get_angular_limit_min(six);
    void *angular_max = rt_sixdof_joint3d_get_angular_limit_max(six);
    void *motor_velocity = rt_sixdof_joint3d_get_linear_motor_velocity(six);
    EXPECT_TRUE(rt_vec3_x(linear_min) == 0.0 && rt_vec3_y(linear_min) == -5.0 &&
                    rt_vec3_z(linear_min) == 0.0,
                "SixDofJoint3D.LinearLimitMin repairs and canonicalizes retained lanes");
    EXPECT_TRUE(rt_vec3_x(linear_max) == 0.0 && rt_vec3_y(linear_max) == 5.0 &&
                    rt_vec3_z(linear_max) == 0.0,
                "SixDofJoint3D.LinearLimitMax repairs and canonicalizes retained lanes");
    EXPECT_TRUE(rt_vec3_x(angular_min) == 0.0 && rt_vec3_y(angular_min) == -4.0 &&
                    rt_vec3_z(angular_min) == 0.0,
                "SixDofJoint3D.AngularLimitMin repairs and canonicalizes retained lanes");
    EXPECT_TRUE(rt_vec3_x(angular_max) == 0.0 && rt_vec3_y(angular_max) == 4.0 &&
                    rt_vec3_z(angular_max) == 0.0,
                "SixDofJoint3D.AngularLimitMax repairs and canonicalizes retained lanes");
    EXPECT_TRUE(rt_sixdof_joint3d_get_linear_motor_enabled(six) == 1 &&
                    six->linear_motor_enabled == 1,
                "SixDofJoint3D.LinearMotorEnabled persists a canonical Boolean");
    EXPECT_TRUE(rt_vec3_x(motor_velocity) == 0.0 && rt_vec3_y(motor_velocity) == 0.0 &&
                    rt_vec3_z(motor_velocity) == 0.0,
                "SixDofJoint3D.LinearMotorVelocity repairs non-finite retained lanes");
    EXPECT_NEAR(rt_sixdof_joint3d_get_linear_motor_max_impulse(six),
                0.0,
                0.0,
                "SixDofJoint3D.LinearMotorMaxImpulse repairs non-finite state");
}

static void test_distance_joint_owned_bodies_survive_mirror_corruption() {
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    void *unrelated = rt_body3d_new_sphere(0.5, 1.0);
    auto *joint = static_cast<DistanceJoint3DTestLayout *>(rt_distance_joint3d_new(a, b, 5.0));
    assert(a && b && unrelated && joint);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 10.0, 0.0, 0.0);
    rt_body3d_set_position(unrelated, 100.0, 0.0, 0.0);
    joint->body_a = unrelated;
    rt_joint3d_solve(joint, RT_JOINT_DISTANCE, 1.0 / 60.0);
    void *a_position = rt_body3d_get_position(a);
    void *unrelated_position = rt_body3d_get_position(unrelated);
    EXPECT_TRUE(rt_vec3_x(a_position) > 0.0,
                "DistanceJoint3D solve repairs body mirrors before applying corrections");
    EXPECT_NEAR(rt_vec3_x(unrelated_position),
                100.0,
                0.0,
                "DistanceJoint3D solve never mutates a substituted unrelated body");
}

static void test_distance_joint_finalizer_releases_owned_bodies_after_mirror_corruption() {
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    void *wrong = rt_vec3_new(0.0, 0.0, 0.0);
    auto *joint = static_cast<DistanceJoint3DTestLayout *>(rt_distance_joint3d_new(a, b, 1.0));
    assert(a && b && wrong && joint);
    joint->body_a = wrong;
    joint->body_b = wrong;
    if (rt_obj_release_check0(joint))
        rt_obj_free(joint);
    int a_released = rt_obj_release_check0(a);
    int b_released = rt_obj_release_check0(b);
    EXPECT_TRUE(a_released == 1,
                "DistanceJoint3D finalizer releases owned body A despite mirror corruption");
    EXPECT_TRUE(b_released == 1,
                "DistanceJoint3D finalizer releases owned body B despite mirror corruption");
    if (a_released)
        rt_obj_free(a);
    if (b_released)
        rt_obj_free(b);
    if (rt_obj_release_check0(wrong))
        rt_obj_free(wrong);
}

static void test_joint3d_extreme_finite_inputs_remain_finite() {
    void *hinge_world = rt_world3d_new(0.0, 0.0, 0.0);
    void *base = rt_body3d_new_sphere(0.25, 0.0);
    void *arm = rt_body3d_new_sphere(0.25, 1.0);
    rt_body3d_set_collision_layer(base, 1);
    rt_body3d_set_collision_mask(base, 1);
    rt_body3d_set_collision_layer(arm, 2);
    rt_body3d_set_collision_mask(arm, 2);
    void *hinge = rt_hinge_joint3d_new(
        base, arm, rt_vec3_new(1.0e300, 0.0, 0.0), rt_vec3_new(1.0e300, 0.0, 0.0));
    EXPECT_TRUE(hinge != nullptr, "HingeJoint3D accepts clampable extreme finite anchor/axis");
    rt_hinge_joint3d_set_motor(hinge, 1, 1.0e300, 1.0e300);
    rt_hinge_joint3d_set_limits(hinge, 1.0e300, -1.0e300);
    rt_world3d_add(hinge_world, base);
    rt_world3d_add(hinge_world, arm);
    rt_world3d_add_joint(hinge_world, hinge, RT_JOINT_HINGE);
    rt_world3d_step(hinge_world, 1.0e300);

    void *arm_pos = rt_body3d_get_position(arm);
    void *arm_ang = rt_body3d_get_angular_velocity(arm);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(arm_pos)) && std::isfinite(rt_vec3_y(arm_pos)) &&
                    std::isfinite(rt_vec3_z(arm_pos)),
                "Extreme finite hinge solve keeps body position finite");
    EXPECT_TRUE(std::isfinite(rt_vec3_x(arm_ang)) && std::isfinite(rt_vec3_y(arm_ang)) &&
                    std::isfinite(rt_vec3_z(arm_ang)),
                "Extreme finite hinge solve keeps angular velocity finite");
    EXPECT_TRUE(std::isfinite(rt_hinge_joint3d_get_angle(hinge)),
                "Extreme finite hinge angle accessor remains finite");

    void *six_world = rt_world3d_new(0.0, 0.0, 0.0);
    void *six_a = rt_body3d_new_sphere(0.25, 0.0);
    void *six_b = rt_body3d_new_sphere(0.25, 1.0);
    void *frame_a = rt_mat4_new(1.0,
                                0.0,
                                0.0,
                                1.0e300,
                                0.0,
                                1.0,
                                0.0,
                                -1.0e300,
                                0.0,
                                0.0,
                                1.0,
                                1.0e300,
                                0.0,
                                0.0,
                                0.0,
                                1.0);
    void *frame_b = rt_mat4_new(1.0,
                                0.0,
                                0.0,
                                -1.0e300,
                                0.0,
                                1.0,
                                0.0,
                                1.0e300,
                                0.0,
                                0.0,
                                1.0,
                                -1.0e300,
                                0.0,
                                0.0,
                                0.0,
                                1.0);
    void *six = rt_sixdof_joint3d_new(six_a, six_b, frame_a, frame_b);
    EXPECT_TRUE(six != nullptr, "SixDofJoint3D clamps extreme finite frame translations");
    rt_sixdof_joint3d_set_linear_limits(
        six, rt_vec3_new(1.0e300, 0.0, -1.0e300), rt_vec3_new(-1.0e300, 0.0, 1.0e300));
    rt_sixdof_joint3d_set_angular_limits(
        six, rt_vec3_new(1.0e300, -1.0e300, 0.0), rt_vec3_new(-1.0e300, 1.0e300, 0.0));
    rt_sixdof_joint3d_set_linear_motor(six, 1, rt_vec3_new(1.0e300, -1.0e300, 0.0), 1.0e300);
    rt_world3d_add(six_world, six_a);
    rt_world3d_add(six_world, six_b);
    rt_world3d_add_joint(six_world, six, RT_JOINT_SIXDOF);
    rt_world3d_step(six_world, 1.0e300);

    void *six_pos = rt_body3d_get_position(six_b);
    void *six_vel = rt_body3d_get_velocity(six_b);
    void *six_rot = rt_body3d_get_orientation(six_b);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(six_pos)) && std::isfinite(rt_vec3_y(six_pos)) &&
                    std::isfinite(rt_vec3_z(six_pos)),
                "Extreme finite 6DOF solve keeps body position finite");
    EXPECT_TRUE(std::isfinite(rt_vec3_x(six_vel)) && std::isfinite(rt_vec3_y(six_vel)) &&
                    std::isfinite(rt_vec3_z(six_vel)),
                "Extreme finite 6DOF solve keeps velocity finite");
    EXPECT_TRUE(std::isfinite(rt_quat_x(six_rot)) && std::isfinite(rt_quat_y(six_rot)) &&
                    std::isfinite(rt_quat_z(six_rot)) && std::isfinite(rt_quat_w(six_rot)),
                "Extreme finite 6DOF solve keeps orientation finite");
}

static void test_distance_joint_constraint() {
    /* Two bodies separated by 10 units, connected by distance joint of 5 */
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(a, 0, 0, 0);
    rt_body3d_set_position(b, 10, 0, 0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_add_joint(world, rt_distance_joint3d_new(a, b, 5.0), RT_JOINT_DISTANCE);

    /* Step several times — bodies should move toward target distance */
    for (int i = 0; i < 60; i++)
        rt_world3d_step(world, 1.0 / 60.0);

    void *pos_a = rt_body3d_get_position(a);
    void *pos_b = rt_body3d_get_position(b);
    double dist = fabs(rt_vec3_x(pos_b) - rt_vec3_x(pos_a));
    EXPECT_NEAR(dist, 5.0, 0.5, "DistanceJoint: bodies converge to target distance");
}

static void test_spring_joint_create() {
    void *a = rt_body3d_new_sphere(1.0, 1.0);
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    void *j = rt_spring_joint3d_new(a, b, 3.0, 100.0, 5.0);
    EXPECT_TRUE(j != nullptr, "SpringJoint3D.New creates joint");
    EXPECT_NEAR(rt_spring_joint3d_get_stiffness(j), 100.0, 0.01, "SpringJoint3D stiffness = 100");
    EXPECT_NEAR(rt_spring_joint3d_get_damping(j), 5.0, 0.01, "SpringJoint3D damping = 5");
    EXPECT_NEAR(rt_spring_joint3d_get_rest_length(j), 3.0, 0.01, "SpringJoint3D rest length = 3");
}

static void test_spring_joint_force() {
    /* Two bodies separated by 10 units, spring rest length 3, high stiffness */
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(a, 0, 0, 0);
    rt_body3d_set_position(b, 10, 0, 0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_add_joint(world, rt_spring_joint3d_new(a, b, 3.0, 50.0, 10.0), RT_JOINT_SPRING);

    /* After stepping, bodies should move closer due to spring force */
    for (int i = 0; i < 30; i++)
        rt_world3d_step(world, 1.0 / 60.0);

    void *pos_b = rt_body3d_get_position(b);
    EXPECT_TRUE(rt_vec3_x(pos_b) < 10.0, "SpringJoint: body B pulled toward A by spring");
}

static void test_hinge_joint_motor_drives_rotation() {
    void *world = rt_world3d_new(0, 0, 0);                /* no gravity */
    void *base = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);  /* static anchor */
    void *wheel = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0); /* dynamic */
    rt_body3d_set_position(base, 0, 0, 0);
    rt_body3d_set_position(wheel, 0, 0, 0);
    /* Keep them off each other's collision masks so only the hinge acts. */
    rt_body3d_set_collision_layer(base, 1);
    rt_body3d_set_collision_mask(base, 1);
    rt_body3d_set_collision_layer(wheel, 2);
    rt_body3d_set_collision_mask(wheel, 2);
    rt_world3d_add(world, base);
    rt_world3d_add(world, wheel);

    void *hinge = rt_hinge_joint3d_new(base, wheel, rt_vec3_new(0, 0, 0), rt_vec3_new(0, 1, 0));
    rt_hinge_joint3d_set_motor(hinge, 1, 2.0, 10.0); /* drive +Y at 2 rad/s */
    rt_world3d_add_joint(world, hinge, RT_JOINT_HINGE);

    for (int i = 0; i < 10; i++)
        rt_world3d_step(world, 1.0 / 60.0);
    void *av = rt_body3d_get_angular_velocity(wheel);
    EXPECT_NEAR(rt_vec3_y(av), 2.0, 0.1, "Hinge motor drives the wheel to target angular velocity");

    /* Re-target the motor to zero — it should brake the wheel to a stop. */
    rt_hinge_joint3d_set_motor(hinge, 1, 0.0, 10.0);
    for (int i = 0; i < 10; i++)
        rt_world3d_step(world, 1.0 / 60.0);
    av = rt_body3d_get_angular_velocity(wheel);
    EXPECT_NEAR(rt_vec3_y(av), 0.0, 0.1, "Hinge motor brakes the wheel toward zero");
}

static void test_hinge_joint_get_angle() {
    void *base = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *arm = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    void *hinge = rt_hinge_joint3d_new(base, arm, rt_vec3_new(0, 0, 0), rt_vec3_new(0, 1, 0));
    EXPECT_NEAR(rt_hinge_joint3d_get_angle(hinge), 0.0, 0.01, "Fresh hinge reads a zero angle");
    /* Rotate the arm 0.5 rad about the hinge axis; the measured angle follows. */
    rt_body3d_set_orientation(arm, rt_quat_from_axis_angle(rt_vec3_new(0, 1, 0), 0.5));
    EXPECT_NEAR(fabs(rt_hinge_joint3d_get_angle(hinge)),
                0.5,
                0.02,
                "Hinge angle tracks rotation about the axis");
}

static void test_hinge_joint_angle_limit() {
    void *world = rt_world3d_new(0, 0, 0);
    void *base = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *arm = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    rt_body3d_set_position(base, 0, 0, 0);
    rt_body3d_set_position(arm, 0, 0, 0);
    rt_body3d_set_collision_layer(base, 1);
    rt_body3d_set_collision_mask(base, 1);
    rt_body3d_set_collision_layer(arm, 2);
    rt_body3d_set_collision_mask(arm, 2);
    rt_world3d_add(world, base);
    rt_world3d_add(world, arm);

    void *hinge = rt_hinge_joint3d_new(base, arm, rt_vec3_new(0, 0, 0), rt_vec3_new(0, 1, 0));
    rt_hinge_joint3d_set_motor(hinge, 1, 2.0, 10.0); /* drive toward +angle */
    rt_hinge_joint3d_set_limits(hinge, -0.5, 0.5);   /* but stop at 0.5 rad */
    rt_world3d_add_joint(world, hinge, RT_JOINT_HINGE);

    for (int i = 0; i < 120; i++)
        rt_world3d_step(world, 1.0 / 60.0);

    double angle = rt_hinge_joint3d_get_angle(hinge);
    /* The motor would spin freely, but the upper limit stops it near +0.5 rad. */
    EXPECT_TRUE(angle <= 0.55, "Hinge angle limit stops rotation at the upper bound");
    EXPECT_TRUE(angle >= 0.45, "Hinge reaches the upper limit");
    void *av = rt_body3d_get_angular_velocity(arm);
    EXPECT_TRUE(fabs(rt_vec3_y(av)) < 0.2, "Hinge is held still at the limit");
}

static void test_hinge_joint_anchor_constraint() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.25, 0.0);
    void *b = rt_body3d_new_sphere(0.25, 1.0);
    void *anchor = rt_vec3_new(0.0, 0.0, 0.0);
    void *axis = rt_vec3_new(0.0, 1.0, 0.0);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 2.0, 0.0, 0.0);
    rt_body3d_set_velocity(b, 0.0, 10.0, 0.0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_add_joint(world, rt_hinge_joint3d_new(a, b, anchor, axis), RT_JOINT_HINGE);

    rt_world3d_step(world, 1.0 / 60.0);

    void *pos_b = rt_body3d_get_position(b);
    EXPECT_NEAR(rt_vec3_x(pos_b), 2.0, 0.05, "HingeJoint3D keeps authored anchor length");
    EXPECT_NEAR(rt_vec3_y(pos_b), 0.0, 0.05, "HingeJoint3D corrects anchor drift");
}

static void test_rope_joint_max_length_constraint() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.25, 0.0);
    void *b = rt_body3d_new_sphere(0.25, 1.0);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 5.0, 0.0, 0.0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_add_joint(world, rt_rope_joint3d_new(a, b, 2.0), RT_JOINT_ROPE);

    rt_world3d_step(world, 1.0 / 60.0);

    void *pos_b = rt_body3d_get_position(b);
    EXPECT_NEAR(rt_vec3_x(pos_b), 2.0, 0.05, "RopeJoint3D enforces MaxLength");

    void *world2 = rt_world3d_new(0, 0, 0);
    void *c = rt_body3d_new_sphere(0.25, 0.0);
    void *d = rt_body3d_new_sphere(0.25, 1.0);
    rt_body3d_set_position(c, 0.0, 0.0, 0.0);
    rt_body3d_set_position(d, 1.0, 0.0, 0.0);
    rt_world3d_add(world2, c);
    rt_world3d_add(world2, d);
    rt_world3d_add_joint(world2, rt_rope_joint3d_new(c, d, 2.0), RT_JOINT_ROPE);
    rt_world3d_step(world2, 1.0 / 60.0);

    void *pos_d = rt_body3d_get_position(d);
    EXPECT_NEAR(rt_vec3_x(pos_d), 1.0, 0.05, "RopeJoint3D does not pull inside MaxLength");
}

static void test_sixdof_joint_linear_motor() {
    void *world = rt_world3d_new(0, 0, 0);
    void *base = rt_body3d_new_sphere(0.25, 0.0);   /* static anchor */
    void *slider = rt_body3d_new_sphere(0.25, 1.0); /* dynamic */
    /* Off each other's collision masks so only the joint acts. */
    rt_body3d_set_collision_layer(base, 1);
    rt_body3d_set_collision_mask(base, 1);
    rt_body3d_set_collision_layer(slider, 2);
    rt_body3d_set_collision_mask(slider, 2);
    void *joint = rt_sixdof_joint3d_new(base, slider, rt_mat4_identity(), rt_mat4_identity());
    /* X free (slider rail), Y/Z locked. */
    rt_sixdof_joint3d_set_linear_limits(
        joint, rt_vec3_new(-10.0, 0.0, 0.0), rt_vec3_new(10.0, 0.0, 0.0));
    rt_sixdof_joint3d_set_linear_motor(joint, 1, rt_vec3_new(2.0, 0.0, 0.0), 10.0);
    rt_body3d_set_position(base, 0.0, 0.0, 0.0);
    rt_body3d_set_position(slider, 0.0, 0.0, 0.0);
    rt_world3d_add(world, base);
    rt_world3d_add(world, slider);
    rt_world3d_add_joint(world, joint, RT_JOINT_SIXDOF);

    for (int i = 0; i < 5; i++)
        rt_world3d_step(world, 1.0 / 60.0);

    void *vel = rt_body3d_get_velocity(slider);
    EXPECT_NEAR(
        rt_vec3_x(vel), 2.0, 0.1, "SixDof linear motor drives the slider along the free axis");
    EXPECT_TRUE(fabs(rt_vec3_y(vel)) < 0.1, "SixDof keeps the locked axis still");
}

static void test_sixdof_joint_frame_anchor_constraint() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.25, 0.0);
    void *b = rt_body3d_new_sphere(0.25, 1.0);
    void *frame_a = rt_mat4_identity();
    void *frame_b = rt_mat4_identity();
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 3.0, 0.0, 0.0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_add_joint(world, rt_sixdof_joint3d_new(a, b, frame_a, frame_b), RT_JOINT_SIXDOF);

    rt_world3d_step(world, 1.0 / 60.0);

    void *pos_b = rt_body3d_get_position(b);
    EXPECT_NEAR(rt_vec3_x(pos_b), 0.0, 0.05, "SixDofJoint3D locks frame anchors");
}

static void test_sixdof_joint_limits() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.25, 0.0);
    void *b = rt_body3d_new_sphere(0.25, 1.0);
    void *joint = rt_sixdof_joint3d_new(a, b, rt_mat4_identity(), rt_mat4_identity());
    rt_sixdof_joint3d_set_linear_limits(
        joint, rt_vec3_new(-1.0, 0.0, 0.0), rt_vec3_new(1.0, 0.0, 0.0));
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 3.0, 0.0, 0.0);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_add_joint(world, joint, RT_JOINT_SIXDOF);
    rt_world3d_step(world, 1.0 / 60.0);

    void *pos_b = rt_body3d_get_position(b);
    EXPECT_NEAR(rt_vec3_x(pos_b), 1.0, 0.05, "SixDofJoint3D SetLinearLimits clamps separation");

    void *world2 = rt_world3d_new(0, 0, 0);
    void *c = rt_body3d_new_sphere(0.25, 0.0);
    void *d = rt_body3d_new_sphere(0.25, 1.0);
    void *angular_joint = rt_sixdof_joint3d_new(c, d, rt_mat4_identity(), rt_mat4_identity());
    rt_sixdof_joint3d_set_linear_limits(
        angular_joint, rt_vec3_new(-10.0, -10.0, -10.0), rt_vec3_new(10.0, 10.0, 10.0));
    rt_sixdof_joint3d_set_angular_limits(
        angular_joint, rt_vec3_new(-1.0, -1.0, -1.0), rt_vec3_new(1.0, 1.0, 1.0));
    rt_body3d_set_position(c, 0.0, 0.0, 0.0);
    rt_body3d_set_position(d, 3.0, 0.0, 0.0);
    rt_body3d_set_orientation(d, rt_quat_from_axis_angle(rt_vec3_new(1.0, 0.0, 0.0), 1.5));
    rt_world3d_add(world2, c);
    rt_world3d_add(world2, d);
    rt_world3d_add_joint(world2, angular_joint, RT_JOINT_SIXDOF);
    rt_world3d_step(world2, 1.0 / 60.0);

    void *rot_d = rt_body3d_get_orientation(d);
    EXPECT_TRUE(fabs(quat_rotation_component(rot_d, 0)) <= 1.05,
                "SixDofJoint3D SetAngularLimits clamps relative pose angle");

    EXPECT_TRUE(expect_trap_contains(
                    [&]() { rt_sixdof_joint3d_set_linear_limits(angular_joint, nullptr, nullptr); },
                    "min and max must be finite Vec3 values"),
                "SixDofJoint3D.SetLinearLimits rejects invalid Vec3 handles");
}

static void test_sixdof_joint_angular_pose_limits_hold_against_spin() {
    void *world = rt_world3d_new(0, 0, 0);
    void *base = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *arm = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    rt_body3d_set_collision_layer(base, 1);
    rt_body3d_set_collision_mask(base, 1);
    rt_body3d_set_collision_layer(arm, 2);
    rt_body3d_set_collision_mask(arm, 2);
    void *joint = rt_sixdof_joint3d_new(base, arm, rt_mat4_identity(), rt_mat4_identity());
    rt_sixdof_joint3d_set_angular_limits(
        joint, rt_vec3_new(-0.25, 0.0, 0.0), rt_vec3_new(0.25, 0.0, 0.0));
    rt_body3d_set_angular_velocity(arm, 4.0, 1.5, -1.0);
    rt_world3d_add(world, base);
    rt_world3d_add(world, arm);
    rt_world3d_add_joint(world, joint, RT_JOINT_SIXDOF);

    for (int i = 0; i < 90; i++)
        rt_world3d_step(world, 1.0 / 60.0);

    void *rot = rt_body3d_get_orientation(arm);
    void *ang = rt_body3d_get_angular_velocity(arm);
    EXPECT_TRUE(fabs(quat_rotation_component(rot, 0)) <= 0.30,
                "SixDof angular pose limit holds X rotation under sustained spin");
    EXPECT_TRUE(fabs(quat_rotation_component(rot, 1)) <= 0.04,
                "SixDof angular pose lock holds Y rotation");
    EXPECT_TRUE(fabs(quat_rotation_component(rot, 2)) <= 0.04,
                "SixDof angular pose lock holds Z rotation");
    EXPECT_TRUE(fabs(rt_vec3_x(ang)) < 0.20,
                "SixDof angular limit removes velocity at the X pose stop");
    EXPECT_TRUE(fabs(rt_vec3_y(ang)) < 0.05 && fabs(rt_vec3_z(ang)) < 0.05,
                "SixDof angular locks remove off-axis spin");
}

static void test_sixdof_joint_linear_motor_preserves_angular_pose_limits() {
    void *world = rt_world3d_new(0, 0, 0);
    void *base = rt_body3d_new_sphere(0.25, 0.0);
    void *slider = rt_body3d_new_sphere(0.25, 1.0);
    rt_body3d_set_collision_layer(base, 1);
    rt_body3d_set_collision_mask(base, 1);
    rt_body3d_set_collision_layer(slider, 2);
    rt_body3d_set_collision_mask(slider, 2);
    void *joint = rt_sixdof_joint3d_new(base, slider, rt_mat4_identity(), rt_mat4_identity());
    rt_sixdof_joint3d_set_linear_limits(
        joint, rt_vec3_new(-10.0, 0.0, 0.0), rt_vec3_new(10.0, 0.0, 0.0));
    rt_sixdof_joint3d_set_angular_limits(
        joint, rt_vec3_new(-0.15, 0.0, 0.0), rt_vec3_new(0.15, 0.0, 0.0));
    rt_sixdof_joint3d_set_linear_motor(joint, 1, rt_vec3_new(1.5, 0.0, 0.0), 10.0);
    rt_body3d_set_angular_velocity(slider, 3.0, 1.0, 0.0);
    rt_world3d_add(world, base);
    rt_world3d_add(world, slider);
    rt_world3d_add_joint(world, joint, RT_JOINT_SIXDOF);

    for (int i = 0; i < 60; i++)
        rt_world3d_step(world, 1.0 / 60.0);

    void *vel = rt_body3d_get_velocity(slider);
    void *rot = rt_body3d_get_orientation(slider);
    EXPECT_NEAR(
        rt_vec3_x(vel), 1.5, 0.1, "SixDof linear motor keeps driving while angular limits solve");
    EXPECT_TRUE(fabs(quat_rotation_component(rot, 0)) <= 0.20,
                "SixDof angular pose limit remains stable with an active linear motor");
    EXPECT_TRUE(fabs(quat_rotation_component(rot, 1)) <= 0.04,
                "SixDof angular pose lock remains stable with an active linear motor");
}

static void test_joint_type_validation_for_new_joint_classes() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.25, 1.0);
    void *b = rt_body3d_new_sphere(0.25, 1.0);
    void *anchor = rt_vec3_new(0.0, 0.0, 0.0);
    void *axis = rt_vec3_new(0.0, 1.0, 0.0);
    void *hinge = rt_hinge_joint3d_new(a, b, anchor, axis);
    EXPECT_TRUE(expect_trap_contains([&]() { rt_world3d_add_joint(world, hinge, RT_JOINT_ROPE); },
                                     "joint object does not match joint type"),
                "World.AddJoint rejects mismatched new joint type tags");
    EXPECT_TRUE(expect_trap_contains(
                    [&]() { rt_hinge_joint3d_new(a, b, anchor, rt_vec3_new(0.0, 0.0, 0.0)); },
                    "axis must be a non-zero finite Vec3"),
                "HingeJoint3D rejects zero axes");
}

static double spring_velocity_after_solver_iterations(int64_t iterations) {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(a, 0, 0, 0);
    rt_body3d_set_position(b, 10, 0, 0);
    rt_world3d_set_solver_iterations(world, iterations);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_add_joint(world, rt_spring_joint3d_new(a, b, 3.0, 10.0, 0.0), RT_JOINT_SPRING);
    rt_world3d_step(world, 1.0 / 60.0);
    void *velocity = rt_body3d_get_velocity(b);
    return fabs(rt_vec3_x(velocity));
}

/* Top-box height of a 3-box stack after a short awake settling window. In a
 * sequential-impulse solver support propagates up a stack one contact per
 * Gauss-Seidel sweep, so more iterations let the stack carry its own weight with
 * less compression: the top box settles higher. (The warm-started solver
 * decouples positional correction from iteration count, so this measures the
 * iterative support of a coupled stack, not the old per-iteration Baumgarte
 * push.) */
static double stack_top_height_after_solver_iterations(int64_t iterations) {
    void *world = rt_world3d_new(0.0, -9.8, 0.0);
    rt_world3d_set_solver_iterations(world, iterations);
    void *floor = rt_body3d_new_aabb(20.0, 0.5, 20.0, 0.0);
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0);
    rt_world3d_add(world, floor);
    void *boxes[3];
    for (int i = 0; i < 3; ++i) {
        boxes[i] = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
        rt_body3d_set_restitution(boxes[i], 0.0);
        rt_body3d_set_position(boxes[i], 0.0, 0.5 + (double)i, 0.0);
        rt_world3d_add(world, boxes[i]);
    }
    for (int step = 0; step < 8; ++step) /* awake settling window (pre-sleep) */
        rt_world3d_step(world, 1.0 / 60.0);
    return rt_vec3_y(rt_body3d_get_position(boxes[2])); /* top box */
}

typedef struct {
    double position[3];
    double velocity[3];
    int64_t steps;
    int64_t dropped_steps;
    double alpha;
} StepFixedSnapshot;

static StepFixedSnapshot run_step_fixed_sequence(const double *frames,
                                                 int frame_count,
                                                 int cycles) {
    StepFixedSnapshot snapshot = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, 0, 0, 0.0};
    const double fixed_dt = 1.0 / 60.0;
    void *world = rt_world3d_new(0.0, -9.8, 0.0);
    void *floor = rt_body3d_new_aabb(20.0, 0.5, 20.0, 0.0);
    void *body = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0);
    rt_world3d_add(world, floor);
    rt_body3d_set_position(body, 0.0, 4.0, 0.0);
    rt_body3d_set_velocity(body, 0.15, 0.0, -0.05);
    rt_world3d_add(world, body);

    for (int c = 0; c < cycles; ++c) {
        for (int i = 0; i < frame_count; ++i)
            snapshot.steps += rt_world3d_step_fixed(world, frames[i], fixed_dt, 8);
    }

    void *pos = rt_body3d_get_position(body);
    void *vel = rt_body3d_get_velocity(body);
    snapshot.position[0] = rt_vec3_x(pos);
    snapshot.position[1] = rt_vec3_y(pos);
    snapshot.position[2] = rt_vec3_z(pos);
    snapshot.velocity[0] = rt_vec3_x(vel);
    snapshot.velocity[1] = rt_vec3_y(vel);
    snapshot.velocity[2] = rt_vec3_z(vel);
    snapshot.dropped_steps = rt_world3d_get_dropped_fixed_steps(world);
    snapshot.alpha = rt_world3d_get_fixed_step_alpha(world);
    return snapshot;
}

static void test_world_step_fixed_accumulator_and_determinism() {
    const double uniform_frame = 1.0 / 60.0;
    const double variable_frames[] = {0.007, 0.013, 0.016};
    StepFixedSnapshot uniform = run_step_fixed_sequence(&uniform_frame, 1, 108);
    StepFixedSnapshot variable = run_step_fixed_sequence(variable_frames, 3, 50);

    EXPECT_TRUE(uniform.steps == 108, "StepFixed: uniform frames run expected fixed steps");
    EXPECT_TRUE(variable.steps == 108, "StepFixed: 7/13/16ms frames run expected fixed steps");
    EXPECT_TRUE(uniform.dropped_steps == 0 && variable.dropped_steps == 0,
                "StepFixed: normal frame slices do not drop steps");
    EXPECT_NEAR(uniform.alpha, 0.0, 0.000001, "StepFixed: uniform sequence has no remainder");
    EXPECT_NEAR(variable.alpha, 0.0, 0.000001, "StepFixed: variable sequence has no remainder");
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(variable.position[i],
                    uniform.position[i],
                    1e-9,
                    "StepFixed: variable and uniform final positions match");
        EXPECT_NEAR(variable.velocity[i],
                    uniform.velocity[i],
                    1e-9,
                    "StepFixed: variable and uniform final velocities match");
    }

    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    int64_t steps = rt_world3d_step_fixed(world, 0.007, 0.01, 8);
    EXPECT_TRUE(steps == 0, "StepFixed: sub-fixed frame carries remainder");
    EXPECT_NEAR(rt_world3d_get_fixed_step_alpha(world),
                0.7,
                0.000001,
                "StepFixed: alpha reports carried remainder");
    steps = rt_world3d_step_fixed(world, 0.003, 0.01, 8);
    EXPECT_TRUE(steps == 1, "StepFixed: accumulated remainder completes one step");
    EXPECT_NEAR(rt_world3d_get_fixed_step_alpha(world),
                0.0,
                0.000001,
                "StepFixed: alpha resets after exact fixed step");

    steps = rt_world3d_step_fixed(world, 0.25, 0.015625, 8);
    EXPECT_TRUE(steps == 8, "StepFixed: maxSteps caps a large frame");
    EXPECT_TRUE(rt_world3d_get_dropped_fixed_steps(world) == 8,
                "StepFixed: maxSteps increments dropped fixed step counter");
    EXPECT_NEAR(rt_world3d_get_fixed_step_alpha(world),
                0.0,
                0.000001,
                "StepFixed: dropped overflow clears remainder after capped frame");
}

/// @brief Verify failed world steps restore simulation/event state and retain fixed time.
/// @details The deterministic hook fires only after integration has changed body
///   payloads. The test first establishes a public collision event, then proves a
///   failed direct Step restores both bodies, every visible count, contact bytes,
///   force/torque accumulators, query revision, and the cached event object. A
///   second world proves StepFixed leaves its quantum queued and can drain it with
///   a zero-delta retry after the failure condition is removed.
static void test_world_step_failure_is_atomic_and_retryable() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *floor = rt_body3d_new_aabb(3.0, 0.5, 3.0, 0.0);
    void *body = rt_body3d_new_aabb(0.5, 0.5, 0.5, 1.0);
    rt_body3d_set_position(floor, 0.0, -0.5, 0.0);
    rt_body3d_set_position(body, 0.0, 0.4, 0.0);
    rt_world3d_add(world, floor);
    rt_world3d_add(world, body);
    rt_world3d_step(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_world3d_get_collision_count(world) > 0,
                "Atomic-step fixture establishes a committed collision");
    void *cached_event_before = rt_world3d_get_collision_event(world, 0);
    rt_body3d_apply_force(body, 3.0, 4.0, 5.0);
    rt_body3d_apply_torque(body, 0.5, 1.0, 1.5);

    auto *world_view = static_cast<rt_world3d *>(world);
    auto *floor_view = static_cast<rt_body3d *>(floor);
    auto *body_view = static_cast<rt_body3d *>(body);
    rt_body3d floor_before;
    rt_body3d body_before;
    rt_contact3d contact_before;
    memcpy(&floor_before, floor_view, sizeof(floor_before));
    memcpy(&body_before, body_view, sizeof(body_before));
    memcpy(&contact_before, &world_view->contacts[0], sizeof(contact_before));
    int32_t contact_count_before = world_view->contact_count;
    int32_t previous_count_before = world_view->previous_contact_count;
    int32_t enter_count_before = world_view->enter_event_count;
    int32_t stay_count_before = world_view->stay_event_count;
    int32_t exit_count_before = world_view->exit_event_count;
    uint64_t revision_before = world_view->broadphase_world_revision;

    rt_world3d_test_set_step_failure(1);
    bool trapped =
        expect_trap_contains([&] { rt_world3d_step(world, 0.1); }, "injected transaction failure");
    rt_world3d_test_set_step_failure(0);

    EXPECT_TRUE(trapped, "Injected post-integration failure reaches the public Step trap");
    EXPECT_TRUE(memcmp(floor_view, &floor_before, sizeof(floor_before)) == 0,
                "Failed Step restores the complete static-body payload");
    EXPECT_TRUE(memcmp(body_view, &body_before, sizeof(body_before)) == 0,
                "Failed Step restores pose, velocity, sleep, force, and torque payloads");
    EXPECT_TRUE(world_view->contact_count == contact_count_before &&
                    world_view->previous_contact_count == previous_count_before &&
                    world_view->enter_event_count == enter_count_before &&
                    world_view->stay_event_count == stay_count_before &&
                    world_view->exit_event_count == exit_count_before,
                "Failed Step restores all visible collision and event counts");
    EXPECT_TRUE(memcmp(&world_view->contacts[0], &contact_before, sizeof(contact_before)) == 0,
                "Failed Step restores the prior committed contact bytes");
    EXPECT_TRUE(world_view->broadphase_world_revision == revision_before,
                "Failed Step restores query broadphase revision state");
    EXPECT_TRUE(world_view->step_transaction_active == 0 &&
                    world_view->step_failure_message == nullptr,
                "Failed Step closes its transaction before raising the public trap");
    EXPECT_TRUE(rt_world3d_get_collision_event(world, 0) == cached_event_before,
                "Failed Step retains the cached object for the prior committed event");

    void *fixed_world = rt_world3d_new(0.0, 0.0, 0.0);
    void *fixed_body = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_velocity(fixed_body, 1.0, 0.0, 0.0);
    rt_body3d_set_linear_damping(fixed_body, 0.0);
    rt_world3d_add(fixed_world, fixed_body);
    rt_world3d_test_set_step_failure(1);
    trapped = expect_trap_contains([&] { rt_world3d_step_fixed(fixed_world, 0.1, 0.1, 4); },
                                   "injected transaction failure");
    rt_world3d_test_set_step_failure(0);
    EXPECT_TRUE(trapped, "StepFixed propagates failure only after transactional rollback");
    EXPECT_NEAR(static_cast<rt_world3d *>(fixed_world)->fixed_step_accumulator,
                0.1,
                1e-12,
                "StepFixed retains an uncommitted fixed quantum");
    EXPECT_NEAR(rt_vec3_x(rt_body3d_get_position(fixed_body)),
                0.0,
                0.0,
                "Failed fixed step restores body position");
    int64_t retry_steps = rt_world3d_step_fixed(fixed_world, 0.0, 0.1, 4);
    EXPECT_TRUE(retry_steps == 1, "Zero-delta StepFixed retries retained time exactly once");
    EXPECT_NEAR(rt_vec3_x(rt_body3d_get_position(fixed_body)),
                0.1,
                1e-12,
                "Retried fixed quantum advances the body once");
    EXPECT_NEAR(static_cast<rt_world3d *>(fixed_world)->fixed_step_accumulator,
                0.0,
                1e-12,
                "Successful retry consumes the retained fixed quantum");
}

static void test_world_solver_iteration_controls() {
    void *world = rt_world3d_new(0, 0, 0);
    EXPECT_TRUE(rt_world3d_get_solver_iterations(world) == 6,
                "World: SolverIterations defaults to historical six passes");
    EXPECT_TRUE(rt_world3d_get_position_iterations(world) == 1,
                "World: PositionIterations defaults to historical single pass");
    EXPECT_NEAR(rt_world3d_get_contact_beta(world),
                0.8,
                0.000001,
                "World: ContactBeta defaults to historical beta");
    EXPECT_NEAR(rt_world3d_get_restitution_threshold(world),
                0.5,
                0.000001,
                "World: RestitutionThreshold defaults to historical threshold");
    rt_world3d_set_solver_iterations(world, 0);
    EXPECT_TRUE(rt_world3d_get_solver_iterations(world) == 1,
                "World: SolverIterations setter clamps low values to one");
    rt_world3d_set_solver_iterations(world, 999);
    EXPECT_TRUE(rt_world3d_get_solver_iterations(world) == 64,
                "World: SolverIterations setter clamps high values to max");
    rt_world3d_set_solver_iterations(world, 4);
    EXPECT_TRUE(rt_world3d_get_solver_iterations(world) == 4,
                "World: SolverIterations setter stores valid values");
    rt_world3d_set_position_iterations(world, 0);
    EXPECT_TRUE(rt_world3d_get_position_iterations(world) == 1,
                "World: PositionIterations clamps low values to one");
    rt_world3d_set_position_iterations(world, 999);
    EXPECT_TRUE(rt_world3d_get_position_iterations(world) == 64,
                "World: PositionIterations clamps high values to max");
    rt_world3d_set_position_iterations(world, 5);
    EXPECT_TRUE(rt_world3d_get_position_iterations(world) == 5,
                "World: PositionIterations stores valid values");
    rt_world3d_set_contact_beta(world, -1.0);
    EXPECT_NEAR(
        rt_world3d_get_contact_beta(world), 0.0, 0.000001, "World: ContactBeta clamps low to zero");
    rt_world3d_set_contact_beta(world, 2.0);
    EXPECT_NEAR(
        rt_world3d_get_contact_beta(world), 1.0, 0.000001, "World: ContactBeta clamps high to one");
    rt_world3d_set_contact_beta(world, 0.35);
    EXPECT_NEAR(rt_world3d_get_contact_beta(world),
                0.35,
                0.000001,
                "World: ContactBeta stores valid values");
    rt_world3d_set_restitution_threshold(world, -1.0);
    EXPECT_NEAR(rt_world3d_get_restitution_threshold(world),
                0.0,
                0.000001,
                "World: RestitutionThreshold clamps low to zero");
    rt_world3d_set_restitution_threshold(world, 1.25);
    EXPECT_NEAR(rt_world3d_get_restitution_threshold(world),
                1.25,
                0.000001,
                "World: RestitutionThreshold stores valid values");

    double one_iter_velocity = spring_velocity_after_solver_iterations(1);
    double four_iter_velocity = spring_velocity_after_solver_iterations(4);
    EXPECT_TRUE(four_iter_velocity > one_iter_velocity * 2.0,
                "World: SolverIterations changes iterative spring solving");

    double one_iter_top = stack_top_height_after_solver_iterations(1);
    double eight_iter_top = stack_top_height_after_solver_iterations(8);
    EXPECT_TRUE(eight_iter_top > one_iter_top + 0.001,
                "World: more SolverIterations support a stack with less compression");
}

static void test_world_joint_management() {
    void *world = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(1.0, 1.0);
    void *b = rt_body3d_new_sphere(1.0, 1.0);
    void *j = rt_distance_joint3d_new(a, b, 5.0);

    EXPECT_TRUE(rt_world3d_joint_count(world) == 0, "World: 0 joints initially");
    rt_world3d_add_joint(world, j, RT_JOINT_DISTANCE);
    EXPECT_TRUE(rt_world3d_joint_count(world) == 1, "World: 1 joint after add");
    rt_world3d_remove_joint(world, j);
    EXPECT_TRUE(rt_world3d_joint_count(world) == 0, "World: 0 joints after remove");
}

/* --- Query broadphase cache: solver isolation + fat-AABB hysteresis --- */

/// The per-step solver broadphase re-sorts its entries on the widest-spread
/// axis. When it shared storage with the query cache, a Step could silently
/// reorder a still-valid min-X-sorted query cache and the early break skipped
/// live bodies. Layout: Y spread (1000) >> X spread (100) forces a Y sweep
/// sort whose order is the reverse of the query cache's X order.
static void test_query_broadphase_survives_step_with_nonx_sweep_axis() {
    void *world = rt_world3d_new(0, 0, 0);
    void *body_a = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *body_b = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    rt_body3d_set_position(body_a, 100.0, 0.0, 0.0);
    rt_body3d_set_position(body_b, 0.0, 1000.0, 0.0);
    rt_world3d_add(world, body_a);
    rt_world3d_add(world, body_b);
    void *origin = rt_vec3_new(-2.0, 1000.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 5.0, 1);
        EXPECT_TRUE(hit != nullptr && rt_physics_hit3d_get_body(hit) == body_b,
                    "query cache: body_b hit before Step");
    }
    rt_world3d_step(world, 1.0 / 60.0);
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 5.0, 1);
        EXPECT_TRUE(hit != nullptr && rt_physics_hit3d_get_body(hit) == body_b,
                    "query cache: body_b still hit after a Step with a Y sweep axis");
    }
}

/// A pose change within PH3D_QUERY_BROADPHASE_MARGIN must reuse the cache
/// (no rebuild) while narrow phase still reports the exact new surface.
static void test_query_broadphase_fat_aabb_skips_rebuild() {
    void *world = rt_world3d_new(0, 0, 0);
    void *target = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *bystander = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    rt_body3d_set_position(target, 5.0, 0.0, 0.0);
    rt_body3d_set_position(bystander, 0.0, 20.0, 0.0);
    rt_world3d_add(world, target);
    rt_world3d_add(world, bystander);
    rt_world3d *view = static_cast<rt_world3d *>(world);
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 20.0, 1);
        EXPECT_TRUE(hit != nullptr, "fat aabb: initial hit builds the cache");
    }
    int64_t builds = view->query_broadphase_rebuild_count;
    EXPECT_TRUE(builds >= 1, "fat aabb: rebuild counter advanced on first build");
    rt_body3d_set_position(target, 5.1, 0.0, 0.0); /* well inside the 0.5 margin */
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 20.0, 1);
        EXPECT_TRUE(hit != nullptr, "fat aabb: hit after sub-margin move");
        double d = hit ? rt_physics_hit3d_get_distance(hit) : 0.0;
        EXPECT_TRUE(d > 4.55 && d < 4.65, "fat aabb: narrow phase sees the moved surface (~4.6)");
    }
    EXPECT_TRUE(view->query_broadphase_rebuild_count == builds,
                "fat aabb: sub-margin move reused the cache without a rebuild");
}

/// A pose change that escapes the fattened bounds must rebuild — and the
/// rebuilt cache must place the body at its new position.
static void test_query_broadphase_escape_rebuilds() {
    void *world = rt_world3d_new(0, 0, 0);
    void *target = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    rt_body3d_set_position(target, 5.0, 0.0, 0.0);
    rt_world3d_add(world, target);
    rt_world3d *view = static_cast<rt_world3d *>(world);
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 30.0, 1);
        EXPECT_TRUE(hit != nullptr, "escape: initial hit builds the cache");
    }
    int64_t builds = view->query_broadphase_rebuild_count;
    rt_body3d_set_position(target, 12.0, 0.0, 0.0); /* far outside the margin */
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 30.0, 1);
        EXPECT_TRUE(hit != nullptr, "escape: hit after escaping move");
        double d = hit ? rt_physics_hit3d_get_distance(hit) : 0.0;
        EXPECT_TRUE(d > 11.45 && d < 11.55, "escape: hit at the new surface (~11.5)");
    }
    EXPECT_TRUE(view->query_broadphase_rebuild_count == builds + 1,
                "escape: escaping move forced exactly one rebuild");
}

/// Membership changes (add/remove) must rebuild and be reflected in results.
static void test_query_broadphase_membership_rebuilds() {
    void *world = rt_world3d_new(0, 0, 0);
    void *first = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    rt_body3d_set_position(first, 8.0, 0.0, 0.0);
    rt_world3d_add(world, first);
    rt_world3d *view = static_cast<rt_world3d *>(world);
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 20.0, 1);
        EXPECT_TRUE(hit != nullptr && rt_physics_hit3d_get_body(hit) == first,
                    "membership: first body hit initially");
    }
    int64_t builds = view->query_broadphase_rebuild_count;
    void *second = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    rt_body3d_set_position(second, 4.0, 0.0, 0.0);
    rt_world3d_add(world, second);
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 20.0, 1);
        EXPECT_TRUE(hit != nullptr && rt_physics_hit3d_get_body(hit) == second,
                    "membership: newly added nearer body is hit");
    }
    EXPECT_TRUE(view->query_broadphase_rebuild_count > builds, "membership: add forced a rebuild");
    builds = view->query_broadphase_rebuild_count;
    rt_world3d_remove(world, second);
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 20.0, 1);
        EXPECT_TRUE(hit != nullptr && rt_physics_hit3d_get_body(hit) == first,
                    "membership: removed body no longer hit");
    }
    EXPECT_TRUE(view->query_broadphase_rebuild_count > builds,
                "membership: remove forced a rebuild");
}

/// Shape changes (scale) and large rotations must be reflected in results.
static void test_query_broadphase_shape_change_rebuilds() {
    void *world = rt_world3d_new(0, 0, 0);
    void *box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    rt_body3d_set_position(box, 5.0, 0.0, 0.0);
    rt_world3d_add(world, box);
    rt_world3d *view = static_cast<rt_world3d *>(world);
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 20.0, 1);
        double d = hit ? rt_physics_hit3d_get_distance(hit) : 0.0;
        EXPECT_TRUE(hit != nullptr && d > 4.45 && d < 4.55, "shape: initial surface at ~4.5");
    }
    int64_t builds = view->query_broadphase_rebuild_count;
    rt_body3d_set_scale(box, 2.0, 2.0, 2.0);
    {
        void *hit = rt_world3d_raycast(world, origin, dir, 20.0, 1);
        double d = hit ? rt_physics_hit3d_get_distance(hit) : 0.0;
        EXPECT_TRUE(hit != nullptr && d > 3.95 && d < 4.05, "shape: scaled surface at ~4.0");
    }
    EXPECT_TRUE(view->query_broadphase_rebuild_count > builds, "shape: scale forced a rebuild");

    /* A long thin box whose AABB swings far past the margin when rotated. */
    void *plank = rt_body3d_new_aabb(3.0, 0.2, 0.2, 0.0);
    rt_body3d_set_position(plank, 0.0, 10.0, 20.0);
    rt_world3d_add(world, plank);
    void *up_origin = rt_vec3_new(0.0, 0.0, 20.0);
    void *up_dir = rt_vec3_new(0.0, 1.0, 0.0);
    {
        void *hit = rt_world3d_raycast(world, up_origin, up_dir, 30.0, 1);
        double d = hit ? rt_physics_hit3d_get_distance(hit) : 0.0;
        EXPECT_TRUE(hit != nullptr && d > 9.75 && d < 9.85, "rotation: flat plank hit at ~9.8");
    }
    void *quat = rt_quat_new(0.0, 0.0, 0.70710678118654752, 0.70710678118654752);
    rt_body3d_set_orientation(plank, quat); /* 90 deg about Z: AABB swings to +-3 in Y */
    {
        void *hit = rt_world3d_raycast(world, up_origin, up_dir, 30.0, 1);
        double d = hit ? rt_physics_hit3d_get_distance(hit) : 0.0;
        EXPECT_TRUE(hit != nullptr && d > 6.95 && d < 7.05, "rotation: rotated plank hit at ~7.0");
    }
}

/// Mixed move/step sequence cross-checked over every query flavor.
static void test_query_broadphase_flavors_after_mixed_moves() {
    void *world = rt_world3d_new(0, 0, 0);
    void *near_box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *mid_box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    void *far_box = rt_body3d_new_aabb(0.5, 0.5, 0.5, 0.0);
    rt_body3d_set_position(near_box, 3.0, 0.0, 0.0);
    rt_body3d_set_position(mid_box, 6.0, 0.0, 0.0);
    rt_body3d_set_position(far_box, 9.0, 0.0, 0.0);
    rt_world3d_add(world, near_box);
    rt_world3d_add(world, mid_box);
    rt_world3d_add(world, far_box);
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    {
        void *hits = rt_world3d_raycast_all(world, origin, dir, 30.0, 1);
        EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) == 3,
                    "flavors: three hits before moves");
    }
    rt_body3d_set_position(mid_box, 6.2, 0.0, 0.0);  /* sub-margin */
    rt_body3d_set_position(far_box, 11.0, 0.0, 0.0); /* escape */
    rt_world3d_step(world, 1.0 / 60.0);
    {
        void *hits = rt_world3d_raycast_all(world, origin, dir, 30.0, 1);
        EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) == 3,
                    "flavors: three hits after mixed moves and a Step");
        if (hits && rt_physics_hit_list3d_get_count(hits) == 3) {
            void *hit2 = rt_physics_hit_list3d_get(hits, 2);
            double d2 = rt_physics_hit3d_get_distance(hit2);
            EXPECT_TRUE(rt_physics_hit3d_get_body(hit2) == far_box && d2 > 10.45 && d2 < 10.55,
                        "flavors: escaped body reported at its new surface (~10.5)");
        }
    }
    {
        void *center = rt_vec3_new(6.2, 0.0, 0.0);
        void *hits = rt_world3d_overlap_sphere(world, center, 1.0, 1);
        EXPECT_TRUE(hits != nullptr && rt_physics_hit_list3d_get_count(hits) == 1 &&
                        rt_physics_hit3d_get_body(rt_physics_hit_list3d_get(hits, 0)) == mid_box,
                    "flavors: overlap sphere finds the sub-margin-moved body at its live position");
    }
}

/* --- Vehicle3D: raycast car on a dynamic chassis --- */

/// Shared rig: big static ground slab (top at y=0), 1200 kg box chassis with
/// four suspension wheels at the corners (fronts steer, all driven).
static void *vehicle_test_rig(void **out_world, void **out_chassis) {
    void *w = rt_world3d_new(0.0, -9.8, 0.0);
    void *ground = rt_body3d_new_aabb(100.0, 0.5, 100.0, 0.0);
    void *chassis = rt_body3d_new_aabb(1.0, 0.4, 2.0, 1200.0);
    rt_body3d_set_position(ground, 0.0, -0.5, 0.0);
    rt_body3d_set_position(chassis, 0.0, 1.0, 0.0);
    rt_world3d_add(w, ground);
    rt_world3d_add(w, chassis);
    void *vehicle = rt_vehicle3d_new(w, chassis);
    /* anchors near the chassis floor; rest 0.5, radius 0.3, k tuned so the
     * equilibrium compression is ~0.25 (load 2940 N / 12000 N/m). */
    rt_vehicle3d_add_wheel(vehicle, -0.9, -0.3, 1.5, 0.3, 0.5, 12000.0, 1900.0, 1, 1);
    rt_vehicle3d_add_wheel(vehicle, 0.9, -0.3, 1.5, 0.3, 0.5, 12000.0, 1900.0, 1, 1);
    rt_vehicle3d_add_wheel(vehicle, -0.9, -0.3, -1.5, 0.3, 0.5, 12000.0, 1900.0, 0, 1);
    rt_vehicle3d_add_wheel(vehicle, 0.9, -0.3, -1.5, 0.3, 0.5, 12000.0, 1900.0, 0, 1);
    if (out_world)
        *out_world = w;
    if (out_chassis)
        *out_chassis = chassis;
    return vehicle;
}

static void test_vehicle_settles_on_suspension() {
    void *w = nullptr;
    void *chassis = nullptr;
    void *vehicle = vehicle_test_rig(&w, &chassis);
    EXPECT_TRUE(vehicle != nullptr, "vehicle created");
    EXPECT_TRUE(rt_vehicle3d_get_wheel_count(vehicle) == 4, "four wheels added");
    for (int i = 0; i < 240; i++) {
        rt_vehicle3d_step(vehicle, 1.0 / 60.0);
        rt_world3d_step(w, 1.0 / 60.0);
    }
    for (int64_t i = 0; i < 4; i++)
        EXPECT_TRUE(rt_vehicle3d_wheel_in_contact(vehicle, i) == 1,
                    "settled vehicle keeps every wheel in ground contact");
    {
        void *pos = rt_body3d_get_position(chassis);
        double y = rt_vec3_y(pos);
        EXPECT_TRUE(y > 0.6 && y < 1.1, "chassis settles onto the suspension (~0.85)");
    }
    {
        void *rot = rt_body3d_get_orientation(chassis);
        EXPECT_TRUE(std::fabs(rt_quat_x(rot)) < 0.1 && std::fabs(rt_quat_z(rot)) < 0.1,
                    "settled chassis stays upright");
    }
    {
        double load = rt_vehicle3d_wheel_load(vehicle, 0);
        EXPECT_TRUE(load > 1500.0 && load < 4500.0,
                    "per-wheel suspension load is near the static share (~2940 N)");
    }
}

static void test_vehicle_drives_and_steers() {
    void *w = nullptr;
    void *chassis = nullptr;
    void *vehicle = vehicle_test_rig(&w, &chassis);
    /* Settle first so the drive phase starts from rest contact. */
    for (int i = 0; i < 120; i++) {
        rt_vehicle3d_step(vehicle, 1.0 / 60.0);
        rt_world3d_step(w, 1.0 / 60.0);
    }
    rt_vehicle3d_set_input(vehicle, 1.0, 0.0, 0.0);
    for (int i = 0; i < 120; i++) {
        rt_vehicle3d_step(vehicle, 1.0 / 60.0);
        rt_world3d_step(w, 1.0 / 60.0);
    }
    {
        void *pos = rt_body3d_get_position(chassis);
        EXPECT_TRUE(rt_vehicle3d_get_speed(vehicle) > 2.0,
                    "full throttle accelerates the vehicle forward");
        EXPECT_TRUE(rt_vec3_z(pos) > 2.0, "vehicle advanced along its forward axis");
    }
    {
        double x_before;
        {
            void *pos = rt_body3d_get_position(chassis);
            x_before = rt_vec3_x(pos);
        }
        rt_vehicle3d_set_input(vehicle, 1.0, 0.0, 1.0);
        for (int i = 0; i < 120; i++) {
            rt_vehicle3d_step(vehicle, 1.0 / 60.0);
            rt_world3d_step(w, 1.0 / 60.0);
        }
        void *pos = rt_body3d_get_position(chassis);
        EXPECT_TRUE(std::fabs(rt_vec3_x(pos) - x_before) > 0.5,
                    "steering input curves the vehicle's path");
    }
    /* Brake to (near) rest. */
    rt_vehicle3d_set_input(vehicle, 0.0, 1.0, 0.0);
    for (int i = 0; i < 240; i++) {
        rt_vehicle3d_step(vehicle, 1.0 / 60.0);
        rt_world3d_step(w, 1.0 / 60.0);
    }
    EXPECT_TRUE(std::fabs(rt_vehicle3d_get_speed(vehicle)) < 1.0,
                "full brake brings the vehicle near rest");
}

static void test_vehicle_validates_membership_and_repairs_private_counts() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *unregistered = rt_body3d_new_aabb(1.0, 0.5, 2.0, 100.0);
    EXPECT_TRUE(expect_trap_contains([&] { rt_vehicle3d_new(world, unregistered); },
                                     "dynamic body already added"),
                "vehicle rejects an unregistered chassis");

    void *static_chassis = rt_body3d_new_aabb(1.0, 0.5, 2.0, 0.0);
    rt_world3d_add(world, static_chassis);
    EXPECT_TRUE(expect_trap_contains([&] { rt_vehicle3d_new(world, static_chassis); },
                                     "dynamic body already added"),
                "vehicle rejects a static chassis");

    void *chassis = rt_body3d_new_aabb(1.0, 0.5, 2.0, 100.0);
    rt_world3d_add(world, chassis);
    auto *vehicle = static_cast<VehicleTestLayout *>(rt_vehicle3d_new(world, chassis));
    EXPECT_TRUE(vehicle != nullptr, "vehicle accepts a registered dynamic chassis");

    vehicle->wheel_count = -123;
    EXPECT_TRUE(rt_vehicle3d_get_wheel_count(vehicle) == 0,
                "negative private wheel count repairs to zero");
    EXPECT_TRUE(
        rt_vehicle3d_add_wheel(vehicle, 0.0, -0.25, 0.0, 0.25, 0.75, 10000.0, 1000.0, 0, 1) == 0,
        "wheel packing resumes safely after a negative count repair");
    vehicle->wheel_count = INT32_MAX;
    EXPECT_TRUE(rt_vehicle3d_get_wheel_count(vehicle) == 8,
                "oversized private wheel count clamps to fixed storage");
    EXPECT_TRUE(rt_vehicle3d_add_wheel(vehicle, 0.0, 0.0, 0.0, 0.25, 0.75, 10000.0, 1000.0, 0, 0) ==
                    -1,
                "full repaired wheel storage rejects another wheel");

    auto *body = static_cast<rt_body3d *>(chassis);
    body->velocity[2] = NAN;
    EXPECT_TRUE(rt_vehicle3d_get_speed(vehicle) == 0.0,
                "non-finite chassis velocity cannot escape the speed getter");
    body->velocity[2] = 0.0;
    vehicle->wheel_count = 1;
    vehicle->wheels[0].in_contact = 1;
    vehicle->wheels[0].contact_load = 123.0;
    rt_world3d_remove(world, chassis);
    rt_vehicle3d_step(vehicle, 1.0 / 60.0);
    EXPECT_TRUE(rt_vehicle3d_wheel_in_contact(vehicle, 0) == 0 &&
                    rt_vehicle3d_wheel_load(vehicle, 0) == 0.0,
                "removing the chassis clears stale wheel observations");
}

static void test_vehicle_uses_relative_contact_motion_and_reaction_forces() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *platform = rt_body3d_new_aabb(5.0, 0.5, 5.0, 1000.0);
    void *chassis = rt_body3d_new_aabb(1.0, 0.5, 2.0, 100.0);
    rt_body3d_set_position(platform, 0.0, -0.5, 0.0);
    rt_body3d_set_position(chassis, 0.0, 1.0, 0.0);
    rt_body3d_set_velocity(platform, 0.0, 1.0, 2.0);
    rt_body3d_set_velocity(chassis, 0.0, 1.0, 2.0);
    rt_world3d_add(world, platform);
    rt_world3d_add(world, chassis);
    void *vehicle = rt_vehicle3d_new(world, chassis);
    rt_vehicle3d_add_wheel(vehicle, 0.0, -0.25, 0.0, 0.25, 0.75, 10000.0, 1000.0, 0, 1);
    rt_vehicle3d_set_input(vehicle, 0.0, 1.0, 0.0);
    rt_vehicle3d_step(vehicle, 1.0 / 60.0);

    auto *chassis_body = static_cast<rt_body3d *>(chassis);
    auto *platform_body = static_cast<rt_body3d *>(platform);
    EXPECT_TRUE(rt_vehicle3d_wheel_in_contact(vehicle, 0) == 1,
                "wheel ray contacts a dynamic platform");
    EXPECT_NEAR(rt_vehicle3d_wheel_load(vehicle, 0),
                2500.0,
                1.0,
                "matched vertical platform motion contributes no artificial damping");
    EXPECT_NEAR(chassis_body->force[1],
                -platform_body->force[1],
                1e-9,
                "dynamic platform receives the opposite suspension force");
    EXPECT_NEAR(chassis_body->force[2],
                0.0,
                1e-9,
                "matched platform forward motion contributes no artificial brake slip");
    EXPECT_NEAR(platform_body->force[2],
                0.0,
                1e-9,
                "matched forward motion leaves the platform without a brake reaction");

    std::memset(chassis_body->force, 0, sizeof(chassis_body->force));
    std::memset(chassis_body->torque, 0, sizeof(chassis_body->torque));
    std::memset(platform_body->force, 0, sizeof(platform_body->force));
    std::memset(platform_body->torque, 0, sizeof(platform_body->torque));
    rt_vehicle3d_set_input(vehicle, 1.0, 0.0, 0.0);
    rt_vehicle3d_step(vehicle, 1.0 / 60.0);
    EXPECT_TRUE(std::fabs(chassis_body->force[2]) > 1000.0,
                "driven wheel applies a longitudinal tire force");
    EXPECT_NEAR(chassis_body->force[2],
                -platform_body->force[2],
                1e-9,
                "dynamic platform receives the opposite tire force");

    std::memset(chassis_body->force, 0, sizeof(chassis_body->force));
    std::memset(chassis_body->torque, 0, sizeof(chassis_body->torque));
    std::memset(platform_body->force, 0, sizeof(platform_body->force));
    std::memset(platform_body->torque, 0, sizeof(platform_body->torque));
    rt_body3d_set_velocity(platform, 0.0, 0.0, 0.0);
    rt_body3d_set_velocity(chassis, 10.0, 0.0, 0.0);
    rt_vehicle3d_set_drive_force(vehicle, 12000.0);
    rt_vehicle3d_set_grip(vehicle, 1.0, 4.0);
    rt_vehicle3d_step(vehicle, 1.0 / 60.0);
    double load = rt_vehicle3d_wheel_load(vehicle, 0);
    double ellipse =
        std::hypot(chassis_body->force[2] / load, chassis_body->force[0] / (4.0 * load));
    EXPECT_TRUE(std::isfinite(ellipse) && ellipse <= 1.000001,
                "asymmetric longitudinal/lateral grip obeys its friction ellipse");
}

/* --- Joint wake + sleep gating (joint3d_pair_begin_solve) --- */

static void test_joint_wakes_sleeping_partner() {
    void *w = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 2.0, 0.0, 0.0);
    rt_world3d_add(w, a);
    rt_world3d_add(w, b);
    void *joint = rt_distance_joint3d_new(a, b, 2.0);
    rt_world3d_add_joint(w, joint, RT_JOINT_DISTANCE);
    rt_body3d_sleep(a);
    rt_body3d_sleep(b);

    /* Teleporting one endpoint must re-activate its sleeping joint partner
     * (world3d_wake_joint_partners), and the joint must then pull the pair
     * back to the target distance instead of leaving the sleeper frozen. */
    rt_body3d_set_position(a, -3.0, 0.0, 0.0);
    EXPECT_TRUE(rt_body3d_is_sleeping(b) == 0, "teleporting A wakes sleeping joint partner B");
    for (int i = 0; i < 60; i++)
        rt_world3d_step(w, 1.0 / 60.0);
    {
        void *pa = rt_body3d_get_position(a);
        void *pb = rt_body3d_get_position(b);
        double dx = rt_vec3_x(pb) - rt_vec3_x(pa);
        double dy = rt_vec3_y(pb) - rt_vec3_y(pa);
        double dz = rt_vec3_z(pb) - rt_vec3_z(pa);
        double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        EXPECT_TRUE(std::fabs(dist - 2.0) < 0.25,
                    "woken pair re-solves the distance joint (dist ~= 2)");
    }
}

static void test_fully_sleeping_joint_pair_stays_asleep() {
    void *w = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 2.0, 0.0, 0.0);
    rt_world3d_add(w, a);
    rt_world3d_add(w, b);
    void *joint = rt_distance_joint3d_new(a, b, 2.0);
    rt_world3d_add_joint(w, joint, RT_JOINT_DISTANCE);
    rt_body3d_sleep(a);
    rt_body3d_sleep(b);
    /* A satisfied joint between two sleeping bodies must not wake them:
     * the solve is skipped entirely (no drift, sleep preserved). */
    for (int i = 0; i < 30; i++)
        rt_world3d_step(w, 1.0 / 60.0);
    EXPECT_TRUE(rt_body3d_is_sleeping(a) != 0, "satisfied sleeping joint pair: A stays asleep");
    EXPECT_TRUE(rt_body3d_is_sleeping(b) != 0, "satisfied sleeping joint pair: B stays asleep");
}

/* --- SixDof linear limits operate in body A's joint frame --- */

static void test_sixdof_linear_limits_follow_body_a_frame() {
    void *w = rt_world3d_new(0, 0, 0);
    void *a = rt_body3d_new_aabb(0.25, 0.25, 0.25, 0.0); /* static anchor */
    void *free_b = rt_body3d_new_sphere(0.25, 1.0);
    void *locked_b = rt_body3d_new_sphere(0.25, 1.0);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    /* Yaw A by +90 degrees: its local +X now points along world -Z. */
    rt_body3d_set_orientation(a, rt_quat_new(0.0, 0.70710678118654752, 0.0, 0.70710678118654752));
    rt_world3d_add(w, a);

    /* free_b sits along A's local +X (world -Z): within the [0, 2] slider
     * range on local X, so the joint must NOT drag it anywhere. */
    rt_body3d_set_position(free_b, 0.0, 0.0, -1.5);
    rt_world3d_add(w, free_b);
    void *slider = rt_sixdof_joint3d_new(a, free_b, rt_mat4_identity(), rt_mat4_identity());
    rt_sixdof_joint3d_set_linear_limits(
        slider, rt_vec3_new(0.0, 0.0, 0.0), rt_vec3_new(2.0, 0.0, 0.0));
    rt_world3d_add_joint(w, slider, RT_JOINT_SIXDOF);
    for (int i = 0; i < 30; i++)
        rt_world3d_step(w, 1.0 / 60.0);
    {
        void *p = rt_body3d_get_position(free_b);
        EXPECT_TRUE(std::fabs(rt_vec3_z(p) + 1.5) < 0.1,
                    "slider along A's local X leaves in-range body untouched (world -Z)");
    }

    /* locked_b sits along A's local -Z (world -X is local +Z? use world +X =
     * local -Z): local Y/Z are locked ([0,0]), so its offset must be
     * projected out and the body pulled to the anchor axis. */
    rt_body3d_set_position(locked_b, 1.5, 0.0, 0.0);
    rt_world3d_add(w, locked_b);
    void *locked_joint = rt_sixdof_joint3d_new(a, locked_b, rt_mat4_identity(), rt_mat4_identity());
    rt_sixdof_joint3d_set_linear_limits(
        locked_joint, rt_vec3_new(0.0, 0.0, 0.0), rt_vec3_new(2.0, 0.0, 0.0));
    rt_world3d_add_joint(w, locked_joint, RT_JOINT_SIXDOF);
    for (int i = 0; i < 60; i++)
        rt_world3d_step(w, 1.0 / 60.0);
    {
        void *p = rt_body3d_get_position(locked_b);
        EXPECT_TRUE(std::fabs(rt_vec3_x(p)) < 0.2,
                    "locked local-Z axis projects out the world-X offset");
    }
}

/* --- Trigger3D: volume overlap + uncapped tracking --- */

static void test_trigger_detects_straddling_large_body() {
    void *w = rt_world3d_new(0, 0, 0);
    /* Center (2,0,0) is outside the [-1,1] trigger, but the body's AABB
     * spans x in [0.8, 3.2] and overlaps it — must register as inside. */
    void *big = rt_body3d_new_aabb(1.2, 1.2, 1.2, 1.0);
    rt_body3d_set_position(big, 2.0, 0.0, 0.0);
    rt_world3d_add(w, big);
    void *t = rt_trigger3d_new(-1, -1, -1, 1, 1, 1);
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(t) == 1,
                "straddling body's AABB overlap registers an enter");
    rt_body3d_set_position(big, 10.0, 0.0, 0.0);
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_exit_count(t) == 1, "straddling body exit registers");
}

static void test_trigger_tracks_beyond_legacy_cap() {
    void *w = rt_world3d_new(0, 0, 0);
    void *t = rt_trigger3d_new(-10, -10, -10, 10, 10, 10);
    const int kBodies = 96; /* old fixed table capped at 64 */
    for (int i = 0; i < kBodies; i++) {
        void *b = rt_body3d_new_sphere(0.05, 1.0);
        rt_body3d_set_position(b, (double)(i % 10), 0.0, (double)(i / 10));
        rt_world3d_add(w, b);
    }
    rt_trigger3d_update(t, w);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(t) == kBodies,
                "trigger tracks every occupant past the old 64-body cap");
}

static void test_trigger_repairs_tracking_and_zeroes_dead_bodies() {
    void *world = rt_world3d_new(0, 0, 0);
    void *trigger = rt_trigger3d_new(-1, -1, -1, 1, 1, 1);
    Trigger3DTestLayout *view = (Trigger3DTestLayout *)trigger;

    view->tracked_count = 7;
    view->tracked_capacity = 16;
    rt_trigger3d_update(trigger, world);
    EXPECT_TRUE(view->tracked_count == 0 && view->tracked_capacity == 0,
                "trigger repairs missing parallel occupancy arrays");

    void *body = rt_body3d_new_sphere(0.25, 1.0);
    rt_world3d_add(world, body);
    rt_trigger3d_update(trigger, world);
    EXPECT_TRUE(view->tracked_count == 1 && rt_trigger3d_get_enter_count(trigger) == 1,
                "trigger records a managed occupant");
    rt_world3d_remove(world, body);
    (void)rt_memory_release(body);
    EXPECT_TRUE(rt_weak_load(&view->tracked_bodies[0]) == nullptr,
                "destroyed bodies zero their trigger occupancy weak handle");
    rt_trigger3d_update(trigger, world);
    EXPECT_TRUE(view->tracked_count == 0 && rt_trigger3d_get_exit_count(trigger) == 1,
                "zeroed weak occupants prune with one exit edge");

    view->bounds_min[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(rt_trigger3d_contains(trigger, rt_vec3_new(0.0, 0.0, 0.0)) != 0,
                "trigger queries repair corrupt non-finite bounds");
}

static void test_trigger_skips_invalid_and_duplicate_world_slots() {
    void *world = rt_world3d_new(0, 0, 0);
    void *body = rt_body3d_new_sphere(0.25, 1.0);
    void *trigger = rt_trigger3d_new(-1, -1, -1, 1, 1, 1);
    rt_world3d_add(world, body);
    rt_world3d *view = (rt_world3d *)world;

    view->bodies[1] = view->bodies[0];
    view->body_count = 2;
    rt_trigger3d_update(trigger, world);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(trigger) == 1,
                "duplicate world body slots produce one trigger enter edge");

    view->body_count = 1;
    rt_trigger3d_update(trigger, world);
    EXPECT_TRUE(rt_trigger3d_get_enter_count(trigger) == 0 &&
                    rt_trigger3d_get_exit_count(trigger) == 0,
                "removing a duplicate slot does not create a false trigger edge");

    rt_body3d *saved = view->bodies[0];
    view->bodies[0] = (rt_body3d *)rt_vec3_new(0.0, 0.0, 0.0);
    rt_trigger3d_update(trigger, world);
    EXPECT_TRUE(rt_trigger3d_get_exit_count(trigger) == 1,
                "wrong-class world body slots are skipped and prior occupants exit once");
    view->bodies[0] = saved;
}

/* --- CCD per-body catch-up segments (clamped-substep regime) --- */

static void test_ccd_clamped_body_catchup_segments_no_tunnel() {
    void *w = rt_world3d_new(0, 0, 0);
    void *wall = rt_body3d_new_aabb(0.1, 4.0, 4.0, 0.0);
    void *bullet = rt_body3d_new_sphere(0.05, 1.0);
    rt_body3d_set_position(wall, 30.0, 0.0, 0.0);
    rt_body3d_set_position(bullet, 0.0, 0.0, 0.0);
    /* 2000 u/s at dt=0.1 wants ~hundreds of substeps — far past the global
     * cap of 8. The per-body swept segments must still stop it at the wall. */
    rt_body3d_set_velocity(bullet, 2000.0, 0.0, 0.0);
    rt_body3d_set_use_ccd(bullet, 1);
    rt_world3d_add(w, wall);
    rt_world3d_add(w, bullet);
    rt_world3d_step(w, 0.1);
    {
        void *pos = rt_body3d_get_position(bullet);
        EXPECT_TRUE(rt_vec3_x(pos) < 30.5,
                    "clamped-regime CCD bullet is stopped by per-body catch-up segments");
    }
    EXPECT_TRUE(rt_world3d_get_last_ccd_requested_substeps(w) > rt_world3d_get_last_ccd_substeps(w),
                "clamp diagnostics still record the capped demand");
}

/* --- Determinism replay checksum -------------------------------------- */

extern "C" int64_t rt_parallel_default_workers(void);

/// @brief FNV-1a accumulator over raw IEEE-754 bit patterns so replay
///        comparisons are bit-exact, not tolerance-based.
struct ReplayChecksum {
    uint64_t value = 14695981039346656037ULL;

    void add(double d) {
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value ^= (bits >> shift) & 0xffU;
            value *= 1099511628211ULL;
        }
    }
};

struct ReplayResult {
    uint64_t checksum;
    int32_t last_island_count;
    int32_t last_contact_count;
    int pool_created;
};

/// @brief Step a scripted 96-box two-cluster pile and checksum every body's
///        full state each 10 steps. The two clusters are disjoint contact
///        islands, and their combined contact count crosses the parallel
///        solver threshold, so the run exercises the island-dispatch path
///        unless @p force_serial disables the solver pool.
static ReplayResult run_replay_scene(int force_serial) {
    void *world = rt_world3d_new(0.0, -9.81, 0.0);
    if (force_serial)
        ((rt_world3d *)world)->solver_pool_failed = 1;

    void *ground = rt_body3d_new_aabb(60.0, 0.5, 60.0, 0.0);
    rt_body3d_set_position(ground, 0.0, -0.5, 0.0);
    rt_world3d_add(world, ground);

    void *bodies[97];
    int body_count = 0;
    bodies[body_count++] = ground;
    const double cluster_centers[2] = {-16.0, 16.0};
    for (int cluster = 0; cluster < 2; ++cluster) {
        for (int layer = 0; layer < 3; ++layer) {
            for (int gx = 0; gx < 4; ++gx) {
                for (int gz = 0; gz < 4; ++gz) {
                    void *box = rt_body3d_new_aabb(0.45, 0.45, 0.45, 1.0);
                    rt_body3d_set_position(box,
                                           cluster_centers[cluster] + (double)gx * 0.95,
                                           0.46 + (double)layer * 0.95,
                                           (double)gz * 0.95);
                    rt_world3d_add(world, box);
                    bodies[body_count++] = box;
                }
            }
        }
    }

    ReplayChecksum checksum;
    for (int step = 0; step < 240; ++step) {
        rt_world3d_step(world, 1.0 / 60.0);
        if (step % 10 != 9)
            continue;
        for (int i = 0; i < body_count; ++i) {
            const rt_body3d *b = (const rt_body3d *)bodies[i];
            for (int k = 0; k < 3; ++k)
                checksum.add(b->position[k]);
            for (int k = 0; k < 4; ++k)
                checksum.add(b->orientation[k]);
            for (int k = 0; k < 3; ++k)
                checksum.add(b->velocity[k]);
            for (int k = 0; k < 3; ++k)
                checksum.add(b->angular_velocity[k]);
        }
    }

    const rt_world3d *w = (const rt_world3d *)world;
    ReplayResult result;
    result.checksum = checksum.value;
    result.last_island_count = w->last_solver_island_count;
    result.last_contact_count = w->last_solver_contact_count;
    result.pool_created = w->solver_pool != NULL;
    return result;
}

static void test_replay_checksum_is_run_to_run_deterministic() {
    ReplayResult first = run_replay_scene(0);
    ReplayResult second = run_replay_scene(0);
    if (first.checksum != second.checksum)
        printf("replay checksums diverged: 0x%llx vs 0x%llx\n",
               (unsigned long long)first.checksum,
               (unsigned long long)second.checksum);
    EXPECT_TRUE(first.checksum == second.checksum,
                "identical scripted scenes replay to bit-identical state streams");
    EXPECT_TRUE(first.last_island_count >= 2,
                "replay scene settles into at least two solver islands");
}

static void test_replay_checksum_parallel_matches_serial() {
    ReplayResult parallel = run_replay_scene(0);
    ReplayResult serial = run_replay_scene(1);
    /* Guard non-vacuity: the scene must actually cross the parallel-dispatch
     * contact threshold, and the parallel run must have built its pool (on
     * multi-core hosts) while the forced-serial run must not. */
    EXPECT_TRUE(parallel.last_contact_count >= 64,
                "replay scene crosses the parallel solver contact threshold");
    if (rt_parallel_default_workers() >= 2)
        EXPECT_TRUE(parallel.pool_created, "parallel replay run created the island worker pool");
    EXPECT_TRUE(!serial.pool_created, "forced-serial replay run never builds the pool");
    if (parallel.checksum != serial.checksum)
        printf("parallel/serial checksums diverged: 0x%llx vs 0x%llx\n",
               (unsigned long long)parallel.checksum,
               (unsigned long long)serial.checksum);
    EXPECT_TRUE(parallel.checksum == serial.checksum,
                "parallel island dispatch is bit-identical to the serial solver");
}

int main() {
    /* Body creation */
    test_body_new_aabb();
    test_body_new_sphere();
    test_body_new_capsule();
    test_body_static_zero_mass();
    test_body_new_and_set_collider();
    test_wrapper_constructors_assign_colliders();
    test_collider_constructors_sanitize_nonfinite_dimensions();
    test_collider_getters_sanitize_corrupt_private_state();
    test_mesh_collider_attaches_to_static_body();

    /* Property accessors */
    test_body_position();
    test_body_scale_affects_queries();
    test_body_orientation_roundtrip();
    test_body_velocity();
    test_body_far_origin_integrates_sub_float_delta();
    test_body_collision_layer_mask();
    test_body_material_coefficients_are_sanitized();
    test_body_getters_sanitize_corrupt_private_state();
    test_body_sanitizes_nonfinite_motion_state();
    test_body_extreme_motion_state_saturates();
    test_body_extreme_scale_and_offcenter_inputs_remain_finite();
    test_world_pathological_step_and_rebase_are_bounded();
    test_body_trigger();
    test_body_torque_updates_angular_velocity_and_orientation();
    test_body_angular_damping();
    test_body_sleep_and_wake();
    test_body_auto_sleep();
    test_kinematic_body_ignores_gravity_but_integrates_velocity();
    test_ccd_prevents_fast_sphere_tunneling();
    test_ccd_proxy_encloses_rotated_box_corners();
    test_ccd_toi_uses_target_collider_material();
    test_ccd_segmentation_is_body_local();
    test_ccd_clamped_body_catchup_segments_no_tunnel();

    /* Vehicle3D */
    test_vehicle_settles_on_suspension();
    test_vehicle_drives_and_steers();
    test_vehicle_validates_membership_and_repairs_private_counts();
    test_vehicle_uses_relative_contact_motion_and_reaction_forces();

    /* Joint sleep/wake gating */
    test_joint_wakes_sleeping_partner();
    test_fully_sleeping_joint_pair_stays_asleep();
    test_sixdof_linear_limits_follow_body_a_frame();

    /* World */
    test_world_create();
    test_world_add_remove();
    test_world_remove_purges_contacts_for_body();
    test_world_rejects_duplicate_body_adds();
    test_world_body_storage_grows_past_initial_capacity();
    test_world_sparse_body_count_step_stress();
    test_world_contact_storage_grows_past_initial_capacity();
    test_world_broadphase_rejects_separated_bodies();
    test_world_broadphase_keeps_legacy_cached_primitives();
    test_gravity_integration();
    test_force_application();
    test_impulse_application();
    test_impulse_at_point_adds_spin();
    test_force_at_point_adds_spin();
    test_rotated_anisotropic_body_uses_world_inverse_inertia();
    test_exponential_damping_is_substep_partition_invariant();

    /* Collision */
    test_collision_aabb_overlap();
    test_collision_layer_filtering();
    test_trigger_no_push();
    test_ground_detection();
    test_box_stack_rests_stably();
    test_ten_box_tower_stacking_regression();
    test_world_solver_island_batches_resting_pile_target();

    /* Character controller */
    test_character_create();
    test_character_position();
    test_character_step_height();
    test_character_sanitizes_motion_config();
    test_character_world_binding();
    test_character_repairs_private_refs_and_clears_stale_grounding();
    test_character_skips_wrong_class_world_slots();
    test_character_sweeps_platform_motion_and_reports_it_in_velocity();
    test_character_slide_against_wall();
    test_character_step_up();
    test_character_crosses_uneven_walkable_heightfield();

    /* Sphere-sphere collision tests — declared below */

    /* Trigger3D */
    test_trigger_create();
    test_trigger_contains_inside();
    test_trigger_contains_outside();
    test_trigger_enter_detection();
    test_trigger_exit_detection();
    test_trigger_multiple_bodies();
    test_trigger_set_bounds();
    test_trigger_detects_straddling_large_body();
    test_trigger_tracks_beyond_legacy_cap();
    test_trigger_repairs_tracking_and_zeroes_dead_bodies();
    test_trigger_skips_invalid_and_duplicate_world_slots();

    /* Sphere-sphere collision */
    test_sphere_sphere_collision();
    test_sphere_sphere_no_overlap();
    test_aabb_sphere_collision();
    test_capsule_aabb_collision();
    test_rotated_capsule_aabb_collision();
    test_offcenter_contact_generates_angular_velocity();
    test_large_floor_contact_point_stays_near_dynamic_body();
    test_convex_hull_collider_blocks_sphere();
    test_convex_hull_gjk_detects_contained_sphere();
    test_convex_hull_gjk_rejects_aabb_false_positive();
    test_unsupported_mesh_pair_rejects_aabb_false_positive();
    test_convex_hull_gjk_detects_overlapping_hulls();
    test_convex_hull_gjk_handles_box_capsule_and_simple_edge_cases();
    test_convex_hull_gjk_perf_target();
    test_mesh_collider_blocks_falling_sphere();
    test_mesh_collider_narrowphase_builds_bvh_for_sphere_and_box();
    test_mesh_convex_hull_bvh_and_body_broadphase_target();
    test_compound_collider_child_transform_affects_contact();
    test_compound_collider_collects_multiple_leaf_contacts();
    test_compound_collider_rejects_transitive_cycle();
    test_compound_collider_rejects_excessive_nesting();
    test_heightfield_collider_supports_ground_contact();
    test_heightfield_box_samples_bottom_edges();

    /* Collision event queue */
    test_collision_event_count();
    test_collision_event_bodies();
    test_physics_world_event_hit_getters_sanitize_corrupt_private_state();
    test_world_raycast_returns_nearest_hit();
    test_world_raycast_all_sorted();
    test_query_broadphase_survives_step_with_nonx_sweep_axis();
    test_query_broadphase_fat_aabb_skips_rebuild();
    test_query_broadphase_escape_rebuilds();
    test_query_broadphase_membership_rebuilds();
    test_query_broadphase_shape_change_rebuilds();
    test_query_broadphase_flavors_after_mixed_moves();
    test_world_overlap_hit_list_reports_truncation();
    test_world_sweep_sphere_reports_started_penetrating();
    test_world_overlap_queries_honor_mask();
    test_world_overlap_queries_reject_nonfinite_inputs();
    test_world_queries_clamp_extreme_finite_inputs();
    test_world_query_broadphase_cache_invalidates_after_body_move();
    test_world_query_broadphase_cache_invalidates_after_compound_mutation();
    test_world_query_broadphase_cache_invalidates_after_mesh_mutation();
    test_world_rebase_origin_shifts_body_contact_and_query_state();
    test_collision_events_enter_stay_exit();
    test_query_mask_zero_matches_no_layers();
    test_kinematic_static_trigger_contacts_are_reported();
    test_contact_identity_survives_broadphase_order_flip();
    test_ccd_substep_contact_generates_frame_event();
    test_collision_event_surface_and_trigger_flag();
    test_rotated_box_box_exposes_clipped_manifold();

    /* Narrowphase correctness probes (third review pass) */
    test_obb_edge_axis_depth_resolves_separation();
    test_heightfield_box_upside_down_still_contacts();
    test_heightfield_slope_depth_projected_onto_normal();
    test_friction_is_direction_independent();
    test_long_capsule_catches_narrow_ridge();
    test_heightfield_long_raycast_hits_distant_terrain();
    test_box_on_heightfield_gets_support_manifold();
    test_box_on_mesh_floor_gets_support_manifold();
    test_capsule_on_heightfield_gets_lengthwise_manifold();
    test_ccd_dynamic_pair_does_not_tunnel();
    test_tilted_box_settles_without_penetration();

    /* Joint tests */
    test_distance_joint_create();
    test_joints_retain_bodies_and_sanitize_parameters();
    test_joint_readbacks_repair_corrupt_private_state();
    test_distance_joint_owned_bodies_survive_mirror_corruption();
    test_distance_joint_finalizer_releases_owned_bodies_after_mirror_corruption();
    test_joint3d_extreme_finite_inputs_remain_finite();
    test_distance_joint_constraint();
    test_spring_joint_create();
    test_spring_joint_force();
    test_hinge_joint_anchor_constraint();
    test_hinge_joint_motor_drives_rotation();
    test_hinge_joint_get_angle();
    test_hinge_joint_angle_limit();
    test_rope_joint_max_length_constraint();
    test_sixdof_joint_frame_anchor_constraint();
    test_sixdof_joint_limits();
    test_sixdof_joint_angular_pose_limits_hold_against_spin();
    test_sixdof_joint_linear_motor_preserves_angular_pose_limits();
    test_sixdof_joint_linear_motor();
    test_joint_type_validation_for_new_joint_classes();
    test_world_step_fixed_accumulator_and_determinism();
    test_world_step_failure_is_atomic_and_retryable();
    test_replay_checksum_is_run_to_run_deterministic();
    test_replay_checksum_parallel_matches_serial();
    test_world_solver_iteration_controls();
    test_world_joint_management();

    printf("Physics3D tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
