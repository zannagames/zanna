//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_shared.c
// Purpose: OpenGL backend helpers shared with sister backends — frame history,
//   mip math, capacity growth, and policy choices for render-target,
//   readback, and morph-cache reuse.
//
// Key invariants:
//   - Helper outputs are deterministic and bounded independently of an active GL context.
//   - Frame-history matrices and camera values are copied through finite-value guards.
//
// Ownership/Lifetime:
//   - Helpers write caller-owned POD buffers and retain no GL names or source pointers.
//
// Links: vgfx3d_backend_opengl_shared.h, vgfx3d_backend_opengl.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic validation, sizing, history, and policy helpers for OpenGL.
/// @details These routines are independent of a live GL context and centralize overflow-safe
///          buffer math, render-target and blend choices, shadow projection, and cache identity
///          rules used by the Linux OpenGL backend.

#include "vgfx3d_backend_opengl_shared.h"

#include "vgfx3d_backend_utils.h"

#include <limits.h>
#include <math.h>
#include <string.h>

/// @brief Roll the OpenGL backend's per-frame VP/inv-VP/cam-pos history forward by one frame.
/// Mirrors the D3D11 helper; see vgfx3d_d3d11_update_frame_history for semantics.
/// @param[in,out] history Caller-owned temporal history to advance.
/// @param vp Borrowed current row-major view-projection matrix.
/// @param inv_vp Borrowed current row-major inverse view-projection matrix.
/// @param cam_pos Borrowed camera position, or NULL to retain the prior scene value.
/// @param is_overlay_pass Nonzero when this update describes an overlay rather than a scene pass.
void vgfx3d_opengl_update_frame_history(vgfx3d_opengl_frame_history_t *history,
                                        const float *vp,
                                        const float *inv_vp,
                                        const float *cam_pos,
                                        int8_t is_overlay_pass) {
    float safe_vp[16];
    float safe_inv_vp[16];

    if (!history || !vp || !inv_vp)
        return;
    vgfx3d_copy_mat4_finite_or_identity(safe_vp, vp);
    vgfx3d_copy_mat4_finite_or_identity(safe_inv_vp, inv_vp);

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
            vgfx3d_copy_float_array_finite_or(history->scene_cam_pos, cam_pos, 3u, 0.0f);
        return;
    }

    memcpy(history->draw_prev_vp, safe_vp, sizeof(history->draw_prev_vp));
}

/// @brief Number of mipmap levels needed to reach 1×1 from (width × height).
/// @param width Base-level width in pixels.
/// @param height Base-level height in pixels.
/// @return Complete mip-chain length; invalid dimensions produce one level.
int32_t vgfx3d_opengl_compute_mip_count(int32_t width, int32_t height) {
    int32_t mip_count = 1;

    if (width <= 0 || height <= 0)
        return 1;
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
/// @param requested Requested anisotropy value.
/// @return @p requested clamped to the OpenGL sampler-cache range.
int32_t vgfx3d_opengl_sanitize_anisotropy(int32_t requested) {
    if (requested < 1)
        return 1;
    if (requested > VGFX3D_OPENGL_MAX_TEXTURE_ANISOTROPY)
        return VGFX3D_OPENGL_MAX_TEXTURE_ANISOTROPY;
    return requested;
}

/// @brief Convert sanitized anisotropy to a compact cache index [0,15].
/// @param requested Requested anisotropy before sanitization.
/// @return Zero-based cache index corresponding to the clamped value.
int32_t vgfx3d_opengl_sampler_anisotropy_index(int32_t requested) {
    return vgfx3d_opengl_sanitize_anisotropy(requested) - 1;
}

/// @brief Capacity-doubling growth helper (saturates at INT_MAX).
/// @param current_capacity Existing element capacity.
/// @param needed Minimum required capacity.
/// @param minimum_capacity Initial floor used when no positive capacity exists.
/// @return Positive doubled capacity covering @p needed without signed overflow.
int32_t vgfx3d_opengl_next_capacity(int32_t current_capacity,
                                    int32_t needed,
                                    int32_t minimum_capacity) {
    int32_t next_capacity;

    if (needed <= 0) {
        next_capacity = current_capacity > 0 ? current_capacity : minimum_capacity;
        return next_capacity > 0 ? next_capacity : 1;
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

/// @brief Multiply two size_t values with overflow checking; writes the product to @p out
///   and returns 1, or zeroes @p out and returns 0 on overflow or a NULL out pointer.
/// @param a Left multiplicand.
/// @param b Right multiplicand.
/// @param[out] out Required destination for the product.
/// @return 1 when multiplication is representable, otherwise 0.
static int vgfx3d_opengl_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (out)
        *out = 0;
    if (!out)
        return 0;
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

/// @brief Overflow-safe size_t capacity growth helper for GL buffer uploads.
/// @param current_capacity Existing byte capacity.
/// @param needed Minimum required byte capacity.
/// @param minimum_capacity Initial floor used when no capacity exists.
/// @param[out] out_capacity Required destination for the selected capacity.
/// @return 1 when a capacity is produced, or 0 for a NULL output.
int vgfx3d_opengl_compute_buffer_capacity(size_t current_capacity,
                                          size_t needed,
                                          size_t minimum_capacity,
                                          size_t *out_capacity) {
    size_t next_capacity;

    if (out_capacity)
        *out_capacity = 0;
    if (!out_capacity)
        return 0;
    if (needed == 0) {
        *out_capacity = current_capacity > 0 ? current_capacity : minimum_capacity;
        return 1;
    }
    next_capacity = current_capacity > 0 ? current_capacity : minimum_capacity;
    if (next_capacity < 1)
        next_capacity = 1;
    while (next_capacity < needed) {
        if (next_capacity > SIZE_MAX / 2) {
            next_capacity = needed;
            break;
        }
        next_capacity *= 2;
    }
    *out_capacity = next_capacity;
    return 1;
}

/// @brief Validate an RGBA8 destination rectangle and optionally return its byte span.
/// @param width Destination width in pixels.
/// @param height Destination height in rows.
/// @param stride Destination row pitch in bytes.
/// @param[out] out_bytes Optional destination for the total buffer span.
/// @return 1 when dimensions and multiplication are valid, otherwise 0.
int vgfx3d_opengl_validate_rgba8_destination(int32_t width,
                                             int32_t height,
                                             int32_t stride,
                                             size_t *out_bytes) {
    size_t min_stride;
    size_t total_bytes;

    if (out_bytes)
        *out_bytes = 0;
    if (width <= 0 || height <= 0 || stride <= 0)
        return 0;
    if (!vgfx3d_opengl_checked_mul_size((size_t)width, 4u, &min_stride))
        return 0;
    if ((size_t)stride < min_stride)
        return 0;
    if (!vgfx3d_opengl_checked_mul_size((size_t)stride, (size_t)height, &total_bytes))
        return 0;
    if (out_bytes)
        *out_bytes = total_bytes;
    return 1;
}

/// @brief Clamp morph shape count to shader and index-range limits.
/// @param vertex_count Number of vertices addressed by every morph shape.
/// @param requested_shape_count Requested number of shapes.
/// @return Usable shape count, or zero when indexing cannot be represented safely.
int32_t vgfx3d_opengl_clamp_morph_shape_count(uint32_t vertex_count,
                                              int32_t requested_shape_count) {
    int32_t shape_count;
    uint32_t max_indexed_vertices;
    uint32_t max_shapes_by_index;

    if (vertex_count == 0 || requested_shape_count <= 0)
        return 0;
    shape_count = requested_shape_count;
    if (shape_count > VGFX3D_OPENGL_MAX_MORPH_SHAPES)
        shape_count = VGFX3D_OPENGL_MAX_MORPH_SHAPES;
    max_indexed_vertices = (uint32_t)((INT_MAX - 2) / 3);
    max_shapes_by_index = max_indexed_vertices / vertex_count;
    if (max_shapes_by_index == 0)
        return 0;
    if ((uint32_t)shape_count > max_shapes_by_index)
        shape_count = (int32_t)max_shapes_by_index;
    return shape_count;
}

/// @brief Validate an R32F texture-buffer payload against a GL texel limit.
/// @details Morph-target buffers are bound as `samplerBuffer` with `R32F`, so
///   one texture-buffer texel stores exactly one float. A non-positive limit
///   means the backend could not query a useful `GL_MAX_TEXTURE_BUFFER_SIZE`,
///   in which case the caller should fall back to overflow-only validation.
/// @param payload_bytes Proposed `R32F` payload size in bytes.
/// @param max_texture_buffer_texels Device texture-buffer limit in float texels.
/// @return Nonzero when the payload is float-aligned and fits the known limit.
int vgfx3d_opengl_texture_buffer_accepts_r32f_payload(size_t payload_bytes,
                                                      int32_t max_texture_buffer_texels) {
    size_t texels;

    if (payload_bytes == 0 || (payload_bytes % sizeof(float)) != 0)
        return 0;
    if (max_texture_buffer_texels <= 0)
        return 1;
    texels = payload_bytes / sizeof(float);
    return texels <= (size_t)max_texture_buffer_texels;
}

/// @brief Pick the right render-target classification for the OpenGL backend.
/// See vgfx3d_d3d11_choose_target_kind for the policy semantics.
/// @param rtt_active Nonzero when an explicit runtime render target is bound.
/// @param gpu_postfx_enabled Nonzero when the window scene uses the offscreen post-FX route.
/// @return RTT, offscreen-scene, or swapchain target classification.
vgfx3d_opengl_target_kind_t vgfx3d_opengl_choose_target_kind(int8_t rtt_active,
                                                             int8_t gpu_postfx_enabled) {
    if (rtt_active)
        return VGFX3D_OPENGL_TARGET_RTT;
    return gpu_postfx_enabled ? VGFX3D_OPENGL_TARGET_SCENE : VGFX3D_OPENGL_TARGET_SWAPCHAIN;
}

/// @brief Pick the GL internal color format for a given render-target classification.
/// @details Scene targets go through tonemap/postfx, so they allocate HDR (half-float)
///   to preserve intensity beyond [0, 1] prior to the final exposure curve. Swapchain
///   and RTT targets stay in UNORM8 since those are the final consumer surfaces and
///   extra precision is wasted on them.
/// @param target_kind Render-target classification being allocated.
/// @return HDR16F for scene targets, otherwise UNORM8.
vgfx3d_opengl_color_format_t vgfx3d_opengl_choose_color_format(
    vgfx3d_opengl_target_kind_t target_kind) {
    return target_kind == VGFX3D_OPENGL_TARGET_SCENE ? VGFX3D_OPENGL_COLOR_FORMAT_HDR16F
                                                     : VGFX3D_OPENGL_COLOR_FORMAT_UNORM8;
}

/// @brief Select the GL blend mode for a draw command.
/// @details Explicit `additive_blend` wins outright (used by particles, decals of fire,
///   etc.). Otherwise the command's material alpha and vertex-color-alpha determine
///   whether alpha blending is needed, via `vgfx3d_draw_cmd_uses_alpha_blend`. Opaque
///   is the default — it skips the blend unit entirely and enables early-Z in the GPU.
/// @param cmd Borrowed draw command whose material blend flags are inspected; may be NULL.
/// @return Additive, alpha, or opaque OpenGL blend classification.
vgfx3d_opengl_blend_mode_t vgfx3d_opengl_choose_blend_mode(const vgfx3d_draw_cmd_t *cmd) {
    if (cmd && cmd->additive_blend)
        return VGFX3D_OPENGL_BLEND_ADDITIVE;
    return vgfx3d_draw_cmd_uses_alpha_blend(cmd) ? VGFX3D_OPENGL_BLEND_ALPHA
                                                 : VGFX3D_OPENGL_BLEND_OPAQUE;
}

/// @brief Decide whether terrain splatting has every required texture bound.
/// @param cmd_has_splat Nonzero when the command requests splatting.
/// @param has_splat_map Nonzero when the control texture is bound.
/// @param has_layer0 Nonzero when layer zero is bound.
/// @param has_layer1 Nonzero when layer one is bound.
/// @param has_layer2 Nonzero when layer two is bound.
/// @param has_layer3 Nonzero when layer three is bound.
/// @return Nonzero only when splatting and all five textures are available.
int vgfx3d_opengl_has_complete_splat(int8_t cmd_has_splat,
                                     int has_splat_map,
                                     int has_layer0,
                                     int has_layer1,
                                     int has_layer2,
                                     int has_layer3) {
    return cmd_has_splat && has_splat_map && has_layer0 && has_layer1 && has_layer2 && has_layer3;
}

/// @brief Decide whether a draw contributes to the motion-vector buffer.
/// @details Motion vectors drive TAA / motion-blur postfx, both of which only run when
///   the scene target is bound. Transparent draws are excluded because blended fragments
///   don't have a single authoritative "this pixel came from there" source — their
///   motion vectors would corrupt the reconstruction. Opaque scene draws write both
///   color and motion; every other combination writes color only.
/// @param target_kind Classification of the active draw target.
/// @param cmd Borrowed draw command whose blend and depth behavior are inspected.
/// @return Color-and-motion for eligible opaque scene draws, otherwise color-only.
vgfx3d_opengl_motion_attachment_mode_t vgfx3d_opengl_choose_motion_attachment_mode(
    vgfx3d_opengl_target_kind_t target_kind, const vgfx3d_draw_cmd_t *cmd) {
    if (target_kind != VGFX3D_OPENGL_TARGET_SCENE)
        return VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_ONLY;
    if (!cmd || cmd->disable_depth_test)
        return VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_ONLY;
    return vgfx3d_opengl_choose_blend_mode(cmd) == VGFX3D_OPENGL_BLEND_OPAQUE
               ? VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_AND_MOTION
               : VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_ONLY;
}

/// @brief Decide whether canvas readback should source the swapchain or a postfx target.
/// @param gpu_postfx_enabled Nonzero when GPU post-processing is active.
/// @return Post-FX composite source when enabled, otherwise the backbuffer.
vgfx3d_opengl_readback_kind_t vgfx3d_opengl_choose_readback_kind(int8_t gpu_postfx_enabled) {
    return gpu_postfx_enabled ? VGFX3D_OPENGL_READBACK_POSTFX_COMPOSITE
                              : VGFX3D_OPENGL_READBACK_BACKBUFFER;
}

/// @brief Clamp light shadow indices to completed contiguous shadow-map slots.
/// @param shadow_index Requested zero-based shadow slot.
/// @param shadow_count Number of complete contiguous slots.
/// @return @p shadow_index when valid, otherwise -1.
int32_t vgfx3d_opengl_sanitize_shadow_index(int32_t shadow_index, int32_t shadow_count) {
    if (shadow_count <= 0 || shadow_count > VGFX3D_MAX_SHADOW_LIGHTS)
        return -1;
    return (shadow_index >= 0 && shadow_index < shadow_count) ? shadow_index : -1;
}

/// @brief Project a world-space point through a shadow VP matrix using GLSL sampling rules.
/// @param shadow_vp Borrowed row-major shadow view-projection matrix.
/// @param projection_type Orthographic, perspective, or cube projection identifier.
/// @param world_pos Borrowed three-component world-space position.
/// @param[out] out_uv_depth Required UV-depth destination, zeroed before validation.
/// @return 1 when finite texture coordinates are produced, otherwise 0.
int vgfx3d_opengl_project_shadow_coord(const float *shadow_vp,
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
    if (!vgfx3d_shadow_matrix_is_usable(shadow_vp) || !world_pos || !out_uv_depth ||
        !vgfx3d_float_array_is_bounded(world_pos, 3u, VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX)) {
        return 0;
    }
    if (projection_type != VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC &&
        projection_type != VGFX3D_SHADOW_PROJECTION_PERSPECTIVE &&
        projection_type != VGFX3D_SHADOW_PROJECTION_CUBE) {
        return 0;
    }
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
    out_uv_depth[1] = ndc_y * 0.5f + 0.5f;
    out_uv_depth[2] = ndc_z * 0.5f + 0.5f;
    return 1;
}

/// @brief Decide whether to reuse a cached morph-target GPU buffer.
/// Returns 1 if the cached payload is still valid (same key + matching shape/vertex count),
/// 0 if the buffer must be re-uploaded.
/// @param cached_key Identity key recorded by the cached payload.
/// @param cached_identity Allocation generation recorded by the cached payload.
/// @param cached_revision Revision recorded by the cached payload.
/// @param cached_shape_count Sanitized shape count recorded by the cache.
/// @param cached_vertex_count Vertex count recorded by the cache.
/// @param cached_has_normal_deltas Nonzero when cached normal deltas exist.
/// @param cmd Borrowed draw command requesting morph data.
/// @return 1 when every identity and layout field matches, otherwise 0.
int vgfx3d_opengl_should_reuse_morph_cache(const void *cached_key,
                                           uint64_t cached_identity,
                                           uint64_t cached_revision,
                                           int32_t cached_shape_count,
                                           uint32_t cached_vertex_count,
                                           int8_t cached_has_normal_deltas,
                                           const vgfx3d_draw_cmd_t *cmd) {
    int32_t shape_count;
    int8_t has_normal_deltas;

    if (!cmd || !cmd->morph_key || cmd->morph_identity == 0 || cmd->morph_revision == 0 ||
        !cmd->morph_deltas || !cmd->morph_weights || cmd->morph_shape_count <= 0 ||
        cmd->vertex_count == 0) {
        return 0;
    }

    shape_count = vgfx3d_opengl_clamp_morph_shape_count(cmd->vertex_count, cmd->morph_shape_count);
    if (shape_count <= 0)
        return 0;
    has_normal_deltas = cmd->morph_normal_deltas ? 1 : 0;
    return cached_key == cmd->morph_key && cached_identity == cmd->morph_identity &&
           cached_revision == cmd->morph_revision && cached_shape_count == shape_count &&
           cached_vertex_count == cmd->vertex_count &&
           cached_has_normal_deltas == has_normal_deltas;
}

/// @brief Decide whether a per-mesh GPU cache entry should be evicted this frame.
/// Returns 1 when the entry has been unused for more than @p ttl_frames frames.
/// @param current_frame Current monotonically increasing frame number.
/// @param last_used_frame Frame number of the entry's most recent use.
/// @param max_age Maximum permitted unused-frame age; zero disables pruning.
/// @return Nonzero when the entry is older than @p max_age.
int vgfx3d_opengl_should_prune_cache_entry(uint64_t current_frame,
                                           uint64_t last_used_frame,
                                           uint64_t max_age) {
    if (max_age == 0 || current_frame <= last_used_frame)
        return 0;
    return (current_frame - last_used_frame) > max_age;
}

/// @brief Invert a finite row-major 4x4 matrix with scaled partial pivoting.
/// @details The former cofactor implementation accepted a NaN determinant because every ordered
/// comparison with NaN is false, then published an all-NaN inverse. Gauss-Jordan elimination lets
/// this helper reject every non-finite intermediate and choose a scale-aware pivot. Failure writes
/// identity so callers cannot accidentally consume partially initialized output.
int vgfx3d_opengl_inverse_matrix4(const float matrix[16], float inverse[16]) {
    double augmented[4][8];
    double row_scales[4];
    float result[16];

    if (!inverse)
        return 0;
    memset(inverse, 0, sizeof(float) * 16u);
    inverse[0] = inverse[5] = inverse[10] = inverse[15] = 1.0f;
    if (!matrix)
        return 0;
    for (int row = 0; row < 4; ++row) {
        double scale = 0.0;
        for (int column = 0; column < 4; ++column) {
            double value = matrix[row * 4 + column];
            if (!isfinite(value))
                return 0;
            augmented[row][column] = value;
            if (fabs(value) > scale)
                scale = fabs(value);
            augmented[row][column + 4] = row == column ? 1.0 : 0.0;
        }
        if (!(scale > 0.0) || !isfinite(scale))
            return 0;
        row_scales[row] = scale;
    }
    for (int column = 0; column < 4; ++column) {
        int pivot_row = column;
        double pivot_magnitude = fabs(augmented[column][column]);
        double pivot_score = pivot_magnitude / row_scales[column];
        for (int row = column + 1; row < 4; ++row) {
            double candidate = fabs(augmented[row][column]);
            double candidate_score = candidate / row_scales[row];
            if (candidate_score > pivot_score) {
                pivot_magnitude = candidate;
                pivot_score = candidate_score;
                pivot_row = row;
            }
        }
        if (!isfinite(pivot_score) || pivot_score <= 1.0e-12)
            return 0;
        if (pivot_row != column) {
            for (int lane = 0; lane < 8; ++lane) {
                double temporary = augmented[column][lane];
                augmented[column][lane] = augmented[pivot_row][lane];
                augmented[pivot_row][lane] = temporary;
            }
            double temporary_scale = row_scales[column];
            row_scales[column] = row_scales[pivot_row];
            row_scales[pivot_row] = temporary_scale;
        }
        double pivot = augmented[column][column];
        if (!isfinite(pivot) || fabs(pivot) <= row_scales[column] * 1.0e-12)
            return 0;
        for (int lane = 0; lane < 8; ++lane) {
            augmented[column][lane] /= pivot;
            if (!isfinite(augmented[column][lane]))
                return 0;
        }
        for (int row = 0; row < 4; ++row) {
            if (row == column)
                continue;
            double factor = augmented[row][column];
            for (int lane = 0; lane < 8; ++lane) {
                augmented[row][lane] -= factor * augmented[column][lane];
                if (!isfinite(augmented[row][lane]))
                    return 0;
            }
        }
    }
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            result[row * 4 + column] = (float)augmented[row][column + 4];
            if (!isfinite(result[row * 4 + column]))
                return 0;
        }
    memcpy(inverse, result, sizeof(result));
    return 1;
}
