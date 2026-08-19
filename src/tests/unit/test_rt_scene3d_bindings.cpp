//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_scene3d_bindings.cpp
// Purpose: Unit tests for SceneGraph / SceneNode runtime bindings to physics
//   bodies and animation controllers.
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_animcontroller3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_physics3d.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
#include "vgfx3d_backend.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_quat_new(double x, double y, double z, double w);
extern void *rt_quat_from_axis_angle(void *axis, double angle);
extern void *rt_quat_from_euler(double pitch, double yaw, double roll);
extern double rt_quat_x(void *q);
extern double rt_quat_y(void *q);
extern double rt_quat_z(void *q);
extern double rt_quat_w(void *q);
extern void *rt_mat4_identity(void);
extern double rt_mat4_get(void *m, int64_t row, int64_t col);
extern rt_string rt_const_cstr(const char *s);
extern void *rt_material3d_new_color(double r, double g, double b);
extern void *rt_mesh3d_new_box(double w, double h, double d);
extern void rt_mesh3d_set_bone_weights(void *mesh,
                                       int64_t vertex_index,
                                       int64_t b0,
                                       double w0,
                                       int64_t b1,
                                       double w1,
                                       int64_t b2,
                                       double w2,
                                       int64_t b3,
                                       double w3);
extern void *rt_camera3d_new(double fov, double aspect, double near, double far);
extern void rt_camera3d_look_at(void *cam, void *eye, void *target, void *up);
extern void *rt_morphtarget3d_new(int64_t vertex_count);
extern int64_t rt_morphtarget3d_add_shape(void *mt, rt_string name);
extern void rt_morphtarget3d_set_delta(
    void *mt, int64_t shape, int64_t vertex, double dx, double dy, double dz);
extern void rt_morphtarget3d_set_weight(void *mt, int64_t shape, double weight);
extern void rt_mesh3d_set_morph_targets(void *mesh, void *morph_targets);
}

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

static void *make_anim(const char *name,
                       int64_t bone_index,
                       double x0,
                       double y0,
                       double z0,
                       double x1,
                       double y1,
                       double z1) {
    void *anim = rt_animation3d_new(rt_const_cstr(name), 1.0);
    void *pos0 = rt_vec3_new(x0, y0, z0);
    void *pos1 = rt_vec3_new(x1, y1, z1);
    void *rot = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_set_looping(anim, 1);
    rt_animation3d_add_keyframe(anim, bone_index, 0.0, pos0, rot, scl);
    rt_animation3d_add_keyframe(anim, bone_index, 1.0, pos1, rot, scl);
    return anim;
}

static void *make_anim_with_rotation(void *rot0, void *rot1) {
    void *anim = rt_animation3d_new(rt_const_cstr("turn"), 1.0);
    void *pos = rt_vec3_new(0.0, 0.0, 0.0);
    void *scl = rt_vec3_new(1.0, 1.0, 1.0);
    rt_animation3d_set_looping(anim, 0);
    rt_animation3d_add_keyframe(anim, 0, 0.0, pos, rot0, scl);
    rt_animation3d_add_keyframe(anim, 0, 1.0, pos, rot1, scl);
    return anim;
}

static void init_test_canvas(rt_canvas3d *canvas, const vgfx3d_backend_t *backend) {
    std::memset(canvas, 0, sizeof(*canvas));
    canvas->backend = backend;
    canvas->gfx_win = (vgfx_window_t)1;
}

static int g_submit_count = 0;
static const float *g_last_bone_palette = nullptr;
static int32_t g_last_bone_count = 0;
static const vgfx3d_vertex_t *g_last_vertices = nullptr;

static void test_begin_frame(void *, const vgfx3d_camera_params_t *) {}

static void test_end_frame(void *) {}

static void test_submit_draw(void *,
                             vgfx_window_t,
                             const vgfx3d_draw_cmd_t *cmd,
                             const vgfx3d_light_params_t *,
                             int32_t,
                             const float *,
                             int8_t,
                             int8_t) {
    g_submit_count++;
    g_last_bone_palette = cmd ? cmd->bone_palette : nullptr;
    g_last_bone_count = cmd ? cmd->bone_count : 0;
    g_last_vertices = cmd ? cmd->vertices : nullptr;
}

static void test_node_from_body_resolves_child_local_space() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *body = rt_body3d_new_sphere(0.5, 1.0);
    void *rot = rt_quat_from_euler(0.0, 1.5707963267948966, 0.0);
    void *pos;
    void *local_rot;
    rt_scene_node3d_set_position(parent, 5.0, 0.0, 0.0);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);
    rt_body3d_set_position(body, 6.0, 2.0, -3.0);
    rt_body3d_set_orientation(body, rot);

    rt_scene_node3d_bind_body(child, body);
    rt_scene_node3d_set_sync_mode(child, RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY);
    rt_scene3d_sync_bindings(scene, 0.016);

    pos = rt_scene_node3d_get_position(child);
    local_rot = rt_scene_node3d_get_rotation(child);
    EXPECT_NEAR(rt_vec3_x(pos), 1.0, 0.001, "NodeFromBody converts world X into child local X");
    EXPECT_NEAR(rt_vec3_y(pos), 2.0, 0.001, "NodeFromBody preserves child local Y");
    EXPECT_NEAR(rt_vec3_z(pos), -3.0, 0.001, "NodeFromBody preserves child local Z");
    EXPECT_NEAR(rt_quat_y(local_rot), rt_quat_y(rot), 0.001, "NodeFromBody copies orientation");
    EXPECT_NEAR(rt_quat_w(local_rot), rt_quat_w(rot), 0.001, "NodeFromBody copies orientation W");
}

static void test_body_from_node_uses_world_space() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *body = rt_body3d_new_sphere(0.5, 1.0);
    void *body_pos;
    void *body_rot;
    rt_scene_node3d_set_rotation(parent, rt_quat_from_euler(0.0, 1.5707963267948966, 0.0));
    rt_scene_node3d_set_position(child, 1.0, 0.0, 0.0);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);

    rt_scene_node3d_bind_body(child, body);
    rt_scene_node3d_set_sync_mode(child, RT_SCENE_NODE3D_SYNC_BODY_FROM_NODE);
    rt_scene3d_sync_bindings(scene, 0.016);

    body_pos = rt_body3d_get_position(body);
    body_rot = rt_body3d_get_orientation(body);
    EXPECT_NEAR(rt_vec3_x(body_pos), 0.0, 0.01, "BodyFromNode pushes rotated child world X");
    EXPECT_NEAR(rt_vec3_y(body_pos), 0.0, 0.01, "BodyFromNode pushes child world Y");
    EXPECT_NEAR(rt_vec3_z(body_pos), -1.0, 0.01, "BodyFromNode pushes rotated child world Z");
    EXPECT_NEAR(std::fabs(rt_quat_y(body_rot)),
                std::fabs(rt_quat_y(rt_scene_node3d_get_rotation(parent))),
                0.001,
                "BodyFromNode composes parent rotation into body orientation");
}

static void test_two_way_kinematic_switches_direction() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *body = rt_body3d_new_sphere(0.5, 1.0);
    void *body_pos;
    void *node_pos;
    rt_scene3d_add(scene, node);
    rt_scene_node3d_bind_body(node, body);
    rt_scene_node3d_set_sync_mode(node, RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC);

    rt_body3d_set_kinematic(body, 1);
    rt_scene_node3d_set_position(node, 2.5, 0.0, 0.0);
    rt_scene3d_sync_bindings(scene, 0.016);
    body_pos = rt_body3d_get_position(body);
    EXPECT_NEAR(
        rt_vec3_x(body_pos), 2.5, 0.001, "TwoWayKinematic pushes node pose into kinematic body");

    rt_body3d_set_kinematic(body, 0);
    rt_body3d_set_position(body, -4.0, 0.0, 0.0);
    rt_scene3d_sync_bindings(scene, 0.016);
    node_pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(
        rt_vec3_x(node_pos), -4.0, 0.001, "TwoWayKinematic pulls dynamic body pose back into node");
}

static void test_node_from_body_preserves_local_scale_under_scaled_parent() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *body = rt_body3d_new_sphere(0.5, 1.0);
    void *local_pos;
    void *local_scale;
    void *local_rot;

    rt_scene_node3d_set_scale(parent, 2.0, 3.0, 4.0);
    rt_scene_node3d_set_scale(child, 0.5, 0.75, 1.25);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);

    rt_body3d_set_position(body, 4.0, 9.0, 16.0);
    rt_body3d_set_orientation(body, rt_quat_new(0.0, 0.0, 0.0, 1.0));

    rt_scene_node3d_bind_body(child, body);
    rt_scene_node3d_set_sync_mode(child, RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY);
    rt_scene3d_sync_bindings(scene, 0.016);

    local_pos = rt_scene_node3d_get_position(child);
    local_scale = rt_scene_node3d_get_scale(child);
    local_rot = rt_scene_node3d_get_rotation(child);

    EXPECT_NEAR(
        rt_vec3_x(local_pos), 2.0, 0.001, "Scaled parent sync divides world X by parent scale");
    EXPECT_NEAR(
        rt_vec3_y(local_pos), 3.0, 0.001, "Scaled parent sync divides world Y by parent scale");
    EXPECT_NEAR(
        rt_vec3_z(local_pos), 4.0, 0.001, "Scaled parent sync divides world Z by parent scale");
    EXPECT_NEAR(rt_vec3_x(local_scale), 0.5, 0.001, "Body sync preserves local X scale");
    EXPECT_NEAR(rt_vec3_y(local_scale), 0.75, 0.001, "Body sync preserves local Y scale");
    EXPECT_NEAR(rt_vec3_z(local_scale), 1.25, 0.001, "Body sync preserves local Z scale");
    EXPECT_NEAR(rt_quat_w(local_rot),
                1.0,
                0.001,
                "Scaled parent sync keeps identity orientation normalized");
}

static void test_animator_root_motion_mode_consumes_delta_once() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *skel = rt_skeleton3d_new();
    void *controller;
    void *walk;
    void *pos;
    rt_scene3d_add(scene, node);

    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    walk = make_anim("walk", 0, 0.0, 0.0, 0.0, 6.0, 0.0, 0.0);
    controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), walk);
    rt_anim_controller3d_set_root_motion_bone(controller, 0);
    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_update(controller, 0.5);

    rt_scene_node3d_bind_animator(node, controller);
    rt_scene_node3d_set_sync_mode(node, RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION);
    rt_scene3d_sync_bindings(scene, 0.016);

    pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 3.0, 0.1, "Animator root motion moves the bound node");

    rt_scene3d_sync_bindings(scene, 0.016);
    pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(pos), 3.0, 0.1, "Animator root motion is consumed once per update");
}

static void test_animator_root_motion_applies_rotation_delta() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *skel = rt_skeleton3d_new();
    void *controller;
    void *turn;
    void *rot0 = rt_quat_new(0.0, 0.0, 0.0, 1.0);
    void *rot1 = rt_quat_from_axis_angle(rt_vec3_new(0.0, 1.0, 0.0), 1.5707963267948966);
    void *rot;
    double first_y;
    double first_w;
    rt_scene3d_add(scene, node);

    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    turn = make_anim_with_rotation(rot0, rot1);
    controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("turn"), turn);
    rt_anim_controller3d_set_root_motion_bone(controller, 0);
    rt_anim_controller3d_play(controller, rt_const_cstr("turn"));
    rt_anim_controller3d_update(controller, 1.0);

    rt_scene_node3d_bind_animator(node, controller);
    rt_scene_node3d_set_sync_mode(node, RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION);
    rt_scene3d_sync_bindings(scene, 0.016);

    rot = rt_scene_node3d_get_rotation(node);
    EXPECT_TRUE(std::fabs(rt_quat_y(rot)) > 0.5, "Animator root motion rotates the bound node");
    EXPECT_TRUE(std::fabs(rt_quat_w(rot)) < 0.9,
                "Animator root motion changes the node quaternion");
    first_y = rt_quat_y(rot);
    first_w = rt_quat_w(rot);

    rt_scene3d_sync_bindings(scene, 0.016);
    rot = rt_scene_node3d_get_rotation(node);
    EXPECT_NEAR(rt_quat_y(rot), first_y, 0.001, "Animator root-motion rotation is consumed once");
    EXPECT_NEAR(
        rt_quat_w(rot), first_w, 0.001, "Animator root-motion W stays stable after consumption");
}

static void test_scene_draw_uses_bound_animator_palette() {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *skel = rt_skeleton3d_new();
    void *controller;
    void *anim;
    int32_t palette_bones = 0;
    const float *palette;

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = test_begin_frame;
    backend.end_frame = test_end_frame;
    backend.submit_draw = test_submit_draw;
    init_test_canvas(&canvas, &backend);
    rt_camera3d_look_at(camera, eye, target, up);
    g_submit_count = 0;
    g_last_bone_palette = nullptr;
    g_last_bone_count = 0;

    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    anim = make_anim("walk", 0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), anim);
    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_update(controller, 0.25);
    palette = rt_anim_controller3d_get_final_palette_data(controller, &palette_bones);

    ((rt_mesh3d *)mesh)->bone_count = 1;
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_bind_animator(node, controller);
    rt_scene3d_add(scene, node);
    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_submit_count == 1, "SceneGraph.Draw submits one animated draw");
    EXPECT_TRUE(g_last_bone_palette == palette,
                "SceneGraph.Draw uses the bound AnimController3D palette");
    EXPECT_TRUE(g_last_bone_count == palette_bones,
                "SceneGraph.Draw forwards the animator bone count");
}

static void test_scene_draw_cpu_skins_bound_animators_for_software_backend() {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *skel = rt_skeleton3d_new();
    void *controller;
    void *anim;

    backend.name = "software";
    backend.begin_frame = test_begin_frame;
    backend.end_frame = test_end_frame;
    backend.submit_draw = test_submit_draw;
    init_test_canvas(&canvas, &backend);
    rt_camera3d_look_at(camera, eye, target, up);
    g_submit_count = 0;
    g_last_bone_palette = nullptr;
    g_last_bone_count = 0;
    g_last_vertices = nullptr;

    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    anim = make_anim("walk", 0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), anim);
    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_update(controller, 0.25);

    ((rt_mesh3d *)mesh)->bone_count = 1;
    rt_mesh3d_set_bone_weights(mesh, 0, 0, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_bind_animator(node, controller);
    rt_scene3d_add(scene, node);
    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_submit_count == 1,
                "SceneGraph.Draw still submits one draw on the software backend");
    EXPECT_TRUE(g_last_vertices != ((rt_mesh3d *)mesh)->vertices,
                "SceneGraph.Draw CPU-skins bound animators on the software backend");
    EXPECT_TRUE(g_last_bone_palette == nullptr && g_last_bone_count == 0,
                "SceneGraph.Draw clears GPU skinning payloads after CPU fallback");
}

static void test_scene_draw_cpu_skin_applies_attached_morph_before_skinning() {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *skel = rt_skeleton3d_new();
    void *controller;
    void *anim;
    void *morph;
    int64_t shape;
    float base_y;

    backend.name = "software";
    backend.begin_frame = test_begin_frame;
    backend.end_frame = test_end_frame;
    backend.submit_draw = test_submit_draw;
    init_test_canvas(&canvas, &backend);
    rt_camera3d_look_at(camera, eye, target, up);
    g_submit_count = 0;
    g_last_vertices = nullptr;

    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    anim = make_anim("walk", 0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), anim);
    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_update(controller, 0.25);

    ((rt_mesh3d *)mesh)->bone_count = 1;
    rt_mesh3d_set_bone_weights(mesh, 0, 0, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    base_y = ((rt_mesh3d *)mesh)->vertices[0].pos[1];

    morph = rt_morphtarget3d_new((int64_t)((rt_mesh3d *)mesh)->vertex_count);
    shape = rt_morphtarget3d_add_shape(morph, rt_const_cstr("fist"));
    rt_morphtarget3d_set_delta(morph, shape, 0, 0.0, 3.0, 0.0);
    rt_morphtarget3d_set_weight(morph, shape, 1.0);
    rt_mesh3d_set_morph_targets(mesh, morph);

    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_bind_animator(node, controller);
    rt_scene3d_add(scene, node);
    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_submit_count == 1, "morph+skin scene draw submits one draw");
    EXPECT_TRUE(g_last_vertices != nullptr, "morph+skin scene draw captures vertices");
    if (g_last_vertices) {
        /* Morph first (+3 y on vertex 0), then skin (bone at +0.25 x). */
        EXPECT_NEAR(g_last_vertices[0].pos[1],
                    base_y + 3.0f,
                    1e-4,
                    "CPU-skin fallback applies the attached morph delta before skinning");
        EXPECT_NEAR(g_last_vertices[0].pos[0],
                    ((rt_mesh3d *)mesh)->vertices[0].pos[0] + 0.25f,
                    1e-4,
                    "CPU-skin fallback still applies the bone palette");
    }
}

static void test_scene_draw_preserves_large_bound_animator_palettes_on_gpu_backends() {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(1.0, 1.0, 1.0);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *eye = rt_vec3_new(0.0, 0.0, 5.0);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *skel = rt_skeleton3d_new();
    void *controller;
    void *anim;

    backend.name = "opengl";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = test_begin_frame;
    backend.end_frame = test_end_frame;
    backend.submit_draw = test_submit_draw;
    init_test_canvas(&canvas, &backend);
    rt_camera3d_look_at(camera, eye, target, up);
    g_submit_count = 0;
    g_last_bone_palette = nullptr;
    g_last_bone_count = 0;

    for (int i = 0; i < 200; i++)
        rt_skeleton3d_add_bone(skel, rt_const_cstr("bone"), i - 1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    anim = make_anim("walk", 0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), anim);
    rt_anim_controller3d_play(controller, rt_const_cstr("walk"));
    rt_anim_controller3d_update(controller, 0.25);

    ((rt_mesh3d *)mesh)->bone_count = 200;
    rt_mesh3d_set_bone_weights(mesh, 0, 199, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    rt_scene_node3d_set_mesh(node, mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_bind_animator(node, controller);
    rt_scene3d_add(scene, node);
    rt_scene3d_draw(scene, &canvas, camera);

    EXPECT_TRUE(g_submit_count == 1, "SceneGraph.Draw still submits one large-rig animated draw");
    EXPECT_TRUE(g_last_bone_palette != nullptr && g_last_bone_count == 200,
                "SceneGraph.Draw preserves expanded animator palettes on GPU backends");
}

static void test_node_animator_rebind_clears_previous_node_owner() {
    void *node_a = rt_scene_node3d_new();
    void *node_b = rt_scene_node3d_new();
    void *clip = rt_node_animation3d_new(rt_const_cstr("Idle"), 1.0);
    void *clips[1] = {clip};
    auto *animator = static_cast<rt_node_animator3d *>(rt_node_animator3d_new_from_clips(clips, 1));

    rt_scene_node3d_bind_node_animator(node_a, animator);
    EXPECT_TRUE(animator->root == node_a, "NodeAnimator initially roots to the first node");
    EXPECT_TRUE(static_cast<rt_scene_node3d *>(node_a)->bound_node_animator == animator,
                "First node retains the bound NodeAnimator");

    rt_scene_node3d_bind_node_animator(node_b, animator);
    EXPECT_TRUE(animator->root == node_b, "NodeAnimator rebind moves root to the second node");
    EXPECT_TRUE(static_cast<rt_scene_node3d *>(node_a)->bound_node_animator == nullptr,
                "NodeAnimator rebind clears the previous node owner");
    EXPECT_TRUE(static_cast<rt_scene_node3d *>(node_b)->bound_node_animator == animator,
                "Second node retains the rebound NodeAnimator");
}

static void test_clear_animator_binding_clears_node_animator() {
    void *node = rt_scene_node3d_new();
    void *clip = rt_node_animation3d_new(rt_const_cstr("Idle"), 1.0);
    void *clips[1] = {clip};
    auto *animator = static_cast<rt_node_animator3d *>(rt_node_animator3d_new_from_clips(clips, 1));

    rt_scene_node3d_bind_node_animator(node, animator);
    EXPECT_TRUE(animator->root == node, "NodeAnimator roots to the node before clear");
    EXPECT_TRUE(static_cast<rt_scene_node3d *>(node)->bound_node_animator == animator,
                "Node retains NodeAnimator before clear");

    rt_scene_node3d_clear_animator_binding(node);
    EXPECT_TRUE(animator->root == nullptr, "ClearAnimatorBinding clears NodeAnimator root");
    EXPECT_TRUE(static_cast<rt_scene_node3d *>(node)->bound_node_animator == nullptr,
                "ClearAnimatorBinding releases the node animator slot");
}

/// Read a node's world position through the public Vec3 accessor.
static void socket_world_pos(void *node, double *x, double *y, double *z) {
    void *v = rt_scene_node3d_get_world_position(node);
    *x = v ? rt_vec3_x(v) : 0.0;
    *y = v ? rt_vec3_y(v) : 0.0;
    *z = v ? rt_vec3_z(v) : 0.0;
}

/// A node attached to an animated bone must track the bone's composited pose
/// (parentWorld x bonePose x offset) through SyncBindings, and stop tracking
/// after DetachBoneSocket.
static void test_bone_socket_follows_animated_bone() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_add_bone(skel, rt_const_cstr("hand"), 0, rt_mat4_identity());
    /* Bone 1 animates from (1,0,0) at t=0 to (1,2,0) at t=1. */
    void *anim = make_anim("wave", 1, 1.0, 0.0, 0.0, 1.0, 2.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("wave"), anim);
    rt_anim_controller3d_play(controller, rt_const_cstr("wave"));
    rt_anim_controller3d_update(controller, 0.0);

    void *scene = rt_scene3d_new();
    void *character = rt_scene_node3d_new();
    void *socket = rt_scene_node3d_new();
    double x;
    double y;
    double z;

    rt_scene3d_add(scene, character);
    rt_scene_node3d_set_position(character, 5.0, 0.0, 0.0);
    rt_scene_node3d_try_add_child(character, socket);

    /* Fail-before: without a socket the child stays at the parent origin. */
    rt_scene3d_sync_bindings(scene, 0.0);
    socket_world_pos(socket, &x, &y, &z);
    EXPECT_NEAR(x, 5.0, 1e-6, "unsocketed child inherits parent world position");
    EXPECT_NEAR(y, 0.0, 1e-6, "unsocketed child stays at parent origin (y)");

    /* Attach with a +0.5y offset in bone space: world = (5+1, 0+0.5, 0). */
    rt_scene_node3d_attach_to_bone(socket, controller, 1, 0.0, 0.5, 0.0);
    rt_scene3d_sync_bindings(scene, 0.0);
    socket_world_pos(socket, &x, &y, &z);
    EXPECT_NEAR(x, 6.0, 1e-5, "socket follows bone translation (x)");
    EXPECT_NEAR(y, 0.5, 1e-5, "socket applies bone-space offset (y)");
    EXPECT_NEAR(z, 0.0, 1e-5, "socket stays on the bone plane (z)");

    /* Advance the animation: bone rises to (1,1,0) at t=0.5. */
    rt_anim_controller3d_update(controller, 0.5);
    rt_scene3d_sync_bindings(scene, 0.0);
    socket_world_pos(socket, &x, &y, &z);
    EXPECT_NEAR(x, 6.0, 1e-5, "socket keeps tracking bone (x)");
    EXPECT_NEAR(y, 1.5, 1e-5, "socket tracks animated bone height (y)");

    /* Moving the character moves the socketed child with it. */
    rt_scene_node3d_set_position(character, 7.0, 0.0, 0.0);
    rt_scene3d_sync_bindings(scene, 0.0);
    socket_world_pos(socket, &x, &y, &z);
    EXPECT_NEAR(x, 8.0, 1e-5, "socket composes with the parent's world transform");

    /* Detach: the node keeps its last pose and stops following. */
    rt_scene_node3d_detach_bone_socket(socket);
    rt_anim_controller3d_update(controller, 0.4); /* bone now near (1,1.8,0) */
    rt_scene3d_sync_bindings(scene, 0.0);
    socket_world_pos(socket, &x, &y, &z);
    EXPECT_NEAR(y, 1.5, 1e-5, "detached socket no longer tracks the bone");
}

/// ADR 0274: a socket rotation offset composes bone-locally after the bone
/// pose, the socket position is unaffected, and a re-attach resets the
/// rotation to identity.
static void test_bone_socket_rotation_offset() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_add_bone(skel, rt_const_cstr("hand"), 0, rt_mat4_identity());
    /* Bone 1 holds a static pose at (1,0,0), identity orientation. */
    void *anim = make_anim("hold", 1, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("hold"), anim);
    rt_anim_controller3d_play(controller, rt_const_cstr("hold"));
    rt_anim_controller3d_update(controller, 0.0);

    void *scene = rt_scene3d_new();
    void *character = rt_scene_node3d_new();
    void *socket = rt_scene_node3d_new();
    double x;
    double y;
    double z;
    const double h = 0.70710678118654752;

    rt_scene3d_add(scene, character);
    rt_scene_node3d_set_position(character, 5.0, 0.0, 0.0);
    rt_scene_node3d_try_add_child(character, socket);

    rt_scene_node3d_attach_to_bone(socket, controller, 1, 0.0, 0.5, 0.0);
    rt_scene_node3d_set_bone_socket_rotation(socket, 0.0, 0.0, h, h);
    rt_scene3d_sync_bindings(scene, 0.0);

    socket_world_pos(socket, &x, &y, &z);
    EXPECT_NEAR(x, 6.0, 1e-5, "socket rotation leaves the offset position alone (x)");
    EXPECT_NEAR(y, 0.5, 1e-5, "socket rotation leaves the offset position alone (y)");

    void *rot = rt_scene_node3d_get_rotation(socket);
    EXPECT_TRUE(rot != nullptr, "socketed node exposes a rotation");
    EXPECT_NEAR(
        std::fabs(rt_quat_z(rot)), h, 1e-5, "socket rotation composes onto the bone pose (z)");
    EXPECT_NEAR(
        std::fabs(rt_quat_w(rot)), h, 1e-5, "socket rotation composes onto the bone pose (w)");

    /* Non-normalized input is normalized on store. */
    rt_scene_node3d_set_bone_socket_rotation(socket, 0.0, 0.0, 2.0, 2.0);
    rt_scene3d_sync_bindings(scene, 0.0);
    rot = rt_scene_node3d_get_rotation(socket);
    EXPECT_NEAR(std::fabs(rt_quat_z(rot)), h, 1e-5, "socket rotation input is normalized");

    /* Re-attaching resets the socket rotation to identity. */
    rt_scene_node3d_attach_to_bone(socket, controller, 1, 0.0, 0.5, 0.0);
    rt_scene3d_sync_bindings(scene, 0.0);
    rot = rt_scene_node3d_get_rotation(socket);
    EXPECT_NEAR(rt_quat_z(rot), 0.0, 1e-5, "re-attach resets the socket rotation (z)");
    EXPECT_NEAR(std::fabs(rt_quat_w(rot)), 1.0, 1e-5, "re-attach resets the socket rotation (w)");
}

/// ZB-15: the parent's world scale must apply to the model-space bone pose.
/// A centimetre-rigged model drawn under an entity-level unit scale otherwise
/// lands its socketed props ~30x away from the hand (world = T x R x S).
static void test_bone_socket_scales_with_parent() {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_add_bone(skel, rt_const_cstr("hand"), 0, rt_mat4_identity());
    /* Bone 1 holds a centimetre-scale pose: (100, 0, 0). */
    void *anim = make_anim("hold", 1, 100.0, 0.0, 0.0, 100.0, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("hold"), anim);
    rt_anim_controller3d_play(controller, rt_const_cstr("hold"));
    rt_anim_controller3d_update(controller, 0.0);

    void *scene = rt_scene3d_new();
    void *character = rt_scene_node3d_new();
    void *socket = rt_scene_node3d_new();
    double x;
    double y;
    double z;

    rt_scene3d_add(scene, character);
    rt_scene_node3d_set_position(character, 5.0, 0.0, 0.0);
    rt_scene_node3d_set_scale(character, 0.01, 0.01, 0.01);
    rt_scene_node3d_try_add_child(character, socket);

    /* Offset (0, 50, 0) in bone space: model = (100, 50, 0), scaled by the
     * parent's 0.01 -> (1.0, 0.5, 0), world = (6.0, 0.5, 0). */
    rt_scene_node3d_attach_to_bone(socket, controller, 1, 0.0, 50.0, 0.0);
    rt_scene3d_sync_bindings(scene, 0.0);
    socket_world_pos(socket, &x, &y, &z);
    EXPECT_NEAR(x, 6.0, 1e-5, "parent world scale applies to the bone pose (x)");
    EXPECT_NEAR(y, 0.5, 1e-5, "parent world scale applies to the socket offset (y)");
    EXPECT_NEAR(z, 0.0, 1e-5, "scaled socket stays on the bone plane (z)");
}

int main() {
    test_node_from_body_resolves_child_local_space();
    test_body_from_node_uses_world_space();
    test_two_way_kinematic_switches_direction();
    test_node_from_body_preserves_local_scale_under_scaled_parent();
    test_animator_root_motion_mode_consumes_delta_once();
    test_animator_root_motion_applies_rotation_delta();
    test_scene_draw_uses_bound_animator_palette();
    test_scene_draw_cpu_skins_bound_animators_for_software_backend();
    test_scene_draw_cpu_skin_applies_attached_morph_before_skinning();
    test_scene_draw_preserves_large_bound_animator_palettes_on_gpu_backends();
    test_node_animator_rebind_clears_previous_node_owner();
    test_clear_animator_binding_clears_node_animator();
    test_bone_socket_follows_animated_bone();
    test_bone_socket_rotation_offset();
    test_bone_socket_scales_with_parent();

    std::printf("SceneGraph binding tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
