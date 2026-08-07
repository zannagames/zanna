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
    std::printf("Game3D quality digest tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok ? 0 : 1;
}
