//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_postfx3d_snapshot.c
// Purpose: Snapshot tests for the 3D post-processing pipeline output.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_postfx3d.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int64_t rt_memory_release(void *obj);
extern void vgfx3d_sanitize_postfx_snapshot(const struct vgfx3d_postfx_snapshot *src,
                                            struct vgfx3d_postfx_snapshot *dst);

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

#define EXPECT_NEARF(a, b, eps, msg) EXPECT_TRUE(fabsf((float)(a) - (float)(b)) <= (eps), msg)

static int snapshot_float_fields_are_finite(const vgfx3d_postfx_snapshot_t *snapshot) {
    return snapshot && isfinite(snapshot->bloom_threshold) && isfinite(snapshot->bloom_intensity) &&
           isfinite(snapshot->tonemap_exposure) && isfinite(snapshot->cg_brightness) &&
           isfinite(snapshot->cg_contrast) && isfinite(snapshot->cg_saturation) &&
           isfinite(snapshot->vignette_radius) && isfinite(snapshot->vignette_softness) &&
           isfinite(snapshot->ssao_radius) && isfinite(snapshot->ssao_intensity) &&
           isfinite(snapshot->dof_focus_distance) && isfinite(snapshot->dof_aperture) &&
           isfinite(snapshot->dof_max_blur) && isfinite(snapshot->motion_blur_intensity);
}

typedef struct {
    int32_t type;
    int8_t enabled;

    union {
        struct {
            float threshold;
            float intensity;
            int32_t blur_passes;
        } bloom;

        struct {
            int32_t mode;
            float exposure;
        } tonemap;

        struct {
            float edge_threshold;
            float min_threshold;
        } fxaa;

        struct {
            float brightness;
            float contrast;
            float saturation;
        } color_grade;

        struct {
            float radius;
            float softness;
        } vignette;

        struct {
            float ao_radius;
            float ao_intensity;
            int32_t ao_samples;
        } ssao;

        struct {
            float focus_distance;
            float aperture;
            float max_blur;
        } dof;

        struct {
            float mb_intensity;
            int32_t mb_samples;
        } motion_blur;

        struct {
            float blend;
        } taa;

        struct {
            float intensity;
            float max_roughness;
            int32_t steps;
        } ssr;

        struct {
            float min_ev;
            float max_ev;
            float adapt_speed;
        } auto_exposure;

        struct {
            float blend;
        } color_lut;

        struct {
            float intensity;
            float decay;
            int32_t samples;
        } sun_shafts;
    } p;
} PostFX3DTestEntry;

typedef struct {
    void *vptr;
    PostFX3DTestEntry *effects;
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
} PostFX3DTestLayout;

static void test_snapshot_includes_advanced_effects(void) {
    void *fx = rt_postfx3d_new();
    vgfx3d_postfx_snapshot_t snapshot;

    rt_postfx3d_add_ssao(fx, 0.8, 1.5, 6);
    rt_postfx3d_add_dof(fx, 12.0, 2.5, 4.0);
    rt_postfx3d_add_motion_blur(fx, 0.7, 5);

    EXPECT_TRUE(vgfx3d_postfx_get_snapshot(fx, &snapshot) == 1,
                "Snapshot export succeeds when advanced effects are present");
    EXPECT_TRUE(snapshot.ssao_enabled == 1, "Snapshot includes SSAO enable flag");
    EXPECT_TRUE(snapshot.ssao_radius == 0.8f && snapshot.ssao_intensity == 1.5f &&
                    snapshot.ssao_samples == 6,
                "Snapshot includes SSAO parameters");
    EXPECT_TRUE(snapshot.dof_enabled == 1, "Snapshot includes DOF enable flag");
    EXPECT_TRUE(snapshot.dof_focus_distance == 12.0f && snapshot.dof_aperture == 2.5f &&
                    snapshot.dof_max_blur == 4.0f,
                "Snapshot includes DOF parameters");
    EXPECT_TRUE(snapshot.motion_blur_enabled == 1, "Snapshot includes motion-blur enable flag");
    EXPECT_TRUE(snapshot.motion_blur_intensity == 0.7f && snapshot.motion_blur_samples == 5,
                "Snapshot includes motion-blur parameters");
}

static void test_snapshot_disabled_returns_zero(void) {
    void *fx = rt_postfx3d_new();
    vgfx3d_postfx_snapshot_t snapshot;

    rt_postfx3d_add_ssao(fx, 1.0, 1.0, 4);
    rt_postfx3d_set_enabled(fx, 0);

    EXPECT_TRUE(vgfx3d_postfx_get_snapshot(fx, &snapshot) == 0,
                "Snapshot export returns zero when PostFX is disabled");
}

static void test_snapshot_preserves_documented_tonemap_and_grade_params(void) {
    void *fx = rt_postfx3d_new();
    vgfx3d_postfx_snapshot_t snapshot;

    rt_postfx3d_add_tonemap(fx, 0, 1.0);
    rt_postfx3d_add_color_grade(fx, 0.02, 1.08, 1.04);

    EXPECT_TRUE(vgfx3d_postfx_get_snapshot(fx, &snapshot) == 1,
                "Snapshot export succeeds for tonemap and color-grade settings");
    EXPECT_TRUE(snapshot.tonemap_mode == 0 && snapshot.tonemap_exposure == 1.0f,
                "Snapshot preserves tonemap mode 0 as disabled");
    EXPECT_TRUE(snapshot.color_grade_enabled == 1, "Snapshot includes color-grade enable flag");
    EXPECT_TRUE(snapshot.cg_brightness == 0.02f && snapshot.cg_contrast == 1.08f &&
                    snapshot.cg_saturation == 1.04f,
                "Snapshot preserves additive color-grade parameters");
}

static void test_effect_chain_grows_past_legacy_cap(void) {
    void *fx = rt_postfx3d_new();

    for (int i = 0; i < 12; i++)
        rt_postfx3d_add_fxaa(fx);

    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) == 12,
                "PostFX3D preserves effects appended past the old 8-entry cap");
}

static void test_chain_export_preserves_effect_order_and_duplicates(void) {
    void *fx = rt_postfx3d_new();
    vgfx3d_postfx_chain_t chain = {0};

    rt_postfx3d_add_bloom(fx, 0.8, 1.5, 2);
    rt_postfx3d_add_vignette(fx, 0.6, 0.25);
    rt_postfx3d_add_bloom(fx, 0.9, 0.4, 1);

    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1,
                "Chain export succeeds for ordered GPU postfx data");
    EXPECT_TRUE(chain.enabled == 1 && chain.effect_count == 3,
                "Chain export preserves every enabled effect entry");
    EXPECT_TRUE(chain.effects[0].type == VGFX3D_POSTFX_EFFECT_BLOOM &&
                    chain.effects[0].snapshot.bloom_threshold == 0.8f &&
                    chain.effects[0].snapshot.bloom_intensity == 1.5f &&
                    chain.effects[0].snapshot.bloom_passes == 2,
                "Chain export preserves the first bloom pass");
    EXPECT_TRUE(chain.effects[1].type == VGFX3D_POSTFX_EFFECT_VIGNETTE &&
                    chain.effects[1].snapshot.vignette_enabled == 1 &&
                    chain.effects[1].snapshot.vignette_radius == 0.6f,
                "Chain export preserves middle-pass ordering");
    EXPECT_TRUE(chain.effects[2].type == VGFX3D_POSTFX_EFFECT_BLOOM &&
                    chain.effects[2].snapshot.bloom_threshold == 0.9f &&
                    chain.effects[2].snapshot.bloom_intensity == 0.4f &&
                    chain.effects[2].snapshot.bloom_passes == 1,
                "Chain export preserves duplicate effect types as separate ordered passes");

    vgfx3d_postfx_chain_free(&chain);
}

static void test_effect_parameters_are_sanitized_for_backend_chain(void) {
    void *fx = rt_postfx3d_new();
    vgfx3d_postfx_chain_t chain = {0};

    rt_postfx3d_add_bloom(fx, NAN, INFINITY, 999);
    rt_postfx3d_add_tonemap(fx, 999, NAN);
    rt_postfx3d_add_color_grade(fx, INFINITY, NAN, -INFINITY);
    rt_postfx3d_add_vignette(fx, NAN, -1.0);
    rt_postfx3d_add_ssao(fx, NAN, INFINITY, 999);
    rt_postfx3d_add_dof(fx, NAN, INFINITY, 9999.0);
    rt_postfx3d_add_motion_blur(fx, INFINITY, 999);

    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1,
                "Chain export succeeds for sanitized parameters");
    EXPECT_TRUE(chain.effect_count == 7, "Chain export preserves sanitized effects");

    EXPECT_TRUE(chain.effects[0].type == VGFX3D_POSTFX_EFFECT_BLOOM,
                "Sanitized chain stores bloom first");
    EXPECT_NEARF(chain.effects[0].snapshot.bloom_threshold,
                 0.8f,
                 0.0001f,
                 "Bloom threshold falls back from NaN");
    EXPECT_NEARF(chain.effects[0].snapshot.bloom_intensity,
                 1.0f,
                 0.0001f,
                 "Bloom intensity falls back from infinity");
    EXPECT_TRUE(chain.effects[0].snapshot.bloom_passes == 32,
                "Bloom blur passes clamp to the quality cap");

    EXPECT_TRUE(chain.effects[1].type == VGFX3D_POSTFX_EFFECT_TONEMAP,
                "Sanitized chain stores tonemap second");
    EXPECT_TRUE(chain.effects[1].snapshot.tonemap_mode == 2,
                "Tonemap mode clamps to the highest supported mode");
    EXPECT_NEARF(chain.effects[1].snapshot.tonemap_exposure,
                 1.0f,
                 0.0001f,
                 "Tonemap exposure falls back from NaN");

    EXPECT_TRUE(chain.effects[2].type == VGFX3D_POSTFX_EFFECT_COLOR_GRADE,
                "Sanitized chain stores color grade third");
    EXPECT_NEARF(chain.effects[2].snapshot.cg_brightness,
                 0.0f,
                 0.0001f,
                 "Color grade brightness falls back from infinity");
    EXPECT_NEARF(chain.effects[2].snapshot.cg_contrast,
                 1.0f,
                 0.0001f,
                 "Color grade contrast falls back from NaN");
    EXPECT_NEARF(chain.effects[2].snapshot.cg_saturation,
                 1.0f,
                 0.0001f,
                 "Color grade saturation falls back from infinity");

    EXPECT_TRUE(chain.effects[3].type == VGFX3D_POSTFX_EFFECT_VIGNETTE,
                "Sanitized chain stores vignette fourth");
    EXPECT_NEARF(chain.effects[3].snapshot.vignette_radius,
                 0.7f,
                 0.0001f,
                 "Vignette radius falls back from NaN");
    EXPECT_NEARF(chain.effects[3].snapshot.vignette_softness,
                 0.001f,
                 0.0001f,
                 "Vignette softness clamps to a non-zero floor");

    EXPECT_TRUE(chain.effects[4].type == VGFX3D_POSTFX_EFFECT_SSAO,
                "Sanitized chain stores SSAO fifth");
    EXPECT_NEARF(
        chain.effects[4].snapshot.ssao_radius, 0.5f, 0.0001f, "SSAO radius falls back from NaN");
    EXPECT_NEARF(chain.effects[4].snapshot.ssao_intensity,
                 1.0f,
                 0.0001f,
                 "SSAO intensity falls back from infinity");
    EXPECT_TRUE(chain.effects[4].snapshot.ssao_samples == 128,
                "SSAO samples clamp to the quality cap");

    EXPECT_TRUE(chain.effects[5].type == VGFX3D_POSTFX_EFFECT_DOF,
                "Sanitized chain stores DOF sixth");
    EXPECT_NEARF(chain.effects[5].snapshot.dof_focus_distance,
                 10.0f,
                 0.0001f,
                 "DOF focus distance falls back from NaN");
    EXPECT_NEARF(chain.effects[5].snapshot.dof_aperture,
                 0.0f,
                 0.0001f,
                 "DOF aperture falls back from infinity");
    EXPECT_NEARF(chain.effects[5].snapshot.dof_max_blur,
                 128.0f,
                 0.0001f,
                 "DOF max blur clamps to the quality cap");

    EXPECT_TRUE(chain.effects[6].type == VGFX3D_POSTFX_EFFECT_MOTION_BLUR,
                "Sanitized chain stores motion blur seventh");
    EXPECT_NEARF(chain.effects[6].snapshot.motion_blur_intensity,
                 0.0f,
                 0.0001f,
                 "Motion-blur intensity falls back from infinity");
    EXPECT_TRUE(chain.effects[6].snapshot.motion_blur_samples == 64,
                "Motion-blur samples clamp to the quality cap");

    vgfx3d_postfx_chain_free(&chain);
}

static void test_extreme_finite_effect_parameters_are_capped(void) {
    void *fx = rt_postfx3d_new();
    vgfx3d_postfx_chain_t chain = {0};
    const double huge = 1.0e300;

    rt_postfx3d_add_bloom(fx, huge, huge, INT64_MAX);
    rt_postfx3d_add_tonemap(fx, INT64_MAX, huge);
    rt_postfx3d_add_color_grade(fx, huge, huge, huge);
    rt_postfx3d_add_vignette(fx, huge, huge);
    rt_postfx3d_add_ssao(fx, huge, huge, INT64_MAX);
    rt_postfx3d_add_dof(fx, huge, huge, huge);
    rt_postfx3d_add_motion_blur(fx, huge, INT64_MAX);

    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1,
                "Chain export succeeds for huge finite parameters");
    EXPECT_TRUE(chain.effect_count == 7, "Huge finite parameters preserve the authored chain");
    for (int32_t i = 0; i < chain.effect_count; i++)
        EXPECT_TRUE(snapshot_float_fields_are_finite(&chain.effects[i].snapshot),
                    "Huge finite parameters export only finite snapshot floats");

    EXPECT_NEARF(chain.effects[0].snapshot.bloom_threshold,
                 64.0f,
                 0.0001f,
                 "Huge bloom threshold clamps to the runtime cap");
    EXPECT_NEARF(chain.effects[0].snapshot.bloom_intensity,
                 64.0f,
                 0.0001f,
                 "Huge bloom intensity clamps to the runtime cap");
    EXPECT_TRUE(chain.effects[0].snapshot.bloom_passes == 32,
                "Huge bloom blur passes clamp to the quality cap");

    EXPECT_NEARF(chain.effects[1].snapshot.tonemap_exposure,
                 64.0f,
                 0.0001f,
                 "Huge tonemap exposure clamps to the runtime cap");
    EXPECT_NEARF(chain.effects[2].snapshot.cg_brightness,
                 1.0f,
                 0.0001f,
                 "Huge color-grade brightness clamps to +1");
    EXPECT_NEARF(chain.effects[2].snapshot.cg_contrast,
                 4.0f,
                 0.0001f,
                 "Huge color-grade contrast clamps to +4");
    EXPECT_NEARF(chain.effects[2].snapshot.cg_saturation,
                 4.0f,
                 0.0001f,
                 "Huge color-grade saturation clamps to +4");
    EXPECT_NEARF(
        chain.effects[3].snapshot.vignette_radius, 1.0f, 0.0001f, "Huge vignette radius clamps");
    EXPECT_NEARF(chain.effects[3].snapshot.vignette_softness,
                 1.0f,
                 0.0001f,
                 "Huge vignette softness clamps");
    EXPECT_NEARF(chain.effects[4].snapshot.ssao_radius,
                 1000000.0f,
                 0.5f,
                 "Huge SSAO radius clamps to the scene cap");
    EXPECT_NEARF(chain.effects[4].snapshot.ssao_intensity,
                 64.0f,
                 0.0001f,
                 "Huge SSAO intensity clamps to the runtime cap");
    EXPECT_NEARF(chain.effects[5].snapshot.dof_focus_distance,
                 1000000.0f,
                 0.5f,
                 "Huge DOF focus distance clamps to the scene cap");
    EXPECT_NEARF(chain.effects[5].snapshot.dof_aperture,
                 64.0f,
                 0.0001f,
                 "Huge DOF aperture clamps to the runtime cap");
    EXPECT_NEARF(chain.effects[5].snapshot.dof_max_blur,
                 128.0f,
                 0.0001f,
                 "Huge DOF blur clamps to the quality cap");
    EXPECT_NEARF(chain.effects[6].snapshot.motion_blur_intensity,
                 1.0f,
                 0.0001f,
                 "Huge motion blur intensity clamps to one");

    vgfx3d_postfx_chain_free(&chain);
}

static void test_chain_copy_rejects_inconsistent_metadata(void) {
    vgfx3d_postfx_effect_desc_t one_effect = {0};
    vgfx3d_postfx_chain_t src = {0};
    vgfx3d_postfx_chain_t dst = {0};

    src.enabled = 1;
    src.effect_count = 2;
    src.effect_capacity = 1;
    src.effects = &one_effect;
    dst.enabled = 1;

    EXPECT_TRUE(vgfx3d_postfx_chain_copy(&dst, &src) == 0,
                "Chain copy rejects sources whose count exceeds capacity");
    EXPECT_TRUE(dst.enabled == 0 && dst.effect_count == 0,
                "Rejected chain copy resets the destination to disabled");
}

static void test_private_effect_count_corruption_is_bounded(void) {
    void *fx = rt_postfx3d_new();
    PostFX3DTestLayout *layout = (PostFX3DTestLayout *)fx;
    vgfx3d_postfx_chain_t chain = {0};
    vgfx3d_postfx_snapshot_t snapshot;

    rt_postfx3d_add_bloom(fx, 0.8, 1.0, 2);
    EXPECT_TRUE(layout->effect_capacity > 0, "PostFX3D corruption fixture allocated effects");

    layout->effect_count = layout->effect_capacity + 100;
    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) == 1,
                "PostFX3D effect count getter restores the authoritative initialized count");
    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1,
                "PostFX3D chain export ignores over-capacity private counts");
    EXPECT_TRUE(chain.effect_count == 1 && chain.effects[0].type == VGFX3D_POSTFX_EFFECT_BLOOM,
                "PostFX3D chain export keeps only valid enabled effects");
    EXPECT_TRUE(vgfx3d_postfx_get_snapshot(fx, &snapshot) == 1 && snapshot.bloom_enabled == 1,
                "PostFX3D snapshot export ignores over-capacity private counts");
    vgfx3d_postfx_chain_free(&chain);

    layout->effect_count = -5;
    rt_postfx3d_add_fxaa(fx);
    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) == 2,
                "PostFX3D append restores the initialized prefix before appending");
    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1,
                "PostFX3D chain export still succeeds after count repair");
    EXPECT_TRUE(chain.effect_count == 2 && chain.effects[0].type == VGFX3D_POSTFX_EFFECT_BLOOM &&
                    chain.effects[1].type == VGFX3D_POSTFX_EFFECT_FXAA,
                "PostFX3D append preserves the initialized prefix after mirror repair");
    vgfx3d_postfx_chain_free(&chain);
}

static void test_private_effect_storage_mirrors_are_not_ownership_authority(void) {
    void *fx = rt_postfx3d_new();
    PostFX3DTestLayout *layout = (PostFX3DTestLayout *)fx;
    PostFX3DTestEntry borrowed_entry = {0};
    PostFX3DTestEntry *owned_effects;
    int32_t owned_capacity;

    rt_postfx3d_add_bloom(fx, 0.8, 1.0, 2);
    owned_effects = layout->effects;
    owned_capacity = layout->effect_capacity;
    layout->effects = &borrowed_entry;
    layout->effect_count = INT32_MAX;
    layout->effect_capacity = INT32_MAX;

    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) == 1,
                "Corrupt PostFX mirrors restore the authoritative effect count");
    EXPECT_TRUE(layout->effects == owned_effects && layout->effect_capacity == owned_capacity,
                "Corrupt PostFX mirrors restore the owned effect allocation");

    layout->effects = NULL;
    layout->effect_count = 0;
    layout->effect_capacity = 0;
    rt_postfx3d_add_fxaa(fx);
    EXPECT_TRUE(layout->effects == owned_effects && rt_postfx3d_get_effect_count(fx) == 2,
                "A cleared effect mirror cannot orphan authoritative storage");
}

static void test_retained_effect_entries_are_repaired_before_export(void) {
    void *fx = rt_postfx3d_new();
    PostFX3DTestLayout *layout = (PostFX3DTestLayout *)fx;
    vgfx3d_postfx_chain_t chain = {0};
    void *lut = rt_postfx3d_make_identity_lut();

    rt_postfx3d_add_bloom(fx, 0.8, 1.0, 2);
    rt_postfx3d_add_tonemap(fx, 1, 1.0);
    rt_postfx3d_add_fxaa(fx);
    rt_postfx3d_add_color_grade(fx, 0.0, 1.0, 1.0);
    rt_postfx3d_add_vignette(fx, 0.7, 0.3);
    rt_postfx3d_add_ssao(fx, 0.5, 1.0, 8);
    rt_postfx3d_add_dof(fx, 10.0, 1.0, 8.0);
    rt_postfx3d_add_motion_blur(fx, 0.5, 6);
    rt_postfx3d_add_taa(fx, 0.9);
    rt_postfx3d_add_ssr(fx, 0.5, 0.4);
    rt_postfx3d_add_auto_exposure(fx, -4.0, 4.0, 3.0);
    rt_postfx3d_add_color_lut(fx, lut, 1.0);
    rt_postfx3d_add_sun_shafts(fx, 0.6, 0.92, 24);

    layout->effects[0].enabled = -9;
    layout->effects[0].p.bloom.threshold = NAN;
    layout->effects[0].p.bloom.intensity = INFINITY;
    layout->effects[0].p.bloom.blur_passes = INT32_MAX;
    layout->effects[1].p.tonemap.mode = INT32_MIN;
    layout->effects[1].p.tonemap.exposure = NAN;
    layout->effects[2].p.fxaa.edge_threshold = NAN;
    layout->effects[2].p.fxaa.min_threshold = INFINITY;
    layout->effects[3].p.color_grade.brightness = NAN;
    layout->effects[3].p.color_grade.contrast = INFINITY;
    layout->effects[3].p.color_grade.saturation = -INFINITY;
    layout->effects[4].p.vignette.radius = NAN;
    layout->effects[4].p.vignette.softness = -INFINITY;
    layout->effects[5].p.ssao.ao_radius = NAN;
    layout->effects[5].p.ssao.ao_intensity = INFINITY;
    layout->effects[5].p.ssao.ao_samples = INT32_MAX;
    layout->effects[6].p.dof.focus_distance = NAN;
    layout->effects[6].p.dof.aperture = INFINITY;
    layout->effects[6].p.dof.max_blur = -INFINITY;
    layout->effects[7].p.motion_blur.mb_intensity = NAN;
    layout->effects[7].p.motion_blur.mb_samples = INT32_MIN;
    layout->effects[8].p.taa.blend = INFINITY;
    layout->effects[9].p.ssr.intensity = NAN;
    layout->effects[9].p.ssr.max_roughness = INFINITY;
    layout->effects[9].p.ssr.steps = INT32_MAX;
    layout->effects[10].p.auto_exposure.min_ev = NAN;
    layout->effects[10].p.auto_exposure.max_ev = INFINITY;
    layout->effects[10].p.auto_exposure.adapt_speed = -INFINITY;
    layout->effects[11].p.color_lut.blend = NAN;
    layout->effects[12].p.sun_shafts.intensity = INFINITY;
    layout->effects[12].p.sun_shafts.decay = NAN;
    layout->effects[12].p.sun_shafts.samples = INT32_MAX;

    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1 && chain.effect_count == 13,
                "All thirteen public effect kinds survive repaired chain export");
    EXPECT_TRUE(layout->effects[0].enabled == 1 && layout->effects[0].p.bloom.threshold == 0.8f &&
                    layout->effects[0].p.bloom.intensity == 1.0f &&
                    layout->effects[0].p.bloom.blur_passes == 32,
                "Bloom retained state is canonical and bounded");
    EXPECT_TRUE(layout->effects[1].p.tonemap.mode == 0 &&
                    layout->effects[1].p.tonemap.exposure == 1.0f,
                "Tonemap retained state is canonical and bounded");
    EXPECT_TRUE(layout->effects[2].p.fxaa.edge_threshold == 0.166f &&
                    layout->effects[2].p.fxaa.min_threshold == 0.0833f,
                "FXAA retained state is repaired to documented defaults");
    EXPECT_TRUE(layout->effects[10].p.auto_exposure.min_ev == -4.0f &&
                    layout->effects[10].p.auto_exposure.max_ev == 4.0f &&
                    layout->effects[10].p.auto_exposure.adapt_speed == 3.0f,
                "Auto-exposure retained state is finite and ordered");
    EXPECT_TRUE(layout->effects[11].p.color_lut.blend == 1.0f,
                "Color-LUT retained blend is repaired");
    EXPECT_TRUE(layout->effects[12].p.sun_shafts.intensity == 0.6f &&
                    layout->effects[12].p.sun_shafts.decay == 0.92f &&
                    layout->effects[12].p.sun_shafts.samples == 48,
                "Sun-shaft retained state is finite and bounded");
    EXPECT_TRUE(chain.effects[10].type == VGFX3D_POSTFX_EFFECT_AUTO_EXPOSURE &&
                    chain.effects[11].type == VGFX3D_POSTFX_EFFECT_COLOR_LUT &&
                    chain.effects[12].type == VGFX3D_POSTFX_EFFECT_SUN_SHAFTS,
                "Backend chain export no longer drops the three newest effect kinds");

    vgfx3d_postfx_chain_free(&chain);
    (void)rt_memory_release(lut);
}

static void test_color_lut_snapshot_carries_payload(void) {
    void *fx = rt_postfx3d_new();
    vgfx3d_postfx_chain_t chain = {0};
    vgfx3d_postfx_snapshot_t malformed;
    vgfx3d_postfx_snapshot_t safe;
    void *lut = rt_postfx3d_make_identity_lut();
    const vgfx3d_postfx_snapshot_t *snap = NULL;

    /* Plan 61 (plan-59 B10): the ordered chain snapshot must hand backends the
     * retained LUT strip so the GPU pass can grade instead of no-opping. */
    rt_postfx3d_add_tonemap(fx, 2, 1.0);
    rt_postfx3d_add_color_lut(fx, lut, 0.75);
    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1 && chain.effect_count == 2 &&
                    chain.effects[1].type == VGFX3D_POSTFX_EFFECT_COLOR_LUT,
                "Color-LUT entry exports through the ordered chain");
    snap = &chain.effects[1].snapshot;
    EXPECT_TRUE(snap->color_lut_enabled == 1, "Color-LUT snapshot payload is enabled");
    EXPECT_NEARF(snap->color_lut_blend, 0.75f, 1e-6f, "Color-LUT snapshot carries the blend");
    EXPECT_TRUE(snap->color_lut_texels != NULL && snap->color_lut_width == 256 &&
                    snap->color_lut_height == 16,
                "Color-LUT snapshot points at the 256x16 strip");
    EXPECT_TRUE(snap->color_lut_texels != NULL && snap->color_lut_texels[0] == 0x000000FFu &&
                    snap->color_lut_texels[(size_t)15 * 256u + 255u] == 0xFFFFFFFFu,
                "Color-LUT snapshot texels read the identity strip in 0xRRGGBBAA packing");

    /* The shared backend sanitizer must disable any malformed payload. */
    memset(&malformed, 0, sizeof(malformed));
    malformed.enabled = 1;
    malformed.color_lut_enabled = 1;
    malformed.color_lut_blend = 0.5f;
    malformed.color_lut_texels = snap->color_lut_texels;
    malformed.color_lut_width = 64; /* wrong layout */
    malformed.color_lut_height = 16;
    vgfx3d_sanitize_postfx_snapshot(&malformed, &safe);
    EXPECT_TRUE(safe.color_lut_enabled == 0 && safe.color_lut_texels == NULL,
                "Sanitizer disables a Color-LUT payload with the wrong strip layout");
    vgfx3d_sanitize_postfx_snapshot(snap, &safe);
    EXPECT_TRUE(safe.color_lut_enabled == 1 && safe.color_lut_texels == snap->color_lut_texels &&
                    safe.color_lut_width == 256 && safe.color_lut_height == 16,
                "Sanitizer passes a well-formed Color-LUT payload through");

    vgfx3d_postfx_chain_free(&chain);
    (void)rt_memory_release(fx);
    (void)rt_memory_release(lut);
}

static void test_color_lut_is_a_replaceable_singleton(void) {
    void *fx = rt_postfx3d_new();
    void *first = rt_postfx3d_make_identity_lut();
    void *second = rt_postfx3d_make_identity_lut();
    vgfx3d_postfx_chain_t chain = {0};
    const uint32_t *first_texels = NULL;

    rt_postfx3d_add_color_lut(fx, first, 0.25);
    if (vgfx3d_postfx_get_chain(fx, &chain) == 1)
        first_texels = chain.effects[0].snapshot.color_lut_texels;
    vgfx3d_postfx_chain_reset(&chain);
    rt_postfx3d_add_color_lut(fx, second, 0.75);

    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) == 1,
                "A later ColorLUT replaces the singleton entry instead of aliasing two passes");
    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1 && chain.effect_count == 1 &&
                    first_texels && chain.effects[0].snapshot.color_lut_texels &&
                    chain.effects[0].snapshot.color_lut_texels != first_texels,
                "The singleton ColorLUT snapshot carries the replacement Pixels source");
    EXPECT_NEARF(chain.effects[0].snapshot.color_lut_blend,
                 0.75f,
                 1e-6f,
                 "The replacement ColorLUT updates its blend in place");

    vgfx3d_postfx_chain_free(&chain);
    (void)rt_memory_release(first);
    (void)rt_memory_release(second);
    (void)rt_memory_release(fx);
}

static void test_invalid_retained_effect_kind_is_compacted(void) {
    void *fx = rt_postfx3d_new();
    PostFX3DTestLayout *layout = (PostFX3DTestLayout *)fx;
    vgfx3d_postfx_chain_t chain = {0};

    rt_postfx3d_add_bloom(fx, 0.8, 1.0, 2);
    rt_postfx3d_add_vignette(fx, 0.7, 0.3);
    rt_postfx3d_add_fxaa(fx);
    layout->effects[1].type = INT32_MAX;

    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) == 2,
                "Unknown retained effect kinds are removed from the initialized prefix");
    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1 && chain.effect_count == 2 &&
                    chain.effects[0].type == VGFX3D_POSTFX_EFFECT_BLOOM &&
                    chain.effects[1].type == VGFX3D_POSTFX_EFFECT_FXAA,
                "Invalid-kind compaction preserves stable order");
    vgfx3d_postfx_chain_free(&chain);
}

static void test_effect_chain_has_a_bounded_policy_limit(void) {
    void *fx = rt_postfx3d_new();
    rt_string error;

    for (int32_t i = 0; i < VGFX3D_POSTFX_MAX_EFFECTS + 17; i++)
        rt_postfx3d_add_fxaa(fx);
    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) == VGFX3D_POSTFX_MAX_EFFECTS,
                "PostFX chain growth stops at the documented policy limit");
    error = rt_postfx3d_get_last_error(fx);
    EXPECT_TRUE(rt_str_len(error) > 0,
                "PostFX reports a recoverable diagnostic when an add exceeds the policy limit");
    rt_string_unref(error);
    (void)rt_memory_release(fx);
}

static void test_backend_chain_ownership_metadata_rejects_borrowed_storage(void) {
    vgfx3d_postfx_effect_desc_t borrowed = {0};
    vgfx3d_postfx_chain_t chain = {0};

    chain.enabled = 1;
    chain.effect_count = 1;
    chain.effect_capacity = 1;
    chain.effects = &borrowed;
    vgfx3d_postfx_chain_reset(&chain);
    EXPECT_TRUE(chain.effects == NULL && chain.effect_capacity == 0 && chain.effect_count == 0,
                "Backend chain reset detaches storage that it cannot prove it owns");

    chain.enabled = 1;
    chain.effect_count = 1;
    chain.effect_capacity = 1;
    chain.effects = &borrowed;
    vgfx3d_postfx_chain_free(&chain);
    EXPECT_TRUE(chain.effects == NULL && chain.effect_capacity == 0,
                "Backend chain free never releases borrowed descriptor storage");
}

static void test_backend_chain_copy_sanitizes_and_bounds_descriptors(void) {
    vgfx3d_postfx_effect_desc_t effect = {0};
    vgfx3d_postfx_chain_t src = {0};
    vgfx3d_postfx_chain_t dst = {0};

    effect.type = VGFX3D_POSTFX_EFFECT_BLOOM;
    effect.snapshot.enabled = -7;
    effect.snapshot.bloom_enabled = -3;
    effect.snapshot.bloom_threshold = NAN;
    effect.snapshot.bloom_intensity = INFINITY;
    effect.snapshot.bloom_passes = INT32_MAX;
    src.enabled = -1;
    src.effect_count = 1;
    src.effect_capacity = 1;
    src.effects = &effect;
    EXPECT_TRUE(vgfx3d_postfx_chain_copy(&dst, &src) == 1,
                "Backend chain copy accepts a structurally valid borrowed source");
    EXPECT_TRUE(dst.enabled == 1 && dst.effects[0].snapshot.enabled == 1 &&
                    dst.effects[0].snapshot.bloom_enabled == 1 &&
                    dst.effects[0].snapshot.bloom_threshold == 0.8f &&
                    dst.effects[0].snapshot.bloom_intensity == 1.0f &&
                    dst.effects[0].snapshot.bloom_passes == 32,
                "Backend chain copy canonicalizes all shader-facing values");
    vgfx3d_postfx_chain_free(&dst);

    src.effect_count = VGFX3D_POSTFX_MAX_EFFECTS + 1;
    src.effect_capacity = src.effect_count;
    EXPECT_TRUE(vgfx3d_postfx_chain_copy(&dst, &src) == 0,
                "Backend chain copy rejects counts above the policy limit");
}

static void test_diagnostic_and_temporal_mirrors_repair_fail_closed(void) {
    void *fx = rt_postfx3d_new();
    PostFX3DTestLayout *layout = (PostFX3DTestLayout *)fx;
    float borrowed_taa[3] = {0};
    float borrowed_fbuf[3] = {0};
    float borrowed_primary[3] = {0};
    float borrowed_secondary[3] = {0};

    memset(layout->last_error, 'x', sizeof(layout->last_error));
    rt_string error = rt_postfx3d_get_last_error(fx);
    EXPECT_TRUE(rt_str_len(error) == 159,
                "PostFX diagnostics are terminated before constructing a runtime string");
    rt_string_unref(error);

    layout->taa_history = borrowed_taa;
    layout->taa_w = INT32_MAX;
    layout->taa_h = INT32_MAX;
    layout->taa_valid = -1;
    layout->cpu_prev_vp_valid = -1;
    layout->auto_exposure_ev = NAN;
    layout->auto_exposure_valid = -1;
    layout->lut_pixels = fx;
    layout->cpu_fbuf = borrowed_fbuf;
    layout->cpu_fbuf_bytes = SIZE_MAX;
    layout->cpu_scratch_primary = borrowed_primary;
    layout->cpu_scratch_primary_bytes = SIZE_MAX;
    layout->cpu_scratch_secondary = borrowed_secondary;
    layout->cpu_scratch_secondary_bytes = SIZE_MAX;
    layout->worker_pool = fx;
    layout->worker_pool_failed = -1;

    EXPECT_TRUE(rt_postfx3d_get_effect_count(fx) == 0,
                "State repair tolerates corrupt temporal and allocation mirrors");
    EXPECT_TRUE(layout->taa_history == NULL && layout->cpu_fbuf == NULL &&
                    layout->cpu_scratch_primary == NULL && layout->cpu_scratch_secondary == NULL &&
                    layout->lut_pixels == NULL && layout->worker_pool == NULL,
                "Unowned PostFX mirrors are detached without dereference");
    EXPECT_TRUE(rt_memory_release(fx) == 0,
                "PostFX finalization ignores detached borrowed mirrors");
}

static void test_set_enabled_normalizes_boolean_state(void) {
    void *fx = rt_postfx3d_new();

    rt_postfx3d_set_enabled(fx, -7);
    EXPECT_TRUE(rt_postfx3d_get_enabled(fx) == 1, "Negative enabled values normalize to true");
    rt_postfx3d_set_enabled(fx, 0);
    EXPECT_TRUE(rt_postfx3d_get_enabled(fx) == 0, "Zero enabled values normalize to false");
}

/* Plan 05: TAA chain export, blend clamping, and the explicit-tonemap marker. */
static void test_taa_and_explicit_tonemap_export(void) {
    void *fx = rt_postfx3d_new();
    vgfx3d_postfx_chain_t chain = {0};
    vgfx3d_postfx_snapshot_t snapshot;

    rt_postfx3d_add_bloom(fx, 0.8, 1.0, 4);
    rt_postfx3d_add_taa(fx, 0.9);
    rt_postfx3d_add_tonemap(fx, 0, 1.25);

    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1 && chain.effect_count == 3,
                "TAA chain export keeps all three effects");
    EXPECT_TRUE(chain.effects[1].type == VGFX3D_POSTFX_EFFECT_TAA &&
                    chain.effects[1].snapshot.taa_enabled == 1,
                "TAA entry exports the appended enum value with taa_enabled set");
    EXPECT_NEARF(chain.effects[1].snapshot.taa_blend,
                 0.9f,
                 0.0001f,
                 "TAA blend passes through inside the valid range");
    EXPECT_TRUE(chain.effects[0].snapshot.tonemap_explicit == 0,
                "Non-tonemap entries do not carry the explicit-tonemap marker");
    EXPECT_TRUE(chain.effects[2].snapshot.tonemap_explicit == 1 &&
                    chain.effects[2].snapshot.tonemap_mode == 0,
                "Explicit mode-0 tonemap entries carry the explicit-tonemap marker");
    EXPECT_TRUE(vgfx3d_postfx_requires_gpu_scene_buffers(fx) == 1,
                "TAA requires GPU scene buffers like SSAO/DOF/motion blur");
    EXPECT_TRUE(vgfx3d_postfx_get_snapshot(fx, &snapshot) == 1 && snapshot.taa_enabled == 1,
                "Legacy flat snapshot reports the TAA effect");
    vgfx3d_postfx_chain_free(&chain);

    fx = rt_postfx3d_new();
    rt_postfx3d_add_taa(fx, 5.0);
    rt_postfx3d_add_taa(fx, 0.1);
    rt_postfx3d_add_taa(fx, NAN);
    EXPECT_TRUE(vgfx3d_postfx_get_chain(fx, &chain) == 1 && chain.effect_count == 3,
                "TAA clamp fixture exports three entries");
    EXPECT_NEARF(
        chain.effects[0].snapshot.taa_blend, 0.98f, 0.0001f, "TAA blend clamps above to 0.98");
    EXPECT_NEARF(
        chain.effects[1].snapshot.taa_blend, 0.5f, 0.0001f, "TAA blend clamps below to 0.5");
    EXPECT_NEARF(chain.effects[2].snapshot.taa_blend,
                 0.9f,
                 0.0001f,
                 "Non-finite TAA blend falls back to the 0.9 default");
    vgfx3d_postfx_chain_free(&chain);
}

int main(void) {
    test_snapshot_includes_advanced_effects();
    test_snapshot_disabled_returns_zero();
    test_snapshot_preserves_documented_tonemap_and_grade_params();
    test_effect_chain_grows_past_legacy_cap();
    test_chain_export_preserves_effect_order_and_duplicates();
    test_effect_parameters_are_sanitized_for_backend_chain();
    test_extreme_finite_effect_parameters_are_capped();
    test_chain_copy_rejects_inconsistent_metadata();
    test_private_effect_count_corruption_is_bounded();
    test_private_effect_storage_mirrors_are_not_ownership_authority();
    test_retained_effect_entries_are_repaired_before_export();
    test_color_lut_snapshot_carries_payload();
    test_color_lut_is_a_replaceable_singleton();
    test_invalid_retained_effect_kind_is_compacted();
    test_effect_chain_has_a_bounded_policy_limit();
    test_backend_chain_ownership_metadata_rejects_borrowed_storage();
    test_backend_chain_copy_sanitizes_and_bounds_descriptors();
    test_diagnostic_and_temporal_mirrors_repair_fail_closed();
    test_set_enabled_normalizes_boolean_state();
    test_taa_and_explicit_tonemap_export();

    printf("rt_postfx3d snapshot tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
