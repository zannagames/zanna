//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTVegetation3DContractTests.cpp
// Purpose: Isolated behavioral contract test for the Graphics3D vegetation
//   subsystem (src/runtime/graphics/3d/world/rt_vegetation3d.c). Pins blade-mesh
//   construction, deterministic density-gated population (configurable LCG seed),
//   wind-time advance, distance LOD culling, and the single instanced draw
//   submission — without a GPU backend, GC, or mesh allocator.
// Key invariants:
//   - rt_vegetation3d.c is compiled against the stubs below; only its own logic
//     is under test. VegetationView mirrors only the unit under test; Terrain3D
//     metadata crosses the same opaque descriptor boundary used in production.
//   - Population is deterministic: a no-density populate(count) yields exactly
//     `count` blades; a full-density (R=255) map keeps all; a zero map keeps none.
//   - Private allocation/resource owners survive corruption of legacy pointer/count mirrors.
//   - Malformed CSR offsets fail over to the bounded linear culling path.
// Ownership/Lifetime:
//   - Fixtures (terrain, density Pixels) are caller-owned and never freed by the
//     subsystem in these tests (no finalize/reassign paths exercised).
// Links: rt_vegetation3d.c, RTParticles3DContractTests.cpp (sibling pattern)
//
//===----------------------------------------------------------------------===//

extern "C" {
#include "rt_canvas3d_internal.h"
#include "rt_pixels_internal.h"
#include "rt_terrain3d_internal.h"
#include "rt_vegetation3d.h"
}

#include <cassert>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

std::jmp_buf g_env;
const char *g_last_trap = nullptr;
bool g_expect_trap = false;

// Tracked handle sets so rt_obj_class_id reports the right class per pointer.
void *g_meshes[128] = {nullptr};
int g_mesh_count = 0;
void *g_materials[128] = {nullptr};
int g_material_count = 0;
void *g_terrain = nullptr;

// Instanced-draw interception records.
int g_instanced_batch_calls = 0;
int g_last_instance_count = 0;
int g_cull_during_batch = -1;
int g_mesh_clear_calls = 0;

// White-box mirror of the private rt_vegetation3d struct (rt_vegetation3d.c).
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
    uint32_t scatter_seed;
    int32_t *grid_cell_start;
    int32_t *grid_indices;
    int32_t grid_cells_x;
    int32_t grid_cells_z;
    float grid_min_x;
    float grid_min_z;
    float grid_cell_w;
    float grid_cell_d;
    int32_t grid_ready;
};

struct FakeTerrain {
    int32_t width;
    int32_t depth;
    double scale[3];
};

bool tracked(void *const *items, int count, void *value) {
    for (int i = 0; i < count; i++) {
        if (items[i] == value)
            return true;
    }
    return false;
}

void track(void **items, int *count, int capacity, void *value) {
    if (!value || tracked(items, *count, value))
        return;
    if (*count < capacity)
        items[(*count)++] = value;
}

void reset_draw_records() {
    g_instanced_batch_calls = 0;
    g_last_instance_count = 0;
    g_cull_during_batch = -1;
}

// Build a density Pixels fixture whose R channel is `r` for every texel.
void fill_density(rt_pixels_impl *px, uint32_t *data, int64_t w, int64_t h, uint32_t r) {
    px->width = w;
    px->height = h;
    px->data = data;
    px->generation = 0;
    px->cache_identity = 0;
    uint32_t texel = (r & 0xFFu) << 24; // RRGGBBAA, R channel only
    for (int64_t i = 0; i < w * h; i++)
        data[i] = texel;
}

} // namespace

extern "C" void *rt_obj_new_i64(int64_t, int64_t byte_size) {
    return std::calloc(1, static_cast<size_t>(byte_size));
}

extern "C" int64_t rt_obj_class_id(void *p) {
    if (!p)
        return 0;
    if (p == g_terrain)
        return RT_G3D_TERRAIN3D_CLASS_ID;
    if (tracked(g_meshes, g_mesh_count, p))
        return RT_G3D_MESH3D_CLASS_ID;
    if (tracked(g_materials, g_material_count, p))
        return RT_G3D_MATERIAL3D_CLASS_ID;
    return RT_G3D_VEGETATION3D_CLASS_ID;
}

extern "C" int8_t rt_obj_is_instance(void *obj, int64_t class_id, size_t) {
    return obj && rt_obj_class_id(obj) == class_id;
}

extern "C" int8_t rt_heap_is_payload(void *) {
    return 0;
}

extern "C" void rt_mesh3d_note_global_geometry_change(void) {}

/**
 * @brief Satisfy the isolated Vegetation3D target's retained-geometry invalidation contract.
 *
 * The production hook retires cached snapshots after blade geometry changes. This
 * test target owns only a minimal mesh stub and never creates retained geometry,
 * leaving the invalidation operation intentionally empty.
 *
 * @param mesh Blade mesh whose production retained state would be invalidated.
 */
extern "C" void rt_mesh3d_invalidate_retained_geometry(rt_mesh3d *mesh) {
    (void)mesh;
}

extern "C" void rt_obj_set_finalizer(void *, void (*)(void *)) {}

extern "C" void rt_obj_retain_maybe(void *) {}

extern "C" int32_t rt_obj_release_check0(void *) {
    return 1;
}

extern "C" void rt_obj_free(void *p) {
    std::free(p);
}

extern "C" void rt_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_env, 1);
    std::abort();
}

extern "C" void *rt_mesh3d_new(void) {
    void *m = std::calloc(1, sizeof(rt_mesh3d));
    track(g_meshes, &g_mesh_count, 128, m);
    return m;
}

extern "C" void rt_mesh3d_add_vertex(
    void *m, double, double, double, double, double, double, double, double) {
    if (m)
        static_cast<rt_mesh3d *>(m)->vertex_count++;
}

extern "C" void rt_mesh3d_add_triangle(void *m, int64_t, int64_t, int64_t) {
    if (m)
        static_cast<rt_mesh3d *>(m)->index_count += 3;
}

extern "C" void rt_mesh3d_clear(void *m) {
    if (!m)
        return;
    rt_mesh3d *mesh = static_cast<rt_mesh3d *>(m);
    g_mesh_clear_calls++;
    mesh->vertex_count = 0;
    mesh->index_count = 0;
}

extern "C" void *rt_material3d_new(void) {
    // Sized generously: vegetation writes real rt_material3d fields (e.g.
    // double_sided) through this handle, so the fixture must cover the full
    // struct, not just an identity-sized stub (ASan caught a 64-byte stub
    // overflowing once the double-sided write landed).
    void *mat = std::calloc(1, 1024);
    track(g_materials, &g_material_count, 128, mat);
    return mat;
}

extern "C" void rt_material3d_set_texture(void *, void *) {}

extern "C" void rt_material3d_set_unlit(void *, int8_t) {}

extern "C" double rt_terrain3d_get_height_at(void *, double, double) {
    return 0.0;
}

/// @brief Test double for Terrain3D's layout-independent metadata query.
/// @param terrain Borrowed FakeTerrain registered as the active terrain fixture.
/// @param out_info Caller-owned descriptor that is zeroed on rejection.
/// @return `1` for a valid active fixture, otherwise `0`.
/// @details Keeping this stub independent of Terrain3D's private payload makes
///   the contract test fail if Vegetation3D resumes reading payload offsets.
extern "C" int8_t rt_terrain3d_get_grid_info_internal(void *terrain,
                                                      rt_terrain3d_grid_info *out_info) {
    if (!out_info)
        return 0;
    std::memset(out_info, 0, sizeof(*out_info));
    if (!terrain || terrain != g_terrain)
        return 0;
    auto *fixture = static_cast<FakeTerrain *>(terrain);
    if (fixture->width < 2 || fixture->depth < 2 || !std::isfinite(fixture->scale[0]) ||
        fixture->scale[0] <= 0.0 || !std::isfinite(fixture->scale[1]) ||
        !std::isfinite(fixture->scale[2]) || fixture->scale[2] <= 0.0)
        return 0;
    out_info->width = fixture->width;
    out_info->depth = fixture->depth;
    out_info->spacing_x = fixture->scale[0];
    out_info->height_scale = fixture->scale[1];
    out_info->spacing_z = fixture->scale[2];
    out_info->extent_x = static_cast<double>(fixture->width - 1) * fixture->scale[0];
    out_info->extent_z = static_cast<double>(fixture->depth - 1) * fixture->scale[2];
    return 1;
}

extern "C" void rt_canvas3d_queue_instanced_batch(void *canvas_obj,
                                                  void *,
                                                  void *,
                                                  const float *,
                                                  int32_t instance_count,
                                                  const float *,
                                                  int8_t) {
    g_instanced_batch_calls++;
    g_last_instance_count = static_cast<int>(instance_count);
    g_cull_during_batch = canvas_obj ? static_cast<rt_canvas3d *>(canvas_obj)->backface_cull : -1;
}

extern "C" void rt_canvas3d_queue_instanced_batch_bounds(void *canvas_obj,
                                                         void *mesh_obj,
                                                         void *material_obj,
                                                         const float *instance_matrices,
                                                         int32_t instance_count,
                                                         const float *prev_instance_matrices,
                                                         int8_t has_prev_instance_matrices,
                                                         const float *,
                                                         const float *,
                                                         int8_t,
                                                         int8_t) {
    rt_canvas3d_queue_instanced_batch(canvas_obj,
                                      mesh_obj,
                                      material_obj,
                                      instance_matrices,
                                      instance_count,
                                      prev_instance_matrices,
                                      has_prev_instance_matrices);
}

extern "C" void vgfx3d_compute_mesh_aabb(
    const void *, uint32_t, uint32_t, float out_min[3], float out_max[3]) {
    out_min[0] = out_min[1] = out_min[2] = 0.0f;
    out_max[0] = out_max[1] = out_max[2] = 1.0f;
}

static FakeTerrain make_terrain(int32_t w, int32_t d) {
    FakeTerrain t = {};
    t.width = w;
    t.depth = d;
    t.scale[0] = 1.0;
    t.scale[1] = 1.0;
    t.scale[2] = 1.0;
    return t;
}

/// @brief Verify population respects sample-interval extents under non-unit scale.
/// @details A Terrain3D with N samples ends at sample N-1. This catches both the
///   former private-layout cast and the former `dimension * spacing` off-by-one.
static void test_populate_uses_opaque_scaled_terrain_extents() {
    void *veg = rt_vegetation3d_new(nullptr);
    auto *view = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(3, 4);
    terrain.scale[0] = 2.5;
    terrain.scale[1] = 7.0;
    terrain.scale[2] = 3.0;
    g_terrain = &terrain;

    rt_vegetation3d_set_seed(veg, 0x12345678);
    rt_vegetation3d_populate(veg, &terrain, 512);

    assert(view->total_count == 512);
    for (int32_t i = 0; i < view->total_count; i++) {
        const double x = view->positions[i * 3 + 0];
        const double z = view->positions[i * 3 + 2];
        assert(x >= 0.0 && x <= 5.0);
        assert(z >= 0.0 && z <= 9.0);
    }
    g_terrain = nullptr;
}

static rt_canvas3d make_frame_canvas() {
    static int dummy_backend_storage = 0;
    rt_canvas3d canvas = {};
    canvas.in_frame = 1;
    canvas.frame_is_2d = 0;
    canvas.backface_cull = 1;
    canvas.backend = reinterpret_cast<const vgfx3d_backend_t *>(&dummy_backend_storage);
    return canvas;
}

static void test_new_builds_blade_mesh_and_defaults() {
    void *veg = rt_vegetation3d_new(nullptr);
    assert(veg != nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);

    assert(v->blade_mesh != nullptr);
    assert(v->blade_material != nullptr);

    // Cross-billboard blade: 2 quads = 8 vertices, 4 triangles = 12 indices.
    rt_mesh3d *mesh = static_cast<rt_mesh3d *>(v->blade_mesh);
    assert(mesh->vertex_count == 8);
    assert(mesh->index_count == 12);

    assert(v->blade_width == 0.4);
    assert(v->blade_height == 1.2);
    assert(v->size_variation == 0.3);
    assert(v->wind_speed == 2.0);
    assert(v->wind_strength == 0.15);
    assert(v->lod_near == 40.0f);
    assert(v->lod_far == 100.0f);
    assert(v->total_count == 0);
    assert(v->visible_count == 0);
    assert(v->scatter_seed != 0);
}

static void test_populate_scatters_requested_count() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 100);
    assert(v->total_count == 100); // no density map => every candidate placed

    rt_vegetation3d_populate(veg, &terrain, 250);
    assert(v->total_count == 250); // re-populate resets and refills deterministically
    g_terrain = nullptr;
}

static void test_seed_controls_population_layout() {
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    void *a_obj = rt_vegetation3d_new(nullptr);
    void *b_obj = rt_vegetation3d_new(nullptr);
    void *c_obj = rt_vegetation3d_new(nullptr);
    auto *a = static_cast<VegetationView *>(a_obj);
    auto *b = static_cast<VegetationView *>(b_obj);
    auto *c = static_cast<VegetationView *>(c_obj);

    rt_vegetation3d_set_seed(a_obj, 12345);
    rt_vegetation3d_set_seed(b_obj, 12345);
    rt_vegetation3d_set_seed(c_obj, 67890);
    rt_vegetation3d_populate(a_obj, &terrain, 32);
    rt_vegetation3d_populate(b_obj, &terrain, 32);
    rt_vegetation3d_populate(c_obj, &terrain, 32);

    assert(a->total_count == 32);
    assert(b->total_count == 32);
    assert(c->total_count == 32);
    assert(std::memcmp(a->positions, b->positions, sizeof(float) * 32 * 3) == 0);
    assert(std::memcmp(a->base_transforms, b->base_transforms, sizeof(float) * 32 * 16) == 0);
    bool different = std::memcmp(a->positions, c->positions, sizeof(float) * 32 * 3) != 0;
    assert(different);

    g_terrain = nullptr;
}

static void test_populate_nonpositive_count_clears() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 0);
    assert(v->total_count == 0);
    rt_vegetation3d_populate(veg, &terrain, -5);
    assert(v->total_count == 0);
    g_terrain = nullptr;
}

static void test_populate_overflow_count_traps() {
    void *veg = rt_vegetation3d_new(nullptr);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    g_last_trap = nullptr;
    g_expect_trap = true;
    if (setjmp(g_env) == 0) {
        rt_vegetation3d_populate(veg, &terrain, 2000001); // > VEGETATION3D_MAX_BLADES
        assert(false && "expected rt_trap for oversized blade count");
    }
    g_expect_trap = false;
    assert(g_last_trap != nullptr);
    assert(std::strstr(g_last_trap, "count exceeds supported range") != nullptr);
    g_terrain = nullptr;
}

static void test_density_map_gates_population() {
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    uint32_t data[64];
    rt_pixels_impl density = {};

    // Full density (R=255) keeps every candidate.
    void *full = rt_vegetation3d_new(nullptr);
    VegetationView *vf = static_cast<VegetationView *>(full);
    fill_density(&density, data, 8, 8, 255);
    rt_vegetation3d_set_density_map(full, &density);
    rt_vegetation3d_populate(full, &terrain, 200);
    assert(vf->total_count == 200);

    // Zero density (R=0) rejects every candidate.
    void *empty = rt_vegetation3d_new(nullptr);
    VegetationView *ve = static_cast<VegetationView *>(empty);
    rt_pixels_impl zero = {};
    uint32_t zero_data[64];
    fill_density(&zero, zero_data, 8, 8, 0);
    rt_vegetation3d_set_density_map(empty, &zero);
    rt_vegetation3d_populate(empty, &terrain, 200);
    assert(ve->total_count == 0);
    g_terrain = nullptr;
}

static void test_update_advances_time_and_keeps_near_visible() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 100);
    rt_vegetation3d_set_lod_distances(veg, 1000.0, 2000.0); // everything inside near band
    rt_vegetation3d_update(veg, 0.5, 32.0, 0.0, 32.0);      // camera at terrain center

    assert(std::fabs(v->time - 0.5) < 1e-9);
    assert(v->total_count == 100);
    assert(v->visible_count == v->total_count); // no thinning within near distance
    g_terrain = nullptr;
}

static void test_wind_sway_is_desynchronized_and_smooth() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 64);
    rt_vegetation3d_set_lod_distances(veg, 1000.0, 2000.0);
    rt_vegetation3d_set_wind_params(veg, 0.85, 0.11, 1.15);
    rt_vegetation3d_update(veg, 0.016, 32.0, 0.0, 32.0);

    assert(v->visible_count == v->total_count);
    float first_x = v->visible_transforms[1];
    bool saw_different_x = false;
    float max_x = 0.0f;
    float max_z = 0.0f;
    for (int32_t i = 0; i < v->visible_count; i++) {
        float wind_x = v->visible_transforms[i * 16 + 1];
        float wind_z = v->visible_transforms[i * 16 + 9];
        if (std::fabs(wind_x - first_x) > 0.001f)
            saw_different_x = true;
        if (std::fabs(wind_x) > max_x)
            max_x = std::fabs(wind_x);
        if (std::fabs(wind_z) > max_z)
            max_z = std::fabs(wind_z);
    }

    assert(saw_different_x);
    assert(max_x <= 0.111f);
    assert(max_z <= 0.047f);

    rt_vegetation3d_update(veg, 0.016, 32.0, 0.0, 32.0);
    assert(std::fabs(v->visible_transforms[1] - first_x) < 0.01f);
    g_terrain = nullptr;
}

static void test_update_far_camera_culls_all() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 100);
    rt_vegetation3d_set_lod_distances(veg, 10.0, 20.0);
    rt_vegetation3d_update(veg, 0.016, 100000.0, 0.0, 100000.0); // far beyond lod_far

    assert(v->visible_count == 0);
    g_terrain = nullptr;
}

static void test_draw_submits_one_instanced_batch_without_mutating_cull() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 64);
    rt_vegetation3d_set_lod_distances(veg, 1000.0, 2000.0);
    rt_vegetation3d_update(veg, 0.016, 32.0, 0.0, 32.0);
    assert(v->visible_count > 0);

    rt_canvas3d canvas = make_frame_canvas();
    reset_draw_records();
    rt_canvas3d_draw_vegetation(&canvas, veg);

    assert(g_instanced_batch_calls == 1);
    assert(g_last_instance_count == v->visible_count);
    assert(g_cull_during_batch == 1);  // grass uses a double-sided material, not canvas mutation
    assert(canvas.backface_cull == 1); // previous state remains untouched
    g_terrain = nullptr;
}

static void test_draw_is_noop_outside_frame() {
    void *veg = rt_vegetation3d_new(nullptr);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 32);
    rt_vegetation3d_set_lod_distances(veg, 1000.0, 2000.0);
    rt_vegetation3d_update(veg, 0.016, 32.0, 0.0, 32.0);

    rt_canvas3d canvas = make_frame_canvas();
    canvas.in_frame = 0; // not between Begin/End
    reset_draw_records();
    rt_canvas3d_draw_vegetation(&canvas, veg);
    assert(g_instanced_batch_calls == 0);
    g_terrain = nullptr;
}

static void test_set_blade_size_rebuilds_mesh() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);

    rt_vegetation3d_set_blade_size(veg, 1.0, 2.0, 0.5);
    rt_mesh3d *mesh = static_cast<rt_mesh3d *>(v->blade_mesh);
    assert(mesh->vertex_count == 8); // cleared and rebuilt to the cross-billboard quad set
    assert(mesh->index_count == 12);
    assert(v->blade_width == 1.0);
    assert(v->blade_height == 2.0);
    assert(v->size_variation == 0.5);
}

static void test_setters_sanitize_nonfinite_inputs() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);

    rt_vegetation3d_set_wind_params(veg, NAN, -3.0, INFINITY);
    assert(v->wind_speed == 0.0);      // NaN -> fallback 0
    assert(v->wind_strength == 0.0);   // negative -> 0
    assert(v->wind_turbulence == 0.0); // Inf -> 0

    rt_vegetation3d_set_lod_distances(veg, NAN, NAN);
    assert(v->lod_near == 40.0f); // non-finite -> defaults restored
    assert(v->lod_far == 100.0f);

    rt_vegetation3d_set_blade_size(veg, NAN, -1.0, 5.0);
    assert(v->blade_width == 0.4);    // NaN -> default
    assert(v->blade_height == 1.2);   // non-positive -> default
    assert(v->size_variation == 1.0); // clamped to [0,1]
}

// Collect the visible set as exact (x, z) translation keys. Translations are copied verbatim
// from base_transforms, so identical blades produce bit-identical keys across updates.
static void collect_visible_keys(const VegetationView *v,
                                 std::pair<float, float> *keys,
                                 int32_t *count) {
    *count = v->visible_count;
    for (int32_t i = 0; i < v->visible_count; i++) {
        keys[i] = {v->visible_transforms[i * 16 + 3], v->visible_transforms[i * 16 + 11]};
    }
}

static bool key_present(const std::pair<float, float> *keys,
                        int32_t count,
                        std::pair<float, float> key) {
    for (int32_t i = 0; i < count; i++) {
        if (keys[i] == key)
            return true;
    }
    return false;
}

// Regression for the non-monotone skip-stride thinning: as the camera recedes (every blade's
// distance strictly increasing), the visible set must only shrink — a blade that dropped out
// must never reappear at a larger distance. The old `hash % skip` test swapped cohorts at each
// stride boundary and fails this.
static void test_lod_thinning_is_monotone_with_distance() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 400);
    rt_vegetation3d_set_lod_distances(veg, 10.0, 120.0);
    rt_vegetation3d_set_wind_params(veg, 0.0, 0.0, 0.0);

    // Camera starts beyond the field's +X edge (blades span x in [2, 62]) and walks away
    // along +X, so every blade's distance is strictly increasing step to step.
    static std::pair<float, float> prev_keys[400];
    static std::pair<float, float> cur_keys[400];
    int32_t prev_count = -1;
    for (int step = 0; step < 60; step++) {
        double cam_x = 70.0 + 2.0 * step;
        rt_vegetation3d_update(veg, 0.0, cam_x, 0.0, 32.0);
        int32_t cur_count = 0;
        collect_visible_keys(v, cur_keys, &cur_count);
        if (prev_count >= 0) {
            assert(cur_count <= prev_count); // set can only shrink
            for (int32_t i = 0; i < cur_count; i++)
                assert(key_present(prev_keys, prev_count, cur_keys[i]));
        }
        std::memcpy(prev_keys, cur_keys, sizeof(cur_keys[0]) * static_cast<size_t>(cur_count));
        prev_count = cur_count;
    }
    assert(prev_count == 0); // far enough that everything has faded out
    g_terrain = nullptr;
}

// Blades in the outer LOD band must shrink (scale fade) instead of holding full size until the
// hard cull. With wind disabled the visible transform is exactly base * fade, so comparing the
// 3x3 basis magnitude against the matching base transform detects the fade.
static void test_lod_far_band_fades_scale() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 300);
    rt_vegetation3d_set_wind_params(veg, 0.0, 0.0, 0.0);
    const double lod_near = 5.0;
    const double lod_far = 40.0;
    rt_vegetation3d_set_lod_distances(veg, lod_near, lod_far);
    const double cam_x = 32.0;
    const double cam_z = 32.0;
    rt_vegetation3d_update(veg, 0.0, cam_x, 0.0, cam_z);
    assert(v->visible_count > 0);

    const double fade_start_dist = lod_near + 0.85 * (lod_far - lod_near);
    int faded_seen = 0;
    for (int32_t i = 0; i < v->visible_count; i++) {
        const float *m = &v->visible_transforms[i * 16];
        // Match the source blade by exact translation.
        const float *base = nullptr;
        for (int32_t b = 0; b < v->total_count; b++) {
            const float *bm = &v->base_transforms[b * 16];
            if (bm[3] == m[3] && bm[11] == m[11]) {
                base = bm;
                break;
            }
        }
        assert(base != nullptr);
        double dx = static_cast<double>(m[3]) - cam_x;
        double dz = static_cast<double>(m[11]) - cam_z;
        double dist = std::sqrt(dx * dx + dz * dz);
        double norm_vis = 0.0, norm_base = 0.0;
        static const int basis_idx[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
        for (int k = 0; k < 9; k++) {
            norm_vis += static_cast<double>(m[basis_idx[k]]) * m[basis_idx[k]];
            norm_base += static_cast<double>(base[basis_idx[k]]) * base[basis_idx[k]];
        }
        if (dist > fade_start_dist + 0.5) {
            assert(norm_vis < norm_base - 1e-6); // shrinking toward the far edge
            faded_seen++;
        } else if (dist < fade_start_dist - 0.5) {
            assert(std::fabs(norm_vis - norm_base) < 1e-6); // untouched inside the band
        }
    }
    assert(faded_seen > 0); // the fixture actually exercised the fade band
    g_terrain = nullptr;
}

// A camera far outside the field's grid bounds must still see every blade when lod_far covers
// the whole field (guards the grid cell-range clamping).
static void test_camera_outside_field_still_sees_blades() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 128);
    rt_vegetation3d_set_lod_distances(veg, 5000.0, 6000.0); // near band covers everything
    rt_vegetation3d_update(veg, 0.0, 200.0, 0.0, -150.0);   // outside the populated area
    assert(v->visible_count == v->total_count);
    g_terrain = nullptr;
}

// Re-populating must invalidate and rebuild the spatial grid.
static void test_grid_rebuilds_after_repopulate() {
    void *veg = rt_vegetation3d_new(nullptr);
    VegetationView *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;

    rt_vegetation3d_populate(veg, &terrain, 100);
    rt_vegetation3d_set_lod_distances(veg, 1000.0, 2000.0);
    rt_vegetation3d_update(veg, 0.0, 32.0, 0.0, 32.0);
    assert(v->grid_ready == 1);
    assert(v->grid_cell_start != nullptr);
    assert(v->grid_indices != nullptr);
    assert(v->visible_count == 100);

    rt_vegetation3d_populate(veg, &terrain, 60);
    assert(v->grid_ready == 0); // population change invalidates the grid
    rt_vegetation3d_update(veg, 0.0, 32.0, 0.0, 32.0);
    assert(v->grid_ready == 1);
    assert(v->visible_count == 60); // rebuilt grid still reaches every blade
    g_terrain = nullptr;
}

static void test_scalar_corruption_is_persistently_repaired() {
    void *veg = rt_vegetation3d_new(nullptr);
    auto *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(32, 32);
    g_terrain = &terrain;
    rt_vegetation3d_populate(veg, &terrain, 32);

    v->blade_width = NAN;
    v->blade_height = -1.0;
    v->size_variation = INFINITY;
    v->wind_speed = INFINITY;
    v->wind_strength = -4.0;
    v->wind_turbulence = NAN;
    v->time = -10.0;
    v->lod_near = NAN;
    v->lod_far = -20.0f;
    v->scatter_seed = 0;

    rt_vegetation3d_update(veg, 0.0, 0.0, 0.0, 0.0);
    assert(v->blade_width == 0.4);
    assert(v->blade_height == 1.2);
    assert(v->size_variation == 0.0);
    assert(v->wind_speed == 0.0);
    assert(v->wind_strength == 0.0);
    assert(v->wind_turbulence == 0.0);
    assert(v->time == 0.0);
    assert(v->lod_near == 40.0f);
    assert(v->lod_far == 100.0f);
    assert(v->scatter_seed != 0);
    g_terrain = nullptr;
}

static void test_population_and_visible_mirrors_restore_owned_allocations() {
    void *veg = rt_vegetation3d_new(nullptr);
    auto *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;
    rt_vegetation3d_populate(veg, &terrain, 64);
    rt_vegetation3d_set_lod_distances(veg, 1000.0, 2000.0);
    rt_vegetation3d_update(veg, 0.0, 32.0, 0.0, 32.0);

    float *owned_base = v->base_transforms;
    float *owned_positions = v->positions;
    float *owned_visible = v->visible_transforms;
    int32_t owned_count = v->total_count;
    int32_t owned_capacity = v->capacity;
    int32_t owned_visible_capacity = v->visible_capacity;
    auto *foreign_base = static_cast<float *>(std::calloc(64u * 16u, sizeof(float)));
    auto *foreign_positions = static_cast<float *>(std::calloc(64u * 3u, sizeof(float)));
    auto *foreign_visible = static_cast<float *>(std::calloc(64u * 16u, sizeof(float)));
    assert(foreign_base && foreign_positions && foreign_visible);

    v->base_transforms = foreign_base;
    v->positions = foreign_positions;
    v->visible_transforms = foreign_visible;
    v->total_count = 1;
    v->capacity = INT32_MAX;
    v->visible_count = INT32_MAX;
    v->visible_capacity = INT32_MAX;
    rt_vegetation3d_update(veg, 0.0, 32.0, 0.0, 32.0);

    assert(v->base_transforms == owned_base);
    assert(v->positions == owned_positions);
    assert(v->visible_transforms == owned_visible);
    assert(v->total_count == owned_count);
    assert(v->capacity == owned_capacity);
    assert(v->visible_capacity == owned_visible_capacity);
    assert(v->visible_count > 0 && v->visible_count <= owned_count);
    std::free(foreign_base);
    std::free(foreign_positions);
    std::free(foreign_visible);
    g_terrain = nullptr;
}

static void test_render_resource_mirrors_restore_retained_owners() {
    void *veg = rt_vegetation3d_new(nullptr);
    auto *v = static_cast<VegetationView *>(veg);
    void *owned_mesh = v->blade_mesh;
    void *owned_material = v->blade_material;
    void *foreign_mesh = rt_mesh3d_new();
    void *foreign_material = rt_material3d_new();

    v->blade_mesh = foreign_mesh;
    v->blade_material = foreign_material;
    rt_vegetation3d_set_blade_size(veg, 0.7, 1.8, 0.2);
    assert(v->blade_mesh == owned_mesh);
    assert(v->blade_material == owned_material);
    auto *mesh = static_cast<rt_mesh3d *>(owned_mesh);
    assert(mesh->vertex_count == 8);
    assert(mesh->index_count == 12);
}

static void test_density_mirror_cannot_replace_retained_map() {
    void *veg = rt_vegetation3d_new(nullptr);
    auto *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(32, 32);
    rt_pixels_impl full = {};
    rt_pixels_impl zero = {};
    uint32_t full_data[16] = {};
    uint32_t zero_data[16] = {};
    fill_density(&full, full_data, 4, 4, 255);
    fill_density(&zero, zero_data, 4, 4, 0);
    g_terrain = &terrain;

    rt_vegetation3d_set_density_map(veg, &full);
    v->density_map = &zero;
    rt_vegetation3d_populate(veg, &terrain, 48);
    assert(v->density_map == &full);
    assert(v->total_count == 48);
    g_terrain = nullptr;
}

static void test_grid_mirrors_and_malformed_offsets_fail_safe() {
    void *veg = rt_vegetation3d_new(nullptr);
    auto *v = static_cast<VegetationView *>(veg);
    FakeTerrain terrain = make_terrain(64, 64);
    g_terrain = &terrain;
    rt_vegetation3d_populate(veg, &terrain, 128);
    rt_vegetation3d_set_lod_distances(veg, 1000.0, 2000.0);
    rt_vegetation3d_update(veg, 0.0, 32.0, 0.0, 32.0);
    assert(v->grid_ready == 1);

    int32_t *owned_starts = v->grid_cell_start;
    int32_t *owned_indices = v->grid_indices;
    int32_t owned_cells_x = v->grid_cells_x;
    int32_t owned_cells_z = v->grid_cells_z;
    int32_t cell_count = owned_cells_x * owned_cells_z;
    auto *foreign_starts =
        static_cast<int32_t *>(std::calloc(static_cast<size_t>(cell_count) + 1u, sizeof(int32_t)));
    auto *foreign_indices = static_cast<int32_t *>(std::calloc(128u, sizeof(int32_t)));
    assert(foreign_starts && foreign_indices);
    v->grid_cell_start = foreign_starts;
    v->grid_indices = foreign_indices;
    v->grid_cells_x = 1;
    v->grid_cells_z = 1;
    v->grid_cell_w = NAN;
    v->grid_cell_d = INFINITY;
    v->grid_ready = 7;
    rt_vegetation3d_update(veg, 0.0, 32.0, 0.0, 32.0);
    assert(v->grid_cell_start == owned_starts);
    assert(v->grid_indices == owned_indices);
    assert(v->grid_cells_x == owned_cells_x);
    assert(v->grid_cells_z == owned_cells_z);
    assert(v->visible_count > 0);
    std::free(foreign_starts);
    std::free(foreign_indices);

    /* Damage the owned CSR terminal offset. The update must reject the table before using any
     * offset as an index, then reproduce visibility through the linear fallback. */
    v->grid_cell_start[cell_count] = 0;
    rt_vegetation3d_update(veg, 0.0, 32.0, 0.0, 32.0);
    assert(v->visible_count > 0);
    assert(v->visible_count <= v->total_count);
    g_terrain = nullptr;
}

static void test_reapplying_blade_size_is_allocation_free() {
    void *veg = rt_vegetation3d_new(nullptr);
    g_mesh_clear_calls = 0;
    rt_vegetation3d_set_blade_size(veg, 0.4, 1.2, 0.3);
    assert(g_mesh_clear_calls == 0);
}

int main() {
    test_new_builds_blade_mesh_and_defaults();
    test_populate_scatters_requested_count();
    test_populate_uses_opaque_scaled_terrain_extents();
    test_seed_controls_population_layout();
    test_populate_nonpositive_count_clears();
    test_populate_overflow_count_traps();
    test_density_map_gates_population();
    test_update_advances_time_and_keeps_near_visible();
    test_wind_sway_is_desynchronized_and_smooth();
    test_update_far_camera_culls_all();
    test_draw_submits_one_instanced_batch_without_mutating_cull();
    test_draw_is_noop_outside_frame();
    test_set_blade_size_rebuilds_mesh();
    test_setters_sanitize_nonfinite_inputs();
    test_lod_thinning_is_monotone_with_distance();
    test_lod_far_band_fades_scale();
    test_camera_outside_field_still_sees_blades();
    test_grid_rebuilds_after_repopulate();
    test_scalar_corruption_is_persistently_repaired();
    test_population_and_visible_mirrors_restore_owned_allocations();
    test_render_resource_mirrors_restore_retained_owners();
    test_density_mirror_cannot_replace_retained_map();
    test_grid_mirrors_and_malformed_offsets_fail_safe();
    test_reapplying_blade_size_is_allocation_free();
    std::printf("RTVegetation3DContractTests passed.\n");
    return 0;
}
