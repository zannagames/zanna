//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_utils.c
// Purpose: Cross-backend utility helpers shared between the Metal/OpenGL/D3D11
//   3D backends — pixel/cubemap unpack to RGBA8, generation tracking,
//   row-flip, scaled scene-target sizing, normal-matrix derivation from a model
//   matrix, and 4×4 inverse.
//
// Key invariants:
//   - Pixels payloads are 0xRRGGBBAA in `uint32_t`, row-major, top-left origin.
//   - Normal matrix is the inverse-transpose of the model matrix's upper 3×3,
//     stored in the upper-left 3×3 of the 4×4 output (M[15] = 1, rest 0).
//   - Cubemap generation mixes a stable cubemap identity plus all six face
//     generations, enabling cheap "did anything change?" checks for backend
//     caches even when the allocator reuses object addresses.
//
// Ownership/Lifetime:
//   - Object arguments and unpacked source bytes are borrowed for each call.
//   - Callers own all destination buffers; helpers retain no backend resources.
//
// Links: vgfx3d_backend_utils.h, vgfx3d_backend_*.c (per-API implementations)
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_backend_utils.c
 * @brief Implements backend-neutral validation, image conversion, upload pacing, matrix, and
 *        compact-vertex helpers for Graphics3D.
 *
 * These stateless routines form the defensive boundary shared by the software, OpenGL, Metal,
 * and D3D11 backends. They sanitize render snapshots, validate compressed texture layouts,
 * convert Pixels and cubemap data into uploadable formats, perform checked color conversions,
 * derive matrix data, and encode compact vertex streams without retaining caller-owned objects.
 */

#include "vgfx3d_backend_utils.h"

#include "rt_canvas3d.h"
#include "rt_textureasset3d.h"
#include "vgfx3d_backend.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Determinant magnitude below which a 3x3/4x4 matrix is treated as singular.
 * Shared by the normal-matrix derivation and the 4x4 inverse so both agree on
 * the bound; the value matches the inverse's long-standing threshold and stays
 * permissive enough not to drop the rotation of legitimately small-scaled
 * (sub-0.01) objects, whose normal matrix is renormalized after derivation. */
static const float kVgfx3dSingularDetEps = 1e-12f;

#define VGFX3D_BACKEND_MAX_CUBEMAP_FACE_SIZE 32768

typedef struct {
    int64_t w;
    int64_t h;
    uint32_t *data;
    uint64_t generation;
    uint64_t cache_identity;
} vgfx3d_pixels_view_t;

typedef struct {
    void *vptr;
    void *faces[6];
    int64_t face_size;
    uint64_t cache_identity;
} vgfx3d_cubemap_view_t;

/// @brief Store the row-major 4x4 identity matrix.
/// @param dst Receives 16 row-major float elements; must be non-null.
static void vgfx3d_store_identity4x4(float *dst) {
    memset(dst, 0, sizeof(float) * 16u);
    dst[0] = 1.0f;
    dst[5] = 1.0f;
    dst[10] = 1.0f;
    dst[15] = 1.0f;
}

/// @brief Return non-zero only when every input lane is finite and bounded.
/// @param values Borrowed float array; may be null only when @p count is zero.
/// @param count Number of lanes to validate.
/// @param abs_max Finite non-negative inclusive absolute bound.
/// @return Non-zero when every lane is finite and within `[-abs_max, abs_max]`, otherwise zero.
int vgfx3d_float_array_is_bounded(const float *values, size_t count, float abs_max) {
    if ((!values && count > 0) || !isfinite(abs_max) || abs_max < 0.0f)
        return 0;
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(values[i]) || values[i] < -abs_max || values[i] > abs_max)
            return 0;
    }
    return 1;
}

/// @brief Copy float constants while replacing non-finite lanes with a stable value.
/// @param dst Receives @p count float values; null is a no-op.
/// @param src Optional borrowed source array; a null source uses the fallback for every lane.
/// @param count Number of lanes to copy.
/// @param fallback Replacement for non-finite lanes; non-finite fallback input becomes zero.
void vgfx3d_copy_float_array_finite_or(float *dst, const float *src, size_t count, float fallback) {
    float safe_fallback = isfinite(fallback) ? fallback : 0.0f;
    if (!dst)
        return;
    for (size_t i = 0; i < count; i++)
        dst[i] = src && isfinite(src[i]) ? src[i] : safe_fallback;
}

/// @brief Copy a bounded matrix or replace it with identity.
/// @param dst Receives a 16-float row-major matrix; null is a no-op.
/// @param src Optional borrowed matrix accepted only when every component is finite and bounded.
void vgfx3d_copy_mat4_finite_or_identity(float *dst, const float *src) {
    if (!dst)
        return;
    if (vgfx3d_float_array_is_bounded(src, 16u, VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX)) {
        memcpy(dst, src, sizeof(float) * 16u);
        return;
    }
    vgfx3d_store_identity4x4(dst);
}

/// @brief Copy a bounded matrix, then a bounded fallback, or identity.
/// @param dst Receives a 16-float row-major matrix; null is a no-op.
/// @param src Preferred optional borrowed matrix.
/// @param fallback Secondary optional borrowed matrix used when @p src is unusable.
void vgfx3d_copy_mat4_finite_or(float *dst, const float *src, const float *fallback) {
    if (!dst)
        return;
    if (vgfx3d_float_array_is_bounded(src, 16u, VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX)) {
        memcpy(dst, src, sizeof(float) * 16u);
        return;
    }
    if (vgfx3d_float_array_is_bounded(fallback, 16u, VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX)) {
        memcpy(dst, fallback, sizeof(float) * 16u);
        return;
    }
    vgfx3d_store_identity4x4(dst);
}

/// @brief Validate a bounded shadow matrix with at least one useful component.
/// @param matrix Borrowed 16-float row-major matrix.
/// @return Non-zero when all components are bounded and at least one has meaningful magnitude.
int vgfx3d_shadow_matrix_is_usable(const float *matrix) {
    float max_abs = 0.0f;

    if (!vgfx3d_float_array_is_bounded(matrix, 16u, VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX)) {
        return 0;
    }
    for (size_t i = 0; i < 16u; i++) {
        float magnitude = fabsf(matrix[i]);
        if (magnitude > max_abs)
            max_abs = magnitude;
    }
    return max_abs > 1.0e-12f ? 1 : 0;
}

/// @brief Copy and normalize a direction with deterministic fallback semantics.
/// @param dst Receives the normalized three-component direction; null is a no-op.
/// @param src Preferred optional borrowed direction.
/// @param fallback Secondary borrowed direction, followed by the built-in negative-Z default.
void vgfx3d_copy_vec3_direction_or(float *dst, const float *src, const float fallback[3]) {
    static const float kDefaultDirection[3] = {0.0f, 0.0f, -1.0f};
    const float *chosen = NULL;
    double length_squared;
    double inverse_length;

    if (!dst)
        return;
    if (vgfx3d_float_array_is_bounded(src, 3u, VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX)) {
        length_squared = (double)src[0] * (double)src[0] + (double)src[1] * (double)src[1] +
                         (double)src[2] * (double)src[2];
        if (length_squared > 1.0e-12 && length_squared < 1.0e24)
            chosen = src;
    }
    if (!chosen &&
        vgfx3d_float_array_is_bounded(fallback, 3u, VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX)) {
        length_squared = (double)fallback[0] * (double)fallback[0] +
                         (double)fallback[1] * (double)fallback[1] +
                         (double)fallback[2] * (double)fallback[2];
        if (length_squared > 1.0e-12 && length_squared < 1.0e24)
            chosen = fallback;
    }
    if (!chosen)
        chosen = kDefaultDirection;
    length_squared = (double)chosen[0] * (double)chosen[0] + (double)chosen[1] * (double)chosen[1] +
                     (double)chosen[2] * (double)chosen[2];
    inverse_length = 1.0 / sqrt(length_squared);
    dst[0] = (float)((double)chosen[0] * inverse_length);
    dst[1] = (float)((double)chosen[1] * inverse_length);
    dst[2] = (float)((double)chosen[2] * inverse_length);
}

/// @brief Make a sanitized secondary direction perpendicular to a sanitized primary direction.
/// @details Preserves the caller's secondary direction whenever it spans a usable plane with the
///          primary direction. Parallel, antiparallel, non-finite, and near-zero inputs select the
///          least-aligned Cartesian axis before Gram-Schmidt projection, producing a deterministic
///          right-angle unit pair without overflow-prone cross products.
/// @param primary Borrowed normalized primary direction.
/// @param requested Preferred optional secondary direction.
/// @param fallback Secondary fallback used before the deterministic Cartesian-axis fallback.
/// @param dst Receives a normalized direction perpendicular to @p primary.
static void vgfx3d_make_perpendicular_direction(const float primary[3],
                                                const float requested[3],
                                                const float fallback[3],
                                                float dst[3]) {
    float secondary[3];
    float projected[3];
    const float axes[3][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    double dot;
    double length_squared;
    double inverse_length;

    vgfx3d_copy_vec3_direction_or(secondary, requested, fallback);
    dot = (double)primary[0] * (double)secondary[0] + (double)primary[1] * (double)secondary[1] +
          (double)primary[2] * (double)secondary[2];
    for (int32_t lane = 0; lane < 3; lane++)
        projected[lane] = (float)((double)secondary[lane] - dot * (double)primary[lane]);
    length_squared = (double)projected[0] * (double)projected[0] +
                     (double)projected[1] * (double)projected[1] +
                     (double)projected[2] * (double)projected[2];
    if (!isfinite(length_squared) || length_squared <= 1.0e-12) {
        int32_t axis = 0;
        if (fabsf(primary[1]) < fabsf(primary[axis]))
            axis = 1;
        if (fabsf(primary[2]) < fabsf(primary[axis]))
            axis = 2;
        dot = (double)primary[axis];
        for (int32_t lane = 0; lane < 3; lane++)
            projected[lane] = axes[axis][lane] - (float)(dot * (double)primary[lane]);
        length_squared = (double)projected[0] * (double)projected[0] +
                         (double)projected[1] * (double)projected[1] +
                         (double)projected[2] * (double)projected[2];
    }
    inverse_length = 1.0 / sqrt(length_squared);
    for (int32_t lane = 0; lane < 3; lane++)
        dst[lane] = (float)((double)projected[lane] * inverse_length);
}

/// @brief Select a finite requested value with deterministic fallback semantics.
/// @param requested Preferred value.
/// @param fallback Secondary value used when @p requested is non-finite.
/// @return @p requested when finite, otherwise finite @p fallback, otherwise zero.
float vgfx3d_finite_or(float requested, float fallback) {
    return isfinite(requested) ? requested : (isfinite(fallback) ? fallback : 0.0f);
}

/// @brief Sanitize and clamp a float while tolerating non-finite or inverted bounds.
/// @param requested Preferred value to constrain.
/// @param min_value Requested lower bound.
/// @param max_value Requested upper bound.
/// @param fallback Replacement for non-finite requested input and unusable bounds.
/// @return Finite value inside the normalized inclusive bound pair.
float vgfx3d_clamp_float_param(float requested, float min_value, float max_value, float fallback) {
    float safe_fallback = isfinite(fallback) ? fallback : 0.0f;
    float temporary;
    if (!isfinite(requested))
        requested = safe_fallback;
    if (!isfinite(min_value) && !isfinite(max_value)) {
        min_value = safe_fallback;
        max_value = safe_fallback;
    } else if (!isfinite(min_value)) {
        min_value = max_value;
    } else if (!isfinite(max_value)) {
        max_value = min_value;
    }
    if (min_value > max_value) {
        temporary = min_value;
        min_value = max_value;
        max_value = temporary;
    }
    if (requested < min_value)
        return min_value;
    if (requested > max_value)
        return max_value;
    return requested;
}

/// @brief Normalize material workflow constants before backend shader dispatch.
/// @param requested Caller-provided workflow enum value.
/// @return PBR for the exact PBR constant, otherwise the legacy workflow.
int32_t vgfx3d_sanitize_material_workflow(int32_t requested) {
    return requested == RT_MATERIAL3D_WORKFLOW_PBR ? RT_MATERIAL3D_WORKFLOW_PBR
                                                   : RT_MATERIAL3D_WORKFLOW_LEGACY;
}

/// @brief Normalize alpha-mode constants before draw-state and shader upload.
/// @param requested Caller-provided alpha-mode enum value.
/// @return A valid opaque, mask, or blend mode; malformed input becomes opaque.
int32_t vgfx3d_sanitize_alpha_mode(int32_t requested) {
    if (requested < RT_MATERIAL3D_ALPHA_MODE_OPAQUE || requested > RT_MATERIAL3D_ALPHA_MODE_BLEND)
        return RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    return requested;
}

/// @brief Normalize Game3D shading-model constants before shader upload.
/// @param requested Caller-provided shading-model value.
/// @return Value in the shader-visible range `[0, 5]`, defaulting to zero.
int32_t vgfx3d_sanitize_shading_model(int32_t requested) {
    return requested >= 0 && requested <= 5 ? requested : 0;
}

/// @brief Normalize shadow-mode constants before shadow submission decisions.
/// @param requested Caller-provided material shadow-mode value.
/// @return Valid auto, none, or cast value; malformed input becomes auto.
int32_t vgfx3d_sanitize_shadow_mode(int32_t requested) {
    if (requested < RT_MATERIAL3D_SHADOW_MODE_AUTO || requested > RT_MATERIAL3D_SHADOW_MODE_CAST)
        return RT_MATERIAL3D_SHADOW_MODE_AUTO;
    return requested;
}

/// @brief Normalize texture-wrap constants before sampler selection and CPU sampling.
/// @param requested Caller-provided texture addressing value.
/// @return Valid repeat, clamp-to-edge, or mirrored-repeat value; malformed input becomes repeat.
int32_t vgfx3d_sanitize_texture_wrap(int32_t requested) {
    if (requested == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE ||
        requested == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
        return requested;
    return RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
}

/// @brief Normalize texture-filter constants before sampler selection and CPU sampling.
/// @param requested Caller-provided texture filter value.
/// @return Nearest for the exact nearest constant, otherwise linear.
int32_t vgfx3d_sanitize_texture_filter(int32_t requested) {
    return requested == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                                                             : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
}

/// @brief Normalize texture mip-filter constants before sampler selection.
/// @param requested Caller-provided mip filter value.
/// @return Valid none, nearest, or linear mip filter; malformed input becomes none.
int32_t vgfx3d_sanitize_texture_mip_filter(int32_t requested) {
    if (requested == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST ||
        requested == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR)
        return requested;
    return RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE;
}

/// @brief Normalize a material texture coordinate selector to uv0 or uv1.
/// @param requested Caller-provided UV-set selector.
/// @return Zero for UV0 or one for any positive UV1 selector.
int32_t vgfx3d_sanitize_texture_uv_set(int32_t requested) {
    return requested > 0 ? 1 : 0;
}

/// @brief Copy one draw command while normalizing all backend-visible material state.
/// @details Canvas normally supplies resolved material values, but backend hooks are an
///          internal C boundary used by tests and tools. Keeping this operation shared
///          prevents API-specific shader behavior when a malformed command crosses it.
/// @param src Borrowed source draw command.
/// @param dst Receives a full copy with matrices, material scalars, enum values, UV transforms,
///            and counts normalized for backend consumption.
void vgfx3d_sanitize_draw_command(const struct vgfx3d_draw_cmd *src, struct vgfx3d_draw_cmd *dst) {
    static const float uv_fallback[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    const float material_max = 1000000.0f;

    if (!src || !dst)
        return;
    *dst = *src;
    vgfx3d_copy_mat4_finite_or_identity(dst->model_matrix, src->model_matrix);
    vgfx3d_copy_mat4_finite_or(dst->prev_model_matrix, src->prev_model_matrix, dst->model_matrix);
    for (int32_t lane = 0; lane < 4; lane++)
        dst->diffuse_color[lane] =
            vgfx3d_clamp_float_param(src->diffuse_color[lane], 0.0f, 1.0f, 1.0f);
    for (int32_t lane = 0; lane < 3; lane++) {
        dst->specular[lane] = vgfx3d_clamp_float_param(src->specular[lane], 0.0f, 1.0f, 1.0f);
        dst->emissive_color[lane] =
            vgfx3d_clamp_float_param(src->emissive_color[lane], 0.0f, material_max, 0.0f);
    }
    dst->shininess = vgfx3d_clamp_float_param(src->shininess, 0.0f, material_max, 32.0f);
    dst->alpha = vgfx3d_clamp_float_param(src->alpha, 0.0f, 1.0f, 1.0f);
    dst->metallic = vgfx3d_clamp_float_param(src->metallic, 0.0f, 1.0f, 0.0f);
    dst->roughness = vgfx3d_clamp_float_param(src->roughness, 0.0f, 1.0f, 0.5f);
    dst->ao = vgfx3d_clamp_float_param(src->ao, 0.0f, 1.0f, 1.0f);
    dst->emissive_intensity =
        vgfx3d_clamp_float_param(src->emissive_intensity, 0.0f, material_max, 1.0f);
    dst->normal_scale = vgfx3d_clamp_float_param(src->normal_scale, 0.0f, 1000.0f, 1.0f);
    dst->alpha_cutoff = vgfx3d_clamp_float_param(src->alpha_cutoff, 0.0f, 1.0f, 0.5f);
    dst->reflectivity = vgfx3d_clamp_float_param(src->reflectivity, 0.0f, 1.0f, 0.0f);
    dst->soft_particle_fade =
        vgfx3d_clamp_float_param(src->soft_particle_fade, 0.0f, material_max, 0.0f);
    dst->depth_bias = vgfx3d_clamp_float_param(src->depth_bias, -0.05f, 0.05f, 0.0f);
    dst->slope_scaled_depth_bias =
        vgfx3d_clamp_float_param(src->slope_scaled_depth_bias, -material_max, material_max, 0.0f);
    dst->workflow = vgfx3d_sanitize_material_workflow(src->workflow);
    dst->alpha_mode = vgfx3d_sanitize_alpha_mode(src->alpha_mode);
    dst->shading_model = vgfx3d_sanitize_shading_model(src->shading_model);
    dst->shadow_mode = vgfx3d_sanitize_shadow_mode(src->shadow_mode);
    dst->texture_wrap_s = vgfx3d_sanitize_texture_wrap(src->texture_wrap_s);
    dst->texture_wrap_t = vgfx3d_sanitize_texture_wrap(src->texture_wrap_t);
    dst->texture_filter = vgfx3d_sanitize_texture_filter(src->texture_filter);
    dst->texture_min_filter = vgfx3d_sanitize_texture_filter(src->texture_min_filter);
    dst->texture_mag_filter = vgfx3d_sanitize_texture_filter(src->texture_mag_filter);
    dst->texture_mip_filter = vgfx3d_sanitize_texture_mip_filter(src->texture_mip_filter);
    if (dst->texture_anisotropy < 1)
        dst->texture_anisotropy = 1;
    if (dst->texture_anisotropy > 16)
        dst->texture_anisotropy = 16;
    for (int32_t lane = 0; lane < 12; lane++)
        dst->custom_params[lane] =
            vgfx3d_clamp_float_param(src->custom_params[lane], -material_max, material_max, 0.0f);
    for (int32_t layer = 0; layer < 4; layer++) {
        float scale = vgfx3d_finite_or(src->splat_layer_scales[layer], 1.0f);
        if (scale <= 0.0f)
            scale = 1.0f;
        if (scale > material_max)
            scale = material_max;
        dst->splat_layer_scales[layer] = scale;
    }
    for (int32_t slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
        dst->texture_slot_wrap_s[slot] =
            vgfx3d_sanitize_texture_wrap(src->texture_slot_wrap_s[slot]);
        dst->texture_slot_wrap_t[slot] =
            vgfx3d_sanitize_texture_wrap(src->texture_slot_wrap_t[slot]);
        dst->texture_slot_filter[slot] =
            vgfx3d_sanitize_texture_filter(src->texture_slot_filter[slot]);
        dst->texture_slot_min_filter[slot] =
            vgfx3d_sanitize_texture_filter(src->texture_slot_min_filter[slot]);
        dst->texture_slot_mag_filter[slot] =
            vgfx3d_sanitize_texture_filter(src->texture_slot_mag_filter[slot]);
        dst->texture_slot_mip_filter[slot] =
            vgfx3d_sanitize_texture_mip_filter(src->texture_slot_mip_filter[slot]);
        dst->texture_slot_uv_set[slot] =
            vgfx3d_sanitize_texture_uv_set(src->texture_slot_uv_set[slot]);
        if (dst->texture_slot_anisotropy[slot] < 1)
            dst->texture_slot_anisotropy[slot] = 1;
        if (dst->texture_slot_anisotropy[slot] > 16)
            dst->texture_slot_anisotropy[slot] = 16;
        for (int32_t lane = 0; lane < 6; lane++)
            dst->texture_slot_uv_transform[slot][lane] =
                vgfx3d_clamp_float_param(src->texture_slot_uv_transform[slot][lane],
                                         -material_max,
                                         material_max,
                                         uv_fallback[lane]);
    }
    dst->compact_vertex_stream = src->compact_vertex_stream != 0;
    dst->unlit = src->unlit != 0;
    dst->disable_depth_test = src->disable_depth_test != 0;
    dst->additive_blend = src->additive_blend != 0;
    dst->double_sided = src->double_sided != 0;
    dst->ibl_env = src->ibl_env != 0;
    dst->has_splat = src->has_splat != 0;
    dst->has_prev_model_matrix = src->has_prev_model_matrix != 0;
    dst->has_prev_instance_matrices = src->has_prev_instance_matrices != 0;
    dst->ssr_enabled = src->ssr_enabled != 0;
    dst->has_alpha_texture = src->has_alpha_texture != 0;
}

/// @brief Copy camera parameters while enforcing the common shader/input contract.
/// @param src Borrowed camera snapshot.
/// @param dst Receives a full copy with matrices, clip planes, fog, IBL, shadow, and boolean state
///            normalized for backend consumption.
void vgfx3d_sanitize_camera_params(const struct vgfx3d_camera_params *src,
                                   struct vgfx3d_camera_params *dst) {
    static const float sun_direction_fallback[3] = {0.0f, 1.0f, 0.0f};
    const float scalar_max = 1000000.0f;
    float fog_near;
    float fog_far;
    float znear;
    float zfar;

    if (!src || !dst)
        return;
    *dst = *src;
    vgfx3d_copy_mat4_finite_or_identity(dst->view, src->view);
    vgfx3d_copy_mat4_finite_or_identity(dst->projection, src->projection);
    for (int32_t lane = 0; lane < 3; lane++)
        dst->position[lane] = vgfx3d_clamp_float_param(src->position[lane],
                                                       -VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX,
                                                       VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX,
                                                       0.0f);
    vgfx3d_copy_vec3_direction_or(dst->forward, src->forward, NULL);
    dst->is_ortho = src->is_ortho != 0;
    dst->fog_enabled = src->fog_enabled != 0;
    fog_near = vgfx3d_clamp_float_param(src->fog_near, 0.0f, 1000000000.0f, 10.0f);
    if (fog_near > 999999999.0f)
        fog_near = 10.0f;
    fog_far = vgfx3d_clamp_float_param(src->fog_far, fog_near + 1.0f, 1000000000.0f, 50.0f);
    if (fog_far <= fog_near) {
        fog_near = 10.0f;
        fog_far = 50.0f;
    }
    dst->fog_near = fog_near;
    dst->fog_far = fog_far;
    for (int32_t lane = 0; lane < 3; lane++)
        dst->fog_color[lane] = vgfx3d_clamp_float_param(src->fog_color[lane], 0.0f, 1.0f, 0.5f);
    dst->height_fog_enabled = src->height_fog_enabled != 0;
    dst->height_fog_base = vgfx3d_clamp_float_param(src->height_fog_base,
                                                    -VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX,
                                                    VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX,
                                                    0.0f);
    dst->height_fog_falloff =
        vgfx3d_clamp_float_param(src->height_fog_falloff, 0.0f, scalar_max, 0.1f);
    dst->height_fog_density =
        vgfx3d_clamp_float_param(src->height_fog_density, 0.0f, scalar_max, 0.0f);
    dst->height_fog_blend = vgfx3d_clamp_float_param(src->height_fog_blend, 0.0f, 1.0f, 0.0f);
    for (int32_t lane = 0; lane < 3; lane++)
        dst->height_fog_sun_color[lane] =
            vgfx3d_clamp_float_param(src->height_fog_sun_color[lane], 0.0f, scalar_max, 1.0f);
    vgfx3d_copy_vec3_direction_or(
        dst->height_fog_sun_dir, src->height_fog_sun_dir, sun_direction_fallback);
    dst->height_fog_sun_power =
        vgfx3d_clamp_float_param(src->height_fog_sun_power, 0.0f, scalar_max, 8.0f);
    dst->height_fog_sun_amount =
        vgfx3d_clamp_float_param(src->height_fog_sun_amount, 0.0f, 1.0f, 0.0f);
    dst->load_existing_color = src->load_existing_color != 0;
    dst->load_existing_depth = src->load_existing_depth != 0;
    dst->ibl_enabled = src->ibl_enabled != 0;
    dst->ibl_intensity = vgfx3d_clamp_float_param(src->ibl_intensity, 0.0f, 8.0f, 1.0f);
    for (int32_t lane = 0; lane < 27; lane++)
        dst->ibl_sh[lane] =
            vgfx3d_clamp_float_param(src->ibl_sh[lane], -scalar_max, scalar_max, 0.0f);
    dst->shadow_strength = vgfx3d_clamp_float_param(src->shadow_strength, 0.0f, 1.0f, 1.0f);
    dst->shadow_slope_bias = vgfx3d_clamp_float_param(src->shadow_slope_bias, 0.0f, 16.0f, 1.0f);
    if (src->shadow_quality < 0)
        dst->shadow_quality = 0;
    else if (src->shadow_quality > 2)
        dst->shadow_quality = 2;
    znear = vgfx3d_clamp_float_param(src->znear, 0.0001f, 500000000.0f, 0.1f);
    if (!isfinite(src->zfar) || src->zfar <= znear * 1.001f)
        zfar = znear * 1000.0f;
    else
        zfar = src->zfar;
    if (zfar > 1000000000.0f)
        zfar = 1000000000.0f;
    if (!isfinite(zfar) || zfar <= znear * 1.001f) {
        znear = 0.1f;
        zfar = 1000.0f;
    }
    dst->znear = znear;
    dst->zfar = zfar;
}

/// @brief Copy one light payload while enforcing finite, backend-portable semantics.
/// @param src Borrowed light snapshot.
/// @param dst Receives a full copy with type, directions, shadow metadata, color, attenuation,
///            shape, and range normalized for backend consumption.
void vgfx3d_sanitize_light_params(const struct vgfx3d_light_params *src,
                                  struct vgfx3d_light_params *dst) {
    static const float direction_fallback[3] = {0.0f, -1.0f, 0.0f};
    static const float basis_u_fallback[3] = {1.0f, 0.0f, 0.0f};
    static const float basis_v_fallback[3] = {0.0f, 1.0f, 0.0f};
    const float scalar_max = 1000000.0f;
    int invalid_type;
    float previous_split = 0.0f;

    if (!src || !dst)
        return;
    *dst = *src;
    invalid_type = src->type < 0 || src->type > 6;
    dst->type = invalid_type ? 2 : src->type;
    dst->casts_shadows = src->casts_shadows != 0;
    if (src->shadow_index < 0 || src->shadow_index >= VGFX3D_MAX_SHADOW_LIGHTS)
        dst->shadow_index = -1;
    if (src->shadow_cascade_count < 1)
        dst->shadow_cascade_count = 1;
    else if (src->shadow_cascade_count > VGFX3D_CSM_SLOTS)
        dst->shadow_cascade_count = VGFX3D_CSM_SLOTS;
    if (src->shadow_projection_type < VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC ||
        src->shadow_projection_type > VGFX3D_SHADOW_PROJECTION_CUBE)
        dst->shadow_projection_type = VGFX3D_SHADOW_PROJECTION_PERSPECTIVE;
    vgfx3d_copy_vec3_direction_or(dst->direction, src->direction, direction_fallback);
    vgfx3d_copy_vec3_direction_or(dst->basis_u, src->basis_u, basis_u_fallback);
    vgfx3d_make_perpendicular_direction(dst->basis_u, src->basis_v, basis_v_fallback, dst->basis_v);
    for (int32_t lane = 0; lane < 3; lane++) {
        dst->position[lane] = vgfx3d_clamp_float_param(src->position[lane],
                                                       -VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX,
                                                       VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX,
                                                       0.0f);
        dst->color[lane] = vgfx3d_clamp_float_param(src->color[lane], 0.0f, 1.0f, 0.0f);
    }
    dst->intensity = vgfx3d_clamp_float_param(src->intensity, 0.0f, scalar_max, 0.0f);
    if (invalid_type)
        dst->intensity = 0.0f;
    dst->attenuation = vgfx3d_clamp_float_param(src->attenuation, 0.0f, scalar_max, 0.0f);
    dst->inner_cos = vgfx3d_clamp_float_param(src->inner_cos, -1.0f, 1.0f, 1.0f);
    dst->outer_cos = vgfx3d_clamp_float_param(src->outer_cos, -1.0f, 1.0f, 0.0f);
    if (dst->inner_cos < dst->outer_cos) {
        float temporary = dst->inner_cos;
        dst->inner_cos = dst->outer_cos;
        dst->outer_cos = temporary;
    }
    for (int32_t cascade = 0; cascade < VGFX3D_CSM_SLOTS; cascade++) {
        float split = vgfx3d_clamp_float_param(
            src->shadow_cascade_splits[cascade], 0.0f, scalar_max, previous_split);
        if (split < previous_split)
            split = previous_split;
        dst->shadow_cascade_splits[cascade] = split;
        previous_split = split;
    }
    dst->width = vgfx3d_clamp_float_param(src->width, 0.000001f, scalar_max, 1.0f);
    dst->height = vgfx3d_clamp_float_param(src->height, 0.000001f, scalar_max, 1.0f);
    dst->radius = vgfx3d_clamp_float_param(src->radius, 0.000001f, scalar_max, 1.0f);
    dst->range = vgfx3d_clamp_float_param(src->range, 0.000001f, scalar_max, 1.0f);
    if (src->decay_type < 0)
        dst->decay_type = 0;
    else if (src->decay_type > 3)
        dst->decay_type = 3;
}

/// @brief Copy up to the fixed renderer light limit into a caller-owned array.
/// @param src Borrowed source-light array.
/// @param count Caller-provided source count.
/// @param dst Receives sanitized light values.
/// @param dst_capacity Positive number of entries available in @p dst.
/// @return Number of entries initialized, constrained by the source count, destination capacity,
///         and @c VGFX3D_MAX_LIGHTS.
int32_t vgfx3d_sanitize_light_array(const struct vgfx3d_light_params *src,
                                    int32_t count,
                                    struct vgfx3d_light_params *dst,
                                    int32_t dst_capacity) {
    if (!src || !dst || count <= 0 || dst_capacity <= 0)
        return 0;
    if (count > VGFX3D_MAX_LIGHTS)
        count = VGFX3D_MAX_LIGHTS;
    if (count > dst_capacity)
        count = dst_capacity;
    for (int32_t index = 0; index < count; index++)
        vgfx3d_sanitize_light_params(&src[index], &dst[index]);
    return count;
}

/// @brief Restrict a sanitized light's shadow slots to a contiguous backend range.
/// @param light Mutable sanitized light whose base slot and cascade/cubemap span are adjusted.
/// @param shadow_count Number of contiguous shadow slots advertised by the backend.
void vgfx3d_sanitize_light_shadow_span(struct vgfx3d_light_params *light, int32_t shadow_count) {
    int32_t remaining;
    if (!light)
        return;
    if (shadow_count < 0)
        shadow_count = 0;
    if (shadow_count > VGFX3D_MAX_SHADOW_LIGHTS)
        shadow_count = VGFX3D_MAX_SHADOW_LIGHTS;
    if (light->shadow_index < 0 || light->shadow_index >= shadow_count) {
        light->shadow_index = -1;
        light->shadow_cascade_count = 1;
        light->shadow_projection_type = VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
        return;
    }
    remaining = shadow_count - light->shadow_index;
    if (light->shadow_projection_type == VGFX3D_SHADOW_PROJECTION_CUBE) {
        if (remaining < VGFX3D_SHADOW_CUBE_FACES) {
            light->shadow_index = -1;
            light->shadow_projection_type = VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
        }
        light->shadow_cascade_count = 1;
        return;
    }
    if (light->shadow_projection_type != VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC) {
        light->shadow_cascade_count = 1;
        return;
    }
    if (light->shadow_cascade_count > remaining)
        light->shadow_cascade_count = remaining;
    if (light->shadow_cascade_count < 1)
        light->shadow_cascade_count = 1;
}

/// @brief Copy finite non-negative ambient RGB, defaulting malformed lanes to black.
/// @param src Optional borrowed ambient RGB triplet; null is treated as black.
/// @param dst Receives three finite, non-negative, bounded components; null is a no-op.
void vgfx3d_sanitize_ambient_rgb(const float *src, float dst[3]) {
    if (!dst)
        return;
    for (int32_t lane = 0; lane < 3; lane++)
        dst[lane] = vgfx3d_clamp_float_param(src ? src[lane] : 0.0f, 0.0f, 1000000.0f, 0.0f);
}

/// @brief Validate clustered-light metadata and every shader-consumed offset/index.
/// @param table Borrowed clustered-light table.
/// @param expected_revision Non-zero light revision the table must match.
/// @param light_count Number of sanitized lights addressable by table indices.
/// @return Non-zero when counts, depth range, monotonic offsets, and every binned index are valid.
int vgfx3d_cluster_table_is_usable(const struct vgfx3d_cluster_table *table,
                                   uint32_t expected_revision,
                                   int32_t light_count) {
    uint16_t previous_offset;
    uint16_t final_offset;

    if (!table || expected_revision == 0 || table->lights_revision != expected_revision ||
        light_count <= 0 || light_count > VGFX3D_MAX_LIGHTS || table->global_light_count < 0 ||
        table->global_light_count > light_count || table->binned_light_count < 0 ||
        table->binned_light_count != light_count - table->global_light_count ||
        table->overflow_count < 0 || !isfinite(table->znear) || !isfinite(table->zfar) ||
        table->znear <= 0.0f || table->zfar <= table->znear || table->offsets[0] != 0)
        return 0;
    previous_offset = table->offsets[0];
    for (int32_t cluster = 1; cluster <= VGFX3D_CLUSTER_COUNT; cluster++) {
        uint16_t offset = table->offsets[cluster];
        if (offset < previous_offset || offset > VGFX3D_MAX_CLUSTER_LIGHT_INDICES)
            return 0;
        previous_offset = offset;
    }
    final_offset = table->offsets[VGFX3D_CLUSTER_COUNT];
    for (uint32_t index = 0; index < (uint32_t)final_offset; index++) {
        uint16_t light_index = table->indices[index];
        if ((int32_t)light_index < table->global_light_count || light_index >= light_count)
            return 0;
    }
    return 1;
}

/// @brief Validate the pointer/count/capacity relationship of an enabled effect chain.
/// @param chain Borrowed post-processing chain.
/// @return Non-zero when the chain is enabled, contains a valid effect array and count, and every
///         effect type lies in the public range.
int vgfx3d_postfx_chain_is_usable(const struct vgfx3d_postfx_chain *chain) {
    if (!chain || !chain->enabled || !chain->effects || chain->effect_count <= 0 ||
        chain->effect_count > VGFX3D_POSTFX_MAX_EFFECTS ||
        chain->effect_capacity < chain->effect_count)
        return 0;
    for (int32_t index = 0; index < chain->effect_count; index++) {
        if (chain->effects[index].type < (int32_t)VGFX3D_POSTFX_EFFECT_BLOOM ||
            chain->effects[index].type > (int32_t)VGFX3D_POSTFX_EFFECT_SHARPEN)
            return 0;
    }
    return 1;
}

/// @brief Copy a post-FX snapshot into bounded shader-facing values.
/// @param src Optional borrowed effect snapshot; null produces an all-disabled result.
/// @param dst Receives normalized toggles, finite scalar settings, and bounded shader loop counts;
///            null is a no-op.
void vgfx3d_sanitize_postfx_snapshot(const struct vgfx3d_postfx_snapshot *src,
                                     struct vgfx3d_postfx_snapshot *dst) {
    const float scalar_max = 1000000.0f;
    if (!dst)
        return;
    memset(dst, 0, sizeof(*dst));
    if (!src)
        return;
    *dst = *src;
    dst->enabled = src->enabled != 0;
    dst->bloom_enabled = src->bloom_enabled != 0;
    dst->bloom_threshold = vgfx3d_clamp_float_param(src->bloom_threshold, 0.0f, scalar_max, 0.8f);
    dst->bloom_intensity = vgfx3d_clamp_float_param(src->bloom_intensity, 0.0f, scalar_max, 1.0f);
    if (src->bloom_passes < 0)
        dst->bloom_passes = 0;
    else if (src->bloom_passes > 32)
        dst->bloom_passes = 32;
    if (src->tonemap_mode < 0 || src->tonemap_mode > 2)
        dst->tonemap_mode = 0;
    dst->tonemap_exposure = vgfx3d_clamp_float_param(src->tonemap_exposure, 0.0f, scalar_max, 1.0f);
    dst->fxaa_enabled = src->fxaa_enabled != 0;
    dst->color_grade_enabled = src->color_grade_enabled != 0;
    dst->cg_brightness = vgfx3d_clamp_float_param(src->cg_brightness, -1.0f, 1.0f, 0.0f);
    dst->cg_contrast = vgfx3d_clamp_float_param(src->cg_contrast, 0.0f, 4.0f, 1.0f);
    dst->cg_saturation = vgfx3d_clamp_float_param(src->cg_saturation, 0.0f, 4.0f, 1.0f);
    dst->vignette_enabled = src->vignette_enabled != 0;
    dst->vignette_radius = vgfx3d_clamp_float_param(src->vignette_radius, 0.0f, 1.0f, 0.7f);
    dst->vignette_softness = vgfx3d_clamp_float_param(src->vignette_softness, 0.001f, 1.0f, 0.3f);
    dst->ssao_enabled = src->ssao_enabled != 0;
    dst->ssao_radius = vgfx3d_clamp_float_param(src->ssao_radius, 0.0f, scalar_max, 0.5f);
    dst->ssao_intensity = vgfx3d_clamp_float_param(src->ssao_intensity, 0.0f, scalar_max, 1.0f);
    if (src->ssao_samples < 4)
        dst->ssao_samples = 4;
    else if (src->ssao_samples > 16)
        dst->ssao_samples = 16;
    dst->dof_enabled = src->dof_enabled != 0;
    dst->dof_focus_distance =
        vgfx3d_clamp_float_param(src->dof_focus_distance, 0.0f, scalar_max, 10.0f);
    dst->dof_aperture = vgfx3d_clamp_float_param(src->dof_aperture, 0.0f, scalar_max, 0.0f);
    dst->dof_max_blur = vgfx3d_clamp_float_param(src->dof_max_blur, 0.0f, 128.0f, 8.0f);
    dst->motion_blur_enabled = src->motion_blur_enabled != 0;
    dst->motion_blur_intensity =
        vgfx3d_clamp_float_param(src->motion_blur_intensity, 0.0f, 1.0f, 0.0f);
    if (src->motion_blur_samples < 2)
        dst->motion_blur_samples = 2;
    else if (src->motion_blur_samples > 8)
        dst->motion_blur_samples = 8;
    dst->taa_enabled = src->taa_enabled != 0;
    dst->taa_blend = vgfx3d_clamp_float_param(src->taa_blend, 0.5f, 0.98f, 0.9f);
    dst->tonemap_explicit = src->tonemap_explicit != 0;
    dst->ssr_enabled = src->ssr_enabled != 0;
    dst->ssr_intensity = vgfx3d_clamp_float_param(src->ssr_intensity, 0.0f, 1.0f, 0.5f);
    dst->ssr_max_roughness = vgfx3d_clamp_float_param(src->ssr_max_roughness, 0.0f, 1.0f, 0.4f);
    if (src->ssr_steps < 8)
        dst->ssr_steps = 8;
    else if (src->ssr_steps > 48)
        dst->ssr_steps = 48;
    dst->sharpen_enabled = src->sharpen_enabled != 0;
    dst->sharpen_amount = vgfx3d_clamp_float_param(src->sharpen_amount, 0.0f, 1.0f, 0.4f);
    /* Plan 61: LUT payload is usable only when the strip pointer and the exact
     * 256x16 tile layout survive together — anything else disables the pass. */
    dst->color_lut_enabled = src->color_lut_enabled != 0 && src->color_lut_texels != NULL &&
                             src->color_lut_width == 256 && src->color_lut_height == 16;
    dst->color_lut_blend = vgfx3d_clamp_float_param(src->color_lut_blend, 0.0f, 1.0f, 1.0f);
    if (!dst->color_lut_enabled) {
        dst->color_lut_texels = NULL;
        dst->color_lut_width = 0;
        dst->color_lut_height = 0;
        dst->color_lut_revision = 0;
    }
}

/// @brief Convert one reversed-Z depth sample into the backend hook's canonical convention.
/// @param reversed_depth Reversed-Z sample where near is one and far is zero.
/// @return Canonical depth in `[0, 1]`, or -1 for non-finite input.
float vgfx3d_sanitize_reversed_depth_probe_result(float reversed_depth) {
    if (!isfinite(reversed_depth))
        return -1.0f;
    return vgfx3d_clamp_float_param(1.0f - reversed_depth, 0.0f, 1.0f, -1.0f);
}

/// @brief Compute a cross-backend scaled scene extent from logical output dimensions.
/// @details Uses truncation toward zero after multiplying positive dimensions, which is exactly
///          `floor` for the accepted positive range. Multiplication is performed in double
///          precision so large, valid `int32_t` dimensions do not acquire avoidable float
///          rounding error. Since `scale <= 1`, a successful result never exceeds its logical
///          output dimension.
/// @param output_width Logical presentation width in pixels; must be positive.
/// @param output_height Logical presentation height in pixels; must be positive.
/// @param scale Finite render scale in the closed interval `[0.25, 1]`.
/// @param out_scene_width Receives the positive scene width; cleared before validation.
/// @param out_scene_height Receives the positive scene height; cleared before validation.
/// @return 1 on success, or 0 for invalid dimensions, scale, or output pointers.
int vgfx3d_compute_scaled_scene_extent(int32_t output_width,
                                       int32_t output_height,
                                       float scale,
                                       int32_t *out_scene_width,
                                       int32_t *out_scene_height) {
    int32_t width;
    int32_t height;

    if (out_scene_width)
        *out_scene_width = 0;
    if (out_scene_height)
        *out_scene_height = 0;
    if (!out_scene_width || !out_scene_height || output_width <= 0 || output_height <= 0 ||
        !isfinite(scale) || scale < 0.25f || scale > 1.0f) {
        return 0;
    }

    width = (int32_t)((double)output_width * (double)scale);
    height = (int32_t)((double)output_height * (double)scale);
    *out_scene_width = width > 0 ? width : 1;
    *out_scene_height = height > 0 ? height : 1;
    return 1;
}

/// @brief Compute the exact extent of one level in a square mip pyramid.
/// @param base_extent Positive base-level width and height.
/// @param mip_level Non-negative mip index.
/// @param out_extent Receives the level extent and is cleared before validation.
/// @return 1 when @p mip_level exists in the pyramid, otherwise 0.
int vgfx3d_expected_square_mip_extent(int32_t base_extent, int32_t mip_level, int32_t *out_extent) {
    int32_t extent;
    int32_t level = 0;

    if (out_extent)
        *out_extent = 0;
    if (!out_extent || base_extent <= 0 || mip_level < 0)
        return 0;
    extent = base_extent;
    while (level < mip_level && extent > 1) {
        extent >>= 1;
        level++;
    }
    if (level != mip_level)
        return 0;
    *out_extent = extent;
    return 1;
}

/// @brief Validate a prefiltered IBL tail against a concrete cubemap mip layout.
/// @param face_size Positive base cubemap face extent.
/// @param ibl_base_size Positive extent of the first supplied IBL mip.
/// @param ibl_mip_count Number of consecutive supplied IBL mips.
/// @param max_ibl_mips Capacity of the caller's IBL mip storage.
/// @param out_level_base Receives the destination level corresponding to @p ibl_base_size.
/// @return 1 when the IBL chain exactly fits a suffix of the complete cubemap pyramid, otherwise 0.
int vgfx3d_validate_cubemap_ibl_layout(int32_t face_size,
                                       int32_t ibl_base_size,
                                       int32_t ibl_mip_count,
                                       int32_t max_ibl_mips,
                                       int32_t *out_level_base) {
    int32_t level_base = 0;
    int32_t available_levels = 1;
    int32_t size;

    if (out_level_base)
        *out_level_base = 0;
    if (!out_level_base || face_size <= 0 || ibl_base_size <= 0 || ibl_mip_count <= 0 ||
        max_ibl_mips <= 0 || ibl_mip_count > max_ibl_mips)
        return 0;
    size = face_size;
    while (size > 1) {
        size >>= 1;
        available_levels++;
    }
    size = face_size;
    while (size > ibl_base_size && size > 1) {
        size >>= 1;
        level_base++;
    }
    if (size != ibl_base_size || level_base >= available_levels ||
        ibl_mip_count > available_levels - level_base)
        return 0;
    *out_level_base = level_base;
    return 1;
}

/// @brief Test a whole upload against the bytes remaining in a frame budget.
/// @param budget Maximum upload bytes for the frame, or @c UINT64_MAX for unlimited.
/// @param used Bytes already consumed from the frame budget.
/// @param requested Bytes required by the indivisible upload.
/// @return Non-zero when the request is empty, unlimited, or fits the remaining budget.
int vgfx3d_upload_budget_allows(uint64_t budget, uint64_t used, uint64_t requested) {
    if (requested == 0 || budget == UINT64_MAX)
        return 1;
    if (used >= budget)
        return 0;
    return requested <= budget - used;
}

/// @brief Read the monotonic generation counter on a Pixels object.
/// Returns 0 for null. Backends compare against last-seen generation to detect
/// when a GPU texture upload is required.
/// @param pixels_ptr Borrowed opaque Pixels object.
/// @return Current content generation, or zero for a null object.
uint64_t vgfx3d_get_pixels_generation(const void *pixels_ptr) {
    const vgfx3d_pixels_view_t *pv = (const vgfx3d_pixels_view_t *)pixels_ptr;
    if (!pv)
        return 0;
    return pv->generation;
}

/// @brief Combine a Pixels object's identity + content generation into one cache key.
/// @details Backends cache GPU-side texture uploads keyed by this value.
///   The key must change whenever *either* the Pixels object is a
///   different identity (recreated with the same address by the
///   allocator — common after GC free+new) *or* the existing object's
///   content mutates (generation bump from a Set / Fill / Paste).
///   Seed is the FNV-1a 64-bit offset basis; the mixing step uses the
///   golden-ratio increment from Boost's hash_combine so unrelated
///   (identity, generation) pairs distribute uniformly across the output
///   space. Null pointer returns 0 as a distinguishable "no Pixels"
///   sentinel. Since an arbitrary 64-bit hash can still land on zero, a
///   populated object's zero result is remapped to one.
/// @param pixels_ptr Borrowed opaque Pixels object.
/// @return Non-zero identity/content signature for a populated object, or zero for null.
uint64_t vgfx3d_get_pixels_cache_key(const void *pixels_ptr) {
    const vgfx3d_pixels_view_t *pv = (const vgfx3d_pixels_view_t *)pixels_ptr;
    uint64_t signature = 1469598103934665603ull;

    if (!pv)
        return 0;

    signature ^= pv->cache_identity + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);
    signature ^= pv->generation + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);
    return signature != 0 ? signature : 1u;
}

/// @brief Whether a texture asset's row-derived native capability is present in @p native_caps.
/// @details TextureAsset3D's authoritative format table supplies the exact backend bit, so this
///          cross-backend query cannot drift from parser block geometry or native format identity.
///          Assets without retained native blocks or a resident range report unsupported.
/// @param asset Borrowed TextureAsset3D object.
/// @param native_caps Backend capability bitset.
/// @return Non-zero when the asset has native content and its exact format bit is advertised.
int vgfx3d_textureasset_native_supported(void *asset, int64_t native_caps) {
    int64_t capability_bit;

    if (!asset || rt_textureasset3d_get_native_cache_key(asset) == 0)
        return 0;
    capability_bit = rt_textureasset3d_get_native_capability_bit(asset);
    return capability_bit != 0 && (native_caps & capability_bit) != 0;
}

/// @brief Fill @p out_mip with the native compressed payload for the resident mip at @p
/// relative_mip.
/// @details @p relative_mip is offset from the asset's first resident mip. Validates that the
/// payload,
///          dimensions, block geometry, and format are all usable before reporting success.
/// @param asset Borrowed TextureAsset3D object.
/// @param relative_mip Zero-based index within the current resident mip window.
/// @param out_mip Receives borrowed payload and format metadata and is cleared before validation.
/// @return 1 with @p out_mip populated, or 0 (out_mip zeroed) if out of range or incomplete.
int vgfx3d_textureasset_get_native_resident_mip(void *asset,
                                                int64_t relative_mip,
                                                vgfx3d_native_texture_mip_t *out_mip) {
    int64_t first;
    int64_t count;

    if (out_mip)
        memset(out_mip, 0, sizeof(*out_mip));
    if (!asset || !out_mip || relative_mip < 0)
        return 0;
    first = rt_textureasset3d_get_resident_mip_start(asset);
    count = rt_textureasset3d_get_resident_mip_count(asset);
    return vgfx3d_textureasset_get_native_snapshot_mip(asset, first, count, relative_mip, out_mip);
}

/// @brief Borrow one native compressed mip from an explicit resident-window snapshot.
/// @details Draw commands record the resident mip window they observed at queue time. Backends use
///          this helper during deferred submission so native compressed uploads cannot switch to a
///          different mip window if streaming code mutates the TextureAsset3D later in the frame.
/// @param asset Borrowed TextureAsset3D object.
/// @param first_mip Absolute first mip captured by the draw.
/// @param mip_count Number of consecutive captured resident mips.
/// @param relative_mip Zero-based offset within the captured window.
/// @param out_mip Receives borrowed payload and format metadata and is cleared before validation.
/// @return 1 when the requested snapshot mip exists and has valid native metadata, otherwise 0.
int vgfx3d_textureasset_get_native_snapshot_mip(void *asset,
                                                int64_t first_mip,
                                                int64_t mip_count,
                                                int64_t relative_mip,
                                                vgfx3d_native_texture_mip_t *out_mip) {
    vgfx3d_native_texture_mip_t mip;

    if (out_mip)
        memset(out_mip, 0, sizeof(*out_mip));
    if (!asset || !out_mip || first_mip < 0 || mip_count <= 0 || relative_mip < 0 ||
        relative_mip >= mip_count || relative_mip > INT64_MAX - first_mip)
        return 0;
    memset(&mip, 0, sizeof(mip));
    if (!rt_textureasset3d_get_native_mip_info(asset,
                                               first_mip + relative_mip,
                                               &mip.data,
                                               &mip.bytes,
                                               &mip.width,
                                               &mip.height,
                                               &mip.block_width,
                                               &mip.block_height,
                                               &mip.block_bytes))
        return 0;
    mip.format_id = rt_textureasset3d_get_native_format_id(asset);
    if (!mip.data || mip.bytes == 0 || mip.width <= 0 || mip.height <= 0 || mip.block_width <= 0 ||
        mip.block_height <= 0 || mip.block_bytes <= 0 ||
        mip.format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE) {
        return 0;
    }
    *out_mip = mip;
    return 1;
}

/// @brief Native bytes still to upload from the current mip/block-row cursor onward.
/// @details Counts the current resident mip from @p next_block_row and later mips from the
///          beginning, saturating at UINT64_MAX. Returns 0 when no upload is in progress, so
///          callers can budget streaming work per frame.
/// @param asset Borrowed TextureAsset3D object whose current resident window is measured.
/// @param next_relative_mip Zero-based current mip within the resident window.
/// @param next_block_row First compressed block row not yet uploaded in the current mip.
/// @param upload_in_progress Non-zero only while the upload cursor is meaningful.
/// @return Remaining native payload bytes, saturated at @c UINT64_MAX.
uint64_t vgfx3d_textureasset_pending_native_bytes(void *asset,
                                                  int64_t next_relative_mip,
                                                  int32_t next_block_row,
                                                  int upload_in_progress) {
    int64_t first;
    int64_t count;

    first = asset ? rt_textureasset3d_get_resident_mip_start(asset) : 0;
    count = asset ? rt_textureasset3d_get_resident_mip_count(asset) : 0;
    return vgfx3d_textureasset_pending_native_snapshot_bytes(
        asset, first, count, next_relative_mip, next_block_row, upload_in_progress);
}

/// @brief Compute pending native upload bytes inside an explicit resident-window snapshot.
/// @param asset Borrowed TextureAsset3D object.
/// @param first_mip Absolute first mip captured by the draw.
/// @param mip_count Number of consecutive captured resident mips.
/// @param next_relative_mip Zero-based current mip within the captured window.
/// @param next_block_row First compressed block row not yet uploaded in the current mip.
/// @param upload_in_progress Non-zero only while the upload cursor is meaningful.
/// @return Remaining snapshot payload bytes, saturated at @c UINT64_MAX.
uint64_t vgfx3d_textureasset_pending_native_snapshot_bytes(void *asset,
                                                           int64_t first_mip,
                                                           int64_t mip_count,
                                                           int64_t next_relative_mip,
                                                           int32_t next_block_row,
                                                           int upload_in_progress) {
    uint64_t total = 0;

    if (!upload_in_progress || !asset || first_mip < 0 || mip_count <= 0 || next_relative_mip < 0)
        return 0;
    if (next_relative_mip >= mip_count)
        return 0;
    for (int64_t i = next_relative_mip; i < mip_count; i++) {
        vgfx3d_native_texture_mip_t mip;
        if (!vgfx3d_textureasset_get_native_snapshot_mip(asset, first_mip, mip_count, i, &mip))
            return total;
        uint64_t bytes = (i == next_relative_mip)
                             ? vgfx3d_pending_block_upload_bytes(mip.width,
                                                                 mip.height,
                                                                 mip.block_width,
                                                                 mip.block_height,
                                                                 mip.block_bytes,
                                                                 next_block_row,
                                                                 upload_in_progress)
                             : mip.bytes;
        if (total > UINT64_MAX - bytes)
            return UINT64_MAX;
        total += bytes;
    }
    return total;
}

/// @brief Decode a Pixels object into a freshly malloc'd RGBA8 byte array.
/// Caller owns and frees the returned buffer. Returns 0 on success, -1 on
/// invalid dimensions or allocation failure. Out-params are unmodified on error.
/// @param pixels_ptr Borrowed opaque Pixels object storing packed @c 0xRRGGBBAA values.
/// @param out_w Receives decoded width on success.
/// @param out_h Receives decoded height on success.
/// @param out_rgba Receives newly allocated tightly packed RGBA8 storage on success.
/// @return 0 on success, otherwise -1 without publishing outputs.
int vgfx3d_unpack_pixels_rgba(const void *pixels_ptr,
                              int32_t *out_w,
                              int32_t *out_h,
                              uint8_t **out_rgba) {
    if (!pixels_ptr || !out_w || !out_h || !out_rgba)
        return -1;

    const vgfx3d_pixels_view_t *pv = (const vgfx3d_pixels_view_t *)pixels_ptr;
    if (!pv->data || pv->w <= 0 || pv->h <= 0 || pv->w > INT32_MAX || pv->h > INT32_MAX)
        return -1;

    int32_t w = (int32_t)pv->w;
    int32_t h = (int32_t)pv->h;
    size_t pixel_count = (size_t)w * (size_t)h;
    if ((size_t)w != 0 && pixel_count / (size_t)w != (size_t)h)
        return -1;
    if (pixel_count > SIZE_MAX / 4u)
        return -1;
    uint8_t *rgba = (uint8_t *)malloc(pixel_count * 4);
    if (!rgba)
        return -1;

    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t px = pv->data[i]; /* 0xRRGGBBAA */
        rgba[i * 4 + 0] = (uint8_t)((px >> 24) & 0xFF);
        rgba[i * 4 + 1] = (uint8_t)((px >> 16) & 0xFF);
        rgba[i * 4 + 2] = (uint8_t)((px >> 8) & 0xFF);
        rgba[i * 4 + 3] = (uint8_t)(px & 0xFF);
    }

    *out_w = w;
    *out_h = h;
    *out_rgba = rgba;
    return 0;
}

/// @brief Read a Pixels object's width/height without unpacking its data.
/// @param pixels_ptr Borrowed opaque Pixels object.
/// @param out_w Receives width and is cleared before validation.
/// @param out_h Receives height and is cleared before validation.
/// @return 1 with @p out_w / @p out_h set, or 0 (both zeroed) for a NULL/empty/oversized surface.
int vgfx3d_get_pixels_extent(const void *pixels_ptr, int32_t *out_w, int32_t *out_h) {
    const vgfx3d_pixels_view_t *pv = (const vgfx3d_pixels_view_t *)pixels_ptr;

    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    if (!pv || !out_w || !out_h || !pv->data || pv->w <= 0 || pv->h <= 0 || pv->w > INT32_MAX ||
        pv->h > INT32_MAX)
        return 0;

    *out_w = (int32_t)pv->w;
    *out_h = (int32_t)pv->h;
    return 1;
}

/// @brief Decode a horizontal band of a Pixels object into a fresh RGBA8 buffer (caller frees).
/// @details Unpacks @p row_count rows from @p start_row (clamped to the image), optionally flipping
///          vertically (@p flip_y) for backends with a bottom-left origin. Enables streaming a
///          large texture upload row-band by row-band.
/// @param pixels_ptr Borrowed opaque Pixels object storing packed @c 0xRRGGBBAA values.
/// @param start_row Zero-based first destination band row.
/// @param row_count Positive requested row count, clamped at the image boundary.
/// @param flip_y Non-zero to map band rows through the vertically reversed source.
/// @param out_w Receives decoded row width and is cleared before validation.
/// @param out_rows Receives actual decoded row count and is cleared before validation.
/// @param out_rgba Receives newly allocated tightly packed RGBA8 storage and is cleared before
///                 validation.
/// @return 0 on success with out-params set, -1 on invalid args or allocation failure.
int vgfx3d_unpack_pixels_rgba_rows(const void *pixels_ptr,
                                   int32_t start_row,
                                   int32_t row_count,
                                   int flip_y,
                                   int32_t *out_w,
                                   int32_t *out_rows,
                                   uint8_t **out_rgba) {
    const vgfx3d_pixels_view_t *pv = (const vgfx3d_pixels_view_t *)pixels_ptr;
    int32_t w;
    int32_t h;
    size_t row_bytes;
    size_t total_bytes;
    uint8_t *rgba;

    if (out_w)
        *out_w = 0;
    if (out_rows)
        *out_rows = 0;
    if (out_rgba)
        *out_rgba = NULL;
    if (!pv || !out_w || !out_rows || !out_rgba || !pv->data || pv->w <= 0 || pv->h <= 0 ||
        pv->w > INT32_MAX || pv->h > INT32_MAX || start_row < 0 || row_count <= 0)
        return -1;

    w = (int32_t)pv->w;
    h = (int32_t)pv->h;
    if (start_row >= h)
        return -1;
    if (row_count > h - start_row)
        row_count = h - start_row;
    if ((size_t)w > SIZE_MAX / 4u)
        return -1;
    row_bytes = (size_t)w * 4u;
    if ((size_t)row_count > SIZE_MAX / row_bytes)
        return -1;
    total_bytes = (size_t)row_count * row_bytes;
    rgba = (uint8_t *)malloc(total_bytes);
    if (!rgba)
        return -1;

    for (int32_t y = 0; y < row_count; y++) {
        int32_t src_y = flip_y ? (h - 1 - (start_row + y)) : (start_row + y);
        const uint32_t *src = pv->data + ((size_t)src_y * (size_t)w);
        uint8_t *dst = rgba + ((size_t)y * row_bytes);
        for (int32_t x = 0; x < w; x++) {
            uint32_t px = src[x]; /* 0xRRGGBBAA */
            dst[(size_t)x * 4u + 0u] = (uint8_t)((px >> 24) & 0xFF);
            dst[(size_t)x * 4u + 1u] = (uint8_t)((px >> 16) & 0xFF);
            dst[(size_t)x * 4u + 2u] = (uint8_t)((px >> 8) & 0xFF);
            dst[(size_t)x * 4u + 3u] = (uint8_t)(px & 0xFF);
        }
    }

    *out_w = w;
    *out_rows = row_count;
    *out_rgba = rgba;
    return 0;
}

/// @brief Compute the RGBA8 byte count uploaded for one Pixels texture.
/// @param pixels_ptr Borrowed opaque Pixels object.
/// @param out_bytes Receives tightly packed RGBA8 bytes and is cleared before validation.
/// @return 1 when the surface is valid and its byte count is representable, otherwise 0.
int vgfx3d_estimate_pixels_rgba_upload_bytes(const void *pixels_ptr, uint64_t *out_bytes) {
    const vgfx3d_pixels_view_t *pv = (const vgfx3d_pixels_view_t *)pixels_ptr;
    uint64_t w;
    uint64_t h;
    uint64_t pixel_count;

    if (out_bytes)
        *out_bytes = 0;
    if (!pv || !out_bytes || !pv->data || pv->w <= 0 || pv->h <= 0 || pv->w > INT32_MAX ||
        pv->h > INT32_MAX)
        return 0;

    w = (uint64_t)pv->w;
    h = (uint64_t)pv->h;
    if (w > UINT64_MAX / h)
        return 0;
    pixel_count = w * h;
    if (pixel_count > UINT64_MAX / 4u)
        return 0;

    *out_bytes = pixel_count * 4u;
    return 1;
}

/// @brief How many texture rows from @p next_row fit in the remaining per-frame upload byte budget.
/// @details UINT64_MAX budget means "all remaining rows"; otherwise divides the leftover budget by
///          the row size, always allowing at least one row so progress is guaranteed.
/// @param width Positive texture width in pixels.
/// @param height Positive texture height in pixels.
/// @param next_row First source row not yet uploaded.
/// @param budget Per-frame byte budget, or @c UINT64_MAX for unlimited.
/// @param used Bytes already consumed during the frame.
/// @return Number of consecutive rows to upload next, or zero when no valid/progressing upload is
///         possible.
int32_t vgfx3d_upload_rows_for_budget(
    int32_t width, int32_t height, int32_t next_row, uint64_t budget, uint64_t used) {
    uint64_t row_bytes;
    uint64_t remaining_budget;
    uint64_t budget_rows;
    int32_t remaining_rows;

    if (width <= 0 || height <= 0 || next_row < 0 || next_row >= height)
        return 0;
    remaining_rows = height - next_row;
    if (budget == UINT64_MAX)
        return remaining_rows;
    if (budget == 0)
        return 0;

    row_bytes = (uint64_t)(uint32_t)width * 4u;
    if (row_bytes == 0 || used >= budget)
        return 0;
    remaining_budget = budget - used;
    budget_rows = remaining_budget / row_bytes;
    if (budget_rows == 0)
        budget_rows = 1;
    if (budget_rows > (uint64_t)remaining_rows)
        budget_rows = (uint64_t)remaining_rows;
    return (int32_t)budget_rows;
}

/// @brief Bytes still to upload for an RGBA texture from @p next_row to the last row.
/// @details Returns 0 when no upload is in progress; saturates at UINT64_MAX. Lets the scheduler
///          weigh this texture's remaining work against the frame budget.
/// @param width Positive texture width in pixels.
/// @param height Positive texture height in pixels.
/// @param next_row First row not yet uploaded.
/// @param upload_in_progress Non-zero only while the row cursor is meaningful.
/// @return Remaining tightly packed RGBA8 bytes, saturated at @c UINT64_MAX.
uint64_t vgfx3d_pending_rgba_upload_bytes(int32_t width,
                                          int32_t height,
                                          int32_t next_row,
                                          int upload_in_progress) {
    uint64_t remaining_rows;
    uint64_t row_bytes;

    if (!upload_in_progress || width <= 0 || height <= 0 || next_row < 0 || next_row >= height)
        return 0;
    remaining_rows = (uint64_t)(uint32_t)(height - next_row);
    row_bytes = (uint64_t)(uint32_t)width * 4u;
    if (row_bytes != 0 && remaining_rows > UINT64_MAX / row_bytes)
        return UINT64_MAX;
    return remaining_rows * row_bytes;
}

/// @brief Bytes still to upload across all remaining cubemap faces and rows.
/// @details Counts the rows left in the current face plus every row of the faces after it (faces
/// are
///          uploaded in order 0..5). Returns 0 when idle; saturates at UINT64_MAX.
/// @param face_size Positive square face extent.
/// @param upload_face Current face index in `[0, 5]`.
/// @param upload_next_row First row not yet uploaded in the current face.
/// @param upload_in_progress Non-zero only while the face/row cursor is meaningful.
/// @return Remaining tightly packed RGBA8 bytes, saturated at @c UINT64_MAX.
uint64_t vgfx3d_pending_cubemap_rgba_upload_bytes(int32_t face_size,
                                                  int32_t upload_face,
                                                  int32_t upload_next_row,
                                                  int upload_in_progress) {
    uint64_t remaining_rows;
    uint64_t row_bytes;

    if (!upload_in_progress || face_size <= 0 || upload_face < 0 || upload_face >= 6 ||
        upload_next_row < 0 || upload_next_row >= face_size)
        return 0;
    remaining_rows = (uint64_t)(uint32_t)(face_size - upload_next_row);
    remaining_rows += (uint64_t)(uint32_t)(5 - upload_face) * (uint64_t)(uint32_t)face_size;
    row_bytes = (uint64_t)(uint32_t)face_size * 4u;
    if (row_bytes != 0 && remaining_rows > UINT64_MAX / row_bytes)
        return UINT64_MAX;
    return remaining_rows * row_bytes;
}

/// @brief Compute a block-compressed texture's block-row count and per-block-row byte size.
/// @details Rounds width/height up to whole blocks (BCn/ASTC/ETC2 tile the image in fixed blocks),
///          overflow-checking the row size. Shared by the block-upload budget/pending helpers.
/// @param width Positive mip width in pixels.
/// @param height Positive mip height in pixels.
/// @param block_width Positive compression-block width in pixels.
/// @param block_height Positive compression-block height in pixels.
/// @param block_bytes Positive encoded bytes per block.
/// @param out_block_rows Optional output receiving the rounded-up block-row count.
/// @param out_row_bytes Optional output receiving bytes per complete block row.
/// @return 1 with the out-params set, 0 on invalid dimensions or overflow.
static int vgfx3d_block_upload_shape(int32_t width,
                                     int32_t height,
                                     int32_t block_width,
                                     int32_t block_height,
                                     int32_t block_bytes,
                                     uint64_t *out_block_rows,
                                     uint64_t *out_row_bytes) {
    uint64_t block_cols;
    uint64_t block_rows;

    if (out_block_rows)
        *out_block_rows = 0;
    if (out_row_bytes)
        *out_row_bytes = 0;
    if (width <= 0 || height <= 0 || block_width <= 0 || block_height <= 0 || block_bytes <= 0)
        return 0;

    block_cols = ((uint64_t)(uint32_t)width + (uint64_t)(uint32_t)block_width - 1u) /
                 (uint64_t)(uint32_t)block_width;
    block_rows = ((uint64_t)(uint32_t)height + (uint64_t)(uint32_t)block_height - 1u) /
                 (uint64_t)(uint32_t)block_height;
    if (block_cols == 0 || block_rows == 0 ||
        block_cols > UINT64_MAX / (uint64_t)(uint32_t)block_bytes)
        return 0;
    if (out_block_rows)
        *out_block_rows = block_rows;
    if (out_row_bytes)
        *out_row_bytes = block_cols * (uint64_t)(uint32_t)block_bytes;
    return 1;
}

/// @brief Verify a native format's fixed/canonical compressed-block footprint.
/// @param mip Borrowed native mip descriptor containing format and block metadata.
/// @return Non-zero when the format is supported and its block dimensions/size are canonical.
int vgfx3d_native_texture_block_layout_is_valid(const vgfx3d_native_texture_mip_t *mip) {
    int astc_shape;

    if (!mip)
        return 0;
    switch (mip->format_id) {
        case RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1:
        case RT_TEXTUREASSET3D_NATIVE_FORMAT_BC4:
            return mip->block_width == 4 && mip->block_height == 4 && mip->block_bytes == 8;
        case RT_TEXTUREASSET3D_NATIVE_FORMAT_BC3:
        case RT_TEXTUREASSET3D_NATIVE_FORMAT_BC5:
        case RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7:
        case RT_TEXTUREASSET3D_NATIVE_FORMAT_ETC2:
            return mip->block_width == 4 && mip->block_height == 4 && mip->block_bytes == 16;
        case RT_TEXTUREASSET3D_NATIVE_FORMAT_ASTC:
            astc_shape =
                (mip->block_width == 4 && mip->block_height == 4) ||
                (mip->block_width == 5 && (mip->block_height == 4 || mip->block_height == 5)) ||
                (mip->block_width == 6 && (mip->block_height == 5 || mip->block_height == 6)) ||
                (mip->block_width == 8 &&
                 (mip->block_height == 5 || mip->block_height == 6 || mip->block_height == 8)) ||
                (mip->block_width == 10 && (mip->block_height == 5 || mip->block_height == 6 ||
                                            mip->block_height == 8 || mip->block_height == 10)) ||
                (mip->block_width == 12 && (mip->block_height == 10 || mip->block_height == 12));
            return astc_shape && mip->block_bytes == 16;
        default:
            return 0;
    }
}

/// @brief Validate dimensions, format, block footprint, and byte span for one native mip.
/// @details Backends capture the base mip and block layout at upload start. Every later streamed
///          mip must halve toward 1x1, retain the same compressed format/footprint, and provide at
///          least the complete rounded-up block payload. The optional payload ceiling lets API
///          adapters reject values their length fields cannot represent before making a driver
///          call.
/// @param mip Borrowed native mip descriptor to validate.
/// @param base_width Positive first uploaded mip width.
/// @param base_height Positive first uploaded mip height.
/// @param relative_mip Zero-based mip index relative to the captured base.
/// @param expected_format_id Native format required throughout the chain.
/// @param expected_block_width Compression-block width required throughout the chain.
/// @param expected_block_height Compression-block height required throughout the chain.
/// @param expected_block_bytes Encoded bytes per block required throughout the chain.
/// @param max_payload_bytes Optional non-zero ceiling imposed by the backend driver API.
/// @param out_required_bytes Receives the exact rounded block payload and is cleared before
///                           validation.
/// @return 1 when all dimensions, layout, payload, and ceiling constraints hold, otherwise 0.
int vgfx3d_validate_native_texture_mip(const vgfx3d_native_texture_mip_t *mip,
                                       int32_t base_width,
                                       int32_t base_height,
                                       int64_t relative_mip,
                                       int32_t expected_format_id,
                                       int32_t expected_block_width,
                                       int32_t expected_block_height,
                                       int32_t expected_block_bytes,
                                       uint64_t max_payload_bytes,
                                       uint64_t *out_required_bytes) {
    int32_t expected_width;
    int32_t expected_height;
    uint64_t block_rows;
    uint64_t row_bytes;
    uint64_t required_bytes;

    if (out_required_bytes)
        *out_required_bytes = 0;
    if (!mip || !out_required_bytes || !mip->data || mip->bytes == 0 || base_width <= 0 ||
        base_height <= 0 || relative_mip < 0 || expected_format_id <= 0 ||
        expected_block_width <= 0 || expected_block_height <= 0 || expected_block_bytes <= 0 ||
        !vgfx3d_native_texture_block_layout_is_valid(mip) || mip->format_id != expected_format_id ||
        mip->block_width != expected_block_width || mip->block_height != expected_block_height ||
        mip->block_bytes != expected_block_bytes)
        return 0;

    expected_width = base_width;
    expected_height = base_height;
    while (relative_mip-- > 0) {
        if (expected_width == 1 && expected_height == 1)
            return 0;
        expected_width = expected_width > 1 ? expected_width / 2 : 1;
        expected_height = expected_height > 1 ? expected_height / 2 : 1;
    }
    if (mip->width != expected_width || mip->height != expected_height ||
        !vgfx3d_block_upload_shape(mip->width,
                                   mip->height,
                                   mip->block_width,
                                   mip->block_height,
                                   mip->block_bytes,
                                   &block_rows,
                                   &row_bytes) ||
        block_rows > UINT64_MAX / row_bytes)
        return 0;
    required_bytes = block_rows * row_bytes;
    if (mip->bytes < required_bytes ||
        (max_payload_bytes != 0 &&
         (mip->bytes > max_payload_bytes || required_bytes > max_payload_bytes)))
        return 0;
    *out_required_bytes = required_bytes;
    return 1;
}

/// @brief How many block-rows from @p next_block_row fit in the remaining per-frame upload budget.
/// @details Block-compressed analogue of vgfx3d_upload_rows_for_budget; UINT64_MAX budget means all
///          remaining block-rows, and at least one block-row is always returned so uploads
///          progress.
/// @param width Positive mip width in pixels.
/// @param height Positive mip height in pixels.
/// @param block_width Positive compression-block width.
/// @param block_height Positive compression-block height.
/// @param block_bytes Positive encoded bytes per block.
/// @param next_block_row First compressed block row not yet uploaded.
/// @param budget Per-frame byte budget, or @c UINT64_MAX for unlimited.
/// @param used Bytes already consumed during the frame.
/// @return Number of consecutive block rows to upload next, or zero when progress is invalid or
///         disallowed.
int32_t vgfx3d_upload_block_rows_for_budget(int32_t width,
                                            int32_t height,
                                            int32_t block_width,
                                            int32_t block_height,
                                            int32_t block_bytes,
                                            int32_t next_block_row,
                                            uint64_t budget,
                                            uint64_t used) {
    uint64_t block_rows;
    uint64_t row_bytes;
    uint64_t remaining_rows;
    uint64_t remaining_budget;
    uint64_t budget_rows;

    if (!vgfx3d_block_upload_shape(
            width, height, block_width, block_height, block_bytes, &block_rows, &row_bytes))
        return 0;
    if (next_block_row < 0 || (uint64_t)(uint32_t)next_block_row >= block_rows || budget == 0 ||
        used >= budget)
        return 0;
    remaining_rows = block_rows - (uint64_t)(uint32_t)next_block_row;
    if (budget == UINT64_MAX)
        return remaining_rows > (uint64_t)INT32_MAX ? INT32_MAX : (int32_t)remaining_rows;

    remaining_budget = budget - used;
    budget_rows = remaining_budget / row_bytes;
    if (budget_rows == 0)
        budget_rows = 1;
    if (budget_rows > remaining_rows)
        budget_rows = remaining_rows;
    return (int32_t)budget_rows;
}

/// @brief Bytes still to upload for a block-compressed texture from @p next_block_row onward.
/// @details Returns 0 when idle; saturates at UINT64_MAX. The block analogue of
///          vgfx3d_pending_rgba_upload_bytes.
/// @param width Positive mip width in pixels.
/// @param height Positive mip height in pixels.
/// @param block_width Positive compression-block width.
/// @param block_height Positive compression-block height.
/// @param block_bytes Positive encoded bytes per block.
/// @param next_block_row First compressed block row not yet uploaded.
/// @param upload_in_progress Non-zero only while the cursor is meaningful.
/// @return Remaining compressed bytes, saturated at @c UINT64_MAX.
uint64_t vgfx3d_pending_block_upload_bytes(int32_t width,
                                           int32_t height,
                                           int32_t block_width,
                                           int32_t block_height,
                                           int32_t block_bytes,
                                           int32_t next_block_row,
                                           int upload_in_progress) {
    uint64_t block_rows;
    uint64_t row_bytes;
    uint64_t remaining_rows;

    if (!upload_in_progress ||
        !vgfx3d_block_upload_shape(
            width, height, block_width, block_height, block_bytes, &block_rows, &row_bytes))
        return 0;
    if (next_block_row < 0 || (uint64_t)(uint32_t)next_block_row >= block_rows)
        return 0;
    remaining_rows = block_rows - (uint64_t)(uint32_t)next_block_row;
    if (row_bytes != 0 && remaining_rows > UINT64_MAX / row_bytes)
        return UINT64_MAX;
    return remaining_rows * row_bytes;
}

/// @brief Decode all six cubemap faces into separate RGBA8 byte arrays.
/// All faces must be square and the same size. Caller owns and frees each
/// face buffer. On error any partially-allocated faces are freed automatically.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @param out_face_size Receives the common square extent and is cleared before validation.
/// @param out_faces Receives six newly allocated tightly packed RGBA8 buffers; all entries are
///                  cleared before validation.
/// @return 0 on success, otherwise -1 after freeing any partial outputs.
int vgfx3d_unpack_cubemap_faces_rgba(const void *cubemap_ptr,
                                     int32_t *out_face_size,
                                     uint8_t *out_faces[6]) {
    int32_t face_size = 0;
    const vgfx3d_cubemap_view_t *cubemap = (const vgfx3d_cubemap_view_t *)cubemap_ptr;

    if (out_face_size)
        *out_face_size = 0;
    if (out_faces) {
        for (int face = 0; face < 6; face++)
            out_faces[face] = NULL;
    }
    if (!cubemap || !out_face_size || !out_faces ||
        !vgfx3d_get_cubemap_face_size(cubemap, &face_size))
        return -1;

    for (int face = 0; face < 6; face++) {
        int32_t w = 0;
        int32_t h = 0;
        if (vgfx3d_unpack_pixels_rgba(cubemap->faces[face], &w, &h, &out_faces[face]) != 0 ||
            w != face_size || h != face_size) {
            for (int cleanup = 0; cleanup < 6; cleanup++) {
                free(out_faces[cleanup]);
                out_faces[cleanup] = NULL;
            }
            return -1;
        }
    }

    *out_face_size = face_size;
    return 0;
}

/// @brief Read a cubemap's face size, verifying all six faces are square and identically sized.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @param out_face_size Receives the validated common extent and is cleared before validation.
/// @return 1 with @p out_face_size set, or 0 (zeroed) if any face is missing or mis-sized.
int vgfx3d_get_cubemap_face_size(const void *cubemap_ptr, int32_t *out_face_size) {
    const vgfx3d_cubemap_view_t *cubemap = (const vgfx3d_cubemap_view_t *)cubemap_ptr;
    int32_t face_size;

    if (out_face_size)
        *out_face_size = 0;
    if (!cubemap || !out_face_size || cubemap->cache_identity == 0 || cubemap->face_size <= 0 ||
        cubemap->face_size > VGFX3D_BACKEND_MAX_CUBEMAP_FACE_SIZE)
        return 0;

    face_size = (int32_t)cubemap->face_size;
    for (int face = 0; face < 6; face++) {
        int32_t w = 0;
        int32_t h = 0;
        if (!vgfx3d_get_pixels_extent(cubemap->faces[face], &w, &h) || w != face_size ||
            h != face_size)
            return 0;
    }

    *out_face_size = face_size;
    return 1;
}

/// @brief Decode a horizontal band of one cubemap face into a fresh RGBA8 buffer (caller frees).
/// @details Per-face, row-band analogue of vgfx3d_unpack_pixels_rgba_rows for streaming cubemap
///          uploads; validates @p face_index in [0, 6) and that the face matches the cube size.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @param face_index Face index in `[0, 5]`.
/// @param start_row Zero-based first destination band row.
/// @param row_count Positive requested row count, clamped at the face boundary.
/// @param flip_y Non-zero to map band rows through the vertically reversed face.
/// @param out_face_size Receives the common face extent and is cleared before validation.
/// @param out_rows Receives actual decoded row count and is cleared before validation.
/// @param out_rgba Receives newly allocated tightly packed RGBA8 storage and is cleared before
///                 validation.
/// @return 0 on success with out-params set, -1 on invalid args or allocation failure.
int vgfx3d_unpack_cubemap_rgba_rows(const void *cubemap_ptr,
                                    int32_t face_index,
                                    int32_t start_row,
                                    int32_t row_count,
                                    int flip_y,
                                    int32_t *out_face_size,
                                    int32_t *out_rows,
                                    uint8_t **out_rgba) {
    const vgfx3d_cubemap_view_t *cubemap = (const vgfx3d_cubemap_view_t *)cubemap_ptr;
    int32_t face_size = 0;
    int32_t w = 0;
    int32_t rows = 0;
    uint8_t *rgba = NULL;

    if (out_face_size)
        *out_face_size = 0;
    if (out_rows)
        *out_rows = 0;
    if (out_rgba)
        *out_rgba = NULL;
    if (!cubemap || !out_face_size || !out_rows || !out_rgba || face_index < 0 || face_index >= 6 ||
        !vgfx3d_get_cubemap_face_size(cubemap, &face_size))
        return -1;

    if (vgfx3d_unpack_pixels_rgba_rows(
            cubemap->faces[face_index], start_row, row_count, flip_y, &w, &rows, &rgba) != 0 ||
        !rgba || w != face_size || rows <= 0) {
        free(rgba);
        return -1;
    }

    *out_face_size = face_size;
    *out_rows = rows;
    *out_rgba = rgba;
    return 0;
}

/// @brief Compute the RGBA8 byte count uploaded for one six-face cubemap.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @param out_bytes Receives total tightly packed bytes for all six faces and is cleared before
///                  validation.
/// @return 1 when every face is valid and the total is representable, otherwise 0.
int vgfx3d_estimate_cubemap_rgba_upload_bytes(const void *cubemap_ptr, uint64_t *out_bytes) {
    const vgfx3d_cubemap_view_t *cubemap = (const vgfx3d_cubemap_view_t *)cubemap_ptr;
    int32_t face_size = 0;
    uint64_t face_bytes = 0;
    uint64_t total = 0;

    if (out_bytes)
        *out_bytes = 0;
    if (!cubemap || !out_bytes || !vgfx3d_get_cubemap_face_size(cubemap, &face_size))
        return 0;

    for (int face = 0; face < 6; face++) {
        const vgfx3d_pixels_view_t *pv = (const vgfx3d_pixels_view_t *)cubemap->faces[face];
        if (!pv || pv->w != face_size || pv->h != face_size ||
            !vgfx3d_estimate_pixels_rgba_upload_bytes(pv, &face_bytes))
            return 0;
        if (total > UINT64_MAX - face_bytes)
            return 0;
        total += face_bytes;
    }

    *out_bytes = total;
    return 1;
}

/// @brief Hash cubemap identity + all six face cache keys into one signature.
/// Uses an FNV-prime mixing scheme so face mutations, face replacement, and
/// cubemap object replacement all invalidate backend caches. Returns 0 when no
/// complete face set is bound.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @return Non-zero identity/content signature, or zero for an incomplete cubemap.
uint64_t vgfx3d_get_cubemap_generation(const void *cubemap_ptr) {
    const vgfx3d_cubemap_view_t *cubemap = (const vgfx3d_cubemap_view_t *)cubemap_ptr;
    uint64_t signature = 1469598103934665603ull;
    int32_t face_size = 0;

    if (!cubemap || !vgfx3d_get_cubemap_face_size(cubemap, &face_size))
        return 0;

    signature ^=
        cubemap->cache_identity + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);

    for (int face = 0; face < 6; face++) {
        uint64_t face_key = vgfx3d_get_pixels_cache_key(cubemap->faces[face]);
        if (face_key == 0)
            return 0;
        signature ^= face_key + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);
    }

    return signature != 0 ? signature : 1u;
}

/// @brief Flip an RGBA8 image vertically in place (top<->bottom row swap).
/// Used to convert between Pixels' top-left origin and APIs that expect
/// bottom-left (e.g., OpenGL textures).
/// @param rgba Mutable tightly packed RGBA8 storage.
/// @param w Positive image width in pixels.
/// @param h Image height in pixels; values below two are a no-op.
void vgfx3d_flip_rgba_rows(uint8_t *rgba, int32_t w, int32_t h) {
    uint8_t temporary[256];
    size_t row_bytes;

    if (!rgba || w <= 0 || h <= 1)
        return;

    if ((size_t)w > SIZE_MAX / 4u)
        return;
    row_bytes = (size_t)w * 4u;
    if (row_bytes != 0 && (size_t)h > SIZE_MAX / row_bytes)
        return;

    for (int32_t y = 0; y < h / 2; y++) {
        uint8_t *top = rgba + (size_t)y * row_bytes;
        uint8_t *bot = rgba + (size_t)(h - 1 - y) * row_bytes;
        size_t offset = 0;
        while (offset < row_bytes) {
            size_t chunk = row_bytes - offset;
            if (chunk > sizeof(temporary))
                chunk = sizeof(temporary);
            memcpy(temporary, top + offset, chunk);
            memcpy(top + offset, bot + offset, chunk);
            memcpy(bot + offset, temporary, chunk);
            offset += chunk;
        }
    }
}

/// @brief Convert IEEE-754 binary16 to binary32.
/// @param bits Raw IEEE-754 binary16 bit pattern.
/// @return Numerically equivalent binary32 value, preserving signed zero, infinity, and NaN class.
float vgfx3d_half_to_float(uint16_t bits) {
    uint32_t sign = (uint32_t)(bits & 0x8000u) << 16;
    uint32_t exp = (bits >> 10) & 0x1Fu;
    uint32_t mant = bits & 0x03FFu;
    uint32_t fbits;
    float out;

    if (exp == 0) {
        if (mant == 0) {
            fbits = sign;
        } else {
            exp = 127u - 15u + 1u;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1u;
                exp--;
            }
            mant &= 0x03FFu;
            fbits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        fbits = sign | 0x7F800000u | (mant << 13);
    } else {
        fbits = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
    }

    memcpy(&out, &fbits, sizeof(out));
    return out;
}

/// @brief Clamp a float to [0,1] and quantize to UNORM8.
/// @param value Linear normalized value; non-positive and NaN input maps to zero.
/// @return Nearest eight-bit unsigned normalized representation.
uint8_t vgfx3d_float_to_unorm8(float value) {
    if (!(value > 0.0f))
        return 0;
    if (value >= 1.0f)
        return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

/// @brief Apply a simple Reinhard tonemap before UNORM8 quantization.
/// @param value Linear HDR channel value. Non-positive and NaN values map to zero, while positive
///              infinity maps to the maximum representable channel value.
/// @return Tonemapped eight-bit unsigned normalized representation.
uint8_t vgfx3d_hdr_to_unorm8(float value) {
    if (!(value > 0.0f))
        return 0;
    if (!isfinite(value))
        return 255;
    return vgfx3d_float_to_unorm8(value / (1.0f + value));
}

/// @brief Validate a row-copy request: positive extents, non-negative strides, no 32-bit
///   overflow in the per-row byte/unit math, and strides large enough for one row.
/// @param copy_w Number of pixels copied from each row.
/// @param copy_h Number of rows copied.
/// @param dst_stride_units Distance between destination rows, measured in destination units.
/// @param src_stride_bytes Distance between source rows, measured in bytes.
/// @param dst_units_per_pixel Number of destination units occupied by one pixel.
/// @param dst_unit_bytes Size of one destination unit in bytes.
/// @param src_bytes_per_pixel Number of source bytes occupied by one pixel.
/// @return 1 when all row sizes and offsets are valid and representable, otherwise 0.
static int vgfx3d_copy_dims_are_valid(int32_t copy_w,
                                      int32_t copy_h,
                                      int32_t dst_stride_units,
                                      int32_t src_stride_bytes,
                                      int32_t dst_units_per_pixel,
                                      int32_t dst_unit_bytes,
                                      int32_t src_bytes_per_pixel) {
    size_t dst_offset;
    size_t dst_required;
    size_t src_offset;
    size_t src_required;
    if (copy_w <= 0 || copy_h <= 0 || dst_stride_units < 0 || src_stride_bytes < 0)
        return 0;
    if (dst_units_per_pixel <= 0 || dst_unit_bytes <= 0 || src_bytes_per_pixel <= 0 ||
        (size_t)copy_w > SIZE_MAX / (size_t)dst_units_per_pixel ||
        (size_t)copy_w > SIZE_MAX / (size_t)src_bytes_per_pixel)
        return 0;
    dst_required = (size_t)copy_w * (size_t)dst_units_per_pixel;
    src_required = (size_t)copy_w * (size_t)src_bytes_per_pixel;
    if (dst_required > (size_t)INT32_MAX || src_required > (size_t)INT32_MAX ||
        (size_t)dst_stride_units < dst_required || (size_t)src_stride_bytes < src_required ||
        dst_required > SIZE_MAX / (size_t)dst_unit_bytes)
        return 0;
    dst_required *= (size_t)dst_unit_bytes;
    if ((size_t)(copy_h - 1) > SIZE_MAX / (size_t)dst_stride_units ||
        (size_t)(copy_h - 1) > SIZE_MAX / (size_t)src_stride_bytes)
        return 0;
    dst_offset = (size_t)(copy_h - 1) * (size_t)dst_stride_units;
    src_offset = (size_t)(copy_h - 1) * (size_t)src_stride_bytes;
    if (dst_offset > SIZE_MAX / (size_t)dst_unit_bytes)
        return 0;
    dst_offset *= (size_t)dst_unit_bytes;
    return dst_offset <= SIZE_MAX - dst_required && src_offset <= SIZE_MAX - src_required;
}

/// @brief Convert linear RGBA16F rows to displayable RGBA8.
/// @details RGB channels are Reinhard-tonemapped before quantization; alpha is clamped directly to
///          UNORM8. Invalid pointers, extents, or strides leave the destination untouched.
/// @param dst_rgba Caller-owned RGBA8 destination buffer.
/// @param dst_stride Distance between destination rows in bytes.
/// @param copy_w Number of pixels converted per row.
/// @param copy_h Number of rows converted.
/// @param src_rgba16f Borrowed source buffer containing IEEE-754 binary16 RGBA pixels.
/// @param src_stride_bytes Distance between source rows in bytes.
void vgfx3d_copy_linear_rgba16f_to_rgba8(uint8_t *dst_rgba,
                                         int32_t dst_stride,
                                         int32_t copy_w,
                                         int32_t copy_h,
                                         const uint16_t *src_rgba16f,
                                         int32_t src_stride_bytes) {
    if (!dst_rgba || !src_rgba16f ||
        !vgfx3d_copy_dims_are_valid(
            copy_w, copy_h, dst_stride, src_stride_bytes, 4, 1, (int32_t)(sizeof(uint16_t) * 4u))) {
        return;
    }

    for (int32_t y = 0; y < copy_h; y++) {
        uint8_t *dst_row = dst_rgba + (size_t)y * (size_t)dst_stride;
        const uint8_t *src_row =
            (const uint8_t *)(const void *)src_rgba16f + (size_t)y * (size_t)src_stride_bytes;
        for (int32_t x = 0; x < copy_w; x++) {
            uint16_t src_pixel[4];
            memcpy(src_pixel, src_row + (size_t)x * sizeof(src_pixel), sizeof(src_pixel));
            dst_row[(size_t)x * 4u + 0u] = vgfx3d_hdr_to_unorm8(vgfx3d_half_to_float(src_pixel[0]));
            dst_row[(size_t)x * 4u + 1u] = vgfx3d_hdr_to_unorm8(vgfx3d_half_to_float(src_pixel[1]));
            dst_row[(size_t)x * 4u + 2u] = vgfx3d_hdr_to_unorm8(vgfx3d_half_to_float(src_pixel[2]));
            dst_row[(size_t)x * 4u + 3u] =
                vgfx3d_float_to_unorm8(vgfx3d_half_to_float(src_pixel[3]));
        }
    }
}

/// @brief Convert linear RGBA16F rows to linear RGBA32F.
/// @details Each binary16 channel is expanded independently without tonemapping. Invalid pointers,
///          extents, or strides leave the destination untouched.
/// @param dst_rgba32f Caller-owned RGBA32F destination buffer.
/// @param dst_stride_floats Distance between destination rows in float elements.
/// @param copy_w Number of pixels converted per row.
/// @param copy_h Number of rows converted.
/// @param src_rgba16f Borrowed source buffer containing IEEE-754 binary16 RGBA pixels.
/// @param src_stride_bytes Distance between source rows in bytes.
void vgfx3d_copy_linear_rgba16f_to_rgba32f(float *dst_rgba32f,
                                           int32_t dst_stride_floats,
                                           int32_t copy_w,
                                           int32_t copy_h,
                                           const uint16_t *src_rgba16f,
                                           int32_t src_stride_bytes) {
    if (!dst_rgba32f || !src_rgba16f ||
        !vgfx3d_copy_dims_are_valid(copy_w,
                                    copy_h,
                                    dst_stride_floats,
                                    src_stride_bytes,
                                    4,
                                    (int32_t)sizeof(float),
                                    (int32_t)(sizeof(uint16_t) * 4u))) {
        return;
    }

    for (int32_t y = 0; y < copy_h; y++) {
        float *dst_row = dst_rgba32f + (size_t)y * (size_t)dst_stride_floats;
        const uint8_t *src_row =
            (const uint8_t *)(const void *)src_rgba16f + (size_t)y * (size_t)src_stride_bytes;
        for (int32_t x = 0; x < copy_w; x++) {
            uint16_t src_pixel[4];
            memcpy(src_pixel, src_row + (size_t)x * sizeof(src_pixel), sizeof(src_pixel));
            dst_row[(size_t)x * 4u + 0u] = vgfx3d_half_to_float(src_pixel[0]);
            dst_row[(size_t)x * 4u + 1u] = vgfx3d_half_to_float(src_pixel[1]);
            dst_row[(size_t)x * 4u + 2u] = vgfx3d_half_to_float(src_pixel[2]);
            dst_row[(size_t)x * 4u + 3u] = vgfx3d_half_to_float(src_pixel[3]);
        }
    }
}

/// @brief Convert linear RGBA32F rows to displayable RGBA8.
/// @details RGB channels are Reinhard-tonemapped before quantization; alpha is clamped directly to
///          UNORM8. Invalid pointers, extents, or strides leave the destination untouched.
/// @param dst_rgba Caller-owned RGBA8 destination buffer.
/// @param dst_stride Distance between destination rows in bytes.
/// @param copy_w Number of pixels converted per row.
/// @param copy_h Number of rows converted.
/// @param src_rgba32f Borrowed source buffer containing linear float RGBA pixels.
/// @param src_stride_bytes Distance between source rows in bytes.
void vgfx3d_copy_linear_rgba32f_to_rgba8(uint8_t *dst_rgba,
                                         int32_t dst_stride,
                                         int32_t copy_w,
                                         int32_t copy_h,
                                         const float *src_rgba32f,
                                         int32_t src_stride_bytes) {
    if (!dst_rgba || !src_rgba32f ||
        !vgfx3d_copy_dims_are_valid(
            copy_w, copy_h, dst_stride, src_stride_bytes, 4, 1, (int32_t)(sizeof(float) * 4u))) {
        return;
    }

    for (int32_t y = 0; y < copy_h; y++) {
        uint8_t *dst_row = dst_rgba + (size_t)y * (size_t)dst_stride;
        const void *src_base = (const void *)src_rgba32f;
        const uint8_t *src_row = (const uint8_t *)src_base + (size_t)y * (size_t)src_stride_bytes;
        for (int32_t x = 0; x < copy_w; x++) {
            float src_px[4];
            memcpy(src_px, src_row + (size_t)x * sizeof(src_px), sizeof(src_px));
            dst_row[(size_t)x * 4u + 0u] = vgfx3d_hdr_to_unorm8(src_px[0]);
            dst_row[(size_t)x * 4u + 1u] = vgfx3d_hdr_to_unorm8(src_px[1]);
            dst_row[(size_t)x * 4u + 2u] = vgfx3d_hdr_to_unorm8(src_px[2]);
            dst_row[(size_t)x * 4u + 3u] = vgfx3d_float_to_unorm8(src_px[3]);
        }
    }
}

/// @brief Write a 4×4 identity matrix into @p out_matrix — the normal-matrix fallback
///   when the model matrix is singular and cannot be inverse-transposed.
/// @param out_matrix Caller-owned storage for 16 row-major float elements.
static void vgfx3d_store_identity_normal_matrix4(float *out_matrix) {
    memset(out_matrix, 0, sizeof(float) * 16);
    out_matrix[0] = 1.0f;
    out_matrix[5] = 1.0f;
    out_matrix[10] = 1.0f;
    out_matrix[15] = 1.0f;
}

/// @brief Compute the normal matrix (inverse-transpose of the upper 3×3 of
/// @p model_matrix) and place it in the upper-left 3×3 of @p out_matrix.
/// Falls back to identity when the matrix is singular or non-finite, avoiding
/// NaN/Inf propagation in shaders and CPU skinning.
/// @param model_matrix Borrowed 4×4 row-major model matrix.
/// @param out_matrix Caller-owned storage for the resulting 4×4 row-major normal matrix. A null
///                   pointer, or a null @p model_matrix, makes the operation a no-op.
void vgfx3d_compute_normal_matrix4(const float *model_matrix, float *out_matrix) {
    if (!model_matrix || !out_matrix)
        return;

    const float a = model_matrix[0], b = model_matrix[1], c = model_matrix[2];
    const float d = model_matrix[4], e = model_matrix[5], f = model_matrix[6];
    const float g = model_matrix[8], h = model_matrix[9], i = model_matrix[10];

    if (!isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d) || !isfinite(e) ||
        !isfinite(f) || !isfinite(g) || !isfinite(h) || !isfinite(i)) {
        vgfx3d_store_identity_normal_matrix4(out_matrix);
        return;
    }

    const float c00 = e * i - f * h;
    const float c01 = -(d * i - f * g);
    const float c02 = d * h - e * g;
    const float c10 = -(b * i - c * h);
    const float c11 = a * i - c * g;
    const float c12 = -(a * h - b * g);
    const float c20 = b * f - c * e;
    const float c21 = -(a * f - c * d);
    const float c22 = a * e - b * d;

    float det = a * c00 + b * c01 + c * c02;
    float inv_det = 0.0f;
    if (isfinite(det) && fabsf(det) > kVgfx3dSingularDetEps)
        inv_det = 1.0f / det;

    memset(out_matrix, 0, sizeof(float) * 16);
    out_matrix[15] = 1.0f;

    if (!isfinite(inv_det) || inv_det == 0.0f) {
        vgfx3d_store_identity_normal_matrix4(out_matrix);
        return;
    }

    /* Normal matrix = (M^-1)^T = cofactor_matrix / det. The cofactor `cij` is the
     * cofactor of element [i][j], so it is placed DIRECTLY at out[i][j] — no
     * transpose. (The plain inverse M^-1 = adjugate/det = cofactor^T/det uses the
     * transpose; the inverse-transpose un-does it. Placing cij at out[j][i] is the
     * classic adjugate/cofactor mix-up and yields M^-1, which counter-rotates
     * normals under any rotation/shear while looking correct for diagonal scales.) */
    out_matrix[0] = c00 * inv_det;
    out_matrix[1] = c01 * inv_det;
    out_matrix[2] = c02 * inv_det;
    out_matrix[4] = c10 * inv_det;
    out_matrix[5] = c11 * inv_det;
    out_matrix[6] = c12 * inv_det;
    out_matrix[8] = c20 * inv_det;
    out_matrix[9] = c21 * inv_det;
    out_matrix[10] = c22 * inv_det;

    /* A near-singular (but above-eps) determinant combined with large cofactors can
     * overflow the scaled result to non-finite even though inv_det itself was finite;
     * fall back to identity so a degenerate transform never poisons lighting normals.
     * (Clamping inv_det instead would wrongly reject legitimately small-scaled
     * objects, whose normals are renormalized in-shader regardless of magnitude.) */
    if (!isfinite(out_matrix[0]) || !isfinite(out_matrix[1]) || !isfinite(out_matrix[2]) ||
        !isfinite(out_matrix[4]) || !isfinite(out_matrix[5]) || !isfinite(out_matrix[6]) ||
        !isfinite(out_matrix[8]) || !isfinite(out_matrix[9]) || !isfinite(out_matrix[10])) {
        vgfx3d_store_identity_normal_matrix4(out_matrix);
    }
}

/// @brief Invert a 4×4 row-major matrix using cofactor expansion.
/// @param matrix Borrowed 16-element source matrix.
/// @param out_matrix Caller-owned 16-element destination matrix.
/// @return 0 on success, -1 if @p matrix is null or singular (|det| < 1e-12).
/// Out-buffer is unmodified on failure.
int vgfx3d_invert_matrix4(const float *matrix, float *out_matrix) {
    float inv[16];
    float result[16];
    float det;

    if (!matrix || !out_matrix)
        return -1;

    inv[0] = matrix[5] * matrix[10] * matrix[15] - matrix[5] * matrix[11] * matrix[14] -
             matrix[9] * matrix[6] * matrix[15] + matrix[9] * matrix[7] * matrix[14] +
             matrix[13] * matrix[6] * matrix[11] - matrix[13] * matrix[7] * matrix[10];
    inv[4] = -matrix[4] * matrix[10] * matrix[15] + matrix[4] * matrix[11] * matrix[14] +
             matrix[8] * matrix[6] * matrix[15] - matrix[8] * matrix[7] * matrix[14] -
             matrix[12] * matrix[6] * matrix[11] + matrix[12] * matrix[7] * matrix[10];
    inv[8] = matrix[4] * matrix[9] * matrix[15] - matrix[4] * matrix[11] * matrix[13] -
             matrix[8] * matrix[5] * matrix[15] + matrix[8] * matrix[7] * matrix[13] +
             matrix[12] * matrix[5] * matrix[11] - matrix[12] * matrix[7] * matrix[9];
    inv[12] = -matrix[4] * matrix[9] * matrix[14] + matrix[4] * matrix[10] * matrix[13] +
              matrix[8] * matrix[5] * matrix[14] - matrix[8] * matrix[6] * matrix[13] -
              matrix[12] * matrix[5] * matrix[10] + matrix[12] * matrix[6] * matrix[9];
    inv[1] = -matrix[1] * matrix[10] * matrix[15] + matrix[1] * matrix[11] * matrix[14] +
             matrix[9] * matrix[2] * matrix[15] - matrix[9] * matrix[3] * matrix[14] -
             matrix[13] * matrix[2] * matrix[11] + matrix[13] * matrix[3] * matrix[10];
    inv[5] = matrix[0] * matrix[10] * matrix[15] - matrix[0] * matrix[11] * matrix[14] -
             matrix[8] * matrix[2] * matrix[15] + matrix[8] * matrix[3] * matrix[14] +
             matrix[12] * matrix[2] * matrix[11] - matrix[12] * matrix[3] * matrix[10];
    inv[9] = -matrix[0] * matrix[9] * matrix[15] + matrix[0] * matrix[11] * matrix[13] +
             matrix[8] * matrix[1] * matrix[15] - matrix[8] * matrix[3] * matrix[13] -
             matrix[12] * matrix[1] * matrix[11] + matrix[12] * matrix[3] * matrix[9];
    inv[13] = matrix[0] * matrix[9] * matrix[14] - matrix[0] * matrix[10] * matrix[13] -
              matrix[8] * matrix[1] * matrix[14] + matrix[8] * matrix[2] * matrix[13] +
              matrix[12] * matrix[1] * matrix[10] - matrix[12] * matrix[2] * matrix[9];
    inv[2] = matrix[1] * matrix[6] * matrix[15] - matrix[1] * matrix[7] * matrix[14] -
             matrix[5] * matrix[2] * matrix[15] + matrix[5] * matrix[3] * matrix[14] +
             matrix[13] * matrix[2] * matrix[7] - matrix[13] * matrix[3] * matrix[6];
    inv[6] = -matrix[0] * matrix[6] * matrix[15] + matrix[0] * matrix[7] * matrix[14] +
             matrix[4] * matrix[2] * matrix[15] - matrix[4] * matrix[3] * matrix[14] -
             matrix[12] * matrix[2] * matrix[7] + matrix[12] * matrix[3] * matrix[6];
    inv[10] = matrix[0] * matrix[5] * matrix[15] - matrix[0] * matrix[7] * matrix[13] -
              matrix[4] * matrix[1] * matrix[15] + matrix[4] * matrix[3] * matrix[13] +
              matrix[12] * matrix[1] * matrix[7] - matrix[12] * matrix[3] * matrix[5];
    inv[14] = -matrix[0] * matrix[5] * matrix[14] + matrix[0] * matrix[6] * matrix[13] +
              matrix[4] * matrix[1] * matrix[14] - matrix[4] * matrix[2] * matrix[13] -
              matrix[12] * matrix[1] * matrix[6] + matrix[12] * matrix[2] * matrix[5];
    inv[3] = -matrix[1] * matrix[6] * matrix[11] + matrix[1] * matrix[7] * matrix[10] +
             matrix[5] * matrix[2] * matrix[11] - matrix[5] * matrix[3] * matrix[10] -
             matrix[9] * matrix[2] * matrix[7] + matrix[9] * matrix[3] * matrix[6];
    inv[7] = matrix[0] * matrix[6] * matrix[11] - matrix[0] * matrix[7] * matrix[10] -
             matrix[4] * matrix[2] * matrix[11] + matrix[4] * matrix[3] * matrix[10] +
             matrix[8] * matrix[2] * matrix[7] - matrix[8] * matrix[3] * matrix[6];
    inv[11] = -matrix[0] * matrix[5] * matrix[11] + matrix[0] * matrix[7] * matrix[9] +
              matrix[4] * matrix[1] * matrix[11] - matrix[4] * matrix[3] * matrix[9] -
              matrix[8] * matrix[1] * matrix[7] + matrix[8] * matrix[3] * matrix[5];
    inv[15] = matrix[0] * matrix[5] * matrix[10] - matrix[0] * matrix[6] * matrix[9] -
              matrix[4] * matrix[1] * matrix[10] + matrix[4] * matrix[2] * matrix[9] +
              matrix[8] * matrix[1] * matrix[6] - matrix[8] * matrix[2] * matrix[5];

    det = matrix[0] * inv[0] + matrix[1] * inv[4] + matrix[2] * inv[8] + matrix[3] * inv[12];
    if (!isfinite(det) || fabsf(det) < kVgfx3dSingularDetEps)
        return -1;

    det = 1.0f / det;
    for (int i = 0; i < 16; i++) {
        result[i] = inv[i] * det;
        if (!isfinite(result[i]))
            return -1;
    }
    memcpy(out_matrix, result, sizeof(result));
    return 0;
}

/*==========================================================================
 * R20 compact static-mesh vertex stream encoding
 *=========================================================================*/

/// @brief Convert a float to IEEE 754 binary16 bits (round-to-nearest-even).
/// @details Deterministic bit-level conversion: no fenv dependence. Values
///          beyond half range clamp to +-65504; NaN maps to a quiet NaN.
/// @param value Binary32 value to encode.
/// @return Raw binary16 bit pattern. Infinities and finite overflow clamp to the largest finite
///         value with the matching sign.
static uint16_t vgfx3d_float_to_half_bits(float value) {
    uint32_t bits;
    uint32_t sign;
    int32_t exponent;
    uint32_t mantissa;

    memcpy(&bits, &value, sizeof(bits));
    sign = (bits >> 16) & 0x8000u;
    exponent = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
    mantissa = bits & 0x7FFFFFu;

    if (((bits >> 23) & 0xFFu) == 0xFFu) {
        /* Inf/NaN: keep Inf as the max finite value (GPU-friendly), NaN quiet. */
        if (mantissa != 0)
            return (uint16_t)(sign | 0x7E00u);
        return (uint16_t)(sign | 0x7BFFu);
    }
    if (exponent >= 0x1F)
        return (uint16_t)(sign | 0x7BFFu); /* overflow: clamp to 65504 */
    if (exponent <= 0) {
        uint32_t shift;
        uint32_t sub;
        uint32_t rest;
        if (exponent < -10)
            return (uint16_t)sign; /* underflow to signed zero */
        mantissa |= 0x800000u;
        shift = (uint32_t)(14 - exponent);
        sub = mantissa >> shift;
        rest = mantissa & ((1u << shift) - 1u);
        if (rest > (1u << (shift - 1u)) || (rest == (1u << (shift - 1u)) && (sub & 1u)))
            sub++;
        return (uint16_t)(sign | sub);
    }
    {
        uint32_t half = (uint32_t)(exponent << 10) | (mantissa >> 13);
        uint32_t rest = mantissa & 0x1FFFu;
        if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
            half++;
        if ((half & 0x7C00u) == 0x7C00u)
            return (uint16_t)(sign | 0x7BFFu); /* rounding overflowed to Inf: clamp */
        return (uint16_t)(sign | half);
    }
}

/// @brief Encode a [-1, 1] float as snorm16 (round-half-away-from-zero, deterministic).
/// @param value Value to clamp and encode; NaN maps to zero.
/// @return Signed normalized 16-bit representation in the range [-32767, 32767].
static int16_t vgfx3d_float_to_snorm16(float value) {
    float clamped = value;
    float scaled;
    if (!(clamped >= -1.0f))
        clamped = clamped < -1.0f ? -1.0f : (clamped == clamped ? clamped : 0.0f);
    if (clamped > 1.0f)
        clamped = 1.0f;
    scaled = clamped * 32767.0f;
    return (int16_t)(scaled >= 0.0f ? (int32_t)(scaled + 0.5f) : -(int32_t)(0.5f - scaled));
}

/// @brief Encode full vertices into the fixed-width compact static-mesh stream.
/// @details Positions remain three binary32 values. Normals and tangents become SNORM16, both UV
///          sets become binary16, and colors and bone weights become UNORM8; bone indices are
///          copied verbatim. Source and destination storage must not overlap.
/// @param src Borrowed array of @p count full vertices.
/// @param count Number of vertices to encode.
/// @param dst Caller-owned buffer of at least `count * VGFX3D_COMPACT_VERTEX_STRIDE` bytes. Null
///            source or destination pointers make the operation a no-op.
void vgfx3d_encode_compact_vertices(const vgfx3d_vertex_t *src, uint32_t count, uint8_t *dst) {
    if (!src || !dst)
        return;
    for (uint32_t i = 0; i < count; i++) {
        const vgfx3d_vertex_t *v = &src[i];
        uint8_t *out = dst + (size_t)i * VGFX3D_COMPACT_VERTEX_STRIDE;
        int16_t packed16[4];
        uint16_t half_bits[2];

        memcpy(out + 0, v->pos, sizeof(float) * 3u);

        packed16[0] = vgfx3d_float_to_snorm16(v->normal[0]);
        packed16[1] = vgfx3d_float_to_snorm16(v->normal[1]);
        packed16[2] = vgfx3d_float_to_snorm16(v->normal[2]);
        packed16[3] = 0;
        memcpy(out + 12, packed16, sizeof(packed16));

        half_bits[0] = vgfx3d_float_to_half_bits(v->uv[0]);
        half_bits[1] = vgfx3d_float_to_half_bits(v->uv[1]);
        memcpy(out + 20, half_bits, sizeof(half_bits));
        half_bits[0] = vgfx3d_float_to_half_bits(v->uv1[0]);
        half_bits[1] = vgfx3d_float_to_half_bits(v->uv1[1]);
        memcpy(out + 24, half_bits, sizeof(half_bits));

        out[28] = vgfx3d_float_to_unorm8(v->color[0]);
        out[29] = vgfx3d_float_to_unorm8(v->color[1]);
        out[30] = vgfx3d_float_to_unorm8(v->color[2]);
        out[31] = vgfx3d_float_to_unorm8(v->color[3]);

        packed16[0] = vgfx3d_float_to_snorm16(v->tangent[0]);
        packed16[1] = vgfx3d_float_to_snorm16(v->tangent[1]);
        packed16[2] = vgfx3d_float_to_snorm16(v->tangent[2]);
        packed16[3] = vgfx3d_float_to_snorm16(v->tangent[3]);
        memcpy(out + 32, packed16, sizeof(packed16));

        memcpy(out + 40, v->bone_indices, 4u);

        out[44] = vgfx3d_float_to_unorm8(v->bone_weights[0]);
        out[45] = vgfx3d_float_to_unorm8(v->bone_weights[1]);
        out[46] = vgfx3d_float_to_unorm8(v->bone_weights[2]);
        out[47] = vgfx3d_float_to_unorm8(v->bone_weights[3]);
    }
}
