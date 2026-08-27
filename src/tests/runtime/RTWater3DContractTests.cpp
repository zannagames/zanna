//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTWater3DContractTests.cpp
// Purpose: Isolated Water3D runtime contract tests for mesh sizing, material
//   wiring, draw-state preservation, numeric sanitization, and private-state repair.
// Key invariants:
//   - WaterView mirrors rt_water3d's private layout for targeted white-box checks.
//   - Stack fixture handles are accepted only through the isolated test stubs.
//   - Corrupted legacy resource mirrors cannot replace independently retained owners.
// Ownership/Lifetime:
//   - Test stubs allocate runtime objects with calloc and free them through
//     rt_obj_free when the code under test releases owned references.
// Links: src/runtime/graphics/3d/world/rt_water3d.c
//
//===----------------------------------------------------------------------===//

extern "C" {
#include "rt_canvas3d_internal.h"
#include "rt_pixels.h"
#include "rt_water3d.h"
}

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct StubMaterial {
    double color[3] = {0.0, 0.0, 0.0};
    double alpha = 0.0;
    double reflectivity = 0.0;
    void *texture = nullptr;
    void *normal_map = nullptr;
    void *env_map = nullptr;
    int8_t double_sided = 0;
};

int g_draw_mesh_calls = 0;
int g_backface_cull_off = 0;
int g_backface_cull_on = 0;
int g_backface_cull_values[8] = {0};
int g_backface_cull_call_count = 0;
int g_dummy_texture = 1;
int g_dummy_normal = 2;
int g_dummy_env = 3;
int g_dummy_incomplete_env = 4;
double g_last_vec3[3] = {0.0, 0.0, 0.0};
void *g_stub_meshes[16] = {nullptr};
int g_stub_mesh_count = 0;
void *g_stub_materials[16] = {nullptr};
int g_stub_material_count = 0;

struct WaterWaveView {
    double dir[2];
    double speed;
    double amplitude;
    double frequency;
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
    WaterWaveView waves[8];
    int32_t wave_count;
    int32_t resolution;
    int8_t mesh_dirty;
    double sim_distance;
    double last_camera_pos[3];
    int8_t has_camera_pos;
};

static bool stub_pointer_tracked(void *const *items, int count, void *value) {
    for (int i = 0; i < count; i++) {
        if (items[i] == value)
            return true;
    }
    return false;
}

static void stub_track_pointer(void **items, int *count, int capacity, void *value) {
    if (!items || !count || !value || stub_pointer_tracked(items, *count, value))
        return;
    if (*count < capacity)
        items[(*count)++] = value;
}

static void stub_untrack_pointer(void **items, int *count, void *value) {
    if (!items || !count || !value)
        return;
    for (int i = 0; i < *count; i++) {
        if (items[i] == value) {
            items[i] = items[*count - 1];
            items[*count - 1] = nullptr;
            (*count)--;
            return;
        }
    }
}

} // namespace

extern "C" void *rt_obj_new_i64(int64_t, int64_t byte_size) {
    return std::calloc(1, static_cast<size_t>(byte_size));
}

extern "C" int64_t rt_obj_class_id(void *obj) {
    if (obj == &g_dummy_texture || obj == &g_dummy_normal)
        return RT_PIXELS_CLASS_ID;
    if (obj == &g_dummy_env || obj == &g_dummy_incomplete_env)
        return RT_G3D_CUBEMAP3D_CLASS_ID;
    if (stub_pointer_tracked(g_stub_meshes, g_stub_mesh_count, obj))
        return RT_G3D_MESH3D_CLASS_ID;
    if (stub_pointer_tracked(g_stub_materials, g_stub_material_count, obj))
        return RT_G3D_MATERIAL3D_CLASS_ID;
    return RT_G3D_WATER3D_CLASS_ID;
}

extern "C" int8_t rt_obj_is_instance(void *obj, int64_t class_id, size_t) {
    return obj && rt_obj_class_id(obj) == class_id;
}

extern "C" int rt_cubemap3d_is_complete(void *cubemap) {
    return cubemap == &g_dummy_env ? 1 : 0;
}

extern "C" int8_t rt_heap_is_payload(void *) {
    return 0;
}

extern "C" void rt_obj_set_finalizer(void *, void (*)(void *)) {}

extern "C" void rt_obj_retain_maybe(void *) {}

/* ADR 0227 readback getters box positions/colors as Vec3; the isolated
 * contract build links no math objects, so a null-returning stub keeps the
 * target minimal (no contract case dereferences the boxed value). */
extern "C" void *rt_vec3_new(double x, double y, double z) {
    g_last_vec3[0] = x;
    g_last_vec3[1] = y;
    g_last_vec3[2] = z;
    return nullptr;
}

extern "C" int32_t rt_obj_release_check0(void *p) {
    if (p == &g_dummy_texture || p == &g_dummy_normal || p == &g_dummy_env ||
        p == &g_dummy_incomplete_env)
        return 0;
    return 1;
}

extern "C" void rt_obj_free(void *p) {
    stub_untrack_pointer(g_stub_meshes, &g_stub_mesh_count, p);
    stub_untrack_pointer(g_stub_materials, &g_stub_material_count, p);
    std::free(p);
}

extern "C" void rt_trap(const char *) {
    std::abort();
}

extern "C" void rt_mesh3d_note_global_geometry_change(void) {}

/**
 * @brief Satisfy the isolated Water3D target's retained-geometry invalidation contract.
 *
 * Water3D calls this hook after rewriting its generated grid mesh. The contract test
 * does not exercise retained snapshots or acceleration structures, so there is no
 * cache to discard in the local mesh stub.
 *
 * @param mesh Generated mesh whose production retained state would be invalidated.
 */
extern "C" void rt_mesh3d_invalidate_retained_geometry(rt_mesh3d *mesh) {
    (void)mesh;
}

extern "C" void *rt_mesh3d_new(void) {
    void *mesh = std::calloc(1, sizeof(rt_mesh3d));
    stub_track_pointer(g_stub_meshes,
                       &g_stub_mesh_count,
                       static_cast<int>(sizeof(g_stub_meshes) / sizeof(g_stub_meshes[0])),
                       mesh);
    return mesh;
}

extern "C" void rt_mesh3d_clear(void *m) {
    rt_mesh3d *mesh = static_cast<rt_mesh3d *>(m);
    if (!mesh)
        return;
    mesh->vertex_count = 0;
    mesh->index_count = 0;
}

extern "C" void rt_mesh3d_add_vertex(
    void *m, double, double, double, double, double, double, double, double) {
    static_cast<rt_mesh3d *>(m)->vertex_count++;
}

extern "C" void rt_mesh3d_add_triangle(void *m, int64_t, int64_t, int64_t) {
    static_cast<rt_mesh3d *>(m)->index_count += 3;
}

/* The isolated target deliberately exercises Water3D's software-compatible fallback without
 * linking the animation subsystem. Production builds provide these through MorphTarget3D. */
extern "C" void *rt_morphtarget3d_new_packed_internal(int64_t, int32_t, float *, float *) {
    return nullptr;
}

extern "C" void rt_morphtarget3d_set_weight(void *, int64_t, double) {}

extern "C" void rt_canvas3d_draw_mesh_matrix_morphed_bounds(void *,
                                                            void *,
                                                            const double *,
                                                            void *,
                                                            const void *,
                                                            void *,
                                                            const float *,
                                                            const float *,
                                                            int8_t,
                                                            int8_t) {}

extern "C" void *rt_mat4_identity(void) {
    static double identity[16] = {
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    return identity;
}

extern "C" void rt_canvas3d_draw_mesh(void *, void *, void *, void *) {
    g_draw_mesh_calls++;
}

extern "C" void rt_canvas3d_draw_mesh_matrix(void *canvas,
                                             void *mesh,
                                             const double *,
                                             void *material) {
    rt_canvas3d_draw_mesh(canvas, mesh, nullptr, material);
}

extern "C" void rt_canvas3d_draw_mesh_matrix_keyed_bounds(void *canvas,
                                                          void *mesh,
                                                          const double *,
                                                          void *material,
                                                          const void *,
                                                          const float *,
                                                          const float *,
                                                          const float *,
                                                          const float *,
                                                          int8_t,
                                                          int8_t,
                                                          float) {
    rt_canvas3d_draw_mesh(canvas, mesh, nullptr, material);
}

extern "C" void vgfx3d_compute_mesh_aabb(
    const void *, uint32_t, uint32_t, float out_min[3], float out_max[3]) {
    out_min[0] = out_min[1] = out_min[2] = 0.0f;
    out_max[0] = out_max[1] = out_max[2] = 1.0f;
}

extern "C" void *rt_material3d_new_color(double, double, double) {
    void *material = std::calloc(1, sizeof(StubMaterial));
    stub_track_pointer(g_stub_materials,
                       &g_stub_material_count,
                       static_cast<int>(sizeof(g_stub_materials) / sizeof(g_stub_materials[0])),
                       material);
    return material;
}

extern "C" void rt_material3d_set_alpha(void *m, double a) {
    static_cast<StubMaterial *>(m)->alpha = a;
}

extern "C" void rt_material3d_set_double_sided(void *m, int8_t enabled) {
    static_cast<StubMaterial *>(m)->double_sided = enabled ? 1 : 0;
}

extern "C" void rt_material3d_set_shininess(void *, double) {}

/* Camera position read used by the SetSimDistance gate; the isolated harness
 * has no camera module, so the gate stays inactive. */
extern "C" int8_t rt_camera3d_get_position_components(void *, double *, double *, double *) {
    return 0;
}

extern "C" void rt_material3d_set_ssr_enabled(void *, int8_t) {}

extern "C" void rt_material3d_set_texture(void *m, void *tex) {
    static_cast<StubMaterial *>(m)->texture = tex;
}

extern "C" void rt_material3d_set_normal_map(void *m, void *tex) {
    static_cast<StubMaterial *>(m)->normal_map = tex;
}

extern "C" void rt_material3d_set_env_map(void *m, void *cubemap) {
    static_cast<StubMaterial *>(m)->env_map = cubemap;
}

extern "C" void rt_material3d_set_reflectivity(void *m, double r) {
    static_cast<StubMaterial *>(m)->reflectivity = r;
}

extern "C" void rt_material3d_set_color(void *m, double r, double g, double b) {
    auto *material = static_cast<StubMaterial *>(m);
    material->color[0] = r;
    material->color[1] = g;
    material->color[2] = b;
}

extern "C" void rt_canvas3d_set_backface_cull(void *canvas, int8_t enabled) {
    if (canvas)
        static_cast<rt_canvas3d *>(canvas)->backface_cull = enabled;
    if (g_backface_cull_call_count <
        (int)(sizeof(g_backface_cull_values) / sizeof(g_backface_cull_values[0]))) {
        g_backface_cull_values[g_backface_cull_call_count] = enabled ? 1 : 0;
    }
    g_backface_cull_call_count++;
    if (enabled)
        g_backface_cull_on++;
    else
        g_backface_cull_off++;
}

static void test_resolution_clamp_drives_mesh_size() {
    void *water = rt_water3d_new(10.0, 10.0);
    assert(water != nullptr);

    rt_water3d_set_resolution(water, 4);
    rt_water3d_update(water, 0.1);

    auto *mesh = static_cast<rt_mesh3d *>(static_cast<WaterView *>(water)->mesh);
    assert(mesh != nullptr);
    assert(mesh->vertex_count == 81);
    assert(mesh->index_count == 8 * 8 * 6);
}

static void test_material_wiring_and_reflectivity() {
    void *water = rt_water3d_new(8.0, 8.0);

    rt_water3d_set_texture(water, &g_dummy_texture);
    rt_water3d_set_normal_map(water, &g_dummy_normal);
    rt_water3d_set_reflectivity(water, 0.75);
    rt_water3d_update(water, 0.1);

    auto *material = static_cast<StubMaterial *>(static_cast<WaterView *>(water)->material);
    assert(material != nullptr);
    assert(material->texture == &g_dummy_texture);
    assert(material->normal_map == &g_dummy_normal);
    assert(material->double_sided == 1);
    assert(material->reflectivity == 0.0);

    rt_water3d_set_env_map(water, &g_dummy_env);
    rt_water3d_update(water, 0.1);
    assert(material->env_map == &g_dummy_env);
    assert(material->reflectivity == 0.75);

    rt_water3d_set_reflectivity(water, 5.0);
    rt_water3d_update(water, 0.1);
    assert(material->reflectivity == 1.0);

    rt_water3d_set_reflectivity(water, 0.65);
    rt_water3d_set_env_map(water, nullptr);
    assert(material->env_map == nullptr);
    assert(material->reflectivity == 0.0);
    rt_water3d_set_env_map(water, &g_dummy_env);
    assert(material->env_map == &g_dummy_env);
    assert(material->reflectivity == 0.65);

    auto *view = static_cast<WaterView *>(water);
    view->env_map = &g_dummy_incomplete_env;
    material->env_map = &g_dummy_incomplete_env;
    material->reflectivity = 0.65;
    rt_water3d_set_env_map(water, &g_dummy_texture);
    assert(view->env_map == &g_dummy_env);
    assert(material->env_map == &g_dummy_env);
    assert(material->reflectivity == 0.65);

    view->env_map = &g_dummy_incomplete_env;
    material->env_map = &g_dummy_incomplete_env;
    material->reflectivity = 0.65;
    rt_water3d_set_reflectivity(water, 0.5);
    assert(view->env_map == &g_dummy_env);
    assert(material->env_map == &g_dummy_env);
    assert(material->reflectivity == 0.5);
}

static void test_resource_mirrors_restore_retained_owners() {
    void *water = rt_water3d_new(8.0, 8.0);
    auto *view = static_cast<WaterView *>(water);
    rt_water3d_set_texture(water, &g_dummy_texture);
    rt_water3d_set_normal_map(water, &g_dummy_normal);
    rt_water3d_set_env_map(water, &g_dummy_env);
    rt_water3d_update(water, 0.1);

    void *owned_mesh = view->mesh;
    void *owned_material = view->material;
    void *foreign_mesh = rt_mesh3d_new();
    void *foreign_material = rt_material3d_new_color(0.0, 0.0, 0.0);
    view->mesh = foreign_mesh;
    view->material = foreign_material;
    view->texture = &g_dummy_normal;
    view->normal_map = &g_dummy_texture;
    view->env_map = &g_dummy_incomplete_env;

    assert(rt_water3d_get_texture(water) == &g_dummy_texture);
    assert(rt_water3d_get_normal_map(water) == &g_dummy_normal);
    assert(rt_water3d_get_env_map(water) == &g_dummy_env);
    rt_water3d_update(water, 0.0);
    assert(view->mesh == owned_mesh);
    assert(view->material == owned_material);
}

static void test_corrupted_readback_state_is_persistently_repaired() {
    const double nan = std::nan("");
    void *water = rt_water3d_new(8.0, 8.0);
    auto *view = static_cast<WaterView *>(water);
    rt_water3d_update(water, 0.1);

    view->width = nan;
    view->depth = -10.0;
    view->height = nan;
    view->center_x = INFINITY;
    view->center_z = -INFINITY;
    view->wave_speed = INFINITY;
    view->wave_amplitude = -1.0;
    view->wave_frequency = nan;
    view->color[0] = nan;
    view->color[1] = 4.0;
    view->color[2] = -2.0;
    view->alpha = nan;
    view->reflectivity = INFINITY;
    view->resolution = INT32_MAX;
    view->sim_distance = -1.0;

    assert(rt_water3d_get_height(water) == 0.0);
    assert(rt_water3d_get_wave_speed(water) == 0.0);
    assert(rt_water3d_get_wave_amplitude(water) == 0.0);
    assert(rt_water3d_get_wave_frequency(water) == 0.0);
    assert(rt_water3d_get_alpha(water) == 0.0);
    assert(rt_water3d_get_reflectivity(water) == 0.0);
    assert(rt_water3d_get_resolution(water) == 64);
    assert(rt_water3d_get_sim_distance(water) == 0.0);

    (void)rt_water3d_get_position(water);
    assert(g_last_vec3[0] == 0.0);
    assert(g_last_vec3[1] == 0.0);
    assert(g_last_vec3[2] == 0.0);
    (void)rt_water3d_get_color(water);
    assert(g_last_vec3[0] == 0.0);
    assert(g_last_vec3[1] == 1.0);
    assert(g_last_vec3[2] == 0.0);

    assert(view->width == 1.0);
    assert(view->depth == 1.0);
    assert(view->resolution == 64);
}

static void test_distance_gate_cannot_bypass_state_repair() {
    const double nan = std::nan("");
    void *water = rt_water3d_new(8.0, 8.0);
    auto *view = static_cast<WaterView *>(water);
    rt_water3d_update(water, 0.1);

    view->sim_distance = 1.0;
    view->has_camera_pos = 1;
    view->last_camera_pos[0] = 1000.0;
    view->last_camera_pos[1] = 1000.0;
    view->last_camera_pos[2] = 1000.0;
    view->wave_speed = nan;
    view->wave_amplitude = -2.0;
    view->wave_frequency = INFINITY;
    view->alpha = 5.0;
    view->resolution = -1;
    view->mesh_dirty = 0;

    rt_water3d_update(water, 0.1);
    assert(view->wave_speed == 0.0);
    assert(view->wave_amplitude == 0.0);
    assert(view->wave_frequency == 0.0);
    assert(view->alpha == 1.0);
    assert(view->resolution == 64);
}

static void test_color_edits_reach_an_existing_material_without_simulation() {
    void *water = rt_water3d_new(8.0, 8.0);
    auto *view = static_cast<WaterView *>(water);
    rt_water3d_update(water, 0.1);
    auto *material = static_cast<StubMaterial *>(view->material);
    assert(material != nullptr);

    rt_water3d_set_color(water, 0.8, 0.6, 0.4, 0.25);
    rt_water3d_update(water, 0.0);
    assert(material->color[0] == 0.8);
    assert(material->color[1] == 0.6);
    assert(material->color[2] == 0.4);
    assert(material->alpha == 0.25);
}

static void test_draw_keeps_canvas_backface_state() {
    void *water = rt_water3d_new(4.0, 4.0);
    rt_canvas3d canvas = {};
    rt_water3d_update(water, 0.1);

    canvas.backface_cull = 0;
    g_draw_mesh_calls = 0;
    g_backface_cull_off = 0;
    g_backface_cull_on = 0;
    g_backface_cull_call_count = 0;
    std::memset(g_backface_cull_values, 0, sizeof(g_backface_cull_values));
    rt_canvas3d_draw_water(&canvas, water, nullptr);

    assert(g_draw_mesh_calls == 1);
    assert(g_backface_cull_off == 0);
    assert(g_backface_cull_on == 0);
    assert(g_backface_cull_call_count == 0);
    assert(canvas.backface_cull == 0);
}

static void test_numeric_inputs_are_sanitized() {
    const double nan = std::nan("");
    void *water = rt_water3d_new(nan, -8.0);
    auto *view = static_cast<WaterView *>(water);
    assert(view->width == 1.0);
    assert(view->depth == 1.0);

    rt_water3d_set_height(water, nan);
    assert(view->height == 0.0);

    rt_water3d_set_wave_params(water, nan, -1.0, nan);
    assert(view->wave_speed == 0.0);
    assert(view->wave_amplitude == 0.0);
    assert(view->wave_frequency == 0.0);

    rt_water3d_set_color(water, nan, 2.0, -4.0, nan);
    assert(view->color[0] == 0.0);
    assert(view->color[1] == 1.0);
    assert(view->color[2] == 0.0);
    assert(view->alpha == 0.0);

    rt_water3d_add_wave(water, nan, 0.0, 1.0, 0.5, 4.0);
    assert(view->wave_count == 0);
    rt_water3d_add_wave(water, 1.0, 0.0, 2.0, 0.5, 4.0);
    assert(view->wave_count == 1);
}

int main() {
    test_resolution_clamp_drives_mesh_size();
    test_material_wiring_and_reflectivity();
    test_resource_mirrors_restore_retained_owners();
    test_draw_keeps_canvas_backface_state();
    test_numeric_inputs_are_sanitized();
    test_corrupted_readback_state_is_persistently_repaired();
    test_distance_gate_cannot_bypass_state_repair();
    test_color_edits_reach_an_existing_material_without_simulation();
    std::printf("RTWater3DContractTests passed.\n");
    return 0;
}
