//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_postfx3d.h
// Purpose: PostFX3D — ordered full-screen color and scene-aware effects for
//   software framebuffers and backend-owned GPU chain snapshots.
//
// Key invariants:
//   - Effects are applied in chain order (first added = first applied).
//   - Authored and exported chains are bounded at VGFX3D_POSTFX_MAX_EFFECTS.
//   - Software effects share a packed RGBRGB... float framebuffer.
//   - SetPostFX on Canvas3D enables automatic application in Flip().
//
// Ownership/Lifetime:
//   - PostFX3D owns its dynamically grown effect array and releases it at finalization.
//   - Exported backend chains deep-copy snapshots so no runtime object pointer escapes a frame.
//
// Links: plans/3d/18-post-processing.md, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares PostFX3D effect-chain construction, Canvas3D integration, and backend export.
/// @details PostFX3D stores effects in authored order, supports quality-profile construction,
///   exposes scene-aware CPU and GPU effects, and deep-copies validated descriptors into
///   backend-owned snapshots.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create an empty PostFX chain.
/// @return A new GC-managed, enabled PostFX3D object with no effect entries.
void *rt_postfx3d_new(void);

/// @brief Append a bloom pass (threshold = brightness cutoff, intensity = mix amount, blur_passes =
/// quality).
/// @param obj PostFX3D chain receiving the effect.
/// @param threshold Non-negative bright-extract luminance cutoff.
/// @param intensity Non-negative bloom composite strength.
/// @param blur_passes Requested progressive blur-chain depth.
void rt_postfx3d_add_bloom(void *obj, double threshold, double intensity, int64_t blur_passes);

/// @brief Append a tonemap pass (mode: 0=off, 1=Reinhard, 2=ACES) with exposure stops.
/// @param obj PostFX3D chain receiving the effect.
/// @param mode Tone-map algorithm selector.
/// @param exposure Linear exposure multiplier applied before mapping.
void rt_postfx3d_add_tonemap(void *obj, int64_t mode, double exposure);

/// @brief Append an FXAA antialiasing pass.
/// @param obj PostFX3D chain receiving the effect.
void rt_postfx3d_add_fxaa(void *obj);

/// @brief Append a color-grading pass.
/// @details `brightness` is a signed additive offset centered on 0.0. `contrast` and `saturation`
/// are multipliers centered on 1.0.
/// @param obj PostFX3D chain receiving the effect.
/// @param brightness Signed channel offset.
/// @param contrast Scale around middle gray.
/// @param saturation Chroma scale relative to luminance.
void rt_postfx3d_add_color_grade(void *obj, double brightness, double contrast, double saturation);

/// @brief Append a vignette pass (radius = circle of full brightness, softness = falloff width).
/// @param obj PostFX3D chain receiving the effect.
/// @param radius Normalized radius unaffected by darkening.
/// @param softness Width of the radial falloff.
void rt_postfx3d_add_vignette(void *obj, double radius, double softness);

/// @brief Enable or disable the entire chain (without removing effects).
/// @param obj PostFX3D chain to update.
/// @param enabled Non-zero to apply the chain, or zero to bypass it.
void rt_postfx3d_set_enabled(void *obj, int8_t enabled);

/// @brief True if the chain is enabled.
/// @param obj Candidate PostFX3D chain.
/// @return 1 for a valid enabled chain, or 0 otherwise.
int8_t rt_postfx3d_get_enabled(void *obj);

/// @brief Remove all effects, release any retained LUT, and reset temporal state.
/// @param obj PostFX3D chain to clear without changing its enabled state or warm allocations.
void rt_postfx3d_clear(void *obj);

/// @brief Number of effects currently in the chain.
/// @param obj Candidate PostFX3D chain.
/// @return The number of safely iterable effect entries, or zero for an invalid chain.
int64_t rt_postfx3d_get_effect_count(void *obj);

/// @brief Kind discriminator of the effect at @p index in application order.
/// @param obj Candidate PostFX3D chain.
/// @param index Zero-based position in the chain.
/// @return The `vgfx3d_postfx_effect_kind_t` value (also exposed through the
///   `Zanna.Graphics3D.PostFXEffectKind` constants), or -1 for an invalid
///   chain or out-of-range index.
int64_t rt_postfx3d_get_effect_kind(void *obj, int64_t index);

/// @brief Remove the effect at @p index, preserving the order of the rest.
/// @param obj Candidate PostFX3D chain.
/// @param index Zero-based position in the chain.
/// @return 1 when an entry was removed, or 0 for an invalid chain or index.
int8_t rt_postfx3d_remove_effect_at(void *obj, int64_t index);

/* Zanna.Graphics3D.PostFXEffectKind constants — the values returned by
 * rt_postfx3d_get_effect_kind (mirror vgfx3d_postfx_effect_kind_t). */
/// @brief PostFXEffectKind.Bloom constant.
int64_t rt_postfx3d_effect_kind_bloom(void);
/// @brief PostFXEffectKind.Tonemap constant.
int64_t rt_postfx3d_effect_kind_tonemap(void);
/// @brief PostFXEffectKind.Fxaa constant.
int64_t rt_postfx3d_effect_kind_fxaa(void);
/// @brief PostFXEffectKind.ColorGrade constant.
int64_t rt_postfx3d_effect_kind_color_grade(void);
/// @brief PostFXEffectKind.Vignette constant.
int64_t rt_postfx3d_effect_kind_vignette(void);
/// @brief PostFXEffectKind.Ssao constant.
int64_t rt_postfx3d_effect_kind_ssao(void);
/// @brief PostFXEffectKind.Dof constant.
int64_t rt_postfx3d_effect_kind_dof(void);
/// @brief PostFXEffectKind.MotionBlur constant.
int64_t rt_postfx3d_effect_kind_motion_blur(void);
/// @brief PostFXEffectKind.Taa constant.
int64_t rt_postfx3d_effect_kind_taa(void);
/// @brief PostFXEffectKind.Ssr constant.
int64_t rt_postfx3d_effect_kind_ssr(void);
/// @brief PostFXEffectKind.AutoExposure constant.
int64_t rt_postfx3d_effect_kind_auto_exposure(void);
/// @brief PostFXEffectKind.ColorLut constant.
int64_t rt_postfx3d_effect_kind_color_lut(void);
/// @brief PostFXEffectKind.SunShafts constant.
int64_t rt_postfx3d_effect_kind_sun_shafts(void);
/// @brief PostFXEffectKind.Sharpen constant.
int64_t rt_postfx3d_effect_kind_sharpen(void);

/// @brief Bind a PostFX chain to a Canvas3D for automatic application during Flip to the active
/// output.
/// @param canvas Canvas3D receiving the retained chain reference.
/// @param postfx PostFX3D chain to attach, or `NULL` to detach the current chain.
void rt_canvas3d_set_post_fx(void *canvas, void *postfx);

/// @brief Performance quality profile identifier.
#define RT_GRAPHICS3D_QUALITY_PERFORMANCE 0
/// @brief Balanced quality profile identifier.
#define RT_GRAPHICS3D_QUALITY_BALANCED 1
/// @brief Cinematic quality profile identifier.
#define RT_GRAPHICS3D_QUALITY_CINEMATIC 2

/// @brief Build a backend-safe PostFX profile for a canvas and quality level.
/// @param canvas Canvas3D used for capability selection and quality-state tracking.
/// @param quality Requested quality identifier; out-of-range values are clamped.
/// @return A new configured PostFX3D chain, or `NULL` for an invalid canvas or allocation failure.
void *rt_postfx3d_new_quality(void *canvas, int64_t quality);

/// @brief Apply a backend-safe quality profile to a Canvas3D.
/// @param canvas Canvas3D receiving the generated profile.
/// @param quality Requested quality identifier; out-of-range values are clamped.
void rt_canvas3d_set_quality(void *canvas, int64_t quality);

/// @brief Last requested quality profile.
/// @param canvas Canvas3D whose requested quality is queried.
/// @return The normalized requested quality, or performance for an invalid canvas.
int64_t rt_canvas3d_get_quality_requested(void *canvas);

/// @brief Active quality profile after capability fallback.
/// @param canvas Canvas3D whose active quality is queried.
/// @return The normalized active quality, or performance for an invalid canvas.
int64_t rt_canvas3d_get_quality_active(void *canvas);

/// @brief True when the last quality application degraded for backend safety.
/// @param canvas Canvas3D whose fallback state is queried.
/// @return 1 when the active profile was degraded, or 0 otherwise.
int8_t rt_canvas3d_get_quality_fallback(void *canvas);

/// @brief Human-readable reason for the last quality fallback, or empty string.
/// @param canvas Canvas3D whose fallback reason is queried.
/// @return A new runtime string containing the reason, or an empty string when none exists.
rt_string rt_canvas3d_get_quality_fallback_reason(void *canvas);

/* Phase F additions */
/// @brief Append an SSAO (screen-space ambient occlusion) pass with sample radius and intensity.
/// @param obj PostFX3D chain receiving the effect.
/// @param radius Non-negative world-space sample radius.
/// @param intensity Non-negative occlusion strength.
/// @param samples Requested sample count.
void rt_postfx3d_add_ssao(void *obj, double radius, double intensity, int64_t samples);

/// @brief Append a depth-of-field pass (focus distance in world units, aperture controls bokeh
/// size).
/// @param obj PostFX3D chain receiving the effect.
/// @param focus_distance Camera-space distance of the focus plane.
/// @param aperture Non-negative circle-of-confusion scale.
/// @param max_blur Maximum blur amount.
void rt_postfx3d_add_dof(void *obj, double focus_distance, double aperture, double max_blur);

/// @brief Live focus-distance mutation for an existing DOF effect (focus pulls);
///        returns 0 (recoverable) when the chain has no DOF effect.
/// @param obj PostFX3D chain containing the DOF entry.
/// @param distance New non-negative focus distance.
/// @return 1 when an enabled DOF entry was updated, or 0 when none exists.
int8_t rt_postfx3d_set_dof_focus(void *obj, double distance);

/// @brief Append a motion-blur pass (intensity 0..1, sample count for blur quality).
/// @param obj PostFX3D chain receiving the effect.
/// @param intensity Motion-vector length scale.
/// @param samples Requested gather sample count.
void rt_postfx3d_add_motion_blur(void *obj, double intensity, int64_t samples);

/// @brief Append a TAA (temporal antialiasing) resolve pass. `blend` is the history weight
/// (0.5..0.98; higher = more temporal smoothing). GPU backends use jittered motion/depth
/// reprojection; the CPU path provides deterministic unjittered reprojection.
/// @param obj PostFX3D chain receiving the effect.
/// @param blend History contribution clamped to the supported temporal range.
void rt_postfx3d_add_taa(void *obj, double blend);

/// @brief Append a screen-space reflections pass (Plan 10). `intensity` scales the
/// reflection contribution (0..1); `max_roughness` is the roughness cutoff above which
/// surfaces fall back to the environment map alone. GPU backends apply it to materials
/// flagged `SsrEnabled`; the CPU path performs a lower-step deterministic depth march.
/// @param obj PostFX3D chain receiving the effect.
/// @param intensity Reflection-hit blend weight.
/// @param max_roughness Maximum material roughness eligible for backend SSR.
void rt_postfx3d_add_ssr(void *obj, double intensity, double max_roughness);

/// @brief Return the last recoverable PostFX configuration error, or an empty string.
/// @param obj Candidate PostFX3D chain.
/// @return A new runtime string containing the stored diagnostic.
rt_string rt_postfx3d_get_last_error(void *obj);

/* Backend-facing PostFX snapshot (MTL-11): compact effect params for GPU backends.
 * Exported from rt_postfx3d.c — backends should NOT inspect the private rt_postfx3d struct. */
/// Maximum authored or backend-snapshot entries in one post-processing chain.
#define VGFX3D_POSTFX_MAX_EFFECTS 4096

/// @brief Compact, backend-readable snapshot of a PostFX chain's enabled effects and
///   their parameters — GPU backends consume this instead of the private rt_postfx3d struct.
typedef struct vgfx3d_postfx_snapshot {
    int8_t enabled;
    int8_t bloom_enabled;
    float bloom_threshold;
    float bloom_intensity;
    int32_t bloom_passes;
    int8_t tonemap_mode; /* 0=off, 1=reinhard, 2=aces */
    float tonemap_exposure;
    int8_t fxaa_enabled;
    int8_t color_grade_enabled;
    float cg_brightness, cg_contrast, cg_saturation;
    int8_t vignette_enabled;
    float vignette_radius, vignette_softness;
    int8_t ssao_enabled;
    float ssao_radius, ssao_intensity;
    int32_t ssao_samples;
    int8_t dof_enabled;
    float dof_focus_distance, dof_aperture, dof_max_blur;
    int8_t motion_blur_enabled;
    float motion_blur_intensity;
    int32_t motion_blur_samples;
    /* Appended fields (ABI-stable extension: existing backends memcpy the whole struct). */
    int8_t taa_enabled;
    float taa_blend;
    /* 1 when this snapshot came from an explicit tonemap chain entry (any mode). Lets
     * backends apply the mode-0 gamma-out transform on linear-HDR scene targets without
     * gamma-lifting passes whose snapshot merely defaults to tonemap_mode == 0. */
    int8_t tonemap_explicit;
    /* Plan 10: screen-space reflections (appended; ABI-stable extension). */
    int8_t ssr_enabled;
    float ssr_intensity;
    float ssr_max_roughness;
    int32_t ssr_steps;
    /* Plan 61 (closes plan-59 B10): 3D LUT grade payload (appended; ABI-stable
     * extension). `color_lut_texels` points at the chain-retained 256x16 strip
     * (16 tiles of 16x16: x = red, y = green, tile = blue), packed 0xRRGGBBAA
     * per texel exactly as `rt_pixels` stores it; it stays valid while the
     * chain retains the LUT Pixels (released on Clear/destroy/replacement).
     * Backends must treat (pointer, revision) as a texture cache key —
     * re-upload when either changes — and never free the pointer. */
    int8_t color_lut_enabled;
    float color_lut_blend;
    const uint32_t *color_lut_texels;
    int32_t color_lut_width;
    int32_t color_lut_height;
    uint64_t color_lut_revision;
    /* Plan 61 / ADR 0271: clamped unsharp-mask sharpen (appended; ABI-stable
     * extension). */
    int8_t sharpen_enabled;
    float sharpen_amount;
} vgfx3d_postfx_snapshot_t;

/// @brief Discriminator identifying which post-processing effect a chain entry describes.
typedef enum {
    VGFX3D_POSTFX_EFFECT_BLOOM = 0,
    VGFX3D_POSTFX_EFFECT_TONEMAP,
    VGFX3D_POSTFX_EFFECT_FXAA,
    VGFX3D_POSTFX_EFFECT_COLOR_GRADE,
    VGFX3D_POSTFX_EFFECT_VIGNETTE,
    VGFX3D_POSTFX_EFFECT_SSAO,
    VGFX3D_POSTFX_EFFECT_DOF,
    VGFX3D_POSTFX_EFFECT_MOTION_BLUR,
    VGFX3D_POSTFX_EFFECT_TAA,           /* appended: backend switches key on the raw type value */
    VGFX3D_POSTFX_EFFECT_SSR,           /* Plan 10 */
    VGFX3D_POSTFX_EFFECT_AUTO_EXPOSURE, /* Track E doc 07: eye adaptation */
    VGFX3D_POSTFX_EFFECT_COLOR_LUT,     /* Track E doc 07: 3D LUT grading */
    VGFX3D_POSTFX_EFFECT_SUN_SHAFTS,    /* Track E doc 07: screen-space god rays */
    VGFX3D_POSTFX_EFFECT_SHARPEN,       /* Plan 61 / ADR 0271: clamped unsharp mask */
} vgfx3d_postfx_effect_kind_t;

/// @brief One ordered chain entry: an effect kind plus its parameter snapshot.
typedef struct {
    int32_t type; /* vgfx3d_postfx_effect_kind_t */
    vgfx3d_postfx_snapshot_t snapshot;
} vgfx3d_postfx_effect_desc_t;

/// @brief Backend-facing ordered effect chain: an enabled flag and a growable array of
///   effect descriptors in application order.
typedef struct vgfx3d_postfx_chain {
    int8_t enabled;
    int32_t effect_count;
    int32_t effect_capacity;
    vgfx3d_postfx_effect_desc_t *effects;
    /* Appended authoritative allocation metadata. The public fields above are traversal
     * mirrors; only a tuple whose address-bound cookie validates may be resized or freed.
     * By-value copies intentionally become borrowed views because their cookie is bound to
     * the original chain address. */
    vgfx3d_postfx_effect_desc_t *owned_effects;
    int32_t owned_effect_capacity;
    uint64_t effect_storage_cookie;
} vgfx3d_postfx_chain_t;

/// @brief Fill a flat snapshot from a PostFX3D object; returns 0 if it is NULL or disabled.
/// @param postfx Candidate gameplay-facing PostFX3D chain.
/// @param[out] out Legacy flat snapshot, always cleared before export.
/// @return 1 when at least one enabled supported effect was exported, or 0 otherwise.
int vgfx3d_postfx_get_snapshot(void *postfx, vgfx3d_postfx_snapshot_t *out);

/// @brief Export the ordered PostFX chain for GPU backends, reusing @p out's storage when possible.
/// @param postfx Candidate gameplay-facing PostFX3D chain.
/// @param[out] out Backend chain whose descriptor allocation is reused or grown.
/// @return 1 when at least one enabled supported effect was exported, or 0 after resetting
///   @p out.
int vgfx3d_postfx_get_chain(void *postfx, vgfx3d_postfx_chain_t *out);

/// @brief Append eye-adaptation auto-exposure (minEv, maxEv, adaptSpeed).
/// @param obj PostFX3D chain receiving the effect.
/// @param min_ev Minimum exposure value in stops.
/// @param max_ev Maximum exposure value in stops.
/// @param adapt_speed Positive temporal adaptation rate.
void rt_postfx3d_add_auto_exposure(void *obj, double min_ev, double max_ev, double adapt_speed);

/// @brief Append a 3D LUT color grade from a 256x16 strip Pixels (retained).
/// @param obj PostFX3D chain receiving the effect and retaining the LUT.
/// @param lut_pixels Pixels object containing the 256-by-16 LUT strip.
/// @param blend LUT contribution clamped to the unit interval.
void rt_postfx3d_add_color_lut(void *obj, void *lut_pixels, double blend);

/// @brief Build the identity 256x16 LUT strip Pixels.
/// @return A new Pixels object containing an identity RGB lookup table.
void *rt_postfx3d_make_identity_lut(void);

/// @brief Append screen-space sun shafts with bounded intensity, decay, and sample count.
/// @param obj PostFX3D chain receiving the effect.
/// @param intensity Positive additive shaft strength.
/// @param decay Per-sample attenuation in the open unit interval.
/// @param samples Requested radial sample count, clamped to at most forty-eight.
void rt_postfx3d_add_sun_shafts(void *obj, double intensity, double decay, int64_t samples);

/// @brief Append a clamped unsharp-mask sharpen pass (Plan 61 / ADR 0271).
/// @param obj PostFX3D chain receiving the effect.
/// @param amount Edge gain clamped to `[0, 1]` (0 is identity).
void rt_postfx3d_add_sharpen(void *obj, double amount);

/// @brief Report whether a chain contains effects requiring scene depth or motion inputs.
/// @param postfx Candidate gameplay-facing PostFX3D chain.
/// @return 1 when an enabled SSAO, DOF, motion-blur, TAA, or SSR entry requires auxiliary scene
///   buffers, or 0 otherwise.
int vgfx3d_postfx_requires_gpu_scene_buffers(void *postfx);

/// @brief Deep-copy one exported PostFX chain into another (grows @p dst as needed).
/// @param dst Destination chain whose descriptor allocation is reused or grown.
/// @param src Source chain to copy, or `NULL` to reset @p dst.
/// @return 1 when an enabled non-empty chain was copied, or 0 when @p dst was reset.
int vgfx3d_postfx_chain_copy(vgfx3d_postfx_chain_t *dst, const vgfx3d_postfx_chain_t *src);

/// @brief Reset an exported PostFX chain to empty while keeping its allocation for reuse.
/// @param chain Backend-facing chain to reset; ignored when `NULL`.
void vgfx3d_postfx_chain_reset(vgfx3d_postfx_chain_t *chain);

/// @brief Release any storage owned by an exported PostFX chain.
/// @param chain Backend-facing chain to free and reset; ignored when `NULL`.
void vgfx3d_postfx_chain_free(vgfx3d_postfx_chain_t *chain);

#ifdef __cplusplus
}
#endif
