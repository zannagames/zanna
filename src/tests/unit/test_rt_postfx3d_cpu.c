//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_postfx3d_cpu.c
// Purpose: Direct offscreen-frame tests for CPU PostFX3D color-buffer semantics.
//
// Key invariants:
//   - Every CPU effect consumes and produces one packed RGBRGB... float buffer.
//   - Temporal history uses the same packed representation as the active frame.
//   - Effects preserve the render target's alpha channel.
//
// Ownership/Lifetime:
//   - Each fixture owns its target, canvas, chain, and optional Pixels objects.
//   - Canvas and PostFX retained references are released in reverse ownership order.
//
// Links: rt_postfx3d.c, docs/internals/graphics3d-runtime-hardening-round-three-2026-08-10.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_object.h"
#include "rt_pixels_internal.h"
#include "rt_postfx3d.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void rt_postfx3d_apply_to_canvas(void *canvas);
extern int64_t rt_parallel_default_workers(void);

static int tests_run = 0;
static int tests_passed = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond))                                                                               \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

typedef struct {
    void *target_obj;
    rt_rendertarget3d *target_wrapper;
    vgfx3d_rendertarget_t *target;
    void *canvas_obj;
    rt_canvas3d *canvas;
} PostFXCPUFixture;

typedef struct {
    void *vptr;
    void *effects;
    int32_t effect_count;
    int32_t effect_capacity;
    int8_t enabled;
    char last_error[160];
    float *taa_history;
    int32_t taa_w;
    int32_t taa_h;
    int8_t taa_valid;
    float cpu_prev_vp[16];
    int8_t cpu_prev_vp_valid;
    float auto_exposure_ev;
    int8_t auto_exposure_valid;
    void *lut_pixels;
    float *cpu_fbuf;
    size_t cpu_fbuf_bytes;
    float *cpu_scratch_primary;
    size_t cpu_scratch_primary_bytes;
    float *cpu_scratch_secondary;
    size_t cpu_scratch_secondary_bytes;
    void *worker_pool;
    int8_t worker_pool_failed;
} PostFXCPUWorkerLayout;

static PostFXCPUFixture postfx_cpu_fixture_new_sized(int32_t width, int32_t height) {
    PostFXCPUFixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.target_obj = rt_rendertarget3d_new(width, height);
    fixture.target_wrapper = (rt_rendertarget3d *)fixture.target_obj;
    fixture.target = fixture.target_wrapper ? fixture.target_wrapper->target : NULL;
    fixture.canvas_obj = fixture.target_obj ? rt_canvas3d_new_offscreen(fixture.target_obj) : NULL;
    fixture.canvas = (rt_canvas3d *)fixture.canvas_obj;
    if (fixture.target)
        (void)vgfx3d_rendertarget_ensure_depth(fixture.target);
    if (fixture.canvas) {
        memset(fixture.canvas->cached_vp, 0, sizeof(fixture.canvas->cached_vp));
        fixture.canvas->cached_vp[0] = 1.0f;
        fixture.canvas->cached_vp[5] = 1.0f;
        fixture.canvas->cached_vp[10] = 1.0f;
        fixture.canvas->cached_vp[15] = 1.0f;
        fixture.canvas->cached_cam_near = 0.1f;
        fixture.canvas->cached_cam_far = 100.0f;
    }
    if (fixture.target && fixture.target->depth_buf)
        for (int32_t i = 0; i < width * height; ++i)
            fixture.target->depth_buf[i] = 0.0f;
    return fixture;
}

static PostFXCPUFixture postfx_cpu_fixture_new(void) {
    return postfx_cpu_fixture_new_sized(2, 1);
}

static void postfx_cpu_fixture_free(PostFXCPUFixture *fixture) {
    if (!fixture)
        return;
    (void)rt_memory_release(fixture->canvas_obj);
    (void)rt_memory_release(fixture->target_obj);
    memset(fixture, 0, sizeof(*fixture));
}

static void postfx_set_two_pixels(vgfx3d_rendertarget_t *target,
                                  uint8_t r0,
                                  uint8_t g0,
                                  uint8_t b0,
                                  uint8_t a0,
                                  uint8_t r1,
                                  uint8_t g1,
                                  uint8_t b1,
                                  uint8_t a1) {
    if (!target || !target->color_buf)
        return;
    target->color_buf[0] = r0;
    target->color_buf[1] = g0;
    target->color_buf[2] = b0;
    target->color_buf[3] = a0;
    target->color_buf[4] = r1;
    target->color_buf[5] = g1;
    target->color_buf[6] = b1;
    target->color_buf[7] = a1;
    target->color_dirty = 0;
}

/* ADR 0247 transfer-identity suite: the 8-bit software scene buffer is
 * SCENE-LINEAR and a real tone curve performs the pipeline's single display
 * encode. A re-introduced input decode (the ZB-20 grey wash: pow(2.2) in
 * front of the curve cancels the encode and runs ACES in display space)
 * fails these anchors loudly — mid-grey byte 46 came out ~33 instead of
 * ~147 under the inverted contract. */

static void test_tonemap_midgrey_anchor(void) {
    PostFXCPUFixture fixture = postfx_cpu_fixture_new();
    void *fx = rt_postfx3d_new();

    EXPECT_TRUE(fixture.canvas && fixture.target && fixture.target->color_buf && fx,
                "Mid-grey anchor fixture initializes");
    /* Uniform linear mid-grey (0.18 = byte 46). ACES(0.18 * 1.10) then the
     * 1/2.2 gamma-out lands at 0.576 -> byte 147. */
    postfx_set_two_pixels(fixture.target, 46, 46, 46, 255, 46, 46, 46, 255);
    rt_postfx3d_add_tonemap(fx, 2, 1.10);
    rt_canvas3d_set_post_fx(fixture.canvas_obj, fx);
    rt_postfx3d_apply_to_canvas(fixture.canvas_obj);

    EXPECT_TRUE(fixture.target->color_buf[0] >= 144 && fixture.target->color_buf[0] <= 150,
                "ACES consumes scene-linear input: byte 46 -> 147 +/- 3 "
                "(a display-space decode crushes it to ~33)");
    EXPECT_TRUE(fixture.target->color_buf[0] == fixture.target->color_buf[1] &&
                    fixture.target->color_buf[1] == fixture.target->color_buf[2],
                "Grey stays neutral through the tone curve");

    (void)rt_memory_release(fx);
    postfx_cpu_fixture_free(&fixture);
}

static void test_tonemap_saturated_patch_anchor(void) {
    PostFXCPUFixture fixture = postfx_cpu_fixture_new();
    void *fx = rt_postfx3d_new();

    EXPECT_TRUE(fixture.canvas && fixture.target && fixture.target->color_buf && fx,
                "Saturated patch fixture initializes");
    /* Linear (60,180,60) through ACES@1.10 + gamma-out -> ~(169,223,169).
     * The inverted contract gave ~(58,208,58): the red/blue channels land
     * >100 bytes low and the hue collapses. */
    postfx_set_two_pixels(fixture.target, 60, 180, 60, 255, 60, 180, 60, 255);
    rt_postfx3d_add_tonemap(fx, 2, 1.10);
    rt_canvas3d_set_post_fx(fixture.canvas_obj, fx);
    rt_postfx3d_apply_to_canvas(fixture.canvas_obj);

    EXPECT_TRUE(fixture.target->color_buf[0] >= 163 && fixture.target->color_buf[0] <= 175,
                "Saturated patch red channel matches the linear-in contract");
    EXPECT_TRUE(fixture.target->color_buf[1] >= 217 && fixture.target->color_buf[1] <= 229,
                "Saturated patch green channel matches the linear-in contract");
    EXPECT_TRUE(fixture.target->color_buf[2] == fixture.target->color_buf[0],
                "Saturated patch stays hue-symmetric (r == b)");

    (void)rt_memory_release(fx);
    postfx_cpu_fixture_free(&fixture);
}

static void test_tonemap_toe_monotone_lift(void) {
    /* Under the correct contract the ACES toe + gamma-out LIFT every dark
     * value (out >= in for bytes 1..128). The display-space decode made the
     * curve crush them instead (byte 40 -> 29). */
    int monotone_ok = 1;
    for (int v = 1; v <= 128; v += 3) {
        PostFXCPUFixture fixture = postfx_cpu_fixture_new();
        void *fx = rt_postfx3d_new();
        if (!fixture.canvas || !fixture.target || !fixture.target->color_buf || !fx) {
            monotone_ok = 0;
            (void)rt_memory_release(fx);
            postfx_cpu_fixture_free(&fixture);
            break;
        }
        postfx_set_two_pixels(fixture.target,
                              (uint8_t)v,
                              (uint8_t)v,
                              (uint8_t)v,
                              255,
                              (uint8_t)v,
                              (uint8_t)v,
                              (uint8_t)v,
                              255);
        rt_postfx3d_add_tonemap(fx, 2, 1.10);
        rt_canvas3d_set_post_fx(fixture.canvas_obj, fx);
        rt_postfx3d_apply_to_canvas(fixture.canvas_obj);
        if (fixture.target->color_buf[0] < v)
            monotone_ok = 0;
        (void)rt_memory_release(fx);
        postfx_cpu_fixture_free(&fixture);
    }
    EXPECT_TRUE(monotone_ok, "ACES toe lifts every dark byte (out >= in for 1..128)");
}

static void test_tonemap_free_chain_is_passthrough(void) {
    PostFXCPUFixture fixture = postfx_cpu_fixture_new();
    void *fx = rt_postfx3d_new();

    EXPECT_TRUE(fixture.canvas && fixture.target && fixture.target->color_buf && fx,
                "Tonemap-free fixture initializes");
    /* An identity color grade carries no tone curve: the chain must be a
     * byte-exact passthrough (no hidden decode/encode without a tonemap). */
    postfx_set_two_pixels(fixture.target, 11, 29, 47, 71, 83, 101, 127, 149);
    rt_postfx3d_add_color_grade(fx, 0.0, 1.0, 1.0);
    rt_canvas3d_set_post_fx(fixture.canvas_obj, fx);
    rt_postfx3d_apply_to_canvas(fixture.canvas_obj);

    {
        /* The float round-trip truncates on write-back, so "passthrough"
         * means within one byte of quantization — a hidden decode/encode
         * shifts values by dozens. */
        static const uint8_t expect[6] = {11, 29, 47, 83, 101, 127};
        static const int idx[6] = {0, 1, 2, 4, 5, 6};
        int passthrough_ok = 1;
        for (int i = 0; i < 6; i++) {
            int got = (int)fixture.target->color_buf[idx[i]];
            if (got < (int)expect[i] - 1 || got > (int)expect[i] + 1)
                passthrough_ok = 0;
        }
        EXPECT_TRUE(passthrough_ok,
                    "Tonemap-free chain is a display-referred passthrough "
                    "(each byte within write-back rounding)");
    }
    EXPECT_TRUE(fixture.target->color_buf[3] == 71 && fixture.target->color_buf[7] == 149,
                "Tonemap-free chain preserves alpha");

    (void)rt_memory_release(fx);
    postfx_cpu_fixture_free(&fixture);
}

static void test_color_lut_uses_packed_rgb_pixels(void) {
    PostFXCPUFixture fixture = postfx_cpu_fixture_new();
    void *fx = rt_postfx3d_new();
    void *lut_obj = rt_postfx3d_make_identity_lut();
    rt_pixels_impl *lut = rt_pixels_checked_impl_or_null(lut_obj);

    EXPECT_TRUE(fixture.canvas && fixture.target && fixture.target->color_buf && fx && lut,
                "Packed LUT fixture initializes");
    if (lut && lut->data) {
        for (size_t i = 0; i < 256u * 16u; i++)
            lut->data[i] = UINT32_C(0xFF0000FF);
        pixels_touch(lut);
    }
    postfx_set_two_pixels(fixture.target, 11, 29, 47, 71, 83, 101, 127, 149);
    rt_postfx3d_add_color_lut(fx, lut_obj, 1.0);
    rt_canvas3d_set_post_fx(fixture.canvas_obj, fx);
    rt_postfx3d_apply_to_canvas(fixture.canvas_obj);

    EXPECT_TRUE(fixture.target->color_buf[0] == 255 && fixture.target->color_buf[1] == 0 &&
                    fixture.target->color_buf[2] == 0 && fixture.target->color_buf[4] == 255 &&
                    fixture.target->color_buf[5] == 0 && fixture.target->color_buf[6] == 0,
                "Color LUT transforms each packed pixel independently");
    EXPECT_TRUE(fixture.target->color_buf[3] == 71 && fixture.target->color_buf[7] == 149,
                "Color LUT preserves per-pixel alpha");

    (void)rt_memory_release(lut_obj);
    (void)rt_memory_release(fx);
    postfx_cpu_fixture_free(&fixture);
}

static void test_sharpen_steepens_soft_edges_without_halos(void) {
    /* Plan 61 / ADR 0271: on a 3x3 target holding a soft horizontal ramp
     * (51, 89, 166 per row), full-amount sharpen darkens the interior pixel
     * toward the edge's dark side but never past the local 5-tap minimum,
     * and border pixels ride the byte-exact tonemap-less passthrough. */
    void *target_obj = rt_rendertarget3d_new(3, 3);
    rt_rendertarget3d *target_wrapper = (rt_rendertarget3d *)target_obj;
    vgfx3d_rendertarget_t *target = target_wrapper ? target_wrapper->target : NULL;
    void *canvas_obj = target_obj ? rt_canvas3d_new_offscreen(target_obj) : NULL;
    void *fx = rt_postfx3d_new();
    static const uint8_t kRamp[3] = {51, 89, 166};

    EXPECT_TRUE(target && target->color_buf && canvas_obj && fx, "Sharpen fixture initializes");
    if (!target || !target->color_buf || !canvas_obj || !fx) {
        (void)rt_memory_release(fx);
        (void)rt_memory_release(canvas_obj);
        (void)rt_memory_release(target_obj);
        return;
    }
    (void)vgfx3d_rendertarget_ensure_depth(target);
    if (target->depth_buf)
        for (int i = 0; i < 9; i++)
            target->depth_buf[i] = 0.0f;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++) {
            uint8_t *px = &target->color_buf[(size_t)(y * 3 + x) * 4u];
            px[0] = kRamp[x];
            px[1] = kRamp[x];
            px[2] = kRamp[x];
            px[3] = 255;
        }

    rt_postfx3d_add_sharpen(fx, 1.0);
    rt_canvas3d_set_post_fx(canvas_obj, fx);
    rt_postfx3d_apply_to_canvas(canvas_obj);

    {
        /* Interior (1,1): avg4 = (89+89+51+166)/4 = 98.75 -> out ~79. */
        const uint8_t *center = &target->color_buf[(size_t)(1 * 3 + 1) * 4u];
        EXPECT_TRUE(center[0] >= 77 && center[0] <= 81 && center[0] < 89,
                    "Sharpen steepens the soft edge's dark side (center ~79)");
        EXPECT_TRUE(center[0] == center[1] && center[1] == center[2],
                    "Sharpen treats the neutral ramp channel-uniformly");
        EXPECT_TRUE(center[3] == 255, "Sharpen preserves alpha");
        EXPECT_TRUE(target->color_buf[0] == 51 && target->color_buf[8] == 166,
                    "Border pixels stay byte-exact (edge-guarded pass)");
        EXPECT_TRUE(center[0] >= 51, "Sharpen never undershoots the local 5-tap minimum");
    }

    (void)rt_memory_release(fx);
    (void)rt_memory_release(canvas_obj);
    (void)rt_memory_release(target_obj);
}

static void test_auto_exposure_samples_packed_luminance(void) {
    PostFXCPUFixture fixture = postfx_cpu_fixture_new();
    void *fx = rt_postfx3d_new();

    EXPECT_TRUE(fixture.canvas && fixture.target && fixture.target->color_buf && fx,
                "Packed auto-exposure fixture initializes");
    postfx_set_two_pixels(fixture.target, 255, 0, 0, 17, 0, 255, 0, 23);
    rt_postfx3d_add_auto_exposure(fx, -4.0, 4.0, 3.0);
    rt_canvas3d_set_post_fx(fixture.canvas_obj, fx);
    rt_postfx3d_apply_to_canvas(fixture.canvas_obj);

    EXPECT_TRUE(fixture.target->color_buf[0] >= 110 && fixture.target->color_buf[0] <= 125 &&
                    fixture.target->color_buf[5] >= 110 && fixture.target->color_buf[5] <= 125,
                "Auto exposure computes luminance from each packed RGB triplet");
    EXPECT_TRUE(fixture.target->color_buf[1] == 0 && fixture.target->color_buf[2] == 0 &&
                    fixture.target->color_buf[4] == 0 && fixture.target->color_buf[6] == 0,
                "Auto exposure does not move color channels between pixels");
    EXPECT_TRUE(fixture.target->color_buf[3] == 17 && fixture.target->color_buf[7] == 23,
                "Auto exposure preserves alpha");

    (void)rt_memory_release(fx);
    postfx_cpu_fixture_free(&fixture);
}

static void test_taa_history_uses_packed_rgb_pixels(void) {
    PostFXCPUFixture fixture = postfx_cpu_fixture_new();
    void *fx = rt_postfx3d_new();

    EXPECT_TRUE(fixture.canvas && fixture.target && fixture.target->color_buf &&
                    fixture.target->depth_buf && fx,
                "Packed TAA fixture initializes");
    rt_postfx3d_add_taa(fx, 0.9);
    rt_canvas3d_set_post_fx(fixture.canvas_obj, fx);

    postfx_set_two_pixels(fixture.target, 255, 0, 0, 31, 0, 0, 255, 37);
    rt_postfx3d_apply_to_canvas(fixture.canvas_obj);
    postfx_set_two_pixels(fixture.target, 0, 255, 0, 41, 255, 255, 0, 43);
    rt_postfx3d_apply_to_canvas(fixture.canvas_obj);

    EXPECT_TRUE(fixture.target->color_buf[0] >= 220 && fixture.target->color_buf[0] <= 235 &&
                    fixture.target->color_buf[1] == 255 && fixture.target->color_buf[2] == 0,
                "TAA reprojects the first packed history pixel without channel aliasing");
    EXPECT_TRUE(fixture.target->color_buf[4] >= 20 && fixture.target->color_buf[4] <= 35 &&
                    fixture.target->color_buf[5] == 255 && fixture.target->color_buf[6] == 0,
                "TAA reprojects the second packed history pixel without cross-pixel aliasing");
    EXPECT_TRUE(fixture.target->color_buf[3] == 41 && fixture.target->color_buf[7] == 43,
                "TAA preserves alpha across temporal frames");

    (void)rt_memory_release(fx);
    postfx_cpu_fixture_free(&fixture);
}

static void test_scene_effects_use_row_band_worker_pool(void) {
    PostFXCPUFixture fixture = postfx_cpu_fixture_new_sized(96, 96);
    void *fx = rt_postfx3d_new();
    PostFXCPUWorkerLayout *layout = (PostFXCPUWorkerLayout *)fx;

    EXPECT_TRUE(fixture.canvas && fixture.target && fixture.target->color_buf &&
                    fixture.target->depth_buf && fx,
                "Banded scene-effect fixture initializes");
    if (!fixture.target || !fixture.target->color_buf || !fixture.target->depth_buf || !fx) {
        (void)rt_memory_release(fx);
        postfx_cpu_fixture_free(&fixture);
        return;
    }
    for (int32_t y = 0; y < 96; ++y) {
        for (int32_t x = 0; x < 96; ++x) {
            size_t index = (size_t)y * 96u + (size_t)x;
            uint8_t *pixel = &fixture.target->color_buf[index * 4u];
            pixel[0] = (uint8_t)(x * 255 / 95);
            pixel[1] = (uint8_t)(y * 255 / 95);
            pixel[2] = (uint8_t)((x + y) * 255 / 190);
            pixel[3] = (uint8_t)(64 + (index % 128u));
            fixture.target->depth_buf[index] = 0.5f;
        }
    }
    rt_postfx3d_add_dof(fx, 10.0, 2.0, 1.0);
    rt_postfx3d_add_motion_blur(fx, 0.5, 6);
    rt_postfx3d_add_ssr(fx, 0.5, 0.8);
    rt_canvas3d_set_post_fx(fixture.canvas_obj, fx);
    rt_postfx3d_apply_to_canvas(fixture.canvas_obj);
    rt_postfx3d_apply_to_canvas(fixture.canvas_obj);

    EXPECT_TRUE(rt_parallel_default_workers() < 2 || layout->worker_pool != NULL,
                "DOF, motion blur, and SSR activate the retained row-band worker pool");
    EXPECT_TRUE(fixture.target->color_buf[3] == 64 &&
                    fixture.target->color_buf[((size_t)95 * 96u + 95u) * 4u + 3u] ==
                        (uint8_t)(64 + (((size_t)95 * 96u + 95u) % 128u)),
                "Banded scene-aware effects preserve alpha at both frame extremes");

    (void)rt_memory_release(fx);
    postfx_cpu_fixture_free(&fixture);
}

int main(void) {
    test_tonemap_midgrey_anchor();
    test_tonemap_saturated_patch_anchor();
    test_tonemap_toe_monotone_lift();
    test_tonemap_free_chain_is_passthrough();
    test_color_lut_uses_packed_rgb_pixels();
    test_auto_exposure_samples_packed_luminance();
    test_taa_history_uses_packed_rgb_pixels();
    test_sharpen_steepens_soft_edges_without_halos();
    test_scene_effects_use_row_band_worker_pool();

    printf("rt_postfx3d CPU tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
