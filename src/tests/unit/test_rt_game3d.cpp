//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d.cpp
// Purpose: Unit tests for the runtime-backed Zanna.Game3D helper layer.
// Key invariants:
//   - Game3D public runtime helpers preserve deterministic state across VM and
//     native-style C entry point usage.
//   - Entity/world lifetime tests assert defined behavior for both live and
//     stale retained handles.
// Ownership/Lifetime:
//   - Test-created runtime handles are process-local and rely on the runtime GC
//     conventions used by the production C ABI.
//   - Private layout mirrors are used only for targeted invariant checks and
//     must stay in sync with rt_game3d_internal.h.
// Links: src/runtime/graphics/3d/rt_game3d.h,
//   src/runtime/graphics/3d/rt_game3d_internal.h
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt.hpp"
#include "rt_animcontroller3d.h"
#include "rt_asset.h"
#include "rt_audio.h"
#include "rt_blendtree3d.h"
#include "rt_canvas3d.h"
#include "rt_decal3d.h"
#include "rt_game3d.h"
#include "rt_game3d_diagnostics.h"
#include "rt_graphics3d_ids.h"
#include "rt_heap.h"
#include "rt_iksolver3d.h"
#include "rt_input.h"
#include "rt_mat4.h"
#include "rt_model3d.h"
#include "rt_navmesh3d.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_particles3d.h"
#include "rt_path3d.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_postfx3d.h"
#include "rt_quat.h"
#include "rt_savedata.h"
#include "rt_scene3d.h"
#include "rt_skeleton3d.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"
#include "rt_string.h"
#include "rt_synth.h"
#include "rt_terrain3d.h"
#include "rt_threads.h"
#include "rt_vec2.h"
#include "rt_vec3.h"

extern "C" {
#include "rt_canvas3d_internal.h"
#include "rt_scene3d_internal.h"
#include "vgfx3d_backend.h"
}

typedef struct {
    double look_sensitivity;
    int8_t has_snapshot;
    uint8_t key_down[ZANNA_KEY_MAX];
    uint8_t key_pressed[ZANNA_KEY_MAX];
    uint8_t key_released[ZANNA_KEY_MAX];
    uint8_t mouse_down[ZANNA_MOUSE_BUTTON_MAX];
    uint8_t mouse_pressed[ZANNA_MOUSE_BUTTON_MAX];
    uint8_t mouse_released[ZANNA_MOUSE_BUTTON_MAX];
    int64_t mouse_dx;
    int64_t mouse_dy;
    double mouse_fdx;
    double mouse_fdy;
    int64_t mouse_x;
    int64_t mouse_y;
    double wheel_y;
    int64_t bound_pad;
    double pad_look_sensitivity;
    double pad_lx;
    double pad_ly;
    double pad_rx;
    double pad_ry;
    int8_t pad_connected;
} Game3DInputTestLayout;

typedef struct {
    void *controller;
    void *node_animator;
    rt_string events[64];
    int32_t event_count;
} Game3DAnimatorTestLayout;

typedef struct {
    int64_t phase;
    void *a;
    void *b;
    void *raw;
} Game3DCollisionEventTestLayout;

typedef struct {
    void *canvas;
    void *camera;
    void *scene;
    void *physics;
    void *input;
    void *audio;
    void *effects;
    void *stream;
    void *camera_controller;
    void **entities;
    int32_t entity_count;
    int32_t entity_capacity;
    void *body_index_entries;
    int32_t body_index_count;
    int32_t body_index_capacity;
    void *name_index_hashes;
    void **name_index_entities;
    int32_t name_index_count;
    int32_t name_index_capacity;
    int8_t name_index_valid;
    void **animation_animators;
    int32_t animation_animator_capacity;
    void **animation_seen_set;
    size_t animation_seen_capacity;
    void *animation_jobs;
    int32_t animation_job_capacity;
    int64_t next_entity_id;
    double dt;
    double elapsed;
    int64_t frame;
    int64_t dropped_fixed_steps;
    double time_scale;
    int8_t paused;
    double hitstop_remaining;
    double unscaled_dt;
    double unscaled_elapsed;
    int64_t worker_count;
    void *job_pool;
    int8_t jobs_enabled;
    int8_t floating_origin;
    double origin_rebase_threshold;
    double world_origin[3];
} Game3DWorldTestLayout;

typedef struct {
    int64_t id;
    void *node;
    void *mesh;
    void *material;
    void *body;
    void *anim;
    void *behavior;
    void **hitboxes;
    int32_t hitbox_count;
    int32_t hitbox_capacity;
    void *health;
    void *ragdoll;
    void *lipsync;
    void *footsteps;
    void *interactable;
    void *interactor;
    void *perception;
    void *btree;
    int64_t layer;
    int64_t collision_mask_bits;
    rt_string name;
    void *world;
    void *parent;
    void **children;
    int32_t child_count;
    int32_t child_capacity;
    int32_t registry_index;
    int8_t group;
    int8_t alive;
    int8_t spawned;
    int8_t destroyed;
    double interp_prev_position[3];
    double interp_prev_rotation[4];
    double interp_saved_position[3];
    double interp_saved_rotation[4];
    int8_t interp_has_prev;
    int8_t interp_pose_blended;
    rt_string persistent_key;
    int64_t state_tag;
    uint32_t sim_tick_stamp;
    int32_t child_storage_capacity;
    uint64_t child_storage_cookie;
} Game3DEntityTestLayout;

typedef struct {
    rt_string path;
    int8_t asset_path;
    void *model;
} Game3DSceneTemplateTestLayout;

typedef struct {
    int64_t shape;
    double half_extents[3];
    double radius;
    double height;
    double mass;
    double friction;
    double restitution;
    int64_t layer;
    int64_t mask_bits;
    int64_t sync_mode;
    int8_t has_layer;
    int8_t has_mask;
    int8_t is_static;
    int8_t is_kinematic;
    int8_t is_trigger;
    int8_t use_ccd;
} Game3DBodyDefTestLayout;

typedef struct {
    void *listener;
    void *camera;
    void **sources;
    int32_t source_count;
    int32_t source_capacity;
    double ref_distance;
    double max_distance;
    int64_t volume;
    int8_t listener_follow_camera;
    void **reverb_zones;
    int32_t reverb_zone_count;
    int32_t reverb_zone_capacity;
    int64_t reverb_group;
    int64_t reverb_fx;
    double reverb_blend;
    double reverb_room;
    double reverb_damp;
    double reverb_wet;
    int8_t reverb_routing;
    int8_t occlusion_enabled;
    int64_t occlusion_mask;
    double occlusion_amount;
    int32_t occlusion_budget;
    int32_t occlusion_cursor;
    int64_t dialogue_group;
    void *ambient_bed;
} Game3DAudioTestLayout;

typedef struct {
    void *postfx;
    void *items;
    int32_t count;
    int32_t capacity;
} Game3DEffectsTestLayout;

typedef struct {
    void *world;
    void *entity;
    void *character;
    double speed;
    double jump_speed;
    double gravity;
    double vertical_velocity;
    double eye_height;
    double stand_height;
    double crouch_height;
    double capsule_radius;
    int8_t crouching;
} Game3DCharacterControllerTestLayout;

typedef struct {
    void *world;
    void *character_controller;
    double speed;
    double look_sensitivity;
    int8_t capture_mouse;
} Game3DFirstPersonControllerTestLayout;

typedef struct {
    void *world;
    double speed;
    double look_sensitivity;
    int8_t capture_mouse;
} Game3DFreeFlyControllerTestLayout;

typedef struct {
    void *world;
    void *target;
    double distance;
    double min_distance;
    double max_distance;
    double yaw;
    double pitch;
    double orbit_sensitivity;
    double zoom_sensitivity;
} Game3DOrbitControllerTestLayout;

typedef struct {
    void *world;
    void *target_entity;
    void *offset;
    double damping;
} Game3DFollowControllerTestLayout;

typedef struct {
    int8_t ready;
    int8_t cancelled;
    int8_t deferred;
    int8_t async_started;
    int8_t asset_path;
    int8_t template_request;
    double progress;
    rt_string error;
    rt_string path;
    void *entity;
    void *model_template;
} Game3DAssetHandleTestLayout;

typedef struct {
    rt_string name;
    rt_string path;
    double center[3];
    double radius;
    int64_t resident_bytes;
    int64_t measured_resident_bytes;
    rt_string material;
    rt_string nav_area;
    rt_string sidecar_path;
    int64_t layer;
    int64_t collision_mask;
    double traversal_cost;
    int8_t has_layer;
    int8_t has_collision_mask;
    int8_t collision_enabled;
    void *scene;
    void *entity;
    int8_t resident;
    void *sidecar_data;
    int64_t sidecar_bytes;
} Game3DStreamCellTestLayout;

typedef struct {
    rt_string name;
    rt_string path;
    rt_string heightmap_path;
    double center[3];
    double scale[3];
    double radius;
    int64_t width;
    int64_t depth;
    int64_t resident_bytes;
    rt_string material;
    rt_string nav_area;
    rt_string sidecar_path;
    int64_t layer;
    int64_t collision_mask;
    double traversal_cost;
    int8_t has_layer;
    int8_t has_collision_mask;
    int8_t collision_enabled;
    void *terrain;
    void *collider_entity;
    void *nav_entity;
    int8_t resident;
} Game3DStreamTerrainTileTestLayout;

typedef struct {
    void *world;
    double center[3];
    double load_radius;
    double unload_radius;
    int64_t residency_budget_bytes;
    int64_t resident_cell_count;
    int64_t resident_terrain_tile_count;
    int64_t pending_request_count;
    int64_t resident_bytes;
    rt_string terrain_manifest;
    rt_string cells_manifest;
    Game3DStreamCellTestLayout *cells;
    Game3DStreamTerrainTileTestLayout *terrain_tiles;
    int32_t cell_count;
    int32_t cell_capacity;
    int32_t terrain_tile_count;
    int32_t terrain_tile_capacity;
    int8_t cells_manifest_loaded;
    int8_t terrain_manifest_loaded;
    int8_t retains_world;
} Game3DWorldStreamTestLayout;

#include <chrono>
#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

typedef void (*rt_game3d_test_async_cache_publish_hook_fn)(void *user_data);
extern "C" void rt_game3d_test_set_async_cache_publish_hook(
    rt_game3d_test_async_cache_publish_hook_fn hook, void *user_data);

namespace {
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;
static bool g_return_trap = false;
static int g_returning_trap_count = 0;
static int g_tests_passed = 0;
static int g_tests_total = 0;
static int g_update_calls = 0;
static int g_overlay_calls = 0;
static double g_update_dt_sum = 0.0;
static rt_canvas3d *g_observed_canvas = nullptr;
static int32_t g_observed_input_source = -1;
static int32_t g_observed_clock_source = -1;
static int64_t g_observed_synthetic_dt_us = -1;
static rt_canvas3d *g_fixed_loop_canvas = nullptr;
static void *g_fixed_loop_world = nullptr;
static int g_fixed_loop_stop_after = 0;
static bool g_fixed_loop_destroy_at_stop = false;
static int64_t g_fixed_loop_observed_dropped = -1;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    if (g_return_trap) {
        ++g_returning_trap_count;
        return;
    }
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

#define TEST(name)                                                                                 \
    do {                                                                                           \
        ++g_tests_total;                                                                           \
        std::printf("  [%d] %s... ", g_tests_total, name);                                         \
    } while (0)

#define PASS()                                                                                     \
    do {                                                                                           \
        ++g_tests_passed;                                                                          \
        std::printf("ok\n");                                                                       \
        return true;                                                                               \
    } while (0)

#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        std::printf("FAIL: %s\n", msg);                                                            \
        return false;                                                                              \
    } while (0)

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond))                                                                               \
            FAIL(msg);                                                                             \
    } while (0)

#define EXPECT_EQ_INT(actual, expected, msg)                                                       \
    do {                                                                                           \
        const long long got_ = (long long)(actual);                                                \
        const long long want_ = (long long)(expected);                                             \
        if (got_ != want_) {                                                                       \
            std::printf("FAIL: %s (got %lld, expected %lld)\n", msg, got_, want_);                 \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(actual, expected, eps, msg)                                                    \
    do {                                                                                           \
        const double got_ = (double)(actual);                                                      \
        const double want_ = (double)(expected);                                                   \
        if (std::fabs(got_ - want_) > (eps)) {                                                     \
            std::printf("FAIL: %s (got %.6f, expected %.6f)\n", msg, got_, want_);                 \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static rt_string test_fixture_path(const char *relative) {
#ifdef ZANNA_SOURCE_DIR
    char path[4096];
    std::snprintf(path, sizeof(path), "%s/%s", ZANNA_SOURCE_DIR, relative);
    return rt_const_cstr(path);
#else
    return rt_const_cstr(relative);
#endif
}

static bool wait_asset_ready(void *handle, int attempts = 200) {
    for (int i = 0; i < attempts; ++i) {
        if (rt_game3d_asset_handle_get_ready(handle) != 0)
            return true;
        rt_thread_sleep(10);
    }
    return rt_game3d_asset_handle_get_ready(handle) != 0;
}

static bool write_text_file(const char *path, const char *text) {
    FILE *file = std::fopen(path, "wb");
    if (!file)
        return false;
    size_t len = std::strlen(text);
    bool ok = len == 0 || std::fwrite(text, 1, len, file) == len;
    return std::fclose(file) == 0 && ok;
}

static std::string terrain_heightmap_fixture(
    int width, int depth, double west_edge, double east_edge, double interior) {
    std::string text =
        "zanna-heightmap-v1 " + std::to_string(width) + " " + std::to_string(depth) + "\n";
    char sample[32];
    for (int z = 0; z < depth; ++z) {
        for (int x = 0; x < width; ++x) {
            double h = interior + (double)((x + z) % 3) * 0.01;
            if (x == 0)
                h = west_edge;
            if (x == width - 1)
                h = east_edge;
            std::snprintf(sample, sizeof(sample), "%.4f", h);
            text += sample;
            text += (x + 1 == width) ? "\n" : " ";
        }
    }
    return text;
}

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

struct Game3DRenderCapture {
    std::vector<uint32_t> pixels;
    int64_t width = 0;
    int64_t height = 0;
    int64_t draw_count = 0;
    int64_t occluded_draw_count = 0;
    int8_t camera_relative_upload = 0;
};

static int game3d_copy_pixels(void *pixels, Game3DRenderCapture &capture) {
    if (!pixels)
        return 0;
    capture.width = rt_pixels_width(pixels);
    capture.height = rt_pixels_height(pixels);
    if (capture.width <= 0 || capture.height <= 0)
        return 0;
    const uint32_t *raw = rt_pixels_raw_buffer(pixels);
    size_t count = (size_t)capture.width * (size_t)capture.height;
    capture.pixels.assign(count, 0);
    if (raw) {
        std::memcpy(capture.pixels.data(), raw, count * sizeof(uint32_t));
    } else {
        for (int64_t y = 0; y < capture.height; ++y) {
            for (int64_t x = 0; x < capture.width; ++x)
                capture.pixels[(size_t)y * (size_t)capture.width + (size_t)x] =
                    (uint32_t)rt_pixels_get_rgba(pixels, x, y);
        }
    }
    return 1;
}

static int game3d_render_parity_scene(double base_x,
                                      int8_t floating_origin,
                                      int8_t frustum_culling,
                                      int8_t toggle_before_off,
                                      int8_t include_scene,
                                      Game3DRenderCapture &capture) {
    capture = Game3DRenderCapture{};
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D 50km Parity"), 64, 48);
    if (!world)
        return 0;
    rt_canvas3d *canvas = (rt_canvas3d *)rt_game3d_world_get_canvas(world);
    if (!canvas) {
        rt_game3d_world_destroy(world);
        return 0;
    }
    rt_canvas3d_set_frustum_culling(canvas, frustum_culling);
    rt_canvas3d_set_backface_cull(canvas, 0);
    rt_game3d_world_set_ambient(world, 1.0, 1.0, 1.0);

    if (include_scene) {
        void *material = rt_material3d_new_color(0.9, 0.2, 0.08);
        rt_material3d_set_unlit(material, 1);
        void *mesh = rt_mesh3d_new_box(1.6, 1.2, 1.0);
        void *visible = rt_game3d_entity_of(mesh, material);
        rt_game3d_entity_set_position(visible, base_x, 1.0, 0.0);
        rt_game3d_world_spawn(world, visible);

        void *decoy_material = rt_material3d_new_color(0.1, 0.8, 0.2);
        rt_material3d_set_unlit(decoy_material, 1);
        void *decoy_mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
        void *decoy = rt_game3d_entity_of(decoy_mesh, decoy_material);
        rt_game3d_entity_set_position(decoy, base_x + 1000.0, 1.0, 0.0);
        rt_game3d_world_spawn(world, decoy);
    }

    void *camera = rt_game3d_world_get_camera(world);
    rt_camera3d_look_at(camera,
                        rt_vec3_new(base_x, 2.0, 6.0),
                        rt_vec3_new(base_x, 1.0, 0.0),
                        rt_vec3_new(0.0, 1.0, 0.0));
    if (toggle_before_off) {
        rt_game3d_world_set_floating_origin(world, 1);
        rt_game3d_world_set_floating_origin(world, 0);
    } else {
        rt_game3d_world_set_floating_origin(world, floating_origin);
    }
    if (floating_origin && !toggle_before_off) {
        rt_game3d_world_set_origin_rebase_threshold(world, 1.0);
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    }

    rt_game3d_world_begin_frame(world);
    capture.camera_relative_upload = canvas->camera_relative_upload;
    rt_game3d_world_draw_scene(world);
    rt_game3d_world_end_scene(world);
    capture.draw_count = rt_game3d_world_get_draw_count(world);
    capture.occluded_draw_count = rt_game3d_world_get_occluded_draw_count(world);
    void *pixels = rt_game3d_world_capture_final_frame(world);
    int ok = game3d_copy_pixels(pixels, capture);
    rt_game3d_world_destroy(world);
    return ok;
}

static int game3d_pixels_match_with_tolerance(const Game3DRenderCapture &a,
                                              const Game3DRenderCapture &b,
                                              int max_channel_delta,
                                              double max_mean_delta) {
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size())
        return 0;
    uint64_t total_delta = 0;
    int max_delta = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        uint32_t pa = a.pixels[i];
        uint32_t pb = b.pixels[i];
        for (int shift = 0; shift <= 24; shift += 8) {
            int da = (int)((pa >> shift) & 0xFFu);
            int db = (int)((pb >> shift) & 0xFFu);
            int delta = std::abs(da - db);
            if (delta > max_delta)
                max_delta = delta;
            total_delta += (uint64_t)delta;
        }
    }
    double denom = a.pixels.empty() ? 1.0 : (double)a.pixels.size() * 4.0;
    double mean_delta = (double)total_delta / denom;
    if (max_delta > max_channel_delta || mean_delta > max_mean_delta) {
        std::fprintf(stderr,
                     "FAIL: render parity delta too high (max=%d mean=%.6f, limits=%d/%.6f)\n",
                     max_delta,
                     mean_delta,
                     max_channel_delta,
                     max_mean_delta);
        return 0;
    }
    return 1;
}

static void set_software_backend_env() {
#if RT_PLATFORM_WINDOWS
    _putenv_s("ZANNA_3D_BACKEND", "software");
#else
    setenv("ZANNA_3D_BACKEND", "software", 1);
#endif
}

extern "C" void game3d_test_update(double dt) {
    ++g_update_calls;
    g_update_dt_sum += dt;
}

extern "C" void game3d_test_observing_update(double dt) {
    game3d_test_update(dt);
    if (g_observed_canvas) {
        g_observed_input_source = g_observed_canvas->input_source;
        g_observed_clock_source = g_observed_canvas->clock_source;
        g_observed_synthetic_dt_us = g_observed_canvas->synthetic_dt_us;
    }
}

extern "C" void game3d_test_trapping_update(double) {
    rt_trap("runFrames callback trap");
}

extern "C" void game3d_test_fixed_update(double dt) {
    game3d_test_update(dt);
    if (g_fixed_loop_canvas && g_fixed_loop_stop_after > 0 &&
        g_update_calls >= g_fixed_loop_stop_after) {
        if (g_fixed_loop_world)
            g_fixed_loop_observed_dropped =
                rt_game3d_world_get_dropped_fixed_steps(g_fixed_loop_world);
        if (g_fixed_loop_destroy_at_stop && g_fixed_loop_world) {
            rt_game3d_world_destroy(g_fixed_loop_world);
            return;
        }
        g_fixed_loop_canvas->should_close = 1;
    }
}

extern "C" void game3d_test_overlay(void) {
    ++g_overlay_calls;
}

template <typename T> static void append_game3d_test_bytes(std::vector<uint8_t> &out, T value) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

static void game3d_test_write16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
}

static void game3d_test_write32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 24));
}

static void game3d_test_write64(std::vector<uint8_t> &out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back((uint8_t)(v >> (i * 8)));
}

static std::vector<uint8_t> game3d_test_persistence_header(uint32_t records, uint32_t flags) {
    std::vector<uint8_t> out = {'V', 'W', '3', 'D', 'S', 'A', 'V', '1'};
    game3d_test_write32(out, 1);
    game3d_test_write32(out, records);
    game3d_test_write32(out, flags);
    for (int i = 0; i < 4; ++i)
        game3d_test_write64(out, 0);
    return out;
}

static void game3d_test_append_persistence_record(std::vector<uint8_t> &out,
                                                  const char *key,
                                                  uint8_t alive) {
    size_t key_length = std::strlen(key);
    game3d_test_write32(out, (uint32_t)key_length);
    out.insert(out.end(), key, key + key_length);
    out.push_back(alive);
    for (int i = 0; i < 11; ++i)
        game3d_test_write64(out, 0);
}

static void game3d_test_align(std::vector<uint8_t> &out, size_t alignment) {
    size_t rem = out.size() % alignment;
    if (rem != 0)
        out.insert(out.end(), alignment - rem, 0);
}

static bool game3d_test_read_file_bytes(const char *path, std::vector<uint8_t> &out) {
    out.clear();
    FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    long size = std::ftell(file);
    if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    out.resize((size_t)size);
    bool ok = size == 0 || std::fread(out.data(), 1, (size_t)size, file) == (size_t)size;
    std::fclose(file);
    return ok;
}

static std::string game3d_test_base64_encode(const uint8_t *data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2u) / 3u) * 4u);
    for (size_t i = 0; i < len; i += 3u) {
        uint32_t value = ((uint32_t)data[i]) << 16;
        int remaining = (int)(len - i);
        if (remaining > 1)
            value |= ((uint32_t)data[i + 1]) << 8;
        if (remaining > 2)
            value |= (uint32_t)data[i + 2];
        out.push_back(table[(value >> 18) & 0x3Fu]);
        out.push_back(table[(value >> 12) & 0x3Fu]);
        out.push_back(remaining > 1 ? table[(value >> 6) & 0x3Fu] : '=');
        out.push_back(remaining > 2 ? table[value & 0x3Fu] : '=');
    }
    return out;
}

static bool write_game3d_embedded_triangle_gltf(const char *path, float x_offset) {
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {
        x_offset, 0.0f, 0.0f, x_offset + 1.0f, 0.0f, 0.0f, x_offset, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_game3d_test_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_game3d_test_bytes(gltf_buffer, v);

    std::string buffer_b64 = game3d_test_base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    return write_text_file(path, gltf_json.c_str());
}

static void release_runtime_ref(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static double game3d_template_aabb_min_x(void *model_template) {
    void *entity = rt_game3d_model_template_instantiate(model_template);
    if (!entity)
        return HUGE_VAL;
    void *node = rt_game3d_entity_get_node(entity);
    void *min_v = rt_scene_node3d_get_aabb_min(node);
    double min_x = min_v ? rt_vec3_x(min_v) : HUGE_VAL;
    release_runtime_ref(min_v);
    release_runtime_ref(entity);
    return min_x;
}

struct Game3DStaleAsyncPublishHookState {
    const char *path;
    int calls;
    int rewrite_ok;
};

extern "C" void game3d_test_clear_cache_during_async_publish(void *user_data) {
    Game3DStaleAsyncPublishHookState *state = (Game3DStaleAsyncPublishHookState *)user_data;
    if (!state)
        return;
    state->calls++;
    rt_game3d_test_set_async_cache_publish_hook(nullptr, nullptr);
    rt_game3d_assets_clear_cache();
    state->rewrite_ok = write_game3d_embedded_triangle_gltf(state->path, 5.0f) ? 1 : 0;
}

struct Game3DTestZpakEntry {
    const char *name;
    const uint8_t *data;
    size_t size;
};

static bool write_game3d_test_zpak(const char *path,
                                   const Game3DTestZpakEntry *entries,
                                   size_t entry_count) {
    std::vector<uint8_t> out;
    std::vector<uint64_t> offsets;
    out.reserve(512);
    out.insert(out.end(), {'Z', 'P', 'A', 'K'});
    game3d_test_write16(out, 1);
    game3d_test_write16(out, 0);
    game3d_test_write32(out, (uint32_t)entry_count);
    size_t toc_offset_pos = out.size();
    game3d_test_write64(out, 0);
    size_t toc_size_pos = out.size();
    game3d_test_write64(out, 0);
    game3d_test_write32(out, 0);

    offsets.reserve(entry_count);
    for (size_t i = 0; i < entry_count; ++i) {
        game3d_test_align(out, 8);
        offsets.push_back((uint64_t)out.size());
        out.insert(out.end(), entries[i].data, entries[i].data + entries[i].size);
    }

    game3d_test_align(out, 8);
    uint64_t toc_offset = (uint64_t)out.size();
    size_t toc_start = out.size();
    for (size_t i = 0; i < entry_count; ++i) {
        size_t name_len = std::strlen(entries[i].name);
        game3d_test_write16(out, (uint16_t)name_len);
        out.insert(out.end(), entries[i].name, entries[i].name + name_len);
        game3d_test_write64(out, offsets[i]);
        game3d_test_write64(out, (uint64_t)entries[i].size);
        game3d_test_write64(out, (uint64_t)entries[i].size);
        game3d_test_write16(out, 0);
        game3d_test_write16(out, 0);
    }
    uint64_t toc_size = (uint64_t)(out.size() - toc_start);
    for (int i = 0; i < 8; ++i) {
        out[toc_offset_pos + i] = (uint8_t)(toc_offset >> (i * 8));
        out[toc_size_pos + i] = (uint8_t)(toc_size >> (i * 8));
    }

    FILE *file = std::fopen(path, "wb");
    if (!file)
        return false;
    bool ok = out.empty() || std::fwrite(out.data(), 1, out.size(), file) == out.size();
    return std::fclose(file) == 0 && ok;
}

static void *make_game3d_test_anim(const char *name,
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

static void *make_game3d_test_controller(double walk_distance, double event_time) {
    void *skel = rt_skeleton3d_new();
    int64_t root = rt_skeleton3d_add_bone(skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(skel);

    void *idle = make_game3d_test_anim("idle", root, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    void *walk = make_game3d_test_anim("walk", root, 0.0, 0.0, 0.0, walk_distance, 0.0, 0.0);
    void *controller = rt_anim_controller3d_new(skel);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("idle"), idle);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("walk"), walk);
    rt_anim_controller3d_add_event(
        controller, rt_const_cstr("walk"), event_time, rt_const_cstr("step"));
    rt_anim_controller3d_set_root_motion_bone(controller, root);
    return controller;
}

static bool test_persistence_snapshot_validator_is_canonical_and_exact() {
    TEST("VW3DSAV1 validator requires canonical exact snapshots");
    std::vector<uint8_t> empty = game3d_test_persistence_header(0, 0);
    EXPECT_TRUE(rt_game3d_persistence_validate(empty.data(), (int64_t)empty.size()) == 1,
                "an exact empty snapshot validates");

    std::vector<uint8_t> trailing = empty;
    trailing.push_back(0x7f);
    EXPECT_TRUE(rt_game3d_persistence_validate(trailing.data(), (int64_t)trailing.size()) == 0,
                "trailing bytes are rejected");

    std::vector<uint8_t> empty_key = game3d_test_persistence_header(1, 0);
    game3d_test_append_persistence_record(empty_key, "", 1);
    EXPECT_TRUE(rt_game3d_persistence_validate(empty_key.data(), (int64_t)empty_key.size()) == 0,
                "empty entity keys are rejected before load");

    std::vector<uint8_t> invalid_alive = game3d_test_persistence_header(1, 0);
    game3d_test_append_persistence_record(invalid_alive, "crate", 2);
    EXPECT_TRUE(
        rt_game3d_persistence_validate(invalid_alive.data(), (int64_t)invalid_alive.size()) == 0,
        "noncanonical alive bytes are rejected");

    std::vector<uint8_t> embedded_nul = game3d_test_persistence_header(1, 0);
    game3d_test_write32(embedded_nul, 3);
    embedded_nul.push_back('a');
    embedded_nul.push_back(0);
    embedded_nul.push_back('b');
    embedded_nul.push_back(1);
    for (int i = 0; i < 11; ++i)
        game3d_test_write64(embedded_nul, 0);
    EXPECT_TRUE(rt_game3d_persistence_validate(embedded_nul.data(), (int64_t)embedded_nul.size()) ==
                    0,
                "embedded NUL bytes cannot alias persistence identities");

    std::vector<uint8_t> record = game3d_test_persistence_header(1, 0);
    game3d_test_append_persistence_record(record, "crate", 1);
    EXPECT_TRUE(rt_game3d_persistence_validate(record.data(), (int64_t)record.size()) == 1,
                "a canonical record snapshot validates");
    PASS();
}

static bool test_persistence_duplicate_load_is_transactional() {
    TEST("VW3DSAV1 duplicate-key load preserves the live persistence store");
    void *world = rt_game3d_world_new(rt_const_cstr("Persistence Transaction Unit"), 32, 24);
    void *entity = rt_game3d_entity_new();
    rt_string key = rt_const_cstr("transactional-crate");
    rt_game3d_entity_set_position(entity, 7.0, 8.0, 9.0);
    rt_game3d_world_spawn(world, entity);
    rt_game3d_entity_set_persistent(entity, key);

    void *before = rt_game3d_world_get_persistent_position(world, key);
    EXPECT_NEAR(rt_vec3_x(before), 7.0, 1e-9, "fixture publishes the live persistent pose");
    if (rt_obj_release_check0(before))
        rt_obj_free(before);

    std::vector<uint8_t> duplicate = game3d_test_persistence_header(2, 0);
    game3d_test_append_persistence_record(duplicate, "transactional-crate", 1);
    game3d_test_append_persistence_record(duplicate, "transactional-crate", 1);
    EXPECT_TRUE(rt_game3d_persistence_validate(duplicate.data(), (int64_t)duplicate.size()) == 1,
                "duplicate-key fixture is structurally valid before semantic staging");

    rt_string app = rt_const_cstr("ZannaGame3DAuditTest");
    unsigned long long nonce =
        (unsigned long long)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    char slot_name[64];
    std::snprintf(slot_name,
                  sizeof(slot_name),
                  "transaction-%llx-%llx",
                  nonce,
                  (unsigned long long)(uintptr_t)world);
    rt_string slot = rt_const_cstr(slot_name);
    rt_string dir = rt_path_data_dir(app);
    std::string path = std::string(rt_string_cstr(dir)) + "/" + slot_name + ".vw3dsav";
    FILE *file = std::fopen(path.c_str(), "wb");
    EXPECT_TRUE(file != nullptr, "transactional load fixture opens in the platform data directory");
    bool wrote = std::fwrite(duplicate.data(), 1, duplicate.size(), file) == duplicate.size();
    wrote = std::fclose(file) == 0 && wrote;
    EXPECT_TRUE(wrote, "transactional load fixture is written completely");

    EXPECT_TRUE(rt_game3d_world_load_state(world, app, slot) == 0,
                "duplicate persistent identities reject during staged load");
    EXPECT_EQ_INT(rt_game3d_world_get_persistent_alive(world, key),
                  1,
                  "failed staged load preserves the live record liveness");
    void *after = rt_game3d_world_get_persistent_position(world, key);
    EXPECT_NEAR(rt_vec3_x(after), 7.0, 1e-9, "failed staged load preserves persistent position X");
    EXPECT_NEAR(rt_vec3_y(after), 8.0, 1e-9, "failed staged load preserves persistent position Y");
    EXPECT_NEAR(rt_vec3_z(after), 9.0, 1e-9, "failed staged load preserves persistent position Z");
    if (rt_obj_release_check0(after))
        rt_obj_free(after);

    std::remove(path.c_str());
    rt_string_unref(dir);
    rt_string_unref(slot);
    rt_string_unref(app);
    rt_string_unref(key);
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_layermasks_and_constants() {
    TEST("Game3D constants and LayerMask operations");
    EXPECT_EQ_INT(rt_game3d_layers_world(), 1, "World layer bit");
    EXPECT_EQ_INT(rt_game3d_layers_dynamic(), 2, "Dynamic layer bit");
    EXPECT_EQ_INT(rt_game3d_layers_player(), 4, "Player layer bit");
    EXPECT_EQ_INT(rt_game3d_quality_balanced(), 1, "Balanced quality value");
    EXPECT_EQ_INT(rt_game3d_collision_any(), 3, "Any collision phase value");
    EXPECT_EQ_INT(rt_game3d_shading_model_unlit(), 3, "Game3D unlit matches backend shading slot");
    EXPECT_EQ_INT(
        rt_game3d_shading_model_fresnel(), 4, "Game3D fresnel matches backend shading slot");
    EXPECT_EQ_INT(
        rt_game3d_shading_model_emissive(), 5, "Game3D emissive matches backend shading slot");

    void *mask = rt_game3d_layermask_of(rt_game3d_layers_world());
    EXPECT_TRUE(mask != nullptr, "LayerMask.Of returns an object");
    EXPECT_EQ_INT(rt_game3d_layermask_get_bits(mask),
                  rt_game3d_layers_world(),
                  "LayerMask.Of stores the layer bit");
    EXPECT_TRUE(rt_game3d_layermask_include(mask, rt_game3d_layers_player()) == mask,
                "LayerMask.include is fluent");
    EXPECT_TRUE(rt_game3d_layermask_includes(mask, rt_game3d_layers_world()) != 0,
                "LayerMask includes initial layer");
    EXPECT_TRUE(rt_game3d_layermask_includes(mask, rt_game3d_layers_player()) != 0,
                "LayerMask includes appended layer");
    EXPECT_TRUE(rt_game3d_layermask_includes(mask, rt_game3d_layers_trigger()) == 0,
                "LayerMask excludes missing layer");

    void *all = rt_game3d_layermask_all();
    EXPECT_EQ_INT(rt_game3d_layermask_get_bits(all), -1, "LayerMask.All uses every bit");
    rt_game3d_layermask_set_bits(all, -1);
    EXPECT_EQ_INT(rt_game3d_layermask_get_bits(all), -1, "negative mask bits clamp to all");

    EXPECT_TRUE(expect_trap_contains([&] { rt_game3d_layermask_of(3); }, "single positive bit"),
                "LayerMask.Of rejects multi-bit layers");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_layermask_include(mask, 0); }, "single positive bit"),
        "LayerMask.include rejects zero layer");
    PASS();
}

static bool test_world_worker_controls() {
    TEST("World3D exposes internal worker controls with Game3D naming");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Worker Controls"), 64, 48);
    EXPECT_TRUE(world != nullptr, "World3D.New returns an object");
    EXPECT_TRUE(rt_game3d_world_get_worker_count(world) >= 1, "workerCount defaults positive");
    EXPECT_EQ_INT(rt_game3d_world_get_jobs_enabled(world),
                  rt_game3d_world_get_worker_count(world) > 1 ? 1 : 0,
                  "jobsEnabled follows workerCount");

    rt_game3d_world_set_worker_count(world, 1);
    EXPECT_EQ_INT(rt_game3d_world_get_worker_count(world), 1, "setWorkerCount stores one worker");
    EXPECT_EQ_INT(rt_game3d_world_get_jobs_enabled(world), 0, "one worker disables jobs");

    rt_game3d_world_set_worker_count(world, 4);
    EXPECT_EQ_INT(rt_game3d_world_get_worker_count(world), 4, "setWorkerCount stores worker count");
    EXPECT_EQ_INT(rt_game3d_world_get_jobs_enabled(world), 1, "multiple workers enable jobs");

    rt_game3d_world_set_worker_count(world, 0);
    EXPECT_EQ_INT(rt_game3d_world_get_worker_count(world), 1, "zero worker count clamps to one");
    EXPECT_EQ_INT(
        rt_game3d_world_get_jobs_enabled(world), 0, "clamped single worker disables jobs");

    rt_game3d_world_set_worker_count(world, 4096);
    EXPECT_EQ_INT(rt_game3d_world_get_worker_count(world), 64, "worker count clamps to max");
    EXPECT_EQ_INT(
        rt_game3d_world_get_jobs_enabled(world), 1, "clamped multi-worker count enables jobs");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_input_axes() {
    TEST("Input3D delegates named input and exposes move/look axes");
    rt_keyboard_init();
    rt_mouse_init();
    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();

    void *input = rt_game3d_input_new();
    rt_game3d_input_set_look_sensitivity(input, 0.05);

    rt_keyboard_on_key_down(rt_game3d_key_w());
    rt_keyboard_on_key_down(rt_game3d_key_space());
    rt_mouse_force_delta(12, -4);
    rt_mouse_button_down(rt_game3d_mouse_left());
    rt_mouse_update_wheel(0.0, 1.25);

    EXPECT_TRUE(rt_game3d_input_is_down(input, rt_game3d_key_w()) != 0, "W is down");
    EXPECT_TRUE(rt_game3d_input_pressed(input, rt_game3d_key_w()) != 0, "W was pressed");
    EXPECT_TRUE(rt_game3d_input_mouse_button(input, rt_game3d_mouse_left()) != 0,
                "left mouse is down");
    EXPECT_TRUE(rt_game3d_input_mouse_pressed(input, rt_game3d_mouse_left()) != 0,
                "left mouse was pressed");
    EXPECT_NEAR(rt_game3d_input_wheel_y(input), 1.25, 0.0001, "wheelY is delegated");

    void *move = rt_game3d_input_move_axis(input);
    const double inv_sqrt2 = 0.7071067811865475;
    EXPECT_NEAR(rt_vec3_x(move), 0.0, 0.0001, "move axis x");
    EXPECT_NEAR(rt_vec3_y(move), inv_sqrt2, 0.0001, "move axis y is normalized");
    EXPECT_NEAR(rt_vec3_z(move), inv_sqrt2, 0.0001, "move axis z is normalized");

    void *look = rt_game3d_input_look_axis(input);
    EXPECT_NEAR(rt_vec2_x(look), 0.60, 0.0001, "look axis x scales mouse delta");
    EXPECT_NEAR(rt_vec2_y(look), -0.20, 0.0001, "look axis y scales mouse delta");
    rt_game3d_input_set_look_sensitivity(input, -2.0);
    EXPECT_NEAR(rt_game3d_input_get_look_sensitivity(input),
                0.01,
                0.0001,
                "negative look sensitivity falls back to default");
    auto *input_view = static_cast<Game3DInputTestLayout *>(input);
    input_view->look_sensitivity = INFINITY;
    EXPECT_NEAR(rt_game3d_input_get_look_sensitivity(input),
                0.01,
                0.0001,
                "look sensitivity getter sanitizes corrupt private state");

    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();
    rt_keyboard_on_key_up(rt_game3d_key_w());
    rt_keyboard_on_key_up(rt_game3d_key_space());
    rt_mouse_button_up(rt_game3d_mouse_left());
    EXPECT_TRUE(rt_game3d_input_released(input, rt_game3d_key_w()) != 0, "W was released");
    PASS();
}

static bool test_input_update_snapshots_frame_state() {
    TEST("Input3D.update snapshots frame state");
    rt_keyboard_init();
    rt_mouse_init();
    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();

    void *input = rt_game3d_input_new();
    rt_game3d_input_set_look_sensitivity(input, 0.1);
    rt_keyboard_on_key_down(rt_game3d_key_w());
    rt_mouse_force_delta(5, -2);
    rt_mouse_update_wheel(0.0, 0.75);
    rt_game3d_input_update(input);

    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();
    rt_keyboard_on_key_up(rt_game3d_key_w());
    rt_mouse_force_delta(100, 100);
    rt_mouse_update_wheel(0.0, 9.0);

    EXPECT_TRUE(rt_game3d_input_is_down(input, rt_game3d_key_w()) != 0,
                "snapshot preserves held key state");
    EXPECT_TRUE(rt_game3d_input_pressed(input, rt_game3d_key_w()) != 0,
                "snapshot preserves pressed edge");
    void *look = rt_game3d_input_look_axis(input);
    EXPECT_NEAR(rt_vec2_x(look), 0.5, 0.0001, "snapshot preserves mouse dx");
    EXPECT_NEAR(rt_vec2_y(look), -0.2, 0.0001, "snapshot preserves mouse dy");
    EXPECT_NEAR(rt_game3d_input_wheel_y(input), 0.75, 0.0001, "snapshot preserves wheel");
    PASS();
}

static bool test_input_repairs_corrupt_snapshots_and_rejects_undersized_handles() {
    TEST("Input3D repairs corrupt snapshots and rejects undersized handles");
    rt_keyboard_init();
    rt_mouse_init();
    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();

    void *input = rt_game3d_input_new();
    auto *view = static_cast<Game3DInputTestLayout *>(input);
    view->look_sensitivity = INFINITY;
    view->has_snapshot = 9;
    view->mouse_dx = INT64_MAX;
    view->mouse_dy = INT64_MIN;
    view->mouse_fdx = INFINITY;
    view->mouse_fdy = -INFINITY;
    view->wheel_y = NAN;
    view->bound_pad = INT64_MAX;
    view->pad_look_sensitivity = INFINITY;
    view->pad_lx = INFINITY;
    view->pad_ly = -INFINITY;
    view->pad_rx = 3.0;
    view->pad_ry = -3.0;
    view->pad_connected = 7;

    EXPECT_EQ_INT(rt_game3d_input_get_pad_bound(input), -1, "invalid pad binding repairs to none");
    EXPECT_NEAR(view->look_sensitivity, 0.01, 0.0001, "look sensitivity repair persists");
    EXPECT_EQ_INT(view->has_snapshot, 1, "snapshot flag repair persists canonically");
    EXPECT_EQ_INT(view->mouse_dx,
                  1000000000000LL,
                  "oversized integer mouse X repair persists at the controller bound");
    EXPECT_EQ_INT(view->mouse_dy,
                  -1000000000000LL,
                  "oversized integer mouse Y repair persists at the controller bound");
    EXPECT_NEAR(view->mouse_fdx, 0.0, 0.0001, "non-finite mouse X repair persists");
    EXPECT_NEAR(view->mouse_fdy, 0.0, 0.0001, "non-finite mouse Y repair persists");
    EXPECT_NEAR(view->wheel_y, 0.0, 0.0001, "non-finite wheel repair persists");
    EXPECT_EQ_INT(view->pad_connected, 0, "invalid binding clears stale connection state");
    EXPECT_NEAR(view->pad_lx, 0.0, 0.0001, "invalid binding clears stale left stick");
    EXPECT_NEAR(view->pad_rx, 0.0, 0.0001, "invalid binding clears stale right stick");
    EXPECT_TRUE(rt_game3d_input_is_down(input, -1) == 0,
                "negative key codes never fall through to live input");
    EXPECT_TRUE(rt_game3d_input_pressed(input, ZANNA_KEY_MAX) == 0,
                "oversized key codes never read past snapshots");
    EXPECT_TRUE(rt_game3d_input_mouse_button(input, -1) == 0,
                "negative mouse buttons never reach the backend");
    EXPECT_TRUE(rt_game3d_input_mouse_pressed(input, ZANNA_MOUSE_BUTTON_MAX) == 0,
                "oversized mouse buttons never read past snapshots");

    rt_game3d_input_bind_pad(input, 0);
    view->pad_connected = 4;
    view->pad_lx = 8.0;
    view->pad_ly = -8.0;
    view->pad_rx = INFINITY;
    view->pad_ry = -INFINITY;
    void *move = rt_game3d_input_move_axis(input);
    void *look = rt_game3d_input_look_axis(input);
    EXPECT_EQ_INT(view->pad_connected, 1, "connected flag is canonicalized");
    EXPECT_NEAR(view->pad_lx, 1.0, 0.0001, "left-stick X clamps to its backend range");
    EXPECT_NEAR(view->pad_ly, -1.0, 0.0001, "left-stick Y clamps to its backend range");
    EXPECT_NEAR(view->pad_rx, 0.0, 0.0001, "non-finite right-stick X repairs to zero");
    EXPECT_NEAR(view->pad_ry, 0.0, 0.0001, "non-finite right-stick Y repairs to zero");
    EXPECT_TRUE(std::isfinite(rt_vec3_x(move)) && std::isfinite(rt_vec3_z(move)),
                "repaired move axes stay finite");
    EXPECT_TRUE(std::isfinite(rt_vec2_x(look)) && std::isfinite(rt_vec2_y(look)),
                "repaired look axes stay finite");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_input_bind_pad(input, ZANNA_PAD_MAX); }, "pad index"),
        "out-of-range pad binding traps");
    EXPECT_EQ_INT(rt_game3d_input_get_pad_bound(input),
                  0,
                  "failed pad binding preserves the previous selection");

    void *undersized = rt_obj_new_i64(RT_G3D_GAME3D_INPUT_CLASS_ID, (int64_t)sizeof(double));
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_input_get_look_sensitivity(undersized); },
                             "invalid input"),
        "class-id spoof with an undersized Input3D payload is rejected");
    if (undersized && rt_obj_release_check0(undersized))
        rt_obj_free(undersized);
    PASS();
}

static bool test_game3d_core_rejects_undersized_class_id_spoofs() {
    TEST("Game3D core rejects undersized class-id spoofs");
    void *entity = rt_obj_new_i64(RT_G3D_GAME3D_ENTITY_CLASS_ID, 1);
    void *world = rt_obj_new_i64(RT_G3D_GAME3D_WORLD_CLASS_ID, 1);
    void *character = rt_obj_new_i64(RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID, 1);
    void *first_person = rt_obj_new_i64(RT_G3D_GAME3D_FIRSTPERSON_CLASS_ID, 1);
    void *free_fly = rt_obj_new_i64(RT_G3D_GAME3D_FREEFLY_CLASS_ID, 1);
    void *orbit = rt_obj_new_i64(RT_G3D_GAME3D_ORBIT_CLASS_ID, 1);
    void *follow = rt_obj_new_i64(RT_G3D_GAME3D_FOLLOW_CLASS_ID, 1);
    void *third_person = rt_obj_new_i64(RT_G3D_GAME3D_THIRDPERSON_CLASS_ID, 1);
    void *rail = rt_obj_new_i64(RT_G3D_GAME3D_RAILCAMERA_CLASS_ID, 1);
    void *layer_mask = rt_obj_new_i64(RT_G3D_GAME3D_LAYERMASK_CLASS_ID, 1);
    void *sound = rt_obj_new_i64(RT_G3D_GAME3D_SOUND_CLASS_ID, 1);
    void *effects = rt_obj_new_i64(RT_G3D_GAME3D_EFFECTS_CLASS_ID, 1);
    void *body_def = rt_obj_new_i64(RT_G3D_GAME3D_BODYDEF_CLASS_ID, 1);
    void *collision_event = rt_obj_new_i64(RT_G3D_GAME3D_COLLISION_EVENT_CLASS_ID, 1);
    void *animator = rt_obj_new_i64(RT_G3D_GAME3D_ANIMATOR3D_CLASS_ID, 1);
    void *model_template = rt_obj_new_i64(RT_G3D_GAME3D_MODEL_TEMPLATE_CLASS_ID, 1);
    void *asset_handle = rt_obj_new_i64(RT_G3D_GAME3D_ASSET_HANDLE3D_CLASS_ID, 1);
    void *world_stream = rt_obj_new_i64(RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID, 1);
    void *lipsync = rt_obj_new_i64(RT_G3D_GAME3D_LIPSYNC_CLASS_ID, 1);
    void *dialogue = rt_obj_new_i64(RT_G3D_GAME3D_DIALOGUE_CLASS_ID, 1);
    void *timeline = rt_obj_new_i64(RT_G3D_GAME3D_TIMELINE_CLASS_ID, 1);
    void *target_lock = rt_obj_new_i64(RT_G3D_GAME3D_TARGETLOCK_CLASS_ID, 1);
    void *canvas = rt_obj_new_i64(RT_G3D_CANVAS3D_CLASS_ID, 1);
    void *camera = rt_obj_new_i64(RT_G3D_CAMERA3D_CLASS_ID, 1);
    void *scene = rt_obj_new_i64(RT_G3D_SCENE3D_CLASS_ID, 1);
    void *scene_node = rt_obj_new_i64(RT_G3D_SCENENODE3D_CLASS_ID, 1);
    void *physics_world = rt_obj_new_i64(RT_G3D_WORLD3D_CLASS_ID, 1);
    void *body = rt_obj_new_i64(RT_G3D_BODY3D_CLASS_ID, 1);
    void *physics_character = rt_obj_new_i64(RT_G3D_CHARACTER3D_CLASS_ID, 1);
    void *trigger = rt_obj_new_i64(RT_G3D_TRIGGER3D_CLASS_ID, 1);
    void *mesh = rt_obj_new_i64(RT_G3D_MESH3D_CLASS_ID, 1);
    void *material = rt_obj_new_i64(RT_G3D_MATERIAL3D_CLASS_ID, 1);
    void *light = rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, 1);
    void *postfx = rt_obj_new_i64(RT_G3D_POSTFX3D_CLASS_ID, 1);
    void *path = rt_obj_new_i64(RT_G3D_PATH3D_CLASS_ID, 1);
    void *live_entity = rt_game3d_entity_new();

    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_entity_get_layer(entity); }, "invalid entity"),
        "undersized Entity3D payload is rejected before field access");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_world_get_frame(world); }, "invalid world"),
        "undersized World3D payload is rejected before field access");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_character_controller_get_speed(character); },
                             "invalid controller"),
        "undersized CharacterController3D payload is rejected");
    EXPECT_TRUE(expect_trap_contains(
                    [&] { (void)rt_game3d_first_person_controller_get_speed(first_person); },
                    "invalid controller"),
                "undersized FirstPersonController payload is rejected");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_free_fly_controller_get_speed(free_fly); },
                             "invalid controller"),
        "undersized FreeFlyController payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_orbit_controller_get_yaw(orbit); },
                                     "invalid controller"),
                "undersized OrbitController payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_follow_controller_get_damping(follow); },
                                     "invalid controller"),
                "undersized FollowController payload is rejected");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_thirdperson_controller_get_yaw(third_person); },
                             "invalid controller"),
        "undersized ThirdPersonController payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_rail_camera_get_progress(rail); },
                                     "invalid rail"),
                "undersized RailCamera3D payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_layermask_get_bits(layer_mask); },
                                     "invalid mask"),
                "undersized LayerMask payload is rejected");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_audio_get_volume(sound); }, "invalid audio"),
        "undersized Sound3D payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_effects_get_count(effects); },
                                     "invalid effects"),
                "undersized EffectRegistry3D payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_body_def_get_mass(body_def); },
                                     "invalid BodyDef"),
                "undersized BodyDef payload is rejected");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_entity_attach_body(live_entity, body_def); },
                             "expected Physics3DBody or BodyDef"),
        "Entity3D.attachBody rejects an undersized BodyDef before reading private fields");
    EXPECT_TRUE(rt_game3d_entity_get_body(live_entity) == nullptr,
                "failed undersized BodyDef attachment preserves the previous empty binding");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_collision_event_get_phase(collision_event); },
                             "invalid event"),
        "undersized Collision3DEvent payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_animator_get_controller(animator); },
                                     "invalid animator"),
                "undersized Animator3D payload is rejected");
    EXPECT_TRUE(expect_trap_contains(
                    [&] { (void)rt_game3d_model_template_get_scene_count(model_template); },
                    "invalid template"),
                "undersized ModelTemplate payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_asset_handle_get_ready(asset_handle); },
                                     "invalid handle"),
                "undersized AssetHandle3D payload is rejected");
    EXPECT_TRUE(expect_trap_contains(
                    [&] { (void)rt_game3d_world_stream_get_resident_cell_count(world_stream); },
                    "invalid stream"),
                "undersized WorldStream3D payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_lipsync_get_driving(lipsync); },
                                     "invalid lip sync"),
                "undersized LipSync3D payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_dialogue_get_active(dialogue); },
                                     "invalid dialogue"),
                "undersized Dialogue3D payload is rejected");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_timeline_get_duration(timeline); },
                                     "invalid timeline"),
                "undersized Timeline3D payload is rejected");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_targetlock_get_max_distance(target_lock); },
                             "invalid lock"),
        "undersized TargetLock3D payload is rejected");

    EXPECT_EQ_INT(rt_canvas3d_get_light_count(canvas),
                  0,
                  "undersized Canvas3D payload returns a neutral light count");
    EXPECT_NEAR(rt_camera3d_get_fov(camera),
                0.0,
                0.0001,
                "undersized Camera3D payload returns a neutral FOV");
    EXPECT_EQ_INT(rt_scene3d_get_node_count(scene),
                  0,
                  "undersized Scene3D payload returns a neutral node count");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_entity_from_node(scene_node); }, "SceneNode3D"),
        "undersized SceneNode3D payload is rejected before FromNode field access");
    EXPECT_EQ_INT(rt_scene_node3d_child_count(scene_node),
                  0,
                  "undersized SceneNode3D payload returns a neutral child count");
    EXPECT_EQ_INT(rt_world3d_get_collision_count(physics_world),
                  0,
                  "undersized Physics3D World payload returns a neutral contact count");
    EXPECT_NEAR(
        rt_body3d_get_mass(body), 0.0, 0.0001, "undersized Body3D payload returns a neutral mass");
    EXPECT_EQ_INT(rt_character3d_is_grounded(physics_character),
                  0,
                  "undersized Character3D payload returns a neutral grounded state");
    EXPECT_EQ_INT(rt_trigger3d_get_enter_count(trigger),
                  0,
                  "undersized Trigger3D payload returns a neutral enter count");
    EXPECT_EQ_INT(rt_mesh3d_get_triangle_count(mesh),
                  0,
                  "undersized Mesh3D payload returns a neutral triangle count");
    EXPECT_NEAR(rt_material3d_get_roughness(material),
                0.5,
                0.0001,
                "undersized Material3D payload returns the documented default roughness");
    EXPECT_NEAR(rt_light3d_get_intensity(light),
                0.0,
                0.0001,
                "undersized Light3D payload returns a neutral intensity");
    EXPECT_EQ_INT(rt_postfx3d_get_effect_count(postfx),
                  0,
                  "undersized PostFX3D payload returns a neutral effect count");
    EXPECT_EQ_INT(rt_path3d_get_point_count(path),
                  0,
                  "undersized Path3D payload returns a neutral point count");

    void *objects[] = {entity,
                       world,
                       character,
                       first_person,
                       free_fly,
                       orbit,
                       follow,
                       third_person,
                       rail,
                       layer_mask,
                       sound,
                       effects,
                       body_def,
                       collision_event,
                       animator,
                       model_template,
                       asset_handle,
                       world_stream,
                       lipsync,
                       dialogue,
                       timeline,
                       target_lock,
                       canvas,
                       camera,
                       scene,
                       scene_node,
                       physics_world,
                       body,
                       physics_character,
                       trigger,
                       mesh,
                       material,
                       light,
                       postfx,
                       path,
                       live_entity};
    for (void *object : objects) {
        if (object && rt_obj_release_check0(object))
            rt_obj_free(object);
    }
    PASS();
}

static bool test_world_entity_registry_and_collision_clear() {
    TEST("World3D owns entities, bodies, names, and collision events");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Unit Registry"), 80, 60);
    EXPECT_TRUE(world != nullptr, "World3D.New returns an object");
    EXPECT_TRUE(rt_game3d_world_get_canvas(world) != nullptr, "World3D has a canvas");
    EXPECT_TRUE(rt_game3d_world_get_camera(world) != nullptr, "World3D has a camera");
    EXPECT_TRUE(rt_game3d_world_get_scene(world) != nullptr, "World3D has a scene");
    EXPECT_TRUE(rt_game3d_world_get_physics(world) != nullptr, "World3D has physics");
    EXPECT_TRUE(rt_game3d_world_get_input(world) != nullptr, "World3D has input");
    EXPECT_TRUE(rt_game3d_world_get_audio(world) != nullptr, "World3D has audio");
    EXPECT_TRUE(rt_game3d_audio_get_listener(rt_game3d_world_get_audio(world)) != nullptr,
                "World3D audio creates a listener");
    EXPECT_TRUE(rt_game3d_world_get_effects(world) != nullptr, "World3D has effects");
    EXPECT_TRUE(rt_game3d_effects_get_postfx(rt_game3d_world_get_effects(world)) != nullptr,
                "World3D effects create post-FX");
    void *owned_stream = rt_game3d_world_get_stream(world);
    EXPECT_TRUE(owned_stream != nullptr, "World3D lazily owns a WorldStream3D");
    EXPECT_TRUE(rt_game3d_world_get_stream(world) == owned_stream,
                "World3D.stream returns a stable owned stream handle");
    EXPECT_EQ_INT(rt_game3d_world_get_entity_count(world), 0, "entityCount starts empty");
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(world), 0, "bodyCount starts empty");
    EXPECT_EQ_INT(rt_game3d_world_get_draw_count(world), 0, "drawCount starts empty");
    EXPECT_EQ_INT(
        rt_game3d_world_get_visible_node_count(world), 0, "visibleNodeCount starts empty");
    EXPECT_EQ_INT(
        rt_game3d_world_get_occluded_draw_count(world), 0, "occludedDrawCount starts empty");
    EXPECT_EQ_INT(
        rt_game3d_world_get_stream_resident_bytes(world), 0, "streamResidentBytes starts empty");

    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *material = rt_material3d_new_color(0.8, 0.2, 0.1);
    void *parent = rt_game3d_entity_of(mesh, material);
    void *child = rt_game3d_entity_new();
    void *body = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
    void *other_body = rt_body3d_new_aabb(1.0, 1.0, 1.0, 0.0);
    void *other = rt_game3d_entity_new();

    rt_game3d_entity_set_name(parent, rt_const_cstr("Player"));
    rt_game3d_entity_set_name(child, rt_const_cstr("Muzzle"));
    rt_game3d_entity_set_name(other, rt_const_cstr("Wall"));
    rt_game3d_entity_add_child(parent, child);
    rt_game3d_entity_set_layer(parent, rt_game3d_layers_player());
    rt_game3d_entity_set_collision_mask(parent, rt_game3d_layermask_of(rt_game3d_layers_world()));
    rt_game3d_entity_attach_body(parent, body);
    rt_game3d_entity_set_layer(other, rt_game3d_layers_world());
    rt_game3d_entity_attach_body(other, other_body);
    rt_game3d_entity_set_position(parent, 0.5, 0.0, 0.0);
    rt_game3d_entity_set_position(other, 0.0, 0.0, 0.0);

    EXPECT_TRUE(rt_game3d_entity_is_group(parent) != 0, "Entity3D.addChild marks group");
    rt_game3d_world_spawn(world, parent);
    rt_game3d_world_spawn(world, other);

    EXPECT_EQ_INT(rt_game3d_entity_get_id(parent), 1, "spawn assigns parent id");
    EXPECT_EQ_INT(rt_game3d_entity_get_id(child), 2, "spawn assigns child id");
    EXPECT_TRUE(rt_game3d_entity_is_spawned(parent) != 0, "parent is spawned");
    EXPECT_TRUE(rt_game3d_entity_is_spawned(child) != 0, "child is spawned");
    EXPECT_EQ_INT(
        rt_game3d_world_get_entity_count(world), 3, "entityCount tracks spawned entity tree");
    EXPECT_EQ_INT(
        rt_game3d_world_get_body_count(world), 2, "bodyCount tracks spawned entity bodies");
    EXPECT_EQ_INT(rt_world3d_body_count(rt_game3d_world_get_physics(world)),
                  2,
                  "spawn registers entity bodies with physics");
    EXPECT_EQ_INT(rt_body3d_get_collision_layer(body),
                  rt_game3d_layers_player(),
                  "entity layer propagates to body");
    EXPECT_EQ_INT(rt_body3d_get_collision_mask(body),
                  rt_game3d_layers_world(),
                  "entity collision mask propagates to body");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("Muzzle")) ==
                    rt_game3d_entity_get_node(child),
                "findNode sees child scene node");
    void *muzzle_option = rt_game3d_world_find_node_option(world, rt_const_cstr("Muzzle"));
    EXPECT_TRUE(rt_option_is_some(muzzle_option) == 1,
                "World3D.FindNodeOption returns Some for child scene node");
    EXPECT_TRUE(rt_option_unwrap(muzzle_option) == rt_game3d_entity_get_node(child),
                "World3D.FindNodeOption unwraps child scene node");
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("Player")) == parent,
                "findEntity returns registered entity");
    void *player_option = rt_game3d_world_find_entity_option(world, rt_const_cstr("Player"));
    EXPECT_TRUE(rt_option_is_some(player_option) == 1,
                "World3D.FindEntityOption returns Some for registered entity");
    EXPECT_TRUE(rt_option_unwrap(player_option) == parent,
                "World3D.FindEntityOption unwraps registered entity");
    rt_game3d_entity_set_name(parent, rt_const_cstr("Hero"));
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("Hero")) == parent,
                "findEntity index updates after rename");
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("Player")) == nullptr,
                "findEntity index drops old names after rename");
    EXPECT_TRUE(
        rt_option_is_none(rt_game3d_world_find_entity_option(world, rt_const_cstr("Player"))) == 1,
        "World3D.FindEntityOption returns None for stale entity names");
    rt_game3d_world_on_resize(world, 96, 72);
    EXPECT_EQ_INT(rt_canvas3d_get_width(rt_game3d_world_get_canvas(world)),
                  96,
                  "World3D.onResize resizes the canvas width");
    EXPECT_EQ_INT(rt_canvas3d_get_height(rt_game3d_world_get_canvas(world)),
                  72,
                  "World3D.onResize resizes the canvas height");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_world_add_light(world, 0, parent); }, "Light3D"),
        "World3D.addLight rejects non-Light3D handles");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_world_set_skybox(world, parent); }, "CubeMap3D"),
        "World3D.setSkybox rejects non-CubeMap3D handles");
    void *sky_px = rt_pixels_new(1, 1);
    rt_pixels_set(sky_px, 0, 0, 0xFFFFFFFF);
    void *bad_skybox = rt_cubemap3d_new(sky_px, sky_px, sky_px, sky_px, sky_px, sky_px);
    ((rt_cubemap3d *)bad_skybox)->face_size = 2;
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_world_set_skybox(world, bad_skybox); }, "CubeMap3D"),
        "World3D.setSkybox rejects incomplete CubeMap3D handles");
    EXPECT_TRUE(((rt_canvas3d *)rt_game3d_world_get_canvas(world))->skybox == nullptr,
                "World3D.setSkybox leaves canvas skybox clear after rejecting incomplete cubemaps");
    void *good_skybox = rt_cubemap3d_new(sky_px, sky_px, sky_px, sky_px, sky_px, sky_px);
    auto *canvas_state = (rt_canvas3d *)rt_game3d_world_get_canvas(world);
    EXPECT_TRUE(good_skybox != nullptr && canvas_state != nullptr,
                "World3D skybox repair fixture exists");
    if (good_skybox && canvas_state) {
        rt_game3d_world_set_skybox(world, good_skybox);
        EXPECT_TRUE(canvas_state->skybox == (rt_cubemap3d *)good_skybox,
                    "World3D.setSkybox binds a valid cubemap");
        canvas_state->skybox_cpu_cache = (uint8_t *)malloc(4);
        canvas_state->skybox_cpu_cache_w = 1;
        canvas_state->skybox_cpu_cache_h = 1;
        canvas_state->skybox_cpu_cache_generation = 7;
        ((rt_cubemap3d *)good_skybox)->face_size = 2;
        EXPECT_TRUE(
            expect_trap_contains([&] { rt_game3d_world_set_skybox(world, parent); }, "CubeMap3D"),
            "World3D.setSkybox still rejects non-CubeMap3D replacements");
        EXPECT_TRUE(canvas_state->skybox == nullptr,
                    "World3D.setSkybox repairs a stale bound canvas skybox before rejecting");
        EXPECT_TRUE(canvas_state->skybox_cpu_cache == nullptr &&
                        canvas_state->skybox_cpu_cache_generation == 0,
                    "World3D.setSkybox invalidates stale skybox CPU cache during repair");
    }
    EXPECT_EQ_INT(rt_scene3d_get_node_count(rt_game3d_world_get_scene(world)),
                  4,
                  "scene contains root, parent, child, and wall");

    rt_game3d_world_begin_frame(world);
    rt_game3d_world_draw_scene(world);
    rt_game3d_world_end_scene(world);
    EXPECT_EQ_INT(rt_game3d_world_get_draw_count(world),
                  rt_canvas3d_get_draw_count(rt_game3d_world_get_canvas(world)),
                  "drawCount wraps canvas telemetry");
    EXPECT_TRUE(rt_game3d_world_get_draw_count(world) >= 1, "drawCount observes scene draws");
    EXPECT_EQ_INT(rt_game3d_world_get_visible_node_count(world),
                  rt_scene3d_get_visible_node_count(rt_game3d_world_get_scene(world)),
                  "visibleNodeCount wraps scene telemetry");
    EXPECT_EQ_INT(rt_game3d_world_get_occluded_draw_count(world),
                  rt_canvas3d_get_occluded_draw_count(rt_game3d_world_get_canvas(world)),
                  "occludedDrawCount wraps canvas telemetry");

    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_world_collision_event_count(world, rt_game3d_collision_any()) > 0,
                "overlapping spawned bodies emit collisions");
    EXPECT_TRUE(rt_game3d_world_collision_event(world, rt_game3d_collision_any(), 0) != nullptr,
                "collisionEvent returns a boxed event");
    rt_game3d_world_clear_collision_events(world);
    EXPECT_EQ_INT(rt_game3d_world_collision_event_count(world, rt_game3d_collision_any()),
                  0,
                  "clearCollisionEvents clears live contacts");
    EXPECT_EQ_INT(rt_game3d_world_collision_event_count(world, rt_game3d_collision_enter()),
                  0,
                  "clearCollisionEvents clears enter events");

    rt_game3d_diagnostics_reset();
    rt_game3d_entity_set_position(parent, 3.0, 4.0, 5.0);
    void *live_pos = rt_game3d_entity_position(parent);
    EXPECT_NEAR(rt_vec3_x(live_pos), 3.0, 0.0001, "live entity position getter returns X");
    EXPECT_NEAR(rt_vec3_y(live_pos), 4.0, 0.0001, "live entity position getter returns Y");
    EXPECT_NEAR(rt_vec3_z(live_pos), 5.0, 0.0001, "live entity position getter returns Z");
    rt_game3d_entity_attach_animator(parent, nullptr);
    EXPECT_EQ_INT(rt_game3d_diagnostics_get_stale_entity_calls(),
                  0,
                  "live entity calls do not increment stale diagnostics");

    rt_game3d_world_despawn(world, parent);
    EXPECT_TRUE(rt_game3d_entity_is_spawned(parent) == 0, "despawn clears parent spawned flag");
    EXPECT_TRUE(rt_game3d_entity_is_spawned(child) == 0, "despawn clears child spawned flag");
    EXPECT_TRUE(rt_game3d_entity_is_destroyed(parent) != 0, "despawn marks parent stale");
    EXPECT_TRUE(rt_game3d_entity_is_destroyed(child) != 0, "despawn marks child stale");
    EXPECT_EQ_INT(
        rt_game3d_world_get_entity_count(world), 1, "entityCount drops despawned subtree");
    EXPECT_EQ_INT(
        rt_game3d_world_get_body_count(world), 1, "bodyCount drops despawned subtree body");
    EXPECT_EQ_INT(rt_world3d_body_count(rt_game3d_world_get_physics(world)),
                  1,
                  "despawn removes entity body from physics");
    EXPECT_TRUE(rt_world3d_contains_body(rt_game3d_world_get_physics(world), body) == 0,
                "despawn removes parent body from physics membership");
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("Hero")) == nullptr,
                "findEntity ignores despawned entities after lazy name-index rebuild");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("Muzzle")) == nullptr,
                "findNode ignores despawned child nodes");
    EXPECT_EQ_INT(rt_scene3d_get_node_count(rt_game3d_world_get_scene(world)),
                  2,
                  "despawn detaches the parent subtree from the scene");

    rt_game3d_diagnostics_reset();
    void *despawned_pos = rt_game3d_entity_position(parent);
    EXPECT_NEAR(rt_vec3_x(despawned_pos), 0.0, 0.0001, "despawned entity position is neutral X");
    EXPECT_NEAR(rt_vec3_y(despawned_pos), 0.0, 0.0001, "despawned entity position is neutral Y");
    EXPECT_NEAR(rt_vec3_z(despawned_pos), 0.0, 0.0001, "despawned entity position is neutral Z");
    rt_game3d_entity_set_position(parent, 10.0, 20.0, 30.0);
    rt_game3d_entity_attach_animator(parent, nullptr);
    EXPECT_EQ_INT(rt_game3d_diagnostics_get_stale_entity_calls(),
                  3,
                  "despawned entity getter, mutator, and animator calls are counted no-ops");

    rt_game3d_world_destroy(world);
    EXPECT_TRUE(rt_game3d_world_is_destroyed(world) != 0, "destroy marks world destroyed");
    EXPECT_TRUE(rt_game3d_entity_is_destroyed(other) != 0,
                "destroy marks still-spawned entities destroyed");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_world_get_frame(world); }, "world is destroyed"),
        "destroyed world handles trap with a clear diagnostic");
    g_returning_trap_count = 0;
    g_return_trap = true;
    int64_t destroyed_frame = rt_game3d_world_get_frame(world);
    g_return_trap = false;
    EXPECT_EQ_INT(
        g_returning_trap_count, 1, "destroyed world getter reports exactly one returning trap");
    EXPECT_EQ_INT(destroyed_frame,
                  0,
                  "destroyed world getter does not dereference state after a returning trap");
    rt_game3d_diagnostics_reset();
    void *destroyed_pos = rt_game3d_entity_position(other);
    EXPECT_NEAR(
        rt_vec3_x(destroyed_pos), 0.0, 0.0001, "world-destroyed entity position is neutral X");
    EXPECT_NEAR(
        rt_vec3_y(destroyed_pos), 0.0, 0.0001, "world-destroyed entity position is neutral Y");
    EXPECT_NEAR(
        rt_vec3_z(destroyed_pos), 0.0, 0.0001, "world-destroyed entity position is neutral Z");
    rt_game3d_entity_set_position(other, 11.0, 22.0, 33.0);
    rt_game3d_entity_attach_animator(other, nullptr);
    EXPECT_EQ_INT(rt_game3d_diagnostics_get_stale_entity_calls(),
                  3,
                  "world-destroyed entity calls are counted no-ops");
    rt_game3d_diagnostics_reset();
    PASS();
}

static bool test_world_body_index_deletion_tombstones_and_duplicate_names() {
    TEST("World3D body/name indices survive removals and duplicate names");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Index Regression"), 80, 60);
    EXPECT_TRUE(world != nullptr, "World3D.New returns an object");
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);

    void *first_named = rt_game3d_entity_new();
    void *second_named = rt_game3d_entity_new();
    rt_game3d_entity_set_name(first_named, rt_const_cstr("SharedName"));
    rt_game3d_entity_set_name(second_named, rt_const_cstr("SharedName"));
    rt_game3d_world_spawn(world, first_named);
    rt_game3d_world_spawn(world, second_named);
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("SharedName")) == first_named,
                "duplicate entity names use first-spawned lookup policy");
    rt_game3d_world_despawn(world, first_named);
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("SharedName")) == second_named,
                "despawning the first duplicate promotes the remaining entity");

    constexpr int kBodyCount = 10;
    void *entities[kBodyCount] = {};
    void *bodies[kBodyCount] = {};
    bool alive[kBodyCount] = {};
    for (int i = 0; i < kBodyCount; ++i) {
        entities[i] = rt_game3d_entity_new();
        bodies[i] = rt_body3d_new_aabb(1.0, 1.0, 1.0, 1.0);
        rt_game3d_entity_attach_body(entities[i], bodies[i]);
        rt_game3d_entity_set_position(entities[i], 0.0, 0.0, 0.0);
        rt_game3d_world_spawn(world, entities[i]);
        alive[i] = true;
    }
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_world_collision_event_count(world, rt_game3d_collision_any()) > 0,
                "overlapping index-regression bodies collide before removals");
    rt_game3d_world_clear_collision_events(world);

    for (int i = 1; i < kBodyCount; i += 2) {
        rt_game3d_world_despawn(world, entities[i]);
        alive[i] = false;
    }
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(world),
                  kBodyCount / 2,
                  "body index count drops removed open-addressing entries");

    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    int64_t event_count = rt_game3d_world_collision_event_count(world, rt_game3d_collision_any());
    EXPECT_TRUE(event_count > 0, "surviving bodies still collide after clustered removals");
    for (int64_t ei = 0; ei < event_count; ++ei) {
        void *event = rt_game3d_world_collision_event(world, rt_game3d_collision_any(), ei);
        void *a = rt_game3d_collision_event_get_a(event);
        void *b = rt_game3d_collision_event_get_b(event);
        bool a_alive = false;
        bool b_alive = false;
        for (int i = 0; i < kBodyCount; ++i) {
            if (!alive[i])
                continue;
            if (a == entities[i])
                a_alive = true;
            if (b == entities[i])
                b_alive = true;
        }
        EXPECT_TRUE(a_alive && b_alive,
                    "body-index deletion keeps collision events mapped to surviving entities");
    }

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_world_navmesh_bake_hooks() {
    TEST("World3D exposes world-scoped navmesh bake hooks");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Nav Bake Unit"), 80, 60);
    EXPECT_TRUE(world != nullptr, "World3D.New returns an object");

    void *mesh = rt_mesh3d_new_plane(8.0, 8.0);
    void *material = rt_material3d_new_color(0.2, 0.6, 0.25);
    void *ground = rt_game3d_entity_of(mesh, material);
    rt_game3d_entity_set_name(ground, rt_const_cstr("NavBakeGround"));
    rt_game3d_entity_set_position(ground, 3.0, 0.0, -2.0);
    rt_game3d_world_spawn(world, ground);

    void *nav = rt_game3d_world_bake_nav_mesh(world, 0.35, 1.8, 45.0, 0.3);
    EXPECT_TRUE(nav != nullptr, "bakeNavMesh returns a NavMesh3D");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(nav) > 100,
                "bakeNavMesh voxelizes the world scene into a navmesh grid");
    void *inside = rt_vec3_new(3.0, 0.0, -2.0);
    EXPECT_TRUE(rt_navmesh3d_is_walkable(nav, inside) != 0,
                "bakeNavMesh preserves world-space transforms");

    void *tiled = rt_game3d_world_bake_tiled_nav_mesh(world, 16.0, 0.35, 1.8, 45.0, 0.3);
    EXPECT_TRUE(tiled != nullptr, "bakeTiledNavMesh returns a NavMesh3D");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(tiled) > 100,
                "bakeTiledNavMesh voxelizes the world scene into a navmesh grid");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(tiled, inside) != 0,
                "bakeTiledNavMesh preserves world-space transforms");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_entity_from_node_wraps_imported_subtree() {
    TEST("Entity3D.FromNode wraps a raw SceneNode subtree");
    void *root = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *grandchild = rt_scene_node3d_new();
    void *mesh_a = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mesh_b = rt_mesh3d_new_sphere(0.5, 8);
    void *material_a = rt_material3d_new_color(0.8, 0.2, 0.1);
    void *material_b = rt_material3d_new_color(0.1, 0.5, 0.9);
    rt_scene_node3d_set_name(root, rt_const_cstr("ImportedRoot"));
    rt_scene_node3d_set_name(child, rt_const_cstr("ImportedChild"));
    rt_scene_node3d_set_name(grandchild, rt_const_cstr("ImportedGrandchild"));
    rt_scene_node3d_set_mesh(child, mesh_a);
    rt_scene_node3d_set_material(child, material_a);
    rt_scene_node3d_set_mesh(grandchild, mesh_a);
    rt_scene_node3d_set_material(grandchild, material_a);
    rt_scene_node3d_add_child(child, grandchild);
    rt_scene_node3d_add_child(root, child);

    void *entity = rt_game3d_entity_from_node(root);
    EXPECT_TRUE(entity != nullptr, "FromNode returns an Entity3D");
    EXPECT_TRUE(rt_game3d_entity_is_group(entity) != 0, "FromNode marks the wrapper as a group");
    EXPECT_TRUE(rt_game3d_entity_get_node(entity) == root, "FromNode uses the provided root node");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_entity_get_name(entity)), "ImportedRoot") == 0,
                "FromNode inherits a non-empty root node name");
    rt_game3d_entity_set_material_recursive(entity, material_b);
    EXPECT_TRUE(rt_scene_node3d_get_material(root) == material_b,
                "setMaterialRecursive applies to the imported root node");
    EXPECT_TRUE(rt_scene_node3d_get_material(child) == material_b,
                "setMaterialRecursive applies to imported child nodes");
    EXPECT_TRUE(rt_scene_node3d_get_material(grandchild) == material_b,
                "setMaterialRecursive applies to imported grandchildren");
    rt_game3d_entity_set_mesh_recursive(entity, mesh_b);
    EXPECT_TRUE(rt_scene_node3d_get_mesh(root) == nullptr,
                "setMeshRecursive leaves non-renderable imported group roots untouched");
    EXPECT_TRUE(rt_scene_node3d_get_mesh(child) == mesh_b,
                "setMeshRecursive updates drawable imported children");
    EXPECT_TRUE(rt_scene_node3d_get_mesh(grandchild) == mesh_b,
                "setMeshRecursive updates drawable imported grandchildren");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D FromNode Unit"), 80, 60);
    rt_game3d_world_spawn(world, entity);
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("ImportedRoot")) == entity,
                "findEntity indexes the imported root wrapper by name");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("ImportedRoot")) == root,
                "findNode sees the imported root node");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("ImportedChild")) == child,
                "findNode sees raw children in the imported subtree");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("ImportedGrandchild")) == grandchild,
                "findNode sees deeper raw children in the imported subtree");
    EXPECT_EQ_INT(rt_scene3d_get_node_count(rt_game3d_world_get_scene(world)),
                  4,
                  "scene contains world root plus imported raw subtree");

    rt_game3d_world_despawn(world, entity);
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("ImportedRoot")) == nullptr,
                "despawn removes imported wrapper from the entity index");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("ImportedChild")) == nullptr,
                "despawn detaches the imported raw subtree");

    void *source_scene = rt_scene3d_new();
    void *source_root = rt_scene3d_get_root(source_scene);
    void *source_child = rt_scene_node3d_new();
    rt_scene_node3d_set_name(source_root, rt_const_cstr("LoadedSceneRoot"));
    rt_scene_node3d_set_name(source_child, rt_const_cstr("LoadedSceneChild"));
    rt_scene_node3d_set_mesh(source_child, mesh_a);
    rt_scene_node3d_add_child(source_root, source_child);
    EXPECT_EQ_INT(rt_scene3d_get_node_count(source_scene),
                  2,
                  "loaded source scene initially owns its complete hierarchy");

    void *loaded_entity = rt_game3d_entity_from_node(source_root);
    void *replacement_root = rt_scene3d_get_root(source_scene);
    EXPECT_TRUE(loaded_entity != nullptr, "FromNode wraps an implicit scene root");
    EXPECT_TRUE(rt_game3d_entity_get_node(loaded_entity) == source_root,
                "FromNode transfers the original scene root into the entity");
    EXPECT_TRUE(replacement_root != nullptr && replacement_root != source_root,
                "source scene receives a distinct replacement root");
    EXPECT_TRUE(scene_node3d_checked(replacement_root)->owner_scene ==
                    scene3d_checked(source_scene),
                "replacement root remains owned by the source scene");
    EXPECT_TRUE(scene_node3d_checked(source_root)->owner_scene == nullptr,
                "transferred root is detached before world spawn");
    EXPECT_TRUE(scene_node3d_checked(source_child)->owner_scene == nullptr,
                "transferred descendants are detached before world spawn");
    EXPECT_EQ_INT(rt_scene3d_get_node_count(source_scene),
                  1,
                  "source scene remains valid and empty after root transfer");

    rt_game3d_world_spawn(world, loaded_entity);
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("LoadedSceneRoot")) ==
                    loaded_entity,
                "transferred scene-root entity spawns successfully");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("LoadedSceneChild")) == source_child,
                "transferred source hierarchy remains intact after spawn");
    EXPECT_EQ_INT(rt_scene3d_get_node_count(rt_game3d_world_get_scene(world)),
                  3,
                  "world owns its implicit root plus the transferred two-node hierarchy");
    EXPECT_EQ_INT(rt_scene3d_get_node_count(source_scene),
                  1,
                  "source scene remains independent after its former root is spawned");
    rt_game3d_world_despawn(world, loaded_entity);

    void *transaction_scene = rt_scene3d_new();
    void *transaction_root = rt_scene3d_get_root(transaction_scene);
    void *last_transaction_child = nullptr;
    for (int i = 0; i < 80; ++i) {
        last_transaction_child = rt_scene_node3d_new();
        rt_scene_node3d_add_child(transaction_root, last_transaction_child);
    }
    EXPECT_EQ_INT(rt_scene3d_get_node_count(transaction_scene),
                  81,
                  "owner-transaction fixture exceeds the inline growth quantum");
    rt_scene3d_test_set_owner_growth_failure(1);
    bool transfer_trapped = expect_trap_contains(
        [&] { (void)rt_game3d_entity_from_node(transaction_root); }, "owner traversal");
    rt_scene3d_test_set_owner_growth_failure(-1);
    EXPECT_TRUE(transfer_trapped, "scene-root transfer reports owner preflight allocation failure");
    EXPECT_TRUE(rt_scene3d_get_root(transaction_scene) == transaction_root,
                "failed transfer preserves the original implicit root");
    EXPECT_TRUE(scene_node3d_checked(transaction_root)->owner_scene ==
                    scene3d_checked(transaction_scene),
                "failed transfer preserves root ownership");
    EXPECT_TRUE(scene_node3d_checked(last_transaction_child)->owner_scene ==
                    scene3d_checked(transaction_scene),
                "failed transfer preserves descendant ownership");
    EXPECT_EQ_INT(rt_scene3d_get_node_count(transaction_scene),
                  81,
                  "failed transfer preserves the complete source hierarchy");

    void *not_node = rt_material3d_new_color(1.0, 1.0, 1.0);
    EXPECT_TRUE(expect_trap_contains([&] { rt_game3d_entity_from_node(not_node); }, "SceneNode"),
                "FromNode rejects non-SceneNode handles");
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_entity_private_slots_reject_wrong_class_refs() {
    TEST("Entity3D ignores wrong-class private node, mesh, material, body, and anim slots");
    void *mesh_a = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mesh_b = rt_mesh3d_new_sphere(0.5, 8);
    void *material_a = rt_material3d_new_color(0.7, 0.2, 0.2);
    void *material_b = rt_material3d_new_color(0.2, 0.5, 0.9);
    void *entity = rt_game3d_entity_of(mesh_a, material_a);
    auto *view = static_cast<Game3DEntityTestLayout *>(entity);

    view->id = -42;
    EXPECT_EQ_INT(
        rt_game3d_entity_get_id(entity), 0, "negative corrupt entity ids read back as unassigned");
    view->id = 0;
    view->layer = 0;
    EXPECT_EQ_INT(rt_game3d_entity_get_layer(entity),
                  rt_game3d_layers_dynamic(),
                  "corrupt entity layers read back as Dynamic");
    view->layer = rt_game3d_layers_dynamic();
    rt_string saved_name = view->name;
    view->name = reinterpret_cast<rt_string>(material_b);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_entity_get_name(entity)), "") == 0,
                "Entity3D.GetName rejects wrong-class private string slots");
    rt_string repaired_name = view->name;
    view->name = saved_name;
    rt_string_unref(repaired_name);

    size_t material_b_ref = rt_heap_hdr(material_b)->refcnt;
    view->node = material_b;
    EXPECT_TRUE(rt_game3d_entity_get_node(entity) == nullptr,
                "wrong-class entity node getter returns null");
    rt_game3d_entity_set_position(entity, 1.0, 2.0, 3.0);
    rt_game3d_entity_set_name(entity, rt_const_cstr("CorruptEntityNode"));
    EXPECT_TRUE(rt_heap_hdr(material_b)->refcnt == material_b_ref,
                "node repair path does not release the wrong-class handle");

    view->mesh = material_b;
    material_b_ref = rt_heap_hdr(material_b)->refcnt;
    EXPECT_TRUE(rt_game3d_entity_get_mesh(entity) == nullptr,
                "wrong-class entity mesh getter returns null");
    rt_game3d_entity_set_mesh(entity, mesh_b);
    EXPECT_TRUE(rt_game3d_entity_get_mesh(entity) == mesh_b,
                "setMesh replaces a wrong-class private mesh slot");
    EXPECT_TRUE(rt_heap_hdr(material_b)->refcnt == material_b_ref,
                "setMesh does not release the wrong-class mesh-slot handle");

    view->material = mesh_a;
    size_t mesh_a_ref = rt_heap_hdr(mesh_a)->refcnt;
    EXPECT_TRUE(rt_game3d_entity_get_material(entity) == nullptr,
                "wrong-class entity material getter returns null");
    rt_game3d_entity_set_material(entity, material_b);
    EXPECT_TRUE(rt_game3d_entity_get_material(entity) == material_b,
                "setMaterial replaces a wrong-class private material slot");
    EXPECT_TRUE(rt_heap_hdr(mesh_a)->refcnt == mesh_a_ref,
                "setMaterial does not release the wrong-class material-slot handle");

    view->body = material_a;
    size_t material_a_ref = rt_heap_hdr(material_a)->refcnt;
    EXPECT_TRUE(rt_game3d_entity_get_body(entity) == nullptr,
                "wrong-class entity body getter returns null");
    rt_game3d_entity_set_layer(entity, rt_game3d_layers_player());
    rt_game3d_entity_set_collision_mask(entity, rt_game3d_layermask_of(rt_game3d_layers_world()));
    EXPECT_TRUE(rt_heap_hdr(material_a)->refcnt == material_a_ref,
                "body property propagation ignores a wrong-class private body");
    void *body = rt_body3d_new(1.0);
    rt_game3d_entity_attach_body(entity, body);
    EXPECT_TRUE(rt_game3d_entity_get_body(entity) == body,
                "attachBody replaces a wrong-class private body slot");
    EXPECT_TRUE(rt_heap_hdr(material_a)->refcnt == material_a_ref,
                "attachBody does not release the wrong-class body-slot handle");

    view->anim = material_a;
    material_a_ref = rt_heap_hdr(material_a)->refcnt;
    EXPECT_TRUE(rt_game3d_entity_get_anim(entity) == nullptr,
                "wrong-class entity animator getter returns null");
    void *controller = make_game3d_test_controller(2.0, 0.25);
    void *animator = rt_game3d_animator_new(controller);
    rt_game3d_entity_attach_animator(entity, animator);
    EXPECT_TRUE(rt_game3d_entity_get_anim(entity) == animator,
                "attachAnimator replaces a wrong-class private animator slot");
    EXPECT_TRUE(rt_heap_hdr(material_a)->refcnt == material_a_ref,
                "attachAnimator does not release the wrong-class animator-slot handle");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Corrupt Entity Slot Unit"), 80, 60);
    rt_game3d_world_spawn(world, entity);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(world),
                  1,
                  "World3D.spawn indexes only the valid repaired body");
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_animator_private_controller_rejects_wrong_class_refs() {
    TEST("Animator3D ignores wrong-class private controller slots");
    void *controller = make_game3d_test_controller(3.0, 0.25);
    void *animator = rt_game3d_animator_new(controller);
    auto *view = static_cast<Game3DAnimatorTestLayout *>(animator);
    void *wrong_controller = rt_material3d_new_color(0.9, 0.9, 0.1);
    size_t wrong_ref = rt_heap_hdr(wrong_controller)->refcnt;

    EXPECT_TRUE(rt_game3d_animator_play(animator, rt_const_cstr("walk")) != 0,
                "Animator3D.play captures valid controller events before corruption");
    rt_game3d_animator_update(animator, 0.25);
    EXPECT_EQ_INT(rt_game3d_animator_event_count(animator),
                  1,
                  "Animator3D has a stale event to clear after controller corruption");

    view->controller = wrong_controller;
    EXPECT_TRUE(rt_game3d_animator_get_controller(animator) == nullptr,
                "wrong-class Animator3D controller getter returns null");
    EXPECT_TRUE(rt_game3d_animator_play(animator, rt_const_cstr("walk")) == 0,
                "Animator3D.play ignores a wrong-class private controller");
    EXPECT_EQ_INT(rt_game3d_animator_event_count(animator),
                  0,
                  "Animator3D.play clears stale events when the controller slot is invalid");
    EXPECT_TRUE(rt_game3d_animator_crossfade(animator, rt_const_cstr("idle"), 0.1) == 0,
                "Animator3D.crossfade ignores a wrong-class private controller");
    rt_game3d_animator_set_speed(animator, rt_const_cstr("walk"), 2.0);
    rt_game3d_animator_update(animator, 0.25);
    EXPECT_EQ_INT(rt_game3d_animator_event_count(animator),
                  0,
                  "Animator3D.update leaves events empty without a valid controller");
    EXPECT_TRUE(rt_heap_hdr(wrong_controller)->refcnt == wrong_ref,
                "Animator3D operations do not release the wrong-class controller-slot handle");
    PASS();
}

static bool test_animator_private_event_slots_reject_wrong_class_refs() {
    TEST("Animator3D repairs wrong-class private event slots");
    void *controller = make_game3d_test_controller(3.0, 0.25);
    void *animator = rt_game3d_animator_new(controller);
    auto *view = static_cast<Game3DAnimatorTestLayout *>(animator);
    void *wrong_event = rt_material3d_new_color(0.1, 0.7, 0.2);
    size_t wrong_ref = rt_heap_hdr(wrong_event)->refcnt;

    view->events[0] = reinterpret_cast<rt_string>(wrong_event);
    view->event_count = 1;

    EXPECT_EQ_INT(rt_game3d_animator_event_count(animator),
                  0,
                  "Animator3D.eventCount excludes wrong-class event handles");
    EXPECT_TRUE(view->events[0] == nullptr,
                "Animator3D.eventCount clears wrong-class private event slots");
    EXPECT_TRUE(rt_heap_hdr(wrong_event)->refcnt == wrong_ref,
                "Animator3D.eventCount does not release the wrong-class event-slot handle");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_animator_event_name(animator, 0)), "") == 0,
                "Animator3D.eventName returns empty after event-slot repair");
    EXPECT_TRUE(rt_heap_hdr(wrong_event)->refcnt == wrong_ref,
                "Animator3D.eventName does not release the wrong-class event-slot handle");
    PASS();
}

static bool test_entity_child_graph_reparents_and_rejects_cycles() {
    TEST("Entity3D child graph reparents and rejects cycles");
    void *parent_a = rt_game3d_entity_new();
    void *parent_b = rt_game3d_entity_new();
    void *child = rt_game3d_entity_new();

    rt_game3d_entity_add_child(parent_a, child);
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(parent_a)),
                  1,
                  "first parent receives child node");
    rt_game3d_entity_add_child(parent_a, child);
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(parent_a)),
                  1,
                  "adding same child twice is idempotent");

    rt_game3d_entity_add_child(parent_b, child);
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(parent_a)),
                  0,
                  "reparent removes child from old parent node");
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(parent_b)),
                  1,
                  "reparent adds child to new parent node");

    EXPECT_TRUE(expect_trap_contains([&] { rt_game3d_entity_add_child(child, parent_b); }, "cycle"),
                "child graph rejects cycles");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_entity_add_child(child, child); }, "own child"),
        "child graph rejects self-parenting");
    PASS();
}

static bool test_entity_child_count_repair_bounds_tree_walks() {
    TEST("Entity3D repairs corrupt child counts before hierarchy traversal");
    void *ungrouped = rt_game3d_entity_new();
    Game3DEntityTestLayout *ungrouped_layout = static_cast<Game3DEntityTestLayout *>(ungrouped);
    ungrouped_layout->children = nullptr;
    ungrouped_layout->child_count = INT32_MAX;
    ungrouped_layout->child_capacity = 0;
    ungrouped_layout->group = 0;
    EXPECT_TRUE(rt_game3d_entity_is_group(ungrouped) == 0,
                "isGroup ignores corrupt child counts without a backing array");

    void *parent = rt_game3d_entity_new();
    void *first = rt_game3d_entity_new();
    void *second = rt_game3d_entity_new();
    void *third = rt_game3d_entity_new();
    void *fourth = rt_game3d_entity_new();
    rt_game3d_entity_set_name(parent, rt_const_cstr("CorruptCountParent"));
    rt_game3d_entity_set_name(first, rt_const_cstr("CorruptCountFirst"));
    rt_game3d_entity_set_name(second, rt_const_cstr("CorruptCountSecond"));
    rt_game3d_entity_set_name(third, rt_const_cstr("CorruptCountThird"));
    rt_game3d_entity_set_name(fourth, rt_const_cstr("CorruptCountFourth"));

    rt_game3d_entity_add_child(parent, first);
    Game3DEntityTestLayout *parent_layout = static_cast<Game3DEntityTestLayout *>(parent);
    parent_layout->child_count = INT32_MAX;
    parent_layout->child_capacity = 1;
    EXPECT_TRUE(rt_game3d_entity_is_group(parent) != 0,
                "isGroup clamps corrupt child count to allocated child slots");

    rt_game3d_entity_add_child(parent, second);
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(parent)),
                  2,
                  "addChild repairs corrupt child count before appending");

    parent_layout->child_count = 0;
    parent_layout->child_capacity = 0;
    rt_game3d_entity_add_child(parent, third);
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(parent)),
                  3,
                  "addChild recovers an underreported dense child prefix");

    parent_layout->child_count = INT32_MAX;
    parent_layout->child_capacity = INT32_MAX;
    rt_game3d_entity_add_child(parent, fourth);
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(parent)),
                  4,
                  "addChild bounds corrupt capacity by trusted allocation metadata");
    EXPECT_EQ_INT(parent_layout->child_capacity,
                  parent_layout->child_storage_capacity,
                  "child capacity repair restores the owned allocation bound");

    void **saved_children = parent_layout->children;
    int32_t saved_child_count = parent_layout->child_count;
    int32_t saved_child_capacity = parent_layout->child_capacity;
    int32_t saved_storage_capacity = parent_layout->child_storage_capacity;
    uint64_t saved_storage_cookie = parent_layout->child_storage_cookie;
    int8_t saved_group = parent_layout->group;
    void *wrong_storage = rt_material3d_new_color(0.1, 0.2, 0.3);
    parent_layout->children = reinterpret_cast<void **>(wrong_storage);
    parent_layout->group = 0;
    EXPECT_EQ_INT(rt_game3d_entity_is_group(parent),
                  0,
                  "child traversal rejects a pointer that does not match its storage marker");
    parent_layout->children = saved_children;
    parent_layout->child_count = saved_child_count;
    parent_layout->child_capacity = saved_child_capacity;
    parent_layout->child_storage_capacity = saved_storage_capacity;
    parent_layout->child_storage_cookie = saved_storage_cookie;
    parent_layout->group = saved_group;
    if (wrong_storage && rt_obj_release_check0(wrong_storage))
        rt_obj_free(wrong_storage);

    parent_layout->child_count = INT32_MAX;
    parent_layout->child_capacity = 4;
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Corrupt Child Count Unit"), 80, 60);
    rt_game3d_world_spawn(world, parent);
    EXPECT_EQ_INT(rt_game3d_world_get_entity_count(world),
                  5,
                  "spawn clamps corrupt entity child count while visiting the tree");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("CorruptCountSecond")) ==
                    rt_game3d_entity_get_node(second),
                "spawn still reaches valid children under the clamped count");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("CorruptCountThird")) ==
                    rt_game3d_entity_get_node(third),
                "spawn reaches children after capacity repair");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("CorruptCountFourth")) ==
                    rt_game3d_entity_get_node(fourth),
                "spawn reaches the complete recovered dense child prefix");

    parent_layout->child_count = INT32_MAX;
    parent_layout->child_capacity = 4;
    rt_game3d_world_despawn(world, parent);
    EXPECT_EQ_INT(rt_game3d_world_get_entity_count(world),
                  0,
                  "despawn clamps corrupt entity child count while visiting the tree");
    EXPECT_TRUE(rt_game3d_entity_is_spawned(first) == 0 && rt_game3d_entity_is_spawned(second) == 0,
                "despawn reaches all valid children under the clamped count");

    rt_game3d_world_destroy(world);

    void *finalizer_parent = rt_game3d_entity_new();
    void *finalizer_child = rt_game3d_entity_new();
    rt_game3d_entity_add_child(finalizer_parent, finalizer_child);
    auto *finalizer_layout = static_cast<Game3DEntityTestLayout *>(finalizer_parent);
    size_t child_refcount = rt_heap_hdr(finalizer_child)->refcnt;
    finalizer_layout->child_count = 0;
    finalizer_layout->child_capacity = 0;
    if (rt_obj_release_check0(finalizer_parent))
        rt_obj_free(finalizer_parent);
    EXPECT_TRUE(rt_heap_hdr(finalizer_child)->refcnt + 1 == child_refcount,
                "finalization releases owned children despite underreported public mirrors");
    if (rt_obj_release_check0(finalizer_child))
        rt_obj_free(finalizer_child);
    PASS();
}

static bool test_frame_loop_manual_frame_and_final_capture() {
    TEST("World3D runFrames, manual frame API, overlay, and final capture");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Unit Frames"), 64, 48);
    EXPECT_TRUE(world != nullptr, "World3D.New returns an object");
    rt_canvas3d *canvas_state = (rt_canvas3d *)rt_game3d_world_get_canvas(world);
    rt_canvas3d_set_input_source(canvas_state, 2);
    rt_canvas3d_set_clock_source(canvas_state, 0);
    rt_canvas3d_set_synthetic_delta_time_sec(canvas_state, 0.123);

    g_update_calls = 0;
    g_update_dt_sum = 0.0;
    g_observed_canvas = canvas_state;
    g_observed_input_source = -1;
    g_observed_clock_source = -1;
    g_observed_synthetic_dt_us = -1;
    rt_game3d_world_run_frames(world, 3, 0.02, (void *)&game3d_test_observing_update);
    EXPECT_EQ_INT(g_update_calls, 3, "runFrames invokes the update callback once per frame");
    EXPECT_NEAR(g_update_dt_sum, 0.06, 0.0001, "runFrames passes fixed dt to callback");
    EXPECT_EQ_INT(g_observed_input_source, 1, "runFrames callback observes synthetic input");
    EXPECT_EQ_INT(g_observed_clock_source, 1, "runFrames callback observes synthetic clock");
    EXPECT_EQ_INT(g_observed_synthetic_dt_us, 20000, "runFrames callback observes fixed dt");
    EXPECT_EQ_INT(rt_game3d_world_get_frame(world), 3, "runFrames increments frame count");
    EXPECT_NEAR(rt_game3d_world_get_dt(world), 0.02, 0.0001, "runFrames stores dt");
    EXPECT_NEAR(rt_game3d_world_get_elapsed(world), 0.06, 0.0001, "runFrames stores elapsed time");
    EXPECT_EQ_INT(canvas_state->input_source, 2, "runFrames restores input source");
    EXPECT_EQ_INT(canvas_state->clock_source, 0, "runFrames restores clock source");
    EXPECT_EQ_INT(canvas_state->synthetic_dt_us, 123000, "runFrames restores synthetic delta");

    EXPECT_TRUE(expect_trap_contains(
                    [&] {
                        rt_game3d_world_run_frames(
                            world, 1, 0.02, (void *)&game3d_test_trapping_update);
                    },
                    "runFrames callback trap"),
                "runFrames propagates callback traps");
    EXPECT_EQ_INT(canvas_state->input_source, 2, "runFrames restores input source after trap");
    EXPECT_EQ_INT(canvas_state->clock_source, 0, "runFrames restores clock source after trap");
    EXPECT_EQ_INT(
        canvas_state->synthetic_dt_us, 123000, "runFrames restores synthetic delta after trap");

    g_overlay_calls = 0;
    rt_game3d_world_begin_frame(world);
    rt_game3d_world_draw_scene(world);
    rt_game3d_world_draw_effects(world);
    rt_game3d_world_end_scene(world);
    rt_game3d_world_draw_overlay(world, (void *)&game3d_test_overlay);
    EXPECT_EQ_INT(g_overlay_calls, 1, "drawOverlay invokes overlay callback");

    void *pixels = rt_game3d_world_capture_final_frame(world);
    EXPECT_TRUE(pixels != nullptr, "captureFinalFrame returns Pixels");
    EXPECT_EQ_INT(rt_pixels_width(pixels),
                  rt_canvas3d_get_window_width(canvas_state),
                  "captured final frame width");
    EXPECT_EQ_INT(rt_pixels_height(pixels),
                  rt_canvas3d_get_window_height(canvas_state),
                  "captured final frame height");
    rt_game3d_world_present(world);
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_run_fixed_accumulator_and_spiral_guard() {
    TEST("World3D.runFixed accumulates fixed steps and caps spiral frames");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Fixed Loop Unit"), 64, 48);
    rt_canvas3d *canvas_state = (rt_canvas3d *)rt_game3d_world_get_canvas(world);
    rt_canvas3d_set_input_source(canvas_state, 1);
    rt_canvas3d_set_clock_source(canvas_state, 1);
    rt_canvas3d_set_dt_max(canvas_state, 0);
    rt_canvas3d_set_synthetic_delta_time_sec(canvas_state, 0.25);

    g_update_calls = 0;
    g_update_dt_sum = 0.0;
    g_overlay_calls = 0;
    g_fixed_loop_canvas = canvas_state;
    g_fixed_loop_world = world;
    g_fixed_loop_stop_after = 4;
    g_fixed_loop_destroy_at_stop = true;
    g_fixed_loop_observed_dropped = -1;
    rt_game3d_world_run_fixed(world, 0.0625, (void *)&game3d_test_fixed_update);
    g_fixed_loop_canvas = nullptr;
    g_fixed_loop_world = nullptr;
    g_fixed_loop_stop_after = 0;
    g_fixed_loop_destroy_at_stop = false;

    EXPECT_EQ_INT(g_update_calls, 4, "runFixed consumes accumulated fixed steps");
    EXPECT_NEAR(g_update_dt_sum, 0.25, 0.0001, "runFixed update callback receives fixed dt");
    EXPECT_EQ_INT(g_fixed_loop_observed_dropped, 0, "normal runFixed frame does not drop steps");
    EXPECT_TRUE(rt_game3d_world_is_destroyed(world) != 0,
                "runFixed can exit when a native callback destroys the world");

    world = rt_game3d_world_new(rt_const_cstr("Game3D Fixed Loop Spiral Unit"), 64, 48);
    canvas_state = (rt_canvas3d *)rt_game3d_world_get_canvas(world);
    rt_canvas3d_set_input_source(canvas_state, 1);
    rt_canvas3d_set_clock_source(canvas_state, 1);
    rt_canvas3d_set_dt_max(canvas_state, 0);
    rt_canvas3d_set_synthetic_delta_time_sec(canvas_state, 0.25);

    g_update_calls = 0;
    g_update_dt_sum = 0.0;
    g_fixed_loop_canvas = canvas_state;
    g_fixed_loop_world = world;
    g_fixed_loop_stop_after = 8;
    g_fixed_loop_destroy_at_stop = true;
    g_fixed_loop_observed_dropped = -1;
    rt_game3d_world_run_fixed(world, 0.015625, (void *)&game3d_test_fixed_update);
    g_fixed_loop_canvas = nullptr;
    g_fixed_loop_world = nullptr;
    g_fixed_loop_stop_after = 0;
    g_fixed_loop_destroy_at_stop = false;

    EXPECT_EQ_INT(g_update_calls, 8, "runFixed caps a large display tick to max fixed steps");
    EXPECT_NEAR(g_update_dt_sum, 0.125, 0.0001, "runFixed cap uses fixed dt for every step");
    EXPECT_EQ_INT(g_fixed_loop_observed_dropped,
                  8,
                  "runFixed records fixed steps discarded by the spiral guard");
    EXPECT_TRUE(rt_game3d_world_is_destroyed(world) != 0,
                "spiral-guard callback destroyed the world");
    PASS();
}

struct WorkerReplaySummary {
    int64_t frame;
    double elapsed;
    double dt;
    double entity_x;
    double entity_y;
    double entity_z;
    int64_t pixels_w;
    int64_t pixels_h;
};

static WorkerReplaySummary run_worker_replay(int64_t worker_count) {
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Worker Replay"), 64, 48);
    rt_game3d_world_set_worker_count(world, worker_count);
    rt_game3d_world_set_gravity(world, 0.0, -9.81, 0.0);

    void *entity = rt_game3d_entity_new();
    rt_game3d_entity_set_position(entity, 0.0, 4.0, 0.0);
    rt_game3d_entity_attach_body(entity, rt_game3d_body_def_sphere(0.5, 1.0));
    rt_game3d_world_spawn(world, entity);
    rt_game3d_world_run_frames_only(world, 5, 1.0 / 60.0);

    void *pos = rt_game3d_entity_world_position(entity);
    void *pixels = rt_game3d_world_capture_final_frame(world);
    WorkerReplaySummary summary = {rt_game3d_world_get_frame(world),
                                   rt_game3d_world_get_elapsed(world),
                                   rt_game3d_world_get_dt(world),
                                   rt_vec3_x(pos),
                                   rt_vec3_y(pos),
                                   rt_vec3_z(pos),
                                   rt_pixels_width(pixels),
                                   rt_pixels_height(pixels)};
    rt_game3d_world_destroy(world);
    return summary;
}

static bool test_worker_count_runframes_replay_parity() {
    TEST("World3D.runFramesOnly is stable across workerCount settings");
    WorkerReplaySummary single = run_worker_replay(1);
    WorkerReplaySummary multi = run_worker_replay(4);

    EXPECT_EQ_INT(single.frame, multi.frame, "single/multi worker frame parity");
    EXPECT_NEAR(single.elapsed, multi.elapsed, 0.000001, "single/multi worker elapsed parity");
    EXPECT_NEAR(single.dt, multi.dt, 0.000001, "single/multi worker dt parity");
    EXPECT_NEAR(single.entity_x, multi.entity_x, 0.000001, "single/multi worker entity x parity");
    EXPECT_NEAR(single.entity_y, multi.entity_y, 0.000001, "single/multi worker entity y parity");
    EXPECT_NEAR(single.entity_z, multi.entity_z, 0.000001, "single/multi worker entity z parity");
    EXPECT_EQ_INT(single.pixels_w, multi.pixels_w, "single/multi worker final width parity");
    EXPECT_EQ_INT(single.pixels_h, multi.pixels_h, "single/multi worker final height parity");
    PASS();
}

struct WorkerAnimBatchSummary {
    double entity_x[12];
    double state_time[12];
    int64_t event_count[12];
    int32_t animation_job_capacity;
    int8_t jobs_enabled;
};

static WorkerAnimBatchSummary run_worker_animation_batch(int64_t worker_count) {
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Worker Animations"), 64, 48);
    rt_game3d_world_set_worker_count(world, worker_count);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);

    void *entities[12] = {};
    void *animators[12] = {};
    for (int32_t i = 0; i < 12; ++i) {
        entities[i] = rt_game3d_entity_new();
        void *controller = make_game3d_test_controller(2.0 + (double)i * 0.25, 0.25);
        animators[i] = rt_game3d_animator_new(controller);
        rt_game3d_entity_attach_animator(entities[i], animators[i]);
        rt_scene_node3d_set_sync_mode(rt_game3d_entity_get_node(entities[i]),
                                      rt_game3d_sync_mode_node_from_anim_root_motion());
        rt_game3d_animator_play(animators[i], rt_const_cstr("walk"));
        rt_game3d_world_spawn(world, entities[i]);
    }

    rt_game3d_world_step_simulation(world, 0.25);

    WorkerAnimBatchSummary summary = {};
    for (int32_t i = 0; i < 12; ++i) {
        void *pos = rt_game3d_entity_position(entities[i]);
        summary.entity_x[i] = rt_vec3_x(pos);
        summary.state_time[i] = rt_game3d_animator_state_time(animators[i]);
        summary.event_count[i] = rt_game3d_animator_event_count(animators[i]);
    }
    Game3DWorldTestLayout *layout = (Game3DWorldTestLayout *)world;
    summary.animation_job_capacity = layout->animation_job_capacity;
    summary.jobs_enabled = layout->jobs_enabled;
    rt_game3d_world_destroy(world);
    return summary;
}

static bool test_worker_count_parallel_animation_parity() {
    TEST("World3D worker jobs update animator batches deterministically");
    WorkerAnimBatchSummary single = run_worker_animation_batch(1);
    WorkerAnimBatchSummary multi = run_worker_animation_batch(4);

    for (int32_t i = 0; i < 12; ++i) {
        EXPECT_NEAR(single.entity_x[i],
                    multi.entity_x[i],
                    0.000001,
                    "single/multi animation root-motion parity");
        EXPECT_NEAR(single.state_time[i],
                    multi.state_time[i],
                    0.000001,
                    "single/multi animation state-time parity");
        EXPECT_EQ_INT(single.event_count[i], 1, "single-worker animation fires event");
        EXPECT_EQ_INT(multi.event_count[i], 1, "multi-worker animation fires event");
    }
    EXPECT_EQ_INT(single.animation_job_capacity, 0, "single-worker animation stays serial");
    EXPECT_TRUE(multi.animation_job_capacity > 1, "multi-worker animation allocated job slots");
    EXPECT_EQ_INT(multi.jobs_enabled, 1, "multi-worker animation keeps jobs enabled");
    PASS();
}

static bool test_world_animation_clamps_corrupt_entity_count() {
    TEST("World3D animation update clamps corrupt entity counts to registry capacity");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Worker Anim Count Corrupt"), 64, 48);
    rt_game3d_world_set_worker_count(world, 4);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);

    void *entity = rt_game3d_entity_new();
    rt_game3d_entity_set_name(entity, rt_const_cstr("corrupt-count-animated"));
    void *controller = make_game3d_test_controller(3.0, 0.25);
    void *animator = rt_game3d_animator_new(controller);
    rt_game3d_entity_attach_animator(entity, animator);
    rt_scene_node3d_set_sync_mode(rt_game3d_entity_get_node(entity),
                                  rt_game3d_sync_mode_node_from_anim_root_motion());
    rt_game3d_animator_play(animator, rt_const_cstr("walk"));
    rt_game3d_world_spawn(world, entity);

    auto *layout = static_cast<Game3DWorldTestLayout *>(world);
    const int32_t saved_count = layout->entity_count;
    const int32_t saved_capacity = layout->entity_capacity;
    layout->entity_count = INT32_MAX;
    layout->entity_capacity = 1;
    rt_game3d_world_step_simulation(world, 0.25);

    EXPECT_EQ_INT(rt_game3d_world_get_entity_count(world),
                  1,
                  "World3D.entityCount reports bounded registry count after corruption");
    EXPECT_NEAR(rt_game3d_animator_state_time(animator),
                0.25,
                0.000001,
                "animation scheduler advances bounded entities after private count corruption");
    EXPECT_EQ_INT(rt_game3d_animator_event_count(animator),
                  1,
                  "animation event polling remains bounded after private count corruption");

    layout->name_index_valid = 0;
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("corrupt-count-animated")) ==
                    entity,
                "findEntity rebuilds a bounded name index after private count corruption");

    void *second = rt_game3d_entity_new();
    rt_game3d_entity_set_name(second, rt_const_cstr("spawn-after-count-repair"));
    rt_game3d_world_spawn(world, second);
    EXPECT_EQ_INT(rt_game3d_world_get_entity_count(world),
                  2,
                  "World3D.spawn repairs corrupt entity count before appending");
    EXPECT_TRUE(rt_game3d_world_find_entity(world, rt_const_cstr("spawn-after-count-repair")) ==
                    second,
                "newly spawned entity remains indexed after count repair");

    layout->entity_count = INT32_MAX;
    layout->entity_capacity = 2;
    (void)saved_count;
    (void)saved_capacity;
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_world_getters_sanitize_corrupt_private_state() {
    TEST("World3D getters validate component handles and sanitize scalar telemetry");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Corrupt World Getters"), 64, 48);
    auto *layout = static_cast<Game3DWorldTestLayout *>(world);
    void *wrong = rt_material3d_new_color(0.1, 0.2, 0.3);
    size_t wrong_ref = rt_heap_hdr(wrong)->refcnt;

    void *saved_canvas = layout->canvas;
    layout->canvas = wrong;
    EXPECT_TRUE(rt_game3d_world_get_canvas(world) == nullptr,
                "World3D.GetCanvas hides wrong-class private canvas slots");
    EXPECT_EQ_INT(rt_game3d_world_get_draw_count(world),
                  0,
                  "World3D.drawCount ignores wrong-class private canvas slots");
    layout->canvas = saved_canvas;

    void *saved_camera = layout->camera;
    layout->camera = wrong;
    EXPECT_TRUE(rt_game3d_world_get_camera(world) == nullptr,
                "World3D.GetCamera hides wrong-class private camera slots");
    layout->camera = saved_camera;

    void *saved_scene = layout->scene;
    layout->scene = wrong;
    EXPECT_TRUE(rt_game3d_world_get_scene(world) == nullptr,
                "World3D.GetScene hides wrong-class private scene slots");
    EXPECT_EQ_INT(rt_game3d_world_get_visible_node_count(world),
                  0,
                  "World3D.visibleNodeCount ignores wrong-class private scene slots");
    layout->scene = saved_scene;

    void *saved_physics = layout->physics;
    layout->physics = wrong;
    EXPECT_TRUE(rt_game3d_world_get_physics(world) == nullptr,
                "World3D.GetPhysics hides wrong-class private physics slots");
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(world),
                  0,
                  "World3D.bodyCount ignores wrong-class private physics slots");
    layout->physics = saved_physics;

    void *saved_input = layout->input;
    layout->input = wrong;
    EXPECT_TRUE(rt_game3d_world_get_input(world) == nullptr,
                "World3D.GetInput hides wrong-class private input slots");
    layout->input = saved_input;

    void *saved_audio = layout->audio;
    layout->audio = wrong;
    EXPECT_TRUE(rt_game3d_world_get_audio(world) == nullptr,
                "World3D.GetAudio hides wrong-class private audio slots");
    layout->audio = saved_audio;

    void *saved_effects = layout->effects;
    layout->effects = wrong;
    EXPECT_TRUE(rt_game3d_world_get_effects(world) == nullptr,
                "World3D.GetEffects hides wrong-class private effects slots");
    layout->effects = saved_effects;

    auto *effects_view = static_cast<Game3DEffectsTestLayout *>(saved_effects);
    void *saved_postfx = effects_view->postfx;
    effects_view->postfx = wrong;
    EXPECT_TRUE(rt_game3d_effects_get_postfx(saved_effects) == nullptr,
                "EffectRegistry3D.GetPostFX hides wrong-class private post-FX slots");
    effects_view->postfx = saved_postfx;

    layout->stream = wrong;
    void *stream = rt_game3d_world_get_stream(world);
    EXPECT_TRUE(stream != nullptr && stream != wrong,
                "World3D.GetStream replaces wrong-class private stream slots");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_ref,
                "World3D component getters do not release wrong-class private handles");

    void *undersized_canvas = rt_obj_new_i64(RT_G3D_CANVAS3D_CLASS_ID, 1);
    void *undersized_camera = rt_obj_new_i64(RT_G3D_CAMERA3D_CLASS_ID, 1);
    void *undersized_scene = rt_obj_new_i64(RT_G3D_SCENE3D_CLASS_ID, 1);
    void *undersized_input = rt_obj_new_i64(RT_G3D_GAME3D_INPUT_CLASS_ID, 1);
    void *undersized_audio = rt_obj_new_i64(RT_G3D_GAME3D_SOUND_CLASS_ID, 1);
    void *undersized_effects = rt_obj_new_i64(RT_G3D_GAME3D_EFFECTS_CLASS_ID, 1);
    void *undersized_stream = rt_obj_new_i64(RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID, 1);
    EXPECT_TRUE(undersized_canvas && undersized_camera && undersized_scene && undersized_input &&
                    undersized_audio && undersized_effects && undersized_stream,
                "same-class undersized component fixtures allocate");

    layout->canvas = undersized_canvas;
    EXPECT_TRUE(rt_game3d_world_get_canvas(world) == nullptr,
                "World3D.GetCanvas rejects an undersized same-class payload");
    rt_game3d_world_set_skybox(world, nullptr);
    rt_game3d_world_set_quality(world, RT_GAME3D_QUALITY_BALANCED);
    layout->canvas = saved_canvas;
    layout->camera = undersized_camera;
    EXPECT_TRUE(rt_game3d_world_get_camera(world) == nullptr,
                "World3D.GetCamera rejects an undersized same-class payload");
    layout->camera = saved_camera;
    layout->scene = undersized_scene;
    EXPECT_TRUE(rt_game3d_world_get_scene(world) == nullptr,
                "World3D.GetScene rejects an undersized same-class payload");
    layout->scene = saved_scene;
    layout->input = undersized_input;
    EXPECT_TRUE(rt_game3d_world_get_input(world) == nullptr,
                "World3D.GetInput rejects an undersized same-class payload");
    layout->input = saved_input;
    layout->audio = undersized_audio;
    EXPECT_TRUE(rt_game3d_world_get_audio(world) == nullptr,
                "World3D.GetAudio rejects an undersized same-class payload");
    layout->audio = saved_audio;
    layout->effects = undersized_effects;
    EXPECT_TRUE(rt_game3d_world_get_effects(world) == nullptr,
                "World3D.GetEffects rejects an undersized same-class payload");
    rt_game3d_world_set_quality(world, RT_GAME3D_QUALITY_BALANCED);
    layout->effects = saved_effects;

    layout->stream = nullptr;
    release_runtime_ref(stream);
    rt_obj_retain_maybe(undersized_stream);
    layout->stream = undersized_stream;
    EXPECT_EQ_INT(rt_game3d_world_get_stream_resident_bytes(world),
                  0,
                  "World3D stream telemetry rejects an undersized same-class payload");
    stream = rt_game3d_world_get_stream(world);
    EXPECT_TRUE(stream != nullptr && stream != undersized_stream,
                "World3D.GetStream replaces an undersized same-class payload");
    EXPECT_EQ_INT(rt_heap_hdr(undersized_stream)->refcnt,
                  1,
                  "World3D.GetStream releases only its retained undersized slot reference");

    void *undersized_components[] = {undersized_canvas,
                                     undersized_camera,
                                     undersized_scene,
                                     undersized_input,
                                     undersized_audio,
                                     undersized_effects,
                                     undersized_stream};
    for (void *component : undersized_components)
        release_runtime_ref(component);

    layout->dt = NAN;
    layout->elapsed = -10.0;
    layout->frame = -11;
    layout->dropped_fixed_steps = -12;
    layout->worker_count = -13;
    layout->jobs_enabled = -14;
    layout->floating_origin = -15;
    layout->world_origin[0] = NAN;
    layout->world_origin[1] = INFINITY;
    layout->world_origin[2] = -10000000000000.0;
    EXPECT_NEAR(rt_game3d_world_get_dt(world), 0.0, 0.000001, "World3D.Dt sanitizes NaN");
    EXPECT_NEAR(
        rt_game3d_world_get_elapsed(world), 0.0, 0.000001, "World3D.Elapsed clamps negative");
    EXPECT_EQ_INT(rt_game3d_world_get_frame(world), 0, "World3D.Frame clamps negative values");
    EXPECT_EQ_INT(rt_game3d_world_get_dropped_fixed_steps(world),
                  0,
                  "World3D.DroppedFixedSteps clamps negative values");
    EXPECT_EQ_INT(rt_game3d_world_get_worker_count(world),
                  1,
                  "World3D.WorkerCount repairs invalid private counts");
    EXPECT_TRUE(rt_game3d_world_get_jobs_enabled(world) == 1,
                "World3D.jobsEnabled getter normalizes corrupt private flags");
    EXPECT_TRUE(rt_game3d_world_get_floating_origin(world) == 1,
                "World3D.floatingOrigin getter normalizes corrupt private flags");
    void *origin = rt_game3d_world_get_world_origin(world);
    EXPECT_NEAR(rt_vec3_x(origin), 0.0, 0.000001, "World3D.worldOrigin sanitizes NaN X");
    EXPECT_NEAR(rt_vec3_y(origin), 0.0, 0.000001, "World3D.worldOrigin sanitizes Inf Y");
    EXPECT_NEAR(
        rt_vec3_z(origin), -1000000000000.0, 0.001, "World3D.worldOrigin clamps oversized Z");

    void *owned_components[] = {layout->stream,
                                layout->effects,
                                layout->audio,
                                layout->input,
                                layout->physics,
                                layout->scene,
                                layout->camera,
                                layout->canvas};
    layout->stream = nullptr;
    layout->effects = nullptr;
    layout->audio = nullptr;
    layout->input = nullptr;
    layout->physics = nullptr;
    layout->scene = nullptr;
    layout->camera = nullptr;
    layout->canvas = nullptr;
    for (void *component : owned_components)
        release_runtime_ref(component);
    layout->stream = wrong;
    layout->effects = wrong;
    layout->audio = wrong;
    layout->input = wrong;
    layout->physics = wrong;
    layout->scene = wrong;
    layout->camera = wrong;
    layout->canvas = wrong;
    rt_game3d_world_destroy(world);
    EXPECT_EQ_INT(rt_heap_hdr(wrong)->refcnt,
                  wrong_ref,
                  "World3D teardown clears wrong-class component corruption as unowned");
    release_runtime_ref(wrong);
    PASS();
}

static bool test_world_floating_origin_controls_and_rebase() {
    TEST("World3D floatingOrigin rebases camera, entities, and bodies");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Floating Origin"), 64, 48);
    EXPECT_EQ_INT(rt_game3d_world_get_floating_origin(world), 0, "floatingOrigin defaults off");
    void *origin = rt_game3d_world_get_world_origin(world);
    EXPECT_NEAR(rt_vec3_x(origin), 0.0, 0.000001, "worldOrigin starts at x=0");

    rt_game3d_world_set_origin_rebase_threshold(world, 10.0);
    void *camera = rt_game3d_world_get_camera(world);
    rt_camera3d_set_position(camera, rt_vec3_new(20.0, 0.0, 0.0));
    void *entity = rt_game3d_entity_new();
    rt_game3d_entity_set_position(entity, 23.0, 0.0, 0.0);
    rt_game3d_entity_attach_body(entity, rt_game3d_body_def_sphere(0.5, 1.0));
    rt_game3d_world_spawn(world, entity);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_NEAR(
        rt_vec3_x(rt_camera3d_get_position(camera)), 20.0, 0.000001, "flag-off camera unchanged");
    EXPECT_NEAR(
        rt_vec3_x(rt_game3d_entity_position(entity)), 23.0, 0.000001, "flag-off entity unchanged");
    EXPECT_NEAR(rt_vec3_x(rt_game3d_world_get_world_origin(world)),
                0.0,
                0.000001,
                "flag-off worldOrigin unchanged");

    rt_game3d_world_set_floating_origin(world, 1);
    EXPECT_EQ_INT(rt_game3d_world_get_floating_origin(world), 1, "floatingOrigin setter enables");
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_NEAR(
        rt_vec3_x(rt_camera3d_get_position(camera)), 0.0, 0.000001, "camera rebased to origin");
    EXPECT_NEAR(rt_vec3_x(rt_game3d_entity_position(entity)),
                3.0,
                0.000001,
                "entity shifted by rebase delta");
    EXPECT_NEAR(rt_vec3_x(rt_body3d_get_position(rt_game3d_entity_get_body(entity))),
                3.0,
                0.000001,
                "body shifted by rebase delta");
    EXPECT_NEAR(rt_vec3_x(rt_game3d_world_get_world_origin(world)),
                20.0,
                0.000001,
                "worldOrigin accumulates rebase delta");

    void *audio = rt_game3d_world_get_audio(world);
    void *listener = rt_game3d_audio_get_listener(audio);
    EXPECT_TRUE(listener != nullptr, "floatingOrigin world has an audio listener");
    rt_game3d_audio_listener_follow_camera(audio, 0);
    rt_camera3d_set_position(camera, rt_vec3_new(50000.0, -5.0, 7000.0));
    rt_game3d_entity_set_position(entity, 50004.0, -3.0, 6998.0);
    rt_body3d_set_position(rt_game3d_entity_get_body(entity), 50004.0, -3.0, 6998.0);
    rt_game3d_audio_set_listener_pose(audio,
                                      rt_vec3_new(50002.0, -4.0, 7001.0),
                                      rt_vec3_new(0.0, 0.0, -1.0),
                                      rt_vec3_new(0.0, 1.0, 0.0));

    void *effects = rt_game3d_world_get_effects(world);
    EXPECT_TRUE(effects != nullptr, "floatingOrigin world has an effect registry");
    void *far_particles = rt_particles3d_new(4);
    rt_particles3d_set_position(far_particles, 50003.0, -4.0, 7002.0);
    rt_particles3d_set_speed(far_particles, 0.0, 0.0);
    rt_particles3d_set_lifetime(far_particles, 10.0, 10.0);
    rt_particles3d_set_size(far_particles, 1.0, 1.0);
    rt_particles3d_burst(far_particles, 1);
    rt_game3d_effects_add_particles(effects, far_particles, -1.0);
    void *far_decal = rt_decal3d_new(
        rt_vec3_new(50006.0, -4.0, 6999.0), rt_vec3_new(0.0, 1.0, 0.0), 1.0, nullptr);
    rt_game3d_effects_add_decal(effects, far_decal);

    rt_canvas3d *canvas_state = (rt_canvas3d *)rt_game3d_world_get_canvas(world);
    EXPECT_TRUE(canvas_state != nullptr, "floatingOrigin world has a live Canvas3D");

    /* The effect registry rides the floating-origin rebase: the particle emitter and the decal
     * keep their authored far world positions until a rebase shifts them. Verify against stored
     * world positions, which is robust and headless-safe -- the rendered path depends on a framed
     * camera the unit harness does not aim at these far-from-origin effects. */
    double particle_pos[3];
    double decal_pos[3];
    rt_particles3d_get_position(far_particles, particle_pos);
    rt_decal3d_get_position(far_decal, decal_pos);
    EXPECT_NEAR(particle_pos[0], 50003.0, 0.000001, "pre-rebase particle emitter at far world X");
    EXPECT_NEAR(particle_pos[1], -4.0, 0.000001, "pre-rebase particle emitter at far world Y");
    EXPECT_NEAR(particle_pos[2], 7002.0, 0.000001, "pre-rebase particle emitter at far world Z");
    EXPECT_NEAR(decal_pos[0], 50006.0, 0.000001, "pre-rebase decal at far world X");
    EXPECT_NEAR(decal_pos[1], -4.0, 0.000001, "pre-rebase decal at far world Y");
    EXPECT_NEAR(decal_pos[2], 6999.0, 0.000001, "pre-rebase decal at far world Z");

    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_NEAR(
        rt_vec3_x(rt_camera3d_get_position(camera)), 0.0, 0.000001, "50km camera X rebased");
    EXPECT_NEAR(
        rt_vec3_y(rt_camera3d_get_position(camera)), 0.0, 0.000001, "50km camera Y rebased");
    EXPECT_NEAR(
        rt_vec3_z(rt_camera3d_get_position(camera)), 0.0, 0.000001, "50km camera Z rebased");
    EXPECT_NEAR(
        rt_vec3_x(rt_game3d_entity_position(entity)), 4.0, 0.000001, "50km entity X shifted");
    EXPECT_NEAR(
        rt_vec3_y(rt_game3d_entity_position(entity)), 2.0, 0.000001, "50km entity Y shifted");
    EXPECT_NEAR(
        rt_vec3_z(rt_game3d_entity_position(entity)), -2.0, 0.000001, "50km entity Z shifted");
    void *far_body_pos = rt_body3d_get_position(rt_game3d_entity_get_body(entity));
    EXPECT_NEAR(rt_vec3_x(far_body_pos), 4.0, 0.000001, "50km body X shifted");
    EXPECT_NEAR(rt_vec3_y(far_body_pos), 2.0, 0.000001, "50km body Y shifted");
    EXPECT_NEAR(rt_vec3_z(far_body_pos), -2.0, 0.000001, "50km body Z shifted");
    void *listener_pos = rt_soundlistener3d_get_position(listener);
    EXPECT_NEAR(rt_vec3_x(listener_pos), 2.0, 0.000001, "50km listener X shifted");
    EXPECT_NEAR(rt_vec3_y(listener_pos), 1.0, 0.000001, "50km listener Y shifted");
    EXPECT_NEAR(rt_vec3_z(listener_pos), 1.0, 0.000001, "50km listener Z shifted");
    EXPECT_NEAR(rt_vec3_x(rt_game3d_world_get_world_origin(world)),
                50020.0,
                0.000001,
                "worldOrigin accumulates 50km X delta");
    EXPECT_NEAR(rt_vec3_y(rt_game3d_world_get_world_origin(world)),
                -5.0,
                0.000001,
                "worldOrigin accumulates 50km Y delta");
    EXPECT_NEAR(rt_vec3_z(rt_game3d_world_get_world_origin(world)),
                7000.0,
                0.000001,
                "worldOrigin accumulates 50km Z delta");
    rt_particles3d_get_position(far_particles, particle_pos);
    rt_decal3d_get_position(far_decal, decal_pos);
    EXPECT_NEAR(
        particle_pos[0], 3.0, 0.000001, "50km rebase shifts particle emitter near origin X");
    EXPECT_NEAR(
        particle_pos[1], 1.0, 0.000001, "50km rebase shifts particle emitter near origin Y");
    EXPECT_NEAR(
        particle_pos[2], 2.0, 0.000001, "50km rebase shifts particle emitter near origin Z");
    EXPECT_NEAR(decal_pos[0], 6.0, 0.000001, "50km rebase shifts decal near origin X");
    EXPECT_NEAR(decal_pos[1], 1.0, 0.000001, "50km rebase shifts decal near origin Y");
    EXPECT_NEAR(decal_pos[2], -1.0, 0.000001, "50km rebase shifts decal near origin Z");

    rt_game3d_world_begin_frame(world);
    EXPECT_TRUE(canvas_state && canvas_state->camera_relative_upload == 1,
                "floatingOrigin enables camera-relative upload for the frame");
    rt_game3d_world_end_scene(world);
    rt_game3d_world_set_floating_origin(world, 0);
    rt_game3d_world_begin_frame(world);
    EXPECT_TRUE(canvas_state && canvas_state->camera_relative_upload == 0,
                "floatingOrigin off keeps bounded-scene upload path unchanged");
    rt_game3d_world_end_scene(world);

    rt_game3d_world_rebase_origin(world, 1.0, 2.0, 3.0);
    EXPECT_NEAR(rt_vec3_x(rt_game3d_world_get_world_origin(world)),
                50021.0,
                0.000001,
                "manual rebase accumulates worldOrigin X");
    EXPECT_NEAR(rt_vec3_y(rt_game3d_world_get_world_origin(world)),
                -3.0,
                0.000001,
                "manual rebase accumulates worldOrigin Y");
    EXPECT_NEAR(rt_vec3_z(rt_game3d_world_get_world_origin(world)),
                7003.0,
                0.000001,
                "manual rebase accumulates worldOrigin Z");
    EXPECT_NEAR(rt_vec3_x(rt_camera3d_get_position(camera)),
                -1.0,
                0.000001,
                "manual rebase shifts camera X");
    EXPECT_NEAR(rt_vec3_y(rt_game3d_entity_position(entity)),
                0.0,
                0.000001,
                "manual rebase shifts entity Y");
    void *manual_body_pos = rt_body3d_get_position(rt_game3d_entity_get_body(entity));
    EXPECT_NEAR(rt_vec3_z(manual_body_pos), -5.0, 0.000001, "manual rebase shifts body Z");
    listener_pos = rt_soundlistener3d_get_position(listener);
    EXPECT_NEAR(rt_vec3_y(listener_pos), -1.0, 0.000001, "manual rebase shifts listener Y");
    rt_particles3d_get_position(far_particles, particle_pos);
    rt_decal3d_get_position(far_decal, decal_pos);
    EXPECT_NEAR(particle_pos[0], 2.0, 0.000001, "manual rebase shifts particle emitter X");
    EXPECT_NEAR(decal_pos[2], -4.0, 0.000001, "manual rebase shifts decal Z");

    rt_game3d_world_begin_frame(world);
    EXPECT_TRUE(expect_trap_contains([&] { rt_game3d_world_rebase_origin(world, 1.0, 0.0, 0.0); },
                                     "between frames"),
                "manual rebase traps while a frame is active");
    rt_game3d_world_end_scene(world);

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_world_floating_origin_rendered_parity_and_flag_off_bytes() {
    TEST("World3D floatingOrigin rendered 50km parity and flag-off byte equality");
    for (int frustum = 0; frustum <= 1; ++frustum) {
        Game3DRenderCapture empty_capture;
        Game3DRenderCapture near_capture;
        Game3DRenderCapture far_capture;
        EXPECT_TRUE(game3d_render_parity_scene(0.0, 0, (int8_t)frustum, 0, 0, empty_capture) != 0,
                    "empty render capture succeeds");
        EXPECT_TRUE(game3d_render_parity_scene(0.0, 0, (int8_t)frustum, 0, 1, near_capture) != 0,
                    "near-origin render capture succeeds");
        EXPECT_TRUE(game3d_render_parity_scene(50000.0, 1, (int8_t)frustum, 0, 1, far_capture) != 0,
                    "50km render capture succeeds");
        EXPECT_TRUE(near_capture.pixels != empty_capture.pixels,
                    "near-origin parity scene renders geometry over the clear frame");
        EXPECT_TRUE(far_capture.pixels != empty_capture.pixels,
                    "50km parity scene renders geometry over the clear frame");
        EXPECT_TRUE(far_capture.camera_relative_upload != 0,
                    "50km render uses camera-relative upload");
        EXPECT_TRUE(game3d_pixels_match_with_tolerance(near_capture, far_capture, 160, 2.0) != 0,
                    frustum ? "50km render matches near render with CPU frustum culling"
                            : "50km render matches near render through backend clipping");
    }

    Game3DRenderCapture baseline_off;
    Game3DRenderCapture toggled_off;
    EXPECT_TRUE(game3d_render_parity_scene(0.0, 0, 1, 0, 1, baseline_off) != 0,
                "flag-off baseline capture succeeds");
    EXPECT_TRUE(game3d_render_parity_scene(0.0, 0, 1, 1, 1, toggled_off) != 0,
                "flag-off toggled capture succeeds");
    EXPECT_TRUE(baseline_off.camera_relative_upload == 0 && toggled_off.camera_relative_upload == 0,
                "floatingOrigin disabled keeps camera-relative upload off");
    EXPECT_TRUE(baseline_off.pixels == toggled_off.pixels,
                "floatingOrigin flag-off bounded-scene output is byte-identical");
    PASS();
}

static bool test_step_simulation_clamps_invalid_dt() {
    TEST("World3D.stepSimulation clamps zero, negative, and non-finite dt");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Step DT Unit"), 64, 48);
    void *entity = rt_game3d_entity_new();
    rt_game3d_world_set_gravity(world, 0.0, -10.0, 0.0);
    rt_game3d_entity_set_position(entity, 0.0, 10.0, 0.0);
    rt_game3d_entity_attach_body(entity, rt_game3d_body_def_sphere(0.5, 1.0));
    rt_game3d_world_spawn(world, entity);

    rt_game3d_world_step_simulation(world, 0.0);
    EXPECT_EQ_INT(rt_game3d_world_get_frame(world), 1, "zero dt clamps and advances frame");
    EXPECT_NEAR(rt_game3d_world_get_dt(world), 1.0 / 60.0, 0.0001, "zero dt clamps to default");
    rt_game3d_world_step_simulation(world, -1.0);
    EXPECT_EQ_INT(rt_game3d_world_get_frame(world), 2, "negative dt clamps and advances frame");
    rt_game3d_world_step_simulation(world, NAN);
    EXPECT_EQ_INT(rt_game3d_world_get_frame(world), 3, "NaN dt clamps and advances frame");
    void *pos = rt_game3d_entity_position(entity);
    EXPECT_TRUE(rt_vec3_y(pos) < 10.0, "clamped invalid dt advances physics safely");
    EXPECT_NEAR(rt_game3d_world_get_elapsed(world),
                3.0 / 60.0,
                0.0001,
                "clamped invalid dt increments elapsed time");

    rt_game3d_world_step_simulation(world, 0.1);
    pos = rt_game3d_entity_position(entity);
    EXPECT_TRUE(rt_vec3_y(pos) < 10.0, "positive dt advances physics");
    EXPECT_NEAR(rt_game3d_world_get_dt(world), 0.1, 0.0001, "positive step stores world dt");
    EXPECT_EQ_INT(rt_game3d_world_get_frame(world), 4, "direct stepSimulation increments frame");
    EXPECT_NEAR(rt_game3d_world_get_elapsed(world),
                0.15,
                0.0001,
                "direct stepSimulation increments elapsed time");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_free_fly_controller_synthetic_input() {
    TEST("FreeFlyController consumes synthetic input deterministically");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Unit FreeFly"), 64, 48);
    void *canvas = rt_game3d_world_get_canvas(world);
    void *camera = rt_game3d_world_get_camera(world);
    void *controller = rt_game3d_free_fly_controller_new(world);
    EXPECT_TRUE(controller != nullptr, "FreeFlyController.New returns an object");
    rt_game3d_free_fly_controller_set_speed(controller, 10.0);
    rt_game3d_free_fly_controller_set_look_sensitivity(controller, 0.10);
    auto *fly_view = static_cast<Game3DFreeFlyControllerTestLayout *>(controller);
    fly_view->speed = -INFINITY;
    fly_view->look_sensitivity = INFINITY;
    EXPECT_NEAR(rt_game3d_free_fly_controller_get_speed(controller),
                6.0,
                0.0001,
                "free-fly speed getter sanitizes corrupt private state");
    EXPECT_NEAR(rt_game3d_free_fly_controller_get_look_sensitivity(controller),
                0.01,
                0.0001,
                "free-fly look sensitivity getter sanitizes corrupt private state");
    EXPECT_NEAR(fly_view->speed, 6.0, 0.0001, "free-fly speed repair persists");
    EXPECT_NEAR(fly_view->look_sensitivity, 0.01, 0.0001, "free-fly sensitivity repair persists");
    fly_view->speed = 10.0;
    fly_view->look_sensitivity = 0.10;
    rt_game3d_world_set_camera_controller(world, controller);

    void *before = rt_camera3d_get_position(camera);
    rt_canvas3d_push_synthetic_key(canvas, rt_game3d_key_w(), 1);
    rt_canvas3d_push_synthetic_mouse(canvas, 12.0, -4.0, 0, 0.0);
    rt_game3d_world_run_frames_only(world, 1, 0.1);
    void *after = rt_camera3d_get_position(camera);

    EXPECT_TRUE(rt_vec3_z(after) < rt_vec3_z(before) - 0.05,
                "W moves the free-fly camera forward before simulation");
    EXPECT_TRUE(std::fabs(rt_vec3_x(after) - rt_vec3_x(before)) > 0.001,
                "synthetic mouse look changes the free-fly heading");
    EXPECT_EQ_INT(
        rt_game3d_world_get_frame(world), 1, "controller frame increments with runFramesOnly");
    rt_canvas3d_clear_synthetic_input(canvas);
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_camera_and_character_controllers_preserve_zero_delta_pauses() {
    TEST("Game3D controllers preserve zero-delta pauses");
    rt_keyboard_init();
    rt_mouse_init();
    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();
    rt_keyboard_on_key_down(rt_game3d_key_w());
    rt_mouse_force_delta(24, -12);
    rt_mouse_button_down(rt_game3d_mouse_left());
    rt_mouse_update_wheel(0.0, 2.0);

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Controller Pause Unit"), 80, 60);
    void *camera = rt_game3d_world_get_camera(world);
    void *input = rt_game3d_world_get_input(world);

    void *free_fly = rt_game3d_free_fly_controller_new(world);
    void *before = rt_camera3d_get_position(camera);
    rt_game3d_free_fly_controller_update(free_fly, world, 0.0);
    void *after = rt_camera3d_get_position(camera);
    EXPECT_NEAR(rt_vec3_x(after), rt_vec3_x(before), 0.000001, "paused free-fly keeps camera X");
    EXPECT_NEAR(rt_vec3_y(after), rt_vec3_y(before), 0.000001, "paused free-fly keeps camera Y");
    EXPECT_NEAR(rt_vec3_z(after), rt_vec3_z(before), 0.000001, "paused free-fly keeps camera Z");

    void *first_person = rt_game3d_first_person_controller_new(world);
    before = rt_camera3d_get_position(camera);
    rt_game3d_first_person_controller_update(first_person, world, NAN);
    after = rt_camera3d_get_position(camera);
    EXPECT_NEAR(rt_vec3_x(after),
                rt_vec3_x(before),
                0.000001,
                "non-finite first-person delta keeps camera X");
    EXPECT_NEAR(rt_vec3_z(after),
                rt_vec3_z(before),
                0.000001,
                "non-finite first-person delta keeps camera Z");

    void *orbit_target = rt_vec3_new(0.0, 1.0, 0.0);
    void *orbit = rt_game3d_orbit_controller_new(world, orbit_target);
    double orbit_yaw = rt_game3d_orbit_controller_get_yaw(orbit);
    double orbit_distance = rt_game3d_orbit_controller_get_distance(orbit);
    rt_game3d_orbit_controller_update(orbit, world, 0.0);
    EXPECT_NEAR(rt_game3d_orbit_controller_get_yaw(orbit),
                orbit_yaw,
                0.000001,
                "paused orbit does not consume mouse drag");
    EXPECT_NEAR(rt_game3d_orbit_controller_get_distance(orbit),
                orbit_distance,
                0.000001,
                "paused orbit does not consume wheel input");

    void *follow_target = rt_game3d_entity_new();
    rt_game3d_entity_set_position(follow_target, 20.0, 3.0, -10.0);
    void *follow_offset = rt_vec3_new(0.0, 4.0, 8.0);
    void *follow = rt_game3d_follow_controller_new(world, follow_target, follow_offset);
    before = rt_camera3d_get_position(camera);
    rt_game3d_follow_controller_late_update(follow, world, 0.0);
    after = rt_camera3d_get_position(camera);
    EXPECT_NEAR(rt_vec3_x(after), rt_vec3_x(before), 0.000001, "paused follow keeps camera X");
    EXPECT_NEAR(rt_vec3_y(after), rt_vec3_y(before), 0.000001, "paused follow keeps camera Y");
    EXPECT_NEAR(rt_vec3_z(after), rt_vec3_z(before), 0.000001, "paused follow keeps camera Z");

    void *path = rt_path3d_new();
    void *path_a = rt_vec3_new(0.0, 0.0, 0.0);
    void *path_b = rt_vec3_new(10.0, 0.0, 0.0);
    rt_path3d_add_point(path, path_a);
    rt_path3d_add_point(path, path_b);
    void *rail = rt_game3d_rail_camera_new(world, path);
    rt_game3d_rail_camera_set_progress(rail, 0.25);
    rt_game3d_rail_camera_set_speed(rail, 10.0);
    rt_game3d_rail_camera_update(rail, world, 0.0);
    EXPECT_NEAR(rt_game3d_rail_camera_get_progress(rail),
                0.25,
                0.000001,
                "paused rail does not auto-advance");

    void *character_entity = rt_game3d_entity_new();
    void *character = rt_game3d_character_controller_new(world, character_entity, 0.3, 1.8, 70.0);
    auto *character_view = static_cast<Game3DCharacterControllerTestLayout *>(character);
    character_view->vertical_velocity = 3.0;
    void *character_before = rt_game3d_entity_position(character_entity);
    rt_game3d_character_controller_update(character, input, camera, 0.0);
    void *character_after = rt_game3d_entity_position(character_entity);
    EXPECT_NEAR(rt_vec3_x(character_after),
                rt_vec3_x(character_before),
                0.000001,
                "paused character keeps entity X");
    EXPECT_NEAR(rt_vec3_y(character_after),
                rt_vec3_y(character_before),
                0.000001,
                "paused character keeps entity Y");
    EXPECT_NEAR(character_view->vertical_velocity,
                3.0,
                0.000001,
                "paused character preserves vertical integration state");

    rt_keyboard_on_key_up(rt_game3d_key_w());
    rt_mouse_button_up(rt_game3d_mouse_left());
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_run_frames_only_preserves_synthetic_holds() {
    TEST("World3D.runFramesOnly preserves held synthetic input across frames");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Synthetic Hold Unit"), 64, 48);
    void *canvas = rt_game3d_world_get_canvas(world);
    void *camera = rt_game3d_world_get_camera(world);
    void *controller = rt_game3d_free_fly_controller_new(world);
    rt_game3d_free_fly_controller_set_speed(controller, 10.0);
    rt_game3d_world_set_camera_controller(world, controller);

    void *before = rt_camera3d_get_position(camera);
    rt_canvas3d_push_synthetic_key(canvas, rt_game3d_key_w(), 1);
    rt_game3d_world_run_frames_only(world, 2, 0.1);
    void *after = rt_camera3d_get_position(camera);

    EXPECT_TRUE(rt_vec3_z(after) < rt_vec3_z(before) - 1.5,
                "held W remains down for both deterministic frames");
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_camera_controller_moves_between_worlds() {
    TEST("World3D.setCameraController moves a controller between worlds");
    void *world_a = rt_game3d_world_new(rt_const_cstr("Game3D Controller World A"), 64, 48);
    void *world_b = rt_game3d_world_new(rt_const_cstr("Game3D Controller World B"), 64, 48);
    void *canvas_a = rt_game3d_world_get_canvas(world_a);
    void *canvas_b = rt_game3d_world_get_canvas(world_b);
    void *camera_a = rt_game3d_world_get_camera(world_a);
    void *camera_b = rt_game3d_world_get_camera(world_b);
    void *controller = rt_game3d_free_fly_controller_new(world_a);
    rt_game3d_free_fly_controller_set_speed(controller, 10.0);
    auto *controller_view = static_cast<Game3DFreeFlyControllerTestLayout *>(controller);
    void *saved_controller_world = controller_view->world;
    void *wrong_world_ref = rt_material3d_new_color(0.2, 0.1, 0.4);
    controller_view->world = wrong_world_ref;
    if (saved_controller_world && rt_obj_release_check0(saved_controller_world))
        rt_obj_free(saved_controller_world);

    rt_game3d_world_set_camera_controller(world_a, controller);
    EXPECT_TRUE(controller_view->world == world_a,
                "World3D.setCameraController clears wrong-class controller world refs");
    if (wrong_world_ref && rt_obj_release_check0(wrong_world_ref))
        rt_obj_free(wrong_world_ref);
    rt_game3d_world_set_camera_controller(world_b, controller);

    void *before_a = rt_camera3d_get_position(camera_a);
    rt_canvas3d_push_synthetic_key(canvas_a, rt_game3d_key_w(), 1);
    rt_game3d_world_run_frames_only(world_a, 1, 0.1);
    void *after_a = rt_camera3d_get_position(camera_a);
    EXPECT_NEAR(rt_vec3_z(after_a),
                rt_vec3_z(before_a),
                0.0001,
                "moving a controller detaches it from the previous world");
    rt_canvas3d_clear_synthetic_input(canvas_a);
    rt_game3d_world_destroy(world_a);

    void *before_b = rt_camera3d_get_position(camera_b);
    rt_canvas3d_push_synthetic_key(canvas_b, rt_game3d_key_w(), 1);
    rt_game3d_world_run_frames_only(world_b, 1, 0.1);
    void *after_b = rt_camera3d_get_position(camera_b);
    EXPECT_TRUE(rt_vec3_z(after_b) < rt_vec3_z(before_b) - 0.05,
                "moved controller drives the new world");

    rt_canvas3d_clear_synthetic_input(canvas_b);
    rt_game3d_world_destroy(world_b);
    PASS();
}

static bool test_character_controller_syncs_world_position_under_parent() {
    TEST("CharacterController3D writes world position through parent transforms");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Character Parent Unit"), 80, 60);
    void *parent = rt_game3d_entity_new();
    void *child = rt_game3d_entity_new();
    rt_game3d_entity_set_position(parent, 10.0, 0.0, 0.0);
    rt_game3d_entity_set_scale(parent, 2.0);
    rt_game3d_entity_set_position(child, 1.0, 0.0, 0.0);
    rt_game3d_entity_add_child(parent, child);
    rt_game3d_world_spawn(world, parent);

    void *controller = rt_game3d_character_controller_new(world, child, 0.3, 1.8, 70.0);
    auto *controller_view = static_cast<Game3DCharacterControllerTestLayout *>(controller);
    void *saved_character = controller_view->character;
    void *saved_entity = controller_view->entity;
    void *wrong_ref = rt_material3d_new_color(0.25, 0.25, 0.25);
    controller_view->speed = -INFINITY;
    controller_view->jump_speed = INFINITY;
    controller_view->gravity = NAN;
    controller_view->vertical_velocity = INFINITY;
    controller_view->eye_height = -INFINITY;
    controller_view->stand_height = -1.0;
    controller_view->crouch_height = INFINITY;
    controller_view->capsule_radius = NAN;
    controller_view->crouching = 7;
    EXPECT_NEAR(rt_game3d_character_controller_get_speed(controller),
                6.0,
                0.0001,
                "character speed getter sanitizes corrupt private state");
    EXPECT_NEAR(rt_game3d_character_controller_get_jump_speed(controller),
                5.5,
                0.0001,
                "character jump getter sanitizes corrupt private state");
    EXPECT_NEAR(rt_game3d_character_controller_get_gravity(controller),
                20.0,
                0.0001,
                "character gravity getter sanitizes corrupt private state");
    EXPECT_NEAR(controller_view->speed, 6.0, 0.0001, "character speed repair persists");
    EXPECT_NEAR(controller_view->jump_speed, 5.5, 0.0001, "character jump repair persists");
    EXPECT_NEAR(controller_view->gravity, 20.0, 0.0001, "character gravity repair persists");
    EXPECT_NEAR(controller_view->vertical_velocity,
                0.0,
                0.0001,
                "character vertical velocity repair persists");
    EXPECT_NEAR(
        controller_view->capsule_radius, 0.3, 0.0001, "character capsule radius repair persists");
    EXPECT_TRUE(controller_view->stand_height >= controller_view->capsule_radius * 2.0,
                "character stand height remains valid for the capsule radius");
    EXPECT_TRUE(controller_view->crouch_height >= controller_view->capsule_radius * 2.0 &&
                    controller_view->crouch_height <= controller_view->stand_height,
                "character crouch height remains inside capsule bounds");
    EXPECT_EQ_INT(controller_view->crouching, 1, "character crouch flag is canonicalized");
    controller_view->character = wrong_ref;
    controller_view->entity = wrong_ref;
    EXPECT_TRUE(rt_game3d_character_controller_get_character(controller) == nullptr,
                "wrong-class CharacterController3D character getter returns null");
    EXPECT_TRUE(rt_game3d_character_controller_get_entity(controller) == nullptr,
                "wrong-class CharacterController3D entity getter returns null");
    EXPECT_EQ_INT(rt_game3d_character_controller_grounded(controller),
                  0,
                  "wrong-class CharacterController3D character reports not grounded");
    void *undersized_entity = rt_obj_new_i64(RT_G3D_GAME3D_ENTITY_CLASS_ID, 1);
    controller_view->entity = undersized_entity;
    EXPECT_TRUE(rt_game3d_character_controller_get_entity(controller) == nullptr,
                "undersized private CharacterController3D entity slots are quarantined");
    if (undersized_entity && rt_obj_release_check0(undersized_entity))
        rt_obj_free(undersized_entity);
    rt_game3d_character_controller_teleport(controller, 12.0, 0.0, 0.0);
    controller_view->character = saved_character;
    controller_view->entity = saved_entity;
    controller_view->speed = 6.0;
    controller_view->jump_speed = 5.5;
    controller_view->gravity = 20.0;
    controller_view->vertical_velocity = 0.0;
    controller_view->eye_height = 0.81;
    controller_view->stand_height = 1.8;
    controller_view->crouch_height = 0.9;
    controller_view->capsule_radius = 0.3;
    controller_view->crouching = 0;
    rt_game3d_character_controller_set_gravity(controller, -9.0);
    EXPECT_NEAR(rt_game3d_character_controller_get_gravity(controller),
                20.0,
                0.0001,
                "negative gravity resets to a downward default magnitude");
    rt_game3d_character_controller_set_gravity(controller, 20.0);

    rt_game3d_character_controller_teleport(controller, 14.0, 0.0, 0.0);
    void *world_pos = rt_scene_node3d_get_world_position(rt_game3d_entity_get_node(child));
    void *local_pos = rt_game3d_entity_position(child);
    EXPECT_NEAR(rt_vec3_x(world_pos), 14.0, 0.0001, "child world position follows character");
    EXPECT_NEAR(rt_vec3_x(local_pos), 2.0, 0.0001, "child local position compensates parent scale");

    rt_game3d_entity_set_scale_xyz(parent, 2.0, 3.0, 4.0);
    rt_game3d_entity_set_rotation_euler(parent, 15.0, 60.0, -10.0);
    rt_game3d_character_controller_teleport(controller, -7.0, 8.0, 9.0);
    world_pos = rt_scene_node3d_get_world_position(rt_game3d_entity_get_node(child));
    EXPECT_NEAR(rt_vec3_x(world_pos), -7.0, 0.0001, "affine parent inverse preserves world X");
    EXPECT_NEAR(rt_vec3_y(world_pos), 8.0, 0.0001, "affine parent inverse preserves world Y");
    EXPECT_NEAR(rt_vec3_z(world_pos), 9.0, 0.0001, "affine parent inverse preserves world Z");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_orbit_and_follow_controllers() {
    TEST("OrbitController and FollowController run update/lateUpdate phases");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Unit Orbit Follow"), 80, 60);
    void *canvas = rt_game3d_world_get_canvas(world);

    void *target = rt_vec3_new(0.0, 1.0, 0.0);
    void *orbit = rt_game3d_orbit_controller_new(world, target);
    auto *orbit_view = static_cast<Game3DOrbitControllerTestLayout *>(orbit);
    void *saved_orbit_target = orbit_view->target;
    void *wrong_ref = rt_material3d_new_color(0.4, 0.4, 0.4);
    orbit_view->target = wrong_ref;
    orbit_view->distance = NAN;
    orbit_view->min_distance = 4.0;
    orbit_view->max_distance = 2.0;
    orbit_view->yaw = INFINITY;
    orbit_view->pitch = 999.0;
    EXPECT_TRUE(rt_game3d_orbit_controller_get_target(orbit) == nullptr,
                "wrong-class OrbitController target getter returns null");
    EXPECT_NEAR(rt_game3d_orbit_controller_get_distance(orbit),
                4.0,
                0.0001,
                "orbit distance getter sanitizes corrupt private range");
    EXPECT_NEAR(rt_game3d_orbit_controller_get_yaw(orbit),
                0.0,
                0.0001,
                "orbit yaw getter sanitizes corrupt private state");
    EXPECT_NEAR(rt_game3d_orbit_controller_get_pitch(orbit),
                85.0,
                0.0001,
                "orbit pitch getter clamps corrupt private state");
    EXPECT_NEAR(orbit_view->distance, 4.0, 0.0001, "orbit distance repair persists");
    EXPECT_NEAR(orbit_view->max_distance, 4.0, 0.0001, "orbit range repair persists");
    EXPECT_NEAR(orbit_view->yaw, 0.0, 0.0001, "orbit yaw repair persists");
    EXPECT_NEAR(orbit_view->pitch, 85.0, 0.0001, "orbit pitch repair persists");
    void *undersized_orbit_target = rt_obj_new_i64(RT_G3D_GAME3D_ENTITY_CLASS_ID, 1);
    orbit_view->target = undersized_orbit_target;
    EXPECT_TRUE(rt_game3d_orbit_controller_get_target(orbit) == nullptr,
                "undersized private OrbitController entity targets are quarantined");
    if (undersized_orbit_target && rt_obj_release_check0(undersized_orbit_target))
        rt_obj_free(undersized_orbit_target);
    rt_game3d_orbit_controller_late_update(orbit, world, 0.0);
    orbit_view->target = saved_orbit_target;
    orbit_view->distance = 6.0;
    orbit_view->min_distance = 1.0;
    orbit_view->max_distance = 100.0;
    orbit_view->yaw = 0.0;
    orbit_view->pitch = 20.0;

    rt_game3d_world_set_camera_controller(world, orbit);
    rt_canvas3d_push_synthetic_mouse(canvas, 20.0, -8.0, 1, 1.0);
    rt_game3d_world_run_frames_only(world, 1, 0.1);
    EXPECT_TRUE(rt_game3d_orbit_controller_get_yaw(orbit) > 0.0,
                "orbit mouse drag updates yaw during update");
    EXPECT_TRUE(rt_game3d_orbit_controller_get_distance(orbit) < 6.0,
                "orbit wheel input updates distance during update");

    void *entity = rt_game3d_entity_new();
    rt_game3d_entity_set_position(entity, 4.0, 0.5, -2.0);
    rt_game3d_world_spawn(world, entity);
    rt_game3d_orbit_controller_set_target(orbit, entity);
    EXPECT_TRUE(rt_game3d_orbit_controller_get_target(orbit) == entity,
                "orbit target can be an Entity3D");
    rt_game3d_orbit_controller_late_update(orbit, world, 0.0);
    void *offset = rt_vec3_new(0.0, 2.0, 5.0);
    void *follow = rt_game3d_follow_controller_new(world, entity, offset);
    rt_game3d_follow_controller_set_damping(follow, 0.0);
    auto *follow_view = static_cast<Game3DFollowControllerTestLayout *>(follow);
    void *saved_follow_target = follow_view->target_entity;
    void *saved_follow_offset = follow_view->offset;
    follow_view->target_entity = wrong_ref;
    follow_view->offset = wrong_ref;
    follow_view->damping = INFINITY;
    EXPECT_TRUE(rt_game3d_follow_controller_get_target(follow) == nullptr,
                "wrong-class FollowController target getter returns null");
    EXPECT_TRUE(rt_game3d_follow_controller_get_offset(follow) == nullptr,
                "wrong-class FollowController offset getter returns null");
    EXPECT_NEAR(rt_game3d_follow_controller_get_damping(follow),
                12.0,
                0.0001,
                "follow damping getter sanitizes corrupt private state");
    EXPECT_NEAR(follow_view->damping, 12.0, 0.0001, "follow damping repair persists");
    void *undersized_follow_target = rt_obj_new_i64(RT_G3D_GAME3D_ENTITY_CLASS_ID, 1);
    follow_view->target_entity = undersized_follow_target;
    EXPECT_TRUE(rt_game3d_follow_controller_get_target(follow) == nullptr,
                "undersized private FollowController entity targets are quarantined");
    if (undersized_follow_target && rt_obj_release_check0(undersized_follow_target))
        rt_obj_free(undersized_follow_target);
    rt_game3d_follow_controller_late_update(follow, world, 0.1);
    follow_view->target_entity = saved_follow_target;
    follow_view->offset = saved_follow_offset;
    follow_view->damping = 0.0;

    rt_game3d_world_set_camera_controller(world, follow);
    rt_game3d_world_run_frames_only(world, 1, 0.1);
    void *pos = rt_camera3d_get_position(rt_game3d_world_get_camera(world));
    EXPECT_NEAR(rt_vec3_x(pos), 4.0, 0.0001, "follow snaps x when damping is zero");
    EXPECT_NEAR(rt_vec3_y(pos), 2.5, 0.0001, "follow applies y offset after physics");
    EXPECT_NEAR(rt_vec3_z(pos), 3.0, 0.0001, "follow applies z offset after physics");

    rt_game3d_entity_set_position(entity, 6.0, 0.5, -3.0);
    rt_game3d_world_step_simulation(world, 0.1);
    pos = rt_camera3d_get_position(rt_game3d_world_get_camera(world));
    EXPECT_NEAR(rt_vec3_x(pos), 6.0, 0.0001, "follow tracks post-physics entity x");
    EXPECT_NEAR(rt_vec3_z(pos), 2.0, 0.0001, "follow tracks post-physics entity z");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_controllers_release_destroyed_entity_targets() {
    TEST("Controllers release destroyed entity targets during repair");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Orbit Stale Target Unit"), 64, 48);
    void *entity = rt_game3d_entity_new();
    rt_game3d_world_spawn(world, entity);
    void *orbit = rt_game3d_orbit_controller_new(world, entity);
    void *offset = rt_vec3_new(0.0, 2.0, 4.0);
    void *follow = rt_game3d_follow_controller_new(world, entity, offset);
    void *character = rt_game3d_character_controller_new(world, entity, 0.3, 1.8, 70.0);
    auto *orbit_view = static_cast<Game3DOrbitControllerTestLayout *>(orbit);
    auto *follow_view = static_cast<Game3DFollowControllerTestLayout *>(follow);
    auto *character_view = static_cast<Game3DCharacterControllerTestLayout *>(character);

    rt_game3d_world_destroy(world);
    size_t before = rt_heap_hdr(entity)->refcnt;
    EXPECT_TRUE(rt_game3d_orbit_controller_get_target(orbit) == nullptr,
                "destroyed entity no longer resolves as an orbit target");
    EXPECT_TRUE(orbit_view->target == nullptr,
                "destroyed orbit target slot is cleared persistently");
    EXPECT_TRUE(rt_heap_hdr(entity)->refcnt + 1 == before,
                "clearing a valid stale target releases its retained reference");

    before = rt_heap_hdr(entity)->refcnt;
    EXPECT_TRUE(rt_game3d_follow_controller_get_target(follow) == nullptr,
                "destroyed entity no longer resolves as a follow target");
    EXPECT_TRUE(follow_view->target_entity == nullptr,
                "destroyed follow target slot is cleared persistently");
    EXPECT_TRUE(rt_heap_hdr(entity)->refcnt + 1 == before,
                "clearing a stale follow target releases its retained reference");

    before = rt_heap_hdr(entity)->refcnt;
    EXPECT_TRUE(rt_game3d_character_controller_get_entity(character) == nullptr,
                "destroyed entity no longer resolves as a character binding");
    EXPECT_TRUE(character_view->entity == nullptr,
                "destroyed character entity slot is cleared persistently");
    EXPECT_TRUE(rt_heap_hdr(entity)->refcnt + 1 == before,
                "clearing a stale character binding releases its retained reference");
    PASS();
}

static bool test_first_person_character_controller_same_frame_motion() {
    TEST("FirstPersonController drives CharacterController3D before physics");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Unit Character"), 80, 60);
    void *canvas = rt_game3d_world_get_canvas(world);

    void *ground = rt_game3d_entity_new();
    void *ground_body = rt_body3d_new_aabb(10.0, 0.5, 10.0, 0.0);
    rt_game3d_entity_attach_body(ground, ground_body);
    rt_game3d_entity_set_position(ground, 0.0, -0.5, 0.0);
    rt_game3d_world_spawn(world, ground);

    void *player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(player, 0.0, 1.0, 4.0);
    rt_game3d_world_spawn(world, player);

    void *character = rt_game3d_character_controller_new(world, player, 0.32, 1.8, 70.0);
    void *fps = rt_game3d_first_person_controller_new(world);
    rt_game3d_first_person_controller_set_character(fps, character);
    rt_game3d_first_person_controller_set_speed(fps, 5.0);
    auto *fps_view = static_cast<Game3DFirstPersonControllerTestLayout *>(fps);
    void *saved_fps_character = fps_view->character_controller;
    void *wrong_ref = rt_material3d_new_color(0.5, 0.5, 0.1);
    fps_view->character_controller = wrong_ref;
    fps_view->speed = -INFINITY;
    fps_view->look_sensitivity = INFINITY;
    EXPECT_TRUE(rt_game3d_first_person_controller_get_character(fps) == nullptr,
                "wrong-class FirstPersonController character getter returns null");
    EXPECT_NEAR(rt_game3d_first_person_controller_get_speed(fps),
                6.0,
                0.0001,
                "first-person speed getter sanitizes corrupt private state");
    EXPECT_NEAR(rt_game3d_first_person_controller_get_look_sensitivity(fps),
                0.01,
                0.0001,
                "first-person look sensitivity getter sanitizes corrupt private state");
    EXPECT_NEAR(fps_view->speed, 6.0, 0.0001, "first-person speed repair persists");
    EXPECT_NEAR(
        fps_view->look_sensitivity, 0.01, 0.0001, "first-person sensitivity repair persists");
    void *undersized_character = rt_obj_new_i64(RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID, 1);
    EXPECT_TRUE(
        expect_trap_contains(
            [&] { rt_game3d_first_person_controller_set_character(fps, undersized_character); },
            "invalid character"),
        "first-person setter rejects an undersized CharacterController payload");
    EXPECT_TRUE(fps_view->character_controller == nullptr,
                "rejected undersized character does not mutate the first-person binding");
    fps_view->character_controller = undersized_character;
    EXPECT_TRUE(rt_game3d_first_person_controller_get_character(fps) == nullptr,
                "undersized private first-person character slots are quarantined");
    if (undersized_character && rt_obj_release_check0(undersized_character))
        rt_obj_free(undersized_character);
    fps_view->character_controller = saved_fps_character;
    fps_view->speed = 5.0;
    fps_view->look_sensitivity = 0.01;
    rt_game3d_world_set_camera_controller(world, fps);

    void *before = rt_game3d_entity_position(player);
    rt_canvas3d_push_synthetic_key(canvas, rt_game3d_key_w(), 1);
    rt_game3d_world_run_frames_only(world, 1, 0.1);
    void *after = rt_game3d_entity_position(player);
    EXPECT_TRUE(rt_vec3_z(after) < rt_vec3_z(before) - 0.05,
                "first-person W moves the character in the same run frame");

    void *camera_pos = rt_camera3d_get_position(rt_game3d_world_get_camera(world));
    EXPECT_NEAR(
        rt_vec3_x(camera_pos), rt_vec3_x(after), 0.0001, "camera lateUpdate follows character x");
    EXPECT_NEAR(
        rt_vec3_z(camera_pos), rt_vec3_z(after), 0.0001, "camera lateUpdate follows character z");
    EXPECT_TRUE(rt_vec3_y(camera_pos) > rt_vec3_y(after),
                "camera lateUpdate places the eye above the character");

    rt_canvas3d_clear_synthetic_input(canvas);
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_character_controller_gravity_and_planar_input() {
    TEST("CharacterController3D gravity falls downward and planar input ignores vertical keys");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Character Gravity Unit"), 80, 60);
    void *canvas = rt_game3d_world_get_canvas(world);
    void *input = rt_game3d_world_get_input(world);
    void *camera = rt_game3d_world_get_camera(world);

    void *ground = rt_game3d_entity_new();
    void *ground_body = rt_body3d_new_aabb(10.0, 0.5, 10.0, 0.0);
    rt_game3d_entity_attach_body(ground, ground_body);
    rt_game3d_entity_set_position(ground, 0.0, -0.5, 0.0);
    rt_game3d_world_spawn(world, ground);

    void *player = rt_game3d_entity_new();
    rt_game3d_entity_set_position(player, 0.0, 3.0, 0.0);
    rt_game3d_world_spawn(world, player);

    void *controller = rt_game3d_character_controller_new(world, player, 0.32, 1.8, 70.0);
    rt_game3d_character_controller_set_gravity(controller, 20.0);
    rt_camera3d_look_at(
        camera, rt_vec3_new(0.0, 1.8, 6.0), rt_vec3_new(0.0, 1.8, 0.0), rt_vec3_new(0.0, 1.0, 0.0));

    void *before_fall = rt_game3d_entity_position(player);
    double max_y = rt_vec3_y(before_fall);
    for (int i = 0; i < 20; ++i) {
        rt_game3d_character_controller_update(controller, input, camera, 0.05);
        void *pos = rt_game3d_entity_position(player);
        if (rt_vec3_y(pos) > max_y)
            max_y = rt_vec3_y(pos);
    }
    void *after_fall = rt_game3d_entity_position(player);
    EXPECT_TRUE(rt_vec3_y(after_fall) < rt_vec3_y(before_fall) - 0.5,
                "positive gravity moves an airborne character downward");
    EXPECT_TRUE(max_y <= rt_vec3_y(before_fall) + 0.0001,
                "positive gravity never accelerates the character upward");

    rt_game3d_character_controller_teleport(controller, 0.0, 1.0, 0.0);
    for (int i = 0; i < 10 && rt_game3d_character_controller_grounded(controller) == 0; ++i)
        rt_game3d_character_controller_update(controller, input, camera, 0.05);
    EXPECT_TRUE(rt_game3d_character_controller_grounded(controller) != 0,
                "character settles onto the floor");

    rt_game3d_character_controller_set_speed(controller, 10.0);
    rt_canvas3d_clear_synthetic_input(canvas);
    rt_canvas3d_push_synthetic_key(canvas, rt_game3d_key_w(), 1);
    rt_canvas3d_advance_synthetic_frame(canvas);
    rt_game3d_input_update(input);
    void *before_w = rt_game3d_entity_position(player);
    rt_game3d_character_controller_update(controller, input, camera, 0.1);
    void *after_w = rt_game3d_entity_position(player);
    double w_distance = rt_vec3_z(before_w) - rt_vec3_z(after_w);

    rt_canvas3d_clear_synthetic_input(canvas);
    rt_game3d_input_update(input);
    rt_game3d_character_controller_teleport(controller, 0.0, 1.0, 0.0);
    for (int i = 0; i < 10 && rt_game3d_character_controller_grounded(controller) == 0; ++i)
        rt_game3d_character_controller_update(controller, input, camera, 0.05);
    rt_canvas3d_push_synthetic_key(canvas, rt_game3d_key_w(), 1);
    rt_canvas3d_push_synthetic_key(canvas, rt_game3d_key_lshift(), 1);
    rt_canvas3d_advance_synthetic_frame(canvas);
    rt_game3d_input_update(input);
    void *before_shift = rt_game3d_entity_position(player);
    rt_game3d_character_controller_update(controller, input, camera, 0.1);
    void *after_shift = rt_game3d_entity_position(player);
    double shift_distance = rt_vec3_z(before_shift) - rt_vec3_z(after_shift);
    EXPECT_NEAR(shift_distance,
                w_distance,
                0.001,
                "holding Shift does not reduce walking controller planar speed");

    rt_canvas3d_clear_synthetic_input(canvas);
    rt_game3d_input_update(input);
    rt_game3d_character_controller_teleport(controller, 0.0, 1.0, 0.0);
    for (int i = 0; i < 10 && rt_game3d_character_controller_grounded(controller) == 0; ++i)
        rt_game3d_character_controller_update(controller, input, camera, 0.05);
    rt_canvas3d_push_synthetic_key(canvas, rt_game3d_key_w(), 1);
    rt_canvas3d_push_synthetic_key(canvas, rt_game3d_key_space(), 1);
    rt_canvas3d_advance_synthetic_frame(canvas);
    rt_game3d_input_update(input);
    void *before_space = rt_game3d_entity_position(player);
    rt_game3d_character_controller_update(controller, input, camera, 0.1);
    void *after_space = rt_game3d_entity_position(player);
    double space_distance = rt_vec3_z(before_space) - rt_vec3_z(after_space);
    EXPECT_NEAR(space_distance,
                w_distance,
                0.001,
                "holding Space preserves walking controller planar speed while jumping");
    EXPECT_TRUE(rt_vec3_y(after_space) > rt_vec3_y(before_space),
                "Space still launches the character upward");

    rt_canvas3d_clear_synthetic_input(canvas);
    rt_game3d_input_update(input);
    rt_game3d_character_controller_teleport(controller, 0.0, 1.0, 0.0);
    auto *input_view = static_cast<Game3DInputTestLayout *>(input);
    input_view->bound_pad = 0;
    input_view->pad_connected = 1;
    input_view->pad_lx = 1.0;
    input_view->pad_ly = 0.0;
    void *before_pad = rt_game3d_entity_position(player);
    rt_game3d_character_controller_update(controller, input, camera, 0.1);
    void *after_pad = rt_game3d_entity_position(player);
    EXPECT_TRUE(rt_vec3_x(after_pad) > rt_vec3_x(before_pad) + 0.05,
                "bound left-stick input drives the walking character laterally");
    rt_game3d_input_bind_pad(input, -1);

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_phase3_material_presets_and_prefabs() {
    TEST("Game3D material presets and prefab factories create usable runtime objects");
    void *plastic = rt_game3d_materials_plastic(0.8, 0.2, 0.1);
    void *metal = rt_game3d_materials_metal(0.7, 0.72, 0.75);
    void *rubber = rt_game3d_materials_rubber(0.05, 0.05, 0.05);
    void *glass = rt_game3d_materials_glass(0.2, 0.6, 0.8, 0.35);
    void *emissive = rt_game3d_materials_emissive(0.8, 0.3, 0.1, 2.5);
    void *unlit = rt_game3d_materials_unlit(0.1, 0.9, 0.4);
    void *pixels = rt_pixels_new(2, 2);
    void *textured = rt_game3d_materials_from_albedo_map(pixels);

    EXPECT_TRUE(plastic != nullptr, "Plastic returns Material3D");
    EXPECT_EQ_INT(rt_material3d_get_shading_model(plastic),
                  rt_game3d_shading_model_pbr(),
                  "Plastic uses PBR shading");
    EXPECT_NEAR(rt_material3d_get_metallic(plastic), 0.0, 0.0001, "Plastic metallic");
    EXPECT_NEAR(rt_material3d_get_metallic(metal), 1.0, 0.0001, "Metal metallic");
    EXPECT_TRUE(rt_material3d_get_roughness(rubber) > 0.80, "Rubber is rough");
    EXPECT_EQ_INT(rt_material3d_get_alpha_mode(glass),
                  rt_game3d_alpha_mode_blend(),
                  "Glass uses blend alpha");
    EXPECT_NEAR(rt_material3d_get_alpha(glass), 0.35, 0.0001, "Glass alpha");
    EXPECT_TRUE(rt_material3d_get_double_sided(glass) != 0, "Glass is double-sided");
    EXPECT_EQ_INT(rt_material3d_get_shading_model(emissive),
                  rt_game3d_shading_model_emissive(),
                  "Emissive uses emissive shading");
    EXPECT_NEAR(rt_material3d_get_emissive_intensity(emissive), 2.5, 0.0001, "Emissive intensity");
    EXPECT_TRUE(rt_material3d_get_unlit(unlit) != 0, "Unlit marks material unlit");
    EXPECT_TRUE(textured != nullptr, "FromAlbedoMap returns Material3D");

    void *box = rt_game3d_prefab_box(1.5, plastic);
    void *box_xyz = rt_game3d_prefab_box_xyz(1.0, 2.0, 3.0, metal);
    void *sphere = rt_game3d_prefab_sphere(0.75, 16, rubber);
    void *cylinder = rt_game3d_prefab_cylinder(0.5, 2.0, 16, glass);
    void *plane = rt_game3d_prefab_plane(4.0, 5.0, unlit);
    void *ground = rt_game3d_prefab_ground(10.0, plastic);

    EXPECT_TRUE(rt_game3d_entity_get_mesh(box) != nullptr, "Box prefab has mesh");
    EXPECT_EQ_INT(rt_mesh3d_get_triangle_count(rt_game3d_entity_get_mesh(box)),
                  12,
                  "Box prefab uses box mesh");
    EXPECT_TRUE(rt_game3d_entity_get_material(box) == plastic,
                "Box prefab retains caller material");
    EXPECT_TRUE(rt_game3d_entity_get_mesh(box_xyz) != nullptr, "BoxXYZ prefab has mesh");
    EXPECT_TRUE(rt_mesh3d_get_triangle_count(rt_game3d_entity_get_mesh(sphere)) > 0,
                "Sphere prefab has triangles");
    EXPECT_TRUE(rt_mesh3d_get_triangle_count(rt_game3d_entity_get_mesh(cylinder)) > 0,
                "Cylinder prefab has triangles");
    EXPECT_EQ_INT(rt_mesh3d_get_triangle_count(rt_game3d_entity_get_mesh(plane)),
                  2,
                  "Plane prefab is a quad");
    EXPECT_EQ_INT(rt_game3d_entity_get_layer(ground),
                  rt_game3d_layers_world(),
                  "Ground prefab uses world layer");

    void *cylinder_low = rt_game3d_prefab_cylinder(0.5, 1.0, -1, plastic);
    void *cylinder_fallback = rt_game3d_prefab_cylinder(0.5, 1.0, 24, plastic);
    void *cylinder_high = rt_game3d_prefab_cylinder(0.5, 1.0, INT64_MAX, plastic);
    void *cylinder_cap = rt_game3d_prefab_cylinder(0.5, 1.0, 256, plastic);
    EXPECT_EQ_INT(rt_mesh3d_get_triangle_count(rt_game3d_entity_get_mesh(cylinder_low)),
                  rt_mesh3d_get_triangle_count(rt_game3d_entity_get_mesh(cylinder_fallback)),
                  "low segment requests use the bounded cylinder fallback");
    EXPECT_EQ_INT(rt_mesh3d_get_triangle_count(rt_game3d_entity_get_mesh(cylinder_high)),
                  rt_mesh3d_get_triangle_count(rt_game3d_entity_get_mesh(cylinder_cap)),
                  "oversized segment requests clamp to the mesh budget");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_prefab_box(1.0, pixels); }, "material"),
                "prefabs reject a wrong-class material before construction");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_materials_from_albedo_map(plastic); }, "Pixels"),
        "FromAlbedoMap rejects non-Pixels objects");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_entity_of(plastic, nullptr); }, "mesh"),
                "Entity3D.Of rejects a wrong-class mesh before allocating an entity");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_entity_of(nullptr, pixels); }, "material"),
        "Entity3D.Of rejects a wrong-class material before allocating an entity");
    void *undersized_material = rt_obj_new_i64(RT_G3D_MATERIAL3D_CLASS_ID, 1);
    void *undersized_mesh = rt_obj_new_i64(RT_G3D_MESH3D_CLASS_ID, 1);
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_prefab_box(1.0, undersized_material); },
                                     "material"),
                "prefabs reject undersized Material3D class-id spoofs before mesh allocation");
    EXPECT_TRUE(
        expect_trap_contains([&] { (void)rt_game3d_entity_of(undersized_mesh, nullptr); }, "mesh"),
        "Entity3D.Of rejects undersized Mesh3D class-id spoofs");
    EXPECT_TRUE(expect_trap_contains(
                    [&] { (void)rt_game3d_entity_of(nullptr, undersized_material); }, "material"),
                "Entity3D.Of rejects undersized Material3D class-id spoofs");
    EXPECT_NEAR(rt_material3d_get_alpha(undersized_material),
                1.0,
                0.0,
                "Material3D accessors reject undersized payloads before field reads");
    void *undersized_objects[] = {undersized_material, undersized_mesh};
    for (void *undersized : undersized_objects) {
        if (undersized && rt_obj_release_check0(undersized))
            rt_obj_free(undersized);
    }
    PASS();
}

static bool test_phase3_world_presets_environment_and_debug() {
    TEST("Game3D lighting, quality, environment, post-FX, and debug helpers compose");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Unit Presets"), 96, 72);
    void *canvas = rt_game3d_world_get_canvas(world);

    rt_game3d_lighting_studio(world);
    EXPECT_EQ_INT(rt_canvas3d_get_light_count(canvas), 2, "Studio lighting installs two lights");

    void *sun = rt_vec3_new(-0.2, -1.0, -0.3);
    rt_game3d_lighting_outdoor(world, sun);
    EXPECT_EQ_INT(rt_canvas3d_get_light_count(canvas), 1, "Outdoor lighting installs one sun");
    rt_game3d_lighting_night(world);
    EXPECT_EQ_INT(rt_canvas3d_get_light_count(canvas), 2, "Night lighting installs moon and fill");
    rt_game3d_lighting_interior(world);
    EXPECT_EQ_INT(rt_canvas3d_get_light_count(canvas), 2, "Interior lighting installs key and rim");
    rt_game3d_lighting_clear(world);
    EXPECT_EQ_INT(rt_canvas3d_get_light_count(canvas), 0, "Lighting.Clear removes lights");

    rt_game3d_quality_apply(world, rt_game3d_quality_cinematic());
    EXPECT_EQ_INT(rt_canvas3d_get_quality_requested(canvas),
                  rt_game3d_quality_cinematic(),
                  "Quality.Apply records requested level");
    EXPECT_TRUE(rt_canvas3d_get_quality_active(canvas) >= rt_game3d_quality_performance() &&
                    rt_canvas3d_get_quality_active(canvas) <= rt_game3d_quality_cinematic(),
                "Quality.Apply leaves a valid active level after fallback");

    rt_game3d_postfx_cinematic(world);
    void *fx = rt_game3d_effects_get_postfx(rt_game3d_world_get_effects(world));
    EXPECT_TRUE(fx != nullptr, "PostFX.Cinematic installs a chain");
    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) >= 4, "PostFX.Cinematic has visible effects");
    vgfx3d_postfx_chain_t chain = {};
    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) != 0, "PostFX.Cinematic exports a chain");
    int found_subtle_vignette = 0;
    for (int32_t i = 0; i < chain.effect_count; i++) {
        if (chain.effects[i].type == VGFX3D_POSTFX_EFFECT_VIGNETTE &&
            chain.effects[i].snapshot.vignette_radius >= 0.95f &&
            chain.effects[i].snapshot.vignette_softness >= 0.25f) {
            found_subtle_vignette = 1;
        }
    }
    vgfx3d_postfx_chain_free(&chain);
    EXPECT_TRUE(found_subtle_vignette != 0,
                "PostFX.Cinematic keeps vignette subtle enough for playable demos");
    rt_game3d_postfx_crisp(world);
    fx = rt_game3d_effects_get_postfx(rt_game3d_world_get_effects(world));
    EXPECT_TRUE(fx != nullptr, "PostFX.Crisp installs a chain");
    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) >= 2, "PostFX.Crisp has visible effects");
    rt_game3d_postfx_none(world);
    fx = rt_game3d_effects_get_postfx(rt_game3d_world_get_effects(world));
    EXPECT_TRUE(fx != nullptr && rt_postfx3d_get_enabled(fx) == 0, "PostFX.None disables chain");

    int64_t nodes_before = rt_scene3d_get_node_count(rt_game3d_world_get_scene(world));
    void *env = rt_game3d_environment_outdoor(world);
    EXPECT_TRUE(env != nullptr, "Environment.Outdoor returns EnvHandle");
    EXPECT_TRUE(rt_scene3d_get_node_count(rt_game3d_world_get_scene(world)) > nodes_before,
                "Environment.Outdoor spawns terrain");
    EXPECT_TRUE(rt_world3d_body_count(rt_game3d_world_get_physics(world)) >= 1,
                "Environment terrain gets a static body");
    rt_game3d_env_handle_with_terrain(env, 48.0, -0.05);
    rt_game3d_env_handle_with_water(env, 0.05);
    rt_game3d_env_handle_with_fog(env, 5.0, 50.0);
    EXPECT_TRUE(rt_scene3d_get_node_count(rt_game3d_world_get_scene(world)) > nodes_before + 1,
                "EnvHandle.withWater spawns water");
    {
        void *water = rt_game3d_world_find_entity(world, rt_const_cstr("Environment Water"));
        rt_mesh3d *water_mesh = (rt_mesh3d *)rt_game3d_entity_get_mesh(water);
        EXPECT_TRUE(water_mesh != nullptr && water_mesh->vertex_count > 0,
                    "Environment water owns a mesh");
        float min_x = water_mesh->vertices[0].pos[0];
        float max_x = water_mesh->vertices[0].pos[0];
        float min_z = water_mesh->vertices[0].pos[2];
        float max_z = water_mesh->vertices[0].pos[2];
        for (uint32_t i = 1; i < water_mesh->vertex_count; ++i) {
            if (water_mesh->vertices[i].pos[0] < min_x)
                min_x = water_mesh->vertices[i].pos[0];
            if (water_mesh->vertices[i].pos[0] > max_x)
                max_x = water_mesh->vertices[i].pos[0];
            if (water_mesh->vertices[i].pos[2] < min_z)
                min_z = water_mesh->vertices[i].pos[2];
            if (water_mesh->vertices[i].pos[2] > max_z)
                max_z = water_mesh->vertices[i].pos[2];
        }
        EXPECT_NEAR(max_x - min_x, 48.0, 0.001, "water width follows terrain size");
        EXPECT_NEAR(max_z - min_z, 48.0, 0.001, "water depth follows terrain size");
    }
    EXPECT_TRUE(rt_game3d_environment_sunset(world) != nullptr,
                "Environment.Sunset returns EnvHandle");
    EXPECT_TRUE(rt_game3d_environment_overcast(world) != nullptr,
                "Environment.Overcast returns EnvHandle");
    EXPECT_TRUE(rt_game3d_environment_night(world) != nullptr,
                "Environment.Night returns EnvHandle");

    rt_game3d_debug_show_overlay(world, 1);
    rt_game3d_debug_draw_axes(world, rt_vec3_new(0.0, 0.0, 0.0), 1.5);
    rt_game3d_debug_draw_physics(world, 1);
    rt_game3d_debug_draw_camera_info(world, 1);
    rt_game3d_debug_draw_capabilities(world, 1);

    rt_game3d_world_begin_frame(world);
    rt_game3d_world_draw_scene(world);
    rt_game3d_world_draw_effects(world);
    rt_game3d_world_end_scene(world);
    void *final_pixels = rt_game3d_world_capture_final_frame(world);
    EXPECT_TRUE(final_pixels != nullptr, "debug overlay finalizes into captured Pixels");
    EXPECT_EQ_INT(rt_pixels_width(final_pixels),
                  rt_canvas3d_get_window_width(canvas),
                  "debug final frame width");
    EXPECT_EQ_INT(rt_pixels_height(final_pixels),
                  rt_canvas3d_get_window_height(canvas),
                  "debug final frame height");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_phase4_assets3d_model_templates() {
    TEST("Assets3D loads models and caches SceneTemplate objects");
    rt_string path =
        test_fixture_path("src/tests/fixtures/runtime/assets/gltf/load_asset_triangle.gltf");

    rt_game3d_assets_clear_cache();

    void *model_entity = rt_game3d_assets_load_model(path);
    EXPECT_TRUE(model_entity != nullptr, "Assets3D.LoadEntity returns an entity");
    EXPECT_TRUE(rt_game3d_entity_is_group(model_entity) != 0, "loaded model entity is a group");
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(model_entity)),
                  1,
                  "loaded model preserves imported root children");

    void *asset_entity = rt_game3d_assets_load_model_asset(path);
    EXPECT_TRUE(asset_entity != nullptr, "Assets3D.LoadEntityAsset returns an entity");
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(asset_entity)),
                  1,
                  "asset-loaded model preserves imported root children");

    void *tpl1 = rt_game3d_assets_load_model_template(path);
    void *tpl2 = rt_game3d_assets_load_model_template(path);
    EXPECT_TRUE(tpl1 != nullptr && tpl1 == tpl2, "LoadTemplate reuses cached template");
    EXPECT_TRUE(rt_game3d_model_template_get_model(tpl1) != nullptr,
                "SceneTemplate exposes raw SceneAsset");
    EXPECT_EQ_INT(rt_model3d_get_mesh_count(rt_game3d_model_template_get_model(tpl1)),
                  1,
                  "SceneTemplate retains imported meshes");
    EXPECT_TRUE(rt_game3d_model_template_get_is_asset(tpl1) == 0,
                "filesystem template reports non-asset path");

    void *inst = rt_game3d_model_template_instantiate(tpl1);
    EXPECT_TRUE(inst != nullptr, "SceneTemplate.instantiate returns an Entity3D");
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(inst)),
                  1,
                  "SceneTemplate.instantiate clones the model root subtree");
    EXPECT_EQ_INT(rt_game3d_model_template_get_scene_count(tpl1),
                  1,
                  "SceneTemplate exposes the synthesized default scene for mesh-only glTF assets");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_model_template_get_scene_name(tpl1, 0)),
                            "default") == 0,
                "SceneTemplate exposes synthesized default scene names");
    EXPECT_EQ_INT(rt_game3d_model_template_get_camera_count(tpl1, 0),
                  0,
                  "SceneTemplate reports zero cameras for camera-less scenes");
    EXPECT_TRUE(rt_game3d_model_template_get_camera(tpl1, 0, 0) == nullptr,
                "SceneTemplate returns null for missing scene cameras");
    void *scene_inst = rt_game3d_model_template_instantiate_scene_at(tpl1, 0);
    EXPECT_TRUE(scene_inst != nullptr, "SceneTemplate.instantiateSceneAt returns an Entity3D");
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(scene_inst)),
                  1,
                  "SceneTemplate.instantiateSceneAt clones selected scene roots");
    EXPECT_TRUE(rt_game3d_model_template_instantiate_scene_at(tpl1, 1) == nullptr,
                "SceneTemplate.instantiateSceneAt rejects invalid scene indices");

    const char *camera_template_path = "/tmp/zanna_game3d_template_camera_scene.gltf";
    const char *camera_gltf = "{"
                              "\"asset\":{\"version\":\"2.0\"},"
                              "\"cameras\":[{\"type\":\"perspective\",\"perspective\":{"
                              "\"yfov\":1.0,\"znear\":0.1,\"zfar\":10.0}}],"
                              "\"nodes\":[{\"name\":\"CameraNode\",\"camera\":0}],"
                              "\"scenes\":[{\"name\":\"CameraScene\",\"nodes\":[0]}],"
                              "\"scene\":0"
                              "}";
    EXPECT_TRUE(write_text_file(camera_template_path, camera_gltf),
                "camera SceneTemplate fixture can be written");
    void *camera_tpl = rt_game3d_assets_load_model_template(rt_const_cstr(camera_template_path));
    EXPECT_TRUE(camera_tpl != nullptr, "LoadTemplate parses camera-only glTF scenes");
    EXPECT_EQ_INT(rt_game3d_model_template_get_scene_count(camera_tpl),
                  1,
                  "SceneTemplate exposes camera-only scene count");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_model_template_get_scene_name(camera_tpl, 0)),
                            "CameraScene") == 0,
                "SceneTemplate preserves imported camera scene names");
    EXPECT_EQ_INT(rt_game3d_model_template_get_camera_count(camera_tpl, 0),
                  1,
                  "SceneTemplate exposes imported scene cameras");
    EXPECT_TRUE(rt_game3d_model_template_get_camera(camera_tpl, 0, 0) != nullptr,
                "SceneTemplate.GetCamera returns imported Camera3D handles");
    void *camera_scene_inst = rt_game3d_model_template_instantiate_scene_at(camera_tpl, 0);
    EXPECT_TRUE(camera_scene_inst != nullptr,
                "SceneTemplate.instantiateSceneAt returns an Entity3D");
    EXPECT_EQ_INT(rt_scene_node3d_child_count(rt_game3d_entity_get_node(camera_scene_inst)),
                  1,
                  "SceneTemplate.instantiateSceneAt clones selected scene roots");
    {
        auto *camera_tpl_view = static_cast<Game3DSceneTemplateTestLayout *>(camera_tpl);
        void *saved_model = camera_tpl_view->model;
        void *wrong_model = rt_material3d_new_color(0.3, 0.3, 0.8);
        size_t wrong_model_ref = rt_heap_hdr(wrong_model)->refcnt;
        camera_tpl_view->model = wrong_model;
        EXPECT_TRUE(rt_game3d_model_template_get_model(camera_tpl) == nullptr,
                    "SceneTemplate.getModel ignores wrong-class private model refs");
        EXPECT_EQ_INT(rt_game3d_model_template_get_scene_count(camera_tpl),
                      0,
                      "SceneTemplate sceneCount ignores wrong-class private model refs");
        EXPECT_TRUE(
            std::strcmp(rt_string_cstr(rt_game3d_model_template_get_scene_name(camera_tpl, 0)),
                        "") == 0,
            "SceneTemplate sceneName falls back without a valid private model");
        EXPECT_EQ_INT(rt_game3d_model_template_get_camera_count(camera_tpl, 0),
                      0,
                      "SceneTemplate cameraCount ignores wrong-class private model refs");
        EXPECT_TRUE(rt_game3d_model_template_get_camera(camera_tpl, 0, 0) == nullptr,
                    "SceneTemplate camera lookup ignores wrong-class private model refs");
        EXPECT_TRUE(rt_game3d_model_template_instantiate(camera_tpl) == nullptr,
                    "SceneTemplate.instantiate ignores wrong-class private model refs");
        EXPECT_TRUE(rt_game3d_model_template_instantiate_scene_at(camera_tpl, 0) == nullptr,
                    "SceneTemplate.instantiateSceneAt ignores wrong-class private model refs");
        EXPECT_TRUE(rt_heap_hdr(wrong_model)->refcnt == wrong_model_ref,
                    "SceneTemplate APIs do not release wrong-class private model refs");
        camera_tpl_view->model = saved_model;
    }

    void *asset_tpl = rt_game3d_assets_load_model_template_asset(path);
    EXPECT_TRUE(asset_tpl != nullptr, "LoadTemplateAsset returns a template");
    EXPECT_TRUE(rt_game3d_model_template_get_is_asset(asset_tpl) != 0,
                "asset template records asset resolver path");
    EXPECT_EQ_INT(rt_model3d_get_mesh_count(rt_game3d_model_template_get_model(asset_tpl)),
                  1,
                  "asset template retains imported meshes");

    void *model_handle = rt_game3d_assets_load_model_async(path);
    EXPECT_TRUE(model_handle != nullptr, "LoadEntityAsync returns an AssetHandle3D");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(model_handle) == 0,
                "entity AssetHandle3D schedules work on first ready observation");
    EXPECT_TRUE(wait_asset_ready(model_handle),
                "entity AssetHandle3D completes after worker commit");
    EXPECT_TRUE(std::fabs(rt_game3d_asset_handle_get_progress(model_handle) - 1.0) < 0.000001,
                "entity AssetHandle3D reports complete progress");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(model_handle)), "") ==
                    0,
                "entity AssetHandle3D has no error");
    void *async_entity = rt_game3d_asset_handle_get_entity(model_handle);
    EXPECT_TRUE(async_entity != nullptr, "entity AssetHandle3D exposes the loaded entity");
    EXPECT_TRUE(rt_game3d_entity_is_group(async_entity) != 0,
                "entity AssetHandle3D result is a group entity");
    EXPECT_TRUE(rt_game3d_asset_handle_get_template(model_handle) == nullptr,
                "entity AssetHandle3D has no template result");
    {
        auto *handle_view = static_cast<Game3DAssetHandleTestLayout *>(model_handle);
        void *saved_entity = handle_view->entity;
        void *wrong_entity = rt_material3d_new_color(0.6, 0.1, 0.8);
        size_t wrong_entity_ref = rt_heap_hdr(wrong_entity)->refcnt;
        handle_view->entity = wrong_entity;
        EXPECT_TRUE(rt_game3d_asset_handle_get_entity(model_handle) == nullptr,
                    "AssetHandle3D.getEntity ignores wrong-class private entity refs");
        EXPECT_TRUE(rt_heap_hdr(wrong_entity)->refcnt == wrong_entity_ref,
                    "AssetHandle3D.getEntity does not release wrong-class entity refs");
        handle_view->entity = saved_entity;
    }
    rt_game3d_asset_handle_cancel(model_handle);
    EXPECT_TRUE(rt_game3d_asset_handle_get_entity(model_handle) == async_entity,
                "cancelling a completed AssetHandle3D leaves the entity result intact");

    void *cancelled_handle = rt_game3d_assets_load_model_async(path);
    EXPECT_TRUE(cancelled_handle != nullptr, "LoadEntityAsync returns a cancellable handle");
    rt_game3d_asset_handle_cancel(cancelled_handle);
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(cancelled_handle) != 0,
                "pre-observation cancelled AssetHandle3D becomes terminal");
    EXPECT_TRUE(std::fabs(rt_game3d_asset_handle_get_progress(cancelled_handle) - 1.0) < 0.000001,
                "cancelled AssetHandle3D reports terminal progress");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(cancelled_handle)),
                            "cancelled") == 0,
                "cancelled AssetHandle3D exposes cancellation error text");
    EXPECT_TRUE(rt_game3d_asset_handle_get_entity(cancelled_handle) == nullptr,
                "cancelled AssetHandle3D has no entity result");

    const char *missing_path = "/tmp/zanna_game3d_missing_async_model.gltf";
    std::remove(missing_path);
    rt_string missing_path_s = rt_string_from_bytes(missing_path, std::strlen(missing_path));
    void *missing_entity_handle = rt_game3d_assets_load_model_async(missing_path_s);
    EXPECT_TRUE(missing_entity_handle != nullptr,
                "LoadEntityAsync returns a handle for a missing filesystem path");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(missing_entity_handle) == 0,
                "missing-path AssetHandle3D schedules worker validation");
    EXPECT_TRUE(wait_asset_ready(missing_entity_handle),
                "missing-path AssetHandle3D becomes terminal after worker validation");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(missing_entity_handle)),
                            "failed to load model") == 0,
                "missing-path AssetHandle3D exposes the worker load error");
    EXPECT_TRUE(std::fabs(rt_game3d_asset_handle_get_progress(missing_entity_handle) - 1.0) <
                    0.000001,
                "missing-path AssetHandle3D reports terminal progress");
    EXPECT_TRUE(rt_game3d_asset_handle_get_entity(missing_entity_handle) == nullptr,
                "missing-path AssetHandle3D has no entity result");
    rt_string_unref(missing_path_s);

    rt_string missing_asset_s = rt_string_from_bytes("zanna/missing/async_model_template.gltf", 39);
    void *missing_asset_handle = rt_game3d_assets_load_model_template_asset_async(missing_asset_s);
    EXPECT_TRUE(missing_asset_handle != nullptr,
                "LoadTemplateAssetAsync returns a handle for a missing asset path");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(missing_asset_handle) == 0,
                "missing-asset AssetHandle3D schedules worker validation");
    EXPECT_TRUE(wait_asset_ready(missing_asset_handle),
                "missing-asset AssetHandle3D becomes terminal after worker validation");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(missing_asset_handle)),
                            "failed to load model asset") == 0,
                "missing-asset AssetHandle3D exposes the worker load error");
    EXPECT_TRUE(rt_game3d_asset_handle_get_template(missing_asset_handle) == nullptr,
                "missing-asset AssetHandle3D has no template result");
    rt_string_unref(missing_asset_s);

    const char *corrupt_path = "/tmp/zanna_game3d_corrupt_async_model.gltf";
    EXPECT_TRUE(write_text_file(corrupt_path, "not valid json"),
                "test can write corrupt async model fixture");
    rt_string corrupt_path_s = rt_string_from_bytes(corrupt_path, std::strlen(corrupt_path));
    void *corrupt_handle = rt_game3d_assets_load_model_async(corrupt_path_s);
    EXPECT_TRUE(corrupt_handle != nullptr,
                "LoadEntityAsync returns a handle for a corrupt filesystem payload");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(corrupt_handle) == 0,
                "corrupt AssetHandle3D schedules worker validation");
    EXPECT_TRUE(wait_asset_ready(corrupt_handle),
                "corrupt AssetHandle3D becomes terminal after worker commit");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(corrupt_handle)),
                            "invalid glTF JSON document") == 0,
                "corrupt AssetHandle3D preserves the specific parser diagnostic");
    EXPECT_TRUE(rt_game3d_asset_handle_get_entity(corrupt_handle) == nullptr,
                "corrupt AssetHandle3D has no entity result");
    rt_string_unref(corrupt_path_s);
    std::remove(corrupt_path);

    const char *corrupt_texture_path = "/tmp/zanna_game3d_corrupt_async_texture_model.gltf";
    std::string corrupt_texture_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"images\":[{\"uri\":\"data:image/png;base64,AAAA\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]"
        "}";
    EXPECT_TRUE(write_text_file(corrupt_texture_path, corrupt_texture_json.c_str()),
                "test can write corrupt async texture fixture");
    rt_string corrupt_texture_s =
        rt_string_from_bytes(corrupt_texture_path, std::strlen(corrupt_texture_path));
    void *corrupt_texture_handle = rt_game3d_assets_load_model_async(corrupt_texture_s);
    EXPECT_TRUE(corrupt_texture_handle != nullptr,
                "LoadEntityAsync returns a handle for a corrupt required texture payload");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(corrupt_texture_handle) == 0,
                "corrupt texture AssetHandle3D schedules worker validation");
    EXPECT_TRUE(wait_asset_ready(corrupt_texture_handle),
                "corrupt texture AssetHandle3D becomes terminal after worker preload");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(corrupt_texture_handle)),
                    "invalid glTF image payload") == 0,
        "corrupt texture AssetHandle3D exposes the worker image error");
    EXPECT_TRUE(rt_game3d_asset_handle_get_entity(corrupt_texture_handle) == nullptr,
                "corrupt texture AssetHandle3D has no entity result");
    rt_string_unref(corrupt_texture_s);
    std::remove(corrupt_texture_path);

    const char *unsupported_path = "/tmp/zanna_game3d_unsupported_async_model.txt";
    EXPECT_TRUE(write_text_file(unsupported_path, "not a model"),
                "test can write unsupported async model fixture");
    rt_string unsupported_path_s =
        rt_string_from_bytes(unsupported_path, std::strlen(unsupported_path));
    void *unsupported_handle = rt_game3d_assets_load_model_template_async(unsupported_path_s);
    EXPECT_TRUE(unsupported_handle != nullptr,
                "LoadTemplateAsync returns a handle for an unsupported extension");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(unsupported_handle) != 0,
                "unsupported-extension AssetHandle3D becomes terminal on observation");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(unsupported_handle)),
                            "unsupported file extension") == 0,
                "unsupported-extension AssetHandle3D exposes a preflight load error");
    EXPECT_TRUE(rt_game3d_asset_handle_get_template(unsupported_handle) == nullptr,
                "unsupported-extension AssetHandle3D has no template result");
    rt_string_unref(unsupported_path_s);
    std::remove(unsupported_path);

    // ADR 0182: the canonical .scene3d extension passes async preflight and
    // completes through the same sync-commit path as the legacy .vscn alias.
    const char *canonical_scene_path = "/tmp/zanna_game3d_async_canonical.scene3d";
    {
        void *canonical_scene = rt_scene3d_new();
        void *canonical_node = rt_scene_node3d_new();
        EXPECT_TRUE(canonical_scene != nullptr && canonical_node != nullptr,
                    "test can allocate the canonical .scene3d fixture");
        rt_scene_node3d_set_name(canonical_node, rt_const_cstr("canonical"));
        rt_scene3d_add(canonical_scene, canonical_node);
        EXPECT_TRUE(rt_scene3d_save(canonical_scene, rt_const_cstr(canonical_scene_path)) == 1,
                    "test can write the canonical .scene3d fixture");
    }
    rt_string canonical_scene_path_s =
        rt_string_from_bytes(canonical_scene_path, std::strlen(canonical_scene_path));
    void *canonical_handle = rt_game3d_assets_load_model_template_async(canonical_scene_path_s);
    EXPECT_TRUE(canonical_handle != nullptr,
                "LoadTemplateAsync returns a handle for a canonical .scene3d path");
    EXPECT_TRUE(wait_asset_ready(canonical_handle),
                ".scene3d AssetHandle3D completes through the async model path");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(canonical_handle)), "") == 0,
        ".scene3d AssetHandle3D reports no preflight or load error");
    EXPECT_TRUE(rt_game3d_asset_handle_get_template(canonical_handle) != nullptr,
                ".scene3d AssetHandle3D exposes the loaded template result");
    rt_string_unref(canonical_scene_path_s);
    std::remove(canonical_scene_path);

    const char *sync_only_path = "/tmp/zanna_game3d_async_sync_only_model.obj";
    EXPECT_TRUE(write_text_file(sync_only_path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"),
                "test can write sync-only OBJ model fixture");
    rt_string sync_only_path_s = rt_string_from_bytes(sync_only_path, std::strlen(sync_only_path));
    void *sync_only_handle = rt_game3d_assets_load_model_template_async(sync_only_path_s);
    EXPECT_TRUE(sync_only_handle != nullptr,
                "LoadTemplateAsync returns a handle for an OBJ model format");
    EXPECT_TRUE(wait_asset_ready(sync_only_handle),
                "OBJ AssetHandle3D completes through the async model path");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(sync_only_handle)), "") == 0,
        "OBJ AssetHandle3D has no async format policy error");
    void *sync_only_template = rt_game3d_asset_handle_get_template(sync_only_handle);
    EXPECT_TRUE(sync_only_template != nullptr,
                "OBJ AssetHandle3D exposes the loaded template result");
    {
        auto *handle_view = static_cast<Game3DAssetHandleTestLayout *>(sync_only_handle);
        void *saved_template = handle_view->model_template;
        void *wrong_template = rt_material3d_new_color(0.2, 0.8, 0.2);
        size_t wrong_template_ref = rt_heap_hdr(wrong_template)->refcnt;
        handle_view->model_template = wrong_template;
        EXPECT_TRUE(rt_game3d_asset_handle_get_template(sync_only_handle) == nullptr,
                    "AssetHandle3D.getTemplate ignores wrong-class private template refs");
        rt_game3d_assets_evict(sync_only_handle);
        EXPECT_TRUE(rt_heap_hdr(wrong_template)->refcnt == wrong_template_ref,
                    "AssetHandle3D template getter/evict does not release wrong-class refs");
        handle_view->model_template = saved_template;
    }
    rt_string_unref(sync_only_path_s);
    std::remove(sync_only_path);

    const char *stl_path = "/tmp/zanna_game3d_async_stl_model.stl";
    const char *stl = "solid tri\n"
                      "facet normal 0 0 1\n"
                      "  outer loop\n"
                      "    vertex 0 0 0\n"
                      "    vertex 1 0 0\n"
                      "    vertex 0 1 0\n"
                      "  endloop\n"
                      "endfacet\n"
                      "endsolid tri\n";
    EXPECT_TRUE(write_text_file(stl_path, stl), "test can write async STL model fixture");
    rt_string stl_path_s = rt_string_from_bytes(stl_path, std::strlen(stl_path));
    void *stl_handle = rt_game3d_assets_load_model_template_async(stl_path_s);
    EXPECT_TRUE(stl_handle != nullptr,
                "LoadTemplateAsync returns a handle for an STL model format");
    EXPECT_TRUE(wait_asset_ready(stl_handle), "STL AssetHandle3D completes through async load");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(stl_handle)), "") == 0,
                "STL AssetHandle3D has no async format policy error");
    EXPECT_TRUE(rt_game3d_asset_handle_get_template(stl_handle) != nullptr,
                "STL AssetHandle3D exposes the loaded template result");
    rt_string_unref(stl_path_s);
    std::remove(stl_path);

    void *asset_model_handle = rt_game3d_assets_load_model_asset_async(path);
    EXPECT_TRUE(asset_model_handle != nullptr, "LoadEntityAssetAsync returns an AssetHandle3D");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(asset_model_handle) == 0,
                "asset entity AssetHandle3D schedules work on first ready observation");
    EXPECT_TRUE(wait_asset_ready(asset_model_handle),
                "asset entity AssetHandle3D completes after worker commit");
    EXPECT_TRUE(rt_game3d_asset_handle_get_entity(asset_model_handle) != nullptr,
                "asset entity AssetHandle3D exposes the loaded entity");

    void *template_handle = rt_game3d_assets_load_model_template_async(path);
    EXPECT_TRUE(template_handle != nullptr, "LoadTemplateAsync returns an AssetHandle3D");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(template_handle) != 0,
                "cached template AssetHandle3D completes on first ready observation");
    EXPECT_TRUE(std::fabs(rt_game3d_asset_handle_get_progress(template_handle) - 1.0) < 0.000001,
                "template AssetHandle3D reports complete progress");
    EXPECT_TRUE(rt_game3d_asset_handle_get_template(template_handle) == tpl1,
                "template AssetHandle3D shares the cached SceneTemplate");
    EXPECT_TRUE(rt_game3d_asset_handle_get_entity(template_handle) == nullptr,
                "template AssetHandle3D has no entity result");

    void *asset_template_handle = rt_game3d_assets_load_model_template_asset_async(path);
    EXPECT_TRUE(asset_template_handle != nullptr,
                "LoadTemplateAssetAsync returns an AssetHandle3D");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(asset_template_handle) != 0,
                "asset template AssetHandle3D is ready");
    EXPECT_TRUE(rt_game3d_model_template_get_is_asset(
                    rt_game3d_asset_handle_get_template(asset_template_handle)) != 0,
                "asset template AssetHandle3D exposes an asset-backed template");

    rt_game3d_assets_evict(model_handle);
    EXPECT_TRUE(rt_game3d_asset_handle_get_entity(model_handle) == async_entity,
                "evicting an entity AssetHandle3D is a stable no-op");

    rt_game3d_assets_evict(template_handle);
    EXPECT_TRUE(rt_game3d_asset_handle_get_template(template_handle) == tpl1,
                "evicting a template handle keeps the handle result alive");
    void *tpl_after_evict = rt_game3d_assets_load_model_template(path);
    EXPECT_TRUE(tpl_after_evict != nullptr && tpl_after_evict != tpl1,
                "Assets3D.Evict drops the shared cached SceneTemplate entry");

    rt_game3d_assets_set_residency_budget(0);
    void *budget_tpl1 = rt_game3d_assets_load_model_template(path);
    void *budget_tpl2 = rt_game3d_assets_load_model_template(path);
    EXPECT_TRUE(budget_tpl1 != nullptr && budget_tpl2 != nullptr && budget_tpl1 != budget_tpl2,
                "zero residency budget evicts cached templates after each load");
    rt_game3d_assets_set_residency_budget(-1);

    rt_game3d_assets_clear_cache();
    void *preload_world = rt_game3d_world_new(rt_const_cstr("Game3D Async Preload Unit"), 64, 48);
    EXPECT_TRUE(preload_world != nullptr, "World3D.New supports async preload commit draining");
    rt_game3d_assets_preload(path);
    for (int i = 0; i < 100; ++i) {
        rt_game3d_world_step_simulation(preload_world, 1.0 / 60.0);
        rt_thread_sleep(5);
    }
    void *preloaded_handle = rt_game3d_assets_load_model_template_async(path);
    EXPECT_TRUE(preloaded_handle != nullptr, "LoadTemplateAsync returns a handle after Preload");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(preloaded_handle) != 0,
                "Assets3D.Preload warms the template cache through world commit draining");
    EXPECT_TRUE(rt_game3d_asset_handle_get_template(preloaded_handle) != nullptr,
                "Assets3D.Preload publishes a cached SceneTemplate");

    const char *corrupt_preload_path = "/tmp/zanna_game3d_corrupt_preload_model.gltf";
    EXPECT_TRUE(write_text_file(corrupt_preload_path, "not valid json"),
                "test can write corrupt preload fixture");
    rt_string corrupt_preload_s =
        rt_string_from_bytes(corrupt_preload_path, std::strlen(corrupt_preload_path));
    rt_game3d_assets_preload(corrupt_preload_s);
    for (int i = 0; i < 20; ++i) {
        rt_game3d_world_step_simulation(preload_world, 1.0 / 60.0);
        rt_thread_sleep(5);
    }
    rt_string_unref(corrupt_preload_s);
    std::remove(corrupt_preload_path);

    rt_game3d_assets_clear_cache();
    rt_game3d_assets_preload(path);
    rt_game3d_assets_clear_cache();
    for (int i = 0; i < 160; ++i) {
        rt_game3d_world_step_simulation(preload_world, 1.0 / 60.0);
        rt_thread_sleep(5);
    }
    void *stale_preload_handle = rt_game3d_assets_load_model_template_async(path);
    EXPECT_TRUE(stale_preload_handle != nullptr,
                "LoadTemplateAsync returns a handle after stale preload clear");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(stale_preload_handle) == 0,
                "Assets3D.ClearCache prevents pre-clear Preload from repopulating cache");
    EXPECT_TRUE(wait_asset_ready(stale_preload_handle),
                "post-clear template handle can still load normally");

    rt_game3d_world_destroy(preload_world);
    rt_game3d_assets_clear_cache();
    PASS();
}

static bool test_phase4_assets3d_stale_async_publish_revalidates_generation() {
    TEST("Assets3D async template publish revalidates cache generation");
    const char *path = "/tmp/zanna_game3d_stale_async_publish.gltf";
    EXPECT_TRUE(write_game3d_embedded_triangle_gltf(path, 0.0f),
                "test can write pre-clear async model fixture");

    rt_game3d_diagnostics_reset();
    rt_game3d_assets_set_residency_budget(-1);
    rt_game3d_assets_set_upload_budget(-1);
    rt_game3d_assets_clear_cache();

    rt_string path_s = rt_string_from_bytes(path, std::strlen(path));
    void *handle = rt_game3d_assets_load_model_template_async(path_s);
    EXPECT_TRUE(handle != nullptr, "LoadTemplateAsync returns a stale-race handle");

    Game3DStaleAsyncPublishHookState hook_state = {path, 0, 0};
    rt_game3d_test_set_async_cache_publish_hook(game3d_test_clear_cache_during_async_publish,
                                                &hook_state);
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(handle) == 0,
                "stale-race handle starts worker loading");
    EXPECT_TRUE(wait_asset_ready(handle, 400),
                "stale-race handle re-enqueues and completes after cache invalidation");
    rt_game3d_test_set_async_cache_publish_hook(nullptr, nullptr);

    EXPECT_EQ_INT(hook_state.calls, 1, "cache publish hook runs exactly once");
    EXPECT_EQ_INT(hook_state.rewrite_ok, 1, "cache publish hook rewrites the model fixture");
    EXPECT_EQ_INT(rt_game3d_diagnostics_get_stale_async_loads_dropped(),
                  1,
                  "stale async publish increments diagnostics");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_asset_handle_get_error(handle)), "") == 0,
                "re-enqueued stale-race handle completes without error");
    void *model_template = rt_game3d_asset_handle_get_template(handle);
    EXPECT_TRUE(model_template != nullptr, "re-enqueued stale-race handle exposes a template");
    EXPECT_NEAR(game3d_template_aabb_min_x(model_template),
                5.0,
                0.0001,
                "stale pre-clear model is not published into the fresh cache");

    rt_string_unref(path_s);
    rt_game3d_assets_clear_cache();
    rt_game3d_diagnostics_reset();
    std::remove(path);
    PASS();
}

static bool test_phase4_assets3d_resident_bytes_returns_to_baseline() {
    TEST("Assets3D resident byte telemetry returns to baseline after churn");
    rt_string path =
        test_fixture_path("src/tests/fixtures/runtime/assets/gltf/load_asset_triangle.gltf");

    rt_game3d_assets_set_residency_budget(-1);
    rt_game3d_assets_clear_cache();
    EXPECT_EQ_INT(rt_game3d_assets_get_resident_bytes(),
                  0,
                  "resident byte telemetry starts at baseline after clear");

    for (int i = 0; i < 3; ++i) {
        void *blocking_template = rt_game3d_assets_load_model_template(path);
        EXPECT_TRUE(blocking_template != nullptr, "blocking template load succeeds during churn");
        EXPECT_TRUE(rt_game3d_assets_get_resident_bytes() > 0,
                    "blocking template load increments resident byte telemetry");
        rt_game3d_assets_clear_cache();
        EXPECT_EQ_INT(rt_game3d_assets_get_resident_bytes(),
                      0,
                      "clearing after blocking load returns resident bytes to baseline");

        void *async_handle = rt_game3d_assets_load_model_template_async(path);
        EXPECT_TRUE(async_handle != nullptr, "async template load returns a handle during churn");
        EXPECT_TRUE(rt_game3d_asset_handle_get_ready(async_handle) == 0,
                    "async template load is scheduled instead of completed synchronously");
        EXPECT_TRUE(wait_asset_ready(async_handle), "async template load completes during churn");
        EXPECT_TRUE(rt_game3d_asset_handle_get_template(async_handle) != nullptr,
                    "async template handle exposes a cached SceneTemplate");
        EXPECT_TRUE(rt_game3d_assets_get_resident_bytes() > 0,
                    "async template load increments resident byte telemetry");
        rt_game3d_assets_clear_cache();
        EXPECT_EQ_INT(rt_game3d_assets_get_resident_bytes(),
                      0,
                      "clearing after async load returns resident bytes to baseline");
    }

    PASS();
}

static bool test_phase4_assets3d_residency_hint_eviction() {
    TEST("Assets3D residency hints guide template cache eviction");
    const char *near_path = "/tmp/zanna_game3d_residency_near.gltf";
    const char *priority_path = "/tmp/zanna_game3d_residency_priority.gltf";
    const char *far_path = "/tmp/zanna_game3d_residency_far.gltf";

    EXPECT_TRUE(write_game3d_embedded_triangle_gltf(near_path, 0.0f),
                "near residency fixture can be written");
    EXPECT_TRUE(write_game3d_embedded_triangle_gltf(priority_path, 2.0f),
                "priority residency fixture can be written");
    EXPECT_TRUE(write_game3d_embedded_triangle_gltf(far_path, 4.0f),
                "far residency fixture can be written");

    rt_game3d_assets_set_residency_budget(-1);
    rt_game3d_assets_clear_cache();
    void *near_tpl = rt_game3d_assets_load_model_template(rt_const_cstr(near_path));
    void *priority_tpl = rt_game3d_assets_load_model_template(rt_const_cstr(priority_path));
    void *far_tpl = rt_game3d_assets_load_model_template(rt_const_cstr(far_path));
    EXPECT_TRUE(near_tpl != nullptr && priority_tpl != nullptr && far_tpl != nullptr,
                "all residency fixture templates load");
    int64_t total_bytes = rt_game3d_assets_get_resident_bytes();
    EXPECT_TRUE(total_bytes > 3, "three cached fixtures report resident bytes");

    rt_game3d_assets_set_residency_hint(near_tpl, 0.0, 10.0);
    rt_game3d_assets_set_residency_hint(priority_tpl, 5.0, 100000.0);
    rt_game3d_assets_set_residency_hint(far_tpl, 0.0, 1000.0);
    rt_game3d_assets_set_residency_budget(total_bytes - 1);
    EXPECT_TRUE(rt_game3d_assets_get_resident_bytes() < total_bytes,
                "budget pressure evicts one hinted cache entry");

    void *near_again = rt_game3d_assets_load_model_template(rt_const_cstr(near_path));
    EXPECT_TRUE(near_again == near_tpl, "near low-priority template survives farther peer");
    void *priority_again = rt_game3d_assets_load_model_template(rt_const_cstr(priority_path));
    EXPECT_TRUE(priority_again == priority_tpl,
                "high-priority template survives despite large distance");
    void *far_again = rt_game3d_assets_load_model_template(rt_const_cstr(far_path));
    EXPECT_TRUE(far_again != far_tpl, "far low-priority template is evicted first");

    rt_game3d_assets_set_residency_budget(-1);
    rt_game3d_assets_clear_cache();
    std::remove(near_path);
    std::remove(priority_path);
    std::remove(far_path);
    PASS();
}

static bool test_phase4_assets3d_texture_residency_budget() {
    TEST("Assets3D residency budget accounts for texture payloads");
    const char *png_path = "/tmp/zanna_game3d_budget_texture.png";
    const char *gltf_path = "/tmp/zanna_game3d_budget_texture.gltf";
    void *pixels = rt_pixels_new(256, 256);
    EXPECT_TRUE(pixels != nullptr, "budget texture Pixels can be allocated");
    for (int64_t y = 0; y < 256; ++y) {
        for (int64_t x = 0; x < 256; ++x)
            rt_pixels_set(pixels, x, y, 0x66AA44FFll);
    }
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "budget texture PNG can be written");

    std::vector<uint8_t> png_bytes;
    EXPECT_TRUE(game3d_test_read_file_bytes(png_path, png_bytes), "budget texture PNG can be read");
    if (png_bytes.empty())
        FAIL("budget texture PNG is empty");

    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const float texcoords[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_game3d_test_bytes(gltf_buffer, v);
    for (float v : texcoords)
        append_game3d_test_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_game3d_test_bytes(gltf_buffer, v);

    std::string buffer_b64 = game3d_test_base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string image_b64 = game3d_test_base64_encode(png_bytes.data(), png_bytes.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"images\":[{\"uri\":\"data:image/png;base64," +
        image_b64 +
        "\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
        "\"indices\":2,"
        "\"material\":0}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json.c_str()),
                "budgeted textured glTF fixture can be written");

    rt_game3d_assets_clear_cache();
    rt_game3d_assets_set_upload_budget(0);
    void *paused_handle = rt_game3d_assets_load_model_template_async(rt_const_cstr(gltf_path));
    EXPECT_TRUE(paused_handle != nullptr,
                "LoadTemplateAsync returns a handle under a paused upload budget");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(paused_handle) == 0,
                "paused upload budget schedules textured async work");
    for (int i = 0; i < 40; ++i) {
        EXPECT_TRUE(rt_game3d_asset_handle_get_ready(paused_handle) == 0,
                    "zero upload budget keeps positive-cost texture commit pending");
        rt_thread_sleep(5);
    }
    rt_game3d_assets_set_upload_budget(1);
    bool saw_partial_upload_progress = false;
    for (int i = 0; i < 8; ++i) {
        int8_t ready = rt_game3d_asset_handle_get_ready(paused_handle);
        double progress = rt_game3d_asset_handle_get_progress(paused_handle);
        if (progress > 0.0 && progress < 1.0)
            saw_partial_upload_progress = true;
        if (ready != 0)
            break;
        rt_thread_sleep(5);
    }
    EXPECT_TRUE(saw_partial_upload_progress,
                "positive upload budget advances a large texture through partial slices");
    EXPECT_TRUE(wait_asset_ready(paused_handle),
                "positive upload budget drains all texture slices and publishes the template");
    EXPECT_TRUE(rt_game3d_asset_handle_get_template(paused_handle) != nullptr,
                "upload-budgeted async handle exposes the loaded textured template");
    rt_game3d_assets_set_upload_budget(-1);
    rt_game3d_assets_clear_cache();

    rt_game3d_assets_set_residency_budget(4096);
    void *tpl1 = rt_game3d_assets_load_model_template(rt_const_cstr(gltf_path));
    EXPECT_TRUE(tpl1 != nullptr, "first textured template load succeeds");
    void *model1 = rt_game3d_model_template_get_model(tpl1);
    auto *mat = static_cast<rt_material3d *>(rt_model3d_get_material(model1, 0));
    EXPECT_TRUE(mat != nullptr && mat->texture != nullptr, "textured template keeps material map");
    void *decoded_texture = rt_material3d_resolve_texture_pixels(mat->texture);
    EXPECT_TRUE(decoded_texture != nullptr,
                "source-backed budget texture retains its decoded Pixels fallback");
    EXPECT_EQ_INT(rt_pixels_width(decoded_texture), 256, "budget texture width is decoded");
    EXPECT_EQ_INT(rt_pixels_height(decoded_texture), 256, "budget texture height is decoded");

    void *tpl2 = rt_game3d_assets_load_model_template(rt_const_cstr(gltf_path));
    EXPECT_TRUE(tpl2 != nullptr, "second textured template load succeeds");
    EXPECT_TRUE(tpl1 != tpl2,
                "texture resident bytes force zero-headroom budget eviction between loads");

    rt_game3d_assets_set_residency_budget(-1);
    rt_game3d_assets_clear_cache();
    std::remove(gltf_path);
    std::remove(png_path);
    PASS();
}

static bool test_assets3d_loads_packaged_gltf_hierarchy() {
    TEST("Assets3D.LoadEntityAsset preserves packaged glTF hierarchy");
    const char *pack_path = "/tmp/zanna_game3d_packaged_hierarchy.zpak";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    for (float v : positions)
        append_game3d_test_bytes(gltf_buffer, v);

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"buffers/tri.bin\",\"byteLength\":" +
        std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"nodes\":["
        "{\"name\":\"PackRoot\",\"children\":[1]},"
        "{\"name\":\"PackChild\",\"mesh\":0,\"translation\":[1.25,0.0,-0.5]}"
        "],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";

    Game3DTestZpakEntry entries[] = {
        {"assets/models/hierarchy.gltf",
         reinterpret_cast<const uint8_t *>(gltf_json.data()),
         gltf_json.size()},
        {"assets/models/buffers/tri.bin", gltf_buffer.data(), gltf_buffer.size()},
    };
    EXPECT_TRUE(write_game3d_test_zpak(pack_path, entries, 2),
                "Game3D hierarchy asset pack can be written");
    EXPECT_TRUE(rt_asset_mount(rt_const_cstr(pack_path)) == 1,
                "Game3D hierarchy asset pack can mount");

    rt_game3d_assets_clear_cache();
    void *preload_world =
        rt_game3d_world_new(rt_const_cstr("Game3D Packaged Preload Unit"), 64, 48);
    EXPECT_TRUE(preload_world != nullptr,
                "World3D.New supports package-aware preload commit draining");
    rt_game3d_assets_preload_asset(rt_const_cstr("assets/models/hierarchy.gltf"));
    for (int i = 0; i < 100; ++i) {
        rt_game3d_world_step_simulation(preload_world, 1.0 / 60.0);
        rt_thread_sleep(5);
    }
    void *preloaded_asset_handle = rt_game3d_assets_load_model_template_asset_async(
        rt_const_cstr("assets/models/hierarchy.gltf"));
    EXPECT_TRUE(preloaded_asset_handle != nullptr,
                "LoadTemplateAssetAsync returns a handle after PreloadAsset");
    EXPECT_TRUE(rt_game3d_asset_handle_get_ready(preloaded_asset_handle) != 0,
                "Assets3D.PreloadAsset warms the package-aware template cache");
    void *preloaded_asset_template = rt_game3d_asset_handle_get_template(preloaded_asset_handle);
    EXPECT_TRUE(preloaded_asset_template != nullptr,
                "PreloadAsset publishes a cached package-aware SceneTemplate");
    EXPECT_TRUE(rt_game3d_model_template_get_is_asset(preloaded_asset_template) != 0,
                "PreloadAsset cache entries preserve asset-path identity");
    rt_game3d_world_destroy(preload_world);

    void *entity = rt_game3d_assets_load_model_asset(rt_const_cstr("assets/models/hierarchy.gltf"));
    EXPECT_TRUE(entity != nullptr, "LoadEntityAsset loads a packaged glTF hierarchy");
    EXPECT_TRUE(rt_game3d_entity_is_group(entity) != 0, "packaged glTF imports as group entity");
    EXPECT_TRUE(rt_scene_node3d_child_count(rt_game3d_entity_get_node(entity)) > 0,
                "packaged glTF root keeps imported children");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Packaged Hierarchy"), 64, 48);
    rt_game3d_world_spawn(world, entity);
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("PackRoot")) != nullptr,
                "spawned packaged glTF exposes root node by name");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("PackChild")) != nullptr,
                "spawned packaged glTF exposes child node by name");
    rt_game3d_world_run_frames_only(world, 1, 1.0 / 60.0);
    void *pixels = rt_game3d_world_capture_final_frame(world);
    EXPECT_TRUE(pixels != nullptr, "packaged glTF hierarchy leaves a capturable frame");
    EXPECT_EQ_INT(rt_pixels_width(pixels), 64, "packaged hierarchy capture width");
    EXPECT_EQ_INT(rt_pixels_height(pixels), 48, "packaged hierarchy capture height");

    rt_game3d_world_destroy(world);
    rt_game3d_assets_clear_cache();
    rt_asset_unmount(rt_const_cstr(pack_path));
    std::remove(pack_path);
    PASS();
}

static bool test_phase5_world_stream3d_baseline() {
    TEST("WorldStream3D tracks mounted manifests and resident budgets");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D WorldStream Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    EXPECT_TRUE(stream != nullptr, "World3D.stream returns an owned stream handle");
    EXPECT_TRUE(rt_game3d_world_get_stream(world) == stream,
                "World3D.stream is stable across repeated property reads");
    void *standalone_stream = rt_game3d_world_stream_new(world);
    rt_game3d_world_stream_set_async_streaming(standalone_stream,
                                               0); /* blocking: test pins synchronous residency */
    EXPECT_TRUE(standalone_stream != nullptr, "WorldStream3D.New still returns a stream handle");
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_cell_count(stream), 0, "initial cell count is zero");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  0,
                  "initial terrain tile count is zero");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_pending_request_count(stream),
                  0,
                  "initial pending request count is zero");
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_bytes(stream), 0, "initial resident bytes are zero");

    void *center = rt_vec3_new(512.0, 0.0, -256.0);
    rt_game3d_world_stream_set_center(stream, center);
    rt_game3d_world_stream_set_radii(stream, 256.0, 128.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr("assets/world/cells.vscn"));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_cell_count(stream) > 0,
                "mounted cell manifest produces resident cell telemetry");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  0,
                  "terrain count stays zero until terrain manifest is mounted");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_pending_request_count(stream),
                  0,
                  "baseline stream has no pending requests");
    int64_t cell_only_bytes = rt_game3d_world_stream_get_resident_bytes(stream);
    EXPECT_TRUE(cell_only_bytes > 0, "mounted cells produce resident-byte telemetry");
    EXPECT_EQ_INT(rt_game3d_world_get_stream_resident_bytes(world),
                  cell_only_bytes,
                  "World3D.streamResidentBytes wraps owned stream telemetry");

    rt_game3d_world_stream_mount_tiled_terrain(stream, rt_const_cstr("assets/world/terrain.vscn"));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_terrain_tile_count(stream) > 0,
                "mounted terrain manifest produces resident terrain telemetry");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_bytes(stream) > cell_only_bytes,
                "terrain residency adds to resident-byte telemetry");

    rt_game3d_world_stream_set_residency_budget(stream, 0);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  0,
                  "zero stream residency budget evicts cells");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  0,
                  "zero stream residency budget evicts terrain tiles");
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_resident_bytes(stream), 0, "zero stream budget clears bytes");

    rt_game3d_world_stream_set_residency_budget(stream, -1);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_cell_count(stream) > 0,
                "negative stream budget restores unlimited residency");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_terrain_tile_count(stream) > 0,
                "negative stream budget restores terrain residency");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_phase5_world_stream3d_terrain_manifest() {
    TEST("WorldStream3D parses tiled terrain manifests for deterministic residency");
    const char *manifest_path = "/tmp/zanna_game3d_terrain_tiles_manifest.json";
    const char *near_height_path = "/tmp/zanna_game3d_terrain_near.height";
    const char *far_height_path = "/tmp/zanna_game3d_terrain_far.height";
    const char *near_heights = "zanna-heightmap-v1 3 3\n"
                               "0.25 0.35 0.45\n"
                               "0.30 0.40 0.50\n"
                               "0.35 0.45 0.55\n";
    const char *far_heights = "zanna-heightmap-v1 3 3\n"
                              "0.45 0.55 0.65\n"
                              "0.50 0.60 0.70\n"
                              "0.55 0.65 0.75\n";
    const char *manifest = "{\"tiles\":["
                           "{\"name\":\"near\",\"path\":\"terrain/"
                           "near.tile\",\"heightmap\":\"zanna_game3d_terrain_near.height\","
                           "\"center\":[0,0,0],\"radius\":32,\"bytes\":1000,"
                           "\"width\":4,\"depth\":4,\"scale\":[2,3,4]},"
                           "{\"name\":\"far\",\"path\":\"terrain/"
                           "far.tile\",\"heightmap\":\"zanna_game3d_terrain_far.height\","
                           "\"center\":[512,0,0],\"radius\":32,\"bytes\":1000,"
                           "\"width\":8,\"depth\":8,\"scale\":[4,3,8]}"
                           "]}";
    EXPECT_TRUE(write_text_file(near_height_path, near_heights),
                "near terrain height sidecar is writable");
    EXPECT_TRUE(write_text_file(far_height_path, far_heights),
                "far terrain height sidecar is writable");
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "terrain manifest fixture is writable");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Terrain Stream Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_radii(stream, 96.0, 96.0);
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_mount_tiled_terrain(stream, rt_const_cstr(manifest_path));
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  1,
                  "terrain manifest loads the near tile only");
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(world),
                  1,
                  "resident terrain tile creates a heightfield collider body");
    void *near_terrain = rt_game3d_world_stream_get_resident_terrain_tile(stream, 0);
    EXPECT_TRUE(near_terrain != nullptr, "resident terrain tile exposes a Terrain3D payload");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_terrain_tile(stream, 1) == nullptr,
                "out-of-range resident terrain tile query returns null");
    EXPECT_TRUE(
        std::strstr(rt_string_cstr(rt_game3d_world_stream_get_terrain_tile_heightmap(stream, 0)),
                    "zanna_game3d_terrain_near.height") != nullptr,
        "terrain tile heightmap inspection returns the resolved sidecar path");
    EXPECT_NEAR(rt_terrain3d_get_height_at(near_terrain, 1.0, 1.0),
                0.9375,
                0.001,
                "manifest terrain payload applies a resampled relative height sidecar");
    void *near_nav = rt_game3d_world_bake_nav_mesh(world, 0.35, 1.8, 45.0, 0.3);
    EXPECT_TRUE(near_nav != nullptr, "resident terrain tile contributes a nav-bake source");
    EXPECT_TRUE(rt_navmesh3d_get_triangle_count(near_nav) > 0,
                "terrain nav-bake source produces walkable triangles");
    EXPECT_TRUE(rt_navmesh3d_is_walkable(near_nav, rt_vec3_new(0.0, 1.0, 0.0)) != 0,
                "terrain nav-bake source uses streamed tile world placement");
    rt_camera3d_look_at(rt_game3d_world_get_camera(world),
                        rt_vec3_new(0.0, 12.0, 18.0),
                        rt_vec3_new(0.0, 0.0, 0.0),
                        rt_vec3_new(0.0, 1.0, 0.0));
    rt_game3d_world_begin_frame(world);
    rt_game3d_world_draw_scene(world);
    rt_game3d_world_end_scene(world);
    EXPECT_TRUE(rt_game3d_world_get_draw_count(world) > 0,
                "World3D.drawScene renders resident stream terrain tiles");
    int64_t near_bytes = rt_game3d_world_stream_get_resident_bytes(stream);
    EXPECT_TRUE(near_bytes > 1000, "terrain manifest contributes metadata and tile bytes");

    rt_game3d_world_stream_set_center(stream, rt_vec3_new(512.0, 0.0, 0.0));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  1,
                  "terrain manifest unloads distant tiles and loads the new near tile");
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(world),
                  1,
                  "terrain heightfield collider body swaps with tile residency");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_terrain_tile(stream, 0) != nullptr,
                "terrain payload remains available after tile residency changes");

    rt_game3d_world_stream_set_radii(stream, 1024.0, 1024.0);
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(256.0, 0.0, 0.0));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  2,
                  "large terrain stream radius admits both manifest tiles");
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(world),
                  2,
                  "both resident terrain tiles own heightfield collider bodies");
    void *near_again = rt_game3d_world_stream_get_resident_terrain_tile(stream, 0);
    void *far_again = rt_game3d_world_stream_get_resident_terrain_tile(stream, 1);
    EXPECT_TRUE(near_again != nullptr && far_again != nullptr,
                "both heightmapped terrain payloads are resident");
    EXPECT_NEAR(rt_terrain3d_get_height_at(near_again, 6.0, 0.0),
                rt_terrain3d_get_height_at(far_again, 0.0, 0.0),
                0.001,
                "adjacent height sidecars can share a seam edge");

    rt_game3d_world_stream_set_residency_budget(stream, 0);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  0,
                  "zero budget clears manifest terrain tiles");
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(world),
                  0,
                  "zero budget unloads terrain heightfield collider bodies");
    EXPECT_TRUE(rt_game3d_world_bake_nav_mesh(world, 0.35, 1.8, 45.0, 0.3) == nullptr,
                "zero budget unloads terrain nav-bake sources");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_terrain_tile(stream, 0) == nullptr,
                "zero budget releases resident terrain payloads");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_bytes(stream),
                  0,
                  "zero budget clears manifest terrain bytes");

    rt_game3d_world_destroy(world);
    std::remove(manifest_path);
    std::remove(near_height_path);
    std::remove(far_height_path);
    PASS();
}

static bool test_phase5_world_stream3d_terrain_slots_reject_wrong_class_refs() {
    TEST("WorldStream3D terrain tiles reject wrong-class private terrain and source slots");

    const char *manifest_path = "/tmp/zanna_game3d_terrain_wrong_slots_manifest.json";
    const char *manifest = "{\"tiles\":["
                           "{\"name\":\"wrong_slots\",\"path\":\"terrain/wrong_slots.tile\","
                           "\"center\":[0,0,0],\"radius\":32,\"bytes\":1000,"
                           "\"width\":4,\"depth\":4,\"scale\":[2,1,2]}"
                           "]}";
    EXPECT_TRUE(write_text_file(manifest_path, manifest),
                "wrong-slot terrain manifest fixture is writable");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Terrain Wrong Slot Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_radii(stream, 96.0, 96.0);
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_mount_tiled_terrain(stream, rt_const_cstr(manifest_path));
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  1,
                  "fixture loads one terrain tile before corruption");

    auto *stream_view = static_cast<Game3DWorldStreamTestLayout *>(stream);
    EXPECT_TRUE(stream_view->terrain_tiles != nullptr && stream_view->terrain_tile_count == 1,
                "test can access the loaded terrain tile slot");

    rt_game3d_world_stream_set_residency_budget(stream, 0);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  0,
                  "fixture unloads cleanly before slot corruption");

    Game3DStreamTerrainTileTestLayout *tile = &stream_view->terrain_tiles[0];
    void *wrong_terrain = rt_vec3_new(1.0, 2.0, 3.0);
    void *wrong_collider = rt_vec3_new(4.0, 5.0, 6.0);
    void *wrong_nav = rt_vec3_new(7.0, 8.0, 9.0);
    EXPECT_TRUE(wrong_terrain != nullptr && wrong_collider != nullptr && wrong_nav != nullptr,
                "wrong-class test handles allocate");

    tile->terrain = wrong_terrain;
    tile->collider_entity = wrong_collider;
    tile->nav_entity = wrong_nav;
    tile->resident = 1;

    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_resident(stream, 0),
                  0,
                  "terrain tile resident query ignores a wrong-class terrain slot");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_terrain_tile(stream, 0) == nullptr,
                "resident terrain accessor hides wrong-class private terrain");
    rt_game3d_world_begin_frame(world);
    rt_game3d_world_draw_scene(world);
    rt_game3d_world_end_scene(world);

    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_TRUE(tile->terrain == nullptr && tile->collider_entity == nullptr &&
                    tile->nav_entity == nullptr,
                "terrain unload clears wrong-class private slots without retaining them");
    EXPECT_EQ_INT(tile->resident, 0, "terrain unload clears corrupt resident state");

    if (wrong_terrain && rt_obj_release_check0(wrong_terrain))
        rt_obj_free(wrong_terrain);
    if (wrong_collider && rt_obj_release_check0(wrong_collider))
        rt_obj_free(wrong_collider);
    if (wrong_nav && rt_obj_release_check0(wrong_nav))
        rt_obj_free(wrong_nav);

    rt_game3d_world_destroy(world);
    std::remove(manifest_path);
    PASS();
}

static bool test_phase5_world_stream3d_heightmap_resample_preserves_edges() {
    TEST("WorldStream3D terrain heightmap resampling preserves source edges");
    const char *manifest_path = "/tmp/zanna_game3d_terrain_resample_manifest.json";
    const char *height_path = "/tmp/zanna_game3d_terrain_resample.height";
    const char *heights = "zanna-heightmap-v1 3 3\n"
                          "0.00 0.10 0.20\n"
                          "0.30 0.25 0.40\n"
                          "0.50 0.70 1.00\n";
    const char *manifest = "{\"tiles\":["
                           "{\"name\":\"resample\",\"path\":\"terrain/resample.tile\","
                           "\"heightmap\":\"zanna_game3d_terrain_resample.height\","
                           "\"center\":[0,0,0],\"radius\":32,\"bytes\":1000,"
                           "\"width\":2,\"depth\":2,\"scale\":[1,10,1]}"
                           "]}";
    EXPECT_TRUE(write_text_file(height_path, heights), "resample height sidecar is writable");
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "resample manifest fixture is writable");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Terrain Resample Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_radii(stream, 96.0, 96.0);
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_mount_tiled_terrain(stream, rt_const_cstr(manifest_path));
    void *terrain = rt_game3d_world_stream_get_resident_terrain_tile(stream, 0);
    EXPECT_TRUE(terrain != nullptr, "downsampled terrain sidecar becomes resident");
    if (terrain) {
        EXPECT_NEAR(rt_terrain3d_get_height_at(terrain, 1.0, 1.0),
                    10.0,
                    0.001,
                    "downsampled terrain preserves the source southeast edge height");
        EXPECT_NEAR(rt_terrain3d_get_height_at(terrain, 0.0, 0.0),
                    0.0,
                    0.001,
                    "downsampled terrain preserves the source northwest edge height");
    }

    rt_game3d_world_destroy(world);
    std::remove(manifest_path);
    std::remove(height_path);
    PASS();
}

static bool test_phase5_world_stream3d_rejects_trailing_heightmap_tokens() {
    TEST("WorldStream3D rejects terrain heightmap sidecars with trailing tokens");
    const char *manifest_path = "/tmp/zanna_game3d_terrain_trailing_manifest.json";
    const char *height_path = "/tmp/zanna_game3d_terrain_trailing.height";
    const char *heights = "zanna-heightmap-v1 2 2\n"
                          "0.00 0.00\n"
                          "0.00 1.00\n"
                          "unexpected-token\n";
    const char *manifest = "{\"tiles\":["
                           "{\"name\":\"trailing\",\"path\":\"terrain/trailing.tile\","
                           "\"heightmap\":\"zanna_game3d_terrain_trailing.height\","
                           "\"center\":[0,0,0],\"radius\":32,\"bytes\":1000,"
                           "\"width\":2,\"depth\":2,\"scale\":[1,10,1]}"
                           "]}";
    EXPECT_TRUE(write_text_file(height_path, heights), "trailing-token height sidecar is writable");
    EXPECT_TRUE(write_text_file(manifest_path, manifest),
                "trailing-token terrain manifest fixture is writable");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Terrain Trailing Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_radii(stream, 96.0, 96.0);
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_mount_tiled_terrain(stream, rt_const_cstr(manifest_path));
    void *terrain = rt_game3d_world_stream_get_resident_terrain_tile(stream, 0);
    EXPECT_TRUE(terrain != nullptr, "terrain still loads with a fallback heightmap");
    EXPECT_NEAR(rt_terrain3d_get_height_at(terrain, 1.0, 1.0),
                0.0,
                0.001,
                "malformed trailing height data is not partially applied");

    rt_game3d_world_destroy(world);
    std::remove(manifest_path);
    std::remove(height_path);
    PASS();
}

static bool test_phase5_world_stream3d_terrain_lod_seams_large_world() {
    TEST("WorldStream3D stitches adjacent terrain LOD seams beyond one heightmap cap");
    const char *manifest_path = "/tmp/zanna_game3d_terrain_lod_seams_manifest.json";
    const char *west_height_path = "/tmp/zanna_game3d_terrain_lod_seams_west.height";
    const char *east_height_path = "/tmp/zanna_game3d_terrain_lod_seams_east.height";
    const double tile_scale = 600.0;
    const int64_t tile_samples = 9;
    const double tile_extent = (double)(tile_samples - 1) * tile_scale;
    const double combined_extent_x = tile_extent * 2.0;
    const double combined_area = combined_extent_x * tile_extent;

    std::string west_heights =
        terrain_heightmap_fixture((int)tile_samples, (int)tile_samples, 0.30, 0.80, 0.42);
    std::string east_heights =
        terrain_heightmap_fixture((int)tile_samples, (int)tile_samples, 0.20, 0.65, 0.46);
    EXPECT_TRUE(write_text_file(west_height_path, west_heights.c_str()),
                "west large-world terrain height sidecar is writable");
    EXPECT_TRUE(write_text_file(east_height_path, east_heights.c_str()),
                "east large-world terrain height sidecar is writable");

    char manifest[2048];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"tiles\":["
                  "{\"name\":\"west_lod_tile\",\"path\":\"terrain/west.tile\","
                  "\"heightmap\":\"zanna_game3d_terrain_lod_seams_west.height\","
                  "\"center\":[0,0,0],\"radius\":6000,\"bytes\":4096,"
                  "\"width\":%lld,\"depth\":%lld,\"scale\":[%.1f,10,%.1f]},"
                  "{\"name\":\"east_lod_tile\",\"path\":\"terrain/east.tile\","
                  "\"heightmap\":\"zanna_game3d_terrain_lod_seams_east.height\","
                  "\"center\":[%.1f,0,0],\"radius\":6000,\"bytes\":4096,"
                  "\"width\":%lld,\"depth\":%lld,\"scale\":[%.1f,10,%.1f]}"
                  "]}",
                  (long long)tile_samples,
                  (long long)tile_samples,
                  tile_scale,
                  tile_scale,
                  tile_extent,
                  (long long)tile_samples,
                  (long long)tile_samples,
                  tile_scale,
                  tile_scale);
    EXPECT_TRUE(write_text_file(manifest_path, manifest),
                "large-world terrain seam manifest is writable");

    void *world = rt_game3d_world_new_with_camera(
        rt_const_cstr("Game3D Terrain Seam Unit"), 96, 72, 60.0, 0.1, 20000.0);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_radii(stream, 7000.0, 8000.0);
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(tile_extent * 0.5, 0.0, 0.0));
    rt_game3d_world_stream_mount_tiled_terrain(stream, rt_const_cstr(manifest_path));

    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  2,
                  "large stream radius admits both adjacent terrain tiles");
    EXPECT_TRUE(tile_samples <= 4096,
                "each proof tile stays within the single Terrain3D.New heightmap cap");
    EXPECT_TRUE(combined_extent_x > 4096.0,
                "stitched terrain proof exceeds the single-heightmap 4096-unit span");
    EXPECT_TRUE(combined_area > 4000000.0,
                "stitched terrain proof exceeds four square kilometers at meter scale");
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(world),
                  2,
                  "stitched terrain tiles rebuild heightfield collider bodies");

    void *west = rt_game3d_world_stream_get_resident_terrain_tile(stream, 0);
    void *east = rt_game3d_world_stream_get_resident_terrain_tile(stream, 1);
    EXPECT_TRUE(west != nullptr && east != nullptr,
                "both adjacent terrain payloads are resident for seam sampling");
    double west_edge = rt_terrain3d_get_height_at(west, tile_extent, tile_extent * 0.5);
    double east_edge = rt_terrain3d_get_height_at(east, 0.0, tile_extent * 0.5);
    EXPECT_NEAR(
        west_edge, east_edge, 0.001, "adjacent resident tiles average mismatched heightmap edges");
    EXPECT_NEAR(west_edge,
                5.0,
                0.02,
                "stitched seam uses world-height averaging, not one tile's original edge");

    rt_terrain3d_set_skirt_depth(west, 0.0);
    rt_terrain3d_set_skirt_depth(east, 0.0);
    rt_terrain3d_set_lod_distances(west, 1.0, 2.0);
    rt_terrain3d_set_lod_distances(east, 100000.0, 200000.0);
    rt_camera3d_look_at(rt_game3d_world_get_camera(world),
                        rt_vec3_new(tile_extent * 0.5, 3500.0, tile_extent * 1.75),
                        rt_vec3_new(tile_extent * 0.5, 0.0, 0.0),
                        rt_vec3_new(0.0, 1.0, 0.0));
    rt_game3d_world_begin_frame(world);
    rt_game3d_world_draw_scene(world);
    rt_game3d_world_end_scene(world);
    EXPECT_TRUE(
        rt_game3d_world_get_draw_count(world) > 0,
        "World3D.drawScene renders stitched terrain while adjacent tiles use different LODs");

    rt_game3d_world_destroy(world);
    std::remove(manifest_path);
    std::remove(west_height_path);
    std::remove(east_height_path);
    PASS();
}

static bool write_stream_cell_scene(const char *path, const char *marker_name) {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    rt_scene_node3d_set_name(node, rt_const_cstr(marker_name));
    rt_scene3d_add(scene, node);
    return rt_scene3d_save(scene, rt_const_cstr(path)) == 1;
}

static bool test_phase5_world_stream3d_budget_prefers_nearest_entries() {
    TEST("WorldStream3D budgeted residency prefers nearest manifest entries");

    const char *near_path = "/tmp/zanna_game3d_nearest_budget_near.vscn";
    const char *far_path = "/tmp/zanna_game3d_nearest_budget_far.vscn";
    const char *cells_manifest_path = "/tmp/zanna_game3d_nearest_budget_cells.vscn";
    EXPECT_TRUE(write_stream_cell_scene(near_path, "nearest_budget_near_marker"),
                "nearest-budget near cell fixture saves");
    EXPECT_TRUE(write_stream_cell_scene(far_path, "nearest_budget_far_marker"),
                "nearest-budget far cell fixture saves");

    char cells_manifest[2048];
    std::snprintf(cells_manifest,
                  sizeof(cells_manifest),
                  "{"
                  "\"cells\":["
                  "{\"name\":\"far_first\",\"path\":\"%s\",\"center\":[100,0,0],"
                  "\"radius\":200,\"bytes\":4096},"
                  "{\"name\":\"near_second\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":200,\"bytes\":4096}"
                  "]"
                  "}",
                  far_path,
                  near_path);
    EXPECT_TRUE(write_text_file(cells_manifest_path, cells_manifest),
                "nearest-budget cell manifest writes");

    void *cell_world =
        rt_game3d_world_new(rt_const_cstr("Game3D Nearest Cell Budget Unit"), 80, 60);
    void *cell_stream = rt_game3d_world_get_stream(cell_world);
    rt_game3d_world_stream_set_async_streaming(cell_stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_center(cell_stream, rt_vec3_new(1000.0, 0.0, 0.0));
    rt_game3d_world_stream_set_radii(cell_stream, 256.0, 256.0);
    rt_game3d_world_stream_mount_cells(cell_stream, rt_const_cstr(cells_manifest_path));
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(cell_stream),
                  0,
                  "cell fixture starts outside the stream radius");
    rt_game3d_world_stream_set_center(cell_stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_update(cell_stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(cell_stream),
                  1,
                  "per-frame cell budget admits only one resident cell");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_resident(cell_stream, 0),
                  0,
                  "far-first manifest cell is skipped under the one-load frame budget");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_resident(cell_stream, 1),
                  1,
                  "nearer later manifest cell is admitted under the one-load frame budget");
    EXPECT_TRUE(rt_game3d_world_find_node(cell_world,
                                          rt_const_cstr("nearest_budget_near_marker")) != nullptr,
                "nearest budget loads the near cell scene");
    EXPECT_TRUE(rt_game3d_world_find_node(cell_world, rt_const_cstr("nearest_budget_far_marker")) ==
                    nullptr,
                "nearest budget leaves the farther first cell unloaded");
    rt_game3d_world_destroy(cell_world);

    const char *terrain_manifest_path = "/tmp/zanna_game3d_nearest_budget_terrain.vscn";
    const char *terrain_manifest =
        "{\"tiles\":["
        "{\"name\":\"far_terrain_first\",\"path\":\"terrain/far.tile\","
        "\"center\":[100,0,0],\"radius\":200,\"bytes\":4096,"
        "\"width\":4,\"depth\":4,\"scale\":[2,1,2]},"
        "{\"name\":\"near_terrain_second\",\"path\":\"terrain/near.tile\","
        "\"center\":[0,0,0],\"radius\":200,\"bytes\":4096,"
        "\"width\":4,\"depth\":4,\"scale\":[2,1,2]}"
        "]}";
    EXPECT_TRUE(write_text_file(terrain_manifest_path, terrain_manifest),
                "nearest-budget terrain manifest writes");

    void *terrain_world =
        rt_game3d_world_new(rt_const_cstr("Game3D Nearest Terrain Budget Unit"), 80, 60);
    void *terrain_stream = rt_game3d_world_get_stream(terrain_world);
    rt_game3d_world_stream_set_async_streaming(terrain_stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_center(terrain_stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_set_radii(terrain_stream, 256.0, 256.0);
    rt_game3d_world_stream_set_residency_budget(terrain_stream, 7000);
    rt_game3d_world_stream_mount_tiled_terrain(terrain_stream,
                                               rt_const_cstr(terrain_manifest_path));
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(terrain_stream),
                  1,
                  "terrain budget admits only one resident tile");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_resident(terrain_stream, 0),
                  0,
                  "far-first manifest terrain tile is skipped under the one-tile budget");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_resident(terrain_stream, 1),
                  1,
                  "nearer later manifest terrain tile is admitted under the one-tile budget");
    EXPECT_EQ_INT(rt_game3d_world_get_body_count(terrain_world),
                  1,
                  "nearest budget creates one terrain collider for the admitted tile");

    rt_game3d_world_destroy(terrain_world);
    std::remove(near_path);
    std::remove(far_path);
    std::remove(cells_manifest_path);
    std::remove(terrain_manifest_path);
    PASS();
}

static void *make_stream_lod_mesh(void) {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_reserve(mesh, 192, 64);
    for (int64_t i = 0; i < 64; ++i) {
        double x = (double)(i % 8) * 2.0;
        double y = (double)(i / 8) * 2.0;
        int64_t base = i * 3;
        rt_mesh3d_add_vertex(mesh, x, y, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
        rt_mesh3d_add_vertex(mesh, x + 1.0, y, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
        rt_mesh3d_add_vertex(mesh, x, y + 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0);
        rt_mesh3d_add_triangle(mesh, base, base + 1, base + 2);
    }
    return mesh;
}

static bool write_stream_lod_cell_scene(const char *path) {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    void *base_mesh = rt_mesh3d_new_box(0.25, 0.25, 0.25);
    void *lod_mesh = make_stream_lod_mesh();
    void *material = rt_material3d_new_color(0.2, 0.3, 0.4);

    rt_scene_node3d_set_name(node, rt_const_cstr("stream_lod_residency_marker"));
    rt_scene_node3d_set_mesh(node, base_mesh);
    rt_scene_node3d_set_material(node, material);
    rt_scene_node3d_add_lod(node, 16.0, lod_mesh);
    rt_scene3d_add(scene, node);
    return rt_scene3d_save(scene, rt_const_cstr(path)) == 1;
}

static bool test_phase5_world_stream3d_manifest_cells() {
    TEST("WorldStream3D mounts and unloads VSCN scene cells from a manifest");

    const char *near_path = "/tmp/zanna_game3d_stream_near.vscn";
    const char *far_path = "/tmp/zanna_game3d_stream_far.vscn";
    const char *manifest_path = "/tmp/zanna_game3d_stream_cells_manifest.vscn";
    EXPECT_TRUE(write_stream_cell_scene(near_path, "stream_near_marker"),
                "near stream cell fixture saves");
    EXPECT_TRUE(write_stream_cell_scene(far_path, "stream_far_marker"),
                "far stream cell fixture saves");

    char manifest[2048];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{"
                  "\"cells\":["
                  "{\"name\":\"near_cell\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536},"
                  "{\"name\":\"far_cell\",\"path\":\"%s\",\"center\":[1000,0,0],"
                  "\"radius\":8,\"bytes\":65536}"
                  "]"
                  "}",
                  near_path,
                  far_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "stream manifest fixture writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D WorldStream Manifest Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);

    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "manifest stream loads only the near cell");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("stream_near_marker")) != nullptr,
                "near cell scene subtree is attached to the world scene");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("stream_far_marker")) == nullptr,
                "far cell remains unloaded outside the load radius");
    int64_t near_bytes = rt_game3d_world_stream_get_resident_bytes(stream);
    EXPECT_TRUE(near_bytes > 0, "manifest stream accounts measured loaded cell bytes");
    EXPECT_TRUE(rt_game3d_world_stream_get_cell_bytes(stream, 0) != 65536,
                "loaded simple cell replaces manifest bytes with measured residency");

    rt_game3d_world_stream_set_center(stream, rt_vec3_new(1000.0, 0.0, 0.0));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "manifest stream swaps residency after crossing cell boundary");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("stream_near_marker")) == nullptr,
                "near cell scene subtree unloads outside the unload radius");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("stream_far_marker")) != nullptr,
                "far cell scene subtree loads inside the load radius");

    rt_game3d_world_stream_set_residency_budget(stream, 0);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  0,
                  "zero manifest stream budget unloads all cells");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("stream_far_marker")) == nullptr,
                "budget eviction detaches the resident cell subtree");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_bytes(stream),
                  0,
                  "zero manifest stream budget clears resident bytes");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_phase5_world_stream3d_cell_binary_sidecar() {
    TEST("WorldStream3D loads, accounts, and frees cell binary sidecar payloads");

    const char *cell_path = "/tmp/zanna_game3d_stream_sidecar_cell.vscn";
    const char *sidecar_path = "/tmp/zanna_game3d_stream_sidecar_payload.bin";
    const char *manifest_path = "/tmp/zanna_game3d_stream_sidecar_manifest.vscn";
    EXPECT_TRUE(write_stream_cell_scene(cell_path, "stream_sidecar_marker"),
                "sidecar stream cell fixture saves");

    // 192-byte opaque binary sidecar payload.
    char payload[193];
    std::memset(payload, 'V', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    EXPECT_TRUE(write_text_file(sidecar_path, payload), "binary sidecar fixture writes");
    const int64_t kSidecarBytes = 192;

    char manifest[2048];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{\"cells\":[{\"name\":\"sidecar_cell\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536,\"sidecar\":\"%s\"}]}",
                  cell_path,
                  sidecar_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "sidecar manifest fixture writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D WorldStream Sidecar Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);

    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "sidecar manifest loads the cell");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_sidecar_bytes(stream, 0),
                  kSidecarBytes,
                  "resident cell reports its loaded binary sidecar byte count");
    EXPECT_TRUE(rt_game3d_world_stream_get_cell_bytes(stream, 0) > kSidecarBytes,
                "resident cell bytes include the sidecar payload on top of scene residency");

    // Unloading the cell frees the sidecar payload and clears its byte count.
    rt_game3d_world_stream_set_residency_budget(stream, 0);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  0,
                  "zero budget unloads the sidecar cell");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_sidecar_bytes(stream, 0),
                  0,
                  "unloaded cell reports zero sidecar bytes");
    rt_game3d_world_destroy(world);

    // A missing/unreadable sidecar is recoverable: the cell still loads with zero sidecar bytes.
    const char *missing_manifest_path = "/tmp/zanna_game3d_stream_sidecar_missing_manifest.vscn";
    char missing_manifest[2048];
    std::snprintf(missing_manifest,
                  sizeof(missing_manifest),
                  "{\"cells\":[{\"name\":\"sidecar_cell\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":65536,"
                  "\"sidecar\":\"/tmp/zanna_game3d_no_such_sidecar.bin\"}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(missing_manifest_path, missing_manifest),
                "missing-sidecar manifest fixture writes");
    void *world2 =
        rt_game3d_world_new(rt_const_cstr("Game3D WorldStream Sidecar Missing Unit"), 80, 60);
    void *stream2 = rt_game3d_world_get_stream(world2);
    rt_game3d_world_stream_set_async_streaming(stream2,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_center(stream2, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_set_radii(stream2, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream2, rt_const_cstr(missing_manifest_path));
    rt_game3d_world_stream_update(stream2, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream2),
                  1,
                  "cell with a missing sidecar still loads");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_sidecar_bytes(stream2, 0),
                  0,
                  "missing sidecar is recoverable with zero bytes");
    rt_game3d_world_destroy(world2);
    PASS();
}

static bool test_phase5_world_stream3d_measures_lod_residency() {
    TEST("WorldStream3D measures loaded VSCN mesh and LOD residency");

    const char *cell_path = "/tmp/zanna_game3d_stream_lod_residency.vscn";
    const char *manifest_path = "/tmp/zanna_game3d_stream_lod_residency_manifest.vscn";
    EXPECT_TRUE(write_stream_lod_cell_scene(cell_path), "LOD stream cell fixture saves");

    char manifest[2048];
    std::snprintf(manifest,
                  sizeof(manifest),
                  "{"
                  "\"cells\":["
                  "{\"name\":\"lod_cell\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":8,\"bytes\":4096}"
                  "]"
                  "}",
                  cell_path);
    EXPECT_TRUE(write_text_file(manifest_path, manifest), "LOD stream manifest fixture writes");

    void *world =
        rt_game3d_world_new(rt_const_cstr("Game3D WorldStream LOD Residency Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest_path));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);

    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "LOD stream loads the authored cell");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("stream_lod_residency_marker")) !=
                    nullptr,
                "LOD cell scene subtree is attached to the world scene");
    int64_t measured_cell_bytes = rt_game3d_world_stream_get_cell_bytes(stream, 0);
    EXPECT_TRUE(measured_cell_bytes > 4096,
                "resident cell bytes measure authored mesh and LOD payloads");
    int64_t measured_total = rt_game3d_world_stream_get_resident_bytes(stream);
    EXPECT_TRUE(measured_total >= measured_cell_bytes,
                "resident stream bytes include measured cell residency");

    void *loaded_lod_node =
        rt_game3d_world_find_node(world, rt_const_cstr("stream_lod_residency_marker"));
    EXPECT_TRUE(loaded_lod_node != nullptr, "loaded stream node is available for LOD residency");
    int64_t lod_resident_bytes =
        loaded_lod_node ? rt_scene_node3d_get_lod_resident_bytes(loaded_lod_node, 0) : 0;
    EXPECT_TRUE(lod_resident_bytes > 0, "loaded stream LOD reports resident mesh bytes");

    if (loaded_lod_node) {
        rt_scene_node3d *loaded_view = static_cast<rt_scene_node3d *>(loaded_lod_node);
        void *saved_mesh = loaded_view->mesh;
        void *saved_material = loaded_view->material;
        rt_scene_node3d **saved_children = loaded_view->children;
        int32_t saved_child_count = loaded_view->child_count;
        int32_t saved_child_capacity = loaded_view->child_capacity;
        int32_t saved_lod_count = loaded_view->lod_count;
        int32_t saved_lod_capacity = loaded_view->lod_capacity;
        void *wrong_class_handle = rt_vec3_new(1.0, 2.0, 3.0);

        loaded_view->mesh = wrong_class_handle;
        loaded_view->material = wrong_class_handle;
        loaded_view->child_count = INT32_MAX;
        loaded_view->child_capacity = 0;
        loaded_view->children = nullptr;
        loaded_view->lod_count = INT32_MAX;
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        int64_t corrupt_cell_bytes = rt_game3d_world_stream_get_cell_bytes(stream, 0);
        EXPECT_TRUE(corrupt_cell_bytes > 0 && corrupt_cell_bytes <= measured_cell_bytes,
                    "stream residency ignores corrupt counts and wrong-class mesh/material refs");

        loaded_view->mesh = saved_mesh;
        loaded_view->material = saved_material;
        loaded_view->children = saved_children;
        loaded_view->child_count = saved_child_count;
        loaded_view->child_capacity = saved_child_capacity;
        loaded_view->lod_count = saved_lod_count;
        loaded_view->lod_capacity = saved_lod_capacity;
        if (wrong_class_handle && rt_obj_release_check0(wrong_class_handle))
            rt_obj_free(wrong_class_handle);
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        measured_cell_bytes = rt_game3d_world_stream_get_cell_bytes(stream, 0);
        measured_total = rt_game3d_world_stream_get_resident_bytes(stream);
    }

    if (loaded_lod_node)
        rt_scene_node3d_set_lod_resident(loaded_lod_node, 0, 0);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    int64_t demoted_cell_bytes = rt_game3d_world_stream_get_cell_bytes(stream, 0);
    EXPECT_TRUE(demoted_cell_bytes > 0 && demoted_cell_bytes < measured_cell_bytes,
                "stream cell remeasures lower bytes after LOD payload demotion");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_bytes(stream) < measured_total,
                "stream resident-byte telemetry follows LOD residency changes");
    EXPECT_TRUE(loaded_lod_node && rt_scene_node3d_get_lod_resident_bytes(loaded_lod_node, 0) == 0,
                "demoted stream LOD reports zero resident mesh bytes");

    if (loaded_lod_node)
        rt_scene_node3d_set_lod_resident(loaded_lod_node, 0, 1);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    measured_cell_bytes = rt_game3d_world_stream_get_cell_bytes(stream, 0);
    measured_total = rt_game3d_world_stream_get_resident_bytes(stream);
    EXPECT_TRUE(measured_cell_bytes > demoted_cell_bytes,
                "stream cell remeasures higher bytes after LOD payload promotion");

    rt_game3d_world_stream_set_residency_budget(stream, measured_total - 1);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "one-byte budget overshoot stays inside the eviction watermark (hysteresis)");
    rt_game3d_world_stream_set_residency_budget(stream, (measured_total * 3) / 4);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  0,
                  "undersized budget evicts measured-over-manifest cell");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("stream_lod_residency_marker")) ==
                    nullptr,
                "measured residency eviction detaches the authored cell subtree");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_bytes(stream, 0),
                  4096,
                  "unloaded cell reports the manifest planning estimate");
    int64_t after_eviction_bytes = rt_game3d_world_stream_get_resident_bytes(stream);
    EXPECT_TRUE(after_eviction_bytes > 0 && after_eviction_bytes < measured_total,
                "evicted measured cell leaves only stream metadata residency");

    rt_game3d_world_stream_set_residency_budget(stream, 0);
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_bytes(stream),
                  0,
                  "zero budget clears stream metadata and resident bytes");

    rt_game3d_world_destroy(world);
    std::remove(cell_path);
    std::remove(manifest_path);
    PASS();
}

static bool test_phase5_world_stream3d_hitch_budgeted_update() {
    TEST("WorldStream3D update budgets stream loads and reports pending requests");

    const char *near_path = "/tmp/zanna_game3d_stream_budget_near.vscn";
    const char *far_path = "/tmp/zanna_game3d_stream_budget_far.vscn";
    const char *cells_manifest_path = "/tmp/zanna_game3d_stream_budget_cells.vscn";
    const char *terrain_manifest_path = "/tmp/zanna_game3d_stream_budget_terrain.vscn";
    EXPECT_TRUE(write_stream_cell_scene(near_path, "budget_near_marker"),
                "near budget cell fixture saves");
    EXPECT_TRUE(write_stream_cell_scene(far_path, "budget_far_marker"),
                "far budget cell fixture saves");

    char cells_manifest[2048];
    std::snprintf(cells_manifest,
                  sizeof(cells_manifest),
                  "{"
                  "\"cells\":["
                  "{\"name\":\"budget_near\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":16,\"bytes\":4096},"
                  "{\"name\":\"budget_far\",\"path\":\"%s\",\"center\":[1000,0,0],"
                  "\"radius\":16,\"bytes\":4096}"
                  "]"
                  "}",
                  near_path,
                  far_path);
    EXPECT_TRUE(write_text_file(cells_manifest_path, cells_manifest),
                "budget cell manifest writes");

    const char *terrain_manifest =
        "{\"tiles\":["
        "{\"name\":\"budget_terrain_near\",\"path\":\"terrain/near.tile\","
        "\"center\":[0,0,0],\"radius\":16,\"bytes\":4096,"
        "\"width\":4,\"depth\":4,\"scale\":[2,1,2]},"
        "{\"name\":\"budget_terrain_far\",\"path\":\"terrain/far.tile\","
        "\"center\":[1000,0,0],\"radius\":16,\"bytes\":4096,"
        "\"width\":4,\"depth\":4,\"scale\":[2,1,2]}"
        "]}";
    EXPECT_TRUE(write_text_file(terrain_manifest_path, terrain_manifest),
                "budget terrain manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Stream Budget Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0);
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(cells_manifest_path));
    rt_game3d_world_stream_mount_tiled_terrain(stream, rt_const_cstr(terrain_manifest_path));
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "initial mount admits the near cell");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  1,
                  "initial mount admits the near terrain tile");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_pending_request_count(stream),
                  0,
                  "initial mount has no pending stream work");

    rt_game3d_world_stream_set_center(stream, rt_vec3_new(1000.0, 0.0, 0.0));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "first budgeted update loads the far cell");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  0,
                  "first budgeted update defers the far terrain tile");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_pending_request_count(stream),
                  1,
                  "deferred terrain load is reported as pending");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("budget_near_marker")) == nullptr,
                "budgeted update unloads the stale near cell immediately");
    EXPECT_TRUE(rt_game3d_world_find_node(world, rt_const_cstr("budget_far_marker")) != nullptr,
                "budgeted update attaches the far cell");

    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "second budgeted update keeps the far cell resident");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  1,
                  "second budgeted update loads the pending terrain tile");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_pending_request_count(stream),
                  0,
                  "pending stream work drains after the second update");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_terrain_tile(stream, 0) != nullptr,
                "budgeted terrain load exposes the resident Terrain3D payload");

    rt_game3d_world_destroy(world);
    std::remove(near_path);
    std::remove(far_path);
    std::remove(cells_manifest_path);
    std::remove(terrain_manifest_path);
    PASS();
}

static bool test_phase5_world_stream3d_budget_hysteresis_and_cooldown() {
    TEST("WorldStream3D budget eviction has hysteresis and a reload cooldown");

    const char *near_path = "/tmp/zanna_game3d_stream_hyst_near.vscn";
    const char *far_path = "/tmp/zanna_game3d_stream_hyst_far.vscn";
    const char *sidecar_path = "/tmp/zanna_game3d_stream_hyst_far.bin";
    const char *cells_manifest_path = "/tmp/zanna_game3d_stream_hyst_cells.vscn";
    EXPECT_TRUE(write_stream_cell_scene(near_path, "hyst_near_marker"),
                "near hysteresis cell fixture saves");
    EXPECT_TRUE(write_stream_cell_scene(far_path, "hyst_far_marker"),
                "far hysteresis cell fixture saves");

    // A 1 MiB binary sidecar dominates the far cell's measured residency so the
    // budget-watermark math below operates on predictable magnitudes.
    {
        std::vector<char> blob(1024u * 1024u, 'x');
        std::FILE *f = std::fopen(sidecar_path, "wb");
        EXPECT_TRUE(f != nullptr, "far sidecar fixture opens");
        EXPECT_TRUE(std::fwrite(blob.data(), 1, blob.size(), f) == blob.size(),
                    "far sidecar fixture writes");
        std::fclose(f);
    }

    char cells_manifest[2048];
    std::snprintf(cells_manifest,
                  sizeof(cells_manifest),
                  "{"
                  "\"cells\":["
                  "{\"name\":\"hyst_near\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":16,\"bytes\":4096},"
                  "{\"name\":\"hyst_far\",\"path\":\"%s\",\"center\":[40,0,0],"
                  "\"radius\":16,\"bytes\":4096,"
                  "\"sidecar\":\"zanna_game3d_stream_hyst_far.bin\"}"
                  "]"
                  "}",
                  near_path,
                  far_path);
    EXPECT_TRUE(write_text_file(cells_manifest_path, cells_manifest),
                "hysteresis cell manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Stream Hysteresis Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_set_radii(stream, 64.0, 96.0); // both cells stay in radius throughout
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(cells_manifest_path));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  2,
                  "unlimited budget admits both cells");
    int64_t both_bytes = rt_game3d_world_stream_get_resident_bytes(stream);
    EXPECT_TRUE(both_bytes > 1024 * 1024, "far sidecar dominates resident-byte telemetry");

    // Hysteresis: a budget just below the resident total must NOT evict the boundary cell —
    // it survives inside the high watermark (budget + max(budget/8, 64 KiB)).
    rt_game3d_world_stream_set_residency_budget(stream, both_bytes - 40 * 1024);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  2,
                  "small budget overshoot keeps the boundary cell resident (hysteresis)");

    // Eviction: cutting well past the watermark evicts the boundary cell and starts its
    // reload cooldown.
    rt_game3d_world_stream_set_residency_budget(stream, both_bytes - 300 * 1024);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "budget cut past the watermark evicts the boundary cell");

    // Cooldown: restoring an unlimited budget must not reload the evicted cell immediately —
    // that instant round-trip is exactly the thrash the cooldown exists to stop.
    rt_game3d_world_stream_set_residency_budget(stream, -1);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "budget-evicted cell holds through its reload cooldown");
    EXPECT_TRUE(rt_game3d_world_stream_get_pending_request_count(stream) >= 1,
                "cooldown-held cell is reported as pending");

    // Cooldown expiry: the cell streams back in after the hold elapses.
    bool reloaded = false;
    for (int i = 0; i < 40 && !reloaded; i++) {
        rt_game3d_world_stream_update(stream, 1.0 / 60.0);
        reloaded = rt_game3d_world_stream_get_resident_cell_count(stream) == 2;
    }
    EXPECT_TRUE(reloaded, "cooldown expires and the evicted cell streams back in");

    rt_game3d_world_destroy(world);
    std::remove(near_path);
    std::remove(far_path);
    std::remove(sidecar_path);
    std::remove(cells_manifest_path);
    PASS();
}

static bool test_phase5_world_stream3d_zero_radius_update_traps() {
    TEST("WorldStream3D.Update traps when a manifest is mounted but the load radius is 0");

    const char *cell_path = "/tmp/zanna_game3d_stream_zero_radius_cell.vscn";
    const char *cells_manifest_path = "/tmp/zanna_game3d_stream_zero_radius_cells.vscn";
    EXPECT_TRUE(write_stream_cell_scene(cell_path, "zero_radius_marker"),
                "zero-radius cell fixture saves");
    char cells_manifest[1024];
    // The cell sits away from the stream center: with zero radii nothing is desired
    // (a cell surrounding the focus point would stream in through its own radius).
    std::snprintf(cells_manifest,
                  sizeof(cells_manifest),
                  "{\"cells\":[{\"name\":\"zero_radius\",\"path\":\"%s\","
                  "\"center\":[100,0,0],\"radius\":16,\"bytes\":4096}]}",
                  cell_path);
    EXPECT_TRUE(write_text_file(cells_manifest_path, cells_manifest),
                "zero-radius cell manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Stream Zero Radius Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    // SetRadii deliberately never called: the default radii are zero, which streams nothing.
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(cells_manifest_path));
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  0,
                  "zero-radius mount streams nothing in");
    EXPECT_TRUE(expect_trap_contains([&]() { rt_game3d_world_stream_update(stream, 1.0 / 60.0); },
                                     "load radius is 0"),
                "update with a mounted manifest and zero radii traps instead of silently rendering "
                "an empty world");

    rt_game3d_world_destroy(world);
    std::remove(cell_path);
    std::remove(cells_manifest_path);
    PASS();
}

static bool test_phase12_world_stream3d_inspection_hooks() {
    TEST("WorldStream3D exposes editor inspection hooks for parsed entries");

    const char *near_path = "/tmp/zanna_game3d_stream_inspect_near.vscn";
    const char *far_path = "/tmp/zanna_game3d_stream_inspect_far.vscn";
    const char *cells_manifest_path = "/tmp/zanna_game3d_stream_inspect_cells.vscn";
    const char *terrain_manifest_path = "/tmp/zanna_game3d_stream_inspect_terrain.vscn";
    EXPECT_TRUE(write_stream_cell_scene(near_path, "inspect_near_marker"),
                "near inspection cell fixture saves");
    EXPECT_TRUE(write_stream_cell_scene(far_path, "inspect_far_marker"),
                "far inspection cell fixture saves");

    char cells_manifest[2048];
    std::snprintf(cells_manifest,
                  sizeof(cells_manifest),
                  "{"
                  "\"cells\":["
                  "{\"name\":\"inspect_near\",\"path\":\"%s\",\"center\":[0,0,0],"
                  "\"radius\":16,\"bytes\":1111,\"material\":\"stone\","
                  "\"layer\":1,\"collisionMask\":5,\"navArea\":\"road\","
                  "\"traversalCost\":1.25,\"sidecar\":\"meta/near_cell.bin\"},"
                  "{\"name\":\"inspect_far\",\"path\":\"%s\",\"center\":[512,0,0],"
                  "\"radius\":16,\"bytes\":2222,\"material\":\"glass\","
                  "\"collision\":{\"enabled\":false,\"layer\":8,\"mask\":2},"
                  "\"navArea\":\"blocked\",\"traversalCost\":3.5,"
                  "\"sidecar\":\"meta/far_cell.bin\"}"
                  "]"
                  "}",
                  near_path,
                  far_path);
    EXPECT_TRUE(write_text_file(cells_manifest_path, cells_manifest),
                "inspection cell manifest writes");

    const char *terrain_manifest =
        "{\"tiles\":["
        "{\"name\":\"terrain_near\",\"path\":\"terrain/near.tile\",\"center\":[0,0,0],"
        "\"radius\":16,\"bytes\":3333,\"width\":8,\"depth\":10,\"scale\":[2,3,4],"
        "\"material\":\"grass\",\"collisionLayer\":1,\"collisionMask\":5,"
        "\"navArea\":\"meadow\",\"traversalCost\":1.5,\"sidecar\":\"terrain/near.meta\"},"
        "{\"name\":\"terrain_far\",\"path\":\"terrain/far.tile\",\"center\":[512,0,0],"
        "\"radius\":16,\"bytes\":4444,\"width\":12,\"depth\":14,\"scale\":[4,5,6],"
        "\"material\":\"rock\",\"collision\":{\"enabled\":false,\"layer\":8,\"mask\":2},"
        "\"navArea\":\"cliff\",\"traversalCost\":4.0,\"binarySidecar\":\"terrain/far.meta\"}"
        "]}";
    EXPECT_TRUE(write_text_file(terrain_manifest_path, terrain_manifest),
                "inspection terrain manifest writes");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Stream Inspection Unit"), 80, 60);
    void *stream = rt_game3d_world_get_stream(world);
    rt_game3d_world_stream_set_async_streaming(stream,
                                               0); /* blocking: test pins synchronous residency */
    rt_game3d_world_stream_set_radii(stream, 64.0, 64.0);
    rt_game3d_world_stream_set_center(stream, rt_vec3_new(0.0, 0.0, 0.0));
    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(cells_manifest_path));
    rt_game3d_world_stream_mount_tiled_terrain(stream, rt_const_cstr(terrain_manifest_path));

    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_count(stream),
                  2,
                  "inspection hooks expose parsed cell count");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_count(stream),
                  2,
                  "inspection hooks expose parsed terrain tile count");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_cell_name(stream, 0)),
                            "inspect_near") == 0,
                "inspection hooks expose cell names");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_terrain_tile_name(stream, 1)),
                            "terrain_far") == 0,
                "inspection hooks expose terrain tile names");
    void *near_cell_center = rt_game3d_world_stream_get_cell_center(stream, 0);
    void *far_tile_center = rt_game3d_world_stream_get_terrain_tile_center(stream, 1);
    EXPECT_NEAR(
        rt_vec3_x(near_cell_center), 0.0, 0.000001, "inspection hook returns cell center x");
    EXPECT_NEAR(rt_vec3_x(far_tile_center),
                512.0,
                0.000001,
                "inspection hook returns terrain tile center x");
    int64_t resident_cell_bytes = rt_game3d_world_stream_get_cell_bytes(stream, 0);
    EXPECT_TRUE(resident_cell_bytes > 0 && resident_cell_bytes != 1111,
                "inspection hooks expose measured resident cell bytes");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_bytes(stream, 1),
                  2222,
                  "inspection hooks expose manifest estimates for unloaded cells");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_cell_material(stream, 0)),
                            "stone") == 0,
                "inspection hooks expose cell material metadata");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_layer(stream, 0),
                  rt_game3d_layers_world(),
                  "inspection hooks expose cell layer metadata");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_collision_mask(stream, 0),
                  rt_game3d_layers_world() | rt_game3d_layers_player(),
                  "inspection hooks expose cell collision mask metadata");
    EXPECT_TRUE(rt_game3d_world_stream_get_cell_collision_enabled(stream, 1) == 0,
                "inspection hooks expose nested cell collision-enabled metadata");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_cell_nav_area(stream, 0)),
                            "road") == 0,
                "inspection hooks expose cell nav-area metadata");
    EXPECT_NEAR(rt_game3d_world_stream_get_cell_traversal_cost(stream, 1),
                3.5,
                0.0001,
                "inspection hooks expose cell traversal-cost metadata");
    EXPECT_TRUE(std::strstr(rt_string_cstr(rt_game3d_world_stream_get_cell_sidecar(stream, 0)),
                            "meta/near_cell.bin") != nullptr,
                "inspection hooks expose resolved cell binary sidecar metadata");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_bytes(stream, 1),
                  4444,
                  "inspection hooks expose terrain tile byte estimates");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_terrain_tile_material(stream, 0)),
                    "grass") == 0,
        "inspection hooks expose terrain material metadata");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_layer(stream, 0),
                  rt_game3d_layers_world(),
                  "inspection hooks expose terrain collision layer metadata");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_collision_mask(stream, 0),
                  rt_game3d_layers_world() | rt_game3d_layers_player(),
                  "inspection hooks expose terrain collision mask metadata");
    EXPECT_TRUE(rt_game3d_world_stream_get_terrain_tile_collision_enabled(stream, 1) == 0,
                "inspection hooks expose nested terrain collision-enabled metadata");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_terrain_tile_nav_area(stream, 1)),
                    "cliff") == 0,
        "inspection hooks expose terrain nav-area metadata");
    EXPECT_NEAR(rt_game3d_world_stream_get_terrain_tile_traversal_cost(stream, 0),
                1.5,
                0.0001,
                "inspection hooks expose terrain traversal-cost metadata");
    EXPECT_TRUE(
        std::strstr(rt_string_cstr(rt_game3d_world_stream_get_terrain_tile_sidecar(stream, 1)),
                    "terrain/far.meta") != nullptr,
        "inspection hooks expose resolved terrain binary sidecar metadata");
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_cell_resident(stream, 0), 1, "near cell starts resident");
    EXPECT_EQ_INT(
        rt_game3d_world_stream_get_cell_resident(stream, 1), 0, "far cell starts nonresident");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_resident(stream, 0),
                  1,
                  "near terrain tile starts resident");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_resident(stream, 1),
                  0,
                  "far terrain tile starts nonresident");

    rt_game3d_world_stream_set_center(stream, rt_vec3_new(512.0, 0.0, 0.0));
    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_resident(stream, 0),
                  0,
                  "manifest-indexed near cell unloads after inspection-center move");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_resident(stream, 1),
                  1,
                  "manifest-indexed far cell becomes resident after inspection-center move");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_cell_name(stream, 0)),
                            "inspect_near") == 0,
                "cell metadata remains manifest-indexed when entry 0 is nonresident");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_cell_name(stream, 1)),
                            "inspect_far") == 0,
                "cell metadata remains manifest-indexed when entry 1 is resident");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_resident(stream, 0),
                  0,
                  "near terrain tile unloads after inspection-center move");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_resident(stream, 1),
                  0,
                  "far terrain tile waits behind the per-frame stream budget");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_pending_request_count(stream),
                  1,
                  "inspection-center move reports the deferred terrain request");

    rt_game3d_world_stream_update(stream, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_resident(stream, 1),
                  1,
                  "far terrain tile becomes resident after inspection-center move");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_pending_request_count(stream),
                  0,
                  "inspection-center stream work drains after a second update");

    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_cell_name(stream, -1)), "") ==
                    0,
                "invalid cell name query returns empty string");
    EXPECT_TRUE(rt_game3d_world_stream_get_cell_center(stream, 99) == nullptr,
                "invalid cell center query returns null");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_resident(stream, 99),
                  0,
                  "invalid cell resident query returns false");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_bytes(stream, 99),
                  0,
                  "invalid terrain byte query returns zero");

    auto *stream_view = static_cast<Game3DWorldStreamTestLayout *>(stream);
    EXPECT_TRUE(stream_view && stream_view->cells && stream_view->terrain_tiles,
                "stream inspection corruption test has parsed backing arrays");
    int32_t saved_cell_count = stream_view->cell_count;
    int32_t saved_cell_capacity = stream_view->cell_capacity;
    int32_t saved_tile_count = stream_view->terrain_tile_count;
    int32_t saved_tile_capacity = stream_view->terrain_tile_capacity;
    int64_t saved_resident_cells = stream_view->resident_cell_count;
    int64_t saved_resident_tiles = stream_view->resident_terrain_tile_count;
    int64_t saved_pending = stream_view->pending_request_count;
    int64_t saved_resident_bytes = stream_view->resident_bytes;
    int64_t saved_cell_sidecar_bytes = stream_view->cells[0].sidecar_bytes;
    int64_t saved_cell_layer = stream_view->cells[0].layer;
    double saved_cell_cost = stream_view->cells[0].traversal_cost;
    int8_t saved_cell_collision = stream_view->cells[0].collision_enabled;
    double saved_tile_cost = stream_view->terrain_tiles[0].traversal_cost;
    int8_t saved_tile_collision = stream_view->terrain_tiles[0].collision_enabled;

    stream_view->cell_count = 99;
    stream_view->cell_capacity = 1;
    stream_view->terrain_tile_count = 99;
    stream_view->terrain_tile_capacity = 1;
    stream_view->resident_cell_count = 99;
    stream_view->resident_terrain_tile_count = 99;
    stream_view->pending_request_count = -5;
    stream_view->resident_bytes = -77;
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_count(stream),
                  1,
                  "stream cell count clamps corrupt count to capacity");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_count(stream),
                  1,
                  "stream terrain tile count clamps corrupt count to capacity");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_cell_count(stream),
                  1,
                  "stream resident cell count clamps corrupt manifest-backed count");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_terrain_tile_count(stream),
                  1,
                  "stream resident terrain count clamps corrupt manifest-backed count");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_world_stream_get_cell_name(stream, 1)), "") ==
                    0,
                "stream cell accessors reject indexes past repaired count");
    EXPECT_TRUE(rt_game3d_world_stream_get_resident_terrain_tile(stream, 1) == nullptr,
                "stream resident terrain accessor rejects indexes past repaired count");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_pending_request_count(stream),
                  0,
                  "stream pending request getter rejects negative telemetry");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_resident_bytes(stream),
                  0,
                  "stream resident byte getter rejects negative telemetry");

    stream_view->cells[0].sidecar_bytes = -9;
    stream_view->cells[0].layer = -123;
    stream_view->cells[0].traversal_cost = -2.0;
    stream_view->cells[0].collision_enabled = 7;
    stream_view->terrain_tiles[0].traversal_cost = -4.0;
    stream_view->terrain_tiles[0].collision_enabled = 3;
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_sidecar_bytes(stream, 0),
                  0,
                  "stream cell sidecar byte getter rejects negative values");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_layer(stream, 0),
                  0,
                  "stream cell layer getter rejects invalid layer bits");
    EXPECT_NEAR(rt_game3d_world_stream_get_cell_traversal_cost(stream, 0),
                0.0,
                0.0001,
                "stream cell traversal getter rejects non-positive costs");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_cell_collision_enabled(stream, 0),
                  1,
                  "stream cell collision-enabled getter normalizes booleans");
    EXPECT_NEAR(rt_game3d_world_stream_get_terrain_tile_traversal_cost(stream, 0),
                0.0,
                0.0001,
                "stream terrain traversal getter rejects non-positive costs");
    EXPECT_EQ_INT(rt_game3d_world_stream_get_terrain_tile_collision_enabled(stream, 0),
                  1,
                  "stream terrain collision-enabled getter normalizes booleans");

    stream_view->cell_count = saved_cell_count;
    stream_view->cell_capacity = saved_cell_capacity;
    stream_view->terrain_tile_count = saved_tile_count;
    stream_view->terrain_tile_capacity = saved_tile_capacity;
    stream_view->resident_cell_count = saved_resident_cells;
    stream_view->resident_terrain_tile_count = saved_resident_tiles;
    stream_view->pending_request_count = saved_pending;
    stream_view->resident_bytes = saved_resident_bytes;
    stream_view->cells[0].sidecar_bytes = saved_cell_sidecar_bytes;
    stream_view->cells[0].layer = saved_cell_layer;
    stream_view->cells[0].traversal_cost = saved_cell_cost;
    stream_view->cells[0].collision_enabled = saved_cell_collision;
    stream_view->terrain_tiles[0].traversal_cost = saved_tile_cost;
    stream_view->terrain_tiles[0].collision_enabled = saved_tile_collision;

    rt_game3d_world_destroy(world);
    std::remove(near_path);
    std::remove(far_path);
    std::remove(cells_manifest_path);
    std::remove(terrain_manifest_path);
    PASS();
}

static bool test_phase4_body_def_attach_body() {
    TEST("BodyDef creates attached bodies with filters and sync policy");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D BodyDef Unit"), 80, 60);
    void *entity = rt_game3d_entity_new();
    rt_game3d_entity_set_position(entity, 1.0, 2.0, 3.0);
    rt_game3d_entity_set_layer(entity, rt_game3d_layers_player());

    void *def = rt_game3d_body_def_sphere(0.5, 2.0);
    rt_game3d_body_def_set_friction(def, 0.7);
    rt_game3d_body_def_set_restitution(def, 0.15);
    rt_game3d_body_def_set_use_ccd(def, 1);
    rt_game3d_body_def_with_mask(def, rt_game3d_layermask_of(rt_game3d_layers_world()));
    rt_game3d_body_def_with_sync(def, rt_game3d_sync_mode_body_from_node());

    rt_game3d_entity_attach_body(entity, def);
    void *body = rt_game3d_entity_get_body(entity);
    EXPECT_TRUE(body != nullptr, "attachBody accepts BodyDef");
    EXPECT_EQ_INT(rt_body3d_get_collision_layer(body),
                  rt_game3d_layers_player(),
                  "BodyDef without explicit layer inherits entity layer");
    EXPECT_EQ_INT(rt_body3d_get_collision_mask(body),
                  rt_game3d_layers_world(),
                  "BodyDef mask applies to attached body");
    EXPECT_NEAR(rt_body3d_get_mass(body), 2.0, 0.0001, "BodyDef mass applies");
    EXPECT_NEAR(rt_body3d_get_friction(body), 0.7, 0.0001, "BodyDef friction applies");
    EXPECT_NEAR(rt_body3d_get_restitution(body), 0.15, 0.0001, "BodyDef restitution applies");
    EXPECT_TRUE(rt_body3d_get_use_ccd(body) != 0, "BodyDef CCD applies");
    EXPECT_EQ_INT(rt_scene_node3d_get_sync_mode(rt_game3d_entity_get_node(entity)),
                  rt_game3d_sync_mode_body_from_node(),
                  "BodyDef sync mode applies to scene node");
    void *body_pos = rt_body3d_get_position(body);
    EXPECT_NEAR(rt_vec3_x(body_pos), 1.0, 0.0001, "attachBody syncs initial node X");
    EXPECT_NEAR(rt_vec3_y(body_pos), 2.0, 0.0001, "attachBody syncs initial node Y");
    EXPECT_NEAR(rt_vec3_z(body_pos), 3.0, 0.0001, "attachBody syncs initial node Z");
    rt_game3d_entity_set_position(entity, 4.0, 5.0, 6.0);
    body_pos = rt_body3d_get_position(body);
    EXPECT_NEAR(rt_vec3_x(body_pos), 4.0, 0.0001, "setPosition syncs body X");
    EXPECT_NEAR(rt_vec3_y(body_pos), 5.0, 0.0001, "setPosition syncs body Y");
    EXPECT_NEAR(rt_vec3_z(body_pos), 6.0, 0.0001, "setPosition syncs body Z");
    rt_game3d_entity_set_rotation_euler(entity, 0.0, 90.0, 0.0);
    {
        void *body_rot = rt_body3d_get_orientation(body);
        EXPECT_TRUE(std::fabs(rt_quat_x(body_rot)) + std::fabs(rt_quat_y(body_rot)) +
                            std::fabs(rt_quat_z(body_rot)) >
                        0.5,
                    "setRotationEuler syncs body orientation");
    }
    rt_game3d_entity_set_scale_xyz(entity, 2.0, 3.0, 4.0);
    void *body_scale = rt_body3d_get_scale(body);
    EXPECT_NEAR(rt_vec3_x(body_scale), 2.0, 0.0001, "setScale syncs body scale X");
    EXPECT_NEAR(rt_vec3_y(body_scale), 3.0, 0.0001, "setScale syncs body scale Y");
    EXPECT_NEAR(rt_vec3_z(body_scale), 4.0, 0.0001, "setScale syncs body scale Z");

    rt_game3d_world_spawn(world, entity);
    EXPECT_EQ_INT(rt_world3d_body_count(rt_game3d_world_get_physics(world)),
                  1,
                  "spawning BodyDef-attached entity registers the body");
    EXPECT_TRUE(rt_game3d_entity_attach_body(entity, body) == entity,
                "reattaching the same spawned body is a no-op");
    EXPECT_TRUE(rt_game3d_entity_get_body(entity) == body,
                "reattaching the same spawned body preserves the body");
    EXPECT_EQ_INT(rt_world3d_body_count(rt_game3d_world_get_physics(world)),
                  1,
                  "reattaching the same spawned body does not duplicate physics membership");

    void *zero_def = rt_game3d_body_def_box(1.0, 1.0, 1.0, 1.0);
    rt_game3d_body_def_set_mass(zero_def, 0.0);
    EXPECT_TRUE(rt_game3d_body_def_get_static(zero_def) != 0,
                "BodyDef zero mass switches to static mode");
    EXPECT_TRUE(rt_game3d_body_def_get_kinematic(zero_def) == 0,
                "BodyDef zero mass clears kinematic mode");
    void *zero_entity = rt_game3d_entity_new();
    rt_game3d_entity_attach_body(zero_entity, zero_def);
    EXPECT_TRUE(rt_body3d_is_static(rt_game3d_entity_get_body(zero_entity)) != 0,
                "zero-mass BodyDef creates a static body");

    void *kinematic_def = rt_game3d_body_def_sphere(0.5, 0.0);
    rt_game3d_body_def_set_kinematic(kinematic_def, 1);
    EXPECT_TRUE(rt_game3d_body_def_get_kinematic(kinematic_def) != 0,
                "BodyDef kinematic flag is retained");
    EXPECT_TRUE(rt_game3d_body_def_get_static(kinematic_def) == 0,
                "BodyDef kinematic clears static mode");
    EXPECT_NEAR(rt_game3d_body_def_get_mass(kinematic_def),
                1.0,
                0.0001,
                "BodyDef kinematic gets a positive mass");
    void *kinematic_entity = rt_game3d_entity_new();
    rt_game3d_entity_attach_body(kinematic_entity, kinematic_def);
    EXPECT_TRUE(rt_body3d_is_kinematic(rt_game3d_entity_get_body(kinematic_entity)) != 0,
                "kinematic BodyDef creates a kinematic body");
    EXPECT_TRUE(rt_body3d_is_static(rt_game3d_entity_get_body(kinematic_entity)) == 0,
                "kinematic BodyDef body is not static");

    void *toggle_def = rt_game3d_body_def_sphere(0.5, 1.0);
    rt_game3d_body_def_set_kinematic(toggle_def, 1);
    rt_game3d_body_def_set_static(toggle_def, 1);
    EXPECT_TRUE(rt_game3d_body_def_get_static(toggle_def) != 0,
                "BodyDef static setter enables static mode");
    EXPECT_TRUE(rt_game3d_body_def_get_kinematic(toggle_def) == 0,
                "BodyDef static setter clears kinematic mode");
    EXPECT_NEAR(
        rt_game3d_body_def_get_mass(toggle_def), 0.0, 0.0001, "BodyDef static setter zeroes mass");
    rt_game3d_body_def_set_static(toggle_def, 0);
    EXPECT_TRUE(rt_game3d_body_def_get_static(toggle_def) == 0,
                "BodyDef static setter can restore dynamic mode");
    EXPECT_NEAR(rt_game3d_body_def_get_mass(toggle_def),
                1.0,
                0.0001,
                "BodyDef dynamic restore keeps a positive default mass");

    auto *corrupt_def = static_cast<Game3DBodyDefTestLayout *>(toggle_def);
    corrupt_def->shape = 99;
    corrupt_def->mass = -INFINITY;
    corrupt_def->friction = INFINITY;
    corrupt_def->restitution = NAN;
    corrupt_def->layer = 0;
    corrupt_def->sync_mode = 99;
    corrupt_def->is_static = -9;
    corrupt_def->is_kinematic = -8;
    corrupt_def->is_trigger = -7;
    corrupt_def->use_ccd = -6;
    EXPECT_EQ_INT(rt_game3d_body_def_get_shape(toggle_def),
                  rt_game3d_body_shape_box(),
                  "BodyDef shape getter defaults corrupt private shape to box");
    EXPECT_NEAR(rt_game3d_body_def_get_mass(toggle_def),
                0.0,
                0.0001,
                "BodyDef mass getter clamps corrupt private mass");
    EXPECT_NEAR(rt_game3d_body_def_get_friction(toggle_def),
                0.0,
                0.0001,
                "BodyDef friction getter clamps corrupt private friction");
    EXPECT_NEAR(rt_game3d_body_def_get_restitution(toggle_def),
                0.0,
                0.0001,
                "BodyDef restitution getter clamps corrupt private restitution");
    EXPECT_EQ_INT(rt_game3d_body_def_get_layer(toggle_def),
                  rt_game3d_layers_dynamic(),
                  "BodyDef layer getter defaults corrupt private layer");
    EXPECT_EQ_INT(rt_game3d_body_def_get_sync_mode(toggle_def),
                  rt_game3d_sync_mode_node_from_body(),
                  "BodyDef sync getter defaults corrupt private sync mode");
    EXPECT_TRUE(rt_game3d_body_def_get_static(toggle_def) == 1 &&
                    rt_game3d_body_def_get_kinematic(toggle_def) == 1 &&
                    rt_game3d_body_def_get_trigger(toggle_def) == 1 &&
                    rt_game3d_body_def_get_use_ccd(toggle_def) == 1,
                "BodyDef boolean getters normalize corrupt private flags");

    void *trigger = rt_game3d_entity_new();
    void *trigger_def = rt_game3d_body_def_static_box(1.0, 1.0, 1.0);
    rt_game3d_body_def_with_layer(trigger_def, rt_game3d_layers_trigger());
    rt_game3d_body_def_as_trigger(trigger_def);
    rt_game3d_entity_attach_body(trigger, trigger_def);
    rt_game3d_entity_set_position(trigger, 0.0, 3.0, 0.0);
    EXPECT_EQ_INT(rt_game3d_entity_get_layer(trigger),
                  rt_game3d_layers_trigger(),
                  "explicit BodyDef layer applies to entity");
    EXPECT_TRUE(rt_body3d_is_static(rt_game3d_entity_get_body(trigger)) != 0,
                "StaticBox creates a static body");
    EXPECT_TRUE(rt_body3d_is_trigger(rt_game3d_entity_get_body(trigger)) != 0,
                "asTrigger marks the body trigger-only");

    void *ground = rt_game3d_entity_new();
    rt_game3d_entity_attach_body(ground, rt_game3d_body_def_static_plane(10.0));
    EXPECT_EQ_INT(rt_game3d_entity_get_layer(ground),
                  rt_game3d_layers_world(),
                  "StaticPlane defaults to the World layer");
    EXPECT_TRUE(rt_body3d_is_static(rt_game3d_entity_get_body(ground)) != 0,
                "StaticPlane creates a static body");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_shared_body_attachment_rejected_without_state_leak() {
    TEST("Game3D rejects sharing one Physics3DBody across spawned entities");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Shared Body Unit"), 80, 60);
    void *owner = rt_game3d_entity_new();
    rt_game3d_entity_attach_body(owner, rt_game3d_body_def_sphere(0.5, 1.0));
    void *shared = rt_game3d_entity_get_body(owner);
    EXPECT_TRUE(shared != nullptr, "owner has a body to share");
    rt_game3d_world_spawn(world, owner);
    EXPECT_EQ_INT(rt_world3d_body_count(rt_game3d_world_get_physics(world)),
                  1,
                  "owner body is registered once");

    void *pending = rt_game3d_entity_new();
    rt_game3d_entity_attach_body(pending, shared);
    EXPECT_TRUE(
        expect_trap_contains([&]() { rt_game3d_world_spawn(world, pending); }, "already attached"),
        "spawn rejects a body already owned by another entity");
    EXPECT_TRUE(rt_game3d_entity_is_spawned(pending) == 0,
                "failed shared-body spawn rolls back spawned flag");
    EXPECT_EQ_INT(rt_world3d_body_count(rt_game3d_world_get_physics(world)),
                  1,
                  "failed shared-body spawn does not duplicate physics body");

    void *spawned = rt_game3d_entity_new();
    rt_game3d_world_spawn(world, spawned);
    EXPECT_TRUE(expect_trap_contains([&]() { rt_game3d_entity_attach_body(spawned, shared); },
                                     "already attached"),
                "attachBody rejects a body already owned by another spawned entity");
    EXPECT_TRUE(rt_game3d_entity_get_body(spawned) == nullptr,
                "failed attachBody leaves the spawned entity bodyless");
    EXPECT_EQ_INT(rt_world3d_body_count(rt_game3d_world_get_physics(world)),
                  1,
                  "failed attachBody preserves physics body count");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_entity_transform_sanitizes_and_respects_sync_mode() {
    TEST("Entity3D transform setters sanitize values and respect physics sync mode");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Transform Sync Unit"), 80, 60);
    void *entity = rt_game3d_entity_new();
    rt_game3d_entity_set_position(entity, 1.0, 2.0, 3.0);
    rt_game3d_entity_attach_body(entity, rt_game3d_body_def_sphere(0.5, 1.0));
    void *body = rt_game3d_entity_get_body(entity);
    void *node = rt_game3d_entity_get_node(entity);
    EXPECT_TRUE(body != nullptr, "BodyDef attach creates a body");
    EXPECT_EQ_INT(rt_scene_node3d_get_sync_mode(node),
                  rt_game3d_sync_mode_node_from_body(),
                  "BodyDef default sync mode is node-from-body");

    rt_game3d_world_spawn(world, entity);
    void *body_pos = rt_body3d_get_position(body);
    EXPECT_NEAR(rt_vec3_x(body_pos), 1.0, 0.0001, "spawn force-syncs initial body X");
    EXPECT_NEAR(rt_vec3_y(body_pos), 2.0, 0.0001, "spawn force-syncs initial body Y");
    EXPECT_NEAR(rt_vec3_z(body_pos), 3.0, 0.0001, "spawn force-syncs initial body Z");

    rt_game3d_entity_set_position(entity, NAN, 4.0, INFINITY);
    void *node_pos = rt_scene_node3d_get_position(node);
    EXPECT_NEAR(rt_vec3_x(node_pos), 0.0, 0.0001, "non-finite position X falls back to zero");
    EXPECT_NEAR(rt_vec3_y(node_pos), 4.0, 0.0001, "finite position Y is retained");
    EXPECT_NEAR(rt_vec3_z(node_pos), 0.0, 0.0001, "non-finite position Z falls back to zero");
    body_pos = rt_body3d_get_position(body);
    EXPECT_NEAR(rt_vec3_x(body_pos), 1.0, 0.0001, "node-from-body mode does not push X");
    EXPECT_NEAR(rt_vec3_y(body_pos), 2.0, 0.0001, "node-from-body mode does not push Y");
    EXPECT_NEAR(rt_vec3_z(body_pos), 3.0, 0.0001, "node-from-body mode does not push Z");

    rt_game3d_entity_set_scale_xyz(entity, 0.0, NAN, INFINITY);
    void *node_scale = rt_scene_node3d_get_scale(node);
    EXPECT_NEAR(rt_vec3_x(node_scale), 1.0, 0.0001, "zero scale X falls back to one");
    EXPECT_NEAR(rt_vec3_y(node_scale), 1.0, 0.0001, "non-finite scale Y falls back to one");
    EXPECT_NEAR(rt_vec3_z(node_scale), 1.0, 0.0001, "non-finite scale Z falls back to one");

    rt_game3d_entity_set_rotation_euler(entity, NAN, 90.0, INFINITY);
    void *node_rot = rt_scene_node3d_get_rotation(node);
    EXPECT_TRUE(std::isfinite(rt_quat_x(node_rot)) && std::isfinite(rt_quat_y(node_rot)) &&
                    std::isfinite(rt_quat_z(node_rot)) && std::isfinite(rt_quat_w(node_rot)),
                "non-finite rotation components produce a finite quaternion");
    void *body_rot = rt_body3d_get_orientation(body);
    EXPECT_NEAR(rt_quat_x(body_rot), 0.0, 0.0001, "node-from-body mode does not push rotation X");
    EXPECT_NEAR(rt_quat_y(body_rot), 0.0, 0.0001, "node-from-body mode does not push rotation Y");
    EXPECT_NEAR(rt_quat_z(body_rot), 0.0, 0.0001, "node-from-body mode does not push rotation Z");
    EXPECT_NEAR(rt_quat_w(body_rot), 1.0, 0.0001, "node-from-body mode does not push rotation W");

    rt_scene_node3d_set_sync_mode(node, rt_game3d_sync_mode_body_from_node());
    rt_game3d_entity_set_position(entity, 7.0, 8.0, 9.0);
    body_pos = rt_body3d_get_position(body);
    EXPECT_NEAR(rt_vec3_x(body_pos), 7.0, 0.0001, "body-from-node mode pushes X");
    EXPECT_NEAR(rt_vec3_y(body_pos), 8.0, 0.0001, "body-from-node mode pushes Y");
    EXPECT_NEAR(rt_vec3_z(body_pos), 9.0, 0.0001, "body-from-node mode pushes Z");

    rt_game3d_world_destroy(world);
    PASS();
}

static int event_mentions(void *event, void *a, void *b) {
    return (rt_game3d_collision_event_get_a(event) == a &&
            rt_game3d_collision_event_get_b(event) == b) ||
           (rt_game3d_collision_event_get_a(event) == b &&
            rt_game3d_collision_event_get_b(event) == a);
}

static bool test_phase4_collision_events_wrapped_with_entities() {
    TEST("Collision3DEvent wraps raw contacts with phases and owning entities");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Collision Unit"), 80, 60);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);

    void *ground = rt_game3d_entity_new();
    rt_game3d_entity_set_name(ground, rt_const_cstr("Ground"));
    rt_game3d_entity_attach_body(ground, rt_game3d_body_def_static_plane(8.0));
    rt_game3d_entity_set_position(ground, 0.0, 0.0, 0.0);

    void *ball = rt_game3d_entity_new();
    rt_game3d_entity_set_name(ball, rt_const_cstr("Ball"));
    void *ball_def = rt_game3d_body_def_sphere(0.5, 1.0);
    rt_game3d_body_def_with_mask(ball_def, rt_game3d_layermask_of(rt_game3d_layers_world()));
    rt_game3d_body_def_with_sync(ball_def, rt_game3d_sync_mode_body_from_node());
    rt_game3d_entity_attach_body(ball, ball_def);
    rt_game3d_entity_set_position(ball, 0.0, 0.45, 0.0);

    rt_game3d_world_spawn(world, ground);
    rt_game3d_world_spawn(world, ball);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);

    EXPECT_TRUE(rt_game3d_world_collision_event_count(world, rt_game3d_collision_enter()) > 0,
                "first overlap emits enter event");
    void *enter = rt_game3d_world_collision_event(world, rt_game3d_collision_enter(), 0);
    EXPECT_TRUE(enter != nullptr, "collisionEvent returns Game3D event wrapper");
    EXPECT_EQ_INT(rt_game3d_collision_event_get_phase(enter),
                  rt_game3d_collision_enter(),
                  "enter event reports Enter phase");
    EXPECT_TRUE(event_mentions(enter, ground, ball), "event maps bodies back to owning entities");
    EXPECT_TRUE(rt_game3d_collision_event_other(enter, ground) == ball,
                "Collision3DEvent.other returns the opposite entity");
    EXPECT_TRUE(rt_game3d_collision_event_get_raw(enter) != nullptr,
                "Collision3DEvent exposes raw Graphics3D event");
    EXPECT_TRUE(rt_game3d_collision_event_point(enter) != nullptr,
                "Collision3DEvent.point returns a Vec3");
    EXPECT_TRUE(rt_game3d_collision_event_normal(enter) != nullptr,
                "Collision3DEvent.normal returns a Vec3");
    EXPECT_EQ_INT(rt_game3d_collision_event_get_contact_count(enter),
                  rt_collision_event3d_get_contact_count(rt_game3d_collision_event_get_raw(enter)),
                  "Collision3DEvent.contactCount mirrors the wrapped raw event");
    EXPECT_TRUE(rt_game3d_collision_event_get_contact_count(enter) >= 1,
                "Collision3DEvent exposes at least one contact for an overlap");
    void *first_point = rt_game3d_collision_event_point(enter);
    void *indexed_point = rt_game3d_collision_event_contact_point(enter, 0);
    EXPECT_NEAR(rt_vec3_x(indexed_point),
                rt_vec3_x(first_point),
                0.0001,
                "contactPoint(0) matches point() X");
    EXPECT_NEAR(rt_vec3_y(indexed_point),
                rt_vec3_y(first_point),
                0.0001,
                "contactPoint(0) matches point() Y");
    EXPECT_NEAR(rt_vec3_z(indexed_point),
                rt_vec3_z(first_point),
                0.0001,
                "contactPoint(0) matches point() Z");
    void *first_normal = rt_game3d_collision_event_normal(enter);
    void *indexed_normal = rt_game3d_collision_event_contact_normal(enter, 0);
    EXPECT_NEAR(rt_vec3_x(indexed_normal),
                rt_vec3_x(first_normal),
                0.0001,
                "contactNormal(0) matches normal() X");
    EXPECT_NEAR(rt_vec3_y(indexed_normal),
                rt_vec3_y(first_normal),
                0.0001,
                "contactNormal(0) matches normal() Y");
    EXPECT_NEAR(rt_vec3_z(indexed_normal),
                rt_vec3_z(first_normal),
                0.0001,
                "contactNormal(0) matches normal() Z");
    EXPECT_TRUE(rt_game3d_collision_event_contact_separation(enter, 0) <= 0.0,
                "contactSeparation(0) reports penetration for overlap");
    void *missing_point = rt_game3d_collision_event_contact_point(enter, 99);
    void *missing_normal = rt_game3d_collision_event_contact_normal(enter, 99);
    EXPECT_NEAR(rt_vec3_x(missing_point), 0.0, 0.0001, "missing contact point X fallback");
    EXPECT_NEAR(rt_vec3_y(missing_point), 0.0, 0.0001, "missing contact point Y fallback");
    EXPECT_NEAR(rt_vec3_z(missing_point), 0.0, 0.0001, "missing contact point Z fallback");
    EXPECT_NEAR(rt_vec3_x(missing_normal), 0.0, 0.0001, "missing contact normal X fallback");
    EXPECT_NEAR(rt_vec3_y(missing_normal), 1.0, 0.0001, "missing contact normal Y fallback");
    EXPECT_NEAR(rt_vec3_z(missing_normal), 0.0, 0.0001, "missing contact normal Z fallback");
    EXPECT_NEAR(rt_game3d_collision_event_contact_separation(enter, 99),
                0.0,
                0.0001,
                "missing contact separation fallback");
    EXPECT_TRUE(rt_game3d_world_collision_event_count(world, rt_game3d_collision_any()) >=
                    rt_game3d_world_collision_event_count(world, rt_game3d_collision_enter()),
                "Any phase includes transition buffers");

    auto *enter_view = static_cast<Game3DCollisionEventTestLayout *>(enter);
    int64_t saved_enter_phase = enter_view->phase;
    void *saved_enter_a = enter_view->a;
    void *saved_enter_b = enter_view->b;
    void *saved_enter_raw = enter_view->raw;
    void *wrong_event_ref = rt_material3d_new_color(0.7, 0.2, 0.2);
    enter_view->phase = INT64_MAX;
    enter_view->a = wrong_event_ref;
    enter_view->b = wrong_event_ref;
    enter_view->raw = wrong_event_ref;
    EXPECT_EQ_INT(rt_game3d_collision_event_get_phase(enter),
                  rt_game3d_collision_any(),
                  "Collision3DEvent clamps corrupted phases to Any");
    EXPECT_TRUE(rt_game3d_collision_event_get_a(enter) == nullptr,
                "Collision3DEvent ignores wrong-class entity A refs");
    EXPECT_TRUE(rt_game3d_collision_event_get_b(enter) == nullptr,
                "Collision3DEvent ignores wrong-class entity B refs");
    EXPECT_TRUE(rt_game3d_collision_event_get_raw(enter) == nullptr,
                "Collision3DEvent ignores wrong-class raw refs");
    EXPECT_TRUE(rt_game3d_collision_event_get_is_trigger(enter) == 0,
                "Collision3DEvent trigger flag falls back for wrong-class raw refs");
    EXPECT_NEAR(rt_game3d_collision_event_get_relative_speed(enter),
                0.0,
                0.0001,
                "Collision3DEvent relative speed falls back for wrong-class raw refs");
    EXPECT_NEAR(rt_game3d_collision_event_get_normal_impulse(enter),
                0.0,
                0.0001,
                "Collision3DEvent normal impulse falls back for wrong-class raw refs");
    EXPECT_EQ_INT(rt_game3d_collision_event_get_contact_count(enter),
                  0,
                  "Collision3DEvent contact count falls back for wrong-class raw refs");
    void *corrupt_point = rt_game3d_collision_event_contact_point(enter, 0);
    void *corrupt_normal = rt_game3d_collision_event_contact_normal(enter, 0);
    EXPECT_NEAR(rt_vec3_x(corrupt_point), 0.0, 0.0001, "corrupt contact point X fallback");
    EXPECT_NEAR(rt_vec3_y(corrupt_point), 0.0, 0.0001, "corrupt contact point Y fallback");
    EXPECT_NEAR(rt_vec3_z(corrupt_point), 0.0, 0.0001, "corrupt contact point Z fallback");
    EXPECT_NEAR(rt_vec3_x(corrupt_normal), 0.0, 0.0001, "corrupt contact normal X fallback");
    EXPECT_NEAR(rt_vec3_y(corrupt_normal), 1.0, 0.0001, "corrupt contact normal Y fallback");
    EXPECT_NEAR(rt_vec3_z(corrupt_normal), 0.0, 0.0001, "corrupt contact normal Z fallback");
    EXPECT_NEAR(rt_game3d_collision_event_contact_separation(enter, 0),
                0.0,
                0.0001,
                "corrupt contact separation fallback");
    EXPECT_TRUE(rt_game3d_collision_event_other(enter, ground) == nullptr,
                "Collision3DEvent.other ignores wrong-class entity refs");
    enter_view->phase = saved_enter_phase;
    enter_view->a = saved_enter_a;
    enter_view->b = saved_enter_b;
    enter_view->raw = saved_enter_raw;
    if (wrong_event_ref && rt_obj_release_check0(wrong_event_ref))
        rt_obj_free(wrong_event_ref);

    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_world_collision_event_count(world, rt_game3d_collision_stay()) > 0,
                "second overlap emits stay event");

    rt_game3d_entity_set_position(ball, 0.0, 5.0, 0.0);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_world_collision_event_count(world, rt_game3d_collision_exit()) > 0,
                "separating bodies emits exit event");
    void *exit = rt_game3d_world_collision_event(world, rt_game3d_collision_exit(), 0);
    EXPECT_EQ_INT(rt_game3d_collision_event_get_phase(exit),
                  rt_game3d_collision_exit(),
                  "exit event reports Exit phase");

    rt_game3d_world_destroy(world);

    world = rt_game3d_world_new(rt_const_cstr("Game3D Trigger Collision Unit"), 80, 60);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *trigger = rt_game3d_entity_new();
    void *trigger_def = rt_game3d_body_def_static_box(1.0, 1.0, 1.0);
    rt_game3d_body_def_with_layer(trigger_def, rt_game3d_layers_trigger());
    rt_game3d_body_def_as_trigger(trigger_def);
    rt_game3d_entity_attach_body(trigger, trigger_def);
    rt_game3d_entity_set_position(trigger, 0.0, 0.0, 0.0);
    ball = rt_game3d_entity_new();
    ball_def = rt_game3d_body_def_sphere(0.5, 1.0);
    void *trigger_mask = rt_game3d_layermask_of(rt_game3d_layers_world());
    rt_game3d_layermask_include(trigger_mask, rt_game3d_layers_trigger());
    rt_game3d_body_def_with_mask(ball_def, trigger_mask);
    rt_game3d_entity_attach_body(ball, ball_def);
    rt_game3d_entity_set_position(ball, 0.0, 0.0, 0.0);
    rt_game3d_world_spawn(world, trigger);
    rt_game3d_world_spawn(world, ball);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *trigger_event = rt_game3d_world_collision_event(world, rt_game3d_collision_enter(), 0);
    EXPECT_TRUE(trigger_event != nullptr, "trigger overlap emits an event");
    EXPECT_TRUE(rt_game3d_collision_event_get_is_trigger(trigger_event) != 0,
                "trigger event reports trigger status");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_phase5_animator3d_events_and_root_motion() {
    TEST("Animator3D wraps controllers, reports events, and drives world root motion");

    void *controller = make_game3d_test_controller(10.0, 0.5);
    void *animator = rt_game3d_animator_new(controller);
    EXPECT_TRUE(animator != nullptr, "Animator3D.New returns an object");
    EXPECT_TRUE(rt_game3d_animator_get_controller(animator) == controller,
                "Animator3D exposes the wrapped controller");
    EXPECT_TRUE(rt_game3d_animator_play(animator, rt_const_cstr("walk")) != 0,
                "Animator3D.play starts a named state");
    EXPECT_TRUE(rt_game3d_animator_is_playing(animator, rt_const_cstr("walk")) != 0,
                "Animator3D.isPlaying checks the active state");
    rt_game3d_animator_set_speed(animator, rt_const_cstr("walk"), 1.0);
    rt_game3d_animator_update(animator, 0.5);
    EXPECT_NEAR(rt_game3d_animator_state_time(animator),
                0.5,
                0.0001,
                "Animator3D.stateTime reports base layer time");
    EXPECT_EQ_INT(rt_game3d_animator_event_count(animator), 1, "Animator3D captures events");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_animator_event_name(animator, 0)), "step") ==
                    0,
                "Animator3D.eventName returns the captured event");

    const int32_t kGame3DAnimEventMax = 64;
    void *overflow_controller = make_game3d_test_controller(1.0, 0.25);
    for (int32_t i = 0; i < kGame3DAnimEventMax + 8; ++i) {
        char event_name[32];
        std::snprintf(event_name, sizeof(event_name), "overflow%d", i);
        rt_anim_controller3d_add_event(
            overflow_controller, rt_const_cstr("walk"), 0.1, rt_const_cstr(event_name));
    }
    void *overflow_animator = rt_game3d_animator_new(overflow_controller);
    rt_game3d_animator_play(overflow_animator, rt_const_cstr("walk"));
    rt_game3d_animator_update(overflow_animator, 0.2);
    EXPECT_EQ_INT(rt_game3d_animator_event_count(overflow_animator),
                  kGame3DAnimEventMax,
                  "Animator3D caps frame events at the public buffer size");
    rt_game3d_animator_update(overflow_animator, 0.0);
    EXPECT_EQ_INT(rt_game3d_animator_event_count(overflow_animator),
                  0,
                  "Animator3D drains capped controller events between frames");
    auto *overflow_layout = static_cast<Game3DAnimatorTestLayout *>(overflow_animator);
    overflow_layout->event_count = INT32_MAX;
    rt_game3d_animator_update(overflow_animator, 0.0);
    EXPECT_EQ_INT(rt_game3d_animator_event_count(overflow_animator),
                  0,
                  "Animator3D repairs corrupt private event counts before draining");

    EXPECT_TRUE(rt_game3d_animator_crossfade(animator, rt_const_cstr("idle"), 0.1) != 0,
                "Animator3D.crossfade switches to another state");
    EXPECT_TRUE(rt_game3d_animator_is_playing(animator, rt_const_cstr("idle")) != 0,
                "Animator3D.isPlaying reflects crossfades");

    void *layer_skel = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(layer_skel, rt_const_cstr("root"), -1, rt_mat4_identity());
    int64_t arm_bone = rt_skeleton3d_add_bone(
        layer_skel, rt_const_cstr("arm"), 0, rt_mat4_translate(0.0, 1.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(layer_skel);
    void *base = make_game3d_test_anim("base", arm_bone, 0.0, 2.0, 0.0, 0.0, 2.0, 0.0);
    void *raise = make_game3d_test_anim("raise", arm_bone, 0.0, 3.0, 0.0, 0.0, 3.0, 0.0);
    void *reach = make_game3d_test_anim("reach", arm_bone, 0.0, 5.0, 0.0, 0.0, 5.0, 0.0);
    void *layer_controller = rt_anim_controller3d_new(layer_skel);
    rt_anim_controller3d_add_state(layer_controller, rt_const_cstr("base"), base);
    rt_anim_controller3d_add_state(layer_controller, rt_const_cstr("raise"), raise);
    rt_anim_controller3d_add_state(layer_controller, rt_const_cstr("reach"), reach);
    rt_anim_controller3d_add_event(
        layer_controller, rt_const_cstr("raise"), 0.0, rt_const_cstr("raiseEnter"));
    rt_anim_controller3d_add_event(
        layer_controller, rt_const_cstr("reach"), 0.0, rt_const_cstr("reachEnter"));
    void *layer_animator = rt_game3d_animator_new(layer_controller);
    rt_game3d_animator_play(layer_animator, rt_const_cstr("base"));
    rt_anim_controller3d_set_layer_mask(layer_controller, 1, arm_bone);
    rt_anim_controller3d_set_layer_weight(layer_controller, 1, 1.0);
    EXPECT_TRUE(rt_game3d_animator_play_layer_additive(layer_animator, 0, rt_const_cstr("raise")) ==
                    0,
                "Animator3D.playLayerAdditive rejects the base layer");
    EXPECT_TRUE(rt_game3d_animator_play_layer_additive(layer_animator, 1, rt_const_cstr("raise")) !=
                    0,
                "Animator3D.playLayerAdditive forwards to the wrapped controller");
    EXPECT_EQ_INT(rt_game3d_animator_event_count(layer_animator),
                  1,
                  "Animator3D.playLayerAdditive captures layer entry events");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_animator_event_name(layer_animator, 0)),
                            "raiseEnter") == 0,
                "Animator3D.playLayerAdditive preserves layer event names");
    rt_game3d_animator_update(layer_animator, 0.0);
    void *arm_mat = rt_anim_controller3d_get_bone_matrix(layer_controller, arm_bone);
    EXPECT_NEAR(rt_mat4_get(arm_mat, 1, 3),
                4.0,
                0.1,
                "Animator3D.playLayerAdditive applies bind-pose delta overlays");
    EXPECT_TRUE(rt_game3d_animator_crossfade_layer_additive(
                    layer_animator, 0, rt_const_cstr("reach"), 1.0) == 0,
                "Animator3D.crossfadeLayerAdditive rejects the base layer");
    EXPECT_TRUE(rt_game3d_animator_crossfade_layer_additive(
                    layer_animator, 1, rt_const_cstr("reach"), 1.0) != 0,
                "Animator3D.crossfadeLayerAdditive forwards to the wrapped controller");
    EXPECT_EQ_INT(rt_game3d_animator_event_count(layer_animator),
                  1,
                  "Animator3D.crossfadeLayerAdditive captures layer entry events");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_animator_event_name(layer_animator, 0)),
                            "reachEnter") == 0,
                "Animator3D.crossfadeLayerAdditive preserves layer event names");
    rt_game3d_animator_update(layer_animator, 0.5);
    arm_mat = rt_anim_controller3d_get_bone_matrix(layer_controller, arm_bone);
    EXPECT_NEAR(rt_mat4_get(arm_mat, 1, 3),
                5.0,
                0.15,
                "Animator3D.crossfadeLayerAdditive blends additive overlay deltas");

    void *bt_idle = make_game3d_test_anim("btIdle", 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    void *bt_lean = make_game3d_test_anim("btLean", 0, 8.0, 0.0, 0.0, 8.0, 0.0, 0.0);
    void *blend_tree = rt_blend_tree3d_new_1d(layer_skel);
    rt_blend_tree3d_add_sample(blend_tree, bt_idle, 0.0, 0.0);
    rt_blend_tree3d_add_sample(blend_tree, bt_lean, 1.0, 0.0);
    rt_blend_tree3d_set_param(blend_tree, 0.25, 0.0);
    EXPECT_TRUE(rt_game3d_animator_set_blend_tree(layer_animator, blend_tree) != 0,
                "Animator3D.setBlendTree forwards to the wrapped controller");
    rt_game3d_animator_update(layer_animator, 0.0);
    void *root_mat = rt_anim_controller3d_get_bone_matrix(layer_controller, 0);
    EXPECT_NEAR(rt_mat4_get(root_mat, 0, 3),
                2.0,
                0.1,
                "Animator3D.setBlendTree drives controller base pose");
    EXPECT_TRUE(rt_game3d_animator_set_blend_tree(layer_animator, layer_skel) == 0,
                "Animator3D.setBlendTree rejects non-tree handles");

    void *ik_skel = rt_skeleton3d_new();
    int64_t hip = rt_skeleton3d_add_bone(ik_skel, rt_const_cstr("hip"), -1, rt_mat4_identity());
    int64_t knee = rt_skeleton3d_add_bone(
        ik_skel, rt_const_cstr("knee"), hip, rt_mat4_translate(1.0, 0.0, 0.0));
    int64_t foot = rt_skeleton3d_add_bone(
        ik_skel, rt_const_cstr("foot"), knee, rt_mat4_translate(1.0, 0.0, 0.0));
    rt_skeleton3d_compute_inverse_bind(ik_skel);
    void *ik_controller = rt_anim_controller3d_new(ik_skel);
    void *ik_animator = rt_game3d_animator_new(ik_controller);
    void *ik_solver = rt_ik_solver3d_two_bone(ik_skel, hip, knee, foot);
    rt_ik_solver3d_set_target(ik_solver, rt_vec3_new(1.0, 1.0, 0.0));
    EXPECT_TRUE(rt_game3d_animator_set_ik_solver(ik_animator, ik_solver) != 0,
                "Animator3D.setIKSolver forwards to the wrapped controller");
    void *foot_mat = rt_anim_controller3d_get_bone_matrix(ik_controller, foot);
    EXPECT_NEAR(rt_mat4_get(foot_mat, 0, 3), 1.0, 0.03, "Animator3D.setIKSolver reaches target x");
    EXPECT_NEAR(rt_mat4_get(foot_mat, 1, 3), 1.0, 0.03, "Animator3D.setIKSolver reaches target y");
    EXPECT_TRUE(rt_game3d_animator_set_ik_solver(ik_animator, ik_skel) == 0,
                "Animator3D.setIKSolver rejects non-solver handles");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Animator Unit"), 80, 60);
    void *entity = rt_game3d_entity_new();
    void *controller2 = make_game3d_test_controller(10.0, 0.25);
    void *animator2 = rt_game3d_animator_new(controller2);
    rt_game3d_entity_attach_animator(entity, animator2);
    EXPECT_TRUE(rt_game3d_entity_get_anim(entity) == animator2,
                "Entity3D.attachAnimator stores the Game3D animator");
    EXPECT_TRUE(rt_scene_node3d_get_animator(rt_game3d_entity_get_node(entity)) == controller2,
                "Entity3D.attachAnimator binds the raw controller to the node");
    rt_scene_node3d_set_sync_mode(rt_game3d_entity_get_node(entity),
                                  rt_game3d_sync_mode_node_from_anim_root_motion());
    rt_game3d_animator_play(animator2, rt_const_cstr("walk"));
    rt_game3d_world_spawn(world, entity);
    rt_game3d_world_step_simulation(world, 0.25);

    void *pos = rt_game3d_entity_position(entity);
    EXPECT_NEAR(rt_vec3_x(pos), 2.5, 0.1, "World3D advances animators before scene sync");
    EXPECT_EQ_INT(rt_game3d_animator_event_count(animator2),
                  1,
                  "World3D animation update exposes frame events");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_game3d_animator_event_name(animator2, 0)), "step") ==
                    0,
                "World3D animation update preserves event names");

    void *entity2 = rt_game3d_entity_new();
    void *controller3 = make_game3d_test_controller(4.0, 0.25);
    rt_game3d_entity_attach_animator(entity2, controller3);
    EXPECT_TRUE(rt_game3d_entity_get_anim(entity2) != nullptr,
                "Entity3D.attachAnimator accepts raw AnimController3D");
    EXPECT_TRUE(rt_game3d_animator_get_controller(rt_game3d_entity_get_anim(entity2)) ==
                    controller3,
                "raw controller attach creates an Animator3D wrapper");

    void *node_entity = rt_game3d_entity_new();
    void *node_clip = rt_node_animation3d_new(rt_const_cstr("nodeMove"), 1.0);
    double node_times[2] = {0.0, 1.0};
    float node_values[6] = {0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f};
    EXPECT_TRUE(rt_node_animation3d_add_channel(node_clip,
                                                rt_const_cstr("game_node_anim_target"),
                                                RT_NODE_ANIM_PATH_TRANSLATION,
                                                RT_NODE_ANIM_INTERP_LINEAR,
                                                2,
                                                3,
                                                node_times,
                                                node_values) >= 0,
                "NodeAnimation3D fixture accepts Game3D entity transform channels");
    void *node_animator = rt_node_animator3d_new(node_clip);
    rt_scene_node3d_set_name(rt_game3d_entity_get_node(node_entity),
                             rt_const_cstr("game_node_anim_target"));
    rt_game3d_entity_attach_animator(node_entity, node_animator);
    void *wrapped_node_animator = rt_game3d_entity_get_anim(node_entity);
    EXPECT_TRUE(wrapped_node_animator != nullptr,
                "Entity3D.attachAnimator accepts raw NodeAnimator3D");
    EXPECT_TRUE(rt_game3d_animator_get_controller(wrapped_node_animator) == nullptr,
                "NodeAnimator-backed Animator3D does not expose a skeletal controller");
    EXPECT_TRUE(rt_game3d_animator_get_node_animator(wrapped_node_animator) == node_animator,
                "Animator3D.nodeAnimator exposes the wrapped NodeAnimator3D");
    EXPECT_TRUE(rt_scene_node3d_get_node_animator(rt_game3d_entity_get_node(node_entity)) ==
                    node_animator,
                "Entity3D.attachAnimator binds raw NodeAnimator3D to the entity node");
    EXPECT_TRUE(rt_game3d_animator_play(wrapped_node_animator, rt_const_cstr("nodeMove")) != 0,
                "Animator3D.play starts a wrapped NodeAnimator3D clip");
    rt_game3d_world_spawn(world, node_entity);
    rt_game3d_world_step_simulation(world, 0.25);
    pos = rt_game3d_entity_position(node_entity);
    EXPECT_NEAR(rt_vec3_x(pos), 1.0, 0.05, "World3D scene sync advances NodeAnimator3D");
    EXPECT_NEAR(rt_game3d_animator_state_time(wrapped_node_animator),
                0.25,
                0.05,
                "Animator3D.stateTime reports wrapped NodeAnimator3D time");

    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_audio_repairs_wrong_class_source_slots() {
    TEST("Sound3D source registry repairs wrong-class private source slots");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Audio Repair Unit"), 32, 24);
    void *audio = rt_game3d_world_get_audio(world);
    auto *layout = static_cast<Game3DAudioTestLayout *>(audio);
    void *wrong = rt_material3d_new_color(0.25, 0.5, 0.75);
    size_t wrong_ref = rt_heap_hdr(wrong)->refcnt;

    layout->sources = static_cast<void **>(std::calloc(2, sizeof(void *)));
    layout->source_capacity = 2;
    layout->source_count = 1;
    layout->sources[0] = wrong;

    EXPECT_EQ_INT(rt_game3d_audio_get_source_count(audio),
                  0,
                  "Sound3D source count compacts wrong-class source slots");
    EXPECT_TRUE(layout->sources[0] == nullptr,
                "Sound3D repair nulls wrong-class private source slots");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_ref,
                "Sound3D repair does not release unrelated wrong-class handles");
    rt_game3d_audio_clear_sources(audio);

    layout->source_count = INT32_MAX;
    layout->source_capacity = 2;
    rt_game3d_world_rebase_origin(world, 1.0, 2.0, 3.0);
    EXPECT_EQ_INT(layout->source_count,
                  INT32_MAX,
                  "audio rebase skips inconsistent source storage without traversing it");

    std::free(layout->sources);
    layout->sources = nullptr;
    layout->source_capacity = 0;
    layout->source_count = 0;
    rt_game3d_world_destroy(world);
    PASS();
}

static bool test_phase6_sound3d_and_effects3d_helpers() {
    TEST("Sound3D helpers and Effects3D presets integrate with World3D");

    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Audio VFX Unit"), 96, 72);
    void *camera = rt_game3d_world_get_camera(world);
    void *audio = rt_game3d_world_get_audio(world);
    void *listener = rt_game3d_audio_get_listener(audio);
    void *effects = rt_game3d_world_get_effects(world);
    EXPECT_TRUE(audio != nullptr, "World3D creates Sound3D");
    EXPECT_TRUE(listener != nullptr, "Sound3D creates a listener");
    EXPECT_TRUE(effects != nullptr, "World3D creates EffectRegistry3D");
    EXPECT_TRUE(rt_game3d_audio_get_listener_follows_camera(audio) != 0,
                "Sound3D listener follows camera by default");

    rt_camera3d_look_at(
        camera, rt_vec3_new(3.0, 2.0, 6.0), rt_vec3_new(3.0, 2.0, 5.0), rt_vec3_new(0.0, 1.0, 0.0));
    rt_game3d_world_step_simulation(world, 0.1);
    void *listener_pos = rt_soundlistener3d_get_position(listener);
    EXPECT_NEAR(rt_vec3_x(listener_pos), 3.0, 0.001, "listener syncs bound camera position");

    rt_game3d_audio_listener_follow_camera(audio, 0);
    rt_camera3d_look_at(
        camera, rt_vec3_new(7.0, 2.0, 6.0), rt_vec3_new(7.0, 2.0, 5.0), rt_vec3_new(0.0, 1.0, 0.0));
    rt_game3d_world_step_simulation(world, 0.1);
    listener_pos = rt_soundlistener3d_get_position(listener);
    EXPECT_NEAR(
        rt_vec3_x(listener_pos), 3.0, 0.001, "listenerFollowCamera(false) freezes listener pose");

    void *manual_pos = rt_vec3_new(9.0, 1.0, 2.0);
    void *manual_forward = rt_vec3_new(0.0, 0.0, -1.0);
    void *manual_up = rt_vec3_new(0.0, 1.0, 0.0);
    rt_game3d_audio_set_listener_pose(audio, manual_pos, manual_forward, manual_up);
    listener_pos = rt_soundlistener3d_get_position(listener);
    EXPECT_NEAR(rt_vec3_x(listener_pos), 9.0, 0.001, "setListenerPose writes listener position");
    EXPECT_TRUE(rt_game3d_audio_get_listener_follows_camera(audio) == 0,
                "setListenerPose disables camera follow");
    rt_game3d_audio_set_attenuation(audio, 2.0, 12.0);
    rt_game3d_audio_set_volume(audio, 70);
    EXPECT_NEAR(
        rt_game3d_audio_get_ref_distance(audio), 2.0, 0.0001, "Sound3D stores ref distance");
    EXPECT_NEAR(
        rt_game3d_audio_get_max_distance(audio), 12.0, 0.0001, "Sound3D stores max distance");
    EXPECT_EQ_INT(rt_game3d_audio_get_volume(audio), 70, "Sound3D clamps/stores volume");

    auto *audio_view = static_cast<Game3DAudioTestLayout *>(audio);
    void *saved_listener = audio_view->listener;
    void *wrong_listener = rt_vec3_new(1.0, 2.0, 3.0);
    audio_view->listener = wrong_listener;
    audio_view->listener_follow_camera = -5;
    audio_view->ref_distance = NAN;
    audio_view->max_distance = 0.5;
    audio_view->volume = 999;
    EXPECT_TRUE(rt_game3d_audio_get_listener(audio) == nullptr,
                "Sound3D listener getter hides wrong-class private listener slots");
    EXPECT_TRUE(rt_game3d_audio_get_listener_follows_camera(audio) == 1,
                "Sound3D camera-follow getter normalizes corrupt private flags");
    EXPECT_NEAR(rt_game3d_audio_get_ref_distance(audio),
                1.0,
                0.0001,
                "Sound3D ref-distance getter defaults corrupt private state");
    EXPECT_NEAR(rt_game3d_audio_get_max_distance(audio),
                1.0,
                0.0001,
                "Sound3D max-distance getter stays at least ref distance");
    EXPECT_EQ_INT(rt_game3d_audio_get_volume(audio),
                  100,
                  "Sound3D volume getter clamps corrupt private state");
    audio_view->listener = saved_listener;
    audio_view->listener_follow_camera = 0;
    audio_view->ref_distance = 2.0;
    audio_view->max_distance = 12.0;
    audio_view->volume = 70;
    if (wrong_listener && rt_obj_release_check0(wrong_listener))
        rt_obj_free(wrong_listener);

    rt_sound3d_listener_state state;
    double pos[3] = {0.0, 0.0, 0.0};
    double fwd[3] = {0.0, 0.0, -1.0};
    double src[3] = {5.0, 0.0, 0.0};
    int64_t spatial_volume = 0;
    int64_t spatial_pan = 0;
    rt_sound3d_listener_state_set(&state, pos, fwd, nullptr);
    rt_sound3d_compute_voice_params(&state, src, 10.0, 100, &spatial_volume, &spatial_pan);
    EXPECT_EQ_INT(spatial_volume, 50, "Sound3D attenuation halves volume at half max distance");
    EXPECT_TRUE(spatial_pan > 90, "Sound3D pans right-side sources to the right");

    if (rt_audio_is_available() && rt_audio_init()) {
        void *clip = rt_synth_tone(440, 80, RT_WAVE_SINE);
        if (clip) {
            audio_view->ref_distance = NAN;
            audio_view->max_distance = 0.5;
            audio_view->volume = 999;
            void *corrupt_one_shot =
                rt_game3d_audio_play_at(audio, clip, rt_vec3_new(2.0, 0.0, 0.0));
            EXPECT_TRUE(corrupt_one_shot != nullptr,
                        "playAt returns a SoundSource3D with corrupt Sound3D state");
            EXPECT_NEAR(rt_soundsource3d_get_ref_distance(corrupt_one_shot),
                        1.0,
                        0.001,
                        "playAt sanitizes corrupt Sound3D ref distance");
            EXPECT_NEAR(rt_soundsource3d_get_max_distance(corrupt_one_shot),
                        1.0,
                        0.001,
                        "playAt sanitizes corrupt Sound3D max distance");
            EXPECT_EQ_INT(rt_soundsource3d_get_volume(corrupt_one_shot),
                          100,
                          "playAt sanitizes corrupt Sound3D volume");
            audio_view->ref_distance = 2.0;
            audio_view->max_distance = 12.0;
            audio_view->volume = 70;

            void *entity = rt_game3d_entity_new();
            rt_game3d_entity_set_position(entity, 1.0, 0.0, 0.0);
            rt_game3d_world_spawn(world, entity);
            void *attached = rt_game3d_audio_play_attached(audio, clip, entity);
            EXPECT_TRUE(attached != nullptr, "playAttached returns an SoundSource3D");
            EXPECT_NEAR(rt_soundsource3d_get_max_distance(attached),
                        12.0,
                        0.001,
                        "playAttached applies Sound3D attenuation");
            rt_game3d_entity_set_position(entity, 4.0, 0.0, 0.0);
            rt_game3d_world_step_simulation(world, 0.1);
            void *source_pos = rt_soundsource3d_get_position(attached);
            EXPECT_NEAR(rt_vec3_x(source_pos), 4.0, 0.001, "attached audio follows entity node");

            void *one_shot = rt_game3d_audio_play_at(audio, clip, rt_vec3_new(2.0, 0.0, 0.0));
            EXPECT_TRUE(one_shot != nullptr, "playAt returns an SoundSource3D");
            (void)rt_game3d_audio_play2d(audio, clip);
            EXPECT_TRUE(rt_game3d_audio_get_source_count(audio) >= 2,
                        "Sound3D tracks sources it creates");
            rt_game3d_audio_clear_sources(audio);
            EXPECT_EQ_INT(
                rt_game3d_audio_get_source_count(audio), 0, "clearSources drops source refs");
        }
    }

    void *origin = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *side = rt_vec3_new(1.0, 0.1, 0.0);
    void *explosion = rt_game3d_effects3d_explosion(world, origin);
    void *sparks = rt_game3d_effects3d_sparks(world, origin, side);
    void *dust = rt_game3d_effects3d_dust(world, origin);
    void *smoke = rt_game3d_effects3d_smoke(world, origin);
    void *decal = rt_game3d_effects3d_impact_decal(world, origin, up);
    EXPECT_TRUE(explosion != nullptr && sparks != nullptr && dust != nullptr && smoke != nullptr,
                "Effects3D particle presets return emitters");
    EXPECT_TRUE(decal != nullptr, "Effects3D.ImpactDecal returns a decal");
    EXPECT_EQ_INT(
        rt_game3d_effects_get_count(effects), 5, "Effects3D presets register five effects");
    EXPECT_EQ_INT(
        rt_game3d_effects_get_particles_count(effects), 4, "Effects3D registers particles");
    EXPECT_EQ_INT(rt_game3d_effects_get_decal_count(effects), 1, "Effects3D registers decals");
    EXPECT_TRUE(rt_particles3d_get_count(explosion) > 0, "Explosion spawns an immediate burst");

    rt_game3d_world_begin_frame(world);
    rt_game3d_world_draw_scene(world);
    rt_game3d_world_draw_effects(world);
    rt_game3d_world_end_scene(world);
    EXPECT_TRUE(rt_game3d_world_capture_final_frame(world) != nullptr,
                "drawEffects renders without failing");

    rt_game3d_effects_update(effects, 3.0);
    EXPECT_EQ_INT(rt_game3d_effects_get_count(effects), 0, "Effects3D presets auto-expire");

    void *manual_particles = rt_particles3d_new(8);
    rt_particles3d_burst(manual_particles, 4);
    rt_game3d_effects_add_particles(effects, manual_particles, 0.1);
    void *manual_decal = rt_decal3d_new(origin, up, 0.25, nullptr);
    rt_decal3d_set_lifetime(manual_decal, 0.1);
    rt_game3d_effects_add_decal(effects, manual_decal);
    EXPECT_EQ_INT(
        rt_game3d_effects_get_count(effects), 2, "EffectRegistry3D accepts manual effects");
    rt_game3d_effects_update(effects, 0.2);
    EXPECT_EQ_INT(
        rt_game3d_effects_get_count(effects), 0, "EffectRegistry3D expires manual effects");

    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *ground = rt_game3d_entity_new();
    rt_game3d_entity_attach_body(ground, rt_game3d_body_def_static_box(1.0, 1.0, 1.0));
    void *ball = rt_game3d_entity_new();
    rt_game3d_entity_attach_body(ball, rt_game3d_body_def_sphere(0.5, 1.0));
    rt_game3d_world_spawn(world, ground);
    rt_game3d_world_spawn(world, ball);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *event = rt_game3d_world_collision_event(world, rt_game3d_collision_enter(), 0);
    EXPECT_TRUE(event != nullptr, "collision event exists for audio/VFX trigger point");
    if (event) {
        void *point = rt_game3d_collision_event_point(event);
        EXPECT_TRUE(rt_game3d_effects3d_dust(world, point) != nullptr,
                    "collision point can spawn a positional effect");
    }

    rt_game3d_world_destroy(world);
    PASS();
}

/// Behavior3D presets drive spawned entities deterministically each simulation
/// step: orbit and sine write position, spin writes rotation, chase closes
/// distance and respects range, lifetime despawns.
static bool test_behavior3d_presets_drive_entities() {
    TEST("Behavior3D presets drive entities through StepSimulation");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Behavior Unit"), 64, 48);
    EXPECT_TRUE(world != NULL, "world created");

    /* Orbit: r=3 about (0,2,0) at 90 deg/s -> after 1s at angle pi/2: (0,2,3). */
    void *orbiter = rt_game3d_entity_new();
    rt_game3d_entity_attach_behavior(
        orbiter, rt_game3d_behavior_add_orbit(rt_game3d_behavior_new(), 0.0, 2.0, 0.0, 3.0, 90.0));
    rt_game3d_world_spawn(world, orbiter);

    /* Sine float: base y=1, amplitude 0.5, speed pi/2 -> after 1s y = 1.5. */
    void *floater = rt_game3d_entity_new();
    rt_game3d_entity_set_position(floater, 4.0, 1.0, 0.0);
    rt_game3d_entity_attach_behavior(
        floater,
        rt_game3d_behavior_add_sine_float(rt_game3d_behavior_new(), 0.5, 1.5707963267948966));
    rt_game3d_world_spawn(world, floater);

    /* Spin: 90 deg/s about +Y -> after 1s quat = (0, sin45, 0, cos45). */
    void *spinner = rt_game3d_entity_new();
    rt_game3d_entity_set_position(spinner, -4.0, 0.0, 0.0);
    rt_game3d_entity_attach_behavior(
        spinner, rt_game3d_behavior_add_spin(rt_game3d_behavior_new(), 0.0, 1.0, 0.0, 90.0));
    rt_game3d_world_spawn(world, spinner);

    /* Chase: from origin toward (0,0,10) at 2 u/s with range 1 -> after 1s z=2. */
    void *target = rt_game3d_entity_new();
    rt_game3d_entity_set_position(target, 0.0, 0.0, 10.0);
    rt_game3d_world_spawn(world, target);
    void *chaser = rt_game3d_entity_new();
    rt_game3d_entity_set_position(chaser, 0.0, 0.0, 0.0);
    rt_game3d_entity_attach_behavior(
        chaser, rt_game3d_behavior_add_chase(rt_game3d_behavior_new(), target, 2.0, 1.0));
    rt_game3d_world_spawn(world, chaser);

    /* FollowPath stores physical distance but Path3D evaluates normalized t.
     * A 2 u/s behavior on this 10-unit line must therefore reach x=2 after 1s,
     * not clamp immediately to the endpoint. */
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_vec3_new(0.0, 0.0, 0.0));
    rt_path3d_add_point(path, rt_vec3_new(10.0, 0.0, 0.0));
    void *follower = rt_game3d_entity_new();
    rt_game3d_entity_attach_behavior(
        follower, rt_game3d_behavior_add_follow_path(rt_game3d_behavior_new(), path, 2.0, 0));
    rt_game3d_world_spawn(world, follower);

    /* Lifetime: despawn after 0.5s. */
    void *ephemeral = rt_game3d_entity_new();
    rt_game3d_entity_attach_behavior(
        ephemeral, rt_game3d_behavior_add_lifetime(rt_game3d_behavior_new(), 0.5));
    rt_game3d_world_spawn(world, ephemeral);
    EXPECT_TRUE(rt_game3d_entity_is_spawned(ephemeral) == 1, "ephemeral entity spawned");

    for (int i = 0; i < 20; i++)
        rt_game3d_world_step_simulation(world, 0.05); /* exactly 1.0s */

    {
        void *pos = rt_game3d_entity_world_position(orbiter);
        EXPECT_NEAR(rt_vec3_x(pos), 0.0, 1e-4, "orbit x after quarter turn");
        EXPECT_NEAR(rt_vec3_y(pos), 2.0, 1e-6, "orbit keeps center height");
        EXPECT_NEAR(rt_vec3_z(pos), 3.0, 1e-4, "orbit z after quarter turn");
    }
    {
        void *pos = rt_game3d_entity_world_position(floater);
        EXPECT_NEAR(rt_vec3_y(pos), 1.5, 1e-4, "sine float peaks at base + amplitude");
        EXPECT_NEAR(rt_vec3_x(pos), 4.0, 1e-6, "sine float leaves x untouched");
    }
    {
        void *node = rt_game3d_entity_get_node(spinner);
        void *rot = rt_scene_node3d_get_rotation(node);
        EXPECT_TRUE(rot != NULL, "spinner rotation readable");
        EXPECT_NEAR(rt_quat_y(rot), 0.70710678, 1e-3, "spin yaw quaternion y at 90 degrees");
        EXPECT_NEAR(rt_quat_w(rot), 0.70710678, 1e-3, "spin yaw quaternion w at 90 degrees");
    }
    {
        void *pos = rt_game3d_entity_world_position(chaser);
        EXPECT_NEAR(rt_vec3_z(pos), 2.0, 1e-4, "chase advances speed*time toward target");
    }
    {
        void *pos = rt_game3d_entity_world_position(follower);
        EXPECT_NEAR(rt_vec3_x(pos), 2.0, 0.02, "follow-path converts distance to normalized t");
        EXPECT_NEAR(rt_vec3_y(pos), 0.0, 1e-6, "follow-path preserves line height");
    }
    EXPECT_TRUE(rt_game3d_entity_is_spawned(ephemeral) == 0, "lifetime behavior despawns");

    /* Chase stops at range: from z=7 it may only advance to range 1 (z=9),
     * even with 1.5s of travel budget (speed 2 would otherwise reach z=10). */
    rt_game3d_entity_set_position(chaser, 0.0, 0.0, 7.0);
    for (int i = 0; i < 30; i++)
        rt_game3d_world_step_simulation(world, 0.05);
    {
        void *pos = rt_game3d_entity_world_position(chaser);
        EXPECT_NEAR(rt_vec3_z(pos), 9.0, 1e-4, "chase stops at the requested range");
    }

    rt_game3d_world_destroy(world);
    PASS();
}

/// Extreme but finite behavior parameters are clamped before arithmetic, and
/// direct callers receive the same hitch-sized delta cap as the world loop.
static bool test_behavior3d_extreme_finite_inputs_remain_finite() {
    TEST("Behavior3D bounds extreme finite input and hot-loop state");
    void *entity = rt_game3d_entity_new();
    void *behavior =
        rt_game3d_behavior_add_spin(rt_game3d_behavior_new(), 1.0e300, -1.0e300, 1.0e300, 1.0e300);
    behavior =
        rt_game3d_behavior_add_orbit(behavior, 1.0e300, -1.0e300, 1.0e300, 1.0e300, -1.0e300);
    behavior = rt_game3d_behavior_add_sine_float(behavior, 1.0e300, 1.0e300);
    rt_game3d_entity_attach_behavior(entity, behavior);

    rt_game3d_behavior_update(behavior, entity, 1.0e300);

    void *position = rt_game3d_entity_world_position(entity);
    void *rotation = rt_scene_node3d_get_rotation(rt_game3d_entity_get_node(entity));
    EXPECT_TRUE(std::isfinite(rt_vec3_x(position)) && std::isfinite(rt_vec3_y(position)) &&
                    std::isfinite(rt_vec3_z(position)),
                "extreme orbit/sine state leaves a finite position");
    EXPECT_TRUE(std::isfinite(rt_quat_x(rotation)) && std::isfinite(rt_quat_y(rotation)) &&
                    std::isfinite(rt_quat_z(rotation)) && std::isfinite(rt_quat_w(rotation)),
                "overflow-safe spin normalization leaves a finite quaternion");
    PASS();
}

/// Sweep regression: a behavior that despawns a DIFFERENT entity mid-sweep
/// compacts the registry with swap-remove. The stamped sweep must neither
/// double-tick the despawner (the old `i--` heuristic re-ran slot i) nor skip
/// the survivor swapped into an already-visited slot. Spin angles measure
/// exactly-once ticking: 90 deg/s x 0.25 s = 22.5 deg per tick.
static bool test_entity_sweep_survives_cross_despawn() {
    TEST("Entity sweep ticks each survivor exactly once across a cross-despawn");
    void *world = rt_game3d_world_new(rt_const_cstr("Game3D Sweep Unit"), 64, 48);

    void *despawner = rt_game3d_entity_new(); /* slot 0: ticks first */
    void *victim = rt_game3d_entity_new();    /* slot 1: despawned by slot 0 */
    void *survivor = rt_game3d_entity_new();  /* slot 2 (last): swapped into slot 1 */
    rt_game3d_world_spawn(world, despawner);
    rt_game3d_world_spawn(world, victim);
    rt_game3d_world_spawn(world, survivor);

    rt_game3d_entity_attach_behavior(
        despawner,
        rt_game3d_behavior_add_despawn_target_internal(
            rt_game3d_behavior_add_spin(rt_game3d_behavior_new(), 0.0, 1.0, 0.0, 90.0), victim));
    rt_game3d_entity_attach_behavior(
        survivor, rt_game3d_behavior_add_spin(rt_game3d_behavior_new(), 0.0, 1.0, 0.0, 90.0));

    rt_game3d_world_step_simulation(world, 0.25);

    EXPECT_TRUE(rt_game3d_entity_is_spawned(victim) == 0, "victim was despawned mid-sweep");
    EXPECT_TRUE(rt_game3d_entity_is_spawned(despawner) == 1, "despawner survives");
    EXPECT_TRUE(rt_game3d_entity_is_spawned(survivor) == 1, "survivor survives");
    {
        /* 22.5 degrees -> quat y = sin(11.25 deg) = 0.19509. A double tick
         * (45 deg -> 0.38268) or a skipped tick (0) both fail. */
        void *node = rt_game3d_entity_get_node(despawner);
        void *rot = rt_scene_node3d_get_rotation(node);
        EXPECT_NEAR(rt_quat_y(rot), 0.19509032, 1e-3, "despawner ticked exactly once");
    }
    {
        void *node = rt_game3d_entity_get_node(survivor);
        void *rot = rt_scene_node3d_get_rotation(node);
        EXPECT_NEAR(rt_quat_y(rot), 0.19509032, 1e-3, "swapped-in survivor ticked exactly once");
    }

    rt_game3d_world_destroy(world);
    PASS();
}

int main() {
    set_software_backend_env();
    bool ok = true;
    ok = test_persistence_snapshot_validator_is_canonical_and_exact() && ok;
    ok = test_persistence_duplicate_load_is_transactional() && ok;
    ok = test_layermasks_and_constants() && ok;
    ok = test_behavior3d_presets_drive_entities() && ok;
    ok = test_behavior3d_extreme_finite_inputs_remain_finite() && ok;
    ok = test_entity_sweep_survives_cross_despawn() && ok;
    ok = test_world_worker_controls() && ok;
    ok = test_input_axes() && ok;
    ok = test_input_update_snapshots_frame_state() && ok;
    ok = test_input_repairs_corrupt_snapshots_and_rejects_undersized_handles() && ok;
    ok = test_game3d_core_rejects_undersized_class_id_spoofs() && ok;
    ok = test_world_entity_registry_and_collision_clear() && ok;
    ok = test_world_body_index_deletion_tombstones_and_duplicate_names() && ok;
    ok = test_world_navmesh_bake_hooks() && ok;
    ok = test_entity_from_node_wraps_imported_subtree() && ok;
    ok = test_entity_private_slots_reject_wrong_class_refs() && ok;
    ok = test_animator_private_controller_rejects_wrong_class_refs() && ok;
    ok = test_animator_private_event_slots_reject_wrong_class_refs() && ok;
    ok = test_entity_child_graph_reparents_and_rejects_cycles() && ok;
    ok = test_entity_child_count_repair_bounds_tree_walks() && ok;
    ok = test_frame_loop_manual_frame_and_final_capture() && ok;
    ok = test_run_fixed_accumulator_and_spiral_guard() && ok;
    ok = test_worker_count_runframes_replay_parity() && ok;
    ok = test_worker_count_parallel_animation_parity() && ok;
    ok = test_world_animation_clamps_corrupt_entity_count() && ok;
    ok = test_world_getters_sanitize_corrupt_private_state() && ok;
    ok = test_world_floating_origin_controls_and_rebase() && ok;
    ok = test_world_floating_origin_rendered_parity_and_flag_off_bytes() && ok;
    ok = test_step_simulation_clamps_invalid_dt() && ok;
    ok = test_free_fly_controller_synthetic_input() && ok;
    ok = test_camera_and_character_controllers_preserve_zero_delta_pauses() && ok;
    ok = test_run_frames_only_preserves_synthetic_holds() && ok;
    ok = test_camera_controller_moves_between_worlds() && ok;
    ok = test_character_controller_syncs_world_position_under_parent() && ok;
    ok = test_orbit_and_follow_controllers() && ok;
    ok = test_controllers_release_destroyed_entity_targets() && ok;
    ok = test_first_person_character_controller_same_frame_motion() && ok;
    ok = test_character_controller_gravity_and_planar_input() && ok;
    ok = test_phase3_material_presets_and_prefabs() && ok;
    ok = test_phase3_world_presets_environment_and_debug() && ok;
    ok = test_phase4_assets3d_model_templates() && ok;
    ok = test_phase4_assets3d_stale_async_publish_revalidates_generation() && ok;
    ok = test_phase4_assets3d_resident_bytes_returns_to_baseline() && ok;
    ok = test_phase4_assets3d_residency_hint_eviction() && ok;
    ok = test_phase4_assets3d_texture_residency_budget() && ok;
    ok = test_assets3d_loads_packaged_gltf_hierarchy() && ok;
    ok = test_phase5_world_stream3d_baseline() && ok;
    ok = test_phase5_world_stream3d_terrain_manifest() && ok;
    ok = test_phase5_world_stream3d_terrain_slots_reject_wrong_class_refs() && ok;
    ok = test_phase5_world_stream3d_heightmap_resample_preserves_edges() && ok;
    ok = test_phase5_world_stream3d_rejects_trailing_heightmap_tokens() && ok;
    ok = test_phase5_world_stream3d_terrain_lod_seams_large_world() && ok;
    ok = test_phase5_world_stream3d_budget_prefers_nearest_entries() && ok;
    ok = test_phase5_world_stream3d_manifest_cells() && ok;
    ok = test_phase5_world_stream3d_cell_binary_sidecar() && ok;
    ok = test_phase5_world_stream3d_measures_lod_residency() && ok;
    ok = test_phase5_world_stream3d_hitch_budgeted_update() && ok;
    ok = test_phase5_world_stream3d_budget_hysteresis_and_cooldown() && ok;
    ok = test_phase5_world_stream3d_zero_radius_update_traps() && ok;
    ok = test_phase12_world_stream3d_inspection_hooks() && ok;
    ok = test_phase4_body_def_attach_body() && ok;
    ok = test_shared_body_attachment_rejected_without_state_leak() && ok;
    ok = test_entity_transform_sanitizes_and_respects_sync_mode() && ok;
    ok = test_phase4_collision_events_wrapped_with_entities() && ok;
    ok = test_phase5_animator3d_events_and_root_motion() && ok;
    ok = test_audio_repairs_wrong_class_source_slots() && ok;
    ok = test_phase6_sound3d_and_effects3d_helpers() && ok;

    std::printf("\nGame3D runtime tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
