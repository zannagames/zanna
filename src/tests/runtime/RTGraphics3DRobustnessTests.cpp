//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTGraphics3DRobustnessTests.cpp
// Purpose: Graphics3D correctness contracts for handle validation, numeric
//   sanitization, packing, and degenerate navigation cases.
// Key invariants:
//   - White-box view structs mirror private runtime layouts used by assertions.
//   - Wrong-class handles must be rejected without dropping existing valid refs.
// Ownership/Lifetime:
//   - Tests use the runtime object system and release only where ownership is
//     explicitly part of the contract under test.
// Links: src/runtime/graphics/3d/, src/runtime/graphics/2d/rt_pixels_internal.h
//
//===----------------------------------------------------------------------===//

// This assertion-driven test harness executes some state transitions inside
// assert expressions, so keep those expressions active in Release builds.
#ifdef NDEBUG
#undef NDEBUG
#endif

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

extern "C" {
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_collider3d.h"
#include "rt_decal3d.h"
#include "rt_graphics3d_ids.h"
#include "rt_instbatch3d.h"
#include "rt_joints3d.h"
#include "rt_lensflare3d.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_navmesh3d.h"
#include "rt_object.h"
#include "rt_particles3d.h"
#include "rt_path3d.h"
#include "rt_perlin.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_quat.h"
#include "rt_raycast3d.h"
#include "rt_reflectionprobe3d.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_skeleton3d.h"
#include "rt_sky3d.h"
#include "rt_sprite3d.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "rt_texatlas3d.h"
#include "rt_timeofday3d.h"
#include "rt_transform3d.h"
#include "rt_vec3.h"
#include "rt_vegetation3d.h"
#include "rt_water3d.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_utils.h"
}

#include <cassert>
#include <clocale>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

extern "C" {
void *rt_mesh3d_new(void);
void rt_mesh3d_add_vertex(
    void *m, double x, double y, double z, double nx, double ny, double nz, double u, double v);
void rt_mesh3d_add_triangle(void *m, int64_t v0, int64_t v1, int64_t v2);
int64_t rt_mesh3d_get_vertex_count(void *m);
int64_t rt_mesh3d_get_triangle_count(void *m);
void rt_mesh3d_transform(void *m, void *mat4);
void rt_mesh3d_calc_tangents(void *m);
void *rt_material3d_new(void);
void rt_material3d_set_unlit(void *obj, int8_t unlit);
void *rt_light3d_new_directional(void *direction, double r, double g, double b);
void *rt_light3d_new_point(void *position, double r, double g, double b, double attenuation);
void *rt_light3d_new_ambient(double r, double g, double b);
void rt_material3d_set_import_texture_slot(void *obj,
                                           int64_t slot,
                                           int64_t uv_set,
                                           double offset_u,
                                           double offset_v,
                                           double scale_u,
                                           double scale_v,
                                           double rotation,
                                           int64_t wrap_s,
                                           int64_t wrap_t,
                                           int64_t filter);
double rt_terrain3d_get_height_at(void *obj, double wx, double wz);
int8_t rt_navmesh3d_is_walkable(void *obj, void *point);
void rt_navmesh3d_set_max_slope(void *obj, double degrees);
}

namespace {

static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;

template <typename Fn> bool expect_trap_contains(Fn &&fn, const char *needle) {
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

} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

namespace {

struct SpriteView {
    void *vptr;
    void *texture;
    double position[3];
    double scale_wh[2];
    double anchor[2];
    int32_t frame_x;
    int32_t frame_y;
    int32_t frame_w;
    int32_t frame_h;
    int32_t tex_w;
    int32_t tex_h;
    int8_t additive; /* additive blend routing */
    double tint[3];
    void *cached_mesh;
    void *cached_material;
    void *cached_texture;
};

struct DecalView {
    void *vptr;
    double position[3];
    double normal[3];
    double size;
    void *texture;
    double lifetime;
    double max_lifetime;
    double alpha;
    double depth_bias; /* constant depth bias override; 0 = auto (size-scaled) */
    void *mesh;
    void *material;
    void *material_texture;
};

using MaterialView = rt_material3d;

struct ParticleView {
    void *vptr;
    void *particles;
    int32_t count;
    int32_t max_particles;
    double position[3];
    double emit_dir[3];
    double emit_spread;
    double speed_min;
    double speed_max;
    double life_min;
    double life_max;
    double size_start;
    double size_end;
    double gravity[3];
    float color_start[3];
    float color_end[3];
    double alpha_start;
    double alpha_end;
    double rate;
    double accumulator;
    int8_t emitting;
    int8_t additive_blend;
    double stretch_k;     /* velocity-aligned stretch factor */
    float trail_lifetime; /* ribbon trail history seconds */
    int32_t trail_segments;
    float *trail_pos;
    float *trail_age;
    int16_t *trail_len;
    int16_t *trail_head;
    double softness; /* Plan 10: soft-particle fade distance */
    void *texture;
    int32_t emitter_shape;
    double emitter_size[3];
    uint32_t prng_state;
    void *cached_material;
    void *draw_vertices[4];
    uint32_t *draw_indices[4];
    uint32_t draw_vertex_capacity[4];
    uint32_t draw_index_capacity[4];
    void *draw_materials[4];
    void *sort_keys;
    int32_t sort_key_capacity;
    uint64_t sort_key_grow_count;
    int64_t draw_frame_serial;
    int32_t draw_slots_used;
};

struct WaterView {
    void *vptr;
    double width;
    double depth;
    double height;
    double center_x;
    double center_z;
    double wave_speed;
    double wave_amplitude;
    double wave_frequency;
    double color[3];
    double alpha;
    double time;
    void *mesh;
    void *material;
    void *texture;
    void *normal_map;
    void *env_map;
    double reflectivity;
    double waves[8][5];
    int32_t wave_count;
    int32_t resolution;
    int8_t mesh_dirty;
};

struct VegetationView {
    void *vptr;
    void *blade_mesh;
    void *blade_material;
    double blade_width;
    double blade_height;
    double size_variation;
    float *base_transforms;
    float *positions;
    int32_t total_count;
    int32_t capacity;
    void *density_map;
    double wind_speed;
    double wind_strength;
    double wind_turbulence;
    double time;
    float lod_near;
    float lod_far;
    float *visible_transforms;
    int32_t visible_count;
    int32_t visible_capacity;
};

struct InstBatchView {
    void *vptr;
    void *mesh;
    void *material;
    float *transforms;
    float *current_snapshot;
    float *prev_transforms;
    int32_t instance_count;
    int32_t instance_capacity;
    int32_t motion_snapshot_count;
    int32_t prev_count;
    int64_t last_motion_frame;
    int8_t has_prev_snapshot;
    double *transforms64;
    float *visible_transforms;
    float *visible_prev_transforms;
    int32_t visible_capacity;
    int32_t visible_prev_capacity;
    float *prev_submit_transforms;
    int32_t prev_submit_capacity;
    int32_t allocation_capacity;
    double *current_snapshot64;
    double *prev_transforms64;
    uint8_t *visibility_mask;
    int32_t visibility_mask_capacity;
    int8_t motion_frame_initialized;
    float *owned_transforms;
    float *owned_current_snapshot;
    float *owned_prev_transforms;
    double *owned_transforms64;
    double *owned_current_snapshot64;
    double *owned_prev_transforms64;
};

struct TransformView {
    void *vptr;
    double position[3];
    double rotation[4];
    double scale[3];
    double matrix[16];
    int8_t dirty;
    int8_t components_clean;
};

struct PathView {
    void *vptr;
    double *xs;
    double *ys;
    double *zs;
    int32_t point_count;
    int32_t point_capacity;
    int8_t looping;
    double cached_length;
    int8_t length_dirty;
    double *spline_cumulative;
    int32_t spline_sample_count;
    int8_t spline_dirty;
    int32_t point_allocation_capacity;
    int32_t spline_capacity;
    double *owned_xs;
    double *owned_ys;
    double *owned_zs;
    double *owned_spline_cumulative;
};

struct LensFlareElementView {
    float axis_offset;
    float size;
    void *ghost;
};

struct LensFlareView {
    void *vptr;
    void *light;
    LensFlareElementView elements[16];
    int32_t element_count;
    float smoothed_visibility;
    uint64_t smoothed_frame_serial;
};

static void release_retained_probe(void *probe);

struct AtlasRegionView {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

struct AtlasView {
    void *vptr;
    uint32_t *data;
    uint32_t *allocated_data;
    size_t data_pixel_count;
    int32_t allocated_width;
    int32_t allocated_height;
    int32_t width;
    int32_t height;
    AtlasRegionView regions[256];
    int32_t region_count;
    int32_t shelf_x;
    int32_t shelf_y;
    int32_t shelf_h;
    void *cached_pixels;
    int8_t dirty;
};

struct SkyView {
    void *vptr;
    double sun_dir[3];
    double turbidity;
    double ground_albedo[3];
    int64_t resolution;
    int8_t dirty;
    int8_t stars_enabled;  /* ADR 0257 */
    double star_intensity; /* ADR 0257 */
    void *cubemap;
};

struct TimeOfDayView {
    void *vptr;
    double hours;
    double day_length_seconds;
    double latitude_degrees;
    double refresh_degrees;
    void *sun_light;
    void *sky;
    void *probe;
    double last_refresh_dir[3];
    int8_t has_refresh_dir;
};

struct ReflectionProbeView {
    void *vptr;
    double position[3];
    double box_min[3];
    double box_max[3];
    double influence_scale;
    int64_t resolution;
    int8_t capture_dirty;
    void *cubemap;
};

static double triangle_area_sq(const rt_mesh3d *mesh, uint32_t i0, uint32_t i1, uint32_t i2) {
    const float *a = mesh->vertices[i0].pos;
    const float *b = mesh->vertices[i1].pos;
    const float *c = mesh->vertices[i2].pos;
    double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    double ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double cross[3] = {
        ab[1] * ac[2] - ab[2] * ac[1],
        ab[2] * ac[0] - ab[0] * ac[2],
        ab[0] * ac[1] - ab[1] * ac[0],
    };
    return cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2];
}

static void test_graphics3d_class_ids_are_stable() {
    void *atlas = rt_texatlas3d_new(16, 16);
    void *path = rt_path3d_new();
    void *terrain = rt_terrain3d_new(2, 2);
    void *water = rt_water3d_new(1.0, 1.0);
    void *sprite = rt_sprite3d_new(nullptr);
    void *mat = rt_mat4_identity();

    assert(rt_obj_class_id(atlas) == RT_G3D_TEXTUREATLAS3D_CLASS_ID);
    assert(rt_obj_class_id(path) == RT_G3D_PATH3D_CLASS_ID);
    assert(rt_obj_class_id(terrain) == RT_G3D_TERRAIN3D_CLASS_ID);
    assert(rt_obj_class_id(water) == RT_G3D_WATER3D_CLASS_ID);
    assert(rt_obj_class_id(sprite) == RT_G3D_SPRITE3D_CLASS_ID);
    assert(rt_obj_class_id(mat) == RT_MAT4_CLASS_ID);
}

static void test_texture_atlas_copies_pixels_and_reports_uvs() {
    void *atlas = rt_texatlas3d_new(16, 16);
    void *pixels = rt_pixels_new(2, 2);
    rt_pixels_set(pixels, 0, 0, 0x11223344);
    rt_pixels_set(pixels, 1, 0, 0x55667788);
    rt_pixels_set(pixels, 0, 1, (int64_t)0x99AABBCC);
    rt_pixels_set(pixels, 1, 1, (int64_t)0xDDEEFF00);

    int64_t id = rt_texatlas3d_add(atlas, pixels);
    assert(id == 0);

    double u0 = 0.0, v0 = 0.0, u1 = 0.0, v1 = 0.0;
    rt_texatlas3d_get_uv_rect(atlas, id, &u0, &v0, &u1, &v1);
    assert(std::fabs(u0 - (1.0 / 16.0)) < 1e-12);
    assert(std::fabs(v0 - (1.0 / 16.0)) < 1e-12);
    assert(std::fabs(u1 - (3.0 / 16.0)) < 1e-12);
    assert(std::fabs(v1 - (3.0 / 16.0)) < 1e-12);

    void *texture = rt_texatlas3d_get_texture(atlas);
    assert(texture != nullptr);
    assert(rt_pixels_get(texture, 1, 1) == 0x11223344);
    assert(rt_pixels_get(texture, 2, 1) == 0x55667788);
    assert(rt_pixels_get(texture, 1, 2) == (int64_t)0x99AABBCC);
    assert(rt_pixels_get(texture, 2, 2) == (int64_t)0xDDEEFF00);
    assert(rt_pixels_get(texture, 1, 0) == 0x11223344);
    assert(rt_pixels_get(texture, 2, 0) == 0x55667788);
    assert(rt_pixels_get(texture, 0, 1) == 0x11223344);
    assert(rt_pixels_get(texture, 3, 1) == 0x55667788);
    assert(rt_pixels_get(texture, 0, 2) == (int64_t)0x99AABBCC);
    assert(rt_pixels_get(texture, 3, 2) == (int64_t)0xDDEEFF00);
    assert(rt_pixels_get(texture, 1, 3) == (int64_t)0x99AABBCC);
    assert(rt_pixels_get(texture, 2, 3) == (int64_t)0xDDEEFF00);
}

static void test_material_rejects_non_pixels_texture_handles() {
    void *fake = rt_obj_new_i64(0, 8);
    void *undersized_pixels = rt_obj_new_i64(RT_PIXELS_CLASS_ID, 8);
    void *pixels = rt_pixels_new(1, 1);
    assert(expect_trap_contains([&] { rt_material3d_new_textured(fake); }, "Pixels"));
    void *mat_obj = rt_material3d_new_textured(pixels);
    auto *mat = static_cast<MaterialView *>(mat_obj);
    assert(mat->texture == pixels);

    assert(expect_trap_contains([&] { rt_material3d_set_texture(mat_obj, fake); }, "Pixels"));
    assert(expect_trap_contains([&] { rt_material3d_set_texture(mat_obj, undersized_pixels); },
                                "Pixels"));
    assert(mat->texture == pixels);
}

static void test_mesh_apis_reject_wrong_class_handles() {
    void *fake = rt_obj_new_i64(0, 8);
    void *mesh = rt_mesh3d_new();
    void *bad_mat4 = rt_obj_new_i64(0, 8);
    void *undersized_mat4 = rt_obj_new_i64(RT_MAT4_CLASS_ID, 8);

    rt_mesh3d_add_vertex(fake, 1.0, 2.0, 3.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    assert(rt_mesh3d_get_vertex_count(fake) == 0);

    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    assert(rt_mesh3d_get_vertex_count(mesh) == 1);
    rt_mesh3d_transform(mesh, bad_mat4);
    rt_mesh3d_transform(mesh, undersized_mat4);
    assert(rt_mesh3d_get_vertex_count(mesh) == 1);
}

static void test_camera_center_ray_and_projection_layout() {
    void *camera = rt_camera3d_new(90.0, 1.0, 0.1, 100.0);
    void *center_ray = rt_camera3d_screen_to_ray(camera, 50, 50, 100, 100);
    assert(std::fabs(rt_vec3_x(center_ray)) < 1e-6);
    assert(std::fabs(rt_vec3_y(center_ray)) < 1e-6);
    assert(std::fabs(rt_vec3_z(center_ray) + 1.0) < 1e-6);

    float projection[16] = {};
    rt_camera3d_get_render_projection(camera, 1.0, projection);
    assert(std::isfinite(projection[10]));
    assert(std::isfinite(projection[11]));
    assert(projection[14] == -1.0f);
}

static void test_camera_sanitizes_extreme_projection_and_basis_inputs() {
    void *camera = rt_camera3d_new(90.0, 1.0e300, 1.0e-300, 1.0e300);
    void *target = rt_vec3_new(0.0, 0.0, 0.0);
    rt_camera3d_orbit(camera, target, 1.0e300, 35.0, 20.0);

    float projection[16] = {};
    rt_camera3d_get_render_projection(camera, 1.0e300, projection);
    for (float value : projection)
        assert(std::isfinite(value));

    void *forward = rt_camera3d_get_forward(camera);
    void *right = rt_camera3d_get_right(camera);
    double flen = std::sqrt(rt_vec3_x(forward) * rt_vec3_x(forward) +
                            rt_vec3_y(forward) * rt_vec3_y(forward) +
                            rt_vec3_z(forward) * rt_vec3_z(forward));
    double rlen =
        std::sqrt(rt_vec3_x(right) * rt_vec3_x(right) + rt_vec3_y(right) * rt_vec3_y(right) +
                  rt_vec3_z(right) * rt_vec3_z(right));
    assert(std::isfinite(flen) && std::fabs(flen - 1.0) < 1e-6);
    assert(std::isfinite(rlen) && std::fabs(rlen - 1.0) < 1e-6);
}

static void test_camera_smooth_look_at_syncs_fps_angles() {
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *target = rt_vec3_new(10.0, 0.0, 0.0);

    rt_camera3d_smooth_look_at(camera, target, 100.0, 1.0);

    double yaw = rt_camera3d_get_yaw(camera);
    assert(std::isfinite(yaw));
    assert(yaw > 80.0 && yaw < 100.0);
}

static void test_generated_sphere_has_no_degenerate_triangles() {
    auto *mesh = static_cast<rt_mesh3d *>(rt_mesh3d_new_sphere(1.0, 8));
    assert(mesh != nullptr);
    assert(rt_mesh3d_get_triangle_count(mesh) > 0);
    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        assert(triangle_area_sq(
                   mesh, mesh->indices[i], mesh->indices[i + 1], mesh->indices[i + 2]) > 1e-12);
    }
}

static void test_generated_plane_faces_positive_y() {
    auto *mesh = static_cast<rt_mesh3d *>(rt_mesh3d_new_plane(2.0, 2.0));
    assert(mesh != nullptr);
    assert(mesh->index_count == 6);

    const uint32_t i0 = mesh->indices[0];
    const uint32_t i1 = mesh->indices[1];
    const uint32_t i2 = mesh->indices[2];
    const float *a = mesh->vertices[i0].pos;
    const float *b = mesh->vertices[i1].pos;
    const float *c = mesh->vertices[i2].pos;
    const double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const double ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const double normal_y = ab[2] * ac[0] - ab[0] * ac[2];
    assert(normal_y > 0.0);
}

static void test_obj_loader_handles_long_lines() {
    const std::string path = "/tmp/zanna_rt_graphics3d_long_line.obj";
    {
        std::ofstream out(path);
        out << '#';
        out << std::string(6000, 'x') << "\n";
        out << "v 0 0 0\n";
        out << "v 1 0 0\n";
        out << "v 0 1 0\n";
        out << "f 1 2 3\n";
    }

    rt_string path_string = rt_string_from_bytes(path.c_str(), static_cast<int64_t>(path.size()));
    void *mesh = rt_mesh3d_from_obj(path_string);
    assert(mesh != nullptr);
    assert(rt_mesh3d_get_triangle_count(mesh) == 1);
    std::remove(path.c_str());
}

static void test_obj_loader_uses_dot_decimal_independent_of_locale() {
    const char *saved_locale = std::setlocale(LC_NUMERIC, nullptr);
    std::string restore_locale = saved_locale ? saved_locale : "C";
    const char *changed_locale = std::setlocale(LC_NUMERIC, "fr_FR.UTF-8");
    if (!changed_locale)
        changed_locale = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");

    const std::string path = "/tmp/zanna_rt_graphics3d_locale_decimal.obj";
    {
        std::ofstream out(path);
        out << "v 1.25 0.5 1e-1\n";
        out << "v 2.25 0.5 1e-1\n";
        out << "v 1.25 1.5 1e-1\n";
        out << "f 1 2 3\n";
    }

    rt_string path_string = rt_string_from_bytes(path.c_str(), static_cast<int64_t>(path.size()));
    auto *mesh = static_cast<rt_mesh3d *>(rt_mesh3d_from_obj(path_string));
    assert(mesh != nullptr);
    assert(mesh->vertex_count == 3);
    assert(std::fabs(mesh->vertices[0].pos[0] - 1.25f) < 1e-6f);
    assert(std::fabs(mesh->vertices[0].pos[1] - 0.5f) < 1e-6f);
    assert(std::fabs(mesh->vertices[0].pos[2] - 0.1f) < 1e-6f);

    std::remove(path.c_str());
    if (changed_locale)
        std::setlocale(LC_NUMERIC, restore_locale.c_str());
}

static void test_scene_particles_water_and_render_targets_reject_wrong_handles() {
    void *fake = rt_obj_new_i64(0, 8);
    void *node = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    rt_scene_node3d_add_lod(node, 4.0, fake);
    assert(rt_scene_node3d_get_lod_count(node) == 0);
    rt_scene_node3d_add_lod(node, 4.0, mesh);
    assert(rt_scene_node3d_get_lod_count(node) == 1);
    assert(rt_scene_node3d_get_lod_mesh(node, 0) == mesh);

    void *particles_obj = rt_particles3d_new(4);
    auto *particles = static_cast<ParticleView *>(particles_obj);
    void *pixels = rt_pixels_new(1, 1);
    void *undersized_pixels = rt_obj_new_i64(RT_PIXELS_CLASS_ID, 8);
    rt_particles3d_set_texture(particles_obj, pixels);
    assert(particles->texture == pixels);
    rt_particles3d_set_texture(particles_obj, fake);
    rt_particles3d_set_texture(particles_obj, undersized_pixels);
    assert(particles->texture == pixels);

    void *water_obj = rt_water3d_new(2.0, 2.0);
    auto *water = static_cast<WaterView *>(water_obj);
    rt_water3d_set_texture(water_obj, pixels);
    rt_water3d_set_normal_map(water_obj, pixels);
    assert(water->texture == pixels);
    assert(water->normal_map == pixels);
    rt_water3d_set_texture(water_obj, fake);
    rt_water3d_set_normal_map(water_obj, fake);
    rt_water3d_set_texture(water_obj, undersized_pixels);
    rt_water3d_set_normal_map(water_obj, undersized_pixels);
    assert(water->texture == pixels);
    assert(water->normal_map == pixels);

    void *cubemap = rt_cubemap3d_new(pixels, pixels, pixels, pixels, pixels, pixels);
    rt_water3d_set_env_map(water_obj, cubemap);
    assert(water->env_map == cubemap);
    rt_water3d_set_env_map(water_obj, fake);
    assert(water->env_map == cubemap);
    void *incomplete_pixels = rt_pixels_new(1, 1);
    void *incomplete_cubemap = rt_cubemap3d_new(incomplete_pixels,
                                                incomplete_pixels,
                                                incomplete_pixels,
                                                incomplete_pixels,
                                                incomplete_pixels,
                                                incomplete_pixels);
    static_cast<rt_pixels_impl *>(incomplete_pixels)->width = 2;
    rt_water3d_set_env_map(water_obj, incomplete_cubemap);
    assert(water->env_map == cubemap);
    water->env_map = incomplete_cubemap;
    rt_water3d_update(water_obj, 0.0);
    assert(water->env_map == cubemap);

    assert(rt_rendertarget3d_get_width(fake) == 0);
    assert(rt_rendertarget3d_get_height(fake) == 0);
    assert(rt_rendertarget3d_as_pixels(fake) == nullptr);
}

static void test_cubemap_sampling_sanitizes_inputs() {
    void *px = rt_pixels_new(1, 1);
    void *face = rt_pixels_new(1, 1);
    rt_pixels_set(px, 0, 0, (int64_t)0xFF0000FF);
    rt_pixels_set(face, 0, 0, (int64_t)0x000000FF);
    auto *cubemap = static_cast<rt_cubemap3d *>(rt_cubemap3d_new(px, face, face, face, face, face));
    assert(cubemap != nullptr);

    float out[3] = {1.0f, 1.0f, 1.0f};
    rt_cubemap_sample(
        cubemap, std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, &out[0], &out[1], &out[2]);
    assert(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);

    rt_cubemap_sample_roughness(cubemap,
                                1.0f,
                                0.0f,
                                0.0f,
                                std::numeric_limits<float>::quiet_NaN(),
                                &out[0],
                                &out[1],
                                &out[2]);
    assert(std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]));
    assert(out[0] > 0.9f);

    rt_cubemap_sample(
        cubemap, std::numeric_limits<float>::max(), 0.0f, 0.0f, &out[0], &out[1], &out[2]);
    assert(out[0] > 0.9f && out[1] < 0.1f && out[2] < 0.1f);

    rt_cubemap_sample_roughness(
        cubemap, std::numeric_limits<float>::max(), 0.0f, 0.0f, 0.6f, &out[0], &out[1], &out[2]);
    assert(std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]));
    assert(out[0] > 0.1f);

    void *fake = rt_obj_new_i64(0, 8);
    cubemap->faces[1] = fake;
    out[0] = out[1] = out[2] = 1.0f;
    rt_cubemap_sample(cubemap, 1.0f, 0.0f, 0.0f, &out[0], &out[1], &out[2]);
    assert(out[0] > 0.9f && out[1] < 0.1f && out[2] < 0.1f);
    assert(cubemap->faces[1] == cubemap->owned_faces[1]);

    cubemap->faces[0] = fake;
    rt_cubemap_sample(cubemap, 1.0f, 0.0f, 0.0f, &out[0], &out[1], &out[2]);
    assert(out[0] > 0.9f && out[1] < 0.1f && out[2] < 0.1f);
    assert(cubemap->faces[0] == cubemap->owned_faces[0]);

    auto *fake_cubemap = static_cast<rt_cubemap3d *>(rt_obj_new_i64(0, sizeof(rt_cubemap3d)));
    out[0] = out[1] = out[2] = 1.0f;
    rt_cubemap_sample(fake_cubemap, 1.0f, 0.0f, 0.0f, &out[0], &out[1], &out[2]);
    assert(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);

    rt_canvas3d canvas{};
    canvas.skybox = fake_cubemap;
    assert(canvas3d_ensure_skybox_cpu_cache(&canvas, 1, 1) == 0);
}

static void test_physics_checked_handles_and_trigger_removal_exit() {
    void *fake = rt_obj_new_i64(0, 8);
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *body = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(body, 0.0, 0.0, 0.0);

    rt_world3d_add(fake, body);
    assert(rt_world3d_body_count(fake) == 0);
    rt_world3d_add(world, body);
    assert(rt_world3d_body_count(world) == 1);

    void *bad_pos = rt_body3d_get_position(fake);
    assert(rt_vec3_x(bad_pos) == 0.0 && rt_vec3_y(bad_pos) == 0.0 && rt_vec3_z(bad_pos) == 0.0);
    assert(rt_physics_hit3d_get_body(fake) == nullptr);
    assert(rt_physics_hit_list3d_get_count(fake) == 0);
    assert(rt_collision_event3d_get_body_a(fake) == nullptr);

    void *trigger = rt_trigger3d_new(-1.0, -1.0, -1.0, 1.0, 1.0, 1.0);
    rt_trigger3d_update(trigger, world);
    assert(rt_trigger3d_get_enter_count(trigger) == 1);
    assert(rt_trigger3d_get_exit_count(trigger) == 0);

    rt_world3d_remove(world, body);
    rt_trigger3d_update(trigger, world);
    assert(rt_trigger3d_get_enter_count(trigger) == 0);
    assert(rt_trigger3d_get_exit_count(trigger) == 1);
}

static void test_material_import_texture_transform_clamps_float_uniforms() {
    auto *mat = static_cast<MaterialView *>(rt_material3d_new());
    assert(mat != nullptr);

    rt_material3d_set_import_texture_slot(
        mat, 0, 0, 1.0e300, -1.0e300, 1.0e300, -1.0e300, 1.0e300, 0, 0, 0);

    for (int i = 0; i < 6; i++) {
        double value = mat->texture_slot_uv_transform[0][i];
        assert(std::isfinite(value));
        assert(std::fabs(value) <= 1000000.0);
    }
}

static void test_material_scalar_setters_clamp_renderer_uniforms() {
    auto *mat = static_cast<MaterialView *>(rt_material3d_new());
    assert(mat != nullptr);

    rt_material3d_set_shininess(mat, 1.0e300);
    rt_material3d_set_emissive_intensity(mat, 1.0e300);
    rt_material3d_set_normal_scale(mat, -1.0e300);
    rt_material3d_set_custom_param(mat, 3, -1.0e300);

    assert(std::isfinite(mat->shininess) && mat->shininess <= 8192.0);
    assert(std::isfinite(mat->emissive_intensity) && mat->emissive_intensity <= 1000000.0);
    assert(std::isfinite(mat->normal_scale) && mat->normal_scale >= -1000.0);
    assert(std::isfinite(mat->custom_params[3]) && mat->custom_params[3] >= -1000000.0);
}

static void test_terrain_heightmap_and_scale_sanitize_inputs() {
    void *terrain = rt_terrain3d_new(2, 2);
    void *heightmap = rt_pixels_new(2, 2);
    rt_pixels_set(heightmap, 0, 0, 0x000000FF);
    rt_pixels_set(heightmap, 1, 0, 0x000000FF);
    rt_pixels_set(heightmap, 0, 1, 0x000000FF);
    rt_pixels_set(heightmap, 1, 1, (int64_t)0xFFFFFFFF);

    rt_terrain3d_set_heightmap(terrain, heightmap);
    rt_terrain3d_set_scale(terrain,
                           -4.0,
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity());
    assert(std::fabs(rt_terrain3d_get_height_at(terrain, 1.0, 1.0) - 1.0) < 1e-6);

    assert(std::isfinite(rt_terrain3d_get_height_at(terrain, 1.0e300, -1.0e300)));
    void *normal = rt_terrain3d_get_normal_at(terrain, 1.0e300, -1.0e300);
    double nlen =
        std::sqrt(rt_vec3_x(normal) * rt_vec3_x(normal) + rt_vec3_y(normal) * rt_vec3_y(normal) +
                  rt_vec3_z(normal) * rt_vec3_z(normal));
    assert(std::isfinite(nlen) && nlen > 0.9 && nlen < 1.1);
}

static void test_sprite3d_clamps_frame_anchor_and_scale() {
    void *texture = rt_pixels_new(4, 4);
    void *sprite = rt_sprite3d_new(texture);
    auto *s = static_cast<SpriteView *>(sprite);

    rt_sprite3d_set_scale(sprite, -1.0, std::numeric_limits<double>::quiet_NaN());
    rt_sprite3d_set_anchor(sprite, -2.0, 4.0);
    rt_sprite3d_set_frame(sprite, -5, 100, 999, 999);

    assert(s->scale_wh[0] == 1.0);
    assert(s->scale_wh[1] == 1.0);
    assert(s->anchor[0] == 0.0);
    assert(s->anchor[1] == 1.0);
    assert(s->frame_x == 0);
    assert(s->frame_y == 3);
    assert(s->frame_w == 4);
    assert(s->frame_h == 1);
}

static void test_decal3d_normal_and_lifetime_are_sanitized() {
    void *pos = rt_vec3_new(std::numeric_limits<double>::quiet_NaN(), 2.0, 3.0);
    void *normal = rt_vec3_new(0.0, 0.0, 0.0);
    void *decal = rt_decal3d_new(pos, normal, -8.0, nullptr);
    auto *d = static_cast<DecalView *>(decal);

    assert(d->position[0] == 0.0);
    assert(d->position[1] == 2.0);
    assert(d->position[2] == 3.0);
    assert(d->normal[0] == 0.0);
    assert(d->normal[1] == 1.0);
    assert(d->normal[2] == 0.0);
    assert(d->size == 1.0);

    rt_decal3d_set_lifetime(decal, std::numeric_limits<double>::quiet_NaN());
    rt_decal3d_update(decal, 1.0);
    assert(rt_decal3d_is_expired(decal) == 0);

    // A positive lifetime counts down across updates and expires once it elapses.
    rt_decal3d_set_lifetime(decal, 0.5);
    assert(rt_decal3d_is_expired(decal) == 0);
    rt_decal3d_update(decal, 0.3);
    assert(rt_decal3d_is_expired(decal) == 0);
    rt_decal3d_update(decal, 0.3); // cumulative 0.6 > 0.5 lifetime
    assert(rt_decal3d_is_expired(decal) == 1);

    void *huge_normal = rt_vec3_new(1.0e300, 0.0, 0.0);
    void *huge_decal = rt_decal3d_new(pos, huge_normal, 1.0e300, nullptr);
    auto *hd = static_cast<DecalView *>(huge_decal);
    assert(std::isfinite(hd->size) && hd->size <= 1000000.0);
    assert(hd->normal[0] == 1.0);
    assert(hd->normal[1] == 0.0);
    assert(hd->normal[2] == 0.0);

    void *fake_texture = rt_obj_new_i64(0, 8);
    void *invalid_texture_decal = rt_decal3d_new(pos, normal, 1.0, fake_texture);
    auto *itd = static_cast<DecalView *>(invalid_texture_decal);
    assert(itd->texture == nullptr);

    rt_canvas3d canvas = {};
    hd->mesh = rt_obj_new_i64(0, 8);
    hd->material = rt_obj_new_i64(0, 8);
    hd->texture = rt_obj_new_i64(0, 8);
    rt_canvas3d_draw_decal(&canvas, huge_decal);
    assert(hd->texture == nullptr);
    assert(hd->mesh != nullptr && rt_obj_class_id(hd->mesh) == RT_G3D_MESH3D_CLASS_ID);
    assert(hd->material != nullptr && rt_obj_class_id(hd->material) == RT_G3D_MATERIAL3D_CLASS_ID);
    auto *mesh = static_cast<rt_mesh3d *>(hd->mesh);
    assert(mesh->vertex_count == 4);
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        assert(std::isfinite(mesh->vertices[i].pos[0]));
        assert(std::isfinite(mesh->vertices[i].pos[1]));
        assert(std::isfinite(mesh->vertices[i].pos[2]));
        assert(std::fabs(mesh->vertices[i].pos[0]) <= 1000000000000.0);
        assert(std::fabs(mesh->vertices[i].pos[1]) <= 1000000000000.0);
        assert(std::fabs(mesh->vertices[i].pos[2]) <= 1000000000000.0);
    }
}

static void test_path3d_growth_preserves_points() {
    void *path = rt_path3d_new();
    for (int i = 0; i < 40; i++) {
        void *point = rt_vec3_new((double)i, 0.0, 0.0);
        rt_path3d_add_point(path, point);
    }

    assert(rt_path3d_get_point_count(path) == 40);
    void *mid = rt_path3d_get_position_at(path, std::numeric_limits<double>::quiet_NaN());
    assert(rt_vec3_x(mid) == 0.0);
    assert(rt_path3d_get_length(path) > 38.0);
}

static void test_path3d_looping_includes_closing_segment() {
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_vec3_new(0.0, 0.0, 0.0));
    rt_path3d_add_point(path, rt_vec3_new(10.0, 0.0, 0.0));
    rt_path3d_add_point(path, rt_vec3_new(10.0, 0.0, 10.0));

    double open_length = rt_path3d_get_length(path);
    rt_path3d_set_looping(path, 1);
    double loop_length = rt_path3d_get_length(path);
    assert(loop_length > open_length + 5.0);

    void *near_start = rt_path3d_get_position_at(path, 0.999);
    double dist = std::sqrt(rt_vec3_x(near_start) * rt_vec3_x(near_start) +
                            rt_vec3_y(near_start) * rt_vec3_y(near_start) +
                            rt_vec3_z(near_start) * rt_vec3_z(near_start));
    assert(dist < 0.5);
}

static void test_path3d_extreme_points_keep_length_and_direction_finite() {
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_vec3_new(0.0, 0.0, 0.0));
    rt_path3d_add_point(path, rt_vec3_new(1.0e308, 0.0, 0.0));
    rt_path3d_add_point(path, rt_vec3_new(-1.0e308, 1.0e308, 0.0));

    void *dir = rt_path3d_get_direction_at(path, 0.5);
    assert(std::isfinite(rt_vec3_x(dir)));
    assert(std::isfinite(rt_vec3_y(dir)));
    assert(std::isfinite(rt_vec3_z(dir)));
    assert(std::isfinite(rt_path3d_get_length(path)));
}

static void test_transform_and_path_private_state_repairs_are_cache_coherent() {
    auto *transform = static_cast<TransformView *>(rt_transform3d_new());
    assert(transform != nullptr);
    assert(transform->dirty == 0 && transform->components_clean == 1);

    rt_transform3d_set_position(transform, 5.0, 6.0, 7.0);
    assert(transform->dirty == 1);
    void *matrix = rt_transform3d_get_matrix(transform);
    assert(rt_mat4_get(matrix, 0, 3) == 5.0);
    assert(transform->dirty == 0);

    rt_transform3d_set_position(transform, 5.0, 6.0, 7.0);
    rt_transform3d_set_scale(transform, 1.0, 1.0, 1.0);
    rt_transform3d_set_rotation(transform, rt_quat_new(0.0, 0.0, 0.0, 1.0));
    rt_transform3d_set_euler(transform, 0.0, 0.0, 0.0);
    rt_transform3d_translate(transform, rt_vec3_new(0.0, 0.0, 0.0));
    rt_transform3d_rotate(transform, rt_vec3_new(0.0, 1.0, 0.0), 6.28318530717958647692);
    assert(transform->dirty == 0);

    transform->position[0] = 9.0;
    transform->components_clean = 2;
    transform->dirty = 0;
    matrix = rt_transform3d_get_matrix(transform);
    assert(rt_mat4_get(matrix, 0, 3) == 9.0);
    assert(transform->components_clean == 1 && transform->dirty == 0);

    transform->matrix[0] = std::numeric_limits<double>::quiet_NaN();
    matrix = rt_transform3d_get_matrix(transform);
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            assert(std::isfinite(rt_mat4_get(matrix, row, col)));

    rt_transform3d_set_position(transform, 1000000000000.0, 0.0, 0.0);
    (void)rt_transform3d_get_matrix(transform);
    rt_transform3d_translate(transform, rt_vec3_new(1.0, 0.0, 0.0));
    assert(transform->position[0] == 1000000000000.0 && transform->dirty == 0);

    rt_transform3d_look_at(
        transform, rt_vec3_new(1000000000000.0, 0.0, -1.0), rt_vec3_new(0.0, 1.0, 0.0));
    (void)rt_transform3d_get_matrix(transform);
    rt_transform3d_look_at(
        transform, rt_vec3_new(1000000000000.0, 0.0, -1.0), rt_vec3_new(0.0, 1.0, 0.0));
    assert(transform->dirty == 0);

    auto *path = static_cast<PathView *>(rt_path3d_new());
    assert(path != nullptr);
    rt_path3d_add_point(path, rt_vec3_new(0.0, 0.0, 0.0));
    rt_path3d_add_point(path, rt_vec3_new(2.0, 0.0, 0.0));
    rt_path3d_add_point(path, rt_vec3_new(4.0, 1.0, 0.0));
    assert(rt_path3d_get_length(path) > 0.0);
    double spline_position[3] = {};
    double spline_tangent[3] = {};
    rt_path3d_eval_spline_raw(path, 0.5, spline_position, spline_tangent);
    assert(path->owned_spline_cumulative != nullptr && path->spline_dirty == 0);

    double *owned_xs = path->owned_xs;
    double *owned_ys = path->owned_ys;
    double *owned_zs = path->owned_zs;
    double *owned_spline = path->owned_spline_cumulative;
    const int32_t owned_capacity = path->point_allocation_capacity;
    assert(owned_ys == owned_xs + owned_capacity);
    assert(owned_zs == owned_ys + owned_capacity);
    path->xs = reinterpret_cast<double *>(uintptr_t{1});
    path->ys = nullptr;
    path->zs = reinterpret_cast<double *>(uintptr_t{3});
    path->point_capacity = INT32_MAX;
    path->spline_cumulative = reinterpret_cast<double *>(uintptr_t{5});
    path->spline_sample_count = INT32_MAX;
    assert(rt_path3d_get_point_count(path) == 3);
    assert(path->xs == owned_xs && path->ys == owned_ys && path->zs == owned_zs);
    assert(path->point_capacity == owned_capacity);
    assert(path->spline_cumulative == owned_spline && path->spline_dirty == 1);

    path->xs[1] = std::numeric_limits<double>::quiet_NaN();
    path->cached_length = 123.0;
    path->length_dirty = 0;
    path->spline_dirty = 0;
    double repaired_length = rt_path3d_get_length(path);
    assert(std::isfinite(repaired_length) && repaired_length != 123.0);
    assert(path->xs[1] == 0.0 && path->length_dirty == 0);

    rt_path3d_eval_spline_raw(path, 0.5, spline_position, spline_tangent);
    assert(path->spline_dirty == 0 && path->spline_sample_count > 1);
    path->spline_cumulative[1] = -1.0;
    path->spline_dirty = 0;
    rt_path3d_eval_spline_raw(path, 0.5, spline_position, spline_tangent);
    assert(path->spline_dirty == 0 && path->spline_cumulative[1] >= 0.0);
    for (double value : spline_position)
        assert(std::isfinite(value));
    for (double value : spline_tangent)
        assert(std::isfinite(value));

    const int8_t length_dirty = path->length_dirty;
    const int8_t spline_dirty = path->spline_dirty;
    rt_path3d_set_looping(path, 0);
    assert(path->length_dirty == length_dirty && path->spline_dirty == spline_dirty);

    rt_path3d_clear(path);
    assert(path->point_count == 0 && path->cached_length == 0.0);
    assert(path->owned_xs == owned_xs && path->point_allocation_capacity == owned_capacity);
    assert(path->owned_spline_cumulative == nullptr && path->spline_capacity == 0);

    auto *finalized_path = static_cast<PathView *>(rt_path3d_new());
    assert(finalized_path != nullptr);
    finalized_path->xs = reinterpret_cast<double *>(uintptr_t{1});
    finalized_path->ys = reinterpret_cast<double *>(uintptr_t{2});
    finalized_path->zs = reinterpret_cast<double *>(uintptr_t{3});
    if (rt_obj_release_check0(finalized_path))
        rt_obj_free(finalized_path);
}

static void test_material_texture_repairs_are_transactional_and_complete() {
    auto *material = static_cast<MaterialView *>(rt_material3d_new());
    void *wrong = rt_obj_new_i64(0, 8);
    assert(material != nullptr && wrong != nullptr);
    const int32_t workflow = material->workflow;

    assert(expect_trap_contains([&] { rt_material3d_set_metallic_roughness_map(material, wrong); },
                                "texture"));
    assert(material->workflow == workflow && material->metallic_roughness_map == nullptr);
    assert(expect_trap_contains([&] { rt_material3d_set_ao_map(material, wrong); }, "texture"));
    assert(material->workflow == workflow && material->ao_map == nullptr);

    rt_obj_retain_maybe(wrong);
    material->lightmap = wrong;
    assert(rt_material3d_get_persisted_texture_ref(
               material, RT_MATERIAL3D_PERSISTED_TEXTURE_SLOT_LIGHTMAP) == nullptr);
    assert(material->lightmap == nullptr);
    release_retained_probe(wrong);

    void *clone_wrong = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(clone_wrong);
    material->lightmap = clone_wrong;
    auto *clone = static_cast<MaterialView *>(rt_material3d_clone(material));
    assert(clone != nullptr && clone->lightmap == nullptr && material->lightmap == nullptr);
    release_retained_probe(clone_wrong);
}

static void test_sprite_decal_and_lensflare_repair_cached_render_state() {
    auto *pixels = rt_pixels_checked_impl_or_null(rt_pixels_new(4, 4));
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    rt_canvas3d canvas = {};
    assert(pixels != nullptr && camera != nullptr);
    canvas.width = 100;
    canvas.height = 100;

    auto *sprite = static_cast<SpriteView *>(rt_sprite3d_new(pixels));
    assert(sprite != nullptr);
    rt_sprite3d_set_additive(sprite, 0);
    rt_canvas3d_draw_sprite3d(&canvas, sprite, camera);
    auto *sprite_material = static_cast<rt_material3d *>(sprite->cached_material);
    assert(sprite_material != nullptr);
    assert(sprite_material->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND);

    sprite->position[0] = std::numeric_limits<double>::quiet_NaN();
    sprite->position[1] = std::numeric_limits<double>::infinity();
    sprite->scale_wh[0] = -1.0;
    sprite->scale_wh[1] = std::numeric_limits<double>::quiet_NaN();
    sprite->anchor[0] = -4.0;
    sprite->anchor[1] = 4.0;
    sprite->tex_w = INT32_MAX;
    sprite->tex_h = -1;
    sprite->frame_x = INT32_MAX;
    sprite->frame_y = -7;
    sprite->frame_w = INT32_MAX;
    sprite->frame_h = INT32_MAX;
    sprite->additive = -7;
    sprite->tint[0] = std::numeric_limits<double>::quiet_NaN();
    sprite->tint[1] = -1.0;
    sprite->tint[2] = 2.0;
    rt_canvas3d_draw_sprite3d(&canvas, sprite, camera);
    assert(sprite->position[0] == 0.0 && sprite->position[1] == 0.0);
    assert(sprite->scale_wh[0] == 1.0 && sprite->scale_wh[1] == 1.0);
    assert(sprite->anchor[0] == 0.0 && sprite->anchor[1] == 1.0);
    assert(sprite->tex_w == 4 && sprite->tex_h == 4);
    assert(sprite->frame_x == 3 && sprite->frame_y == 0);
    assert(sprite->frame_w == 1 && sprite->frame_h == 4);
    assert(sprite->additive == 1);
    assert(sprite->tint[0] == 1.0 && sprite->tint[1] == 0.0 && sprite->tint[2] == 1.0);

    uint32_t *pixel_data = pixels->data;
    pixels->data = nullptr;
    assert(expect_trap_contains([&] { (void)rt_sprite3d_new(pixels); }, "non-empty Pixels"));
    pixels->data = pixel_data;

    auto *decal = static_cast<DecalView *>(
        rt_decal3d_new(rt_vec3_new(5.0, 0.0, 0.0), rt_vec3_new(0.0, 1.0, 0.0), 2.0, pixels));
    assert(decal != nullptr);
    rt_canvas3d_draw_decal(&canvas, decal);
    assert(decal->mesh != nullptr && decal->material != nullptr &&
           decal->material_texture == pixels);
    decal->position[0] = std::numeric_limits<double>::quiet_NaN();
    decal->normal[0] = decal->normal[1] = decal->normal[2] = 0.0;
    decal->size = std::numeric_limits<double>::quiet_NaN();
    decal->depth_bias = 1.0;
    decal->lifetime = std::numeric_limits<double>::quiet_NaN();
    decal->max_lifetime = 10.0;
    rt_canvas3d_draw_decal(&canvas, decal);
    assert(decal->position[0] == 0.0 && decal->normal[1] == 1.0 && decal->size == 1.0);
    assert(decal->max_lifetime == -1.0 && decal->alpha == 1.0);
    assert(decal->depth_bias == 0.05);
    auto *decal_mesh = static_cast<rt_mesh3d *>(decal->mesh);
    auto *decal_material = static_cast<rt_material3d *>(decal->material);
    assert(decal_mesh != nullptr && decal_mesh->vertex_count == 4);
    assert(std::fabs(decal_mesh->vertices[0].pos[0]) < 2.0f);
    assert(decal_material->depth_bias == 0.05);

    void *decal_wrong = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(decal_wrong);
    decal->texture = decal_wrong;
    rt_canvas3d_draw_decal(&canvas, decal);
    assert(decal->texture == nullptr && decal->material_texture == nullptr);
    assert(static_cast<rt_material3d *>(decal->material)->texture == nullptr);
    release_retained_probe(decal_wrong);

    pixels->data = nullptr;
    auto *empty_texture_decal = static_cast<DecalView *>(
        rt_decal3d_new(rt_vec3_new(0.0, 0.0, 0.0), rt_vec3_new(0.0, 1.0, 0.0), 1.0, pixels));
    assert(empty_texture_decal != nullptr && empty_texture_decal->texture == nullptr);
    pixels->data = pixel_data;

    void *light = rt_light3d_new_point(rt_vec3_new(0.0, 0.0, 0.0), 1.0, 1.0, 1.0, 0.1);
    auto *flare = static_cast<LensFlareView *>(rt_lensflare3d_new(light));
    assert(flare != nullptr);
    flare->element_count = -7;
    rt_lensflare3d_add_element(flare, -4.0, -1.0, -1, 0.0);
    assert(flare->element_count == 1);
    assert(flare->elements[0].axis_offset == -1.0f && flare->elements[0].size == 32.0f);

    canvas.in_frame = 1;
    canvas.frame_is_2d = 0;
    canvas.cached_vp[0] = canvas.cached_vp[5] = canvas.cached_vp[10] = canvas.cached_vp[15] = 1.0f;
    flare->elements[0].axis_offset = std::numeric_limits<float>::quiet_NaN();
    flare->elements[0].size = std::numeric_limits<float>::infinity();
    flare->smoothed_visibility = 0.5f;
    flare->smoothed_frame_serial = UINT64_MAX;
    void *flare_wrong = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(flare_wrong);
    flare->elements[1].ghost = flare_wrong;
    void *undersized_pixels = rt_obj_new_i64(RT_PIXELS_CLASS_ID, 8);
    rt_obj_retain_maybe(undersized_pixels);
    flare->elements[2].ghost = undersized_pixels;
    flare->element_count = INT32_MAX;
    rt_canvas3d_draw_lens_flare(&canvas, flare);
    assert(flare->element_count == 16);
    assert(flare->elements[0].axis_offset == 0.0f && flare->elements[0].size == 32.0f);
    assert(std::isfinite(flare->smoothed_visibility));
    assert(flare->elements[1].ghost == nullptr && flare->elements[2].ghost == nullptr);
    release_retained_probe(flare_wrong);
    assert(rt_obj_release_check0(undersized_pixels) == 1);
    rt_obj_free(undersized_pixels);

    constexpr double large_origin = 999999999999.0;
    void *relative_light =
        rt_light3d_new_point(rt_vec3_new(large_origin + 0.25, 0.0, 0.0), 1.0, 1.0, 1.0, 0.1);
    auto *relative_flare = static_cast<LensFlareView *>(rt_lensflare3d_new(relative_light));
    assert(relative_flare != nullptr);
    rt_lensflare3d_add_element(relative_flare, 0.0, 16.0, 0xFFFFFF, 0.0);
    relative_flare->elements[0].axis_offset = std::numeric_limits<float>::quiet_NaN();
    canvas.camera_relative_upload = 1;
    canvas.camera_relative_origin[0] = large_origin;
    rt_canvas3d_draw_lens_flare(&canvas, relative_flare);
    assert(relative_flare->elements[0].axis_offset == 0.0f);
    canvas.camera_relative_upload = 0;
    canvas.camera_relative_origin[0] = 0.0;

    auto *sparse_flare = static_cast<LensFlareView *>(rt_lensflare3d_new(light));
    rt_lensflare3d_add_element(sparse_flare, 0.0, 16.0, 0xFFFFFF, 0.0);
    void *sparse_ghost = sparse_flare->elements[0].ghost;
    rt_obj_retain_maybe(sparse_ghost);
    sparse_flare->elements[15].ghost = sparse_ghost;
    sparse_flare->elements[0].ghost = nullptr;
    sparse_flare->element_count = -1;
    if (rt_obj_release_check0(sparse_flare))
        rt_obj_free(sparse_flare);
    assert(rt_obj_release_check0(sparse_ghost) == 1);
    rt_obj_free(sparse_ghost);
}

static void test_navmesh_sample_position_handles_empty_mesh() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);

    void *navmesh = rt_navmesh3d_build(mesh, -1.0, std::numeric_limits<double>::quiet_NaN());
    assert(navmesh != nullptr);
    assert(rt_navmesh3d_get_triangle_count(navmesh) == 0);

    void *point = rt_vec3_new(4.0, 5.0, 6.0);
    void *sampled = rt_navmesh3d_sample_position(navmesh, point);
    assert(rt_vec3_x(sampled) == 4.0);
    assert(rt_vec3_y(sampled) == 5.0);
    assert(rt_vec3_z(sampled) == 6.0);
    assert(rt_navmesh3d_is_walkable(navmesh, point) == 0);
}

static void test_navmesh_slope_refilter_and_sloped_height_projection() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 2.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);

    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    assert(navmesh != nullptr);
    assert(rt_navmesh3d_get_triangle_count(navmesh) == 0);

    rt_navmesh3d_set_max_slope(navmesh, 80.0);
    assert(rt_navmesh3d_get_triangle_count(navmesh) == 1);

    void *query = rt_vec3_new(0.25, 10.0, 0.25);
    void *sampled = rt_navmesh3d_sample_position(navmesh, query);
    assert(std::fabs(rt_vec3_x(sampled) - 0.25) < 1e-6);
    assert(std::fabs(rt_vec3_y(sampled) - 0.5) < 1e-6);
    assert(std::fabs(rt_vec3_z(sampled) - 0.25) < 1e-6);
}

static void test_navmesh_sample_position_uses_closest_triangle_point() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 2.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_vertex(mesh, 2.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);

    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    assert(navmesh != nullptr);
    assert(rt_navmesh3d_get_triangle_count(navmesh) == 1);

    void *query = rt_vec3_new(2.0, 5.0, 2.0);
    void *sampled = rt_navmesh3d_sample_position(navmesh, query);
    assert(std::fabs(rt_vec3_x(sampled) - 1.0) < 1e-6);
    assert(std::fabs(rt_vec3_y(sampled)) < 1e-6);
    assert(std::fabs(rt_vec3_z(sampled) - 1.0) < 1e-6);
}

static void test_navmesh_walkable_uses_vertical_tolerance() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 2.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    rt_mesh3d_add_vertex(mesh, 2.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);

    void *navmesh = rt_navmesh3d_build(mesh, 0.4, 1.8);
    assert(navmesh != nullptr);

    void *near_point = rt_vec3_new(0.25, 0.5, 0.25);
    void *high_point = rt_vec3_new(0.25, 10.0, 0.25);
    void *near_goal = rt_vec3_new(0.5, 0.0, 0.5);
    assert(rt_navmesh3d_is_walkable(navmesh, near_point) == 1);
    assert(rt_navmesh3d_is_walkable(navmesh, high_point) == 0);
    assert(rt_navmesh3d_find_path(navmesh, near_point, near_goal) != nullptr);
    assert(rt_navmesh3d_find_path(navmesh, high_point, near_goal) == nullptr);

    void *sampled = rt_navmesh3d_sample_position(navmesh, high_point);
    assert(std::fabs(rt_vec3_x(sampled) - 0.25) < 1e-6);
    assert(std::fabs(rt_vec3_y(sampled)) < 1e-6);
    assert(std::fabs(rt_vec3_z(sampled) - 0.25) < 1e-6);
}

static void test_capsule_collider_clamps_total_height_to_diameter() {
    void *capsule = rt_collider3d_new_capsule(2.0, 1.0);
    double min_v[3];
    double max_v[3];
    rt_collider3d_get_local_bounds_raw(capsule, min_v, max_v);
    assert(std::fabs(rt_collider3d_get_radius_raw(capsule) - 2.0) < 1e-9);
    assert(std::fabs(rt_collider3d_get_height_raw(capsule) - 4.0) < 1e-9);
    assert(std::fabs(min_v[0] + 2.0) < 1e-9);
    assert(std::fabs(min_v[1] + 2.0) < 1e-9);
    assert(std::fabs(max_v[1] - 2.0) < 1e-9);
    assert(std::fabs(max_v[2] - 2.0) < 1e-9);
}

static void test_morphtarget_sanitizes_nonfinite_weights_and_deltas() {
    void *mt = rt_morphtarget3d_new(1);
    assert(mt != nullptr);
    int64_t shape = rt_morphtarget3d_add_shape(mt, nullptr);
    assert(shape == 0);

    rt_morphtarget3d_set_weight(mt, shape, std::numeric_limits<double>::quiet_NaN());
    assert(rt_morphtarget3d_get_weight(mt, shape) == 0.0);

    rt_morphtarget3d_set_delta(mt, shape, 0, std::numeric_limits<double>::infinity(), 2.0, -3.0);
    const float *packed = rt_morphtarget3d_get_packed_deltas(mt);
    assert(packed != nullptr);
    assert(packed[0] == 0.0f);
    assert(packed[1] == 2.0f);
    assert(packed[2] == -3.0f);

    rt_morphtarget3d_set_delta(mt, shape, 0, 1.0e300, -1.0e300, 4.0);
    packed = rt_morphtarget3d_get_packed_deltas(mt);
    assert(packed[0] == 0.0f);
    assert(packed[1] == 0.0f);
    assert(packed[2] == 4.0f);
}

static void test_scene_reparent_preserves_child_and_counts() {
    void *p1 = rt_scene_node3d_new();
    void *p2 = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();

    rt_scene_node3d_add_child(p1, child);
    assert(rt_scene_node3d_child_count(p1) == 1);
    assert(rt_scene_node3d_get_parent(child) == p1);

    rt_scene_node3d_add_child(p2, child);
    assert(rt_scene_node3d_child_count(p1) == 0);
    assert(rt_scene_node3d_child_count(p2) == 1);
    assert(rt_scene_node3d_get_child(p2, 0) == child);
    assert(rt_scene_node3d_get_parent(child) == p2);

    void *scene = rt_scene3d_new();
    assert(scene != nullptr);
    assert(rt_scene3d_get_node_count(scene) == 1);
}

static void test_scene_deep_hierarchy_traversal_and_transform_clamps() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene3d_get_root(scene);
    void *deepest = nullptr;

    for (int i = 0; i < 3000; i++) {
        void *node = rt_scene_node3d_new();
        rt_scene_node3d_add_child(parent, node);
        parent = node;
        deepest = node;
    }

    rt_string name = rt_string_from_bytes("deepest", 7);
    rt_scene_node3d_set_name(deepest, name);
    assert(rt_scene3d_get_node_count(scene) == 3001);
    assert(rt_scene_node3d_find(rt_scene3d_get_root(scene), name) == deepest);

    rt_scene_node3d_set_position(deepest, 1.0e300, -1.0e300, 2.0);
    void *pos = rt_scene_node3d_get_position(deepest);
    assert(std::isfinite(rt_vec3_x(pos)) && std::fabs(rt_vec3_x(pos)) <= 1000000000000.0);
    assert(std::isfinite(rt_vec3_y(pos)) && std::fabs(rt_vec3_y(pos)) <= 1000000000000.0);

    void *world = rt_scene_node3d_get_world_matrix(deepest);
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++)
            assert(std::isfinite(rt_mat4_get(world, row, col)));
    }
}

static void test_mesh_bone_weights_are_validated_and_dirty_geometry() {
    auto *mesh = static_cast<rt_mesh3d *>(rt_mesh3d_new());
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);
    uint32_t before = mesh->geometry_revision;

    rt_mesh3d_set_bone_weights(
        mesh, 0, 1, 2.0, 999, 3.0, 3, -1.0, 4, std::numeric_limits<double>::quiet_NaN());

    assert(mesh->vertices[0].bone_indices[0] == 1);
    assert(mesh->vertices[0].bone_indices[1] == 0);
    assert(mesh->vertices[0].bone_indices[2] == 3);
    assert(mesh->vertices[0].bone_indices[3] == 4);
    assert(std::fabs(mesh->vertices[0].bone_weights[0] - 1.0f) < 1e-6f);
    assert(mesh->vertices[0].bone_weights[1] == 0.0f);
    assert(mesh->vertices[0].bone_weights[2] == 0.0f);
    assert(mesh->vertices[0].bone_weights[3] == 0.0f);
    assert(mesh->bone_count == 2);
    assert(mesh->geometry_revision != before);

    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, nullptr, -1, nullptr);
    rt_skeleton3d_add_bone(skel, nullptr, 0, nullptr);
    rt_mesh3d_set_skeleton(mesh, skel);
    assert(mesh->skeleton_ref == skel);
    assert(mesh->bone_count == 2);
}

static void test_transform_and_instance_batch_sanitize_extreme_matrices() {
    void *xf = rt_transform3d_new();
    rt_transform3d_set_position(xf, 1.0e300, -1.0e300, 2.0);
    rt_transform3d_set_scale(xf, 0.0, std::numeric_limits<double>::quiet_NaN(), 1.0e300);
    rt_transform3d_translate(xf, rt_vec3_new(-1.0e300, 1.0e300, 1.0e300));

    void *pos = rt_transform3d_get_position(xf);
    void *scale = rt_transform3d_get_scale(xf);
    assert(std::isfinite(rt_vec3_x(pos)) && std::fabs(rt_vec3_x(pos)) <= 1000000000000.0);
    assert(std::isfinite(rt_vec3_y(pos)) && std::fabs(rt_vec3_y(pos)) <= 1000000000000.0);
    assert(rt_vec3_x(scale) == 0.0);
    assert(std::isfinite(rt_vec3_z(scale)) && std::fabs(rt_vec3_z(scale)) <= 1000000000000.0);

    void *matrix = rt_transform3d_get_matrix(xf);
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++)
            assert(std::isfinite(rt_mat4_get(matrix, row, col)));
    }

    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mat = rt_material3d_new();
    auto *batch = static_cast<InstBatchView *>(rt_instbatch3d_new(mesh, mat));
    void *undersized_mat4 = rt_obj_new_i64(RT_MAT4_CLASS_ID, 8);
    rt_instbatch3d_add(batch, undersized_mat4);
    assert(rt_instbatch3d_count(batch) == 0);
    void *bad_matrix = rt_mat4_new(1.0e300,
                                   -1.0e300,
                                   1.0e300,
                                   -1.0e300,
                                   1.0e300,
                                   -1.0e300,
                                   1.0e300,
                                   -1.0e300,
                                   1.0e300,
                                   -1.0e300,
                                   1.0e300,
                                   -1.0e300,
                                   1.0e300,
                                   -1.0e300,
                                   1.0e300,
                                   -1.0e300);
    rt_instbatch3d_add(batch, bad_matrix);
    assert(rt_instbatch3d_count(batch) == 1);
    for (int i = 0; i < 16; i++)
        assert(std::isfinite(batch->transforms[i]));
    assert(batch->transforms[0] == 1.0f);
    assert(batch->transforms[5] == 1.0f);
    assert(batch->transforms[10] == 1.0f);
    assert(batch->transforms[15] == 1.0f);

    void *node = rt_scene_node3d_new();
    rt_scene_node3d_set_scale(node, 0.0, -0.0, 2.0);
    void *node_scale = rt_scene_node3d_get_scale(node);
    assert(rt_vec3_x(node_scale) == 0.0);
    assert(rt_vec3_y(node_scale) == 0.0);
    assert(rt_vec3_z(node_scale) == 2.0);
}

static void test_camera_and_transform_reduce_extreme_motion_inputs() {
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 1000.0);
    rt_camera3d_set_position(
        camera, rt_vec3_new(1.0e300, -1.0e300, std::numeric_limits<double>::infinity()));
    void *cam_pos = rt_camera3d_get_position(camera);
    assert(std::isfinite(rt_vec3_x(cam_pos)) && std::fabs(rt_vec3_x(cam_pos)) <= 1000000000000.0);
    assert(std::isfinite(rt_vec3_y(cam_pos)) && std::fabs(rt_vec3_y(cam_pos)) <= 1000000000000.0);
    assert(rt_vec3_z(cam_pos) == 0.0);

    rt_camera3d_fps_update(camera, 1.0e300, 1.0e300, 1.0e300, -1.0e300, 1.0e300, 1.0e300, 1.0e300);
    cam_pos = rt_camera3d_get_position(camera);
    assert(std::isfinite(rt_vec3_x(cam_pos)) && std::fabs(rt_vec3_x(cam_pos)) <= 1000000000000.0);
    assert(std::isfinite(rt_vec3_y(cam_pos)) && std::fabs(rt_vec3_y(cam_pos)) <= 1000000000000.0);
    assert(std::isfinite(rt_vec3_z(cam_pos)) && std::fabs(rt_vec3_z(cam_pos)) <= 1000000000000.0);

    void *xf = rt_transform3d_new();
    rt_transform3d_set_euler(xf, 1.0e300, -1.0e300, 1.0e300);
    rt_transform3d_rotate(xf, rt_vec3_new(0.0, 1.0, 0.0), 1.0e300);
    void *rot = rt_transform3d_get_rotation(xf);
    double len = std::sqrt(rt_quat_x(rot) * rt_quat_x(rot) + rt_quat_y(rot) * rt_quat_y(rot) +
                           rt_quat_z(rot) * rt_quat_z(rot) + rt_quat_w(rot) * rt_quat_w(rot));
    assert(std::isfinite(len) && std::fabs(len - 1.0) < 1e-6);
}

static void test_zero_size_colliders_fallback_to_positive_extents() {
    void *box = rt_collider3d_new_box(0.0, std::numeric_limits<double>::quiet_NaN(), -0.0);
    double min_v[3];
    double max_v[3];
    rt_collider3d_get_local_bounds_raw(box, min_v, max_v);
    assert(std::fabs(min_v[0] + 1.0) < 1e-9);
    assert(std::fabs(min_v[1] + 1.0) < 1e-9);
    assert(std::fabs(min_v[2] + 1.0) < 1e-9);
    assert(std::fabs(max_v[0] - 1.0) < 1e-9);
    assert(std::fabs(max_v[1] - 1.0) < 1e-9);
    assert(std::fabs(max_v[2] - 1.0) < 1e-9);

    void *sphere = rt_collider3d_new_sphere(0.0);
    assert(std::fabs(rt_collider3d_get_radius_raw(sphere) - 1.0) < 1e-9);

    void *capsule = rt_collider3d_new_capsule(0.0, 0.0);
    assert(std::fabs(rt_collider3d_get_radius_raw(capsule) - 1.0) < 1e-9);
    assert(std::fabs(rt_collider3d_get_height_raw(capsule) - 2.0) < 1e-9);
}

static void test_obj_loader_recalculates_mixed_missing_normals() {
    const std::string path = "/tmp/zanna_rt_graphics3d_mixed_normals.obj";
    {
        std::ofstream out(path);
        out << "v 0 0 0\n";
        out << "v 1 0 0\n";
        out << "v 0 1 0\n";
        out << "vn 1 0 0\n";
        out << "f 1 2 3\n";
    }

    rt_string path_string = rt_string_from_bytes(path.c_str(), static_cast<int64_t>(path.size()));
    auto *mesh = static_cast<rt_mesh3d *>(rt_mesh3d_from_obj(path_string));
    assert(mesh != nullptr);
    assert(mesh->vertex_count == 3);
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        assert(std::fabs(mesh->vertices[i].normal[2] - 1.0f) < 1e-6f);
    }
    std::remove(path.c_str());
}

static void test_skeleton_bind_pose_and_animation_duration_sanitize() {
    void *bad_bind = rt_mat4_new(std::numeric_limits<double>::quiet_NaN(),
                                 0.0,
                                 0.0,
                                 0.0,
                                 0.0,
                                 1.0,
                                 0.0,
                                 0.0,
                                 0.0,
                                 0.0,
                                 1.0,
                                 0.0,
                                 0.0,
                                 0.0,
                                 0.0,
                                 1.0);
    void *skel = rt_skeleton3d_new();
    assert(rt_skeleton3d_add_bone(skel, nullptr, -1, bad_bind) == 0);
    void *pose = rt_skeleton3d_get_bone_bind_pose(skel, 0);
    assert(rt_mat4_get(pose, 0, 0) == 1.0);
    assert(rt_mat4_get(pose, 1, 1) == 1.0);
    assert(rt_mat4_get(pose, 2, 2) == 1.0);
    assert(rt_mat4_get(pose, 3, 3) == 1.0);

    void *anim = rt_animation3d_new(nullptr, std::numeric_limits<double>::quiet_NaN());
    assert(std::fabs(rt_animation3d_get_duration(anim) - 1.0) < 1e-12);
}

static void test_light_and_material_boolean_state_is_initialized() {
    void *dir = rt_vec3_new(0.0, -1.0, 0.0);
    auto *directional = static_cast<rt_light3d *>(rt_light3d_new_directional(dir, 1.0, 1.0, 1.0));
    auto *point = static_cast<rt_light3d *>(rt_light3d_new_point(dir, 1.0, 0.0, 0.0, 1.0));
    auto *ambient = static_cast<rt_light3d *>(rt_light3d_new_ambient(0.1, 0.2, 0.3));
    assert(directional->inner_cos == 0.0 && directional->outer_cos == 0.0);
    assert(point->inner_cos == 0.0 && point->outer_cos == 0.0);
    assert(ambient->inner_cos == 0.0 && ambient->outer_cos == 0.0);

    auto *mat = static_cast<rt_material3d *>(rt_material3d_new());
    rt_material3d_set_unlit(mat, 42);
    assert(mat->unlit == 1);
    rt_material3d_set_unlit(mat, 0);
    assert(mat->unlit == 0);
}

static void test_light_noop_mutations_and_getters_repair_renderer_state() {
    void *position = rt_vec3_new(1.0, 2.0, 3.0);
    void *direction = rt_vec3_new(0.0, -1.0, 0.0);
    auto *point = static_cast<rt_light3d *>(rt_light3d_new_point(position, 1.0, 0.25, 0.5, 0.25));
    auto *directional =
        static_cast<rt_light3d *>(rt_light3d_new_directional(direction, 1.0, 1.0, 1.0));
    auto *rectangle = static_cast<rt_light3d *>(
        rt_light3d_new_area_rectangle(position, direction, 2.0, 3.0, 0.5, 0.5, 0.5, 0.2, 8.0));
    auto *sphere =
        static_cast<rt_light3d *>(rt_light3d_new_area_sphere(position, 2.0, 0.5, 0.5, 0.5, 8.0));
    auto *volume =
        static_cast<rt_light3d *>(rt_light3d_new_volume(position, 2.0, 0.5, 0.5, 0.5, 8.0));
    auto *spot = static_cast<rt_light3d *>(
        rt_light3d_new_spot(position, direction, 1.0, 1.0, 1.0, 0.25, 10.0, 20.0));
    assert(point && directional && rectangle && sphere && volume && spot);

    auto expect_no_revision = [](auto &&operation) {
        const uint64_t before = rt_light3d_mutation_revision();
        operation();
        assert(rt_light3d_mutation_revision() == before);
    };
    expect_no_revision([&] { rt_light3d_set_intensity(point, 1.0); });
    expect_no_revision([&] { rt_light3d_set_attenuation(point, 0.25); });
    expect_no_revision([&] { rt_light3d_set_color(point, 1.0, 0.25, 0.5); });
    expect_no_revision([&] { rt_light3d_set_enabled(point, 1); });
    expect_no_revision([&] { rt_light3d_set_casts_shadows(point, 0); });
    expect_no_revision([&] { rt_light3d_set_position(point, position); });
    expect_no_revision([&] { rt_light3d_set_direction(directional, direction); });
    expect_no_revision([&] { rt_light3d_set_width(rectangle, 2.0); });
    expect_no_revision([&] { rt_light3d_set_height(rectangle, 3.0); });
    expect_no_revision([&] { rt_light3d_set_attenuation(rectangle, 0.2); });
    expect_no_revision([&] { rt_light3d_set_range(rectangle, 8.0); });
    expect_no_revision([&] { rt_light3d_set_decay_type(rectangle, 2); });
    expect_no_revision([&] { rt_light3d_set_radius(sphere, 2.0); });
    expect_no_revision([&] { rt_light3d_set_range(sphere, 8.0); });
    expect_no_revision([&] { rt_light3d_set_radius(volume, 2.0); });
    expect_no_revision([&] { rt_light3d_set_range(volume, 8.0); });
    expect_no_revision([&] { rt_light3d_set_casts_shadows(volume, 1); });
    rt_light3d_set_spot_cone(spot, 10.0, 20.0);
    expect_no_revision([&] { rt_light3d_set_spot_cone(spot, 10.0, 20.0); });

    point->intensity = std::numeric_limits<double>::quiet_NaN();
    uint64_t before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_intensity(point) == 0.0);
    assert(point->intensity == 0.0 && rt_light3d_mutation_revision() == before + 1);
    expect_no_revision([&] { (void)rt_light3d_get_intensity(point); });

    point->color[0] = std::numeric_limits<double>::quiet_NaN();
    point->color[1] = -1.0;
    point->color[2] = 2.0;
    before = rt_light3d_mutation_revision();
    void *color = rt_light3d_get_color(point);
    assert(rt_vec3_x(color) == 0.0 && rt_vec3_y(color) == 0.0 && rt_vec3_z(color) == 1.0);
    assert(rt_light3d_mutation_revision() == before + 1);

    point->position[0] = std::numeric_limits<double>::infinity();
    point->position[1] = -1.0e300;
    before = rt_light3d_mutation_revision();
    void *repaired_position = rt_light3d_get_position(point);
    assert(rt_vec3_x(repaired_position) == 0.0);
    assert(rt_vec3_y(repaired_position) == -1000000000000.0);
    assert(rt_light3d_mutation_revision() == before + 1);

    point->enabled = -7;
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_enabled(point) == 1);
    assert(point->enabled == 1 && rt_light3d_mutation_revision() == before + 1);
    point->casts_shadows = -3;
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_casts_shadows(point) == 1);
    assert(point->casts_shadows == 1 && rt_light3d_mutation_revision() == before + 1);
    point->attenuation = std::numeric_limits<double>::quiet_NaN();
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_attenuation(point) == RT_LIGHT3D_DEFAULT_ATTENUATION);
    assert(rt_light3d_mutation_revision() == before + 1);

    rectangle->basis_u[0] = std::numeric_limits<double>::quiet_NaN();
    rectangle->basis_v[2] = std::numeric_limits<double>::infinity();
    before = rt_light3d_mutation_revision();
    void *repaired_direction = rt_light3d_get_direction(rectangle);
    assert(std::isfinite(rt_vec3_x(repaired_direction)));
    for (double lane : rectangle->basis_u)
        assert(std::isfinite(lane));
    for (double lane : rectangle->basis_v)
        assert(std::isfinite(lane));
    assert(rt_light3d_mutation_revision() == before + 1);

    rectangle->width = std::numeric_limits<double>::quiet_NaN();
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_width(rectangle) == 1.0);
    assert(rt_light3d_mutation_revision() == before + 1);
    rectangle->height = 0.0;
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_height(rectangle) == 1.0);
    assert(rt_light3d_mutation_revision() == before + 1);
    rectangle->range = std::numeric_limits<double>::infinity();
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_range(rectangle) == 1.0);
    assert(rt_light3d_mutation_revision() == before + 1);
    rectangle->decay_type = 99;
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_decay_type(rectangle) == 2);
    assert(rt_light3d_mutation_revision() == before + 1);

    sphere->radius = -1.0;
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_radius(sphere) == 1.0);
    assert(rt_light3d_mutation_revision() == before + 1);
    volume->casts_shadows = 1;
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_casts_shadows(volume) == 0);
    assert(rt_light3d_mutation_revision() == before + 1);

    spot->inner_cos = -1.0;
    spot->outer_cos = 2.0;
    before = rt_light3d_mutation_revision();
    double inner = rt_light3d_get_inner_cone_degrees(spot);
    double outer = rt_light3d_get_outer_cone_degrees(spot);
    assert(std::isfinite(inner) && std::isfinite(outer) && inner < outer);
    assert(rt_light3d_mutation_revision() == before + 1);

    point->type = 99;
    before = rt_light3d_mutation_revision();
    assert(rt_light3d_get_type(point) == 0);
    assert(point->type == 0 && rt_light3d_mutation_revision() == before + 1);
}

static void test_instance_batch_repairs_storage_and_preserves_relative_motion_history() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new();
    auto *batch = static_cast<InstBatchView *>(rt_instbatch3d_new(mesh, material));
    assert(batch != nullptr);
    rt_instbatch3d_add(batch, rt_mat4_identity());
    assert(rt_instbatch3d_count(batch) == 1);

    batch->transforms = reinterpret_cast<float *>(uintptr_t{1});
    batch->current_snapshot = nullptr;
    batch->prev_transforms = reinterpret_cast<float *>(uintptr_t{3});
    batch->transforms64 = reinterpret_cast<double *>(uintptr_t{5});
    batch->current_snapshot64 = nullptr;
    batch->prev_transforms64 = reinterpret_cast<double *>(uintptr_t{7});
    batch->instance_capacity = INT32_MAX;
    assert(rt_instbatch3d_count(batch) == 1);
    assert(batch->transforms == batch->owned_transforms);
    assert(batch->current_snapshot == nullptr && batch->owned_current_snapshot == nullptr);
    assert(batch->prev_transforms == nullptr && batch->owned_prev_transforms == nullptr);
    assert(batch->transforms64 == batch->owned_transforms64);
    assert(batch->current_snapshot64 == batch->owned_current_snapshot64);
    assert(batch->prev_transforms64 == batch->owned_prev_transforms64);
    assert(batch->instance_capacity == batch->allocation_capacity);

    batch->instance_count = INT32_MAX;
    batch->motion_snapshot_count = INT32_MAX;
    batch->prev_count = INT32_MAX;
    batch->has_prev_snapshot = -7;
    assert(rt_instbatch3d_count(batch) == batch->allocation_capacity);
    assert(batch->motion_snapshot_count == batch->allocation_capacity);
    assert(batch->prev_count == batch->allocation_capacity);
    assert(batch->has_prev_snapshot == 1);
    batch->instance_count = 1;
    batch->motion_snapshot_count = 0;
    batch->prev_count = 0;
    batch->has_prev_snapshot = 0;

    rt_canvas3d canvas = {};
    canvas.width = 100;
    canvas.height = 100;
    canvas.in_frame = 1;
    canvas.frame_is_2d = 0;
    canvas.backend = &vgfx3d_software_backend;
    canvas.cached_vp[0] = canvas.cached_vp[5] = canvas.cached_vp[10] = canvas.cached_vp[15] = 1.0f;
    canvas.frame_serial = 0;
    rt_canvas3d_draw_instanced(&canvas, batch);
    assert(batch->motion_frame_initialized == 1);
    assert(batch->motion_snapshot_count == 1 && batch->last_motion_frame == 0);

    void *translated =
        rt_mat4_new(1.0, 0.0, 0.0, 2.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
    rt_instbatch3d_set(batch, 0, translated);
    canvas.frame_serial = 1;
    rt_canvas3d_draw_instanced(&canvas, batch);
    assert(batch->has_prev_snapshot == 1 && batch->prev_count == 1);
    assert(batch->prev_transforms64[3] == 0.0);
    assert(batch->current_snapshot64[3] == 2.0);

    const double origin = 999999999999.0;
    void *large_translation = rt_mat4_new(
        1.0, 0.0, 0.0, origin + 0.25, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
    rt_instbatch3d_set(batch, 0, large_translation);
    canvas.camera_relative_upload = 1;
    canvas.camera_relative_origin[0] = origin;
    canvas.frame_serial = 2;
    rt_canvas3d_draw_instanced(&canvas, batch);
    assert(batch->visible_transforms != nullptr);
    assert(std::fabs(batch->visible_transforms[3] - 0.25f) < 1e-6f);
    assert(batch->current_snapshot64[3] == origin + 0.25);
    assert(batch->prev_transforms64[3] == 2.0);

    rt_instbatch3d_clear(batch);
    assert(batch->instance_count == 0 && batch->motion_snapshot_count == 0);
    assert(batch->prev_count == 0 && batch->has_prev_snapshot == 0);
    assert(batch->motion_frame_initialized == 0);
}

static void test_physics_joints_deduplicate_and_raycast_is_true_ray() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *a = rt_body3d_new_sphere(0.5, 1.0);
    void *b = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, 0.0, 0.0, 2.0);
    void *joint = rt_distance_joint3d_new(a, b, 2.0);
    rt_world3d_add_joint(world, joint, RT_JOINT_DISTANCE);
    rt_world3d_add_joint(world, joint, RT_JOINT_DISTANCE);
    assert(rt_world3d_joint_count(world) == 1);

    void *ray_world = rt_world3d_new(0.0, 0.0, 0.0);
    void *near_miss = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(near_miss, 0.5005, 0.0, 5.0);
    rt_world3d_add(ray_world, near_miss);
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(0.0, 0.0, 1.0);
    assert(rt_world3d_raycast(ray_world, origin, dir, 10.0, -1) == nullptr);

    void *hit_body = rt_body3d_new_sphere(0.5, 1.0);
    rt_body3d_set_position(hit_body, 0.0, 0.0, 5.0);
    rt_world3d_add(ray_world, hit_body);
    void *hit = rt_world3d_raycast(ray_world, origin, dir, 10.0, -1);
    assert(hit != nullptr);
    assert(rt_physics_hit3d_get_body(hit) == hit_body);
    assert(std::fabs(rt_physics_hit3d_get_distance(hit) - 4.5) < 1e-6);
}

static void test_raycast_math_and_backend_guards_are_strict() {
    void *mn = rt_vec3_new(0.0, 0.0, 0.0);
    void *mx = rt_vec3_new(1.0, 1.0, 1.0);
    void *tangent_center = rt_vec3_new(2.0, 0.5, 0.5);
    assert(rt_aabb3d_sphere_overlaps(mn, mx, tangent_center, 1.0) == 1);

    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *dir = rt_vec3_new(1.0, 0.0, 0.0);
    void *far_min = rt_vec3_new(1.0e35, -1.0, -1.0);
    void *far_max = rt_vec3_new(1.0e35 + 1.0e31, 1.0, 1.0);
    double far_hit = rt_ray3d_intersect_aabb(origin, dir, far_min, far_max);
    assert(std::isfinite(far_hit));
    assert(far_hit > 0.0 && far_hit <= 1000000000.0);

    void *fake = rt_obj_new_i64(0, 8);
    assert(rt_sphere3d_overlaps(fake, 1.0, tangent_center, 1.0) == 0);
    void *closest = rt_segment3d_closest_point(fake, mn, mx);
    assert(rt_vec3_x(closest) == 0.0 && rt_vec3_y(closest) == 0.0 && rt_vec3_z(closest) == 0.0);

    uint8_t dst_rgba[4] = {7, 7, 7, 7};
    uint16_t src16[4] = {0, 0, 0, 0};
    float dst32[4] = {7.0f, 7.0f, 7.0f, 7.0f};
    float src32[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int32_t huge = std::numeric_limits<int32_t>::max();
    int32_t huge_w = huge / 2 + 1;
    vgfx3d_copy_linear_rgba16f_to_rgba8(dst_rgba, huge, huge_w, 1, src16, huge);
    vgfx3d_copy_linear_rgba16f_to_rgba32f(dst32, huge, huge_w, 1, src16, huge);
    vgfx3d_copy_linear_rgba32f_to_rgba8(dst_rgba, huge, huge_w, 1, src32, huge);
    assert(dst_rgba[0] == 7);
    assert(dst32[0] == 7.0f);

    float matrix[16] = {1.0f,
                        0.0f,
                        0.0f,
                        0.0f,
                        0.0f,
                        1.0f,
                        0.0f,
                        0.0f,
                        0.0f,
                        0.0f,
                        1.0f,
                        0.0f,
                        0.0f,
                        0.0f,
                        0.0f,
                        1.0f};
    matrix[0] = std::numeric_limits<float>::quiet_NaN();
    float inverse[16];
    int invert_result = vgfx3d_invert_matrix4(matrix, inverse);
    assert(invert_result == -1);
    (void)invert_result;
}

static void test_physics_oriented_boxes_do_not_collide_as_aabbs() {
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    void *a = rt_body3d_new_aabb(1.0, 0.5, 0.1, 0.0);
    void *b = rt_body3d_new_aabb(1.0, 0.5, 0.1, 1.0);
    const double angle = 0.78539816339744830962;
    void *q = rt_quat_new(0.0, std::sin(angle * 0.5), 0.0, std::cos(angle * 0.5));
    const double sep = 0.25;

    rt_body3d_set_orientation(a, q);
    rt_body3d_set_orientation(b, q);
    rt_body3d_set_position(a, 0.0, 0.0, 0.0);
    rt_body3d_set_position(b, std::sin(angle) * sep, 0.0, std::cos(angle) * sep);
    rt_world3d_add(world, a);
    rt_world3d_add(world, b);
    rt_world3d_step(world, 0.016);

    assert(rt_world3d_get_collision_count(world) == 0);
}

static void test_physics_raycast_uses_mesh_triangles_not_mesh_aabb() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 5.0, 0.0, 0.0, -1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 10.0, 0.0, 5.0, 0.0, 0.0, -1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 10.0, 5.0, 0.0, 0.0, -1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);

    void *collider = rt_collider3d_new_mesh(mesh);
    void *body = rt_body3d_new(0.0);
    rt_body3d_set_collider(body, collider);
    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    rt_world3d_add(world, body);

    void *dir = rt_vec3_new(0.0, 0.0, 1.0);
    assert(rt_world3d_raycast(world, rt_vec3_new(9.0, 9.0, 0.0), dir, 10.0, -1) == nullptr);

    void *hit = rt_world3d_raycast(world, rt_vec3_new(1.0, 1.0, 0.0), dir, 10.0, -1);
    assert(hit != nullptr);
    assert(rt_physics_hit3d_get_body(hit) == body);
    assert(std::fabs(rt_physics_hit3d_get_distance(hit) - 5.0) < 1e-6);
}

static void test_physics_mesh_box_collision_uses_triangles_not_mesh_aabb() {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 10.0, 0.0, 0.0, 0.0, 0.0, -1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 10.0, 0.0, 0.0, 0.0, -1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);

    void *mesh_body = rt_body3d_new(0.0);
    rt_body3d_set_collider(mesh_body, rt_collider3d_new_mesh(mesh));
    void *box = rt_body3d_new_aabb(0.1, 0.1, 0.1, 1.0);
    rt_body3d_set_position(box, 9.0, 9.0, 0.0);

    void *world = rt_world3d_new(0.0, 0.0, 0.0);
    rt_world3d_add(world, mesh_body);
    rt_world3d_add(world, box);
    rt_world3d_step(world, 0.016);

    assert(rt_world3d_get_collision_count(world) == 0);
}

static void test_physics_sweeps_handle_thin_geometry_and_long_capsules() {
    void *sphere_world = rt_world3d_new(0.0, 0.0, 0.0);
    void *thin_wall = rt_body3d_new_aabb(0.002, 1.0, 1.0, 0.0);
    rt_body3d_set_position(thin_wall, 1.007, 0.0, 0.0);
    rt_world3d_add(sphere_world, thin_wall);
    void *sphere_hit = rt_world3d_sweep_sphere(
        sphere_world, rt_vec3_new(0.0, 0.0, 0.0), 0.002, rt_vec3_new(2.0, 0.0, 0.0), -1);
    assert(sphere_hit != nullptr);

    void *capsule_world = rt_world3d_new(0.0, 0.0, 0.0);
    void *small_obstacle = rt_body3d_new_sphere(0.05, 0.0);
    rt_body3d_set_position(small_obstacle, 1.0, 0.83, 0.0);
    rt_world3d_add(capsule_world, small_obstacle);
    void *capsule_hit = rt_world3d_sweep_capsule(capsule_world,
                                                 rt_vec3_new(0.0, 0.0, 0.0),
                                                 rt_vec3_new(0.0, 10.0, 0.0),
                                                 0.05,
                                                 rt_vec3_new(2.0, 0.0, 0.0),
                                                 -1);
    assert(capsule_hit != nullptr);
}

static void test_terrain_water_and_vegetation_zero_dt_paths() {
    void *terrain = rt_terrain3d_new(4, 4);
    void *perlin = rt_perlin_new(123);
    rt_terrain3d_generate_perlin(terrain,
                                 perlin,
                                 std::numeric_limits<double>::quiet_NaN(),
                                 1000,
                                 std::numeric_limits<double>::quiet_NaN());
    double h = rt_terrain3d_get_height_at(terrain, 1.0, 1.0);
    assert(std::isfinite(h));
    assert(h >= 0.0 && h <= 1.0);

    auto *water = static_cast<WaterView *>(rt_water3d_new(2.0, 2.0));
    rt_water3d_update(water, 0.0);
    assert(water->mesh != nullptr);
    int64_t initial_vertices = rt_mesh3d_get_vertex_count(water->mesh);
    rt_water3d_set_resolution(water, 8);
    assert(water->mesh_dirty == 1);
    rt_water3d_update(water, 0.0);
    assert(water->mesh_dirty == 0);
    assert(rt_mesh3d_get_vertex_count(water->mesh) == 81);
    assert(rt_mesh3d_get_vertex_count(water->mesh) != initial_vertices);

    auto *veg = static_cast<VegetationView *>(rt_vegetation3d_new(nullptr));
    rt_vegetation3d_set_lod_distances(veg, 50.0, 100.0);
    rt_vegetation3d_populate(veg, terrain, 8);
    rt_vegetation3d_update(veg, 0.0, 0.0, 0.0, 0.0);
    assert(veg->total_count > 0);
    assert(veg->visible_count > 0);
}

static void test_vegetation_density_map_edges_are_exact() {
    void *terrain = rt_terrain3d_new(4, 4);
    void *density = rt_pixels_new(1, 1);
    auto *veg = static_cast<VegetationView *>(rt_vegetation3d_new(nullptr));
    assert(terrain != nullptr && density != nullptr && veg != nullptr);

    rt_pixels_set(density, 0, 0, 0x000000FFll);
    rt_vegetation3d_set_density_map(veg, density);
    rt_vegetation3d_populate(veg, terrain, 512);
    assert(veg->total_count == 0);

    rt_pixels_set(density, 0, 0, 0xFF0000FFll);
    rt_vegetation3d_populate(veg, terrain, 32);
    assert(veg->total_count == 32);
}

static void test_vegetation_extreme_wind_and_corrupt_instances_are_compacted() {
    void *terrain = rt_terrain3d_new(8, 8);
    auto *veg = static_cast<VegetationView *>(rt_vegetation3d_new(nullptr));
    assert(terrain != nullptr && veg != nullptr);

    rt_vegetation3d_set_blade_size(veg, 1.0e300, 1.0e300, 1.0e300);
    rt_vegetation3d_set_lod_distances(veg, 1.0e300, 1.0e300);
    rt_vegetation3d_set_wind_params(veg, 1.0e300, 1.0e300, 1.0e300);
    rt_vegetation3d_populate(veg, terrain, 16);
    assert(veg->total_count == 16);

    veg->positions[0] = std::numeric_limits<float>::infinity();
    veg->base_transforms[16] = std::numeric_limits<float>::quiet_NaN();
    veg->base_transforms[32 + 3] = 1.0e30f;
    veg->visible_count = veg->visible_capacity + 99;

    rt_vegetation3d_update(veg, 1.0e300, 0.0, 1.0e300, 0.0);

    assert(std::isfinite(veg->time));
    assert(veg->visible_count >= 0 && veg->visible_count <= veg->visible_capacity);
    assert(veg->visible_count > 0);
    for (int32_t i = 0; i < veg->visible_count * 16; i++) {
        assert(std::isfinite(veg->visible_transforms[i]));
        assert(std::fabs(veg->visible_transforms[i]) <= 1000000.0f);
    }
}

static void test_particles_and_water_numeric_knobs_are_bounded() {
    auto *particles = static_cast<ParticleView *>(rt_particles3d_new(8));
    rt_particles3d_set_position(particles, 1.0e300, -1.0e300, 2.0);
    rt_particles3d_set_speed(particles, 1.0e300, -1.0e300);
    rt_particles3d_set_lifetime(particles, 1.0e300, -1.0e300);
    rt_particles3d_set_size(particles, 1.0e300, -1.0e300);
    rt_particles3d_set_gravity(particles, 1.0e300, -1.0e300, 0.0);
    rt_particles3d_set_rate(particles, 1.0e300);
    rt_particles3d_set_emitter_size(particles, 1.0e300, -1.0e300, 4.0);
    rt_particles3d_burst(particles, 16);
    rt_particles3d_update(particles, 0.016);

    assert(std::isfinite(particles->position[0]) && particles->position[0] <= 1000000000000.0);
    assert(std::isfinite(particles->position[1]) && particles->position[1] >= -1000000000000.0);
    assert(std::isfinite(particles->speed_min) && particles->speed_min >= 0.0);
    assert(std::isfinite(particles->speed_max) && particles->speed_max <= 1000000.0);
    assert(std::isfinite(particles->life_min) && particles->life_min >= 0.0);
    assert(std::isfinite(particles->size_start) && particles->size_start <= 1000000.0);
    assert(std::isfinite(particles->gravity[0]) && particles->gravity[0] <= 1000000.0);
    assert(std::isfinite(particles->emitter_size[0]) && particles->emitter_size[0] <= 1000000.0);
    assert(particles->count <= particles->max_particles);

    auto *water = static_cast<WaterView *>(rt_water3d_new(1.0e300, -1.0));
    assert(std::isfinite(water->width) && water->width <= 1000000.0);
    assert(water->depth == 1.0);
    rt_water3d_set_height(water, 1.0e300);
    rt_water3d_set_wave_params(water, 1.0e300, 1.0e300, -1.0e300);
    rt_water3d_add_wave(water, 1.0, 0.0, 1.0e300, 1.0e300, 1.0e300);
    rt_water3d_update(water, 0.0);

    assert(std::isfinite(water->height) && water->height <= 1000000000000.0);
    assert(std::isfinite(water->wave_speed) && water->wave_speed <= 1000000.0);
    assert(std::isfinite(water->wave_amplitude) && water->wave_amplitude <= 1000000.0);
    assert(std::isfinite(water->wave_frequency) && water->wave_frequency >= -1000000.0);
    assert(water->wave_count == 1);
    assert(std::isfinite(water->waves[0][2]) && water->waves[0][2] <= 1000000.0);
    assert(std::isfinite(water->waves[0][3]) && water->waves[0][3] <= 1000000.0);
    assert(water->mesh != nullptr);
    assert(static_cast<rt_mesh3d *>(water->mesh)->build_failed == 0);
}

static void test_water_extreme_wave_directions_and_cached_resources_are_repaired() {
    auto *water = static_cast<WaterView *>(rt_water3d_new(16.0, 16.0));
    assert(water != nullptr);

    water->wave_count = -7;
    rt_water3d_add_wave(water, 1.0e300, -1.0e300, 1.0e300, 1.0e300, 1.0e300);
    assert(water->wave_count == 1);
    assert(std::isfinite(water->waves[0][0]));
    assert(std::isfinite(water->waves[0][1]));
    assert(std::fabs(std::fabs(water->waves[0][0]) - 0.70710678) < 0.001);
    assert(std::fabs(std::fabs(water->waves[0][1]) - 0.70710678) < 0.001);
    assert(std::isfinite(water->waves[0][2]) && water->waves[0][2] <= 1000000.0);
    assert(std::isfinite(water->waves[0][3]) && water->waves[0][3] <= 1000000.0);
    assert(std::isfinite(water->waves[0][4]) && water->waves[0][4] > 0.0);

    water->mesh = rt_obj_new_i64(0, 8);
    water->material = rt_obj_new_i64(0, 8);
    water->texture = rt_obj_new_i64(0, 8);
    water->normal_map = rt_obj_new_i64(0, 8);
    water->env_map = rt_obj_new_i64(0, 8);
    water->mesh_dirty = 0;

    rt_water3d_update(water, 0.0);
    assert(water->mesh != nullptr);
    assert(water->material != nullptr);
    assert(rt_obj_class_id(water->mesh) == RT_G3D_MESH3D_CLASS_ID);
    assert(rt_obj_class_id(water->material) == RT_G3D_MATERIAL3D_CLASS_ID);
    assert(water->texture == nullptr);
    assert(water->normal_map == nullptr);
    assert(water->env_map == nullptr);

    auto *mesh = static_cast<rt_mesh3d *>(water->mesh);
    assert(mesh->vertex_count > 0);
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        assert(std::isfinite(mesh->vertices[i].pos[0]));
        assert(std::isfinite(mesh->vertices[i].pos[1]));
        assert(std::isfinite(mesh->vertices[i].pos[2]));
        assert(std::isfinite(mesh->vertices[i].normal[0]));
        assert(std::isfinite(mesh->vertices[i].normal[1]));
        assert(std::isfinite(mesh->vertices[i].normal[2]));
    }
}

static void test_water_wrong_class_private_resources_clear_without_release() {
    auto *water = static_cast<WaterView *>(rt_water3d_new(8.0, 8.0));
    assert(water != nullptr);

    void *wrong = rt_obj_new_i64(0, 8);
    assert(wrong != nullptr);
    rt_obj_retain_maybe(wrong);
    water->mesh = wrong;
    water->material = wrong;
    water->texture = wrong;
    water->normal_map = wrong;
    water->env_map = wrong;
    water->mesh_dirty = 0;

    rt_water3d_update(water, 0.0);

    assert(water->mesh != nullptr);
    assert(water->material != nullptr);
    assert(rt_obj_class_id(water->mesh) == RT_G3D_MESH3D_CLASS_ID);
    assert(rt_obj_class_id(water->material) == RT_G3D_MATERIAL3D_CLASS_ID);
    assert(water->texture == nullptr);
    assert(water->normal_map == nullptr);
    assert(water->env_map == nullptr);
    assert(rt_obj_release_check0(wrong) == 0);
    if (rt_obj_release_check0(wrong))
        rt_obj_free(wrong);
}

static void test_vegetation_wrong_class_private_resources_clear_without_release() {
    auto *veg = static_cast<VegetationView *>(rt_vegetation3d_new(nullptr));
    assert(veg != nullptr);
    void *owned_mesh = veg->blade_mesh;
    void *owned_material = veg->blade_material;

    void *wrong = rt_obj_new_i64(0, 8);
    assert(wrong != nullptr);
    rt_obj_retain_maybe(wrong);
    veg->blade_mesh = wrong;
    veg->blade_material = wrong;
    veg->density_map = wrong;

    rt_vegetation3d_update(veg, 0.0, 0.0, 0.0, 0.0);
    assert(veg->blade_mesh == owned_mesh);
    assert(veg->blade_material == owned_material);
    assert(veg->density_map == nullptr);
    assert(rt_obj_release_check0(wrong) == 0);
    if (rt_obj_release_check0(wrong))
        rt_obj_free(wrong);

    rt_vegetation3d_set_blade_size(veg, 0.5, 1.5, 0.25);
    assert(veg->blade_mesh != nullptr);
    assert(rt_obj_class_id(veg->blade_mesh) == RT_G3D_MESH3D_CLASS_ID);
}

static void release_retained_probe(void *probe) {
    assert(rt_obj_release_check0(probe) == 0);
    if (rt_obj_release_check0(probe))
        rt_obj_free(probe);
}

static void test_texture_atlas_repairs_corrupt_storage_and_preserves_stable_ids() {
    auto *atlas = static_cast<AtlasView *>(rt_texatlas3d_new(32, 16));
    auto *pixels = rt_pixels_checked_impl_or_null(rt_pixels_new(2, 2));
    assert(atlas != nullptr && pixels != nullptr);
    rt_pixels_set(pixels, 0, 0, 0x01020304);
    rt_pixels_set(pixels, 1, 0, 0x11121314);
    rt_pixels_set(pixels, 0, 1, 0x21222324);
    rt_pixels_set(pixels, 1, 1, 0x31323334);

    assert(rt_texatlas3d_add(atlas, pixels) == 0);
    void *first_texture = rt_texatlas3d_get_texture(atlas);
    assert(first_texture != nullptr);
    uint64_t first_generation = vgfx3d_get_pixels_generation(first_texture);
    assert(first_generation > 0);
    assert(rt_texatlas3d_get_texture(atlas) == first_texture);
    assert(vgfx3d_get_pixels_generation(first_texture) == first_generation);

    atlas->width = INT32_MAX;
    atlas->height = -1;
    atlas->shelf_x = INT32_MAX;
    atlas->shelf_y = INT32_MAX;
    atlas->shelf_h = INT32_MAX;
    assert(rt_texatlas3d_add(atlas, pixels) == 1);
    assert(atlas->width == 32 && atlas->height == 16);
    assert(atlas->regions[1].x == 5 && atlas->regions[1].y == 1);

    atlas->data = reinterpret_cast<uint32_t *>(uintptr_t{1});
    assert(rt_texatlas3d_get_texture(atlas) != nullptr);
    assert(atlas->data == atlas->allocated_data);

    int64_t source_width = pixels->width;
    uint32_t *source_data = pixels->data;
    pixels->width = std::numeric_limits<int64_t>::max();
    assert(rt_texatlas3d_add(atlas, pixels) == -1);
    pixels->width = source_width;
    pixels->data = source_data + 1;
    assert(rt_texatlas3d_add(atlas, pixels) == -1);
    pixels->data = source_data;
    assert(rt_texatlas3d_add(atlas, rt_obj_new_i64(0, 8)) == -1);

    assert(rt_texatlas3d_add(atlas, pixels) == 2);
    atlas->regions[1].x = 17;
    double u0 = -1.0, v0 = -1.0, u1 = -1.0, v1 = -1.0;
    rt_texatlas3d_get_uv_rect(atlas, 2, &u0, &v0, &u1, &v1);
    assert(u0 == 0.0 && v0 == 0.0 && u1 == 1.0 && v1 == 1.0);
    assert(atlas->region_count == 1);
    rt_texatlas3d_get_uv_rect(atlas, 0, &u0, &v0, &u1, &v1);
    assert(std::fabs(u0 - 1.0 / 32.0) < 1e-12);

    atlas->regions[0].y = INT32_MIN;
    atlas->region_count = 1;
    rt_texatlas3d_get_uv_rect(atlas, 0, &u0, &v0, &u1, &v1);
    assert(atlas->region_count == 0);
    assert(u0 == 0.0 && v0 == 0.0 && u1 == 1.0 && v1 == 1.0);

    auto *wrong_cache_atlas = static_cast<AtlasView *>(rt_texatlas3d_new(16, 16));
    void *wrong_cache = rt_obj_new_i64(0, 8);
    assert(wrong_cache_atlas != nullptr && wrong_cache != nullptr);
    rt_obj_retain_maybe(wrong_cache);
    wrong_cache_atlas->cached_pixels = wrong_cache;
    wrong_cache_atlas->dirty = 0;
    assert(rt_texatlas3d_get_texture(wrong_cache_atlas) != nullptr);
    assert(rt_obj_class_id(wrong_cache_atlas->cached_pixels) == RT_PIXELS_CLASS_ID);
    release_retained_probe(wrong_cache);
}

static void test_sky_and_timeofday_repair_extreme_and_wrong_class_state() {
    auto *sky = static_cast<SkyView *>(rt_sky3d_new());
    assert(sky != nullptr);
    rt_sky3d_set_resolution(sky, 16);
    void *huge_direction = rt_vec3_new(std::numeric_limits<double>::max(),
                                       std::numeric_limits<double>::max() * 0.5,
                                       -std::numeric_limits<double>::max() * 0.25);
    rt_sky3d_set_sun_direction(sky, huge_direction);
    assert(std::isfinite(sky->sun_dir[0]) && std::isfinite(sky->sun_dir[1]) &&
           std::isfinite(sky->sun_dir[2]));
    double sun_length =
        std::sqrt(sky->sun_dir[0] * sky->sun_dir[0] + sky->sun_dir[1] * sky->sun_dir[1] +
                  sky->sun_dir[2] * sky->sun_dir[2]);
    assert(std::fabs(sun_length - 1.0) < 1e-12);
    rt_sky3d_set_ground_albedo(sky, 0.2, 0.3, 0.4);
    assert(rt_sky3d_update(sky, nullptr) == 1);
    assert(sky->dirty == 0 && rt_cubemap3d_is_complete(sky->cubemap));
    auto *cubemap = static_cast<rt_cubemap3d *>(sky->cubemap);
    for (void *face : cubemap->faces) {
        uint64_t generation = vgfx3d_get_pixels_generation(face);
        assert(generation > 0 && generation <= 2);
    }

    rt_sky3d_set_sun_direction(sky, huge_direction);
    rt_sky3d_set_ground_albedo(sky, 0.2, 0.3, 0.4);
    assert(sky->dirty == 0);

    sky->sun_dir[0] *= 7.0;
    sky->sun_dir[1] *= 7.0;
    sky->sun_dir[2] *= 7.0;
    sky->turbidity = std::numeric_limits<double>::quiet_NaN();
    sky->ground_albedo[1] = std::numeric_limits<double>::infinity();
    sky->resolution = -99;
    sky->dirty = 0;
    assert(rt_sky3d_update(sky, nullptr) == 1);
    assert(sky->resolution == 16 && sky->turbidity == 2.5 && sky->ground_albedo[1] == 0.25);
    sun_length = std::sqrt(sky->sun_dir[0] * sky->sun_dir[0] + sky->sun_dir[1] * sky->sun_dir[1] +
                           sky->sun_dir[2] * sky->sun_dir[2]);
    assert(std::fabs(sun_length - 1.0) < 1e-12 && sky->dirty == 0);
    assert(rt_sky3d_update(sky, rt_vec3_new(0.0, 0.0, 0.0)) == 0);

    auto *wrong_cubemap_sky = static_cast<SkyView *>(rt_sky3d_new());
    void *wrong_cubemap = rt_obj_new_i64(0, 8);
    assert(wrong_cubemap_sky != nullptr && wrong_cubemap != nullptr);
    rt_obj_retain_maybe(wrong_cubemap);
    wrong_cubemap_sky->cubemap = wrong_cubemap;
    wrong_cubemap_sky->dirty = 0;
    assert(rt_sky3d_get_cubemap(wrong_cubemap_sky) == nullptr);
    assert(wrong_cubemap_sky->cubemap == nullptr && wrong_cubemap_sky->dirty == 1);
    release_retained_probe(wrong_cubemap);

    auto *tod = static_cast<TimeOfDayView *>(rt_timeofday3d_new());
    assert(tod != nullptr);
    rt_timeofday3d_set_day_length_seconds(tod, std::numeric_limits<double>::denorm_min());
    rt_timeofday3d_advance(tod, std::numeric_limits<double>::max(), nullptr);
    assert(std::isfinite(tod->hours) && tod->hours >= 0.0 && tod->hours < 24.0);

    tod->hours = std::numeric_limits<double>::quiet_NaN();
    tod->day_length_seconds = -1.0;
    tod->latitude_degrees = std::numeric_limits<double>::infinity();
    tod->refresh_degrees = std::numeric_limits<double>::quiet_NaN();
    tod->last_refresh_dir[0] = std::numeric_limits<double>::quiet_NaN();
    tod->has_refresh_dir = 2;
    assert(rt_timeofday3d_get_hours(tod) == 12.0);
    assert(rt_timeofday3d_get_day_length_seconds(tod) == 0.0);
    assert(rt_timeofday3d_get_latitude_degrees(tod) == 35.0);
    assert(rt_timeofday3d_get_refresh_degrees(tod) == 2.0);
    assert(tod->has_refresh_dir == 0);

    double raw_direction[3] = {9.0, 9.0, 9.0};
    void *wrong_clock = rt_obj_new_i64(0, 8);
    assert(expect_trap_contains(
        [&] { rt_timeofday3d_get_sun_direction_raw(wrong_clock, raw_direction); },
        "invalid clock"));
    assert(raw_direction[0] == 0.0 && raw_direction[1] == 1.0 && raw_direction[2] == 0.0);

    auto *refresh_sky = static_cast<SkyView *>(rt_sky3d_new());
    rt_sky3d_set_resolution(refresh_sky, 16);
    rt_timeofday3d_set_sky(tod, refresh_sky);
    tod->has_refresh_dir = 0;
    rt_timeofday3d_advance(tod, 0.0, wrong_clock);
    assert(tod->has_refresh_dir == 0);
    rt_timeofday3d_advance(tod, 0.0, nullptr);
    assert(tod->has_refresh_dir == 1);

    auto *wrong_refs = static_cast<TimeOfDayView *>(rt_timeofday3d_new());
    void *wrong_ref = rt_obj_new_i64(0, 8);
    assert(wrong_refs != nullptr && wrong_ref != nullptr);
    rt_obj_retain_maybe(wrong_ref);
    wrong_refs->sun_light = wrong_ref;
    wrong_refs->sky = wrong_ref;
    wrong_refs->probe = wrong_ref;
    (void)rt_timeofday3d_get_hours(wrong_refs);
    assert(wrong_refs->sun_light == nullptr && wrong_refs->sky == nullptr &&
           wrong_refs->probe == nullptr);
    release_retained_probe(wrong_ref);
}

static void test_reflection_probe_repairs_bounds_and_restores_render_targets() {
    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *minimum = rt_vec3_new(-1.0, -2.0, -3.0);
    void *maximum = rt_vec3_new(1.0, 2.0, 3.0);
    auto *probe =
        static_cast<ReflectionProbeView *>(rt_reflectionprobe3d_new(origin, minimum, maximum));
    assert(probe != nullptr);
    rt_reflectionprobe3d_set_resolution(probe, 16);

    void *target = rt_rendertarget3d_new(16, 16);
    void *canvas = rt_canvas3d_new_offscreen(target);
    void *scene = rt_scene3d_new();
    assert(target != nullptr && canvas != nullptr && scene != nullptr);
    auto *canvas_view = static_cast<rt_canvas3d *>(canvas);
    assert(canvas_view->render_target_owner == target);
    assert(rt_reflectionprobe3d_capture(probe, canvas, scene) == 1);
    assert(canvas_view->render_target_owner == target);
    assert(probe->capture_dirty == 0 && rt_cubemap3d_is_complete(probe->cubemap));

    rt_reflectionprobe3d_set_resolution(probe, 32);
    assert(probe->resolution == 32 && probe->capture_dirty == 1);
    probe->box_min[0] = -std::numeric_limits<double>::max();
    probe->box_max[0] = std::numeric_limits<double>::max();
    probe->box_min[1] = probe->box_min[2] = -1.0;
    probe->box_max[1] = probe->box_max[2] = 1.0;
    probe->influence_scale = 1.0;
    assert(rt_reflectionprobe3d_contains(probe, origin) == 1);
    probe->influence_scale = std::numeric_limits<double>::quiet_NaN();
    assert(rt_reflectionprobe3d_contains(probe, origin) == 0);

    void *wrong = rt_obj_new_i64(0, 8);
    rt_reflectionprobe3d_set_capture_dirty(probe, 0);
    assert(rt_reflectionprobe3d_capture(probe, wrong, scene) == 0);
    assert(rt_reflectionprobe3d_get_capture_dirty(probe) == 1);
    rt_reflectionprobe3d_set_capture_dirty(probe, 0);
    assert(rt_reflectionprobe3d_capture(probe, canvas, wrong) == 0);
    assert(rt_reflectionprobe3d_get_capture_dirty(probe) == 1);
    canvas_view->in_frame = 1;
    assert(rt_reflectionprobe3d_capture(probe, canvas, scene) == 0);
    canvas_view->in_frame = 0;

    void *saved_target = canvas_view->render_target_owner;
    void *wrong_target = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_target);
    canvas_view->render_target_owner = static_cast<rt_rendertarget3d *>(wrong_target);
    assert(rt_reflectionprobe3d_capture(probe, canvas, scene) == 0);
    canvas_view->render_target_owner = static_cast<rt_rendertarget3d *>(saved_target);
    release_retained_probe(wrong_target);

    auto *wrong_map_probe =
        static_cast<ReflectionProbeView *>(rt_reflectionprobe3d_new(origin, minimum, maximum));
    void *wrong_map = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_map);
    wrong_map_probe->cubemap = wrong_map;
    wrong_map_probe->capture_dirty = 0;
    assert(rt_reflectionprobe3d_get_cubemap(wrong_map_probe) == nullptr);
    assert(wrong_map_probe->cubemap == nullptr && wrong_map_probe->capture_dirty == 1);
    release_retained_probe(wrong_map);

    void *nonfinite = rt_vec3_new(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0);
    assert(expect_trap_contains([&] { rt_reflectionprobe3d_new(nonfinite, minimum, maximum); },
                                "must be Vec3"));
}

static void test_mesh_animation_refs_clear_wrong_class_without_release() {
    auto *mesh = static_cast<rt_mesh3d *>(rt_mesh3d_new());
    assert(mesh != nullptr);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0);

    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, nullptr, -1, nullptr);
    void *wrong_skel = rt_obj_new_i64(0, 8);
    assert(skel != nullptr && wrong_skel != nullptr);
    rt_obj_retain_maybe(wrong_skel);
    mesh->skeleton_ref = wrong_skel;
    rt_mesh3d_set_skeleton(mesh, skel);
    assert(mesh->skeleton_ref == skel);
    release_retained_probe(wrong_skel);
    rt_mesh3d_set_skeleton(mesh, nullptr);
    assert(mesh->skeleton_ref == nullptr);

    void *mt = rt_morphtarget3d_new(1);
    void *wrong_morph = rt_obj_new_i64(0, 8);
    assert(mt != nullptr && wrong_morph != nullptr);
    rt_obj_retain_maybe(wrong_morph);
    mesh->morph_targets_ref = wrong_morph;
    rt_mesh3d_set_morph_targets(mesh, mt);
    assert(mesh->morph_targets_ref == mt);
    release_retained_probe(wrong_morph);
    rt_mesh3d_set_morph_targets(mesh, nullptr);
    assert(mesh->morph_targets_ref == nullptr);

    void *wrong_clone_ref = rt_obj_new_i64(0, 8);
    assert(wrong_clone_ref != nullptr);
    rt_obj_retain_maybe(wrong_clone_ref);
    mesh->skeleton_ref = wrong_clone_ref;
    mesh->morph_targets_ref = wrong_clone_ref;
    auto *clone = static_cast<rt_mesh3d *>(rt_mesh3d_clone(mesh));
    assert(clone != nullptr);
    assert(mesh->skeleton_ref == nullptr);
    assert(mesh->morph_targets_ref == nullptr);
    assert(clone->skeleton_ref == nullptr);
    assert(clone->morph_targets_ref == nullptr);
    release_retained_probe(wrong_clone_ref);

    void *wrong_clear_ref = rt_obj_new_i64(0, 8);
    assert(wrong_clear_ref != nullptr);
    rt_obj_retain_maybe(wrong_clear_ref);
    mesh->skeleton_ref = wrong_clear_ref;
    mesh->morph_targets_ref = wrong_clear_ref;
    rt_mesh3d_clear(mesh);
    assert(mesh->skeleton_ref == nullptr);
    assert(mesh->morph_targets_ref == nullptr);
    release_retained_probe(wrong_clear_ref);
}

static void test_scene_node_private_refs_clear_wrong_class_without_release() {
    auto *finalizer_node = static_cast<rt_scene_node3d *>(rt_scene_node3d_new());
    void *child_wrong = rt_obj_new_i64(0, 8);
    assert(finalizer_node != nullptr && child_wrong != nullptr);
    rt_obj_retain_maybe(child_wrong);
    finalizer_node->children =
        static_cast<rt_scene_node3d **>(std::calloc(1, sizeof(rt_scene_node3d *)));
    assert(finalizer_node->children != nullptr);
    finalizer_node->children[0] = static_cast<rt_scene_node3d *>(child_wrong);
    finalizer_node->child_count = 1;
    finalizer_node->child_capacity = 1;
    if (rt_obj_release_check0(finalizer_node))
        rt_obj_free(finalizer_node);
    release_retained_probe(child_wrong);

    auto *node = static_cast<rt_scene_node3d *>(rt_scene_node3d_new());
    assert(node != nullptr);

    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *wrong_mesh = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_mesh);
    node->mesh = wrong_mesh;
    rt_scene_node3d_set_mesh(node, mesh);
    assert(node->mesh == mesh);
    release_retained_probe(wrong_mesh);
    rt_scene_node3d_set_mesh(node, nullptr);

    void *material = rt_material3d_new_color(1.0, 0.0, 0.0);
    void *wrong_material = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_material);
    node->material = wrong_material;
    rt_scene_node3d_set_material(node, material);
    assert(node->material == material);
    release_retained_probe(wrong_material);
    rt_scene_node3d_set_material(node, nullptr);

    void *light = rt_light3d_new_ambient(1.0, 1.0, 1.0);
    void *wrong_light = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_light);
    node->light = wrong_light;
    rt_scene_node3d_set_light(node, light);
    assert(node->light == light);
    release_retained_probe(wrong_light);
    rt_scene_node3d_set_light(node, nullptr);

    void *wrong_body = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_body);
    node->bound_body = wrong_body;
    rt_scene_node3d_clear_body_binding(node);
    assert(node->bound_body == nullptr);
    assert(rt_scene_node3d_get_body(node) == nullptr);
    release_retained_probe(wrong_body);

    void *wrong_animator = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_animator);
    node->bound_animator = wrong_animator;
    rt_scene_node3d_clear_animator_binding(node);
    assert(node->bound_animator == nullptr);
    assert(rt_scene_node3d_get_animator(node) == nullptr);
    release_retained_probe(wrong_animator);

    void *wrong_node_animator = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_node_animator);
    node->bound_node_animator = wrong_node_animator;
    rt_scene_node3d_bind_node_animator(node, nullptr);
    assert(node->bound_node_animator == nullptr);
    release_retained_probe(wrong_node_animator);

    void *lod_a = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *lod_b = rt_mesh3d_new_box(0.5, 0.5, 0.5);
    void *wrong_lod = rt_obj_new_i64(0, 8);
    rt_scene_node3d_add_lod(node, 5.0, lod_a);
    assert(node->lod_count == 1);
    rt_obj_retain_maybe(wrong_lod);
    node->lod_levels[0].mesh = wrong_lod;
    rt_scene_node3d_add_lod(node, 5.0, lod_b);
    assert(node->lod_levels[0].mesh == lod_b);
    release_retained_probe(wrong_lod);
    rt_scene_node3d_clear_lod(node);

    void *wrong_impostor = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_impostor);
    node->impostor_pixels = wrong_impostor;
    node->impostor_mesh = wrong_impostor;
    node->impostor_material = wrong_impostor;
    rt_scene_node3d_set_impostor(node, 0.0, nullptr);
    assert(node->impostor_pixels == nullptr);
    assert(node->impostor_mesh == nullptr);
    assert(node->impostor_material == nullptr);
    release_retained_probe(wrong_impostor);
}

static void test_billboard_cached_resource_slots_clear_wrong_class_without_release() {
    void *pixels = rt_pixels_new(1, 1);
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    rt_canvas3d canvas = {};
    assert(pixels != nullptr && camera != nullptr);

    auto *sprite = static_cast<SpriteView *>(rt_sprite3d_new(pixels));
    assert(sprite != nullptr);
    void *sprite_wrong = rt_obj_new_i64(0, 8);
    assert(sprite_wrong != nullptr);
    rt_obj_retain_maybe(sprite_wrong);
    sprite->texture = sprite_wrong;
    sprite->cached_mesh = sprite_wrong;
    sprite->cached_material = sprite_wrong;
    sprite->cached_texture = sprite_wrong;
    rt_canvas3d_draw_sprite3d(&canvas, sprite, camera);
    assert(sprite->texture == nullptr);
    assert(sprite->cached_mesh != nullptr);
    assert(sprite->cached_material != nullptr);
    assert(sprite->cached_texture == nullptr);
    assert(rt_obj_class_id(sprite->cached_mesh) == RT_G3D_MESH3D_CLASS_ID);
    assert(rt_obj_class_id(sprite->cached_material) == RT_G3D_MATERIAL3D_CLASS_ID);
    release_retained_probe(sprite_wrong);

    auto *decal = static_cast<DecalView *>(
        rt_decal3d_new(rt_vec3_new(0.0, 0.0, 0.0), rt_vec3_new(0.0, 1.0, 0.0), 1.0, pixels));
    assert(decal != nullptr);
    void *decal_wrong = rt_obj_new_i64(0, 8);
    assert(decal_wrong != nullptr);
    rt_obj_retain_maybe(decal_wrong);
    decal->texture = decal_wrong;
    decal->mesh = decal_wrong;
    decal->material = decal_wrong;
    rt_canvas3d_draw_decal(&canvas, decal);
    assert(decal->texture == nullptr);
    assert(decal->mesh != nullptr);
    assert(decal->material != nullptr);
    assert(rt_obj_class_id(decal->mesh) == RT_G3D_MESH3D_CLASS_ID);
    assert(rt_obj_class_id(decal->material) == RT_G3D_MATERIAL3D_CLASS_ID);
    release_retained_probe(decal_wrong);

    auto *particles = static_cast<ParticleView *>(rt_particles3d_new(4));
    assert(particles != nullptr);
    rt_particles3d_set_lifetime(particles, 10.0, 10.0);
    rt_particles3d_set_speed(particles, 0.0, 0.0);
    rt_particles3d_set_size(particles, 1.0, 1.0);
    rt_particles3d_burst(particles, 1);
    assert(particles->count == 1);
    void *particles_wrong = rt_obj_new_i64(0, 8);
    assert(particles_wrong != nullptr);
    rt_obj_retain_maybe(particles_wrong);
    particles->texture = particles_wrong;
    particles->cached_material = particles_wrong;
    particles->draw_materials[0] = particles_wrong;
    particles->draw_slots_used = 0;
    rt_particles3d_draw(particles, &canvas, camera);
    assert(particles->texture == nullptr);
    assert(particles->cached_material == nullptr);
    assert(particles->draw_materials[0] != nullptr);
    assert(rt_obj_class_id(particles->draw_materials[0]) == RT_G3D_MATERIAL3D_CLASS_ID);
    release_retained_probe(particles_wrong);
}

} // namespace

int main() {
    test_graphics3d_class_ids_are_stable();
    test_texture_atlas_copies_pixels_and_reports_uvs();
    test_material_rejects_non_pixels_texture_handles();
    test_mesh_apis_reject_wrong_class_handles();
    test_camera_center_ray_and_projection_layout();
    test_camera_sanitizes_extreme_projection_and_basis_inputs();
    test_camera_smooth_look_at_syncs_fps_angles();
    test_generated_sphere_has_no_degenerate_triangles();
    test_generated_plane_faces_positive_y();
    test_obj_loader_handles_long_lines();
    test_obj_loader_uses_dot_decimal_independent_of_locale();
    test_scene_particles_water_and_render_targets_reject_wrong_handles();
    test_cubemap_sampling_sanitizes_inputs();
    test_physics_checked_handles_and_trigger_removal_exit();
    test_material_import_texture_transform_clamps_float_uniforms();
    test_material_scalar_setters_clamp_renderer_uniforms();
    test_terrain_heightmap_and_scale_sanitize_inputs();
    test_sprite3d_clamps_frame_anchor_and_scale();
    test_decal3d_normal_and_lifetime_are_sanitized();
    test_path3d_growth_preserves_points();
    test_path3d_looping_includes_closing_segment();
    test_path3d_extreme_points_keep_length_and_direction_finite();
    test_transform_and_path_private_state_repairs_are_cache_coherent();
    test_material_texture_repairs_are_transactional_and_complete();
    test_sprite_decal_and_lensflare_repair_cached_render_state();
    test_navmesh_sample_position_handles_empty_mesh();
    test_navmesh_slope_refilter_and_sloped_height_projection();
    test_navmesh_sample_position_uses_closest_triangle_point();
    test_navmesh_walkable_uses_vertical_tolerance();
    test_capsule_collider_clamps_total_height_to_diameter();
    test_morphtarget_sanitizes_nonfinite_weights_and_deltas();
    test_scene_reparent_preserves_child_and_counts();
    test_scene_deep_hierarchy_traversal_and_transform_clamps();
    test_mesh_bone_weights_are_validated_and_dirty_geometry();
    test_transform_and_instance_batch_sanitize_extreme_matrices();
    test_camera_and_transform_reduce_extreme_motion_inputs();
    test_zero_size_colliders_fallback_to_positive_extents();
    test_obj_loader_recalculates_mixed_missing_normals();
    test_skeleton_bind_pose_and_animation_duration_sanitize();
    test_light_and_material_boolean_state_is_initialized();
    test_light_noop_mutations_and_getters_repair_renderer_state();
    test_instance_batch_repairs_storage_and_preserves_relative_motion_history();
    test_physics_joints_deduplicate_and_raycast_is_true_ray();
    test_raycast_math_and_backend_guards_are_strict();
    test_physics_oriented_boxes_do_not_collide_as_aabbs();
    test_physics_raycast_uses_mesh_triangles_not_mesh_aabb();
    test_physics_mesh_box_collision_uses_triangles_not_mesh_aabb();
    test_physics_sweeps_handle_thin_geometry_and_long_capsules();
    test_terrain_water_and_vegetation_zero_dt_paths();
    test_vegetation_density_map_edges_are_exact();
    test_vegetation_extreme_wind_and_corrupt_instances_are_compacted();
    test_particles_and_water_numeric_knobs_are_bounded();
    test_water_extreme_wave_directions_and_cached_resources_are_repaired();
    test_water_wrong_class_private_resources_clear_without_release();
    test_vegetation_wrong_class_private_resources_clear_without_release();
    test_texture_atlas_repairs_corrupt_storage_and_preserves_stable_ids();
    test_sky_and_timeofday_repair_extreme_and_wrong_class_state();
    test_reflection_probe_repairs_bounds_and_restores_render_targets();
    test_mesh_animation_refs_clear_wrong_class_without_release();
    test_scene_node_private_refs_clear_wrong_class_without_release();
    test_billboard_cached_resource_slots_clear_wrong_class_without_release();
    std::printf("RTGraphics3DRobustnessTests passed.\n");
    return 0;
}
