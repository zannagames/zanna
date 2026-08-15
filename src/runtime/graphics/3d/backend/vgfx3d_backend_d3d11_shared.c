//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_shared.c
// Purpose: D3D11 backend helpers shared with other backends — constant-buffer
//   packing (HLSL float4 alignment), instance/bone palette upload prep,
//   per-frame TAA history, mip count math, capacity growth, and policy
//   choices for render-target / blend / color-format selection.
//
// Key invariants:
//   - HLSL constant buffers require float4 alignment, so scalar arrays must be
//     packed into vec4 slots (one scalar per .x, padding zeros in .yzw).
//   - Bone palette is a fixed VGFX3D_D3D11_MAX_BONES × mat4 (16 floats); the
//     upload is zero-padded and clamped to that supported cap to keep the
//     cbuffer size constant.
//   - Frame history tracks scene VP and previous-frame VP separately from
//     overlay/draw VP so motion vectors stay correct across overlay passes.
//
// Ownership/Lifetime:
//   - Helpers write only caller-owned buffers and retain no native D3D resources.
//   - Returned policy values and validation results contain no borrowed pointers.
//
// Links: vgfx3d_backend_d3d11_shared.h, vgfx3d_backend_d3d11.c
//
//===----------------------------------------------------------------------===//

/// @file vgfx3d_backend_d3d11_shared.c
/// @brief Implements platform-neutral validation, packing, sizing, and policy helpers
///   shared by the D3D11 backend implementation.
/// @details The helpers in this file operate on caller-owned values and descriptors.
///   They centralize defensive arithmetic and state decisions so native D3D11 resource
///   code can consume normalized, bounded inputs.

#include "vgfx3d_backend_d3d11_shared.h"

#include "rt_textureasset3d.h"
#include "vgfx3d_backend_utils.h"

#include <limits.h>
#include <math.h>
#include <string.h>

/// @brief Pack a flat scalar array into HLSL-aligned float4 slots, four scalars per vector.
/// Remaining vector lanes are zeroed. Truncates if @p src exceeds @p dst capacity.
/// @param[out] dst Destination array of HLSL-compatible four-float vectors.
/// @param[in] dst_vec_count Number of vectors available in @p dst.
/// @param[in] src Flat scalar input; may be `NULL` to produce an all-zero destination.
/// @param[in] src_scalar_count Number of readable scalars in @p src.
void vgfx3d_d3d11_pack_scalar_array4(float (*dst)[4],
                                     int32_t dst_vec_count,
                                     const float *src,
                                     int32_t src_scalar_count) {
    int32_t scalar_capacity;

    if (!dst || dst_vec_count <= 0)
        return;

    memset(dst, 0, (size_t)dst_vec_count * sizeof(dst[0]));
    if (!src || src_scalar_count <= 0)
        return;

    scalar_capacity = dst_vec_count > INT_MAX / 4 ? INT_MAX : dst_vec_count * 4;
    if (src_scalar_count > scalar_capacity)
        src_scalar_count = scalar_capacity;
    for (int32_t i = 0; i < src_scalar_count; i++)
        dst[i / 4][i % 4] = isfinite(src[i]) ? src[i] : 0.0f;
}

/// @brief Store a row-major identity matrix into one fixed bone-palette slot.
/// @details D3D11 identity-pads unused bones instead of zero-padding them so
///   malformed skinning indices that reference an unused palette entry leave
///   vertices in bind pose instead of collapsing them to the origin.
/// @param[out] dst Writable storage for sixteen row-major float elements.
static void vgfx3d_d3d11_store_identity4x4(float *dst) {
    memset(dst, 0, sizeof(float) * 16u);
    dst[0] = 1.0f;
    dst[5] = 1.0f;
    dst[10] = 1.0f;
    dst[15] = 1.0f;
}

/// @brief Return non-zero only when every float in @p values is finite.
/// @param[in] values Array to validate; may be `NULL` only when @p count is zero.
/// @param[in] count Number of elements to inspect.
/// @return One when every requested element is finite; otherwise zero.
int vgfx3d_d3d11_float_array_is_finite(const float *values, size_t count) {
    if (!values && count > 0)
        return 0;
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(values[i]))
            return 0;
    }
    return 1;
}

/// @brief Return non-zero only when every float is finite and within @p abs_max.
/// @param[in] values Array to validate; may be `NULL` only when @p count is zero.
/// @param[in] count Number of elements to inspect.
/// @param[in] abs_max Inclusive finite magnitude limit.
/// @return One when every element is finite and within the limit; otherwise zero.
int vgfx3d_d3d11_float_array_is_bounded(const float *values, size_t count, float abs_max) {
    if ((!values && count > 0) || !isfinite(abs_max) || abs_max < 0.0f)
        return 0;
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(values[i]) || values[i] < -abs_max || values[i] > abs_max)
            return 0;
    }
    return 1;
}

/// @brief Copy float constants while replacing NaN/Inf lanes with @p fallback.
/// @param[out] dst Destination array.
/// @param[in] src Source array, or `NULL` to fill entirely with the fallback.
/// @param[in] count Number of elements to write.
/// @param[in] fallback Replacement for non-finite or absent source values.
void vgfx3d_d3d11_copy_float_array_finite_or(float *dst,
                                             const float *src,
                                             size_t count,
                                             float fallback) {
    float safe_fallback = isfinite(fallback) ? fallback : 0.0f;

    if (!dst || count == 0)
        return;
    if (!src) {
        for (size_t i = 0; i < count; i++)
            dst[i] = safe_fallback;
        return;
    }
    for (size_t i = 0; i < count; i++)
        dst[i] = isfinite(src[i]) ? src[i] : safe_fallback;
}

/// @brief Copy float constants while substituting invalid lanes and clamping finite extremes.
/// @param[out] dst Destination array.
/// @param[in] src Source array, or `NULL` to fill with the normalized fallback.
/// @param[in] count Number of elements to write.
/// @param[in] min_value Inclusive lower bound; invalid bounds collapse to zero.
/// @param[in] max_value Inclusive upper bound; invalid bounds collapse to zero.
/// @param[in] fallback Replacement for absent or non-finite source values.
void vgfx3d_d3d11_copy_float_array_clamped_finite_or(
    float *dst, const float *src, size_t count, float min_value, float max_value, float fallback) {
    float safe_fallback;

    if (!dst || count == 0)
        return;
    if (!isfinite(min_value) || !isfinite(max_value) || min_value > max_value) {
        min_value = 0.0f;
        max_value = 0.0f;
    }
    safe_fallback = isfinite(fallback) ? fallback : 0.0f;
    if (safe_fallback < min_value)
        safe_fallback = min_value;
    else if (safe_fallback > max_value)
        safe_fallback = max_value;
    for (size_t i = 0; i < count; i++) {
        float value = src && isfinite(src[i]) ? src[i] : safe_fallback;
        if (value < min_value)
            value = min_value;
        else if (value > max_value)
            value = max_value;
        dst[i] = value;
    }
}

/// @brief Validate a finite direction vector before CPU constants or HLSL normalize.
/// @param[in] values Three-component direction vector to inspect.
/// @return One when the vector is finite and has a safe, nondegenerate length; otherwise zero.
int vgfx3d_d3d11_vec3_direction_is_usable(const float *values) {
    double len2;

    if (!values || !vgfx3d_d3d11_float_array_is_finite(values, 3u))
        return 0;
    len2 = (double)values[0] * (double)values[0] + (double)values[1] * (double)values[1] +
           (double)values[2] * (double)values[2];
    return len2 > 1.0e-12 && len2 < 1.0e20 ? 1 : 0;
}

/// @brief Copy and normalize a direction vector with a stable default for invalid sources.
/// @param[out] dst Destination for the normalized three-component vector.
/// @param[in] src Preferred source direction.
/// @param[in] fallback Secondary direction used when @p src is unusable.
void vgfx3d_d3d11_copy_vec3_direction_or(float *dst, const float *src, const float fallback[3]) {
    static const float default_forward[3] = {0.0f, 0.0f, -1.0f};
    const float *chosen;
    double inv_length;
    double length_squared;

    if (!dst)
        return;
    if (vgfx3d_d3d11_vec3_direction_is_usable(src))
        chosen = src;
    else if (vgfx3d_d3d11_vec3_direction_is_usable(fallback))
        chosen = fallback;
    else
        chosen = default_forward;
    length_squared = (double)chosen[0] * (double)chosen[0] + (double)chosen[1] * (double)chosen[1] +
                     (double)chosen[2] * (double)chosen[2];
    inv_length = 1.0 / sqrt(length_squared);
    dst[0] = (float)((double)chosen[0] * inv_length);
    dst[1] = (float)((double)chosen[1] * inv_length);
    dst[2] = (float)((double)chosen[2] * inv_length);
}

/// @brief Copy a matrix when finite, otherwise write identity.
/// @param[out] dst Destination for sixteen row-major matrix elements.
/// @param[in] src Preferred matrix, or `NULL`; all elements must be bounded.
void vgfx3d_d3d11_copy_mat4_finite_or_identity(float *dst, const float *src) {
    if (!dst)
        return;
    if (src &&
        vgfx3d_d3d11_float_array_is_bounded(src, 16u, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX)) {
        memcpy(dst, src, sizeof(float) * 16u);
        return;
    }
    vgfx3d_d3d11_store_identity4x4(dst);
}

/// @brief Copy a matrix when finite, otherwise copy a finite fallback or identity.
/// @param[out] dst Destination for sixteen row-major matrix elements.
/// @param[in] src Preferred matrix, or `NULL`.
/// @param[in] fallback Secondary matrix, or `NULL`; identity is the final fallback.
void vgfx3d_d3d11_copy_mat4_finite_or(float *dst, const float *src, const float *fallback) {
    if (!dst)
        return;
    if (src &&
        vgfx3d_d3d11_float_array_is_bounded(src, 16u, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX)) {
        memcpy(dst, src, sizeof(float) * 16u);
        return;
    }
    if (fallback &&
        vgfx3d_d3d11_float_array_is_bounded(fallback, 16u, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX)) {
        memcpy(dst, fallback, sizeof(float) * 16u);
        return;
    }
    vgfx3d_d3d11_store_identity4x4(dst);
}

/// @brief Validate a bounded light view-projection matrix with at least one useful lane.
/// @param[in] matrix Sixteen-element matrix to inspect.
/// @return One when all elements are bounded and at least one is non-negligible; otherwise zero.
int vgfx3d_d3d11_shadow_matrix_is_usable(const float *matrix) {
    float max_abs = 0.0f;

    if (!vgfx3d_d3d11_float_array_is_bounded(matrix, 16u, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX))
        return 0;
    for (size_t i = 0; i < 16u; i++) {
        float magnitude = fabsf(matrix[i]);
        if (magnitude > max_abs)
            max_abs = magnitude;
    }
    return max_abs > 1.0e-12f ? 1 : 0;
}

/// @brief Copy a bone palette (mat4 per bone) into a fixed-size cbuffer slot.
/// Fills unused slots with identity so out-of-range indices do not collapse vertices.
/// If @p bone_count exceeds `VGFX3D_D3D11_MAX_BONES`, the upload is clamped to
/// the largest supported palette size for this backend.
/// @param[out] dst Fixed-capacity destination palette.
/// @param[in] src Contiguous row-major source matrices, or `NULL`.
/// @param[in] bone_count Number of source matrices requested for packing.
void vgfx3d_d3d11_pack_bone_palette(float *dst, const float *src, int32_t bone_count) {
    int32_t first_unused = 0;

    if (!dst)
        return;

    if (src && bone_count > 0) {
        if (bone_count > VGFX3D_D3D11_MAX_BONES)
            bone_count = VGFX3D_D3D11_MAX_BONES;
        for (int32_t i = 0; i < bone_count; i++)
            vgfx3d_d3d11_copy_mat4_finite_or_identity(&dst[(size_t)i * 16u], &src[(size_t)i * 16u]);
        first_unused = bone_count;
    }
    for (int32_t i = first_unused; i < VGFX3D_D3D11_MAX_BONES; i++)
        vgfx3d_d3d11_store_identity4x4(&dst[(size_t)i * 16u]);
}

/// @brief Build per-instance cbuffer entries (model + normal + prev_model) for instanced draws.
/// When previous-frame matrices are missing, prev_model is filled with the current model so
/// motion-vector shaders compute zero displacement (no false motion).
/// @param[out] dst Destination array of per-instance constant-buffer entries.
/// @param[in] instance_count Number of instances to populate.
/// @param[in] instance_matrices Current row-major model matrices.
/// @param[in] prev_instance_matrices Optional previous-frame model matrices.
/// @param[in] has_prev_instance_matrices Nonzero when the previous matrix array is valid.
void vgfx3d_d3d11_fill_instance_data(vgfx3d_d3d11_instance_data_t *dst,
                                     int32_t instance_count,
                                     const float *instance_matrices,
                                     const float *prev_instance_matrices,
                                     int8_t has_prev_instance_matrices) {
    if (!dst || instance_count <= 0 || !instance_matrices)
        return;

    for (int32_t i = 0; i < instance_count; i++) {
        const float *model = &instance_matrices[(size_t)i * 16u];
        vgfx3d_d3d11_copy_mat4_finite_or_identity(dst[i].model, model);
        vgfx3d_compute_normal_matrix4(dst[i].model, dst[i].normal);
        if (has_prev_instance_matrices && prev_instance_matrices) {
            vgfx3d_d3d11_copy_mat4_finite_or(
                dst[i].prev_model, &prev_instance_matrices[(size_t)i * 16u], dst[i].model);
        } else {
            memcpy(dst[i].prev_model, dst[i].model, sizeof(dst[i].prev_model));
        }
    }
}

/// @brief Decide whether instanced motion-history attributes are actually available.
/// @param[in] prev_instance_matrices Optional previous-frame matrix array.
/// @param[in] has_prev_instance_matrices Caller-provided availability flag.
/// @return One only when the availability flag is set and the array exists.
int vgfx3d_d3d11_should_use_previous_instance_matrices(const float *prev_instance_matrices,
                                                       int8_t has_prev_instance_matrices) {
    return has_prev_instance_matrices && prev_instance_matrices ? 1 : 0;
}

/// @brief Roll the per-frame VP/inv-VP/cam-pos history forward by one frame.
/// Scene VP and overlay VP are tracked separately because overlay passes use
/// the current VP for both "current" and "previous" (no temporal coherence).
/// @param[in,out] history History record to update.
/// @param[in] vp Current view-projection matrix.
/// @param[in] inv_vp Current inverse view-projection matrix.
/// @param[in] cam_pos Optional current camera position.
/// @param[in] is_overlay_pass Nonzero when updating an overlay rather than scene pass.
/// @param[in] uses_separate_overlay_target Nonzero when the overlay has a distinct target.
void vgfx3d_d3d11_update_frame_history(vgfx3d_d3d11_frame_history_t *history,
                                       const float *vp,
                                       const float *inv_vp,
                                       const float *cam_pos,
                                       int8_t is_overlay_pass,
                                       int8_t uses_separate_overlay_target) {
    float safe_vp[16];
    float safe_inv_vp[16];

    if (!history || !vp || !inv_vp)
        return;
    vgfx3d_d3d11_copy_mat4_finite_or_identity(safe_vp, vp);
    vgfx3d_d3d11_copy_mat4_finite_or_identity(safe_inv_vp, inv_vp);

    if (!is_overlay_pass) {
        if (history->scene_history_valid) {
            memcpy(history->scene_prev_vp, history->scene_vp, sizeof(history->scene_prev_vp));
        } else {
            memcpy(history->scene_prev_vp, safe_vp, sizeof(history->scene_prev_vp));
            history->scene_history_valid = 1;
        }
        memcpy(history->scene_vp, safe_vp, sizeof(history->scene_vp));
        memcpy(history->scene_inv_vp, safe_inv_vp, sizeof(history->scene_inv_vp));
        memcpy(history->draw_prev_vp, history->scene_prev_vp, sizeof(history->draw_prev_vp));
        if (cam_pos)
            vgfx3d_d3d11_copy_float_array_finite_or(history->scene_cam_pos, cam_pos, 3u, 0.0f);
        history->overlay_used_this_frame = 0;
        return;
    }

    memcpy(history->draw_prev_vp, safe_vp, sizeof(history->draw_prev_vp));
    history->overlay_used_this_frame = uses_separate_overlay_target ? 1 : 0;
}

/// @brief Reconcile bone-buffer upload outcomes into the per-object draw flags.
/// If the current upload failed, both skinning flags are cleared so the shader
/// falls back to the unskinned path. If only the prev-frame upload failed, motion
/// vectors degrade gracefully to "no skinning history".
/// @param[in,out] object_data Per-object flags to reconcile.
/// @param[in] current_upload_ok Nonzero when the current bone palette was uploaded.
/// @param[in] prev_upload_ok Nonzero when the previous-frame palette was uploaded.
void vgfx3d_d3d11_resolve_bone_upload_status(vgfx3d_d3d11_per_object_t *object_data,
                                             int current_upload_ok,
                                             int prev_upload_ok) {
    if (!object_data)
        return;
    if (!current_upload_ok) {
        object_data->has_skinning = 0;
        object_data->has_prev_skinning = 0;
        return;
    }
    if (!prev_upload_ok)
        object_data->has_prev_skinning = 0;
}

/// @brief Decide if shader skinning can run; palette uploads clamp to shader capacity.
/// @param[in] bone_palette Optional source bone palette.
/// @param[in] bone_count Number of matrices advertised by @p bone_palette.
/// @return One when a nonempty palette is available; otherwise zero.
int vgfx3d_d3d11_should_enable_skinning(const float *bone_palette, int32_t bone_count) {
    return (bone_palette && bone_count > 0) ? 1 : 0;
}

/// @brief Reconcile morph-target upload outcomes (positions and normals) into draw flags.
/// On failure the shape count drops to 0 (mesh renders un-morphed). If only normal
/// deltas fail, the position morph still applies but normals will be re-derived from
/// the morphed positions.
/// @param[in,out] object_data Per-object morph flags to reconcile.
/// @param[in] morph_upload_ok Nonzero when position deltas were uploaded.
/// @param[in] morph_normal_upload_ok Nonzero when normal deltas were uploaded.
void vgfx3d_d3d11_resolve_morph_upload_status(vgfx3d_d3d11_per_object_t *object_data,
                                              int morph_upload_ok,
                                              int morph_normal_upload_ok) {
    if (!object_data)
        return;
    if (!morph_upload_ok) {
        object_data->morph_shape_count = 0;
        object_data->vertex_count = 0;
        object_data->has_prev_morph_weights = 0;
        object_data->has_morph_normal_deltas = 0;
        return;
    }
    if (!morph_normal_upload_ok)
        object_data->has_morph_normal_deltas = 0;
}

/// @brief Compute the number of mipmap levels required to reach 1×1 from (width × height).
/// Returns 0 for invalid dimensions. Used when creating textures with full mip chains.
/// @param[in] width Base-level width in pixels.
/// @param[in] height Base-level height in pixels.
/// @return Full mip-chain level count, or zero for nonpositive dimensions.
int32_t vgfx3d_d3d11_compute_mip_count(int32_t width, int32_t height) {
    int32_t mip_count = 1;

    if (width <= 0 || height <= 0)
        return 0;
    while (width > 1 || height > 1) {
        if (width > 1)
            width >>= 1;
        if (height > 1)
            height >>= 1;
        mip_count++;
    }
    return mip_count;
}

/// @brief Clamp material sampler anisotropy into the backend cacheable [1,16] range.
/// @param[in] requested Requested anisotropy level.
/// @return The request clamped to the backend-supported range.
int32_t vgfx3d_d3d11_sanitize_anisotropy(int32_t requested) {
    if (requested < 1)
        return 1;
    if (requested > VGFX3D_D3D11_MAX_TEXTURE_ANISOTROPY)
        return VGFX3D_D3D11_MAX_TEXTURE_ANISOTROPY;
    return requested;
}

/// @brief Convert sanitized anisotropy to a compact cache index [0,15].
/// @param[in] requested Requested anisotropy level.
/// @return Zero-based cache index for the sanitized level.
int32_t vgfx3d_d3d11_sampler_anisotropy_index(int32_t requested) {
    return vgfx3d_d3d11_sanitize_anisotropy(requested) - 1;
}

/// @brief Normalize texture UV-set selectors to the shader-visible uv0/uv1 range.
/// @param[in] requested Caller-provided UV-set selector.
/// @return Zero for UV0 or one for every positive selector.
int32_t vgfx3d_d3d11_sanitize_texture_uv_set(int32_t requested) {
    return requested > 0 ? 1 : 0;
}

/// @brief Clamp an integer cbuffer parameter, tolerating inverted caller bounds.
/// @param[in] requested Value to clamp.
/// @param[in] min_value First inclusive bound.
/// @param[in] max_value Second inclusive bound.
/// @return @p requested clamped between the ordered bounds.
int32_t vgfx3d_d3d11_clamp_int_param(int32_t requested, int32_t min_value, int32_t max_value) {
    int32_t tmp;

    if (min_value > max_value) {
        tmp = min_value;
        min_value = max_value;
        max_value = tmp;
    }
    if (requested < min_value)
        return min_value;
    if (requested > max_value)
        return max_value;
    return requested;
}

/// @brief Replace NaN/Inf float parameters before D3D11 cbuffer/state upload.
/// @param[in] requested Preferred value.
/// @param[in] fallback Replacement when @p requested is not finite.
/// @return The requested value when finite, otherwise a finite fallback or zero.
float vgfx3d_d3d11_finite_or(float requested, float fallback) {
    if (isfinite(requested))
        return requested;
    return isfinite(fallback) ? fallback : 0.0f;
}

/// @brief Clamp a finite float parameter, tolerating inverted caller bounds.
/// @param[in] requested Preferred value.
/// @param[in] min_value First inclusive bound.
/// @param[in] max_value Second inclusive bound.
/// @param[in] fallback Replacement when the requested value is not finite.
/// @return A finite value clamped between normalized bounds.
float vgfx3d_d3d11_clamp_float_param(float requested,
                                     float min_value,
                                     float max_value,
                                     float fallback) {
    float safe_fallback = isfinite(fallback) ? fallback : 0.0f;
    float tmp;

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
        tmp = min_value;
        min_value = max_value;
        max_value = tmp;
    }
    if (requested < min_value)
        return min_value;
    if (requested > max_value)
        return max_value;
    return requested;
}

/// @brief Normalize arbitrary integer flags to shader-facing 0/1 values.
/// @param[in] requested Arbitrary caller-provided flag.
/// @return One for any nonzero input; otherwise zero.
int32_t vgfx3d_d3d11_sanitize_bool_flag(int32_t requested) {
    return requested ? 1 : 0;
}

/// @brief Normalize light type constants before indexing shader-side branches.
/// @param[in] requested Caller-provided light type.
/// @return A supported type in `[0, 6]`, defaulting to zero.
int32_t vgfx3d_d3d11_sanitize_light_type(int32_t requested) {
    return requested >= 0 && requested <= 6 ? requested : 0;
}

/// @brief Normalize shadow projection constants after the shadow slot is known valid.
/// @param[in] sanitized_shadow_index Previously validated shadow slot, or a negative sentinel.
/// @param[in] requested_projection_type Caller-provided projection type.
/// @return Perspective or cube when valid for a real slot; otherwise orthographic.
int32_t vgfx3d_d3d11_sanitize_shadow_projection_type(int32_t sanitized_shadow_index,
                                                     int32_t requested_projection_type) {
    if (sanitized_shadow_index < 0)
        return VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
    if (requested_projection_type == VGFX3D_SHADOW_PROJECTION_PERSPECTIVE ||
        requested_projection_type == VGFX3D_SHADOW_PROJECTION_CUBE)
        return requested_projection_type;
    return VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
}

/// @brief Clamp and order spot-light cone cosines before shader upload.
/// @param[in] requested_inner Requested inner-cone cosine.
/// @param[in] requested_outer Requested outer-cone cosine.
/// @param[out] out_inner Optional receiver for the sanitized inner cosine.
/// @param[out] out_outer Optional receiver for the sanitized outer cosine.
void vgfx3d_d3d11_sanitize_spot_cone(float requested_inner,
                                     float requested_outer,
                                     float *out_inner,
                                     float *out_outer) {
    float inner = vgfx3d_d3d11_clamp_float_param(requested_inner, -1.0f, 1.0f, 1.0f);
    float outer = vgfx3d_d3d11_clamp_float_param(requested_outer, -1.0f, 1.0f, 0.0f);

    if (inner < outer) {
        float tmp = inner;
        inner = outer;
        outer = tmp;
    }
    if (out_inner)
        *out_inner = inner;
    if (out_outer)
        *out_outer = outer;
}

/// @brief Sanitize shadow cascade split distances into a finite nondecreasing sequence.
/// @param[out] dst Destination split array.
/// @param[in] src Optional source split array.
/// @param[in] count Number of split values to write.
void vgfx3d_d3d11_sanitize_shadow_cascade_splits(float *dst, const float *src, size_t count) {
    float previous = 0.0f;

    if (!dst)
        return;
    for (size_t i = 0; i < count; i++) {
        float split = src ? src[i] : 0.0f;
        split =
            vgfx3d_d3d11_clamp_float_param(split, 0.0f, VGFX3D_D3D11_POSTFX_SCALAR_MAX, previous);
        if (split < previous)
            split = previous;
        dst[i] = split;
        previous = split;
    }
}

/// @brief Clamp a clustered-light global prefix to the uploaded light-array range.
/// @param[in] requested Requested number of globally evaluated lights, or a negative sentinel.
/// @param[in] light_count Number of uploaded lights.
/// @return `-1` for a disabled clustered path, otherwise a count bounded by uploaded lights.
int32_t vgfx3d_d3d11_sanitize_cluster_global_count(int32_t requested, int32_t light_count) {
    int32_t max_count;

    if (requested < 0)
        return -1;
    max_count = vgfx3d_d3d11_clamp_int_param(light_count, 0, VGFX3D_MAX_LIGHTS);
    return requested > max_count ? max_count : requested;
}

/// @brief Validate a clustered-light table before uploading it to fixed-size HLSL arrays.
/// @param[in] table Cluster offsets, indices, depth range, and revision to validate.
/// @param[in] expected_revision Required nonzero light revision.
/// @param[in] light_count Number of lights expected in the table.
/// @return One when all metadata, offsets, and indices are internally consistent.
int vgfx3d_d3d11_cluster_table_is_usable(const vgfx3d_cluster_table_t *table,
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
    for (int32_t i = 1; i <= VGFX3D_CLUSTER_COUNT; i++) {
        uint16_t offset = table->offsets[i];
        if (offset < previous_offset || offset > VGFX3D_MAX_CLUSTER_LIGHT_INDICES)
            return 0;
        previous_offset = offset;
    }

    final_offset = table->offsets[VGFX3D_CLUSTER_COUNT];
    for (uint32_t i = 0; i < (uint32_t)final_offset; i++) {
        uint16_t light_index = table->indices[i];
        if ((int32_t)light_index < table->global_light_count || light_index >= light_count)
            return 0;
    }
    return 1;
}

/// @brief Sanitize the clustered-light logarithmic Z range before shader upload.
/// @param[in] requested_near Requested positive near distance.
/// @param[in] requested_far Requested far distance greater than the near distance.
/// @param[out] out_near Optional receiver for the sanitized near distance.
/// @param[out] out_far Optional receiver for the sanitized far distance.
void vgfx3d_d3d11_sanitize_cluster_depth_range(float requested_near,
                                               float requested_far,
                                               float *out_near,
                                               float *out_far) {
    float znear;
    float zfar;
    float min_far;
    float fallback_far;

    znear = vgfx3d_d3d11_clamp_float_param(requested_near,
                                           VGFX3D_D3D11_CLUSTER_ZNEAR_MIN,
                                           VGFX3D_D3D11_CLUSTER_ZFAR_MAX * 0.5f,
                                           VGFX3D_D3D11_CLUSTER_ZNEAR_FALLBACK);
    min_far = znear * (1.0f + 1.0e-3f);
    fallback_far = znear * VGFX3D_D3D11_CLUSTER_ZFAR_FALLBACK;
    if (!isfinite(fallback_far) || fallback_far <= min_far)
        fallback_far = znear + VGFX3D_D3D11_CLUSTER_ZFAR_FALLBACK;
    if (fallback_far > VGFX3D_D3D11_CLUSTER_ZFAR_MAX)
        fallback_far = VGFX3D_D3D11_CLUSTER_ZFAR_MAX;

    if (!isfinite(requested_far) || requested_far <= min_far)
        zfar = fallback_far;
    else if (requested_far > VGFX3D_D3D11_CLUSTER_ZFAR_MAX)
        zfar = VGFX3D_D3D11_CLUSTER_ZFAR_MAX;
    else
        zfar = requested_far;

    if (zfar <= min_far) {
        znear = VGFX3D_D3D11_CLUSTER_ZNEAR_FALLBACK;
        zfar = VGFX3D_D3D11_CLUSTER_ZFAR_FALLBACK;
    }
    if (out_near)
        *out_near = znear;
    if (out_far)
        *out_far = zfar;
}

/// @brief Sanitize slope-scaled rasterizer bias before D3D11 state creation/cache keys.
/// @param[in] requested Requested slope-scaled depth bias.
/// @return Finite bias clamped to the backend-supported magnitude.
float vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(float requested) {
    return vgfx3d_d3d11_clamp_float_param(requested,
                                          -VGFX3D_D3D11_MAX_SLOPE_SCALED_DEPTH_BIAS,
                                          VGFX3D_D3D11_MAX_SLOPE_SCALED_DEPTH_BIAS,
                                          0.0f);
}

/// @brief Convert constant depth bias using the selected depth convention.
/// @param[in] requested Requested floating-point bias.
/// @param[in] reversed_z Nonzero to invert the bias for reversed-Z rendering.
/// @return D3D11 integer depth-bias units.
int32_t vgfx3d_d3d11_depth_bias_units(float requested, int reversed_z) {
    float bias = vgfx3d_d3d11_finite_or(requested, 0.0f);
    return vgfx3d_depth_bias_d3d11_units(reversed_z ? -bias : bias);
}

/// @brief Clamp and sign slope-scaled depth bias using the selected depth convention.
/// @param[in] requested Requested slope-scaled bias.
/// @param[in] reversed_z Nonzero to convert for reversed-Z rendering.
/// @return Sanitized slope-scaled bias in the selected depth convention.
float vgfx3d_d3d11_depth_slope_bias(float requested, int reversed_z) {
    float bias = vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(requested);
    return reversed_z ? vgfx3d_depth_bias_slope_reversed_z(bias) : bias;
}

/// @brief Validate one packed per-instance bone palette against the fixed shader palette.
/// @param[in] requested_stride Bone matrices assigned to each instance.
/// @param[in] total_bone_count Total matrices in the packed palette.
/// @param[in] instance_count Number of instances using the palette.
/// @return The validated stride, or zero when counts are invalid or inconsistent.
int32_t vgfx3d_d3d11_sanitize_instance_bone_stride(int32_t requested_stride,
                                                   int32_t total_bone_count,
                                                   int32_t instance_count) {
    uint64_t required_bones;

    if (requested_stride <= 0)
        return 0;
    if (requested_stride > VGFX3D_D3D11_MAX_BONES || total_bone_count <= 0 ||
        total_bone_count > VGFX3D_D3D11_MAX_BONES || instance_count <= 0)
        return 0;
    required_bones = (uint64_t)(uint32_t)requested_stride * (uint64_t)(uint32_t)instance_count;
    if (required_bones != (uint64_t)(uint32_t)total_bone_count)
        return 0;
    return requested_stride;
}

/// @brief Convert one NDC coordinate to an in-bounds D3D11 texture coordinate.
/// @param[in] ndc Normalized-device coordinate, clamped to `[-1, 1]`.
/// @param[in] extent Texture extent along the selected axis.
/// @param[in] invert_axis Nonzero to reverse the unit coordinate before conversion.
/// @param[out] out_pixel Receives the clamped integer texel coordinate.
/// @return One on successful conversion; otherwise zero.
int vgfx3d_d3d11_ndc_to_pixel(float ndc, int32_t extent, int invert_axis, int32_t *out_pixel) {
    double unit;
    int32_t pixel;

    if (out_pixel)
        *out_pixel = 0;
    if (!out_pixel || !isfinite(ndc) || extent <= 0 ||
        extent > VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION)
        return 0;
    if (ndc < -1.0f)
        ndc = -1.0f;
    else if (ndc > 1.0f)
        ndc = 1.0f;
    unit = (double)ndc * 0.5 + 0.5;
    if (invert_axis)
        unit = 1.0 - unit;
    pixel = (int32_t)(unit * (double)extent);
    if (pixel >= extent)
        pixel = extent - 1;
    if (pixel < 0)
        pixel = 0;
    *out_pixel = pixel;
    return 1;
}

/// @brief Convert reversed-Z storage to canonical depth while rejecting invalid samples.
/// @param[in] reversed_depth Raw reversed-Z depth sample.
/// @return Sanitized canonical depth, or the shared invalid-depth sentinel.
float vgfx3d_d3d11_sanitize_depth_probe_result(float reversed_depth) {
    return vgfx3d_sanitize_reversed_depth_probe_result(reversed_depth);
}

/// @brief Keep the CPU-side SSR request identical to the shader's loop bounds.
/// @param[in] requested Requested ray-march step count.
/// @return Step count clamped to the shader-supported range.
int32_t vgfx3d_d3d11_sanitize_ssr_steps(int32_t requested) {
    return vgfx3d_d3d11_clamp_int_param(
        requested, VGFX3D_D3D11_SSR_STEPS_MIN, VGFX3D_D3D11_SSR_STEPS_MAX);
}

/// @brief Normalize material workflow constants before the shader branches on them.
/// @param[in] requested Caller-provided material workflow.
/// @return PBR for the exact PBR constant; otherwise the legacy workflow.
int32_t vgfx3d_d3d11_sanitize_material_workflow(int32_t requested) {
    return requested == RT_MATERIAL3D_WORKFLOW_PBR ? RT_MATERIAL3D_WORKFLOW_PBR
                                                   : RT_MATERIAL3D_WORKFLOW_LEGACY;
}

/// @brief Normalize alpha-mode constants before draw-state and shader upload.
/// @param[in] requested Caller-provided alpha mode.
/// @return A supported alpha mode, defaulting to opaque.
int32_t vgfx3d_d3d11_sanitize_alpha_mode(int32_t requested) {
    if (requested < RT_MATERIAL3D_ALPHA_MODE_OPAQUE || requested > RT_MATERIAL3D_ALPHA_MODE_BLEND)
        return RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    return requested;
}

/// @brief Normalize Game3D shading-model constants before shader upload.
/// @param[in] requested Caller-provided shading-model index.
/// @return A supported model index, defaulting to zero.
int32_t vgfx3d_d3d11_sanitize_shading_model(int32_t requested) {
    if (requested < 0 || requested > VGFX3D_D3D11_SHADING_MODEL_MAX)
        return 0;
    return requested;
}

/// @brief Normalize tonemap mode constants before shader upload.
/// @param[in] requested Caller-provided tonemap mode.
/// @return A supported mode index, defaulting to zero.
int32_t vgfx3d_d3d11_sanitize_tonemap_mode(int32_t requested) {
    if (requested < 0 || requested > VGFX3D_D3D11_TONEMAP_MODE_MAX)
        return 0;
    return requested;
}

/// @brief Sanitize fog near/far distances before scene constant upload.
/// @param[in] requested_near Requested fog start distance.
/// @param[in] requested_far Requested fog end distance.
/// @param[out] out_near Optional receiver for the sanitized start distance.
/// @param[out] out_far Optional receiver for the sanitized end distance.
void vgfx3d_d3d11_sanitize_fog_range(float requested_near,
                                     float requested_far,
                                     float *out_near,
                                     float *out_far) {
    float fog_near =
        vgfx3d_d3d11_clamp_float_param(requested_near, 0.0f, VGFX3D_D3D11_FOG_DISTANCE_MAX, 10.0f);
    float min_far = fog_near + 1.0f;
    float fog_far;

    if (min_far > VGFX3D_D3D11_FOG_DISTANCE_MAX)
        min_far = VGFX3D_D3D11_FOG_DISTANCE_MAX;
    fog_far = vgfx3d_d3d11_clamp_float_param(
        requested_far, min_far, VGFX3D_D3D11_FOG_DISTANCE_MAX, 50.0f);
    if (fog_far <= fog_near) {
        fog_near = 10.0f;
        fog_far = 50.0f;
    }
    if (out_near)
        *out_near = fog_near;
    if (out_far)
        *out_far = fog_far;
}

/// @brief Sanitize D3D11 shader-facing shadow depth bias.
/// @param[in] requested Requested shadow comparison bias.
/// @return Finite bias clamped to the supported magnitude.
float vgfx3d_d3d11_sanitize_shadow_bias(float requested) {
    return vgfx3d_d3d11_clamp_float_param(
        requested, -VGFX3D_D3D11_SHADOW_BIAS_MAX, VGFX3D_D3D11_SHADOW_BIAS_MAX, 0.0f);
}

/// @brief Validate a backend-facing post-FX chain before indexed iteration.
/// @param[in] chain Post-processing chain to validate.
/// @return Nonzero when the chain descriptor and effect count are usable.
int vgfx3d_d3d11_postfx_chain_is_usable(const vgfx3d_postfx_chain_t *chain) {
    return vgfx3d_postfx_chain_is_usable(chain);
}

/// @brief Return non-zero when one PostFX effect descriptor actually changes rendering.
/// @param[in] effect Effect descriptor to inspect.
/// @return One when the effect type's corresponding snapshot flag is enabled.
int vgfx3d_d3d11_postfx_effect_is_active(const vgfx3d_postfx_effect_desc_t *effect) {
    const vgfx3d_postfx_snapshot_t *snapshot;

    if (!effect)
        return 0;
    snapshot = &effect->snapshot;
    switch (effect->type) {
        case VGFX3D_POSTFX_EFFECT_BLOOM:
            return snapshot->bloom_enabled ? 1 : 0;
        case VGFX3D_POSTFX_EFFECT_TONEMAP:
            return snapshot->tonemap_explicit ? 1 : 0;
        case VGFX3D_POSTFX_EFFECT_FXAA:
            return snapshot->fxaa_enabled ? 1 : 0;
        case VGFX3D_POSTFX_EFFECT_COLOR_GRADE:
            return snapshot->color_grade_enabled ? 1 : 0;
        case VGFX3D_POSTFX_EFFECT_VIGNETTE:
            return snapshot->vignette_enabled ? 1 : 0;
        case VGFX3D_POSTFX_EFFECT_SSAO:
            return snapshot->ssao_enabled ? 1 : 0;
        case VGFX3D_POSTFX_EFFECT_DOF:
            return snapshot->dof_enabled ? 1 : 0;
        case VGFX3D_POSTFX_EFFECT_MOTION_BLUR:
            return snapshot->motion_blur_enabled ? 1 : 0;
        case VGFX3D_POSTFX_EFFECT_TAA:
            return snapshot->taa_enabled ? 1 : 0;
        case VGFX3D_POSTFX_EFFECT_SSR:
            return snapshot->ssr_enabled ? 1 : 0;
        default:
            return 0;
    }
}

/// @brief Return non-zero when a usable chain contains an active effect of @p type_value.
/// @param[in] chain Post-processing chain to inspect.
/// @param[in] type_value Effect type to locate.
/// @return One when a matching active effect is present; otherwise zero.
int vgfx3d_d3d11_postfx_chain_has_active_effect(const vgfx3d_postfx_chain_t *chain,
                                                int32_t type_value) {
    if (!vgfx3d_d3d11_postfx_chain_is_usable(chain))
        return 0;
    for (int32_t i = 0; i < chain->effect_count; i++) {
        if (chain->effects[i].type == type_value &&
            vgfx3d_d3d11_postfx_effect_is_active(&chain->effects[i]))
            return 1;
    }
    return 0;
}

/// @brief Return non-zero when a usable chain contains any active effect.
/// @param[in] chain Post-processing chain to inspect.
/// @return One when at least one effect is active; otherwise zero.
int vgfx3d_d3d11_postfx_chain_has_active_effects(const vgfx3d_postfx_chain_t *chain) {
    if (!vgfx3d_d3d11_postfx_chain_is_usable(chain))
        return 0;
    for (int32_t i = 0; i < chain->effect_count; i++) {
        if (vgfx3d_d3d11_postfx_effect_is_active(&chain->effects[i]))
            return 1;
    }
    return 0;
}

/// @brief Decide whether a draw needs current/previous bone cbuffer uploads.
/// @param[in] has_skinning Nonzero when current-frame skinning is enabled.
/// @param[in] has_prev_skinning Nonzero when previous-frame skinning history is enabled.
/// @return One when either palette is required; otherwise zero.
int vgfx3d_d3d11_should_upload_bone_palette(int has_skinning, int has_prev_skinning) {
    return has_skinning || has_prev_skinning ? 1 : 0;
}

/// @brief Add two uint64_t counters with saturation instead of wraparound.
/// @param[in] a First unsigned counter.
/// @param[in] b Second unsigned counter.
/// @return Their sum, saturated at `UINT64_MAX`.
uint64_t vgfx3d_d3d11_saturating_add_u64(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

/// @brief Capacity-doubling growth helper: returns the next power-of-2 capacity >= @p needed.
/// Saturates at @p needed when doubling would overflow INT_MAX.
/// @param[in] current_capacity Existing allocation capacity.
/// @param[in] needed Minimum capacity required by the caller.
/// @param[in] minimum_capacity Initial growth floor when no capacity exists.
/// @return Existing or geometrically grown capacity sufficient for @p needed.
int32_t vgfx3d_d3d11_next_capacity(int32_t current_capacity,
                                   int32_t needed,
                                   int32_t minimum_capacity) {
    int32_t next_capacity;

    if (needed <= 0) {
        if (current_capacity > 0)
            return current_capacity;
        return minimum_capacity > 0 ? minimum_capacity : 1;
    }
    next_capacity = current_capacity > 0 ? current_capacity : minimum_capacity;
    if (next_capacity < 1)
        next_capacity = 1;
    while (next_capacity < needed) {
        if (next_capacity > INT_MAX / 2)
            return needed;
        next_capacity *= 2;
    }
    return next_capacity;
}

/// @brief Overflow-checked size_t multiplication used by byte-span helpers.
/// @details The destination is cleared before any validation so callers never
///   observe a stale byte count after a rejected span.
/// @param[in] a First multiplicand.
/// @param[in] b Second multiplicand.
/// @param[out] out Receives the product on success and zero on failure.
/// @return One when the product is representable; otherwise zero.
static int vgfx3d_d3d11_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (out)
        *out = 0;
    if (!out)
        return 0;
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

/// @brief Compute tightly packed row bytes for a positive-width pixel row.
/// @details Used before upload/readback copies so all callers share the same
///   overflow behavior and reject non-positive dimensions before casting to
///   unsigned D3D11 pitches.
/// @param[in] width Positive row width in pixels or elements.
/// @param[in] bytes_per_pixel Positive byte size of each element.
/// @param[out] out_bytes Receives the checked packed row size.
/// @return One on success; otherwise zero with @p out_bytes cleared.
int vgfx3d_d3d11_compute_row_bytes(int32_t width, int32_t bytes_per_pixel, size_t *out_bytes) {
    if (out_bytes)
        *out_bytes = 0;
    if (!out_bytes || width <= 0 || bytes_per_pixel <= 0)
        return 0;
    return vgfx3d_d3d11_checked_mul_size((size_t)width, (size_t)bytes_per_pixel, out_bytes);
}

/// @brief Compute a valid D3D11 buffer ByteWidth from a size_t byte count.
/// @param[in] size Requested nonzero buffer size.
/// @param[out] out_width Receives the D3D11-compatible unsigned width.
/// @return One when @p size fits `UINT`; otherwise zero.
int vgfx3d_d3d11_compute_buffer_byte_width(size_t size, uint32_t *out_width) {
    if (out_width)
        *out_width = 0;
    if (!out_width || size == 0 || size > UINT_MAX)
        return 0;
    *out_width = (uint32_t)size;
    return 1;
}

/// @brief Compute a valid 16-byte-aligned D3D11 constant-buffer ByteWidth.
/// @param[in] size Requested logical constant-buffer size.
/// @param[out] out_width Receives the aligned D3D11 byte width.
/// @return One when the aligned size is valid for a constant buffer; otherwise zero.
int vgfx3d_d3d11_compute_constant_buffer_byte_width(size_t size, uint32_t *out_width) {
    size_t aligned_size;

    if (out_width)
        *out_width = 0;
    if (!out_width || size == 0 || size > VGFX3D_D3D11_MAX_CONSTANT_BUFFER_BYTES ||
        size > SIZE_MAX - 15u)
        return 0;
    aligned_size = (size + 15u) & ~(size_t)15u;
    if (aligned_size == 0 || aligned_size > VGFX3D_D3D11_MAX_CONSTANT_BUFFER_BYTES ||
        aligned_size > UINT_MAX)
        return 0;
    *out_width = (uint32_t)aligned_size;
    return 1;
}

/// @brief Compute a D3D11 row pitch for a tightly packed RGBA8 upload.
/// @param[in] width Positive image width in pixels.
/// @param[out] out_pitch Receives four times @p width when representable.
/// @return One on success; otherwise zero.
int vgfx3d_d3d11_compute_rgba8_upload_pitch(int32_t width, uint32_t *out_pitch) {
    size_t row_bytes;

    if (out_pitch)
        *out_pitch = 0;
    if (!out_pitch || !vgfx3d_d3d11_compute_row_bytes(width, 4, &row_bytes) || row_bytes > UINT_MAX)
        return 0;
    *out_pitch = (uint32_t)row_bytes;
    return 1;
}

/// @brief Compute one mip level extent for a square D3D11 texture chain.
/// @param[in] base_extent Base square extent.
/// @param[in] mip_level Zero-based mip level.
/// @param[out] out_extent Receives the expected level extent.
/// @return One when the base and level are valid; otherwise zero.
int vgfx3d_d3d11_expected_square_mip_extent(int32_t base_extent,
                                            int32_t mip_level,
                                            int32_t *out_extent) {
    if (out_extent)
        *out_extent = 0;
    if (!out_extent || !vgfx3d_d3d11_is_valid_cubemap_extent(base_extent) || mip_level < 0)
        return 0;
    return vgfx3d_expected_square_mip_extent(base_extent, mip_level, out_extent);
}

/// @brief Compute a bloom mip extent using the backend's bounded half-res policy.
/// @param[in] width Source scene width in pixels.
/// @param[in] height Source scene height in pixels.
/// @param[in] mip_level Zero-based bloom mip level.
/// @param[out] out_width Receives the mip width.
/// @param[out] out_height Receives the mip height.
/// @return One when the requested bloom level is valid; otherwise zero.
int vgfx3d_d3d11_compute_bloom_mip_extent(
    int32_t width, int32_t height, int32_t mip_level, int32_t *out_width, int32_t *out_height) {
    int32_t mip_width;
    int32_t mip_height;

    if (out_width)
        *out_width = 0;
    if (out_height)
        *out_height = 0;
    if (!out_width || !out_height || mip_level < 0 ||
        mip_level >= VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX ||
        !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return 0;

    mip_width = width > 1 ? width / 2 : 1;
    mip_height = height > 1 ? height / 2 : 1;
    for (int32_t i = 0; i < mip_level; i++) {
        if (mip_width / 2 < VGFX3D_D3D11_BLOOM_MIN_DOWNSAMPLE_EXTENT ||
            mip_height / 2 < VGFX3D_D3D11_BLOOM_MIN_DOWNSAMPLE_EXTENT)
            return 0;
        mip_width = mip_width > 1 ? mip_width / 2 : 1;
        mip_height = mip_height > 1 ? mip_height / 2 : 1;
    }

    *out_width = mip_width > 0 ? mip_width : 1;
    *out_height = mip_height > 0 ? mip_height : 1;
    return 1;
}

/// @brief Validate an IBL mip payload extent against the destination cubemap mip.
/// @param[in] face_size Base cubemap face size.
/// @param[in] mip_level Zero-based mip level.
/// @param[in] width Payload width.
/// @param[in] height Payload height.
/// @return One when both payload dimensions equal the expected mip extent.
int vgfx3d_d3d11_validate_ibl_mip_extent(int32_t face_size,
                                         int32_t mip_level,
                                         int32_t width,
                                         int32_t height) {
    int32_t expected_extent;

    if (!vgfx3d_d3d11_expected_square_mip_extent(face_size, mip_level, &expected_extent))
        return 0;
    return width == expected_extent && height == expected_extent;
}

/// @brief Validate an IBL mip chain against a concrete D3D11 cubemap mip layout.
/// @param[in] face_size Destination cubemap base face size.
/// @param[in] ibl_base_size Source IBL chain base size.
/// @param[in] ibl_mip_count Number of source IBL levels.
/// @param[in] max_ibl_mips Maximum levels accepted by the destination.
/// @param[out] out_level_base Receives the destination level corresponding to the IBL base.
/// @return One when the chain maps exactly into the destination layout; otherwise zero.
int vgfx3d_d3d11_validate_ibl_layout(int32_t face_size,
                                     int32_t ibl_base_size,
                                     int32_t ibl_mip_count,
                                     int32_t max_ibl_mips,
                                     int32_t *out_level_base) {
    if (out_level_base)
        *out_level_base = 0;
    if (!out_level_base || !vgfx3d_d3d11_is_valid_cubemap_extent(face_size) || ibl_base_size <= 0 ||
        ibl_mip_count <= 0 || max_ibl_mips <= 0 || ibl_mip_count > max_ibl_mips)
        return 0;
    return vgfx3d_validate_cubemap_ibl_layout(
        face_size, ibl_base_size, ibl_mip_count, max_ibl_mips, out_level_base);
}

/// @brief Check a whole-resource upload against a saturating per-frame byte budget.
/// @param[in] budget Per-frame byte budget.
/// @param[in] used Bytes already consumed.
/// @param[in] requested Additional bytes requested.
/// @return One when the additional upload fits the budget; otherwise zero.
int vgfx3d_d3d11_upload_budget_allows(uint64_t budget, uint64_t used, uint64_t requested) {
    return vgfx3d_upload_budget_allows(budget, used, requested);
}

/// @brief Select cache-owned native telemetry or compute pending RGBA row bytes.
/// @param[in] has_native_asset Nonzero when upload telemetry comes from a native asset.
/// @param[in] cached_native_bytes Cached pending size for that native asset.
/// @param[in] width RGBA fallback width in pixels.
/// @param[in] height RGBA fallback height in pixels.
/// @param[in] next_row Next RGBA row awaiting upload.
/// @param[in] upload_in_progress Nonzero while either upload path is active.
/// @return Pending upload bytes for the selected asset representation.
uint64_t vgfx3d_d3d11_cached_pending_texture_bytes(int has_native_asset,
                                                   uint64_t cached_native_bytes,
                                                   int32_t width,
                                                   int32_t height,
                                                   int32_t next_row,
                                                   int upload_in_progress) {
    if (has_native_asset)
        return upload_in_progress ? cached_native_bytes : 0;
    return vgfx3d_pending_rgba_upload_bytes(width, height, next_row, upload_in_progress ? 1 : 0);
}

/// @brief Compute a checked per-instance vertex-buffer upload size.
/// @details The D3D11 `ByteWidth` field is a UINT, so the helper rejects both
///   size_t multiplication overflow and byte counts that cannot be represented
///   by the D3D11 buffer descriptor. The output is cleared on failure.
/// @param[in] instance_count Number of instances to upload.
/// @param[in] instance_stride Bytes occupied by each instance.
/// @param[out] out_bytes Receives the checked total byte size.
/// @return One when the total is nonzero and representable by D3D11; otherwise zero.
int vgfx3d_d3d11_compute_instance_upload_bytes(int32_t instance_count,
                                               size_t instance_stride,
                                               size_t *out_bytes) {
    size_t bytes;

    if (out_bytes)
        *out_bytes = 0;
    if (!out_bytes || instance_count <= 0 || instance_stride == 0)
        return 0;
    if (!vgfx3d_d3d11_checked_mul_size((size_t)instance_count, instance_stride, &bytes))
        return 0;
    if (bytes > UINT_MAX)
        return 0;
    *out_bytes = bytes;
    return 1;
}

/// @brief Compute the exact byte range for updating live float SRV elements.
/// @details The backing buffer can be larger than the live morph payload after
///   capacity growth; this helper keeps UpdateSubresource boxed to the live
///   elements and rejects stale counts that exceed the allocation.
/// @param[in] element_count Number of live float elements.
/// @param[in] capacity Allocated float-element capacity.
/// @param[out] out_bytes Receives the live byte count.
/// @return One when both counts are valid and the live range fits; otherwise zero.
int vgfx3d_d3d11_compute_float_srv_update_bytes(size_t element_count,
                                                size_t capacity,
                                                size_t *out_bytes) {
    if (out_bytes)
        *out_bytes = 0;
    if (!out_bytes || element_count > capacity ||
        !vgfx3d_d3d11_is_valid_float_srv_element_count(element_count) ||
        !vgfx3d_d3d11_is_valid_float_srv_element_count(capacity))
        return 0;
    return vgfx3d_d3d11_checked_mul_size(element_count, sizeof(float), out_bytes);
}

/// @brief Check the element limit for a typed D3D11 buffer SRV.
/// @param[in] element_count Number of float elements.
/// @return One when the count is nonzero and fits D3D11 and byte-width limits.
int vgfx3d_d3d11_is_valid_float_srv_element_count(size_t element_count) {
    return element_count > 0 && element_count <= VGFX3D_D3D11_MAX_BUFFER_TEXELS &&
           element_count <= (size_t)(UINT_MAX / sizeof(float));
}

/// @brief Grow typed-float storage geometrically while respecting D3D11 limits.
/// @param[in] current_capacity Existing float-element capacity.
/// @param[in] needed_capacity Required live float-element count.
/// @param[out] out_capacity Receives a valid retained or grown capacity.
/// @return One when a supported capacity can satisfy the request; otherwise zero.
int vgfx3d_d3d11_compute_float_srv_capacity(size_t current_capacity,
                                            size_t needed_capacity,
                                            size_t *out_capacity) {
    size_t capacity;

    if (out_capacity)
        *out_capacity = 0;
    if (!out_capacity || !vgfx3d_d3d11_is_valid_float_srv_element_count(needed_capacity))
        return 0;
    if (current_capacity >= needed_capacity &&
        vgfx3d_d3d11_is_valid_float_srv_element_count(current_capacity)) {
        *out_capacity = current_capacity;
        return 1;
    }
    capacity =
        vgfx3d_d3d11_is_valid_float_srv_element_count(current_capacity) ? current_capacity : 0u;
    if (capacity < VGFX3D_D3D11_MIN_FLOAT_SRV_CAPACITY)
        capacity = VGFX3D_D3D11_MIN_FLOAT_SRV_CAPACITY;
    while (capacity < needed_capacity) {
        if (capacity > (size_t)VGFX3D_D3D11_MAX_BUFFER_TEXELS / 2u) {
            capacity = VGFX3D_D3D11_MAX_BUFFER_TEXELS;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed_capacity || !vgfx3d_d3d11_is_valid_float_srv_element_count(capacity))
        return 0;
    *out_capacity = capacity;
    return 1;
}

/// @brief Validate a cached typed-float buffer before a boxed update.
/// @details The native byte width must encode exactly the backend-tracked
///   float-element capacity. This prevents stale metadata from authorizing an
///   UpdateSubresource box beyond the resource or seeding oversized growth.
/// @param[in] byte_width Native descriptor byte width.
/// @param[in] tracked_capacity Backend-tracked float-element capacity.
/// @param[in] required_elements Minimum elements needed for the pending upload.
/// @param[in] has_default_usage Nonzero when usage is DEFAULT.
/// @param[in] has_exact_srv_bind Nonzero for exactly SHADER_RESOURCE binding.
/// @param[in] has_no_cpu_access Nonzero when CPU access flags are zero.
/// @param[in] misc_flags Descriptor miscellaneous flags.
/// @param[in] structure_byte_stride Structured-buffer stride field.
/// @return One when the descriptor is safe for the pending update.
int vgfx3d_d3d11_float_srv_buffer_desc_is_usable(uint32_t byte_width,
                                                 size_t tracked_capacity,
                                                 size_t required_elements,
                                                 int has_default_usage,
                                                 int has_exact_srv_bind,
                                                 int has_no_cpu_access,
                                                 uint32_t misc_flags,
                                                 uint32_t structure_byte_stride) {
    size_t expected_bytes = 0u;

    return required_elements <= tracked_capacity &&
           vgfx3d_d3d11_is_valid_float_srv_element_count(tracked_capacity) &&
           vgfx3d_d3d11_checked_mul_size(tracked_capacity, sizeof(float), &expected_bytes) &&
           expected_bytes == (size_t)byte_width && has_default_usage && has_exact_srv_bind &&
           has_no_cpu_access && misc_flags == 0u && structure_byte_stride == 0u;
}

/// @brief Validate a cached typed-float shader-resource view.
/// @details Morph buffers expose every backing float starting at element zero;
///   accepting a narrower, offset, or differently typed view would bind stale
///   or unrelated data even when the backing buffer itself remained valid.
/// @param[in] tracked_capacity Backend-tracked float-element capacity.
/// @param[in] has_r32_float_format Nonzero for the R32_FLOAT format.
/// @param[in] has_buffer_dimension Nonzero for the BUFFER view dimension.
/// @param[in] first_element First element exposed by the view.
/// @param[in] num_elements Number of elements exposed by the view.
/// @return One when the view exposes exactly the tracked typed-float buffer.
int vgfx3d_d3d11_float_srv_view_desc_is_usable(size_t tracked_capacity,
                                               int has_r32_float_format,
                                               int has_buffer_dimension,
                                               uint32_t first_element,
                                               uint32_t num_elements) {
    return vgfx3d_d3d11_is_valid_float_srv_element_count(tracked_capacity) &&
           tracked_capacity == (size_t)num_elements && has_r32_float_format &&
           has_buffer_dimension && first_element == 0u;
}

/// @brief Validate the fields required for WRITE_DISCARD constant-buffer updates.
/// @param[in] byte_width Descriptor byte width.
/// @param[in] has_dynamic_usage Nonzero when usage is dynamic.
/// @param[in] has_constant_buffer_bind Nonzero when constant-buffer binding is enabled.
/// @param[in] has_cpu_write_access Nonzero when CPU write access is enabled.
/// @param[in] misc_flags Descriptor miscellaneous flags.
/// @param[in] structure_byte_stride Structured-buffer stride field.
/// @return One when the descriptor is valid for mapped constant-buffer updates.
int vgfx3d_d3d11_constant_buffer_desc_is_usable(uint32_t byte_width,
                                                int has_dynamic_usage,
                                                int has_constant_buffer_bind,
                                                int has_cpu_write_access,
                                                uint32_t misc_flags,
                                                uint32_t structure_byte_stride) {
    return byte_width > 0 && byte_width <= VGFX3D_D3D11_MAX_CONSTANT_BUFFER_BYTES &&
           (byte_width & 15u) == 0u && has_dynamic_usage && has_constant_buffer_bind &&
           has_cpu_write_access && misc_flags == 0u && structure_byte_stride == 0u;
}

/// @brief Validate a cached dynamic vertex/index buffer before WRITE_DISCARD reuse.
/// @details The native ByteWidth must agree exactly with the backend's tracked
///   capacity. This prevents stale metadata from authorizing a memcpy beyond
///   the mapped resource or seeding an unbounded replacement allocation.
/// @param[in] byte_width Native descriptor byte width.
/// @param[in] tracked_capacity Backend-tracked byte capacity.
/// @param[in] required_bytes Minimum bytes needed for the pending upload.
/// @param[in] has_dynamic_usage Nonzero when usage is dynamic.
/// @param[in] has_exact_bind_flags Nonzero when the bind class is exactly the requested class.
/// @param[in] has_cpu_write_access Nonzero when CPU write access is enabled.
/// @param[in] misc_flags Descriptor miscellaneous flags.
/// @param[in] structure_byte_stride Structured-buffer stride field.
/// @return One when the descriptor is safe for the pending mapped upload.
int vgfx3d_d3d11_dynamic_buffer_desc_is_usable(uint32_t byte_width,
                                               size_t tracked_capacity,
                                               size_t required_bytes,
                                               int has_dynamic_usage,
                                               int has_exact_bind_flags,
                                               int has_cpu_write_access,
                                               uint32_t misc_flags,
                                               uint32_t structure_byte_stride) {
    return byte_width > 0u && tracked_capacity == (size_t)byte_width &&
           required_bytes <= tracked_capacity && has_dynamic_usage && has_exact_bind_flags &&
           has_cpu_write_access && misc_flags == 0u && structure_byte_stride == 0u;
}

/// @brief Validate an RGBA8 destination rectangle and optionally return its byte span.
/// @details The total stride * height span is checked even when @p out_bytes is
///   NULL. That keeps callers that only need a boolean answer from accepting an
///   impossible destination size on narrower hosts.
/// @param[in] width Destination width in pixels.
/// @param[in] height Destination height in pixels.
/// @param[in] stride Destination row stride in bytes.
/// @param[out] out_bytes Optional receiver for the total destination span.
/// @return One when dimensions, stride, and total span are valid; otherwise zero.
int vgfx3d_d3d11_validate_rgba8_destination(int32_t width,
                                            int32_t height,
                                            int32_t stride,
                                            size_t *out_bytes) {
    size_t min_stride;
    size_t total_bytes;

    if (out_bytes)
        *out_bytes = 0;
    if (width <= 0 || height <= 0 || stride <= 0)
        return 0;
    if (!vgfx3d_d3d11_compute_row_bytes(width, 4, &min_stride))
        return 0;
    if ((size_t)stride < min_stride)
        return 0;
    if (!vgfx3d_d3d11_checked_mul_size((size_t)stride, (size_t)height, &total_bytes))
        return 0;
    if (out_bytes)
        *out_bytes = total_bytes;
    return 1;
}

/// @brief Validate a row span before converting it into unsigned D3D11 box bounds.
/// @param[in] extent Total row count.
/// @param[in] start First row in the requested span.
/// @param[in] count Number of rows in the span.
/// @return One when the positive span lies wholly within the extent.
int vgfx3d_d3d11_validate_row_span(int32_t extent, int32_t start, int32_t count) {
    if (extent <= 0 || start < 0 || count <= 0 || start >= extent)
        return 0;
    return count <= extent - start;
}

/// @brief Check 2D texture dimensions against D3D11 feature-level 11 limits.
/// @param[in] width Texture width in pixels.
/// @param[in] height Texture height in pixels.
/// @return One when both dimensions are positive and supported; otherwise zero.
int vgfx3d_d3d11_is_valid_texture2d_extent(int32_t width, int32_t height) {
    return width > 0 && height > 0 && width <= VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION &&
           height <= VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION;
}

/// @brief Validate the descriptor shape required by CopyResource into a single-mip staging tex.
/// @param[in] width Texture width.
/// @param[in] height Texture height.
/// @param[in] mip_levels Descriptor mip-level count.
/// @param[in] array_size Descriptor array size.
/// @param[in] sample_count Multisample count.
/// @param[in] sample_quality Multisample quality.
/// @return One for a bounded, single-level, non-array, non-multisampled texture.
int vgfx3d_d3d11_is_single_subresource_texture2d(uint32_t width,
                                                 uint32_t height,
                                                 uint32_t mip_levels,
                                                 uint32_t array_size,
                                                 uint32_t sample_count,
                                                 uint32_t sample_quality) {
    return width > 0 && height > 0 && width <= VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION &&
           height <= VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION && mip_levels == 1 && array_size == 1 &&
           sample_count == 1 && sample_quality == 0;
}

/// @brief Check a square cubemap face dimension against D3D11 limits.
/// @param[in] face_size Cubemap face width and height.
/// @return One when the positive size is supported; otherwise zero.
int vgfx3d_d3d11_is_valid_cubemap_extent(int32_t face_size) {
    return face_size > 0 && face_size <= VGFX3D_D3D11_MAX_CUBEMAP_DIMENSION;
}

/// @brief Validate an in-progress row upload without repairing corrupted state.
/// @param[in] extent Total row count.
/// @param[in] next_row Next row awaiting upload.
/// @return One when the cursor identifies an unfinished in-bounds row.
int vgfx3d_d3d11_row_upload_cursor_is_valid(int32_t extent, int32_t next_row) {
    return extent > 0 && next_row >= 0 && next_row < extent;
}

/// @brief Validate an in-progress cubemap face/row upload cursor.
/// @param[in] face_size Cubemap face extent.
/// @param[in] face Zero-based cubemap face index.
/// @param[in] next_row Next row awaiting upload within that face.
/// @return One when the face and row identify an unfinished upload.
int vgfx3d_d3d11_cubemap_upload_cursor_is_valid(int32_t face_size, int32_t face, int32_t next_row) {
    return vgfx3d_d3d11_is_valid_cubemap_extent(face_size) && face >= 0 && face < 6 &&
           vgfx3d_d3d11_row_upload_cursor_is_valid(face_size, next_row);
}

/// @brief Validate an in-progress native compressed mip/block-row cursor.
/// @param[in] mip_count Total native mip count.
/// @param[in] next_mip Next mip awaiting upload.
/// @param[in] block_rows Number of block rows in that mip.
/// @param[in] next_block_row Next compressed block row awaiting upload.
/// @return One when the cursor identifies an unfinished valid native upload.
int vgfx3d_d3d11_native_upload_cursor_is_valid(int64_t mip_count,
                                               int64_t next_mip,
                                               uint64_t block_rows,
                                               int32_t next_block_row) {
    return mip_count > 0 && next_mip >= 0 && next_mip < mip_count && block_rows > 0 &&
           block_rows <= UINT_MAX && next_block_row >= 0 &&
           (uint64_t)(uint32_t)next_block_row < block_rows;
}

/// @brief Validate source/destination row spans for a mapped texture readback copy.
/// @param[in] width Number of pixels copied per row.
/// @param[in] dst_stride Destination RGBA8 row stride in bytes.
/// @param[in] src_row_pitch Mapped source row pitch in bytes.
/// @param[in] src_bytes_per_pixel Source pixel size in bytes.
/// @param[out] out_src_row_bytes Optional receiver for the packed source row size.
/// @param[out] out_dst_row_bytes Optional receiver for the packed RGBA8 row size.
/// @return One when both row buffers can contain the requested copy; otherwise zero.
int vgfx3d_d3d11_validate_mapped_texture_copy(int32_t width,
                                              int32_t dst_stride,
                                              uint32_t src_row_pitch,
                                              int32_t src_bytes_per_pixel,
                                              size_t *out_src_row_bytes,
                                              size_t *out_dst_row_bytes) {
    size_t src_row_bytes;
    size_t dst_row_bytes;

    if (out_src_row_bytes)
        *out_src_row_bytes = 0;
    if (out_dst_row_bytes)
        *out_dst_row_bytes = 0;
    if (width <= 0 || dst_stride <= 0 || src_row_pitch == 0 || src_bytes_per_pixel <= 0)
        return 0;
    if (!vgfx3d_d3d11_compute_row_bytes(width, src_bytes_per_pixel, &src_row_bytes) ||
        !vgfx3d_d3d11_compute_row_bytes(width, 4, &dst_row_bytes))
        return 0;
    if ((size_t)src_row_pitch < src_row_bytes || (size_t)dst_stride < dst_row_bytes)
        return 0;
    if (out_src_row_bytes)
        *out_src_row_bytes = src_row_bytes;
    if (out_dst_row_bytes)
        *out_dst_row_bytes = dst_row_bytes;
    return 1;
}

/// @brief Bytes per compressed/native block row for D3D11 texture updates.
/// @param[in] mip Native mip descriptor.
/// @return Required block-row byte count, or zero for invalid input or overflow.
uint64_t vgfx3d_d3d11_native_mip_row_bytes(const vgfx3d_native_texture_mip_t *mip) {
    uint64_t cols;

    if (!mip || mip->width <= 0 || mip->block_width <= 0 || mip->block_bytes <= 0)
        return 0;
    cols = ((uint64_t)(uint32_t)mip->width + (uint64_t)(uint32_t)mip->block_width - 1u) /
           (uint64_t)(uint32_t)mip->block_width;
    if (cols > UINT64_MAX / (uint64_t)(uint32_t)mip->block_bytes)
        return 0;
    return cols * (uint64_t)(uint32_t)mip->block_bytes;
}

/// @brief Number of compressed/native block rows needed to cover a mip height.
/// @param[in] mip Native mip descriptor.
/// @return Required block-row count, or zero for an invalid descriptor.
uint64_t vgfx3d_d3d11_native_mip_block_rows(const vgfx3d_native_texture_mip_t *mip) {
    if (!mip || mip->height <= 0 || mip->block_height <= 0)
        return 0;
    return ((uint64_t)(uint32_t)mip->height + (uint64_t)(uint32_t)mip->block_height - 1u) /
           (uint64_t)(uint32_t)mip->block_height;
}

/// @brief Minimum payload bytes required by a complete compressed/native mip.
/// @param[in] mip Native mip descriptor.
/// @return Product of block-row size and count, or zero for invalid input or overflow.
uint64_t vgfx3d_d3d11_native_mip_required_bytes(const vgfx3d_native_texture_mip_t *mip) {
    uint64_t row_bytes;
    uint64_t block_rows;

    row_bytes = vgfx3d_d3d11_native_mip_row_bytes(mip);
    block_rows = vgfx3d_d3d11_native_mip_block_rows(mip);
    if (row_bytes == 0 || block_rows == 0 || block_rows > UINT64_MAX / row_bytes)
        return 0;
    return row_bytes * block_rows;
}

/// @brief Return the block footprint D3D11 expects for one native compressed format.
/// @param[in] format_id Runtime native texture format identifier.
/// @param[out] out_block_width Receives the format block width in texels.
/// @param[out] out_block_height Receives the format block height in texels.
/// @param[out] out_block_bytes Receives bytes occupied by one compressed block.
/// @return One for a supported BC format; otherwise zero with outputs cleared.
int vgfx3d_d3d11_native_format_block_layout(int32_t format_id,
                                            int32_t *out_block_width,
                                            int32_t *out_block_height,
                                            int32_t *out_block_bytes) {
    int32_t block_bytes = 0;

    if (out_block_width)
        *out_block_width = 0;
    if (out_block_height)
        *out_block_height = 0;
    if (out_block_bytes)
        *out_block_bytes = 0;
    if (!out_block_width || !out_block_height || !out_block_bytes)
        return 0;

    if (format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1 ||
        format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC4) {
        block_bytes = 8;
    } else if (format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC3 ||
               format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC5 ||
               format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7) {
        block_bytes = 16;
    } else {
        return 0;
    }

    *out_block_width = 4;
    *out_block_height = 4;
    *out_block_bytes = block_bytes;
    return 1;
}

/// @brief Check one native compressed mip against D3D11 chain and block invariants.
/// @param[in] mip Descriptor to validate.
/// @param[in] previous_mip Optional immediately preceding, larger mip descriptor.
/// @param[in] expected_format_id Required native format identifier.
/// @param[in] expected_block_width Required block width, or nonpositive to infer it.
/// @param[in] expected_block_height Required block height, or nonpositive to infer it.
/// @param[in] expected_block_bytes Required block byte size, or nonpositive to infer it.
/// @return One when format, extent, layout, and payload size are valid; otherwise zero.
int vgfx3d_d3d11_validate_native_mip_desc(const vgfx3d_native_texture_mip_t *mip,
                                          const vgfx3d_native_texture_mip_t *previous_mip,
                                          int32_t expected_format_id,
                                          int32_t expected_block_width,
                                          int32_t expected_block_height,
                                          int32_t expected_block_bytes) {
    uint64_t required_bytes;
    int32_t format_block_width;
    int32_t format_block_height;
    int32_t format_block_bytes;

    if (!mip || !mip->data || mip->bytes == 0 || expected_format_id <= 0)
        return 0;
    if (mip->bytes > UINT_MAX)
        return 0;
    if (!vgfx3d_d3d11_native_format_block_layout(
            expected_format_id, &format_block_width, &format_block_height, &format_block_bytes))
        return 0;
    if (!vgfx3d_d3d11_is_valid_texture2d_extent(mip->width, mip->height))
        return 0;
    if (mip->format_id != expected_format_id)
        return 0;
    if (mip->block_width <= 0 || mip->block_height <= 0 || mip->block_bytes <= 0)
        return 0;
    if (mip->block_width != format_block_width || mip->block_height != format_block_height ||
        mip->block_bytes != format_block_bytes)
        return 0;
    if (expected_block_width > 0 && mip->block_width != expected_block_width)
        return 0;
    if (expected_block_height > 0 && mip->block_height != expected_block_height)
        return 0;
    if (expected_block_bytes > 0 && mip->block_bytes != expected_block_bytes)
        return 0;
    if (previous_mip) {
        int32_t expected_width = previous_mip->width > 1 ? previous_mip->width >> 1 : 1;
        int32_t expected_height = previous_mip->height > 1 ? previous_mip->height >> 1 : 1;
        uint64_t previous_required_bytes;

        if (!previous_mip->data || previous_mip->bytes == 0 ||
            previous_mip->format_id != expected_format_id)
            return 0;
        if (!vgfx3d_d3d11_is_valid_texture2d_extent(previous_mip->width, previous_mip->height))
            return 0;
        if (previous_mip->block_width <= 0 || previous_mip->block_height <= 0 ||
            previous_mip->block_bytes <= 0)
            return 0;
        if (previous_mip->block_width != format_block_width ||
            previous_mip->block_height != format_block_height ||
            previous_mip->block_bytes != format_block_bytes)
            return 0;
        if (expected_block_width > 0 && previous_mip->block_width != expected_block_width)
            return 0;
        if (expected_block_height > 0 && previous_mip->block_height != expected_block_height)
            return 0;
        if (expected_block_bytes > 0 && previous_mip->block_bytes != expected_block_bytes)
            return 0;
        previous_required_bytes = vgfx3d_d3d11_native_mip_required_bytes(previous_mip);
        if (previous_required_bytes == 0 || previous_mip->bytes < previous_required_bytes ||
            previous_mip->bytes > UINT_MAX)
            return 0;
        if (mip->width != expected_width || mip->height != expected_height)
            return 0;
    }
    required_bytes = vgfx3d_d3d11_native_mip_required_bytes(mip);
    if (required_bytes == 0 || mip->bytes < required_bytes)
        return 0;
    return 1;
}

/// @brief Check that a native mip count can fit in D3D11's MipLevels field and chain length.
/// @param[in] base_width Base texture width.
/// @param[in] base_height Base texture height.
/// @param[in] mip_count Requested native mip count.
/// @return One when the count is positive, representable, and no longer than a full chain.
int vgfx3d_d3d11_is_valid_native_mip_count(int32_t base_width,
                                           int32_t base_height,
                                           int64_t mip_count) {
    if (!vgfx3d_d3d11_is_valid_texture2d_extent(base_width, base_height) || mip_count <= 0 ||
        mip_count > UINT_MAX)
        return 0;
    return mip_count <= vgfx3d_d3d11_compute_mip_count(base_width, base_height);
}

/// @brief Clamp morph shape count to shader and index-range limits.
/// @details HLSL buffer indexing is signed-int based in the shader source, so
///   the largest accepted shape count is also bounded by
///   `(shape * vertex_count + vertex_id) * 3 + component <= INT_MAX`.
/// @param[in] vertex_count Vertices addressed by each morph shape.
/// @param[in] requested_shape_count Requested number of shapes.
/// @return Shape count clamped to shader and signed-index limits, or zero.
int32_t vgfx3d_d3d11_clamp_morph_shape_count(uint32_t vertex_count, int32_t requested_shape_count) {
    int32_t shape_count;
    uint32_t max_indexed_vertices;
    uint32_t max_shapes_by_index;

    if (vertex_count == 0 || requested_shape_count <= 0)
        return 0;
    shape_count = requested_shape_count;
    if (shape_count > VGFX3D_D3D11_MAX_MORPH_SHAPES)
        shape_count = VGFX3D_D3D11_MAX_MORPH_SHAPES;
    max_indexed_vertices = (uint32_t)((INT_MAX - 2) / 3);
    max_shapes_by_index = max_indexed_vertices / vertex_count;
    if (max_shapes_by_index == 0)
        return 0;
    if ((uint32_t)shape_count > max_shapes_by_index)
        shape_count = (int32_t)max_shapes_by_index;
    return shape_count;
}

/// @brief Compute the float element count for a D3D11 morph-delta SRV upload.
/// @param[in] vertex_count Vertices addressed by each shape.
/// @param[in] requested_shape_count Requested number of shapes.
/// @param[out] out_elements Receives `shapes * vertices * 3` when valid.
/// @return One when the element and byte counts fit backend limits; otherwise zero.
int vgfx3d_d3d11_compute_morph_float_count(uint32_t vertex_count,
                                           int32_t requested_shape_count,
                                           size_t *out_elements) {
    size_t shaped_vertices;
    size_t elements;
    size_t bytes;
    int32_t shape_count;

    if (out_elements)
        *out_elements = 0;
    if (!out_elements)
        return 0;
    shape_count = vgfx3d_d3d11_clamp_morph_shape_count(vertex_count, requested_shape_count);
    if (shape_count <= 0)
        return 0;
    if (!vgfx3d_d3d11_checked_mul_size(
            (size_t)shape_count, (size_t)vertex_count, &shaped_vertices) ||
        !vgfx3d_d3d11_checked_mul_size(shaped_vertices, 3u, &elements) ||
        !vgfx3d_d3d11_checked_mul_size(elements, sizeof(float), &bytes) || bytes > UINT_MAX ||
        !vgfx3d_d3d11_is_valid_float_srv_element_count(elements))
        return 0;
    *out_elements = elements;
    return 1;
}

/// @brief Decide whether a compacting cache sweep can drop one aged entry.
/// @details The predicate keeps enough unvisited entries to preserve the
///   resident floor. `kept_count` is the number already copied to the compacted
///   prefix, and `scan_index` is the current entry in the original array.
/// @param[in] total_count Entries in the original cache.
/// @param[in] kept_count Entries already retained in the compacted prefix.
/// @param[in] scan_index Index currently being considered.
/// @param[in] age Current entry's age in frame serials.
/// @param[in] max_resident Minimum resident entry count to preserve.
/// @param[in] prune_age Age threshold that must be exceeded.
/// @return One when the current entry may be dropped safely; otherwise zero.
int vgfx3d_d3d11_should_prune_cache_entry(int32_t total_count,
                                          int32_t kept_count,
                                          int32_t scan_index,
                                          uint64_t age,
                                          int32_t max_resident,
                                          uint64_t prune_age) {
    int32_t remaining_after_current;

    if (total_count <= 0 || kept_count < 0 || scan_index < 0 || scan_index >= total_count)
        return 0;
    if (kept_count > total_count)
        return 0;
    if (max_resident < 0)
        max_resident = 0;
    if (total_count <= max_resident || age <= prune_age)
        return 0;
    if (kept_count >= max_resident)
        return 1;
    remaining_after_current = total_count - scan_index - 1;
    return remaining_after_current >= max_resident - kept_count;
}

/// @brief Convert one completed timestamp query pair to rounded microseconds.
/// @param[in] disjoint Nonzero when the timestamp interval is invalid.
/// @param[in] frequency GPU timestamp ticks per second.
/// @param[in] start_ticks Beginning timestamp.
/// @param[in] end_ticks Ending timestamp.
/// @param[out] out_microseconds Receives the rounded elapsed microseconds.
/// @return One for a valid ordered timestamp pair; otherwise zero.
int vgfx3d_d3d11_compute_gpu_time_us(int disjoint,
                                     uint64_t frequency,
                                     uint64_t start_ticks,
                                     uint64_t end_ticks,
                                     uint64_t *out_microseconds) {
    double microseconds;

    if (out_microseconds)
        *out_microseconds = 0;
    if (!out_microseconds || disjoint || frequency == 0 || end_ticks < start_ticks)
        return 0;
    microseconds = ((double)(end_ticks - start_ticks) * 1000000.0) / (double)frequency;
    if (!isfinite(microseconds) || microseconds < 0.0)
        return 0;
    *out_microseconds =
        microseconds >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)(microseconds + 0.5);
    return 1;
}

/// @brief Bound non-blocking timestamp polling so one lost query cannot disable telemetry forever.
/// @param[in] pending_polls Number of unsuccessful non-blocking polls.
/// @return One when the configured abandonment limit has been reached.
int vgfx3d_d3d11_should_abandon_frame_timing(uint32_t pending_polls) {
    return pending_polls >= VGFX3D_D3D11_FRAME_TIMING_PENDING_POLL_LIMIT;
}

/// @brief Bound non-blocking probe polling so one busy staging copy cannot starve later frames.
/// @param[in] pending_polls Number of unsuccessful non-blocking polls.
/// @return One when the configured depth-probe abandonment limit has been reached.
int vgfx3d_d3d11_should_abandon_depth_probe(uint32_t pending_polls) {
    return pending_polls >= VGFX3D_D3D11_DEPTH_PROBE_PENDING_POLL_LIMIT;
}

/// @brief Require both halves of the exported frame protocol to be idle.
/// @details EndFrame clears the active bit before Present consumes the pending
///   bit. Treating only `frame_active` as busy allowed target and post-FX
///   mutation to invalidate a completed frame before it reached the swapchain.
/// @param[in] frame_active Nonzero between begin-frame and end-frame.
/// @param[in] frame_pending_present Nonzero while a completed frame awaits presentation.
/// @return One only when neither protocol phase is active.
int vgfx3d_d3d11_frame_state_is_idle(int8_t frame_active, int8_t frame_pending_present) {
    return !frame_active && !frame_pending_present;
}

/// @brief Validate the state required by an ordinary color draw.
/// @details Shadow rendering has a dedicated submission entry point. Main and
///   overlay draws must therefore be inside BeginFrame/EndFrame, outside a
///   shadow pass, and have a concrete color target and positive viewport.
/// @param[in] frame_active Nonzero while an ordinary frame is open.
/// @param[in] shadow_pass_slot Active shadow slot, or a negative sentinel.
/// @param[in] has_device_context Nonzero when a native device context exists.
/// @param[in] render_target_count Number of bound color targets.
/// @param[in] has_primary_render_target Nonzero when color target zero is valid.
/// @param[in] target_width Active target width.
/// @param[in] target_height Active target height.
/// @return One when all ordinary-draw prerequisites are satisfied.
int vgfx3d_d3d11_draw_submission_is_ready(int8_t frame_active,
                                          int32_t shadow_pass_slot,
                                          int has_device_context,
                                          uint32_t render_target_count,
                                          int has_primary_render_target,
                                          int32_t target_width,
                                          int32_t target_height) {
    return frame_active && shadow_pass_slot < 0 && has_device_context && render_target_count > 0 &&
           render_target_count <= 2 && has_primary_render_target && target_width > 0 &&
           target_height > 0;
}

/// @brief Validate the fixed two-entry render-target mirror before passing it to D3D11.
/// @details A count above two would make `OMSetRenderTargets` read beyond the backend's
///   `current_rtvs` array. A missing view or mismatched empty-state extent can otherwise leave
///   the previous output/viewport active while the CPU mirror claims a different binding.
/// @param[in] render_target_count Number of color views requested.
/// @param[in] has_primary_render_target Whether slot zero is nonnull.
/// @param[in] has_secondary_render_target Whether slot one is nonnull.
/// @param[in] has_depth_target Whether a depth view accompanies the color binding.
/// @param[in] target_width Tracked viewport width.
/// @param[in] target_height Tracked viewport height.
/// @return One for canonical empty state or a complete one/two-view state.
int vgfx3d_d3d11_target_binding_is_usable(uint32_t render_target_count,
                                          int has_primary_render_target,
                                          int has_secondary_render_target,
                                          int has_depth_target,
                                          int32_t target_width,
                                          int32_t target_height) {
    if (render_target_count == 0)
        return !has_primary_render_target && !has_secondary_render_target && !has_depth_target &&
               target_width == 0 && target_height == 0;
    if (render_target_count > 2 || !has_primary_render_target || target_width <= 0 ||
        target_height <= 0) {
        return 0;
    }
    return render_target_count == 2 ? has_secondary_render_target : !has_secondary_render_target;
}

/// @brief Pick the right render-target classification for the current draw context.
/// Order of priority: explicit RTT > swapchain (no postfx) > overlay (loading existing
/// color) > scene (HDR intermediate that postfx will tonemap).
/// @param[in] rtt_active Nonzero when an explicit render target is bound.
/// @param[in] gpu_postfx_enabled Nonzero when window rendering uses the offscreen scene route.
/// @param[in] load_existing_color Nonzero when the pass must preserve prior color.
/// @return Target class selected by the backend routing priority.
vgfx3d_d3d11_target_kind_t vgfx3d_d3d11_choose_target_kind(int8_t rtt_active,
                                                           int8_t gpu_postfx_enabled,
                                                           int8_t load_existing_color) {
    if (rtt_active)
        return VGFX3D_D3D11_TARGET_RTT;
    if (!gpu_postfx_enabled)
        return VGFX3D_D3D11_TARGET_SWAPCHAIN;
    if (load_existing_color)
        return VGFX3D_D3D11_TARGET_OVERLAY;
    return VGFX3D_D3D11_TARGET_SCENE;
}

/// @brief Resolve a requested target to one with complete backing resources.
/// @details Invalid enum values are treated as swapchain requests rather than
///   propagated to the backend state machine. Overlay falls back to scene first
///   because that preserves the already-rendered 3D color when a separate HUD
///   target allocation failed.
/// @param[in] requested Requested target class.
/// @param[in] scene_available Nonzero when the scene target set is complete.
/// @param[in] overlay_available Nonzero when the overlay target set is complete.
/// @param[in] rtt_available Nonzero when the explicit RTT target set is complete.
/// @return The requested target when available, or its safest available fallback.
vgfx3d_d3d11_target_kind_t vgfx3d_d3d11_resolve_available_target(
    vgfx3d_d3d11_target_kind_t requested,
    int scene_available,
    int overlay_available,
    int rtt_available) {
    if (requested == VGFX3D_D3D11_TARGET_RTT)
        return rtt_available ? VGFX3D_D3D11_TARGET_RTT : VGFX3D_D3D11_TARGET_SWAPCHAIN;
    if (requested == VGFX3D_D3D11_TARGET_OVERLAY) {
        if (overlay_available)
            return VGFX3D_D3D11_TARGET_OVERLAY;
        return scene_available ? VGFX3D_D3D11_TARGET_SCENE : VGFX3D_D3D11_TARGET_SWAPCHAIN;
    }
    if (requested == VGFX3D_D3D11_TARGET_SCENE && !scene_available)
        return VGFX3D_D3D11_TARGET_SWAPCHAIN;
    if (requested == VGFX3D_D3D11_TARGET_SCENE || requested == VGFX3D_D3D11_TARGET_SWAPCHAIN)
        return requested;
    return VGFX3D_D3D11_TARGET_SWAPCHAIN;
}

/// @brief Decide whether a pass should preserve existing color contents.
/// @details Overlay targets only load after this frame has already rendered
///   into the separate overlay target; the first overlay pass clears stale HUD
///   contents from prior frames.
/// @param[in] target_kind Resolved target class for the pass.
/// @param[in] requested_load_existing_color Caller request to preserve target color.
/// @param[in] overlay_used_this_frame Nonzero after an earlier pass wrote the overlay target.
/// @return One when the selected pass should load rather than clear color.
int8_t vgfx3d_d3d11_should_load_existing_color(vgfx3d_d3d11_target_kind_t target_kind,
                                               int8_t requested_load_existing_color,
                                               int8_t overlay_used_this_frame) {
    if (!requested_load_existing_color)
        return 0;
    if (target_kind != VGFX3D_D3D11_TARGET_OVERLAY)
        return 1;
    return overlay_used_this_frame ? 1 : 0;
}

/// @brief Decide whether a cached morph SRV payload can be reused for a draw.
/// @details The key includes the normal-delta presence bit in addition to the
///   stable morph identity, revision, clamped shape count, and vertex count.
///   Without that bit, a payload uploaded for position-only morphing could be
///   reused for a later draw that requires normal deltas but did not change the
///   content revision.
/// @param[in] cached_key Stable identity stored by the cache entry.
/// @param[in] cached_revision Content revision stored by the cache entry.
/// @param[in] cached_shape_count Shape count stored by the cache entry.
/// @param[in] cached_vertex_count Vertex count stored by the cache entry.
/// @param[in] cached_has_normal_deltas Nonzero when cached normal deltas exist.
/// @param[in] cmd Candidate draw command.
/// @return One when every cache discriminator matches the sanitized command.
int vgfx3d_d3d11_should_reuse_morph_cache(const void *cached_key,
                                          uint64_t cached_revision,
                                          int32_t cached_shape_count,
                                          uint32_t cached_vertex_count,
                                          int8_t cached_has_normal_deltas,
                                          const vgfx3d_draw_cmd_t *cmd) {
    int32_t shape_count;
    int8_t has_normal_deltas;

    if (!cmd || !cmd->morph_key || cmd->morph_revision == 0 || !cmd->morph_deltas ||
        !cmd->morph_weights || cmd->morph_shape_count <= 0 || cmd->vertex_count == 0)
        return 0;

    shape_count = vgfx3d_d3d11_clamp_morph_shape_count(cmd->vertex_count, cmd->morph_shape_count);
    if (shape_count <= 0)
        return 0;
    has_normal_deltas = cmd->morph_normal_deltas ? 1 : 0;
    return cached_key == cmd->morph_key && cached_revision == cmd->morph_revision &&
           cached_shape_count == shape_count && cached_vertex_count == cmd->vertex_count &&
           (cached_has_normal_deltas ? 1 : 0) == has_normal_deltas;
}

/// @brief Count contiguous complete shadow slots starting at slot 0.
/// @details The HLSL shader receives only `shadowCount` plus two fixed SRV
///   bindings, so advertising a higher slot while an earlier slot is missing
///   would let a light sample an unbound shadow texture. Requiring a contiguous
///   prefix keeps `0 <= shadowIndex < shadowCount` equivalent to "SRV exists".
/// @param[in] slot_count Number of entries exposed by @p slot_complete.
/// @param[in] slot_complete Per-slot completion flags starting at slot zero.
/// @return Length of the contiguous positive prefix, bounded by backend capacity.
int32_t vgfx3d_d3d11_compute_shadow_count(int32_t slot_count, const int *slot_complete) {
    int32_t count = 0;
    int32_t max_slots;

    if (!slot_complete || slot_count <= 0)
        return 0;
    max_slots = slot_count > VGFX3D_MAX_SHADOW_LIGHTS ? VGFX3D_MAX_SHADOW_LIGHTS : slot_count;
    while (count < max_slots && slot_complete[count] > 0)
        count++;
    return count;
}

/// @brief Sanitize a light's requested shadow slot against the advertised range.
/// @details Invalid, negative, sparse, or out-of-range slots are converted to -1,
///   which the shader treats as unshadowed. This prevents stale scene data from
///   indexing a shadow SRV that was not allocated this frame.
/// @param[in] requested_shadow_index Light-provided shadow slot.
/// @param[in] advertised_shadow_count Number of contiguous completed slots.
/// @return Valid slot index, or `-1` when the light must be treated as unshadowed.
int32_t vgfx3d_d3d11_sanitize_shadow_index(int32_t requested_shadow_index,
                                           int32_t advertised_shadow_count) {
    advertised_shadow_count = vgfx3d_d3d11_clamp_shadow_count(advertised_shadow_count);
    if (advertised_shadow_count <= 0 || requested_shadow_index < 0 ||
        requested_shadow_index >= advertised_shadow_count)
        return -1;
    return requested_shadow_index;
}

/// @brief Require every cube face to fit in the completed contiguous shadow prefix.
/// @param[in] requested_shadow_index First slot requested by the light.
/// @param[in] advertised_shadow_count Number of contiguous completed slots.
/// @param[in] projection_type Sanitized light shadow projection type.
/// @return Valid first slot, or `-1` when the projection cannot fit.
int32_t vgfx3d_d3d11_sanitize_shadow_index_for_projection(int32_t requested_shadow_index,
                                                          int32_t advertised_shadow_count,
                                                          int32_t projection_type) {
    int32_t shadow_index =
        vgfx3d_d3d11_sanitize_shadow_index(requested_shadow_index, advertised_shadow_count);

    if (shadow_index < 0 || projection_type != VGFX3D_SHADOW_PROJECTION_CUBE)
        return shadow_index;
    advertised_shadow_count = vgfx3d_d3d11_clamp_shadow_count(advertised_shadow_count);
    if (advertised_shadow_count - shadow_index < VGFX3D_SHADOW_CUBE_FACES)
        return -1;
    return shadow_index;
}

/// @brief Clamp a light's cascade count so it cannot address beyond advertised shadow slots.
/// @param[in] requested_cascade_count Requested directional cascade count.
/// @param[in] sanitized_shadow_index Validated first shadow slot.
/// @param[in] advertised_shadow_count Number of contiguous completed slots.
/// @param[in] projection_type Sanitized shadow projection type.
/// @return Safe cascade count; non-cascade and invalid cases resolve to one.
int32_t vgfx3d_d3d11_sanitize_shadow_cascade_count(int32_t requested_cascade_count,
                                                   int32_t sanitized_shadow_index,
                                                   int32_t advertised_shadow_count,
                                                   int32_t projection_type) {
    int32_t remaining_slots;

    advertised_shadow_count = vgfx3d_d3d11_clamp_shadow_count(advertised_shadow_count);
    if (sanitized_shadow_index < 0 || sanitized_shadow_index >= advertised_shadow_count)
        return 1;
    if (projection_type == VGFX3D_SHADOW_PROJECTION_CUBE)
        return 1;
    remaining_slots = advertised_shadow_count - sanitized_shadow_index;
    if (remaining_slots > VGFX3D_CSM_SLOTS)
        remaining_slots = VGFX3D_CSM_SLOTS;
    if (requested_cascade_count < 1)
        return 1;
    return requested_cascade_count > remaining_slots ? remaining_slots : requested_cascade_count;
}

/// @brief Clamp a shadow-count value to the fixed HLSL shadow texture bindings.
/// @param[in] advertised_shadow_count Caller-provided completed-slot count.
/// @return Count clamped to `[0, VGFX3D_MAX_SHADOW_LIGHTS]`.
int32_t vgfx3d_d3d11_clamp_shadow_count(int32_t advertised_shadow_count) {
    if (advertised_shadow_count <= 0)
        return 0;
    return advertised_shadow_count > VGFX3D_MAX_SHADOW_LIGHTS ? VGFX3D_MAX_SHADOW_LIGHTS
                                                              : advertised_shadow_count;
}

/// @brief Select scene depth according to the actual render route for this frame.
/// @param[in] rtt_active Nonzero when explicit RTT rendering is active.
/// @param[in] gpu_postfx_enabled Nonzero when window rendering uses scene targets.
/// @param[in] has_rtt_depth Nonzero when RTT depth is complete.
/// @param[in] has_scene_depth Nonzero when offscreen scene depth is complete.
/// @param[in] has_swapchain_depth Nonzero when swapchain depth is complete.
/// @return Target class containing readable frame depth, or `VGFX3D_D3D11_TARGET_NONE`.
vgfx3d_d3d11_target_kind_t vgfx3d_d3d11_choose_depth_probe_target(int8_t rtt_active,
                                                                  int8_t gpu_postfx_enabled,
                                                                  int has_rtt_depth,
                                                                  int has_scene_depth,
                                                                  int has_swapchain_depth) {
    if (rtt_active)
        return has_rtt_depth ? VGFX3D_D3D11_TARGET_RTT : VGFX3D_D3D11_TARGET_NONE;
    if (gpu_postfx_enabled && has_scene_depth)
        return VGFX3D_D3D11_TARGET_SCENE;
    if (has_swapchain_depth)
        return VGFX3D_D3D11_TARGET_SWAPCHAIN;
    return VGFX3D_D3D11_TARGET_NONE;
}

/// @brief Project a world-space point through a shadow VP matrix using HLSL sampling rules.
/// @param[in] shadow_vp Row-major light view-projection matrix.
/// @param[in] projection_type Orthographic, perspective, or cube projection type.
/// @param[in] world_pos Three-component world-space position.
/// @param[out] out_uv_depth Receives texture U, V, and canonical depth.
/// @return One when projection produces finite coordinates; otherwise zero.
int vgfx3d_d3d11_project_shadow_coord(const float *shadow_vp,
                                      int32_t projection_type,
                                      const float world_pos[3],
                                      float out_uv_depth[3]) {
    float lx;
    float ly;
    float lz;
    float lw;
    float ndc_x;
    float ndc_y;
    float ndc_z;

    if (out_uv_depth) {
        out_uv_depth[0] = 0.0f;
        out_uv_depth[1] = 0.0f;
        out_uv_depth[2] = 0.0f;
    }
    if (!vgfx3d_d3d11_shadow_matrix_is_usable(shadow_vp) || !world_pos || !out_uv_depth ||
        !vgfx3d_d3d11_float_array_is_bounded(world_pos, 3u, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX))
        return 0;
    if (projection_type != VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC &&
        projection_type != VGFX3D_SHADOW_PROJECTION_PERSPECTIVE &&
        projection_type != VGFX3D_SHADOW_PROJECTION_CUBE)
        return 0;
    lx = world_pos[0] * shadow_vp[0] + world_pos[1] * shadow_vp[1] + world_pos[2] * shadow_vp[2] +
         shadow_vp[3];
    ly = world_pos[0] * shadow_vp[4] + world_pos[1] * shadow_vp[5] + world_pos[2] * shadow_vp[6] +
         shadow_vp[7];
    lz = world_pos[0] * shadow_vp[8] + world_pos[1] * shadow_vp[9] + world_pos[2] * shadow_vp[10] +
         shadow_vp[11];
    lw = world_pos[0] * shadow_vp[12] + world_pos[1] * shadow_vp[13] +
         world_pos[2] * shadow_vp[14] + shadow_vp[15];
    if (projection_type != VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC) {
        if (!isfinite(lw) || lw <= 0.0001f || lw >= 1.0e20f)
            return 0;
        ndc_x = lx / lw;
        ndc_y = ly / lw;
        ndc_z = lz / lw;
    } else {
        ndc_x = lx;
        ndc_y = ly;
        ndc_z = lz;
    }
    if (!isfinite(ndc_x) || !isfinite(ndc_y) || !isfinite(ndc_z))
        return 0;
    out_uv_depth[0] = ndc_x * 0.5f + 0.5f;
    out_uv_depth[1] = 0.5f - ndc_y * 0.5f;
    out_uv_depth[2] = ndc_z * 0.5f + 0.5f;
    return 1;
}

/// @brief Decide whether an RTT can safely mark its CPU-side mirror dirty.
/// @details End-of-frame dirtying is meaningful only when the target handle and
///   every GPU resource needed for a later staging copy are present. This helper
///   keeps partial allocation failures from installing stale sync hooks.
/// @param[in] rtt_active Nonzero when RTT rendering is active.
/// @param[in] has_target Nonzero when the host target descriptor exists.
/// @param[in] has_color_tex Nonzero when the color texture exists.
/// @param[in] has_color_rtv Nonzero when its render-target view exists.
/// @param[in] has_depth_tex Nonzero when the depth texture exists.
/// @param[in] has_depth_dsv Nonzero when its depth-stencil view exists.
/// @param[in] has_staging Nonzero when the readback staging texture exists.
/// @return One only when dirtying can later be serviced safely.
int vgfx3d_d3d11_should_mark_rtt_dirty(int8_t rtt_active,
                                       int has_target,
                                       int has_color_tex,
                                       int has_color_rtv,
                                       int has_depth_tex,
                                       int has_depth_dsv,
                                       int has_staging) {
    return rtt_active && has_target && has_color_tex && has_color_rtv && has_depth_tex &&
           has_depth_dsv && has_staging;
}

/// @brief Require the host target metadata to match the resources retained by the backend.
/// @param[in] target_width Host target width.
/// @param[in] target_height Host target height.
/// @param[in] target_format Host target color format.
/// @param[in] resource_width Native resource width.
/// @param[in] resource_height Native resource height.
/// @param[in] resource_format Native resource color format.
/// @return One when dimensions and supported formats match exactly.
int vgfx3d_d3d11_rtt_readback_state_matches(int32_t target_width,
                                            int32_t target_height,
                                            int32_t target_format,
                                            int32_t resource_width,
                                            int32_t resource_height,
                                            int32_t resource_format) {
    return vgfx3d_d3d11_is_valid_texture2d_extent(target_width, target_height) &&
           vgfx3d_d3d11_is_valid_rtt_color_format(target_format) &&
           vgfx3d_d3d11_is_valid_rtt_color_format(resource_format) &&
           target_width == resource_width && target_height == resource_height &&
           target_format == resource_format;
}

/// @brief Validate the two color formats supported by RenderTarget3D.
/// @param[in] color_format RenderTarget3D color-format value.
/// @return One for UNORM8 or HDR16F; otherwise zero.
int vgfx3d_d3d11_is_valid_rtt_color_format(int32_t color_format) {
    return color_format == (int32_t)VGFX3D_RENDERTARGET_COLOR_FORMAT_UNORM8 ||
           color_format == (int32_t)VGFX3D_RENDERTARGET_COLOR_FORMAT_HDR16F;
}

/// @brief Validate the two internal color classes accepted by D3D11 target creation.
/// @param[in] color_format Internal D3D11 color-format value.
/// @return One for UNORM8 or HDR16F; otherwise zero.
int vgfx3d_d3d11_is_valid_color_format(int32_t color_format) {
    return color_format == (int32_t)VGFX3D_D3D11_COLOR_FORMAT_UNORM8 ||
           color_format == (int32_t)VGFX3D_D3D11_COLOR_FORMAT_HDR16F;
}

/// @brief Keep cached bloom counts within the fixed context arrays.
/// @param[in] mip_count Cached bloom mip count.
/// @return One when the count is positive and within fixed array capacity.
int vgfx3d_d3d11_is_valid_bloom_mip_count(int32_t mip_count) {
    return mip_count > 0 && mip_count <= VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX;
}

/// @brief Map a draw command to its required blend state (alpha vs opaque).
/// @param[in] cmd Draw command whose additive and alpha settings are inspected.
/// @return Additive, alpha, or opaque backend blend mode.
vgfx3d_d3d11_blend_mode_t vgfx3d_d3d11_choose_blend_mode(const vgfx3d_draw_cmd_t *cmd) {
    if (cmd && cmd->additive_blend)
        return VGFX3D_D3D11_BLEND_ADDITIVE;
    return vgfx3d_draw_cmd_uses_alpha_blend(cmd) ? VGFX3D_D3D11_BLEND_ALPHA
                                                 : VGFX3D_D3D11_BLEND_OPAQUE;
}

/// @brief Pick the color format for a render target — HDR16F for the scene pass
/// (so post-FX tonemapping has headroom), UNORM8 for everything else.
/// @param[in] target_kind Target class being allocated.
/// @return HDR16F for scene intermediates; otherwise UNORM8.
vgfx3d_d3d11_color_format_t vgfx3d_d3d11_choose_color_format(
    vgfx3d_d3d11_target_kind_t target_kind) {
    return target_kind == VGFX3D_D3D11_TARGET_SCENE ? VGFX3D_D3D11_COLOR_FORMAT_HDR16F
                                                    : VGFX3D_D3D11_COLOR_FORMAT_UNORM8;
}

/// @brief Decide whether the current pass should bind the motion-vector target.
/// @details Motion vectors are only meaningful for opaque scene draws. Alpha
///   and additive passes blend multiple histories into one pixel, so they draw
///   color only and leave motion at the clear "no object history" sentinel.
/// @param[in] target_kind Target class receiving the draw.
/// @param[in] cmd Draw command used to evaluate depth and blend behavior.
/// @return Color-and-motion mode for eligible opaque scene draws; otherwise color-only.
vgfx3d_d3d11_motion_attachment_mode_t vgfx3d_d3d11_choose_motion_attachment_mode(
    vgfx3d_d3d11_target_kind_t target_kind, const vgfx3d_draw_cmd_t *cmd) {
    if (target_kind != VGFX3D_D3D11_TARGET_SCENE)
        return VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_ONLY;
    if (!cmd || cmd->disable_depth_test)
        return VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_ONLY;
    return vgfx3d_d3d11_choose_blend_mode(cmd) == VGFX3D_D3D11_BLEND_OPAQUE
               ? VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_AND_MOTION
               : VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_ONLY;
}

/// @brief Decide whether terrain splatting has every required texture bound.
/// @details D3D11's shader samples the control map plus four layers as a unit;
///   partial binds are treated as no splat so missing layers do not sample NULL
///   resources or produce backend-specific black terrain.
/// @param[in] cmd_has_splat Nonzero when the draw requests terrain splatting.
/// @param[in] has_splat_map Nonzero when the control map is bound.
/// @param[in] has_layer0 Nonzero when layer zero is bound.
/// @param[in] has_layer1 Nonzero when layer one is bound.
/// @param[in] has_layer2 Nonzero when layer two is bound.
/// @param[in] has_layer3 Nonzero when layer three is bound.
/// @return One only when splatting is requested and every required texture exists.
int vgfx3d_d3d11_has_complete_splat(int8_t cmd_has_splat,
                                    int has_splat_map,
                                    int has_layer0,
                                    int has_layer1,
                                    int has_layer2,
                                    int has_layer3) {
    return cmd_has_splat && has_splat_map && has_layer0 && has_layer1 && has_layer2 && has_layer3;
}

/// @brief A streaming fallback is bindable but must not advertise the authored map as resident.
/// @param[in] has_srv Nonzero when some shader resource view is bindable.
/// @param[in] is_fallback_srv Nonzero when that view is a streaming placeholder.
/// @return One only for a non-fallback shader resource view.
int vgfx3d_d3d11_srv_is_ready(int has_srv, int is_fallback_srv) {
    return has_srv && !is_fallback_srv;
}

/// @brief Decide whether the offscreen scene still needs a swapchain composite.
/// @param[in] rtt_active Nonzero when rendering to an explicit target.
/// @param[in] gpu_postfx_enabled Nonzero when the offscreen scene route is enabled.
/// @param[in] has_scene_targets Nonzero when scene targets are complete.
/// @param[in] scene_composited_to_swapchain Nonzero after this frame was already resolved.
/// @return One when an uncomposited window scene still needs resolution.
int vgfx3d_d3d11_should_composite_to_swapchain(int8_t rtt_active,
                                               int8_t gpu_postfx_enabled,
                                               int has_scene_targets,
                                               int8_t scene_composited_to_swapchain) {
    return !rtt_active && gpu_postfx_enabled && has_scene_targets && !scene_composited_to_swapchain;
}

/// @brief Decide whether a new begin-frame invalidates a prior swapchain composite.
/// @param[in] rtt_active Nonzero when the new frame targets an explicit RTT.
/// @param[in] load_existing_color Nonzero when the new pass preserves prior color.
/// @return One when the recorded composite must be reset.
int vgfx3d_d3d11_should_reset_composited_swapchain_for_frame(int8_t rtt_active,
                                                             int8_t load_existing_color) {
    return rtt_active || !load_existing_color;
}

/// @brief Decide whether a post-FX enable update invalidates a prior swapchain composite.
/// @param[in] current_enabled Current normalized enable state.
/// @param[in] requested_enabled Requested enable state.
/// @return One when the normalized enable state changes.
int vgfx3d_d3d11_should_reset_composited_swapchain_for_postfx_update(int8_t current_enabled,
                                                                     int8_t requested_enabled) {
    return (current_enabled ? 1 : 0) != (requested_enabled ? 1 : 0);
}

/// @brief Decide whether a begin-frame should preserve scene temporal history.
/// @param[in] resolved_target_kind Available target selected for the new pass.
/// @param[in] requested_load_existing_color Caller request to preserve existing color.
/// @return One when the pass is logically an overlay for temporal-history purposes.
int vgfx3d_d3d11_should_treat_begin_frame_as_overlay(
    vgfx3d_d3d11_target_kind_t resolved_target_kind, int8_t requested_load_existing_color) {
    if (resolved_target_kind == VGFX3D_D3D11_TARGET_OVERLAY)
        return 1;
    if (!requested_load_existing_color)
        return 0;
    return resolved_target_kind == VGFX3D_D3D11_TARGET_SWAPCHAIN ||
           resolved_target_kind == VGFX3D_D3D11_TARGET_SCENE;
}

/// @brief Decide whether overlay contents are in the separate overlay target.
/// @param[in] resolved_target_kind Available target selected for the pass.
/// @param[in] has_overlay_target Nonzero when separate overlay resources exist.
/// @return One when the resolved route uses the separate overlay target.
int vgfx3d_d3d11_uses_separate_overlay_target(vgfx3d_d3d11_target_kind_t resolved_target_kind,
                                              int has_overlay_target) {
    return resolved_target_kind == VGFX3D_D3D11_TARGET_OVERLAY && has_overlay_target;
}

/// @brief Choose the readback source class without touching D3D11 resources.
/// @param[in] presented_snapshot_valid Nonzero when a successful snapshot is recorded.
/// @param[in] presented_snapshot_has_texture Nonzero when its texture still exists.
/// @param[in] scene_composited_to_swapchain Nonzero when the current scene is on the backbuffer.
/// @param[in] gpu_postfx_enabled Nonzero when the offscreen post-FX route is enabled.
/// @param[in] postfx_chain_valid Nonzero when a stable chain snapshot exists.
/// @param[in] postfx_chain_enabled Nonzero when that chain is enabled.
/// @param[in] postfx_effect_count Number of effects in the chain.
/// @param[in] postfx_has_effects Nonzero when the chain contains an active effect.
/// @param[in] has_scene_targets Nonzero when offscreen scene resources are complete.
/// @param[in] current_target_kind Target class most recently selected.
/// @return Presented snapshot, backbuffer, post-FX composite, or scene-color source class.
vgfx3d_d3d11_readback_kind_t vgfx3d_d3d11_choose_readback_kind(
    int8_t presented_snapshot_valid,
    int presented_snapshot_has_texture,
    int8_t scene_composited_to_swapchain,
    int8_t gpu_postfx_enabled,
    int8_t postfx_chain_valid,
    int8_t postfx_chain_enabled,
    int32_t postfx_effect_count,
    int postfx_has_effects,
    int has_scene_targets,
    vgfx3d_d3d11_target_kind_t current_target_kind) {
    if (presented_snapshot_valid && presented_snapshot_has_texture)
        return VGFX3D_D3D11_READBACK_PRESENTED_SNAPSHOT;
    if (scene_composited_to_swapchain)
        return VGFX3D_D3D11_READBACK_BACKBUFFER;
    if (gpu_postfx_enabled && postfx_chain_valid && postfx_chain_enabled && has_scene_targets &&
        postfx_effect_count > 0 && postfx_has_effects)
        return VGFX3D_D3D11_READBACK_POSTFX_COMPOSITE;
    if (has_scene_targets && (current_target_kind == VGFX3D_D3D11_TARGET_SCENE ||
                              current_target_kind == VGFX3D_D3D11_TARGET_OVERLAY ||
                              current_target_kind == VGFX3D_D3D11_TARGET_RTT))
        return VGFX3D_D3D11_READBACK_SCENE_COLOR;
    return VGFX3D_D3D11_READBACK_BACKBUFFER;
}

/// @brief Accept only an exact `S_OK` as proof that DXGI completed the display path.
/// @details DXGI informational success values such as `DXGI_STATUS_OCCLUDED` do not prove that the
///   captured backbuffer reached the display and therefore must not advance presentation telemetry.
/// @param[in] present_status Signed 32-bit HRESULT/status returned by `IDXGISwapChain::Present`.
/// @return One for `S_OK`; otherwise zero.
int vgfx3d_d3d11_present_status_confirms_display(int32_t present_status) {
    return present_status == 0 ? 1 : 0;
}

/// @brief Keep a pre-present snapshot only when both snapshot and Present succeeded.
/// @param[in] snapshot_ok Nonzero when snapshot capture succeeded.
/// @param[in] present_ok Nonzero when swapchain presentation succeeded.
/// @return One only when both operations succeeded.
int vgfx3d_d3d11_should_keep_presented_snapshot(int snapshot_ok, int present_ok) {
    return snapshot_ok && present_ok ? 1 : 0;
}
