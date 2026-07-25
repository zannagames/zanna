//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_skybox.c
// Purpose: Canvas3D CPU skybox fallback — rasterize the bound cubemap into a
//   pixel buffer (per-pixel inverse-VP ray for perspective, single-color fill
//   for ortho) with a per-frame cache keyed by viewport/camera/cubemap
//   generation. Used when the backend can't render the skybox directly (e.g.
//   the software renderer). Split out of rt_canvas3d.c.
// Key invariants:
//   - The cache is valid only while viewport, cubemap generation, projection
//     mode, and VP/camera (or forward, for ortho) are unchanged.
//   - OOM on cache (re)allocation invalidates the cache rather than leaving a
//     half-valid state.
// Ownership/Lifetime:
//   - Canvas3D owns the retained RGBA8 cache allocation.
//   - Cubemap, camera state, and caller-provided destination buffers are borrowed.
// Links: rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the cached CPU fallback for Canvas3D cubemap skyboxes.
/// @details Software rendering samples a cubemap from cached camera rays into a
///   bounded RGBA8 image, reuses it while its composite key remains valid, and
///   scales it into the active frame destination during composition.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d_internal.h"
#include "vgfx3d_backend_utils.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS3D_SKYBOX_CPU_CACHE_MAX_DIM 1024

/// @brief Free and reset the canvas's cached CPU-rendered skybox, forcing a re-render on the
///   next software skybox draw (call when the skybox texture or view parameters change).
/// @param c Canvas whose owned cache allocation and key snapshot are cleared;
///   `NULL` is accepted as a no-op.
void rt_canvas3d_invalidate_skybox_cache(rt_canvas3d *c) {
    if (!c)
        return;
    free(c->skybox_cpu_cache);
    c->skybox_cpu_cache = NULL;
    c->skybox_cpu_cache_w = 0;
    c->skybox_cpu_cache_h = 0;
    c->skybox_cpu_cache_generation = 0;
    c->skybox_cpu_cache_is_ortho = 0;
    memset(c->skybox_cpu_cache_vp, 0, sizeof(c->skybox_cpu_cache_vp));
    memset(c->skybox_cpu_cache_cam_pos, 0, sizeof(c->skybox_cpu_cache_cam_pos));
    memset(c->skybox_cpu_cache_forward, 0, sizeof(c->skybox_cpu_cache_forward));
}

/// @brief True if two float arrays match element-wise within @p eps; any non-finite
///   element or NULL/negative input fails the comparison.
/// @param a First array containing at least @p count floats.
/// @param b Second array containing at least @p count floats.
/// @param count Number of elements to compare; zero is a vacuous match.
/// @param eps Maximum permitted absolute difference, expected to be nonnegative.
/// @return Nonzero when both arrays are valid and all requested finite elements
///   differ by no more than @p eps; zero otherwise.
static int canvas3d_float_array_close(const float *a, const float *b, int32_t count, float eps) {
    if (!a || !b || count < 0)
        return 0;
    for (int32_t i = 0; i < count; i++) {
        if (!isfinite(a[i]) || !isfinite(b[i]))
            return 0;
        if (fabsf(a[i] - b[i]) > eps)
            return 0;
    }
    return 1;
}

/// @brief Choose the retained CPU skybox cache dimensions for a destination size.
/// @details Large software fallback targets do not need one cubemap sample per output pixel: the
///   skybox is infinitely distant and retained low-resolution sampling is visually
///   stable. This caps the largest cache axis while preserving aspect ratio and
///   exact dimensions for small targets.
/// @param dst_w Destination width in pixels.
/// @param dst_h Destination height in pixels.
/// @param out_w Optional output receiving the positive capped cache width, or zero
///   when either destination dimension is nonpositive.
/// @param out_h Optional output receiving the positive capped cache height, or zero
///   when either destination dimension is nonpositive.
static void canvas3d_skybox_cpu_cache_dimensions(int32_t dst_w,
                                                 int32_t dst_h,
                                                 int32_t *out_w,
                                                 int32_t *out_h) {
    int32_t cache_w = dst_w;
    int32_t cache_h = dst_h;

    if (dst_w <= 0 || dst_h <= 0) {
        if (out_w)
            *out_w = 0;
        if (out_h)
            *out_h = 0;
        return;
    }
    if (dst_w > CANVAS3D_SKYBOX_CPU_CACHE_MAX_DIM || dst_h > CANVAS3D_SKYBOX_CPU_CACHE_MAX_DIM) {
        if (dst_w >= dst_h) {
            cache_w = CANVAS3D_SKYBOX_CPU_CACHE_MAX_DIM;
            cache_h = (int32_t)(((int64_t)dst_h * cache_w + dst_w / 2) / dst_w);
        } else {
            cache_h = CANVAS3D_SKYBOX_CPU_CACHE_MAX_DIM;
            cache_w = (int32_t)(((int64_t)dst_w * cache_h + dst_h / 2) / dst_h);
        }
        if (cache_w < 1)
            cache_w = 1;
        if (cache_h < 1)
            cache_h = 1;
    }
    if (out_w)
        *out_w = cache_w;
    if (out_h)
        *out_h = cache_h;
}

/// @brief Normalize a skybox sample direction, using @p fallback then -Z for malformed input.
/// @details Scales in double precision before measuring length to avoid overflow
///   for very large finite vectors. The candidate may alias @p out.
/// @param candidate Optional three-element direction to normalize.
/// @param fallback Optional three-element direction used when @p candidate is
///   nonfinite or nearly zero.
/// @param out Output three-element finite unit vector; receives `(0, 0, -1)` if
///   neither source is usable.
static void canvas3d_sanitize_skybox_dir(const float *candidate,
                                         const float *fallback,
                                         float out[3]) {
    double x = candidate ? (double)candidate[0] : 0.0;
    double y = candidate ? (double)candidate[1] : 0.0;
    double z = candidate ? (double)candidate[2] : 0.0;
    double scale = fabs(x);
    double len;

    if (fabs(y) > scale)
        scale = fabs(y);
    if (fabs(z) > scale)
        scale = fabs(z);
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(scale) || scale <= 1e-20) {
        x = fallback ? (double)fallback[0] : 0.0;
        y = fallback ? (double)fallback[1] : 0.0;
        z = fallback ? (double)fallback[2] : -1.0;
        scale = fabs(x);
        if (fabs(y) > scale)
            scale = fabs(y);
        if (fabs(z) > scale)
            scale = fabs(z);
    }
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(scale) || scale <= 1e-20) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = -1.0f;
        return;
    }
    x /= scale;
    y /= scale;
    z /= scale;
    len = sqrt(x * x + y * y + z * z);
    if (!isfinite(len) || len <= 1e-20) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = -1.0f;
        return;
    }
    out[0] = (float)(x / len);
    out[1] = (float)(y / len);
    out[2] = (float)(z / len);
}

/// @brief CPU-rasterize the bound skybox cubemap into a destination pixel buffer.
/// @details For perspective cameras, unprojects each destination pixel from NDC
///   back to a world-space direction by multiplying through `inverse(VP)`, then
///   samples the cubemap along that ray. Orthographic cameras collapse to a
///   single-color fill along the camera forward direction because an ortho
///   projection has no per-pixel ray divergence — all pixels see the same
///   skybox direction. This is the slow fallback path used when the GPU
///   backend can't render the skybox directly (e.g. the software renderer).
/// @param c Canvas supplying a valid cubemap and cached camera state.
/// @param dst_pixels Writable RGBA8 destination covering @p dst_h rows.
/// @param dst_w Positive destination width in pixels.
/// @param dst_h Positive destination height in pixels.
/// @param dst_stride Byte distance between rows, validated for @p dst_w RGBA8 pixels.
/// @return 1 on success, 0 when the VP matrix is non-invertible or inputs
///   are otherwise malformed.
static int canvas3d_render_skybox_cpu(
    rt_canvas3d *c, uint8_t *dst_pixels, int32_t dst_w, int32_t dst_h, int32_t dst_stride) {
    if (!c || !c->skybox || !rt_g3d_has_class(c->skybox, RT_G3D_CUBEMAP3D_CLASS_ID) ||
        !dst_pixels || !canvas3d_rgba8_stride_valid(dst_w, dst_h, dst_stride))
        return 0;

    if (c->cached_cam_is_ortho) {
        float dir[3];
        float r;
        float g;
        float b;

        canvas3d_sanitize_skybox_dir(c->cached_cam_forward, NULL, dir);
        rt_cubemap_sample_unit(c->skybox, dir[0], dir[1], dir[2], &r, &g, &b);
        uint8_t r8 = canvas3d_clamp01_to_u8(r);
        uint8_t g8 = canvas3d_clamp01_to_u8(g);
        uint8_t b8 = canvas3d_clamp01_to_u8(b);
        size_t row_bytes = (size_t)dst_w * 4u;
        uint8_t *first_row = dst_pixels;
        for (int32_t x = 0; x < dst_w; x++) {
            uint8_t *dst = &first_row[(size_t)x * 4u];
            dst[0] = r8;
            dst[1] = g8;
            dst[2] = b8;
            dst[3] = 0xFF;
        }
        for (int32_t y = 1; y < dst_h; y++) {
            uint8_t *dst = &dst_pixels[(size_t)y * (size_t)dst_stride];
            memcpy(dst, first_row, row_bytes);
        }
        return 1;
    }

    float inv_vp[16];
    if (vgfx3d_invert_matrix4(c->cached_vp, inv_vp) != 0)
        return 0;

    float inv_w = 1.0f / (float)dst_w;
    float inv_h = 1.0f / (float)dst_h;
    /* Every skybox pixel uses clip = (ndc_x, ndc_y, 1, 1), so the z and w
     * columns of inv_vp are loop-invariant and the y column is constant across
     * a row. Fold them out so the per-pixel transform is one multiply-add per
     * component instead of a full 4x4 multiply. */
    float colc[4];
    for (int32_t k = 0; k < 4; k++)
        colc[k] = inv_vp[k * 4 + 2] + inv_vp[k * 4 + 3];
    for (int32_t y = 0; y < dst_h; y++) {
        float ndc_y = 1.0f - 2.0f * ((float)y + 0.5f) * inv_h;
        uint8_t *row = &dst_pixels[(size_t)y * (size_t)dst_stride];
        float rowc[4];
        for (int32_t k = 0; k < 4; k++)
            rowc[k] = inv_vp[k * 4 + 1] * ndc_y + colc[k];
        for (int32_t x = 0; x < dst_w; x++) {
            float ndc_x = 2.0f * ((float)x + 0.5f) * inv_w - 1.0f;
            float world[4];
            float dir[3];
            float r;
            float g;
            float b;
            uint8_t *dst;

            world[0] = inv_vp[0] * ndc_x + rowc[0];
            world[1] = inv_vp[4] * ndc_x + rowc[1];
            world[2] = inv_vp[8] * ndc_x + rowc[2];
            world[3] = inv_vp[12] * ndc_x + rowc[3];
            if (isfinite(world[3]) && fabsf(world[3]) > 1e-7f) {
                world[0] /= world[3];
                world[1] /= world[3];
                world[2] /= world[3];
            }
            dir[0] = world[0] - c->cached_cam_pos[0];
            dir[1] = world[1] - c->cached_cam_pos[1];
            dir[2] = world[2] - c->cached_cam_pos[2];
            canvas3d_sanitize_skybox_dir(dir, c->cached_cam_forward, dir);
            rt_cubemap_sample_unit(c->skybox, dir[0], dir[1], dir[2], &r, &g, &b);
            dst = &row[(size_t)x * 4u];
            dst[0] = canvas3d_clamp01_to_u8(r);
            dst[1] = canvas3d_clamp01_to_u8(g);
            dst[2] = canvas3d_clamp01_to_u8(b);
            dst[3] = 0xFF;
        }
    }
    return 1;
}

/// @brief Check whether the cached CPU skybox can satisfy the current frame.
/// @details Composite key: viewport (w, h), cubemap content generation
///   (bumped whenever a face is uploaded), projection mode (ortho vs
///   perspective), and then either the camera forward vector (ortho) or the
///   full VP matrix + camera position (perspective). The split on projection
///   mode exists because ortho cameras fill the entire target with one
///   sampled direction, so only the forward vector needs to match — the VP
///   matrix can drift without affecting the rendered skybox.
/// @param c Canvas containing the retained image and captured key state.
/// @param w Expected cache width in pixels.
/// @param h Expected cache height in pixels.
/// @param generation Current nonzero cubemap content generation.
/// @return 1 if the cache is valid for this frame, 0 if it must be re-rendered.
static int canvas3d_skybox_cache_matches(const rt_canvas3d *c,
                                         int32_t w,
                                         int32_t h,
                                         uint64_t generation) {
    if (!c || !c->skybox_cpu_cache || w <= 0 || h <= 0)
        return 0;
    if (c->skybox_cpu_cache_w != w || c->skybox_cpu_cache_h != h ||
        c->skybox_cpu_cache_generation != generation ||
        c->skybox_cpu_cache_is_ortho != c->cached_cam_is_ortho)
        return 0;
    if (c->cached_cam_is_ortho) {
        float cached_forward[3];
        float current_forward[3];
        canvas3d_sanitize_skybox_dir(c->skybox_cpu_cache_forward, NULL, cached_forward);
        canvas3d_sanitize_skybox_dir(c->cached_cam_forward, NULL, current_forward);
        return canvas3d_float_array_close(cached_forward, current_forward, 3, 1e-6f);
    }
    return canvas3d_float_array_close(c->skybox_cpu_cache_vp, c->cached_vp, 16, 1e-6f) &&
           canvas3d_float_array_close(c->skybox_cpu_cache_cam_pos, c->cached_cam_pos, 3, 1e-6f);
}

/// @brief Populate (or refresh) the CPU skybox cache so it's ready for blitting.
/// @details If the cache already matches the current viewport/camera/generation
///   we return immediately (cheap fast path). Otherwise we (re)allocate the
///   backing buffer only when its pixel dimensions actually change, then call
///   `canvas3d_render_skybox_cpu` to paint the fresh image and snapshot the
///   cache key. OOM on reallocation invalidates the cache rather than leaving
///   it in a half-valid state. Also guards against `w * h * 4` overflow on
///   32-bit size_t targets.
/// @param c Canvas owning the cache and supplying the cubemap/camera snapshot.
/// @param w Positive final destination width used to select the capped cache size.
/// @param h Positive final destination height used to select the capped cache size.
/// @return 1 when a usable cache is ready, 0 on failure (caller should skip
///   the skybox for this frame rather than render garbage).
int canvas3d_ensure_skybox_cpu_cache(rt_canvas3d *c, int32_t w, int32_t h) {
    uint64_t generation;
    size_t bytes;
    int32_t cache_w;
    int32_t cache_h;

    if (!c || !c->skybox || !rt_g3d_has_class(c->skybox, RT_G3D_CUBEMAP3D_CLASS_ID) || w <= 0 ||
        h <= 0 || (size_t)w > (size_t)INT32_MAX / 4u)
        return 0;
    canvas3d_skybox_cpu_cache_dimensions(w, h, &cache_w, &cache_h);
    if (cache_w <= 0 || cache_h <= 0)
        return 0;
    generation = vgfx3d_get_cubemap_generation(c->skybox);
    if (generation == 0) {
        rt_canvas3d_invalidate_skybox_cache(c);
        return 0;
    }
    if (canvas3d_skybox_cache_matches(c, cache_w, cache_h, generation))
        return 1;
    if ((size_t)cache_w > SIZE_MAX / (size_t)cache_h / 4u)
        return 0;
    bytes = (size_t)cache_w * (size_t)cache_h * 4u;
    if (c->skybox_cpu_cache_w != cache_w || c->skybox_cpu_cache_h != cache_h ||
        !c->skybox_cpu_cache) {
        uint8_t *new_cache = (uint8_t *)realloc(c->skybox_cpu_cache, bytes);
        if (!new_cache) {
            rt_canvas3d_invalidate_skybox_cache(c);
            return 0;
        }
        c->skybox_cpu_cache = new_cache;
    }
    c->skybox_cpu_cache_w = cache_w;
    c->skybox_cpu_cache_h = cache_h;
    if (!canvas3d_render_skybox_cpu(
            c, c->skybox_cpu_cache, cache_w, cache_h, (int32_t)((int64_t)cache_w * 4))) {
        rt_canvas3d_invalidate_skybox_cache(c);
        return 0;
    }
    c->skybox_cpu_cache_generation = generation;
    c->skybox_cpu_cache_is_ortho = c->cached_cam_is_ortho;
    memcpy(c->skybox_cpu_cache_vp, c->cached_vp, sizeof(c->skybox_cpu_cache_vp));
    memcpy(c->skybox_cpu_cache_cam_pos, c->cached_cam_pos, sizeof(c->skybox_cpu_cache_cam_pos));
    if (c->cached_cam_is_ortho)
        canvas3d_sanitize_skybox_dir(c->cached_cam_forward, NULL, c->skybox_cpu_cache_forward);
    else
        memcpy(c->skybox_cpu_cache_forward,
               c->cached_cam_forward,
               sizeof(c->skybox_cpu_cache_forward));
    return 1;
}

/// @brief Copy the cached CPU skybox into the frame's destination buffer row-by-row.
/// @details Performs no rendering — it assumes `canvas3d_ensure_skybox_cpu_cache`
///   has already refreshed the cache for this frame. Exact-size caches use row
///   copies; downsampled large-target caches are nearest-upscaled to the target.
/// @param c Canvas containing a ready borrowed source cache.
/// @param dst_pixels Writable RGBA8 destination covering @p dst_h rows.
/// @param dst_w Positive destination width in pixels.
/// @param dst_h Positive destination height in pixels.
/// @param dst_stride Byte distance between destination rows, validated for RGBA8.
void canvas3d_blit_skybox_cpu_cache(
    rt_canvas3d *c, uint8_t *dst_pixels, int32_t dst_w, int32_t dst_h, int32_t dst_stride) {
    int32_t src_stride;

    if (!c || !c->skybox_cpu_cache || !dst_pixels || dst_w <= 0 || dst_h <= 0 ||
        !canvas3d_rgba8_stride_valid(dst_w, dst_h, dst_stride))
        return;
    if (c->skybox_cpu_cache_w <= 0 || c->skybox_cpu_cache_h <= 0)
        return;
    src_stride = (int32_t)((int64_t)c->skybox_cpu_cache_w * 4);
    if (c->skybox_cpu_cache_w == dst_w && c->skybox_cpu_cache_h == dst_h) {
        for (int32_t y = 0; y < dst_h; y++)
            memcpy(&dst_pixels[(size_t)y * (size_t)dst_stride],
                   &c->skybox_cpu_cache[(size_t)y * (size_t)src_stride],
                   (size_t)src_stride);
        return;
    }
    for (int32_t y = 0; y < dst_h; y++) {
        int32_t sy = (int32_t)(((int64_t)y * c->skybox_cpu_cache_h) / dst_h);
        const uint8_t *src_row = &c->skybox_cpu_cache[(size_t)sy * (size_t)src_stride];
        uint8_t *dst_row = &dst_pixels[(size_t)y * (size_t)dst_stride];
        for (int32_t x = 0; x < dst_w; x++) {
            int32_t sx = (int32_t)(((int64_t)x * c->skybox_cpu_cache_w) / dst_w);
            const uint8_t *src = &src_row[(size_t)sx * 4u];
            uint8_t *dst = &dst_row[(size_t)x * 4u];
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
        }
    }
}

#endif /* ZANNA_ENABLE_GRAPHICS */
