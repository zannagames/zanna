//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_scene3d.cpp
// Purpose: Unit tests for Zanna.Graphics3D.SceneGraph and SceneNode —
//   hierarchy management, TRS transform propagation, dirty flags, and
//   search-by-name.
// Key invariants:
//   - Scene roots cannot be reparented and owner changes are transactional.
//   - Preserve-world reparenting is exact or rejects before any graph mutation.
//   - Complete world-matrix assignment is exact or leaves every local TRS lane unchanged.
//   - World transforms, bounds, culling, persistence, and animation remain deterministic.
//   - Typed node metadata preserves exact kinds and values through VSCN v6.
// Ownership/Lifetime:
//   - Runtime fixtures are managed by the runtime test heap.
//   - White-box payload pointers are borrowed only while the owning fixture is live.
// Links: src/runtime/graphics/3d/scene/rt_scene3d.h, rt_scene3d_internal.h,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md,
//   docs/adr/0161-stable-scenenode-sibling-reordering.md,
//   docs/adr/0166-exact-scenenode-world-matrix-assignment.md,
//   docs/adr/0172-public-scenenode-light-authoring-and-studio-light-inspector.md,
//   docs/adr/0295-portable-vscn-binary-wire-layouts.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt.hpp"
#include "rt_animcontroller3d.h"
#include "rt_asset_error.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_internal.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_option.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_raycast3d.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_skeleton3d.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "vgfx3d_backend.h"
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#define _USE_MATH_DEFINES
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern rt_string rt_const_cstr(const char *s);
extern void *rt_quat_new(double x, double y, double z, double w);
extern void *rt_quat_from_euler(double pitch, double yaw, double roll);
extern int8_t rt_scene_node3d_get_position_components(void *node, double *x, double *y, double *z);
extern int8_t rt_scene_node3d_get_rotation_components(
    void *node, double *x, double *y, double *z, double *w);
extern int8_t rt_scene_node3d_get_world_rotation_components(
    void *node, double *x, double *y, double *z, double *w);
extern int8_t rt_scene_node3d_get_scale_components(void *node, double *x, double *y, double *z);
extern int8_t rt_scene_node3d_get_world_scale_components(void *node,
                                                         double *x,
                                                         double *y,
                                                         double *z);
extern int8_t rt_scene_node3d_get_world_matrix_components(void *node, double out[16]);
extern int8_t rt_scene_node3d_try_add_child_preserve_world(void *parent, void *child);
extern void rt_scene_node3d_set_transform(void *node,
                                          double px,
                                          double py,
                                          double pz,
                                          double qx,
                                          double qy,
                                          double qz,
                                          double qw,
                                          double sx,
                                          double sy,
                                          double sz);
extern void rt_scene_node3d_set_transform_batch(void *nodes, void *values);
extern void *rt_seq_new(void);
extern void rt_seq_push(void *obj, void *val);
extern void *rt_box_f64(double val);
extern double rt_quat_x(void *q);
extern double rt_quat_y(void *q);
extern double rt_quat_z(void *q);
extern double rt_quat_w(void *q);
extern void rt_camera3d_look_at(void *cam, void *eye, void *target, void *up);
extern void *rt_material3d_new_color(double r, double g, double b);
extern void *rt_camera3d_new(double fov, double aspect, double near, double far);
extern void *rt_mesh3d_new(void);
extern void *rt_mesh3d_new_box(double w, double h, double d);
extern int64_t rt_mesh3d_get_resident_bytes(void *obj);
extern void rt_mesh3d_set_resident(void *obj, int8_t resident);
extern void rt_mesh3d_add_vertex(
    void *obj, double x, double y, double z, double nx, double ny, double nz, double u, double v);
extern void rt_mesh3d_add_triangle(void *obj, int64_t i0, int64_t i1, int64_t i2);
extern void rt_scene_node3d_set_lod_resident(void *node, int64_t index, int8_t resident);
extern int8_t rt_scene_node3d_get_lod_resident(void *node, int64_t index);
extern int64_t rt_scene_node3d_get_lod_resident_bytes(void *node, int64_t index);
extern void *rt_scene_node3d_metadata_keys(void *node);
extern rt_string rt_scene_node3d_metadata_kind(void *node, rt_string key);
extern int8_t rt_scene_node3d_metadata_has(void *node, rt_string key);
extern int64_t rt_scene_node3d_metadata_get_int(void *node, rt_string key, int64_t def);
extern double rt_scene_node3d_metadata_get_float(void *node, rt_string key, double def);
extern int8_t rt_scene_node3d_metadata_get_bool(void *node, rt_string key, int8_t def);
extern rt_string rt_scene_node3d_metadata_get_string(void *node, rt_string key, rt_string def);
extern int8_t rt_scene_node3d_metadata_set_null(void *node, rt_string key);
extern int8_t rt_scene_node3d_metadata_set_int(void *node, rt_string key, int64_t value);
extern int8_t rt_scene_node3d_metadata_set_float(void *node, rt_string key, double value);
extern int8_t rt_scene_node3d_metadata_set_bool(void *node, rt_string key, int8_t value);
extern int8_t rt_scene_node3d_metadata_set_string(void *node, rt_string key, rt_string value);
extern int8_t rt_scene_node3d_metadata_remove(void *node, rt_string key);
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_retain_maybe(void *p);
extern int32_t rt_obj_release_check0(void *p);
extern void rt_obj_free(void *p);
extern void rt_scene3d_test_set_precise_hit_growth_failure(int8_t enabled);
}

static int tests_passed = 0;
static int tests_run = 0;
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;
static bool g_return_from_trap = false;

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_return_from_trap)
        return;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
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
        if (fabs((a) - (b)) > (eps)) {                                                             \
            fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", msg, (double)(a), (double)(b));    \
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

static bool read_text_file(const char *path, std::string &out) {
    out.clear();
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    long size = std::ftell(f);
    if (size < 0 || std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)size);
    if (size > 0 && std::fread(out.data(), 1, (size_t)size, f) != (size_t)size) {
        std::fclose(f);
        out.clear();
        return false;
    }
    std::fclose(f);
    return true;
}

static bool write_text_file(const char *path, const char *text) {
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    const size_t len = std::strlen(text);
    const bool ok = len == 0 || std::fwrite(text, 1, len, f) == len;
    const bool closed = std::fclose(f) == 0;
    return ok && closed;
}

static bool replace_first_json_string_value(std::string &text,
                                            const char *field,
                                            const std::string &replacement) {
    const std::string prefix = std::string("\"") + field + "\": \"";
    size_t value_offset = text.find(prefix);
    if (value_offset == std::string::npos)
        return false;
    value_offset += prefix.size();
    const size_t value_end = text.find('"', value_offset);
    if (value_end == std::string::npos)
        return false;
    text.replace(value_offset, value_end - value_offset, replacement);
    return true;
}

static std::string scene_test_base64_encode(const uint8_t *data, size_t len) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output(((len + 2u) / 3u) * 4u, '=');
    size_t input_offset = 0;
    size_t output_offset = 0;
    while (input_offset < len) {
        const uint32_t a = data[input_offset++];
        const uint32_t b = input_offset < len ? data[input_offset++] : 0u;
        const uint32_t c = input_offset < len ? data[input_offset++] : 0u;
        const uint32_t triple = (a << 16u) | (b << 8u) | c;
        output[output_offset++] = chars[(triple >> 18u) & 0x3fu];
        output[output_offset++] = chars[(triple >> 12u) & 0x3fu];
        output[output_offset++] = chars[(triple >> 6u) & 0x3fu];
        output[output_offset++] = chars[triple & 0x3fu];
    }
    const size_t padding = (3u - (len % 3u)) % 3u;
    for (size_t index = 0; index < padding; ++index)
        output[output.size() - 1u - index] = '=';
    return output;
}

static void scene_test_write_f32_le(uint8_t *destination, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    destination[0] = static_cast<uint8_t>(bits & 0xffu);
    destination[1] = static_cast<uint8_t>((bits >> 8u) & 0xffu);
    destination[2] = static_cast<uint8_t>((bits >> 16u) & 0xffu);
    destination[3] = static_cast<uint8_t>((bits >> 24u) & 0xffu);
}

static void test_create_scene_and_node() {
    void *scene = rt_scene3d_new();
    EXPECT_TRUE(scene != nullptr, "SceneGraph.New returns non-null");

    void *root = rt_scene3d_get_root(scene);
    EXPECT_TRUE(root != nullptr, "SceneGraph.Root is non-null at creation");

    void *node = rt_scene_node3d_new();
    EXPECT_TRUE(node != nullptr, "SceneNode.New returns non-null");

    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 1, "Initial node count is 1 (root)");
}

static void test_add_remove_child() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();

    rt_scene3d_add(scene, node);
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 2, "After Add: node count is 2");
    EXPECT_TRUE(rt_scene_node3d_child_count(rt_scene3d_get_root(scene)) == 1, "Root has 1 child");

    void *parent = rt_scene_node3d_get_parent(node);
    EXPECT_TRUE(parent == rt_scene3d_get_root(scene), "Child's parent is root");

    rt_scene3d_remove(scene, node);
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 1, "After Remove: node count is 1");
    EXPECT_TRUE(rt_scene_node3d_get_parent(node) == nullptr, "Removed node has no parent");
}

static void test_try_add_reports_parenting_success() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();

    EXPECT_TRUE(rt_scene3d_try_add(scene, parent) != 0, "SceneGraph.TryAdd succeeds for a node");
    EXPECT_TRUE(rt_scene_node3d_get_parent(parent) == rt_scene3d_get_root(scene),
                "SceneGraph.TryAdd parents node under root");
    EXPECT_TRUE(rt_scene3d_try_add(scene, scene) == 0,
                "SceneGraph.TryAdd rejects non-node handles");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 2,
                "SceneGraph.TryAdd failure leaves node count unchanged");

    EXPECT_TRUE(rt_scene_node3d_try_add_child(parent, child) != 0,
                "SceneNode.TryAddChild succeeds for valid child");
    EXPECT_TRUE(rt_scene_node3d_try_add_child(parent, child) != 0,
                "SceneNode.TryAddChild reports success for existing child");
    EXPECT_TRUE(rt_scene_node3d_child_count(parent) == 1,
                "SceneNode.TryAddChild keeps duplicate child count stable");
    EXPECT_TRUE(rt_scene_node3d_try_add_child(child, parent) == 0,
                "SceneNode.TryAddChild rejects cycles");
    EXPECT_TRUE(rt_scene_node3d_get_parent(parent) == rt_scene3d_get_root(scene),
                "Cycle rejection leaves existing parent intact");
}

static void test_scene_remove_ignores_nodes_from_other_scenes() {
    void *scene_a = rt_scene3d_new();
    void *scene_b = rt_scene3d_new();
    void *node = rt_scene_node3d_new();

    rt_scene3d_add(scene_b, node);
    EXPECT_TRUE(rt_scene_node3d_get_parent(node) == rt_scene3d_get_root(scene_b),
                "Node starts parented in scene B");

    rt_scene3d_remove(scene_a, node);

    EXPECT_TRUE(rt_scene_node3d_get_parent(node) == rt_scene3d_get_root(scene_b),
                "SceneGraph.Remove ignores nodes outside the scene root subtree");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene_a) == 1, "Scene A count remains unchanged");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene_b) == 2, "Scene B keeps its node");
}

static void test_scene_rejects_reparenting_implicit_root() {
    rt_scene3d *scene_a = (rt_scene3d *)rt_scene3d_new();
    void *scene_b = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    rt_scene_node3d *root_a = scene_a->root;

    rt_scene3d_add(scene_b, parent);
    EXPECT_TRUE(rt_scene_node3d_try_add_child(parent, root_a) == 0,
                "SceneNode.TryAddChild rejects another scene's implicit root");
    EXPECT_TRUE(root_a->parent == nullptr, "Rejected implicit root keeps no parent");
    EXPECT_TRUE(root_a->owner_scene == scene_a, "Rejected implicit root keeps its owner scene");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene_a) == 1, "Source scene root count is unchanged");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene_b) == 2, "Destination scene count is unchanged");

    void *root_b = rt_scene3d_get_root(scene_b);
    EXPECT_TRUE(rt_scene_node3d_try_add_child(root_b, root_a) == 0,
                "SceneNode.TryAddChild rejects a scene root under another scene root");
    EXPECT_TRUE(root_a->parent == nullptr, "Cross-scene root attempt preserves the parent link");
    EXPECT_TRUE(root_a->owner_scene == scene_a,
                "Cross-scene root attempt preserves the owner link");
    EXPECT_TRUE(rt_scene_node3d_child_count(root_b) == 1,
                "Cross-scene root attempt preserves destination root children");
    EXPECT_TRUE(expect_trap_contains([&] { rt_scene_node3d_add_child(root_b, root_a); },
                                     "cannot reparent a scene root"),
                "SceneNode.AddChild traps for a cross-scene root attempt");
    EXPECT_TRUE(root_a->parent == nullptr && root_a->owner_scene == scene_a,
                "Trapping root reparent attempt remains state-preserving");
}

#include "test_rt_scene3d_owner_atomic.inc"

static void test_translation_propagation() {
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    rt_scene_node3d_set_position(parent, 5.0, 0.0, 0.0);
    rt_scene_node3d_set_position(child, 1.0, 0.0, 0.0);
    rt_scene_node3d_add_child(parent, child);

    /* World matrix of child: parent(5,0,0) + child(1,0,0) = (6,0,0) */
    void *wm = rt_scene_node3d_get_world_matrix(child);
    EXPECT_TRUE(wm != nullptr, "GetWorldMatrix returns non-null");

    /* Extract translation from row-major Mat4: m[3], m[7], m[11] */
    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)wm;
    EXPECT_NEAR(mv->m[3], 6.0, 0.001, "Child world X = parent(5) + local(1) = 6");
    EXPECT_NEAR(mv->m[7], 0.0, 0.001, "Child world Y = 0");
    EXPECT_NEAR(mv->m[11], 0.0, 0.001, "Child world Z = 0");
}

static void test_world_position_and_scale_getters() {
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    rt_scene_node3d_set_position(parent, 5.0, 2.0, -1.0);
    rt_scene_node3d_set_scale(parent, 2.0, 3.0, 4.0);
    rt_scene_node3d_set_position(child, 1.0, 0.0, 0.5);
    rt_scene_node3d_set_scale(child, 0.5, 2.0, 0.25);
    rt_scene_node3d_add_child(parent, child);

    void *world_pos = rt_scene_node3d_get_world_position(child);
    void *world_scale = rt_scene_node3d_get_world_scale(child);
    double component_x = -9.0;
    double component_y = -9.0;
    double component_z = -9.0;
    EXPECT_TRUE(rt_scene_node3d_get_world_position_components(
                    child, &component_x, &component_y, &component_z) != 0,
                "WorldPosition components helper succeeds for nodes");
    EXPECT_NEAR(rt_vec3_x(world_pos), 7.0, 0.001, "WorldPosition includes parent scaled X");
    EXPECT_NEAR(rt_vec3_y(world_pos), 2.0, 0.001, "WorldPosition includes parent Y");
    EXPECT_NEAR(rt_vec3_z(world_pos), 1.0, 0.001, "WorldPosition includes parent scaled Z");
    EXPECT_NEAR(component_x, 7.0, 0.001, "WorldPosition component X matches Vec3 getter");
    EXPECT_NEAR(component_y, 2.0, 0.001, "WorldPosition component Y matches Vec3 getter");
    EXPECT_NEAR(component_z, 1.0, 0.001, "WorldPosition component Z matches Vec3 getter");
    EXPECT_NEAR(rt_vec3_x(world_scale), 1.0, 0.001, "WorldScale X composes parent/child");
    EXPECT_NEAR(rt_vec3_y(world_scale), 6.0, 0.001, "WorldScale Y composes parent/child");
    EXPECT_NEAR(rt_vec3_z(world_scale), 1.0, 0.001, "WorldScale Z composes parent/child");

    component_x = 77.0;
    component_y = 88.0;
    component_z = 99.0;
    EXPECT_TRUE(rt_scene_node3d_get_world_position_components(
                    nullptr, &component_x, &component_y, &component_z) == 0,
                "WorldPosition components helper rejects invalid nodes");
    EXPECT_NEAR(component_x, 77.0, 0.001, "Invalid component helper leaves X untouched");
    EXPECT_NEAR(component_y, 88.0, 0.001, "Invalid component helper leaves Y untouched");
    EXPECT_NEAR(component_z, 99.0, 0.001, "Invalid component helper leaves Z untouched");
}

static void test_scene_rebase_origin_shifts_root_subtrees() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    rt_scene_node3d_set_position(parent, 20.0, 3.0, -5.0);
    rt_scene_node3d_set_position(child, 2.0, 0.0, 1.0);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);

    rt_scene3d_rebase_origin(scene, 10.0, 1.0, -4.0);

    void *root_pos = rt_scene_node3d_get_position(rt_scene3d_get_root(scene));
    void *parent_world = rt_scene_node3d_get_world_position(parent);
    void *child_world = rt_scene_node3d_get_world_position(child);
    void *child_local = rt_scene_node3d_get_position(child);
    EXPECT_NEAR(rt_vec3_x(root_pos), 0.0, 0.001, "RebaseOrigin leaves scene root X unchanged");
    EXPECT_NEAR(rt_vec3_y(root_pos), 0.0, 0.001, "RebaseOrigin leaves scene root Y unchanged");
    EXPECT_NEAR(rt_vec3_z(root_pos), 0.0, 0.001, "RebaseOrigin leaves scene root Z unchanged");
    EXPECT_NEAR(rt_vec3_x(parent_world), 10.0, 0.001, "RebaseOrigin shifts parent world X");
    EXPECT_NEAR(rt_vec3_y(parent_world), 2.0, 0.001, "RebaseOrigin shifts parent world Y");
    EXPECT_NEAR(rt_vec3_z(parent_world), -1.0, 0.001, "RebaseOrigin shifts parent world Z");
    EXPECT_NEAR(rt_vec3_x(child_world), 12.0, 0.001, "RebaseOrigin shifts child world X");
    EXPECT_NEAR(rt_vec3_y(child_world), 2.0, 0.001, "RebaseOrigin shifts child world Y");
    EXPECT_NEAR(rt_vec3_z(child_world), 0.0, 0.001, "RebaseOrigin shifts child world Z");
    EXPECT_NEAR(rt_vec3_x(child_local), 2.0, 0.001, "RebaseOrigin preserves child local X");
    EXPECT_NEAR(rt_vec3_y(child_local), 0.0, 0.001, "RebaseOrigin preserves child local Y");
    EXPECT_NEAR(rt_vec3_z(child_local), 1.0, 0.001, "RebaseOrigin preserves child local Z");

    rt_scene3d_rebase_origin(scene, NAN, 0.0, INFINITY);
    void *after_nonfinite = rt_scene_node3d_get_world_position(child);
    EXPECT_NEAR(rt_vec3_x(after_nonfinite), 12.0, 0.001, "RebaseOrigin ignores non-finite X");
    EXPECT_NEAR(rt_vec3_z(after_nonfinite), 0.0, 0.001, "RebaseOrigin ignores non-finite Z");
}

static void test_transform_components_and_batch() {
    void *node = rt_scene_node3d_new();
    double x = 0, y = 0, z = 0, w = 0;
    double m[16];

    /* Combined setter: one call sets TRS without intermediate Vec3/Quat allocations. */
    rt_scene_node3d_set_transform(
        node, 1.0, 2.0, 3.0, 0.0, 0.7071067812, 0.0, 0.7071067812, 2.0, 2.0, 2.0);
    EXPECT_TRUE(rt_scene_node3d_get_position_components(node, &x, &y, &z) == 1,
                "position components read succeeds");
    EXPECT_NEAR(x, 1.0, 1e-9, "SetTransform stores position X");
    EXPECT_NEAR(y, 2.0, 1e-9, "SetTransform stores position Y");
    EXPECT_NEAR(z, 3.0, 1e-9, "SetTransform stores position Z");
    EXPECT_TRUE(rt_scene_node3d_get_rotation_components(node, &x, &y, &z, &w) == 1,
                "rotation components read succeeds");
    EXPECT_NEAR(y, 0.7071067812, 1e-6, "SetTransform stores rotation (normalized) Y");
    EXPECT_NEAR(w, 0.7071067812, 1e-6, "SetTransform stores rotation (normalized) W");
    EXPECT_TRUE(rt_scene_node3d_get_scale_components(node, &x, &y, &z) == 1,
                "scale components read succeeds");
    EXPECT_NEAR(x, 2.0, 1e-9, "SetTransform stores scale");
    EXPECT_TRUE(rt_scene_node3d_get_world_scale_components(node, &x, &y, &z) == 1,
                "world scale components read succeeds");
    EXPECT_NEAR(x, 2.0, 1e-6, "world scale magnitude matches local for a root node");
    EXPECT_TRUE(rt_scene_node3d_get_world_rotation_components(node, &x, &y, &z, &w) == 1,
                "world rotation components read succeeds");
    EXPECT_NEAR(w, 0.7071067812, 1e-6, "world rotation matches local for a root node");
    EXPECT_TRUE(rt_scene_node3d_get_world_matrix_components(node, m) == 1,
                "world matrix components read succeeds");
    EXPECT_NEAR(m[3], 1.0, 1e-9, "world matrix carries translation X");
    EXPECT_NEAR(m[7], 2.0, 1e-9, "world matrix carries translation Y");

    /* Batch setter: 10 packed floats per node, one runtime call for the whole list. */
    void *a = rt_scene_node3d_new();
    void *b = rt_scene_node3d_new();
    void *nodes = rt_seq_new();
    void *values = rt_seq_new();
    rt_seq_push(nodes, a);
    rt_seq_push(nodes, b);
    const double packed[20] = {
        /* node a */ 10.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0,
        /* node b */ 0.0,  5.0, 0.0, 0.0, 0.0, 0.0, 1.0, 3.0, 3.0, 3.0};
    for (int i = 0; i < 20; i++)
        rt_seq_push(values, rt_box_f64(packed[i]));
    rt_scene_node3d_set_transform_batch(nodes, values);
    EXPECT_TRUE(rt_scene_node3d_get_position_components(a, &x, &y, &z) == 1,
                "batch node A read succeeds");
    EXPECT_NEAR(x, 10.0, 1e-9, "batch applied node A position");
    EXPECT_TRUE(rt_scene_node3d_get_position_components(b, &x, &y, &z) == 1,
                "batch node B read succeeds");
    EXPECT_NEAR(y, 5.0, 1e-9, "batch applied node B position");
    EXPECT_TRUE(rt_scene_node3d_get_scale_components(b, &x, &y, &z) == 1,
                "batch node B scale read succeeds");
    EXPECT_NEAR(x, 3.0, 1e-9, "batch applied node B scale");
}

static void test_rotation_propagation() {
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();

    /* Rotate parent 90° around Y axis.
     * rt_quat_from_euler(pitch, yaw, roll) — yaw maps to Y rotation. */
    double angle = M_PI / 2.0;
    void *rot = rt_quat_from_euler(0.0, angle, 0.0);
    rt_scene_node3d_set_rotation(parent, rot);

    /* Child at local position (1, 0, 0) */
    rt_scene_node3d_set_position(child, 1.0, 0.0, 0.0);
    rt_scene_node3d_add_child(parent, child);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_scene_node3d_get_world_matrix(child);

    /* 90° Y rotation of (1,0,0) → (0,0,-1) */
    EXPECT_NEAR(mv->m[3], 0.0, 0.01, "Rotated child world X ≈ 0");
    EXPECT_NEAR(mv->m[7], 0.0, 0.01, "Rotated child world Y ≈ 0");
    EXPECT_NEAR(mv->m[11], -1.0, 0.01, "Rotated child world Z ≈ -1");
}

static void test_scale_propagation() {
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    rt_scene_node3d_set_scale(parent, 2.0, 2.0, 2.0);
    rt_scene_node3d_set_position(child, 1.0, 1.0, 1.0);
    rt_scene_node3d_add_child(parent, child);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_scene_node3d_get_world_matrix(child);
    /* Scale(2) * Translate(1,1,1) → world pos = (2,2,2) */
    EXPECT_NEAR(mv->m[3], 2.0, 0.001, "Scaled child world X = 2");
    EXPECT_NEAR(mv->m[7], 2.0, 0.001, "Scaled child world Y = 2");
    EXPECT_NEAR(mv->m[11], 2.0, 0.001, "Scaled child world Z = 2");
}

static void test_deep_hierarchy() {
    /* 5-level chain: each node translates +1 in X */
    void *nodes[5];
    for (int i = 0; i < 5; i++) {
        nodes[i] = rt_scene_node3d_new();
        rt_scene_node3d_set_position(nodes[i], 1.0, 0.0, 0.0);
        if (i > 0)
            rt_scene_node3d_add_child(nodes[i - 1], nodes[i]);
    }

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_scene_node3d_get_world_matrix(nodes[4]);
    EXPECT_NEAR(mv->m[3], 5.0, 0.001, "5-level hierarchy: world X = 5");
}

static void test_hierarchy_epochs_are_root_local() {
    auto *root_a = static_cast<rt_scene_node3d *>(rt_scene_node3d_new());
    auto *root_b = static_cast<rt_scene_node3d *>(rt_scene_node3d_new());
    void *child_a = rt_scene_node3d_new();
    void *child_b = rt_scene_node3d_new();
    uint64_t a_before = scene3d_hierarchy_epoch(root_a);
    uint64_t b_before = scene3d_hierarchy_epoch(root_b);

    rt_scene_node3d_add_child(root_a, child_a);
    EXPECT_TRUE(scene3d_hierarchy_epoch(root_a) != a_before,
                "mutated root advances its topology epoch");
    EXPECT_TRUE(scene3d_hierarchy_epoch(root_b) == b_before,
                "unrelated root keeps its topology epoch");

    uint64_t a_after = scene3d_hierarchy_epoch(root_a);
    rt_scene_node3d_add_child(root_b, child_b);
    EXPECT_TRUE(scene3d_hierarchy_epoch(root_a) == a_after,
                "first root is unchanged by second-root mutation");
    EXPECT_TRUE(scene3d_hierarchy_epoch(root_b) != b_before, "second root advances independently");
}

static void test_deep_hierarchy_iterative_traversal() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene3d_get_root(scene);
    void *leaf = nullptr;
    const int depth = 1024;
    for (int i = 0; i < depth; i++) {
        leaf = rt_scene_node3d_new();
        rt_scene_node3d_set_position(leaf, 1.0, 0.0, 0.0);
        rt_scene_node3d_add_child(parent, leaf);
        parent = leaf;
    }

    rt_scene_node3d_set_position(rt_scene3d_get_root(scene), 0.0, 0.0, 0.0);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_scene_node3d_get_world_matrix(leaf);
    EXPECT_NEAR(
        mv->m[3], (double)depth, 0.001, "Deep hierarchy world matrix traversal is iterative");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == depth + 1,
                "Deep hierarchy node count traversal is iterative");
}

static void test_dirty_flag() {
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    rt_scene_node3d_add_child(parent, child);

    /* Access child world matrix to clear dirty */
    rt_scene_node3d_get_world_matrix(child);

    /* Change parent → child should become dirty and recompute */
    rt_scene_node3d_set_position(parent, 10.0, 0.0, 0.0);

    typedef struct {
        double m[16];
    } mat4_view;

    mat4_view *mv = (mat4_view *)rt_scene_node3d_get_world_matrix(child);
    EXPECT_NEAR(mv->m[3], 10.0, 0.001, "After parent move: child world X updated to 10");
}

static void test_find_by_name() {
    void *scene = rt_scene3d_new();
    void *n1 = rt_scene_node3d_new();
    void *n2 = rt_scene_node3d_new();

    rt_string name_bat = rt_const_cstr("bat");
    rt_string name_glove = rt_const_cstr("glove");
    rt_scene_node3d_set_name(n1, name_bat);
    rt_scene_node3d_set_name(n2, name_glove);
    rt_scene3d_add(scene, n1);
    rt_scene_node3d_add_child(n1, n2);

    void *found = rt_scene3d_find(scene, name_bat);
    EXPECT_TRUE(found == n1, "Find 'bat' returns correct node");

    void *found2 = rt_scene3d_find(scene, name_glove);
    EXPECT_TRUE(found2 == n2, "Find 'glove' returns nested node");

    rt_string name_missing = rt_const_cstr("nonexistent");
    void *nf = rt_scene3d_find(scene, name_missing);
    EXPECT_TRUE(nf == nullptr, "Find nonexistent returns null");
    void *found_option = rt_scene3d_find_option(scene, name_glove);
    EXPECT_TRUE(rt_option_is_some(found_option) == 1, "SceneGraph.FindOption returns Some");
    EXPECT_TRUE(rt_option_unwrap(found_option) == n2, "SceneGraph.FindOption unwraps found node");
    void *node_option = rt_scene_node3d_find_option(n1, name_glove);
    EXPECT_TRUE(rt_option_is_some(node_option) == 1, "SceneNode.FindOption returns Some");
    EXPECT_TRUE(rt_option_unwrap(node_option) == n2, "SceneNode.FindOption unwraps found node");
    EXPECT_TRUE(rt_option_is_none(rt_scene3d_find_option(scene, name_missing)) == 1,
                "SceneGraph.FindOption returns None for missing node");
    EXPECT_TRUE(rt_option_is_none(rt_scene_node3d_find_option(n1, name_missing)) == 1,
                "SceneNode.FindOption returns None for missing node");
}

static void test_scene_node_names_reject_wrong_string_handles() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    rt_string parent_name = rt_const_cstr("parent");
    rt_string child_name = rt_const_cstr("child");

    rt_scene_node3d_set_name(parent, parent_name);
    rt_scene_node3d_set_name(child, child_name);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);

    void *wrong_name = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_name);
    rt_string fake_name = reinterpret_cast<rt_string>(wrong_name);

    EXPECT_TRUE(rt_scene3d_find(scene, fake_name) == nullptr,
                "SceneGraph.Find rejects wrong-class query string handles");
    EXPECT_TRUE(rt_scene_node3d_find(parent, fake_name) == nullptr,
                "SceneNode.Find rejects wrong-class query string handles");

    rt_scene_node3d_set_name(parent, fake_name);
    rt_string returned_name = rt_scene_node3d_get_name(parent);
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(returned_name), "parent") == 0,
        "SceneNode.SetName rejects wrong-class string handles without clearing a valid name");
    rt_string_unref(returned_name);
    EXPECT_TRUE(rt_scene3d_find(scene, parent_name) == parent,
                "SceneGraph.Find still locates a node after a rejected wrong-class name");

    auto *parent_view = static_cast<rt_scene_node3d *>(parent);
    rt_string saved_name = parent_view->name;
    parent_view->name = fake_name;
    EXPECT_TRUE(rt_scene3d_find(scene, child_name) == child,
                "SceneGraph.Find skips corrupt stored node names while walking descendants");
    EXPECT_TRUE(parent_view->name == nullptr,
                "SceneGraph.Find repairs corrupt private node name slots");
    EXPECT_TRUE(rt_scene3d_find(scene, parent_name) == nullptr,
                "SceneGraph.Find does not match a repaired-away corrupt node name");
    parent_view->name = saved_name;

    parent_view->name = fake_name;
    returned_name = rt_scene_node3d_get_name(parent);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(returned_name), "") == 0,
                "SceneNode.GetName returns empty for corrupt private node name slots");
    rt_string_unref(returned_name);
    EXPECT_TRUE(parent_view->name == nullptr,
                "SceneNode.GetName repairs corrupt private node name slots");
    parent_view->name = saved_name;

    EXPECT_TRUE(rt_obj_release_check0(wrong_name) == 0,
                "SceneNode name guards do not release wrong-class handles");
    if (rt_obj_release_check0(wrong_name))
        rt_obj_free(wrong_name);
}

static void test_reparenting() {
    void *scene = rt_scene3d_new();
    void *a = rt_scene_node3d_new();
    void *b = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();

    rt_scene3d_add(scene, a);
    rt_scene3d_add(scene, b);
    rt_scene_node3d_add_child(a, child);
    EXPECT_TRUE(rt_scene_node3d_get_parent(child) == a, "Initial parent is A");

    /* Reparent child from A to B */
    rt_scene_node3d_add_child(b, child);
    EXPECT_TRUE(rt_scene_node3d_get_parent(child) == b, "After reparent: parent is B");
    EXPECT_TRUE(rt_scene_node3d_child_count(a) == 0, "A has 0 children after reparent");
    EXPECT_TRUE(rt_scene_node3d_child_count(b) == 1, "B has 1 child after reparent");
}

static void test_node_count_tracks_nested_hierarchy_edits() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();

    rt_scene3d_add(scene, parent);
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 2, "Scene count includes root and parent");

    rt_scene_node3d_add_child(parent, child);
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 3,
                "Scene count reflects nested children added outside SceneGraph.Add");

    rt_scene_node3d_remove_child(parent, child);
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 2,
                "Scene count reflects nested children removed outside SceneGraph.Remove");
}

static void test_prevent_cycle() {
    void *a = rt_scene_node3d_new();
    void *b = rt_scene_node3d_new();
    void *c = rt_scene_node3d_new();

    rt_scene_node3d_add_child(a, b);
    rt_scene_node3d_add_child(b, c);
    rt_scene_node3d_add_child(c, a);

    EXPECT_TRUE(rt_scene_node3d_get_parent(a) == nullptr,
                "Cycle insertion leaves ancestor parent unchanged");
    EXPECT_TRUE(rt_scene_node3d_get_parent(b) == a,
                "Existing parent link is preserved after cycle attempt");
    EXPECT_TRUE(rt_scene_node3d_get_parent(c) == b,
                "Descendant parent link is preserved after cycle attempt");
    EXPECT_TRUE(rt_scene_node3d_child_count(c) == 0, "Cycle insertion does not add a child");
}

static void test_visibility() {
    void *node = rt_scene_node3d_new();
    EXPECT_TRUE(rt_scene_node3d_get_visible(node) == 1, "Default visible = true");

    rt_scene_node3d_set_visible(node, 0);
    EXPECT_TRUE(rt_scene_node3d_get_visible(node) == 0, "After set visible=false");

    static_cast<rt_scene_node3d *>(node)->visible = -5;
    EXPECT_TRUE(rt_scene_node3d_get_visible(node) == 1,
                "SceneNode visibility getter normalizes corrupt private flags");
}

static void test_subtree_aabb_includes_child_meshes() {
    void *root = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(2.0, 4.0, 6.0);

    rt_scene_node3d_set_mesh(child, mesh);
    rt_scene_node3d_set_scale(child, 1.5, 0.5, 2.0);
    rt_scene_node3d_set_position(child, 5.0, 1.0, -2.0);
    rt_scene_node3d_add_child(root, child);

    void *min_v = rt_scene_node3d_get_aabb_min(root);
    void *max_v = rt_scene_node3d_get_aabb_max(root);

    EXPECT_NEAR(rt_vec3_x(min_v), 3.5, 0.001, "Synthetic roots report subtree AABB min X");
    EXPECT_NEAR(rt_vec3_y(min_v), 0.0, 0.001, "Synthetic roots report subtree AABB min Y");
    EXPECT_NEAR(rt_vec3_z(min_v), -8.0, 0.001, "Synthetic roots report subtree AABB min Z");

    EXPECT_NEAR(rt_vec3_x(max_v), 6.5, 0.001, "Synthetic roots report subtree AABB max X");
    EXPECT_NEAR(rt_vec3_y(max_v), 2.0, 0.001, "Synthetic roots report subtree AABB max Y");
    EXPECT_NEAR(rt_vec3_z(max_v), 4.0, 0.001, "Synthetic roots report subtree AABB max Z");

    rt_scene_node3d *root_impl = (rt_scene_node3d *)root;
    EXPECT_NEAR(root_impl->aabb_min[0], 0.0, 0.001, "AABB query does not mutate node min X");
    EXPECT_NEAR(root_impl->aabb_max[2], 0.0, 0.001, "AABB query does not mutate node max Z");
}

static void test_clear() {
    void *scene = rt_scene3d_new();
    void *n1 = rt_scene_node3d_new();
    void *n2 = rt_scene_node3d_new();
    rt_scene3d_add(scene, n1);
    rt_scene3d_add(scene, n2);
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 3, "Before clear: 3 nodes");

    rt_scene3d_clear(scene);
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 1, "After clear: 1 node (root)");
    EXPECT_TRUE(rt_scene_node3d_child_count(rt_scene3d_get_root(scene)) == 0,
                "Root has 0 children");

    void *corrupt_scene = rt_scene3d_new();
    void *child = rt_scene_node3d_new();
    void *wrong_child_slot = rt_material3d_new_color(0.2, 0.3, 0.4);
    rt_scene3d_add(corrupt_scene, child);
    auto *root_view = static_cast<rt_scene_node3d *>(rt_scene3d_get_root(corrupt_scene));
    EXPECT_TRUE(root_view->child_count == 1 && root_view->children != nullptr,
                "clear corruption fixture has a root child slot");
    if (root_view->children)
        root_view->children[0] = reinterpret_cast<rt_scene_node3d *>(wrong_child_slot);
    rt_scene3d_clear(corrupt_scene);
    EXPECT_TRUE(rt_scene3d_get_node_count(corrupt_scene) == 1,
                "SceneGraph.Clear repairs wrong-class root child slots");
    EXPECT_TRUE(rt_scene_node3d_child_count(rt_scene3d_get_root(corrupt_scene)) == 0,
                "SceneGraph.Clear leaves the root empty after child-slot repair");
    if (wrong_child_slot && rt_obj_release_check0(wrong_child_slot))
        rt_obj_free(wrong_child_slot);
}

static void test_get_child() {
    void *parent = rt_scene_node3d_new();
    void *c1 = rt_scene_node3d_new();
    void *c2 = rt_scene_node3d_new();
    rt_scene_node3d_add_child(parent, c1);
    rt_scene_node3d_add_child(parent, c2);

    EXPECT_TRUE(rt_scene_node3d_get_child(parent, 0) == c1, "GetChild(0) returns first child");
    EXPECT_TRUE(rt_scene_node3d_get_child(parent, 1) == c2, "GetChild(1) returns second child");
    EXPECT_TRUE(rt_scene_node3d_get_child(parent, 2) == nullptr,
                "GetChild(2) out of bounds returns null");
}

static void test_try_move_child_reorders_atomically() {
    void *parent = rt_scene_node3d_new();
    void *a = rt_scene_node3d_new();
    void *b = rt_scene_node3d_new();
    void *c = rt_scene_node3d_new();
    void *d = rt_scene_node3d_new();
    void *foreign = rt_scene_node3d_new();
    rt_scene_node3d_add_child(parent, a);
    rt_scene_node3d_add_child(parent, b);
    rt_scene_node3d_add_child(parent, c);
    rt_scene_node3d_add_child(parent, d);

    EXPECT_TRUE(rt_scene_node3d_try_move_child(parent, c, 0) == 1,
                "TryMoveChild moves an existing child toward the front");
    EXPECT_TRUE(
        rt_scene_node3d_get_child(parent, 0) == c && rt_scene_node3d_get_child(parent, 1) == a &&
            rt_scene_node3d_get_child(parent, 2) == b && rt_scene_node3d_get_child(parent, 3) == d,
        "TryMoveChild preserves stable sibling order around a front move");
    EXPECT_TRUE(rt_scene_node3d_get_parent(c) == parent && rt_scene_node3d_child_count(parent) == 4,
                "TryMoveChild preserves parent identity and child count");

    EXPECT_TRUE(rt_scene_node3d_try_move_child(parent, c, 3) == 1,
                "TryMoveChild moves an existing child toward the back");
    EXPECT_TRUE(
        rt_scene_node3d_get_child(parent, 0) == a && rt_scene_node3d_get_child(parent, 1) == b &&
            rt_scene_node3d_get_child(parent, 2) == d && rt_scene_node3d_get_child(parent, 3) == c,
        "TryMoveChild preserves stable sibling order around a back move");
    EXPECT_TRUE(rt_scene_node3d_try_move_child(parent, c, 3) == 1,
                "TryMoveChild accepts an already-satisfied target index");

    EXPECT_TRUE(rt_scene_node3d_try_move_child(parent, c, -1) == 0 &&
                    rt_scene_node3d_try_move_child(parent, c, 4) == 0 &&
                    rt_scene_node3d_try_move_child(parent, foreign, 0) == 0,
                "TryMoveChild rejects invalid targets and unrelated nodes");
    EXPECT_TRUE(rt_scene_node3d_get_child(parent, 0) == a &&
                    rt_scene_node3d_get_child(parent, 1) == b &&
                    rt_scene_node3d_get_child(parent, 2) == d &&
                    rt_scene_node3d_get_child(parent, 3) == c &&
                    rt_scene_node3d_get_parent(foreign) == nullptr,
                "Rejected TryMoveChild calls leave both hierarchies unchanged");

    auto *parent_view = scene_node3d_checked(parent);
    rt_scene_node3d *saved_slot = parent_view->children[1];
    parent_view->children[1] = reinterpret_cast<rt_scene_node3d *>(foreign);
    EXPECT_TRUE(rt_scene_node3d_try_move_child(parent, c, 0) == 0,
                "TryMoveChild rejects a corrupt direct-child table");
    EXPECT_TRUE(parent_view->children[0] == reinterpret_cast<rt_scene_node3d *>(a) &&
                    parent_view->children[1] == reinterpret_cast<rt_scene_node3d *>(foreign) &&
                    parent_view->children[2] == reinterpret_cast<rt_scene_node3d *>(d) &&
                    parent_view->children[3] == reinterpret_cast<rt_scene_node3d *>(c),
                "TryMoveChild validates every direct child before mutation");
    parent_view->children[1] = saved_slot;
}

static void test_try_add_child_preserve_world_keeps_complete_subtree_pose() {
    void *scene = rt_scene3d_new();
    void *source_parent = rt_scene_node3d_new();
    void *target_parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *grandchild = rt_scene_node3d_new();
    void *source_rotation = rt_quat_from_euler(0.2, -0.4, 0.3);
    void *target_rotation = rt_quat_from_euler(-0.1, 0.6, -0.25);
    void *child_rotation = rt_quat_from_euler(0.35, 0.15, -0.5);
    double child_world_before[16]{};
    double grandchild_world_before[16]{};
    double child_world_after[16]{};
    double grandchild_world_after[16]{};
    double local_x_before = 0.0;
    double local_y_before = 0.0;
    double local_z_before = 0.0;
    double local_x_after = 0.0;
    double local_y_after = 0.0;
    double local_z_after = 0.0;

    rt_scene_node3d_set_transform(source_parent,
                                  8.0,
                                  -3.0,
                                  2.0,
                                  rt_quat_x(source_rotation),
                                  rt_quat_y(source_rotation),
                                  rt_quat_z(source_rotation),
                                  rt_quat_w(source_rotation),
                                  2.0,
                                  2.0,
                                  2.0);
    rt_scene_node3d_set_transform(target_parent,
                                  -4.0,
                                  5.0,
                                  7.0,
                                  rt_quat_x(target_rotation),
                                  rt_quat_y(target_rotation),
                                  rt_quat_z(target_rotation),
                                  rt_quat_w(target_rotation),
                                  0.5,
                                  0.5,
                                  0.5);
    rt_scene_node3d_set_transform(child,
                                  1.5,
                                  -2.0,
                                  0.75,
                                  rt_quat_x(child_rotation),
                                  rt_quat_y(child_rotation),
                                  rt_quat_z(child_rotation),
                                  rt_quat_w(child_rotation),
                                  -1.25,
                                  0.8,
                                  1.6);
    rt_scene_node3d_set_position(grandchild, 0.25, 1.0, -0.5);
    rt_scene3d_add(scene, source_parent);
    rt_scene3d_add(scene, target_parent);
    rt_scene_node3d_add_child(source_parent, child);
    rt_scene_node3d_add_child(child, grandchild);

    EXPECT_TRUE(
        rt_scene_node3d_get_world_matrix_components(child, child_world_before) != 0 &&
            rt_scene_node3d_get_world_matrix_components(grandchild, grandchild_world_before) != 0 &&
            rt_scene_node3d_get_position_components(
                child, &local_x_before, &local_y_before, &local_z_before) != 0,
        "Preserve-world fixture exposes complete source transforms");
    EXPECT_TRUE(rt_scene_node3d_try_add_child_preserve_world(target_parent, child) != 0,
                "TryAddChildPreserveWorld accepts an exactly representable transform");
    EXPECT_TRUE(rt_scene_node3d_get_parent(child) == target_parent &&
                    rt_scene_node3d_child_count(source_parent) == 0 &&
                    rt_scene_node3d_child_count(target_parent) == 1,
                "Preserve-world reparent changes only the requested hierarchy link");
    EXPECT_TRUE(
        rt_scene_node3d_get_world_matrix_components(child, child_world_after) != 0 &&
            rt_scene_node3d_get_world_matrix_components(grandchild, grandchild_world_after) != 0 &&
            rt_scene_node3d_get_position_components(
                child, &local_x_after, &local_y_after, &local_z_after) != 0,
        "Preserve-world reparent exposes its resulting transforms");
    for (int lane = 0; lane < 16; ++lane) {
        EXPECT_NEAR(child_world_after[lane],
                    child_world_before[lane],
                    1e-8,
                    "TryAddChildPreserveWorld keeps every child world-matrix lane");
        EXPECT_NEAR(grandchild_world_after[lane],
                    grandchild_world_before[lane],
                    1e-8,
                    "TryAddChildPreserveWorld keeps descendant world-matrix lanes");
    }
    EXPECT_TRUE(fabs(local_x_after - local_x_before) > 0.001 ||
                    fabs(local_y_after - local_y_before) > 0.001 ||
                    fabs(local_z_after - local_z_before) > 0.001,
                "Preserve-world reparent derives a new local transform");
    EXPECT_TRUE(rt_scene_node3d_try_add_child_preserve_world(target_parent, child) != 0,
                "TryAddChildPreserveWorld accepts an already-satisfied parent");
}

static void test_try_add_child_preserve_world_rejects_lossy_changes_atomically() {
    void *scene = rt_scene3d_new();
    void *source_parent = rt_scene_node3d_new();
    void *target_parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *grandchild = rt_scene_node3d_new();
    void *child_rotation = rt_quat_from_euler(0.0, 0.0, 0.7853981633974483);
    double world_before[16];
    double world_after[16];
    double local_before[3];
    double local_after[3];

    rt_scene3d_add(scene, source_parent);
    rt_scene3d_add(scene, target_parent);
    rt_scene_node3d_add_child(source_parent, child);
    rt_scene_node3d_add_child(child, grandchild);
    rt_scene_node3d_set_transform(child,
                                  2.0,
                                  3.0,
                                  4.0,
                                  rt_quat_x(child_rotation),
                                  rt_quat_y(child_rotation),
                                  rt_quat_z(child_rotation),
                                  rt_quat_w(child_rotation),
                                  1.0,
                                  1.0,
                                  1.0);
    rt_scene_node3d_set_scale(target_parent, 0.0, 1.0, 1.0);
    EXPECT_TRUE(rt_scene_node3d_get_world_matrix_components(child, world_before) != 0 &&
                    rt_scene_node3d_get_position_components(
                        child, &local_before[0], &local_before[1], &local_before[2]) != 0,
                "Lossy preserve-world fixture exposes its source transform");

    EXPECT_TRUE(rt_scene_node3d_try_add_child_preserve_world(target_parent, child) == 0,
                "TryAddChildPreserveWorld rejects a singular destination parent");
    EXPECT_TRUE(rt_scene_node3d_get_parent(child) == source_parent &&
                    rt_scene_node3d_child_count(source_parent) == 1 &&
                    rt_scene_node3d_child_count(target_parent) == 0,
                "Singular-parent rejection leaves hierarchy links untouched");

    rt_scene_node3d_set_scale(target_parent, 2.0, 1.0, 1.0);
    EXPECT_TRUE(rt_scene_node3d_try_add_child_preserve_world(target_parent, child) == 0,
                "TryAddChildPreserveWorld rejects a conversion that requires shear");
    EXPECT_TRUE(rt_scene_node3d_get_world_matrix_components(child, world_after) != 0 &&
                    rt_scene_node3d_get_position_components(
                        child, &local_after[0], &local_after[1], &local_after[2]) != 0,
                "Rejected preserve-world conversion keeps readable transforms");
    for (int lane = 0; lane < 16; ++lane)
        EXPECT_NEAR(world_after[lane],
                    world_before[lane],
                    1e-12,
                    "Rejected preserve-world conversion keeps the world matrix");
    for (int axis = 0; axis < 3; ++axis)
        EXPECT_NEAR(local_after[axis],
                    local_before[axis],
                    1e-12,
                    "Rejected preserve-world conversion keeps local position");
    EXPECT_TRUE(rt_scene_node3d_get_parent(child) == source_parent &&
                    rt_scene_node3d_child_count(source_parent) == 1 &&
                    rt_scene_node3d_child_count(target_parent) == 0,
                "Shear rejection leaves both parent child tables untouched");
    EXPECT_TRUE(rt_scene_node3d_try_add_child_preserve_world(grandchild, child) == 0,
                "TryAddChildPreserveWorld rejects hierarchy cycles");
    EXPECT_TRUE(rt_scene_node3d_try_add_child_preserve_world(target_parent,
                                                             rt_scene3d_get_root(scene)) == 0,
                "TryAddChildPreserveWorld rejects implicit scene roots");
    EXPECT_TRUE(rt_scene_node3d_try_add_child_preserve_world(scene, child) == 0,
                "TryAddChildPreserveWorld rejects wrong-class parent handles");
    EXPECT_TRUE(rt_scene_node3d_get_parent(child) == source_parent,
                "All rejected preserve-world calls retain the original parent");
}

static void test_try_set_world_matrix_is_exact_and_atomic() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *parent_rotation = rt_quat_from_euler(0.0, 0.0, M_PI / 6.0);
    void *desired_translation = rt_mat4_translate(9.0, -3.0, 2.0);
    void *desired_rotation = rt_mat4_rotate_z(M_PI / 5.0);
    void *desired_scale = rt_mat4_scale(1.5, 0.75, 2.0);
    void *desired_basis = rt_mat4_mul(desired_rotation, desired_scale);
    void *desired_world = rt_mat4_mul(desired_translation, desired_basis);
    double requested[16];
    double actual[16];

    rt_scene_node3d_set_transform(parent,
                                  5.0,
                                  -2.0,
                                  1.0,
                                  rt_quat_x(parent_rotation),
                                  rt_quat_y(parent_rotation),
                                  rt_quat_z(parent_rotation),
                                  rt_quat_w(parent_rotation),
                                  2.0,
                                  2.0,
                                  2.0);
    rt_scene3d_add(scene, parent);
    rt_scene_node3d_add_child(parent, child);

    EXPECT_TRUE(rt_scene_node3d_try_set_world_matrix(child, desired_world) != 0,
                "TrySetWorldMatrix accepts an exactly representable parent-relative TRS");
    EXPECT_TRUE(rt_scene_node3d_get_world_matrix_components(child, actual) != 0,
                "TrySetWorldMatrix publishes a readable world matrix");
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column)
            requested[row * 4 + column] = rt_mat4_get(desired_world, row, column);
    }
    for (int lane = 0; lane < 16; ++lane)
        EXPECT_NEAR(actual[lane],
                    requested[lane],
                    1e-9,
                    "TrySetWorldMatrix reproduces every requested world-matrix lane");
    EXPECT_TRUE(rt_scene_node3d_try_set_world_matrix(child, desired_world) != 0,
                "TrySetWorldMatrix accepts an already-satisfied world transform");

    double local_before[10];
    double local_after[10];
    EXPECT_TRUE(
        rt_scene_node3d_get_position_components(
            child, &local_before[0], &local_before[1], &local_before[2]) != 0 &&
            rt_scene_node3d_get_rotation_components(
                child, &local_before[3], &local_before[4], &local_before[5], &local_before[6]) !=
                0 &&
            rt_scene_node3d_get_scale_components(
                child, &local_before[7], &local_before[8], &local_before[9]) != 0,
        "TrySetWorldMatrix rejection fixture captures the complete local TRS");

    rt_scene_node3d_set_scale(parent, 0.0, 1.0, 1.0);
    EXPECT_TRUE(rt_scene_node3d_try_set_world_matrix(child, desired_world) == 0,
                "TrySetWorldMatrix rejects a singular parent");
    rt_scene_node3d_set_scale(parent, 2.0, 1.0, 1.0);
    void *sheared_local =
        rt_mat4_mul(rt_mat4_translate(3.0, 4.0, 5.0), rt_mat4_rotate_z(M_PI / 4.0));
    EXPECT_TRUE(rt_scene_node3d_try_set_world_matrix(child, sheared_local) == 0,
                "TrySetWorldMatrix rejects a request requiring local shear");
    EXPECT_TRUE(rt_scene_node3d_try_set_world_matrix(child, rt_vec3_new(1.0, 2.0, 3.0)) == 0 &&
                    rt_scene_node3d_try_set_world_matrix(child, nullptr) == 0 &&
                    rt_scene_node3d_try_set_world_matrix(scene, desired_world) == 0,
                "TrySetWorldMatrix rejects wrong-class and null handles");

    EXPECT_TRUE(
        rt_scene_node3d_get_position_components(
            child, &local_after[0], &local_after[1], &local_after[2]) != 0 &&
            rt_scene_node3d_get_rotation_components(
                child, &local_after[3], &local_after[4], &local_after[5], &local_after[6]) != 0 &&
            rt_scene_node3d_get_scale_components(
                child, &local_after[7], &local_after[8], &local_after[9]) != 0,
        "Rejected TrySetWorldMatrix calls leave the complete local TRS readable");
    for (int lane = 0; lane < 10; ++lane)
        EXPECT_NEAR(local_after[lane],
                    local_before[lane],
                    1e-12,
                    "Rejected TrySetWorldMatrix calls are transform-atomic");
}

static void test_default_transform() {
    void *node = rt_scene_node3d_new();
    void *pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.001, "Default position X = 0");
    EXPECT_NEAR(rt_vec3_y(pos), 0.0, 0.001, "Default position Y = 0");
    EXPECT_NEAR(rt_vec3_z(pos), 0.0, 0.001, "Default position Z = 0");

    void *scl = rt_scene_node3d_get_scale(node);
    EXPECT_NEAR(rt_vec3_x(scl), 1.0, 0.001, "Default scale X = 1");
    EXPECT_NEAR(rt_vec3_y(scl), 1.0, 0.001, "Default scale Y = 1");
    EXPECT_NEAR(rt_vec3_z(scl), 1.0, 0.001, "Default scale Z = 1");

    void *rot = rt_scene_node3d_get_rotation(node);
    EXPECT_NEAR(rt_quat_w(rot), 1.0, 0.001, "Default rotation W = 1 (identity)");
}

static void test_node_sanitizes_nonfinite_transform_and_lod() {
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);

    rt_scene_node3d_set_position(node, NAN, 3.0, INFINITY);
    void *pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.001, "SceneNode non-finite position X falls back to 0");
    EXPECT_NEAR(rt_vec3_y(pos), 3.0, 0.001, "SceneNode finite position Y is preserved");
    EXPECT_NEAR(rt_vec3_z(pos), 0.0, 0.001, "SceneNode non-finite position Z falls back to 0");

    rt_scene_node3d_set_scale(node, NAN, -2.0, INFINITY);
    void *scale = rt_scene_node3d_get_scale(node);
    EXPECT_NEAR(rt_vec3_x(scale), 1.0, 0.001, "SceneNode non-finite scale X falls back to 1");
    EXPECT_NEAR(rt_vec3_y(scale), -2.0, 0.001, "SceneNode finite negative scale is preserved");
    EXPECT_NEAR(rt_vec3_z(scale), 1.0, 0.001, "SceneNode non-finite scale Z falls back to 1");

    rt_scene_node3d_set_scale(node, 0.0, -0.0, 1e-16);
    scale = rt_scene_node3d_get_scale(node);
    EXPECT_NEAR(rt_vec3_x(scale), 0.0, 0.001, "SceneNode zero scale X is preserved");
    EXPECT_NEAR(rt_vec3_y(scale), 0.0, 0.001, "SceneNode negative-zero scale Y is preserved");
    EXPECT_NEAR(rt_vec3_z(scale), 1e-16, 0.001, "SceneNode near-zero finite scale Z is preserved");

    rt_scene_node3d_set_rotation(node, rt_quat_new(NAN, 0.0, 0.0, 0.0));
    void *rot = rt_scene_node3d_get_rotation(node);
    EXPECT_NEAR(rt_quat_x(rot), 0.0, 0.001, "SceneNode invalid quaternion resets X");
    EXPECT_NEAR(rt_quat_y(rot), 0.0, 0.001, "SceneNode invalid quaternion resets Y");
    EXPECT_NEAR(rt_quat_z(rot), 0.0, 0.001, "SceneNode invalid quaternion resets Z");
    EXPECT_NEAR(rt_quat_w(rot), 1.0, 0.001, "SceneNode invalid quaternion resets W");

    rt_scene_node3d_set_rotation(node, node);
    rot = rt_scene_node3d_get_rotation(node);
    EXPECT_NEAR(rt_quat_w(rot), 1.0, 0.001, "SceneNode rejects non-Quat rotation handles");

    rt_scene_node3d_set_visible(node, 42);
    EXPECT_TRUE(rt_scene_node3d_get_visible(node) == 1, "SceneNode visibility normalizes to bool");

    rt_scene_node3d_add_lod(node, NAN, mesh);
    EXPECT_TRUE(rt_scene_node3d_get_lod_count(node) == 1, "SceneNode accepts sanitized LOD");
    EXPECT_NEAR(rt_scene_node3d_get_lod_distance(node, 0),
                0.0,
                0.001,
                "SceneNode non-finite LOD distance clamps to zero");
    void *replacement = rt_mesh3d_new_box(0.5, 0.5, 0.5);
    rt_scene_node3d_add_lod(node, NAN, replacement);
    EXPECT_TRUE(rt_scene_node3d_get_lod_count(node) == 1,
                "SceneNode duplicate LOD thresholds replace the existing entry");
    EXPECT_TRUE(rt_scene_node3d_get_lod_mesh(node, 0) == replacement,
                "SceneNode duplicate LOD replacement keeps the new mesh");
}

static void test_scene_repairs_corrupt_private_counts() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(0.6, 0.7, 0.8);
    void *lod_node = rt_scene_node3d_new();
    void *lod_mesh = rt_mesh3d_new_box(0.5, 0.5, 0.5);

    rt_scene_node3d_set_name(parent, rt_const_cstr("corrupt_anim_target"));
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);

    auto *parent_view = static_cast<rt_scene_node3d *>(parent);
    parent_view->child_count = INT32_MAX;
    parent_view->child_capacity = 1;
    EXPECT_TRUE(rt_scene_node3d_child_count(parent) == 1,
                "SceneNode childCount clamps corrupt count to capacity");
    EXPECT_TRUE(rt_scene_node3d_get_child(parent, 1) == nullptr,
                "SceneNode GetChild rejects indexes beyond clamped child capacity");
    rt_scene_node3d *saved_child_slot = parent_view->children[0];
    parent_view->children[0] = reinterpret_cast<rt_scene_node3d *>(material);
    EXPECT_TRUE(rt_scene_node3d_get_child(parent, 0) == nullptr,
                "SceneNode GetChild rejects wrong-class private child slots");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 2,
                "SceneGraph node counting skips wrong-class private child slots");
    {
        const char *corrupt_save_path = "/tmp/zanna_scene_corrupt_child_slot_save.vscn";
        EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(corrupt_save_path)) == 1,
                    "SceneGraph.Save skips wrong-class private child slots");
        std::remove(corrupt_save_path);
    }
    parent_view->children[0] = saved_child_slot;
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 3,
                "SceneGraph node counting walks clamped child arrays");
    EXPECT_TRUE(rt_scene3d_find(scene, rt_const_cstr("corrupt_anim_target")) == parent,
                "SceneGraph find walks clamped child arrays");
    rt_scene_node3d *saved_parent_slot = parent_view->parent;
    parent_view->parent = reinterpret_cast<rt_scene_node3d *>(material);
    EXPECT_TRUE(rt_scene_node3d_get_parent(parent) == nullptr,
                "SceneNode GetParent rejects wrong-class private parent slots");
    parent_view->parent = saved_parent_slot;
    EXPECT_TRUE(rt_scene_node3d_get_parent(parent) == rt_scene3d_get_root(scene),
                "SceneNode GetParent returns valid parent slots");
    parent_view->sync_mode = INT32_MAX;
    EXPECT_TRUE(rt_scene_node3d_get_sync_mode(parent) == RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY,
                "SceneNode GetSyncMode defaults corrupt private sync modes");
    parent_view->sync_mode = RT_SCENE_NODE3D_SYNC_BODY_FROM_NODE;
    EXPECT_TRUE(rt_scene_node3d_get_sync_mode(parent) == RT_SCENE_NODE3D_SYNC_BODY_FROM_NODE,
                "SceneNode GetSyncMode preserves valid private sync modes");

    auto *scene_view = static_cast<rt_scene3d *>(scene);
    rt_scene_node3d *saved_scene_root = scene_view->root;
    scene_view->root = reinterpret_cast<rt_scene_node3d *>(material);
    EXPECT_TRUE(rt_scene3d_get_root(scene) == nullptr,
                "SceneGraph.GetRoot rejects wrong-class private root slots");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 0,
                "SceneGraph node counting rejects wrong-class private root slots");
    EXPECT_TRUE(rt_scene3d_find(scene, rt_const_cstr("corrupt_anim_target")) == nullptr,
                "SceneGraph.Find rejects wrong-class private root slots");
    rt_scene3d_clear(scene);
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 0,
                "SceneGraph.Clear ignores wrong-class private root slots");
    scene_view->root = saved_scene_root;
    EXPECT_TRUE(rt_scene3d_get_root(scene) == saved_scene_root,
                "SceneGraph.GetRoot returns restored valid root slots");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 3,
                "SceneGraph node counting recovers after restoring the root slot");

    scene_view->last_culled_count = -5;
    scene_view->last_visible_node_count = -6;
    scene_view->last_pvs_culled_count = -7;
    scene_view->visibility_zone_count = -8;
    scene_view->visibility_portal_count = -9;
    EXPECT_TRUE(rt_scene3d_get_culled_count(scene) == 0,
                "SceneGraph culled telemetry clamps corrupt negative counters");
    EXPECT_TRUE(rt_scene3d_get_visible_node_count(scene) == 0,
                "SceneGraph visible telemetry clamps corrupt negative counters");
    EXPECT_TRUE(rt_scene3d_get_pvs_culled_count(scene) == 0,
                "SceneGraph PVS telemetry clamps corrupt negative counters");
    EXPECT_TRUE(rt_scene3d_get_visibility_zone_count(scene) == 0,
                "SceneGraph visibility zone count clamps corrupt negative counters");
    EXPECT_TRUE(rt_scene3d_get_visibility_portal_count(scene) == 0,
                "SceneGraph visibility portal count clamps corrupt negative counters");

    void *zone_min = rt_vec3_new(-1.0, -1.0, -1.0);
    void *zone_max = rt_vec3_new(1.0, 1.0, 1.0);
    EXPECT_TRUE(
        rt_scene3d_add_visibility_zone(scene, rt_const_cstr("zone_a"), zone_min, zone_max) == 0,
        "SceneGraph visibility test fixture creates zone A");
    EXPECT_TRUE(
        rt_scene3d_add_visibility_zone(scene, rt_const_cstr("zone_b"), zone_min, zone_max) == 1,
        "SceneGraph visibility test fixture creates zone B");
    EXPECT_TRUE(rt_scene3d_add_visibility_portal(scene, 0, 1, 1) == 0,
                "SceneGraph visibility test fixture creates directed portals");
    int32_t saved_zone_count = scene_view->visibility_zone_count;
    int32_t saved_zone_capacity = scene_view->visibility_zone_capacity;
    int32_t saved_portal_count = scene_view->visibility_portal_count;
    int32_t saved_portal_capacity = scene_view->visibility_portal_capacity;
    scene_view->visibility_zone_count = INT32_MAX;
    scene_view->visibility_portal_count = INT32_MAX;
    EXPECT_TRUE(rt_scene3d_get_visibility_zone_count(scene) == saved_zone_capacity,
                "SceneGraph visibility zone count clamps corrupt counters to capacity");
    EXPECT_TRUE(rt_scene3d_get_visibility_portal_count(scene) == saved_portal_capacity,
                "SceneGraph visibility portal count clamps corrupt counters to capacity");
    scene_view->visibility_zone_count = saved_zone_count;
    scene_view->visibility_zone_capacity = saved_zone_capacity;
    scene_view->visibility_portal_count = saved_portal_count;
    scene_view->visibility_portal_capacity = saved_portal_capacity;

    rt_scene_node3d_set_mesh(parent, mesh);
    rt_scene_node3d_set_material(parent, material);
    parent_view->mesh = material;
    EXPECT_TRUE(rt_scene_node3d_get_mesh(parent) == nullptr,
                "SceneNode.GetMesh rejects wrong-class private mesh slots");
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_aabb_min(parent)),
                0.0,
                0.001,
                "SceneNode AABB queries skip wrong-class private mesh slots");
    parent_view->mesh = mesh;
    parent_view->material = mesh;
    EXPECT_TRUE(rt_scene_node3d_get_material(parent) == nullptr,
                "SceneNode.GetMaterial rejects wrong-class private material slots");
    parent_view->material = material;

    rt_scene_node3d_add_lod(parent, 8.0, mesh);
    parent_view->lod_count = INT32_MAX;
    parent_view->lod_capacity = 1;
    EXPECT_TRUE(rt_scene_node3d_get_lod_count(parent) == 1,
                "SceneNode lodCount clamps corrupt count to capacity");
    EXPECT_TRUE(rt_scene_node3d_get_lod_mesh(parent, 1) == nullptr,
                "SceneNode GetLodMesh rejects indexes beyond clamped LOD capacity");
    EXPECT_TRUE(rt_scene_node3d_get_lod_resident_bytes(parent, 1) == 0,
                "SceneNode LOD resident bytes rejects indexes beyond clamped capacity");
    parent_view->lod_levels[0].distance = NAN;
    EXPECT_TRUE(rt_scene_node3d_get_lod_distance(parent, 0) == 0.0,
                "SceneNode GetLodDistance sanitizes non-finite private LOD distances");
    parent_view->lod_levels[0].distance = -12.0;
    EXPECT_TRUE(rt_scene_node3d_get_lod_distance(parent, 0) == 0.0,
                "SceneNode GetLodDistance clamps negative private LOD distances");
    parent_view->lod_levels[0].distance = 8.0;
    parent_view->lod_levels[0].mesh = material;
    EXPECT_TRUE(rt_scene_node3d_get_lod_mesh(parent, 0) == nullptr,
                "SceneNode GetLodMesh rejects wrong-class private LOD mesh slots");
    parent_view->lod_levels[0].mesh = mesh;

    auto *lod_view = static_cast<rt_scene_node3d *>(lod_node);
    lod_view->lod_count = INT32_MAX;
    lod_view->lod_capacity = 4;
    lod_view->lod_levels = nullptr;
    rt_scene_node3d_add_lod(lod_node, 2.0, lod_mesh);
    EXPECT_TRUE(rt_scene_node3d_get_lod_count(lod_node) == 1,
                "SceneNode AddLOD repairs null LOD array with corrupt positive capacity");
    EXPECT_TRUE(rt_scene_node3d_get_lod_mesh(lod_node, 0) == lod_mesh,
                "SceneNode AddLOD writes the repaired LOD slot");

    double times[2] = {0.0, 1.0};
    float translation_values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    void *clip = rt_node_animation3d_new(rt_const_cstr("corrupt_clip"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(clip,
                                                rt_const_cstr("corrupt_anim_target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                translation_values) == 0,
                "NodeAnimation channel fixture is created");
    auto *clip_view = static_cast<rt_node_animation3d *>(clip);
    clip_view->channel_count = INT32_MAX;
    clip_view->channel_capacity = 1;
    void *clips[1] = {clip};
    auto *animator = static_cast<rt_node_animator3d *>(rt_node_animator3d_new_from_clips(clips, 1));
    EXPECT_TRUE(animator != nullptr, "NodeAnimator fixture is created");
    animator->animation_count = INT32_MAX;
    animator->animation_capacity = 1;
    rt_scene_node3d_bind_node_animator(parent, animator);
    rt_scene3d_sync_bindings(scene, 0.5);
    EXPECT_TRUE(clip_view->channel_count == 1,
                "NodeAnimator playback repairs corrupt clip channel count");
    EXPECT_TRUE(animator->animation_count == 1, "NodeAnimator playback repairs corrupt clip count");
    void *pos = rt_scene_node3d_get_position(parent);
    EXPECT_NEAR(rt_vec3_x(pos), 0.5, 0.001, "NodeAnimator still samples repaired channels");

    parent_view->bound_node_animator = rt_material3d_new_color(0.2, 0.3, 0.4);
    rt_scene_node3d_set_position(parent, 0.0, 0.0, 0.0);
    rt_scene3d_sync_bindings(scene, 0.5);
    pos = rt_scene_node3d_get_position(parent);
    EXPECT_NEAR(rt_vec3_x(pos),
                0.0,
                0.001,
                "SceneGraph.SyncBindings ignores wrong-class bound node animators");

    parent_view->bound_animator = rt_material3d_new_color(0.4, 0.3, 0.2);
    rt_scene_node3d_set_sync_mode(parent, RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION);
    rt_scene3d_sync_bindings(scene, 0.5);
    pos = rt_scene_node3d_get_position(parent);
    EXPECT_NEAR(rt_vec3_x(pos),
                0.0,
                0.001,
                "SceneGraph.SyncBindings ignores wrong-class root-motion animators");
}

static void test_node_body_sync_preserves_negative_scale_handedness() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *body = rt_body3d_new_sphere(0.5, 1.0);

    rt_scene_node3d_set_scale(node, -5.0, 3.0, 4.0);
    rt_scene_node3d_bind_body(node, body);
    rt_scene3d_add(scene, node);
    rt_body3d_set_position(body, 2.0, 3.0, 4.0);

    rt_scene3d_sync_bindings(scene, 0.016);

    void *scale = rt_scene_node3d_get_scale(node);
    EXPECT_NEAR(rt_vec3_x(scale), -5.0, 0.001, "SceneNode body sync preserves mirrored X scale");
    EXPECT_NEAR(rt_vec3_y(scale), 3.0, 0.001, "SceneNode body sync preserves Y scale");
    EXPECT_NEAR(rt_vec3_z(scale), 4.0, 0.001, "SceneNode body sync preserves Z scale");
}

/*==========================================================================
 * Frustum culling tests
 *=========================================================================*/

extern "C" {
extern void *rt_mesh3d_new_box(double w, double h, double d);
extern void *rt_mesh3d_new_sphere(double r, int64_t seg);
extern void *rt_mesh3d_new_plane(double sx, double sz);
extern int64_t rt_scene3d_get_culled_count(void *scene);
}

static int g_scene_submit_count = 0;
static int g_scene_begin_count = 0;
static int g_scene_end_count = 0;
static const void *g_scene_last_vertices = nullptr;
static uint32_t g_scene_last_vertex_count = 0;
static const void *g_scene_last_texture = nullptr;
static int8_t g_scene_last_unlit = 0;
static int32_t g_scene_last_bone_count = 0;
static vgfx3d_light_params_t g_scene_last_lights[VGFX3D_MAX_LIGHTS];
static int32_t g_scene_last_light_count = 0;

static void scene_test_begin_frame(void *, const vgfx3d_camera_params_t *) {
    g_scene_begin_count++;
}

static void scene_test_end_frame(void *) {
    g_scene_end_count++;
}

static void scene_test_submit_draw(void *,
                                   vgfx_window_t,
                                   const vgfx3d_draw_cmd_t *cmd,
                                   const vgfx3d_light_params_t *lights,
                                   int32_t light_count,
                                   const float *,
                                   int8_t,
                                   int8_t) {
    g_scene_submit_count++;
    g_scene_last_vertices = cmd ? cmd->vertices : nullptr;
    g_scene_last_vertex_count = cmd ? cmd->vertex_count : 0;
    g_scene_last_texture = cmd ? cmd->texture : nullptr;
    g_scene_last_unlit = cmd ? cmd->unlit : 0;
    g_scene_last_bone_count = cmd ? cmd->bone_count : 0;
    g_scene_last_light_count = light_count > VGFX3D_MAX_LIGHTS ? VGFX3D_MAX_LIGHTS : light_count;
    if (lights && g_scene_last_light_count > 0)
        std::memcpy(g_scene_last_lights,
                    lights,
                    (size_t)g_scene_last_light_count * sizeof(g_scene_last_lights[0]));
}

static void init_scene_test_canvas(rt_canvas3d *canvas, const vgfx3d_backend_t *backend) {
    std::memset(canvas, 0, sizeof(*canvas));
    canvas->backend = backend;
    canvas->gfx_win = (vgfx_window_t)1;
}

static void reset_scene_capture(void) {
    g_scene_submit_count = 0;
    g_scene_begin_count = 0;
    g_scene_end_count = 0;
    g_scene_last_vertices = nullptr;
    g_scene_last_vertex_count = 0;
    g_scene_last_texture = nullptr;
    g_scene_last_unlit = 0;
    g_scene_last_bone_count = 0;
    std::memset(g_scene_last_lights, 0, sizeof(g_scene_last_lights));
    g_scene_last_light_count = 0;
}

static void test_scene_spatial_queries_flat_walk_reference() {
    void *scene = rt_scene3d_new();
    void *near_node = rt_scene_node3d_new();
    void *far_node = rt_scene_node3d_new();
    void *hidden_node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);

    rt_scene_node3d_set_name(near_node, rt_const_cstr("near"));
    rt_scene_node3d_set_position(near_node, 0.0, 0.0, -4.0);
    rt_scene_node3d_set_mesh(near_node, mesh);
    rt_scene_node3d_set_material(near_node, material);
    rt_scene3d_add(scene, near_node);

    rt_scene_node3d_set_name(far_node, rt_const_cstr("far"));
    rt_scene_node3d_set_position(far_node, 0.0, 0.0, -6.0);
    rt_scene_node3d_set_mesh(far_node, mesh);
    rt_scene_node3d_set_material(far_node, material);
    rt_scene3d_add(scene, far_node);

    rt_scene_node3d_set_name(hidden_node, rt_const_cstr("hidden"));
    rt_scene_node3d_set_position(hidden_node, 3.0, 0.0, -4.0);
    rt_scene_node3d_set_mesh(hidden_node, mesh);
    rt_scene_node3d_set_material(hidden_node, material);
    rt_scene_node3d_set_visible(hidden_node, 0);
    rt_scene3d_add(scene, hidden_node);

    void *aabb_hits = rt_scene3d_query_aabb(
        scene, rt_vec3_new(-0.75, -0.75, -4.75), rt_vec3_new(0.75, 0.75, -3.25));
    EXPECT_TRUE(rt_seq_len(aabb_hits) == 1, "QueryAABB returns one matching visible mesh node");
    EXPECT_TRUE(rt_seq_get(aabb_hits, 0) == near_node, "QueryAABB returns the near node");

    void *sphere_hits = rt_scene3d_query_sphere(scene, rt_vec3_new(0.0, 0.0, -6.0), 0.75);
    EXPECT_TRUE(rt_seq_len(sphere_hits) == 1, "QuerySphere returns one matching visible node");
    EXPECT_TRUE(rt_seq_get(sphere_hits, 0) == far_node, "QuerySphere returns the far node");

    void *hidden_hits =
        rt_scene3d_query_aabb(scene, rt_vec3_new(2.0, -1.0, -5.0), rt_vec3_new(4.0, 1.0, -3.0));
    EXPECT_TRUE(rt_seq_len(hidden_hits) == 0, "Scene queries skip hidden subtrees");

    void *hit = rt_scene3d_raycast_nodes(
        scene, rt_vec3_new(0.0, 0.0, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 20.0);
    EXPECT_TRUE(hit == near_node, "RaycastNodes returns the closest visible mesh node");

    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;
    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    reset_scene_capture();
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    rt_camera3d_look_at(camera,
                        rt_vec3_new(0.0, 0.0, 2.0),
                        rt_vec3_new(0.0, 0.0, -5.0),
                        rt_vec3_new(0.0, 1.0, 0.0));

    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(rt_scene3d_get_visible_node_count(scene) == 2,
                "VisibleNodeCount tracks submitted drawable nodes from the last draw");
}

static void test_scene_spatial_query_append_telemetry_and_stable_order() {
    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *first = rt_scene_node3d_new();
    void *second = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    scene3d_spatial_candidate_list_t candidates = {};
    const double first_min[3] = {-1.0, -1.0, -5.0};
    const double first_max[3] = {1.0, 1.0, -3.0};
    const double second_min[3] = {9.0, -1.0, -5.0};
    const double second_max[3] = {11.0, 1.0, -3.0};

    rt_scene_node3d_set_position(first, 0.0, 0.0, -4.0);
    rt_scene_node3d_set_mesh(first, mesh);
    rt_scene3d_add(scene, first);
    rt_scene_node3d_set_position(second, 10.0, 0.0, -4.0);
    rt_scene_node3d_set_mesh(second, mesh);
    rt_scene3d_add(scene, second);

    EXPECT_TRUE(scene3d_spatial_collect_aabb(scene_impl, first_min, first_max, &candidates, 0) != 0,
                "internal AABB collection succeeds for the first append");
    EXPECT_TRUE(candidates.count == 1 && candidates.items[0]->node == first,
                "first spatial append contains the first node");
    EXPECT_TRUE(scene_impl->spatial_index.last_candidate_count == 1,
                "first spatial append reports its own candidate delta");
    int32_t *retained_stack = scene_impl->spatial_index.query_stack;

    EXPECT_TRUE(scene3d_spatial_collect_aabb(scene_impl, second_min, second_max, &candidates, 0) !=
                    0,
                "internal AABB collection succeeds when appending to an existing list");
    EXPECT_TRUE(candidates.count == 2 && candidates.items[0]->node == first &&
                    candidates.items[1]->node == second,
                "spatial append preserves stable scene traversal order");
    EXPECT_TRUE(scene_impl->spatial_index.last_candidate_count == 1,
                "appended spatial telemetry reports only the newly collected candidate");
    EXPECT_TRUE(scene_impl->spatial_index.query_stack == retained_stack &&
                    retained_stack != nullptr,
                "AABB traversal reuses its retained BVH stack across queries");
    EXPECT_TRUE(scene_impl->spatial_index.query_order_scratch == nullptr,
                "unordered internal collection skips all radix-ordering passes");
    std::free(candidates.items);
}

static void test_scene_spatial_queries_validate_vec3_args_before_result_alloc() {
    void *scene = rt_scene3d_new();
    void *min = rt_vec3_new(-1.0, -1.0, -1.0);
    void *max = rt_vec3_new(1.0, 1.0, 1.0);

    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_scene3d_query_aabb(scene, scene, max); },
                                     "min must be Vec3"),
                "QueryAABB traps with a clear message for non-Vec3 min");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_scene3d_query_aabb(scene, min, scene); },
                                     "max must be Vec3"),
                "QueryAABB traps with a clear message for non-Vec3 max");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_scene3d_query_sphere(scene, scene, 1.0); },
                                     "center must be Vec3"),
                "QuerySphere traps with a clear message for non-Vec3 center");
}

static void test_scene_precise_raycast_selects_through_aabb_gaps() {
    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *corner_node = rt_scene_node3d_new();
    void *behind_node = rt_scene_node3d_new();
    void *farther_node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);

    /* A box rotated 45° about Z inflates its world AABB past its geometry:
     * the ray below passes inside that AABB but outside the diamond
     * cross-section, so only triangle-accurate picking can reject it. */
    rt_scene_node3d_set_name(corner_node, rt_const_cstr("corner"));
    rt_scene_node3d_set_position(corner_node, 1.2, 0.0, -4.0);
    rt_scene_node3d_set_rotation(corner_node, rt_quat_from_euler(0.0, 0.0, M_PI / 4.0));
    rt_scene_node3d_set_mesh(corner_node, mesh);
    rt_scene_node3d_set_material(corner_node, material);
    rt_scene3d_add(scene, corner_node);

    rt_scene_node3d_set_name(behind_node, rt_const_cstr("behind"));
    rt_scene_node3d_set_position(behind_node, 0.0, 0.5, -8.0);
    rt_scene_node3d_set_mesh(behind_node, mesh);
    rt_scene_node3d_set_material(behind_node, material);
    rt_scene3d_add(scene, behind_node);

    rt_scene_node3d_set_name(farther_node, rt_const_cstr("farther"));
    rt_scene_node3d_set_position(farther_node, 0.0, 0.5, -12.0);
    rt_scene_node3d_set_mesh(farther_node, mesh);
    rt_scene_node3d_set_material(farther_node, material);
    rt_scene3d_add(scene, farther_node);

    /* The AABB query provably picks the wrong node here — the empty box
     * corner is nearer than the box actually on the ray. */
    void *aabb_hit = rt_scene3d_raycast_nodes(
        scene, rt_vec3_new(0.0, 0.5, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 40.0);
    EXPECT_TRUE(aabb_hit == corner_node,
                "RaycastNodes hits the rotated box's empty AABB corner (the gap scenario)");

    void *precise_hit = rt_scene3d_raycast_nodes_precise(
        scene, rt_vec3_new(0.0, 0.5, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 40.0);
    EXPECT_TRUE(precise_hit == behind_node,
                "RaycastNodesPrecise selects the triangle-hit node behind the empty corner");

    void *all_hits = rt_scene3d_raycast_nodes_precise_all(
        scene, rt_vec3_new(0.0, 0.5, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 40.0);
    EXPECT_TRUE(rt_seq_len(all_hits) == 2,
                "RaycastNodesPreciseAll returns only triangle-hit nodes");
    EXPECT_TRUE(rt_seq_get(all_hits, 0) == behind_node,
                "RaycastNodesPreciseAll sorts the nearer hit first");
    EXPECT_TRUE(rt_seq_get(all_hits, 1) == farther_node,
                "RaycastNodesPreciseAll sorts the farther hit second");
    void *retained_precise_hits = scene_impl->query_precise_hits;
    int32_t retained_precise_capacity = scene_impl->query_precise_hit_capacity;
    EXPECT_TRUE(retained_precise_hits != nullptr && retained_precise_capacity >= 2,
                "precise collect-all retains its hit scratch on the scene");
    void *repeated_hits = rt_scene3d_raycast_nodes_precise_all(
        scene, rt_vec3_new(0.0, 0.5, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 40.0);
    EXPECT_TRUE(rt_seq_len(repeated_hits) == 2 &&
                    scene_impl->query_precise_hits == retained_precise_hits &&
                    scene_impl->query_precise_hit_capacity == retained_precise_capacity,
                "precise collect-all reuses retained hit scratch across calls");

    void *hit_info = rt_scene3d_raycast_precise_hit(
        scene, rt_vec3_new(0.0, 0.5, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 40.0);
    EXPECT_TRUE(hit_info != nullptr, "RaycastPreciseHit returns the nearest triangle hit");
    if (hit_info) {
        EXPECT_NEAR(rt_ray3d_hit_distance(hit_info),
                    7.0,
                    1e-6,
                    "RaycastPreciseHit distance reaches the near face of the behind box");
        void *point = rt_ray3d_hit_point(hit_info);
        EXPECT_NEAR(rt_vec3_z(point), -7.0, 1e-6, "RaycastPreciseHit point sits on the near face");
        void *normal = rt_ray3d_hit_normal(hit_info);
        EXPECT_NEAR(rt_vec3_z(normal), 1.0, 1e-6, "RaycastPreciseHit face normal faces the ray");
    }

    /* Range and visibility follow the AABB query's contract. */
    void *capped = rt_scene3d_raycast_nodes_precise(
        scene, rt_vec3_new(0.0, 0.5, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 5.0);
    EXPECT_TRUE(capped == nullptr, "RaycastNodesPrecise respects the distance cap");

    rt_scene_node3d_set_visible(behind_node, 0);
    void *skipping_hidden = rt_scene3d_raycast_nodes_precise(
        scene, rt_vec3_new(0.0, 0.5, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 40.0);
    EXPECT_TRUE(skipping_hidden == farther_node,
                "RaycastNodesPrecise skips hidden nodes like the AABB query");
    rt_scene_node3d_set_visible(behind_node, 1);

    EXPECT_TRUE(expect_trap_contains(
                    [&] {
                        (void)rt_scene3d_raycast_nodes_precise(
                            scene, scene, rt_vec3_new(0.0, 0.0, -1.0), 40.0);
                    },
                    "origin must be Vec3"),
                "RaycastNodesPrecise traps with a clear message for non-Vec3 origin");
}

static void test_scene_precise_raycast_allocation_failure_returns_no_partial_result() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
    rt_scene_node3d_set_position(node, 0.0, 0.0, -5.0);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene3d_add(scene, node);

    g_last_trap = nullptr;
    g_return_from_trap = true;
    rt_scene3d_test_set_precise_hit_growth_failure(1);
    void *hits = rt_scene3d_raycast_nodes_precise_all(
        scene, rt_vec3_new(0.0, 0.0, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 20.0);
    rt_scene3d_test_set_precise_hit_growth_failure(0);
    g_return_from_trap = false;
    EXPECT_TRUE(hits == nullptr,
                "precise collect-all discards partial hits after allocation failure");
    EXPECT_TRUE(g_last_trap && std::strstr(g_last_trap, "result allocation failed"),
                "precise collect-all reports its allocation failure before returning null");
}

static void test_scene_spatial_index_rebuilds_on_dirty_node() {
    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);

    rt_scene_node3d_set_position(node, 0.0, 0.0, -4.0);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene3d_add(scene, node);

    void *first_hits =
        rt_scene3d_query_aabb(scene, rt_vec3_new(-1.0, -1.0, -5.0), rt_vec3_new(1.0, 1.0, -3.0));
    EXPECT_TRUE(rt_seq_len(first_hits) == 1, "Indexed QueryAABB returns the initial node");
    EXPECT_TRUE(scene_impl->spatial_index.valid == 1,
                "SceneGraph spatial index is valid after query");
    EXPECT_TRUE(scene_impl->spatial_index.count == 1,
                "SceneGraph spatial index tracks drawable nodes");
    EXPECT_TRUE(scene_impl->spatial_index.root_node >= 0,
                "SceneGraph spatial index builds a BVH root");
    EXPECT_TRUE(scene_impl->spatial_index.node_count == 1,
                "SceneGraph spatial index uses a single BVH leaf for one drawable");
    uint32_t first_build_count = scene_impl->spatial_index.build_count;
    uint32_t first_refit_count = scene_impl->spatial_index.refit_count;

    void *second_hits =
        rt_scene3d_query_aabb(scene, rt_vec3_new(-1.0, -1.0, -5.0), rt_vec3_new(1.0, 1.0, -3.0));
    EXPECT_TRUE(rt_seq_len(second_hits) == 1, "Indexed QueryAABB remains stable across reuse");
    EXPECT_TRUE(scene_impl->spatial_index.build_count == first_build_count,
                "SceneGraph spatial index reuses a clean build");
    EXPECT_TRUE(scene_impl->spatial_index.refit_count == first_refit_count,
                "SceneGraph spatial index does not refit a clean build");

    rt_scene_node3d_set_position(node, 10.0, 0.0, -4.0);
    EXPECT_TRUE(scene_impl->spatial_index.dirty_node_count == 1 &&
                    scene_impl->spatial_index.dirty_all == 0,
                "transform invalidation queues only its changed subtree");
    void *old_hits =
        rt_scene3d_query_aabb(scene, rt_vec3_new(-1.0, -1.0, -5.0), rt_vec3_new(1.0, 1.0, -3.0));
    EXPECT_TRUE(rt_seq_len(old_hits) == 0,
                "Dirty spatial index drops the moved node from old bounds");
    EXPECT_TRUE(scene_impl->spatial_index.build_count == first_build_count,
                "SceneGraph spatial index refits instead of rebuilding after a transform change");
    EXPECT_TRUE(scene_impl->spatial_index.refit_count == first_refit_count + 1,
                "SceneGraph spatial index records the transform-only refit");
    EXPECT_TRUE(scene_impl->spatial_index.dirty_node_count == 0,
                "subtree refit drains the dirty-node queue");

    void *new_hits =
        rt_scene3d_query_aabb(scene, rt_vec3_new(9.0, -1.0, -5.0), rt_vec3_new(11.0, 1.0, -3.0));
    EXPECT_TRUE(rt_seq_len(new_hits) == 1, "Dirty spatial index returns the moved node");

    rt_mesh3d_add_vertex(mesh, 10.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    void *geometry_hits =
        rt_scene3d_query_aabb(scene, rt_vec3_new(19.0, -1.0, -5.0), rt_vec3_new(21.0, 1.0, -3.0));
    EXPECT_TRUE(rt_seq_len(geometry_hits) == 1,
                "Spatial index observes geometry changes on an already-bound mesh");
    EXPECT_TRUE(scene_impl->spatial_index.refit_count == first_refit_count + 2,
                "Spatial index refits after bound mesh geometry changes");

    rt_scene_node3d_set_visible(node, 0);
    void *hidden_hits =
        rt_scene3d_query_aabb(scene, rt_vec3_new(9.0, -1.0, -5.0), rt_vec3_new(11.0, 1.0, -3.0));
    EXPECT_TRUE(rt_seq_len(hidden_hits) == 0, "Spatial index filters hidden nodes from queries");
    EXPECT_TRUE(scene_impl->spatial_index.build_count == first_build_count,
                "SceneGraph spatial index refits (does NOT rebuild) on a visibility toggle");
    EXPECT_TRUE(scene_impl->spatial_index.refit_count == first_refit_count + 3,
                "SceneGraph spatial index records the visibility refit");

    rt_scene_node3d_set_visible(node, 1);
    void *reshown_hits =
        rt_scene3d_query_aabb(scene, rt_vec3_new(9.0, -1.0, -5.0), rt_vec3_new(11.0, 1.0, -3.0));
    EXPECT_TRUE(rt_seq_len(reshown_hits) == 1,
                "Spatial index restores re-shown nodes without a rebuild");
    EXPECT_TRUE(scene_impl->spatial_index.build_count == first_build_count,
                "SceneGraph spatial index still has not rebuilt after re-show");
}

static void test_scene_spatial_mesh_dependencies_are_scene_local() {
    void *scene_a = rt_scene3d_new();
    void *scene_b = rt_scene3d_new();
    auto *a = static_cast<rt_scene3d *>(scene_a);
    auto *b = static_cast<rt_scene3d *>(scene_b);
    void *node_a = rt_scene_node3d_new();
    void *node_b = rt_scene_node3d_new();
    void *mesh_a = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mesh_b = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    rt_scene_node3d_set_mesh(node_a, mesh_a);
    rt_scene_node3d_set_mesh(node_b, mesh_b);
    rt_scene3d_add(scene_a, node_a);
    rt_scene3d_add(scene_b, node_b);
    auto query = [](void *scene) {
        return rt_scene3d_query_aabb(
            scene, rt_vec3_new(-2.0, -2.0, -2.0), rt_vec3_new(2.0, 2.0, 2.0));
    };
    EXPECT_TRUE(rt_seq_len(query(scene_a)) == 1 && rt_seq_len(query(scene_b)) == 1,
                "scene-local mesh dependency fixtures build both indexes");
    uint32_t b_refits = b->spatial_index.refit_count;

    rt_mesh3d_add_vertex(mesh_a, 5.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    EXPECT_TRUE(rt_seq_len(query(scene_b)) == 1,
                "an unrelated mesh mutation leaves the second scene query unchanged");
    EXPECT_TRUE(b->spatial_index.last_mesh_dependency_probe_count == 1,
                "the unrelated scene checks only its unique mesh dependency");
    EXPECT_TRUE(b->spatial_index.last_full_geometry_scan_count == 0 &&
                    b->spatial_index.refit_count == b_refits,
                "the unrelated scene does not scan or refit all spatial entries");

    uint32_t a_refits = a->spatial_index.refit_count;
    (void)query(scene_a);
    EXPECT_TRUE(a->spatial_index.last_mesh_dependency_probe_count == 1 &&
                    a->spatial_index.last_full_geometry_scan_count == 1 &&
                    a->spatial_index.refit_count == a_refits + 1,
                "the scene that owns the changed mesh still performs its required refit");
}

static void test_scene_spatial_index_repairs_storage_and_query_pool_metadata() {
    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *node = rt_scene_node3d_new();
    rt_scene_node3d_set_mesh(node, rt_mesh3d_new_box(1.0, 1.0, 1.0));
    rt_scene_node3d_set_position(node, 0.0, 0.0, -4.0);
    rt_scene3d_add(scene, node);
    auto query = [&] {
        return rt_scene3d_query_aabb(
            scene, rt_vec3_new(-1.0, -1.0, -5.0), rt_vec3_new(1.0, 1.0, -3.0));
    };

    EXPECT_TRUE(rt_seq_len(query()) == 1, "spatial storage fixture builds its initial index");

    rt_scene3d_spatial_entry *orphaned_entries = scene_impl->spatial_index.entries;
    scene_impl->spatial_index.entries = nullptr;
    EXPECT_TRUE(rt_seq_len(query()) == 1,
                "null entry pointer with positive capacity rebuilds instead of dereferencing");
    free(orphaned_entries);

    int32_t *orphaned_indices = scene_impl->spatial_index.entry_indices;
    scene_impl->spatial_index.entry_indices = nullptr;
    EXPECT_TRUE(rt_seq_len(query()) == 1,
                "null leaf-order pointer with positive capacity rebuilds safely");
    free(orphaned_indices);

    rt_scene3d_spatial_bvh_node *orphaned_nodes = scene_impl->spatial_index.nodes;
    scene_impl->spatial_index.nodes = nullptr;
    EXPECT_TRUE(rt_seq_len(query()) == 1,
                "null BVH pointer with positive capacity rebuilds safely");
    free(orphaned_nodes);

    scene_impl->spatial_index.capacity = std::numeric_limits<int32_t>::max();
    EXPECT_TRUE(rt_seq_len(query()) == 1,
                "policy-exceeding spatial capacity is discarded and rebuilt");

    free(scene_impl->query_candidates);
    scene_impl->query_candidates = nullptr;
    scene_impl->query_candidate_capacity = 64;
    EXPECT_TRUE(rt_seq_len(query()) == 1,
                "null pooled candidate pointer with positive capacity is repaired");
}

static void test_scene_spatial_index_contains_corrupt_cached_topology() {
    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    std::vector<void *> nodes;
    for (int i = 0; i < 12; ++i) {
        void *node = rt_scene_node3d_new();
        rt_scene_node3d_set_mesh(node, mesh);
        rt_scene_node3d_set_position(node, (double)i * 2.0, 0.0, -4.0);
        rt_scene3d_add(scene, node);
        nodes.push_back(node);
    }
    auto query_all = [&] {
        return rt_scene3d_query_aabb(
            scene, rt_vec3_new(-2.0, -2.0, -6.0), rt_vec3_new(30.0, 2.0, -2.0));
    };

    EXPECT_TRUE(rt_seq_len(query_all()) == 12 && scene_impl->spatial_index.node_count > 1,
                "topology corruption fixture builds an internal BVH");
    EXPECT_TRUE(scene_impl->spatial_index.query_stack != nullptr &&
                    scene_impl->spatial_index.query_stack_capacity > 0,
                "BVH query traversal stack is retained for subsequent queries");
    EXPECT_TRUE(scene_impl->spatial_index.query_order_scratch != nullptr &&
                    scene_impl->spatial_index.query_order_scratch_capacity >= 12,
                "linear candidate ordering retains high-water scratch");

    int32_t root = scene_impl->spatial_index.root_node;
    int64_t build_count = scene_impl->spatial_index.build_count;
    scene_impl->spatial_index.nodes[root].parent = root;
    EXPECT_TRUE(rt_seq_len(query_all()) == 12,
                "non-root cached parent topology rebuilds without looping");
    EXPECT_TRUE(scene_impl->spatial_index.build_count == build_count + 1 &&
                    scene_impl->spatial_index.nodes[scene_impl->spatial_index.root_node].parent ==
                        -1,
                "invalid BVH root parent is detected and repaired");

    root = scene_impl->spatial_index.root_node;
    scene_impl->spatial_index.nodes[root].left = root;
    EXPECT_TRUE(rt_seq_len(query_all()) == 12,
                "self-referential BVH child falls back without looping or losing results");
    EXPECT_TRUE(scene_impl->spatial_index.valid == 0,
                "invalid BVH child topology invalidates the cache");
    EXPECT_TRUE(rt_seq_len(query_all()) == 12 && scene_impl->spatial_index.valid == 1,
                "the next indexed query rebuilds invalid child topology");

    root = scene_impl->spatial_index.root_node;
    scene_impl->spatial_index.nodes[root].world_min[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(rt_seq_len(query_all()) == 12,
                "non-finite cached BVH bounds fail over to the flat reference");
    (void)query_all();

    scene_impl->spatial_index.entry_indices[0] = std::numeric_limits<int32_t>::max();
    EXPECT_TRUE(rt_seq_len(query_all()) == 12,
                "out-of-range leaf entry indexes cannot escape the allocation");
    (void)query_all();

    int32_t leaf = -1;
    for (int32_t i = 0; i < scene_impl->spatial_index.node_count; ++i) {
        if (scene_impl->spatial_index.nodes[i].leaf) {
            leaf = i;
            break;
        }
    }
    EXPECT_TRUE(leaf >= 0, "topology fixture exposes a leaf for range corruption");
    scene_impl->spatial_index.nodes[leaf].start = std::numeric_limits<int32_t>::max();
    EXPECT_TRUE(rt_seq_len(query_all()) == 12,
                "overflowing cached leaf range falls back without an out-of-bounds read");
    (void)query_all();

    auto *cycle_node = (rt_scene_node3d *)nodes[0];
    rt_scene_node3d *saved_parent = cycle_node->parent;
    cycle_node->parent = cycle_node;
    scene_impl->spatial_index.dirty = 1;
    EXPECT_TRUE(rt_seq_len(query_all()) == 11,
                "cyclic parent chains are skipped with bounded work during rebuild");
    cycle_node->parent = saved_parent;
    scene_impl->spatial_index.dirty = 1;
    scene_impl->spatial_index.topology_dirty = 1;
    EXPECT_TRUE(rt_seq_len(query_all()) == 12,
                "restoring the parent chain makes the node indexable again");
}

static void test_scene_spatial_refit_repairs_bounds_when_revisions_agree() {
    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *node = rt_scene_node3d_new();
    auto *node_impl = (rt_scene_node3d *)node;
    rt_scene_node3d_set_mesh(node, rt_mesh3d_new_box(1.0, 1.0, 1.0));
    rt_scene_node3d_set_position(node, 0.0, 0.0, -4.0);
    rt_scene3d_add(scene, node);
    auto query = [&] {
        return rt_scene3d_query_aabb(
            scene, rt_vec3_new(-1.0, -1.0, -5.0), rt_vec3_new(1.0, 1.0, -3.0));
    };
    EXPECT_TRUE(rt_seq_len(query()) == 1, "refit corruption fixture builds its initial leaf");

    auto &entry = scene_impl->spatial_index.entries[0];
    auto &root = scene_impl->spatial_index.nodes[scene_impl->spatial_index.root_node];
    for (int axis = 0; axis < 3; ++axis) {
        entry.world_min[axis] = 100.0;
        entry.world_max[axis] = 101.0;
        root.world_min[axis] = 100.0;
        root.world_max[axis] = 101.0;
    }
    entry.leaf_node = -1;
    node_impl->world_dirty = 1;
    scene_impl->spatial_index.dirty = 1;
    EXPECT_TRUE(rt_seq_len(query()) == 1,
                "a refresh attempt refits the tree even when revisions remain unchanged");
    EXPECT_TRUE(root.world_min[0] < 1.0 && root.world_max[0] > -1.0,
                "full refit replaces stale cached root bounds");
}

static void test_scene_draw_spatial_index_matches_flat_reference() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);

    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    rt_camera3d_look_at(camera,
                        rt_vec3_new(0.0, 0.0, 2.0),
                        rt_vec3_new(0.0, 0.0, -5.0),
                        rt_vec3_new(0.0, 1.0, 0.0));

    void *visible_node = rt_scene_node3d_new();
    rt_scene_node3d_set_position(visible_node, 0.0, 0.0, -5.0);
    rt_scene_node3d_set_mesh(visible_node, mesh);
    rt_scene_node3d_set_material(visible_node, material);
    rt_scene3d_add(scene, visible_node);

    for (int i = 0; i < 6; ++i) {
        void *offscreen = rt_scene_node3d_new();
        rt_scene_node3d_set_position(offscreen, 200.0 + (double)i * 3.0, 0.0, -5.0);
        rt_scene_node3d_set_mesh(offscreen, mesh);
        rt_scene_node3d_set_material(offscreen, material);
        rt_scene3d_add(scene, offscreen);
    }

    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);
    int indexed_submit_count = g_scene_submit_count;
    int64_t indexed_culled_count = rt_scene3d_get_culled_count(scene);
    int64_t indexed_visible_count = rt_scene3d_get_visible_node_count(scene);
    int32_t indexed_candidates = scene_impl->spatial_index.last_candidate_count;
    int32_t indexed_prefiltered = scene_impl->spatial_index.last_prefiltered_count;
    int32_t indexed_entries = scene_impl->spatial_index.count;

    scene_impl->use_spatial_index = 0;
    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(indexed_submit_count == g_scene_submit_count,
                "Indexed SceneGraph.Draw submits the same nodes as the flat path");
    EXPECT_TRUE(indexed_culled_count == rt_scene3d_get_culled_count(scene),
                "Indexed SceneGraph.Draw preserves flat-path culled count");
    EXPECT_TRUE(indexed_visible_count == rt_scene3d_get_visible_node_count(scene),
                "Indexed SceneGraph.Draw preserves flat-path visible node count");
    EXPECT_TRUE(indexed_prefiltered > 0, "SceneGraph spatial draw prefilters off-frustum nodes");
    EXPECT_TRUE(indexed_candidates < indexed_entries,
                "SceneGraph spatial draw yields fewer candidates than indexed drawables");
}

static void test_scene_spatial_index_10k_scaling_fixture() {
    constexpr int kColumns = 100;
    constexpr int kRows = 100;
    constexpr int kNodeCount = kColumns * kRows;
    constexpr double kSpacing = 8.0;

    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);

    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);

    void *target_node = nullptr;
    const int target_col = 37;
    const int target_row = 41;
    const double target_x = (double)target_col * kSpacing;
    const double target_z = -10.0 - (double)target_row * kSpacing;

    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kColumns; ++col) {
            void *node = rt_scene_node3d_new();
            rt_scene_node3d_set_position(
                node, (double)col * kSpacing, 0.0, -10.0 - (double)row * kSpacing);
            rt_scene_node3d_set_mesh(node, mesh);
            rt_scene_node3d_set_material(node, material);
            rt_scene3d_add(scene, node);
            if (col == target_col && row == target_row)
                target_node = node;
        }
    }

    void *hits = rt_scene3d_query_aabb(scene,
                                       rt_vec3_new(target_x - 0.75, -1.0, target_z - 0.75),
                                       rt_vec3_new(target_x + 0.75, 1.0, target_z + 0.75));
    EXPECT_TRUE(rt_seq_len(hits) == 1, "10k QueryAABB fixture returns the isolated target node");
    EXPECT_TRUE(rt_seq_get(hits, 0) == target_node,
                "10k QueryAABB fixture preserves node identity");
    EXPECT_TRUE(scene_impl->spatial_index.valid == 1, "10k fixture builds a valid spatial index");
    EXPECT_TRUE(scene_impl->spatial_index.count == kNodeCount,
                "10k fixture indexes every visible drawable node");
    EXPECT_TRUE(scene_impl->spatial_index.node_count > 1,
                "10k fixture builds a multi-node BVH instead of a flat sweep array");
    EXPECT_TRUE(scene_impl->spatial_index.root_node >= 0, "10k fixture records a BVH root node");
    EXPECT_TRUE(scene_impl->spatial_index.last_candidate_count == 1,
                "10k fixture narrows an isolated query to one spatial candidate");
    EXPECT_TRUE(scene_impl->spatial_index.last_prefiltered_count >= kNodeCount - 1,
                "10k fixture records broad prefiltering for isolated spatial queries");

    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 120.0);
    rt_camera3d_look_at(camera,
                        rt_vec3_new(0.0, 5.0, 5.0),
                        rt_vec3_new(0.0, 0.0, -40.0),
                        rt_vec3_new(0.0, 1.0, 0.0));

    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);
    int indexed_submit_count = g_scene_submit_count;
    int64_t indexed_culled_count = rt_scene3d_get_culled_count(scene);
    int64_t indexed_visible_count = rt_scene3d_get_visible_node_count(scene);
    int32_t indexed_candidates = scene_impl->spatial_index.last_candidate_count;
    int32_t indexed_prefiltered = scene_impl->spatial_index.last_prefiltered_count;

    EXPECT_TRUE(indexed_submit_count > 0, "10k draw fixture submits visible nodes");
    EXPECT_TRUE(indexed_visible_count == indexed_submit_count,
                "10k draw fixture visible-node counter matches submissions");
    EXPECT_TRUE(indexed_candidates < 1000,
                "10k draw fixture keeps indexed draw candidates below 10 percent of total nodes");
    EXPECT_TRUE(indexed_prefiltered > 9000,
                "10k draw fixture prefilters most off-frustum drawable nodes");

    scene_impl->use_spatial_index = 0;
    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(indexed_submit_count == g_scene_submit_count,
                "10k indexed SceneGraph.Draw submits the same nodes as the flat path");
    EXPECT_TRUE(indexed_culled_count == rt_scene3d_get_culled_count(scene),
                "10k indexed SceneGraph.Draw preserves flat-path culled count");
    EXPECT_TRUE(indexed_visible_count == rt_scene3d_get_visible_node_count(scene),
                "10k indexed SceneGraph.Draw preserves flat-path visible node count");

    // Indexed-vs-flat cull speedup baseline (AC-003): an isolated point query narrows
    // to one node through the BVH but scans all 10k AABBs on the flat path. Time both
    // and record the ratio as telemetry for the perf baseline (not a pass/fail gate).
    void *q_min = rt_vec3_new(target_x - 0.75, -1.0, target_z - 0.75);
    void *q_max = rt_vec3_new(target_x + 0.75, 1.0, target_z + 0.75);
    constexpr int kQueryIterations = 4000;

    scene_impl->use_spatial_index = 1;
    (void)rt_scene3d_query_aabb(scene, q_min, q_max); // warm the index
    auto indexed_begin = std::chrono::steady_clock::now();
    for (int i = 0; i < kQueryIterations; ++i)
        (void)rt_scene3d_query_aabb(scene, q_min, q_max);
    auto indexed_end = std::chrono::steady_clock::now();

    scene_impl->use_spatial_index = 0;
    auto flat_begin = std::chrono::steady_clock::now();
    for (int i = 0; i < kQueryIterations; ++i)
        (void)rt_scene3d_query_aabb(scene, q_min, q_max);
    auto flat_end = std::chrono::steady_clock::now();
    scene_impl->use_spatial_index = 1;

    long long indexed_us = (long long)std::chrono::duration_cast<std::chrono::microseconds>(
                               indexed_end - indexed_begin)
                               .count();
    long long flat_us =
        (long long)std::chrono::duration_cast<std::chrono::microseconds>(flat_end - flat_begin)
            .count();
    double speedup = indexed_us > 0 ? (double)flat_us / (double)indexed_us : 0.0;
    std::printf("SCENE3D_INDEX_SPEEDUP_TARGET: nodes=%d queries=%d flat_us=%lld indexed_us=%lld "
                "speedup=%.2fx\n",
                kNodeCount,
                kQueryIterations,
                flat_us,
                indexed_us,
                speedup);
    EXPECT_TRUE(flat_us >= indexed_us,
                "10k indexed point query is no slower than the flat O(N) sweep");
}

static void test_scene_occlusion_grid_uses_spatial_candidates() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);

    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *mesh = rt_mesh3d_new_box(2.0, 2.0, 0.2);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    rt_camera3d_look_at(
        camera, rt_vec3_new(0.0, 0.0, 5.0), rt_vec3_new(0.0, 0.0, 0.0), rt_vec3_new(0.0, 1.0, 0.0));

    void *front = rt_scene_node3d_new();
    rt_scene_node3d_set_position(front, 0.0, 0.0, 0.0);
    rt_scene_node3d_set_mesh(front, mesh);
    rt_scene_node3d_set_material(front, material);
    rt_scene3d_add(scene, front);

    void *covered = rt_scene_node3d_new();
    rt_scene_node3d_set_position(covered, 0.0, 0.0, -2.0);
    rt_scene_node3d_set_mesh(covered, mesh);
    rt_scene_node3d_set_material(covered, material);
    rt_scene3d_add(scene, covered);

    for (int i = 0; i < 128; ++i) {
        void *offscreen = rt_scene_node3d_new();
        rt_scene_node3d_set_position(offscreen, 200.0 + (double)i * 4.0, 0.0, -2.0);
        rt_scene_node3d_set_mesh(offscreen, mesh);
        rt_scene_node3d_set_material(offscreen, material);
        rt_scene3d_add(scene, offscreen);
    }

    rt_canvas3d_set_occlusion_culling(&canvas, 1);
    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);
    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);
    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(scene_impl->spatial_index.count == 130,
                "SceneGraph occlusion fixture indexes every drawable node");
    EXPECT_TRUE(
        scene_impl->spatial_index.last_candidate_count == 2,
        "SceneGraph spatial index narrows occlusion-visible draw candidates before sorting");
    EXPECT_TRUE(scene_impl->spatial_index.last_prefiltered_count >= 128,
                "SceneGraph spatial index prefilters off-frustum occlusion non-candidates");
    EXPECT_TRUE(rt_canvas3d_get_occlusion_candidate_count(&canvas) == 2,
                "Canvas3D CPU occlusion grid tests only spatial draw candidates");
    EXPECT_TRUE(rt_canvas3d_get_occluded_draw_count(&canvas) >= 129,
                "Canvas3D visibility telemetry includes spatial prefilter and occlusion skips");
    EXPECT_TRUE(g_scene_submit_count == 1,
                "SceneGraph indexed occlusion submits only the unoccluded front draw");
}

static void test_scene_shadow_caster_sweep_keeps_offscreen_casters() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);

    void *scene = rt_scene3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    rt_camera3d_look_at(
        camera, rt_vec3_new(0.0, 0.0, 5.0), rt_vec3_new(0.0, 0.0, 0.0), rt_vec3_new(0.0, 1.0, 0.0));

    void *visible = rt_scene_node3d_new();
    rt_scene_node3d_set_position(visible, 0.0, 0.0, 0.0);
    rt_scene_node3d_set_mesh(visible, mesh);
    rt_scene_node3d_set_material(visible, material);
    rt_scene3d_add(scene, visible);

    /* Far above the view cone: its shadow (sun pointing straight down) falls into the
     * frustum, so shadow-aware traversal must keep it while plain culling drops it. */
    void *caster = rt_scene_node3d_new();
    rt_scene_node3d_set_position(caster, 0.0, 60.0, -2.0);
    rt_scene_node3d_set_mesh(caster, mesh);
    rt_scene_node3d_set_material(caster, material);
    rt_scene3d_add(scene, caster);

    /* Without shadows: the off-screen caster is frustum-culled at traversal. */
    rt_canvas3d_begin(&canvas, camera);
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(canvas.draw_count == 1, "plain traversal culls the off-screen node");
    EXPECT_TRUE(canvas.shadow_caster_sweep_active == 0,
                "no sweep is active while shadows are disabled");
    rt_canvas3d_end(&canvas);

    void *sun_dir = rt_vec3_new(0.0, -1.0, 0.0);
    void *sun = rt_light3d_new_directional(sun_dir, 1.0, 1.0, 1.0);
    rt_light3d_set_casts_shadows(sun, 1);
    rt_canvas3d_set_light(&canvas, 0, sun);
    rt_canvas3d_enable_shadows(&canvas, 256);

    rt_canvas3d_begin(&canvas, camera);
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(canvas.shadow_caster_sweep_active == 1,
                "a shadow-casting directional light activates the caster sweep");
    EXPECT_TRUE(canvas.shadow_caster_sweep[1] < -50.0f,
                "the sweep extends along the sun's travel direction");
    EXPECT_TRUE(canvas.draw_count == 2,
                "shadow-aware traversal keeps the off-screen caster enqueued");
    rt_canvas3d_end(&canvas);
}

static void test_scene_portal_pvs_culls_unlinked_interior_zones() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);

    void *scene = rt_scene3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(70.0, 1.0, 0.1, 100.0);
    rt_camera3d_look_at(camera,
                        rt_vec3_new(0.0, 0.0, 0.0),
                        rt_vec3_new(10.0, 0.0, 0.0),
                        rt_vec3_new(0.0, 1.0, 0.0));

    int64_t zone_a = rt_scene3d_add_visibility_zone(
        scene, rt_const_cstr("room-a"), rt_vec3_new(-5.0, -5.0, -5.0), rt_vec3_new(5.0, 5.0, 5.0));
    int64_t zone_b = rt_scene3d_add_visibility_zone(
        scene, rt_const_cstr("room-b"), rt_vec3_new(8.0, -5.0, -5.0), rt_vec3_new(16.0, 5.0, 5.0));
    int64_t zone_c = rt_scene3d_add_visibility_zone(
        scene, rt_const_cstr("room-c"), rt_vec3_new(28.0, -5.0, -5.0), rt_vec3_new(36.0, 5.0, 5.0));
    EXPECT_TRUE(zone_a == 0 && zone_b == 1 && zone_c == 2,
                "SceneGraph visibility zones return stable zero-based indexes");
    EXPECT_TRUE(rt_scene3d_add_visibility_portal(scene, zone_a, zone_b, 1) == 0,
                "SceneGraph visibility portal links adjacent rooms");
    EXPECT_TRUE(rt_scene3d_get_visibility_zone_count(scene) == 3,
                "SceneGraph reports authored visibility zone count");
    EXPECT_TRUE(rt_scene3d_get_visibility_portal_count(scene) == 2,
                "SceneGraph stores bidirectional portals as directed PVS links");

    for (int i = 0; i < 3; ++i) {
        void *node = rt_scene_node3d_new();
        rt_scene_node3d_set_position(node, i == 0 ? 2.0 : (i == 1 ? 12.0 : 32.0), 0.0, 0.0);
        rt_scene_node3d_set_mesh(node, mesh);
        rt_scene_node3d_set_material(node, material);
        rt_scene3d_add(scene, node);
    }

    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 2,
                "SceneGraph portal/PVS culls the unlinked interior room");
    EXPECT_TRUE(rt_scene3d_get_pvs_culled_count(scene) == 1,
                "SceneGraph.PvsCulledCount reports portal/PVS skips");
    EXPECT_TRUE(rt_scene3d_get_visible_node_count(scene) == 2,
                "SceneGraph visible-node telemetry excludes PVS-hidden rooms");

    EXPECT_TRUE(rt_scene3d_add_visibility_portal(scene, zone_b, zone_c, 1) == 2,
                "SceneGraph can extend the authored PVS graph");
    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 3,
                "SceneGraph portal/PVS reveals rooms reachable through authored portals");
    EXPECT_TRUE(rt_scene3d_get_pvs_culled_count(scene) == 0,
                "SceneGraph.PvsCulledCount clears when all authored zones are visible");
}

static void test_scene_spatial_index_preserves_far_origin_precision() {
    void *scene = rt_scene3d_new();
    auto *scene_impl = (rt_scene3d *)scene;
    void *mesh = rt_mesh3d_new_box(0.25, 0.25, 0.25);

    constexpr double kBase = 1000000000.0;
    void *target_node = rt_scene_node3d_new();
    rt_scene_node3d_set_position(target_node, kBase, 0.0, -10.0);
    rt_scene_node3d_set_mesh(target_node, mesh);
    rt_scene3d_add(scene, target_node);

    void *offset_node = rt_scene_node3d_new();
    rt_scene_node3d_set_position(offset_node, kBase + 4.0, 0.0, -5.0);
    rt_scene_node3d_set_mesh(offset_node, mesh);
    rt_scene3d_add(scene, offset_node);

    void *aabb_hits = rt_scene3d_query_aabb(
        scene, rt_vec3_new(kBase - 0.5, -1.0, -10.5), rt_vec3_new(kBase + 0.5, 1.0, -9.5));
    EXPECT_TRUE(rt_seq_len(aabb_hits) == 1,
                "Far-origin QueryAABB keeps sub-float-spaced nodes distinct");
    EXPECT_TRUE(rt_seq_get(aabb_hits, 0) == target_node,
                "Far-origin QueryAABB returns the exact target node");
    EXPECT_TRUE(scene_impl->spatial_index.last_candidate_count == 1,
                "Far-origin spatial index culls the offset node before exact testing");

    void *sphere_hits = rt_scene3d_query_sphere(scene, rt_vec3_new(kBase, 0.0, -10.0), 0.75);
    EXPECT_TRUE(rt_seq_len(sphere_hits) == 1,
                "Far-origin QuerySphere keeps sub-float-spaced nodes distinct");
    EXPECT_TRUE(rt_seq_get(sphere_hits, 0) == target_node,
                "Far-origin QuerySphere returns the exact target node");

    void *ray_hit = rt_scene3d_raycast_nodes(
        scene, rt_vec3_new(kBase, 0.0, 0.0), rt_vec3_new(0.0, 0.0, -1.0), 20.0);
    EXPECT_TRUE(ray_hit == target_node,
                "Far-origin RaycastNodes does not collapse the nearer offset node onto the ray");
}

static void test_scene_draw_reuses_active_frame() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    canvas.in_frame = 1;
    canvas.frame_is_2d = 0;
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_scene_begin_count == 0,
                "SceneGraph.Draw does not nest Begin when a 3D frame is already active");
    EXPECT_TRUE(g_scene_end_count == 0, "SceneGraph.Draw does not end an externally-owned frame");
    EXPECT_TRUE(canvas.draw_count == 1,
                "SceneGraph.Draw queues scene geometry inside an externally-owned frame");
    EXPECT_TRUE(g_scene_submit_count == 0,
                "SceneGraph.Draw defers backend submission until the caller ends the frame");
    EXPECT_TRUE(canvas.in_frame == 1, "SceneGraph.Draw leaves the caller-owned frame active");
}

static void test_scene_draw_culling_uses_canvas_output_aspect() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    canvas.width = 200;
    canvas.height = 100;
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    rt_scene_node3d_set_position(node, 4.0, 0.0, 0.0);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(
        g_scene_submit_count == 1,
        "SceneGraph culling uses the active canvas aspect instead of the camera's stored aspect");
    EXPECT_TRUE(rt_scene3d_get_culled_count(scene) == 0,
                "Wide active outputs keep edge-visible nodes from being culled by a stale camera "
                "projection");
}

static void test_scene_save_escapes_json_names() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    const char *path = "/tmp/zanna_scene_escape_test.vscn";
    const char *name = "quote\"slash\\line\nbreak";

    rt_scene_node3d_set_name(node, rt_const_cstr(name));
    rt_scene3d_add(scene, node);

    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save writes the scene file");

    std::string text;
    EXPECT_TRUE(read_text_file(path, text), "SceneGraph.Save output can be reopened");
    if (text.empty())
        return;

    EXPECT_TRUE(text.find("\"name\": \"quote\\\"slash\\\\line\\nbreak\"") != std::string::npos,
                "SceneGraph.Save escapes quotes, backslashes, and newlines in node names");
}

static void test_scene_save_text_matches_file_bytes() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    const char *path = "/tmp/zanna_scene_save_text_parity.vscn";

    rt_scene_node3d_set_name(node, rt_const_cstr("text-parity"));
    rt_scene_node3d_set_mesh(node, rt_mesh3d_new_box(1.0, 1.0, 1.0));
    rt_scene_node3d_set_material(node, rt_material3d_new_color(0.2, 0.4, 0.6));
    /* Typed metadata promotes the document to v6 so version selection is
     * exercised identically on both endpoints (ADR 0190). */
    rt_scene_node3d_metadata_set_int(node, rt_const_cstr("hp"), 7);
    rt_scene3d_add(scene, node);

    rt_string memory = rt_scene3d_save_text(scene);
    EXPECT_TRUE(memory && rt_str_len(memory) > 0, "SceneGraph.SaveToText returns document text");
    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save writes the SaveToText parity file");

    std::string file_text;
    EXPECT_TRUE(read_text_file(path, file_text), "SaveToText parity file can be reopened");
    std::string memory_text(rt_string_cstr(memory), (size_t)rt_str_len(memory));
    EXPECT_TRUE(memory_text == file_text,
                "SceneGraph.SaveToText matches SceneGraph.Save bytes exactly (ADR 0190)");
    EXPECT_TRUE(memory_text.find("\"version\": 6") != std::string::npos,
                "SceneGraph.SaveToText applies the same version-selection rules as Save");

    EXPECT_TRUE(rt_str_len(rt_scene3d_save_text(NULL)) == 0,
                "SceneGraph.SaveToText returns the empty string for null scenes");
    EXPECT_TRUE(rt_str_len(rt_scene3d_save_text(node)) == 0,
                "SceneGraph.SaveToText returns the empty string for wrong handles");
    std::remove(path);
}

static void test_scene_save_serializes_visibility_and_lod_metadata() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    const char *path = "/tmp/zanna_scene_metadata_test.vscn";

    rt_scene_node3d_set_visible(node, 0);
    rt_scene_node3d_set_mesh(node, rt_mesh3d_new_box(1.0, 1.0, 1.0));
    rt_scene_node3d_set_material(node, rt_material3d_new_color(1.0, 0.0, 0.0));
    rt_scene_node3d_add_lod(node, 10.0, rt_mesh3d_new_box(0.5, 0.5, 0.5));
    rt_scene_node3d_set_lod_resident(node, 0, 0);
    rt_scene_node3d_set_auto_lod(node, 1, 12.5);
    rt_scene3d_add(scene, node);

    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save writes metadata-rich scene files");

    std::string text;
    EXPECT_TRUE(read_text_file(path, text), "SceneGraph.Save metadata output can be reopened");
    if (text.empty())
        return;

    EXPECT_TRUE(text.find("\"visible\": false") != std::string::npos,
                "SceneGraph.Save serializes node visibility");
    EXPECT_TRUE(text.find("\"hasMesh\": true") != std::string::npos,
                "SceneGraph.Save serializes mesh presence");
    EXPECT_TRUE(text.find("\"hasMaterial\": true") != std::string::npos,
                "SceneGraph.Save serializes material presence");
    EXPECT_TRUE(text.find("\"lod\": [") != std::string::npos,
                "SceneGraph.Save serializes LOD metadata");
    EXPECT_TRUE(text.find("\"distance\": 10") != std::string::npos,
                "SceneGraph.Save serializes LOD distances");
    EXPECT_TRUE(text.find("\"resident\": false") != std::string::npos,
                "SceneGraph.Save serializes nonresident mesh state");
    EXPECT_TRUE(text.find("\"autoLOD\": {") != std::string::npos,
                "SceneGraph.Save serializes auto-LOD metadata");
    EXPECT_TRUE(text.find("\"screenErrorPx\": 12.5") != std::string::npos,
                "SceneGraph.Save serializes auto-LOD screen error");
}

static void test_scene_roundtrip_loads_shared_assets() {
    const char *path = "/tmp/zanna_scene_roundtrip_test.vscn";
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new();
    void *lod_mesh = rt_mesh3d_new_box(0.5, 0.5, 0.5);
    rt_material3d *material = (rt_material3d *)rt_material3d_new();

    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    ((rt_mesh3d *)mesh)->vertices[0].tangent[0] = 1.0f;
    ((rt_mesh3d *)mesh)->vertices[0].bone_indices[0] = 3;
    ((rt_mesh3d *)mesh)->vertices[0].bone_weights[0] = 1.0f;
    ((rt_mesh3d *)mesh)->bone_count = 4;

    void *diffuse = rt_pixels_new(2, 1);
    void *normal = rt_pixels_new(1, 1);
    void *specular = rt_pixels_new(1, 1);
    void *emissive = rt_pixels_new(1, 1);
    void *metallic_roughness = rt_pixels_new(1, 1);
    void *ao = rt_pixels_new(1, 1);
    void *faces[6];
    const int64_t face_colors[6] = {
        0xFF0000FFll, 0x00FF00FFll, 0x0000FFFFll, 0xFFFF00FFll, 0xFF00FFFFll, 0x00FFFFFFll};

    rt_pixels_set(diffuse, 0, 0, 0x10203040ll);
    rt_pixels_set(diffuse, 1, 0, 0x50607080ll);
    rt_pixels_set(normal, 0, 0, 0x7F7FFFFFll);
    rt_pixels_set(specular, 0, 0, 0x808080FFll);
    rt_pixels_set(emissive, 0, 0, 0xFF8040FFll);
    rt_pixels_set(metallic_roughness, 0, 0, 0x2244CCFFll);
    rt_pixels_set(ao, 0, 0, 0x7F0000FFll);
    for (int i = 0; i < 6; i++) {
        faces[i] = rt_pixels_new(1, 1);
        rt_pixels_set(faces[i], 0, 0, face_colors[i]);
    }

    material->diffuse[0] = 0.25;
    material->diffuse[1] = 0.5;
    material->diffuse[2] = 0.75;
    material->diffuse[3] = 0.9;
    material->specular[0] = 0.2;
    material->specular[1] = 0.4;
    material->specular[2] = 0.6;
    material->shininess = 48.0;
    material->workflow = RT_MATERIAL3D_WORKFLOW_PBR;
    material->emissive[0] = 0.1;
    material->emissive[1] = 0.2;
    material->emissive[2] = 0.3;
    material->metallic = 0.65;
    material->roughness = 0.35;
    material->ao = 0.55;
    material->emissive_intensity = 1.8;
    material->normal_scale = 0.7;
    material->alpha = 0.8;
    material->alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
    material->alpha_cutoff = 0.42;
    material->double_sided = 1;
    material->reflectivity = 0.6;
    material->unlit = 1;
    material->shading_model = 4;
    material->custom_params[0] = 3.5;
    material->custom_params[1] = 1.25;
    material->texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] = 1;
    material->texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] =
        RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE;
    material->texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] =
        RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT;
    material->texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] =
        RT_MATERIAL3D_TEXTURE_FILTER_NEAREST;
    material->texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] =
        RT_MATERIAL3D_TEXTURE_FILTER_NEAREST;
    material->texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] =
        RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    material->texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] =
        RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST;
    material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][0] = 2.0;
    material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][3] = 3.0;
    material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][4] = 0.25;
    material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][5] = 0.5;
    material->texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL] = 1;
    material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL][0] = 0.5;
    material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL][3] = 0.75;
    material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL][4] = -0.125;
    material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL][5] = 0.875;
    material->texture_wrap_s = material->texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_wrap_t = material->texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_filter = material->texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_min_filter =
        material->texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_mag_filter =
        material->texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_mip_filter =
        material->texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    rt_material3d_set_texture(material, diffuse);
    rt_material3d_set_normal_map(material, normal);
    rt_material3d_set_specular_map(material, specular);
    rt_material3d_set_emissive_map(material, emissive);
    rt_material3d_set_metallic_roughness_map(material, metallic_roughness);
    rt_material3d_set_ao_map(material, ao);
    rt_material3d_set_env_map(
        material, rt_cubemap3d_new(faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]));

    rt_scene_node3d_set_name(parent, rt_const_cstr("parent"));
    rt_scene_node3d_set_name(child, rt_const_cstr("child"));
    rt_scene_node3d_set_position(parent, 1.0, 2.0, 3.0);
    rt_scene_node3d_set_scale(parent, 2.0, 2.0, 2.0);
    rt_scene_node3d_set_visible(parent, 0);
    rt_scene_node3d_set_mesh(parent, mesh);
    rt_scene_node3d_set_material(parent, material);
    rt_scene_node3d_add_lod(parent, 10.0, lod_mesh);
    rt_scene_node3d_set_lod_resident(parent, 0, 0);
    rt_scene_node3d_set_auto_lod(parent, 1, 18.0);
    rt_scene_node3d_set_mesh(child, mesh);
    rt_scene_node3d_set_material(child, material);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);

    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save writes roundtrip scene files");

    void *loaded_scene = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded_scene != nullptr, "SceneGraph.Load reconstructs saved scenes");
    if (!loaded_scene)
        return;

    void *loaded_parent = rt_scene3d_find(loaded_scene, rt_const_cstr("parent"));
    void *loaded_child = rt_scene3d_find(loaded_scene, rt_const_cstr("child"));
    EXPECT_TRUE(loaded_parent != nullptr, "SceneGraph.Load restores named parent nodes");
    EXPECT_TRUE(loaded_child != nullptr, "SceneGraph.Load restores named child nodes");
    if (!loaded_parent || !loaded_child)
        return;

    EXPECT_TRUE(rt_scene_node3d_get_parent(loaded_child) == loaded_parent,
                "SceneGraph.Load restores hierarchy links");
    EXPECT_TRUE(rt_scene_node3d_get_visible(loaded_parent) == 0,
                "SceneGraph.Load restores node visibility");
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(loaded_parent)),
                1.0,
                0.001,
                "SceneGraph.Load restores node position");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_scale(loaded_parent)),
                2.0,
                0.001,
                "SceneGraph.Load restores node scale");
    EXPECT_TRUE(rt_scene_node3d_get_mesh(loaded_parent) == rt_scene_node3d_get_mesh(loaded_child),
                "SceneGraph.Load preserves shared mesh references");
    EXPECT_TRUE(rt_scene_node3d_get_material(loaded_parent) ==
                    rt_scene_node3d_get_material(loaded_child),
                "SceneGraph.Load preserves shared material references");
    EXPECT_TRUE(rt_scene_node3d_get_lod_count(loaded_parent) == 1,
                "SceneGraph.Load restores LOD entries");
    EXPECT_NEAR(rt_scene_node3d_get_lod_distance(loaded_parent, 0),
                10.0,
                0.001,
                "SceneGraph.Load restores LOD distances");
    EXPECT_TRUE(rt_scene_node3d_get_lod_mesh(loaded_parent, 0) !=
                    rt_scene_node3d_get_mesh(loaded_parent),
                "SceneGraph.Load restores LOD mesh references");
    EXPECT_TRUE(rt_scene_node3d_get_lod_resident(loaded_parent, 0) == 0,
                "SceneGraph.Load restores nonresident LOD mesh state");
    EXPECT_TRUE(rt_scene_node3d_get_lod_resident_bytes(loaded_parent, 0) == 0,
                "SceneGraph.Load preserves zero resident bytes for nonresident LODs");
    EXPECT_TRUE(((rt_scene_node3d *)loaded_parent)->auto_lod_enabled == 1,
                "SceneGraph.Load restores auto-LOD enable state");
    EXPECT_NEAR(((rt_scene_node3d *)loaded_parent)->auto_lod_screen_error_px,
                18.0,
                0.001,
                "SceneGraph.Load restores auto-LOD screen error");

    rt_mesh3d *loaded_mesh = (rt_mesh3d *)rt_scene_node3d_get_mesh(loaded_parent);
    EXPECT_TRUE(loaded_mesh != nullptr && loaded_mesh->vertex_count == 3 &&
                    loaded_mesh->index_count == 3,
                "SceneGraph.Load restores mesh geometry");
    if (loaded_mesh) {
        EXPECT_NEAR(loaded_mesh->vertices[1].pos[0],
                    1.0,
                    0.001,
                    "SceneGraph.Load restores vertex positions");
        EXPECT_NEAR(loaded_mesh->vertices[0].tangent[0],
                    1.0,
                    0.001,
                    "SceneGraph.Load restores vertex tangents");
        EXPECT_TRUE(loaded_mesh->vertices[0].bone_indices[0] == 3 &&
                        fabs(loaded_mesh->vertices[0].bone_weights[0] - 1.0f) < 0.001f &&
                        loaded_mesh->bone_count == 4,
                    "SceneGraph.Load restores skinning vertex data");
    }

    rt_material3d *loaded_material = (rt_material3d *)rt_scene_node3d_get_material(loaded_parent);
    EXPECT_TRUE(loaded_material != nullptr, "SceneGraph.Load restores materials");
    if (loaded_material) {
        EXPECT_NEAR(loaded_material->diffuse[0],
                    0.25,
                    0.001,
                    "SceneGraph.Load restores material diffuse color");
        EXPECT_NEAR(loaded_material->alpha, 0.8, 0.001, "SceneGraph.Load restores material alpha");
        EXPECT_TRUE(loaded_material->workflow == RT_MATERIAL3D_WORKFLOW_PBR,
                    "SceneGraph.Load restores the PBR workflow");
        EXPECT_NEAR(
            loaded_material->metallic, 0.65, 0.001, "SceneGraph.Load restores material metallic");
        EXPECT_NEAR(
            loaded_material->roughness, 0.35, 0.001, "SceneGraph.Load restores material roughness");
        EXPECT_NEAR(loaded_material->ao, 0.55, 0.001, "SceneGraph.Load restores material AO");
        EXPECT_NEAR(loaded_material->emissive_intensity,
                    1.8,
                    0.001,
                    "SceneGraph.Load restores emissive intensity");
        EXPECT_NEAR(
            loaded_material->normal_scale, 0.7, 0.001, "SceneGraph.Load restores normal scale");
        EXPECT_TRUE(loaded_material->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND &&
                        std::fabs(loaded_material->alpha_cutoff - 0.42) < 0.001 &&
                        loaded_material->double_sided == 1,
                    "SceneGraph.Load restores alpha-mode and culling flags");
        EXPECT_NEAR(loaded_material->reflectivity,
                    0.6,
                    0.001,
                    "SceneGraph.Load restores material reflectivity");
        EXPECT_TRUE(loaded_material->unlit == 1 && loaded_material->shading_model == 4,
                    "SceneGraph.Load restores material shading flags");
        EXPECT_NEAR(loaded_material->custom_params[0],
                    3.5,
                    0.001,
                    "SceneGraph.Load restores custom shader params");
        EXPECT_TRUE(
            loaded_material->texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] == 1 &&
                loaded_material->texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                    RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE &&
                loaded_material->texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                    RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT &&
                loaded_material->texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                    RT_MATERIAL3D_TEXTURE_FILTER_NEAREST &&
                loaded_material->texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                    RT_MATERIAL3D_TEXTURE_FILTER_LINEAR &&
                loaded_material->texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                    RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST,
            "SceneGraph.Load restores base texture slot sampler and UV set");
        EXPECT_NEAR(
            loaded_material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][0],
            2.0,
            0.001,
            "SceneGraph.Load restores base texture slot U scale");
        EXPECT_NEAR(
            loaded_material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][5],
            0.5,
            0.001,
            "SceneGraph.Load restores base texture slot V offset");
        EXPECT_TRUE(
            loaded_material->texture_wrap_s == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE &&
                loaded_material->texture_wrap_t == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT &&
                loaded_material->texture_filter == RT_MATERIAL3D_TEXTURE_FILTER_LINEAR &&
                loaded_material->texture_min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST &&
                loaded_material->texture_mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_LINEAR &&
                loaded_material->texture_mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST,
            "SceneGraph.Load keeps legacy primary sampler fields in sync");
        EXPECT_TRUE(loaded_material->texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL] == 1,
                    "SceneGraph.Load restores normal-map texture coordinate set");
        EXPECT_NEAR(
            loaded_material->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL][4],
            -0.125,
            0.001,
            "SceneGraph.Load restores independent normal-map UV transform");
        EXPECT_TRUE(loaded_material->texture != nullptr &&
                        rt_pixels_get(loaded_material->texture, 0, 0) == 0x10203040ll &&
                        rt_pixels_get(loaded_material->texture, 1, 0) == 0x50607080ll,
                    "SceneGraph.Load restores diffuse textures");
        EXPECT_TRUE(loaded_material->normal_map != nullptr &&
                        rt_pixels_get(loaded_material->normal_map, 0, 0) == 0x7F7FFFFFll,
                    "SceneGraph.Load restores normal maps");
        EXPECT_TRUE(loaded_material->specular_map != nullptr &&
                        rt_pixels_get(loaded_material->specular_map, 0, 0) == 0x808080FFll,
                    "SceneGraph.Load restores specular maps");
        EXPECT_TRUE(loaded_material->emissive_map != nullptr &&
                        rt_pixels_get(loaded_material->emissive_map, 0, 0) == 0xFF8040FFll,
                    "SceneGraph.Load restores emissive maps");
        EXPECT_TRUE(loaded_material->metallic_roughness_map != nullptr &&
                        rt_pixels_get(loaded_material->metallic_roughness_map, 0, 0) ==
                            0x2244CCFFll,
                    "SceneGraph.Load restores metallic-roughness maps");
        EXPECT_TRUE(loaded_material->ao_map != nullptr &&
                        rt_pixels_get(loaded_material->ao_map, 0, 0) == 0x7F0000FFll,
                    "SceneGraph.Load restores AO maps");
        rt_cubemap3d *env = (rt_cubemap3d *)loaded_material->env_map;
        EXPECT_TRUE(env != nullptr && env->face_size == 1,
                    "SceneGraph.Load restores environment cubemaps");
        if (env) {
            EXPECT_TRUE(rt_pixels_get(env->faces[0], 0, 0) == face_colors[0] &&
                            rt_pixels_get(env->faces[5], 0, 0) == face_colors[5],
                        "SceneGraph.Load restores cubemap face textures");
        }
    }
}

static void test_node_animator_handles_large_morph_weight_channels() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new();
    void *morph = rt_morphtarget3d_new(3);
    double times[2] = {0.0, 1.0};
    float values[40] = {};
    void *anim;
    void *animator;
    void *clips[1];

    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);

    for (int i = 0; i < 20; i++)
        rt_morphtarget3d_add_shape(morph, rt_const_cstr("shape"));
    rt_mesh3d_set_morph_targets(mesh, morph);
    rt_scene_node3d_set_name(node, rt_const_cstr("target"));
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene3d_add(scene, node);

    values[20 + 19] = 0.8f;
    anim = rt_node_animation3d_new(rt_const_cstr("many_weights"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_WEIGHTS,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                20,
                                                times,
                                                values) >= 0,
                "Node animation accepts morph-weight channels wider than the stack scratch buffer");
    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);

    EXPECT_NEAR(rt_morphtarget3d_get_weight(morph, 19),
                0.4,
                0.001,
                "Node animation applies morph weights beyond the old fixed scratch limit");
}

static void test_node_animator_public_controls_drive_bound_nodes() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    double times[2] = {0.0, 1.0};
    float translation_values[6] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    void *clip = rt_node_animation3d_new(rt_const_cstr("move"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(clip,
                                                rt_const_cstr("public_target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                translation_values) >= 0,
                "Node animation public-control fixture creates a translation channel");
    void *animator = rt_node_animator3d_new(clip);
    EXPECT_TRUE(animator != nullptr, "NodeAnimator3D.New creates an animator");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_node_animation3d_get_name(clip)), "move") == 0,
                "NodeAnimation3D.Name exposes the clip name");
    EXPECT_NEAR(rt_node_animation3d_get_duration(clip),
                1.0,
                0.0001,
                "NodeAnimation3D.Duration exposes the clip duration");
    EXPECT_TRUE(rt_node_animation3d_get_channel_count(clip) == 1,
                "NodeAnimation3D.ChannelCount exposes repaired channel count");
    EXPECT_TRUE(rt_node_animator3d_get_clip_count(animator) == 1,
                "NodeAnimator3D.ClipCount exposes the retained clip");
    EXPECT_TRUE(rt_node_animator3d_get_clip(animator, 0) == clip,
                "NodeAnimator3D.GetClip returns the retained clip");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_node_animator3d_get_clip_name(animator, 0)), "move") == 0,
        "NodeAnimator3D.GetClipName exposes clip names");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_node_animator3d_get_current_clip(animator)), "move") == 0,
        "NodeAnimator3D.CurrentClip starts on the first clip");
    EXPECT_TRUE(rt_node_animator3d_get_playing(animator) != 0,
                "NodeAnimator3D.Playing defaults to true");
    EXPECT_NEAR(rt_node_animator3d_get_speed(animator),
                1.0,
                0.0001,
                "NodeAnimator3D.Speed defaults to one");

    rt_scene_node3d_set_name(node, rt_const_cstr("public_target"));
    rt_scene3d_add(scene, node);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    EXPECT_TRUE(rt_scene_node3d_get_node_animator(rt_scene3d_get_root(scene)) == animator,
                "SceneNode.NodeAnimator returns the bound NodeAnimator3D");

    rt_scene3d_sync_bindings(scene, 0.25);
    void *pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 0.5, 0.001, "NodeAnimator3D.Update drives bound nodes");
    EXPECT_NEAR(rt_node_animator3d_get_time(animator),
                0.25,
                0.001,
                "NodeAnimator3D.Time advances during SceneGraph.SyncBindings");

    rt_node_animator3d_set_speed(animator, 2.0);
    rt_scene3d_sync_bindings(scene, 0.25);
    pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 1.5, 0.001, "NodeAnimator3D.SetSpeed scales playback time");

    rt_node_animator3d_stop(animator);
    rt_scene3d_sync_bindings(scene, 0.25);
    pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 1.5, 0.001, "NodeAnimator3D.Stop pauses scene-side updates");
    EXPECT_TRUE(rt_node_animator3d_get_playing(animator) == 0,
                "NodeAnimator3D.Playing reflects Stop");

    EXPECT_TRUE(rt_node_animator3d_play(animator, rt_const_cstr("missing")) == 0,
                "NodeAnimator3D.Play rejects unknown clips");
    EXPECT_TRUE(rt_node_animator3d_play(animator, rt_const_cstr("move")) != 0,
                "NodeAnimator3D.Play restarts a named clip");
    rt_node_animator3d_set_time(animator, 0.5);
    rt_node_animator3d_update(animator, 0.0);
    pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 1.0, 0.001, "NodeAnimator3D.SetTime applies on manual update");

    rt_scene_node3d_clear_node_animator_binding(rt_scene3d_get_root(scene));
    EXPECT_TRUE(rt_scene_node3d_get_node_animator(rt_scene3d_get_root(scene)) == nullptr,
                "SceneNode.ClearNodeAnimatorBinding detaches the NodeAnimator3D");
}

static void test_node_animator_empty_clip_is_noop_but_advances_time() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *clip = rt_node_animation3d_new(rt_const_cstr("empty"), 1.0);
    void *animator = rt_node_animator3d_new(clip);

    rt_scene_node3d_set_name(node, rt_const_cstr("empty_target"));
    rt_scene_node3d_set_position(node, 3.0, 0.0, 0.0);
    rt_scene3d_add(scene, node);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.25);

    void *pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 3.0, 0.001, "Empty NodeAnimation3D clips leave nodes unchanged");
    EXPECT_NEAR(rt_node_animator3d_get_time(animator),
                0.25,
                0.001,
                "Empty NodeAnimation3D clips still advance animator time");
}

static void test_node_animator_clears_unkeyed_morph_weight_tail() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new();
    void *morph = rt_morphtarget3d_new(3);
    double times[2] = {0.0, 1.0};
    float values[2] = {0.0f, 1.0f};
    void *anim;
    void *animator;
    void *clips[1];

    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);

    rt_morphtarget3d_add_shape(morph, rt_const_cstr("driven"));
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("stale"));
    rt_morphtarget3d_set_weight(morph, 1, 0.75);
    rt_mesh3d_set_morph_targets(mesh, morph);
    rt_scene_node3d_set_name(node, rt_const_cstr("target"));
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene3d_add(scene, node);

    anim = rt_node_animation3d_new(rt_const_cstr("short_weights"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_WEIGHTS,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                1,
                                                times,
                                                values) >= 0,
                "Node animation accepts a short morph-weight vector");
    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);

    EXPECT_NEAR(rt_morphtarget3d_get_weight(morph, 0),
                0.5,
                0.001,
                "Node animation applies provided morph weights");
    EXPECT_NEAR(rt_morphtarget3d_get_weight(morph, 1),
                0.0,
                0.001,
                "Node animation clears morph weights outside the sampled vector");
}

static void test_node_animator_skips_corrupt_channel_shape() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new();
    void *morph = rt_morphtarget3d_new(3);
    double times[2] = {0.0, 1.0};
    float values[2] = {0.0f, 1.0f};
    void *anim;
    void *animator;
    void *clips[1];

    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("shape"));
    rt_mesh3d_set_morph_targets(mesh, morph);
    rt_scene_node3d_set_name(node, rt_const_cstr("target"));
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene3d_add(scene, node);

    anim = rt_node_animation3d_new(rt_const_cstr("corrupt_shape"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_WEIGHTS,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                1,
                                                times,
                                                values) >= 0,
                "Node animation accepts the valid fixture channel before corruption");
    auto *anim_view = static_cast<rt_node_animation3d *>(anim);
    anim_view->channels[0].key_count = INT32_MAX;
    anim_view->channels[0].value_width = INT32_MAX;

    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(morph, 0),
                0.0,
                0.001,
                "Node animator skips privately corrupt channel key/value dimensions");
}

static void test_node_animator_skips_corrupt_channel_interpolation() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    double times[2] = {0.0, 1.0};
    float values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    void *anim;
    void *animator;
    void *clips[1];

    rt_scene_node3d_set_name(node, rt_const_cstr("target"));
    rt_scene3d_add(scene, node);
    anim = rt_node_animation3d_new(rt_const_cstr("bad_interp"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                values) >= 0,
                "Node animation accepts the valid interpolation fixture");
    auto *anim_view = static_cast<rt_node_animation3d *>(anim);
    anim_view->channels[0].interpolation = INT32_MAX;

    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);

    void *pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(
        rt_vec3_x(pos), 0.0, 0.001, "Node animator skips privately corrupt interpolation values");
}

static void test_node_animator_import_index_binding_does_not_fallback_to_duplicate_name() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    double times[2] = {0.0, 1.0};
    float values[6] = {0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f};
    void *anim;
    void *animator;
    void *clips[1];

    rt_scene_node3d_set_name(node, rt_const_cstr("duplicate"));
    static_cast<rt_scene_node3d *>(node)->import_index = 7;
    rt_scene3d_add(scene, node);

    anim = rt_node_animation3d_new(rt_const_cstr("indexed"), 1.0);
    int64_t channel = rt_node_animation3d_add_channel(anim,
                                                      rt_const_cstr("duplicate"),
                                                      RT_NODE_ANIM_PATH_TRANSLATION,
                                                      RT_NODE_ANIM_INTERP_LINEAR,
                                                      2,
                                                      3,
                                                      times,
                                                      values);
    EXPECT_TRUE(channel >= 0, "Node animation accepts the indexed-target fixture");
    rt_node_animation3d_set_channel_target_node_index(anim, channel, 42);

    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);

    void *pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos),
                0.0,
                0.001,
                "Import-index-bound node animation does not drive a same-name fallback node");
}

static void test_node_animator_rebuilds_all_targets_in_one_tree_walk() {
    constexpr int node_count = 64;
    void *scene = rt_scene3d_new();
    void *clip = rt_node_animation3d_new(rt_const_cstr("many_targets"), 1.0);
    double times[2] = {0.0, 1.0};
    float values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

    for (int i = 0; i < node_count; i++) {
        char name_bytes[32];
        int name_length = std::snprintf(name_bytes, sizeof(name_bytes), "animated_%d", i);
        rt_string name = rt_string_from_bytes(name_bytes, (size_t)name_length);
        void *node = rt_scene_node3d_new();
        rt_scene_node3d_set_name(node, name);
        rt_scene3d_add(scene, node);
        EXPECT_TRUE(rt_node_animation3d_add_channel(clip,
                                                    name,
                                                    RT_NODE_ANIM_PATH_TRANSLATION,
                                                    RT_NODE_ANIM_INTERP_LINEAR,
                                                    2,
                                                    3,
                                                    times,
                                                    values) >= 0,
                    "many-target fixture channel is accepted");
        rt_string_unref(name);
    }
    void *clips[1] = {clip};
    auto *animator = static_cast<rt_node_animator3d *>(rt_node_animator3d_new_from_clips(clips, 1));
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);

    EXPECT_TRUE(animator->target_cache_complete == 1,
                "bulk target cache records resolved and negative results");
    EXPECT_TRUE(animator->target_cache_rebuild_node_visits == (uint64_t)(node_count + 1),
                "bulk target cache visits each hierarchy node exactly once");
    for (int i = 0; i < node_count; i++)
        EXPECT_TRUE(animator->cached_targets[i] != nullptr,
                    "every channel target is populated by the one-pass rebuild");

    uint64_t visits = animator->target_cache_rebuild_node_visits;
    rt_scene3d_sync_bindings(scene, 0.25);
    EXPECT_TRUE(animator->target_cache_rebuild_node_visits == visits,
                "stable hierarchy reuses the completed target cache");
}

static void test_scene_roundtrip_deep_hierarchy_uses_format_depth_limit() {
    const char *path = "/tmp/zanna_scene_deep_roundtrip_test.vscn";
    constexpr int supported_depth = 98;
    void *scene = rt_scene3d_new();
    void *parent = rt_scene3d_get_root(scene);

    for (int i = 0; i < supported_depth; ++i) {
        void *child = rt_scene_node3d_new();
        rt_scene_node3d_set_position(child, 1.0, 0.0, 0.0);
        EXPECT_TRUE(rt_scene_node3d_try_add_child(parent, child) == 1,
                    "Deep VSCN fixture attaches every supported node level");
        parent = child;
    }
    rt_scene_node3d_set_name(parent, rt_const_cstr("deep-leaf"));

    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save accepts the full VSCN node-depth limit");
    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded != nullptr,
                "SceneGraph.Load accepts every hierarchy emitted at the VSCN depth limit");
    if (loaded) {
        void *current = rt_scene3d_get_root(loaded);
        int loaded_depth = 0;
        while (current && rt_scene_node3d_child_count(current) == 1) {
            current = rt_scene_node3d_get_child(current, 0);
            loaded_depth++;
        }
        EXPECT_TRUE(loaded_depth == supported_depth,
                    "VSCN iterative traversal preserves all deep hierarchy levels");
        EXPECT_TRUE(current != nullptr && rt_scene_node3d_child_count(current) == 0,
                    "VSCN deep hierarchy terminates at the expected leaf");
    }
    std::remove(path);

    void *too_deep_scene = rt_scene3d_new();
    parent = rt_scene3d_get_root(too_deep_scene);
    for (int i = 0; i <= supported_depth; ++i) {
        void *child = rt_scene_node3d_new();
        rt_scene_node3d_add_child(parent, child);
        parent = child;
    }
    EXPECT_TRUE(rt_scene3d_save(too_deep_scene, rt_const_cstr(path)) == 0,
                "SceneGraph.Save rejects a hierarchy deeper than its loader can parse");
    std::remove(path);
}

static void test_scene_save_skips_invalid_material_asset_refs() {
    const char *path = "/tmp/zanna_scene_invalid_material_refs.vscn";
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    rt_material3d *material = (rt_material3d *)rt_material3d_new_color(0.2, 0.3, 0.4);

    rt_scene_node3d_set_name(node, rt_const_cstr("invalid_material_refs"));
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene3d_add(scene, node);

    material->texture = rt_vec3_new(1.0, 0.0, 0.0);
    material->normal_map = rt_vec3_new(2.0, 0.0, 0.0);
    material->specular_map = rt_vec3_new(3.0, 0.0, 0.0);
    material->emissive_map = rt_vec3_new(4.0, 0.0, 0.0);
    material->metallic_roughness_map = rt_vec3_new(5.0, 0.0, 0.0);
    material->ao_map = rt_vec3_new(6.0, 0.0, 0.0);
    material->env_map = rt_vec3_new(7.0, 0.0, 0.0);

    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save treats wrong-class material asset refs as absent");
    void *loaded_scene = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded_scene != nullptr,
                "SceneGraph.Load reads scene saved after asset-ref repair");
    if (loaded_scene) {
        void *loaded_node = rt_scene3d_find(loaded_scene, rt_const_cstr("invalid_material_refs"));
        rt_material3d *loaded_material =
            loaded_node ? (rt_material3d *)rt_scene_node3d_get_material(loaded_node) : nullptr;
        EXPECT_TRUE(loaded_material != nullptr, "SceneGraph.Load preserves the material itself");
        if (loaded_material) {
            EXPECT_TRUE(
                loaded_material->texture == nullptr && loaded_material->normal_map == nullptr &&
                    loaded_material->specular_map == nullptr &&
                    loaded_material->emissive_map == nullptr &&
                    loaded_material->metallic_roughness_map == nullptr &&
                    loaded_material->ao_map == nullptr && loaded_material->env_map == nullptr,
                "SceneGraph.Load does not persist invalid material asset refs");
        }
    }

    std::remove(path);
}

static void test_node_animator_weights_all_morph_primitives_in_subtree() {
    void *scene = rt_scene3d_new();
    void *target = rt_scene_node3d_new();
    void *child_a = rt_scene_node3d_new();
    void *child_b = rt_scene_node3d_new();
    void *mesh_a = rt_mesh3d_new();
    void *mesh_b = rt_mesh3d_new();
    void *morph_a = rt_morphtarget3d_new(3);
    void *morph_b = rt_morphtarget3d_new(3);
    double times[2] = {0.0, 1.0};
    float values[2] = {0.0f, 1.0f};
    void *anim;
    void *animator;
    void *clips[1];

    for (void *mesh : {mesh_a, mesh_b}) {
        rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
        rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
        rt_mesh3d_add_vertex(mesh, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
        rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    }
    rt_morphtarget3d_add_shape(morph_a, rt_const_cstr("a"));
    rt_morphtarget3d_add_shape(morph_b, rt_const_cstr("b"));
    rt_mesh3d_set_morph_targets(mesh_a, morph_a);
    rt_mesh3d_set_morph_targets(mesh_b, morph_b);
    rt_scene_node3d_set_name(target, rt_const_cstr("target"));
    rt_scene_node3d_set_mesh(child_a, mesh_a);
    rt_scene_node3d_set_mesh(child_b, mesh_b);
    rt_scene_node3d_add_child(target, child_a);
    rt_scene_node3d_add_child(target, child_b);
    rt_scene3d_add(scene, target);

    anim = rt_node_animation3d_new(rt_const_cstr("weights"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_WEIGHTS,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                1,
                                                times,
                                                values) >= 0,
                "Node animation accepts subtree morph weight channel");
    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);

    EXPECT_NEAR(rt_morphtarget3d_get_weight(morph_a, 0),
                0.5,
                0.001,
                "Node animation applies weights to the first primitive morph set");
    EXPECT_NEAR(rt_morphtarget3d_get_weight(morph_b, 0),
                0.5,
                0.001,
                "Node animation applies weights to sibling primitive morph sets");
}

static void test_node_animator_samples_cubic_translation_channels() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    double times[2] = {0.0, 1.0};
    float values[6] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    float in_tangents[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float out_tangents[6] = {2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    void *anim;
    void *animator;
    void *clips[1];

    rt_scene_node3d_set_name(node, rt_const_cstr("target"));
    rt_scene3d_add(scene, node);
    anim = rt_node_animation3d_new(rt_const_cstr("cubic"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_cubic_channel(anim,
                                                      rt_const_cstr("target"),
                                                      RT_NODE_ANIM_PATH_TRANSLATION,
                                                      2,
                                                      3,
                                                      times,
                                                      values,
                                                      in_tangents,
                                                      out_tangents) >= 0,
                "Node animation accepts CUBICSPLINE channels with tangent payloads");
    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);

    void *pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos),
                1.25,
                0.001,
                "Node animation evaluates CUBICSPLINE translation with Hermite tangents");
}

static void test_node_animator_slerps_linear_rotation_channels() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    double times[2] = {0.0, 1.0};
    float values[8] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.70710678f, 0.70710678f};
    void *anim;
    void *animator;
    void *clips[1];

    rt_scene_node3d_set_name(node, rt_const_cstr("target"));
    rt_scene3d_add(scene, node);
    anim = rt_node_animation3d_new(rt_const_cstr("rotate"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_ROTATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                4,
                                                times,
                                                values) >= 0,
                "Node animation accepts linear rotation channels");
    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.25);

    void *rot = rt_scene_node3d_get_rotation(node);
    EXPECT_NEAR(
        rt_quat_z(rot), 0.19509, 0.001, "Node animation uses quaternion slerp for rotation Z");
    EXPECT_NEAR(
        rt_quat_w(rot), 0.98079, 0.001, "Node animation uses quaternion slerp for rotation W");
}

static void test_node_animator_samples_cubic_rotation_shortest_hemisphere() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    double times[2] = {0.0, 1.0};
    float values[8] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -0.70710678f, -0.70710678f};
    float tangents[8] = {};
    void *anim;
    void *animator;
    void *clips[1];

    rt_scene_node3d_set_name(node, rt_const_cstr("target"));
    rt_scene3d_add(scene, node);
    anim = rt_node_animation3d_new(rt_const_cstr("cubic_rotate"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_cubic_channel(anim,
                                                      rt_const_cstr("target"),
                                                      RT_NODE_ANIM_PATH_ROTATION,
                                                      2,
                                                      4,
                                                      times,
                                                      values,
                                                      tangents,
                                                      tangents) >= 0,
                "Node animation accepts cubic rotation channels");
    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);

    void *rot = rt_scene_node3d_get_rotation(node);
    EXPECT_NEAR(rt_quat_z(rot),
                0.38268,
                0.001,
                "Cubic node rotation samples the shortest quaternion hemisphere");
    EXPECT_NEAR(
        rt_quat_w(rot), 0.92388, 0.001, "Cubic node rotation avoids antipodal midpoint collapse");
}

static void test_node_animator_repairs_corrupt_clip_duration() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    double times[2] = {0.0, 1.0};
    float values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    void *anim;
    void *animator;
    void *clips[1];

    rt_scene_node3d_set_name(node, rt_const_cstr("target"));
    rt_scene3d_add(scene, node);
    anim = rt_node_animation3d_new(rt_const_cstr("duration_repair"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                values) >= 0,
                "Node animation accepts the duration repair fixture");
    auto *anim_view = static_cast<rt_node_animation3d *>(anim);
    anim_view->duration = 0.0;

    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 1.5);

    void *pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(anim_view->duration, 1.0, 0.001, "Node animator repairs corrupt clip duration");
    EXPECT_NEAR(rt_vec3_x(pos),
                0.5,
                0.001,
                "Node animator wraps with repaired duration instead of clamping at the last key");
}

static void test_node_animation_rejects_invalid_channel_data() {
    void *anim = rt_node_animation3d_new(rt_const_cstr("invalid"), 1.0);
    double unsorted_times[2] = {1.0, 0.0};
    double valid_times[2] = {0.0, 1.0};
    float translation_values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    float bad_values[6] = {0.0f, 0.0f, 0.0f, NAN, 0.0f, 0.0f};
    float tangent_values[6] = {};
    float bad_tangents[6] = {0.0f, INFINITY, 0.0f, 0.0f, 0.0f, 0.0f};

    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                unsorted_times,
                                                translation_values) < 0,
                "Node animation rejects unsorted key times");
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                valid_times,
                                                bad_values) < 0,
                "Node animation rejects non-finite values");
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                99,
                                                2,
                                                3,
                                                valid_times,
                                                translation_values) < 0,
                "Node animation rejects invalid interpolation modes");
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                2,
                                                valid_times,
                                                translation_values) < 0,
                "Node animation rejects translation channels narrower than xyz");
    EXPECT_TRUE(rt_node_animation3d_add_cubic_channel(anim,
                                                      rt_const_cstr("target"),
                                                      RT_NODE_ANIM_PATH_TRANSLATION,
                                                      2,
                                                      3,
                                                      valid_times,
                                                      translation_values,
                                                      bad_tangents,
                                                      tangent_values) < 0,
                "Node animation rejects non-finite cubic tangents");
}

static void test_node_animation_rejects_wrong_string_handles() {
    void *scene = rt_scene3d_new();
    void *target = rt_scene_node3d_new();
    rt_string target_name = rt_const_cstr("target");
    double times[2] = {0.0, 1.0};
    float values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

    rt_scene_node3d_set_name(target, target_name);
    rt_scene3d_add(scene, target);

    void *wrong_name = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_name);
    rt_string fake_name = reinterpret_cast<rt_string>(wrong_name);

    void *bad_named_clip = rt_node_animation3d_new(fake_name, 1.0);
    auto *bad_named_view = static_cast<rt_node_animation3d *>(bad_named_clip);
    EXPECT_TRUE(bad_named_view && bad_named_view->name &&
                    std::strcmp(rt_string_cstr(bad_named_view->name), "") == 0,
                "NodeAnimation3D.New replaces wrong-class clip names with an empty string");
    EXPECT_TRUE(rt_node_animation3d_add_channel(bad_named_clip,
                                                fake_name,
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                values) < 0,
                "NodeAnimation3D.AddChannel rejects wrong-class target name handles");

    void *anim = rt_node_animation3d_new(rt_const_cstr("clip"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                target_name,
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                values) == 0,
                "NodeAnimation3D string-handle fixture creates a valid channel");
    void *clips[1] = {anim};
    void *animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.5);
    void *pos = rt_scene_node3d_get_position(target);
    EXPECT_NEAR(rt_vec3_x(pos), 0.5, 0.001, "NodeAnimation3D fixture resolves a valid target");

    auto *anim_view = static_cast<rt_node_animation3d *>(anim);
    rt_string saved_channel_name = anim_view->channels[0].target_name;
    rt_scene_node3d_set_position(target, 0.0, 0.0, 0.0);
    anim_view->channels[0].target_name = fake_name;
    rt_scene3d_sync_bindings(scene, 0.5);
    pos = rt_scene_node3d_get_position(target);
    EXPECT_NEAR(rt_vec3_x(pos),
                0.0,
                0.001,
                "NodeAnimator skips channels whose private target name becomes wrong-class");
    anim_view->channels[0].target_name = saved_channel_name;

    auto *target_view = static_cast<rt_scene_node3d *>(target);
    rt_string saved_node_name = target_view->name;
    rt_scene_node3d_set_position(target, 0.0, 0.0, 0.0);
    target_view->name = fake_name;
    rt_scene3d_sync_bindings(scene, 0.25);
    pos = rt_scene_node3d_get_position(target);
    EXPECT_NEAR(rt_vec3_x(pos),
                0.0,
                0.001,
                "NodeAnimator skips cached targets whose private node name becomes wrong-class");
    EXPECT_TRUE(target_view->name == nullptr,
                "NodeAnimator target lookup repairs wrong-class cached node names");
    target_view->name = saved_node_name;

    void *finalizer_clip = rt_node_animation3d_new(rt_const_cstr("finalizer"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(finalizer_clip,
                                                target_name,
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                values) == 0,
                "NodeAnimation3D finalizer fixture creates a valid channel");
    auto *finalizer_view = static_cast<rt_node_animation3d *>(finalizer_clip);
    finalizer_view->channels[0].target_name = fake_name;
    if (rt_obj_release_check0(finalizer_clip))
        rt_obj_free(finalizer_clip);
    if (rt_obj_release_check0(bad_named_clip))
        rt_obj_free(bad_named_clip);

    EXPECT_TRUE(rt_obj_release_check0(wrong_name) == 0,
                "NodeAnimation3D string guards do not release wrong-class handles");
    if (rt_obj_release_check0(wrong_name))
        rt_obj_free(wrong_name);
}

static void test_node_animation_step_accepts_duplicate_key_times() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *anim = rt_node_animation3d_new(rt_const_cstr("step_duplicates"), 1.0);
    void *animator;
    void *clips[1];
    double duplicate_times[3] = {0.0, 0.0, 1.0};
    double linear_duplicate_times[2] = {0.0, 0.0};
    float step_values[9] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f};
    float linear_values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

    rt_scene_node3d_set_name(node, rt_const_cstr("target"));
    rt_scene3d_add(scene, node);
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                linear_duplicate_times,
                                                linear_values) < 0,
                "Linear node animation still rejects duplicate key times");
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_STEP,
                                                3,
                                                3,
                                                duplicate_times,
                                                step_values) >= 0,
                "Step node animation accepts duplicate key times");
    ((rt_node_animation3d *)anim)->looping = 0;
    clips[0] = anim;
    animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);
    rt_scene3d_sync_bindings(scene, 0.0);

    void *pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos),
                2.0,
                0.001,
                "Step duplicate timestamps sample the last key at the current time");

    rt_scene3d_sync_bindings(scene, 1.0);
    pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 3.0, 0.001, "Step animation still reaches later keys");
}

static void test_node_animation_rejects_pathological_channel_sizes() {
    void *anim = rt_node_animation3d_new(rt_const_cstr("pathological_sizes"), 1.0);
    double one_time[1] = {0.0};
    float one_value[1] = {0.0f};
    std::vector<double> many_times(2000);
    for (size_t i = 0; i < many_times.size(); i++)
        many_times[i] = (double)i * 0.001;

    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_WEIGHTS,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                1,
                                                4097,
                                                one_time,
                                                one_value) < 0,
                "Node animation rejects morph-weight channels wider than the runtime cap");
    EXPECT_TRUE(rt_node_animation3d_add_channel(anim,
                                                rt_const_cstr("target"),
                                                RT_NODE_ANIM_PATH_WEIGHTS,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                (int64_t)many_times.size(),
                                                3000,
                                                many_times.data(),
                                                one_value) < 0,
                "Node animation rejects channels whose key-width product is too large");
}

static void test_node_animation_rejects_ambiguous_byte_names() {
    const char embedded_target_bytes[] = {'t', 'a', 'r', 'g', 'e', 't', '\0', 'x'};
    const char malformed_utf8_bytes[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    rt_string embedded_target =
        rt_string_from_bytes(embedded_target_bytes, sizeof(embedded_target_bytes));
    rt_string malformed_target =
        rt_string_from_bytes(malformed_utf8_bytes, sizeof(malformed_utf8_bytes));
    double times[2] = {0.0, 1.0};
    float values[6] = {};

    void *bad_name_clip = rt_node_animation3d_new(embedded_target, 1.0);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_node_animation3d_get_name(bad_name_clip)), "") == 0,
                "NodeAnimation3D.New rejects embedded-NUL clip names");

    void *clip = rt_node_animation3d_new(rt_const_cstr("target"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(clip,
                                                embedded_target,
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                values) < 0,
                "NodeAnimation3D.AddChannel rejects embedded-NUL target aliases");
    EXPECT_TRUE(rt_node_animation3d_add_channel(clip,
                                                malformed_target,
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                values) < 0,
                "NodeAnimation3D.AddChannel rejects malformed UTF-8 target names");

    void *animator = rt_node_animator3d_new(clip);
    EXPECT_TRUE(rt_node_animator3d_play(animator, embedded_target) == 0,
                "NodeAnimator3D.Play compares complete stored names without C-string aliasing");
}

static void test_node_animator_repairs_cache_and_sample_corruption() {
    void *scene = rt_scene3d_new();
    auto *root = static_cast<rt_scene_node3d *>(rt_scene3d_get_root(scene));
    void *target = rt_scene_node3d_new();
    rt_scene_node3d_set_name(target, rt_const_cstr("animated"));
    rt_scene3d_add(scene, target);

    double times[3] = {0.0, 0.5, 1.0};
    float values[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    void *clip = rt_node_animation3d_new(rt_const_cstr("cache_repair"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(clip,
                                                rt_const_cstr("animated"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                3,
                                                3,
                                                times,
                                                values) == 0,
                "NodeAnimator corruption fixture creates a translation channel");
    static_cast<rt_node_animation3d *>(clip)->looping = 0;
    auto *animator = static_cast<rt_node_animator3d *>(rt_node_animator3d_new(clip));
    animator->cached_target_capacity = 64;
    animator->cached_targets = nullptr;
    animator->traversal_stack_capacity = 64;
    animator->traversal_stack = nullptr;
    rt_scene_node3d_bind_node_animator(root, animator);

    rt_node_animator3d_update(animator, 0.25);
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(target)),
                0.5,
                0.001,
                "NodeAnimator repairs null cache/stack pointers with stale positive capacities");
    EXPECT_TRUE(animator->cached_targets != nullptr && animator->traversal_stack != nullptr,
                "NodeAnimator reconstructs reusable cache and traversal storage");

    auto *channel = &static_cast<rt_node_animation3d *>(clip)->channels[0];
    channel->times[1] = std::numeric_limits<double>::quiet_NaN();
    rt_scene_node3d_set_position(target, 9.0, 0.0, 0.0);
    rt_node_animator3d_update(animator, 0.1);
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(target)),
                9.0,
                0.001,
                "NodeAnimator skips a channel with a corrupt interior key time");
    channel->times[1] = 0.5;

    channel->values[3] = std::numeric_limits<float>::infinity();
    rt_node_animator3d_set_time(animator, 0.25);
    rt_node_animator3d_update(animator, 0.0);
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(target)),
                9.0,
                0.001,
                "NodeAnimator skips non-finite retained sample lanes");
    channel->values[3] = 1.0f;

    rt_scene_node3d *cached_target = animator->cached_targets[0];
    if (cached_target && rt_obj_release_check0(cached_target))
        rt_obj_free(cached_target);
    animator->cached_targets[0] =
        static_cast<rt_scene_node3d *>(rt_material3d_new_color(0.1, 0.2, 0.3));
    rt_scene_node3d_set_position(target, 0.0, 0.0, 0.0);
    rt_node_animator3d_update(animator, 0.0);
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(target)),
                0.5,
                0.001,
                "NodeAnimator discards a wrong-class cached target and resolves again");

    animator->speed = std::numeric_limits<double>::max();
    animator->time = -100.0;
    EXPECT_NEAR(rt_node_animator3d_get_speed(animator),
                1000000.0,
                0.001,
                "NodeAnimator.Speed caps corrupt retained multipliers");
    EXPECT_NEAR(rt_node_animator3d_get_time(animator),
                0.0,
                0.001,
                "NodeAnimator.Time rejects corrupt negative retained time");
}

static void test_node_animator_stops_on_exact_one_shot_endpoints() {
    void *scene = rt_scene3d_new();
    void *target = rt_scene_node3d_new();
    rt_scene_node3d_set_name(target, rt_const_cstr("one_shot_target"));
    rt_scene3d_add(scene, target);
    double times[2] = {0.0, 1.0};
    float values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    void *clip = rt_node_animation3d_new(rt_const_cstr("one_shot"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(clip,
                                                rt_const_cstr("one_shot_target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                values) == 0,
                "one-shot endpoint fixture creates a translation channel");
    static_cast<rt_node_animation3d *>(clip)->looping = 0;
    void *animator = rt_node_animator3d_new(clip);
    rt_scene_node3d_bind_node_animator(rt_scene3d_get_root(scene), animator);

    rt_node_animator3d_update(animator, 1.0);
    EXPECT_TRUE(rt_node_animator3d_get_playing(animator) == 0,
                "forward one-shot playback stops when it lands exactly on duration");
    EXPECT_NEAR(rt_node_animator3d_get_time(animator), 1.0, 0.001, "forward endpoint is retained");

    EXPECT_TRUE(rt_node_animator3d_play(animator, rt_const_cstr("one_shot")) != 0,
                "one-shot clip restarts for reverse endpoint coverage");
    rt_node_animator3d_set_time(animator, 1.0);
    rt_node_animator3d_set_speed(animator, -1.0);
    rt_node_animator3d_update(animator, 1.0);
    EXPECT_TRUE(rt_node_animator3d_get_playing(animator) == 0,
                "reverse one-shot playback stops when it lands exactly on zero");
    EXPECT_NEAR(rt_node_animator3d_get_time(animator), 0.0, 0.001, "reverse endpoint is retained");
}

static void test_frustum_aabb_inside() {
    /* Object at origin, camera looking at it → visible (not culled) */
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    rt_scene_node3d_set_mesh(node, rt_mesh3d_new_box(1.0, 1.0, 1.0));
    rt_scene_node3d_set_material(node, rt_material3d_new_color(1, 0, 0));
    rt_scene3d_add(scene, node);

    /* Can't call Draw without a canvas (needs window), but we can
     * verify AABB is computed by checking aabb_min/max via getters */
    void *amin = rt_scene_node3d_get_aabb_min(node);
    void *amax = rt_scene_node3d_get_aabb_max(node);
    EXPECT_NEAR(rt_vec3_x(amin), -0.5, 0.01, "Box AABB min X = -0.5");
    EXPECT_NEAR(rt_vec3_y(amin), -0.5, 0.01, "Box AABB min Y = -0.5");
    EXPECT_NEAR(rt_vec3_z(amin), -0.5, 0.01, "Box AABB min Z = -0.5");
    EXPECT_NEAR(rt_vec3_x(amax), 0.5, 0.01, "Box AABB max X = 0.5");
    EXPECT_NEAR(rt_vec3_y(amax), 0.5, 0.01, "Box AABB max Y = 0.5");
    EXPECT_NEAR(rt_vec3_z(amax), 0.5, 0.01, "Box AABB max Z = 0.5");
}

static void test_frustum_sphere_aabb() {
    /* Sphere AABB should be [-radius, +radius] in all axes */
    void *node = rt_scene_node3d_new();
    extern void *rt_mesh3d_new_sphere(double r, int64_t seg);
    rt_scene_node3d_set_mesh(node, rt_mesh3d_new_sphere(2.0, 8));
    void *amin = rt_scene_node3d_get_aabb_min(node);
    void *amax = rt_scene_node3d_get_aabb_max(node);
    EXPECT_NEAR(rt_vec3_x(amin), -2.0, 0.1, "Sphere AABB min X ≈ -2");
    EXPECT_NEAR(rt_vec3_x(amax), 2.0, 0.1, "Sphere AABB max X ≈ 2");
}

static void test_frustum_plane_aabb() {
    /* Plane AABB should match half-extents */
    void *node = rt_scene_node3d_new();
    extern void *rt_mesh3d_new_plane(double sx, double sz);
    rt_scene_node3d_set_mesh(node, rt_mesh3d_new_plane(6.0, 4.0));
    void *amin = rt_scene_node3d_get_aabb_min(node);
    void *amax = rt_scene_node3d_get_aabb_max(node);
    EXPECT_NEAR(rt_vec3_x(amin), -3.0, 0.01, "Plane AABB min X = -3");
    EXPECT_NEAR(rt_vec3_z(amin), -2.0, 0.01, "Plane AABB min Z = -2");
    EXPECT_NEAR(rt_vec3_x(amax), 3.0, 0.01, "Plane AABB max X = 3");
    EXPECT_NEAR(rt_vec3_z(amax), 2.0, 0.01, "Plane AABB max Z = 2");
    /* Plane is at Y=0, so Y min=max=0 */
    EXPECT_NEAR(rt_vec3_y(amin), 0.0, 0.01, "Plane AABB min Y = 0");
    EXPECT_NEAR(rt_vec3_y(amax), 0.0, 0.01, "Plane AABB max Y = 0");
}

static void test_frustum_no_mesh_no_aabb() {
    /* Node without mesh has zero AABB */
    void *node = rt_scene_node3d_new();
    void *amin = rt_scene_node3d_get_aabb_min(node);
    EXPECT_NEAR(rt_vec3_x(amin), 0.0, 0.01, "No-mesh AABB min X = 0");
}

static void test_node_aabb_refreshes_after_mesh_mutation() {
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    rt_scene_node3d_set_mesh(node, mesh);

    void *amax = rt_scene_node3d_get_aabb_max(node);
    EXPECT_NEAR(rt_vec3_x(amax), 1.0, 0.01, "Initial mesh AABB max X = 1");

    rt_mesh3d_add_vertex(mesh, 2.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    amax = rt_scene_node3d_get_aabb_max(node);
    EXPECT_NEAR(rt_vec3_x(amax), 2.0, 0.01, "Scene node AABB refreshes after mesh mutation");
}

static void test_frustum_culled_count_initial() {
    void *scene = rt_scene3d_new();
    EXPECT_TRUE(rt_scene3d_get_culled_count(scene) == 0, "Initial culled count = 0");
}

static void test_lod_culling_uses_stable_union_mesh_bounds() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *base_mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *lod_mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(lod_mesh, 5.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(lod_mesh, 6.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(lod_mesh, 5.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(lod_mesh, 0, 1, 2);

    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    rt_scene_node3d_set_mesh(node, base_mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_add_lod(node, 0.0, lod_mesh);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_scene_submit_count == 1,
                "SceneGraph keeps LOD draws visible when the node's stable union bounds intersect");
    EXPECT_TRUE(rt_scene3d_get_culled_count(scene) == 0,
                "SceneGraph does not cull only because the selected LOD mesh bounds are outside "
                "the frustum");
    EXPECT_TRUE(rt_canvas3d_get_occluded_draw_count(&canvas) == 0,
                "Canvas3D.OccludedDrawCount mirrors the latest scene visibility skip count");
}

static void test_auto_lod_uses_screen_error_selection() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    canvas.width = 100;
    canvas.height = 100;
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *base_mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
    void *lod_mesh = rt_mesh3d_new();
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 25.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    rt_mesh3d_add_vertex(lod_mesh, -0.25, -0.25, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(lod_mesh, 0.25, -0.25, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(lod_mesh, 0.0, 0.25, 0.0, 0.0, 0.0, 1.0, 0.5, 1.0);
    rt_mesh3d_add_triangle(lod_mesh, 0, 1, 2);

    rt_scene_node3d_set_mesh(node, base_mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_add_lod(node, 1000.0, lod_mesh);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene_node3d_set_auto_lod(node, 1, 16.0);
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 1, "SceneGraph draws the auto-LOD fixture");
    EXPECT_TRUE(g_scene_last_vertex_count == 3,
                "SceneNode.SetAutoLOD can select a projected small LOD before distance LOD");

    reset_scene_capture();
    rt_scene_node3d_set_auto_lod(node, 0, 16.0);
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 1, "SceneGraph redraws after disabling auto LOD");
    EXPECT_TRUE(g_scene_last_vertex_count != 3,
                "SceneNode.SetAutoLod(false) restores authored distance thresholds");
}

static void test_auto_lod_radius_uses_world_scale() {
    /* Same fixture as the screen-error selection test, but the node is
     * scaled 4x: the WORLD radius (the contract of scene3d_auto_lod_mesh)
     * quadruples, the projected size re-crosses the screen-error
     * threshold, and LOD0 must draw. Before the fix the cull path passed
     * the LOCAL-space radius, so scaled models under-reported projected
     * size by their node scale and auto-LOD stuck on coarse levels (the
     * 3.28x-scaled stadium shell). */
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    canvas.width = 100;
    canvas.height = 100;
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *base_mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
    void *lod_mesh = rt_mesh3d_new();
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 25.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    rt_mesh3d_add_vertex(lod_mesh, -0.25, -0.25, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(lod_mesh, 0.25, -0.25, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(lod_mesh, 0.0, 0.25, 0.0, 0.0, 0.0, 1.0, 0.5, 1.0);
    rt_mesh3d_add_triangle(lod_mesh, 0, 1, 2);

    rt_scene_node3d_set_mesh(node, base_mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_add_lod(node, 1000.0, lod_mesh);
    rt_scene_node3d_set_scale(node, 4.0, 4.0, 4.0);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene_node3d_set_auto_lod(node, 1, 16.0);
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 1, "SceneGraph draws the scaled auto-LOD fixture");
    EXPECT_TRUE(g_scene_last_vertex_count != 3,
                "auto-LOD projects the WORLD-space radius: a 4x-scaled node "
                "re-crosses the screen-error threshold and keeps LOD0");
}

static void test_lod_residency_falls_back_and_reports_bytes() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    canvas.width = 100;
    canvas.height = 100;
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *base_mesh = rt_mesh3d_new_box(2.0, 2.0, 2.0);
    void *lod_mesh = rt_mesh3d_new();
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 8.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    rt_mesh3d_add_vertex(lod_mesh, -0.25, -0.25, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(lod_mesh, 0.25, -0.25, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(lod_mesh, 0.0, 0.25, 0.0, 0.0, 0.0, 1.0, 0.5, 1.0);
    rt_mesh3d_add_triangle(lod_mesh, 0, 1, 2);

    rt_scene_node3d_set_mesh(node, base_mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_add_lod(node, 0.0, lod_mesh);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    EXPECT_TRUE(rt_mesh3d_get_resident_bytes(base_mesh) > 0,
                "Mesh3D.ResidentBytes reports base mesh payload bytes");
    EXPECT_TRUE(rt_scene_node3d_get_lod_resident(node, 0) == 1, "new LOD meshes start resident");
    EXPECT_TRUE(rt_scene_node3d_get_lod_resident_bytes(node, 0) > 0,
                "SceneNode exposes LOD resident bytes");

    rt_scene_node3d_set_lod_resident(node, 0, 0);
    EXPECT_TRUE(rt_scene_node3d_get_lod_resident(node, 0) == 0,
                "SceneNode.SetLodResident marks LOD payload nonresident");
    EXPECT_TRUE(rt_scene_node3d_get_lod_resident_bytes(node, 0) == 0,
                "nonresident LOD reports zero resident bytes");
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 1,
                "SceneGraph falls back to the base mesh when selected LOD is nonresident");
    EXPECT_TRUE(g_scene_last_vertex_count != 3,
                "SceneGraph did not submit the nonresident LOD mesh");

    reset_scene_capture();
    rt_scene_node3d_set_lod_resident(node, 0, 1);
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 1,
                "SceneGraph redraws after the LOD payload becomes resident");
    EXPECT_TRUE(g_scene_last_vertex_count == 3, "SceneGraph selects the resident LOD mesh");

    reset_scene_capture();
    rt_scene_node3d_set_lod_resident(node, 0, 0);
    rt_mesh3d_set_resident(base_mesh, 0);
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(
        g_scene_submit_count == 0,
        "SceneGraph skips drawing when both base and selected LOD payloads are nonresident");
}

static void test_impostor_proxy_draws_textured_quad() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    canvas.width = 128;
    canvas.height = 128;
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *pixels = rt_pixels_new(4, 2);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    rt_pixels_set(pixels, 0, 0, 0xFF0000FFll);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_set_impostor(node, 0.0, pixels);
    auto *node_view = static_cast<rt_scene_node3d *>(node);
    EXPECT_TRUE(rt_g3d_has_class(node_view->impostor_mesh, RT_G3D_MESH3D_CLASS_ID),
                "SceneNode.SetImpostor creates a Mesh3D proxy");
    EXPECT_TRUE(rt_g3d_has_class(node_view->impostor_material, RT_G3D_MATERIAL3D_CLASS_ID),
                "SceneNode.SetImpostor creates a Material3D proxy");
    EXPECT_TRUE(rt_mesh3d_get_resident(node_view->impostor_mesh) == 1,
                "SceneNode.SetImpostor creates a resident proxy mesh");
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_scene_submit_count == 1, "SceneGraph draws the impostor fixture");
    EXPECT_TRUE(g_scene_last_vertex_count == 4,
                "SceneNode.SetImpostor swaps to a generated quad mesh");
    EXPECT_TRUE(g_scene_last_texture == pixels,
                "SceneNode.SetImpostor binds the supplied Pixels texture");
    EXPECT_TRUE(g_scene_last_unlit == 1, "SceneNode.SetImpostor uses an unlit proxy material");
}

static void test_scene_draw_rejects_corrupt_draw_handles() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    canvas.width = 128;
    canvas.height = 128;

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *pixels = rt_pixels_new(4, 2);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    auto *node_view = static_cast<rt_scene_node3d *>(node);

    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    reset_scene_capture();
    node_view->mesh = material;
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 0,
                "SceneGraph.Draw skips wrong-class node mesh slots without submitting");
    node_view->mesh = mesh;

    reset_scene_capture();
    node_view->material = mesh;
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 0,
                "SceneGraph.Draw skips wrong-class node material slots without submitting");
    node_view->material = material;

    rt_scene_node3d_set_impostor(node, 0.0, pixels);

    reset_scene_capture();
    node_view->impostor_mesh = material;
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 1,
                "SceneGraph.Draw falls back to base mesh when impostor mesh is wrong-class");
    EXPECT_TRUE(
        g_scene_last_vertex_count != 4,
        "SceneGraph.Draw does not submit the generated impostor quad after mesh corruption");
    node_view->impostor_mesh = nullptr;
    rt_scene_node3d_set_impostor(node, 0.0, pixels);

    reset_scene_capture();
    node_view->impostor_material = mesh;
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 1,
                "SceneGraph.Draw falls back to base mesh when impostor material is wrong-class");
    EXPECT_TRUE(
        g_scene_last_vertex_count != 4,
        "SceneGraph.Draw does not submit the generated impostor quad after material corruption");
}

static void test_dynamic_deformation_uses_conservative_frustum_culling() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    ((rt_mesh3d *)mesh)->morph_shape_count = 1;
    rt_scene_node3d_set_position(node, 1000.0, 0.0, 0.0);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_scene_submit_count == 0,
                "SceneGraph culls dynamic meshes using conservative deformation bounds");
    EXPECT_TRUE(rt_scene3d_get_culled_count(scene) == 1,
                "SceneGraph records frustum culls for dynamic-deformation meshes");
}

static void test_morph_delta_bounds_keep_deformed_mesh_visible() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *morph = rt_morphtarget3d_new(((rt_mesh3d *)mesh)->vertex_count);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);

    int64_t shape = rt_morphtarget3d_add_shape(morph, rt_const_cstr("reach"));
    rt_morphtarget3d_set_delta(morph, shape, 0, -4.0, 0.0, 0.0);
    rt_mesh3d_set_morph_targets(mesh, morph);
    rt_scene_node3d_set_position(node, 4.0, 0.0, 0.0);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_scene_submit_count == 1,
                "SceneGraph keeps morph-deformed meshes visible using authored delta bounds");
}

static void test_raw_morph_delta_bounds_handle_large_finite_values() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    std::vector<float> deltas((size_t)mesh_view->vertex_count * 3u, 0.0f);
    deltas[0] = std::numeric_limits<float>::max();
    mesh_view->morph_deltas = deltas.data();
    mesh_view->morph_shape_count = 1;

    double pad = scene3d_mesh_dynamic_bound_pad(mesh_view, NULL, 1.0);
    EXPECT_TRUE(std::isfinite(pad) && std::fabs(pad - SCENE3D_ABS_MAX) < 1.0,
                "SceneGraph raw morph bounds clamp huge finite deltas instead of discarding them");

    uint32_t saved_vertex_count = mesh_view->vertex_count;
    uint32_t saved_vertex_capacity = mesh_view->vertex_capacity;
    mesh_view->morph_bound_valid = 0;
    mesh_view->morph_shape_count = 32;
    mesh_view->vertex_count = 32769;
    mesh_view->vertex_capacity = 32769;
    pad = scene3d_mesh_dynamic_bound_pad(mesh_view, NULL, 1.0);
    EXPECT_TRUE(std::isfinite(pad) && std::fabs(pad - SCENE3D_ABS_MAX) < 1.0,
                "SceneGraph uses a conservative bound when a supported raw morph span exceeds "
                "the scan budget");

    mesh_view->vertex_count = saved_vertex_count;
    mesh_view->vertex_capacity = saved_vertex_capacity;

    mesh_view->morph_deltas = NULL;
    mesh_view->morph_shape_count = 0;
    mesh_view->morph_bound_valid = 0;
}

static void test_dynamic_deformation_contains_nonfinite_palette_translation() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *skeleton = rt_skeleton3d_new();
    void *bind = rt_mat4_identity();
    rt_skeleton3d_add_bone(skeleton, rt_const_cstr("root"), -1, bind);
    rt_skeleton3d_compute_inverse_bind(skeleton);
    void *controller = rt_anim_controller3d_new(skeleton);
    int32_t bone_count = 0;
    const float *palette = rt_anim_controller3d_get_final_palette_data(controller, &bone_count);
    EXPECT_TRUE(palette != nullptr && bone_count == 1,
                "non-finite palette fixture exposes one skinning matrix");

    float *mutable_palette = const_cast<float *>(palette);
    mutable_palette[3] = std::numeric_limits<float>::quiet_NaN();
    double pad = scene3d_mesh_dynamic_bound_pad((rt_mesh3d *)mesh, controller, 1.0);
    EXPECT_TRUE(std::isfinite(pad) && std::fabs(pad - SCENE3D_ABS_MAX) < 1.0,
                "SceneGraph contains non-finite skinning translations with a conservative bound");
}

static void test_dynamic_deformation_rejects_corrupt_morph_delta_span() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    static const float tiny_delta[3] = {1.0f, 0.0f, 0.0f};

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    mesh_view->morph_deltas = tiny_delta;
    mesh_view->morph_shape_count = INT32_MAX;
    rt_scene_node3d_set_position(node, 1000.0, 0.0, 0.0);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene3d_add(scene, node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_scene_submit_count == 0,
                "SceneGraph does not draw a corrupt raw morph-delta span outside the frustum");
    EXPECT_TRUE(rt_scene3d_get_culled_count(scene) == 1,
                "SceneGraph culls corrupt morph spans without scanning bogus shape counts");
}

static void test_parent_animator_drives_child_skinned_meshes() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new();
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *bind = rt_mat4_identity();
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, bind);
    rt_skeleton3d_compute_inverse_bind(skel);
    void *controller = rt_anim_controller3d_new(skel);

    rt_mesh3d_add_vertex(mesh, -0.5, -0.5, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.5, -0.5, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.5, 0.0, 0.0, 0.0, 1.0, 0.5, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    rt_mesh3d_set_bone_weights(mesh, 0, 0, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    rt_mesh3d_set_bone_weights(mesh, 1, 0, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    rt_mesh3d_set_bone_weights(mesh, 2, 0, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    ((rt_mesh3d *)mesh)->bone_count = 1;

    rt_scene_node3d_bind_animator(parent, controller);
    rt_scene_node3d_set_mesh(child, mesh);
    rt_scene_node3d_set_material(child, material);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_scene_submit_count == 1, "SceneGraph draws the child skinned mesh");
    EXPECT_TRUE(g_scene_last_bone_count == 1,
                "SceneGraph inherits a bound parent animator when drawing child meshes");

    static_cast<rt_scene_node3d *>(parent)->bound_animator = material;
    reset_scene_capture();
    rt_scene3d_draw(scene, &canvas, camera);
    EXPECT_TRUE(g_scene_submit_count == 1,
                "SceneGraph still draws child meshes when parent animator binding is wrong-class");
    EXPECT_TRUE(g_scene_last_bone_count == 0,
                "SceneGraph does not inherit wrong-class parent animator bindings");
}

static void test_scene_draw_includes_node_attached_lights() {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.begin_frame = scene_test_begin_frame;
    backend.end_frame = scene_test_end_frame;
    backend.submit_draw = scene_test_submit_draw;

    rt_canvas3d canvas;
    init_scene_test_canvas(&canvas, &backend);
    reset_scene_capture();

    void *scene = rt_scene3d_new();
    void *mesh_node = rt_scene_node3d_new();
    void *light_node = rt_scene_node3d_new();
    void *area_node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *local_pos = rt_vec3_new(0.0, 0.0, 0.0);
    void *light = rt_light3d_new_point(local_pos, 0.25, 0.5, 1.0, 0.2);
    void *area_light = rt_light3d_new_area_rectangle(
        local_pos, rt_vec3_new(0.0, 0.0, -1.0), 4.0, 2.0, 1.0, 0.5, 0.25, 0.1, 12.0);

    rt_scene_node3d_set_mesh(mesh_node, mesh);
    rt_scene_node3d_set_material(mesh_node, material);
    rt_scene_node3d_set_position(light_node, 2.0, 3.0, 4.0);
    rt_scene_node3d_set_light(light_node, light);
    rt_scene_node3d_set_position(area_node, -2.0, 1.0, -3.0);
    rt_scene_node3d_set_scale(area_node, 2.0, 3.0, 4.0);
    rt_scene_node3d_set_light(area_node, area_light);
    rt_scene3d_add(scene, mesh_node);
    rt_scene3d_add(scene, light_node);
    rt_scene3d_add(scene, area_node);
    rt_camera3d_look_at(camera, eye, target, up);

    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_scene_submit_count == 1, "SceneGraph draws lit scene geometry");
    EXPECT_TRUE(g_scene_last_light_count == 2, "SceneGraph forwards node-attached lights");
    if (g_scene_last_light_count > 0) {
        EXPECT_TRUE(g_scene_last_lights[0].type == 1,
                    "SceneGraph preserves node-attached point light type");
        EXPECT_NEAR(g_scene_last_lights[0].position[0],
                    2.0,
                    0.001,
                    "SceneGraph transforms node-attached light X position");
        EXPECT_NEAR(g_scene_last_lights[0].position[1],
                    3.0,
                    0.001,
                    "SceneGraph transforms node-attached light Y position");
        EXPECT_NEAR(g_scene_last_lights[0].position[2],
                    4.0,
                    0.001,
                    "SceneGraph transforms node-attached light Z position");
    }
    if (g_scene_last_light_count > 1) {
        EXPECT_TRUE(g_scene_last_lights[1].type == 4,
                    "SceneGraph preserves native rectangle light type");
        EXPECT_NEAR(g_scene_last_lights[1].position[0],
                    -2.0,
                    0.001,
                    "SceneGraph transforms rectangle X position");
        EXPECT_NEAR(g_scene_last_lights[1].position[1],
                    1.0,
                    0.001,
                    "SceneGraph transforms rectangle Y position");
        EXPECT_NEAR(g_scene_last_lights[1].position[2],
                    -3.0,
                    0.001,
                    "SceneGraph transforms rectangle Z position");
        EXPECT_NEAR(g_scene_last_lights[1].width,
                    8.0,
                    0.001,
                    "SceneGraph scales rectangle width on its U axis");
        EXPECT_NEAR(g_scene_last_lights[1].height,
                    6.0,
                    0.001,
                    "SceneGraph scales rectangle height on its V axis");
        EXPECT_NEAR(g_scene_last_lights[1].range,
                    48.0,
                    0.001,
                    "SceneGraph scales rectangle range conservatively");
        EXPECT_NEAR(g_scene_last_lights[1].basis_u[0] * g_scene_last_lights[1].basis_v[0] +
                        g_scene_last_lights[1].basis_u[1] * g_scene_last_lights[1].basis_v[1] +
                        g_scene_last_lights[1].basis_u[2] * g_scene_last_lights[1].basis_v[2],
                    0.0,
                    0.001,
                    "SceneGraph keeps the transformed rectangle basis orthogonal");
    }
}

static void test_scene_node_attached_camera_tracks_world_transform() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *node = rt_scene_node3d_new();
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *wrong_class = rt_material3d_new_color(1.0, 0.0, 0.0);

    rt_scene_node3d_set_position(parent, 10.0, 2.0, 3.0);
    rt_scene_node3d_set_position(node, 1.0, 2.0, 3.0);
    rt_scene_node3d_set_camera(node, camera);
    EXPECT_TRUE(rt_scene_node3d_get_camera(node) == camera,
                "SceneNode retains an attached Camera3D");
    rt_scene_node3d_set_camera(node, wrong_class);
    EXPECT_TRUE(rt_scene_node3d_get_camera(node) == camera,
                "SceneNode rejects wrong-class camera replacements");
    rt_scene_node3d_add_child(parent, node);
    rt_scene3d_add(scene, parent);

    rt_scene3d_sync_bindings(scene, 0.0);
    void *position = rt_camera3d_get_position(camera);
    void *forward = rt_camera3d_get_forward(camera);
    EXPECT_NEAR(rt_vec3_x(position), 11.0, 0.001, "Attached camera follows world X");
    EXPECT_NEAR(rt_vec3_y(position), 4.0, 0.001, "Attached camera follows world Y");
    EXPECT_NEAR(rt_vec3_z(position), 6.0, 0.001, "Attached camera follows world Z");
    EXPECT_NEAR(rt_vec3_x(forward), 0.0, 0.001, "Identity node keeps camera forward X");
    EXPECT_NEAR(rt_vec3_y(forward), 0.0, 0.001, "Identity node keeps camera forward Y");
    EXPECT_NEAR(rt_vec3_z(forward), -1.0, 0.001, "Identity node keeps camera forward -Z");

    const double half_sqrt2 = std::sqrt(0.5);
    rt_scene_node3d_set_rotation(node, rt_quat_new(0.0, half_sqrt2, 0.0, half_sqrt2));
    rt_scene3d_sync_bindings(scene, 0.0);
    forward = rt_camera3d_get_forward(camera);
    EXPECT_NEAR(rt_vec3_x(forward), -1.0, 0.001, "Attached camera follows world rotation");
    EXPECT_NEAR(rt_vec3_y(forward), 0.0, 0.001, "Attached camera rotation stays level");
    EXPECT_NEAR(rt_vec3_z(forward), 0.0, 0.001, "Attached camera rotates local -Z");

    rt_scene_node3d_set_camera(node, nullptr);
    EXPECT_TRUE(rt_scene_node3d_get_camera(node) == nullptr,
                "SceneNode camera binding can be cleared");
}

static void test_scene_node_light_property_retains_rejects_and_clears() {
    void *node = rt_scene_node3d_new();
    void *position = rt_vec3_new(1.0, 2.0, 3.0);
    void *light = rt_light3d_new_point(position, 1.0, 0.5, 0.25, 0.1);
    void *wrong_class = rt_material3d_new_color(1.0, 0.0, 0.0);

    rt_scene_node3d_set_light(node, light);
    EXPECT_TRUE(rt_scene_node3d_get_light(node) == light, "SceneNode exposes an attached Light3D");
    rt_scene_node3d_set_light(node, wrong_class);
    EXPECT_TRUE(rt_scene_node3d_get_light(node) == light,
                "SceneNode rejects wrong-class light replacements without mutation");
    rt_scene_node3d_set_light(node, nullptr);
    EXPECT_TRUE(rt_scene_node3d_get_light(node) == nullptr,
                "SceneNode light property accepts null to clear");
}

static void test_scene_roundtrip_preserves_node_lights() {
    const char *path = "/tmp/zanna_scene_node_light_roundtrip.vscn";
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *pos = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(0.0, 0.0, -1.0);
    void *light = rt_light3d_new_spot(pos, dir, 0.3, 0.6, 0.9, 0.25, 10.0, 30.0);

    rt_light3d_set_intensity(light, 4.0);
    rt_light3d_set_casts_shadows(light, 0);
    rt_scene_node3d_set_name(node, rt_const_cstr("lamp"));
    rt_scene_node3d_set_position(node, 1.0, 2.0, 3.0);
    rt_scene_node3d_set_light(node, light);
    rt_scene3d_add(scene, node);

    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save writes node-light fixtures");
    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded != nullptr, "SceneGraph.Load reads node-light fixtures");
    if (!loaded)
        return;
    void *loaded_node = rt_scene3d_find(loaded, rt_const_cstr("lamp"));
    rt_light3d *loaded_light = (rt_light3d *)rt_scene_node3d_get_light(loaded_node);
    EXPECT_TRUE(loaded_light != nullptr, "SceneGraph.Load restores node-attached lights");
    if (!loaded_light)
        return;
    EXPECT_TRUE(loaded_light->type == 3, "SceneGraph.Load restores spot-light type");
    EXPECT_NEAR(loaded_light->color[2], 0.9, 0.001, "SceneGraph.Load restores light color");
    EXPECT_NEAR(loaded_light->intensity, 4.0, 0.001, "SceneGraph.Load restores light intensity");
    EXPECT_NEAR(
        loaded_light->attenuation, 0.25, 0.001, "SceneGraph.Load restores light attenuation");
    EXPECT_TRUE(loaded_light->casts_shadows == 0,
                "SceneGraph.Load restores light shadow-caster flag");
    EXPECT_TRUE(loaded_light->inner_cos > loaded_light->outer_cos,
                "SceneGraph.Load restores spot cone cosines");
}

/// @brief Authoring metadata (bake/static flags, physics sync mode, auto-LOD
///        policy, visibility) must survive a full save/load cycle — the scene
///        editor depends on `.vscn` being a lossless authoring format.
static void test_scene_roundtrip_preserves_authoring_metadata() {
    const char *path = "/tmp/zanna_scene_authoring_roundtrip.vscn";
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();

    rt_scene_node3d_set_name(node, rt_const_cstr("bake_probe"));
    rt_scene_node3d_set_position(node, 4.0, 5.0, 6.0);
    rt_scene_node3d_set_visible(node, 0);
    rt_scene_node3d_set_static(node, 1);
    rt_scene_node3d_set_sync_mode(node, RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC);
    rt_scene_node3d_set_auto_lod(node, 1, 12.5);
    rt_scene3d_add(scene, node);

    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save writes authoring-metadata fixtures");
    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded != nullptr, "SceneGraph.Load reads authoring-metadata fixtures");
    if (!loaded)
        return;
    void *loaded_node = rt_scene3d_find(loaded, rt_const_cstr("bake_probe"));
    EXPECT_TRUE(loaded_node != nullptr, "SceneGraph.Load restores named authoring nodes");
    if (!loaded_node)
        return;
    EXPECT_TRUE(rt_scene_node3d_get_visible(loaded_node) == 0,
                "SceneGraph.Load restores node visibility");
    EXPECT_TRUE(rt_scene_node3d_get_static(loaded_node) == 1,
                "SceneGraph.Load restores the static-bake flag");
    EXPECT_TRUE(rt_scene_node3d_get_sync_mode(loaded_node) ==
                    RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC,
                "SceneGraph.Load restores the physics sync mode");
    const rt_scene_node3d *view = (const rt_scene_node3d *)loaded_node;
    EXPECT_TRUE(view->auto_lod_enabled == 1, "SceneGraph.Load restores the auto-LOD flag");
    EXPECT_NEAR(view->auto_lod_screen_error_px,
                12.5,
                0.0001,
                "SceneGraph.Load restores the auto-LOD screen error");
}

/// @brief Gameplay-facing node metadata must preserve scalar kinds and exact
///        values through the same canonical VSCN path Studio uses for history.
static void test_scene_node_metadata_is_typed_bounded_and_persistent() {
    const char *path = "/tmp/zanna_scene_node_metadata_roundtrip.vscn";
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    rt_string empty = rt_const_cstr("");
    rt_string role = rt_const_cstr("role");
    rt_string health = rt_const_cstr("health");
    rt_string minimum_health = rt_const_cstr("minimumHealth");
    rt_string radius = rt_const_cstr("radius");
    rt_string active = rt_const_cstr("active");
    rt_string optional_target = rt_const_cstr("optionalTarget");

    rt_scene_node3d_set_name(node, rt_const_cstr("gameplay_probe"));
    EXPECT_TRUE(rt_scene_node3d_metadata_set_string(node, role, rt_const_cstr("boss\"\nphase")) !=
                    0,
                "SceneNode.MetadataSetString accepts bounded gameplay text");
    EXPECT_TRUE(rt_scene_node3d_metadata_set_int(node, health, INT64_MAX) != 0,
                "SceneNode.MetadataSetInt accepts the complete i64 range");
    EXPECT_TRUE(rt_scene_node3d_metadata_set_int(node, minimum_health, INT64_MIN) != 0,
                "SceneNode.MetadataSetInt accepts the i64 lower bound");
    EXPECT_TRUE(rt_scene_node3d_metadata_set_float(node, radius, 4.0) != 0,
                "SceneNode.MetadataSetFloat preserves integral-looking floats");
    EXPECT_TRUE(rt_scene_node3d_metadata_set_bool(node, active, 1) != 0,
                "SceneNode.MetadataSetBool accepts booleans");
    EXPECT_TRUE(rt_scene_node3d_metadata_set_null(node, optional_target) != 0,
                "SceneNode.MetadataSetNull records explicit null");
    EXPECT_TRUE(rt_scene_node3d_metadata_set_int(node, empty, 1) == 0,
                "SceneNode metadata rejects empty keys");

    std::string oversized_key(129, 'k');
    EXPECT_TRUE(rt_scene_node3d_metadata_set_int(
                    node, rt_string_from_bytes(oversized_key.data(), oversized_key.size()), 1) == 0,
                "SceneNode metadata rejects keys beyond the documented limit");
    EXPECT_TRUE(rt_scene_node3d_metadata_set_float(node, rt_const_cstr("bad"), INFINITY) == 0,
                "SceneNode metadata rejects non-finite floats");
    std::string oversized_value(65537, 'v');
    EXPECT_TRUE(rt_scene_node3d_metadata_set_string(
                    node,
                    rt_const_cstr("tooLarge"),
                    rt_string_from_bytes(oversized_value.data(), oversized_value.size())) == 0,
                "SceneNode metadata rejects strings beyond the documented limit");
    const char invalid_utf8_bytes[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    rt_string invalid_utf8 = rt_string_from_bytes(invalid_utf8_bytes, sizeof(invalid_utf8_bytes));
    EXPECT_TRUE(rt_scene_node3d_metadata_set_int(node, invalid_utf8, 1) == 0 &&
                    rt_scene_node3d_metadata_set_string(
                        node, rt_const_cstr("invalidUtf8"), invalid_utf8) == 0,
                "SceneNode metadata rejects malformed UTF-8 keys and string values");
    rt_string_unref(invalid_utf8);

    EXPECT_TRUE(rt_scene_node3d_metadata_has(node, role) != 0,
                "SceneNode.MetadataHas distinguishes present values");
    EXPECT_TRUE(rt_scene_node3d_metadata_has(node, rt_const_cstr("missing")) == 0,
                "SceneNode.MetadataHas distinguishes missing values");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_kind(node, role)), "string") == 0 &&
            std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_kind(node, health)), "int") == 0 &&
            std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_kind(node, radius)), "float") ==
                0 &&
            std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_kind(node, active)), "bool") == 0 &&
            std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_kind(node, optional_target)),
                        "null") == 0,
        "SceneNode.MetadataKind reports every stable scalar token");
    EXPECT_TRUE(rt_scene_node3d_metadata_get_int(node, health, 0) == INT64_MAX,
                "SceneNode.MetadataGetInt returns exact i64 values");
    EXPECT_NEAR(rt_scene_node3d_metadata_get_float(node, radius, 0.0),
                4.0,
                0.0001,
                "SceneNode.MetadataGetFloat returns exact float values");
    EXPECT_TRUE(rt_scene_node3d_metadata_get_bool(node, active, 0) != 0,
                "SceneNode.MetadataGetBool returns exact boolean values");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_get_string(node, role, empty)),
                            "boss\"\nphase") == 0,
                "SceneNode.MetadataGetString returns exact string values");
    EXPECT_TRUE(rt_scene_node3d_metadata_get_int(node, role, 17) == 17,
                "SceneNode typed getters return defaults on kind mismatch");

    void *keys = rt_scene_node3d_metadata_keys(node);
    EXPECT_TRUE(rt_seq_len(keys) == 6, "SceneNode.MetadataKeys returns every accepted key");
    const char *expected_keys[6] = {
        "active", "health", "minimumHealth", "optionalTarget", "radius", "role"};
    for (int64_t index = 0; index < 6; ++index) {
        rt_string key = rt_seq_get_str(keys, index);
        EXPECT_TRUE(std::strcmp(rt_string_cstr(key), expected_keys[index]) == 0,
                    "SceneNode.MetadataKeys uses deterministic lexicographic order");
        rt_string_unref(key);
    }

    auto *node_view = static_cast<rt_scene_node3d *>(node);
    const int32_t saved_metadata_count = node_view->metadata_count;
    node_view->metadata_count = RT_SCENE_NODE3D_MAX_METADATA_ENTRIES + 1;
    void *corrupt_keys = rt_scene_node3d_metadata_keys(node);
    EXPECT_TRUE(rt_seq_len(corrupt_keys) == 0 && rt_scene_node3d_metadata_has(node, health) == 0 &&
                    rt_scene_node3d_metadata_set_int(node, health, 1) == 0,
                "SceneNode metadata APIs reject corrupt native table bounds safely");
    node_view->metadata_count = saved_metadata_count;

    const int32_t saved_key_length = node_view->metadata[0].key_length;
    node_view->metadata[0].key_length = -1;
    void *invalid_entry_keys = rt_scene_node3d_metadata_keys(node);
    EXPECT_TRUE(rt_seq_len(invalid_entry_keys) == 0 &&
                    rt_scene_node3d_metadata_has(node, active) == 0 &&
                    rt_scene_node3d_metadata_set_int(node, health, 1) == 0,
                "SceneNode metadata rejects malformed retained entry bounds as one table");
    node_view->metadata[0].key_length = saved_key_length;

    const int8_t saved_bool = node_view->metadata[0].value.bool_value;
    node_view->metadata[0].value.bool_value = 2;
    EXPECT_TRUE(rt_scene_node3d_metadata_get_bool(node, active, 0) == 0 &&
                    rt_scene_node3d_metadata_has(node, health) == 0,
                "SceneNode metadata getters fail closed on a non-canonical retained payload");
    node_view->metadata[0].value.bool_value = saved_bool;

    const double saved_float = node_view->metadata[4].value.float_value;
    node_view->metadata[4].value.float_value = INFINITY;
    EXPECT_TRUE(rt_scene_node3d_metadata_get_float(node, radius, 19.0) == 19.0,
                "SceneNode metadata float reads return the default for corrupt retained values");
    node_view->metadata[4].value.float_value = saved_float;

    rt_scene3d_add(scene, node);
    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save writes typed node metadata");
    std::string text;
    EXPECT_TRUE(read_text_file(path, text), "Typed node metadata VSCN can be reopened");
    EXPECT_TRUE(text.find("\"version\": 6") != std::string::npos,
                "Node metadata promotes VSCN output to version 6");
    EXPECT_TRUE(text.find("\"metadata\": {") != std::string::npos &&
                    text.find("\"kind\": \"float\", \"value\": 4") != std::string::npos &&
                    text.find("\"kind\": \"int\", \"value\": \"9223372036854775807\"") !=
                        std::string::npos &&
                    text.find("\"kind\": \"int\", \"value\": \"-9223372036854775808\"") !=
                        std::string::npos,
                "VSCN uses tagged metadata that preserves scalar kinds and full i64 values");

    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded != nullptr, "SceneGraph.Load reads VSCN v6 node metadata");
    if (!loaded)
        return;
    void *loaded_node = rt_scene3d_find(loaded, rt_const_cstr("gameplay_probe"));
    EXPECT_TRUE(loaded_node != nullptr, "SceneGraph.Load restores metadata-bearing nodes");
    if (!loaded_node)
        return;
    EXPECT_TRUE(rt_scene_node3d_metadata_get_int(loaded_node, health, 0) == INT64_MAX,
                "VSCN v6 round-trip preserves full-width integers");
    EXPECT_TRUE(rt_scene_node3d_metadata_get_int(loaded_node, minimum_health, 0) == INT64_MIN,
                "VSCN v6 round-trip preserves the i64 lower bound");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_kind(loaded_node, radius)), "float") ==
                0 &&
            std::fabs(rt_scene_node3d_metadata_get_float(loaded_node, radius, 0.0) - 4.0) < 0.0001,
        "VSCN v6 round-trip preserves integral-looking float kind");
    EXPECT_TRUE(
        rt_scene_node3d_metadata_get_bool(loaded_node, active, 0) != 0 &&
            std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_kind(loaded_node, optional_target)),
                        "null") == 0,
        "VSCN v6 round-trip preserves bool and explicit null");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_get_string(loaded_node, role, empty)),
                    "boss\"\nphase") == 0,
        "VSCN v6 round-trip preserves escaped string metadata");
    EXPECT_TRUE(rt_scene_node3d_metadata_remove(loaded_node, role) != 0 &&
                    rt_scene_node3d_metadata_remove(loaded_node, role) == 0,
                "SceneNode.MetadataRemove reports removal and missing-key no-op distinctly");
}

/// @brief VSCN v3 rig persistence must reconstruct cached bind TRS, preserve exact
///        names, reject JSON type confusion, and emit deterministic keyframe bytes.
static void test_scene_skeletal_roundtrip_repairs_bind_cache_and_validates_wire_data() {
    const char *path = "/tmp/zanna_scene_skeletal_roundtrip.vscn";
    const char *second_path = "/tmp/zanna_scene_skeletal_roundtrip_second.vscn";
    const char *malformed_path = "/tmp/zanna_scene_skeletal_wrong_numeric_type.vscn";
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new();
    void *skeleton = rt_skeleton3d_new();
    const char bone_name_bytes[3] = {'r', '\0', 'x'};
    rt_string bone_name = rt_string_from_bytes(bone_name_bytes, sizeof(bone_name_bytes));

    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    EXPECT_TRUE(rt_skeleton3d_add_bone(skeleton, bone_name, -1, rt_mat4_translate(3.0, 0.0, 0.0)) ==
                    0,
                "Skeletal VSCN fixture creates a translated bind pose");
    rt_string_unref(bone_name);
    rt_skeleton3d_compute_inverse_bind(skeleton);
    rt_mesh3d_set_skeleton(mesh, skeleton);
    auto *mesh_view = static_cast<rt_mesh3d *>(mesh);
    mesh_view->bone_map = static_cast<int32_t *>(calloc(1, sizeof(int32_t)));
    mesh_view->extra_influences = static_cast<vgfx3d_extra_influences_t *>(
        calloc(mesh_view->vertex_count, sizeof(vgfx3d_extra_influences_t)));
    EXPECT_TRUE(mesh_view->bone_map != nullptr && mesh_view->extra_influences != nullptr,
                "Skeletal VSCN fixture allocates rig side streams");
    if (!mesh_view->bone_map || !mesh_view->extra_influences)
        return;
    mesh_view->extra_influences[0].weights[0] = 0.25f;
    rt_scene_node3d_set_name(node, rt_const_cstr("rigged"));
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene3d_add(scene, node);

    void *animation = rt_animation3d_new(rt_const_cstr("partial"), 1.0);
    void *rotation = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scale = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_add_keyframe(animation, 0, 0.0, nullptr, rotation, scale);
    rt_animation3d_add_keyframe(animation, 0, 1.0, nullptr, rotation, scale);
    auto *scene_view = static_cast<rt_scene3d *>(scene);
    scene_view->baked_animations = static_cast<void **>(calloc(1, sizeof(void *)));
    EXPECT_TRUE(scene_view->baked_animations != nullptr,
                "Skeletal VSCN fixture allocates an animation carrier");
    if (!scene_view->baked_animations)
        return;
    scene_view->baked_animations[0] = animation;
    scene_view->baked_animation_count = 1;
    rt_obj_retain_maybe(animation);

    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1 &&
                    rt_scene3d_save(scene, rt_const_cstr(second_path)) == 1,
                "SceneGraph.Save writes repeatable skeletal VSCN documents");
    std::string first_text;
    std::string second_text;
    EXPECT_TRUE(read_text_file(path, first_text) && read_text_file(second_path, second_text) &&
                    first_text == second_text,
                "Skeletal VSCN keyframe payloads are byte-deterministic");
    EXPECT_TRUE(first_text.find("\"vertexFormat\": \"vgfx3d_vertex_le_v3\"") != std::string::npos &&
                    first_text.find("\"indexFormat\": \"u32le-v1\"") != std::string::npos &&
                    first_text.find("\"boneMapFormat\": \"i32le-v1\"") != std::string::npos &&
                    first_text.find("\"extraInfluencesFormat\": "
                                    "\"vgfx3d_extra_influences_le_v1\"") != std::string::npos &&
                    first_text.find("\"keyframeFormat\": \"vgfx3d_keyframe_le_v3\"") !=
                        std::string::npos,
                "VSCN rig payloads declare portable padding-free wire layouts");

    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded != nullptr, "SceneGraph.Load reads the skeletal VSCN fixture");
    if (loaded) {
        void *loaded_node = rt_scene3d_find(loaded, rt_const_cstr("rigged"));
        auto *loaded_mesh =
            static_cast<rt_mesh3d *>(loaded_node ? rt_scene_node3d_get_mesh(loaded_node) : nullptr);
        void *loaded_skeleton = loaded_mesh ? loaded_mesh->skeleton_ref : nullptr;
        auto *loaded_scene = static_cast<rt_scene3d *>(loaded);
        EXPECT_TRUE(loaded_skeleton != nullptr &&
                        rt_skeleton3d_get_bone_count(loaded_skeleton) == 1,
                    "Skeletal VSCN restores the mesh skeleton reference");
        if (loaded_skeleton) {
            rt_string loaded_name = rt_skeleton3d_get_bone_name(loaded_skeleton, 0);
            EXPECT_TRUE(rt_str_len(loaded_name) == 3 && std::memcmp(rt_string_cstr(loaded_name),
                                                                    bone_name_bytes,
                                                                    sizeof(bone_name_bytes)) == 0,
                        "Skeletal VSCN preserves exact bone-name bytes after embedded NUL");
        }
        EXPECT_TRUE(loaded_scene->baked_animation_count == 1 &&
                        loaded_scene->baked_animations != nullptr,
                    "Skeletal VSCN restores its animation carrier");
        if (loaded_skeleton && loaded_scene->baked_animation_count == 1 &&
            loaded_scene->baked_animations) {
            void *player = rt_anim_player3d_new(loaded_skeleton);
            rt_anim_player3d_play(player, loaded_scene->baked_animations[0]);
            rt_anim_player3d_set_time(player, 0.5);

            struct MatrixView {
                double m[16];
            };

            auto *bone_matrix =
                static_cast<MatrixView *>(rt_anim_player3d_get_bone_matrix(player, 0));
            EXPECT_TRUE(bone_matrix != nullptr,
                        "Loaded skeletal animation exposes a sampled bone matrix");
            if (bone_matrix)
                EXPECT_NEAR(bone_matrix->m[3],
                            3.0,
                            0.001,
                            "Partial loaded tracks fall back to reconstructed bind translation");
        }
    }

    size_t bind_tag = first_text.find("\"bindLocal\": [");
    size_t bind_value = bind_tag == std::string::npos ? std::string::npos
                                                      : bind_tag + std::strlen("\"bindLocal\": [");
    size_t bind_comma =
        bind_value == std::string::npos ? std::string::npos : first_text.find(',', bind_value);
    EXPECT_TRUE(bind_value != std::string::npos && bind_comma != std::string::npos,
                "Skeletal VSCN fixture contains a bindLocal numeric array");
    if (bind_value != std::string::npos && bind_comma != std::string::npos) {
        first_text.replace(bind_value, bind_comma - bind_value, "true");
        EXPECT_TRUE(write_text_file(malformed_path, first_text.c_str()),
                    "Wrong-typed bindLocal fixture can be written");
        EXPECT_TRUE(rt_scene3d_load(rt_const_cstr(malformed_path)) == nullptr,
                    "SceneGraph.Load rejects booleans in exact numeric bind arrays");
    }

    {
        std::string unknown_vertex_format = second_text;
        EXPECT_TRUE(replace_first_json_string_value(
                        unknown_vertex_format, "vertexFormat", "vgfx3d_vertex_le_future"),
                    "Unknown vertex-format mutation finds its field");
        EXPECT_TRUE(write_text_file(malformed_path, unknown_vertex_format.c_str()) &&
                        rt_scene3d_load(rt_const_cstr(malformed_path)) == nullptr,
                    "SceneGraph.Load rejects an unknown explicit vertex format");
    }
    {
        std::string malformed_extra = second_text;
        EXPECT_TRUE(
            replace_first_json_string_value(malformed_extra, "extraInfluencesBase64", "%%%%"),
            "Malformed extra-influence mutation finds its field");
        EXPECT_TRUE(write_text_file(malformed_path, malformed_extra.c_str()) &&
                        rt_scene3d_load(rt_const_cstr(malformed_path)) == nullptr &&
                        rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                    "Present malformed extra influences reject the complete VSCN transaction");
    }
    {
        std::vector<uint8_t> invalid_extra(static_cast<size_t>(mesh_view->vertex_count) * 24u, 0u);
        invalid_extra[0] = 1u;
        scene_test_write_f32_le(invalid_extra.data() + 8u, 0.25f);
        std::string invalid_semantics = second_text;
        EXPECT_TRUE(replace_first_json_string_value(
                        invalid_semantics,
                        "extraInfluencesBase64",
                        scene_test_base64_encode(invalid_extra.data(), invalid_extra.size())),
                    "Invalid extra-influence semantic mutation finds its field");
        EXPECT_TRUE(write_text_file(malformed_path, invalid_semantics.c_str()) &&
                        rt_scene3d_load(rt_const_cstr(malformed_path)) == nullptr &&
                        rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                    "SceneGraph.Load rejects positively weighted out-of-range extra bones");
    }
    {
        std::string malformed_map = second_text;
        EXPECT_TRUE(replace_first_json_string_value(malformed_map, "boneMapBase64", "AAAA"),
                    "Truncated bone-map mutation finds its field");
        EXPECT_TRUE(write_text_file(malformed_path, malformed_map.c_str()) &&
                        rt_scene3d_load(rt_const_cstr(malformed_path)) == nullptr &&
                        rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                    "Present truncated bone maps reject the complete VSCN transaction");
    }

    auto *animation_view = static_cast<rt_animation3d *>(animation);
    float saved_rotation = animation_view->channels[0].keyframes[0].rotation[0];
    animation_view->channels[0].keyframes[0].rotation[0] = NAN;
    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(second_path)) == 0,
                "SceneGraph.Save rejects non-finite skeletal keyframe lanes");
    animation_view->channels[0].keyframes[0].rotation[0] = saved_rotation;
}

static void test_scene_save_rejects_wrong_handle() {
    void *node = rt_scene_node3d_new();
    EXPECT_TRUE(rt_scene3d_save(node, rt_const_cstr("/tmp/zanna_scene_wrong_handle.vscn")) == 0,
                "SceneGraph.Save rejects non-SceneGraph handles");
}

static void test_scene_load_rejects_malformed_node_metadata() {
    const char *path = "/tmp/zanna_scene_invalid_node_metadata.vscn";
    const char *invalid_payloads[] = {"100",
                                      "\"9223372036854775808\"",
                                      "\"-9223372036854775809\"",
                                      "\"+1\"",
                                      "\"01\"",
                                      "\"-0\"",
                                      "\"\""};
    for (const char *payload : invalid_payloads) {
        std::string json =
            "{\n"
            "  \"format\": \"vscn\",\n"
            "  \"version\": 6,\n"
            "  \"textures\": [],\n"
            "  \"cubemaps\": [],\n"
            "  \"materials\": [],\n"
            "  \"meshes\": [],\n"
            "  \"nodes\": [\n"
            "    {\"name\": \"bad\", \"position\": [0,0,0], \"rotation\": [0,0,0,1], "
            "\"scale\": [1,1,1], \"visible\": true, \"mesh\": -1, \"material\": -1, "
            "\"metadata\": {\"health\": {\"kind\": \"int\", \"value\": " +
            std::string(payload) +
            "}}}\n"
            "  ]\n"
            "}\n";
        EXPECT_TRUE(write_text_file(path, json.c_str()),
                    "Malformed node-metadata fixture can be written");
        EXPECT_TRUE(rt_scene3d_load(rt_const_cstr(path)) == nullptr,
                    "SceneGraph.Load rejects non-canonical or out-of-range metadata integers");
    }
}

static void test_scene_load_rejects_malformed_json() {
    const char *path = "/tmp/zanna_scene_malformed_json.vscn";
    EXPECT_TRUE(write_text_file(path, "{\"format\":\"vscn\", \"nodes\": ["),
                "Malformed VSCN fixture can be written");
    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded == nullptr,
                "SceneGraph.Load rejects malformed JSON instead of partial maps");
}

static void test_scene_load_rejects_invalid_node_references() {
    const char *path = "/tmp/zanna_scene_invalid_node_ref.vscn";
    const char *json = "{\n"
                       "  \"format\": \"vscn\",\n"
                       "  \"version\": 2,\n"
                       "  \"textures\": [],\n"
                       "  \"cubemaps\": [],\n"
                       "  \"materials\": [],\n"
                       "  \"meshes\": [],\n"
                       "  \"nodes\": [\n"
                       "    {\"name\": \"bad\", \"position\": [0,0,0], \"rotation\": [0,0,0,1], "
                       "\"scale\": [1,1,1], \"visible\": true, \"mesh\": 0, \"material\": -1}\n"
                       "  ]\n"
                       "}\n";
    EXPECT_TRUE(write_text_file(path, json), "Invalid-reference VSCN fixture can be written");
    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded == nullptr,
                "SceneGraph.Load rejects node mesh references outside the mesh table");
}

static void test_scene_load_rejects_out_of_range_numeric_indices() {
    const char *path = "/tmp/zanna_scene_out_of_range_index.vscn";
    const char *json = "{\n"
                       "  \"format\": \"vscn\",\n"
                       "  \"version\": 2,\n"
                       "  \"textures\": [],\n"
                       "  \"cubemaps\": [],\n"
                       "  \"materials\": [],\n"
                       "  \"meshes\": [],\n"
                       "  \"nodes\": [\n"
                       "    {\"name\": \"bad\", \"position\": [0,0,0], \"rotation\": [0,0,0,1], "
                       "\"scale\": [1,1,1], \"visible\": true, "
                       "\"mesh\": 9223372036854775808.0, \"material\": -1}\n"
                       "  ]\n"
                       "}\n";
    EXPECT_TRUE(write_text_file(path, json), "Out-of-range-index VSCN fixture can be written");
    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded == nullptr,
                "SceneGraph.Load rejects out-of-range numeric indices without unsafe casts");
}

static void test_scene_load_rejects_fractional_counts_and_indices() {
    const char *texture_path = "/tmp/zanna_scene_fractional_texture_size.vscn";
    const char *texture_json = "{\n"
                               "  \"format\": \"vscn\",\n"
                               "  \"version\": 2,\n"
                               "  \"textures\": ["
                               "{\"width\": 1.5, \"height\": 1, \"rgbaBase64\": \"AAAAAA==\"}],\n"
                               "  \"cubemaps\": [],\n"
                               "  \"materials\": [],\n"
                               "  \"meshes\": [],\n"
                               "  \"nodes\": []\n"
                               "}\n";
    const char *mesh_path = "/tmp/zanna_scene_fractional_mesh_count.vscn";
    const char *mesh_json = "{\n"
                            "  \"format\": \"vscn\",\n"
                            "  \"version\": 2,\n"
                            "  \"textures\": [],\n"
                            "  \"cubemaps\": [],\n"
                            "  \"materials\": [],\n"
                            "  \"meshes\": ["
                            "{\"vertexCount\": 0.5, \"indexCount\": 0, "
                            "\"verticesBase64\": \"\", \"indicesBase64\": \"\"}],\n"
                            "  \"nodes\": []\n"
                            "}\n";
    const char *node_path = "/tmp/zanna_scene_fractional_node_ref.vscn";
    const char *node_json = "{\n"
                            "  \"format\": \"vscn\",\n"
                            "  \"version\": 2,\n"
                            "  \"textures\": [],\n"
                            "  \"cubemaps\": [],\n"
                            "  \"materials\": [],\n"
                            "  \"meshes\": [],\n"
                            "  \"nodes\": ["
                            "{\"name\": \"bad\", \"mesh\": 0.5, \"material\": -1}]\n"
                            "}\n";

    EXPECT_TRUE(write_text_file(texture_path, texture_json),
                "Fractional texture-size VSCN fixture can be written");
    EXPECT_TRUE(rt_scene3d_load(rt_const_cstr(texture_path)) == nullptr,
                "SceneGraph.Load rejects fractional texture dimensions");
    EXPECT_TRUE(write_text_file(mesh_path, mesh_json),
                "Fractional mesh-count VSCN fixture can be written");
    EXPECT_TRUE(rt_scene3d_load(rt_const_cstr(mesh_path)) == nullptr,
                "SceneGraph.Load rejects fractional mesh counts");
    EXPECT_TRUE(write_text_file(node_path, node_json),
                "Fractional node-reference VSCN fixture can be written");
    EXPECT_TRUE(rt_scene3d_load(rt_const_cstr(node_path)) == nullptr,
                "SceneGraph.Load rejects fractional node references");
}

static void test_scene_load_rejects_wrong_typed_vscn_fields() {
    const char *bool_ref_path = "/tmp/zanna_scene_bool_node_ref.vscn";
    const char *bool_ref_json = "{\n"
                                "  \"format\": \"vscn\",\n"
                                "  \"version\": 2,\n"
                                "  \"textures\": [],\n"
                                "  \"cubemaps\": [],\n"
                                "  \"materials\": [],\n"
                                "  \"meshes\": [],\n"
                                "  \"nodes\": ["
                                "{\"name\": \"bad\", \"mesh\": true, \"material\": -1}]\n"
                                "}\n";
    const char *children_path = "/tmp/zanna_scene_children_wrong_type.vscn";
    const char *children_json = "{\n"
                                "  \"format\": \"vscn\",\n"
                                "  \"version\": 2,\n"
                                "  \"textures\": [],\n"
                                "  \"cubemaps\": [],\n"
                                "  \"materials\": [],\n"
                                "  \"meshes\": [],\n"
                                "  \"nodes\": ["
                                "{\"name\": \"bad\", \"mesh\": -1, \"material\": -1, "
                                "\"children\": {}}]\n"
                                "}\n";

    EXPECT_TRUE(write_text_file(bool_ref_path, bool_ref_json),
                "Boolean-reference VSCN fixture can be written");
    EXPECT_TRUE(rt_scene3d_load(rt_const_cstr(bool_ref_path)) == nullptr,
                "SceneGraph.Load rejects boolean mesh references");
    EXPECT_TRUE(write_text_file(children_path, children_json),
                "Wrong-typed children VSCN fixture can be written");
    EXPECT_TRUE(rt_scene3d_load(rt_const_cstr(children_path)) == nullptr,
                "SceneGraph.Load rejects non-array children fields");
}

static void test_scene_load_sanitizes_degenerate_rotation() {
    const char *path = "/tmp/zanna_scene_degenerate_rotation.vscn";
    const char *json = "{\n"
                       "  \"format\": \"vscn\",\n"
                       "  \"version\": 2,\n"
                       "  \"textures\": [],\n"
                       "  \"cubemaps\": [],\n"
                       "  \"materials\": [],\n"
                       "  \"meshes\": [],\n"
                       "  \"nodes\": [\n"
                       "    {\"name\": \"rot\", \"position\": [0,0,0], \"rotation\": [0,0,0,0], "
                       "\"scale\": [1,1,1], \"visible\": true, \"mesh\": -1, \"material\": -1}\n"
                       "  ]\n"
                       "}\n";
    EXPECT_TRUE(write_text_file(path, json), "Degenerate-rotation VSCN fixture can be written");
    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded != nullptr,
                "SceneGraph.Load accepts valid scene with sanitized transform data");
    if (!loaded)
        return;
    void *node = rt_scene3d_find(loaded, rt_const_cstr("rot"));
    void *rotation = rt_scene_node3d_get_rotation(node);
    EXPECT_NEAR(rt_quat_x(rotation), 0.0, 0.001, "SceneGraph.Load resets degenerate rotation X");
    EXPECT_NEAR(rt_quat_y(rotation), 0.0, 0.001, "SceneGraph.Load resets degenerate rotation Y");
    EXPECT_NEAR(rt_quat_z(rotation), 0.0, 0.001, "SceneGraph.Load resets degenerate rotation Z");
    EXPECT_NEAR(rt_quat_w(rotation), 1.0, 0.001, "SceneGraph.Load resets degenerate rotation W");
}

static void test_scene_roundtrip_preserves_high_precision_transform() {
    const char *path = "/tmp/zanna_scene_precision_roundtrip.vscn";
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    const double x = 0.000000123456789;
    rt_scene_node3d_set_name(node, rt_const_cstr("precise"));
    rt_scene_node3d_set_position(node, x, -2.5, 3.75);
    rt_scene3d_add(scene, node);

    EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                "SceneGraph.Save writes high-precision transform scene");
    void *loaded = rt_scene3d_load(rt_const_cstr(path));
    EXPECT_TRUE(loaded != nullptr, "SceneGraph.Load reads high-precision transform scene");
    if (!loaded)
        return;
    void *loaded_node = rt_scene3d_find(loaded, rt_const_cstr("precise"));
    void *pos = rt_scene_node3d_get_position(loaded_node);
    EXPECT_NEAR(rt_vec3_x(pos), x, 1e-12, "SceneGraph.Save/Load preserves sub-micro positions");
}

static void test_scene_extreme_finite_transforms_and_queries_remain_bounded() {
    typedef struct {
        double m[16];
    } mat4_view;

    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    rt_scene_node3d_set_position(node, 1.0e300, 0.0, -1.0e300);
    rt_scene_node3d_set_scale(node, 1.0e300, -1.0e300, 1.0e300);
    rt_scene_node3d_set_rotation(node, rt_quat_new(1.0e300, 0.0, 0.0, 0.0));
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene3d_add(scene, node);

    mat4_view *wm = (mat4_view *)rt_scene_node3d_get_world_matrix(node);
    for (int i = 0; i < 16; ++i)
        EXPECT_TRUE(std::isfinite(wm->m[i]), "Extreme finite SceneNode world matrix is finite");

    void *world_pos = rt_scene_node3d_get_world_position(node);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(world_pos)) && std::isfinite(rt_vec3_y(world_pos)) &&
                    std::isfinite(rt_vec3_z(world_pos)),
                "Extreme finite SceneNode world position is finite");
    EXPECT_TRUE(fabs(rt_vec3_x(world_pos)) <= 1000000000000.0 &&
                    fabs(rt_vec3_z(world_pos)) <= 1000000000000.0,
                "Extreme finite SceneNode world position is clamped");

    void *aabb_hits = rt_scene3d_query_aabb(
        scene, rt_vec3_new(-1.0e300, -1.0e300, -1.0e300), rt_vec3_new(1.0e300, 1.0e300, 1.0e300));
    EXPECT_TRUE(rt_seq_len(aabb_hits) == 1, "Extreme finite QueryAABB returns the bounded node");

    void *sphere_hits = rt_scene3d_query_sphere(scene, rt_vec3_new(0.0, 0.0, 0.0), 1.0e300);
    EXPECT_TRUE(rt_seq_len(sphere_hits) == 1,
                "Extreme finite QuerySphere radius is clamped but still covers the bounded node");

    void *ray_hit = rt_scene3d_raycast_nodes(
        scene, rt_vec3_new(1.0e300, 0.0, -1.0e300), rt_vec3_new(-1.0e300, 0.0, 1.0e300), INFINITY);
    EXPECT_TRUE(ray_hit == node, "Extreme finite RaycastNodes stays bounded and hits the node");
}

/// UAF regression: the animator's channel-target cache must retain its nodes.
/// A playing clip whose target is detached (and its last external reference
/// dropped) previously left a dangling weak pointer in the cache; the next
/// update dereferenced freed memory inside the descendant re-check.
static void test_node_animator_survives_target_removal_mid_clip() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    rt_scene_node3d_set_name(child, rt_const_cstr("limb"));
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);

    double times[2] = {0.0, 1.0};
    float translation_values[6] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    void *clip = rt_node_animation3d_new(rt_const_cstr("limb_clip"), 1.0);
    EXPECT_TRUE(rt_node_animation3d_add_channel(clip,
                                                rt_const_cstr("limb"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                times,
                                                translation_values) == 0,
                "limb channel is created");
    void *clips[1] = {clip};
    void *animator = rt_node_animator3d_new_from_clips(clips, 1);
    rt_scene_node3d_bind_node_animator(parent, animator);

    /* First update resolves and caches the child target. */
    rt_scene3d_sync_bindings(scene, 0.1);
    void *pos = rt_scene_node3d_get_position(child);
    EXPECT_TRUE(rt_vec3_x(pos) > 0.0, "clip drives the named child before removal");

    /* Drop our reference, then detach: the parent's release is the LAST
     * external one, so without the cache retaining the node this frees it. */
    if (rt_obj_release_check0(child))
        rt_obj_free(child);
    rt_scene_node3d_remove_child(parent, child);

    /* Next updates must not touch freed memory (cache re-validates safely). */
    rt_scene3d_sync_bindings(scene, 0.1);
    rt_scene3d_sync_bindings(scene, 0.1);
    EXPECT_TRUE(1, "animator update after target removal is memory-safe");
}

/// @brief Read one whole file into a std::string ("" when unreadable).
static std::string prefab_read_file(const char *path) {
    std::string contents;
    FILE *file = fopen(path, "rb");
    if (!file)
        return contents;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0)
        contents.append(chunk, got);
    fclose(file);
    return contents;
}

/// @brief ADR 0187: VSCN v7 prefab references — serialize reference-only,
///        graft on load, placeholder safety, unpack, and v6 non-promotion.
static void test_scene_prefab_reference_nodes() {
    const char *source_path = "/tmp/zanna_prefab_source.scene3d";
    const char *world_path = "/tmp/zanna_prefab_world.scene3d";
    const char *world_resaved_path = "/tmp/zanna_prefab_world_resaved.scene3d";
    const char *broken_path = "/tmp/zanna_prefab_broken.scene3d";
    const char *broken_resaved_path = "/tmp/zanna_prefab_broken_resaved.scene3d";
    const char *cycle_path = "/tmp/zanna_prefab_cycle.scene3d";
    const char *plain_path = "/tmp/zanna_prefab_plain.scene3d";

    /* Source prefab: a parent mesh node with one child. */
    void *source = rt_scene3d_new();
    void *lamp = rt_scene_node3d_new();
    rt_scene_node3d_set_name(lamp, rt_const_cstr("Lamp Post"));
    void *lamp_head = rt_scene_node3d_new();
    rt_scene_node3d_set_name(lamp_head, rt_const_cstr("Lamp Head"));
    rt_scene_node3d_set_position(lamp_head, 0.0, 1.6, 0.0);
    rt_scene_node3d_add_child(lamp, lamp_head);
    rt_scene3d_add(source, lamp);
    EXPECT_TRUE(rt_scene3d_save(source, rt_const_cstr(source_path)) == 1,
                "prefab source scene saves");

    /* Referencing world: two instances plus one plain node. */
    void *world = rt_scene3d_new();
    void *instance_a = rt_scene_node3d_new();
    rt_scene_node3d_set_name(instance_a, rt_const_cstr("Lamp A"));
    rt_scene_node3d_set_position(instance_a, 5.0, 0.0, 0.0);
    EXPECT_TRUE(rt_scene_node3d_set_prefab_reference(
                    instance_a, rt_const_cstr("zanna_prefab_source.scene3d")) == 1,
                "relative prefab reference is authored");
    EXPECT_TRUE(
        rt_scene_node3d_set_prefab_reference(instance_a, rt_const_cstr("/abs/source.scene3d")) == 0,
        "absolute prefab reference is rejected at authoring time");
    {
        rt_string ref = rt_scene_node3d_get_prefab_path(instance_a);
        EXPECT_TRUE(ref && strcmp(rt_string_cstr(ref), "zanna_prefab_source.scene3d") == 0,
                    "PrefabPath reports the authored reference");
        rt_string_unref(ref);
    }
    void *instance_b = rt_scene_node3d_new();
    rt_scene_node3d_set_name(instance_b, rt_const_cstr("Lamp B"));
    rt_scene_node3d_set_prefab_reference(instance_b, rt_const_cstr("zanna_prefab_source.scene3d"));
    rt_scene_node3d_metadata_set_string(instance_b, rt_const_cstr("zone"), rt_const_cstr("plaza"));
    void *plain = rt_scene_node3d_new();
    rt_scene_node3d_set_name(plain, rt_const_cstr("Ground"));
    rt_scene3d_add(world, instance_a);
    rt_scene3d_add(world, instance_b);
    rt_scene3d_add(world, plain);
    EXPECT_TRUE(rt_scene3d_save(world, rt_const_cstr(world_path)) == 1, "prefab world scene saves");
    {
        std::string text = prefab_read_file(world_path);
        EXPECT_TRUE(text.find("\"version\": 7") != std::string::npos,
                    "prefab scenes serialize as VSCN v7");
        EXPECT_TRUE(text.find("Lamp Post") == std::string::npos,
                    "grafted source content never serializes into the referencing file");
    }

    void *loaded = rt_scene3d_load(rt_const_cstr(world_path));
    EXPECT_TRUE(loaded != nullptr, "v7 world loads");
    EXPECT_TRUE(rt_scene3d_get_unresolved_prefab_count(loaded) == 0,
                "fully resolved worlds report zero unresolved prefabs");
    EXPECT_TRUE(rt_asset_error_get_warning_count() == 0,
                "fully resolved worlds load without prefab warnings");
    if (loaded) {
        void *root = rt_scene3d_get_root(loaded);
        EXPECT_TRUE(rt_scene_node3d_child_count(root) == 3, "world keeps three top nodes");
        void *loaded_a = rt_scene_node3d_get_child(root, 0);
        EXPECT_TRUE(rt_scene_node3d_child_count(loaded_a) == 1,
                    "instance grafts the referenced root children");
        void *grafted = rt_scene_node3d_get_child(loaded_a, 0);
        EXPECT_TRUE(rt_scene_node3d_get_is_instance_content(grafted) == 1,
                    "grafted nodes carry the instance-content flag");
        EXPECT_TRUE(
            rt_scene_node3d_get_is_instance_content(rt_scene_node3d_get_child(grafted, 0)) == 1,
            "instance-content flag reaches grafted descendants");
        EXPECT_TRUE(rt_scene_node3d_get_is_instance_content(rt_scene_node3d_get_child(root, 2)) ==
                        0,
                    "plain nodes never gain the instance flag");
        {
            rt_string zone = rt_scene_node3d_metadata_get_string(
                rt_scene_node3d_get_child(root, 1), rt_const_cstr("zone"), rt_const_cstr(""));
            EXPECT_TRUE(zone && strcmp(rt_string_cstr(zone), "plaza") == 0,
                        "prefab node metadata overrides round-trip");
            rt_string_unref(zone);
        }
        EXPECT_TRUE(rt_scene3d_save(loaded, rt_const_cstr(world_resaved_path)) == 1,
                    "loaded v7 world resaves");
        EXPECT_TRUE(prefab_read_file(world_resaved_path) == prefab_read_file(world_path),
                    "v7 round-trip is byte-identical");
    }

    /* Missing source: reference-retaining placeholder, byte-stable. */
    void *broken = rt_scene3d_new();
    void *ghost = rt_scene_node3d_new();
    rt_scene_node3d_set_name(ghost, rt_const_cstr("Ghost"));
    rt_scene_node3d_set_prefab_reference(ghost, rt_const_cstr("zanna_prefab_missing.scene3d"));
    rt_scene3d_add(broken, ghost);
    EXPECT_TRUE(rt_scene3d_save(broken, rt_const_cstr(broken_path)) == 1,
                "broken-reference scene saves");
    void *broken_loaded = rt_scene3d_load(rt_const_cstr(broken_path));
    EXPECT_TRUE(broken_loaded != nullptr, "missing prefab source still loads the scene");
    EXPECT_TRUE(rt_scene3d_get_unresolved_prefab_count(broken_loaded) == 1,
                "a missing prefab source is counted as unresolved (ADR 0227)");
    EXPECT_TRUE(rt_asset_error_get_warning_count() == 1,
                "a missing prefab source adds exactly one load warning");
    EXPECT_TRUE(strstr(rt_asset_error_get_warning(0), "zanna_prefab_missing.scene3d") != nullptr &&
                    strstr(rt_asset_error_get_warning(0), "placeholder") != nullptr,
                "the prefab warning names the unresolved reference");
    if (broken_loaded) {
        void *ghost_loaded = rt_scene_node3d_get_child(rt_scene3d_get_root(broken_loaded), 0);
        EXPECT_TRUE(rt_scene_node3d_child_count(ghost_loaded) == 0,
                    "missing source loads as an empty placeholder");
        rt_string ref = rt_scene_node3d_get_prefab_path(ghost_loaded);
        EXPECT_TRUE(ref && strcmp(rt_string_cstr(ref), "zanna_prefab_missing.scene3d") == 0,
                    "placeholder retains its prefab reference");
        rt_string_unref(ref);
        EXPECT_TRUE(rt_scene3d_save(broken_loaded, rt_const_cstr(broken_resaved_path)) == 1,
                    "placeholder scene resaves");
        EXPECT_TRUE(prefab_read_file(broken_resaved_path) == prefab_read_file(broken_path),
                    "placeholder round-trip is byte-identical");
    }

    /* Self-cycle: loads as a placeholder without recursion. */
    void *cyclic = rt_scene3d_new();
    void *self_node = rt_scene_node3d_new();
    rt_scene_node3d_set_name(self_node, rt_const_cstr("Self"));
    rt_scene_node3d_set_prefab_reference(self_node, rt_const_cstr("zanna_prefab_cycle.scene3d"));
    rt_scene3d_add(cyclic, self_node);
    EXPECT_TRUE(rt_scene3d_save(cyclic, rt_const_cstr(cycle_path)) == 1, "cycle scene saves");
    void *cycle_loaded = rt_scene3d_load(rt_const_cstr(cycle_path));
    EXPECT_TRUE(cycle_loaded != nullptr, "self-referencing scene loads");
    EXPECT_TRUE(rt_scene3d_get_unresolved_prefab_count(cycle_loaded) == 1,
                "a reference cycle is counted as unresolved (ADR 0227)");
    EXPECT_TRUE(rt_asset_error_get_warning_count() == 1 &&
                    strstr(rt_asset_error_get_warning(0), "cycle") != nullptr,
                "a reference cycle adds one warning naming the reason");
    if (cycle_loaded) {
        EXPECT_TRUE(rt_scene_node3d_child_count(
                        rt_scene_node3d_get_child(rt_scene3d_get_root(cycle_loaded), 0)) == 0,
                    "self-reference resolves to an empty placeholder");
    }

    /* Unpack: clearing the reference makes grafted content plain. */
    void *unpack_scene = rt_scene3d_load(rt_const_cstr(world_path));
    if (unpack_scene) {
        void *unpack_node = rt_scene_node3d_get_child(rt_scene3d_get_root(unpack_scene), 0);
        EXPECT_TRUE(rt_scene_node3d_clear_prefab_reference(unpack_node) == 1,
                    "unpack clears the prefab reference");
        rt_string cleared = rt_scene_node3d_get_prefab_path(unpack_node);
        EXPECT_TRUE(cleared && rt_string_cstr(cleared)[0] == '\0',
                    "unpacked nodes report an empty reference");
        rt_string_unref(cleared);
        EXPECT_TRUE(
            rt_scene_node3d_get_is_instance_content(rt_scene_node3d_get_child(unpack_node, 0)) == 0,
            "unpack clears descendant instance flags");
        EXPECT_TRUE(rt_scene3d_save(unpack_scene, rt_const_cstr(plain_path)) == 1,
                    "unpacked scene saves");
        std::string unpacked_text = prefab_read_file(plain_path);
        EXPECT_TRUE(unpacked_text.find("Lamp Post") != std::string::npos,
                    "unpacked instance content serializes as plain nodes");
    }

    /* Golden fixture: the checked-in v7 file keeps loading with the same
     * structure and resaving byte-identically as the serializer evolves. */
    {
        std::string golden_path = "src/tests/fixtures/runtime/prefab_world_v7.scene3d";
        const char *golden_resave = "/tmp/zanna_prefab_golden_resave.scene3d";
        std::string golden_text = prefab_read_file(golden_path.c_str());
        if (golden_text.empty()) {
            /* ctest runs from build/src/tests; direct runs use the repo root. */
            golden_path = "../../../src/tests/fixtures/runtime/prefab_world_v7.scene3d";
            golden_text = prefab_read_file(golden_path.c_str());
        }
        EXPECT_TRUE(!golden_text.empty(), "golden v7 fixture is present");
        if (!golden_text.empty()) {
            void *golden = rt_scene3d_load(rt_const_cstr(golden_path.c_str()));
            EXPECT_TRUE(golden != nullptr, "golden v7 fixture loads");
            if (golden) {
                void *golden_root = rt_scene3d_get_root(golden);
                void *golden_instance = rt_scene_node3d_get_child(golden_root, 0);
                EXPECT_TRUE(rt_scene_node3d_child_count(golden_instance) == 1,
                            "golden fixture grafts its referenced pillar");
                EXPECT_TRUE(rt_scene_node3d_get_is_instance_content(
                                rt_scene_node3d_get_child(golden_instance, 0)) == 1,
                            "golden grafted content is instance-flagged");
                EXPECT_TRUE(rt_scene3d_save(golden, rt_const_cstr(golden_resave)) == 1,
                            "golden fixture resaves");
                EXPECT_TRUE(prefab_read_file(golden_resave) == golden_text,
                            "golden v7 fixture round-trips byte-identically");
            }
        }
    }

    /* Scenes without prefabs never promote to v7. */
    void *plain_scene = rt_scene3d_new();
    void *sole = rt_scene_node3d_new();
    rt_scene_node3d_set_name(sole, rt_const_cstr("Sole"));
    rt_scene3d_add(plain_scene, sole);
    EXPECT_TRUE(rt_scene3d_save(plain_scene, rt_const_cstr(plain_path)) == 1,
                "prefab-free scene saves");
    EXPECT_TRUE(prefab_read_file(plain_path).find("\"version\": 7") == std::string::npos,
                "prefab-free scenes stay below v7");
}

static void test_scene_adopt_baked_animations() {
    void *source = rt_scene3d_new();
    void *destination = rt_scene3d_new();
    void *clip_walk = rt_animation3d_new(rt_const_cstr("walk"), 1.0);
    void *clip_run = rt_animation3d_new(rt_const_cstr("run"), 0.5);

    /* Install the clip carrier exactly as InstantiateScene does. */
    rt_scene3d *source_view = (rt_scene3d *)source;
    source_view->baked_animations = (void **)calloc(2, sizeof(void *));
    EXPECT_TRUE(source_view->baked_animations != NULL,
                "adopt fixture allocates the source clip carrier");
    source_view->baked_animations[0] = clip_walk;
    source_view->baked_animations[1] = clip_run;
    source_view->baked_animation_count = 2;
    rt_obj_retain_maybe(clip_walk);
    rt_obj_retain_maybe(clip_run);

    EXPECT_TRUE(rt_scene3d_adopt_baked_animations(destination, source) == 2,
                "adopting a two-clip carrier reports two adopted clips");
    rt_scene3d *destination_view = (rt_scene3d *)destination;
    EXPECT_TRUE(destination_view->baked_animation_count == 2,
                "destination carries both adopted clips");
    EXPECT_TRUE(destination_view->baked_animations[0] == clip_walk &&
                    destination_view->baked_animations[1] == clip_run,
                "adopted clips keep source order and identity");
    EXPECT_TRUE(source_view->baked_animation_count == 2,
                "adoption is copy-retain: the source keeps its clips");

    /* Idempotent: pointer-identical clips are skipped on re-adoption. */
    EXPECT_TRUE(rt_scene3d_adopt_baked_animations(destination, source) == 0,
                "re-adopting the same carrier adopts nothing");
    EXPECT_TRUE(destination_view->baked_animation_count == 2,
                "re-adoption leaves the destination carrier unchanged");

    /* Partial overlap: only the fresh clip is adopted. */
    void *clip_idle = rt_animation3d_new(rt_const_cstr("idle"), 2.0);
    void **grown = (void **)calloc(3, sizeof(void *));
    EXPECT_TRUE(grown != NULL, "adopt fixture grows the source carrier");
    grown[0] = clip_walk;
    grown[1] = clip_run;
    grown[2] = clip_idle;
    rt_obj_retain_maybe(clip_idle);
    free(source_view->baked_animations);
    source_view->baked_animations = grown;
    source_view->baked_animation_count = 3;
    EXPECT_TRUE(rt_scene3d_adopt_baked_animations(destination, source) == 1,
                "overlapping adoption reports only the fresh clip");
    EXPECT_TRUE(destination_view->baked_animation_count == 3 &&
                    destination_view->baked_animations[2] == clip_idle,
                "the fresh clip appends after the existing carrier");

    /* Degenerate inputs adopt nothing. */
    EXPECT_TRUE(rt_scene3d_adopt_baked_animations(source, source) == 0, "self-adoption is a no-op");
    EXPECT_TRUE(rt_scene3d_adopt_baked_animations(NULL, source) == 0,
                "a null destination adopts nothing");
    EXPECT_TRUE(rt_scene3d_adopt_baked_animations(destination, NULL) == 0,
                "a null source adopts nothing");
    EXPECT_TRUE(rt_scene3d_adopt_baked_animations(destination, rt_scene3d_new()) == 0,
                "an empty source adopts nothing");
}

int main(int argc, char **argv) {
    /* The 10k spatial-index scaling fixture costs ~30 seconds and used to
     * push this whole suite behind the slow label, which excluded every
     * VSCN round-trip, metadata, and prefab check from the default gate.
     * The scale fixture runs only under --scale (registered as the slow
     * test test_rt_scene3d_scale); everything else runs by default. */
    if (argc > 1 && std::strcmp(argv[1], "--scale") == 0) {
        test_scene_spatial_index_10k_scaling_fixture();
        return 0;
    }
    test_node_animator_survives_target_removal_mid_clip();
    test_create_scene_and_node();
    test_add_remove_child();
    test_try_add_reports_parenting_success();
    test_scene_remove_ignores_nodes_from_other_scenes();
    test_scene_rejects_reparenting_implicit_root();
    test_owner_propagation_allocation_failure_is_atomic();
    test_translation_propagation();
    test_world_position_and_scale_getters();
    test_scene_rebase_origin_shifts_root_subtrees();
    test_transform_components_and_batch();
    test_rotation_propagation();
    test_scale_propagation();
    test_deep_hierarchy();
    test_hierarchy_epochs_are_root_local();
    test_deep_hierarchy_iterative_traversal();
    test_dirty_flag();
    test_find_by_name();
    test_scene_node_names_reject_wrong_string_handles();
    test_reparenting();
    test_node_count_tracks_nested_hierarchy_edits();
    test_prevent_cycle();
    test_visibility();
    test_subtree_aabb_includes_child_meshes();
    test_clear();
    test_get_child();
    test_try_move_child_reorders_atomically();
    test_try_add_child_preserve_world_keeps_complete_subtree_pose();
    test_try_add_child_preserve_world_rejects_lossy_changes_atomically();
    test_try_set_world_matrix_is_exact_and_atomic();
    test_default_transform();
    test_node_sanitizes_nonfinite_transform_and_lod();
    test_scene_repairs_corrupt_private_counts();
    test_node_body_sync_preserves_negative_scale_handedness();

    /* Frustum culling */
    test_frustum_aabb_inside();
    test_frustum_sphere_aabb();
    test_frustum_plane_aabb();
    test_frustum_no_mesh_no_aabb();
    test_node_aabb_refreshes_after_mesh_mutation();
    test_frustum_culled_count_initial();
    test_lod_culling_uses_stable_union_mesh_bounds();
    test_auto_lod_uses_screen_error_selection();
    test_auto_lod_radius_uses_world_scale();
    test_lod_residency_falls_back_and_reports_bytes();
    test_impostor_proxy_draws_textured_quad();
    test_scene_draw_rejects_corrupt_draw_handles();
    test_dynamic_deformation_uses_conservative_frustum_culling();
    test_morph_delta_bounds_keep_deformed_mesh_visible();
    test_raw_morph_delta_bounds_handle_large_finite_values();
    test_dynamic_deformation_contains_nonfinite_palette_translation();
    test_dynamic_deformation_rejects_corrupt_morph_delta_span();
    test_parent_animator_drives_child_skinned_meshes();
    test_scene_spatial_queries_flat_walk_reference();
    test_scene_spatial_query_append_telemetry_and_stable_order();
    test_scene_precise_raycast_selects_through_aabb_gaps();
    test_scene_precise_raycast_allocation_failure_returns_no_partial_result();
    test_scene_spatial_queries_validate_vec3_args_before_result_alloc();
    test_scene_spatial_index_rebuilds_on_dirty_node();
    test_scene_spatial_mesh_dependencies_are_scene_local();
    test_scene_spatial_index_repairs_storage_and_query_pool_metadata();
    test_scene_spatial_index_contains_corrupt_cached_topology();
    test_scene_spatial_refit_repairs_bounds_when_revisions_agree();
    test_scene_draw_spatial_index_matches_flat_reference();
    test_scene_extreme_finite_transforms_and_queries_remain_bounded();
    test_scene_occlusion_grid_uses_spatial_candidates();
    test_scene_shadow_caster_sweep_keeps_offscreen_casters();
    test_scene_portal_pvs_culls_unlinked_interior_zones();
    test_scene_spatial_index_preserves_far_origin_precision();
    test_scene_draw_reuses_active_frame();
    test_scene_draw_culling_uses_canvas_output_aspect();
    test_scene_prefab_reference_nodes();
    test_scene_adopt_baked_animations();
    test_scene_save_escapes_json_names();
    test_scene_save_text_matches_file_bytes();
    test_scene_save_serializes_visibility_and_lod_metadata();
    test_scene_roundtrip_loads_shared_assets();
    test_scene_roundtrip_preserves_authoring_metadata();
    test_scene_node_metadata_is_typed_bounded_and_persistent();
    test_scene_skeletal_roundtrip_repairs_bind_cache_and_validates_wire_data();
    test_scene_roundtrip_deep_hierarchy_uses_format_depth_limit();
    test_scene_save_skips_invalid_material_asset_refs();
    test_node_animator_handles_large_morph_weight_channels();
    test_node_animator_public_controls_drive_bound_nodes();
    test_node_animator_empty_clip_is_noop_but_advances_time();
    test_node_animator_clears_unkeyed_morph_weight_tail();
    test_node_animator_skips_corrupt_channel_shape();
    test_node_animator_skips_corrupt_channel_interpolation();
    test_node_animator_import_index_binding_does_not_fallback_to_duplicate_name();
    test_node_animator_rebuilds_all_targets_in_one_tree_walk();
    test_node_animator_weights_all_morph_primitives_in_subtree();
    test_node_animator_samples_cubic_translation_channels();
    test_node_animator_slerps_linear_rotation_channels();
    test_node_animator_samples_cubic_rotation_shortest_hemisphere();
    test_node_animator_repairs_corrupt_clip_duration();
    test_node_animation_rejects_invalid_channel_data();
    test_node_animation_rejects_wrong_string_handles();
    test_node_animation_step_accepts_duplicate_key_times();
    test_node_animation_rejects_pathological_channel_sizes();
    test_node_animation_rejects_ambiguous_byte_names();
    test_node_animator_repairs_cache_and_sample_corruption();
    test_node_animator_stops_on_exact_one_shot_endpoints();
    test_scene_draw_includes_node_attached_lights();
    test_scene_node_light_property_retains_rejects_and_clears();
    test_scene_node_attached_camera_tracks_world_transform();
    test_scene_roundtrip_preserves_node_lights();
    test_scene_save_rejects_wrong_handle();
    test_scene_load_rejects_malformed_node_metadata();
    test_scene_load_rejects_malformed_json();
    test_scene_load_rejects_invalid_node_references();
    test_scene_load_rejects_out_of_range_numeric_indices();
    test_scene_load_rejects_fractional_counts_and_indices();
    test_scene_load_rejects_wrong_typed_vscn_fields();
    test_scene_load_sanitizes_degenerate_rotation();
    test_scene_roundtrip_preserves_high_precision_transform();

    printf("SceneGraph tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
