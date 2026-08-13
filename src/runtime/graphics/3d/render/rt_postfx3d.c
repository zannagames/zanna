//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_postfx3d.c
// Purpose: PostFX3D — ordered full-screen color and scene-aware effects for
//   software framebuffers and backend-owned GPU chain snapshots.
//
// Key invariants:
//   - CPU effects share one packed RGBRGB... float representation.
//   - Bloom and scene-aware effects reuse chain-owned scratch allocations.
//   - Effects chain in order: first added = first applied.
//   - CPU temporary buffers are retained on the PostFX3D object and reused.
//   - CPU scene-aware effects consume bounded depth and camera snapshots.
//   - Tonemap mode 0 is identity (passthrough); modes 1/2 are Reinhard/ACES.
//
// Ownership/Lifetime:
//   - PostFX3D is GC-managed; finalizer frees the heap effect array.
//   - Backend chain snapshots own their own effect-descriptor storage and
//     are reset / freed via vgfx3d_postfx_chain_reset / _free.
//
// Links: rt_postfx3d.h, plans/3d/18-post-processing.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements configurable CPU and backend-facing PostFX3D effect chains.
/// @details The runtime stores ordered effect descriptors, applies color-only and scene-aware
///   effects to software or HDR framebuffers, manages reusable scratch/history allocations, and
///   exports validated snapshots for graphics backends.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_postfx3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_platform.h"
#include "rt_threads.h"
#include "vgfx.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_utils.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_pixels_internal.h"
#include "rt_trap.h"

#define POSTFX3D_FLOAT_ABS_MAX 3.40282346638528859812e38
#define POSTFX3D_PARAM_MAX 64.0f
#define POSTFX3D_RADIUS_MAX 1000000.0f
#define POSTFX3D_FOCUS_MAX 1000000.0f
#define POSTFX3D_GAMMA_LUT_SIZE 1024u
#define POSTFX3D_EV_MIN (-64.0f)
#define POSTFX3D_EV_MAX 64.0f
#define POSTFX3D_RGB_FLOAT_MAX_BYTES                                                               \
    ((size_t)VGFX3D_RENDERTARGET_DIM_MAX * (size_t)VGFX3D_RENDERTARGET_DIM_MAX * 3u * sizeof(float))
#define POSTFX3D_EFFECT_STORAGE_COOKIE UINT64_C(0x9F3A5D71C4E2860B)
#define POSTFX3D_TAA_STORAGE_COOKIE UINT64_C(0x47D90813BE62F5AC)
#define POSTFX3D_FBUF_STORAGE_COOKIE UINT64_C(0xC103A75E29D84B6F)
#define POSTFX3D_PRIMARY_STORAGE_COOKIE UINT64_C(0xE6852B19A4C70D3F)
#define POSTFX3D_SECONDARY_STORAGE_COOKIE UINT64_C(0x31AF94D6E8702BC5)
#define POSTFX3D_LUT_OWNER_COOKIE UINT64_C(0x72C5E18B903FA64D)
#define POSTFX3D_POOL_OWNER_COOKIE UINT64_C(0xD4A8096F15CE327B)

/*==========================================================================
 * Effect types
 *=========================================================================*/

typedef enum {
    POSTFX_BLOOM = 0,
    POSTFX_TONEMAP,
    POSTFX_FXAA,
    POSTFX_COLOR_GRADE,
    POSTFX_VIGNETTE,
    POSTFX_SSAO,
    POSTFX_DOF,
    POSTFX_MOTION_BLUR,
    POSTFX_TAA,           /* appended: value mirrors VGFX3D_POSTFX_EFFECT_TAA */
    POSTFX_SSR,           /* Plan 10: value mirrors VGFX3D_POSTFX_EFFECT_SSR */
    POSTFX_AUTO_EXPOSURE, /* mirrors VGFX3D_POSTFX_EFFECT_AUTO_EXPOSURE */
    POSTFX_COLOR_LUT,     /* mirrors VGFX3D_POSTFX_EFFECT_COLOR_LUT */
    POSTFX_SUN_SHAFTS,    /* mirrors VGFX3D_POSTFX_EFFECT_SUN_SHAFTS */
} postfx_type_t;

typedef struct {
    postfx_type_t type;
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
} postfx_entry_t;

typedef struct {
    void *vptr;
    postfx_entry_t *effects;
    int32_t effect_count;
    int32_t effect_capacity;
    int8_t enabled;
    /* Plan 09: last recoverable configuration error ("" = none). Set by
     * capability validation at Canvas3D.SetPostFX bind time so callers can
     * query why a chain was rejected instead of trapping at apply time. */
    char last_error[160];
    /* CPU scene-effect state (software path): TAA history (packed RGB float)
     * and the previous frame's view-projection for reprojection. Owned by the
     * chain; released by the finalizer; reset on size change or NotifyCut. */
    float *taa_history;
    int32_t taa_w;
    int32_t taa_h;
    int8_t taa_valid;
    float cpu_prev_vp[16];
    int8_t cpu_prev_vp_valid;
    /* Auto-exposure smoothed state (log2 exposure multiplier). */
    float auto_exposure_ev;
    int8_t auto_exposure_valid;
    /* Color LUT source: retained Pixels (256x16 strip = 16 tiles of 16x16). */
    void *lut_pixels;
    /* Reusable CPU apply scratch: packed RGB framebuffer plus two effect work buffers. */
    float *cpu_fbuf;
    size_t cpu_fbuf_bytes;
    float *cpu_scratch_primary;
    size_t cpu_scratch_primary_bytes;
    float *cpu_scratch_secondary;
    size_t cpu_scratch_secondary_bytes;
    /* Lazily-created worker pool for row-banded CPU passes (same owned-pool
     * pattern as the software rasterizer context). NULL until first use or when
     * creation failed; the chain then runs serially. */
    void *worker_pool;
    int8_t worker_pool_failed;
    /* Authoritative ownership state. The fields above remain mutable mirrors so corrupted
     * runtime payloads can be repaired without freeing, reallocating, or dereferencing a
     * borrowed pointer. These fields stay at the tail to preserve test-fixture prefixes. */
    postfx_entry_t *owned_effects;
    int32_t owned_effect_capacity;
    int32_t initialized_effect_count;
    uint64_t effect_storage_cookie;
    float *owned_taa_history;
    size_t taa_history_capacity_bytes;
    uint64_t taa_storage_cookie;
    float *owned_cpu_fbuf;
    size_t cpu_fbuf_capacity_bytes;
    uint64_t cpu_fbuf_storage_cookie;
    float *owned_cpu_scratch_primary;
    size_t cpu_scratch_primary_capacity_bytes;
    uint64_t cpu_scratch_primary_storage_cookie;
    float *owned_cpu_scratch_secondary;
    size_t cpu_scratch_secondary_capacity_bytes;
    uint64_t cpu_scratch_secondary_storage_cookie;
    void *owned_lut_pixels;
    uint64_t lut_owner_cookie;
    void *owned_worker_pool;
    int32_t worker_count;
    uint64_t worker_pool_owner_cookie;
} rt_postfx3d;

/// @brief Record a recoverable configuration error on the chain (NULL msg clears).
/// @param fx PostFX chain whose fixed-size diagnostic buffer is updated; ignored when `NULL`.
/// @param msg Null-terminated diagnostic text, or `NULL` to clear the current diagnostic.
static void postfx3d_set_last_error(rt_postfx3d *fx, const char *msg) {
    if (!fx)
        return;
    if (!msg) {
        fx->last_error[0] = '\0';
        return;
    }
    snprintf(fx->last_error, sizeof(fx->last_error), "%s", msg);
}

typedef struct {
    rt_postfx3d *fx;
} postfx_scratch_t;

/* Process-lifetime display-gamma table. 0 = uninitialized, 1 = builder active, 2 = ready. */
static float g_postfx3d_gamma_lut[POSTFX3D_GAMMA_LUT_SIZE + 1u];
static int g_postfx3d_gamma_lut_state = 0;

static uint64_t postfx3d_storage_cookie_value(const rt_postfx3d *fx,
                                              const void *storage,
                                              size_t capacity,
                                              uint64_t domain);
static void postfx3d_reset_temporal_state(rt_postfx3d *fx);

/*==========================================================================
 * Row-banded parallel execution for CPU passes
 *
 * Every converted pass is a pure per-pixel map (each output row depends only
 * on pass inputs, never on other output rows), so splitting the y-range across
 * workers produces bit-identical results to the serial loop regardless of the
 * band count or scheduling order. Passes with cross-row hazards keep their
 * serial form (or band each internal stage with a wait between stages).
 *=========================================================================*/

extern void *rt_threadpool_new(int64_t size);
extern int8_t rt_threadpool_submit_fn(void *pool, void (*callback)(void *), void *arg);
extern void rt_threadpool_wait(void *pool);
extern void rt_threadpool_shutdown(void *pool);
extern int64_t rt_parallel_default_workers(void);

#define POSTFX3D_MAX_BANDS 8
#define POSTFX3D_MIN_ROWS_PER_BAND 32

static int postfx3d_worker_pool_owner_is_valid(const rt_postfx3d *fx);

/// @brief Process one inclusive-exclusive horizontal post-processing band.
/// @param ctx Borrowed pass-specific callback context.
/// @param y0 First destination row to process.
/// @param y1 One-past-last destination row to process.
typedef void (*postfx_band_fn)(void *ctx, int32_t y0, int32_t y1);

typedef struct {
    postfx_band_fn fn;
    void *ctx;
    int32_t y0;
    int32_t y1;
} postfx_band_task_t;

/// @brief Invoke a row-band callback from the thread-pool callback ABI.
/// @param arg Pointer to a `postfx_band_task_t` describing the callback, context, and row range.
static void postfx_band_trampoline(void *arg) {
    postfx_band_task_t *task = (postfx_band_task_t *)arg;
    if (task && task->fn)
        task->fn(task->ctx, task->y0, task->y1);
}

/// @brief Lazily create (once) and return the chain's worker pool, or NULL.
/// @param fx PostFX chain that owns the lazily allocated pool.
/// @return The chain-owned worker pool, or `NULL` when parallel execution is unavailable.
static void *postfx3d_worker_pool(rt_postfx3d *fx) {
    int64_t workers;
    if (!fx)
        return NULL;
    if (postfx3d_worker_pool_owner_is_valid(fx)) {
        fx->worker_pool = fx->owned_worker_pool;
        fx->worker_pool_failed = 0;
        return fx->worker_pool;
    }
    fx->worker_pool = NULL;
    fx->owned_worker_pool = NULL;
    fx->worker_count = 0;
    fx->worker_pool_owner_cookie = 0;
    fx->worker_pool_failed = fx->worker_pool_failed ? 1 : 0;
    if (fx->worker_pool_failed)
        return NULL;
    workers = rt_parallel_default_workers();
    if (workers > POSTFX3D_MAX_BANDS)
        workers = POSTFX3D_MAX_BANDS;
    if (workers < 2) {
        fx->worker_pool_failed = 1;
        return NULL;
    }
    fx->owned_worker_pool = rt_threadpool_new(workers);
    if (!fx->owned_worker_pool) {
        fx->worker_pool_failed = 1;
        return NULL;
    }
    fx->worker_count = (int32_t)workers;
    fx->worker_pool_owner_cookie = postfx3d_storage_cookie_value(
        fx, fx->owned_worker_pool, (size_t)fx->worker_count, POSTFX3D_POOL_OWNER_COOKIE);
    fx->worker_pool = fx->owned_worker_pool;
    return fx->worker_pool;
}

/// @brief Run @p fn over [0, h) rows, banded across the chain's worker pool.
/// @details Falls back to one serial call when no pool exists, the image is
///   small, or any submission fails (the failed band runs inline, so every row
///   is always processed exactly once).
/// @param fx PostFX chain providing the optional worker pool.
/// @param h Total number of image rows.
/// @param fn Pass callback invoked once for each non-overlapping row band.
/// @param ctx Opaque pass context forwarded unchanged to @p fn.
static void postfx_run_bands(rt_postfx3d *fx, int32_t h, postfx_band_fn fn, void *ctx) {
    void *pool;
    postfx_band_task_t tasks[POSTFX3D_MAX_BANDS];
    int32_t bands;
    int32_t rows_per_band;
    int32_t extra;
    int32_t y = 0;
    if (!fn || h <= 0)
        return;
    pool = fx ? postfx3d_worker_pool(fx) : NULL;
    bands = (int32_t)(fx && pool ? fx->worker_count : 1);
    if (bands > POSTFX3D_MAX_BANDS)
        bands = POSTFX3D_MAX_BANDS;
    if (bands > h / POSTFX3D_MIN_ROWS_PER_BAND)
        bands = h / POSTFX3D_MIN_ROWS_PER_BAND;
    if (!pool || bands <= 1) {
        fn(ctx, 0, h);
        return;
    }
    rows_per_band = h / bands;
    extra = h % bands;
    for (int32_t b = 0; b < bands; b++) {
        int32_t band_rows = rows_per_band + (b < extra ? 1 : 0);
        tasks[b].fn = fn;
        tasks[b].ctx = ctx;
        tasks[b].y0 = y;
        tasks[b].y1 = y + band_rows;
        y += band_rows;
        if (!rt_threadpool_submit_fn(pool, postfx_band_trampoline, &tasks[b]))
            postfx_band_trampoline(&tasks[b]);
    }
    rt_threadpool_wait(pool);
}

/// @brief Validate @p obj as a PostFX3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Candidate runtime object.
/// @return The validated PostFX3D pointer, or `NULL` for a null or wrong-class object.
static rt_postfx3d *postfx3d_checked(void *obj) {
    return rt_obj_is_instance(obj, RT_G3D_POSTFX3D_CLASS_ID, sizeof(rt_postfx3d))
               ? (rt_postfx3d *)obj
               : NULL;
}

/// @brief Derive an address-bound cookie for one PostFX-owned allocation tuple.
static uint64_t postfx3d_storage_cookie_value(const rt_postfx3d *fx,
                                              const void *storage,
                                              size_t capacity,
                                              uint64_t domain) {
    return domain ^ (uint64_t)(uintptr_t)fx ^ (uint64_t)(uintptr_t)storage ^
           ((uint64_t)capacity * UINT64_C(0x9E3779B185EBCA87));
}

/// @brief Test whether the authoritative effect allocation may be touched.
static int postfx3d_effect_storage_is_valid(const rt_postfx3d *fx) {
    return fx && fx->owned_effects && fx->owned_effect_capacity > 0 &&
           fx->owned_effect_capacity <= VGFX3D_POSTFX_MAX_EFFECTS &&
           fx->effect_storage_cookie ==
               postfx3d_storage_cookie_value(fx,
                                             fx->owned_effects,
                                             (size_t)fx->owned_effect_capacity,
                                             POSTFX3D_EFFECT_STORAGE_COOKIE);
}

/// @brief Restore effect traversal mirrors from the authoritative allocation tuple.
static void postfx3d_repair_effect_storage(rt_postfx3d *fx) {
    if (!fx)
        return;
    if (!postfx3d_effect_storage_is_valid(fx)) {
        fx->effects = NULL;
        fx->effect_count = 0;
        fx->effect_capacity = 0;
        fx->owned_effects = NULL;
        fx->owned_effect_capacity = 0;
        fx->initialized_effect_count = 0;
        fx->effect_storage_cookie = 0;
        return;
    }
    if (fx->initialized_effect_count < 0)
        fx->initialized_effect_count = 0;
    if (fx->initialized_effect_count > fx->owned_effect_capacity)
        fx->initialized_effect_count = fx->owned_effect_capacity;
    fx->effects = fx->owned_effects;
    fx->effect_capacity = fx->owned_effect_capacity;
    fx->effect_count = fx->initialized_effect_count;
}

static int32_t postfx3d_repair_state(rt_postfx3d *fx);
static int postfx_rgb_float_layout(
    int32_t w, int32_t h, size_t *out_pixels, size_t *out_floats, size_t *out_bytes);
static void postfx3d_repair_float_storage(rt_postfx3d *fx,
                                          float **mirror,
                                          size_t *mirror_capacity,
                                          float **owned,
                                          size_t *owned_capacity,
                                          uint64_t *cookie,
                                          uint64_t domain);

/// @brief Return the authoritative, repaired number of initialized effect entries.
static int32_t postfx3d_safe_effect_count(rt_postfx3d *fx) {
    return postfx3d_repair_state(fx);
}

/*==========================================================================
 * Helpers
 *=========================================================================*/

/// @brief Clamp `v` into the closed interval `[lo, hi]`. Used throughout the effect
/// pipeline to keep intermediate float values inside the displayable range before
/// converting back to 8-bit pixels at the end.
/// @param v Value to clamp; non-finite values map to the lower bound.
/// @param lo First interval bound.
/// @param hi Second interval bound; bounds are exchanged when out of order.
/// @return The finite value constrained to the normalized closed interval.
static float clampf(float v, float lo, float hi) {
    if (lo > hi) {
        float tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (!isfinite(v))
        return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

/// @brief Clamp a double into a finite float parameter range, using fallback for NaN/Inf.
/// @param value Runtime-supplied value to narrow and clamp.
/// @param fallback Replacement used when @p value is not finite.
/// @param lo First permitted bound.
/// @param hi Second permitted bound; bounds are exchanged when out of order.
/// @return A finite float constrained to the normalized interval.
static float sanitize_range_f32(double value, float fallback, float lo, float hi) {
    float narrowed;
    if (lo > hi) {
        float tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (!isfinite(value))
        return clampf(fallback, lo, hi);
    if (value <= (double)lo)
        return lo;
    if (value >= (double)hi)
        return hi;
    if (value > POSTFX3D_FLOAT_ABS_MAX || value < -POSTFX3D_FLOAT_ABS_MAX)
        return value < 0.0 ? lo : hi;
    narrowed = (float)value;
    return isfinite(narrowed) ? clampf(narrowed, lo, hi) : (value < 0.0 ? lo : hi);
}

/// @brief `sanitize_f32` plus clamp-to-zero floor — for strengths/intensities that must be ≥ 0.
/// @param value Runtime-supplied value to sanitize.
/// @param fallback Replacement used when @p value is not finite.
/// @return A finite float in the supported non-negative parameter range.
static float sanitize_nonnegative_f32(double value, float fallback) {
    return sanitize_range_f32(value, fallback, 0.0f, POSTFX3D_PARAM_MAX);
}

/// @brief Clamp an HDR color channel into the valid [0, POSTFX3D_FOCUS_MAX] range.
/// @param value HDR channel value to sanitize.
/// @return A finite channel value in the supported HDR range.
static float sanitize_hdr_channel(float value) {
    return clampf(value, 0.0f, POSTFX3D_FOCUS_MAX);
}

/// @brief Clamp a 64-bit integer into a 32-bit range, truncating outside values.
/// @details Used where IL-side integer knobs (kernel radii, sample counts) need to match
///   backend-side `int32_t` slots without trapping on out-of-range input.
/// @param value Runtime-supplied 64-bit value.
/// @param lo First permitted 32-bit bound.
/// @param hi Second permitted 32-bit bound; bounds are exchanged when out of order.
/// @return @p value narrowed after clamping to the normalized interval.
static int32_t clamp_i64_to_i32(int64_t value, int32_t lo, int32_t hi) {
    if (lo > hi) {
        int32_t tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return (int32_t)value;
}

/// @brief Validate the retained LUT object's address-bound ownership record.
static int postfx3d_lut_owner_is_valid(const rt_postfx3d *fx) {
    return fx && fx->owned_lut_pixels &&
           rt_obj_is_instance(fx->owned_lut_pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl)) &&
           fx->lut_owner_cookie == postfx3d_storage_cookie_value(
                                       fx, fx->owned_lut_pixels, 1u, POSTFX3D_LUT_OWNER_COOKIE);
}

/// @brief Validate the lazily allocated worker pool and its address-bound owner record.
static int postfx3d_worker_pool_owner_is_valid(const rt_postfx3d *fx) {
    return fx && fx->owned_worker_pool && fx->worker_count >= 2 &&
           fx->worker_count <= POSTFX3D_MAX_BANDS &&
           rt_obj_is_instance(fx->owned_worker_pool, RT_THREADPOOL_CLASS_ID, 0) &&
           fx->worker_pool_owner_cookie ==
               postfx3d_storage_cookie_value(
                   fx, fx->owned_worker_pool, (size_t)fx->worker_count, POSTFX3D_POOL_OWNER_COOKIE);
}

/// @brief Canonicalize and bound every field of one retained effect descriptor.
/// @return Non-zero for a known effect kind, or zero when the entry must be removed.
static int postfx3d_sanitize_effect_entry(postfx_entry_t *entry) {
    float min_ev;
    float max_ev;
    if (!entry || entry->type < POSTFX_BLOOM || entry->type > POSTFX_SUN_SHAFTS)
        return 0;
    entry->enabled = entry->enabled ? 1 : 0;
    switch (entry->type) {
        case POSTFX_BLOOM:
            entry->p.bloom.threshold = sanitize_nonnegative_f32(entry->p.bloom.threshold, 0.8f);
            entry->p.bloom.intensity = sanitize_nonnegative_f32(entry->p.bloom.intensity, 1.0f);
            entry->p.bloom.blur_passes = clamp_i64_to_i32(entry->p.bloom.blur_passes, 0, 32);
            break;
        case POSTFX_TONEMAP:
            entry->p.tonemap.mode = clamp_i64_to_i32(entry->p.tonemap.mode, 0, 2);
            entry->p.tonemap.exposure = sanitize_nonnegative_f32(entry->p.tonemap.exposure, 1.0f);
            break;
        case POSTFX_FXAA:
            entry->p.fxaa.edge_threshold =
                sanitize_range_f32(entry->p.fxaa.edge_threshold, 0.166f, 0.0f, 1.0f);
            entry->p.fxaa.min_threshold =
                sanitize_range_f32(entry->p.fxaa.min_threshold, 0.0833f, 0.0f, 1.0f);
            break;
        case POSTFX_COLOR_GRADE:
            entry->p.color_grade.brightness =
                sanitize_range_f32(entry->p.color_grade.brightness, 0.0f, -1.0f, 1.0f);
            entry->p.color_grade.contrast =
                sanitize_range_f32(entry->p.color_grade.contrast, 1.0f, 0.0f, 4.0f);
            entry->p.color_grade.saturation =
                sanitize_range_f32(entry->p.color_grade.saturation, 1.0f, 0.0f, 4.0f);
            break;
        case POSTFX_VIGNETTE:
            entry->p.vignette.radius =
                sanitize_range_f32(entry->p.vignette.radius, 0.7f, 0.0f, 1.0f);
            entry->p.vignette.softness =
                sanitize_range_f32(entry->p.vignette.softness, 0.3f, 0.001f, 1.0f);
            break;
        case POSTFX_SSAO:
            entry->p.ssao.ao_radius =
                sanitize_range_f32(entry->p.ssao.ao_radius, 0.5f, 0.0f, POSTFX3D_RADIUS_MAX);
            entry->p.ssao.ao_intensity = sanitize_nonnegative_f32(entry->p.ssao.ao_intensity, 1.0f);
            entry->p.ssao.ao_samples = clamp_i64_to_i32(entry->p.ssao.ao_samples, 1, 128);
            break;
        case POSTFX_DOF:
            entry->p.dof.focus_distance =
                sanitize_range_f32(entry->p.dof.focus_distance, 10.0f, 0.0f, POSTFX3D_FOCUS_MAX);
            entry->p.dof.aperture = sanitize_nonnegative_f32(entry->p.dof.aperture, 0.0f);
            entry->p.dof.max_blur = sanitize_range_f32(entry->p.dof.max_blur, 8.0f, 0.0f, 128.0f);
            break;
        case POSTFX_MOTION_BLUR:
            entry->p.motion_blur.mb_intensity =
                sanitize_range_f32(entry->p.motion_blur.mb_intensity, 0.0f, 0.0f, 1.0f);
            entry->p.motion_blur.mb_samples =
                clamp_i64_to_i32(entry->p.motion_blur.mb_samples, 1, 64);
            break;
        case POSTFX_TAA:
            entry->p.taa.blend = sanitize_range_f32(entry->p.taa.blend, 0.9f, 0.5f, 0.98f);
            break;
        case POSTFX_SSR:
            entry->p.ssr.intensity = sanitize_range_f32(entry->p.ssr.intensity, 0.5f, 0.0f, 1.0f);
            entry->p.ssr.max_roughness =
                sanitize_range_f32(entry->p.ssr.max_roughness, 0.4f, 0.0f, 1.0f);
            entry->p.ssr.steps = clamp_i64_to_i32(entry->p.ssr.steps, 1, 128);
            break;
        case POSTFX_AUTO_EXPOSURE:
            min_ev = sanitize_range_f32(
                entry->p.auto_exposure.min_ev, -4.0f, POSTFX3D_EV_MIN, POSTFX3D_EV_MAX);
            max_ev = sanitize_range_f32(
                entry->p.auto_exposure.max_ev, 4.0f, POSTFX3D_EV_MIN, POSTFX3D_EV_MAX);
            if (!(max_ev > min_ev)) {
                min_ev = -4.0f;
                max_ev = 4.0f;
            }
            entry->p.auto_exposure.min_ev = min_ev;
            entry->p.auto_exposure.max_ev = max_ev;
            entry->p.auto_exposure.adapt_speed = sanitize_range_f32(
                entry->p.auto_exposure.adapt_speed, 3.0f, 0.001f, POSTFX3D_PARAM_MAX);
            break;
        case POSTFX_COLOR_LUT:
            entry->p.color_lut.blend =
                sanitize_range_f32(entry->p.color_lut.blend, 1.0f, 0.0f, 1.0f);
            break;
        case POSTFX_SUN_SHAFTS:
            entry->p.sun_shafts.intensity =
                sanitize_nonnegative_f32(entry->p.sun_shafts.intensity, 0.6f);
            entry->p.sun_shafts.decay =
                sanitize_range_f32(entry->p.sun_shafts.decay, 0.92f, 0.001f, 0.999f);
            entry->p.sun_shafts.samples = clamp_i64_to_i32(entry->p.sun_shafts.samples, 8, 48);
            break;
    }
    return 1;
}

/// @brief Repair all PostFX retained state before any traversal, mutation, or final export.
static int32_t postfx3d_repair_state(rt_postfx3d *fx) {
    int32_t read_index;
    int32_t write_index = 0;
    size_t taa_bytes = 0;
    if (!fx)
        return 0;
    fx->enabled = fx->enabled ? 1 : 0;
    fx->last_error[sizeof(fx->last_error) - 1u] = '\0';

    postfx3d_repair_effect_storage(fx);
    for (read_index = 0; read_index < fx->initialized_effect_count; read_index++) {
        postfx_entry_t entry = fx->owned_effects[read_index];
        if (!postfx3d_sanitize_effect_entry(&entry))
            continue;
        fx->owned_effects[write_index++] = entry;
    }
    if (fx->owned_effects && write_index < fx->initialized_effect_count) {
        memset(fx->owned_effects + write_index,
               0,
               (size_t)(fx->initialized_effect_count - write_index) * sizeof(*fx->owned_effects));
    }
    fx->initialized_effect_count = write_index;
    fx->effect_count = write_index;

    postfx3d_repair_float_storage(fx,
                                  &fx->taa_history,
                                  &fx->taa_history_capacity_bytes,
                                  &fx->owned_taa_history,
                                  &fx->taa_history_capacity_bytes,
                                  &fx->taa_storage_cookie,
                                  POSTFX3D_TAA_STORAGE_COOKIE);
    /* TAA has only one capacity field because its public mirror historically exposed dimensions,
     * not bytes. Reassert the pointer directly after validating the owner tuple above. */
    fx->taa_history = fx->owned_taa_history;
    if (!fx->taa_history ||
        !postfx_rgb_float_layout(fx->taa_w, fx->taa_h, NULL, NULL, &taa_bytes) ||
        taa_bytes > fx->taa_history_capacity_bytes) {
        fx->taa_w = 0;
        fx->taa_h = 0;
        fx->taa_valid = 0;
    } else {
        fx->taa_valid = fx->taa_valid ? 1 : 0;
    }

    postfx3d_repair_float_storage(fx,
                                  &fx->cpu_fbuf,
                                  &fx->cpu_fbuf_bytes,
                                  &fx->owned_cpu_fbuf,
                                  &fx->cpu_fbuf_capacity_bytes,
                                  &fx->cpu_fbuf_storage_cookie,
                                  POSTFX3D_FBUF_STORAGE_COOKIE);
    postfx3d_repair_float_storage(fx,
                                  &fx->cpu_scratch_primary,
                                  &fx->cpu_scratch_primary_bytes,
                                  &fx->owned_cpu_scratch_primary,
                                  &fx->cpu_scratch_primary_capacity_bytes,
                                  &fx->cpu_scratch_primary_storage_cookie,
                                  POSTFX3D_PRIMARY_STORAGE_COOKIE);
    postfx3d_repair_float_storage(fx,
                                  &fx->cpu_scratch_secondary,
                                  &fx->cpu_scratch_secondary_bytes,
                                  &fx->owned_cpu_scratch_secondary,
                                  &fx->cpu_scratch_secondary_capacity_bytes,
                                  &fx->cpu_scratch_secondary_storage_cookie,
                                  POSTFX3D_SECONDARY_STORAGE_COOKIE);

    fx->cpu_prev_vp_valid = fx->cpu_prev_vp_valid ? 1 : 0;
    if (fx->cpu_prev_vp_valid) {
        for (int32_t lane = 0; lane < 16; lane++) {
            if (!isfinite(fx->cpu_prev_vp[lane])) {
                memset(fx->cpu_prev_vp, 0, sizeof(fx->cpu_prev_vp));
                fx->cpu_prev_vp_valid = 0;
                break;
            }
        }
    }
    if (!isfinite(fx->auto_exposure_ev) || fx->auto_exposure_ev < POSTFX3D_EV_MIN ||
        fx->auto_exposure_ev > POSTFX3D_EV_MAX) {
        fx->auto_exposure_ev = 0.0f;
        fx->auto_exposure_valid = 0;
    } else {
        fx->auto_exposure_valid = fx->auto_exposure_valid ? 1 : 0;
    }

    if (postfx3d_lut_owner_is_valid(fx)) {
        fx->lut_pixels = fx->owned_lut_pixels;
    } else {
        fx->lut_pixels = NULL;
        fx->owned_lut_pixels = NULL;
        fx->lut_owner_cookie = 0;
    }
    if (postfx3d_worker_pool_owner_is_valid(fx)) {
        fx->worker_pool = fx->owned_worker_pool;
        fx->worker_pool_failed = 0;
    } else {
        fx->worker_pool = NULL;
        fx->owned_worker_pool = NULL;
        fx->worker_count = 0;
        fx->worker_pool_owner_cookie = 0;
        fx->worker_pool_failed = fx->worker_pool_failed ? 1 : 0;
    }
    return write_index;
}

/// @brief Perceptual luminance of a linear sRGB colour using the Rec. 709 weights
/// (0.2126 R + 0.7152 G + 0.0722 B). Used by the bloom extract pass, the FXAA edge
/// detector, and the saturation term of `apply_color_grade`.
/// @param r Linear red channel.
/// @param g Linear green channel.
/// @param b Linear blue channel.
/// @return Rec. 709 weighted luminance.
static float luminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/// @brief Validate an RGB float buffer shape and compute its pixel/byte counts.
/// @param w Image width in pixels.
/// @param h Image height in pixels.
/// @param[out] out_pixels Optional destination for the total pixel count.
/// @param[out] out_floats Optional destination for the packed RGB float count.
/// @param[out] out_bytes Optional destination for the packed RGB byte count.
/// @return 1 when all dimensions and derived sizes are valid, or 0 on invalid input or overflow.
static int postfx_rgb_float_layout(
    int32_t w, int32_t h, size_t *out_pixels, size_t *out_floats, size_t *out_bytes) {
    size_t width;
    size_t height;
    size_t pixels;
    size_t floats;
    if (w <= 0 || h <= 0 || w > VGFX3D_RENDERTARGET_DIM_MAX || h > VGFX3D_RENDERTARGET_DIM_MAX)
        return 0;
    width = (size_t)w;
    height = (size_t)h;
    if (width > SIZE_MAX / height)
        return 0;
    pixels = width * height;
    if (pixels > SIZE_MAX / 3u)
        return 0;
    floats = pixels * 3u;
    if (floats > SIZE_MAX / sizeof(float))
        return 0;
    if (out_pixels)
        *out_pixels = pixels;
    if (out_floats)
        *out_floats = floats;
    if (out_bytes)
        *out_bytes = floats * sizeof(float);
    return 1;
}

/// @brief Validate an authoritative float-allocation tuple against its owner and policy limit.
static int postfx3d_float_storage_is_valid(const rt_postfx3d *fx,
                                           const float *owned,
                                           size_t capacity_bytes,
                                           uint64_t cookie,
                                           uint64_t domain) {
    return fx && owned && capacity_bytes > 0 && capacity_bytes <= POSTFX3D_RGB_FLOAT_MAX_BYTES &&
           cookie == postfx3d_storage_cookie_value(fx, owned, capacity_bytes, domain);
}

/// @brief Restore one float-storage mirror, or detach a tuple that cannot prove ownership.
static void postfx3d_repair_float_storage(rt_postfx3d *fx,
                                          float **mirror,
                                          size_t *mirror_capacity,
                                          float **owned,
                                          size_t *owned_capacity,
                                          uint64_t *cookie,
                                          uint64_t domain) {
    if (!fx || !mirror || !mirror_capacity || !owned || !owned_capacity || !cookie)
        return;
    if (!postfx3d_float_storage_is_valid(fx, *owned, *owned_capacity, *cookie, domain)) {
        *mirror = NULL;
        *mirror_capacity = 0;
        *owned = NULL;
        *owned_capacity = 0;
        *cookie = 0;
        return;
    }
    *mirror = *owned;
    *mirror_capacity = *owned_capacity;
}

/// @brief Grow one PostFX-owned float allocation geometrically and publish it transactionally.
static float *postfx3d_reserve_float_storage(rt_postfx3d *fx,
                                             float **mirror,
                                             size_t *mirror_capacity,
                                             float **owned,
                                             size_t *owned_capacity,
                                             uint64_t *cookie,
                                             uint64_t domain,
                                             size_t needed_bytes,
                                             int clear) {
    size_t capacity;
    float *grown;
    if (!fx || !mirror || !mirror_capacity || !owned || !owned_capacity || !cookie ||
        needed_bytes == 0 || needed_bytes > POSTFX3D_RGB_FLOAT_MAX_BYTES)
        return NULL;
    postfx3d_repair_float_storage(
        fx, mirror, mirror_capacity, owned, owned_capacity, cookie, domain);
    if (needed_bytes > *owned_capacity) {
        capacity = *owned_capacity > 0 ? *owned_capacity : 4096u;
        while (capacity < needed_bytes) {
            size_t next = capacity + capacity / 2u;
            if (next <= capacity || next > POSTFX3D_RGB_FLOAT_MAX_BYTES) {
                capacity = needed_bytes;
                break;
            }
            capacity = next;
        }
        grown = (float *)realloc(*owned, capacity);
        if (!grown)
            return NULL;
        *owned = grown;
        *owned_capacity = capacity;
        *cookie = postfx3d_storage_cookie_value(fx, grown, capacity, domain);
        *mirror = grown;
        *mirror_capacity = capacity;
    }
    if (!*owned)
        return NULL;
    if (clear)
        memset(*owned, 0, needed_bytes);
    return *owned;
}

/// @brief Reserve one of the two reusable effect scratch allocations.
static float *postfx_scratch_reserve(postfx_scratch_t *scratch,
                                     int secondary,
                                     size_t needed_bytes,
                                     int clear) {
    rt_postfx3d *fx = scratch ? scratch->fx : NULL;
    if (!fx)
        return NULL;
    if (secondary) {
        return postfx3d_reserve_float_storage(fx,
                                              &fx->cpu_scratch_secondary,
                                              &fx->cpu_scratch_secondary_bytes,
                                              &fx->owned_cpu_scratch_secondary,
                                              &fx->cpu_scratch_secondary_capacity_bytes,
                                              &fx->cpu_scratch_secondary_storage_cookie,
                                              POSTFX3D_SECONDARY_STORAGE_COOKIE,
                                              needed_bytes,
                                              clear);
    }
    return postfx3d_reserve_float_storage(fx,
                                          &fx->cpu_scratch_primary,
                                          &fx->cpu_scratch_primary_bytes,
                                          &fx->owned_cpu_scratch_primary,
                                          &fx->cpu_scratch_primary_capacity_bytes,
                                          &fx->cpu_scratch_primary_storage_cookie,
                                          POSTFX3D_PRIMARY_STORAGE_COOKIE,
                                          needed_bytes,
                                          clear);
}

/// @brief Reserve the retained CPU framebuffer scratch for one PostFX apply.
/// @details CPU post-processing runs through a packed RGB float buffer. Keeping that allocation on
///          the PostFX object avoids malloc/free churn for stable frame sizes; the buffer grows on
///          demand and is released by the PostFX finalizer.
/// @param fx PostFX chain that owns the scratch buffer.
/// @param needed_bytes Required byte count for this frame.
/// @return Retained framebuffer scratch pointer, or NULL on allocation failure.
static float *postfx3d_reserve_cpu_fbuf(rt_postfx3d *fx, size_t needed_bytes) {
    if (!fx || needed_bytes == 0)
        return NULL;
    return postfx3d_reserve_float_storage(fx,
                                          &fx->cpu_fbuf,
                                          &fx->cpu_fbuf_bytes,
                                          &fx->owned_cpu_fbuf,
                                          &fx->cpu_fbuf_capacity_bytes,
                                          &fx->cpu_fbuf_storage_cookie,
                                          POSTFX3D_FBUF_STORAGE_COOKIE,
                                          needed_bytes,
                                          0);
}

/// @brief Populate a small lookup table for linear-to-display gamma correction.
/// @details The tonemap pass clamps channels into [0, 1] before gamma. Building one table per
///          tonemap invocation replaces three `powf` calls per pixel with linear interpolation
///          over @c POSTFX3D_GAMMA_LUT_SIZE samples while preserving smooth output.
/// @param lut Output array with @c POSTFX3D_GAMMA_LUT_SIZE + 1 entries.
static void postfx_build_gamma_lut(float lut[POSTFX3D_GAMMA_LUT_SIZE + 1u]) {
    for (uint32_t i = 0; i <= POSTFX3D_GAMMA_LUT_SIZE; i++) {
        float x = (float)i / (float)POSTFX3D_GAMMA_LUT_SIZE;
        lut[i] = powf(x, 1.0f / 2.2f);
    }
}

/// @brief Return the immutable process-wide gamma table, building it exactly once.
static const float *postfx_gamma_lut_data(void) {
    if (rt_atomic_load_i32(&g_postfx3d_gamma_lut_state, __ATOMIC_ACQUIRE) != 2) {
        int expected = 0;
        if (rt_atomic_compare_exchange_i32(
                &g_postfx3d_gamma_lut_state, &expected, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            postfx_build_gamma_lut(g_postfx3d_gamma_lut);
            rt_atomic_store_i32(&g_postfx3d_gamma_lut_state, 2, __ATOMIC_RELEASE);
        } else {
            while (rt_atomic_load_i32(&g_postfx3d_gamma_lut_state, __ATOMIC_ACQUIRE) != 2) {
                /* Another caller is publishing the small process-lifetime table. */
            }
        }
    }
    return g_postfx3d_gamma_lut;
}

/// @brief Sample a gamma lookup table with linear interpolation.
/// @param lut Gamma table built by `postfx_build_gamma_lut`.
/// @param value Linear channel in approximately [0, 1].
/// @return Gamma-corrected channel clamped to [0, 1].
static float postfx_gamma_lut_sample(const float lut[POSTFX3D_GAMMA_LUT_SIZE + 1u], float value) {
    float scaled;
    uint32_t index;
    float t;
    value = clampf(isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
    scaled = value * (float)POSTFX3D_GAMMA_LUT_SIZE;
    index = (uint32_t)scaled;
    if (index >= POSTFX3D_GAMMA_LUT_SIZE)
        return lut[POSTFX3D_GAMMA_LUT_SIZE];
    t = scaled - (float)index;
    return lut[index] + (lut[index + 1u] - lut[index]) * t;
}

/// @brief Reserve and zero one more `postfx_entry_t` slot in the chain, growing the heap
/// buffer when capacity is exceeded. Capacity doubles on each grow (starting at 8) so the
/// amortized cost of long chains is O(1). Returns NULL on realloc failure — the caller's
/// chain stays intact and the caller silently drops the add, matching the prior fixed-
/// size-array implementation's "silent drop past N" contract.
/// @param fx PostFX chain whose internal effect array receives the new entry.
/// @return A zero-initialized chain-owned entry, or `NULL` when storage cannot grow.
static postfx_entry_t *postfx_append_entry(rt_postfx3d *fx) {
    postfx_entry_t *effects;
    int32_t new_capacity;

    if (!fx)
        return NULL;
    (void)postfx3d_repair_state(fx);
    if (fx->initialized_effect_count >= VGFX3D_POSTFX_MAX_EFFECTS)
        return NULL;
    if (fx->effect_count < fx->effect_capacity) {
        postfx_entry_t *entry = &fx->effects[fx->effect_count++];
        fx->initialized_effect_count = fx->effect_count;
        memset(entry, 0, sizeof(*entry));
        postfx3d_reset_temporal_state(fx);
        return entry;
    }

    if (fx->effect_capacity > VGFX3D_POSTFX_MAX_EFFECTS / 2)
        new_capacity = fx->effect_count + 1;
    else
        new_capacity = fx->effect_capacity > 0 ? fx->effect_capacity * 2 : 8;
    if (new_capacity < fx->effect_count + 1)
        new_capacity = fx->effect_count + 1;
    if (new_capacity > VGFX3D_POSTFX_MAX_EFFECTS)
        new_capacity = VGFX3D_POSTFX_MAX_EFFECTS;
    if (new_capacity <= fx->effect_capacity ||
        (size_t)new_capacity > SIZE_MAX / sizeof(postfx_entry_t))
        return NULL;
    effects =
        (postfx_entry_t *)realloc(fx->owned_effects, (size_t)new_capacity * sizeof(postfx_entry_t));
    if (!effects)
        return NULL;
    memset(effects + fx->owned_effect_capacity,
           0,
           (size_t)(new_capacity - fx->owned_effect_capacity) * sizeof(postfx_entry_t));
    fx->owned_effects = effects;
    fx->owned_effect_capacity = new_capacity;
    fx->effect_storage_cookie = postfx3d_storage_cookie_value(
        fx, effects, (size_t)new_capacity, POSTFX3D_EFFECT_STORAGE_COOKIE);
    fx->effects = effects;
    fx->effect_capacity = new_capacity;
    effects = &fx->effects[fx->effect_count++];
    fx->initialized_effect_count = fx->effect_count;
    memset(effects, 0, sizeof(*effects));
    postfx3d_reset_temporal_state(fx);
    return effects;
}

/// Domain separator for address-bound backend-chain allocation cookies.
#define POSTFX3D_BACKEND_CHAIN_STORAGE_COOKIE UINT64_C(0xA61D4F39C708E25B)

static uint64_t vgfx3d_postfx_chain_cookie_value(const vgfx3d_postfx_chain_t *chain,
                                                 const void *storage,
                                                 int32_t capacity) {
    return POSTFX3D_BACKEND_CHAIN_STORAGE_COOKIE ^ (uint64_t)(uintptr_t)chain ^
           (uint64_t)(uintptr_t)storage ^
           ((uint64_t)(uint32_t)capacity * UINT64_C(0x9E3779B185EBCA87));
}

static int vgfx3d_postfx_chain_storage_is_valid(const vgfx3d_postfx_chain_t *chain) {
    return chain && chain->owned_effects && chain->owned_effect_capacity > 0 &&
           chain->owned_effect_capacity <= VGFX3D_POSTFX_MAX_EFFECTS &&
           chain->effect_storage_cookie ==
               vgfx3d_postfx_chain_cookie_value(
                   chain, chain->owned_effects, chain->owned_effect_capacity);
}

static void vgfx3d_postfx_chain_repair_storage(vgfx3d_postfx_chain_t *chain) {
    if (!chain)
        return;
    if (!vgfx3d_postfx_chain_storage_is_valid(chain)) {
        chain->effects = NULL;
        chain->effect_count = 0;
        chain->effect_capacity = 0;
        chain->owned_effects = NULL;
        chain->owned_effect_capacity = 0;
        chain->effect_storage_cookie = 0;
        return;
    }
    chain->effects = chain->owned_effects;
    chain->effect_capacity = chain->owned_effect_capacity;
    if (chain->effect_count < 0)
        chain->effect_count = 0;
    if (chain->effect_count > chain->effect_capacity)
        chain->effect_count = chain->effect_capacity;
}

/// @brief Ensure the backend-facing postfx chain buffer has room for at least `needed`
/// effect descriptors, doubling capacity each grow (starting at 8) and saturating at
/// `INT32_MAX` to avoid integer overflow in the size-in-bytes multiplication. Returns 1
/// on success, 0 on realloc failure. Freshly allocated entries are zeroed so unused
/// tail descriptors carry an `enabled = 0` marker by default.
/// @param chain Backend-facing chain whose descriptor allocation may grow.
/// @param needed Minimum number of descriptors the allocation must hold.
/// @return 1 when the requested capacity is available, or 0 for invalid metadata or allocation
///   failure.
static int vgfx3d_postfx_chain_reserve(vgfx3d_postfx_chain_t *chain, int32_t needed) {
    vgfx3d_postfx_effect_desc_t *effects;
    int32_t new_capacity;
    int32_t old_capacity;

    if (!chain || needed < 0 || needed > VGFX3D_POSTFX_MAX_EFFECTS)
        return 0;
    vgfx3d_postfx_chain_repair_storage(chain);
    if (needed <= 0)
        return 1;
    if (chain->effect_capacity >= needed && chain->effects)
        return 1;

    old_capacity = chain->effect_capacity;
    new_capacity = chain->effect_capacity > 0 ? chain->effect_capacity : 8;
    while (new_capacity < needed) {
        if (new_capacity > VGFX3D_POSTFX_MAX_EFFECTS / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > VGFX3D_POSTFX_MAX_EFFECTS)
        new_capacity = VGFX3D_POSTFX_MAX_EFFECTS;
    if ((size_t)new_capacity > SIZE_MAX / sizeof(*effects))
        return 0;
    effects = (vgfx3d_postfx_effect_desc_t *)realloc(chain->owned_effects,
                                                     (size_t)new_capacity * sizeof(*effects));
    if (!effects)
        return 0;
    if (new_capacity > old_capacity) {
        memset(effects + old_capacity, 0, (size_t)(new_capacity - old_capacity) * sizeof(*effects));
    }
    chain->effects = effects;
    chain->effect_capacity = new_capacity;
    chain->owned_effects = effects;
    chain->owned_effect_capacity = new_capacity;
    chain->effect_storage_cookie = vgfx3d_postfx_chain_cookie_value(chain, effects, new_capacity);
    return 1;
}

/// @brief Translate an internal `postfx_entry_t` into the backend-facing snapshot
/// descriptor consumed by GPU postfx passes. Returns 0 (skip this slot) for disabled
/// effects, NULL inputs, or unknown effect types so the backend never walks partial data.
/// The snapshot is a stable, flat struct so GPU shaders don't have to chase the internal
/// tagged union.
/// @param e Internal effect entry to translate.
/// @param[out] out_effect Destination backend descriptor.
/// @return 1 when a supported enabled effect was written, or 0 when the entry must be skipped.
static int vgfx3d_postfx_fill_effect_snapshot(const postfx_entry_t *e,
                                              vgfx3d_postfx_effect_desc_t *out_effect) {
    postfx_entry_t safe_entry;
    vgfx3d_postfx_snapshot_t snapshot;

    if (!e || !out_effect)
        return 0;
    memset(out_effect, 0, sizeof(*out_effect));
    safe_entry = *e;
    if (!postfx3d_sanitize_effect_entry(&safe_entry) || !safe_entry.enabled)
        return 0;
    e = &safe_entry;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.enabled = 1;
    out_effect->type = (int32_t)e->type;

    switch (e->type) {
        case POSTFX_BLOOM:
            snapshot.bloom_enabled = 1;
            snapshot.bloom_threshold = e->p.bloom.threshold;
            snapshot.bloom_intensity = e->p.bloom.intensity;
            snapshot.bloom_passes = e->p.bloom.blur_passes;
            break;
        case POSTFX_TONEMAP:
            snapshot.tonemap_mode = (int8_t)e->p.tonemap.mode;
            snapshot.tonemap_exposure = e->p.tonemap.exposure;
            snapshot.tonemap_explicit = 1;
            break;
        case POSTFX_FXAA:
            snapshot.fxaa_enabled = 1;
            break;
        case POSTFX_COLOR_GRADE:
            snapshot.color_grade_enabled = 1;
            snapshot.cg_brightness = e->p.color_grade.brightness;
            snapshot.cg_contrast = e->p.color_grade.contrast;
            snapshot.cg_saturation = e->p.color_grade.saturation;
            break;
        case POSTFX_VIGNETTE:
            snapshot.vignette_enabled = 1;
            snapshot.vignette_radius = e->p.vignette.radius;
            snapshot.vignette_softness = e->p.vignette.softness;
            break;
        case POSTFX_SSAO:
            snapshot.ssao_enabled = 1;
            snapshot.ssao_radius = e->p.ssao.ao_radius;
            snapshot.ssao_intensity = e->p.ssao.ao_intensity;
            snapshot.ssao_samples = e->p.ssao.ao_samples;
            break;
        case POSTFX_DOF:
            snapshot.dof_enabled = 1;
            snapshot.dof_focus_distance = e->p.dof.focus_distance;
            snapshot.dof_aperture = e->p.dof.aperture;
            snapshot.dof_max_blur = e->p.dof.max_blur;
            break;
        case POSTFX_MOTION_BLUR:
            snapshot.motion_blur_enabled = 1;
            snapshot.motion_blur_intensity = e->p.motion_blur.mb_intensity;
            snapshot.motion_blur_samples = e->p.motion_blur.mb_samples;
            break;
        case POSTFX_TAA:
            snapshot.taa_enabled = 1;
            snapshot.taa_blend = e->p.taa.blend;
            break;
        case POSTFX_SSR:
            snapshot.ssr_enabled = 1;
            snapshot.ssr_intensity = e->p.ssr.intensity;
            snapshot.ssr_max_roughness = e->p.ssr.max_roughness;
            snapshot.ssr_steps = e->p.ssr.steps;
            break;
        case POSTFX_AUTO_EXPOSURE:
        case POSTFX_COLOR_LUT:
        case POSTFX_SUN_SHAFTS:
            /* These software-reference effects do not yet have legacy flat-snapshot parameter
             * slots. Preserve their ordered discriminator instead of silently deleting them. */
            break;
        default:
            return 0;
    }

    out_effect->snapshot = snapshot;
    return 1;
}

/// @brief Report whether the chain contains any effect that needs GPU-side scene
///        inputs beyond the final color buffer.
/// @details SSAO needs a depth buffer, DOF needs depth (and sometimes velocity), and
///          motion blur needs a velocity buffer — those inputs are expensive to
///          allocate and pipe through, so backends skip them when the chain doesn't
///          need them. Any other effect (bloom, tonemap, color grade, vignette)
///          composes on the color buffer alone and doesn't force the extra targets.
///          This gate is what the backend render-target allocator checks before
///          creating the depth/velocity attachments.
/// @param postfx Candidate PostFX3D chain.
/// @return 1 if the chain requires auxiliary GPU scene buffers, 0 if color-only.
int vgfx3d_postfx_requires_gpu_scene_buffers(void *postfx) {
    rt_postfx3d *fx = postfx3d_checked(postfx);
    int32_t effect_count = postfx3d_safe_effect_count(fx);
    if (!fx || !fx->enabled || effect_count <= 0)
        return 0;
    for (int32_t i = 0; i < effect_count; i++) {
        const postfx_entry_t *e = &fx->effects[i];
        if (!e->enabled)
            continue;
        if (e->type == POSTFX_SSAO || e->type == POSTFX_DOF || e->type == POSTFX_MOTION_BLUR ||
            e->type == POSTFX_TAA || e->type == POSTFX_SSR)
            return 1;
    }
    return 0;
}

/*==========================================================================
 * Effect implementations (per-pixel on float buffer)
 *=========================================================================*/

#define POSTFX3D_BLOOM_MAX_LEVELS 6
#define POSTFX3D_BLOOM_MIN_DIM 8

/// @brief Sample one RGB float level bilinearly at a continuous pixel coordinate.
/// @details Coordinates are in the destination level's own pixel space; edges clamp.
///          Shared by the bloom chain's progressive upsample and the final composite.
/// @param level Packed RGB source level.
/// @param lw Source width in pixels.
/// @param lh Source height in pixels.
/// @param fx Continuous source-space x coordinate.
/// @param fy Continuous source-space y coordinate.
/// @param[out] out Three-element destination receiving the interpolated RGB value.
static void postfx_bloom_sample_bilinear(
    const float *level, int32_t lw, int32_t lh, float fx, float fy, float out[3]) {
    int32_t x0 = (int32_t)floorf(fx);
    int32_t y0 = (int32_t)floorf(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;
    int32_t x1;
    int32_t y1;
    if (x0 < 0) {
        x0 = 0;
        tx = 0.0f;
    }
    if (y0 < 0) {
        y0 = 0;
        ty = 0.0f;
    }
    x1 = x0 + 1;
    y1 = y0 + 1;
    if (x0 >= lw)
        x0 = lw - 1;
    if (y0 >= lh)
        y0 = lh - 1;
    if (x1 >= lw)
        x1 = lw - 1;
    if (y1 >= lh)
        y1 = lh - 1;
    {
        const float *p00 = &level[(y0 * lw + x0) * 3];
        const float *p10 = &level[(y0 * lw + x1) * 3];
        const float *p01 = &level[(y1 * lw + x0) * 3];
        const float *p11 = &level[(y1 * lw + x1) * 3];
        for (int32_t c = 0; c < 3; c++) {
            float top = p00[c] + (p10[c] - p00[c]) * tx;
            float bottom = p01[c] + (p11[c] - p01[c]) * tx;
            out[c] = top + (bottom - top) * ty;
        }
    }
}

/// @brief Apply bloom via a progressive downsample/upsample mip chain (Plan 05).
/// @details Level 0 is a half-resolution bright extract: each destination texel averages a
///          2×2 source box with Karis luma weights (1 / (1 + luma)) to suppress single-pixel
///          fireflies, then applies the soft-knee threshold `(lum - threshold) / (lum + ε)`.
///          Deeper levels are plain 2×2 box downsamples of the previous level. The upsample
///          walk accumulates each deeper level into the one above it with bilinear filtering,
///          so the final level 0 holds the sum of every blur octave — a wide, stable halo
///          that a single-level Gaussian cannot produce. `blur_passes` selects the chain
///          depth (clamped to POSTFX3D_BLOOM_MAX_LEVELS and to levels whose min dimension
///          stays ≥ POSTFX3D_BLOOM_MIN_DIM); the composite divides by the accumulated level
///          count so perceived brightness stays roughly independent of depth. All levels
///          pack into one scratch allocation (offsets never alias between adjacent levels).
/// @param buf Packed linear RGB framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param threshold Luminance threshold for the bright-extract pass.
/// @param intensity Scale applied when compositing the accumulated bloom chain.
/// @param blur_passes Requested number of downsample levels, clamped to the supported range.
/// @param scratch Reusable allocations owned by the active PostFX chain.
static void apply_bloom(float *buf,
                        int32_t w,
                        int32_t h,
                        float threshold,
                        float intensity,
                        int32_t blur_passes,
                        postfx_scratch_t *scratch) {
    int32_t lw[POSTFX3D_BLOOM_MAX_LEVELS];
    int32_t lh[POSTFX3D_BLOOM_MAX_LEVELS];
    size_t offset[POSTFX3D_BLOOM_MAX_LEVELS];
    int32_t levels = 0;
    int32_t depth = blur_passes < 1 ? 1 : blur_passes;
    size_t total_floats = 0;
    float *chain;

    if (depth > POSTFX3D_BLOOM_MAX_LEVELS)
        depth = POSTFX3D_BLOOM_MAX_LEVELS;
    if (!buf || w / 2 < 1 || h / 2 < 1 || !scratch)
        return;

    /* Plan the chain: level 0 = half res, each next level halves again. */
    for (int32_t i = 0; i < depth; i++) {
        int32_t cw = (i == 0) ? w / 2 : lw[i - 1] / 2;
        int32_t ch = (i == 0) ? h / 2 : lh[i - 1] / 2;
        size_t level_floats;
        if (i > 0 && (cw < POSTFX3D_BLOOM_MIN_DIM || ch < POSTFX3D_BLOOM_MIN_DIM))
            break;
        if (cw < 1 || ch < 1)
            break;
        if (!postfx_rgb_float_layout(cw, ch, NULL, &level_floats, NULL))
            return;
        if (total_floats > SIZE_MAX - level_floats)
            return;
        lw[i] = cw;
        lh[i] = ch;
        offset[i] = total_floats;
        total_floats += level_floats;
        levels = i + 1;
    }
    if (levels <= 0 || total_floats > SIZE_MAX / sizeof(float))
        return;

    chain = postfx_scratch_reserve(scratch, 0, total_floats * sizeof(float), 1);
    if (!chain)
        return;

    /* Level 0: Karis-weighted 2x2 box + soft-knee bright extract from the full-res buffer. */
    {
        float *dst = chain + offset[0];
        for (int32_t y = 0; y < lh[0]; y++)
            for (int32_t x = 0; x < lw[0]; x++) {
                float acc[3] = {0.0f, 0.0f, 0.0f};
                float wsum = 0.0f;
                for (int32_t dy = 0; dy < 2; dy++)
                    for (int32_t dx = 0; dx < 2; dx++) {
                        int32_t sx = x * 2 + dx;
                        int32_t sy = y * 2 + dy;
                        const float *p;
                        float kw;
                        if (sx >= w)
                            sx = w - 1;
                        if (sy >= h)
                            sy = h - 1;
                        p = &buf[(sy * w + sx) * 3];
                        kw = 1.0f / (1.0f + luminance(p[0], p[1], p[2]));
                        acc[0] += p[0] * kw;
                        acc[1] += p[1] * kw;
                        acc[2] += p[2] * kw;
                        wsum += kw;
                    }
                if (wsum > 0.0f) {
                    acc[0] /= wsum;
                    acc[1] /= wsum;
                    acc[2] /= wsum;
                }
                {
                    float lum = luminance(acc[0], acc[1], acc[2]);
                    float *d = &dst[(y * lw[0] + x) * 3];
                    if (lum > threshold) {
                        float scale = (lum - threshold) / (lum + 1e-6f);
                        d[0] = acc[0] * scale;
                        d[1] = acc[1] * scale;
                        d[2] = acc[2] * scale;
                    }
                }
            }
    }

    /* Progressive downsample: plain 2x2 box from the previous level. */
    for (int32_t i = 1; i < levels; i++) {
        const float *src = chain + offset[i - 1];
        float *dst = chain + offset[i];
        for (int32_t y = 0; y < lh[i]; y++)
            for (int32_t x = 0; x < lw[i]; x++) {
                float acc[3] = {0.0f, 0.0f, 0.0f};
                for (int32_t dy = 0; dy < 2; dy++)
                    for (int32_t dx = 0; dx < 2; dx++) {
                        int32_t sx = x * 2 + dx;
                        int32_t sy = y * 2 + dy;
                        const float *p;
                        if (sx >= lw[i - 1])
                            sx = lw[i - 1] - 1;
                        if (sy >= lh[i - 1])
                            sy = lh[i - 1] - 1;
                        p = &src[(sy * lw[i - 1] + sx) * 3];
                        acc[0] += p[0];
                        acc[1] += p[1];
                        acc[2] += p[2];
                    }
                {
                    float *d = &dst[(y * lw[i] + x) * 3];
                    d[0] = acc[0] * 0.25f;
                    d[1] = acc[1] * 0.25f;
                    d[2] = acc[2] * 0.25f;
                }
            }
    }

    /* Progressive upsample + accumulate: each deeper octave adds into the level above. */
    for (int32_t i = levels - 2; i >= 0; i--) {
        const float *src = chain + offset[i + 1];
        float *dst = chain + offset[i];
        for (int32_t y = 0; y < lh[i]; y++)
            for (int32_t x = 0; x < lw[i]; x++) {
                float fx = ((float)x + 0.5f) * 0.5f - 0.5f;
                float fy = ((float)y + 0.5f) * 0.5f - 0.5f;
                float s[3];
                float *d = &dst[(y * lw[i] + x) * 3];
                postfx_bloom_sample_bilinear(src, lw[i + 1], lh[i + 1], fx, fy, s);
                d[0] += s[0];
                d[1] += s[1];
                d[2] += s[2];
            }
    }

    /* Composite: level 0 now carries the accumulated chain; normalize by level count. */
    {
        const float *level0 = chain + offset[0];
        float scale = intensity / (float)levels;
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++) {
                float fx = ((float)x + 0.5f) * 0.5f - 0.5f;
                float fy = ((float)y + 0.5f) * 0.5f - 0.5f;
                float s[3];
                float *d = &buf[(y * w + x) * 3];
                postfx_bloom_sample_bilinear(level0, lw[0], lh[0], fx, fy, s);
                d[0] += s[0] * scale;
                d[1] += s[1] * scale;
                d[2] += s[2] * scale;
            }
    }
}

/// @brief Apply HDR → LDR tone mapping in linear RGB. `mode = 1` selects simple
/// Reinhard (`c / (c + 1)`), `mode = 2` selects the Narkowicz ACES filmic approximation
/// (five-constant rational that matches the Academy curve). Mode 0 is passthrough on LDR
/// buffers but, when @p hdr_gamma is non-zero (linear-HDR source), applies exposure +
/// clamp + gamma-out so an explicit "tonemap off" still produces perceptual sRGB output
/// matching modes 1/2's output transform (Plan 05 gamma fix). Multiplies by `exposure`
/// before mapping so scenes keyed for EV 0 stay in the mapper's responsive range.
/// Finishes with a 2.2-gamma correction so the 8-bit framebuffer gets perceptual output
/// (the earlier float buffer is linear).
typedef struct {
    float *buf;
    int32_t w;
    int32_t mode;
    float exposure;
    const float *gamma_lut;
} postfx_tonemap_band_ctx;

/// @brief Tone-map one non-overlapping band of packed RGB pixels.
/// @param ctx_ptr Pointer to a fully initialized `postfx_tonemap_band_ctx`.
/// @param y0 Inclusive first row to process.
/// @param y1 Exclusive row limit.
static void apply_tonemap_rows(void *ctx_ptr, int32_t y0, int32_t y1) {
    const postfx_tonemap_band_ctx *bc = (const postfx_tonemap_band_ctx *)ctx_ptr;
    float *buf = bc->buf;
    int32_t mode = bc->mode;
    float exposure = bc->exposure;
    const float *gamma_lut = bc->gamma_lut;
    size_t begin = (size_t)y0 * (size_t)bc->w;
    size_t end = (size_t)y1 * (size_t)bc->w;
    for (size_t i = begin; i < end; i++) {
        float *p = &buf[i * 3u];
        float r = p[0] * exposure, g = p[1] * exposure, b = p[2] * exposure;
        if (mode == 0) {
            /* Linear passthrough: exposure + clamp + gamma-out only. */
        } else if (mode == 1) {
            /* Reinhard */
            r = r / (r + 1.0f);
            g = g / (g + 1.0f);
            b = b / (b + 1.0f);
        } else {
            /* ACES filmic approximation (Narkowicz) */
            float ar = r * (r * 2.51f + 0.03f);
            float br = r * (r * 2.43f + 0.59f) + 0.14f;
            r = clampf(ar / br, 0.0f, 1.0f);
            float ag = g * (g * 2.51f + 0.03f);
            float bg = g * (g * 2.43f + 0.59f) + 0.14f;
            g = clampf(ag / bg, 0.0f, 1.0f);
            float ab = b * (b * 2.51f + 0.03f);
            float bb = b * (b * 2.43f + 0.59f) + 0.14f;
            b = clampf(ab / bb, 0.0f, 1.0f);
        }
        /* Gamma correction */
        r = clampf(isfinite(r) ? r : 0.0f, 0.0f, 1.0f);
        g = clampf(isfinite(g) ? g : 0.0f, 0.0f, 1.0f);
        b = clampf(isfinite(b) ? b : 0.0f, 0.0f, 1.0f);
        p[0] = postfx_gamma_lut_sample(gamma_lut, r);
        p[1] = postfx_gamma_lut_sample(gamma_lut, g);
        p[2] = postfx_gamma_lut_sample(gamma_lut, b);
    }
}

/// @brief Apply the selected exposure, tone curve, and display gamma transform.
/// @param fx PostFX chain providing row-band parallelism.
/// @param buf Packed linear RGB framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param mode Tone-map mode: zero for passthrough, one for Reinhard, or two for ACES.
/// @param exposure Linear exposure multiplier applied before the tone curve.
/// @param hdr_gamma Non-zero when mode zero must still convert linear HDR input to display gamma.
static void apply_tonemap(rt_postfx3d *fx,
                          float *buf,
                          int32_t w,
                          int32_t h,
                          int32_t mode,
                          float exposure,
                          int hdr_gamma) {
    size_t count;
    postfx_tonemap_band_ctx ctx;
    if (mode != 1 && mode != 2 && !(mode == 0 && hdr_gamma))
        return;
    if (!postfx_rgb_float_layout(w, h, &count, NULL, NULL))
        return;
    ctx.buf = buf;
    ctx.w = w;
    ctx.mode = mode;
    ctx.exposure = exposure;
    ctx.gamma_lut = postfx_gamma_lut_data();
    postfx_run_bands(fx, h, apply_tonemap_rows, &ctx);
}

/// @brief Apply a simplified FXAA pass — sample each pixel's luminance plus its four
/// neighbours, compute the luminance range, and if it exceeds both `edge_threshold *
/// lmax` and `min_threshold` (avoids smoothing near-uniform regions), replace the
/// pixel with a 3×3 box average. Writes into a scratch buffer and memcpys back, so
/// the pass is order-independent within a single invocation. Cheaper than real FXAA
/// 3.11 but good enough for jagged-edge cleanup over the software pipeline's output.
typedef struct {
    float *buf;
    float *out;
    int32_t w;
    int32_t h;
    float edge_thresh;
    float min_thresh;
} postfx_fxaa_band_ctx;

/// @brief Detect and smooth contrast edges in one FXAA row band.
/// @param ctx_ptr Pointer to a fully initialized `postfx_fxaa_band_ctx`.
/// @param band_y0 Inclusive first row assigned to the band.
/// @param band_y1 Exclusive row limit assigned to the band.
static void apply_fxaa_rows(void *ctx_ptr, int32_t band_y0, int32_t band_y1) {
    const postfx_fxaa_band_ctx *bc = (const postfx_fxaa_band_ctx *)ctx_ptr;
    float *buf = bc->buf;
    float *out = bc->out;
    int32_t w = bc->w;
    int32_t h = bc->h;
    float edge_thresh = bc->edge_thresh;
    float min_thresh = bc->min_thresh;
    int32_t y_begin = band_y0 < 1 ? 1 : band_y0;
    int32_t y_end = band_y1 > h - 1 ? h - 1 : band_y1;
    for (int32_t y = y_begin; y < y_end; y++)
        for (int32_t x = 1; x < w - 1; x++) {
            /* Sample 5 luminances */
            float lC =
                luminance(buf[(y * w + x) * 3], buf[(y * w + x) * 3 + 1], buf[(y * w + x) * 3 + 2]);
            float lN = luminance(buf[((y - 1) * w + x) * 3],
                                 buf[((y - 1) * w + x) * 3 + 1],
                                 buf[((y - 1) * w + x) * 3 + 2]);
            float lS = luminance(buf[((y + 1) * w + x) * 3],
                                 buf[((y + 1) * w + x) * 3 + 1],
                                 buf[((y + 1) * w + x) * 3 + 2]);
            float lE = luminance(buf[(y * w + x + 1) * 3],
                                 buf[(y * w + x + 1) * 3 + 1],
                                 buf[(y * w + x + 1) * 3 + 2]);
            float lW = luminance(buf[(y * w + x - 1) * 3],
                                 buf[(y * w + x - 1) * 3 + 1],
                                 buf[(y * w + x - 1) * 3 + 2]);

            float lmax = lC;
            if (lN > lmax)
                lmax = lN;
            if (lS > lmax)
                lmax = lS;
            if (lE > lmax)
                lmax = lE;
            if (lW > lmax)
                lmax = lW;
            float lmin = lC;
            if (lN < lmin)
                lmin = lN;
            if (lS < lmin)
                lmin = lS;
            if (lE < lmin)
                lmin = lE;
            if (lW < lmin)
                lmin = lW;

            float range = lmax - lmin;
            float thresh = lmax * edge_thresh;
            if (thresh < min_thresh)
                thresh = min_thresh;
            if (range < thresh)
                continue; /* not an edge */

            /* Simple 3x3 average for edge pixels */
            int32_t oi = (y * w + x) * 3;
            for (int c = 0; c < 3; c++) {
                float sum = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        sum += buf[((y + dy) * w + (x + dx)) * 3 + c];
                out[oi + c] = sum / 9.0f;
            }
        }
}

/// @brief Apply the simplified order-independent FXAA filter through a copy-out buffer.
/// @param fx PostFX chain providing row-band parallelism.
/// @param buf Packed RGB framebuffer modified in place after filtering.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param edge_thresh Relative luminance-range threshold for edge detection.
/// @param min_thresh Absolute lower bound for the edge threshold.
/// @param scratch Reusable allocation supplying the copy-out buffer.
static void apply_fxaa(rt_postfx3d *fx,
                       float *buf,
                       int32_t w,
                       int32_t h,
                       float edge_thresh,
                       float min_thresh,
                       postfx_scratch_t *scratch) {
    size_t bytes;
    postfx_fxaa_band_ctx ctx;
    if (!postfx_rgb_float_layout(w, h, NULL, NULL, &bytes))
        return;
    float *out = scratch ? postfx_scratch_reserve(scratch, 0, bytes, 0) : NULL;
    if (!out)
        return;
    memcpy(out, buf, bytes);
    ctx.buf = buf;
    ctx.out = out;
    ctx.w = w;
    ctx.h = h;
    ctx.edge_thresh = edge_thresh;
    ctx.min_thresh = min_thresh;
    /* Bands only READ buf and write disjoint rows of out, so the pass stays
     * order-independent exactly like the serial copy-out/copy-back version. */
    postfx_run_bands(fx, h, apply_fxaa_rows, &ctx);
    memcpy(buf, out, bytes);
}

/// @brief Apply colour grading in a single pass — contrast scales each channel around
/// mid-grey (0.5), brightness is a signed add, then saturation blends between grayscale
/// (the Rec. 709 luminance computed on the post-contrast values) and the full-chroma
/// result: `lum + (c - lum) * saturation`. All three terms are multiplicative around
/// 1.0 being neutral. Final clamp to `[0, 1]` prevents the 8-bit write back from
/// overflowing.
typedef struct {
    float *buf;
    int32_t w;
    float brightness;
    float contrast;
    float saturation;
} postfx_grade_band_ctx;

/// @brief Apply brightness, contrast, and saturation adjustments to one row band.
/// @param ctx_ptr Pointer to a fully initialized `postfx_grade_band_ctx`.
/// @param y0 Inclusive first row to process.
/// @param y1 Exclusive row limit.
static void apply_color_grade_rows(void *ctx_ptr, int32_t y0, int32_t y1) {
    const postfx_grade_band_ctx *bc = (const postfx_grade_band_ctx *)ctx_ptr;
    float *buf = bc->buf;
    float brightness = bc->brightness;
    float contrast = bc->contrast;
    float saturation = bc->saturation;
    size_t begin = (size_t)y0 * (size_t)bc->w;
    size_t end = (size_t)y1 * (size_t)bc->w;
    for (size_t i = begin; i < end; i++) {
        float *p = &buf[i * 3u];
        /* Brightness + contrast */
        p[0] = (p[0] - 0.5f) * contrast + 0.5f + brightness;
        p[1] = (p[1] - 0.5f) * contrast + 0.5f + brightness;
        p[2] = (p[2] - 0.5f) * contrast + 0.5f + brightness;
        /* Saturation */
        float lum = luminance(p[0], p[1], p[2]);
        p[0] = lum + (p[0] - lum) * saturation;
        p[1] = lum + (p[1] - lum) * saturation;
        p[2] = lum + (p[2] - lum) * saturation;
        /* Clamp */
        p[0] = clampf(p[0], 0.0f, 1.0f);
        p[1] = clampf(p[1], 0.0f, 1.0f);
        p[2] = clampf(p[2], 0.0f, 1.0f);
    }
}

/// @brief Apply color grading across the packed RGB framebuffer.
/// @param fx PostFX chain providing row-band parallelism.
/// @param buf Packed RGB framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param brightness Signed channel offset.
/// @param contrast Scale around middle gray.
/// @param saturation Chroma scale relative to Rec. 709 luminance.
static void apply_color_grade(rt_postfx3d *fx,
                              float *buf,
                              int32_t w,
                              int32_t h,
                              float brightness,
                              float contrast,
                              float saturation) {
    size_t count;
    postfx_grade_band_ctx ctx;
    if (!postfx_rgb_float_layout(w, h, &count, NULL, NULL))
        return;
    ctx.buf = buf;
    ctx.w = w;
    ctx.brightness = brightness;
    ctx.contrast = contrast;
    ctx.saturation = saturation;
    postfx_run_bands(fx, h, apply_color_grade_rows, &ctx);
}

/// @brief Apply a radial-falloff vignette. Normalises the pixel-to-centre distance by
/// the half-screen diagonal so the effect is resolution-independent, then multiplies
/// the colour by a `1 → 0` ramp that starts at normalized distance `radius` and reaches
/// full black at `radius + softness`. Pixels inside the radius are untouched. Alpha is
/// preserved — only the RGB channels are scaled.
typedef struct {
    float *buf;
    int32_t w;
    float cx;
    float cy;
    float maxdist;
    float radius;
    float softness;
} postfx_vignette_band_ctx;

/// @brief Apply the radial vignette multiplier to one row band.
/// @param ctx_ptr Pointer to a fully initialized `postfx_vignette_band_ctx`.
/// @param y0 Inclusive first row to process.
/// @param y1 Exclusive row limit.
static void apply_vignette_rows(void *ctx_ptr, int32_t y0, int32_t y1) {
    const postfx_vignette_band_ctx *bc = (const postfx_vignette_band_ctx *)ctx_ptr;
    float *buf = bc->buf;
    int32_t w = bc->w;
    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = 0; x < w; x++) {
            float dx = ((float)x - bc->cx) / bc->maxdist;
            float dy = ((float)y - bc->cy) / bc->maxdist;
            float dist = sqrtf(dx * dx + dy * dy);
            float vig = 1.0f;
            if (dist > bc->radius) {
                vig = 1.0f - clampf((dist - bc->radius) / (bc->softness + 1e-6f), 0.0f, 1.0f);
            }
            int32_t si = (y * w + x) * 3;
            buf[si] *= vig;
            buf[si + 1] *= vig;
            buf[si + 2] *= vig;
        }
}

/// @brief Apply a resolution-independent radial vignette to a packed RGB framebuffer.
/// @param fx PostFX chain providing row-band parallelism.
/// @param buf Packed RGB framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param radius Normalized distance at which attenuation begins.
/// @param softness Normalized width of the attenuation ramp.
static void apply_vignette(
    rt_postfx3d *fx, float *buf, int32_t w, int32_t h, float radius, float softness) {
    postfx_vignette_band_ctx ctx;
    if (!buf || w <= 0 || h <= 0)
        return;
    float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    float maxdist = sqrtf(cx * cx + cy * cy);
    if (!isfinite(maxdist) || maxdist <= 1e-6f)
        return;
    ctx.buf = buf;
    ctx.w = w;
    ctx.cx = cx;
    ctx.cy = cy;
    ctx.maxdist = maxdist;
    ctx.radius = radius;
    ctx.softness = softness;
    postfx_run_bands(fx, h, apply_vignette_rows, &ctx);
}

/*==========================================================================
 * Apply entire effect chain to a framebuffer
 *=========================================================================*/

/// @brief Check whether the chain contains an active tonemap pass that produced
///        display-referred (gamma-encoded) output.
/// @details Tonemap modes 1/2 always count. Mode 0 counts only when @p hdr_active is
///          non-zero: on a linear-HDR source an explicit mode-0 entry now applies the
///          exposure + gamma-out transform (Plan 05 gamma fix), so the final 8-bit
///          conversion must treat the buffer as already display-encoded. On LDR sources
///          mode 0 stays a passthrough identity and doesn't count.
/// @param fx PostFX chain to inspect.
/// @param hdr_active Non-zero when the source buffer contains linear HDR values.
/// @return 1 when an enabled entry performs the display transform, or 0 otherwise.
static int postfx_chain_has_tonemap(rt_postfx3d *fx, int hdr_active) {
    int32_t effect_count = postfx3d_safe_effect_count(fx);
    if (!fx || !fx->enabled || effect_count <= 0)
        return 0;
    for (int32_t i = 0; i < effect_count; i++) {
        const postfx_entry_t *e = &fx->effects[i];
        if (e->enabled && e->type == POSTFX_TONEMAP && (e->p.tonemap.mode != 0 || hdr_active))
            return 1;
    }
    return 0;
}

/*==========================================================================
 * CPU scene effects (software post-FX parity)
 * Depth-aware effects (SSAO/DOF/MotionBlur/SSR/TAA) run on the CPU using the
 * software rasterizer's NDC depth buffer plus the frame's view-projection.
 * They render the same phenomena as the GPU versions at documented lower
 * sample counts and are fully deterministic (fixed tap tables, no clock).
 *=========================================================================*/

/// @brief Convenience wrappers over postfx_scratch_reserve for float-count sizing.
/// @param scratch Scratch descriptor whose primary allocation may grow.
/// @param float_count Number of float elements required.
/// @return The reusable primary buffer, or `NULL` for invalid size or allocation failure.
static float *postfx_scratch_primary(postfx_scratch_t *scratch, size_t float_count) {
    if (!scratch || float_count == 0 || float_count > SIZE_MAX / sizeof(float))
        return NULL;
    return postfx_scratch_reserve(scratch, 0, float_count * sizeof(float), 0);
}

/// @brief Reserve the secondary reusable scratch allocation by float count.
/// @param scratch Scratch descriptor whose secondary allocation may grow.
/// @param float_count Number of float elements required.
/// @return The reusable secondary buffer, or `NULL` for invalid size or allocation failure.
static float *postfx_scratch_secondary(postfx_scratch_t *scratch, size_t float_count) {
    if (!scratch || float_count == 0 || float_count > SIZE_MAX / sizeof(float))
        return NULL;
    return postfx_scratch_reserve(scratch, 1, float_count * sizeof(float), 0);
}

/// @brief Scene inputs the depth-aware CPU effects consume. All optional: when
///   @ref has_depth or @ref has_inv is 0, those effects no-op for the frame.
typedef struct {
    const float *depth; /* NDC z in [-1,1]; FLT_MAX = empty (SW zbuf convention) */
    int32_t depth_w;
    int32_t depth_h;
    float vp[16];
    float inv_vp[16];
    float prev_vp[16];
    int8_t has_depth;
    int8_t has_inv;
    int8_t has_prev_vp;
    float cam_near;
    float cam_far;
    float cam_pos[3];
    /* Primary directional light's projected screen position (pixels). */
    float sun_screen[2];
    int8_t has_sun;
} postfx_scene_in_t;

/// @brief Adjugate 4x4 inverse (row-major float). Returns 0 on singular input.
/// @param m Sixteen-element source matrix in row-major order.
/// @param[out] out Sixteen-element destination receiving the inverse on success.
/// @return 1 when the matrix is invertible and finite, or 0 for singular input.
static int postfx_mat4_invert(const float *m, float *out) {
    float inv[16];
    if (!m || !out)
        return 0;
    for (int32_t i = 0; i < 16; i++) {
        if (!isfinite(m[i]))
            return 0;
    }
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!isfinite(det) || fabsf(det) < 1e-20f)
        return 0;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) {
        inv[i] *= det;
        if (!isfinite(inv[i]))
            return 0;
    }
    memcpy(out, inv, sizeof(inv));
    return 1;
}

/// @brief Fetch NDC depth at (x,y), or FLT_MAX when empty/out of range.
/// @param sc Scene-input descriptor containing the optional depth buffer.
/// @param x Horizontal pixel coordinate.
/// @param y Vertical pixel coordinate.
/// @return The stored NDC depth, or `FLT_MAX` when depth is unavailable or out of bounds.
static inline float postfx_depth_at(const postfx_scene_in_t *sc, int32_t x, int32_t y) {
    float depth;
    if (!sc || !sc->has_depth || x < 0 || y < 0 || x >= sc->depth_w || y >= sc->depth_h)
        return FLT_MAX;
    depth = sc->depth[(size_t)y * (size_t)sc->depth_w + (size_t)x];
    return isfinite(depth) && depth >= -1.0f && depth <= 1.0f ? depth : FLT_MAX;
}

/// @brief Convert NDC depth ([-1,1]) to camera-space linear depth.
/// @param sc Scene inputs supplying the near and far clip distances.
/// @param ndc_z Normalized-device-coordinate depth.
/// @return Positive camera-space depth, with safe clip-plane fallbacks for invalid metadata.
static inline float postfx_linear_depth(const postfx_scene_in_t *sc, float ndc_z) {
    float n;
    float f;
    if (!sc || !isfinite(ndc_z) || ndc_z < -1.0f || ndc_z > 1.0f)
        return 1.0f;
    n = isfinite(sc->cam_near) && sc->cam_near > 1e-5f ? sc->cam_near : 0.1f;
    f = isfinite(sc->cam_far) && sc->cam_far > n * 1.001f ? sc->cam_far : n * 1000.0f;
    float denom = f + n - ndc_z * (f - n);
    if (!isfinite(denom) || fabsf(denom) < 1e-9f)
        return f;
    float lin = (2.0f * f * n) / denom;
    return isfinite(lin) && lin > 0.0f ? lin : f;
}

/// @brief Reconstruct the render-space world position of pixel (x,y) at @p ndc_z.
/// @param sc Scene inputs supplying the inverse view-projection matrix.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param x Horizontal pixel coordinate.
/// @param y Vertical pixel coordinate.
/// @param ndc_z Normalized-device-coordinate depth at the pixel.
/// @param[out] out Three-element destination for the reconstructed world position.
/// @return 1 when a finite position was reconstructed, or 0 when inputs are invalid.
static inline int postfx_world_at(const postfx_scene_in_t *sc,
                                  int32_t w,
                                  int32_t h,
                                  float x,
                                  float y,
                                  float ndc_z,
                                  float out[3]) {
    if (!sc || !out || !sc->has_inv || w <= 0 || h <= 0 || w > VGFX3D_RENDERTARGET_DIM_MAX ||
        h > VGFX3D_RENDERTARGET_DIM_MAX || !isfinite(x) || !isfinite(y) || !isfinite(ndc_z) ||
        ndc_z < -1.0f || ndc_z > 1.0f)
        return 0;
    float nx = (x + 0.5f) / (float)w * 2.0f - 1.0f;
    float ny = 1.0f - (y + 0.5f) / (float)h * 2.0f;
    const float *m = sc->inv_vp;
    float cx = m[0] * nx + m[1] * ny + m[2] * ndc_z + m[3];
    float cy = m[4] * nx + m[5] * ny + m[6] * ndc_z + m[7];
    float cz = m[8] * nx + m[9] * ny + m[10] * ndc_z + m[11];
    float cw = m[12] * nx + m[13] * ny + m[14] * ndc_z + m[15];
    if (!isfinite(cw) || fabsf(cw) < 1e-9f)
        return 0;
    out[0] = cx / cw;
    out[1] = cy / cw;
    out[2] = cz / cw;
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

/// @brief Project a render-space point with @p vp; returns pixel coords + NDC z.
/// @param vp Sixteen-element view-projection matrix in row-major order.
/// @param world Three-element render-space position.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param[out] out_xyz Destination receiving pixel x, pixel y, and NDC depth.
/// @return 1 when the point projects in front of the camera with finite coordinates.
static inline int postfx_project(
    const float *vp, const float world[3], int32_t w, int32_t h, float out_xyz[3]) {
    if (!vp || !world || !out_xyz || w <= 0 || h <= 0 || w > VGFX3D_RENDERTARGET_DIM_MAX ||
        h > VGFX3D_RENDERTARGET_DIM_MAX)
        return 0;
    if (!isfinite(world[0]) || !isfinite(world[1]) || !isfinite(world[2]))
        return 0;
    float cx = vp[0] * world[0] + vp[1] * world[1] + vp[2] * world[2] + vp[3];
    float cy = vp[4] * world[0] + vp[5] * world[1] + vp[6] * world[2] + vp[7];
    float cz = vp[8] * world[0] + vp[9] * world[1] + vp[10] * world[2] + vp[11];
    float cw = vp[12] * world[0] + vp[13] * world[1] + vp[14] * world[2] + vp[15];
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(cz) || !isfinite(cw) || cw <= 1e-7f)
        return 0;
    out_xyz[0] = (cx / cw * 0.5f + 0.5f) * (float)w;
    out_xyz[1] = (1.0f - cy / cw) * 0.5f * (float)h;
    out_xyz[2] = cz / cw;
    return isfinite(out_xyz[0]) && isfinite(out_xyz[1]) && isfinite(out_xyz[2]);
}

/* Deterministic 12-point rotated-Poisson table (shares the shadow PCF family). */
static const float postfx_poisson12[12][2] = {{0.4824f, 0.3453f},
                                              {0.0799f, 0.6412f},
                                              {-0.2113f, -0.3051f},
                                              {0.4856f, -0.6382f},
                                              {-0.1973f, 0.2523f},
                                              {-0.8238f, 0.5511f},
                                              {-0.3336f, 0.8860f},
                                              {0.8357f, 0.0281f},
                                              {-0.9217f, -0.2893f},
                                              {-0.1843f, -0.7691f},
                                              {0.4499f, -0.1952f},
                                              {0.5347f, 0.7958f}};

/// @brief SSAO: hemisphere-style AO from depth comparisons, 3x3 blurred, multiplied in.
typedef struct {
    float *fbuf;
    const postfx_scene_in_t *sc;
    float *ao;
    float *ao_blur;
    int32_t w;
    int32_t h;
    float radius;
    float intensity;
    int32_t samples;
    size_t count;
} postfx_ssao_band_ctx;

/// @brief Compute raw depth-derived ambient occlusion for one row band.
/// @param ctx_ptr Pointer to a fully initialized `postfx_ssao_band_ctx`.
/// @param y0 Inclusive first row to process.
/// @param y1 Exclusive row limit.
static void apply_ssao_occlusion_rows(void *ctx_ptr, int32_t y0, int32_t y1) {
    const postfx_ssao_band_ctx *bc = (const postfx_ssao_band_ctx *)ctx_ptr;
    const postfx_scene_in_t *sc = bc->sc;
    float *ao = bc->ao;
    int32_t w = bc->w;
    int32_t h = bc->h;
    float radius = bc->radius;
    float intensity = bc->intensity;
    int32_t samples = bc->samples;
    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = 0; x < w; x++) {
            size_t idx = (size_t)y * (size_t)w + (size_t)x;
            float ndc = postfx_depth_at(sc, x, y);
            if (ndc > 1.0f) { /* empty */
                ao[idx] = 1.0f;
                continue;
            }
            float lin = postfx_linear_depth(sc, ndc);
            float radius_px = radius * (float)h * 0.5f / lin;
            if (radius_px < 1.0f)
                radius_px = 1.0f;
            if (radius_px > 24.0f)
                radius_px = 24.0f;
            float occl = 0.0f;
            for (int32_t t = 0; t < samples; t++) {
                int32_t sx = x + (int32_t)(postfx_poisson12[t][0] * radius_px);
                int32_t sy = y + (int32_t)(postfx_poisson12[t][1] * radius_px);
                float sn = postfx_depth_at(sc, sx, sy);
                if (sn > 1.0f)
                    continue;
                float slin = postfx_linear_depth(sc, sn);
                float diff = lin - slin; /* >0: sample closer to camera (occluder) */
                if (diff > 0.02f) {
                    float fall = 1.0f - diff / (radius > 0.0f ? radius : 1.0f);
                    if (fall > 0.0f)
                        occl += fall;
                }
            }
            float a = 1.0f - intensity * (occl / (float)samples);
            ao[idx] = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        }
    }
}

/// @brief Apply the SSAO 3-by-3 spatial blur to one row band.
/// @param ctx_ptr Pointer to a `postfx_ssao_band_ctx` containing raw and blurred AO buffers.
/// @param y0 Inclusive first row to process.
/// @param y1 Exclusive row limit.
static void apply_ssao_blur_rows(void *ctx_ptr, int32_t y0, int32_t y1) {
    const postfx_ssao_band_ctx *bc = (const postfx_ssao_band_ctx *)ctx_ptr;
    const float *ao = bc->ao;
    float *ao_blur = bc->ao_blur;
    int32_t w = bc->w;
    int32_t h = bc->h;
    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = 0; x < w; x++) {
            float sum = 0.0f;
            int32_t n = 0;
            for (int32_t dy = -1; dy <= 1; dy++) {
                for (int32_t dx = -1; dx <= 1; dx++) {
                    int32_t sx = x + dx;
                    int32_t sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                        continue;
                    sum += ao[(size_t)sy * (size_t)w + (size_t)sx];
                    n++;
                }
            }
            ao_blur[(size_t)y * (size_t)w + (size_t)x] = n > 0 ? sum / (float)n : 1.0f;
        }
    }
}

/// @brief Multiply packed framebuffer channels by blurred AO for one row band.
/// @param ctx_ptr Pointer to a `postfx_ssao_band_ctx` containing framebuffer and AO buffers.
/// @param y0 Inclusive first row to process.
/// @param y1 Exclusive row limit.
static void apply_ssao_modulate_rows(void *ctx_ptr, int32_t y0, int32_t y1) {
    const postfx_ssao_band_ctx *bc = (const postfx_ssao_band_ctx *)ctx_ptr;
    const float *ao_blur = bc->ao_blur;
    size_t begin = (size_t)y0 * (size_t)bc->w;
    size_t end = (size_t)y1 * (size_t)bc->w;
    for (size_t i = begin; i < end; i++) {
        float *pixel = &bc->fbuf[i * 3u];
        pixel[0] *= ao_blur[i];
        pixel[1] *= ao_blur[i];
        pixel[2] *= ao_blur[i];
    }
}

/// @brief Apply the complete CPU SSAO pipeline using depth comparisons and a spatial blur.
/// @param fx PostFX chain providing row-band parallelism.
/// @param fbuf Packed RGB float framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param sc Scene inputs supplying a matching NDC depth buffer.
/// @param radius World-relative occlusion sampling radius.
/// @param intensity Occlusion strength, normalized to the supported range.
/// @param samples Requested Poisson tap count, clamped between four and twelve.
/// @param scratch Reusable primary and secondary allocations for AO stages.
static void apply_ssao_cpu(rt_postfx3d *fx,
                           float *fbuf,
                           int32_t w,
                           int32_t h,
                           const postfx_scene_in_t *sc,
                           float radius,
                           float intensity,
                           int32_t samples,
                           postfx_scratch_t *scratch) {
    size_t count = (size_t)w * (size_t)h;
    float *ao = postfx_scratch_primary(scratch, count);
    float *ao_blur = postfx_scratch_secondary(scratch, count);
    postfx_ssao_band_ctx ctx;
    if (!ao || !ao_blur || !sc->has_depth || sc->depth_w != w || sc->depth_h != h)
        return;
    if (samples < 4)
        samples = 4;
    if (samples > 12)
        samples = 12;
    if (!(radius > 0.0f))
        radius = 0.5f;
    if (!(intensity > 0.0f))
        intensity = 1.0f;
    if (intensity > 4.0f)
        intensity = 4.0f;
    ctx.fbuf = fbuf;
    ctx.sc = sc;
    ctx.ao = ao;
    ctx.ao_blur = ao_blur;
    ctx.w = w;
    ctx.h = h;
    ctx.radius = radius;
    ctx.intensity = intensity;
    ctx.samples = samples;
    ctx.count = count;
    /* Three banded stages with a full barrier between each (postfx_run_bands
     * waits before returning): occlusion writes ao, the blur reads ao and
     * writes ao_blur, the modulate reads ao_blur and scales the framebuffer. */
    postfx_run_bands(fx, h, apply_ssao_occlusion_rows, &ctx);
    postfx_run_bands(fx, h, apply_ssao_blur_rows, &ctx);
    postfx_run_bands(fx, h, apply_ssao_modulate_rows, &ctx);
}

/// @brief DOF: circle-of-confusion gather blur; CoC from |linear - focus| / aperture.
/// @param fbuf Packed RGB float framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param sc Scene inputs supplying a matching NDC depth buffer.
/// @param focus_distance Camera-space distance of the focus plane.
/// @param aperture Distance scale controlling the circle of confusion.
/// @param max_blur User-facing maximum blur amount, converted to a bounded pixel radius.
/// @param scratch Reusable allocation used to preserve the unblurred framebuffer.
static void apply_dof_cpu(float *fbuf,
                          int32_t w,
                          int32_t h,
                          const postfx_scene_in_t *sc,
                          float focus_distance,
                          float aperture,
                          float max_blur,
                          postfx_scratch_t *scratch) {
    size_t count = (size_t)w * (size_t)h;
    float *copy = postfx_scratch_primary(scratch, count * 3u);
    if (!copy || !sc->has_depth || sc->depth_w != w || sc->depth_h != h)
        return;
    if (!(focus_distance > 0.0f))
        focus_distance = 10.0f;
    if (!(aperture > 0.0f))
        aperture = 5.0f;
    float max_radius = max_blur > 0.0f ? max_blur * 8.0f : 6.0f;
    if (max_radius > 12.0f)
        max_radius = 12.0f;
    memcpy(copy, fbuf, count * 3u * sizeof(float));
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            size_t idx = (size_t)y * (size_t)w + (size_t)x;
            float ndc = postfx_depth_at(sc, x, y);
            if (ndc > 1.0f)
                continue;
            float lin = postfx_linear_depth(sc, ndc);
            float coc = fabsf(lin - focus_distance) / aperture;
            if (coc > 1.0f)
                coc = 1.0f;
            float radius = coc * max_radius;
            if (radius < 0.75f)
                continue;
            float sr = 0.0f;
            float sg = 0.0f;
            float sb = 0.0f;
            float wsum = 0.0f;
            for (int32_t t = 0; t < 12; t++) {
                float px = postfx_poisson12[t][0];
                float py = postfx_poisson12[t][1];
                int32_t sx = x + (int32_t)(px * radius);
                int32_t sy = y + (int32_t)(py * radius);
                if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                    continue;
                size_t sidx = (size_t)sy * (size_t)w + (size_t)sx;
                /* Scatter-as-gather weighting: a tap only contributes to this pixel if its
                 * OWN circle of confusion could reach here. Without this, sharp in-focus
                 * geometry bleeds across silhouettes into blurred neighbors (halos) and
                 * blurred backgrounds contaminate in-focus edges. */
                float wt = 1.0f;
                float sndc = postfx_depth_at(sc, sx, sy);
                if (sndc <= 1.0f) {
                    float slin = postfx_linear_depth(sc, sndc);
                    float scoc = fabsf(slin - focus_distance) / aperture;
                    if (scoc > 1.0f)
                        scoc = 1.0f;
                    float reach = scoc * max_radius;
                    float tap_dist = radius * sqrtf(px * px + py * py);
                    if (tap_dist > 0.5f)
                        wt = reach >= tap_dist ? 1.0f : (reach + 0.5f) / (tap_dist + 0.5f);
                }
                const float *sample = &copy[sidx * 3u];
                sr += sample[0] * wt;
                sg += sample[1] * wt;
                sb += sample[2] * wt;
                wsum += wt;
            }
            if (wsum < 1e-4f)
                continue;
            float br = sr / wsum;
            float bg = sg / wsum;
            float bb = sb / wsum;
            float *pixel = &fbuf[idx * 3u];
            pixel[0] = pixel[0] * (1.0f - coc) + br * coc;
            pixel[1] = pixel[1] * (1.0f - coc) + bg * coc;
            pixel[2] = pixel[2] * (1.0f - coc) + bb * coc;
        }
    }
}

/// @brief Motion blur: camera-reprojection velocity, up to 6 samples along it.
///   Per-object velocity is a documented divergence from the GPU path.
/// @param fbuf Packed RGB float framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param sc Scene inputs supplying depth, inverse projection, and prior-frame projection.
/// @param strength Scale applied to the reconstructed screen-space velocity.
/// @param samples Requested gather count, clamped between two and six.
/// @param scratch Reusable allocation used to preserve the source framebuffer.
static void apply_motion_blur_cpu(float *fbuf,
                                  int32_t w,
                                  int32_t h,
                                  const postfx_scene_in_t *sc,
                                  float strength,
                                  int32_t samples,
                                  postfx_scratch_t *scratch) {
    size_t count = (size_t)w * (size_t)h;
    float *copy = postfx_scratch_primary(scratch, count * 3u);
    if (!copy || !sc->has_depth || !sc->has_inv || !sc->has_prev_vp || sc->depth_w != w ||
        sc->depth_h != h)
        return;
    if (samples < 2)
        samples = 2;
    if (samples > 6)
        samples = 6;
    if (!(strength > 0.0f))
        return;
    if (strength > 2.0f)
        strength = 2.0f;
    memcpy(copy, fbuf, count * 3u * sizeof(float));
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            size_t idx = (size_t)y * (size_t)w + (size_t)x;
            float ndc = postfx_depth_at(sc, x, y);
            if (ndc > 1.0f)
                continue;
            float world[3];
            float prev[3];
            if (!postfx_world_at(sc, w, h, (float)x, (float)y, ndc, world))
                continue;
            if (!postfx_project(sc->prev_vp, world, w, h, prev))
                continue;
            float vx = ((float)x + 0.5f - prev[0]) * strength;
            float vy = ((float)y + 0.5f - prev[1]) * strength;
            float vlen = sqrtf(vx * vx + vy * vy);
            if (vlen < 0.75f)
                continue;
            if (vlen > 32.0f) {
                vx *= 32.0f / vlen;
                vy *= 32.0f / vlen;
            }
            float sr = 0.0f;
            float sg = 0.0f;
            float sb = 0.0f;
            int32_t n = 0;
            for (int32_t t = 0; t < samples; t++) {
                float f = (float)t / (float)(samples - 1) - 0.5f;
                int32_t sx = x + (int32_t)(vx * f);
                int32_t sy = y + (int32_t)(vy * f);
                if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                    continue;
                size_t sidx = (size_t)sy * (size_t)w + (size_t)sx;
                const float *sample = &copy[sidx * 3u];
                sr += sample[0];
                sg += sample[1];
                sb += sample[2];
                n++;
            }
            if (n == 0)
                continue;
            float *pixel = &fbuf[idx * 3u];
            pixel[0] = sr / (float)n;
            pixel[1] = sg / (float)n;
            pixel[2] = sb / (float)n;
        }
    }
}

/// @brief SSR: coarse screen-space march along the depth-reconstructed reflection ray.
///   Misses keep the base color (no environment fallback on CPU — documented).
/// @param fbuf Packed RGB float framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param sc Scene inputs supplying depth, camera position, and projection matrices.
/// @param intensity Blend weight applied to successful reflection hits.
/// @param steps Requested ray-march iteration count, clamped between four and sixteen.
/// @param scratch Reusable allocation used to preserve the source framebuffer.
static void apply_ssr_cpu(float *fbuf,
                          int32_t w,
                          int32_t h,
                          const postfx_scene_in_t *sc,
                          float intensity,
                          int32_t steps,
                          postfx_scratch_t *scratch) {
    size_t count = (size_t)w * (size_t)h;
    float *copy = postfx_scratch_primary(scratch, count * 3u);
    if (!copy || !sc->has_depth || !sc->has_inv || sc->depth_w != w || sc->depth_h != h)
        return;
    if (steps < 4)
        steps = 4;
    if (steps > 16)
        steps = 16;
    if (!(intensity > 0.0f))
        return;
    if (intensity > 1.0f)
        intensity = 1.0f;
    memcpy(copy, fbuf, count * 3u * sizeof(float));
    for (int32_t y = 1; y + 1 < h; y++) {
        for (int32_t x = 1; x + 1 < w; x++) {
            size_t idx = (size_t)y * (size_t)w + (size_t)x;
            float ndc = postfx_depth_at(sc, x, y);
            if (ndc > 1.0f)
                continue;
            float pw[3];
            float pr[3];
            float pd[3];
            float nr = postfx_depth_at(sc, x + 1, y);
            float nd = postfx_depth_at(sc, x, y + 1);
            if (nr > 1.0f || nd > 1.0f)
                continue;
            if (!postfx_world_at(sc, w, h, (float)x, (float)y, ndc, pw) ||
                !postfx_world_at(sc, w, h, (float)(x + 1), (float)y, nr, pr) ||
                !postfx_world_at(sc, w, h, (float)x, (float)(y + 1), nd, pd))
                continue;
            float ex[3] = {pr[0] - pw[0], pr[1] - pw[1], pr[2] - pw[2]};
            float ey[3] = {pd[0] - pw[0], pd[1] - pw[1], pd[2] - pw[2]};
            float nrm[3] = {ex[1] * ey[2] - ex[2] * ey[1],
                            ex[2] * ey[0] - ex[0] * ey[2],
                            ex[0] * ey[1] - ex[1] * ey[0]};
            float nl = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            if (!isfinite(nl) || nl < 1e-9f)
                continue;
            nrm[0] /= nl;
            nrm[1] /= nl;
            nrm[2] /= nl;
            float vdir[3] = {
                pw[0] - sc->cam_pos[0], pw[1] - sc->cam_pos[1], pw[2] - sc->cam_pos[2]};
            float vl = sqrtf(vdir[0] * vdir[0] + vdir[1] * vdir[1] + vdir[2] * vdir[2]);
            if (!isfinite(vl) || vl < 1e-6f)
                continue;
            vdir[0] /= vl;
            vdir[1] /= vl;
            vdir[2] /= vl;
            if (nrm[0] * vdir[0] + nrm[1] * vdir[1] + nrm[2] * vdir[2] > 0.0f) {
                nrm[0] = -nrm[0];
                nrm[1] = -nrm[1];
                nrm[2] = -nrm[2];
            }
            float vdn = vdir[0] * nrm[0] + vdir[1] * nrm[1] + vdir[2] * nrm[2];
            float rdir[3] = {vdir[0] - 2.0f * vdn * nrm[0],
                             vdir[1] - 2.0f * vdn * nrm[1],
                             vdir[2] - 2.0f * vdn * nrm[2]};
            /* Only strongly reflective grazing setups matter at this quality tier;
             * skip rays pointing back at the camera. */
            float step_len = vl * 0.15f;
            if (step_len < 0.05f)
                step_len = 0.05f;
            float hit_r = 0.0f;
            float hit_g = 0.0f;
            float hit_b = 0.0f;
            int hit = 0;
            float pos[3] = {pw[0], pw[1], pw[2]};
            for (int32_t t = 0; t < steps; t++) {
                pos[0] += rdir[0] * step_len;
                pos[1] += rdir[1] * step_len;
                pos[2] += rdir[2] * step_len;
                float scr[3];
                if (!postfx_project(sc->vp, pos, w, h, scr))
                    break;
                int32_t sx = (int32_t)scr[0];
                int32_t sy = (int32_t)scr[1];
                if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                    break;
                float sndc = postfx_depth_at(sc, sx, sy);
                if (sndc > 1.0f)
                    continue;
                if (sndc < scr[2] - 1e-4f) {
                    float slin = postfx_linear_depth(sc, sndc);
                    float rlin = postfx_linear_depth(sc, scr[2]);
                    if (rlin - slin < step_len * 2.0f) {
                        size_t sidx = (size_t)sy * (size_t)w + (size_t)sx;
                        const float *sample = &copy[sidx * 3u];
                        hit_r = sample[0];
                        hit_g = sample[1];
                        hit_b = sample[2];
                        hit = 1;
                    }
                    break;
                }
            }
            if (!hit)
                continue;
            float k = intensity;
            float *pixel = &fbuf[idx * 3u];
            pixel[0] = pixel[0] * (1.0f - k) + hit_r * k;
            pixel[1] = pixel[1] * (1.0f - k) + hit_g * k;
            pixel[2] = pixel[2] * (1.0f - k) + hit_b * k;
        }
    }
}

/// @brief Auto-exposure: geometric-mean luminance -> smoothed EV multiplier.
/// @details Target exposure centers the scene's geometric mean at middle gray
///   (0.18), clamped to [min_ev, max_ev] in stops. Smoothing uses a fixed
///   deterministic 1/60 s step; downward adaptation runs 2.5x faster than
///   upward for the classic cinematic feel.
/// @param fx PostFX chain storing the temporally smoothed exposure state.
/// @param fbuf Packed RGB float framebuffer scaled in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param min_ev Minimum permitted exposure value in stops.
/// @param max_ev Maximum permitted exposure value in stops.
/// @param adapt_speed Temporal adaptation rate.
static void apply_auto_exposure_cpu(rt_postfx3d *fx,
                                    float *fbuf,
                                    int32_t w,
                                    int32_t h,
                                    float min_ev,
                                    float max_ev,
                                    float adapt_speed) {
    size_t count = (size_t)w * (size_t)h;
    if (!fx || count == 0)
        return;
    if (!(max_ev > min_ev)) {
        min_ev = -4.0f;
        max_ev = 4.0f;
    }
    if (!(adapt_speed > 0.0f))
        adapt_speed = 3.0f;
    double log_sum = 0.0;
    size_t stride = count > 4096 ? count / 4096 : 1; /* bounded sampling */
    size_t sampled = 0;
    for (size_t i = 0; i < count; i += stride) {
        const float *pixel = &fbuf[i * 3u];
        float lum = luminance(pixel[0], pixel[1], pixel[2]);
        if (!isfinite(lum) || lum < 0.0f)
            lum = 0.0f;
        log_sum += log((double)(lum > 1e-4f ? lum : 1e-4f));
        sampled++;
    }
    if (sampled == 0)
        return;
    float geo_mean = (float)exp(log_sum / (double)sampled);
    float target_ev = log2f(0.18f / (geo_mean > 1e-6f ? geo_mean : 1e-6f));
    if (target_ev < min_ev)
        target_ev = min_ev;
    if (target_ev > max_ev)
        target_ev = max_ev;
    if (!fx->auto_exposure_valid) {
        fx->auto_exposure_ev = target_ev;
        fx->auto_exposure_valid = 1;
    } else {
        float rate = adapt_speed * (1.0f / 60.0f);
        if (target_ev < fx->auto_exposure_ev)
            rate *= 2.5f; /* adapt down (bright flash) faster than up */
        if (rate > 1.0f)
            rate = 1.0f;
        fx->auto_exposure_ev += (target_ev - fx->auto_exposure_ev) * rate;
    }
    float mul = exp2f(fx->auto_exposure_ev);
    if (!isfinite(mul) || mul <= 0.0f)
        return;
    for (size_t i = 0; i < count * 3u; i++)
        fbuf[i] *= mul;
}

/// @brief 3D LUT color grade from a 256x16 strip (16 tiles of 16x16), trilinear.
/// @param fx PostFX chain owning the retained lookup-table Pixels object.
/// @param fbuf Packed RGB float framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param blend Lookup-table blend weight, clamped to the unit interval.
static void apply_color_lut_cpu(
    const rt_postfx3d *fx, float *fbuf, int32_t w, int32_t h, float blend) {
    size_t count = (size_t)w * (size_t)h;
    rt_pixels_impl *lut = fx ? rt_pixels_checked_impl_or_null(fx->lut_pixels) : NULL;
    if (!lut || !lut->data || lut->width != 256 || lut->height != 16 || count == 0)
        return;
    if (blend <= 0.0f)
        return;
    if (blend > 1.0f)
        blend = 1.0f;
    const uint32_t *ld = lut->data;
    for (size_t i = 0; i < count; i++) {
        float *pixel = &fbuf[i * 3u];
        float r = clampf(pixel[0], 0.0f, 1.0f);
        float g = clampf(pixel[1], 0.0f, 1.0f);
        float b = clampf(pixel[2], 0.0f, 1.0f);
        float rf = r * 15.0f;
        float gf = g * 15.0f;
        float bf = b * 15.0f;
        int32_t r0 = (int32_t)rf;
        int32_t g0 = (int32_t)gf;
        int32_t b0 = (int32_t)bf;
        int32_t r1 = r0 < 15 ? r0 + 1 : 15;
        int32_t g1 = g0 < 15 ? g0 + 1 : 15;
        int32_t b1 = b0 < 15 ? b0 + 1 : 15;
        float tr = rf - (float)r0;
        float tg = gf - (float)g0;
        float tb = bf - (float)b0;
        float acc[3] = {0.0f, 0.0f, 0.0f};
        for (int corner = 0; corner < 8; corner++) {
            int32_t rr = (corner & 1) ? r1 : r0;
            int32_t gg = (corner & 2) ? g1 : g0;
            int32_t bb = (corner & 4) ? b1 : b0;
            float wgt = ((corner & 1) ? tr : 1.0f - tr) * ((corner & 2) ? tg : 1.0f - tg) *
                        ((corner & 4) ? tb : 1.0f - tb);
            /* Strip layout: tile index = blue slice; within tile x = red, y = green. */
            uint32_t texel = ld[(size_t)gg * 256u + (size_t)(bb * 16 + rr)];
            acc[0] += wgt * (float)((texel >> 24) & 0xFFu) / 255.0f;
            acc[1] += wgt * (float)((texel >> 16) & 0xFFu) / 255.0f;
            acc[2] += wgt * (float)((texel >> 8) & 0xFFu) / 255.0f;
        }
        pixel[0] = pixel[0] * (1.0f - blend) + acc[0] * blend;
        pixel[1] = pixel[1] * (1.0f - blend) + acc[1] * blend;
        pixel[2] = pixel[2] * (1.0f - blend) + acc[2] * blend;
    }
}

/// @brief Screen-space sun shafts: radial accumulation of the sky mask toward the
///   primary directional light's projected position (classic god rays). Sky =
///   pixels with no depth (cleared FLT_MAX); occluders carve dark wedges for free.
///   No-ops when the sun projects far off-screen or behind the camera.
/// @param fbuf Packed RGB float framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param sc Scene inputs supplying depth and the projected sun position.
/// @param intensity Additive shaft strength.
/// @param decay Per-sample radial attenuation factor.
/// @param samples Requested radial sample count, clamped between eight and forty-eight.
/// @param scratch Reusable allocation used for the sky luminance mask.
static void apply_sun_shafts_cpu(float *fbuf,
                                 int32_t w,
                                 int32_t h,
                                 const postfx_scene_in_t *sc,
                                 float intensity,
                                 float decay,
                                 int32_t samples,
                                 postfx_scratch_t *scratch) {
    size_t count = (size_t)w * (size_t)h;
    float *mask = postfx_scratch_primary(scratch, count);
    if (!mask || !sc->has_depth || !sc->has_sun || sc->depth_w != w || sc->depth_h != h)
        return;
    if (!(intensity > 0.0f))
        return;
    if (intensity > 2.0f)
        intensity = 2.0f;
    if (!(decay > 0.0f) || decay >= 1.0f)
        decay = 0.92f;
    if (samples < 8)
        samples = 8;
    if (samples > 48)
        samples = 48;
    float sx = sc->sun_screen[0];
    float sy = sc->sun_screen[1];
    if (sx < -(float)w || sx > 2.0f * (float)w || sy < -(float)h || sy > 2.0f * (float)h)
        return;
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            size_t idx = (size_t)y * (size_t)w + (size_t)x;
            float ndc = postfx_depth_at(sc, x, y);
            /* Sky bright-pass: empty depth contributes its luminance. */
            if (ndc > 1.0f) {
                const float *pixel = &fbuf[idx * 3u];
                mask[idx] = luminance(pixel[0], pixel[1], pixel[2]);
            } else {
                mask[idx] = 0.0f;
            }
        }
    }
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            size_t idx = (size_t)y * (size_t)w + (size_t)x;
            float dx = (sx - (float)x) / (float)samples;
            float dy = (sy - (float)y) / (float)samples;
            float weight = 1.0f;
            float accum = 0.0f;
            float px = (float)x;
            float py = (float)y;
            for (int32_t t = 0; t < samples; t++) {
                px += dx;
                py += dy;
                int32_t ix = (int32_t)px;
                int32_t iy = (int32_t)py;
                if (ix < 0 || iy < 0 || ix >= w || iy >= h)
                    break;
                accum += mask[(size_t)iy * (size_t)w + (size_t)ix] * weight;
                weight *= decay;
            }
            float shaft = accum * intensity / (float)samples;
            if (shaft <= 0.0f)
                continue;
            float *pixel = &fbuf[idx * 3u];
            pixel[0] += shaft;
            pixel[1] += shaft * 0.95f;
            pixel[2] += shaft * 0.85f;
        }
    }
}

/// @brief TAA: reprojected history blend with 3x3 neighborhood clamp.
/// @details Deliberately unjittered: the software path is the deterministic reference
///   for golden-frame tests and VM/native parity, and sub-pixel projection jitter would
///   perturb every rasterized sample. On this path TAA therefore acts as reprojected
///   temporal smoothing (stabilizing motion, not resolving new edge coverage); the GPU
///   backends run the full Halton-jittered TAA resolve.
/// @param fx PostFX chain owning the persistent history buffer and its validity state.
/// @param fbuf Packed RGB float framebuffer modified in place and copied into history.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param sc Optional scene inputs used to reproject history into the current frame.
/// @param blend History contribution, clamped below one to retain current-frame influence.
/// @param scratch Reusable storage used to snapshot the current frame before blending.
static void apply_taa_cpu(rt_postfx3d *fx,
                          float *fbuf,
                          int32_t w,
                          int32_t h,
                          const postfx_scene_in_t *sc,
                          float blend,
                          postfx_scratch_t *scratch) {
    size_t count;
    size_t history_bytes;
    int reset_history;
    float *current_frame;
    if (!fx || !fbuf || !postfx_rgb_float_layout(w, h, &count, NULL, &history_bytes))
        return;
    if (blend < 0.0f)
        blend = 0.0f;
    if (blend > 0.95f)
        blend = 0.95f;
    reset_history = !fx->taa_history || fx->taa_w != w || fx->taa_h != h;
    fx->taa_history = postfx3d_reserve_float_storage(fx,
                                                     &fx->taa_history,
                                                     &fx->taa_history_capacity_bytes,
                                                     &fx->owned_taa_history,
                                                     &fx->taa_history_capacity_bytes,
                                                     &fx->taa_storage_cookie,
                                                     POSTFX3D_TAA_STORAGE_COOKIE,
                                                     history_bytes,
                                                     0);
    if (!fx->taa_history)
        return;
    if (reset_history) {
        fx->taa_w = w;
        fx->taa_h = h;
        fx->taa_valid = 0;
    }
    if (fx->taa_valid && sc && sc->has_depth && sc->has_inv && sc->has_prev_vp &&
        sc->depth_w == w && sc->depth_h == h) {
        current_frame = postfx_scratch_primary(scratch, count * 3u);
        if (!current_frame)
            return;
        memcpy(current_frame, fbuf, history_bytes);
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                size_t idx = (size_t)y * (size_t)w + (size_t)x;
                float ndc = postfx_depth_at(sc, x, y);
                float hx = (float)x;
                float hy = (float)y;
                if (ndc <= 1.0f) {
                    float world[3];
                    float prev[3];
                    if (postfx_world_at(sc, w, h, (float)x, (float)y, ndc, world) &&
                        postfx_project(sc->prev_vp, world, w, h, prev)) {
                        hx = prev[0] - 0.5f;
                        hy = prev[1] - 0.5f;
                    }
                }
                /* Floor-based rejection: the old truncation-toward-zero rounding accepted
                 * reprojections in (-1, 0) as column/row 0, smearing wrong history along
                 * the left/top border. */
                if (!(hx >= 0.0f) || !(hy >= 0.0f) || hx > (float)(w - 1) || hy > (float)(h - 1))
                    continue;
                int32_t ix0 = (int32_t)floorf(hx);
                int32_t iy0 = (int32_t)floorf(hy);
                int32_t ix1 = ix0 + 1 < w ? ix0 + 1 : w - 1;
                int32_t iy1 = iy0 + 1 < h ? iy0 + 1 : h - 1;
                float tx = hx - (float)ix0;
                float ty = hy - (float)iy0;
                size_t i00 = (size_t)iy0 * (size_t)w + (size_t)ix0;
                size_t i10 = (size_t)iy0 * (size_t)w + (size_t)ix1;
                size_t i01 = (size_t)iy1 * (size_t)w + (size_t)ix0;
                size_t i11 = (size_t)iy1 * (size_t)w + (size_t)ix1;
                float w00 = (1.0f - tx) * (1.0f - ty);
                float w10 = tx * (1.0f - ty);
                float w01 = (1.0f - tx) * ty;
                float w11 = tx * ty;
                /* 3x3 neighborhood clamp bounds ghosting. */
                float mn[3] = {1e30f, 1e30f, 1e30f};
                float mx[3] = {-1e30f, -1e30f, -1e30f};
                for (int32_t dy = -1; dy <= 1; dy++) {
                    for (int32_t dx = -1; dx <= 1; dx++) {
                        int32_t sx = x + dx;
                        int32_t sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                            continue;
                        size_t sidx = (size_t)sy * (size_t)w + (size_t)sx;
                        const float *current = &current_frame[sidx * 3u];
                        float cr = current[0];
                        float cg = current[1];
                        float cb = current[2];
                        if (cr < mn[0])
                            mn[0] = cr;
                        if (cg < mn[1])
                            mn[1] = cg;
                        if (cb < mn[2])
                            mn[2] = cb;
                        if (cr > mx[0])
                            mx[0] = cr;
                        if (cg > mx[1])
                            mx[1] = cg;
                        if (cb > mx[2])
                            mx[2] = cb;
                    }
                }
                /* Bilinear history sampling: point sampling cannot resolve the sub-pixel
                 * jitter TAA relies on — history snaps between texels and edges shimmer
                 * instead of converging. */
                const float *h00 = &fx->taa_history[i00 * 3u];
                const float *h10 = &fx->taa_history[i10 * 3u];
                const float *h01 = &fx->taa_history[i01 * 3u];
                const float *h11 = &fx->taa_history[i11 * 3u];
                float histr = h00[0] * w00 + h10[0] * w10 + h01[0] * w01 + h11[0] * w11;
                float histg = h00[1] * w00 + h10[1] * w10 + h01[1] * w01 + h11[1] * w11;
                float histb = h00[2] * w00 + h10[2] * w10 + h01[2] * w01 + h11[2] * w11;
                if (histr < mn[0])
                    histr = mn[0];
                if (histg < mn[1])
                    histg = mn[1];
                if (histb < mn[2])
                    histb = mn[2];
                if (histr > mx[0])
                    histr = mx[0];
                if (histg > mx[1])
                    histg = mx[1];
                if (histb > mx[2])
                    histb = mx[2];
                const float *source = &current_frame[idx * 3u];
                float *output = &fbuf[idx * 3u];
                output[0] = source[0] * (1.0f - blend) + histr * blend;
                output[1] = source[1] * (1.0f - blend) + histg * blend;
                output[2] = source[2] * (1.0f - blend) + histb * blend;
            }
        }
    }
    memcpy(fx->taa_history, fbuf, history_bytes);
    fx->taa_valid = 1;
}

/// @brief Run the HDR float-buffer stage of the postfx chain in authored order.
/// @details Color and scene-aware effects compose in linear float space before final LDR
///          conversion. Scene-aware entries use the validated depth/camera snapshot supplied by
///          the software canvas; an HDR target without that snapshot safely skips only those
///          entries. @p hdr_active selects the linear-HDR source behavior for explicit mode-0
///          tonemap entries (gamma-out; see `apply_tonemap`).
/// @param fx PostFX chain whose enabled entries are applied in insertion order.
/// @param fbuf Packed RGB float framebuffer modified in place.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param hdr_active Non-zero when @p fbuf contains linear HDR source values.
/// @param scene Optional scene inputs for depth- and history-aware CPU effects.
static void postfx_apply_float_effects(rt_postfx3d *fx,
                                       float *fbuf,
                                       int32_t w,
                                       int32_t h,
                                       int hdr_active,
                                       const postfx_scene_in_t *scene) {
    int32_t effect_count = postfx3d_safe_effect_count(fx);
    postfx_scratch_t scratch;
    if (!fx || !fx->enabled || effect_count == 0 || !fbuf)
        return;
    scratch.fx = fx;

    for (int32_t i = 0; i < effect_count; i++) {
        postfx_entry_t *e = &fx->effects[i];
        if (!e->enabled)
            continue;

        switch (e->type) {
            case POSTFX_BLOOM:
                apply_bloom(fbuf,
                            w,
                            h,
                            e->p.bloom.threshold,
                            e->p.bloom.intensity,
                            e->p.bloom.blur_passes,
                            &scratch);
                break;
            case POSTFX_TONEMAP:
                apply_tonemap(fx, fbuf, w, h, e->p.tonemap.mode, e->p.tonemap.exposure, hdr_active);
                break;
            case POSTFX_FXAA:
                apply_fxaa(
                    fx, fbuf, w, h, e->p.fxaa.edge_threshold, e->p.fxaa.min_threshold, &scratch);
                break;
            case POSTFX_COLOR_GRADE:
                apply_color_grade(fx,
                                  fbuf,
                                  w,
                                  h,
                                  e->p.color_grade.brightness,
                                  e->p.color_grade.contrast,
                                  e->p.color_grade.saturation);
                break;
            case POSTFX_VIGNETTE:
                apply_vignette(fx, fbuf, w, h, e->p.vignette.radius, e->p.vignette.softness);
                break;
            case POSTFX_SSAO:
                if (scene)
                    apply_ssao_cpu(fx,
                                   fbuf,
                                   w,
                                   h,
                                   scene,
                                   e->p.ssao.ao_radius,
                                   e->p.ssao.ao_intensity,
                                   e->p.ssao.ao_samples,
                                   &scratch);
                break;
            case POSTFX_DOF:
                if (scene)
                    apply_dof_cpu(fbuf,
                                  w,
                                  h,
                                  scene,
                                  e->p.dof.focus_distance,
                                  e->p.dof.aperture,
                                  e->p.dof.max_blur,
                                  &scratch);
                break;
            case POSTFX_MOTION_BLUR:
                if (scene)
                    apply_motion_blur_cpu(fbuf,
                                          w,
                                          h,
                                          scene,
                                          e->p.motion_blur.mb_intensity,
                                          e->p.motion_blur.mb_samples,
                                          &scratch);
                break;
            case POSTFX_TAA:
                if (scene)
                    apply_taa_cpu(fx, fbuf, w, h, scene, e->p.taa.blend, &scratch);
                break;
            case POSTFX_SSR:
                if (scene)
                    apply_ssr_cpu(fbuf, w, h, scene, e->p.ssr.intensity, e->p.ssr.steps, &scratch);
                break;
            case POSTFX_AUTO_EXPOSURE:
                apply_auto_exposure_cpu(fx,
                                        fbuf,
                                        w,
                                        h,
                                        e->p.auto_exposure.min_ev,
                                        e->p.auto_exposure.max_ev,
                                        e->p.auto_exposure.adapt_speed);
                break;
            case POSTFX_COLOR_LUT:
                apply_color_lut_cpu(fx, fbuf, w, h, e->p.color_lut.blend);
                break;
            case POSTFX_SUN_SHAFTS:
                if (scene)
                    apply_sun_shafts_cpu(fbuf,
                                         w,
                                         h,
                                         scene,
                                         e->p.sun_shafts.intensity,
                                         e->p.sun_shafts.decay,
                                         e->p.sun_shafts.samples,
                                         &scratch);
                break;
        }
    }
}

/// @brief Run the CPU-supported effect chain over a framebuffer.
/// @details Converts RGBA8 pixels to a temporary planar-RGB float buffer, applies each
///   enabled CPU effect in insertion order, then writes RGB back with alpha preserved.
///   SSAO, DOF, and motion blur require GPU scene depth/motion buffers and are rejected
///   before this helper is called.
/// @param fx PostFX chain whose enabled entries are applied.
/// @param pixels Mutable RGBA8 framebuffer.
/// @param w Framebuffer width in pixels.
/// @param h Framebuffer height in pixels.
/// @param stride Byte distance between consecutive framebuffer rows.
/// @param scene Optional scene inputs for depth- and history-aware effects.
static void postfx_apply(rt_postfx3d *fx,
                         uint8_t *pixels,
                         int32_t w,
                         int32_t h,
                         int32_t stride,
                         const postfx_scene_in_t *scene) {
    size_t pixel_count;
    size_t fbuf_bytes;
    int32_t effect_count = postfx3d_safe_effect_count(fx);
    if (!fx || !fx->enabled || effect_count == 0 || !pixels)
        return;
    if (stride < 0 || (size_t)stride < (size_t)w * 4u ||
        !postfx_rgb_float_layout(w, h, &pixel_count, NULL, &fbuf_bytes))
        return;
    (void)pixel_count;

    /* Convert framebuffer to retained float RGB scratch for processing.
     * ADR 0246: the 8-bit software scene buffer is SCENE-REFERRED LINEAR —
     * the rasterizer writes `clamp01(f) * 255` with no encode
     * (vgfx3d_backend_sw_raster.inc) and albedo textures are linearized at
     * sample time (vgfx3d_backend_sw_texture.inc), so there is NO storage
     * gamma to undo here. When the chain carries a real tone curve
     * (Reinhard/ACES) that curve consumes these linear values and performs
     * the pipeline's SINGLE display encode (its 1/2.2 gamma-out). A prior
     * change (144b4a2a7) decoded the input as display-referred sRGB first;
     * the decode cancelled the encode, ran the curve in display space, and
     * crushed everything under ACES' toe — the grey-wash regression
     * (ZB-20). Tonemap-less chains remain a byte-exact display-referred
     * passthrough. */
    float *fbuf = postfx3d_reserve_cpu_fbuf(fx, fbuf_bytes);
    if (!fbuf)
        return;

    int tone_active = postfx_chain_has_tonemap(fx, /*hdr_active=*/0);

    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            const uint8_t *src = &pixels[(size_t)y * (size_t)stride + (size_t)x * 4u];
            size_t di = ((size_t)y * (size_t)w + (size_t)x) * 3u;
            fbuf[di] = (float)src[0] / 255.0f;
            fbuf[di + 1] = (float)src[1] / 255.0f;
            fbuf[di + 2] = (float)src[2] / 255.0f;
        }

    /* tone_active selects the linear-chain contract inside the chain: the
     * tone curve consumes linear values and performs the single gamma-out. */
    postfx_apply_float_effects(fx, fbuf, w, h, /*hdr_active=*/tone_active, scene);

    /* Write back to framebuffer */
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            uint8_t *dst = &pixels[(size_t)y * (size_t)stride + (size_t)x * 4u];
            size_t si = ((size_t)y * (size_t)w + (size_t)x) * 3u;
            dst[0] = (uint8_t)(clampf(fbuf[si], 0.0f, 1.0f) * 255.0f);
            dst[1] = (uint8_t)(clampf(fbuf[si + 1], 0.0f, 1.0f) * 255.0f);
            dst[2] = (uint8_t)(clampf(fbuf[si + 2], 0.0f, 1.0f) * 255.0f);
            /* Preserve alpha */
        }
}

/// @brief Apply post-processing effects to an HDR render target's floating-point color buffer.
/// @details Copies the HDR RGBA16F buffer into a temporary packed RGB float buffer, applies
///          the enabled post-fx chain (tone mapping, bloom, color grading, etc.), then
///          writes the result back into the 8-bit UNORM color buffer for display/readback.
/// @param fx PostFX chain whose enabled entries are applied.
/// @param target Render target providing HDR source storage and mutable LDR output storage.
static void postfx_apply_hdr_target(rt_postfx3d *fx, vgfx3d_rendertarget_t *target) {
    size_t count;
    size_t fbuf_bytes;
    int tonemapped;
    float *fbuf;

    if (!fx || !target || !target->hdr_color_buf || !target->color_buf || target->width <= 0 ||
        target->height <= 0)
        return;
    if (target->stride < 0 || (size_t)target->stride < (size_t)target->width * 4u ||
        !postfx_rgb_float_layout(target->width, target->height, &count, NULL, &fbuf_bytes))
        return;
    fbuf = postfx3d_reserve_cpu_fbuf(fx, fbuf_bytes);
    if (!fbuf)
        return;
    for (size_t i = 0; i < count; i++) {
        fbuf[i * 3u + 0u] = sanitize_hdr_channel(target->hdr_color_buf[i * 4u + 0u]);
        fbuf[i * 3u + 1u] = sanitize_hdr_channel(target->hdr_color_buf[i * 4u + 1u]);
        fbuf[i * 3u + 2u] = sanitize_hdr_channel(target->hdr_color_buf[i * 4u + 2u]);
    }

    /* HDR RT chains skip the depth-aware effects for now (documented: the LDR
     * software path is the parity reference). */
    postfx_apply_float_effects(fx, fbuf, target->width, target->height, /*hdr_active=*/1, NULL);
    tonemapped = postfx_chain_has_tonemap(fx, /*hdr_active=*/1);
    for (int32_t y = 0; y < target->height; y++) {
        uint8_t *dst = target->color_buf + (size_t)y * (size_t)target->stride;
        for (int32_t x = 0; x < target->width; x++) {
            size_t i = (size_t)y * (size_t)target->width + (size_t)x;
            float r = sanitize_hdr_channel(fbuf[i * 3u + 0u]);
            float g = sanitize_hdr_channel(fbuf[i * 3u + 1u]);
            float b = sanitize_hdr_channel(fbuf[i * 3u + 2u]);
            target->hdr_color_buf[i * 4u + 0u] = r;
            target->hdr_color_buf[i * 4u + 1u] = g;
            target->hdr_color_buf[i * 4u + 2u] = b;
            dst[(size_t)x * 4u + 0u] =
                tonemapped ? (uint8_t)(clampf(r, 0.0f, 1.0f) * 255.0f) : vgfx3d_hdr_to_unorm8(r);
            dst[(size_t)x * 4u + 1u] =
                tonemapped ? (uint8_t)(clampf(g, 0.0f, 1.0f) * 255.0f) : vgfx3d_hdr_to_unorm8(g);
            dst[(size_t)x * 4u + 2u] =
                tonemapped ? (uint8_t)(clampf(b, 0.0f, 1.0f) * 255.0f) : vgfx3d_hdr_to_unorm8(b);
        }
    }
}

/*==========================================================================
 * PostFX3D lifecycle + API
 *=========================================================================*/

static void postfx3d_release_lut_owner(rt_postfx3d *fx) {
    if (!fx)
        return;
    if (postfx3d_lut_owner_is_valid(fx)) {
        void *lut = fx->owned_lut_pixels;
        if (rt_obj_release_check0(lut))
            rt_obj_free(lut);
    }
    fx->lut_pixels = NULL;
    fx->owned_lut_pixels = NULL;
    fx->lut_owner_cookie = 0;
}

static void postfx3d_reset_temporal_state(rt_postfx3d *fx) {
    if (!fx)
        return;
    fx->taa_valid = 0;
    fx->cpu_prev_vp_valid = 0;
    memset(fx->cpu_prev_vp, 0, sizeof(fx->cpu_prev_vp));
    fx->auto_exposure_ev = 0.0f;
    fx->auto_exposure_valid = 0;
}

/// @brief Release all heap buffers, retained objects, and the worker pool owned by PostFX3D.
/// @details Backend snapshots are rebuilt independently and are not owned by this object.
/// @param obj PostFX3D runtime object being finalized; ignored when `NULL`.
static void rt_postfx3d_finalize(void *obj) {
    rt_postfx3d *fx = (rt_postfx3d *)obj;
    if (!fx)
        return;
    if (postfx3d_float_storage_is_valid(fx,
                                        fx->owned_taa_history,
                                        fx->taa_history_capacity_bytes,
                                        fx->taa_storage_cookie,
                                        POSTFX3D_TAA_STORAGE_COOKIE))
        free(fx->owned_taa_history);
    fx->taa_history = NULL;
    fx->owned_taa_history = NULL;
    fx->taa_history_capacity_bytes = 0;
    fx->taa_storage_cookie = 0;
    if (postfx3d_float_storage_is_valid(fx,
                                        fx->owned_cpu_fbuf,
                                        fx->cpu_fbuf_capacity_bytes,
                                        fx->cpu_fbuf_storage_cookie,
                                        POSTFX3D_FBUF_STORAGE_COOKIE))
        free(fx->owned_cpu_fbuf);
    fx->cpu_fbuf = NULL;
    fx->cpu_fbuf_bytes = 0;
    fx->owned_cpu_fbuf = NULL;
    fx->cpu_fbuf_capacity_bytes = 0;
    fx->cpu_fbuf_storage_cookie = 0;
    if (postfx3d_float_storage_is_valid(fx,
                                        fx->owned_cpu_scratch_primary,
                                        fx->cpu_scratch_primary_capacity_bytes,
                                        fx->cpu_scratch_primary_storage_cookie,
                                        POSTFX3D_PRIMARY_STORAGE_COOKIE))
        free(fx->owned_cpu_scratch_primary);
    fx->cpu_scratch_primary = NULL;
    fx->cpu_scratch_primary_bytes = 0;
    fx->owned_cpu_scratch_primary = NULL;
    fx->cpu_scratch_primary_capacity_bytes = 0;
    fx->cpu_scratch_primary_storage_cookie = 0;
    if (postfx3d_float_storage_is_valid(fx,
                                        fx->owned_cpu_scratch_secondary,
                                        fx->cpu_scratch_secondary_capacity_bytes,
                                        fx->cpu_scratch_secondary_storage_cookie,
                                        POSTFX3D_SECONDARY_STORAGE_COOKIE))
        free(fx->owned_cpu_scratch_secondary);
    fx->cpu_scratch_secondary = NULL;
    fx->cpu_scratch_secondary_bytes = 0;
    fx->owned_cpu_scratch_secondary = NULL;
    fx->cpu_scratch_secondary_capacity_bytes = 0;
    fx->cpu_scratch_secondary_storage_cookie = 0;
    postfx3d_release_lut_owner(fx);
    if (postfx3d_worker_pool_owner_is_valid(fx)) {
        /* Same teardown as the software rasterizer's owned pool: shut down,
         * then drop the runtime object reference. */
        void *pool = fx->owned_worker_pool;
        fx->worker_pool = NULL;
        fx->owned_worker_pool = NULL;
        rt_threadpool_shutdown(pool);
        if (rt_obj_release_check0(pool))
            rt_obj_free(pool);
    }
    fx->worker_pool = NULL;
    fx->owned_worker_pool = NULL;
    fx->worker_count = 0;
    fx->worker_pool_owner_cookie = 0;
    if (postfx3d_effect_storage_is_valid(fx))
        free(fx->owned_effects);
    fx->effects = NULL;
    fx->effect_capacity = 0;
    fx->effect_count = 0;
    fx->owned_effects = NULL;
    fx->owned_effect_capacity = 0;
    fx->initialized_effect_count = 0;
    fx->effect_storage_cookie = 0;
}

/// @brief Create a new post-FX chain. Starts empty (no effects) and with the master
/// enable flag on — a fresh chain is inert until the caller appends effects via
/// `_add_bloom` / `_add_tonemap` / etc. The internal `effects` array is lazily
/// allocated on the first append, so a never-used chain pays zero extra memory
/// beyond the wrapper struct.
/// @return A new GC-managed PostFX3D object, or `NULL` after trapping on allocation failure.
void *rt_postfx3d_new(void) {
    rt_postfx3d *fx =
        (rt_postfx3d *)rt_obj_new_i64(RT_G3D_POSTFX3D_CLASS_ID, (int64_t)sizeof(rt_postfx3d));
    if (!fx) {
        rt_trap("PostFX3D.New: memory allocation failed");
        return NULL;
    }
    fx->vptr = NULL;
    fx->effects = NULL;
    fx->effect_count = 0;
    fx->effect_capacity = 0;
    fx->enabled = 1;
    fx->last_error[0] = '\0';
    rt_obj_set_finalizer(fx, rt_postfx3d_finalize);
    return fx;
}

/// @brief Append a Bloom effect: extracts pixels brighter than `threshold`, blurs `blur_passes`
/// times, then composites back at `intensity` strength. Common values: threshold 0.8–1.0,
/// intensity 0.3–1.5, passes 2–6.
/// @param obj PostFX3D chain receiving the effect.
/// @param threshold Non-negative bright-extract luminance threshold.
/// @param intensity Non-negative composite strength.
/// @param blur_passes Requested mip-chain depth, sanitized for internal storage.
void rt_postfx3d_add_bloom(void *obj, double threshold, double intensity, int64_t blur_passes) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_BLOOM;
    e->enabled = 1;
    e->p.bloom.threshold = sanitize_nonnegative_f32(threshold, 0.8f);
    e->p.bloom.intensity = sanitize_nonnegative_f32(intensity, 1.0f);
    e->p.bloom.blur_passes = clamp_i64_to_i32(blur_passes, 0, 32);
}

/// @brief Append a tone-map (HDR → LDR compression). `mode`: 0 = off, 1 = Reinhard,
/// 2 = ACES filmic. `exposure` scales the input before mapping (typical 0.5–2.0).
/// @param obj PostFX3D chain receiving the effect.
/// @param mode Tone-map selector clamped to the supported zero-through-two range.
/// @param exposure Non-negative linear exposure multiplier.
void rt_postfx3d_add_tonemap(void *obj, int64_t mode, double exposure) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_TONEMAP;
    e->enabled = 1;
    e->p.tonemap.mode = clamp_i64_to_i32(mode, 0, 2);
    e->p.tonemap.exposure = sanitize_nonnegative_f32(exposure, 1.0f);
}

/// @brief Append FXAA (Fast Approximate Anti-Aliasing). Smooths jagged edges by detecting
/// luminance discontinuities. Defaults to standard edge-threshold 0.166 / min-threshold 0.0833.
/// @param obj PostFX3D chain receiving the effect.
void rt_postfx3d_add_fxaa(void *obj) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_FXAA;
    e->enabled = 1;
    e->p.fxaa.edge_threshold = 0.166f;
    e->p.fxaa.min_threshold = 0.0833f;
}

/// @brief Append a color-grading effect.
/// @details `brightness` is a signed additive offset centered on 0.0. `contrast` scales around
/// mid-grey (0.5) and `saturation` interpolates from grayscale; both are multipliers centered
/// on 1.0.
/// @param obj PostFX3D chain receiving the effect.
/// @param brightness Signed channel offset clamped to the supported range.
/// @param contrast Scale around middle gray, where one is neutral.
/// @param saturation Chroma scale relative to luminance, where one is neutral.
void rt_postfx3d_add_color_grade(void *obj, double brightness, double contrast, double saturation) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_COLOR_GRADE;
    e->enabled = 1;
    e->p.color_grade.brightness = sanitize_range_f32(brightness, 0.0f, -1.0f, 1.0f);
    e->p.color_grade.contrast = sanitize_range_f32(contrast, 1.0f, 0.0f, 4.0f);
    e->p.color_grade.saturation = sanitize_range_f32(saturation, 1.0f, 0.0f, 4.0f);
}

/// @brief Append a vignette (radial darkening toward edges). `radius` is the bright region
/// before corner-normalized falloff starts; `softness` controls the falloff width.
/// @param obj PostFX3D chain receiving the effect.
/// @param radius Normalized unaffected radius, clamped to the unit interval.
/// @param softness Width of the radial attenuation ramp.
void rt_postfx3d_add_vignette(void *obj, double radius, double softness) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_VIGNETTE;
    e->enabled = 1;
    e->p.vignette.radius = sanitize_range_f32(radius, 0.7f, 0.0f, 1.0f);
    e->p.vignette.softness = sanitize_range_f32(softness, 0.3f, 0.001f, 1.0f);
}

/// @brief Master enable/disable for the entire effect chain. Disabled = framebuffer passes
/// through unchanged. Individual effects keep their own configuration.
/// @param obj PostFX3D chain to update.
/// @param enabled Non-zero to enable chain application, or zero to bypass it.
void rt_postfx3d_set_enabled(void *obj, int8_t enabled) {
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (fx)
        fx->enabled = enabled ? 1 : 0;
}

/// @brief Returns 1 if the post-FX chain is currently enabled.
/// @param obj Candidate PostFX3D chain.
/// @return 1 when the object is a valid enabled chain, or 0 otherwise.
int8_t rt_postfx3d_get_enabled(void *obj) {
    rt_postfx3d *fx = postfx3d_checked(obj);
    (void)postfx3d_repair_state(fx);
    return fx && fx->enabled ? 1 : 0;
}

/// @brief Drop every effect in the chain (fresh state). Master enable flag preserved.
/// @param obj PostFX3D chain whose effect entries are cleared.
void rt_postfx3d_clear(void *obj) {
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    (void)postfx3d_repair_state(fx);
    fx->effect_count = 0;
    fx->initialized_effect_count = 0;
    if (fx->owned_effects && fx->owned_effect_capacity > 0) {
        memset(
            fx->owned_effects, 0, (size_t)fx->owned_effect_capacity * sizeof(*fx->owned_effects));
    }
    postfx3d_release_lut_owner(fx);
    postfx3d_reset_temporal_state(fx);
}

/// @brief Number of effects currently in the chain.
/// @param obj Candidate PostFX3D chain.
/// @return The bounded number of stored effects, or zero for an invalid chain.
int64_t rt_postfx3d_get_effect_count(void *obj) {
    rt_postfx3d *fx = postfx3d_checked(obj);
    return postfx3d_safe_effect_count(fx);
}

/// @brief Kind discriminator of the effect at @p index in application order.
/// @details Values mirror `vgfx3d_postfx_effect_kind_t` (the private entry
///   types were defined to the same numbering), which is what the
///   `Zanna.Graphics3D.PostFXEffectKind` constants expose to callers.
/// @param obj Candidate PostFX3D chain.
/// @param index Zero-based position in the chain.
/// @return The kind value, or -1 for an invalid chain or out-of-range index.
int64_t rt_postfx3d_get_effect_kind(void *obj, int64_t index) {
    rt_postfx3d *fx = postfx3d_checked(obj);
    int32_t count = postfx3d_safe_effect_count(fx);
    if (!fx || index < 0 || index >= (int64_t)count)
        return -1;
    return (int64_t)fx->effects[(int32_t)index].type;
}

/// @brief Remove the effect at @p index, preserving the order of the rest.
/// @details Later entries shift down one slot and the vacated tail entry is
///   zeroed. Removing the final color-LUT entry releases its retained Pixels source.
/// @param obj Candidate PostFX3D chain.
/// @param index Zero-based position in the chain.
/// @return 1 when an entry was removed, or 0 for an invalid chain or index.
int8_t rt_postfx3d_remove_effect_at(void *obj, int64_t index) {
    rt_postfx3d *fx = postfx3d_checked(obj);
    int32_t count = postfx3d_safe_effect_count(fx);
    if (!fx || index < 0 || index >= (int64_t)count)
        return 0;
    int32_t at = (int32_t)index;
    if (at + 1 < count) {
        memmove(&fx->effects[at],
                &fx->effects[at + 1],
                (size_t)(count - at - 1) * sizeof(*fx->effects));
    }
    memset(&fx->effects[count - 1], 0, sizeof(*fx->effects));
    fx->effect_count = count - 1;
    fx->initialized_effect_count = count - 1;
    postfx3d_reset_temporal_state(fx);
    if (fx->owned_lut_pixels) {
        int has_lut = 0;
        for (int32_t i = 0; i < fx->initialized_effect_count; i++) {
            if (fx->owned_effects[i].type == POSTFX_COLOR_LUT) {
                has_lut = 1;
                break;
            }
        }
        if (!has_lut)
            postfx3d_release_lut_owner(fx);
    }
    return 1;
}

/* Zanna.Graphics3D.PostFXEffectKind constants — one registered getter per
 * effect kind, mirroring vgfx3d_postfx_effect_kind_t (see the enum note in
 * rt_postfx3d.h; the private postfx_type_t uses the same numbering). */
/// @brief PostFXEffectKind.Bloom constant.
int64_t rt_postfx3d_effect_kind_bloom(void) {
    return (int64_t)POSTFX_BLOOM;
}

/// @brief PostFXEffectKind.Tonemap constant.
int64_t rt_postfx3d_effect_kind_tonemap(void) {
    return (int64_t)POSTFX_TONEMAP;
}

/// @brief PostFXEffectKind.Fxaa constant.
int64_t rt_postfx3d_effect_kind_fxaa(void) {
    return (int64_t)POSTFX_FXAA;
}

/// @brief PostFXEffectKind.ColorGrade constant.
int64_t rt_postfx3d_effect_kind_color_grade(void) {
    return (int64_t)POSTFX_COLOR_GRADE;
}

/// @brief PostFXEffectKind.Vignette constant.
int64_t rt_postfx3d_effect_kind_vignette(void) {
    return (int64_t)POSTFX_VIGNETTE;
}

/// @brief PostFXEffectKind.Ssao constant.
int64_t rt_postfx3d_effect_kind_ssao(void) {
    return (int64_t)POSTFX_SSAO;
}

/// @brief PostFXEffectKind.Dof constant.
int64_t rt_postfx3d_effect_kind_dof(void) {
    return (int64_t)POSTFX_DOF;
}

/// @brief PostFXEffectKind.MotionBlur constant.
int64_t rt_postfx3d_effect_kind_motion_blur(void) {
    return (int64_t)POSTFX_MOTION_BLUR;
}

/// @brief PostFXEffectKind.Taa constant.
int64_t rt_postfx3d_effect_kind_taa(void) {
    return (int64_t)POSTFX_TAA;
}

/// @brief PostFXEffectKind.Ssr constant.
int64_t rt_postfx3d_effect_kind_ssr(void) {
    return (int64_t)POSTFX_SSR;
}

/// @brief PostFXEffectKind.AutoExposure constant.
int64_t rt_postfx3d_effect_kind_auto_exposure(void) {
    return (int64_t)POSTFX_AUTO_EXPOSURE;
}

/// @brief PostFXEffectKind.ColorLut constant.
int64_t rt_postfx3d_effect_kind_color_lut(void) {
    return (int64_t)POSTFX_COLOR_LUT;
}

/// @brief PostFXEffectKind.SunShafts constant.
int64_t rt_postfx3d_effect_kind_sun_shafts(void) {
    return (int64_t)POSTFX_SUN_SHAFTS;
}

/*==========================================================================
 * Canvas3D integration
 *=========================================================================*/

/// @brief Determine whether a canvas can execute backend GPU scene-aware effects.
/// @param c Canvas to inspect.
/// @return 1 for a window-backed canvas with a post-FX presentation hook, or 0 otherwise.
static int postfx3d_canvas_supports_gpu_scene_effects(const rt_canvas3d *c);

/// @brief Attach a PostFX3D chain to a Canvas3D. Pass NULL to detach. The canvas retains a
/// reference; the previous chain is released. Apply runs automatically on `_flip`.
/// @details Plan 09: chains carrying GPU-scene-buffer effects (SSAO/DOF/motion blur/TAA)
///   are validated against the canvas at bind time. On an unsupported canvas the bind is
///   refused and the reason is recorded on the chain (`PostFX3D.LastError`) instead of
///   trapping at first apply — supporting the capability-gated-fallback pattern games use.
/// @param canvas Canvas3D receiving or releasing the retained chain reference.
/// @param postfx PostFX3D chain to attach, or `NULL` to detach the current chain.
void rt_canvas3d_set_post_fx(void *canvas, void *postfx) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    if (!c)
        return;
    if (postfx && !postfx3d_checked(postfx))
        return;
    /* Depth-aware effects have CPU implementations on the software path, so
     * chains carrying them attach everywhere; GPU backends keep their native
     * versions. (The old bind-time refusal is gone — one chain runs on every
     * backend.) */
    if (postfx)
        postfx3d_set_last_error((rt_postfx3d *)postfx, NULL);
    if (c->postfx == postfx)
        return;
    if (postfx)
        rt_obj_retain_maybe(postfx);
    if (c->postfx && rt_obj_release_check0(c->postfx))
        rt_obj_free(c->postfx);
    c->postfx = postfx;
}

/// @brief `Canvas3D.PostFX` — borrowed retained post-effect chain (ADR 0233).
/// @param canvas Canvas3D receiver or supported stack-wrapper handle.
/// @return Borrowed PostFX3D handle, or NULL when no chain is attached.
void *rt_canvas3d_get_post_fx(void *canvas) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    return c ? c->postfx : NULL;
}

/// @brief Last recoverable PostFX configuration error ("" when none) — see SetPostFX.
/// @param obj Candidate PostFX3D chain.
/// @return A new runtime string containing the last error, or an empty string when none exists.
rt_string rt_postfx3d_get_last_error(void *obj) {
    rt_postfx3d *fx = postfx3d_checked(obj);
    (void)postfx3d_repair_state(fx);
    const char *msg = fx ? fx->last_error : "";
    return rt_string_from_bytes(msg, strlen(msg));
}

enum {
    POSTFX3D_QUALITY_FALLBACK_NONE = 0,
    POSTFX3D_QUALITY_FALLBACK_GPU_POSTFX_UNAVAILABLE = 1,
};

/// @brief Clamp an arbitrary quality value to the valid PERFORMANCE..CINEMATIC range.
/// @param quality Requested runtime quality value.
/// @return The corresponding supported quality constant after endpoint clamping.
static int32_t postfx3d_quality_level(int64_t quality) {
    if (quality < RT_GRAPHICS3D_QUALITY_PERFORMANCE)
        return RT_GRAPHICS3D_QUALITY_PERFORMANCE;
    if (quality > RT_GRAPHICS3D_QUALITY_CINEMATIC)
        return RT_GRAPHICS3D_QUALITY_CINEMATIC;
    return (int32_t)quality;
}

/// @brief True if the canvas can run GPU scene-buffer effects (SSAO/DoF/motion blur):
///   it must have a backend with a present_postfx hook and be rendering to the window
///   (no offscreen render target).
/// @param c Canvas to inspect.
/// @return 1 when GPU scene-aware post-processing is available, or 0 otherwise.
static int postfx3d_canvas_supports_gpu_scene_effects(const rt_canvas3d *c) {
    return c && c->backend && c->backend->present_postfx && c->render_target == NULL;
}

/// @brief Human-readable text for a quality-fallback reason code (empty when none).
/// @param reason Internal quality-fallback reason code.
/// @return Static null-terminated reason text, or an empty string for no known reason.
static const char *postfx3d_quality_fallback_reason_text(int32_t reason) {
    switch (reason) {
        case POSTFX3D_QUALITY_FALLBACK_GPU_POSTFX_UNAVAILABLE:
            return "gpu-postfx unavailable; using CPU-safe cinematic postfx";
        default:
            return "";
    }
}

/// @brief Populate a PostFX chain with the preset effect stack for @p quality
///   (performance/balanced/cinematic) and record the requested/active quality on the
///   canvas. Cinematic adds GPU scene effects only when supported, otherwise it records
///   a quality fallback so callers can report the downgrade.
/// @param fx PostFX chain to clear and populate with the preset entries.
/// @param canvas Optional canvas whose quality and fallback metadata is updated.
/// @param quality Normalized quality constant selecting the preset.
static void postfx3d_configure_quality_profile(rt_postfx3d *fx,
                                               rt_canvas3d *canvas,
                                               int32_t quality) {
    int gpu_scene_effects = postfx3d_canvas_supports_gpu_scene_effects(canvas);

    if (!fx)
        return;
    rt_postfx3d_clear(fx);

    if (canvas) {
        canvas->quality_requested = quality;
        canvas->quality_active = quality;
        canvas->quality_fallback = 0;
        canvas->quality_fallback_reason = POSTFX3D_QUALITY_FALLBACK_NONE;
    }

    switch (quality) {
        case RT_GRAPHICS3D_QUALITY_PERFORMANCE:
            rt_postfx3d_add_fxaa(fx);
            break;
        case RT_GRAPHICS3D_QUALITY_BALANCED:
            rt_postfx3d_add_bloom(fx, 0.88, 0.12, 1);
            rt_postfx3d_add_tonemap(fx, 1, 1.05);
            rt_postfx3d_add_fxaa(fx);
            rt_postfx3d_add_color_grade(fx, 0.0, 1.04, 1.03);
            break;
        case RT_GRAPHICS3D_QUALITY_CINEMATIC:
        default:
            rt_postfx3d_add_bloom(fx, 0.78, 0.22, 4);
            rt_postfx3d_add_tonemap(fx, 2, 1.10);
            /* TAA supersedes FXAA at cinematic when the backend can run temporal
             * resolve (GPU window postfx); FXAA remains the spatial fallback. */
            if (gpu_scene_effects)
                rt_postfx3d_add_taa(fx, 0.9);
            else
                rt_postfx3d_add_fxaa(fx);
            rt_postfx3d_add_color_grade(fx, 0.015, 1.08, 1.06);
            rt_postfx3d_add_vignette(fx, 0.96, 0.28);
            if (gpu_scene_effects) {
                rt_postfx3d_add_ssao(fx, 0.5, 0.65, 16);
                rt_postfx3d_add_ssr(fx, 0.5, 0.4);
                rt_postfx3d_add_dof(fx, 10.0, 0.08, 3.0);
                rt_postfx3d_add_motion_blur(fx, 0.12, 6);
            } else if (canvas) {
                canvas->quality_fallback = 1;
                canvas->quality_fallback_reason = POSTFX3D_QUALITY_FALLBACK_GPU_POSTFX_UNAVAILABLE;
            }
            break;
    }
}

/// @brief Build a backend-safe PostFX chain for a canvas and requested quality level.
/// @param canvas Canvas used for validation and quality-state tracking.
/// @param quality Requested quality value, clamped to the supported range.
/// @return A new configured PostFX3D chain, or `NULL` when the canvas is invalid or allocation
///   fails.
void *rt_postfx3d_new_quality(void *canvas, int64_t quality) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    if (!c)
        return NULL;
    rt_postfx3d *fx = (rt_postfx3d *)rt_postfx3d_new();
    if (!fx)
        return NULL;
    postfx3d_configure_quality_profile(fx, c, postfx3d_quality_level(quality));
    return fx;
}

/// @brief Apply a backend-safe quality profile to a Canvas3D.
/// @param canvas Canvas receiving the newly constructed retained PostFX chain.
/// @param quality Requested quality value, clamped to the supported range.
void rt_canvas3d_set_quality(void *canvas, int64_t quality) {
    void *fx = rt_postfx3d_new_quality(canvas, quality);
    if (!fx)
        return;
    rt_canvas3d_set_post_fx(canvas, fx);
    if (rt_obj_release_check0(fx))
        rt_obj_free(fx);
}

/// @brief Get the last quality level requested via SetQuality. See header.
/// @param canvas Canvas whose requested quality is queried.
/// @return The normalized requested quality, or performance for an invalid canvas.
int64_t rt_canvas3d_get_quality_requested(void *canvas) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    return c ? postfx3d_quality_level(c->quality_requested) : RT_GRAPHICS3D_QUALITY_PERFORMANCE;
}

/// @brief Get the quality level actually active after capability fallback. See header.
/// @param canvas Canvas whose active quality is queried.
/// @return The normalized active quality, or performance for an invalid canvas.
int64_t rt_canvas3d_get_quality_active(void *canvas) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    return c ? postfx3d_quality_level(c->quality_active) : RT_GRAPHICS3D_QUALITY_PERFORMANCE;
}

/// @brief True if the last quality application was degraded for backend safety. See header.
/// @param canvas Canvas whose fallback state is queried.
/// @return 1 when the last profile was degraded, or 0 otherwise.
int8_t rt_canvas3d_get_quality_fallback(void *canvas) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    return c && c->quality_fallback ? 1 : 0;
}

/// @brief Get a human-readable reason for the last quality fallback (empty if none). See header.
/// @param canvas Canvas whose fallback reason is queried.
/// @return A new runtime string containing the reason, or an empty string when no fallback exists.
rt_string rt_canvas3d_get_quality_fallback_reason(void *canvas) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    const char *reason = c ? postfx3d_quality_fallback_reason_text(c->quality_fallback_reason) : "";
    return rt_string_from_bytes(reason, strlen(reason));
}

/// @brief Apply the canvas's attached post-FX chain in place to its framebuffer pixels. Called
/// from `rt_canvas3d_flip` after rendering completes. No-op if no chain attached / disabled /
/// the framebuffer is unmapped (e.g., GPU-only window).
/// @param canvas Canvas supplying the attached chain, framebuffer, and current scene inputs.
void rt_postfx3d_apply_to_canvas(void *canvas) {
    uint8_t *pixels = NULL;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    if (!c)
        return;
    rt_postfx3d *fx = postfx3d_checked(c->postfx);
    if (!fx || !fx->enabled || postfx3d_safe_effect_count(fx) == 0)
        return;
    /* Depth-aware effects (SSAO/DOF/MotionBlur/SSR/TAA) run on the CPU too:
     * scene inputs come from the software depth buffer (or the render target's)
     * plus the frame's cached view-projection. */
    const float *scene_depth = NULL;
    int32_t scene_dw = 0;
    int32_t scene_dh = 0;
    if (c->render_target) {
        if (!vgfx3d_rendertarget_ensure_color(c->render_target))
            return;
        if (!vgfx3d_rendertarget_sync_color_if_needed(c->render_target))
            return;
        if (vgfx3d_rendertarget_is_hdr(c->render_target) && c->render_target->hdr_color_valid &&
            c->render_target->hdr_color_buf) {
            postfx_apply_hdr_target(fx, c->render_target);
            return;
        }
        pixels = c->render_target->color_buf;
        width = c->render_target->width;
        height = c->render_target->height;
        stride = c->render_target->stride;
        if (c->render_target->depth_buf) {
            scene_depth = c->render_target->depth_buf;
            scene_dw = c->render_target->width;
            scene_dh = c->render_target->height;
        }
    } else {
        vgfx_framebuffer_t fb;
        if (c->backend && c->backend != &vgfx3d_software_backend &&
            (!c->backend->name || strcmp(c->backend->name, "software") != 0))
            return;
        if (!c->gfx_win)
            return;
        if (!vgfx_get_framebuffer(c->gfx_win, &fb) || !fb.pixels)
            return;
        pixels = fb.pixels;
        width = fb.width;
        height = fb.height;
        stride = fb.stride;
        scene_depth = vgfx3d_sw_get_zbuf(c->backend_ctx, &scene_dw, &scene_dh);
    }

    if (!pixels || width <= 0 || height <= 0 || width > VGFX3D_RENDERTARGET_DIM_MAX ||
        height > VGFX3D_RENDERTARGET_DIM_MAX || stride <= 0)
        return;
    {
        postfx_scene_in_t scene;
        memset(&scene, 0, sizeof(scene));
        if (scene_depth && scene_dw == width && scene_dh == height) {
            scene.depth = scene_depth;
            scene.depth_w = scene_dw;
            scene.depth_h = scene_dh;
            scene.has_depth = 1;
        }
        memcpy(scene.vp, c->cached_vp, sizeof(scene.vp));
        scene.has_inv = postfx_mat4_invert(scene.vp, scene.inv_vp) ? 1 : 0;
        if (fx->cpu_prev_vp_valid) {
            memcpy(scene.prev_vp, fx->cpu_prev_vp, sizeof(scene.prev_vp));
            scene.has_prev_vp = 1;
        }
        scene.cam_near =
            isfinite(c->cached_cam_near) && c->cached_cam_near > 1e-5f ? c->cached_cam_near : 0.1f;
        scene.cam_far = isfinite(c->cached_cam_far) && c->cached_cam_far > scene.cam_near * 1.001f
                            ? c->cached_cam_far
                            : scene.cam_near * 1000.0f;
        for (int32_t lane = 0; lane < 3; lane++) {
            float value = c->cached_render_cam_pos[lane];
            scene.cam_pos[lane] =
                isfinite(value) && fabsf(value) <= POSTFX3D_FOCUS_MAX ? value : 0.0f;
        }
        /* Project the primary directional light for the sun-shafts pass. */
        for (int li = 0; scene.has_inv && li < VGFX3D_MAX_LIGHTS; li++) {
            const rt_light3d *l = c->lights[li];
            if (!l || !l->enabled || l->type != 0)
                continue;
            float dir[3] = {(float)l->direction[0], (float)l->direction[1], (float)l->direction[2]};
            float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (!isfinite(len) || len < 1e-6f)
                break;
            float sun_world[3] = {scene.cam_pos[0] - dir[0] / len * 10000.0f,
                                  scene.cam_pos[1] - dir[1] / len * 10000.0f,
                                  scene.cam_pos[2] - dir[2] / len * 10000.0f};
            float proj[3];
            if (postfx_project(scene.vp, sun_world, width, height, proj)) {
                scene.sun_screen[0] = proj[0];
                scene.sun_screen[1] = proj[1];
                scene.has_sun = 1;
            }
            break;
        }
        postfx_apply(fx, pixels, width, height, stride, &scene);
        if (scene.has_inv) {
            memcpy(fx->cpu_prev_vp, scene.vp, sizeof(fx->cpu_prev_vp));
            fx->cpu_prev_vp_valid = 1;
        } else {
            memset(fx->cpu_prev_vp, 0, sizeof(fx->cpu_prev_vp));
            fx->cpu_prev_vp_valid = 0;
            fx->taa_valid = 0;
        }
    }
}

/// @brief Append eye-adaptation auto-exposure. Target exposure centers the scene's
/// geometric-mean luminance at middle gray, clamped to [minEv, maxEv] stops and
/// smoothed by adaptSpeed (downward adaptation runs 2.5x faster).
/// @param obj PostFX3D chain receiving the effect.
/// @param min_ev Minimum exposure value in stops.
/// @param max_ev Maximum exposure value in stops.
/// @param adapt_speed Positive temporal adaptation rate.
void rt_postfx3d_add_auto_exposure(void *obj, double min_ev, double max_ev, double adapt_speed) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_AUTO_EXPOSURE;
    e->enabled = 1;
    e->p.auto_exposure.min_ev = sanitize_range_f32(min_ev, -4.0f, POSTFX3D_EV_MIN, POSTFX3D_EV_MAX);
    e->p.auto_exposure.max_ev = sanitize_range_f32(max_ev, 4.0f, POSTFX3D_EV_MIN, POSTFX3D_EV_MAX);
    e->p.auto_exposure.adapt_speed =
        sanitize_range_f32(adapt_speed, 3.0f, 0.001f, POSTFX3D_PARAM_MAX);
    (void)postfx3d_sanitize_effect_entry(e);
    fx->auto_exposure_valid = 0;
}

/// @brief Append a 3D LUT color grade. @p lut_pixels is a 256x16 strip (16 tiles of
/// 16x16: x = red, y = green, tile = blue), trilinear sampled, blended 0..1 with the
/// ungraded color. The chain retains the Pixels.
/// @param obj PostFX3D chain receiving the effect and retaining the LUT.
/// @param lut_pixels Pixels object containing the required 256-by-16 LUT strip.
/// @param blend LUT contribution clamped to the unit interval.
void rt_postfx3d_add_color_lut(void *obj, void *lut_pixels, double blend) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    rt_pixels_impl *lut = rt_pixels_checked_impl_or_null(lut_pixels);
    if (!fx)
        return;
    if (!lut || lut->width != 256 || lut->height != 16) {
        rt_trap("PostFX3D.AddColorLUT: LUT must be a 256x16 Pixels strip");
        return;
    }
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_COLOR_LUT;
    e->enabled = 1;
    if (!isfinite(blend))
        blend = 1.0;
    if (blend < 0.0)
        blend = 0.0;
    if (blend > 1.0)
        blend = 1.0;
    e->p.color_lut.blend = (float)blend;
    rt_obj_retain_maybe(lut_pixels);
    postfx3d_release_lut_owner(fx);
    fx->owned_lut_pixels = lut_pixels;
    fx->lut_owner_cookie =
        postfx3d_storage_cookie_value(fx, lut_pixels, 1u, POSTFX3D_LUT_OWNER_COOKIE);
    fx->lut_pixels = lut_pixels;
}

/// @brief Append screen-space sun shafts: radial sky-mask accumulation toward the
/// primary directional light's screen position; auto-fades when the sun is
/// off-screen or behind the camera.
/// @param obj PostFX3D chain receiving the effect.
/// @param intensity Positive additive shaft strength.
/// @param decay Per-sample attenuation in the open unit interval.
/// @param samples Requested radial sample count, clamped between eight and forty-eight.
void rt_postfx3d_add_sun_shafts(void *obj, double intensity, double decay, int64_t samples) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_SUN_SHAFTS;
    e->enabled = 1;
    e->p.sun_shafts.intensity = sanitize_nonnegative_f32(intensity, 0.6f);
    e->p.sun_shafts.decay = sanitize_range_f32(decay, 0.92f, 0.001f, 0.999f);
    e->p.sun_shafts.samples = clamp_i64_to_i32(samples, 8, 48);
}

/// @brief Build the identity 256x16 LUT strip. Screenshot it composited over a
/// reference frame, grade the screenshot in any editor, crop the strip back out,
/// and feed it to AddColorLUT — that is the whole grading workflow.
/// @return A new 256-by-16 Pixels object encoding the identity RGB lookup table.
void *rt_postfx3d_make_identity_lut(void) {
    void *pixels = rt_pixels_new(256, 16);
    rt_pixels_impl *pv = rt_pixels_checked_impl_or_null(pixels);
    if (!pv || !pv->data)
        return pixels;
    for (int32_t g = 0; g < 16; g++) {
        for (int32_t b = 0; b < 16; b++) {
            for (int32_t r = 0; r < 16; r++) {
                uint32_t rr = (uint32_t)(r * 255 / 15);
                uint32_t gg = (uint32_t)(g * 255 / 15);
                uint32_t bb = (uint32_t)(b * 255 / 15);
                pv->data[(size_t)g * 256u + (size_t)(b * 16 + r)] =
                    (rr << 24) | (gg << 16) | (bb << 8) | 0xFFu;
            }
        }
    }
    pixels_touch(pv);
    return pixels;
}

/// @brief Append SSAO (Screen-Space Ambient Occlusion). `radius` (world units) is the sample
/// neighborhood; `intensity` is the darkening multiplier; `samples` controls quality (8..64).
/// Higher `samples` = better quality but slower.
/// @param obj PostFX3D chain receiving the effect.
/// @param radius Non-negative world-space sampling radius.
/// @param intensity Non-negative occlusion strength.
/// @param samples Requested sample count, clamped to the backend descriptor range.
void rt_postfx3d_add_ssao(void *obj, double radius, double intensity, int64_t samples) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_SSAO;
    e->enabled = 1;
    e->p.ssao.ao_radius = sanitize_range_f32(radius, 0.5f, 0.0f, POSTFX3D_RADIUS_MAX);
    e->p.ssao.ao_intensity = sanitize_nonnegative_f32(intensity, 1.0f);
    e->p.ssao.ao_samples = clamp_i64_to_i32(samples, 1, 128);
}

/// @brief Append depth-of-field. `focus_distance` (world units) is the sharply-focused depth;
/// `aperture` controls how quickly things outside that depth blur; `max_blur` caps the blur
/// kernel radius in pixels. Larger aperture = shallower DOF.
/// @param obj PostFX3D chain receiving the effect.
/// @param focus_distance Non-negative camera-space focus distance.
/// @param aperture Non-negative blur growth parameter.
/// @param max_blur Maximum blur amount stored in the effect descriptor.
void rt_postfx3d_add_dof(void *obj, double focus_distance, double aperture, double max_blur) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_DOF;
    e->enabled = 1;
    e->p.dof.focus_distance = sanitize_range_f32(focus_distance, 10.0f, 0.0f, POSTFX3D_FOCUS_MAX);
    e->p.dof.aperture = sanitize_nonnegative_f32(aperture, 0.0f);
    e->p.dof.max_blur = sanitize_range_f32(max_blur, 8.0f, 0.0f, 128.0f);
}

/// @brief Live focus-distance mutation for an existing DOF effect (focus pulls).
/// @details Walks the chain for the first enabled DOF entry and rewrites its
///          focus parameter — no topology change, no reallocation. Returns 0
///          (recoverable) when the chain has no DOF effect.
/// @param obj PostFX3D chain containing the effect to update.
/// @param distance New non-negative focus distance.
/// @return 1 when an enabled DOF entry was updated, or 0 when none was found.
int8_t rt_postfx3d_set_dof_focus(void *obj, double distance) {
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return 0;
    int32_t count = postfx3d_safe_effect_count(fx);
    for (int32_t i = 0; i < count; ++i) {
        postfx_entry_t *e = &fx->effects[i];
        if (e->type == POSTFX_DOF && e->enabled) {
            e->p.dof.focus_distance =
                sanitize_range_f32(distance, e->p.dof.focus_distance, 0.0f, POSTFX3D_FOCUS_MAX);
            return 1;
        }
    }
    return 0;
}

/// @brief Append per-pixel motion blur. `intensity` controls the blur length; `samples` is
/// the per-pixel sample count along the motion vector (more = smoother but slower).
/// @param obj PostFX3D chain receiving the effect.
/// @param intensity Motion-vector length scale clamped to the unit interval.
/// @param samples Requested sample count, clamped to the backend descriptor range.
void rt_postfx3d_add_motion_blur(void *obj, double intensity, int64_t samples) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_MOTION_BLUR;
    e->enabled = 1;
    e->p.motion_blur.mb_intensity = sanitize_range_f32(intensity, 0.0f, 0.0f, 1.0f);
    e->p.motion_blur.mb_samples = clamp_i64_to_i32(samples, 1, 64);
}

/// @brief Append a TAA (temporal antialiasing) resolve pass. `blend` is the history weight:
/// each frame blends `blend` of the reprojected, neighborhood-clamped history with
/// `1 - blend` of the current frame. Typical 0.85–0.95; clamped to [0.5, 0.98]. GPU
/// backends can use motion and depth buffers, while the deterministic CPU path performs
/// unjittered view-projection reprojection.
/// @param obj PostFX3D chain receiving the effect.
/// @param blend History weight clamped to the supported temporal range.
void rt_postfx3d_add_taa(void *obj, double blend) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_TAA;
    e->enabled = 1;
    e->p.taa.blend = sanitize_range_f32(blend, 0.9f, 0.5f, 0.98f);
}

/// @brief Append screen-space reflections (Plan 10). Ray-marches the opaque scene in
/// screen space for materials flagged `SsrEnabled`, compositing hits over the env-map
/// term with screen-edge fade; misses keep the env-map reflection. The CPU path uses a
/// lower-step deterministic depth reconstruction and keeps the base color on misses.
/// @param obj PostFX3D chain receiving the effect.
/// @param intensity Reflection-hit blend weight clamped to the unit interval.
/// @param max_roughness Maximum material roughness eligible for backend SSR.
void rt_postfx3d_add_ssr(void *obj, double intensity, double max_roughness) {
    postfx_entry_t *e;
    rt_postfx3d *fx = postfx3d_checked(obj);
    if (!fx)
        return;
    e = postfx_append_entry(fx);
    if (!e)
        return;
    e->type = POSTFX_SSR;
    e->enabled = 1;
    e->p.ssr.intensity = sanitize_range_f32(intensity, 0.5f, 0.0f, 1.0f);
    e->p.ssr.max_roughness = sanitize_range_f32(max_roughness, 0.4f, 0.0f, 1.0f);
    e->p.ssr.steps = 24;
}

/*==========================================================================
 * Backend-facing snapshot export (MTL-11)
 *=========================================================================*/

/// @brief Wipe a backend-facing postfx chain to "disabled / empty" state *without* freeing
/// its effect-descriptor storage. Used by the GPU frame loop to reuse a preallocated chain
/// buffer across frames — zeroing the descriptors keeps the allocation warm while discarding
/// last frame's contents. Pair with `_free` to release the buffer when the chain is retired.
/// @param chain Backend-facing chain to reset; ignored when `NULL`.
void vgfx3d_postfx_chain_reset(vgfx3d_postfx_chain_t *chain) {
    if (!chain)
        return;
    vgfx3d_postfx_chain_repair_storage(chain);
    chain->enabled = 0;
    chain->effect_count = 0;
    if (chain->effects && chain->effect_capacity > 0) {
        memset(chain->effects, 0, (size_t)chain->effect_capacity * sizeof(*chain->effects));
    }
}

/// @brief Release the effect-descriptor storage of a backend-facing chain and reset it to
/// a freshly-initialised state. Used at backend teardown / canvas destruction. NULL-safe.
/// @param chain Backend-facing chain whose allocation is released; ignored when `NULL`.
void vgfx3d_postfx_chain_free(vgfx3d_postfx_chain_t *chain) {
    if (!chain)
        return;
    if (vgfx3d_postfx_chain_storage_is_valid(chain))
        free(chain->owned_effects);
    chain->effects = NULL;
    chain->effect_capacity = 0;
    chain->effect_count = 0;
    chain->enabled = 0;
    chain->owned_effects = NULL;
    chain->owned_effect_capacity = 0;
    chain->effect_storage_cookie = 0;
}

/// @brief Copy the enabled effect descriptors from `src` into `dst`, growing `dst`'s
/// capacity if needed. Falls back to a reset (empty, disabled) on a disabled or empty
/// source, or on realloc failure — the caller can safely treat a zero return as "no
/// chain active" without inspecting `dst` further. Unused tail descriptors in `dst`
/// are zeroed so a stale shader that over-reads sees explicit disables instead of
/// garbage.
/// @param dst Destination chain whose owned storage is reused or grown.
/// @param src Source chain to copy; may be `NULL` to reset @p dst.
/// @return 1 when an enabled non-empty chain was copied, or 0 when the destination was reset.
int vgfx3d_postfx_chain_copy(vgfx3d_postfx_chain_t *dst, const vgfx3d_postfx_chain_t *src) {
    if (!dst)
        return 0;
    if (!src || !src->enabled || src->effect_count <= 0 ||
        src->effect_count > VGFX3D_POSTFX_MAX_EFFECTS || !src->effects ||
        src->effect_capacity < src->effect_count) {
        vgfx3d_postfx_chain_reset(dst);
        return 0;
    }
    for (int32_t i = 0; i < src->effect_count; i++) {
        if (src->effects[i].type < (int32_t)VGFX3D_POSTFX_EFFECT_BLOOM ||
            src->effects[i].type > (int32_t)VGFX3D_POSTFX_EFFECT_SUN_SHAFTS) {
            vgfx3d_postfx_chain_reset(dst);
            return 0;
        }
    }
    if (!vgfx3d_postfx_chain_reserve(dst, src->effect_count)) {
        vgfx3d_postfx_chain_reset(dst);
        return 0;
    }
    for (int32_t i = 0; i < src->effect_count; i++) {
        vgfx3d_postfx_snapshot_t safe_snapshot;
        dst->effects[i].type = src->effects[i].type;
        vgfx3d_sanitize_postfx_snapshot(&src->effects[i].snapshot, &safe_snapshot);
        dst->effects[i].snapshot = safe_snapshot;
    }
    if (dst->effect_capacity > src->effect_count) {
        memset(dst->effects + src->effect_count,
               0,
               (size_t)(dst->effect_capacity - src->effect_count) * sizeof(*dst->effects));
    }
    dst->enabled = src->enabled ? 1 : 0;
    dst->effect_count = src->effect_count;
    return 1;
}

/// @brief Export a gameplay-facing `PostFX3D` as a backend-consumable chain of effect
/// descriptors. Walks the internal effect list and appends only the *enabled* entries
/// (translated via `vgfx3d_postfx_fill_effect_snapshot`) into `out->effects`. Returns 1
/// when at least one effect made it through, 0 when the chain is empty / disabled / all
/// entries disabled / the target buffer couldn't be grown. The per-frame backend reads
/// this into a local snapshot so later edits to the gameplay chain don't affect the
/// in-flight frame.
/// @param postfx Candidate gameplay-facing PostFX3D chain.
/// @param[out] out Backend chain whose owned descriptor storage receives enabled effects.
/// @return 1 when at least one enabled supported effect was exported, or 0 after resetting
///   @p out.
int vgfx3d_postfx_get_chain(void *postfx, vgfx3d_postfx_chain_t *out) {
    rt_postfx3d *fx;
    int32_t enabled_count = 0;
    int32_t effect_count = 0;

    if (!out)
        return 0;
    fx = postfx3d_checked(postfx);
    if (!fx) {
        vgfx3d_postfx_chain_reset(out);
        return 0;
    }
    effect_count = postfx3d_safe_effect_count(fx);
    if (!fx->enabled || effect_count == 0) {
        vgfx3d_postfx_chain_reset(out);
        return 0;
    }

    for (int32_t i = 0; i < effect_count; i++) {
        if (fx->effects[i].enabled)
            enabled_count++;
    }
    if (enabled_count == 0) {
        vgfx3d_postfx_chain_reset(out);
        return 0;
    }
    if (!vgfx3d_postfx_chain_reserve(out, enabled_count)) {
        vgfx3d_postfx_chain_reset(out);
        return 0;
    }

    out->enabled = 1;
    out->effect_count = 0;
    for (int32_t i = 0; i < effect_count; i++) {
        if (!vgfx3d_postfx_fill_effect_snapshot(&fx->effects[i], &out->effects[out->effect_count]))
            continue;
        out->effect_count++;
    }
    if (out->effect_capacity > out->effect_count) {
        memset(out->effects + out->effect_count,
               0,
               (size_t)(out->effect_capacity - out->effect_count) * sizeof(*out->effects));
    }
    if (out->effect_count <= 0) {
        vgfx3d_postfx_chain_reset(out);
        return 0;
    }
    return 1;
}

/// @brief Flatten a gameplay-facing `PostFX3D` into a single struct whose fields carry
/// the parameters of the *last* occurrence of each effect type — the legacy (pre-chain)
/// API shape kept for backends that don't yet iterate the chain. When the same effect
/// type appears more than once, later entries stomp earlier ones. Returns 1 when the
/// chain is enabled and has ≥1 entry, 0 otherwise; `out` is always zeroed before use so
/// disabled fields read as zero instead of garbage.
/// @param postfx Candidate gameplay-facing PostFX3D chain.
/// @param[out] out Legacy flat snapshot, always cleared before validation and export.
/// @return 1 when at least one enabled supported effect was flattened, or 0 otherwise.
int vgfx3d_postfx_get_snapshot(void *postfx, vgfx3d_postfx_snapshot_t *out) {
    rt_postfx3d *fx;
    int32_t valid_count = 0;
    int32_t effect_count = 0;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    fx = postfx3d_checked(postfx);
    if (!fx)
        return 0;
    effect_count = postfx3d_safe_effect_count(fx);
    if (!fx->enabled || effect_count == 0)
        return 0;

    for (int32_t i = 0; i < effect_count; i++) {
        vgfx3d_postfx_effect_desc_t effect;
        if (!vgfx3d_postfx_fill_effect_snapshot(&fx->effects[i], &effect))
            continue;
        if (effect.type > (int32_t)POSTFX_SSR)
            continue;
        valid_count++;
        switch ((postfx_type_t)effect.type) {
            case POSTFX_BLOOM:
                out->bloom_enabled = 1;
                out->bloom_threshold = effect.snapshot.bloom_threshold;
                out->bloom_intensity = effect.snapshot.bloom_intensity;
                out->bloom_passes = effect.snapshot.bloom_passes;
                break;
            case POSTFX_TONEMAP:
                out->tonemap_mode = effect.snapshot.tonemap_mode;
                out->tonemap_exposure = effect.snapshot.tonemap_exposure;
                break;
            case POSTFX_FXAA:
                out->fxaa_enabled = 1;
                break;
            case POSTFX_COLOR_GRADE:
                out->color_grade_enabled = 1;
                out->cg_brightness = effect.snapshot.cg_brightness;
                out->cg_contrast = effect.snapshot.cg_contrast;
                out->cg_saturation = effect.snapshot.cg_saturation;
                break;
            case POSTFX_VIGNETTE:
                out->vignette_enabled = 1;
                out->vignette_radius = effect.snapshot.vignette_radius;
                out->vignette_softness = effect.snapshot.vignette_softness;
                break;
            case POSTFX_SSAO:
                out->ssao_enabled = 1;
                out->ssao_radius = effect.snapshot.ssao_radius;
                out->ssao_intensity = effect.snapshot.ssao_intensity;
                out->ssao_samples = effect.snapshot.ssao_samples;
                break;
            case POSTFX_DOF:
                out->dof_enabled = 1;
                out->dof_focus_distance = effect.snapshot.dof_focus_distance;
                out->dof_aperture = effect.snapshot.dof_aperture;
                out->dof_max_blur = effect.snapshot.dof_max_blur;
                break;
            case POSTFX_MOTION_BLUR:
                out->motion_blur_enabled = 1;
                out->motion_blur_intensity = effect.snapshot.motion_blur_intensity;
                out->motion_blur_samples = effect.snapshot.motion_blur_samples;
                break;
            case POSTFX_TAA:
                out->taa_enabled = 1;
                out->taa_blend = effect.snapshot.taa_blend;
                break;
            case POSTFX_SSR:
                out->ssr_enabled = 1;
                out->ssr_intensity = effect.snapshot.ssr_intensity;
                out->ssr_max_roughness = effect.snapshot.ssr_max_roughness;
                out->ssr_steps = effect.snapshot.ssr_steps;
                break;
            default:
                break;
        }
    }
    if (valid_count <= 0) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->enabled = 1;
    return 1;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
