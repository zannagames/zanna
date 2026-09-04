//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTParticles3DContractTests.cpp
// Purpose: Isolated contract tests for the 3D particle runtime.
// Key invariants:
//   - Particle draw ordering is deterministic for alpha-blended billboards.
//   - Persistent draw and sort scratch buffers grow predictably and are reused.
//   - Hardware compact records reconstruct the software billboard corners and preserve CPU trails.
// Ownership/Lifetime:
//   - Runtime objects are allocated through local test stubs and freed by process exit.
//   - Captured draw data is copied into test-owned globals before each assertion.
// Links: src/runtime/graphics/3d/world/rt_particles3d.c
//
//===----------------------------------------------------------------------===//

extern "C" {
#include "rt_canvas3d_internal.h"
#include "rt_particles3d.h"
#include "rt_pixels_internal.h"
}

#include <cassert>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::jmp_buf g_env;
const char *g_last_trap = nullptr;
bool g_expect_trap = false;
constexpr int kAlphaModeOpaque = 0;
constexpr int kAlphaModeBlend = 2;
int g_draw_mesh_calls = 0;
int g_draw_mesh_matrix_keyed_calls = 0;
int g_last_mesh_vertex_count = 0;
int g_last_mesh_index_count = 0;
double g_last_mesh_quad_z[16] = {0.0};
double g_last_mesh_draw_quad_z[16] = {0.0};
double g_keyed_draw_z[16] = {0.0};
double g_keyed_draw_alpha[16] = {0.0};
int g_keyed_draw_additive[16] = {0};
double g_last_draw_alpha = 0.0;
int g_last_draw_additive = 0;
int g_last_draw_alpha_mode = 0;
uint64_t g_last_mesh_signature = 0;
vgfx3d_vertex_t g_last_mesh_vertices[64] = {};
int g_particle_instancing_supported = 0;
int g_particle_batch_calls = 0;
int g_particle_batch_count = 0;
vgfx3d_particle_instance_t g_particle_batch_instances[64] = {};
double g_particle_batch_alpha = 0.0;
int g_particle_batch_additive = 0;
double g_last_vec3[3] = {0.0, 0.0, 0.0};
void *g_stub_materials[128] = {nullptr};
int g_stub_material_count = 0;
void *g_stub_pixels[16] = {nullptr};
int g_stub_pixel_count = 0;

static bool tracked_pointer(void *const *items, int count, const void *value) {
    for (int i = 0; i < count; i++) {
        if (items[i] == value)
            return true;
    }
    return false;
}

static void track_pointer(void **items, int *count, int capacity, void *value) {
    if (!items || !count || !value || tracked_pointer(items, *count, value))
        return;
    if (*count < capacity)
        items[(*count)++] = value;
}

static void untrack_pointer(void **items, int *count, const void *value) {
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

struct StubMaterial {
    void *vptr = nullptr;
    double diffuse[4] = {0.0, 0.0, 0.0, 1.0};
    double specular[3] = {0.0, 0.0, 0.0};
    double shininess = 0.0;
    int32_t workflow = 0;
    void *texture = nullptr;
    void *normal_map = nullptr;
    void *specular_map = nullptr;
    void *emissive_map = nullptr;
    void *metallic_roughness_map = nullptr;
    void *ao_map = nullptr;
    void *lightmap = nullptr;
    double emissive[3] = {0.0, 0.0, 0.0};
    double metallic = 0.0;
    double roughness = 0.0;
    double ao = 0.0;
    double emissive_intensity = 0.0;
    double normal_scale = 1.0;
    double alpha = 1.0;
    double alpha_cutoff = 0.5;
    void *env_map = nullptr;
    double reflectivity = 0.0;
    int8_t unlit = 0;
    int8_t double_sided = 0;
    int8_t additive_blend = 0;
    int32_t alpha_mode = kAlphaModeOpaque;
    int8_t alpha_mode_auto = 0;
    int32_t shadow_mode = 0;
    int32_t texture_wrap_s = 0;
    int32_t texture_wrap_t = 0;
    int32_t texture_filter = 0;
    int32_t texture_min_filter = 0;
    int32_t texture_mag_filter = 0;
    int32_t texture_mip_filter = 0;
    int32_t anisotropy = 1;
    int32_t texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_COUNT] = {0};
    int32_t texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_COUNT] = {0};
    int32_t texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT] = {0};
    int32_t texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT] = {0};
    int32_t texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT] = {0};
    int32_t texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT] = {0};
    int32_t texture_slot_anisotropy[RT_MATERIAL3D_TEXTURE_SLOT_COUNT] = {0};
    int32_t texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_COUNT] = {0};
    double texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_COUNT][6] = {{0.0}};
    int32_t shading_model = 0;
    double custom_params[12] = {0.0};
    double depth_bias = 0.0;
    double slope_scaled_depth_bias = 0.0;
    double soft_fade = 0.0;
    int8_t ssr_enabled = 0;
    /* ADR 0312 projected decal layer (mirrors rt_material3d). */
    void *decal_map = nullptr;
    double decal_rows[12] = {0.0};
    double decal_forward[3] = {0.0};
    double decal_opacity = 0.0;
    int8_t decal_projector_set = 0;
    uint64_t identity_serial = 0;
};

static_assert(sizeof(StubMaterial) == sizeof(rt_material3d));
static_assert(offsetof(StubMaterial, soft_fade) == offsetof(rt_material3d, soft_fade));

struct ParticlesView {
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
    vgfx3d_vertex_t *draw_vertices[4];
    uint32_t *draw_indices[4];
    uint32_t draw_vertex_capacity[4];
    uint32_t draw_index_capacity[4];
    void *draw_materials[4];
    vgfx3d_vertex_t **overflow_draw_vertices;
    uint32_t **overflow_draw_indices;
    uint32_t *overflow_draw_vertex_capacity;
    uint32_t *overflow_draw_index_capacity;
    void **overflow_draw_materials;
    int32_t overflow_draw_slot_capacity;
    void *sort_keys;
    void *sort_scratch;
    int32_t sort_key_capacity;
    uint64_t sort_key_grow_count;
    int64_t draw_frame_serial;
    int32_t draw_slots_used;
    double simulation_residual;
    double dropped_time_total;
    double dropped_time_last_update;
    int32_t terminal_count;
    int8_t render_final_frame;
    vgfx3d_particle_instance_t *instance_scratch;
    int32_t instance_scratch_capacity;
    uint64_t instance_scratch_grow_count;
};

struct RealParticleView {
    double pos[3];
    double vel[3];
    float color[4];
    float size;
    float life;
    float max_life;
    double remaining_life;
};

static uint64_t hash_bytes(uint64_t seed, const void *data, size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    uint64_t hash = seed ? seed : 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

static rt_pixels_impl *make_test_pixels(uint32_t texel) {
    size_t bytes = sizeof(rt_pixels_impl) + sizeof(uint32_t);
    auto *pixels = static_cast<rt_pixels_impl *>(std::calloc(1, bytes));
    assert(pixels != nullptr);
    pixels->width = 1;
    pixels->height = 1;
    pixels->data = reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(pixels) +
                                                sizeof(rt_pixels_impl));
    pixels->data[0] = texel;
    pixels->cache_identity = 1;
    track_pointer(g_stub_pixels,
                  &g_stub_pixel_count,
                  static_cast<int>(sizeof(g_stub_pixels) / sizeof(g_stub_pixels[0])),
                  pixels);
    return pixels;
}

} // namespace

extern "C" int64_t rt_particles3d_test_sort_key_capacity(void *o);
extern "C" uint64_t rt_particles3d_test_sort_key_grow_count(void *o);
extern "C" int64_t rt_particles3d_test_instance_scratch_capacity(void *o);
extern "C" uint64_t rt_particles3d_test_instance_scratch_grow_count(void *o);
extern "C" void rt_particles3d_test_age_overflow_draw_slots(void *o, int32_t frames);

extern "C" void *rt_obj_new_i64(int64_t, int64_t byte_size) {
    return std::calloc(1, static_cast<size_t>(byte_size));
}

extern "C" int64_t rt_obj_class_id(void *obj) {
    if (tracked_pointer(g_stub_materials, g_stub_material_count, obj))
        return RT_G3D_MATERIAL3D_CLASS_ID;
    if (tracked_pointer(g_stub_pixels, g_stub_pixel_count, obj))
        return RT_PIXELS_CLASS_ID;
    return RT_G3D_PARTICLES3D_CLASS_ID;
}

extern "C" int8_t rt_obj_is_instance(void *obj, int64_t class_id, size_t) {
    return obj && rt_obj_class_id(obj) == class_id;
}

extern "C" int8_t rt_heap_is_payload(void *) {
    return 0;
}

extern "C" void rt_obj_set_finalizer(void *, void (*)(void *)) {}

extern "C" void rt_obj_retain_maybe(void *) {}

/* ADR 0233 emitter readback boxes positions/directions as Vec3; the isolated
 * contract build links no math objects, so a null-returning stub keeps the
 * target minimal (no contract case dereferences the boxed value). */
extern "C" void *rt_vec3_new(double x, double y, double z) {
    g_last_vec3[0] = x;
    g_last_vec3[1] = y;
    g_last_vec3[2] = z;
    return nullptr;
}

extern "C" int32_t rt_obj_release_check0(void *) {
    return 1;
}

extern "C" void rt_obj_free(void *obj) {
    untrack_pointer(g_stub_materials, &g_stub_material_count, obj);
    std::free(obj);
}

extern "C" int rt_canvas3d_add_temp_buffer(void *, void *) {
    return 1;
}

extern "C" int rt_canvas3d_remove_temp_buffer(void *, void *) {
    return 1;
}

extern "C" int rt_canvas3d_get_camera_relative_origin(void *, double out_origin[3]) {
    if (out_origin) {
        out_origin[0] = 0.0;
        out_origin[1] = 0.0;
        out_origin[2] = 0.0;
    }
    return 0;
}

/// @brief Test double exposing whether the fixture should take the hardware particle path.
/// @return The mutable fixture capability flag; no Canvas3D state is inspected.
extern "C" int rt_canvas3d_supports_particle_instancing(void *) {
    return g_particle_instancing_supported;
}

/// @brief Snapshot one compact particle batch for equivalence assertions.
/// @details Invalid or oversized inputs are rejected; accepted records and the configured blend
///   material are copied into test-owned globals before the emitter may reuse its scratch.
/// @return Non-zero when the batch fits the fixed capture buffer; otherwise zero.
extern "C" int rt_canvas3d_queue_particle_batch(void *,
                                                void *material,
                                                const vgfx3d_particle_instance_t *instances,
                                                int32_t instance_count) {
    g_particle_batch_calls++;
    g_particle_batch_count = instance_count;
    if (!instances || instance_count <= 0 ||
        instance_count > static_cast<int32_t>(sizeof(g_particle_batch_instances) /
                                              sizeof(g_particle_batch_instances[0])))
        return 0;
    std::memcpy(g_particle_batch_instances,
                instances,
                static_cast<size_t>(instance_count) * sizeof(*instances));
    g_particle_batch_alpha = material ? static_cast<StubMaterial *>(material)->alpha : 0.0;
    g_particle_batch_additive =
        material ? static_cast<StubMaterial *>(material)->additive_blend : 0;
    return 1;
}

extern "C" void *rt_material3d_new(void) {
    void *material = std::calloc(1, sizeof(StubMaterial));
    track_pointer(g_stub_materials,
                  &g_stub_material_count,
                  static_cast<int>(sizeof(g_stub_materials) / sizeof(g_stub_materials[0])),
                  material);
    return material;
}

extern "C" void rt_material3d_set_color(void *m, double r, double g, double b) {
    StubMaterial *mat = static_cast<StubMaterial *>(m);
    mat->diffuse[0] = r;
    mat->diffuse[1] = g;
    mat->diffuse[2] = b;
    mat->diffuse[3] = mat->alpha;
}

extern "C" void rt_material3d_set_unlit(void *m, int8_t enabled) {
    static_cast<StubMaterial *>(m)->unlit = enabled;
}

extern "C" void rt_material3d_set_alpha(void *m, double a) {
    static_cast<StubMaterial *>(m)->alpha = a;
}

extern "C" void rt_material3d_set_alpha_mode(void *m, int64_t mode) {
    static_cast<StubMaterial *>(m)->alpha_mode = static_cast<int32_t>(mode);
}

extern "C" void rt_material3d_set_texture(void *m, void *tex) {
    static_cast<StubMaterial *>(m)->texture = tex;
}

extern "C" void *rt_mat4_identity(void) {
    static double identity[16] = {
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    return identity;
}

extern "C" void rt_canvas3d_draw_mesh(void *, void *mesh, void *, void *material) {
    rt_mesh3d *m = static_cast<rt_mesh3d *>(mesh);
    g_draw_mesh_calls++;
    g_last_mesh_vertex_count = m ? (int)m->vertex_count : 0;
    g_last_mesh_index_count = m ? (int)m->index_count : 0;
    g_last_mesh_signature = 1469598103934665603ull;
    g_last_mesh_signature = hash_bytes(
        g_last_mesh_signature, &g_last_mesh_vertex_count, sizeof(g_last_mesh_vertex_count));
    g_last_mesh_signature = hash_bytes(
        g_last_mesh_signature, &g_last_mesh_index_count, sizeof(g_last_mesh_index_count));
    std::memset(g_last_mesh_quad_z, 0, sizeof(g_last_mesh_quad_z));
    std::memset(g_last_mesh_draw_quad_z, 0, sizeof(g_last_mesh_draw_quad_z));
    std::memset(g_last_mesh_vertices, 0, sizeof(g_last_mesh_vertices));
    if (m && m->vertices) {
        size_t copied_vertices = m->vertex_count;
        if (copied_vertices > sizeof(g_last_mesh_vertices) / sizeof(g_last_mesh_vertices[0]))
            copied_vertices = sizeof(g_last_mesh_vertices) / sizeof(g_last_mesh_vertices[0]);
        std::memcpy(
            g_last_mesh_vertices, m->vertices, copied_vertices * sizeof(g_last_mesh_vertices[0]));
        g_last_mesh_signature =
            hash_bytes(g_last_mesh_signature,
                       m->vertices,
                       static_cast<size_t>(m->vertex_count) * sizeof(*m->vertices));
        int quad_count = m->vertex_count / 4;
        if (quad_count > (int)(sizeof(g_last_mesh_quad_z) / sizeof(g_last_mesh_quad_z[0])))
            quad_count = (int)(sizeof(g_last_mesh_quad_z) / sizeof(g_last_mesh_quad_z[0]));
        for (int i = 0; i < quad_count; ++i)
            g_last_mesh_quad_z[i] = m->vertices[i * 4].pos[2];
    }
    if (m && m->indices) {
        g_last_mesh_signature =
            hash_bytes(g_last_mesh_signature,
                       m->indices,
                       static_cast<size_t>(m->index_count) * sizeof(*m->indices));
        int quad_count = m->index_count / 6;
        if (quad_count >
            (int)(sizeof(g_last_mesh_draw_quad_z) / sizeof(g_last_mesh_draw_quad_z[0])))
            quad_count =
                (int)(sizeof(g_last_mesh_draw_quad_z) / sizeof(g_last_mesh_draw_quad_z[0]));
        for (int i = 0; i < quad_count; ++i) {
            uint32_t vertex_index = m->indices[i * 6];
            if (vertex_index < m->vertex_count)
                g_last_mesh_draw_quad_z[i] = m->vertices[vertex_index].pos[2];
        }
    }
    g_last_draw_alpha = material ? static_cast<StubMaterial *>(material)->alpha : 0.0;
    g_last_draw_additive = material ? static_cast<StubMaterial *>(material)->additive_blend : 0;
    g_last_draw_alpha_mode = material ? static_cast<StubMaterial *>(material)->alpha_mode : 0;
}

extern "C" void rt_canvas3d_draw_mesh_matrix(void *canvas,
                                             void *mesh,
                                             const double *,
                                             void *material) {
    rt_canvas3d_draw_mesh(canvas, mesh, nullptr, material);
}

extern "C" void rt_canvas3d_draw_mesh_matrix_keyed(void *,
                                                   void *,
                                                   const double *model_matrix,
                                                   void *material,
                                                   const void *,
                                                   const float *,
                                                   const float *) {
    int draw_index = g_draw_mesh_matrix_keyed_calls++;
    if (draw_index < (int)(sizeof(g_keyed_draw_z) / sizeof(g_keyed_draw_z[0]))) {
        g_keyed_draw_z[draw_index] = model_matrix ? model_matrix[11] : 0.0;
        g_keyed_draw_alpha[draw_index] =
            material ? static_cast<StubMaterial *>(material)->alpha : 0.0;
        g_keyed_draw_additive[draw_index] =
            material ? static_cast<StubMaterial *>(material)->additive_blend : 0;
    }
}

extern "C" void rt_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_env, 1);
    std::abort();
}

static void expect_trap_on_invalid_capacity() {
    g_expect_trap = true;
    if (setjmp(g_env) == 0) {
        (void)rt_particles3d_new(0);
        assert(false && "expected rt_trap");
    }
    g_expect_trap = false;
    assert(g_last_trap != nullptr);
    assert(std::strstr(g_last_trap, "max_particles") != nullptr);
}

static void test_burst_and_clear() {
    void *ps = rt_particles3d_new(8);
    assert(ps != nullptr);
    assert(rt_particles3d_get_count(ps) == 0);
    assert(rt_particles3d_get_emitting(ps) == 0);

    rt_particles3d_burst(ps, 3);
    assert(rt_particles3d_get_count(ps) == 3);

    rt_particles3d_clear(ps);
    assert(rt_particles3d_get_count(ps) == 0);
}

static void test_burst_caps_to_available_pool() {
    void *ps = rt_particles3d_new(4);
    assert(ps != nullptr);

    rt_particles3d_burst(ps, 100);
    assert(rt_particles3d_get_count(ps) == 4);

    rt_particles3d_burst(ps, 100);
    assert(rt_particles3d_get_count(ps) == 4);
}

static void test_start_stop_and_update_spawns_particles() {
    void *ps = rt_particles3d_new(8);
    assert(ps != nullptr);

    rt_particles3d_set_rate(ps, 4.0);
    rt_particles3d_start(ps);
    assert(rt_particles3d_get_emitting(ps) == 1);

    rt_particles3d_update(ps, 0.5);
    assert(rt_particles3d_get_count(ps) == 2);

    rt_particles3d_stop(ps);
    assert(rt_particles3d_get_emitting(ps) == 0);
    int64_t count_after_stop = rt_particles3d_get_count(ps);
    rt_particles3d_update(ps, 0.5);
    assert(rt_particles3d_get_count(ps) <= count_after_stop);
}

static void test_particles_expire_after_lifetime() {
    void *ps = rt_particles3d_new(8);
    assert(ps != nullptr);

    rt_particles3d_set_lifetime(ps, 0.1, 0.1);
    rt_particles3d_burst(ps, 4);
    assert(rt_particles3d_get_count(ps) == 4);

    // Expiration is part of this update: terminal rendering does not keep particles live.
    rt_particles3d_update(ps, 0.2);
    assert(rt_particles3d_get_count(ps) == 0);
}

static void reset_draw_records();
static rt_camera3d make_test_camera();

static void test_particle_final_frame_renders_end_state() {
    void *ps = rt_particles3d_new(4);
    assert(ps != nullptr);
    ParticlesView *view = static_cast<ParticlesView *>(ps);

    rt_particles3d_set_lifetime(ps, 0.1, 0.1);
    rt_particles3d_set_position(ps, 0.0, 0.0, 2.0);
    rt_particles3d_set_direction(ps, 0.0, 0.0, 1.0, 0.0);
    rt_particles3d_set_speed(ps, 1.0, 1.0);
    rt_particles3d_set_gravity(ps, 0.0, 0.0, 0.0);
    rt_particles3d_set_size(ps, 0.5, 2.5);
    rt_particles3d_set_alpha(ps, 1.0, 0.8);               // does NOT fade to zero
    rt_particles3d_set_color(ps, 0xFFFFFFll, 0x336699ll); // end = (0.2, 0.4, 0.6)
    rt_particles3d_burst(ps, 1);
    assert(rt_particles3d_get_count(ps) == 1);
    const RealParticleView *live = static_cast<const RealParticleView *>(view->particles);
    double expected_endpoint_z = live[0].pos[2] + live[0].vel[2] * (double)live[0].max_life;

    // Exhaust the lifetime in one call. The live particle expires now, while its exact endpoint is
    // copied to the pool-tail terminal snapshot for this update's optional final draw.
    rt_particles3d_update(ps, 0.2);
    assert(rt_particles3d_get_count(ps) == 0);
    assert(rt_particles3d_get_render_final_frame(ps) == 1);
    const RealParticleView *p = static_cast<const RealParticleView *>(view->particles);
    const RealParticleView &terminal = p[view->max_particles - 1];
    assert(terminal.life == 0.0f);
    assert(std::fabs(terminal.pos[2] - expected_endpoint_z) < 1e-9);
    assert(std::fabs(terminal.size - 2.5f) < 1e-6f);
    assert(std::fabs(terminal.color[0] - 0.2f) < 0.01f);
    assert(std::fabs(terminal.color[1] - 0.4f) < 0.01f);
    assert(std::fabs(terminal.color[2] - 0.6f) < 0.01f);
    assert(std::fabs(terminal.color[3] - 0.8f) < 1e-6f);

    rt_canvas3d canvas = {};
    rt_camera3d cam = make_test_camera();
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_draw_mesh_calls == 1);
    assert(g_last_mesh_vertex_count == 4);
    assert(g_last_mesh_index_count == 6);
    assert(std::fabs(g_last_mesh_quad_z[0] - expected_endpoint_z) < 1e-5);

    // Any subsequent valid update ends the terminal interval, even if it only carries a residual.
    rt_particles3d_update(ps, 0.001);
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_draw_mesh_calls == 0);
}

/// @brief Verify callers can disable the one-update terminal endpoint snapshot.
/// @details A particle that expires during a large update must leave the live count and emit no
///          billboard when RenderFinalFrame is false, while preserving the compatibility setter/
///          getter contract.
static void test_particle_final_frame_can_be_disabled() {
    void *ps = rt_particles3d_new(4);
    rt_canvas3d canvas = {};
    rt_camera3d cam = make_test_camera();
    assert(ps != nullptr);
    rt_particles3d_set_render_final_frame(ps, 0);
    assert(rt_particles3d_get_render_final_frame(ps) == 0);
    rt_particles3d_set_lifetime(ps, 0.1, 0.1);
    rt_particles3d_burst(ps, 1);
    rt_particles3d_update(ps, 0.2);
    assert(rt_particles3d_get_count(ps) == 0);
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_draw_mesh_calls == 0);
}

/// @brief Configure one deterministic particle used to compare time partitioning.
/// @param ps Particles3D object receiving fixed direction, speed, gravity, lifetime, and one burst.
static void configure_partition_fixture(void *ps) {
    rt_particles3d_set_direction(ps, 1.0, 0.0, 0.0, 0.0);
    rt_particles3d_set_speed(ps, 3.0, 3.0);
    rt_particles3d_set_gravity(ps, 0.0, -2.0, 0.0);
    rt_particles3d_set_lifetime(ps, 10.0, 10.0);
    rt_particles3d_burst(ps, 1);
}

/// @brief Verify bounded catch-up matches explicit substeps and reports discarded time exactly.
/// @details A 2.25-second update advances only the one-second safety budget, produces the same
///          particle state as sixty explicit steps, carries a sub-step residual, and exposes the
///          remaining 1.25 seconds through both per-update and cumulative telemetry.
static void test_update_is_bounded_and_reports_exact_dropped_time() {
    constexpr double step = 1.0 / 60.0;
    void *large = rt_particles3d_new(8);
    void *partitioned = rt_particles3d_new(8);
    assert(large != nullptr && partitioned != nullptr);
    configure_partition_fixture(large);
    configure_partition_fixture(partitioned);

    rt_particles3d_update(large, 2.25);
    for (int i = 0; i < 60; ++i)
        rt_particles3d_update(partitioned, step);

    assert(rt_particles3d_get_count(large) == 1);
    assert(rt_particles3d_get_count(partitioned) == 1);
    const ParticlesView *large_view = static_cast<const ParticlesView *>(large);
    const ParticlesView *partitioned_view = static_cast<const ParticlesView *>(partitioned);
    const RealParticleView *large_particle =
        static_cast<const RealParticleView *>(large_view->particles);
    const RealParticleView *partitioned_particle =
        static_cast<const RealParticleView *>(partitioned_view->particles);
    for (int c = 0; c < 3; ++c) {
        assert(std::fabs(large_particle[0].pos[c] - partitioned_particle[0].pos[c]) < 1e-12);
        assert(std::fabs(large_particle[0].vel[c] - partitioned_particle[0].vel[c]) < 1e-12);
    }
    assert(std::fabs((double)large_particle[0].life - (double)partitioned_particle[0].life) < 1e-6);
    assert(std::fabs(rt_particles3d_get_last_dropped_time(large) - 1.25) < 1e-9);
    assert(std::fabs(rt_particles3d_get_dropped_time(large) - 1.25) < 1e-9);
    assert(rt_particles3d_get_residual_time(large) < step);
    assert(rt_particles3d_get_dropped_time(partitioned) == 0.0);

    rt_particles3d_reset_dropped_time(large);
    assert(rt_particles3d_get_dropped_time(large) == 0.0);
    assert(rt_particles3d_get_last_dropped_time(large) == 0.0);

    void *residual = rt_particles3d_new(4);
    configure_partition_fixture(residual);
    const ParticlesView *residual_view = static_cast<const ParticlesView *>(residual);
    const RealParticleView *residual_particle =
        static_cast<const RealParticleView *>(residual_view->particles);
    double start_x = residual_particle[0].pos[0];
    rt_particles3d_update(residual, 0.01);
    assert(std::fabs(rt_particles3d_get_residual_time(residual) - 0.01) < 1e-12);
    assert(std::fabs(residual_particle[0].pos[0] - start_x) < 1e-12);
    rt_particles3d_update(residual, step - 0.01);
    assert(rt_particles3d_get_residual_time(residual) < 1e-12);
    assert(std::fabs(residual_particle[0].pos[0] - (start_x + 3.0 * step)) < 1e-12);

    rt_particles3d_update(residual, 1.0e300);
    assert(rt_particles3d_get_count(residual) == 1);
    assert(rt_particles3d_get_last_dropped_time(residual) > 1.0e299);
}

static void test_setters_sanitize_nonfinite_ranges() {
    void *ps = rt_particles3d_new(8);
    assert(ps != nullptr);
    ParticlesView *view = static_cast<ParticlesView *>(ps);

    rt_particles3d_set_position(ps, NAN, 2.0, INFINITY);
    assert(view->position[0] == 0.0);
    assert(view->position[1] == 2.0);
    assert(view->position[2] == 0.0);

    rt_particles3d_set_direction(ps, NAN, 0.0, INFINITY, INFINITY);
    assert(std::fabs(view->emit_dir[0]) < 1e-9);
    assert(std::fabs(view->emit_dir[1] - 1.0) < 1e-9);
    assert(std::fabs(view->emit_dir[2]) < 1e-9);
    assert(view->emit_spread == 0.0);
    rt_particles3d_set_direction(ps, 1.0, 0.0, 0.0, 90.0);
    assert(std::fabs(view->emit_dir[0] - 1.0) < 1e-9);
    assert(std::fabs(view->emit_spread - 1.5707963267948966) < 1e-9);
    rt_particles3d_set_direction(ps, 0.0, 1.0, 0.0, 720.0);
    assert(std::fabs(view->emit_spread - 3.1415926535897932) < 1e-9);

    rt_particles3d_set_speed(ps, 4.0, -2.0);
    assert(view->speed_min == 0.0);
    assert(view->speed_max == 4.0);

    rt_particles3d_set_lifetime(ps, NAN, -1.0);
    assert(view->life_min >= 0.01);
    assert(view->life_max >= 0.01);

    rt_particles3d_set_size(ps, NAN, -3.0);
    assert(view->size_start == 0.0);
    assert(view->size_end == 0.0);

    rt_particles3d_set_gravity(ps, INFINITY, -9.0, NAN);
    assert(view->gravity[0] == 0.0);
    assert(view->gravity[1] == -9.0);
    assert(view->gravity[2] == 0.0);

    rt_particles3d_set_alpha(ps, -1.0, 2.0);
    assert(view->alpha_start == 0.0);
    assert(view->alpha_end == 1.0);

    rt_particles3d_set_rate(ps, INFINITY);
    assert(view->rate == 0.0);
    rt_particles3d_set_additive(ps, 7);
    assert(view->additive_blend == 1);

    rt_particles3d_set_emitter_shape(ps, 99);
    assert(view->emitter_shape == 2);
    rt_particles3d_set_emitter_shape(ps, -1);
    assert(view->emitter_shape == 0);
    rt_particles3d_set_emitter_size(ps, NAN, -2.0, INFINITY);
    assert(view->emitter_size[0] == 0.0);
    assert(view->emitter_size[1] == 2.0);
    assert(view->emitter_size[2] == 0.0);
}

static void test_rebase_origin_shifts_emitter_and_live_particles() {
    void *ps = rt_particles3d_new(8);
    assert(ps != nullptr);
    ParticlesView *view = static_cast<ParticlesView *>(ps);

    rt_particles3d_set_position(ps, 1000.0, -5.0, 20.0);
    rt_particles3d_set_speed(ps, 0.0, 0.0);
    rt_particles3d_set_lifetime(ps, 10.0, 10.0);
    rt_particles3d_burst(ps, 1);
    assert(rt_particles3d_get_count(ps) == 1);

    rt_particles3d_rebase_origin(ps, 990.0, -7.0, 5.0);
    double pos[3] = {0.0, 0.0, 0.0};
    rt_particles3d_get_position(ps, pos);
    assert(std::fabs(pos[0] - 10.0) < 1e-9);
    assert(std::fabs(pos[1] - 2.0) < 1e-9);
    assert(std::fabs(pos[2] - 15.0) < 1e-9);
    assert(view->particles != nullptr);

    struct ParticleView {
        double pos[3];
        double vel[3];
        double age;
        double life;
        double size0;
        double size1;
        float color0[3];
        float color1[3];
        double alpha0;
        double alpha1;
    };

    ParticleView *particle = static_cast<ParticleView *>(view->particles);
    assert(std::fabs(particle[0].pos[0] - 10.0) < 1e-9);
    assert(std::fabs(particle[0].pos[1] - 2.0) < 1e-9);
    assert(std::fabs(particle[0].pos[2] - 15.0) < 1e-9);
}

static void reset_draw_records() {
    g_draw_mesh_calls = 0;
    g_draw_mesh_matrix_keyed_calls = 0;
    g_last_mesh_vertex_count = 0;
    g_last_mesh_index_count = 0;
    g_last_draw_alpha = 0.0;
    g_last_draw_additive = 0;
    g_last_draw_alpha_mode = 0;
    g_last_mesh_signature = 0;
    g_particle_batch_calls = 0;
    g_particle_batch_count = 0;
    g_particle_batch_alpha = 0.0;
    g_particle_batch_additive = 0;
    std::memset(g_last_mesh_quad_z, 0, sizeof(g_last_mesh_quad_z));
    std::memset(g_last_mesh_draw_quad_z, 0, sizeof(g_last_mesh_draw_quad_z));
    std::memset(g_last_mesh_vertices, 0, sizeof(g_last_mesh_vertices));
    std::memset(g_particle_batch_instances, 0, sizeof(g_particle_batch_instances));
    std::memset(g_keyed_draw_z, 0, sizeof(g_keyed_draw_z));
    std::memset(g_keyed_draw_alpha, 0, sizeof(g_keyed_draw_alpha));
    std::memset(g_keyed_draw_additive, 0, sizeof(g_keyed_draw_additive));
}

static rt_camera3d make_test_camera() {
    rt_camera3d cam = {};
    cam.view[0] = 1.0;
    cam.view[5] = 1.0;
    cam.view[10] = 1.0;
    cam.view[15] = 1.0;
    cam.projection[0] = 1.0;
    cam.projection[5] = 1.0;
    cam.projection[10] = 1.0;
    cam.projection[15] = 1.0;
    cam.eye[0] = 0.0;
    cam.eye[1] = 0.0;
    cam.eye[2] = 0.0;
    return cam;
}

static void test_draw_batches_additive_and_alpha_particles() {
    void *ps = rt_particles3d_new(8);
    rt_canvas3d canvas = {};
    rt_camera3d cam = make_test_camera();
    assert(ps != nullptr);

    rt_particles3d_set_position(ps, 0.0, 0.0, 1.0);
    rt_particles3d_burst(ps, 1);
    rt_particles3d_set_position(ps, 0.0, 0.0, 5.0);
    rt_particles3d_burst(ps, 1);
    rt_particles3d_set_position(ps, 0.0, 0.0, 3.0);
    rt_particles3d_burst(ps, 1);
    assert(rt_particles3d_get_count(ps) == 3);

    rt_particles3d_set_additive(ps, 1);
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_draw_mesh_calls == 1);
    assert(g_draw_mesh_matrix_keyed_calls == 0);
    assert(g_last_mesh_vertex_count == 12);
    assert(g_last_mesh_index_count == 18);
    assert(std::fabs(g_last_mesh_quad_z[0] - 1.0) < 1e-6);
    assert(std::fabs(g_last_draw_alpha - 1.0) < 1e-6);
    assert(g_last_draw_additive == 1);
    assert(g_last_draw_alpha_mode == kAlphaModeBlend);

    rt_particles3d_set_additive(ps, 0);
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_draw_mesh_calls == 1);
    assert(g_draw_mesh_matrix_keyed_calls == 0);
    assert(g_last_mesh_vertex_count == 12);
    assert(g_last_mesh_index_count == 18);
    assert(std::fabs(g_last_mesh_quad_z[0] - 5.0) < 1e-6);
    assert(std::fabs(g_last_mesh_quad_z[1] - 3.0) < 1e-6);
    assert(std::fabs(g_last_mesh_quad_z[2] - 1.0) < 1e-6);
    assert(std::fabs(g_last_draw_alpha - 1.0) < 1e-6);
    assert(g_last_draw_additive == 0);
    assert(g_last_draw_alpha_mode == kAlphaModeBlend);
}

/// @brief Prove compact records reconstruct the sorted software billboard mesh exactly.
/// @details Ten repeated hardware frames must preserve scratch capacity, queue one compact batch,
///   and submit no CPU billboard mesh.
static void test_hardware_instances_match_software_billboards_and_reuse_scratch() {
    void *ps = rt_particles3d_new(8);
    rt_canvas3d canvas = {};
    rt_camera3d cam = make_test_camera();
    vgfx3d_vertex_t software_vertices[8] = {};
    assert(ps != nullptr);

    rt_particles3d_set_direction(ps, 0.0, 1.0, 0.0, 0.0);
    rt_particles3d_set_speed(ps, 2.0, 2.0);
    rt_particles3d_set_lifetime(ps, 10.0, 10.0);
    rt_particles3d_set_size(ps, 2.0, 2.0);
    rt_particles3d_set_color(ps, 0x3366CC, 0x3366CC);
    rt_particles3d_set_alpha(ps, 0.75, 0.75);
    rt_particles3d_set_gravity(ps, 0.0, 0.0, 0.0);
    rt_particles3d_set_stretch(ps, 2.0);
    rt_particles3d_set_position(ps, 2.0, 3.0, 1.0);
    rt_particles3d_burst(ps, 1);
    rt_particles3d_set_position(ps, -4.0, 6.0, 5.0);
    rt_particles3d_burst(ps, 1);

    g_particle_instancing_supported = 0;
    canvas.frame_serial = 1;
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_draw_mesh_calls == 1);
    assert(g_particle_batch_calls == 0);
    assert(g_last_mesh_vertex_count == 8);
    assert(g_last_mesh_index_count == 12);
    std::memcpy(software_vertices, g_last_mesh_vertices, sizeof(software_vertices));

    g_particle_instancing_supported = 1;
    canvas.frame_serial = 2;
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_draw_mesh_calls == 0);
    assert(g_particle_batch_calls == 1);
    assert(g_particle_batch_count == 2);
    assert(std::fabs(g_particle_batch_alpha - 1.0) < 1e-6);
    assert(g_particle_batch_additive == 0);
    assert(std::fabs(g_particle_batch_instances[0].center[2] - 5.0f) < 1e-6f);
    assert(std::fabs(g_particle_batch_instances[1].center[2] - 1.0f) < 1e-6f);

    constexpr float unit_corners[4][2] = {
        {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    for (int particle = 0; particle < 2; particle++) {
        const vgfx3d_particle_instance_t &instance = g_particle_batch_instances[particle];
        assert(instance.center[3] == 1.0f);
        assert(instance.right[3] == 0.0f);
        assert(instance.up[3] == 0.0f);
        for (int corner = 0; corner < 4; corner++) {
            const vgfx3d_vertex_t &vertex = software_vertices[particle * 4 + corner];
            for (int axis = 0; axis < 3; axis++) {
                float reconstructed = instance.center[axis] +
                                      instance.right[axis] * unit_corners[corner][0] +
                                      instance.up[axis] * unit_corners[corner][1];
                assert(std::fabs(reconstructed - vertex.pos[axis]) < 1e-6f);
            }
            for (int channel = 0; channel < 4; channel++)
                assert(std::fabs(instance.color[channel] - vertex.color[channel]) < 1e-6f);
        }
    }

    int64_t retained_capacity = rt_particles3d_test_instance_scratch_capacity(ps);
    uint64_t retained_grows = rt_particles3d_test_instance_scratch_grow_count(ps);
    assert(retained_capacity >= 2);
    assert(retained_grows == 1);
    for (int frame = 3; frame <= 12; frame++) {
        canvas.frame_serial = frame;
        reset_draw_records();
        rt_particles3d_draw(ps, &canvas, &cam);
        assert(g_draw_mesh_calls == 0);
        assert(g_particle_batch_calls == 1);
        assert(g_particle_batch_count == 2);
        assert(rt_particles3d_test_instance_scratch_capacity(ps) == retained_capacity);
        assert(rt_particles3d_test_instance_scratch_grow_count(ps) == retained_grows);
    }
    g_particle_instancing_supported = 0;
}

/// @brief Prove alpha trails keep billboards and ribbons in one sortable primitive stream.
/// @details Hardware particle support must not split conventional-alpha billboards away from their
/// trails because the two submissions cannot establish a correct primitive-level blend order.
static void test_alpha_trails_use_one_cpu_primitive_stream() {
    void *ps = rt_particles3d_new(4);
    rt_canvas3d canvas = {};
    rt_camera3d cam = make_test_camera();
    assert(ps != nullptr);

    rt_particles3d_set_direction(ps, 1.0, 0.0, 0.0, 0.0);
    rt_particles3d_set_speed(ps, 1.0, 1.0);
    rt_particles3d_set_lifetime(ps, 10.0, 10.0);
    rt_particles3d_set_gravity(ps, 0.0, 0.0, 0.0);
    rt_particles3d_set_trail(ps, 0.1, 4);
    rt_particles3d_burst(ps, 1);
    rt_particles3d_update(ps, 0.05);

    g_particle_instancing_supported = 0;
    canvas.frame_serial = 20;
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_draw_mesh_calls == 1);
    assert(g_particle_batch_calls == 0);
    assert(g_last_mesh_vertex_count >= 8);
    int software_vertex_count = g_last_mesh_vertex_count;
    int software_index_count = g_last_mesh_index_count;

    g_particle_instancing_supported = 1;
    canvas.frame_serial = 21;
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_particle_batch_calls == 0);
    assert(g_particle_batch_count == 0);
    assert(g_draw_mesh_calls == 1);
    assert(g_last_mesh_vertex_count == software_vertex_count);
    assert(g_last_mesh_index_count == software_index_count);
    g_particle_instancing_supported = 0;
}

/// @brief Prove billboards and trail segments share a back-to-front draw order and trail tails fade
/// to zero.
static void test_alpha_trails_sort_all_quads_and_fade_complete_tail() {
    void *ps = rt_particles3d_new(2);
    auto *view = static_cast<ParticlesView *>(ps);
    rt_canvas3d canvas = {};
    rt_camera3d cam = make_test_camera();
    assert(ps != nullptr);

    rt_particles3d_set_speed(ps, 0.0, 0.0);
    rt_particles3d_set_lifetime(ps, 10.0, 10.0);
    rt_particles3d_set_size(ps, 1.0, 1.0);
    rt_particles3d_set_alpha(ps, 1.0, 1.0);
    rt_particles3d_set_additive(ps, 0);
    rt_particles3d_set_trail(ps, 1.0, 4);
    rt_particles3d_set_position(ps, 0.0, 0.0, 1.0);
    rt_particles3d_burst(ps, 1);
    rt_particles3d_set_position(ps, 0.0, 0.0, 5.0);
    rt_particles3d_burst(ps, 1);

    constexpr int trail_segments = 4;
    constexpr size_t trail_stride = trail_segments * 3u;
    for (int particle = 0; particle < 2; ++particle) {
        float depth = particle == 0 ? 10.0f : 3.0f;
        float *ring = &view->trail_pos[(size_t)particle * trail_stride];
        ring[0] = 0.0f;
        ring[1] = 0.0f;
        ring[2] = depth;
        ring[3] = 1.0f;
        ring[4] = 0.0f;
        ring[5] = depth;
        view->trail_len[particle] = 2;
        view->trail_head[particle] = 2;
    }

    g_particle_instancing_supported = 1;
    canvas.frame_serial = 22;
    reset_draw_records();
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(g_particle_batch_calls == 0);
    assert(g_draw_mesh_calls == 1);
    assert(g_last_mesh_vertex_count == 16);
    assert(g_last_mesh_index_count == 24);
    assert(std::fabs(g_last_mesh_draw_quad_z[0] - 10.0) < 1e-6);
    assert(std::fabs(g_last_mesh_draw_quad_z[1] - 5.0) < 1e-6);
    assert(std::fabs(g_last_mesh_draw_quad_z[2] - 3.0) < 1e-6);
    assert(std::fabs(g_last_mesh_draw_quad_z[3] - 1.0) < 1e-6);
    assert(std::fabs(g_last_mesh_vertices[9].color[3]) < 1e-6);
    assert(std::fabs(g_last_mesh_vertices[10].color[3]) < 1e-6);
    g_particle_instancing_supported = 0;
}

static void fill_particle_line(void *ps, double z_start, double z_step, int count) {
    rt_particles3d_set_speed(ps, 0.0, 0.0);
    rt_particles3d_set_lifetime(ps, 10.0, 10.0);
    rt_particles3d_set_size(ps, 1.0, 1.0);
    rt_particles3d_set_alpha(ps, 1.0, 1.0);
    for (int i = 0; i < count; ++i) {
        rt_particles3d_set_position(ps, 0.0, 0.0, z_start + z_step * static_cast<double>(i));
        rt_particles3d_burst(ps, 1);
    }
    assert(rt_particles3d_get_count(ps) == count);
}

static uint64_t draw_particle_signature(void *ps, rt_canvas3d *canvas, rt_camera3d *cam) {
    reset_draw_records();
    rt_particles3d_draw(ps, canvas, cam);
    assert(g_draw_mesh_calls == 1);
    assert(g_last_mesh_vertex_count == 4000);
    assert(g_last_mesh_index_count == 6000);
    return g_last_mesh_signature;
}

static void test_alpha_sort_scratch_grows_to_capacity_and_reuses_for_repeated_draws() {
    void *partial = rt_particles3d_new(1000);
    void *front_to_back = rt_particles3d_new(1000);
    void *back_to_front = rt_particles3d_new(1000);
    rt_canvas3d canvas = {};
    rt_camera3d cam = make_test_camera();
    assert(partial != nullptr);
    assert(front_to_back != nullptr);
    assert(back_to_front != nullptr);

    rt_particles3d_set_additive(partial, 0);
    fill_particle_line(partial, 0.0, 1.0, 16);
    reset_draw_records();
    rt_particles3d_draw(partial, &canvas, &cam);
    assert(g_draw_mesh_calls == 1);
    assert(rt_particles3d_test_sort_key_capacity(partial) == 1000);
    assert(rt_particles3d_test_sort_key_grow_count(partial) == 1);

    rt_particles3d_set_additive(front_to_back, 0);
    rt_particles3d_set_additive(back_to_front, 0);
    fill_particle_line(front_to_back, 0.0, 0.01, 1000);
    fill_particle_line(back_to_front, 9.99, -0.01, 1000);

    canvas.frame_serial = 1;
    uint64_t front_sig = draw_particle_signature(front_to_back, &canvas, &cam);
    uint64_t front_grows = rt_particles3d_test_sort_key_grow_count(front_to_back);
    assert(rt_particles3d_test_sort_key_capacity(front_to_back) == 1000);
    assert(front_grows == 1);

    canvas.frame_serial = 1;
    uint64_t back_sig = draw_particle_signature(back_to_front, &canvas, &cam);
    uint64_t back_grows = rt_particles3d_test_sort_key_grow_count(back_to_front);
    assert(rt_particles3d_test_sort_key_capacity(back_to_front) == 1000);
    assert(back_grows == 1);

    for (int frame = 2; frame <= 20; ++frame) {
        canvas.frame_serial = frame;
        assert(draw_particle_signature(front_to_back, &canvas, &cam) == front_sig);
        assert(rt_particles3d_test_sort_key_grow_count(front_to_back) == front_grows);

        canvas.frame_serial = frame;
        assert(draw_particle_signature(back_to_front, &canvas, &cam) == back_sig);
        assert(rt_particles3d_test_sort_key_grow_count(back_to_front) == back_grows);
    }
}

static void test_corrupted_emitter_state_is_persistently_repaired() {
    void *ps = rt_particles3d_new(8);
    auto *v = static_cast<ParticlesView *>(ps);
    assert(ps != nullptr);

    v->position[0] = NAN;
    v->position[1] = INFINITY;
    v->position[2] = -INFINITY;
    v->emit_dir[0] = 0.0;
    v->emit_dir[1] = 0.0;
    v->emit_dir[2] = 0.0;
    v->emit_spread = INFINITY;
    v->speed_min = -2.0;
    v->speed_max = NAN;
    v->life_min = -4.0;
    v->life_max = INFINITY;
    v->size_start = -1.0;
    v->size_end = NAN;
    v->gravity[0] = NAN;
    v->gravity[1] = INFINITY;
    v->gravity[2] = -INFINITY;
    v->color_start[0] = -1.0f;
    v->color_start[1] = 2.0f;
    v->color_start[2] = 0.5f;
    v->color_end[0] = 2.0f;
    v->color_end[1] = -1.0f;
    v->color_end[2] = 0.25f;
    v->alpha_start = -2.0;
    v->alpha_end = 4.0;
    v->rate = NAN;
    v->accumulator = INFINITY;
    v->emitting = 7;
    v->additive_blend = -4;
    v->stretch_k = INFINITY;
    v->softness = -5.0;
    v->emitter_shape = INT32_MAX;
    v->emitter_size[0] = -1.0;
    v->emitter_size[1] = NAN;
    v->emitter_size[2] = INFINITY;
    v->prng_state = 0;
    v->simulation_residual = INFINITY;
    v->dropped_time_total = -1.0;
    v->dropped_time_last_update = NAN;
    v->render_final_frame = -8;

    assert(rt_particles3d_get_rate(ps) == 0.0);
    assert(rt_particles3d_get_lifetime_min(ps) == 0.01);
    assert(rt_particles3d_get_lifetime_max(ps) == 0.01);
    assert(rt_particles3d_get_speed_min(ps) == 0.0);
    assert(rt_particles3d_get_speed_max(ps) == 0.0);
    assert(rt_particles3d_get_size_start(ps) == 0.0);
    assert(rt_particles3d_get_size_end(ps) == 0.0);
    assert(rt_particles3d_get_alpha_start(ps) == 0.0);
    assert(rt_particles3d_get_alpha_end(ps) == 1.0);
    assert(rt_particles3d_get_color_start(ps) == 0x00FF80);
    assert(rt_particles3d_get_color_end(ps) == 0xFF0040);
    assert(rt_particles3d_get_spread(ps) == 0.0);
    assert(rt_particles3d_get_emitter_shape(ps) == 0);
    assert(rt_particles3d_get_stretch(ps) == 0.0);
    assert(rt_particles3d_get_softness(ps) == 0.0);
    assert(rt_particles3d_get_seed(ps) != 0);
    assert(rt_particles3d_get_emitting(ps) == 1);
    assert(rt_particles3d_get_additive(ps) == 1);
    assert(rt_particles3d_get_render_final_frame(ps) == 1);
    assert(rt_particles3d_get_dropped_time(ps) == 0.0);
    assert(rt_particles3d_get_last_dropped_time(ps) == 0.0);
    assert(rt_particles3d_get_residual_time(ps) == 0.0);

    (void)rt_particles3d_get_position_vec3(ps);
    assert(g_last_vec3[0] == 0.0 && g_last_vec3[1] == 0.0 && g_last_vec3[2] == 0.0);
    (void)rt_particles3d_get_direction(ps);
    assert(g_last_vec3[0] == 0.0 && g_last_vec3[1] == 1.0 && g_last_vec3[2] == 0.0);
    (void)rt_particles3d_get_gravity(ps);
    assert(g_last_vec3[0] == 0.0 && g_last_vec3[1] == 0.0 && g_last_vec3[2] == 0.0);
    (void)rt_particles3d_get_emitter_size(ps);
    assert(g_last_vec3[0] == 1.0 && g_last_vec3[1] == 0.0 && g_last_vec3[2] == 0.0);
    assert(v->rate == 0.0);
    assert(v->life_min == 0.01 && v->life_max == 0.01);
    assert(v->emitting == 1 && v->additive_blend == 1 && v->render_final_frame == 1);
}

static void test_pool_and_trail_mirrors_restore_owned_storage() {
    void *ps = rt_particles3d_new(8);
    auto *v = static_cast<ParticlesView *>(ps);
    rt_particles3d_burst(ps, 3);
    void *owned_particles = v->particles;
    auto *foreign_particles =
        static_cast<RealParticleView *>(std::calloc(8u, sizeof(RealParticleView)));
    assert(foreign_particles != nullptr);
    v->particles = foreign_particles;
    v->max_particles = INT32_MAX;
    v->count = 3;
    assert(rt_particles3d_get_count(ps) == 3);
    assert(v->particles == owned_particles);
    assert(v->max_particles == 8);
    std::free(foreign_particles);

    rt_particles3d_set_trail(ps, 1.0, 4);
    float *owned_pos = v->trail_pos;
    float *owned_age = v->trail_age;
    int16_t *owned_len = v->trail_len;
    int16_t *owned_head = v->trail_head;
    auto *foreign_pos = static_cast<float *>(std::calloc(8u * 4u * 3u, sizeof(float)));
    auto *foreign_age = static_cast<float *>(std::calloc(8u, sizeof(float)));
    auto *foreign_len = static_cast<int16_t *>(std::calloc(8u, sizeof(int16_t)));
    auto *foreign_head = static_cast<int16_t *>(std::calloc(8u, sizeof(int16_t)));
    assert(foreign_pos && foreign_age && foreign_len && foreign_head);
    v->trail_pos = foreign_pos;
    v->trail_age = foreign_age;
    v->trail_len = foreign_len;
    v->trail_head = foreign_head;
    rt_particles3d_set_trail(ps, 2.0, 4);
    assert(v->trail_pos == owned_pos);
    assert(v->trail_age == owned_age);
    assert(v->trail_len == owned_len);
    assert(v->trail_head == owned_head);
    assert(v->trail_lifetime == 2.0f);
    std::free(foreign_pos);
    std::free(foreign_age);
    std::free(foreign_len);
    std::free(foreign_head);
}

static void test_scalar_getters_do_not_scan_live_trails() {
    void *ps = rt_particles3d_new(4);
    auto *v = static_cast<ParticlesView *>(ps);
    rt_particles3d_burst(ps, 1);
    rt_particles3d_set_trail(ps, 1.0, 4);
    v->trail_age[0] = NAN;
    v->trail_len[0] = 99;
    v->trail_head[0] = 99;

    assert(rt_particles3d_get_count(ps) == 1);
    assert(std::isnan(v->trail_age[0]));
    assert(v->trail_len[0] == 99);
    assert(v->trail_head[0] == 99);

    rt_particles3d_update(ps, 0.001);
    assert(v->trail_age[0] == 0.0f);
    assert(v->trail_len[0] == 0);
    assert(v->trail_head[0] == 0);
}

static void test_texture_mirror_restores_retained_owner() {
    void *ps = rt_particles3d_new(4);
    auto *v = static_cast<ParticlesView *>(ps);
    rt_pixels_impl *owned_texture = make_test_pixels(0xFFFFFFFFu);
    rt_pixels_impl *foreign_texture = make_test_pixels(0x000000FFu);
    rt_particles3d_set_texture(ps, owned_texture);
    assert(v->texture == owned_texture);
    v->texture = foreign_texture;
    assert(rt_particles3d_get_texture(ps) == owned_texture);
    assert(v->texture == owned_texture);
}

static void test_draw_and_scratch_mirrors_restore_owned_storage() {
    void *ps = rt_particles3d_new(8);
    auto *v = static_cast<ParticlesView *>(ps);
    rt_canvas3d canvas = {};
    rt_camera3d cam = make_test_camera();
    rt_particles3d_set_lifetime(ps, 10.0, 10.0);
    rt_particles3d_burst(ps, 3);

    g_particle_instancing_supported = 0;
    canvas.frame_serial = 70;
    rt_particles3d_draw(ps, &canvas, &cam);
    auto *owned_vertices = v->draw_vertices[0];
    auto *owned_indices = v->draw_indices[0];
    void *owned_material = v->draw_materials[0];
    uint32_t vertex_capacity = v->draw_vertex_capacity[0];
    uint32_t index_capacity = v->draw_index_capacity[0];
    auto *foreign_vertices =
        static_cast<vgfx3d_vertex_t *>(std::calloc(vertex_capacity, sizeof(vgfx3d_vertex_t)));
    auto *foreign_indices = static_cast<uint32_t *>(std::calloc(index_capacity, sizeof(uint32_t)));
    void *foreign_material = rt_material3d_new();
    assert(foreign_vertices && foreign_indices && foreign_material);
    v->draw_vertices[0] = foreign_vertices;
    v->draw_indices[0] = foreign_indices;
    v->draw_materials[0] = foreign_material;
    canvas.frame_serial = 71;
    rt_particles3d_draw(ps, &canvas, &cam);
    assert(v->draw_vertices[0] == owned_vertices);
    assert(v->draw_indices[0] == owned_indices);
    assert(v->draw_materials[0] == owned_material);
    std::free(foreign_vertices);
    std::free(foreign_indices);
    rt_obj_free(foreign_material);

    void *owned_sort_keys = v->sort_keys;
    void *owned_sort_scratch = v->sort_scratch;
    int32_t owned_sort_capacity = v->sort_key_capacity;
    void *foreign_sort_keys = std::calloc(8u, 32u);
    void *foreign_sort_scratch = std::calloc(8u, 32u);
    v->sort_keys = foreign_sort_keys;
    v->sort_scratch = foreign_sort_scratch;
    v->sort_key_capacity = INT32_MAX;
    assert(rt_particles3d_test_sort_key_capacity(ps) == owned_sort_capacity);
    assert(v->sort_keys == owned_sort_keys);
    assert(v->sort_scratch == owned_sort_scratch);
    std::free(foreign_sort_keys);
    std::free(foreign_sort_scratch);

    g_particle_instancing_supported = 1;
    canvas.frame_serial = 72;
    rt_particles3d_draw(ps, &canvas, &cam);
    auto *owned_instances = v->instance_scratch;
    int32_t owned_instance_capacity = v->instance_scratch_capacity;
    auto *foreign_instances = static_cast<vgfx3d_particle_instance_t *>(
        std::calloc(8u, sizeof(vgfx3d_particle_instance_t)));
    v->instance_scratch = foreign_instances;
    v->instance_scratch_capacity = INT32_MAX;
    assert(rt_particles3d_test_instance_scratch_capacity(ps) == owned_instance_capacity);
    assert(v->instance_scratch == owned_instances);
    std::free(foreign_instances);
    g_particle_instancing_supported = 0;
}

static void test_overflow_table_mirrors_restore_owned_storage() {
    void *ps = rt_particles3d_new(4);
    auto *v = static_cast<ParticlesView *>(ps);
    rt_canvas3d canvas = {};
    rt_camera3d cam = make_test_camera();
    rt_particles3d_set_lifetime(ps, 10.0, 10.0);
    rt_particles3d_burst(ps, 1);
    canvas.frame_serial = 90;
    for (int i = 0; i < 5; i++)
        rt_particles3d_draw(ps, &canvas, &cam);
    assert(v->overflow_draw_slot_capacity > 0);

    auto *owned_vertices = v->overflow_draw_vertices;
    auto *owned_indices = v->overflow_draw_indices;
    auto *owned_vertex_caps = v->overflow_draw_vertex_capacity;
    auto *owned_index_caps = v->overflow_draw_index_capacity;
    auto *owned_materials = v->overflow_draw_materials;
    int32_t owned_capacity = v->overflow_draw_slot_capacity;
    auto *foreign_vertices = static_cast<vgfx3d_vertex_t **>(
        std::calloc(static_cast<size_t>(owned_capacity), sizeof(vgfx3d_vertex_t *)));
    auto *foreign_indices = static_cast<uint32_t **>(
        std::calloc(static_cast<size_t>(owned_capacity), sizeof(uint32_t *)));
    auto *foreign_vertex_caps =
        static_cast<uint32_t *>(std::calloc(static_cast<size_t>(owned_capacity), sizeof(uint32_t)));
    auto *foreign_index_caps =
        static_cast<uint32_t *>(std::calloc(static_cast<size_t>(owned_capacity), sizeof(uint32_t)));
    auto *foreign_materials =
        static_cast<void **>(std::calloc(static_cast<size_t>(owned_capacity), sizeof(void *)));
    assert(foreign_vertices && foreign_indices && foreign_vertex_caps && foreign_index_caps &&
           foreign_materials);
    v->overflow_draw_vertices = foreign_vertices;
    v->overflow_draw_indices = foreign_indices;
    v->overflow_draw_vertex_capacity = foreign_vertex_caps;
    v->overflow_draw_index_capacity = foreign_index_caps;
    v->overflow_draw_materials = foreign_materials;
    v->overflow_draw_slot_capacity = owned_capacity;
    assert(rt_particles3d_get_count(ps) == 1);
    assert(v->overflow_draw_vertices == owned_vertices);
    assert(v->overflow_draw_indices == owned_indices);
    assert(v->overflow_draw_vertex_capacity == owned_vertex_caps);
    assert(v->overflow_draw_index_capacity == owned_index_caps);
    assert(v->overflow_draw_materials == owned_materials);
    assert(v->overflow_draw_slot_capacity == owned_capacity);
    std::free(foreign_vertices);
    std::free(foreign_indices);
    std::free(foreign_vertex_caps);
    std::free(foreign_index_caps);
    std::free(foreign_materials);

    rt_particles3d_test_age_overflow_draw_slots(ps, 119);
    assert(v->overflow_draw_slot_capacity == owned_capacity);
    rt_particles3d_test_age_overflow_draw_slots(ps, 1);
    assert(v->overflow_draw_slot_capacity == 0);
    assert(v->overflow_draw_vertices == nullptr && v->overflow_draw_indices == nullptr);
}

int main() {
    expect_trap_on_invalid_capacity();
    test_burst_and_clear();
    test_burst_caps_to_available_pool();
    test_start_stop_and_update_spawns_particles();
    test_particles_expire_after_lifetime();
    test_particle_final_frame_renders_end_state();
    test_particle_final_frame_can_be_disabled();
    test_update_is_bounded_and_reports_exact_dropped_time();
    test_setters_sanitize_nonfinite_ranges();
    test_rebase_origin_shifts_emitter_and_live_particles();
    test_draw_batches_additive_and_alpha_particles();
    test_hardware_instances_match_software_billboards_and_reuse_scratch();
    test_alpha_trails_use_one_cpu_primitive_stream();
    test_alpha_trails_sort_all_quads_and_fade_complete_tail();
    test_alpha_sort_scratch_grows_to_capacity_and_reuses_for_repeated_draws();
    test_corrupted_emitter_state_is_persistently_repaired();
    test_pool_and_trail_mirrors_restore_owned_storage();
    test_scalar_getters_do_not_scan_live_trails();
    test_texture_mirror_restores_retained_owner();
    test_draw_and_scratch_mirrors_restore_owned_storage();
    test_overflow_table_mirrors_restore_owned_storage();
    std::printf("RTParticles3DContractTests passed.\n");
    return 0;
}
