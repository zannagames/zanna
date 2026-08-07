//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d_quality_digest.cpp
// Purpose: Software-tier frame-digest contract under SetQuality (graphics3d
//          adoption verification, plan E4): every quality preset renders
//          deterministically on the software backend (same scene, same
//          steps -> byte-identical frames across runs), and re-applying a
//          preset is idempotent, so live tier toggles and headless probes
//          that pin digests under a fixed tier can trust the frame bytes.
// Key invariants:
//   - Digest = FNV-1a over every RGBA texel of CaptureFinalFrame.
//   - Each preset is rendered twice from a freshly built world.
// Ownership/Lifetime:
//   - Worlds/entities are created and destroyed per render pass.
// Links: src/runtime/graphics/3d/rt_game3d.c (rt_game3d_world_set_quality),
//        baseball watch3d quality tiers (adopter).
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt.hpp"
#include "rt_game3d.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_string.h"

extern "C" void *rt_material3d_new_pbr(double r, double g, double b);
extern "C" void rt_material3d_set_texture(void *obj, void *pixels);
extern "C" void rt_material3d_set_alpha_mode(void *obj, int64_t mode);
extern "C" void *rt_mesh3d_new_box(double w, double h, double d);
extern "C" void rt_canvas3d_enable_shadows(void *canvas, int64_t resolution);
extern "C" void *rt_game3d_world_get_canvas(void *world);
extern "C" void rt_canvas3d_set_ambient(void *canvas, double r, double g, double b);

#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" void vm_trap(const char *msg) {
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

namespace {
static int g_tests_passed = 0;
static int g_tests_total = 0;

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

static void set_software_backend_env() {
#if RT_PLATFORM_WINDOWS
    _putenv_s("ZANNA_3D_BACKEND", "software");
#else
    setenv("ZANNA_3D_BACKEND", "software", 1);
#endif
}

/// Render a tiny deterministic scene (ground box + falling sphere) for a
/// few frames under the given quality preset (-1 = never call SetQuality;
/// applies > 1 re-apply the same preset) and return the FNV-1a digest of
/// the captured frame.
static uint64_t render_quality_digest_n(int64_t quality, int applies) {
    set_software_backend_env();
    void *world = rt_game3d_world_new(rt_const_cstr("Quality Digest"), 96, 64);
    rt_game3d_world_set_worker_count(world, 1);
    for (int i = 0; i < applies && quality >= 0; ++i)
        rt_game3d_world_set_quality(world, quality);

    void *ground = rt_game3d_entity_new();
    rt_game3d_entity_set_position(ground, 0.0, -0.5, 0.0);
    rt_game3d_entity_attach_body(ground, rt_game3d_body_def_box(8.0, 0.5, 8.0, 0.0));
    rt_game3d_world_spawn(world, ground);

    void *ball = rt_game3d_entity_new();
    rt_game3d_entity_set_position(ball, 0.3, 3.0, 0.2);
    rt_game3d_entity_attach_body(ball, rt_game3d_body_def_sphere(0.5, 1.0));
    rt_game3d_world_spawn(world, ball);

    rt_game3d_world_run_frames_only(world, 6, 1.0 / 60.0);
    void *pixels = rt_game3d_world_capture_final_frame(world);
    uint64_t digest = 1469598103934665603ull;
    if (pixels) {
        int64_t w = rt_pixels_width(pixels);
        int64_t h = rt_pixels_height(pixels);
        for (int64_t y = 0; y < h; ++y)
            for (int64_t x = 0; x < w; ++x) {
                uint64_t v = (uint64_t)rt_pixels_get_rgba(pixels, x, y);
                digest ^= v;
                digest *= 1099511628211ull;
            }
    }
    rt_game3d_world_destroy(world);
    return digest;
}

static uint64_t render_quality_digest(int64_t quality) {
    return render_quality_digest_n(quality, 1);
}

static bool test_quality_presets_render_deterministically() {
    TEST("SetQuality presets are digest-deterministic on software");
    const int64_t presets[3] = {
        RT_GAME3D_QUALITY_PERFORMANCE, RT_GAME3D_QUALITY_BALANCED, RT_GAME3D_QUALITY_CINEMATIC};
    for (int i = 0; i < 3; ++i) {
        uint64_t first = render_quality_digest(presets[i]);
        uint64_t second = render_quality_digest(presets[i]);
        EXPECT_TRUE(first == second, "same preset must reproduce the same frame digest");
        EXPECT_TRUE(first != 1469598103934665603ull, "capture must produce pixels");
    }
    PASS();
}

/// Render a box (whose material carries a fractional-alpha texture) on a
/// plane with shadows enabled and return the frame's minimum luma — a cast
/// shadow drags it far below the flat-lit plane.
static int64_t render_min_luma_with_alpha_texture(int explicit_opaque) {
    set_software_backend_env();
    void *world = rt_game3d_world_new(rt_const_cstr("Alpha Shadow"), 128, 96);
    rt_game3d_world_set_worker_count(world, 1);
    void *canvas = rt_game3d_world_get_canvas(world);
    rt_game3d_lighting_outdoor(world, NULL);
    rt_canvas3d_set_ambient(canvas, 0.08, 0.08, 0.1);
    rt_canvas3d_enable_shadows(canvas, 1024);

    void *ground = rt_game3d_entity_new();
    rt_game3d_entity_set_mesh_prop(ground, rt_mesh3d_new_box(40.0, 0.2, 40.0));
    rt_game3d_entity_set_material_prop(ground, rt_material3d_new_pbr(0.6, 0.6, 0.55));
    rt_game3d_entity_set_position(ground, 0.0, -0.1, 0.0);
    rt_game3d_world_spawn(world, ground);

    /* 2x2 texture whose alpha channel is fractional: AUTO classification
     * promotes the material to BLEND (excluded from shadows) unless the
     * user explicitly declared it opaque. */
    void *px = rt_pixels_new(2, 2);
    for (int64_t y = 0; y < 2; ++y)
        for (int64_t x = 0; x < 2; ++x)
            rt_pixels_set_rgba(px, x, y, 0xCC4433A0); /* RGBA: alpha 0xA0 */
    void *mat = rt_material3d_new_pbr(0.8, 0.3, 0.2);
    rt_material3d_set_texture(mat, px);
    if (explicit_opaque)
        rt_material3d_set_alpha_mode(mat, 0 /* RT_MATERIAL3D_ALPHA_MODE_OPAQUE */);

    void *box = rt_game3d_entity_new();
    rt_game3d_entity_set_mesh_prop(box, rt_mesh3d_new_box(3.0, 6.0, 3.0));
    rt_game3d_entity_set_material_prop(box, mat);
    rt_game3d_entity_set_position(box, 0.0, 3.0, 0.0);
    rt_game3d_world_spawn(world, box);

    rt_game3d_world_run_frames_only(world, 4, 1.0 / 60.0);
    void *pixels = rt_game3d_world_capture_final_frame(world);
    int64_t min_luma = 256;
    if (pixels) {
        int64_t w = rt_pixels_width(pixels);
        int64_t h = rt_pixels_height(pixels);
        for (int64_t y = h / 2; y < h; ++y)
            for (int64_t x = 0; x < w; ++x) {
                int64_t p = rt_pixels_get_rgba(pixels, x, y);
                int64_t luma = (((p >> 24) & 255) * 77 + ((p >> 16) & 255) * 150 +
                                ((p >> 8) & 255) * 29) >>
                               8;
                if (luma < min_luma)
                    min_luma = luma;
            }
    }
    rt_game3d_world_destroy(world);
    return min_luma;
}

static bool test_explicit_opaque_alpha_texture_casts_shadows() {
    TEST("Explicit SetAlphaMode(Opaque) beats texture-alpha blend promotion");
    int64_t auto_min = render_min_luma_with_alpha_texture(0);
    int64_t explicit_min = render_min_luma_with_alpha_texture(1);
    /* AUTO promotes the textured material to BLEND -> excluded from the
     * shadow pass -> the ground stays flat-lit. The explicit opaque
     * declaration keeps the caster and the ground picks up a dark shadow. */
    EXPECT_TRUE(explicit_min < auto_min - 8,
                "explicit-opaque frame must contain a cast shadow the auto frame lacks");
    PASS();
}

static bool test_quality_preset_is_idempotent() {
    TEST("Re-applying a preset never drifts the digest");
    uint64_t once = render_quality_digest_n(RT_GAME3D_QUALITY_BALANCED, 1);
    uint64_t twice = render_quality_digest_n(RT_GAME3D_QUALITY_BALANCED, 2);
    EXPECT_TRUE(once == twice, "SetQuality must be idempotent (live tier toggles re-apply it)");
    PASS();
}
} // namespace

int main() {
    bool ok = true;
    ok = test_quality_presets_render_deterministically() && ok;
    ok = test_quality_preset_is_idempotent() && ok;
    ok = test_explicit_opaque_alpha_texture_casts_shadows() && ok;
    std::printf("Game3D quality digest tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok ? 0 : 1;
}
