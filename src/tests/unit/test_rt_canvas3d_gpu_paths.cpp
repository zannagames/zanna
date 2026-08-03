//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_canvas3d_gpu_paths.cpp
// Purpose: Unit coverage for Canvas3D deferred submission, GPU-facing payloads,
//   backend-path policy, and frame telemetry counters.
//
// Key invariants:
//   - Stack Canvas3D fixtures exercise backend dispatch without opening windows.
//   - Deferred draw mirrors stay layout-compatible with runtime deferred draws.
//   - Compact particle commands snapshot instances and share retained unit geometry.
//
// Ownership/Lifetime:
//   - Test-created runtime objects are process-local and released by existing helpers.
//   - Fake backend records point to copied command payloads where lifetime matters.
//
// Links: src/runtime/graphics/3d/render/rt_canvas3d.c,
//   src/runtime/graphics/3d/backend/vgfx3d_backend.h
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
extern "C" {
#include "rt_canvas3d_internal.h"
#include "rt_pixels_internal.h"
#include "vgfx3d_brdf_lut.h"
}
#include "rt_heap.h"
#include "rt_input.h"
#include "rt_instbatch3d.h"
#include "rt_morphtarget3d.h"
#include "rt_postfx3d.h"
#include "rt_skeleton3d.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "vgfx3d_backend.h"

#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

extern "C" {
extern void *rt_mat4_identity(void);
extern void *rt_mat4_translate(double tx, double ty, double tz);
extern rt_string rt_const_cstr(const char *s);
extern void *rt_pixels_new(int64_t width, int64_t height);
extern void rt_pixels_set(void *pixels, int64_t x, int64_t y, int64_t color);
extern void rt_pixels_set_rgba(void *pixels, int64_t x, int64_t y, int64_t rgba);
extern void rt_pixels_fill_rgba(void *pixels, int64_t rgba);
extern void *rt_canvas3d_screenshot(void *canvas);
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
}

static int tests_run = 0;
static int tests_passed = 0;
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
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

typedef struct {
    int kind;
    int pass_kind;
    vgfx3d_draw_cmd_t cmd;
    const float *instance_matrices;
    int32_t instance_count;
    vgfx3d_light_params_t *lights;
    int32_t light_count;
    int32_t light_offset;
    float ambient[3];
    int8_t wireframe;
    int8_t backface_cull;
    int8_t has_local_bounds;
    int8_t visible;
    int8_t requires_blend;
    int8_t conservative_bounds;
    int8_t occlusion_test_disabled;
    int8_t occlusion_write_disabled;
    float culling_pad;
    float local_bounds_min[3];
    float local_bounds_max[3];
    int8_t has_world_bounds;
    float world_bounds_min[3];
    float world_bounds_max[3];
    float sort_key;
    uintptr_t stable_sort_id;
    uintptr_t occlusion_fingerprint;
    uintptr_t occlusion_key;
    int32_t enqueue_index;
} test_deferred_draw_t;

typedef struct {
    void *vptr;
    float *heights;
    int32_t width;
    int32_t depth;
    int64_t height_count;
    double scale[3];
    void **chunk_meshes;
    void **chunk_meshes_lod1;
    void **chunk_meshes_lod2;
    float *chunk_aabbs;
    uint8_t *chunk_lod_state;
    int32_t chunks_x;
    int32_t chunks_z;
    int32_t chunk_capacity;
    void *material;
    float lod_dist1;
    float lod_dist2;
    float lod_hysteresis;
    float skirt_depth;
    void *splat_map;
    void *splat_map2;
    void *layer_textures[8];
    double layer_scales[8];
    void *base_texture;
    void *baked_texture;
    int8_t splat_dirty;
} test_terrain3d_view;

static vgfx3d_backend_t make_backend(const char *name) {
    vgfx3d_backend_t backend = {};
    backend.name = name;
    /* Mocks mirror the real vtables: GPU backends consume bone palettes. */
    backend.gpu_skinning = (name != nullptr && std::strcmp(name, "software") != 0) ? 1 : 0;
    return backend;
}

static vgfx3d_backend_t kOpenGLBackend = make_backend("opengl");
static vgfx3d_backend_t kD3D11Backend = make_backend("d3d11");
static vgfx3d_backend_t kMetalBackend = make_backend("metal");
static vgfx3d_backend_t kSoftwareBackend = make_backend("software");

static int skybox_draw_calls = 0;
static int shadow_begin_calls = 0;
static int shadow_draw_calls = 0;
static int shadow_end_calls = 0;
static int draw_submit_calls = 0;
static const void *submitted_geometry_keys[16];
static const void *submitted_textures[16];
static int submitted_order_count = 0;
static int shadow_begin_slots[VGFX3D_MAX_SHADOW_LIGHTS];
static int shadow_end_slots[VGFX3D_MAX_SHADOW_LIGHTS];
static float shadow_vps[VGFX3D_MAX_SHADOW_LIGHTS][16];
static vgfx3d_light_params_t last_draw_lights[VGFX3D_MAX_LIGHTS];
static int32_t last_draw_light_count = 0;
static vgfx3d_draw_cmd_t last_instanced_cmd;
static const float *last_instance_matrices = nullptr;
static int32_t last_instance_count = 0;
static float *last_instance_matrices_copy = nullptr;
static float *last_prev_instance_matrices_copy = nullptr;
static int32_t last_readback_w = 0;
static int32_t last_readback_h = 0;
static int32_t last_readback_stride = 0;
static int begin_frame_calls = 0;
static vgfx3d_camera_params_t begin_frame_params[4];
static int set_gpu_postfx_enabled_calls = 0;
static int8_t set_gpu_postfx_enabled_values[4];
static int set_gpu_postfx_snapshot_calls = 0;
static int8_t set_gpu_postfx_snapshot_present[4];
static vgfx3d_postfx_chain_t set_gpu_postfx_chains[4];
static int final_submit_draw_calls = 0;
static int final_end_frame_calls = 0;
static int final_readback_calls = 0;
static int final_readback_saw_finalized = 0;
static int final_readback_saw_submit_count = 0;
static int final_present_postfx_calls = 0;
static int final_present_postfx_saw_submit_count = 0;
static int final_apply_postfx_calls = 0;
static int final_apply_postfx_saw_submit_count = 0;
static int final_present_calls = 0;
static int final_present_saw_submit_count = 0;
static vgfx3d_draw_cmd_t final_last_draw_cmd;

static void noop_end_frame(void *) {}

static void noop_present_postfx(void *, const vgfx3d_postfx_chain_t *) {}

static void record_present_postfx(void *, const vgfx3d_postfx_chain_t *) {
    final_present_postfx_calls++;
    final_present_postfx_saw_submit_count = final_submit_draw_calls;
}

static void record_apply_postfx(void *, const vgfx3d_postfx_chain_t *) {
    final_apply_postfx_calls++;
    final_apply_postfx_saw_submit_count = final_submit_draw_calls;
}

static void record_present(void *) {
    final_present_calls++;
    final_present_saw_submit_count = final_submit_draw_calls;
}

static void noop_draw(void *,
                      vgfx_window_t,
                      const vgfx3d_draw_cmd_t *,
                      const vgfx3d_light_params_t *,
                      int32_t,
                      const float *,
                      int8_t,
                      int8_t) {}

static void record_final_draw(void *,
                              vgfx_window_t,
                              const vgfx3d_draw_cmd_t *cmd,
                              const vgfx3d_light_params_t *,
                              int32_t,
                              const float *,
                              int8_t,
                              int8_t) {
    final_submit_draw_calls++;
    if (cmd)
        final_last_draw_cmd = *cmd;
}

static void record_final_end_frame(void *) {
    final_end_frame_calls++;
}

static void record_draw_skybox(void *, const void *) {
    skybox_draw_calls++;
}

static void reset_submission_order(void) {
    submitted_order_count = 0;
    std::memset(submitted_geometry_keys, 0, sizeof(submitted_geometry_keys));
    std::memset(submitted_textures, 0, sizeof(submitted_textures));
}

static void reset_shadow_counts(void) {
    shadow_begin_calls = 0;
    shadow_draw_calls = 0;
    shadow_end_calls = 0;
    reset_submission_order();
    std::memset(shadow_begin_slots, 0xFF, sizeof(shadow_begin_slots));
    std::memset(shadow_end_slots, 0xFF, sizeof(shadow_end_slots));
    std::memset(shadow_vps, 0, sizeof(shadow_vps));
    std::memset(last_draw_lights, 0, sizeof(last_draw_lights));
    last_draw_light_count = 0;
    draw_submit_calls = 0;
}

static void record_shadow_begin(
    void *, int32_t slot, float *, int32_t, int32_t, const float *light_vp) {
    if (shadow_begin_calls < VGFX3D_MAX_SHADOW_LIGHTS)
        shadow_begin_slots[shadow_begin_calls] = slot;
    if (light_vp && slot >= 0 && slot < VGFX3D_MAX_SHADOW_LIGHTS)
        std::memcpy(shadow_vps[slot], light_vp, sizeof(shadow_vps[slot]));
    shadow_begin_calls++;
}

static void record_shadow_draw(void *, const vgfx3d_draw_cmd_t *) {
    shadow_draw_calls++;
}

static void record_shadow_end(void *, int32_t slot, float) {
    if (shadow_end_calls < VGFX3D_MAX_SHADOW_LIGHTS)
        shadow_end_slots[shadow_end_calls] = slot;
    shadow_end_calls++;
}

static void record_draw_with_lights(void *,
                                    vgfx_window_t,
                                    const vgfx3d_draw_cmd_t *cmd,
                                    const vgfx3d_light_params_t *lights,
                                    int32_t light_count,
                                    const float *,
                                    int8_t,
                                    int8_t) {
    draw_submit_calls++;
    if (submitted_order_count < 16) {
        submitted_geometry_keys[submitted_order_count] = cmd ? cmd->geometry_key : nullptr;
        submitted_textures[submitted_order_count] = cmd ? cmd->texture : nullptr;
        submitted_order_count++;
    }
    last_draw_light_count = light_count > VGFX3D_MAX_LIGHTS ? VGFX3D_MAX_LIGHTS : light_count;
    if (lights && last_draw_light_count > 0)
        std::memcpy(last_draw_lights,
                    lights,
                    static_cast<size_t>(last_draw_light_count) * sizeof(vgfx3d_light_params_t));
}

static void reset_recorded_instancing(void) {
    std::free(last_instance_matrices_copy);
    std::free(last_prev_instance_matrices_copy);
    last_instance_matrices_copy = nullptr;
    last_prev_instance_matrices_copy = nullptr;
    last_instance_matrices = nullptr;
    last_instance_count = 0;
    std::memset(&last_instanced_cmd, 0, sizeof(last_instanced_cmd));
}

static void record_draw_instanced(void *,
                                  vgfx_window_t,
                                  const vgfx3d_draw_cmd_t *cmd,
                                  const float *instance_matrices,
                                  int32_t instance_count,
                                  const vgfx3d_light_params_t *,
                                  int32_t,
                                  const float *,
                                  int8_t,
                                  int8_t) {
    reset_recorded_instancing();
    if (cmd)
        last_instanced_cmd = *cmd;
    last_instance_count = instance_count;
    if (instance_matrices && instance_count > 0) {
        size_t bytes = static_cast<size_t>(instance_count) * 16u * sizeof(float);
        last_instance_matrices_copy = (float *)std::malloc(bytes);
        if (last_instance_matrices_copy) {
            std::memcpy(last_instance_matrices_copy, instance_matrices, bytes);
            last_instance_matrices = last_instance_matrices_copy;
        }
    }
    if (cmd && cmd->prev_instance_matrices && cmd->has_prev_instance_matrices &&
        instance_count > 0) {
        size_t bytes = static_cast<size_t>(instance_count) * 16u * sizeof(float);
        last_prev_instance_matrices_copy = (float *)std::malloc(bytes);
        if (last_prev_instance_matrices_copy) {
            std::memcpy(last_prev_instance_matrices_copy, cmd->prev_instance_matrices, bytes);
            last_instanced_cmd.prev_instance_matrices = last_prev_instance_matrices_copy;
        } else {
            last_instanced_cmd.prev_instance_matrices = nullptr;
        }
    }
}

static int record_readback_rgba(void *, uint8_t *dst_rgba, int32_t w, int32_t h, int32_t stride) {
    last_readback_w = w;
    last_readback_h = h;
    last_readback_stride = stride;
    if (!dst_rgba || stride < w * 4)
        return 0;
    std::memset(dst_rgba, 0, static_cast<size_t>(stride) * static_cast<size_t>(h));
    dst_rgba[0] = 0x12;
    dst_rgba[1] = 0x34;
    dst_rgba[2] = 0x56;
    dst_rgba[3] = 0x78;
    return 1;
}

static int record_hidpi_readback_rgba(
    void *, uint8_t *dst_rgba, int32_t w, int32_t h, int32_t stride) {
    static const uint8_t colors[4][4] = {
        {0x10, 0x20, 0x30, 0xFF},
        {0x40, 0x50, 0x60, 0xFF},
        {0x70, 0x80, 0x90, 0xFF},
        {0xA0, 0xB0, 0xC0, 0xFF},
    };

    last_readback_w = w;
    last_readback_h = h;
    last_readback_stride = stride;
    if (!dst_rgba || w <= 0 || h <= 0 || stride < w * 4)
        return 0;
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            int quadrant = (y >= h / 2 ? 2 : 0) + (x >= w / 2 ? 1 : 0);
            uint8_t *p = dst_rgba + (size_t)y * (size_t)stride + (size_t)x * 4u;
            p[0] = colors[quadrant][0];
            p[1] = colors[quadrant][1];
            p[2] = colors[quadrant][2];
            p[3] = colors[quadrant][3];
        }
    }
    return 1;
}

static int record_final_readback_rgba(
    void *ctx, uint8_t *dst_rgba, int32_t w, int32_t h, int32_t stride) {
    rt_canvas3d *canvas = (rt_canvas3d *)ctx;
    final_readback_calls++;
    final_readback_saw_finalized = canvas && canvas->frame_finalized ? 1 : 0;
    final_readback_saw_submit_count = final_submit_draw_calls;
    return record_readback_rgba(ctx, dst_rgba, w, h, stride);
}

static void reset_postfx_records(void) {
    begin_frame_calls = 0;
    std::memset(begin_frame_params, 0, sizeof(begin_frame_params));
    set_gpu_postfx_enabled_calls = 0;
    std::memset(set_gpu_postfx_enabled_values, 0, sizeof(set_gpu_postfx_enabled_values));
    for (vgfx3d_postfx_chain_t &chain : set_gpu_postfx_chains)
        vgfx3d_postfx_chain_free(&chain);
    set_gpu_postfx_snapshot_calls = 0;
    std::memset(set_gpu_postfx_snapshot_present, 0, sizeof(set_gpu_postfx_snapshot_present));
    std::memset(set_gpu_postfx_chains, 0, sizeof(set_gpu_postfx_chains));
}

static void reset_final_frame_records(void) {
    final_submit_draw_calls = 0;
    final_end_frame_calls = 0;
    final_readback_calls = 0;
    final_readback_saw_finalized = 0;
    final_readback_saw_submit_count = 0;
    final_present_postfx_calls = 0;
    final_present_postfx_saw_submit_count = 0;
    final_apply_postfx_calls = 0;
    final_apply_postfx_saw_submit_count = 0;
    final_present_calls = 0;
    final_present_saw_submit_count = 0;
    std::memset(&final_last_draw_cmd, 0, sizeof(final_last_draw_cmd));
    last_readback_w = 0;
    last_readback_h = 0;
    last_readback_stride = 0;
}

static void record_begin_frame(void *, const vgfx3d_camera_params_t *cam) {
    if (begin_frame_calls < (int)(sizeof(begin_frame_params) / sizeof(begin_frame_params[0])) &&
        cam)
        begin_frame_params[begin_frame_calls] = *cam;
    begin_frame_calls++;
}

static void record_set_gpu_postfx_enabled(void *, int8_t enabled) {
    if (set_gpu_postfx_enabled_calls <
        (int)(sizeof(set_gpu_postfx_enabled_values) / sizeof(set_gpu_postfx_enabled_values[0]))) {
        set_gpu_postfx_enabled_values[set_gpu_postfx_enabled_calls] = enabled;
    }
    set_gpu_postfx_enabled_calls++;
}

static void record_set_gpu_postfx_snapshot(void *, const vgfx3d_postfx_chain_t *snapshot) {
    if (set_gpu_postfx_snapshot_calls < (int)(sizeof(set_gpu_postfx_snapshot_present) /
                                              sizeof(set_gpu_postfx_snapshot_present[0]))) {
        set_gpu_postfx_snapshot_present[set_gpu_postfx_snapshot_calls] = snapshot ? 1 : 0;
        if (snapshot)
            vgfx3d_postfx_chain_copy(&set_gpu_postfx_chains[set_gpu_postfx_snapshot_calls],
                                     snapshot);
    }
    set_gpu_postfx_snapshot_calls++;
}

static void set_identity4x4(float *m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void set_identity4x4d(double *m) {
    std::memset(m, 0, sizeof(double) * 16);
    m[0] = 1.0;
    m[5] = 1.0;
    m[10] = 1.0;
    m[15] = 1.0;
}

static void init_fake_canvas(rt_canvas3d *canvas, const vgfx3d_backend_t *backend) {
    std::memset(canvas, 0, sizeof(*canvas));
    canvas->backend = backend;
    canvas->gfx_win = (vgfx_window_t)1;
    canvas->in_frame = 1;
    set_identity4x4(canvas->cached_vp);
    std::memcpy(canvas->last_scene_vp, canvas->cached_vp, sizeof(canvas->last_scene_vp));
    canvas->has_last_scene_vp = 1;
}

static void cleanup_fake_canvas(rt_canvas3d *canvas) {
    canvas3d_release_retained_mesh_revisions(canvas);
    canvas3d_frame_arena_free(canvas);
    for (int32_t i = 0; i < canvas->temp_buf_count; i++)
        std::free(canvas->temp_buffers[i]);
    for (int32_t i = 0; i < canvas->temp_obj_count; i++) {
        if (canvas->temp_objects[i] && rt_obj_release_check0(canvas->temp_objects[i]))
            rt_obj_free(canvas->temp_objects[i]);
    }
    std::free(canvas->temp_buffers);
    std::free(canvas->temp_objects);
    std::free(canvas->temp_buffer_set);
    std::free(canvas->temp_object_set);
    std::free(canvas->float_snapshots);
    std::free(canvas->mesh_snapshots);
    std::free(canvas->mesh_snapshot_hash);
    for (int32_t i = 0; i < canvas->final_overlay_temp_buf_count; i++)
        std::free(canvas->final_overlay_temp_buffers[i]);
    std::free(canvas->final_overlay_cmds);
    std::free(canvas->final_overlay_temp_buffers);
    std::free(canvas->draw_cmds);
    std::free(canvas->sort_cmds);
    std::free(canvas->motion_history);
    std::free(canvas->readback_rgba_scratch);
    if (canvas->postfx && rt_obj_release_check0(canvas->postfx))
        rt_obj_free(canvas->postfx);
    vgfx3d_postfx_chain_free(&canvas->frame_postfx_chain);
    canvas->temp_buffers = nullptr;
    canvas->temp_objects = nullptr;
    canvas->temp_buffer_set = nullptr;
    canvas->temp_object_set = nullptr;
    canvas->float_snapshots = nullptr;
    canvas->mesh_snapshots = nullptr;
    canvas->mesh_snapshot_hash = nullptr;
    canvas->final_overlay_cmds = nullptr;
    canvas->final_overlay_temp_buffers = nullptr;
    canvas->draw_cmds = nullptr;
    canvas->sort_cmds = nullptr;
    canvas->motion_history = nullptr;
    canvas->readback_rgba_scratch = nullptr;
    canvas->readback_rgba_scratch_capacity = 0;
    canvas->postfx = nullptr;
    canvas->temp_buf_count = canvas->temp_buf_capacity = 0;
    canvas->temp_obj_count = canvas->temp_obj_capacity = 0;
    canvas->temp_buffer_set_capacity = 0;
    canvas->temp_object_set_capacity = 0;
    canvas->float_snapshot_count = canvas->float_snapshot_capacity = 0;
    canvas->mesh_snapshot_count = canvas->mesh_snapshot_capacity = 0;
    canvas->mesh_snapshot_hash_capacity = 0;
    canvas->final_overlay_count = canvas->final_overlay_capacity = 0;
    canvas->final_overlay_temp_buf_count = canvas->final_overlay_temp_buf_capacity = 0;
    canvas->draw_count = canvas->draw_capacity = 0;
    canvas->sort_capacity = 0;
    canvas->motion_history_count = canvas->motion_history_capacity = 0;
    reset_recorded_instancing();
}

static void reset_canvas_frame(rt_canvas3d *canvas, int64_t frame_serial) {
    canvas->draw_count = 0;
    canvas->frame_serial = frame_serial;
    canvas->in_frame = 1;
    canvas->frame_is_2d = 0;
    canvas->frame_draws_submitted = 0;
    canvas->frame_aabb_transforms = 0;
    canvas->frame_sort_passes = 0;
    canvas->frame_backend_state_changes = 0;
    canvas->frame_has_backend_state_key = 0;
    canvas->frame_last_backend_state_key = 0;
    std::memset(canvas->world_bounds_cache, 0, sizeof(canvas->world_bounds_cache));
}

static void enable_latched_motion_blur(rt_canvas3d *canvas) {
    if (!canvas)
        return;
    canvas->frame_gpu_postfx_enabled = 1;
    canvas->frame_postfx_state_latched = 1;
    vgfx3d_postfx_chain_free(&canvas->frame_postfx_chain);
    std::memset(&canvas->frame_postfx_chain, 0, sizeof(canvas->frame_postfx_chain));
    canvas->frame_postfx_chain.effects =
        (vgfx3d_postfx_effect_desc_t *)std::calloc(1, sizeof(vgfx3d_postfx_effect_desc_t));
    if (!canvas->frame_postfx_chain.effects)
        return;
    canvas->frame_postfx_chain.enabled = 1;
    canvas->frame_postfx_chain.effect_count = 1;
    canvas->frame_postfx_chain.effect_capacity = 1;
    canvas->frame_postfx_chain.effects[0].type = VGFX3D_POSTFX_EFFECT_MOTION_BLUR;
    canvas->frame_postfx_chain.effects[0].snapshot.enabled = 1;
    canvas->frame_postfx_chain.effects[0].snapshot.motion_blur_enabled = 1;
}

static void *make_test_mesh(void) {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    for (int64_t i = 0; i < 3; i++)
        rt_mesh3d_set_bone_weights(mesh, i, 0, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    return mesh;
}

static void *make_degenerate_basis_mesh(void) {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, -0.5, -0.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.5, -0.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    return mesh;
}

static void *make_depth_test_mesh(float z0, float z1, float z2) {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 0.0, 0.0, z0, 0.0, 0.0, 1.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, z1, 0.0, 0.0, 1.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 1.0, z2, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    return mesh;
}

static bool matrices_nearly_equal(const float *a, const float *b, float epsilon) {
    if (!a || !b)
        return false;
    for (int i = 0; i < 16; i++) {
        if (std::fabs(a[i] - b[i]) > epsilon)
            return false;
    }
    return true;
}

static void *make_test_player(void) {
    void *skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    return rt_anim_player3d_new(skel);
}

static void *make_test_player_with_bones(int bone_count) {
    void *skel = rt_skeleton3d_new();
    for (int i = 0; i < bone_count; i++)
        rt_skeleton3d_add_bone(skel, rt_const_cstr("bone"), i - 1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);
    return rt_anim_player3d_new(skel);
}

static void test_gpu_skinning_bypass_for_opengl(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *player = make_test_player();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "OpenGL skinned draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0, "OpenGL skinned draw avoids CPU temp buffer");
    EXPECT_TRUE(draws[0].cmd.vertices == mesh_view->vertices,
                "OpenGL skinned draw keeps original mesh vertices for GPU skinning");
    EXPECT_TRUE(draws[0].cmd.bone_palette != nullptr, "OpenGL skinned draw forwards bone palette");
    EXPECT_TRUE(draws[0].cmd.bone_count == 1, "OpenGL skinned draw forwards bone count");

    cleanup_fake_canvas(&canvas);
}

static void test_draw_repairs_corrupt_mesh_geometry_counts(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    mesh_view->vertex_capacity = mesh_view->vertex_count;
    mesh_view->index_capacity = mesh_view->index_count;
    mesh_view->vertex_count = UINT32_MAX;
    mesh_view->index_count = UINT32_MAX;

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Canvas3D draws the valid prefix of a count-corrupt mesh");
    EXPECT_TRUE(mesh_view->vertex_count == 3 && mesh_view->index_count == 3,
                "Canvas3D repairs corrupt mesh counts before queuing");
    EXPECT_TRUE(draws[0].cmd.vertex_count == 3 && draws[0].cmd.index_count == 3,
                "Canvas3D queues repaired mesh counts");

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_skinning_bypass_for_d3d11(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kD3D11Backend);

    void *mesh = make_test_mesh();
    void *player = make_test_player();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "D3D11 skinned draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0, "D3D11 skinned draw avoids CPU temp buffer");
    EXPECT_TRUE(draws[0].cmd.vertices == mesh_view->vertices,
                "D3D11 skinned draw keeps original mesh vertices for GPU skinning");
    EXPECT_TRUE(draws[0].cmd.bone_palette != nullptr, "D3D11 skinned draw forwards bone palette");
    EXPECT_TRUE(draws[0].cmd.bone_count == 1, "D3D11 skinned draw forwards bone count");

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_skinning_bypass_for_metal(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kMetalBackend);

    void *mesh = make_test_mesh();
    void *player = make_test_player();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Metal skinned draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0, "Metal skinned draw avoids CPU temp buffer");
    EXPECT_TRUE(draws[0].cmd.vertices == mesh_view->vertices,
                "Metal skinned draw keeps original mesh vertices for GPU skinning");
    EXPECT_TRUE(draws[0].cmd.bone_palette != nullptr, "Metal skinned draw forwards bone palette");
    EXPECT_TRUE(draws[0].cmd.bone_count == 1, "Metal skinned draw forwards bone count");

    cleanup_fake_canvas(&canvas);
}

static void test_cpu_skinning_fallback_for_software(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kSoftwareBackend);

    void *mesh = make_test_mesh();
    void *player = make_test_player();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Software skinned draw enqueues one draw");
    /* CPU-skinned vertices live in the per-frame bump arena now (stable until
     * frame flush) instead of a tracked malloc — no temp-buffer entry. */
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "Software skinned draw takes no tracked temp buffer (frame arena)");
    EXPECT_TRUE(canvas.frame_arena_frame_bytes > 0,
                "Software skinned draw consumes frame-arena bytes");
    EXPECT_TRUE(draws[0].cmd.vertices != mesh_view->vertices,
                "Software skinned draw uses CPU-skinned vertex buffer");
    EXPECT_TRUE(draws[0].cmd.bone_palette == nullptr && draws[0].cmd.bone_count == 0,
                "Software skinned draw clears GPU palette payloads after CPU fallback");

    cleanup_fake_canvas(&canvas);
}

static void test_large_gpu_skinning_bypass_for_opengl(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *player = make_test_player_with_bones(200);
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_mesh3d_set_bone_weights(mesh, 0, 199, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);

    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "OpenGL large-rig skinned draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0, "OpenGL large-rig skinned draw stays on the GPU path");
    EXPECT_TRUE(draws[0].cmd.vertices == mesh_view->vertices,
                "OpenGL large-rig skinned draw avoids CPU skinning");
    EXPECT_TRUE(draws[0].cmd.bone_count == 200,
                "OpenGL large-rig skinned draw forwards the expanded bone count");

    cleanup_fake_canvas(&canvas);
}

static void test_large_gpu_skinning_bypass_for_d3d11(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kD3D11Backend);

    void *mesh = make_test_mesh();
    void *player = make_test_player_with_bones(200);
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_mesh3d_set_bone_weights(mesh, 0, 199, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);

    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "D3D11 large-rig skinned draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0, "D3D11 large-rig skinned draw stays on the GPU path");
    EXPECT_TRUE(draws[0].cmd.vertices == mesh_view->vertices,
                "D3D11 large-rig skinned draw avoids CPU skinning");
    EXPECT_TRUE(draws[0].cmd.bone_count == 200,
                "D3D11 large-rig skinned draw forwards the expanded bone count");

    cleanup_fake_canvas(&canvas);
}

static void test_large_gpu_skinning_bypass_for_metal(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kMetalBackend);

    void *mesh = make_test_mesh();
    void *player = make_test_player_with_bones(200);
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_mesh3d_set_bone_weights(mesh, 0, 199, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);

    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Metal large-rig skinned draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0, "Metal large-rig skinned draw stays on the GPU path");
    EXPECT_TRUE(draws[0].cmd.vertices == mesh_view->vertices,
                "Metal large-rig skinned draw avoids CPU skinning");
    EXPECT_TRUE(draws[0].cmd.bone_count == 200,
                "Metal large-rig skinned draw forwards the expanded bone count");

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_morph_payload_for_opengl(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "OpenGL morphed draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "OpenGL morphed draw avoids transient packed payload buffers");
    EXPECT_TRUE(canvas.temp_obj_count == 3,
                "OpenGL morphed draw retains mesh, material, and morph state until frame end");
    EXPECT_TRUE(rt_heap_hdr(morph)->refcnt == 2,
                "OpenGL morphed draw retains the morph object until frame end");
    EXPECT_TRUE(draws[0].cmd.vertices == mesh_view->vertices,
                "OpenGL morphed draw keeps original mesh vertices for GPU morphing");
    EXPECT_TRUE(draws[0].cmd.morph_deltas != nullptr,
                "OpenGL morphed draw forwards packed morph deltas");
    EXPECT_TRUE(draws[0].cmd.morph_normal_deltas == nullptr,
                "OpenGL morphed draw leaves normal-delta payload null when absent");
    EXPECT_TRUE(draws[0].cmd.morph_weights != nullptr,
                "OpenGL morphed draw forwards packed morph weights");
    EXPECT_TRUE(draws[0].cmd.morph_shape_count == 1, "OpenGL morphed draw forwards shape count");
    EXPECT_TRUE(draws[0].cmd.morph_key == morph,
                "OpenGL morphed draw forwards the stable morph identity");
    EXPECT_TRUE(draws[0].cmd.morph_revision == rt_morphtarget3d_get_payload_generation(morph),
                "OpenGL morphed draw forwards the morph payload revision");
    if (draws[0].cmd.morph_deltas && draws[0].cmd.morph_weights) {
        EXPECT_TRUE(draws[0].cmd.morph_deltas[0] == 1.0f && draws[0].cmd.morph_deltas[1] == 2.0f &&
                        draws[0].cmd.morph_deltas[2] == 3.0f,
                    "OpenGL morphed draw packs vertex deltas in XYZ order");
        EXPECT_TRUE(draws[0].cmd.morph_weights[0] == 0.5f,
                    "OpenGL morphed draw forwards morph weights");
    }

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_morph_payload_for_d3d11(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kD3D11Backend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "D3D11 morphed draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "D3D11 morphed draw avoids transient packed payload buffers");
    EXPECT_TRUE(canvas.temp_obj_count == 3,
                "D3D11 morphed draw retains mesh, material, and morph state until frame end");
    EXPECT_TRUE(rt_heap_hdr(morph)->refcnt == 2,
                "D3D11 morphed draw retains the morph object until frame end");
    EXPECT_TRUE(draws[0].cmd.vertices == mesh_view->vertices,
                "D3D11 morphed draw keeps original mesh vertices for GPU morphing");
    EXPECT_TRUE(draws[0].cmd.morph_deltas != nullptr,
                "D3D11 morphed draw forwards packed morph deltas");
    EXPECT_TRUE(draws[0].cmd.morph_weights != nullptr,
                "D3D11 morphed draw forwards packed morph weights");
    EXPECT_TRUE(draws[0].cmd.morph_shape_count == 1, "D3D11 morphed draw forwards shape count");
    EXPECT_TRUE(draws[0].cmd.morph_key == morph,
                "D3D11 morphed draw forwards the stable morph identity");
    EXPECT_TRUE(draws[0].cmd.morph_revision == rt_morphtarget3d_get_payload_generation(morph),
                "D3D11 morphed draw forwards the morph payload revision");

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_morph_payload_for_metal(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kMetalBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Metal morphed draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "Metal morphed draw avoids transient packed payload buffers");
    EXPECT_TRUE(canvas.temp_obj_count == 3,
                "Metal morphed draw retains mesh, material, and morph state until frame end");
    EXPECT_TRUE(rt_heap_hdr(morph)->refcnt == 2,
                "Metal morphed draw retains the morph object until frame end");
    EXPECT_TRUE(draws[0].cmd.vertices == mesh_view->vertices,
                "Metal morphed draw keeps original mesh vertices for GPU morphing");
    EXPECT_TRUE(draws[0].cmd.morph_deltas != nullptr,
                "Metal morphed draw forwards packed morph deltas");
    EXPECT_TRUE(draws[0].cmd.morph_weights != nullptr,
                "Metal morphed draw forwards packed morph weights");
    EXPECT_TRUE(draws[0].cmd.morph_shape_count == 1, "Metal morphed draw forwards shape count");
    EXPECT_TRUE(draws[0].cmd.morph_key == morph,
                "Metal morphed draw forwards the stable morph identity");
    EXPECT_TRUE(draws[0].cmd.morph_revision == rt_morphtarget3d_get_payload_generation(morph),
                "Metal morphed draw forwards the morph payload revision");

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_morph_normal_payload_for_d3d11(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kD3D11Backend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_normal_delta(morph, 0, 0, 0.25, 0.5, 0.75);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "D3D11 morphed-normal draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "D3D11 morphed-normal draw avoids transient packed payload buffers");
    EXPECT_TRUE(
        canvas.temp_obj_count == 3,
        "D3D11 morphed-normal draw retains mesh, material, and morph state until frame end");
    EXPECT_TRUE(rt_heap_hdr(morph)->refcnt == 2,
                "D3D11 morphed-normal draw retains the morph object until frame end");
    EXPECT_TRUE(draws[0].cmd.morph_normal_deltas != nullptr,
                "D3D11 morphed-normal draw forwards packed morph normal deltas");
    if (draws[0].cmd.morph_normal_deltas) {
        EXPECT_TRUE(draws[0].cmd.morph_normal_deltas[0] == 0.25f &&
                        draws[0].cmd.morph_normal_deltas[1] == 0.5f &&
                        draws[0].cmd.morph_normal_deltas[2] == 0.75f,
                    "D3D11 morphed-normal draw packs normal deltas in XYZ order");
    }

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_morph_normal_payload_for_opengl(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_normal_delta(morph, 0, 0, 0.25, 0.5, 0.75);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "OpenGL morphed-normal draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "OpenGL morphed-normal draw avoids transient packed payload buffers");
    EXPECT_TRUE(
        canvas.temp_obj_count == 3,
        "OpenGL morphed-normal draw retains mesh, material, and morph state until frame end");
    EXPECT_TRUE(rt_heap_hdr(morph)->refcnt == 2,
                "OpenGL morphed-normal draw retains the morph object until frame end");
    EXPECT_TRUE(draws[0].cmd.morph_normal_deltas != nullptr,
                "OpenGL morphed-normal draw forwards packed morph normal deltas");
    if (draws[0].cmd.morph_normal_deltas) {
        EXPECT_TRUE(draws[0].cmd.morph_normal_deltas[0] == 0.25f &&
                        draws[0].cmd.morph_normal_deltas[1] == 0.5f &&
                        draws[0].cmd.morph_normal_deltas[2] == 0.75f,
                    "OpenGL morphed-normal draw packs normal deltas in XYZ order");
    }

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_morph_normal_payload_for_metal(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kMetalBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_normal_delta(morph, 0, 0, 0.25, 0.5, 0.75);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Metal morphed-normal draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "Metal morphed-normal draw avoids transient packed payload buffers");
    EXPECT_TRUE(
        canvas.temp_obj_count == 3,
        "Metal morphed-normal draw retains mesh, material, and morph state until frame end");
    EXPECT_TRUE(rt_heap_hdr(morph)->refcnt == 2,
                "Metal morphed-normal draw retains the morph object until frame end");
    EXPECT_TRUE(draws[0].cmd.morph_normal_deltas != nullptr,
                "Metal morphed-normal draw forwards packed morph normal deltas");
    if (draws[0].cmd.morph_normal_deltas) {
        EXPECT_TRUE(draws[0].cmd.morph_normal_deltas[0] == 0.25f &&
                        draws[0].cmd.morph_normal_deltas[1] == 0.5f &&
                        draws[0].cmd.morph_normal_deltas[2] == 0.75f,
                    "Metal morphed-normal draw packs normal deltas in XYZ order");
    }

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_morph_rejects_nonfinite_position_payload(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);

    float *packed = const_cast<float *>(rt_morphtarget3d_get_packed_deltas(morph));
    EXPECT_TRUE(packed != nullptr, "Test morph builds a packed position payload");
    if (packed)
        packed[0] = std::numeric_limits<float>::quiet_NaN();

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Corrupt morph payload still submits the base mesh draw");
    EXPECT_TRUE(draws[0].cmd.morph_deltas == nullptr && draws[0].cmd.morph_weights == nullptr &&
                    draws[0].cmd.morph_shape_count == 0,
                "Non-finite position morph payload disables GPU morph bindings");

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_morph_drops_nonfinite_normal_payload(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_normal_delta(morph, 0, 0, 0.25, 0.5, 0.75);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);

    const float *packed_pos = rt_morphtarget3d_get_packed_deltas(morph);
    float *packed_normal = const_cast<float *>(rt_morphtarget3d_get_packed_normal_deltas(morph));
    EXPECT_TRUE(packed_pos != nullptr && packed_normal != nullptr,
                "Test morph builds packed position and normal payloads");
    if (packed_normal)
        packed_normal[0] = std::numeric_limits<float>::quiet_NaN();

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Corrupt normal payload still submits the morphed draw");
    EXPECT_TRUE(draws[0].cmd.morph_deltas != nullptr && draws[0].cmd.morph_weights != nullptr &&
                    draws[0].cmd.morph_shape_count == 1,
                "Valid position morph payload remains active");
    EXPECT_TRUE(draws[0].cmd.morph_normal_deltas == nullptr,
                "Non-finite optional normal morph payload is not forwarded to the backend");

    cleanup_fake_canvas(&canvas);
}

static void test_attached_morph_targets_route_through_draw_mesh(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);
    rt_mesh3d_set_morph_targets(mesh, morph);

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Attached morph targets still enqueue one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "Attached morph targets avoid transient GPU morph payload buffers");
    EXPECT_TRUE(canvas.temp_obj_count == 3,
                "Attached morph targets retain mesh, material, and morph state until frame end");
    EXPECT_TRUE(rt_heap_hdr(morph)->refcnt == 3,
                "Attached morph targets retain the morph object until frame end");
    EXPECT_TRUE(draws[0].cmd.morph_deltas != nullptr,
                "Attached morph targets route DrawMesh through the morph payload path");
    EXPECT_TRUE(draws[0].cmd.morph_shape_count == 1,
                "Attached morph targets preserve the shape count on DrawMesh");

    cleanup_fake_canvas(&canvas);
}

static void test_brdf_lut_matches_split_sum_reference(void) {
    /* The precomputed split-sum table must behave like the physical integral:
     * energy-bounded, near-total reflectance for smooth head-on viewing, and
     * within loose range of the Karis analytic fit it replaced. */
    float ab[2];
    vgfx3d_brdf_lut_sample(1.0f, 0.02f, ab);
    EXPECT_TRUE(ab[0] > 0.9f && ab[0] <= 1.01f,
                "BRDF LUT scale approaches 1 for smooth head-on viewing");
    EXPECT_TRUE(ab[1] >= 0.0f && ab[1] < 0.1f,
                "BRDF LUT bias stays small for smooth head-on viewing");
    for (int yi = 0; yi < 8; yi++) {
        for (int xi = 0; xi < 8; xi++) {
            float ndv = (float)(xi + 1) / 8.0f;
            float rough = (float)(yi + 1) / 8.0f;
            vgfx3d_brdf_lut_sample(ndv, rough, ab);
            EXPECT_TRUE(ab[0] >= 0.0f && ab[1] >= 0.0f && ab[0] + ab[1] <= 1.05f,
                        "BRDF LUT stays energy-bounded across the domain");
        }
    }
    /* Mid-domain agreement with the Karis fit (the fit is accurate to a few
     * percent there, so a wide tolerance still catches axis swaps / garbage). */
    vgfx3d_brdf_lut_sample(0.5f, 0.5f, ab);
    {
        float r = 0.5f, ndv = 0.5f;
        float bx = r * -1.0f + 1.0f;
        float by = r * -0.0275f + 0.0425f;
        float bz = r * -0.572f + 1.04f;
        float bw = r * 0.022f - 0.04f;
        float a004 = bx * bx;
        float e9 = exp2f(-9.28f * ndv);
        if (e9 < a004)
            a004 = e9;
        a004 = a004 * bx + by;
        float ref_a = -1.04f * a004 + bz;
        float ref_b = 1.04f * a004 + bw;
        EXPECT_TRUE(fabsf(ab[0] - ref_a) < 0.08f,
                    "BRDF LUT scale tracks the analytic fit mid-domain");
        EXPECT_TRUE(fabsf(ab[1] - ref_b) < 0.08f,
                    "BRDF LUT bias tracks the analytic fit mid-domain");
    }
}

static void test_brdf_lut_concurrent_first_use_is_safe() {
    constexpr int worker_count = 16;
    const float *tables[worker_count] = {};
    float samples[worker_count][2] = {};
    std::thread workers[worker_count];
    for (int i = 0; i < worker_count; i++) {
        workers[i] = std::thread([&, i]() {
            tables[i] = vgfx3d_brdf_lut_data();
            vgfx3d_brdf_lut_sample(0.37f, 0.61f, samples[i]);
        });
    }
    for (auto &worker : workers)
        worker.join();
    bool same = tables[0] != nullptr;
    for (int i = 1; i < worker_count; i++) {
        same = same && tables[i] == tables[0] && samples[i][0] == samples[0][0] &&
               samples[i][1] == samples[0][1];
    }
    EXPECT_TRUE(same, "BRDF LUT concurrent first use publishes one complete immutable table");
}

static void test_large_morph_payload_falls_back_to_cpu_for_opengl(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    for (int i = 0; i < 65; i++)
        rt_morphtarget3d_add_shape(morph, rt_const_cstr("shape"));
    rt_morphtarget3d_set_delta(morph, 64, 0, 4.0, 0.0, 0.0);
    rt_morphtarget3d_set_weight(morph, 64, 1.0);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "OpenGL large morph draw still enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 1,
                "OpenGL falls back to CPU morphing when shape count exceeds shader capacity");
    EXPECT_TRUE(draws[0].cmd.vertices != mesh_view->vertices,
                "OpenGL oversize morph payload uses CPU-morphed vertices");
    EXPECT_TRUE(draws[0].cmd.morph_shape_count == 0 && draws[0].cmd.morph_deltas == nullptr,
                "OpenGL oversize morph payload leaves GPU morph bindings empty");
    EXPECT_TRUE(draws[0].cmd.vertices[0].pos[0] == 4.0f,
                "OpenGL CPU morph fallback still applies shapes beyond slot 63");

    cleanup_fake_canvas(&canvas);
}

static void test_large_morph_payload_falls_back_to_cpu_for_d3d11(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kD3D11Backend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    for (int i = 0; i < 65; i++)
        rt_morphtarget3d_add_shape(morph, rt_const_cstr("shape"));
    rt_morphtarget3d_set_delta(morph, 64, 0, 5.0, 0.0, 0.0);
    rt_morphtarget3d_set_weight(morph, 64, 1.0);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "D3D11 large morph draw still enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 1,
                "D3D11 falls back to CPU morphing when shape count exceeds shader capacity");
    EXPECT_TRUE(draws[0].cmd.vertices != mesh_view->vertices,
                "D3D11 oversize morph payload uses CPU-morphed vertices");
    EXPECT_TRUE(draws[0].cmd.morph_shape_count == 0 && draws[0].cmd.morph_deltas == nullptr,
                "D3D11 oversize morph payload leaves GPU morph bindings empty");
    EXPECT_TRUE(draws[0].cmd.vertices[0].pos[0] == 5.0f,
                "D3D11 CPU morph fallback still applies shapes beyond slot 63");

    cleanup_fake_canvas(&canvas);
}

static void test_large_morph_payload_stays_on_gpu_for_metal(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kMetalBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    for (int i = 0; i < 33; i++)
        rt_morphtarget3d_add_shape(morph, rt_const_cstr("shape"));
    rt_morphtarget3d_set_delta(morph, 32, 0, 6.0, 0.0, 0.0);
    rt_morphtarget3d_set_weight(morph, 32, 1.0);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Metal large morph draw still enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0, "Metal keeps large morph payloads on the GPU path");
    EXPECT_TRUE(draws[0].cmd.morph_shape_count == 33,
                "Metal forwards morph shape counts beyond the old 32-shape ceiling");
    EXPECT_TRUE(draws[0].cmd.morph_deltas != nullptr,
                "Metal forwards packed morph data for large shape counts");
    if (draws[0].cmd.morph_deltas) {
        size_t offset = (size_t)32 * 9u;
        EXPECT_TRUE(draws[0].cmd.morph_deltas[offset] == 6.0f,
                    "Metal packed morph payload retains shape 32 deltas");
    }

    cleanup_fake_canvas(&canvas);
}

static void test_cpu_morph_fallback_for_software(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kSoftwareBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Software morphed draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 1, "Software morphed draw allocates one CPU temp buffer");
    EXPECT_TRUE(draws[0].cmd.vertices != mesh_view->vertices,
                "Software morphed draw uses CPU-morphed vertices");
    EXPECT_TRUE(draws[0].cmd.morph_deltas == nullptr && draws[0].cmd.morph_weights == nullptr &&
                    draws[0].cmd.morph_shape_count == 0,
                "Software morphed draw leaves GPU morph payload empty");

    cleanup_fake_canvas(&canvas);
}

static void test_cpu_morph_tracking_failure_rolls_back_new_mesh_retain(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kSoftwareBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 0.0, 0.0);
    rt_morphtarget3d_set_weight(morph, 0, 1.0);

    canvas.temp_buf_capacity = -1;
    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    EXPECT_TRUE(canvas.draw_count == 0,
                "Rejected CPU morph resource setup does not enqueue a draw");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "Rejected CPU morph resource setup transfers no scratch-buffer ownership");
    EXPECT_TRUE(canvas.temp_obj_count == 0,
                "Rejected CPU morph resource setup rolls back its new mesh retain");

    canvas.temp_buf_capacity = 0;
    cleanup_fake_canvas(&canvas);
}

static void test_gpu_morph_tracking_failure_preserves_existing_morph_retain(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 0.0, 0.0);
    rt_morphtarget3d_set_weight(morph, 0, 1.0);

    EXPECT_TRUE(rt_canvas3d_add_temp_object(&canvas, morph) == 1,
                "GPU rollback fixture pre-tracks the shared morph payload");
    int32_t allocation_capacity = canvas.temp_obj_capacity;
    canvas.temp_obj_capacity = -1;
    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    EXPECT_TRUE(canvas.draw_count == 0,
                "Rejected GPU morph resource setup does not enqueue a draw");
    EXPECT_TRUE(canvas.temp_obj_count == 1 && canvas.temp_objects[0] == morph,
                "GPU rollback preserves a morph retain owned by an earlier draw");

    canvas.temp_obj_capacity = allocation_capacity;
    cleanup_fake_canvas(&canvas);
}

static void test_morph_tangent_deltas_fall_back_to_cpu_for_metal(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kMetalBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("twist"));
    rt_morphtarget3d_set_tangent_delta(morph, 0, 0, 1.0, 0.0, 0.0);
    rt_morphtarget3d_set_weight(morph, 0, 1.0);

    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Metal tangent-morph draw enqueues one draw");
    EXPECT_TRUE(canvas.temp_buf_count == 1,
                "Metal tangent morph falls back to CPU until GPU tangent payloads exist");
    EXPECT_TRUE(draws[0].cmd.vertices != mesh_view->vertices,
                "Metal tangent morph uses CPU-morphed vertices");
    EXPECT_TRUE(draws[0].cmd.morph_shape_count == 0 && draws[0].cmd.morph_deltas == nullptr,
                "Metal tangent morph leaves GPU morph bindings empty");
    EXPECT_TRUE(std::fabs(draws[0].cmd.vertices[0].tangent[0] - 1.0f) < 0.001f,
                "CPU tangent morph applies and normalizes tangent deltas");

    cleanup_fake_canvas(&canvas);
}

static void test_env_map_payload_forwarded(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *px = rt_pixels_new(1, 1);
    rt_pixels_set(px, 0, 0, 0xFF0000FF);
    void *cubemap = rt_cubemap3d_new(px, px, px, px, px, px);
    rt_material3d_set_env_map(material, cubemap);
    rt_material3d_set_reflectivity(material, 0.75);

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Env-map draw enqueues one draw");
    EXPECT_TRUE(draws[0].cmd.env_map == cubemap, "Env-map draw forwards cubemap payload");
    EXPECT_TRUE(draws[0].cmd.reflectivity == 0.75f, "Env-map draw forwards reflectivity payload");

    cleanup_fake_canvas(&canvas);
}

static void test_backend_skybox_hook_used(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.draw_skybox = record_draw_skybox;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    canvas.backend_ctx = &canvas;
    skybox_draw_calls = 0;

    void *px = rt_pixels_new(1, 1);
    rt_pixels_set(px, 0, 0, 0x00FF00FF);
    canvas.skybox = (rt_cubemap3d *)rt_cubemap3d_new(px, px, px, px, px, px);

    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(skybox_draw_calls == 1,
                "Canvas3D.End delegates skybox rendering to the backend hook when available");
    EXPECT_TRUE(canvas.in_frame == 0, "Canvas3D.End completes cleanly for skybox-only scenes");

    cleanup_fake_canvas(&canvas);
}

static void test_incomplete_cubemaps_are_not_forwarded(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.draw_skybox = record_draw_skybox;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    canvas.backend_ctx = &canvas;

    void *px = rt_pixels_new(1, 1);
    rt_pixels_set(px, 0, 0, 0xFF0000FF);
    void *cubemap = rt_cubemap3d_new(px, px, px, px, px, px);
    ((rt_cubemap3d *)cubemap)->face_size = 2;
    EXPECT_TRUE(rt_cubemap3d_is_complete(cubemap) == 0,
                "CubeMap3D completeness rejects mismatched face sizes");

    skybox_draw_calls = 0;
    rt_canvas3d_set_skybox(&canvas, cubemap);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(skybox_draw_calls == 0,
                "Canvas3D.SetSkybox rejects incomplete cubemaps before backend submission");

    init_fake_canvas(&canvas, &backend);
    canvas.backend_ctx = &canvas;
    void *bound_cubemap = rt_cubemap3d_new(px, px, px, px, px, px);
    rt_canvas3d_set_skybox(&canvas, bound_cubemap);
    EXPECT_TRUE(rt_heap_hdr(bound_cubemap)->refcnt == 2,
                "Canvas3D.SetSkybox retains a complete cubemap");
    ((rt_cubemap3d *)bound_cubemap)->face_size = 2;
    skybox_draw_calls = 0;
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(skybox_draw_calls == 0, "Canvas3D.End rejects skyboxes corrupted after binding");
    EXPECT_TRUE(canvas.skybox == nullptr,
                "Canvas3D.End clears corrupted skyboxes before later frames reuse them");
    EXPECT_TRUE(rt_heap_hdr(bound_cubemap)->refcnt == 1,
                "Canvas3D.End releases its retain on a corrupted bound skybox");

    init_fake_canvas(&canvas, &kOpenGLBackend);
    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    EXPECT_TRUE(expect_trap_contains([&]() { rt_material3d_set_env_map(material, cubemap); },
                                     "complete CubeMap3D"),
                "Material3D.SetEnvMap rejects incomplete cubemap payloads");
    rt_material3d_set_reflectivity(material, 0.75);
    rt_canvas3d_draw_mesh(&canvas, mesh, rt_mat4_identity(), material);
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Incomplete env-map draw still enqueues the mesh");
    EXPECT_TRUE(draws[0].cmd.env_map == nullptr,
                "Material3D incomplete env maps are not submitted");
    EXPECT_TRUE(draws[0].cmd.reflectivity == 0.0f,
                "Material3D incomplete env maps clear reflectivity in draw payloads");

    reset_canvas_frame(&canvas, 2);
    ((rt_material3d *)material)->env_map = cubemap;
    ((rt_material3d *)material)->reflectivity = 0.5;
    rt_canvas3d_draw_mesh(&canvas, mesh, rt_mat4_identity(), material);
    draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[0].cmd.env_map == nullptr,
                "Canvas3D draw command repair drops directly corrupted env maps");
    EXPECT_TRUE(draws[0].cmd.reflectivity == 0.0f,
                "Canvas3D draw command repair drops corrupted env-map reflectivity");

    cleanup_fake_canvas(&canvas);
}

static void test_material_repairs_wrong_class_private_refs_without_release(void) {
    void *material = rt_material3d_new();
    void *wrong = rt_material3d_new();
    rt_material3d *mat = (rt_material3d *)material;
    size_t wrong_refcnt = rt_heap_hdr(wrong)->refcnt;

    mat->texture = wrong;
    EXPECT_TRUE(rt_material3d_get_has_texture(material) == 0,
                "Material3D.GetHasTexture rejects wrong-class private texture refs");
    EXPECT_TRUE(mat->texture == nullptr,
                "Material3D.GetHasTexture clears wrong-class private texture refs");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "Material3D wrong-class texture repair does not release unowned refs");

    mat->env_map = wrong;
    wrong_refcnt = rt_heap_hdr(wrong)->refcnt;
    EXPECT_TRUE(rt_material3d_get_has_env_map(material) == 0,
                "Material3D.GetHasEnvMap rejects wrong-class private env-map refs");
    EXPECT_TRUE(mat->env_map == nullptr,
                "Material3D.GetHasEnvMap clears wrong-class private env-map refs");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "Material3D wrong-class env-map repair does not release unowned refs");

    void *px = rt_pixels_new(1, 1);
    rt_pixels_set(px, 0, 0, 0xFFFFFFFF);
    void *cubemap = rt_cubemap3d_new(px, px, px, px, px, px);
    rt_material3d_set_env_map(material, cubemap);
    EXPECT_TRUE(rt_heap_hdr(cubemap)->refcnt == 2,
                "Material3D.SetEnvMap retains a complete cubemap");
    ((rt_cubemap3d *)cubemap)->face_size = 2;
    EXPECT_TRUE(rt_material3d_get_has_env_map(material) == 0,
                "Material3D.GetHasEnvMap rejects cubemaps corrupted after assignment");
    EXPECT_TRUE(mat->env_map == nullptr,
                "Material3D.GetHasEnvMap clears cubemaps corrupted after assignment");
    EXPECT_TRUE(rt_heap_hdr(cubemap)->refcnt == 1,
                "Material3D.GetHasEnvMap releases retained cubemaps corrupted after assignment");
}

static void test_cubemap_finalizer_skips_wrong_class_private_faces(void) {
    void *px = rt_pixels_new(1, 1);
    void *cubemap = rt_cubemap3d_new(px, px, px, px, px, px);
    void *wrong = rt_material3d_new();
    EXPECT_TRUE(px != nullptr && cubemap != nullptr && wrong != nullptr,
                "CubeMap3D private-face corruption fixture is created");
    if (!px || !cubemap || !wrong)
        return;

    rt_obj_retain_maybe(wrong);
    size_t wrong_refcnt = rt_heap_hdr(wrong)->refcnt;
    ((rt_cubemap3d *)cubemap)->faces[2] = wrong;
    EXPECT_TRUE(rt_cubemap3d_is_complete(cubemap) == 0,
                "CubeMap3D completeness rejects wrong-class private face refs");
    if (rt_obj_release_check0(cubemap))
        rt_obj_free(cubemap);
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "CubeMap3D finalizer does not release unowned wrong-class face refs");
    if (rt_obj_release_check0(wrong))
        rt_obj_free(wrong);
}

static void test_terrain_draw_sanitizes_private_splat_scales(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *terrain = rt_terrain3d_new(2, 2);
    void *material = rt_material3d_new();
    void *splat = rt_pixels_new(1, 1);
    void *layer = rt_pixels_new(1, 1);
    rt_pixels_set(splat, 0, 0, 0xFFFFFFFF);
    rt_pixels_set(layer, 0, 0, 0xFFFFFFFF);
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_splat_map(terrain, splat);
    for (int i = 0; i < 4; i++)
        rt_terrain3d_set_layer_texture(terrain, i, layer);

    test_terrain3d_view *view = (test_terrain3d_view *)terrain;
    view->layer_scales[0] = std::numeric_limits<double>::quiet_NaN();
    view->layer_scales[1] = std::numeric_limits<double>::infinity();
    view->layer_scales[2] = -3.0;
    view->layer_scales[3] = 1.0e30;

    rt_canvas3d_draw_terrain_at(&canvas, terrain, 0.0, 0.0, 0.0);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Terrain splat fixture enqueues one terrain chunk");
    EXPECT_TRUE(draws[0].cmd.has_splat == 1, "Terrain draw forwards splat payload");
    EXPECT_TRUE(draws[0].cmd.splat_map == splat, "Terrain draw forwards splat map payload");
    EXPECT_TRUE(fabsf(draws[0].cmd.splat_layer_scales[0] - 1.0f) < 0.001f,
                "Terrain draw repairs NaN private splat scale");
    EXPECT_TRUE(fabsf(draws[0].cmd.splat_layer_scales[1] - 1.0f) < 0.001f,
                "Terrain draw repairs infinite private splat scale");
    EXPECT_TRUE(fabsf(draws[0].cmd.splat_layer_scales[2] - 1.0f) < 0.001f,
                "Terrain draw repairs negative private splat scale");
    EXPECT_TRUE(fabsf(draws[0].cmd.splat_layer_scales[3] - 1000000.0f) < 1.0f,
                "Terrain draw clamps huge private splat scale");

    cleanup_fake_canvas(&canvas);
}

static void test_terrain_draw_rejects_private_wrong_class_material(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *terrain = rt_terrain3d_new(2, 2);
    void *splat = rt_pixels_new(1, 1);
    void *not_material = rt_pixels_new(1, 1);
    rt_pixels_set(splat, 0, 0, 0xFFFFFFFF);
    rt_pixels_set(not_material, 0, 0, 0xFFFFFFFF);
    rt_terrain3d_set_splat_map(terrain, splat);

    test_terrain3d_view *view = (test_terrain3d_view *)terrain;
    view->material = not_material;
    view->splat_dirty = 1;

    rt_canvas3d_draw_terrain_at(&canvas, terrain, 0.0, 0.0, 0.0);

    EXPECT_TRUE(canvas.draw_count == 0,
                "Terrain draw rejects private wrong-class material pointers before splat bake");

    cleanup_fake_canvas(&canvas);
}

static void test_terrain_draw_rejects_private_wrong_class_splat_textures(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *terrain = rt_terrain3d_new(2, 2);
    void *material = rt_material3d_new();
    void *splat = rt_pixels_new(1, 1);
    void *not_pixels = rt_material3d_new();
    rt_pixels_set(splat, 0, 0, 0xFFFFFFFF);
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_splat_map(terrain, splat);

    test_terrain3d_view *view = (test_terrain3d_view *)terrain;
    ((rt_material3d *)material)->texture = not_pixels;
    size_t wrong_texture_refcnt = rt_heap_hdr(not_pixels)->refcnt;
    view->layer_textures[0] = not_pixels;
    rt_canvas3d_draw_terrain_at(&canvas, terrain, 0.0, 0.0, 0.0);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1,
                "Terrain wrong-class layer texture fixture enqueues one chunk");
    EXPECT_TRUE(view->base_texture == nullptr,
                "Terrain splat bake skips wrong-class material texture snapshots");
    EXPECT_TRUE(rt_heap_hdr(not_pixels)->refcnt == wrong_texture_refcnt,
                "Terrain splat bake does not retain unowned wrong-class material textures");
    EXPECT_TRUE(view->splat_map == splat,
                "Terrain keeps valid splat map when only a layer texture is corrupt");
    EXPECT_TRUE(draws[0].cmd.has_splat == 0 && draws[0].cmd.splat_map == nullptr,
                "Terrain disables GPU splatting when a private layer texture is corrupt");

    reset_canvas_frame(&canvas, 2);
    view->layer_textures[0] = nullptr;
    view->splat_map = not_pixels;
    view->splat_dirty = 1;
    rt_canvas3d_draw_terrain_at(&canvas, terrain, 0.0, 0.0, 0.0);
    draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1,
                "Terrain wrong-class splat map fixture still enqueues the chunk");
    EXPECT_TRUE(draws[0].cmd.has_splat == 0 && draws[0].cmd.splat_map == nullptr,
                "Terrain draw disables splatting for wrong-class private splat maps");

    cleanup_fake_canvas(&canvas);
}

static void test_terrain_set_material_restores_previous_splat_base_texture(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *terrain = rt_terrain3d_new(2, 2);
    void *material = rt_material3d_new();
    void *replacement = rt_material3d_new();
    void *base = rt_pixels_new(1, 1);
    void *splat = rt_pixels_new(1, 1);
    EXPECT_TRUE(terrain && material && replacement && base && splat,
                "Terrain material-restore fixture is created");
    if (!terrain || !material || !replacement || !base || !splat)
        return;
    rt_pixels_set(base, 0, 0, 0x224466FF);
    rt_pixels_set(splat, 0, 0, 0xFFFFFFFF);
    rt_material3d_set_texture(material, base);
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_splat_map(terrain, splat);

    rt_canvas3d_draw_terrain_at(&canvas, terrain, 0.0, 0.0, 0.0);

    test_terrain3d_view *view = (test_terrain3d_view *)terrain;
    rt_material3d *mat = (rt_material3d *)material;
    EXPECT_TRUE(canvas.draw_count == 1, "Terrain splat material-restore fixture draws once");
    EXPECT_TRUE(view->baked_texture != nullptr && mat->texture == view->baked_texture,
                "Terrain splat bake temporarily replaces the material texture");
    EXPECT_TRUE(mat->texture != base, "Terrain splat bake uses a distinct baked texture");

    rt_terrain3d_set_material(terrain, replacement);

    EXPECT_TRUE(mat->texture == base,
                "Terrain SetMaterial restores the previous material's base texture");
    EXPECT_TRUE(view->base_texture == nullptr && view->baked_texture == nullptr,
                "Terrain SetMaterial clears terrain-owned bake texture refs");

    cleanup_fake_canvas(&canvas);
}

static void test_terrain_chunk_aabb_includes_skirt_depth(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *terrain = rt_terrain3d_new(2, 2);
    void *material = rt_material3d_new();
    rt_terrain3d_set_material(terrain, material);
    rt_terrain3d_set_skirt_depth(terrain, 3.0);

    test_terrain3d_view *view = (test_terrain3d_view *)terrain;
    rt_canvas3d_draw_terrain_at(&canvas, terrain, 0.0, 0.0, 0.0);

    EXPECT_TRUE(canvas.draw_count == 1, "Terrain skirt AABB fixture enqueues one chunk");
    EXPECT_TRUE(view->chunk_aabbs != nullptr && view->chunk_aabbs[1] <= -3.0f,
                "Terrain cached chunk AABB includes downward skirt depth");

    cleanup_fake_canvas(&canvas);
}

static void test_metal_robustness_probe_accepts_degenerate_basis_and_skybox_forward(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = record_begin_frame;
    backend.submit_draw = record_draw_with_lights;
    backend.draw_skybox = record_draw_skybox;
    backend.end_frame = noop_end_frame;

    rt_canvas3d canvas;
    rt_camera3d camera = {};
    init_fake_canvas(&canvas, &backend);
    canvas.backend_ctx = &canvas;
    canvas.in_frame = 0;
    canvas.width = 64;
    canvas.height = 64;
    camera.fov = 60.0;
    camera.aspect = 1.0;
    camera.near_plane = 0.1;
    camera.far_plane = 20.0;
    camera.is_ortho = 1;
    camera.ortho_size = 2.0;

    void *face = rt_pixels_new(1, 1);
    rt_pixels_set(face, 0, 0, 0x202080FF);
    canvas.skybox = (rt_cubemap3d *)rt_cubemap3d_new(face, face, face, face, face, face);

    void *normal_map = rt_pixels_new(1, 1);
    rt_pixels_set(normal_map, 0, 0, 0x8080FFFF);
    void *mesh = make_degenerate_basis_mesh();
    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    void *material = rt_material3d_new();
    rt_material3d_set_normal_map(material, normal_map);
    void *transform = rt_mat4_identity();

    reset_postfx_records();
    reset_shadow_counts();
    skybox_draw_calls = 0;

    rt_canvas3d_begin(&canvas, &camera);

    EXPECT_TRUE(begin_frame_calls == 1, "Metal robustness probe begins one frame");
    EXPECT_TRUE(begin_frame_params[0].is_ortho == 1,
                "Metal robustness probe forwards the orthographic skybox flag");
    EXPECT_TRUE(fabsf(begin_frame_params[0].forward[0]) < 0.0001f &&
                    fabsf(begin_frame_params[0].forward[1]) < 0.0001f &&
                    fabsf(begin_frame_params[0].forward[2] + 1.0f) < 0.0001f,
                "Canvas3D sanitizes a zero-length camera forward vector to -Z");

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1,
                "Metal robustness probe queues a degenerate-basis normal-mapped draw");
    EXPECT_TRUE(draws[0].cmd.normal_map == normal_map,
                "Metal robustness probe forwards the normal-map payload");
    EXPECT_TRUE(draws[0].cmd.vertices != mesh_view->vertices,
                "Normal-mapped degenerate tangents are repaired on a queued snapshot");
    if (draws[0].cmd.vertices) {
        const vgfx3d_vertex_t *v = &draws[0].cmd.vertices[0];
        float tangent_len2 = v->tangent[0] * v->tangent[0] + v->tangent[1] * v->tangent[1] +
                             v->tangent[2] * v->tangent[2];
        EXPECT_TRUE(v->normal[0] == 0.0f && v->normal[1] == 0.0f && v->normal[2] == 0.0f,
                    "Degenerate authored normals remain a shader-guard responsibility");
        EXPECT_TRUE(std::isfinite(tangent_len2) && tangent_len2 > 0.5f && tangent_len2 < 1.5f &&
                        std::isfinite(v->tangent[3]),
                    "Queued tangent fallback is finite and non-zero");
    }
    EXPECT_TRUE(mesh_view->vertices[0].tangent[0] == 0.0f &&
                    mesh_view->vertices[0].tangent[1] == 0.0f &&
                    mesh_view->vertices[0].tangent[2] == 0.0f,
                "Degenerate-basis probe does not mutate caller-owned mesh geometry");

    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(skybox_draw_calls == 1,
                "Metal robustness probe delegates skybox rendering to the backend hook");
    EXPECT_TRUE(draw_submit_calls == 1,
                "Metal robustness probe submits the degenerate-basis mesh draw");
    EXPECT_TRUE(canvas.last_draw_count == 1,
                "Metal robustness probe records one visible main draw");
    EXPECT_TRUE(canvas.in_frame == 0, "Metal robustness probe ends cleanly");

    cleanup_fake_canvas(&canvas);
}

static void test_deferred_draw_retains_mesh_and_material_until_end(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    EXPECT_TRUE(rt_heap_hdr(mesh)->refcnt == 1, "Fresh mesh starts with a single owned reference");
    EXPECT_TRUE(rt_heap_hdr(material)->refcnt == 1,
                "Fresh material starts with a single owned reference");

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    EXPECT_TRUE(canvas.temp_obj_count == 2,
                "Deferred mesh draw tracks both mesh and material until frame end");
    EXPECT_TRUE(rt_heap_hdr(mesh)->refcnt == 2,
                "Deferred mesh draw retains the mesh until the queue flushes");
    EXPECT_TRUE(rt_heap_hdr(material)->refcnt == 2,
                "Deferred mesh draw retains the material until the queue flushes");

    if (rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    if (rt_obj_release_check0(material))
        rt_obj_free(material);

    cleanup_fake_canvas(&canvas);
}

static void test_mesh_draw_traps_when_deferred_queue_cannot_grow(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.submit_draw = record_draw_with_lights;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    reset_shadow_counts();

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    EXPECT_TRUE(canvas.draw_count == 1, "Queue-failure fixture publishes its first command");
    const test_deferred_draw_t first_before = ((test_deferred_draw_t *)canvas.draw_cmds)[0];

    rt_canvas3d_reset_submission_diagnostics(&canvas);
    rt_canvas3d_test_fail_next_queue_reserve(&canvas);
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_canvas3d_draw_mesh(&canvas, mesh, transform, material); },
                             "deferred draw queue allocation failed"),
        "Mesh draw traps when the deferred queue cannot grow");
    EXPECT_TRUE(draw_submit_calls == 0,
                "Mesh draw does not bypass the sorted deferred queue after allocation failure");
    EXPECT_TRUE(canvas.draw_count == 1,
                "Failed deferred enqueue preserves the command already published in the frame");
    EXPECT_TRUE(std::memcmp(&first_before,
                            &((test_deferred_draw_t *)canvas.draw_cmds)[0],
                            sizeof(first_before)) == 0,
                "Failed deferred enqueue leaves the prior command byte-for-byte unchanged");
    EXPECT_TRUE(rt_canvas3d_get_last_submission_status(&canvas) ==
                    RT_CANVAS3D_SUBMISSION_QUEUE_FAILURE,
                "Deferred queue failure publishes the additive queue status");
    EXPECT_TRUE(rt_canvas3d_get_submission_failure_count(&canvas) == 1,
                "Deferred queue failure increments the resettable diagnostic count once");
    rt_canvas3d_reset_submission_diagnostics(&canvas);
    EXPECT_TRUE(rt_canvas3d_get_last_submission_status(&canvas) == RT_CANVAS3D_SUBMISSION_OK &&
                    rt_canvas3d_get_submission_failure_count(&canvas) == 0,
                "Submission diagnostic reset clears status and count without clearing commands");
    EXPECT_TRUE(canvas.draw_count == 1,
                "Submission diagnostic reset does not mutate the deferred queue");

    cleanup_fake_canvas(&canvas);
}

static void test_rect2d_queues_overlay_pass(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    rt_canvas3d_draw_rect2d(&canvas, 10, 20, 30, 40, 0xFFAA00FF);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Rect2D enqueues one overlay draw");
    EXPECT_TRUE(draws[0].pass_kind == 1,
                "Rect2D routes through the screen-overlay deferred pass during 3D frames");
    EXPECT_TRUE(draws[0].cmd.unlit == 1, "Rect2D overlay draw is submitted as unlit geometry");

    cleanup_fake_canvas(&canvas);
}

static void test_transform_history_forwarded_for_motion_blur(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[0].cmd.has_prev_model_matrix == 0,
                "First keyed draw has no previous model matrix");

    ((mat4_impl *)transform)->m[3] = 5.0;
    reset_canvas_frame(&canvas, 2);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[0].cmd.has_prev_model_matrix == 1,
                "Second keyed draw forwards previous model-matrix history");
    EXPECT_TRUE(draws[0].cmd.prev_model_matrix[3] == 0.0f,
                "Previous model matrix preserves the prior translation");

    cleanup_fake_canvas(&canvas);
}

static void test_morph_weight_history_forwarded(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 0.0, 0.0);

    reset_canvas_frame(&canvas, 1);
    rt_morphtarget3d_set_weight(morph, 0, 0.25);
    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[0].cmd.prev_morph_weights == nullptr,
                "First GPU morph draw has no previous-weight history");

    reset_canvas_frame(&canvas, 2);
    rt_morphtarget3d_set_weight(morph, 0, 0.75);
    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);
    draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[0].cmd.prev_morph_weights != nullptr,
                "Second GPU morph draw forwards previous morph weights");
    if (draws[0].cmd.prev_morph_weights)
        EXPECT_TRUE(draws[0].cmd.prev_morph_weights[0] == 0.25f,
                    "Previous morph weights preserve the prior frame value");

    cleanup_fake_canvas(&canvas);
}

static void test_rejected_morph_draw_does_not_advance_weight_history(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *morph = rt_morphtarget3d_new(3);
    rt_morphtarget3d_add_shape(morph, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(morph, 0, 0, 1.0, 0.0, 0.0);

    reset_canvas_frame(&canvas, 1);
    rt_morphtarget3d_set_weight(morph, 0, 0.25);
    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);
    EXPECT_TRUE(canvas.draw_count == 1, "History fixture accepts its first morph draw");
    canvas3d_clear_temp_objects(&canvas);

    reset_canvas_frame(&canvas, 2);
    rt_morphtarget3d_set_weight(morph, 0, 0.5);
    int32_t allocation_capacity = canvas.temp_obj_capacity;
    canvas.temp_obj_capacity = -1;
    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);
    EXPECT_TRUE(canvas.draw_count == 0, "History fixture rejects the resource-starved draw");
    canvas.temp_obj_capacity = allocation_capacity;

    reset_canvas_frame(&canvas, 3);
    rt_morphtarget3d_set_weight(morph, 0, 0.75);
    rt_canvas3d_draw_mesh_morphed(&canvas, mesh, transform, material, morph);
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "History fixture accepts the recovery draw");
    EXPECT_TRUE(draws[0].cmd.prev_morph_weights != nullptr,
                "Recovery draw retains the last accepted morph history");
    if (draws[0].cmd.prev_morph_weights)
        EXPECT_TRUE(draws[0].cmd.prev_morph_weights[0] == 0.25f,
                    "Rejected draw does not replace the previous accepted morph weight");

    cleanup_fake_canvas(&canvas);
}

static void test_skinning_palette_history_forwarded(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *player = make_test_player();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[0].cmd.prev_bone_palette == nullptr,
                "First GPU skinned draw has no previous palette history");

    reset_canvas_frame(&canvas, 2);
    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);
    draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[0].cmd.prev_bone_palette != nullptr,
                "Second GPU skinned draw forwards previous palette history");

    cleanup_fake_canvas(&canvas);
}

static void test_skinning_missing_previous_palette_disables_history(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *player = make_test_player();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);

    rt_anim_player3d *player_view = (rt_anim_player3d *)player;
    std::free(player_view->owned_prev_bone_palette);
    player_view->owned_prev_bone_palette = nullptr;
    player_view->prev_bone_palette = nullptr;
    player_view->has_prev_motion_palette = 1;

    reset_canvas_frame(&canvas, 2);
    rt_canvas3d_draw_mesh_skinned(&canvas, mesh, transform, material, player);
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;

    EXPECT_TRUE(canvas.draw_count == 1,
                "Missing previous skinning palette still allows the current skinned draw");
    EXPECT_TRUE(draws[0].cmd.prev_bone_palette == nullptr,
                "Missing previous skinning palette disables GPU skinning history");
    EXPECT_TRUE(player_view->has_prev_motion_palette == 0,
                "Missing previous skinning palette clears stale history state");

    cleanup_fake_canvas(&canvas);
}

static void test_instanced_transform_history_forwarded(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw_instanced = record_draw_instanced;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    enable_latched_motion_blur(&canvas);
    reset_recorded_instancing();

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *batch = rt_instbatch3d_new(mesh, material);
    void *t0 = rt_mat4_identity();
    void *t1 = rt_mat4_identity();
    ((mat4_impl *)t0)->m[3] = -0.75;
    ((mat4_impl *)t1)->m[3] = -0.25;
    rt_instbatch3d_add(batch, t0);
    rt_instbatch3d_add(batch, t1);

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_instanced(&canvas, batch);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(last_instance_count == 2, "Instanced draw submits both instances");
    EXPECT_TRUE(last_instanced_cmd.has_prev_instance_matrices == 1,
                "First instanced draw synthesizes previous transform history");
    EXPECT_TRUE(last_instanced_cmd.prev_instance_matrices != nullptr,
                "First instanced draw exposes a previous instance matrix payload");
    if (last_instanced_cmd.prev_instance_matrices) {
        EXPECT_TRUE(
            last_instanced_cmd.prev_instance_matrices[3] == -0.75f,
            "First instanced draw seeds the first previous transform from the current pose");
        EXPECT_TRUE(
            last_instanced_cmd.prev_instance_matrices[19] == -0.25f,
            "First instanced draw seeds the second previous transform from the current pose");
    }

    ((mat4_impl *)t0)->m[3] = 0.0;
    rt_instbatch3d_set(batch, 0, t0);
    reset_canvas_frame(&canvas, 2);
    rt_canvas3d_draw_instanced(&canvas, batch);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(last_instanced_cmd.has_prev_instance_matrices == 1,
                "Second instanced draw forwards previous instance transforms");
    EXPECT_TRUE(last_instanced_cmd.prev_instance_matrices != nullptr,
                "Instanced draw exposes previous instance matrix payload");
    if (last_instanced_cmd.prev_instance_matrices)
        EXPECT_TRUE(last_instanced_cmd.prev_instance_matrices[3] == -0.75f,
                    "Previous instance matrix preserves the prior translation");

    cleanup_fake_canvas(&canvas);
}

static void test_instanced_transform_history_survives_count_changes(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw_instanced = record_draw_instanced;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    enable_latched_motion_blur(&canvas);
    reset_recorded_instancing();

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *batch = rt_instbatch3d_new(mesh, material);
    void *t0 = rt_mat4_identity();
    void *t1 = rt_mat4_identity();
    ((mat4_impl *)t0)->m[3] = -0.75;
    ((mat4_impl *)t1)->m[3] = 0.5;
    rt_instbatch3d_add(batch, t0);

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_instanced(&canvas, batch);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(last_instance_count == 1, "Initial instanced draw submits the original instance");

    ((mat4_impl *)t0)->m[3] = 0.0;
    rt_instbatch3d_set(batch, 0, t0);
    rt_instbatch3d_add(batch, t1);

    reset_canvas_frame(&canvas, 2);
    rt_canvas3d_draw_instanced(&canvas, batch);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(last_instance_count == 2,
                "Instanced draw still submits all instances after the batch grows");
    EXPECT_TRUE(last_instanced_cmd.has_prev_instance_matrices == 1,
                "Instanced draw keeps previous-transform history when the count changes");
    EXPECT_TRUE(last_instanced_cmd.prev_instance_matrices != nullptr,
                "Instanced draw provides a padded previous-transform buffer on count changes");
    if (last_instanced_cmd.prev_instance_matrices) {
        EXPECT_TRUE(last_instanced_cmd.prev_instance_matrices[3] == -0.75f,
                    "Existing instances preserve their prior transform when the batch grows");
        EXPECT_TRUE(last_instanced_cmd.prev_instance_matrices[19] == 0.5f,
                    "New instances seed previous transforms from their current pose");
    }

    cleanup_fake_canvas(&canvas);
}

static void test_deferred_instanced_draw_snapshots_instance_buffers(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw_instanced = record_draw_instanced;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    enable_latched_motion_blur(&canvas);
    reset_recorded_instancing();

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *batch = rt_instbatch3d_new(mesh, material);
    void *t0 = rt_mat4_identity();
    ((mat4_impl *)t0)->m[3] = 0.0;
    rt_instbatch3d_add(batch, t0);

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_instanced(&canvas, batch);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(last_instance_matrices != nullptr && last_instance_matrices[3] == 0.0f,
                "First instanced frame submits the original matrix");

    ((mat4_impl *)t0)->m[3] = 0.1;
    rt_instbatch3d_set(batch, 0, t0);
    reset_canvas_frame(&canvas, 2);
    rt_canvas3d_draw_instanced(&canvas, batch);

    ((mat4_impl *)t0)->m[3] = 0.25;
    rt_instbatch3d_set(batch, 0, t0);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(last_instance_matrices != nullptr && last_instance_matrices[3] == 0.1f,
                "Deferred instanced draw snapshots current matrices at enqueue time");
    EXPECT_TRUE(last_instanced_cmd.prev_instance_matrices != nullptr &&
                    last_instanced_cmd.prev_instance_matrices[3] == 0.0f,
                "Deferred instanced draw snapshots previous matrices at enqueue time");

    cleanup_fake_canvas(&canvas);
}

static void test_instanced_transform_history_skips_payload_without_motion_blur(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw_instanced = record_draw_instanced;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    reset_recorded_instancing();

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *batch = rt_instbatch3d_new(mesh, material);
    void *t0 = rt_mat4_identity();
    rt_instbatch3d_add(batch, t0);

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_instanced(&canvas, batch);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(last_instance_count == 1,
                "Instanced draw still submits instances when motion blur is disabled");
    EXPECT_TRUE(last_instanced_cmd.has_prev_instance_matrices == 0,
                "Instanced draw skips previous-transform payloads without motion blur");
    EXPECT_TRUE(last_instanced_cmd.prev_instance_matrices == nullptr,
                "Instanced draw avoids allocating previous-transform buffers without motion blur");

    cleanup_fake_canvas(&canvas);
}

static void test_instanced_material_payload_forwarded(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw_instanced = record_draw_instanced;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    enable_latched_motion_blur(&canvas);
    reset_recorded_instancing();

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    rt_material3d *mat_view = (rt_material3d *)material;
    mat_view->diffuse[0] = 0.2;
    mat_view->diffuse[1] = 0.4;
    mat_view->diffuse[2] = 0.6;
    mat_view->diffuse[3] = 0.8;
    mat_view->specular[0] = 0.9;
    mat_view->specular[1] = 0.7;
    mat_view->specular[2] = 0.5;
    mat_view->shininess = 48.0;
    mat_view->alpha = 0.65;
    mat_view->emissive[0] = 0.1;
    mat_view->emissive[1] = 0.2;
    mat_view->emissive[2] = 0.3;

    void *px = rt_pixels_new(1, 1);
    rt_pixels_set(px, 0, 0, 0xFFAA00FF);
    void *cubemap = rt_cubemap3d_new(px, px, px, px, px, px);
    mat_view->texture = px;
    mat_view->normal_map = px;
    mat_view->specular_map = px;
    mat_view->emissive_map = px;
    mat_view->env_map = cubemap;
    mat_view->reflectivity = 0.55;

    void *batch = rt_instbatch3d_new(mesh, material);
    void *transform = rt_mat4_identity();
    rt_instbatch3d_add(batch, transform);

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_instanced(&canvas, batch);
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;

    EXPECT_TRUE(canvas.draw_count == 1,
                "Transparent instanced material draw enqueues one mesh draw");
    EXPECT_TRUE(last_instance_count == 0,
                "Transparent instanced material draw avoids backend instancing so it can sort");
    EXPECT_TRUE(draws[0].cmd.texture == px, "Instanced draw forwards diffuse texture");
    EXPECT_TRUE(draws[0].cmd.normal_map == px, "Instanced draw forwards normal map");
    EXPECT_TRUE(draws[0].cmd.specular_map == px, "Instanced draw forwards specular map");
    EXPECT_TRUE(draws[0].cmd.emissive_map == px, "Instanced draw forwards emissive map");
    EXPECT_TRUE(draws[0].cmd.env_map == cubemap, "Instanced draw forwards environment map");
    EXPECT_TRUE(draws[0].cmd.reflectivity == 0.55f, "Instanced draw forwards reflectivity");
    EXPECT_TRUE(draws[0].cmd.specular[0] == 0.9f && draws[0].cmd.specular[1] == 0.7f &&
                    draws[0].cmd.specular[2] == 0.5f,
                "Instanced draw forwards specular color");
    EXPECT_TRUE(draws[0].cmd.diffuse_color[3] == 0.8f,
                "Instanced draw preserves diffuse alpha separate from material alpha");
    EXPECT_TRUE(draws[0].cmd.alpha == 0.65f, "Instanced draw forwards material opacity");

    cleanup_fake_canvas(&canvas);
}

static void test_pbr_material_payload_forwarded(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);
    canvas.backface_cull = 1;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new_pbr(0.6, 0.4, 0.2);
    void *transform = rt_mat4_identity();
    void *px = rt_pixels_new(1, 1);
    rt_pixels_set(px, 0, 0, 0x80C040CC);

    rt_material3d_set_albedo_map(material, px);
    rt_material3d_set_normal_map(material, px);
    rt_material3d_set_metallic_roughness_map(material, px);
    rt_material3d_set_ao_map(material, px);
    rt_material3d_set_emissive_map(material, px);
    rt_material3d_set_metallic(material, 0.75);
    rt_material3d_set_roughness(material, 0.35);
    rt_material3d_set_ao(material, 0.85);
    rt_material3d_set_emissive_intensity(material, 1.8);
    rt_material3d_set_normal_scale(material, 0.55);
    rt_material3d_set_alpha(material, 0.6);
    rt_material3d_set_alpha_mode(material, RT_MATERIAL3D_ALPHA_MODE_BLEND);
    rt_material3d_set_double_sided(material, 1);
    ((rt_material3d *)material)
        ->texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] = 1;
    ((rt_material3d *)material)
        ->texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] =
        RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE;
    ((rt_material3d *)material)
        ->texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] =
        RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT;
    rt_material3d_set_import_texture_slot_sampler_axes(
        material,
        RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS,
        1,
        0.0,
        0.25,
        1.5,
        1.0,
        0.0,
        RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE,
        RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT,
        RT_MATERIAL3D_TEXTURE_FILTER_NEAREST,
        RT_MATERIAL3D_TEXTURE_FILTER_LINEAR,
        RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST);
    ((rt_material3d *)material)
        ->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS][0] = 1.5;
    ((rt_material3d *)material)
        ->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS][5] = 0.25;

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "PBR material draw enqueues one draw");
    EXPECT_TRUE(draws[0].cmd.workflow == RT_MATERIAL3D_WORKFLOW_PBR,
                "PBR material draw forwards workflow");
    EXPECT_TRUE(draws[0].cmd.texture == px, "PBR material draw forwards albedo map");
    EXPECT_TRUE(draws[0].cmd.normal_map == px, "PBR material draw forwards normal map");
    EXPECT_TRUE(draws[0].cmd.metallic_roughness_map == px,
                "PBR material draw forwards metallic-roughness map");
    EXPECT_TRUE(draws[0].cmd.ao_map == px, "PBR material draw forwards AO map");
    EXPECT_TRUE(draws[0].cmd.emissive_map == px, "PBR material draw forwards emissive map");
    EXPECT_TRUE(draws[0].cmd.alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND,
                "PBR material draw forwards alpha mode");
    EXPECT_TRUE(draws[0].cmd.double_sided == 1, "PBR material draw forwards double-sided state");
    EXPECT_TRUE(draws[0].backface_cull == 0,
                "Double-sided PBR materials disable deferred backface culling");
    EXPECT_TRUE(draws[0].cmd.metallic == 0.75f && draws[0].cmd.roughness == 0.35f &&
                    draws[0].cmd.ao == 0.85f,
                "PBR material draw forwards metallic, roughness, and AO scalars");
    EXPECT_TRUE(draws[0].cmd.emissive_intensity == 1.8f,
                "PBR material draw forwards emissive intensity");
    EXPECT_TRUE(draws[0].cmd.normal_scale == 0.55f, "PBR material draw forwards normal scale");
    EXPECT_TRUE(draws[0].cmd.alpha == 0.6f,
                "PBR material draw forwards material opacity separately from alpha mode");
    EXPECT_TRUE(draws[0].cmd.texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] ==
                    1,
                "PBR material draw forwards imported texture slot UV set");
    EXPECT_TRUE(
        draws[0].cmd.texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] ==
                RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE &&
            draws[0].cmd.texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] ==
                RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT &&
            draws[0].cmd.texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] ==
                RT_MATERIAL3D_TEXTURE_FILTER_NEAREST &&
            draws[0].cmd.texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] ==
                RT_MATERIAL3D_TEXTURE_FILTER_LINEAR &&
            draws[0].cmd.texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] ==
                RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST,
        "PBR material draw forwards imported texture slot sampler state");
    EXPECT_TRUE(
        std::fabs(
            draws[0]
                .cmd.texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS][0] -
            1.5f) < 0.001f &&
            std::fabs(
                draws[0]
                    .cmd
                    .texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS][5] -
                0.25f) < 0.001f,
        "PBR material draw forwards imported texture slot UV transform");

    cleanup_fake_canvas(&canvas);
}

static void test_material_draw_uses_neutral_fallbacks_for_nonfinite_private_scalars(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new_pbr(0.6, 0.4, 0.2);
    void *transform = rt_mat4_identity();
    auto *mat = (rt_material3d *)material;
    mat->diffuse[3] = std::numeric_limits<double>::quiet_NaN();
    mat->alpha = std::numeric_limits<double>::quiet_NaN();
    mat->roughness = std::numeric_limits<double>::quiet_NaN();
    mat->ao = std::numeric_limits<double>::quiet_NaN();
    mat->alpha_cutoff = std::numeric_limits<double>::quiet_NaN();

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Corrupt private material scalar draw enqueues one draw");
    EXPECT_TRUE(draws[0].cmd.diffuse_color[3] == 1.0f,
                "Non-finite diffuse alpha falls back to opaque");
    EXPECT_TRUE(draws[0].cmd.alpha == 1.0f, "Non-finite material alpha falls back to opaque");
    EXPECT_TRUE(draws[0].cmd.roughness == 0.5f,
                "Non-finite roughness falls back to the material default");
    EXPECT_TRUE(draws[0].cmd.ao == 1.0f, "Non-finite AO falls back to unoccluded");
    EXPECT_TRUE(draws[0].cmd.alpha_cutoff == 0.5f,
                "Non-finite alpha cutoff falls back to the material default");

    cleanup_fake_canvas(&canvas);
}

static void test_instanced_runtime_culls_outside_frustum(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw_instanced = record_draw_instanced;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    enable_latched_motion_blur(&canvas);
    reset_recorded_instancing();

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *batch = rt_instbatch3d_new(mesh, material);
    void *visible = rt_mat4_identity();
    void *hidden = rt_mat4_identity();
    ((mat4_impl *)hidden)->m[3] = 4.0;
    rt_instbatch3d_add(batch, visible);
    rt_instbatch3d_add(batch, hidden);

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_instanced(&canvas, batch);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(last_instance_count == 1,
                "Runtime instanced path drops off-frustum instances before backend submission");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "Instanced-only frames release culled-instance temp buffers at End()");
    EXPECT_TRUE(last_instance_matrices != nullptr && last_instance_matrices[3] == 0.0f,
                "Instanced culling preserves the visible instance transform payload");

    ((mat4_impl *)visible)->m[3] = 0.5;
    rt_instbatch3d_set(batch, 0, visible);
    reset_canvas_frame(&canvas, 2);
    rt_canvas3d_draw_instanced(&canvas, batch);
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(
        last_instanced_cmd.has_prev_instance_matrices == 1,
        "Instanced culling keeps previous-transform history enabled for surviving instances");
    EXPECT_TRUE(
        last_instanced_cmd.prev_instance_matrices != nullptr &&
            last_instanced_cmd.prev_instance_matrices[3] == 0.0f,
        "Instanced culling keeps previous-transform history aligned to surviving instances");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "Instanced culling cleanup remains correct on later frames");

    cleanup_fake_canvas(&canvas);
}

static void test_instanced_shadow_pass_includes_instances(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw_instanced = record_draw_instanced;
    backend.shadow_begin = record_shadow_begin;
    backend.shadow_draw = record_shadow_draw;
    backend.shadow_end = record_shadow_end;

    rt_canvas3d canvas;
    vgfx3d_rendertarget_t shadow_rt = {};
    float shadow_depth[16] = {};
    rt_light3d light = {};
    init_fake_canvas(&canvas, &backend);
    reset_recorded_instancing();
    reset_shadow_counts();

    shadow_rt.depth_buf = shadow_depth;
    shadow_rt.width = 4;
    shadow_rt.height = 4;
    canvas.shadow_rts[0] = &shadow_rt;
    canvas.shadows_enabled = 1;
    canvas.shadow_bias = 0.0025f;
    light.type = 0;
    light.direction[1] = -1.0;
    light.color[0] = 1.0;
    light.color[1] = 1.0;
    light.color[2] = 1.0;
    light.intensity = 1.0;
    light.enabled = 1;
    light.casts_shadows = 1;
    canvas.lights[0] = &light;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *batch = rt_instbatch3d_new(mesh, material);
    void *t0 = rt_mat4_identity();
    void *t1 = rt_mat4_identity();
    ((mat4_impl *)t0)->m[3] = -0.5;
    ((mat4_impl *)t1)->m[3] = 0.5;
    rt_instbatch3d_add(batch, t0);
    rt_instbatch3d_add(batch, t1);

    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_instanced(&canvas, batch);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(shadow_begin_calls == 1 && shadow_begin_slots[0] == 0,
                "Instanced draws participate in the first shadow-map slot");
    EXPECT_TRUE(shadow_draw_calls == 2,
                "Shadow rendering expands instanced draws so each visible instance casts a shadow");
    EXPECT_TRUE(shadow_end_calls == 1 && shadow_end_slots[0] == 0,
                "Instanced shadow rendering finalizes the first shadow pass once");

    cleanup_fake_canvas(&canvas);
}

static void test_transparent_sort_key_uses_mesh_bounds_depth(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);
    reset_canvas_frame(&canvas, 1);
    canvas.cached_cam_forward[2] = -1.0f;

    void *mesh = make_depth_test_mesh(-10.0f, -10.0f, -9.0f);
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_material3d *mat_view = (rt_material3d *)material;
    mat_view->alpha = 0.5;
    mat_view->alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Transparent bounds-sort test enqueues one draw");
    EXPECT_TRUE(std::fabs(draws[0].sort_key - 10.0f) < 0.001f,
                "Transparent sorting uses the mesh bounds depth instead of the model origin");

    cleanup_fake_canvas(&canvas);
}

static void test_frame_stats_count_submissions_and_cache_repeated_world_bounds(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    canvas.opaque_depth_sorting = 1;
    canvas.cached_cam_forward[2] = -1.0f;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    for (int i = 0; i < 1000; i++)
        rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    EXPECT_TRUE(canvas.draw_count == 1000, "Frame stats cache test enqueues repeated draws");
    EXPECT_TRUE(rt_canvas3d_get_aabb_transforms(&canvas) == 1,
                "Repeated mesh+matrix world bounds use one AABB transform per frame");

    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(rt_canvas3d_get_draws_submitted(&canvas) == 1000,
                "Frame stats count backend draw submissions");
    EXPECT_TRUE(rt_canvas3d_get_backend_state_changes(&canvas) == 1,
                "Identical repeated draws produce one backend state run");
    EXPECT_TRUE(rt_canvas3d_get_sort_passes(&canvas) > 0, "Frame stats count deferred sort passes");

    cleanup_fake_canvas(&canvas);
}

static void test_opaque_sort_groups_material_state_before_depth(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    canvas.opaque_depth_sorting = 1;
    canvas.cached_cam_forward[2] = -1.0f;

    void *mesh = make_test_mesh();
    void *tex_a = rt_pixels_new(1, 1);
    void *tex_b = rt_pixels_new(1, 1);
    rt_pixels_set(tex_a, 0, 0, 0xFFFFFFFF);
    rt_pixels_set(tex_b, 0, 0, 0x80FFFFFF);

    void *mat_a = rt_material3d_new();
    void *mat_b = rt_material3d_new();
    rt_material3d_set_texture(mat_a, tex_a);
    rt_material3d_set_texture(mat_b, tex_b);

    void *tx0 = rt_mat4_identity();
    void *tx1 = rt_mat4_identity();
    void *tx2 = rt_mat4_identity();
    void *tx3 = rt_mat4_identity();
    ((mat4_impl *)tx0)->m[11] = -1.0;
    ((mat4_impl *)tx1)->m[11] = -2.0;
    ((mat4_impl *)tx2)->m[11] = -3.0;
    ((mat4_impl *)tx3)->m[11] = -4.0;

    rt_canvas3d_draw_mesh(&canvas, mesh, tx0, mat_a);
    rt_canvas3d_draw_mesh(&canvas, mesh, tx1, mat_b);
    rt_canvas3d_draw_mesh(&canvas, mesh, tx2, mat_a);
    rt_canvas3d_draw_mesh(&canvas, mesh, tx3, mat_b);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(draw_submit_calls == 4, "Opaque state-sort test submits all draws");
    EXPECT_TRUE(rt_canvas3d_get_backend_state_changes(&canvas) == 2,
                "Opaque sorting groups matching material/texture state before depth");

    cleanup_fake_canvas(&canvas);
}

static void test_opaque_sort_keeps_depth_order_on_software_backend(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "software";
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    canvas.opaque_depth_sorting = 1;
    canvas.cached_cam_forward[2] = -1.0f;

    void *mesh = make_test_mesh();
    void *tex_a = rt_pixels_new(1, 1);
    void *tex_b = rt_pixels_new(1, 1);
    rt_pixels_set(tex_a, 0, 0, 0xFFFFFFFF);
    rt_pixels_set(tex_b, 0, 0, 0x80FFFFFF);

    void *mat_a = rt_material3d_new();
    void *mat_b = rt_material3d_new();
    rt_material3d_set_texture(mat_a, tex_a);
    rt_material3d_set_texture(mat_b, tex_b);

    void *tx0 = rt_mat4_identity();
    void *tx1 = rt_mat4_identity();
    void *tx2 = rt_mat4_identity();
    void *tx3 = rt_mat4_identity();
    ((mat4_impl *)tx0)->m[11] = -1.0;
    ((mat4_impl *)tx1)->m[11] = -2.0;
    ((mat4_impl *)tx2)->m[11] = -3.0;
    ((mat4_impl *)tx3)->m[11] = -4.0;

    rt_canvas3d_draw_mesh(&canvas, mesh, tx0, mat_a);
    rt_canvas3d_draw_mesh(&canvas, mesh, tx1, mat_b);
    rt_canvas3d_draw_mesh(&canvas, mesh, tx2, mat_a);
    rt_canvas3d_draw_mesh(&canvas, mesh, tx3, mat_b);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(draw_submit_calls == 4, "Software opaque sort test submits all draws");
    EXPECT_TRUE(submitted_order_count == 4 && submitted_textures[0] == tex_a &&
                    submitted_textures[1] == tex_b && submitted_textures[2] == tex_a &&
                    submitted_textures[3] == tex_b,
                "Software opaque sorting preserves front-to-back order instead of grouping state");
    EXPECT_TRUE(rt_canvas3d_get_backend_state_changes(&canvas) == 0,
                "Software backend does not report GPU state changes");

    cleanup_fake_canvas(&canvas);
}

static void test_transparent_sort_preserves_stable_sort_id_tie_break(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    canvas.cached_cam_forward[2] = -1.0f;

    void *mesh_a = make_depth_test_mesh(-2.0f, -2.0f, -2.0f);
    void *mesh_b = make_depth_test_mesh(-2.0f, -2.0f, -2.0f);
    void *mat_a = rt_material3d_new();
    void *mat_b = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_material3d_set_alpha(mat_a, 0.5);
    rt_material3d_set_alpha(mat_b, 0.5);
    rt_material3d_set_alpha_mode(mat_a, RT_MATERIAL3D_ALPHA_MODE_BLEND);
    rt_material3d_set_alpha_mode(mat_b, RT_MATERIAL3D_ALPHA_MODE_BLEND);

    rt_canvas3d_draw_mesh(&canvas, mesh_a, transform, mat_a);
    rt_canvas3d_draw_mesh(&canvas, mesh_b, transform, mat_b);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    const void *expected_first = (draws[0].stable_sort_id <= draws[1].stable_sort_id)
                                     ? draws[0].cmd.geometry_key
                                     : draws[1].cmd.geometry_key;

    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(draw_submit_calls == 2, "Transparent tie-sort test submits both draws");
    EXPECT_TRUE(submitted_order_count >= 2 && submitted_geometry_keys[0] == expected_first,
                "Transparent sorting preserves stable_sort_id before enqueue order");

    cleanup_fake_canvas(&canvas);
}

static void test_transparent_sort_refines_depth_within_bucket(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    canvas.cached_cam_forward[2] = -1.0f;

    void *mesh_far = make_depth_test_mesh(-1000.0f, -1000.0f, -1000.0f);
    void *mesh_close_a = make_depth_test_mesh(-1.0f, -1.0f, -1.0f);
    void *mesh_close_b = make_depth_test_mesh(-1.01f, -1.01f, -1.01f);
    void *mat_far = rt_material3d_new();
    void *mat_close_a = rt_material3d_new();
    void *mat_close_b = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_material3d_set_alpha(mat_far, 0.5);
    rt_material3d_set_alpha(mat_close_a, 0.5);
    rt_material3d_set_alpha(mat_close_b, 0.5);
    rt_material3d_set_alpha_mode(mat_far, RT_MATERIAL3D_ALPHA_MODE_BLEND);
    rt_material3d_set_alpha_mode(mat_close_a, RT_MATERIAL3D_ALPHA_MODE_BLEND);
    rt_material3d_set_alpha_mode(mat_close_b, RT_MATERIAL3D_ALPHA_MODE_BLEND);

    rt_canvas3d_draw_mesh(&canvas, mesh_far, transform, mat_far);
    rt_canvas3d_draw_mesh(&canvas, mesh_close_a, transform, mat_close_a);
    rt_canvas3d_draw_mesh(&canvas, mesh_close_b, transform, mat_close_b);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    const void *far_key = draws[0].cmd.geometry_key;
    const void *close_a_key = draws[1].cmd.geometry_key;
    const void *close_b_key = draws[2].cmd.geometry_key;

    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(draw_submit_calls == 3, "Transparent bucket-depth test submits all draws");
    EXPECT_TRUE(submitted_order_count >= 3 && submitted_geometry_keys[0] == far_key &&
                    submitted_geometry_keys[1] == close_b_key &&
                    submitted_geometry_keys[2] == close_a_key,
                "Transparent bucket sort refines close depths with the legacy comparator order");

    cleanup_fake_canvas(&canvas);
}

static void test_instanced_batch_sort_key_uses_aggregate_bounds_center(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw_instanced = record_draw_instanced;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    reset_canvas_frame(&canvas, 1);
    canvas.cached_cam_forward[2] = -1.0f;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *batch = rt_instbatch3d_new(mesh, material);
    void *near_tx = rt_mat4_identity();
    void *far_tx = rt_mat4_identity();
    ((mat4_impl *)near_tx)->m[11] = -0.25;
    ((mat4_impl *)far_tx)->m[11] = -0.75;
    rt_instbatch3d_add(batch, near_tx);
    rt_instbatch3d_add(batch, far_tx);

    rt_canvas3d_draw_instanced(&canvas, batch);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Opaque instanced batch enqueues one deferred draw");
    EXPECT_TRUE(std::fabs(draws[0].sort_key - 0.5f) < 0.01f,
                "Opaque instanced batch sorting uses the aggregate bounds center instead of the "
                "nearest instance");

    cleanup_fake_canvas(&canvas);
}

static void test_shadow_selection_prefers_strongest_directional_light_regardless_of_slot(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;
    backend.shadow_begin = record_shadow_begin;
    backend.shadow_draw = record_shadow_draw;
    backend.shadow_end = record_shadow_end;

    rt_canvas3d canvas;
    vgfx3d_rendertarget_t shadow_rt0 = {};
    vgfx3d_rendertarget_t shadow_rt1 = {};
    float shadow_depth0[16] = {};
    float shadow_depth1[16] = {};
    rt_light3d dim_light = {};
    rt_light3d mid_light = {};
    rt_light3d bright_light = {};
    float first_shadow_vp0[16];
    float first_shadow_vp1[16];
    init_fake_canvas(&canvas, &backend);

    shadow_rt0.depth_buf = shadow_depth0;
    shadow_rt0.width = 4;
    shadow_rt0.height = 4;
    shadow_rt1.depth_buf = shadow_depth1;
    shadow_rt1.width = 4;
    shadow_rt1.height = 4;
    canvas.shadow_rts[0] = &shadow_rt0;
    canvas.shadow_rts[1] = &shadow_rt1;
    canvas.shadows_enabled = 1;
    canvas.shadow_bias = 0.0025f;

    dim_light.type = 0;
    dim_light.direction[1] = -1.0;
    dim_light.color[0] = dim_light.color[1] = dim_light.color[2] = 1.0;
    dim_light.intensity = 0.25;
    dim_light.enabled = 1;
    dim_light.casts_shadows = 1;

    mid_light.type = 0;
    mid_light.direction[2] = -1.0;
    mid_light.color[0] = mid_light.color[1] = mid_light.color[2] = 1.0;
    mid_light.intensity = 1.5;
    mid_light.enabled = 1;
    mid_light.casts_shadows = 1;

    bright_light.type = 0;
    bright_light.direction[0] = 1.0 / 1.41421356237;
    bright_light.direction[1] = -1.0 / 1.41421356237;
    bright_light.color[0] = bright_light.color[1] = bright_light.color[2] = 1.0;
    bright_light.intensity = 3.0;
    bright_light.enabled = 1;
    bright_light.casts_shadows = 1;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    ((mat4_impl *)transform)->m[11] = -2.0;

    canvas.lights[0] = &dim_light;
    canvas.lights[1] = &bright_light;
    canvas.lights[2] = &mid_light;
    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_end(&canvas);
    std::memcpy(first_shadow_vp0, shadow_vps[0], sizeof(first_shadow_vp0));
    std::memcpy(first_shadow_vp1, shadow_vps[1], sizeof(first_shadow_vp1));

    canvas.lights[0] = &bright_light;
    canvas.lights[1] = &dim_light;
    canvas.lights[2] = &mid_light;
    reset_shadow_counts();
    reset_canvas_frame(&canvas, 2);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(shadow_begin_calls == 2 && shadow_draw_calls == 2 && shadow_end_calls == 2,
                "Shadow selection renders one pass per selected directional shadow light");
    EXPECT_TRUE(shadow_begin_slots[0] == 0 && shadow_begin_slots[1] == 1 &&
                    shadow_end_slots[0] == 0 && shadow_end_slots[1] == 1,
                "Shadow selection fills contiguous shadow slots");
    EXPECT_TRUE(matrices_nearly_equal(first_shadow_vp0, shadow_vps[0], 0.0001f) &&
                    matrices_nearly_equal(first_shadow_vp1, shadow_vps[1], 0.0001f),
                "Shadow-map selection follows directional light strength instead of slot order");
    EXPECT_TRUE(
        last_draw_light_count == 3 && last_draw_lights[0].shadow_index == 0 &&
            last_draw_lights[2].shadow_index == 1 && last_draw_lights[1].shadow_index == -1,
        "Main-pass light payload tags only the two strongest directional lights with shadow slots");

    cleanup_fake_canvas(&canvas);
}

static void test_shadow_cascades_render_primary_directional_light_slots(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;
    backend.shadow_begin = record_shadow_begin;
    backend.shadow_draw = record_shadow_draw;
    backend.shadow_end = record_shadow_end;

    rt_canvas3d canvas;
    vgfx3d_rendertarget_t shadow_rt[VGFX3D_MAX_SHADOW_LIGHTS] = {};
    float shadow_depth[VGFX3D_MAX_SHADOW_LIGHTS][16] = {};
    rt_light3d light = {};
    init_fake_canvas(&canvas, &backend);

    for (int32_t slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++) {
        shadow_rt[slot].depth_buf = shadow_depth[slot];
        shadow_rt[slot].width = 4;
        shadow_rt[slot].height = 4;
        canvas.shadow_rts[slot] = &shadow_rt[slot];
    }
    canvas.cached_cam_forward[2] = -1.0f;
    canvas.shadows_enabled = 1;
    canvas.shadow_bias = 0.0025f;
    canvas.shadow_cascade_count = 3;

    light.type = 0;
    light.direction[0] = 0.35;
    light.direction[1] = -1.0;
    light.direction[2] = -0.2;
    light.color[0] = light.color[1] = light.color[2] = 1.0;
    light.intensity = 2.0;
    light.enabled = 1;
    light.casts_shadows = 1;
    canvas.lights[0] = &light;

    void *mesh = make_depth_test_mesh(-1.0f, -6.0f, -12.0f);
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(shadow_begin_calls == 3 && shadow_draw_calls == 3 && shadow_end_calls == 3,
                "CSM renders one shadow pass per configured cascade for the primary light");
    EXPECT_TRUE(shadow_begin_slots[0] == 0 && shadow_begin_slots[1] == 1 &&
                    shadow_begin_slots[2] == 2 && shadow_end_slots[2] == 2,
                "CSM fills contiguous shadow cascade slots");
    EXPECT_TRUE(last_draw_light_count == 1 && last_draw_lights[0].shadow_index == 0 &&
                    last_draw_lights[0].shadow_cascade_count == 3,
                "Main-pass primary light receives the first cascade slot and cascade count");
    EXPECT_TRUE(last_draw_lights[0].shadow_cascade_splits[0] <
                        last_draw_lights[0].shadow_cascade_splits[1] &&
                    last_draw_lights[0].shadow_cascade_splits[1] <
                        last_draw_lights[0].shadow_cascade_splits[2],
                "CSM publishes monotonic camera-depth split distances to backends");

    cleanup_fake_canvas(&canvas);
}

static void test_spot_shadow_selection_fills_budget_after_directionals(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;
    backend.shadow_begin = record_shadow_begin;
    backend.shadow_draw = record_shadow_draw;
    backend.shadow_end = record_shadow_end;

    rt_canvas3d canvas;
    vgfx3d_rendertarget_t shadow_rt[VGFX3D_MAX_SHADOW_LIGHTS] = {};
    float shadow_depth[VGFX3D_MAX_SHADOW_LIGHTS][16] = {};
    rt_light3d directional = {};
    rt_light3d weak_spot = {};
    rt_light3d near_spot = {};
    rt_light3d mid_spot = {};
    rt_light3d far_spot = {};
    init_fake_canvas(&canvas, &backend);

    for (int32_t slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++) {
        shadow_rt[slot].depth_buf = shadow_depth[slot];
        shadow_rt[slot].width = 4;
        shadow_rt[slot].height = 4;
        canvas.shadow_rts[slot] = &shadow_rt[slot];
    }
    canvas.shadows_enabled = 1;
    canvas.shadow_bias = 0.0025f;
    canvas.shadow_cascade_count = 1;

    directional.type = 0;
    directional.direction[1] = -1.0;
    directional.color[0] = directional.color[1] = directional.color[2] = 1.0;
    directional.intensity = 0.1;
    directional.enabled = 1;
    directional.casts_shadows = 1;

    weak_spot.type = near_spot.type = mid_spot.type = far_spot.type = 3;
    weak_spot.direction[2] = near_spot.direction[2] = mid_spot.direction[2] =
        far_spot.direction[2] = -1.0;
    weak_spot.color[0] = weak_spot.color[1] = weak_spot.color[2] = 1.0;
    near_spot.color[0] = near_spot.color[1] = near_spot.color[2] = 1.0;
    mid_spot.color[0] = mid_spot.color[1] = mid_spot.color[2] = 1.0;
    far_spot.color[0] = far_spot.color[1] = far_spot.color[2] = 1.0;
    weak_spot.position[2] = 0.5;
    near_spot.position[2] = 1.0;
    mid_spot.position[2] = 3.0;
    far_spot.position[2] = 10.0;
    weak_spot.intensity = 0.1;
    near_spot.intensity = 1.0;
    mid_spot.intensity = 3.0;
    far_spot.intensity = 4.0;
    weak_spot.outer_cos = near_spot.outer_cos = mid_spot.outer_cos = far_spot.outer_cos = 0.5;
    weak_spot.inner_cos = near_spot.inner_cos = mid_spot.inner_cos = far_spot.inner_cos = 0.8;
    weak_spot.enabled = near_spot.enabled = mid_spot.enabled = far_spot.enabled = 1;
    weak_spot.casts_shadows = near_spot.casts_shadows = mid_spot.casts_shadows =
        far_spot.casts_shadows = 1;

    canvas.lights[0] = &directional;
    canvas.lights[1] = &weak_spot;
    canvas.lights[2] = &near_spot;
    canvas.lights[3] = &mid_spot;
    canvas.lights[4] = &far_spot;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    ((mat4_impl *)transform)->m[11] = -2.0;

    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_end(&canvas);

    /* This fake backend has no atlas-slot sampling, so the frame budget is the
     * classic per-texture cap (VGFX3D_CSM_SLOTS), not VGFX3D_MAX_SHADOW_LIGHTS. */
    EXPECT_TRUE(shadow_begin_calls == VGFX3D_CSM_SLOTS && shadow_draw_calls == 4 &&
                    shadow_end_calls == 4,
                "Shadow selection fills the fixed budget with directionals first then spots");
    EXPECT_TRUE(last_draw_light_count == 5 && last_draw_lights[0].shadow_index == 0 &&
                    last_draw_lights[3].shadow_index == 1 &&
                    last_draw_lights[2].shadow_index == 2 &&
                    last_draw_lights[4].shadow_index == 3 && last_draw_lights[1].shadow_index == -1,
                "Spot shadow selection ranks by intensity and distance after directional lights");
    EXPECT_TRUE(
        last_draw_lights[3].shadow_projection_type == VGFX3D_SHADOW_PROJECTION_PERSPECTIVE &&
            last_draw_lights[2].shadow_projection_type == VGFX3D_SHADOW_PROJECTION_PERSPECTIVE &&
            last_draw_lights[4].shadow_projection_type == VGFX3D_SHADOW_PROJECTION_PERSPECTIVE,
        "Selected spot lights advertise perspective shadow projection to backends");
    EXPECT_TRUE(std::fabs(shadow_vps[1][12]) + std::fabs(shadow_vps[1][13]) +
                        std::fabs(shadow_vps[1][14]) >
                    0.1f,
                "Spot shadow VP stores a perspective W row in the per-slot matrix");

    cleanup_fake_canvas(&canvas);
}

static void test_spot_shadow_selection_uses_single_slot_without_cascades(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;
    backend.shadow_begin = record_shadow_begin;
    backend.shadow_draw = record_shadow_draw;
    backend.shadow_end = record_shadow_end;

    rt_canvas3d canvas;
    vgfx3d_rendertarget_t shadow_rt = {};
    float shadow_depth[16] = {};
    rt_light3d spot = {};
    init_fake_canvas(&canvas, &backend);

    shadow_rt.depth_buf = shadow_depth;
    shadow_rt.width = 4;
    shadow_rt.height = 4;
    canvas.shadow_rts[0] = &shadow_rt;
    canvas.shadows_enabled = 1;
    canvas.shadow_bias = 0.0025f;
    canvas.shadow_cascade_count = 3;

    spot.type = 3;
    spot.direction[2] = -1.0;
    spot.position[2] = 2.0;
    spot.color[0] = spot.color[1] = spot.color[2] = 1.0;
    spot.intensity = 2.0;
    spot.outer_cos = 0.5;
    spot.inner_cos = 0.8;
    spot.enabled = 1;
    spot.casts_shadows = 1;
    canvas.lights[0] = &spot;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    ((mat4_impl *)transform)->m[11] = -2.0;

    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(shadow_begin_calls == 1 && shadow_draw_calls == 1 && shadow_end_calls == 1,
                "Spot shadows ignore directional cascade count and render one shadow slot");
    EXPECT_TRUE(last_draw_light_count == 1 && last_draw_lights[0].shadow_index == 0 &&
                    last_draw_lights[0].shadow_cascade_count == 1 &&
                    last_draw_lights[0].shadow_projection_type ==
                        VGFX3D_SHADOW_PROJECTION_PERSPECTIVE,
                "Spot shadow payload uses a single perspective shadow slot");

    cleanup_fake_canvas(&canvas);
}

static void test_shadow_selection_uses_queued_scene_light_snapshots(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;
    backend.shadow_begin = record_shadow_begin;
    backend.shadow_draw = record_shadow_draw;
    backend.shadow_end = record_shadow_end;

    rt_canvas3d canvas;
    vgfx3d_rendertarget_t shadow_rt = {};
    float shadow_depth[16] = {};
    rt_light3d imported_light = {};
    init_fake_canvas(&canvas, &backend);

    shadow_rt.depth_buf = shadow_depth;
    shadow_rt.width = 4;
    shadow_rt.height = 4;
    canvas.shadow_rts[0] = &shadow_rt;
    canvas.shadows_enabled = 1;
    canvas.shadow_bias = 0.0025f;

    imported_light.type = 0;
    imported_light.direction[1] = -1.0;
    imported_light.color[0] = imported_light.color[1] = imported_light.color[2] = 1.0;
    imported_light.intensity = 2.0;
    imported_light.enabled = 1;
    imported_light.casts_shadows = 1;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    ((mat4_impl *)transform)->m[11] = -2.0;

    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    canvas.scene_lights[0] = &imported_light;
    canvas.scene_light_count = 1;
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    canvas.scene_lights[0] = nullptr;
    canvas.scene_light_count = 0;
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(shadow_begin_calls == 1 && shadow_draw_calls == 1 && shadow_end_calls == 1,
                "Shadow selection uses queued SceneGraph node-light snapshots after Draw returns");
    EXPECT_TRUE(last_draw_light_count == 1 && last_draw_lights[0].shadow_index == 0,
                "Queued SceneGraph node lights receive shadow indices in the main pass");

    cleanup_fake_canvas(&canvas);
}

static void test_occlusion_mode_rejects_off_frustum_draws_before_submission(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "opengl";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    canvas.occlusion_culling = 1;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *inside_tx = rt_mat4_identity();
    void *outside_tx = rt_mat4_identity();
    ((mat4_impl *)outside_tx)->m[3] = 8.0;

    rt_canvas3d_draw_mesh(&canvas, mesh, inside_tx, material);
    rt_canvas3d_draw_mesh(&canvas, mesh, outside_tx, material);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(
        draw_submit_calls == 1,
        "Occlusion-culling mode performs coarse frustum rejection before main-pass submission");

    cleanup_fake_canvas(&canvas);
}

static void test_casts_shadows_false_skips_shadow_selection(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.end_frame = noop_end_frame;
    backend.submit_draw = record_draw_with_lights;
    backend.shadow_begin = record_shadow_begin;
    backend.shadow_draw = record_shadow_draw;
    backend.shadow_end = record_shadow_end;

    rt_canvas3d canvas;
    vgfx3d_rendertarget_t shadow_rt = {};
    float shadow_depth[16] = {};
    rt_light3d noncaster = {};
    rt_light3d caster = {};
    init_fake_canvas(&canvas, &backend);

    shadow_rt.depth_buf = shadow_depth;
    shadow_rt.width = 4;
    shadow_rt.height = 4;
    canvas.shadow_rts[0] = &shadow_rt;
    canvas.shadows_enabled = 1;
    canvas.shadow_bias = 0.0025f;

    noncaster.type = 0;
    noncaster.direction[1] = -1.0;
    noncaster.color[0] = noncaster.color[1] = noncaster.color[2] = 1.0;
    noncaster.intensity = 10.0;
    noncaster.enabled = 1;
    noncaster.casts_shadows = 0;

    caster.type = 0;
    caster.direction[2] = -1.0;
    caster.color[0] = caster.color[1] = caster.color[2] = 1.0;
    caster.intensity = 0.5;
    caster.enabled = 1;
    caster.casts_shadows = 1;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    ((mat4_impl *)transform)->m[11] = -2.0;

    canvas.lights[0] = &noncaster;
    canvas.lights[1] = &caster;
    reset_shadow_counts();
    reset_canvas_frame(&canvas, 1);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(shadow_begin_calls == 1 && shadow_draw_calls == 1 && shadow_end_calls == 1,
                "CastsShadows=false excludes a stronger directional light from shadow passes");
    EXPECT_TRUE(last_draw_light_count == 2 && last_draw_lights[0].shadow_index == -1 &&
                    last_draw_lights[1].shadow_index == 0,
                "Only lights with CastsShadows=true receive shadow indices");

    cleanup_fake_canvas(&canvas);
}

static void test_draw_mesh_preserves_forward_light_capacity(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);
    reset_canvas_frame(&canvas, 1);

    rt_light3d lights[VGFX3D_FORWARD_LIGHT_LIMIT];
    std::memset(lights, 0, sizeof(lights));
    for (int32_t i = 0; i < VGFX3D_FORWARD_LIGHT_LIMIT; i++) {
        lights[i].type = 1;
        lights[i].position[0] = (double)i;
        lights[i].color[0] = 0.1 + 0.01 * (double)i;
        lights[i].color[1] = 0.2 + 0.01 * (double)i;
        lights[i].color[2] = 0.3 + 0.01 * (double)i;
        lights[i].intensity = 1.0 + 0.25 * (double)i;
        lights[i].enabled = 1;
        canvas.lights[i] = &lights[i];
    }

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Full-light-capacity test enqueues one draw");
    EXPECT_TRUE(draws[0].light_count == VGFX3D_FORWARD_LIGHT_LIMIT,
                "Deferred draw preserves every configured forward light slot");
    EXPECT_TRUE(std::fabs(draws[0].lights[VGFX3D_FORWARD_LIGHT_LIMIT - 1].position[0] -
                          (float)(VGFX3D_FORWARD_LIGHT_LIMIT - 1)) < 0.001f,
                "Deferred draw includes the last forward light slot position");
    EXPECT_TRUE(std::fabs(draws[0].lights[VGFX3D_FORWARD_LIGHT_LIMIT - 1].intensity -
                          (float)(1.0 + 0.25 * (double)(VGFX3D_FORWARD_LIGHT_LIMIT - 1))) < 0.001f,
                "Deferred draw includes the last forward light slot intensity");

    cleanup_fake_canvas(&canvas);
}

static void test_disabled_lights_are_not_submitted(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);
    reset_canvas_frame(&canvas, 1);

    rt_light3d enabled = {};
    rt_light3d disabled = {};
    enabled.type = 0;
    enabled.direction[1] = -1.0;
    enabled.color[0] = enabled.color[1] = enabled.color[2] = 1.0;
    enabled.intensity = 1.0;
    enabled.enabled = 1;
    disabled = enabled;
    disabled.enabled = 0;
    disabled.intensity = 5.0;
    canvas.lights[0] = &disabled;
    canvas.lights[1] = &enabled;

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Disabled-light test enqueues one draw");
    EXPECT_TRUE(draws[0].light_count == 1, "Disabled lights are skipped before backend submission");
    EXPECT_TRUE(std::fabs(draws[0].lights[0].intensity - 1.0f) < 0.001f,
                "Enabled light remains in the submitted light payload");

    cleanup_fake_canvas(&canvas);
}

static void test_clear_lights_keeps_scene_explicitly_dark(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);
    reset_canvas_frame(&canvas, 1);

    rt_canvas3d_set_default_lighting(&canvas);
    EXPECT_TRUE(rt_canvas3d_get_light_count(&canvas) == 2,
                "SetDefaultLighting remains explicit opt-in setup");
    rt_canvas3d_clear_lights(&canvas);
    rt_canvas3d_set_ambient(&canvas, 0.0, 0.0, 0.0);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 1, "Explicit-dark lighting test enqueues one draw");
    EXPECT_TRUE(draws[0].light_count == 0,
                "ClearLights does not trigger implicit fallback lights during draw");
    EXPECT_TRUE(std::fabs(draws[0].ambient[0]) < 0.001f &&
                    std::fabs(draws[0].ambient[1]) < 0.001f &&
                    std::fabs(draws[0].ambient[2]) < 0.001f,
                "Zero ambient remains zero without fallback lighting");

    cleanup_fake_canvas(&canvas);
}

static void test_screenshot_prefers_backend_readback(void) {
    typedef struct {
        int64_t w;
        int64_t h;
        uint32_t *data;
    } pixels_view_t;

    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.readback_rgba = record_readback_rgba;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    canvas.width = 2;
    canvas.height = 2;
    last_readback_w = 0;
    last_readback_h = 0;
    last_readback_stride = 0;

    void *shot = rt_canvas3d_screenshot(&canvas);
    pixels_view_t *view = (pixels_view_t *)shot;
    EXPECT_TRUE(shot != nullptr, "Canvas3D.Screenshot produces a Pixels object");
    EXPECT_TRUE(last_readback_w == 2 && last_readback_h == 2,
                "Canvas3D.Screenshot requests backend readback at canvas dimensions");
    EXPECT_TRUE(last_readback_stride == 8, "Canvas3D.Screenshot uses tightly packed RGBA rows");
    if (view && view->data) {
        EXPECT_TRUE(view->data[0] == 0x12345678u,
                    "Canvas3D.Screenshot stores backend RGBA bytes in Pixels order");
    }
    if (shot && rt_obj_release_check0(shot))
        rt_obj_free(shot);
    cleanup_fake_canvas(&canvas);
}

static void test_screenshot_copy_reuses_destination_and_gpu_scratch(void) {
    typedef struct {
        int64_t w;
        int64_t h;
        uint32_t *data;
        uint64_t generation;
    } pixels_view_t;

    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.readback_rgba = record_readback_rgba;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    canvas.width = 2;
    canvas.height = 2;
    void *pixels = rt_pixels_new(2, 2);
    pixels_view_t *view = (pixels_view_t *)pixels;

    EXPECT_TRUE(rt_canvas3d_try_copy_screenshot_to(&canvas, pixels) == 1,
                "TryCopyScreenshotTo reads into an existing Pixels object");
    uint8_t *scratch = canvas.readback_rgba_scratch;
    size_t capacity = canvas.readback_rgba_scratch_capacity;
    uint64_t generation = view ? view->generation : 0;
    EXPECT_TRUE(scratch != nullptr && capacity >= 16,
                "TryCopyScreenshotTo keeps reusable GPU staging storage");
    EXPECT_TRUE(view && view->data && view->data[0] == 0x12345678u,
                "TryCopyScreenshotTo preserves Pixels RGBA packing");

    EXPECT_TRUE(rt_canvas3d_try_copy_screenshot_to(&canvas, pixels) == 1,
                "TryCopyScreenshotTo supports repeated capture");
    EXPECT_TRUE(canvas.readback_rgba_scratch == scratch &&
                    canvas.readback_rgba_scratch_capacity == capacity,
                "same-size repeated capture reuses GPU staging allocation");
    EXPECT_TRUE(view && view->generation == generation + 1,
                "successful repeated capture advances Pixels generation");

    void *wrong_size = rt_pixels_new(1, 1);
    EXPECT_TRUE(rt_canvas3d_try_copy_screenshot_to(&canvas, wrong_size) == 0,
                "TryCopyScreenshotTo reports a destination size mismatch");
    EXPECT_TRUE(rt_canvas3d_try_copy_screenshot_to(nullptr, pixels) == 0 &&
                    rt_canvas3d_try_copy_screenshot_to(&canvas, nullptr) == 0,
                "TryCopyScreenshotTo rejects null handles without dereferencing them");

    if (wrong_size && rt_obj_release_check0(wrong_size))
        rt_obj_free(wrong_size);
    if (pixels && rt_obj_release_check0(pixels))
        rt_obj_free(pixels);
    cleanup_fake_canvas(&canvas);
}

static void test_screenshot_reads_physical_framebuffer_to_logical_pixels(void) {
    typedef struct {
        int64_t w;
        int64_t h;
        uint32_t *data;
    } pixels_view_t;

    vgfx3d_backend_t backend = {};
    backend.name = "metal";
    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.readback_rgba = record_hidpi_readback_rgba;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    canvas.width = 2;
    canvas.height = 2;
    canvas.framebuffer_width = 4;
    canvas.framebuffer_height = 4;
    last_readback_w = 0;
    last_readback_h = 0;
    last_readback_stride = 0;

    void *shot = rt_canvas3d_screenshot(&canvas);
    pixels_view_t *view = (pixels_view_t *)shot;
    EXPECT_TRUE(shot != nullptr, "HiDPI Canvas3D.Screenshot produces a Pixels object");
    EXPECT_TRUE(last_readback_w == 4 && last_readback_h == 4,
                "HiDPI Canvas3D.Screenshot reads the physical framebuffer");
    EXPECT_TRUE(last_readback_stride == 16,
                "HiDPI Canvas3D.Screenshot uses the physical RGBA row stride");
    if (view && view->data) {
        EXPECT_TRUE(view->w == 2 && view->h == 2,
                    "HiDPI Canvas3D.Screenshot returns logical canvas dimensions");
        EXPECT_TRUE(view->data[0] == 0x102030FFu && view->data[1] == 0x405060FFu &&
                        view->data[2] == 0x708090FFu && view->data[3] == 0xA0B0C0FFu,
                    "HiDPI Canvas3D.Screenshot downsamples physical pixels into logical pixels");
    }

    if (shot && rt_obj_release_check0(shot))
        rt_obj_free(shot);
    cleanup_fake_canvas(&canvas);
}

static void test_final_overlay_replays_after_finalize(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "testgpu";
    backend.begin_frame = record_begin_frame;
    backend.submit_draw = record_final_draw;
    backend.end_frame = record_final_end_frame;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    canvas.in_frame = 0;
    canvas.width = 64;
    canvas.height = 48;
    reset_postfx_records();
    reset_final_frame_records();

    rt_canvas3d_begin_overlay(&canvas);
    EXPECT_TRUE(canvas.final_overlay_recording == 1,
                "Canvas3D.BeginOverlay enters final-overlay recording mode");
    EXPECT_TRUE(canvas.in_frame == 1, "Canvas3D.BeginOverlay opens a 2D recording frame");
    EXPECT_TRUE(begin_frame_calls == 0,
                "Canvas3D.BeginOverlay records commands without touching the backend");

    rt_canvas3d_draw_rect2d(&canvas, 4, 5, 12, 8, 0x62D2FF);
    EXPECT_TRUE(canvas.final_overlay_count == 1,
                "Final overlay screen draws are stored in the final overlay queue");
    EXPECT_TRUE(canvas.draw_count == 0,
                "Final overlay draws do not enter the normal scene draw queue");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "Final overlay geometry does not use the normal end-of-frame temp list");
    EXPECT_TRUE(canvas.final_overlay_temp_buf_count == 0,
                "Final overlay arena avoids the legacy per-draw temp buffer list");
    EXPECT_TRUE(canvas.final_overlay_arena_used > 0,
                "Final overlay arena retains geometry through normal End cleanup");

    rt_canvas3d_end_overlay(&canvas);
    EXPECT_TRUE(canvas.final_overlay_recording == 0,
                "Canvas3D.EndOverlay exits final-overlay recording mode");
    EXPECT_TRUE(canvas.in_frame == 0, "Canvas3D.EndOverlay closes the recording frame");

    rt_canvas3d_clear_overlay(&canvas);
    EXPECT_TRUE(canvas.final_overlay_count == 0,
                "Canvas3D.ClearOverlay discards recorded final overlay draws");
    EXPECT_TRUE(canvas.final_overlay_arena_used == 0,
                "Canvas3D.ClearOverlay resets recorded final overlay geometry");

    rt_canvas3d_begin_overlay(&canvas);
    rt_canvas3d_draw_rect2d(&canvas, 4, 5, 12, 8, 0x62D2FF);
    rt_canvas3d_end_overlay(&canvas);

    rt_canvas3d_finalize_frame(&canvas);
    EXPECT_TRUE(rt_canvas3d_get_frame_finalized(&canvas) == 1,
                "Canvas3D.FinalizeFrame marks the frame as finalized");
    EXPECT_TRUE(begin_frame_calls == 1,
                "Canvas3D.FinalizeFrame starts one backend pass for final overlay replay");
    EXPECT_TRUE(begin_frame_params[0].load_existing_color == 1,
                "Final overlay replay preserves post-FX scene color");
    EXPECT_TRUE(final_submit_draw_calls == 1,
                "Canvas3D.FinalizeFrame submits the recorded final overlay draw");
    EXPECT_TRUE(final_last_draw_cmd.disable_depth_test == 1,
                "Canvas3D.FinalizeFrame submits final overlays with depth disabled");
    EXPECT_TRUE(std::fabs(final_last_draw_cmd.diffuse_color[0] - 1.0f) < 0.001f &&
                    std::fabs(final_last_draw_cmd.diffuse_color[1] - 1.0f) < 0.001f &&
                    std::fabs(final_last_draw_cmd.diffuse_color[2] - 1.0f) < 0.001f,
                "Canvas3D.FinalizeFrame keeps screen overlay material diffuse white");
    if (final_last_draw_cmd.vertices) {
        const vgfx3d_vertex_t *verts =
            static_cast<const vgfx3d_vertex_t *>(final_last_draw_cmd.vertices);
        EXPECT_TRUE(std::fabs(verts[0].color[0] - (98.0f / 255.0f)) < 0.001f &&
                        std::fabs(verts[0].color[1] - (210.0f / 255.0f)) < 0.001f &&
                        std::fabs(verts[0].color[2] - 1.0f) < 0.001f,
                    "Canvas3D.FinalizeFrame carries screen overlay color in vertex color");
    }
    EXPECT_TRUE(final_end_frame_calls == 1,
                "Canvas3D.FinalizeFrame closes the final overlay backend pass");

    rt_canvas3d_finalize_frame(&canvas);
    EXPECT_TRUE(begin_frame_calls == 1 && final_submit_draw_calls == 1 &&
                    final_end_frame_calls == 1,
                "Canvas3D.FinalizeFrame is idempotent within one frame");

    cleanup_fake_canvas(&canvas);
}

static void test_gpu_postfx_final_overlay_presents_composited_frame(void) {
    vgfx3d_backend_t backend = {};
    backend.name = "testgpu";
    backend.begin_frame = record_begin_frame;
    backend.submit_draw = record_final_draw;
    backend.end_frame = record_final_end_frame;
    backend.present_postfx = record_present_postfx;
    backend.apply_postfx = record_apply_postfx;
    backend.present = record_present;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    canvas.in_frame = 0;
    canvas.width = 64;
    canvas.height = 48;
    canvas.frame_gpu_postfx_enabled = 1;
    canvas.frame_postfx_state_latched = 1;
    reset_postfx_records();
    reset_final_frame_records();

    rt_canvas3d_begin_overlay(&canvas);
    rt_canvas3d_draw_rect2d(&canvas, 4, 5, 12, 8, 0xFF00FFFF);
    rt_canvas3d_end_overlay(&canvas);
    rt_canvas3d_finalize_frame(&canvas);

    EXPECT_TRUE(final_submit_draw_calls == 1,
                "GPU postfx finalize replays final overlay during finalization");
    EXPECT_TRUE(final_apply_postfx_calls == 1,
                "GPU postfx finalization applies post-FX after the final overlay replay");
    // The final overlay must be replayed (submit_draw == 1) BEFORE post-FX is
    // applied, so the backend composites the overlay on top of the post-processed
    // scene. Applying post-FX first left the overlay in a separate target that
    // present never composited (the HUD flickered against the 3D scene).
    EXPECT_TRUE(final_apply_postfx_saw_submit_count == 1,
                "Backend post-FX apply runs after final overlay replay so the overlay "
                "composites into the presented frame");
    EXPECT_TRUE(final_present_postfx_calls == 0,
                "Split GPU postfx path avoids legacy present_postfx overwrite");
    EXPECT_TRUE(final_present_calls == 1,
                "GPU postfx final overlays present through the normal backend present hook");
    EXPECT_TRUE(final_present_saw_submit_count == 1,
                "Backend presentation runs after final overlay replay");
    EXPECT_TRUE(canvas.frame_presented_by_finalize == 1,
                "Final-overlay GPU postfx frames are presented by finalization");
    EXPECT_TRUE(rt_canvas3d_get_frame_finalized(&canvas) == 1,
                "GPU postfx finalize marks the frame finalized");
    cleanup_fake_canvas(&canvas);
}

static void test_screenshot_final_finalizes_before_readback(void) {
    typedef struct {
        int64_t w;
        int64_t h;
        uint32_t *data;
    } pixels_view_t;

    vgfx3d_backend_t backend = {};
    backend.name = "testgpu";
    backend.begin_frame = record_begin_frame;
    backend.submit_draw = record_final_draw;
    backend.end_frame = record_final_end_frame;
    backend.readback_rgba = record_final_readback_rgba;

    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &backend);
    canvas.backend_ctx = &canvas;
    canvas.in_frame = 0;
    canvas.width = 2;
    canvas.height = 2;
    reset_postfx_records();
    reset_final_frame_records();

    rt_canvas3d_begin_overlay(&canvas);
    rt_canvas3d_draw_rect2d(&canvas, 0, 0, 2, 2, 0xFFFFFFFF);
    rt_canvas3d_end_overlay(&canvas);

    void *shot = rt_canvas3d_screenshot_final(&canvas);
    pixels_view_t *view = (pixels_view_t *)shot;

    EXPECT_TRUE(shot != nullptr, "Canvas3D.ScreenshotFinal produces a Pixels object");
    EXPECT_TRUE(final_readback_calls == 1,
                "Canvas3D.ScreenshotFinal requests backend readback exactly once");
    EXPECT_TRUE(final_readback_saw_finalized == 1,
                "Canvas3D.ScreenshotFinal finalizes the frame before readback");
    EXPECT_TRUE(final_readback_saw_submit_count == 1,
                "Canvas3D.ScreenshotFinal replays final overlay before readback");
    EXPECT_TRUE(last_readback_w == 2 && last_readback_h == 2,
                "Canvas3D.ScreenshotFinal reads finalized pixels at canvas dimensions");
    if (view && view->data) {
        EXPECT_TRUE(view->data[0] == 0x12345678u,
                    "Canvas3D.ScreenshotFinal stores backend RGBA bytes in Pixels order");
    }

    void *reused = rt_pixels_new(2, 2);
    EXPECT_TRUE(rt_canvas3d_try_copy_screenshot_final_to(&canvas, reused) == 1,
                "TryCopyScreenshotFinalTo captures into an existing Pixels object");
    EXPECT_TRUE(final_readback_calls == 2,
                "TryCopyScreenshotFinalTo reads an already-finalized frame exactly once");

    if (shot && rt_obj_release_check0(shot))
        rt_obj_free(shot);
    if (reused && rt_obj_release_check0(reused))
        rt_obj_free(reused);
    cleanup_fake_canvas(&canvas);
}

static void test_gpu_postfx_state_latches_across_overlay_pass(void) {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    rt_camera3d camera = {};
    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *fx = rt_postfx3d_new();

    backend.name = "d3d11";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = record_begin_frame;
    backend.submit_draw = noop_draw;
    backend.end_frame = noop_end_frame;
    backend.present_postfx = noop_present_postfx;
    backend.set_gpu_postfx_enabled = record_set_gpu_postfx_enabled;
    backend.set_gpu_postfx_snapshot = record_set_gpu_postfx_snapshot;

    init_fake_canvas(&canvas, &backend);
    canvas.in_frame = 0;
    canvas.width = 320;
    canvas.height = 180;
    set_identity4x4d(camera.view);
    set_identity4x4d(camera.projection);
    camera.eye[2] = 3.0;
    rt_postfx3d_add_bloom(fx, 0.8, 1.5, 2);
    rt_canvas3d_set_post_fx(&canvas, fx);
    reset_postfx_records();

    rt_canvas3d_begin(&canvas, &camera);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_draw_rect2d(&canvas, 10, 20, 30, 40, 0xFFFFFFFF);
    rt_canvas3d_end(&canvas);

    EXPECT_TRUE(begin_frame_calls == 2,
                "Canvas3D issues separate backend begin_frame calls for scene and overlay passes");
    EXPECT_TRUE(begin_frame_params[0].load_existing_color == 0,
                "Main scene pass starts with a fresh color target");
    EXPECT_TRUE(begin_frame_params[1].load_existing_color == 1,
                "Overlay pass preserves the main scene color");
    EXPECT_TRUE(set_gpu_postfx_enabled_calls == 2,
                "Canvas3D reapplies the latched GPU postfx state for the overlay pass");
    EXPECT_TRUE(set_gpu_postfx_enabled_values[0] == 1 && set_gpu_postfx_enabled_values[1] == 1,
                "Canvas3D keeps GPU postfx enabled across both backend passes");
    EXPECT_TRUE(set_gpu_postfx_snapshot_calls == 2,
                "Canvas3D forwards the latched postfx snapshot to both backend passes");
    EXPECT_TRUE(set_gpu_postfx_snapshot_present[0] == 1 && set_gpu_postfx_snapshot_present[1] == 1,
                "Canvas3D keeps the postfx snapshot alive across the overlay pass");
    EXPECT_TRUE(set_gpu_postfx_chains[0].effect_count == 1 &&
                    set_gpu_postfx_chains[1].effect_count == 1,
                "Canvas3D forwards the same one-effect postfx chain to both backend passes");
    EXPECT_TRUE(set_gpu_postfx_chains[0].effects[0].snapshot.bloom_enabled == 1 &&
                    set_gpu_postfx_chains[1].effects[0].snapshot.bloom_threshold == 0.8f &&
                    set_gpu_postfx_chains[1].effects[0].snapshot.bloom_intensity == 1.5f &&
                    set_gpu_postfx_chains[1].effects[0].snapshot.bloom_passes == 2,
                "Canvas3D forwards the same latched postfx effect values to both backend passes");
    EXPECT_TRUE(canvas.frame_postfx_state_latched == 1,
                "Canvas3D keeps the frame postfx snapshot latched until Flip");
    EXPECT_TRUE(canvas.frame_gpu_postfx_enabled == 1,
                "Canvas3D records the frame as GPU-postfx-enabled");
    EXPECT_TRUE(canvas.frame_postfx_chain.effect_count == 1 &&
                    canvas.frame_postfx_chain.effects[0].snapshot.bloom_enabled == 1 &&
                    canvas.frame_postfx_chain.effects[0].snapshot.bloom_threshold == 0.8f &&
                    canvas.frame_postfx_chain.effects[0].snapshot.bloom_intensity == 1.5f &&
                    canvas.frame_postfx_chain.effects[0].snapshot.bloom_passes == 2,
                "Canvas3D preserves the original postfx chain across the overlay pass");

    cleanup_fake_canvas(&canvas);
}

static void test_begin_frame_forwards_camera_forward_and_ortho_flag(void) {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    rt_camera3d camera = {};

    backend.name = "metal";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.begin_frame = record_begin_frame;
    backend.end_frame = noop_end_frame;

    init_fake_canvas(&canvas, &backend);
    canvas.in_frame = 0;
    set_identity4x4d(camera.view);
    set_identity4x4d(camera.projection);
    camera.eye[0] = 1.5;
    camera.eye[1] = -2.0;
    camera.eye[2] = 7.0;
    camera.is_ortho = 1;
    reset_postfx_records();

    rt_canvas3d_begin(&canvas, &camera);

    EXPECT_TRUE(begin_frame_calls == 1, "Canvas3D.Begin forwards one backend begin_frame call");
    EXPECT_TRUE(begin_frame_params[0].is_ortho == 1,
                "Canvas3D.Begin forwards the orthographic camera flag");
    EXPECT_TRUE(fabsf(begin_frame_params[0].forward[0]) < 0.0001f &&
                    fabsf(begin_frame_params[0].forward[1]) < 0.0001f &&
                    fabsf(begin_frame_params[0].forward[2] + 1.0f) < 0.0001f,
                "Canvas3D.Begin forwards the camera forward vector extracted from the view matrix");
    EXPECT_TRUE(fabsf(begin_frame_params[0].position[0] - 1.5f) < 0.0001f &&
                    fabsf(begin_frame_params[0].position[1] + 2.0f) < 0.0001f &&
                    fabsf(begin_frame_params[0].position[2] - 7.0f) < 0.0001f,
                "Canvas3D.Begin forwards the camera position payload");

    rt_canvas3d_end(&canvas);
    cleanup_fake_canvas(&canvas);
}

static void test_synthetic_input_and_clock_advance_through_public_canvas_api(void) {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;

    init_fake_canvas(&canvas, &backend);
    canvas.gfx_win = nullptr;
    rt_keyboard_init();
    rt_mouse_init();

    rt_canvas3d_set_dt_max(&canvas, 0);
    rt_canvas3d_set_input_source(&canvas, 1);
    rt_canvas3d_set_clock_source(&canvas, 1);
    rt_canvas3d_set_synthetic_delta_time_sec(&canvas, 1.0 / 30.0);
    rt_canvas3d_push_synthetic_key(&canvas, ZANNA_KEY_W, 1);
    rt_canvas3d_push_synthetic_mouse(&canvas, 7.0, -3.0, 1LL << ZANNA_MOUSE_BUTTON_LEFT, 1.5);

    int64_t poll_open = rt_canvas3d_poll(&canvas);

    EXPECT_TRUE(poll_open == 1, "Synthetic-only Canvas3D.Poll stays open without platform events");
    EXPECT_TRUE(rt_keyboard_was_pressed(ZANNA_KEY_W) == 1,
                "Canvas3D synthetic key down records a pressed edge");
    EXPECT_TRUE(rt_keyboard_is_down(ZANNA_KEY_W) == 1,
                "Canvas3D synthetic key down holds through normal keyboard state");
    EXPECT_TRUE(rt_mouse_delta_x() == 7 && rt_mouse_delta_y() == -3,
                "Canvas3D synthetic mouse movement flows through Mouse.Delta");
    EXPECT_TRUE(rt_mouse_was_pressed(ZANNA_MOUSE_BUTTON_LEFT) == 1 && rt_mouse_left() == 1,
                "Canvas3D synthetic mouse buttons use normal button state");
    EXPECT_TRUE(std::fabs(rt_mouse_wheel_yf() - 1.5) < 0.0001,
                "Canvas3D synthetic mouse wheel keeps fractional precision");
    EXPECT_TRUE(std::fabs(rt_canvas3d_get_delta_time_sec(&canvas) - (1.0 / 30.0)) < 0.000001,
                "Canvas3D synthetic clock reports the fixed frame delta");

    rt_canvas3d_push_synthetic_key(&canvas, ZANNA_KEY_W, 0);
    rt_canvas3d_push_synthetic_mouse(&canvas, 0.0, 0.0, 0, 0.0);
    rt_canvas3d_advance_synthetic_frame(&canvas);

    EXPECT_TRUE(rt_keyboard_was_released(ZANNA_KEY_W) == 1 && rt_keyboard_is_up(ZANNA_KEY_W) == 1,
                "Canvas3D synthetic key up records a released edge");
    EXPECT_TRUE(rt_mouse_was_released(ZANNA_MOUSE_BUTTON_LEFT) == 1 && rt_mouse_left() == 0,
                "Canvas3D synthetic mouse button release uses normal button state");
    EXPECT_TRUE(rt_mouse_delta_x() == 0 && rt_mouse_delta_y() == 0 &&
                    std::fabs(rt_mouse_wheel_yf()) < 0.0001,
                "Canvas3D synthetic input queues are consumed once per frame");

    rt_canvas3d_clear_synthetic_input(&canvas);
    cleanup_fake_canvas(&canvas);
}

static int chain_has_effect(const vgfx3d_postfx_chain_t *chain, int32_t effect_type) {
    if (!chain || !chain->enabled || !chain->effects)
        return 0;
    for (int32_t i = 0; i < chain->effect_count; i++) {
        if (chain->effects[i].type == effect_type)
            return 1;
    }
    return 0;
}

static void test_quality_profile_degrades_cinematic_without_gpu_postfx(void) {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    vgfx3d_postfx_chain_t chain = {};

    backend.name = "software";
    init_fake_canvas(&canvas, &backend);

    rt_canvas3d_set_quality(&canvas, RT_GRAPHICS3D_QUALITY_CINEMATIC);
    rt_string reason = rt_canvas3d_get_quality_fallback_reason(&canvas);
    const char *reason_cs = reason ? rt_string_cstr(reason) : "";

    EXPECT_TRUE(canvas.postfx != nullptr, "Canvas3D.SetQuality attaches a PostFX profile");
    EXPECT_TRUE(rt_canvas3d_get_quality_requested(&canvas) == RT_GRAPHICS3D_QUALITY_CINEMATIC,
                "Canvas3D records the requested cinematic quality profile");
    EXPECT_TRUE(rt_canvas3d_get_quality_active(&canvas) == RT_GRAPHICS3D_QUALITY_CINEMATIC,
                "Canvas3D keeps cinematic as the active CPU-safe profile");
    EXPECT_TRUE(rt_canvas3d_get_quality_fallback(&canvas) == 1,
                "Canvas3D records cinematic quality fallback without GPU postfx");
    EXPECT_TRUE(std::strstr(reason_cs, "gpu-postfx") != nullptr,
                "Canvas3D records a readable quality fallback reason");
    EXPECT_TRUE(vgfx3d_postfx_requires_gpu_scene_buffers(canvas.postfx) == 0,
                "CPU-safe cinematic fallback does not require GPU scene buffers");
    EXPECT_TRUE(vgfx3d_postfx_get_chain(canvas.postfx, &chain) == 1,
                "CPU-safe cinematic fallback exports a PostFX chain");
    EXPECT_TRUE(!chain_has_effect(&chain, VGFX3D_POSTFX_EFFECT_SSAO) &&
                    !chain_has_effect(&chain, VGFX3D_POSTFX_EFFECT_DOF) &&
                    !chain_has_effect(&chain, VGFX3D_POSTFX_EFFECT_MOTION_BLUR),
                "CPU-safe cinematic fallback excludes GPU-only effects");

    vgfx3d_postfx_chain_free(&chain);
    cleanup_fake_canvas(&canvas);
}

static void test_quality_profile_enables_gpu_effects_when_backend_supports_postfx(void) {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    vgfx3d_postfx_chain_t chain = {};

    backend.name = "metal";

    backend.gpu_skinning = 1; /* mock mirrors real GPU backends */
    backend.present_postfx = noop_present_postfx;
    init_fake_canvas(&canvas, &backend);

    rt_canvas3d_set_quality(&canvas, RT_GRAPHICS3D_QUALITY_CINEMATIC);

    EXPECT_TRUE(canvas.postfx != nullptr, "GPU cinematic quality attaches a PostFX profile");
    EXPECT_TRUE(rt_canvas3d_get_quality_fallback(&canvas) == 0,
                "GPU cinematic quality does not record a fallback when GPU postfx is available");
    EXPECT_TRUE(vgfx3d_postfx_requires_gpu_scene_buffers(canvas.postfx) == 1,
                "GPU cinematic quality includes scene-buffer effects");
    EXPECT_TRUE(vgfx3d_postfx_get_chain(canvas.postfx, &chain) == 1,
                "GPU cinematic quality exports a PostFX chain");
    EXPECT_TRUE(chain_has_effect(&chain, VGFX3D_POSTFX_EFFECT_SSAO) &&
                    chain_has_effect(&chain, VGFX3D_POSTFX_EFFECT_DOF) &&
                    chain_has_effect(&chain, VGFX3D_POSTFX_EFFECT_MOTION_BLUR),
                "GPU cinematic quality includes GPU-only postfx when supported");

    vgfx3d_postfx_chain_free(&chain);
    cleanup_fake_canvas(&canvas);
}

#include "test_rt_canvas3d_gpu_hardening.inc"

int main() {
    test_brdf_lut_concurrent_first_use_is_safe();
    test_gpu_skinning_bypass_for_opengl();
    test_draw_repairs_corrupt_mesh_geometry_counts();
    test_gpu_skinning_bypass_for_d3d11();
    test_gpu_skinning_bypass_for_metal();
    test_cpu_skinning_fallback_for_software();
    test_large_gpu_skinning_bypass_for_opengl();
    test_large_gpu_skinning_bypass_for_d3d11();
    test_large_gpu_skinning_bypass_for_metal();
    test_gpu_morph_payload_for_opengl();
    test_gpu_morph_payload_for_d3d11();
    test_gpu_morph_payload_for_metal();
    test_gpu_morph_normal_payload_for_opengl();
    test_gpu_morph_normal_payload_for_d3d11();
    test_gpu_morph_normal_payload_for_metal();
    test_gpu_morph_rejects_nonfinite_position_payload();
    test_gpu_morph_drops_nonfinite_normal_payload();
    test_attached_morph_targets_route_through_draw_mesh();
    test_brdf_lut_matches_split_sum_reference();
    test_large_morph_payload_falls_back_to_cpu_for_opengl();
    test_large_morph_payload_falls_back_to_cpu_for_d3d11();
    test_large_morph_payload_stays_on_gpu_for_metal();
    test_cpu_morph_fallback_for_software();
    test_cpu_morph_tracking_failure_rolls_back_new_mesh_retain();
    test_gpu_morph_tracking_failure_preserves_existing_morph_retain();
    test_morph_tangent_deltas_fall_back_to_cpu_for_metal();
    test_env_map_payload_forwarded();
    test_backend_skybox_hook_used();
    test_incomplete_cubemaps_are_not_forwarded();
    test_material_repairs_wrong_class_private_refs_without_release();
    test_cubemap_finalizer_skips_wrong_class_private_faces();
    test_terrain_draw_sanitizes_private_splat_scales();
    test_terrain_draw_rejects_private_wrong_class_material();
    test_terrain_draw_rejects_private_wrong_class_splat_textures();
    test_terrain_set_material_restores_previous_splat_base_texture();
    test_terrain_chunk_aabb_includes_skirt_depth();
    test_metal_robustness_probe_accepts_degenerate_basis_and_skybox_forward();
    test_static_mesh_geometry_identity_forwarded();
    test_deferred_draw_retains_mesh_and_material_until_end();
    test_mesh_draw_traps_when_deferred_queue_cannot_grow();
    test_mesh_snapshot_failure_reports_status_and_preserves_prior_commands();
    test_rect2d_queues_overlay_pass();
    test_transform_history_forwarded_for_motion_blur();
    test_morph_weight_history_forwarded();
    test_rejected_morph_draw_does_not_advance_weight_history();
    test_skinning_palette_history_forwarded();
    test_skinning_missing_previous_palette_disables_history();
    test_instanced_transform_history_forwarded();
    test_compact_particle_batches_snapshot_instances_and_retain_unit_geometry();
    test_instanced_transform_history_survives_count_changes();
    test_deferred_instanced_draw_snapshots_instance_buffers();
    test_instanced_transform_history_skips_payload_without_motion_blur();
    test_instanced_material_payload_forwarded();
    test_pbr_material_payload_forwarded();
    test_material_draw_uses_neutral_fallbacks_for_nonfinite_private_scalars();
    test_pixels_alpha_classification_is_cached_by_content_generation();
    test_instanced_runtime_culls_outside_frustum();
    test_instanced_shadow_pass_includes_instances();
    test_transparent_sort_key_uses_mesh_bounds_depth();
    test_frame_stats_count_submissions_and_cache_repeated_world_bounds();
    test_opaque_sort_groups_material_state_before_depth();
    test_opaque_sort_keeps_depth_order_on_software_backend();
    test_transparent_sort_preserves_stable_sort_id_tie_break();
    test_transparent_sort_refines_depth_within_bucket();
    test_instanced_batch_sort_key_uses_aggregate_bounds_center();
    test_shadow_selection_prefers_strongest_directional_light_regardless_of_slot();
    test_shadow_cascades_render_primary_directional_light_slots();
    test_spot_shadow_selection_fills_budget_after_directionals();
    test_spot_shadow_selection_uses_single_slot_without_cascades();
    test_shadow_selection_uses_queued_scene_light_snapshots();
    test_casts_shadows_false_skips_shadow_selection();
    test_occlusion_mode_rejects_off_frustum_draws_before_submission();
    test_draw_mesh_preserves_forward_light_capacity();
    test_disabled_lights_are_not_submitted();
    test_clear_lights_keeps_scene_explicitly_dark();
    test_screenshot_prefers_backend_readback();
    test_screenshot_copy_reuses_destination_and_gpu_scratch();
    test_screenshot_reads_physical_framebuffer_to_logical_pixels();
    test_final_overlay_replays_after_finalize();
    test_gpu_postfx_final_overlay_presents_composited_frame();
    test_screenshot_final_finalizes_before_readback();
    test_gpu_postfx_state_latches_across_overlay_pass();
    test_begin_frame_forwards_camera_forward_and_ortho_flag();
    test_synthetic_input_and_clock_advance_through_public_canvas_api();
    test_quality_profile_degrades_cinematic_without_gpu_postfx();
    test_quality_profile_enables_gpu_effects_when_backend_supports_postfx();

    std::printf("Canvas3D GPU path tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
