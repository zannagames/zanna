//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_sw.c
// Purpose: Software rasterizer backend for Zanna.Graphics3D. Implements the
//   vgfx3d_backend_t vtable using CPU-based edge-function rasterization.
//
// Key invariants:
//   - Writes directly to vgfx framebuffer (uint8_t RGBA)
//   - Z-buffer is float[width*height], cleared to FLT_MAX
//   - CCW winding is front-facing
//   - Sutherland-Hodgman frustum clipping in 4D clip space
//   - Blinn-Phong per-vertex lighting (Gouraud shading)
//   - Perspective-correct texture mapping
//   - Shadow pass clips each tri against the shadow frustum before rasterization.
//
// Ownership/Lifetime:
//   - Software backend context is owned by Canvas3D; freed in its destroy_ctx hook.
//   - Z-buffer and vertex scratch are heap allocations resized on demand.
//   - The context owns one persistent worker pool, created once and shut down
//     before the context storage is released.
//
// Links: vgfx3d_backend.h, rt_canvas3d_internal.h, src/runtime/threads/rt_threadpool.h
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_backend_sw.c
 * @brief Implements the CPU software-rasterization backend for Zanna Graphics3D.
 *
 * The translation unit owns the software backend context, safe numeric helpers, analytic-light
 * and shadow sampling, depth probes, worker-pool lifetime, and backend selection. Rasterization,
 * texture sampling, vertex processing, shadow rendering, and vtable operations are split into
 * dependency-ordered implementation fragments included below.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_object.h"
#include "rt_parallel.h"
#include "rt_platform.h"
#include "rt_threadpool.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_utils.h"
#include "vgfx3d_brdf_lut.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW_TILE_SIZE 64
#define SW_MAX_WORKERS 16
#define SW_MAX_TASKS (SW_MAX_WORKERS + 1)
#define SW_PARALLEL_MIN_INDICES 192u
#define SW_MATERIAL_PARAM_ABS_MAX 1000000.0f

/// @brief Submit a typed task callback to the runtime thread pool.
/// @details The public Pool API accepts callbacks through the generic runtime
///          object-call surface. The software renderer uses this internal entry
///          point so C function pointers are passed with their real type.
/// @param pool Pool object pointer.
/// @param callback Task function to execute.
/// @param arg Argument passed to @p callback.
/// @return 1 when queued, 0 when the pool is invalid or shutting down.
extern int8_t rt_threadpool_submit_fn(void *pool, void (*callback)(void *), void *arg);

/*==========================================================================
 * Software backend context
 *=========================================================================*/

typedef struct pipe_vert pipe_vert_t;

typedef struct {
    float *zbuf;
    size_t zbuf_capacity;
    pipe_vert_t *vertex_scratch;
    uint32_t vertex_scratch_capacity;
    int32_t width, height;
    float vp[16]; /* view * projection (float, row-major) */
    float cam_pos[3];
    float cam_forward[3];
    int8_t cam_is_ortho;
    /* Render target override (NULL = render to window framebuffer) */
    vgfx3d_rendertarget_t *render_target;
    /* Fog parameters (copied from Canvas3D each begin_frame) */
    int8_t fog_enabled;
    float fog_near, fog_far;
    float fog_color[3];
    int8_t height_fog_enabled;
    float height_fog_base;
    float height_fog_falloff;
    float height_fog_density;
    float height_fog_blend;
    float height_fog_sun_color[3];
    float height_fog_sun_dir[3];
    float height_fog_sun_power;
    float height_fog_sun_amount;
    /* Image-based lighting (copied from Canvas3D each begin_frame) */
    int8_t ibl_enabled;
    float ibl_intensity;
    float ibl_sh[27];
    /* Shadow mapping state */
    float *shadow_depth[VGFX3D_MAX_SHADOW_LIGHTS];
    int32_t shadow_w[VGFX3D_MAX_SHADOW_LIGHTS], shadow_h[VGFX3D_MAX_SHADOW_LIGHTS];
    float shadow_vp[VGFX3D_MAX_SHADOW_LIGHTS][16];
    float shadow_bias[VGFX3D_MAX_SHADOW_LIGHTS];
    /* Plan 06: per-frame shadow filtering params from camera params. */
    float shadow_strength;
    float shadow_slope_bias;
    int32_t shadow_quality;
    int8_t shadow_pass_slot;
    int8_t shadow_count;
    int8_t shadow_complete[VGFX3D_MAX_SHADOW_LIGHTS];
    /* Plan 10: opaque-pass depth snapshot for soft particles. Holds the active
     * target's zbuf (NDC sz values, FLT_MAX = empty) copied at the
     * opaque->transparent seam; valid resets every begin_frame. */
    float *opaque_zbuf;
    size_t opaque_zbuf_capacity;
    int32_t opaque_zbuf_w, opaque_zbuf_h;
    int8_t opaque_zbuf_valid;
    float cam_znear, cam_zfar;
    /* Persistent worker pool for deterministic tiled rasterization. */
    void *worker_pool;
    int64_t worker_count;
    int32_t debug_draw_count;
    int32_t debug_tri_count;
    /* Scene-depth probes (lens flares): answered synchronously from the CPU
     * z-buffer at queue time; slots reset every begin_frame. */
    float depth_probe_results[VGFX3D_DEPTH_PROBE_MAX];
    int32_t depth_probe_count;
    /* Render scale: window-backed frames rasterize into an internal buffer at
     * render_scale times the framebuffer size, then end_frame upscales
     * (nearest-neighbor) into the framebuffer. The dominant software cost is
     * per-pixel shading, so 0.5x scale is roughly a 4x rasterization speedup.
     * RTT frames always render at the target's exact size. */
    vgfx_window_t win;
    float render_scale;
    uint32_t *scaled_color;
    size_t scaled_color_capacity; /* in pixels */
    int32_t scaled_w, scaled_h;
    int8_t scaled_frame_active;
} sw_context_t;

/// @brief Select a checked geometric allocation capacity for a reusable software buffer.
/// @details A 1.5x growth policy amortizes monotonically increasing scene sizes while preserving
///          the old allocation when the requested element count or byte count is unrepresentable.
/// @param current_elements Current allocation capacity in elements.
/// @param required_elements Minimum required element count.
/// @param element_size Size of one element in bytes.
/// @param out_capacity Receives a capacity no smaller than @p required_elements.
/// @return 1 when the capacity and byte product are representable, otherwise 0.
static int sw_geometric_capacity(size_t current_elements,
                                 size_t required_elements,
                                 size_t element_size,
                                 size_t *out_capacity) {
    size_t capacity;
    size_t max_elements;

    if (!out_capacity || required_elements == 0 || element_size == 0)
        return 0;
    max_elements = SIZE_MAX / element_size;
    if (required_elements > max_elements)
        return 0;
    capacity = current_elements;
    if (capacity < 256u)
        capacity = 256u;
    if (capacity > max_elements)
        capacity = max_elements;
    while (capacity < required_elements) {
        size_t increment = capacity / 2u + 1u;
        if (increment > max_elements - capacity) {
            capacity = required_elements;
            break;
        }
        capacity += increment;
    }
    *out_capacity = capacity;
    return 1;
}

/// @brief Compute the normalized world-space direction from a shaded point toward the camera.
/// @param ctx Borrowed software context containing camera projection and pose state.
/// @param wx World-space point X coordinate.
/// @param wy World-space point Y coordinate.
/// @param wz World-space point Z coordinate.
/// @param out_vx Receives the view-direction X component.
/// @param out_vy Receives the view-direction Y component.
/// @param out_vz Receives the view-direction Z component.
static inline void sw_compute_view_vector(const sw_context_t *ctx,
                                          float wx,
                                          float wy,
                                          float wz,
                                          float *out_vx,
                                          float *out_vy,
                                          float *out_vz);

/// @brief Compute an overflow-resistant Euclidean length for a 3-vector.
/// @details Scales components by their maximum absolute lane before squaring,
///          so very large finite vectors do not overflow to infinity during
///          normalization, fog, or light attenuation calculations.
/// @param x X component.
/// @param y Y component.
/// @param z Z component.
/// @return Vector length, or INFINITY when inputs are non-finite/overflowed.
static float sw_length3(float x, float y, float z) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return INFINITY;
    float ax = fabsf(x);
    float ay = fabsf(y);
    float az = fabsf(z);
    float m = ax;
    if (ay > m)
        m = ay;
    if (az > m)
        m = az;
    if (m <= 0.0f)
        return 0.0f;
    x /= m;
    y /= m;
    z /= m;
    return m * sqrtf(x * x + y * y + z * z);
}

/// @brief Fill a software depth buffer with a constant depth value.
/// @details Writes the first value once, then doubles the initialized prefix with
///          `memcpy`. Large clears avoid one scalar store per pixel, while small
///          buffers still pay only a tiny constant setup cost. Keeping this in one
///          helper also makes future platform-specific vectorization local.
/// @param depth Depth-buffer pointer; NULL is ignored.
/// @param count Number of float entries to write.
/// @param value Depth value to store in every entry.
static void sw_fill_depth_buffer(float *depth, size_t count, float value) {
    size_t filled;
    if (!depth || count == 0)
        return;
    depth[0] = value;
    filled = 1u;
    while (filled < count) {
        size_t copy_count = filled;
        if (copy_count > count - filled)
            copy_count = count - filled;
        memcpy(depth + filled, depth, copy_count * sizeof(*depth));
        filled += copy_count;
    }
}

/// @brief Normalize the vector (*x,*y,*z) in place; on non-finite or near-zero length,
///   substitute the fallback vector and return 0, else return 1.
/// @param x In/out X component; receives @p fallback_x on invalid input.
/// @param y In/out Y component; receives @p fallback_y on invalid input.
/// @param z In/out Z component; receives @p fallback_z on invalid input.
/// @param fallback_x Replacement X component for an invalid or degenerate vector.
/// @param fallback_y Replacement Y component for an invalid or degenerate vector.
/// @param fallback_z Replacement Z component for an invalid or degenerate vector.
/// @return 1 when the original vector was normalized, otherwise 0 after applying fallbacks.
static int sw_normalize3(
    float *x, float *y, float *z, float fallback_x, float fallback_y, float fallback_z) {
    float len;
    if (!x || !y || !z || !isfinite(*x) || !isfinite(*y) || !isfinite(*z)) {
        if (x)
            *x = fallback_x;
        if (y)
            *y = fallback_y;
        if (z)
            *z = fallback_z;
        return 0;
    }
    len = sw_length3(*x, *y, *z);
    if (!isfinite(len) || len <= 1e-7f) {
        *x = fallback_x;
        *y = fallback_y;
        *z = fallback_z;
        return 0;
    }
    *x /= len;
    *y /= len;
    *z /= len;
    return 1;
}

/// @brief Clamp a float to [0, 1], mapping every non-positive/NaN value to zero.
/// @param value Floating-point value to clamp.
/// @return @p value constrained to `[0, 1]`, with NaN mapped to zero.
static inline float clamp01f(float value) {
    if (!(value > 0.0f))
        return 0.0f;
    return value >= 1.0f ? 1.0f : value;
}

/// @brief Compute x^5 without a libm call for Schlick Fresnel terms.
/// @param value Base value to raise to the fifth power.
/// @return The arithmetic product @p value raised to the fifth power.
static inline float pow5f_fast(float value) {
    float squared = value * value;
    return squared * squared * value;
}

/// @brief Raise a clamped material term to a finite, bounded exponent.
/// @param value Material term clamped to `[0, 1]` before exponentiation.
/// @param power Requested exponent; non-finite values become one and values above 4096 are capped.
/// @return The sanitized power result, including one for non-positive exponents.
static inline float sw_material_powf(float value, float power) {
    value = clamp01f(value);
    if (!isfinite(power))
        power = 1.0f;
    if (power > 4096.0f)
        power = 4096.0f;
    if (power <= 0.0f)
        return 1.0f;
    if (fabsf(power - 1.0f) < 1e-6f)
        return value;
    if (fabsf(power - 2.0f) < 1e-6f)
        return value * value;
    if (fabsf(power - 3.0f) < 1e-6f)
        return value * value * value;
    if (fabsf(power - 4.0f) < 1e-6f) {
        float squared = value * value;
        return squared * squared;
    }
    if (fabsf(power - 5.0f) < 1e-6f)
        return pow5f_fast(value);
    return powf(value, power);
}

/// @brief Clamp a caller-provided light array count to the fixed renderer payload.
/// @param lights Borrowed light array; null is treated as empty.
/// @param count Caller-provided array length.
/// @return A count in `[0, VGFX3D_MAX_LIGHTS]`.
static int32_t sw_sanitize_light_count(const vgfx3d_light_params_t *lights, int32_t count) {
    if (!lights || count <= 0)
        return 0;
    return count > VGFX3D_MAX_LIGHTS ? VGFX3D_MAX_LIGHTS : count;
}

/// @brief Validate an RGBA8 surface before any row or pixel pointer arithmetic.
/// @param pixels Borrowed first byte of the surface.
/// @param width Signed pixel width.
/// @param height Signed pixel height.
/// @param stride Signed row stride in bytes.
/// @return Non-zero when the dimensions, stride, and final row offset form a representable
///         RGBA8 surface; otherwise zero.
static int sw_rgba_surface_is_valid(const uint8_t *pixels,
                                    int32_t width,
                                    int32_t height,
                                    int32_t stride) {
    size_t row_bytes;
    size_t final_offset;
    if (!pixels || width <= 0 || height <= 0 || stride <= 0)
        return 0;
    if ((size_t)width > SIZE_MAX / 4u)
        return 0;
    row_bytes = (size_t)width * 4u;
    if ((size_t)stride < row_bytes)
        return 0;
    if ((size_t)(height - 1) > (SIZE_MAX - row_bytes) / (size_t)stride)
        return 0;
    final_offset = (size_t)(height - 1) * (size_t)stride + row_bytes;
    return final_offset >= row_bytes;
}

/// @brief Floor a finite screen coordinate and clamp it to an inclusive pixel extent.
/// @param value Screen coordinate to floor.
/// @param max_inclusive Largest legal pixel coordinate.
/// @return The floored coordinate clamped to `[0, max_inclusive]`; invalid inputs map to zero.
static int32_t sw_floor_pixel_coord(float value, int32_t max_inclusive) {
    double rounded;
    if (!isfinite(value) || max_inclusive <= 0 || value <= 0.0f)
        return 0;
    rounded = floor((double)value);
    if (rounded >= (double)max_inclusive)
        return max_inclusive;
    return (int32_t)rounded;
}

/// @brief Ceil a finite screen coordinate and clamp it to an inclusive pixel extent.
/// @param value Screen coordinate to ceil.
/// @param max_inclusive Largest legal pixel coordinate.
/// @return The ceiled coordinate clamped to `[0, max_inclusive]`, or -1 for a non-finite or
///         negative input.
static int32_t sw_ceil_pixel_coord(float value, int32_t max_inclusive) {
    double rounded;
    if (!isfinite(value))
        return -1;
    if (value < 0.0f)
        return -1;
    if (max_inclusive <= 0 || value == 0.0f)
        return 0;
    rounded = ceil((double)value);
    if (rounded >= (double)max_inclusive)
        return max_inclusive;
    return (int32_t)rounded;
}

/// @brief Convert a normalized half-open coordinate to a valid texture/depth index.
/// @param value Normalized coordinate, conventionally in `[0, 1]`.
/// @param extent Positive number of addressable elements.
/// @return A valid index in `[0, extent)`, with invalid input mapped to zero.
static int32_t sw_unit_coord_to_index(float value, int32_t extent) {
    double scaled;
    if (!isfinite(value) || extent <= 0)
        return 0;
    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return extent - 1;
    scaled = floor((double)value * (double)extent);
    if (scaled >= (double)extent)
        return extent - 1;
    return (int32_t)scaled;
}

/// @brief Copy one draw command while replacing malformed numeric material state.
/// @details The canvas normally resolves finite values before backend dispatch, but the
///          backend vtable is also an internal C boundary exercised by tests and tools.
///          Keeping sanitization here prevents NaNs from reaching libm or byte casts.
/// @param src Borrowed draw-command snapshot to sanitize.
/// @param dst Receives the sanitized command.
static void sw_sanitize_draw_command(const vgfx3d_draw_cmd_t *src, vgfx3d_draw_cmd_t *dst) {
    vgfx3d_sanitize_draw_command(src, dst);
}

/// @brief Copy and sanitize one bounded software-light snapshot.
/// @param src Borrowed source-light array.
/// @param count Number of source entries supplied by the caller.
/// @param dst Receives at most @c VGFX3D_MAX_LIGHTS sanitized entries.
/// @return Number of valid entries written to @p dst.
static int32_t sw_sanitize_lights(const vgfx3d_light_params_t *src,
                                  int32_t count,
                                  vgfx3d_light_params_t dst[VGFX3D_MAX_LIGHTS]) {
    return vgfx3d_sanitize_light_array(src, count, dst, VGFX3D_MAX_LIGHTS);
}

/// @brief Copy non-negative, bounded ambient RGB state.
/// @param src Borrowed three-component ambient color.
/// @param dst Receives the sanitized RGB components.
static void sw_sanitize_ambient(const float *src, float dst[3]) {
    vgfx3d_sanitize_ambient_rgb(src, dst);
}

/// @brief Smoothly fade a finite-range emitter to zero at its authored boundary.
/// @param distance Non-negative distance from the shaded point to the emitter.
/// @param range Positive authored cutoff distance.
/// @return Smoothstep-shaped fade in `[0, 1]`, or zero for invalid/out-of-range input.
static float sw_light_range_fade(float distance, float range) {
    float remaining;
    if (!isfinite(distance) || !isfinite(range) || range <= 1e-7f || distance >= range)
        return 0.0f;
    remaining = 1.0f - distance / range;
    return remaining * remaining * (3.0f - 2.0f * remaining);
}

/// @brief Evaluate the FBX decay exponent using the light's legacy attenuation coefficient.
/// @param light Borrowed light supplying decay type and attenuation coefficient.
/// @param distance Non-negative distance to the emitter.
/// @return Finite attenuation in `[0, 1]`, or zero for invalid input.
static float sw_light_distance_decay(const vgfx3d_light_params_t *light, float distance) {
    float attenuation;
    float powered_distance;
    if (!light || !isfinite(distance) || distance < 0.0f)
        return 0.0f;
    if (light->decay_type == 0)
        return 1.0f;
    attenuation =
        isfinite(light->attenuation) && light->attenuation > 0.0f ? light->attenuation : 0.001f;
    powered_distance = distance;
    if (light->decay_type >= 2)
        powered_distance *= distance;
    if (light->decay_type >= 3)
        powered_distance *= distance;
    if (!isfinite(powered_distance))
        return 0.0f;
    {
        double denominator = 1.0 + (double)attenuation * (double)powered_distance;
        if (!isfinite(denominator) || denominator <= 0.0)
            return 0.0f;
        return (float)(1.0 / denominator);
    }
}

/// @brief Overflow-safe legacy inverse-quadratic attenuation for point/spot lights.
/// @param coefficient Non-negative inverse-quadratic attenuation coefficient.
/// @param distance Non-negative distance to the emitter.
/// @return Finite attenuation in `[0, 1]`, or zero for invalid arithmetic.
static float sw_punctual_attenuation(float coefficient, float distance) {
    double denominator;
    if (!isfinite(distance) || distance < 0.0f)
        return 0.0f;
    if (!isfinite(coefficient) || coefficient < 0.0f)
        coefficient = 0.0f;
    denominator = 1.0 + (double)coefficient * (double)distance * (double)distance;
    if (!isfinite(denominator) || denominator <= 0.0)
        return 0.0f;
    return (float)(1.0 / denominator);
}

/// @brief Evaluate one analytic light at a world point.
/// @param light Borrowed sanitized light description.
/// @param wx World-space X coordinate of the shaded point.
/// @param wy World-space Y coordinate of the shaded point.
/// @param wz World-space Z coordinate of the shaded point.
/// @param out_lx Receives the X component of the fragment-to-emitter direction.
/// @param out_ly Receives the Y component of the fragment-to-emitter direction.
/// @param out_lz Receives the Z component of the fragment-to-emitter direction.
/// @param out_attenuation Receives the finite scalar light attenuation.
/// @return 0 for no contribution/ambient, 1 for directional BRDF lighting, or 2 for isotropic
///   volume irradiance. For result 1, @p out_l* is the unit fragment-to-emitter direction.
static int sw_eval_analytic_light(const vgfx3d_light_params_t *light,
                                  float wx,
                                  float wy,
                                  float wz,
                                  float *out_lx,
                                  float *out_ly,
                                  float *out_lz,
                                  float *out_attenuation) {
    float lx;
    float ly;
    float lz;
    float attenuation = 1.0f;
    float distance;
    if (!light || !out_lx || !out_ly || !out_lz || !out_attenuation)
        return 0;
    if (light->type == 0) {
        lx = -light->direction[0];
        ly = -light->direction[1];
        lz = -light->direction[2];
        if (!sw_normalize3(&lx, &ly, &lz, 0.0f, 0.0f, 0.0f))
            return 0;
    } else if (light->type == 1) {
        lx = light->position[0] - wx;
        ly = light->position[1] - wy;
        lz = light->position[2] - wz;
        distance = sw_length3(lx, ly, lz);
        if (!isfinite(distance) || distance <= 1e-7f)
            return 0;
        lx /= distance;
        ly /= distance;
        lz /= distance;
        attenuation = sw_punctual_attenuation(light->attenuation, distance);
    } else if (light->type == 3) {
        float sx;
        float sy;
        float sz;
        float spot_dot;
        lx = light->position[0] - wx;
        ly = light->position[1] - wy;
        lz = light->position[2] - wz;
        distance = sw_length3(lx, ly, lz);
        if (!isfinite(distance) || distance <= 1e-7f)
            return 0;
        lx /= distance;
        ly /= distance;
        lz /= distance;
        attenuation = sw_punctual_attenuation(light->attenuation, distance);
        sx = -light->direction[0];
        sy = -light->direction[1];
        sz = -light->direction[2];
        if (!sw_normalize3(&sx, &sy, &sz, 0.0f, 0.0f, 0.0f))
            return 0;
        spot_dot = lx * sx + ly * sy + lz * sz;
        if (spot_dot < light->outer_cos) {
            attenuation = 0.0f;
        } else if (spot_dot < light->inner_cos) {
            float cone_range = light->inner_cos - light->outer_cos;
            if (cone_range <= 1e-6f) {
                attenuation = 0.0f;
            } else {
                float t = (spot_dot - light->outer_cos) / cone_range;
                attenuation *= t * t * (3.0f - 2.0f * t);
            }
        }
    } else if (light->type == 4) {
        float ux = light->basis_u[0];
        float uy = light->basis_u[1];
        float uz = light->basis_u[2];
        float vx = light->basis_v[0];
        float vy = light->basis_v[1];
        float vz = light->basis_v[2];
        float rel_x = wx - light->position[0];
        float rel_y = wy - light->position[1];
        float rel_z = wz - light->position[2];
        float half_width = fmaxf(light->width * 0.5f, 1e-6f);
        float half_height = fmaxf(light->height * 0.5f, 1e-6f);
        float u;
        float v;
        float emitter_cosine;
        float area;
        float solid_angle;
        if (!sw_normalize3(&ux, &uy, &uz, 1.0f, 0.0f, 0.0f) ||
            !sw_normalize3(&vx, &vy, &vz, 0.0f, 1.0f, 0.0f))
            return 0;
        u = rel_x * ux + rel_y * uy + rel_z * uz;
        v = rel_x * vx + rel_y * vy + rel_z * vz;
        if (u < -half_width)
            u = -half_width;
        if (u > half_width)
            u = half_width;
        if (v < -half_height)
            v = -half_height;
        if (v > half_height)
            v = half_height;
        lx = light->position[0] + ux * u + vx * v - wx;
        ly = light->position[1] + uy * u + vy * v - wy;
        lz = light->position[2] + uz * u + vz * v - wz;
        distance = sw_length3(lx, ly, lz);
        if (!isfinite(distance))
            return 0;
        if (distance <= 1e-7f) {
            lx = -light->direction[0];
            ly = -light->direction[1];
            lz = -light->direction[2];
            if (!sw_normalize3(&lx, &ly, &lz, 0.0f, 0.0f, 1.0f))
                return 0;
            distance = 0.0f;
        } else {
            lx /= distance;
            ly /= distance;
            lz /= distance;
        }
        emitter_cosine =
            -(light->direction[0] * lx + light->direction[1] * ly + light->direction[2] * lz);
        if (!isfinite(emitter_cosine) || emitter_cosine <= 0.0f)
            return 0;
        if (emitter_cosine > 1.0f)
            emitter_cosine = 1.0f;
        area = fmaxf(light->width * light->height, 1e-6f);
        solid_angle = area / (area + 3.14159265f * distance * distance);
        attenuation = emitter_cosine * solid_angle * sw_light_distance_decay(light, distance) *
                      sw_light_range_fade(distance, light->range);
    } else if (light->type == 5) {
        float radius = fmaxf(light->radius, 1e-6f);
        float center_distance;
        float solid_angle;
        lx = light->position[0] - wx;
        ly = light->position[1] - wy;
        lz = light->position[2] - wz;
        center_distance = sw_length3(lx, ly, lz);
        if (!isfinite(center_distance))
            return 0;
        if (center_distance <= 1e-7f) {
            lx = 0.0f;
            ly = 1.0f;
            lz = 0.0f;
            distance = 0.0f;
        } else {
            lx /= center_distance;
            ly /= center_distance;
            lz /= center_distance;
            distance = center_distance > radius ? center_distance - radius : 0.0f;
        }
        solid_angle = radius * radius / (radius * radius + distance * distance);
        attenuation = solid_angle * sw_light_distance_decay(light, distance) *
                      sw_light_range_fade(distance, light->range);
    } else if (light->type == 6) {
        float radius = fmaxf(light->radius, 1e-6f);
        float t;
        distance =
            sw_length3(wx - light->position[0], wy - light->position[1], wz - light->position[2]);
        if (!isfinite(distance) || distance >= radius)
            return 0;
        t = 1.0f - distance / radius;
        *out_lx = *out_ly = *out_lz = 0.0f;
        *out_attenuation = t * t * (3.0f - 2.0f * t);
        return 2;
    } else {
        return 0;
    }
    if (!isfinite(attenuation) || attenuation <= 0.0f)
        return 0;
    *out_lx = lx;
    *out_ly = ly;
    *out_lz = lz;
    *out_attenuation = attenuation;
    return 1;
}

/// @brief Recount the highest complete shadow slot and cache the scan bound.
/// @details Shadow slots can be sparse while lights are reconfigured. Counting
///          through the highest valid slot lets later valid slots remain visible
///          even when an earlier slot is empty; invalid gaps simply sample as lit.
/// @param ctx Borrowed software context whose shadow scan bound is recomputed; null is a no-op.
static void sw_recompute_shadow_count(sw_context_t *ctx) {
    int8_t count = 0;
    if (!ctx)
        return;
    for (int slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++) {
        if (ctx->shadow_complete[slot] && ctx->shadow_depth[slot] && ctx->shadow_w[slot] > 0 &&
            ctx->shadow_h[slot] > 0)
            count = (int8_t)(slot + 1);
    }
    ctx->shadow_count = count;
}

/* Plan 06: 16-point Poisson disk (unit-disk offsets), deterministic dart-throwing
 * (64-bit LCG, seed 80, min separation 0.4369) — the same table as the GPU shader
 * twins. Tap subsets stride the table so smaller tiers stay well-distributed. */
static const float sw_shadow_poisson[16][2] = {
    {0.482408f, 0.345286f},
    {0.079872f, 0.641222f},
    {-0.211339f, -0.305133f},
    {0.485624f, -0.638176f},
    {-0.197270f, 0.252333f},
    {-0.823845f, 0.551135f},
    {-0.333578f, 0.886038f},
    {0.835697f, 0.028104f},
    {-0.921729f, -0.289255f},
    {-0.184344f, -0.769094f},
    {0.449912f, -0.195238f},
    {0.534685f, 0.795828f},
    {-0.661694f, -0.644587f},
    {-0.599728f, 0.062200f},
    {0.899330f, -0.413558f},
    {0.200012f, -0.976846f},
};

/// @brief Sample the shadow map for @p slot at a world-space position and return a visibility
/// factor.
/// @details Plan 06 sampling model: the receiver is normal-offset ~1.5 shadow texels (world
///          units, derived from the shadow VP row lengths), the compare bias adds the map-gradient
///          slope term scaled by the canvas slope-bias knob, and filtering uses a rotated-Poisson
///          PCF (4 taps at PERFORMANCE, 8 at BALANCED/CINEMATIC — CPU-cost bounded) rotated by
///          interleaved-gradient noise over the shadow-map texel coordinate. Fully-occluded taps
///          contribute `1 - shadow_strength` so `ShadowStrength = 1` yields black shadows.
/// @param ctx Borrowed software context containing shadow maps and filter settings.
/// @param slot Shadow-map array slot to sample.
/// @param wx World-space receiver X coordinate.
/// @param wy World-space receiver Y coordinate.
/// @param wz World-space receiver Z coordinate.
/// @param nx World-space receiver-normal X component.
/// @param ny World-space receiver-normal Y component.
/// @param nz World-space receiver-normal Z component.
/// @return Visibility in [1 - strength, 1]: 1.0 = fully lit. Returns 1.0 (fully lit) when the
///         slot is invalid, outside the shadow frustum, or the context is null.
static float sw_sample_shadow_visibility(const sw_context_t *ctx,
                                         int32_t slot,
                                         float wx,
                                         float wy,
                                         float wz,
                                         float nx,
                                         float ny,
                                         float nz) {
    const float *svp;
    float lx;
    float ly;
    float lz;
    float lw;
    float su;
    float sv;
    float sd;
    int32_t shadow_w;
    int32_t shadow_h;
    int sxi;
    int syi;
    float sz_map;
    float slope_bias;
    float dz_du = 0.0f;
    float dz_dv = 0.0f;
    float slope;
    float occluded_vis;
    float texel_world = 0.0f;
    float row0_len;
    float visibility_sum = 0.0f;
    int sample_count = 0;
    int taps;
    float ign;
    float ca;
    float sa;

    if (!ctx || slot < 0 || slot >= ctx->shadow_count || slot >= VGFX3D_MAX_SHADOW_LIGHTS ||
        !ctx->shadow_complete[slot] || !ctx->shadow_depth[slot] || ctx->shadow_w[slot] <= 0 ||
        ctx->shadow_h[slot] <= 0)
        return 1.0f;

    shadow_w = ctx->shadow_w[slot];
    shadow_h = ctx->shadow_h[slot];
    svp = ctx->shadow_vp[slot];

    /* Normal-offset the receiver by ~1.5 shadow texels in world units (row 0 of the
     * row-major VP maps world X-extent onto clip x in [-1, 1]). */
    row0_len = sw_length3(svp[0], svp[1], svp[2]);
    if (isfinite(row0_len) && row0_len > 1e-9f)
        texel_world = 2.0f / (row0_len * (float)shadow_w);
    wx += nx * texel_world * 1.5f;
    wy += ny * texel_world * 1.5f;
    wz += nz * texel_world * 1.5f;

    lx = wx * svp[0] + wy * svp[1] + wz * svp[2] + svp[3];
    ly = wx * svp[4] + wy * svp[5] + wz * svp[6] + svp[7];
    lz = wx * svp[8] + wy * svp[9] + wz * svp[10] + svp[11];
    lw = wx * svp[12] + wy * svp[13] + wz * svp[14] + svp[15];
    if (lw <= 1e-7f || !isfinite(lw))
        return 1.0f;

    su = (lx / lw) * 0.5f + 0.5f;
    sv = (1.0f - ly / lw) * 0.5f;
    sd = (lz / lw) * 0.5f + 0.5f;
    if (!isfinite(su) || !isfinite(sv) || !isfinite(sd) || su < 0.0f || su >= 1.0f || sv < 0.0f ||
        sv >= 1.0f || sd < 0.0f || sd > 1.0f)
        return 1.0f;

    sxi = sw_unit_coord_to_index(su, shadow_w);
    syi = sw_unit_coord_to_index(sv, shadow_h);
    if (sxi < 0 || sxi >= shadow_w || syi < 0 || syi >= shadow_h)
        return 1.0f;

    sz_map = ctx->shadow_depth[slot][(size_t)syi * (size_t)shadow_w + (size_t)sxi];
    slope_bias = vgfx3d_clamp_float_param(ctx->shadow_bias[slot], -0.05f, 0.05f, 0.0f);
    if (!isfinite(sz_map) || sz_map > FLT_MAX * 0.5f)
        sz_map = 1.0f;
    if (sxi + 1 < shadow_w) {
        float neighbor = ctx->shadow_depth[slot][(size_t)syi * (size_t)shadow_w + (size_t)sxi + 1u];
        if (isfinite(neighbor) && neighbor < FLT_MAX * 0.5f)
            dz_du = neighbor - sz_map;
    }
    if (syi + 1 < shadow_h) {
        float neighbor =
            ctx->shadow_depth[slot][(size_t)(syi + 1) * (size_t)shadow_w + (size_t)sxi];
        if (isfinite(neighbor) && neighbor < FLT_MAX * 0.5f)
            dz_dv = neighbor - sz_map;
    }
    slope = sw_length3(dz_du, dz_dv, 0.0f);
    if (!isfinite(slope))
        slope = 0.0f;
    slope_bias += slope_bias * slope * 4.0f *
                  vgfx3d_clamp_float_param(ctx->shadow_slope_bias, 0.0f, 1000.0f, 1.0f);
    slope_bias = vgfx3d_clamp_float_param(slope_bias, -0.05f, 0.05f, 0.0f);

    occluded_vis = 1.0f - (isfinite(ctx->shadow_strength)
                               ? (ctx->shadow_strength < 0.0f
                                      ? 0.0f
                                      : (ctx->shadow_strength > 1.0f ? 1.0f : ctx->shadow_strength))
                               : 0.85f);
    taps = ctx->shadow_quality <= 0 ? 4 : 8; /* CPU cost bound: BALANCED cap */
    /* Interleaved-gradient noise over the map texel coordinate (fract emulation). */
    {
        double t = 0.06711056 * (double)sxi + 0.00583715 * (double)syi;
        t -= (double)(int64_t)t;
        t *= 52.9829189;
        t -= (double)(int64_t)t;
        ign = (float)t;
    }
    ca = cosf(6.2831853f * ign);
    sa = sinf(6.2831853f * ign);

    for (int i = 0; i < taps; i++) {
        const float *p = sw_shadow_poisson[i * (16 / taps)];
        float ox = (p[0] * ca - p[1] * sa) * 1.5f;
        float oy = (p[0] * sa + p[1] * ca) * 1.5f;
        int px = sxi + (int)(ox >= 0.0f ? ox + 0.5f : ox - 0.5f);
        int py = syi + (int)(oy >= 0.0f ? oy + 0.5f : oy - 0.5f);
        float sample_depth;
        if (px < 0 || px >= shadow_w || py < 0 || py >= shadow_h) {
            visibility_sum += 1.0f;
            sample_count++;
            continue;
        }
        sample_depth = ctx->shadow_depth[slot][(size_t)py * (size_t)shadow_w + (size_t)px];
        if (!isfinite(sample_depth) || sample_depth > FLT_MAX * 0.5f) {
            visibility_sum += 1.0f;
            sample_count++;
            continue;
        }
        visibility_sum += (sd > sample_depth + slope_bias) ? occluded_vis : 1.0f;
        sample_count++;
    }

    return sample_count > 0 ? visibility_sum / (float)sample_count : 1.0f;
}

/// @brief Resolve the concrete shadow-map slot for a light at the given world position.
/// @param ctx Borrowed software context containing camera and shadow-slot state.
/// @param light Borrowed light containing the base slot and projection metadata.
/// @param wx World-space receiver X coordinate.
/// @param wy World-space receiver Y coordinate.
/// @param wz World-space receiver Z coordinate.
/// @return Selected cubemap face or directional cascade slot, or -1 when no valid slot exists.
static int32_t sw_resolve_shadow_slot(
    const sw_context_t *ctx, const vgfx3d_light_params_t *light, float wx, float wy, float wz) {
    int32_t base_slot;
    int32_t cascade_count;
    float view_depth;

    if (!ctx || !light || light->shadow_index < 0)
        return -1;
    base_slot = light->shadow_index;
    if (base_slot >= VGFX3D_MAX_SHADOW_LIGHTS || base_slot >= ctx->shadow_count)
        return -1;
    if (light->shadow_projection_type == VGFX3D_SHADOW_PROJECTION_CUBE) {
        /* Omni shadow: pick the face slot by the dominant axis of light->fragment. */
        float dx = wx - light->position[0];
        float dy = wy - light->position[1];
        float dz = wz - light->position[2];
        float ax;
        float ay = fabsf(dy);
        float az = fabsf(dz);
        int32_t face;
        if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz) ||
            base_slot > VGFX3D_MAX_SHADOW_LIGHTS - VGFX3D_SHADOW_CUBE_FACES ||
            base_slot > ctx->shadow_count - VGFX3D_SHADOW_CUBE_FACES)
            return -1;
        ax = fabsf(dx);
        if (ax >= ay && ax >= az)
            face = dx >= 0.0f ? 0 : 1;
        else if (ay >= az)
            face = dy >= 0.0f ? 2 : 3;
        else
            face = dz >= 0.0f ? 4 : 5;
        return base_slot + face;
    }
    cascade_count = light->shadow_cascade_count;
    if (cascade_count <= 1)
        return base_slot;
    if (cascade_count > VGFX3D_MAX_SHADOW_LIGHTS - base_slot)
        cascade_count = VGFX3D_MAX_SHADOW_LIGHTS - base_slot;
    if (cascade_count > ctx->shadow_count - base_slot)
        cascade_count = ctx->shadow_count - base_slot;
    if (cascade_count <= 1)
        return base_slot;
    {
        float previous_split = 0.0f;
        for (int32_t cascade = 0; cascade < cascade_count; cascade++) {
            float split = light->shadow_cascade_splits[cascade];
            if (!isfinite(split) || split <= previous_split)
                return base_slot;
            previous_split = split;
        }
    }
    view_depth = (wx - ctx->cam_pos[0]) * ctx->cam_forward[0] +
                 (wy - ctx->cam_pos[1]) * ctx->cam_forward[1] +
                 (wz - ctx->cam_pos[2]) * ctx->cam_forward[2];
    if (!isfinite(view_depth))
        return base_slot;
    for (int32_t cascade = 0; cascade < cascade_count - 1; cascade++) {
        if (view_depth <= light->shadow_cascade_splits[cascade])
            return base_slot + cascade;
    }
    return base_slot + cascade_count - 1;
}

/// @brief Effective cascade count for a light after slot/base clamping (mirrors
///        sw_resolve_shadow_slot's bounds).
/// @param ctx Borrowed software context containing the available shadow-slot range.
/// @param light Borrowed light containing base-slot, count, and split metadata.
/// @return Validated cascade count, zero when unavailable, or one when malformed splits force the
///         base-cascade fallback.
static int32_t sw_effective_cascade_count(const sw_context_t *ctx,
                                          const vgfx3d_light_params_t *light) {
    int32_t base_slot;
    int32_t cascade_count;

    if (!ctx || !light || light->shadow_index < 0)
        return 0;
    base_slot = light->shadow_index;
    if (base_slot >= VGFX3D_MAX_SHADOW_LIGHTS || base_slot >= ctx->shadow_count)
        return 0;
    cascade_count = light->shadow_cascade_count;
    if (cascade_count > VGFX3D_MAX_SHADOW_LIGHTS - base_slot)
        cascade_count = VGFX3D_MAX_SHADOW_LIGHTS - base_slot;
    if (cascade_count > ctx->shadow_count - base_slot)
        cascade_count = ctx->shadow_count - base_slot;
    if (cascade_count <= 0)
        return 0;
    {
        float previous_split = 0.0f;
        for (int32_t cascade = 0; cascade < cascade_count; cascade++) {
            float split = light->shadow_cascade_splits[cascade];
            if (!isfinite(split) || split <= previous_split)
                return 1;
            previous_split = split;
        }
    }
    return cascade_count;
}

/// @brief Sample a light's shadow visibility with cascade blending and distance fade.
/// @details Mirrors the GPU shader twins: the last ~12% of each cascade cross-fades
///   into the next so the cascade handoff cannot pop as the camera moves, and the last
///   cascade fades to fully lit approaching the capped shadow distance instead of
///   ending on a hard line. Cube/spot projections resolve exactly as before.
/// @param ctx Borrowed software context containing camera and shadow-map state.
/// @param light Borrowed light containing projection and cascade metadata.
/// @param wx World-space receiver X coordinate.
/// @param wy World-space receiver Y coordinate.
/// @param wz World-space receiver Z coordinate.
/// @param nx World-space receiver-normal X component.
/// @param ny World-space receiver-normal Y component.
/// @param nz World-space receiver-normal Z component.
/// @return Shadow visibility in `[0, 1]`, with one used for absent or invalid shadows.
static float sw_sample_shadow_light(const sw_context_t *ctx,
                                    const vgfx3d_light_params_t *light,
                                    float wx,
                                    float wy,
                                    float wz,
                                    float nx,
                                    float ny,
                                    float nz) {
    int32_t slot = sw_resolve_shadow_slot(ctx, light, wx, wy, wz);
    float vis;
    int32_t base_slot;
    int32_t cascade_count;
    int32_t cascade;
    float view_depth;
    float split_far;
    float split_near;
    float band;

    if (slot < 0)
        return 1.0f;
    vis = sw_sample_shadow_visibility(ctx, slot, wx, wy, wz, nx, ny, nz);
    if (!light || light->shadow_projection_type != VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC)
        return vis;
    base_slot = light->shadow_index;
    cascade_count = sw_effective_cascade_count(ctx, light);
    if (cascade_count <= 1 || slot < base_slot)
        return vis;
    cascade = slot - base_slot;
    if (cascade >= cascade_count)
        return vis;
    view_depth = (wx - ctx->cam_pos[0]) * ctx->cam_forward[0] +
                 (wy - ctx->cam_pos[1]) * ctx->cam_forward[1] +
                 (wz - ctx->cam_pos[2]) * ctx->cam_forward[2];
    if (!isfinite(view_depth))
        return vis;
    split_far = light->shadow_cascade_splits[cascade];
    split_near = cascade > 0 ? light->shadow_cascade_splits[cascade - 1] : 0.0f;
    if (!isfinite(split_far) || !isfinite(split_near) || split_far <= split_near)
        return vis;
    band = fmaxf(0.5f, 0.12f * (split_far - split_near));
    if (cascade < cascade_count - 1) {
        float blend = (view_depth - (split_far - band)) / band;
        if (blend > 0.0f) {
            float next_vis;
            if (blend > 1.0f)
                blend = 1.0f;
            next_vis = sw_sample_shadow_visibility(ctx, slot + 1, wx, wy, wz, nx, ny, nz);
            vis += (next_vis - vis) * blend;
        }
    } else {
        float shadow_far = light->shadow_cascade_splits[cascade_count - 1];
        if (!isfinite(shadow_far) || shadow_far <= 0.0f)
            return vis;
        float fade_band = fmaxf(2.0f, 0.10f * shadow_far);
        float fade = (shadow_far - view_depth) / fade_band;
        if (fade < 0.0f)
            fade = 0.0f;
        if (fade > 1.0f)
            fade = 1.0f;
        vis = 1.0f + (vis - 1.0f) * fade;
    }
    return vis;
}

/// @brief Return whether a draw must defer legacy lighting to the fragment stage.
/// @details PBR and normal maps are always per-pixel. Legacy materials also become
///          per-pixel when at least one referenced shadow slot is complete; otherwise
///          applying Gouraud lighting first and Phong lighting again during shadow
///          sampling squares the light contribution.
/// @param ctx Borrowed software context containing available shadow slots.
/// @param cmd Borrowed draw command containing material workflow and normal-map state.
/// @param lights Borrowed light array referenced by the draw.
/// @param light_count Number of entries supplied in @p lights.
/// @return Non-zero when lighting must run per pixel, otherwise zero.
static int sw_draw_requires_per_pixel_lighting(const sw_context_t *ctx,
                                               const vgfx3d_draw_cmd_t *cmd,
                                               const vgfx3d_light_params_t *lights,
                                               int32_t light_count) {
    if (!cmd)
        return 0;
    if (cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR || cmd->normal_map)
        return 1;
    if (!ctx || ctx->shadow_count <= 0)
        return 0;
    light_count = sw_sanitize_light_count(lights, light_count);
    for (int32_t i = 0; i < light_count; i++) {
        const vgfx3d_light_params_t *light = &lights[i];
        int32_t first = light->shadow_index;
        int32_t slot_count = 1;
        if (light->type != 0 && light->type != 1 && light->type != 3 && light->type != 4 &&
            light->type != 5)
            continue;
        if (first < 0 || first >= ctx->shadow_count || first >= VGFX3D_MAX_SHADOW_LIGHTS)
            continue;
        if (light->shadow_projection_type == VGFX3D_SHADOW_PROJECTION_CUBE)
            slot_count = VGFX3D_SHADOW_CUBE_FACES;
        else if (light->shadow_projection_type == VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC &&
                 light->shadow_cascade_count > 1)
            slot_count = light->shadow_cascade_count;
        if (slot_count > VGFX3D_MAX_SHADOW_LIGHTS - first)
            slot_count = VGFX3D_MAX_SHADOW_LIGHTS - first;
        if (slot_count > ctx->shadow_count - first)
            slot_count = ctx->shadow_count - first;
        for (int32_t offset = 0; offset < slot_count; offset++) {
            int32_t slot = first + offset;
            if (ctx->shadow_complete[slot] && ctx->shadow_depth[slot])
                return 1;
        }
    }
    return 0;
}

/// @brief Queue (and immediately answer) one scene-depth probe from the CPU z-buffer.
/// @details Software depth is available synchronously, so the probe result is captured
///   at queue time: NDC z converted to the hook's window-depth convention
///   ([0, 1], cleared/empty depth reported as 1.0). Returns the slot id or -1.
/// @param ctx_ptr Opaque borrowed pointer to the active @c sw_context_t.
/// @param ndc_x Horizontal normalized-device coordinate to sample.
/// @param ndc_y Vertical normalized-device coordinate to sample.
/// @return Frame-local probe slot, or -1 when the context or probe capacity is unavailable.
static int32_t sw_queue_depth_probe(void *ctx_ptr, float ndc_x, float ndc_y) {
    sw_context_t *ctx = (sw_context_t *)ctx_ptr;
    const float *depth = NULL;
    int32_t depth_width = 0;
    int32_t depth_height = 0;
    int32_t slot;
    float result = -1.0f;

    if (!ctx || ctx->depth_probe_count >= VGFX3D_DEPTH_PROBE_MAX)
        return -1;
    slot = ctx->depth_probe_count++;
    if (ctx->render_target) {
        depth = ctx->render_target->depth_buf;
        depth_width = ctx->render_target->width;
        depth_height = ctx->render_target->height;
    } else {
        depth = ctx->zbuf;
        depth_width = ctx->width;
        depth_height = ctx->height;
    }
    if (depth && depth_width > 0 && depth_height > 0 && isfinite(ndc_x) && isfinite(ndc_y)) {
        double bounded_x = (double)vgfx3d_clamp_float_param(ndc_x, -1.0f, 1.0f, 0.0f);
        double bounded_y = (double)vgfx3d_clamp_float_param(ndc_y, -1.0f, 1.0f, 0.0f);
        int32_t px = (int32_t)((bounded_x * 0.5 + 0.5) * (double)depth_width);
        int32_t py = (int32_t)((0.5 - bounded_y * 0.5) * (double)depth_height);
        if (px >= depth_width)
            px = depth_width - 1;
        if (py >= depth_height)
            py = depth_height - 1;
        if (px >= 0 && px < depth_width && py >= 0 && py < depth_height) {
            float ndc_z = depth[(size_t)py * (size_t)depth_width + (size_t)px];
            if (!isfinite(ndc_z) || ndc_z > 1.0f)
                result = 1.0f; /* cleared: nothing occludes here */
            else
                result = ndc_z * 0.5f + 0.5f;
            if (result < 0.0f)
                result = 0.0f;
            if (result > 1.0f)
                result = 1.0f;
        }
    }
    ctx->depth_probe_results[slot] = result;
    return slot;
}

/// @brief Read a probe captured this frame (software answers synchronously).
/// @param ctx_ptr Opaque borrowed pointer to the active @c sw_context_t.
/// @param slot Frame-local slot previously returned by @c sw_queue_depth_probe().
/// @return Captured window-space depth in `[0, 1]`, or -1 for an invalid slot.
static float sw_read_depth_probe(void *ctx_ptr, int32_t slot) {
    sw_context_t *ctx = (sw_context_t *)ctx_ptr;
    if (!ctx || slot < 0 || slot >= ctx->depth_probe_count)
        return -1.0f;
    return ctx->depth_probe_results[slot];
}

/// @brief Resize the depth buffer if dimensions changed; idempotent on match.
///
/// Reallocates `width × height` floats. Used during resize and on
/// first frame after context creation. Returns 0 on overflow / OOM.
/// @param ctx Borrowed software context that owns the depth buffer.
/// @param width Positive required width in pixels.
/// @param height Positive required height in pixels.
/// @return 1 when a matching buffer is ready, otherwise 0 for invalid dimensions, overflow, or
///         allocation failure.
static int sw_ensure_zbuf_capacity(sw_context_t *ctx, int32_t width, int32_t height) {
    size_t allocation_count;
    size_t pixel_count;

    if (!ctx || width <= 0 || height <= 0)
        return 0;
    if (ctx->zbuf && ctx->width == width && ctx->height == height)
        return 1;

    if ((size_t)width > SIZE_MAX / (size_t)height)
        return 0;
    pixel_count = (size_t)width * (size_t)height;
    if (!sw_geometric_capacity(ctx->zbuf_capacity, pixel_count, sizeof(float), &allocation_count))
        return 0;

    if (ctx->zbuf_capacity < pixel_count) {
        float *new_zbuf = (float *)realloc(ctx->zbuf, allocation_count * sizeof(float));
        if (!new_zbuf)
            return 0;

        ctx->zbuf = new_zbuf;
        ctx->zbuf_capacity = allocation_count;
    }
    ctx->width = width;
    ctx->height = height;
    return 1;
}

/// @brief Clamp a software-rasterizer worker count to [1, SW_MAX_WORKERS].
/// @param count Requested worker count.
/// @return @p count constrained to `[1, SW_MAX_WORKERS]`.
static int64_t sw_clamp_worker_count(int64_t count) {
    if (count < 1)
        return 1;
    if (count > SW_MAX_WORKERS)
        return SW_MAX_WORKERS;
    return count;
}

/// @brief Default software-rasterizer worker count: hardware parallelism capped to SW_MAX_WORKERS.
/// @return Hardware-derived worker count constrained to the software backend limits.
static int64_t sw_default_worker_count(void) {
    return sw_clamp_worker_count(rt_parallel_default_workers());
}

/// @brief Resolve ZANNA_3D_SW_THREADS into a deterministic worker budget.
/// @return Valid environment override constrained to backend limits, or the hardware-derived
///         default when the environment value is absent or malformed.
static int64_t sw_resolve_worker_count_from_env(void) {
    const char *env = getenv("ZANNA_3D_SW_THREADS");
    char *end = NULL;
    long long parsed;
    if (!env || !*env)
        return sw_default_worker_count();
    errno = 0;
    parsed = strtoll(env, &end, 10);
    if (errno == ERANGE || end == env || (end && *end != '\0'))
        return sw_default_worker_count();
    return sw_clamp_worker_count((int64_t)parsed);
}

/// @brief Create the context-owned worker pool unless the requested count is one.
/// @param ctx Borrowed software context that receives the pool and resolved worker count; null is
///            a no-op.
static void sw_init_worker_pool(sw_context_t *ctx) {
    if (!ctx)
        return;
    ctx->worker_count = sw_resolve_worker_count_from_env();
    ctx->worker_pool = NULL;
    if (ctx->worker_count <= 1)
        return;
    ctx->worker_pool = rt_threadpool_new(ctx->worker_count);
    if (!ctx->worker_pool)
        ctx->worker_count = 1;
}

/// @brief Shut down and release the context-owned worker pool.
/// @param ctx Borrowed software context whose pool is detached and released; null or a missing
///            pool is a no-op.
static void sw_release_worker_pool(sw_context_t *ctx) {
    if (!ctx || !ctx->worker_pool)
        return;
    void *pool = ctx->worker_pool;
    ctx->worker_pool = NULL;
    ctx->worker_count = 1;
    rt_threadpool_shutdown(pool);
    if (rt_obj_release_check0(pool))
        rt_obj_free(pool);
}

/// @brief Test-only probe for unit tests; not part of the script-facing API.
/// @param ctx_ptr Opaque borrowed pointer to a software context.
/// @return The context's worker count constrained to backend limits, or one for a null context.
int64_t vgfx3d_software_backend_thread_count_for_test(const void *ctx_ptr) {
    const sw_context_t *ctx = (const sw_context_t *)ctx_ptr;
    return ctx ? sw_clamp_worker_count(ctx->worker_count) : 1;
}

/*==========================================================================
 * Matrix helpers
 *=========================================================================*/

/// @brief Row-major 4×4 matrix multiply: `out = a * b`.
/// @param a Borrowed first 16-float matrix.
/// @param b Borrowed second 16-float matrix.
/// @param out Receives the 16-float product and must not alias an input.
static void mat4f_mul(const float *a, const float *b, float *out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

/// @brief Transform a homogeneous 4-vector by a row-major 4×4 matrix.
/// @param m Borrowed 16-float row-major matrix.
/// @param in Borrowed four-component input vector.
/// @param out Receives the four transformed components and must not alias @p in.
static void mat4f_transform4(const float *m, const float *in, float *out) {
    out[0] = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3] * in[3];
    out[1] = m[4] * in[0] + m[5] * in[1] + m[6] * in[2] + m[7] * in[3];
    out[2] = m[8] * in[0] + m[9] * in[1] + m[10] * in[2] + m[11] * in[3];
    out[3] = m[12] * in[0] + m[13] * in[1] + m[14] * in[2] + m[15] * in[3];
}

/// @brief Compute a positive width*height pixel count without overflowing size_t.
/// @details Software backend render targets use signed 32-bit dimensions, while
///          depth/color buffer loops use `size_t`. This helper rejects invalid or
///          overflowing dimensions before callers clear linear buffers.
/// @param width Positive signed pixel width.
/// @param height Positive signed pixel height.
/// @param out_count Receives the pixel count on success or zero on failure; may be null.
/// @return 1 when the dimensions and corresponding float-buffer size are representable,
///         otherwise 0.
static int sw_pixel_count_checked(int32_t width, int32_t height, size_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (!out_count || width <= 0 || height <= 0)
        return 0;
    if ((size_t)width > SIZE_MAX / (size_t)height)
        return 0;
    *out_count = (size_t)width * (size_t)height;
    if (*out_count > SIZE_MAX / sizeof(float)) {
        *out_count = 0;
        return 0;
    }
    return 1;
}

// clang-format off
/* These implementation fragments have type/function dependencies. */
#include "vgfx3d_backend_sw_texture.inc"
#include "vgfx3d_backend_sw_vertex.inc"
#include "vgfx3d_backend_sw_raster.inc"
#include "vgfx3d_backend_sw_shadow.inc"
#include "vgfx3d_backend_sw_vtable.inc"
// clang-format on

/* Test-only robustness probes. These mirror the existing worker-count probe and
 * deliberately stay out of the script/runtime API registry. */
/// @brief Expose the software backend's unit-interval clamp to native robustness tests.
/// @param value Floating-point value to clamp.
/// @return @p value constrained to `[0, 1]`, with NaN mapped to zero.
float vgfx3d_software_backend_clamp01_for_test(float value) {
    return clamp01f(value);
}

/// @brief Expose sanitized material exponentiation to native robustness tests.
/// @param value Material term clamped to `[0, 1]`.
/// @param power Requested exponent subject to the production sanitizer.
/// @return The same bounded power result used by software material shading.
float vgfx3d_software_backend_material_pow_for_test(float value, float power) {
    return sw_material_powf(value, power);
}

/// @brief Expose software texture-index wrapping to native robustness tests.
/// @param index Signed source index to wrap or clamp.
/// @param size Positive texture-axis extent.
/// @param mode Runtime texture addressing mode.
/// @return The production sampler's resolved integer texel index.
int32_t vgfx3d_software_backend_wrap_index_for_test(int64_t index, int32_t size, int32_t mode) {
    return sw_wrap_index(index, size, mode);
}

/// @brief Expose normalized-coordinate conversion to native robustness tests.
/// @param value Normalized coordinate to convert.
/// @param extent Positive texture-axis extent.
/// @return The production sampler's valid integer index.
int32_t vgfx3d_software_backend_unit_coord_index_for_test(float value, int32_t extent) {
    return sw_unit_coord_to_index(value, extent);
}

/// @brief Expose perspective-correct barycentric reweighting to native robustness tests.
/// @param b0 First screen-space barycentric weight.
/// @param b1 Second screen-space barycentric weight.
/// @param b2 Third screen-space barycentric weight.
/// @param inv_w0 Reciprocal clip-space W for the first vertex.
/// @param inv_w1 Reciprocal clip-space W for the second vertex.
/// @param inv_w2 Reciprocal clip-space W for the third vertex.
/// @param out_b0 Receives the corrected first weight.
/// @param out_b1 Receives the corrected second weight.
/// @param out_b2 Receives the corrected third weight.
/// @return Non-zero when finite corrected weights were produced, otherwise zero.
int32_t vgfx3d_software_backend_perspective_weights_for_test(float b0,
                                                             float b1,
                                                             float b2,
                                                             float inv_w0,
                                                             float inv_w1,
                                                             float inv_w2,
                                                             float *out_b0,
                                                             float *out_b1,
                                                             float *out_b2) {
    return sw_perspective_attribute_weights(
        b0, b1, b2, inv_w0, inv_w1, inv_w2, out_b0, out_b1, out_b2);
}

/// @brief Expose RGBA8 surface validation to native robustness tests.
/// @param pixels Borrowed surface pointer.
/// @param width Signed pixel width.
/// @param height Signed pixel height.
/// @param stride Signed row stride in bytes.
/// @return Non-zero when the surface layout is valid and representable, otherwise zero.
int32_t vgfx3d_software_backend_rgba_surface_valid_for_test(const uint8_t *pixels,
                                                            int32_t width,
                                                            int32_t height,
                                                            int32_t stride) {
    return sw_rgba_surface_is_valid(pixels, width, height, stride);
}

/*==========================================================================
 * Exported backend + selection
 *=========================================================================*/

/// @brief Read-only view of the software rasterizer's NDC depth buffer for the
///   CPU post-FX scene effects. Returns NULL for non-software contexts, when a
///   render target is bound (its own depth_buf is used instead), or before the
///   first frame allocates the buffer.
/// @param ctx_ptr Opaque borrowed pointer to a software backend context.
/// @param out_w Optional output receiving the depth-buffer width, reset to zero on failure.
/// @param out_h Optional output receiving the depth-buffer height, reset to zero on failure.
/// @return Borrowed context-owned depth-buffer storage, or null when unavailable.
const float *vgfx3d_sw_get_zbuf(void *ctx_ptr, int32_t *out_w, int32_t *out_h) {
    sw_context_t *ctx = (sw_context_t *)ctx_ptr;
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    if (!ctx || ctx->render_target || !ctx->zbuf || ctx->width <= 0 || ctx->height <= 0)
        return NULL;
    if (out_w)
        *out_w = ctx->width;
    if (out_h)
        *out_h = ctx->height;
    return ctx->zbuf;
}

const vgfx3d_backend_t vgfx3d_software_backend = {
    .name = "software",
    .create_ctx = sw_create_ctx,
    .destroy_ctx = sw_destroy_ctx,
    .clear = sw_clear,
    /* CPU sampling is index-based with macro-sized slot arrays, so the
     * software backend supports the full 12-slot shadow budget directly. */
    .shadow_atlas_slots = 1,
    .resize = sw_resize,
    .begin_frame = sw_begin_frame,
    .submit_draw = sw_submit_draw,
    .submit_draw_instanced = sw_submit_draw_instanced,
    .end_frame = sw_end_frame,
    .set_render_target = sw_set_render_target,
    .shadow_begin = sw_shadow_begin,
    .shadow_draw = sw_shadow_draw,
    .shadow_end = sw_shadow_end,
    .shadow_reuse = sw_shadow_reuse,
    .present = NULL, /* software renders to CPU framebuffer; vgfx_update handles display */
    .show_gpu_layer = NULL,
    .hide_gpu_layer = NULL,
    .resolve_opaque_targets = sw_resolve_opaque_targets,
    .queue_depth_probe = sw_queue_depth_probe,
    .read_depth_probe = sw_read_depth_probe,
    .set_render_scale = sw_set_render_scale,
};

/// @brief Resolve an exact backend name among implementations compiled for this platform.
/// @param name Null-terminated backend name such as @c software, @c metal, @c d3d11, or
///             @c opengl.
/// @return Address of the matching static backend descriptor, or null when unavailable.
static const vgfx3d_backend_t *vgfx3d_backend_from_name(const char *name) {
    if (!name)
        return NULL;
    if (strcmp(name, "software") == 0)
        return &vgfx3d_software_backend;
#if RT_PLATFORM_MACOS
    if (strcmp(name, "metal") == 0)
        return &vgfx3d_metal_backend;
#elif RT_PLATFORM_WINDOWS
    if (strcmp(name, "d3d11") == 0)
        return &vgfx3d_d3d11_backend;
#elif RT_PLATFORM_LINUX && !defined(ZANNA_GRAPHICS_HEADLESS)
    if (strcmp(name, "opengl") == 0)
        return &vgfx3d_opengl_backend;
#endif
    return NULL;
}

/// @brief Public: pick the best 3D backend at startup.
///
/// Tries each compiled-in backend in priority order (D3D11 on
/// Windows, Metal on macOS, OpenGL on Linux), falling back to the
/// software backend when no GPU backend is available. Called once
/// at canvas-create time and the choice is cached.
/// @return Address of the selected static backend descriptor; always falls back to software.
const vgfx3d_backend_t *vgfx3d_select_backend(void) {
    vgfx3d_backend_platform_t platform = VGFX3D_BACKEND_PLATFORM_OTHER;
    const vgfx3d_backend_t *backend;

    /* Only honor overrides for backends compiled on this platform. */
    const char *env = getenv("ZANNA_3D_BACKEND");
    if (env) {
        backend = vgfx3d_backend_from_name(env);
        if (backend)
            return backend;
    }

    /* Prefer platform GPU backends by default; Canvas3D falls back to the
     * software backend at context creation when the GPU path is unavailable. */
#if RT_PLATFORM_MACOS
    platform = VGFX3D_BACKEND_PLATFORM_MACOS;
#elif RT_PLATFORM_WINDOWS
#if defined(_M_ARM64) || defined(__aarch64__)
    /* Several Windows-on-ARM GPU stacks expose D3D11 but crash inside the
     * display driver during Present. Keep x64 on D3D11, but default ARM64 to
     * the portable backend so Canvas3D demos launch reliably. Users can still
     * opt into D3D11 with ZANNA_3D_BACKEND=d3d11. */
    platform = VGFX3D_BACKEND_PLATFORM_WINDOWS_ARM64;
#else
    platform = VGFX3D_BACKEND_PLATFORM_WINDOWS;
#endif
#elif RT_PLATFORM_LINUX && !defined(ZANNA_GRAPHICS_HEADLESS)
    platform = VGFX3D_BACKEND_PLATFORM_LINUX;
#endif

    backend = vgfx3d_backend_from_name(vgfx3d_default_backend_name_for_platform(platform));
    return backend ? backend : &vgfx3d_software_backend;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
