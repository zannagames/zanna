//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_shared.h
// Purpose: Shared declarations/constants for the Direct3D 11 vgfx3d backend —
//   bone-palette limits, shared constant-buffer layouts, and helper routines
//   used by both the main D3D11 backend and its shared support unit.
//
// Key invariants:
//   - Layouts here must stay in lock-step with the HLSL shaders and the
//     equivalent Metal/OpenGL shared layouts (same bone/light limits).
//   - Internal to the D3D11 backend; not part of the public Zanna API.
//
// Ownership/Lifetime:
//   - Declarations only; no allocation or ownership semantics here.
//
// Links: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11.c,
//        src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_shared.c
//
//===----------------------------------------------------------------------===//

/// @file vgfx3d_backend_d3d11_shared.h
/// @brief Declares D3D11 backend constants, shader-compatible data layouts, and
///   platform-neutral validation, sizing, and routing helpers.
/// @details These declarations are internal to the D3D11 backend. Struct layouts
///   mirror embedded HLSL constant buffers, while helper functions normalize
///   untrusted runtime inputs before native resource and pipeline operations.

#pragma once

#include "vgfx3d_backend.h"
#include "vgfx3d_backend_utils.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VGFX3D_D3D11_MAX_BONES 256
#define VGFX3D_D3D11_BONE_PALETTE_FLOATS (VGFX3D_D3D11_MAX_BONES * 16u)
#define VGFX3D_D3D11_BONE_PALETTE_BYTES (sizeof(float) * VGFX3D_D3D11_BONE_PALETTE_FLOATS)
#define VGFX3D_D3D11_MAX_MORPH_SHAPES 64
#define VGFX3D_D3D11_PACKED_MORPH_WEIGHT_VECS (VGFX3D_D3D11_MAX_MORPH_SHAPES / 4)
#define VGFX3D_D3D11_PACKED_CUSTOM_PARAM_VECS 3
#define VGFX3D_D3D11_TEXTURE_SLOT_COUNT RT_MATERIAL3D_TEXTURE_SLOT_COUNT
#define VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION 16384
#define VGFX3D_D3D11_MAX_CUBEMAP_DIMENSION 16384
#define VGFX3D_D3D11_MAX_TEXTURE_ANISOTROPY 16
#define VGFX3D_D3D11_ANISOTROPY_LEVEL_COUNT VGFX3D_D3D11_MAX_TEXTURE_ANISOTROPY
#define VGFX3D_D3D11_MAX_BUFFER_TEXELS (1u << 27)
#define VGFX3D_D3D11_MIN_FLOAT_SRV_CAPACITY 256u
#define VGFX3D_D3D11_MAX_CONSTANT_BUFFER_BYTES (64u * 1024u)
#define VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX 1000000000000.0f
#define VGFX3D_D3D11_MAX_SLOPE_SCALED_DEPTH_BIAS 16.0f
#define VGFX3D_D3D11_MATERIAL_SHININESS_MAX 1000000.0f
#define VGFX3D_D3D11_MATERIAL_SPLAT_SCALE_MAX 1000000.0f
#define VGFX3D_D3D11_LIGHT_INTENSITY_MAX 1000000.0f
#define VGFX3D_D3D11_LIGHT_ATTENUATION_MAX 1000000.0f
#define VGFX3D_D3D11_POSTFX_SCALAR_MAX 1000000.0f
#define VGFX3D_D3D11_FOG_DISTANCE_MAX 1000000000.0f
#define VGFX3D_D3D11_SHADOW_BIAS_MAX 1000000.0f
#define VGFX3D_D3D11_SHADING_MODEL_MAX 5
#define VGFX3D_D3D11_TONEMAP_MODE_MAX 2
#define VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX 6
#define VGFX3D_D3D11_BLOOM_MIN_DOWNSAMPLE_EXTENT 8
#define VGFX3D_D3D11_SHADOW_ATLAS_COLUMNS 4
#define VGFX3D_D3D11_SHADOW_ATLAS_ROWS 2
#define VGFX3D_D3D11_FRAME_TIMING_PENDING_POLL_LIMIT 120u
#define VGFX3D_D3D11_DEPTH_PROBE_PENDING_POLL_LIMIT 120u
#define VGFX3D_D3D11_SSR_STEPS_MIN 8
#define VGFX3D_D3D11_SSR_STEPS_MAX 48
#define VGFX3D_D3D11_SSR_STEPS_DEFAULT 24
#define VGFX3D_D3D11_CLUSTER_ZNEAR_MIN 0.0001f
#define VGFX3D_D3D11_CLUSTER_ZNEAR_FALLBACK 0.1f
#define VGFX3D_D3D11_CLUSTER_ZFAR_FALLBACK 1000.0f
#define VGFX3D_D3D11_CLUSTER_ZFAR_MAX 1000000000.0f

/// @brief Blend state required by a draw: opaque, standard alpha, or additive.
typedef enum {
    VGFX3D_D3D11_BLEND_OPAQUE = 0,
    VGFX3D_D3D11_BLEND_ALPHA = 1,
    VGFX3D_D3D11_BLEND_ADDITIVE = 2,
} vgfx3d_d3d11_blend_mode_t;

/// @brief Render-target classification: the swapchain back buffer, the offscreen HDR
///   scene target, a user render-to-texture target, or the overlay target.
typedef enum {
    VGFX3D_D3D11_TARGET_NONE = -1,
    VGFX3D_D3D11_TARGET_SWAPCHAIN = 0,
    VGFX3D_D3D11_TARGET_SCENE = 1,
    VGFX3D_D3D11_TARGET_RTT = 2,
    VGFX3D_D3D11_TARGET_OVERLAY = 3,
} vgfx3d_d3d11_target_kind_t;

/// @brief Color format of a target: 8-bit UNORM (display) or 16-bit float (HDR scene).
typedef enum {
    VGFX3D_D3D11_COLOR_FORMAT_UNORM8 = 0,
    VGFX3D_D3D11_COLOR_FORMAT_HDR16F = 1,
} vgfx3d_d3d11_color_format_t;

/// @brief Whether a pass attaches only color or also the motion-vector target.
typedef enum {
    VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_ONLY = 0,
    VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_AND_MOTION = 1,
} vgfx3d_d3d11_motion_attachment_mode_t;

/// @brief Source texture/surface class for a canvas readback request.
typedef enum {
    VGFX3D_D3D11_READBACK_BACKBUFFER = 0,
    VGFX3D_D3D11_READBACK_POSTFX_COMPOSITE = 1,
    VGFX3D_D3D11_READBACK_SCENE_COLOR = 2,
    VGFX3D_D3D11_READBACK_PRESENTED_SNAPSHOT = 3,
} vgfx3d_d3d11_readback_kind_t;

/// @brief Outcome of one budgeted texture-upload continuation step.
typedef enum {
    VGFX3D_D3D11_UPLOAD_FAILED = -1,
    VGFX3D_D3D11_UPLOAD_PENDING = 0,
    VGFX3D_D3D11_UPLOAD_COMPLETE = 1,
} vgfx3d_d3d11_upload_result_t;

/// @brief Per-object HLSL constant buffer: model/prev-model/normal matrices plus skinning,
///   morph, and instancing flags and packed morph weights.
typedef struct {
    float model[16];
    float prev_model[16];
    float normal[16];
    int32_t has_prev_model_matrix;
    int32_t has_skinning;
    int32_t has_prev_skinning;
    int32_t has_morph_normal_deltas;
    int32_t morph_shape_count;
    int32_t vertex_count;
    int32_t has_prev_morph_weights;
    int32_t has_prev_instance_matrices;
    /* R18 per-instance bone-palette stride (0 = shared palette); padded to a
     * full float4 row so the HLSL cbuffer layout stays 16-byte aligned. */
    int32_t instance_bone_stride;
    int32_t _pad_stride[3];
    float morph_weights_packed[VGFX3D_D3D11_PACKED_MORPH_WEIGHT_VECS][4];
    float prev_morph_weights_packed[VGFX3D_D3D11_PACKED_MORPH_WEIGHT_VECS][4];
} vgfx3d_d3d11_per_object_t;

/// @brief Per-material HLSL constant buffer: colors, scalar/PBR factor banks, flag banks,
///   terrain-splat scales, shading model, packed custom params, and per-slot UV set +
///   transform data.
typedef struct {
    float diffuse[4];
    float specular[4];
    float emissive[4];
    float scalars[4];
    float pbr_scalars0[4];
    float pbr_scalars1[4];
    int32_t flags0[4];
    int32_t flags1[4];
    int32_t pbr_flags[4];
    float splat_scales[4];
    int32_t shading_model;
    float _pad0[3];
    float custom_params_packed[VGFX3D_D3D11_PACKED_CUSTOM_PARAM_VECS][4];
    int32_t texture_uv_sets0[4];
    int32_t texture_uv_sets1[4];
    float texture_uv_transform0[VGFX3D_D3D11_TEXTURE_SLOT_COUNT][4];
    float texture_uv_transform1[VGFX3D_D3D11_TEXTURE_SLOT_COUNT][4];
} vgfx3d_d3d11_per_material_t;

/// @brief One per-instance vertex-buffer entry for instanced draws: model, normal, and
///   previous-frame model matrices.
typedef struct {
    float model[16];
    float normal[16];
    float prev_model[16];
} vgfx3d_d3d11_instance_data_t;

/// @brief Per-frame view/projection history for motion vectors: current/previous/inverse
///   scene VP, the draw's previous VP, camera position, and scene/overlay validity flags.
typedef struct {
    float scene_vp[16];
    float scene_prev_vp[16];
    float scene_inv_vp[16];
    float draw_prev_vp[16];
    float scene_cam_pos[3];
    int8_t scene_history_valid;
    int8_t overlay_used_this_frame;
} vgfx3d_d3d11_frame_history_t;

/// @brief Pack scalar array into HLSL float4-aligned slots (four scalars per float4).
/// @param[out] dst Destination vector array.
/// @param[in] dst_vec_count Number of vectors in @p dst.
/// @param[in] src Optional flat scalar source.
/// @param[in] src_scalar_count Number of source scalars.
void vgfx3d_d3d11_pack_scalar_array4(float (*dst)[4],
                                     int32_t dst_vec_count,
                                     const float *src,
                                     int32_t src_scalar_count);
/// @brief Copy bone palette into a fixed-size cbuffer slot (identity-pads unused bones).
/// @param[out] dst Fixed-capacity destination palette.
/// @param[in] src Optional contiguous source matrices.
/// @param[in] bone_count Number of matrices to copy.
void vgfx3d_d3d11_pack_bone_palette(float *dst, const float *src, int32_t bone_count);
/// @brief Build per-instance cbuffer entries (model + normal + prev_model) for instanced draws.
/// @param[out] dst Destination instance-data array.
/// @param[in] instance_count Number of entries to populate.
/// @param[in] instance_matrices Current model matrices.
/// @param[in] prev_instance_matrices Optional previous-frame model matrices.
/// @param[in] has_prev_instance_matrices Nonzero when previous matrices are available.
void vgfx3d_d3d11_fill_instance_data(vgfx3d_d3d11_instance_data_t *dst,
                                     int32_t instance_count,
                                     const float *instance_matrices,
                                     const float *prev_instance_matrices,
                                     int8_t has_prev_instance_matrices);
/// @brief Decide whether instanced motion-history attributes are actually available.
/// @param[in] prev_instance_matrices Optional previous-frame matrices.
/// @param[in] has_prev_instance_matrices Caller-provided availability flag.
/// @return One only when the flag is set and the matrix array exists.
int vgfx3d_d3d11_should_use_previous_instance_matrices(const float *prev_instance_matrices,
                                                       int8_t has_prev_instance_matrices);
/// @brief Roll per-frame VP/inv-VP/cam-pos history forward by one frame (scene + overlay tracked
/// separately).
/// @param[in,out] history History record to update.
/// @param[in] vp Current view-projection matrix.
/// @param[in] inv_vp Current inverse view-projection matrix.
/// @param[in] cam_pos Optional camera position.
/// @param[in] is_overlay_pass Nonzero for an overlay pass.
/// @param[in] uses_separate_overlay_target Nonzero when overlay storage is separate.
void vgfx3d_d3d11_update_frame_history(vgfx3d_d3d11_frame_history_t *history,
                                       const float *vp,
                                       const float *inv_vp,
                                       const float *cam_pos,
                                       int8_t is_overlay_pass,
                                       int8_t uses_separate_overlay_target);
/// @brief Reconcile bone-buffer upload outcome into per-object draw flags (clears skinning on
/// failure).
/// @param[in,out] object_data Per-object flags to reconcile.
/// @param[in] current_upload_ok Nonzero when current palette upload succeeded.
/// @param[in] prev_upload_ok Nonzero when history palette upload succeeded.
void vgfx3d_d3d11_resolve_bone_upload_status(vgfx3d_d3d11_per_object_t *object_data,
                                             int current_upload_ok,
                                             int prev_upload_ok);
/// @brief Decide if shader skinning can run; palette uploads clamp to the shader maximum.
/// @param[in] bone_palette Optional source palette.
/// @param[in] bone_count Advertised matrix count.
/// @return One for a nonempty palette; otherwise zero.
int vgfx3d_d3d11_should_enable_skinning(const float *bone_palette, int32_t bone_count);
/// @brief Reconcile morph-target upload outcome (positions + normals) into draw flags.
/// @param[in,out] object_data Per-object morph flags to reconcile.
/// @param[in] morph_upload_ok Nonzero when position upload succeeded.
/// @param[in] morph_normal_upload_ok Nonzero when normal upload succeeded.
void vgfx3d_d3d11_resolve_morph_upload_status(vgfx3d_d3d11_per_object_t *object_data,
                                              int morph_upload_ok,
                                              int morph_normal_upload_ok);
/// @brief Number of mipmap levels needed to reach 1×1 from (width × height).
/// @param[in] width Base width.
/// @param[in] height Base height.
/// @return Full-chain level count, or zero for invalid dimensions.
int32_t vgfx3d_d3d11_compute_mip_count(int32_t width, int32_t height);
/// @brief Clamp material sampler anisotropy into the backend cacheable [1,16] range.
/// @param[in] requested Requested anisotropy.
/// @return Sanitized supported anisotropy.
int32_t vgfx3d_d3d11_sanitize_anisotropy(int32_t requested);
/// @brief Convert sanitized anisotropy to a compact cache index [0,15].
/// @param[in] requested Requested anisotropy.
/// @return Zero-based sampler-cache index.
int32_t vgfx3d_d3d11_sampler_anisotropy_index(int32_t requested);
/// @brief Normalize texture UV-set selectors to the two shader-visible channels.
/// @param[in] requested Requested UV-set selector.
/// @return Zero for UV0 or one for a positive selector.
int32_t vgfx3d_d3d11_sanitize_texture_uv_set(int32_t requested);
/// @brief Clamp integer post-FX sample/pass counts before cbuffer upload.
/// @param[in] requested Value to clamp.
/// @param[in] min_value First inclusive bound.
/// @param[in] max_value Second inclusive bound.
/// @return Value clamped between ordered bounds.
int32_t vgfx3d_d3d11_clamp_int_param(int32_t requested, int32_t min_value, int32_t max_value);
/// @brief Replace non-finite float parameters before D3D11 cbuffer/state upload.
/// @param[in] requested Preferred value.
/// @param[in] fallback Replacement for non-finite input.
/// @return Finite requested value, fallback, or zero.
float vgfx3d_d3d11_finite_or(float requested, float fallback);
/// @brief Clamp finite float parameters, using @p fallback for NaN/Inf values.
/// @param[in] requested Preferred value.
/// @param[in] min_value First inclusive bound.
/// @param[in] max_value Second inclusive bound.
/// @param[in] fallback Replacement for non-finite input.
/// @return Finite value clamped between normalized bounds.
float vgfx3d_d3d11_clamp_float_param(float requested,
                                     float min_value,
                                     float max_value,
                                     float fallback);
/// @brief Normalize arbitrary integer flags to shader-facing 0/1 values.
/// @param[in] requested Arbitrary integer flag.
/// @return One for nonzero input; otherwise zero.
int32_t vgfx3d_d3d11_sanitize_bool_flag(int32_t requested);
/// @brief Normalize light type constants to the shader-visible range.
/// @param[in] requested Requested light type.
/// @return Supported type, defaulting to zero.
int32_t vgfx3d_d3d11_sanitize_light_type(int32_t requested);
/// @brief Normalize shadow projection type, disabling projected modes for unshadowed lights.
/// @param[in] sanitized_shadow_index Validated slot or negative sentinel.
/// @param[in] requested_projection_type Requested projection type.
/// @return Sanitized projection type.
int32_t vgfx3d_d3d11_sanitize_shadow_projection_type(int32_t sanitized_shadow_index,
                                                     int32_t requested_projection_type);
/// @brief Clamp and order spot-light cone cosines before shader upload.
/// @param[in] requested_inner Requested inner cosine.
/// @param[in] requested_outer Requested outer cosine.
/// @param[out] out_inner Optional sanitized inner cosine.
/// @param[out] out_outer Optional sanitized outer cosine.
void vgfx3d_d3d11_sanitize_spot_cone(float requested_inner,
                                     float requested_outer,
                                     float *out_inner,
                                     float *out_outer);
/// @brief Sanitize shadow cascade split distances into a finite nondecreasing sequence.
/// @param[out] dst Destination split array.
/// @param[in] src Optional source split array.
/// @param[in] count Number of splits.
void vgfx3d_d3d11_sanitize_shadow_cascade_splits(float *dst, const float *src, size_t count);
/// @brief Clamp a clustered-light global prefix to the uploaded light-array range.
/// @param[in] requested Requested global prefix or negative sentinel.
/// @param[in] light_count Uploaded light count.
/// @return Disabled sentinel or bounded prefix count.
int32_t vgfx3d_d3d11_sanitize_cluster_global_count(int32_t requested, int32_t light_count);
/// @brief Validate every count, offset, and light index before a cluster table reaches HLSL.
/// @param[in] table Cluster table to validate.
/// @param[in] expected_revision Required nonzero light revision.
/// @param[in] light_count Expected uploaded light count.
/// @return One when the table is internally consistent.
int vgfx3d_d3d11_cluster_table_is_usable(const vgfx3d_cluster_table_t *table,
                                         uint32_t expected_revision,
                                         int32_t light_count);
/// @brief Sanitize the clustered-light logarithmic Z range before shader upload.
/// @param[in] requested_near Requested near distance.
/// @param[in] requested_far Requested far distance.
/// @param[out] out_near Optional sanitized near distance.
/// @param[out] out_far Optional sanitized far distance.
void vgfx3d_d3d11_sanitize_cluster_depth_range(float requested_near,
                                               float requested_far,
                                               float *out_near,
                                               float *out_far);
/// @brief Return non-zero only when every value in @p values is finite.
/// @param[in] values Array to inspect.
/// @param[in] count Number of elements.
/// @return One when every requested element is finite.
int vgfx3d_d3d11_float_array_is_finite(const float *values, size_t count);
/// @brief Return non-zero only when every value is finite and within an absolute bound.
/// @param[in] values Array to inspect.
/// @param[in] count Number of elements.
/// @param[in] abs_max Inclusive magnitude limit.
/// @return One when all elements are finite and bounded.
int vgfx3d_d3d11_float_array_is_bounded(const float *values, size_t count, float abs_max);
/// @brief Copy float constants while replacing NaN/Inf lanes with @p fallback.
/// @param[out] dst Destination array.
/// @param[in] src Optional source array.
/// @param[in] count Number of elements.
/// @param[in] fallback Invalid-value replacement.
void vgfx3d_d3d11_copy_float_array_finite_or(float *dst,
                                             const float *src,
                                             size_t count,
                                             float fallback);
/// @brief Copy float constants while replacing invalid lanes and clamping finite extremes.
/// @param[out] dst Destination array.
/// @param[in] src Optional source array.
/// @param[in] count Number of elements.
/// @param[in] min_value Inclusive lower bound.
/// @param[in] max_value Inclusive upper bound.
/// @param[in] fallback Invalid-value replacement.
void vgfx3d_d3d11_copy_float_array_clamped_finite_or(
    float *dst, const float *src, size_t count, float min_value, float max_value, float fallback);
/// @brief Validate a finite, non-degenerate direction vector for CPU/shader upload.
/// @param[in] values Three-component direction.
/// @return One when the direction can be normalized safely.
int vgfx3d_d3d11_vec3_direction_is_usable(const float *values);
/// @brief Copy and normalize a direction vector or a stable fallback when unusable.
/// @param[out] dst Normalized destination vector.
/// @param[in] src Preferred direction.
/// @param[in] fallback Secondary three-component direction.
void vgfx3d_d3d11_copy_vec3_direction_or(float *dst, const float *src, const float fallback[3]);
/// @brief Copy a matrix when finite, otherwise write the identity matrix.
/// @param[out] dst Destination matrix.
/// @param[in] src Preferred matrix.
void vgfx3d_d3d11_copy_mat4_finite_or_identity(float *dst, const float *src);
/// @brief Copy a matrix when finite, otherwise copy @p fallback or identity.
/// @param[out] dst Destination matrix.
/// @param[in] src Preferred matrix.
/// @param[in] fallback Secondary matrix.
void vgfx3d_d3d11_copy_mat4_finite_or(float *dst, const float *src, const float *fallback);
/// @brief Validate a bounded, non-zero light view-projection matrix.
/// @param[in] matrix Matrix to inspect.
/// @return One when bounded and nonzero.
int vgfx3d_d3d11_shadow_matrix_is_usable(const float *matrix);
/// @brief Sanitize D3D11 rasterizer slope-scaled depth bias.
/// @param[in] requested Requested bias.
/// @return Finite, bounded bias.
float vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(float requested);
/// @brief Convert renderer depth bias for either reversed-Z scene or standard-Z shadow passes.
/// @param[in] requested Requested bias.
/// @param[in] reversed_z Nonzero for reversed-Z conversion.
/// @return D3D11 integer depth-bias units.
int32_t vgfx3d_d3d11_depth_bias_units(float requested, int reversed_z);
/// @brief Sign and clamp slope bias for either reversed-Z scene or standard-Z shadow passes.
/// @param[in] requested Requested slope bias.
/// @param[in] reversed_z Nonzero for reversed-Z conversion.
/// @return Sanitized convention-aware slope bias.
float vgfx3d_d3d11_depth_slope_bias(float requested, int reversed_z);
/// @brief Validate a packed per-instance bone-palette layout before shader indexing.
/// @param[in] requested_stride Bones assigned per instance.
/// @param[in] total_bone_count Total packed bones.
/// @param[in] instance_count Number of instances.
/// @return Validated stride, or zero.
int32_t vgfx3d_d3d11_sanitize_instance_bone_stride(int32_t requested_stride,
                                                   int32_t total_bone_count,
                                                   int32_t instance_count);
/// @brief Convert one finite NDC coordinate to a clamped texture pixel coordinate.
/// @param[in] ndc Coordinate to convert.
/// @param[in] extent Texture-axis extent.
/// @param[in] invert_axis Nonzero to reverse the axis.
/// @param[out] out_pixel Resulting texel coordinate.
/// @return One on success; otherwise zero.
int vgfx3d_d3d11_ndc_to_pixel(float ndc, int32_t extent, int invert_axis, int32_t *out_pixel);
/// @brief Convert reversed-Z probe storage to canonical window depth or -1 on invalid input.
/// @param[in] reversed_depth Raw reversed-Z sample.
/// @return Canonical depth or invalid sentinel.
float vgfx3d_d3d11_sanitize_depth_probe_result(float reversed_depth);
/// @brief Clamp screen-space-reflection steps to the shader's actual loop bounds.
/// @param[in] requested Requested step count.
/// @return Shader-supported step count.
int32_t vgfx3d_d3d11_sanitize_ssr_steps(int32_t requested);
/// @brief Normalize material workflow constants before shader upload.
/// @param[in] requested Requested workflow.
/// @return PBR or legacy workflow.
int32_t vgfx3d_d3d11_sanitize_material_workflow(int32_t requested);
/// @brief Normalize material alpha-mode constants before shader upload.
/// @param[in] requested Requested alpha mode.
/// @return Supported mode, defaulting to opaque.
int32_t vgfx3d_d3d11_sanitize_alpha_mode(int32_t requested);
/// @brief Normalize Game3D shading-model constants before shader upload.
/// @param[in] requested Requested model index.
/// @return Supported model, defaulting to zero.
int32_t vgfx3d_d3d11_sanitize_shading_model(int32_t requested);
/// @brief Normalize tonemap mode constants before post-FX shader upload.
/// @param[in] requested Requested tonemap mode.
/// @return Supported mode, defaulting to zero.
int32_t vgfx3d_d3d11_sanitize_tonemap_mode(int32_t requested);
/// @brief Sanitize fog near/far distances before scene constant upload.
/// @param[in] requested_near Requested fog start.
/// @param[in] requested_far Requested fog end.
/// @param[out] out_near Optional sanitized start.
/// @param[out] out_far Optional sanitized end.
void vgfx3d_d3d11_sanitize_fog_range(float requested_near,
                                     float requested_far,
                                     float *out_near,
                                     float *out_far);
/// @brief Sanitize D3D11 shader-facing shadow depth bias.
/// @param[in] requested Requested bias.
/// @return Finite bounded bias.
float vgfx3d_d3d11_sanitize_shadow_bias(float requested);
/// @brief Validate a backend-facing post-FX chain before indexed iteration.
/// @param[in] chain Chain to validate.
/// @return Nonzero when usable.
int vgfx3d_d3d11_postfx_chain_is_usable(const vgfx3d_postfx_chain_t *chain);
/// @brief Return non-zero when one PostFX effect descriptor actually changes rendering.
/// @param[in] effect Effect to inspect.
/// @return One when its corresponding enable flag is active.
int vgfx3d_d3d11_postfx_effect_is_active(const vgfx3d_postfx_effect_desc_t *effect);
/// @brief Return non-zero when a usable chain contains an active effect of @p type_value.
/// @param[in] chain Chain to inspect.
/// @param[in] type_value Effect type to locate.
/// @return One when a matching active effect exists.
int vgfx3d_d3d11_postfx_chain_has_active_effect(const vgfx3d_postfx_chain_t *chain,
                                                int32_t type_value);
/// @brief Return non-zero when a usable chain contains any active effect.
/// @param[in] chain Chain to inspect.
/// @return One when any effect is active.
int vgfx3d_d3d11_postfx_chain_has_active_effects(const vgfx3d_postfx_chain_t *chain);
/// @brief Decide whether the bone constant buffers need a per-draw upload.
/// @param[in] has_skinning Current-palette requirement.
/// @param[in] has_prev_skinning History-palette requirement.
/// @return One when either palette is required.
int vgfx3d_d3d11_should_upload_bone_palette(int has_skinning, int has_prev_skinning);
/// @brief Saturating unsigned 64-bit addition for upload counters.
/// @param[in] a First value.
/// @param[in] b Second value.
/// @return Saturating sum.
uint64_t vgfx3d_d3d11_saturating_add_u64(uint64_t a, uint64_t b);
/// @brief Capacity-doubling growth helper (saturates at INT_MAX).
/// @param[in] current_capacity Existing capacity.
/// @param[in] needed Required capacity.
/// @param[in] minimum_capacity Initial growth floor.
/// @return Retained or grown sufficient capacity.
int32_t vgfx3d_d3d11_next_capacity(int32_t current_capacity,
                                   int32_t needed,
                                   int32_t minimum_capacity);
/// @brief Overflow-safe row byte computation for tightly packed readback rows.
/// @param[in] width Positive row width.
/// @param[in] bytes_per_pixel Positive element size.
/// @param[out] out_bytes Checked row size.
/// @return One on success; otherwise zero.
int vgfx3d_d3d11_compute_row_bytes(int32_t width, int32_t bytes_per_pixel, size_t *out_bytes);
/// @brief Compute a valid non-zero D3D11 buffer ByteWidth.
/// @param[in] size Requested byte size.
/// @param[out] out_width D3D11-compatible width.
/// @return One when representable.
int vgfx3d_d3d11_compute_buffer_byte_width(size_t size, uint32_t *out_width);
/// @brief Compute a 16-byte-aligned D3D11 constant-buffer ByteWidth.
/// @param[in] size Logical byte size.
/// @param[out] out_width Aligned descriptor width.
/// @return One when supported.
int vgfx3d_d3d11_compute_constant_buffer_byte_width(size_t size, uint32_t *out_width);
/// @brief Compute a D3D11 UpdateSubresource pitch for tightly packed RGBA8 rows.
/// @param[in] width Image width.
/// @param[out] out_pitch Checked row pitch.
/// @return One on success.
int vgfx3d_d3d11_compute_rgba8_upload_pitch(int32_t width, uint32_t *out_pitch);
/// @brief Compute one mip level extent for a square D3D11 texture chain.
/// @param[in] base_extent Base square extent.
/// @param[in] mip_level Zero-based mip.
/// @param[out] out_extent Resulting extent.
/// @return One when valid.
int vgfx3d_d3d11_expected_square_mip_extent(int32_t base_extent,
                                            int32_t mip_level,
                                            int32_t *out_extent);
/// @brief Compute one bloom mip extent for a scene-sized D3D11 post-FX chain.
/// @param[in] width Scene width.
/// @param[in] height Scene height.
/// @param[in] mip_level Zero-based bloom mip.
/// @param[out] out_width Result width.
/// @param[out] out_height Result height.
/// @return One when the level is valid.
int vgfx3d_d3d11_compute_bloom_mip_extent(
    int32_t width, int32_t height, int32_t mip_level, int32_t *out_width, int32_t *out_height);
/// @brief Validate an IBL mip payload extent against the destination cubemap mip.
/// @param[in] face_size Base face size.
/// @param[in] mip_level Zero-based mip.
/// @param[in] width Payload width.
/// @param[in] height Payload height.
/// @return One for the expected square extent.
int vgfx3d_d3d11_validate_ibl_mip_extent(int32_t face_size,
                                         int32_t mip_level,
                                         int32_t width,
                                         int32_t height);
/// @brief Validate a prefiltered IBL chain and return its destination base mip.
/// @param[in] face_size Destination base size.
/// @param[in] ibl_base_size Source IBL base size.
/// @param[in] ibl_mip_count Source level count.
/// @param[in] max_ibl_mips Destination level limit.
/// @param[out] out_level_base Destination starting level.
/// @return One for an exact valid mapping.
int vgfx3d_d3d11_validate_ibl_layout(int32_t face_size,
                                     int32_t ibl_base_size,
                                     int32_t ibl_mip_count,
                                     int32_t max_ibl_mips,
                                     int32_t *out_level_base);
/// @brief Return non-zero when one complete upload fits the remaining frame budget.
/// @param[in] budget Total budget.
/// @param[in] used Already consumed bytes.
/// @param[in] requested Additional bytes.
/// @return One when the request fits.
int vgfx3d_d3d11_upload_budget_allows(uint64_t budget, uint64_t used, uint64_t requested);
/// @brief Report pending texture bytes without dereferencing a cache-owned source identity.
/// @param[in] has_native_asset Native-asset selection flag.
/// @param[in] cached_native_bytes Cached native pending bytes.
/// @param[in] width RGBA fallback width.
/// @param[in] height RGBA fallback height.
/// @param[in] next_row RGBA fallback cursor.
/// @param[in] upload_in_progress Active-upload flag.
/// @return Pending bytes for the selected representation.
uint64_t vgfx3d_d3d11_cached_pending_texture_bytes(int has_native_asset,
                                                   uint64_t cached_native_bytes,
                                                   int32_t width,
                                                   int32_t height,
                                                   int32_t next_row,
                                                   int upload_in_progress);
/// @brief Compute a bounded morph-delta float count for D3D11 float SRV uploads.
/// @param[in] vertex_count Vertices per shape.
/// @param[in] requested_shape_count Requested shapes.
/// @param[out] out_elements Float element count.
/// @return One when representable.
int vgfx3d_d3d11_compute_morph_float_count(uint32_t vertex_count,
                                           int32_t requested_shape_count,
                                           size_t *out_elements);
/// @brief Compute a D3D11 vertex-buffer upload span for per-instance data.
/// @param[in] instance_count Instance count.
/// @param[in] instance_stride Bytes per instance.
/// @param[out] out_bytes Total upload bytes.
/// @return One when valid and representable.
int vgfx3d_d3d11_compute_instance_upload_bytes(int32_t instance_count,
                                               size_t instance_stride,
                                               size_t *out_bytes);
/// @brief Compute the byte span for a partial float-SRV buffer update.
/// @param[in] element_count Live elements.
/// @param[in] capacity Allocated elements.
/// @param[out] out_bytes Live byte span.
/// @return One when the live range is valid.
int vgfx3d_d3d11_compute_float_srv_update_bytes(size_t element_count,
                                                size_t capacity,
                                                size_t *out_bytes);
/// @brief Check a float-buffer element count against D3D11's typed-buffer limit.
/// @param[in] element_count Float elements.
/// @return One when nonzero and supported.
int vgfx3d_d3d11_is_valid_float_srv_element_count(size_t element_count);
/// @brief Choose a geometrically grown typed-float-buffer capacity.
/// @param[in] current_capacity Existing elements.
/// @param[in] needed_capacity Required elements.
/// @param[out] out_capacity Retained or grown capacity.
/// @return One when a supported capacity exists.
int vgfx3d_d3d11_compute_float_srv_capacity(size_t current_capacity,
                                            size_t needed_capacity,
                                            size_t *out_capacity);
/// @brief Validate cached typed-float buffer descriptor metadata.
/// @param[in] byte_width Native descriptor width.
/// @param[in] tracked_capacity Backend-tracked float-element capacity.
/// @param[in] required_elements Minimum elements required by the next upload.
/// @param[in] has_default_usage Default-usage flag.
/// @param[in] has_exact_srv_bind Exact shader-resource bind flag.
/// @param[in] has_no_cpu_access No-CPU-access flag.
/// @param[in] misc_flags Miscellaneous flags.
/// @param[in] structure_byte_stride Structure stride.
/// @return One when the native descriptor and cached capacity agree.
int vgfx3d_d3d11_float_srv_buffer_desc_is_usable(uint32_t byte_width,
                                                 size_t tracked_capacity,
                                                 size_t required_elements,
                                                 int has_default_usage,
                                                 int has_exact_srv_bind,
                                                 int has_no_cpu_access,
                                                 uint32_t misc_flags,
                                                 uint32_t structure_byte_stride);
/// @brief Validate cached typed-float shader-resource-view metadata.
/// @param[in] tracked_capacity Backend-tracked float-element capacity.
/// @param[in] has_r32_float_format R32_FLOAT format flag.
/// @param[in] has_buffer_dimension Buffer-dimension flag.
/// @param[in] first_element First exposed buffer element.
/// @param[in] num_elements Number of exposed elements.
/// @return One when the view exposes exactly the tracked float buffer.
int vgfx3d_d3d11_float_srv_view_desc_is_usable(size_t tracked_capacity,
                                               int has_r32_float_format,
                                               int has_buffer_dimension,
                                               uint32_t first_element,
                                               uint32_t num_elements);
/// @brief Validate the structural contract required by a dynamic constant buffer.
/// @param[in] byte_width Descriptor width.
/// @param[in] has_dynamic_usage Dynamic-usage flag.
/// @param[in] has_constant_buffer_bind Constant-buffer bind flag.
/// @param[in] has_cpu_write_access CPU-write flag.
/// @param[in] misc_flags Miscellaneous flags.
/// @param[in] structure_byte_stride Structure stride.
/// @return One for a usable descriptor.
int vgfx3d_d3d11_constant_buffer_desc_is_usable(uint32_t byte_width,
                                                int has_dynamic_usage,
                                                int has_constant_buffer_bind,
                                                int has_cpu_write_access,
                                                uint32_t misc_flags,
                                                uint32_t structure_byte_stride);
/// @brief Validate cached dynamic vertex/index-buffer descriptor metadata.
/// @param[in] byte_width Native descriptor width.
/// @param[in] tracked_capacity Backend-tracked byte capacity.
/// @param[in] required_bytes Minimum bytes required by the next upload.
/// @param[in] has_dynamic_usage Dynamic-usage flag.
/// @param[in] has_exact_bind_flags Exact expected vertex/index bind flag.
/// @param[in] has_cpu_write_access CPU-write flag.
/// @param[in] misc_flags Miscellaneous flags.
/// @param[in] structure_byte_stride Structure stride.
/// @return One when the native descriptor and cached capacity agree.
int vgfx3d_d3d11_dynamic_buffer_desc_is_usable(uint32_t byte_width,
                                               size_t tracked_capacity,
                                               size_t required_bytes,
                                               int has_dynamic_usage,
                                               int has_exact_bind_flags,
                                               int has_cpu_write_access,
                                               uint32_t misc_flags,
                                               uint32_t structure_byte_stride);
/// @brief Validate an RGBA8 readback destination span.
/// @param[in] width Destination width.
/// @param[in] height Destination height.
/// @param[in] stride Row stride.
/// @param[out] out_bytes Optional total span.
/// @return One when dimensions and span are valid.
int vgfx3d_d3d11_validate_rgba8_destination(int32_t width,
                                            int32_t height,
                                            int32_t stride,
                                            size_t *out_bytes);
/// @brief Validate a row range before building a D3D11_BOX.
/// @param[in] extent Total rows.
/// @param[in] start First row.
/// @param[in] count Row count.
/// @return One for an in-bounds positive span.
int vgfx3d_d3d11_validate_row_span(int32_t extent, int32_t start, int32_t count);
/// @brief Check dimensions against D3D11 feature-level 11 texture limits.
/// @param[in] width Texture width.
/// @param[in] height Texture height.
/// @return One when both are supported.
int vgfx3d_d3d11_is_valid_texture2d_extent(int32_t width, int32_t height);
/// @brief Validate the structural fields required for a one-subresource Texture2D copy.
/// @param[in] width Texture width.
/// @param[in] height Texture height.
/// @param[in] mip_levels Mip count.
/// @param[in] array_size Array size.
/// @param[in] sample_count Sample count.
/// @param[in] sample_quality Sample quality.
/// @return One for the required single-subresource shape.
int vgfx3d_d3d11_is_single_subresource_texture2d(uint32_t width,
                                                 uint32_t height,
                                                 uint32_t mip_levels,
                                                 uint32_t array_size,
                                                 uint32_t sample_count,
                                                 uint32_t sample_quality);
/// @brief Check a square cubemap face dimension against D3D11 limits.
/// @param[in] face_size Cubemap face extent.
/// @return One when supported.
int vgfx3d_d3d11_is_valid_cubemap_extent(int32_t face_size);
/// @brief Validate an in-progress row-upload cursor without silently rewinding it.
/// @param[in] extent Total rows.
/// @param[in] next_row Pending row.
/// @return One for an unfinished in-bounds cursor.
int vgfx3d_d3d11_row_upload_cursor_is_valid(int32_t extent, int32_t next_row);
/// @brief Validate an in-progress cubemap face/row cursor.
/// @param[in] face_size Face extent.
/// @param[in] face Pending face.
/// @param[in] next_row Pending row.
/// @return One for a valid unfinished cursor.
int vgfx3d_d3d11_cubemap_upload_cursor_is_valid(int32_t face_size, int32_t face, int32_t next_row);
/// @brief Validate an in-progress native mip/block-row cursor.
/// @param[in] mip_count Total mips.
/// @param[in] next_mip Pending mip.
/// @param[in] block_rows Rows in that mip.
/// @param[in] next_block_row Pending block row.
/// @return One for a valid unfinished cursor.
int vgfx3d_d3d11_native_upload_cursor_is_valid(int64_t mip_count,
                                               int64_t next_mip,
                                               uint64_t block_rows,
                                               int32_t next_block_row);
/// @brief Validate a mapped texture row span before copying it into an RGBA8 destination.
/// @param[in] width Pixels per row.
/// @param[in] dst_stride Destination stride.
/// @param[in] src_row_pitch Source pitch.
/// @param[in] src_bytes_per_pixel Source pixel size.
/// @param[out] out_src_row_bytes Optional packed source bytes.
/// @param[out] out_dst_row_bytes Optional packed destination bytes.
/// @return One when both rows fit.
int vgfx3d_d3d11_validate_mapped_texture_copy(int32_t width,
                                              int32_t dst_stride,
                                              uint32_t src_row_pitch,
                                              int32_t src_bytes_per_pixel,
                                              size_t *out_src_row_bytes,
                                              size_t *out_dst_row_bytes);
/// @brief Bytes in one compressed/native texture block row.
/// @param[in] mip Native mip descriptor.
/// @return Required bytes, or zero.
uint64_t vgfx3d_d3d11_native_mip_row_bytes(const vgfx3d_native_texture_mip_t *mip);
/// @brief Number of compressed/native block rows in a mip.
/// @param[in] mip Native mip descriptor.
/// @return Required rows, or zero.
uint64_t vgfx3d_d3d11_native_mip_block_rows(const vgfx3d_native_texture_mip_t *mip);
/// @brief Minimum bytes needed for the complete compressed/native mip payload.
/// @param[in] mip Native mip descriptor.
/// @return Required payload bytes, or zero.
uint64_t vgfx3d_d3d11_native_mip_required_bytes(const vgfx3d_native_texture_mip_t *mip);
/// @brief Expected block layout for a D3D11-native compressed texture format.
/// @param[in] format_id Native format.
/// @param[out] out_block_width Block width.
/// @param[out] out_block_height Block height.
/// @param[out] out_block_bytes Bytes per block.
/// @return One for a supported format.
int vgfx3d_d3d11_native_format_block_layout(int32_t format_id,
                                            int32_t *out_block_width,
                                            int32_t *out_block_height,
                                            int32_t *out_block_bytes);
/// @brief Check that a native compressed mip is valid for a D3D11 texture mip chain.
/// @param[in] mip Descriptor to validate.
/// @param[in] previous_mip Optional preceding mip.
/// @param[in] expected_format_id Required format.
/// @param[in] expected_block_width Required or inferred block width.
/// @param[in] expected_block_height Required or inferred block height.
/// @param[in] expected_block_bytes Required or inferred bytes per block.
/// @return One when chain, layout, and payload are valid.
int vgfx3d_d3d11_validate_native_mip_desc(const vgfx3d_native_texture_mip_t *mip,
                                          const vgfx3d_native_texture_mip_t *previous_mip,
                                          int32_t expected_format_id,
                                          int32_t expected_block_width,
                                          int32_t expected_block_height,
                                          int32_t expected_block_bytes);
/// @brief Check that a native mip count can fit in the D3D11 texture descriptor.
/// @param[in] base_width Base width.
/// @param[in] base_height Base height.
/// @param[in] mip_count Requested levels.
/// @return One when representable and within a full chain.
int vgfx3d_d3d11_is_valid_native_mip_count(int32_t base_width,
                                           int32_t base_height,
                                           int64_t mip_count);
/// @brief Clamp morph shapes so shader-side int indexing cannot overflow.
/// @param[in] vertex_count Vertices per shape.
/// @param[in] requested_shape_count Requested shapes.
/// @return Safe shape count, or zero.
int32_t vgfx3d_d3d11_clamp_morph_shape_count(uint32_t vertex_count, int32_t requested_shape_count);
/// @brief Decide whether an aged cache entry can be pruned without dropping below the floor.
/// @param[in] total_count Original entries.
/// @param[in] kept_count Already retained entries.
/// @param[in] scan_index Current entry.
/// @param[in] age Current age.
/// @param[in] max_resident Resident floor.
/// @param[in] prune_age Pruning threshold.
/// @return One when safe to remove.
int vgfx3d_d3d11_should_prune_cache_entry(int32_t total_count,
                                          int32_t kept_count,
                                          int32_t scan_index,
                                          uint64_t age,
                                          int32_t max_resident,
                                          uint64_t prune_age);
/// @brief Convert a valid D3D11 timestamp pair to rounded microseconds.
/// @param[in] disjoint Invalid-interval flag.
/// @param[in] frequency Ticks per second.
/// @param[in] start_ticks Start timestamp.
/// @param[in] end_ticks End timestamp.
/// @param[out] out_microseconds Rounded duration.
/// @return One for a valid interval.
int vgfx3d_d3d11_compute_gpu_time_us(int disjoint,
                                     uint64_t frequency,
                                     uint64_t start_ticks,
                                     uint64_t end_ticks,
                                     uint64_t *out_microseconds);
/// @brief Decide when a perpetually busy timestamp query should be abandoned.
/// @param[in] pending_polls Unsuccessful poll count.
/// @return One at or beyond the limit.
int vgfx3d_d3d11_should_abandon_frame_timing(uint32_t pending_polls);
/// @brief Decide when a perpetually busy depth-probe staging map should be abandoned.
/// @param[in] pending_polls Unsuccessful poll count.
/// @return One at or beyond the limit.
int vgfx3d_d3d11_should_abandon_depth_probe(uint32_t pending_polls);
/// @brief Return non-zero only when no frame is active or awaiting presentation.
/// @param[in] frame_active Open-frame flag.
/// @param[in] frame_pending_present Pending-present flag.
/// @return One when both are clear.
int vgfx3d_d3d11_frame_state_is_idle(int8_t frame_active, int8_t frame_pending_present);
/// @brief Validate the CPU-side state required by ordinary color draw submission.
/// @param[in] frame_active Open-frame flag.
/// @param[in] shadow_pass_slot Active shadow slot or negative.
/// @param[in] has_device_context Device-context flag.
/// @param[in] render_target_count Bound color targets.
/// @param[in] has_primary_render_target Primary-target flag.
/// @param[in] target_width Viewport width.
/// @param[in] target_height Viewport height.
/// @return One when all draw prerequisites hold.
int vgfx3d_d3d11_draw_submission_is_ready(int8_t frame_active,
                                          int32_t shadow_pass_slot,
                                          int has_device_context,
                                          uint32_t render_target_count,
                                          int has_primary_render_target,
                                          int32_t target_width,
                                          int32_t target_height);
/// @brief Pick the right render-target classification (RTT > swapchain > overlay > scene).
/// @param[in] rtt_active Explicit RTT flag.
/// @param[in] gpu_postfx_enabled Offscreen scene-route flag.
/// @param[in] load_existing_color Preserve-color flag.
/// @return Selected target class.
vgfx3d_d3d11_target_kind_t vgfx3d_d3d11_choose_target_kind(int8_t rtt_active,
                                                           int8_t gpu_postfx_enabled,
                                                           int8_t load_existing_color);
/// @brief Downgrade an intended target when allocation failed or the resource is unavailable.
/// @param[in] requested Requested target.
/// @param[in] scene_available Scene-resource flag.
/// @param[in] overlay_available Overlay-resource flag.
/// @param[in] rtt_available RTT-resource flag.
/// @return Requested target or safe fallback.
vgfx3d_d3d11_target_kind_t vgfx3d_d3d11_resolve_available_target(
    vgfx3d_d3d11_target_kind_t requested,
    int scene_available,
    int overlay_available,
    int rtt_available);
/// @brief Decide whether a cached morph payload still matches a draw command.
/// @param[in] cached_key Cached identity.
/// @param[in] cached_revision Cached revision.
/// @param[in] cached_shape_count Cached shapes.
/// @param[in] cached_vertex_count Cached vertices.
/// @param[in] cached_has_normal_deltas Cached-normal flag.
/// @param[in] cmd Candidate command.
/// @return One when all discriminators match.
int vgfx3d_d3d11_should_reuse_morph_cache(const void *cached_key,
                                          uint64_t cached_revision,
                                          int32_t cached_shape_count,
                                          uint32_t cached_vertex_count,
                                          int8_t cached_has_normal_deltas,
                                          const vgfx3d_draw_cmd_t *cmd);
/// @brief Count the contiguous complete shadow slots that are safe to advertise to HLSL.
/// @param[in] slot_count Available flags.
/// @param[in] slot_complete Per-slot completion flags.
/// @return Length of the completed prefix.
int32_t vgfx3d_d3d11_compute_shadow_count(int32_t slot_count, const int *slot_complete);
/// @brief Clamp or disable a light's shadow slot against the advertised slot range.
/// @param[in] requested_shadow_index Requested slot.
/// @param[in] advertised_shadow_count Completed prefix length.
/// @return Valid slot or `-1`.
int32_t vgfx3d_d3d11_sanitize_shadow_index(int32_t requested_shadow_index,
                                           int32_t advertised_shadow_count);
/// @brief Validate that a projected shadow owns every slot its projection mode requires.
/// @param[in] requested_shadow_index First requested slot.
/// @param[in] advertised_shadow_count Completed prefix length.
/// @param[in] projection_type Projection mode.
/// @return Valid first slot or `-1`.
int32_t vgfx3d_d3d11_sanitize_shadow_index_for_projection(int32_t requested_shadow_index,
                                                          int32_t advertised_shadow_count,
                                                          int32_t projection_type);
/// @brief Clamp a light's cascade count so it cannot address beyond advertised shadow slots.
/// @param[in] requested_cascade_count Requested cascades.
/// @param[in] sanitized_shadow_index Validated first slot.
/// @param[in] advertised_shadow_count Completed prefix length.
/// @param[in] projection_type Projection mode.
/// @return Safe cascade count.
int32_t vgfx3d_d3d11_sanitize_shadow_cascade_count(int32_t requested_cascade_count,
                                                   int32_t sanitized_shadow_index,
                                                   int32_t advertised_shadow_count,
                                                   int32_t projection_type);
/// @brief Clamp a shadow-count value to the D3D11 shader-visible shadow slots.
/// @param[in] advertised_shadow_count Requested count.
/// @return Count clamped to fixed capacity.
int32_t vgfx3d_d3d11_clamp_shadow_count(int32_t advertised_shadow_count);
/// @brief Select the depth texture that represents the active scene route.
/// @param[in] rtt_active Explicit RTT flag.
/// @param[in] gpu_postfx_enabled Offscreen scene-route flag.
/// @param[in] has_rtt_depth RTT-depth flag.
/// @param[in] has_scene_depth Scene-depth flag.
/// @param[in] has_swapchain_depth Swapchain-depth flag.
/// @return Readable depth target class or none.
vgfx3d_d3d11_target_kind_t vgfx3d_d3d11_choose_depth_probe_target(int8_t rtt_active,
                                                                  int8_t gpu_postfx_enabled,
                                                                  int has_rtt_depth,
                                                                  int has_scene_depth,
                                                                  int has_swapchain_depth);
/// @brief Project a world-space point through a shadow matrix to HLSL UV/depth coordinates.
/// @param[in] shadow_vp Light view-projection matrix.
/// @param[in] projection_type Projection mode.
/// @param[in] world_pos World-space point.
/// @param[out] out_uv_depth Texture U, V, and depth.
/// @return One for finite valid projection.
int vgfx3d_d3d11_project_shadow_coord(const float *shadow_vp,
                                      int32_t projection_type,
                                      const float world_pos[3],
                                      float out_uv_depth[3]);
/// @brief Decide whether an RTT frame has enough state to mark its CPU mirror dirty.
/// @param[in] rtt_active RTT-active flag.
/// @param[in] has_target Host-target flag.
/// @param[in] has_color_tex Color-texture flag.
/// @param[in] has_color_rtv Color-RTV flag.
/// @param[in] has_depth_tex Depth-texture flag.
/// @param[in] has_depth_dsv Depth-view flag.
/// @param[in] has_staging Staging-texture flag.
/// @return One when all required state exists.
int vgfx3d_d3d11_should_mark_rtt_dirty(int8_t rtt_active,
                                       int has_target,
                                       int has_color_tex,
                                       int has_color_rtv,
                                       int has_depth_tex,
                                       int has_depth_dsv,
                                       int has_staging);
/// @brief Validate host/render-target metadata before a D3D11 RTT staging copy.
/// @param[in] target_width Host width.
/// @param[in] target_height Host height.
/// @param[in] target_format Host format.
/// @param[in] resource_width Resource width.
/// @param[in] resource_height Resource height.
/// @param[in] resource_format Resource format.
/// @return One when supported metadata matches.
int vgfx3d_d3d11_rtt_readback_state_matches(int32_t target_width,
                                            int32_t target_height,
                                            int32_t target_format,
                                            int32_t resource_width,
                                            int32_t resource_height,
                                            int32_t resource_format);
/// @brief Validate the public render-target color-format discriminator.
/// @param[in] color_format Public format value.
/// @return One for UNORM8 or HDR16F.
int vgfx3d_d3d11_is_valid_rtt_color_format(int32_t color_format);
/// @brief Validate the internal D3D11 target color-format class.
/// @param[in] color_format Internal format value.
/// @return One for UNORM8 or HDR16F.
int vgfx3d_d3d11_is_valid_color_format(int32_t color_format);
/// @brief Validate a cached bloom mip count before indexing fixed backend arrays.
/// @param[in] mip_count Cached level count.
/// @return One when within fixed capacity.
int vgfx3d_d3d11_is_valid_bloom_mip_count(int32_t mip_count);
/// @brief Map a draw command to its required blend state (alpha vs opaque).
/// @param[in] cmd Draw command.
/// @return Additive, alpha, or opaque mode.
vgfx3d_d3d11_blend_mode_t vgfx3d_d3d11_choose_blend_mode(const vgfx3d_draw_cmd_t *cmd);
/// @brief Pick the color format — HDR16F for the scene pass, UNORM8 elsewhere.
/// @param[in] target_kind Target class.
/// @return HDR16F for scene, otherwise UNORM8.
vgfx3d_d3d11_color_format_t vgfx3d_d3d11_choose_color_format(
    vgfx3d_d3d11_target_kind_t target_kind);
/// @brief Decide whether the next pass should preserve existing color contents.
/// @param[in] target_kind Resolved target.
/// @param[in] requested_load_existing_color Preserve request.
/// @param[in] overlay_used_this_frame Prior-overlay flag.
/// @return One when color should be loaded.
int8_t vgfx3d_d3d11_should_load_existing_color(vgfx3d_d3d11_target_kind_t target_kind,
                                               int8_t requested_load_existing_color,
                                               int8_t overlay_used_this_frame);
/// @brief Decide whether to attach the motion-vector target.
/// @param[in] target_kind Target class.
/// @param[in] cmd Draw command.
/// @return Color-and-motion or color-only mode.
vgfx3d_d3d11_motion_attachment_mode_t vgfx3d_d3d11_choose_motion_attachment_mode(
    vgfx3d_d3d11_target_kind_t target_kind, const vgfx3d_draw_cmd_t *cmd);
/// @brief Decide whether terrain splatting has all required textures bound.
/// @param[in] cmd_has_splat Draw splat flag.
/// @param[in] has_splat_map Control-map flag.
/// @param[in] has_layer0 Layer-zero flag.
/// @param[in] has_layer1 Layer-one flag.
/// @param[in] has_layer2 Layer-two flag.
/// @param[in] has_layer3 Layer-three flag.
/// @return One when the complete splat set exists.
int vgfx3d_d3d11_has_complete_splat(int8_t cmd_has_splat,
                                    int has_splat_map,
                                    int has_layer0,
                                    int has_layer1,
                                    int has_layer2,
                                    int has_layer3);
/// @brief Report whether an SRV is a completed resource rather than a streaming fallback.
/// @param[in] has_srv Bindable-view flag.
/// @param[in] is_fallback_srv Placeholder flag.
/// @return One for a resident non-fallback view.
int vgfx3d_d3d11_srv_is_ready(int has_srv, int is_fallback_srv);
/// @brief Decide whether scene color should be composited to the swapchain now.
/// @param[in] rtt_active Explicit RTT flag.
/// @param[in] gpu_postfx_enabled Offscreen route flag.
/// @param[in] has_scene_targets Scene-resource flag.
/// @param[in] scene_composited_to_swapchain Already-resolved flag.
/// @return One when composition remains necessary.
int vgfx3d_d3d11_should_composite_to_swapchain(int8_t rtt_active,
                                               int8_t gpu_postfx_enabled,
                                               int has_scene_targets,
                                               int8_t scene_composited_to_swapchain);
/// @brief Decide whether a new begin-frame invalidates a prior swapchain composite.
/// @param[in] rtt_active New-frame RTT flag.
/// @param[in] load_existing_color Preserve-color flag.
/// @return One when prior composite state must reset.
int vgfx3d_d3d11_should_reset_composited_swapchain_for_frame(int8_t rtt_active,
                                                             int8_t load_existing_color);
/// @brief Decide whether a post-FX enable update invalidates a prior swapchain composite.
/// @param[in] current_enabled Current state.
/// @param[in] requested_enabled Requested state.
/// @return One when normalized states differ.
int vgfx3d_d3d11_should_reset_composited_swapchain_for_postfx_update(int8_t current_enabled,
                                                                     int8_t requested_enabled);
/// @brief Decide whether a begin-frame should preserve scene temporal history as an overlay pass.
/// @param[in] resolved_target_kind Resolved target.
/// @param[in] requested_load_existing_color Preserve request.
/// @return One for a logical overlay pass.
int vgfx3d_d3d11_should_treat_begin_frame_as_overlay(
    vgfx3d_d3d11_target_kind_t resolved_target_kind, int8_t requested_load_existing_color);
/// @brief Decide whether the overlay pass is rendering into the separate overlay target.
/// @param[in] resolved_target_kind Resolved target.
/// @param[in] has_overlay_target Overlay-resource flag.
/// @return One for the separate overlay route.
int vgfx3d_d3d11_uses_separate_overlay_target(vgfx3d_d3d11_target_kind_t resolved_target_kind,
                                              int has_overlay_target);
/// @brief Decide whether readback should source a present snapshot, backbuffer, post-FX, or scene.
/// @param[in] presented_snapshot_valid Snapshot-valid flag.
/// @param[in] presented_snapshot_has_texture Snapshot-texture flag.
/// @param[in] scene_composited_to_swapchain Scene-resolved flag.
/// @param[in] gpu_postfx_enabled Post-FX route flag.
/// @param[in] postfx_chain_valid Chain-valid flag.
/// @param[in] postfx_chain_enabled Chain-enabled flag.
/// @param[in] postfx_effect_count Effect count.
/// @param[in] postfx_has_effects Active-effect flag.
/// @param[in] has_scene_targets Scene-resource flag.
/// @param[in] current_target_kind Current target class.
/// @return Selected readback source class.
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
    vgfx3d_d3d11_target_kind_t current_target_kind);
/// @brief Decide whether a DXGI Present status confirms that a frame reached the display path.
/// @param[in] present_status Signed 32-bit HRESULT/status value returned by `Present`.
/// @return One only for the exact `S_OK` value; informational success statuses are not counted.
int vgfx3d_d3d11_present_status_confirms_display(int32_t present_status);
/// @brief Decide whether a pre-present snapshot remains valid after Present returns.
/// @param[in] snapshot_ok Snapshot result.
/// @param[in] present_ok Present result.
/// @return One only when both succeeded.
int vgfx3d_d3d11_should_keep_presented_snapshot(int snapshot_ok, int present_ok);

#ifdef __cplusplus
}
#endif
