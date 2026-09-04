//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_metal_shared.h
// Purpose: Shared declarations/constants for the Metal vgfx3d backend —
//   bone-palette limits and shared uniform layouts used by the Objective-C
//   Metal backend and its C shared support unit.
//
// Key invariants:
//   - Layouts here must match the Metal shaders and stay consistent with the
//     D3D11/OpenGL shared layouts (same bone/light limits).
//   - Internal to the Metal backend; not part of the public Zanna API.
//
// Ownership/Lifetime:
//   - Declarations only; no allocation or ownership semantics here.
//
// Links: src/runtime/graphics/3d/backend/vgfx3d_backend_metal_shared.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares platform-neutral data layouts and policy helpers used by the Metal 3D backend.
/// @details The constants and structures in this internal header form the CPU side of several MSL
///          buffer contracts. Matrix packing, cache decisions, render-target selection, and
///          shadow-coordinate helpers are implemented without Objective-C dependencies so their
///          behavior remains independently reusable and deterministic.

#pragma once

#include "vgfx3d_backend.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VGFX3D_METAL_MAX_BONES 256
#define VGFX3D_METAL_BONE_PALETTE_FLOATS (VGFX3D_METAL_MAX_BONES * 16u)
#define VGFX3D_METAL_BONE_PALETTE_BYTES (sizeof(float) * VGFX3D_METAL_BONE_PALETTE_FLOATS)
#define VGFX3D_METAL_MAX_MORPH_SHAPES 64
#define VGFX3D_METAL_MAX_TEXTURE_ANISOTROPY 16
#define VGFX3D_METAL_ANISOTROPY_LEVEL_COUNT VGFX3D_METAL_MAX_TEXTURE_ANISOTROPY
#define VGFX3D_METAL_MAX_TEXTURE2D_DIMENSION 16384

/// @brief Blend state required by a draw: opaque, standard alpha, or additive.
typedef enum {
    VGFX3D_METAL_BLEND_OPAQUE = 0,
    VGFX3D_METAL_BLEND_ALPHA = 1,
    VGFX3D_METAL_BLEND_ADDITIVE = 2,
} vgfx3d_metal_blend_mode_t;

/// @brief Render-target classification: swapchain, offscreen HDR scene, RTT, or overlay.
typedef enum {
    VGFX3D_METAL_TARGET_SWAPCHAIN = 0,
    VGFX3D_METAL_TARGET_SCENE = 1,
    VGFX3D_METAL_TARGET_RTT = 2,
    VGFX3D_METAL_TARGET_OVERLAY = 3,
} vgfx3d_metal_target_kind_t;

/// @brief Color format of a target: 8-bit UNORM (display) or 16-bit float (HDR scene).
typedef enum {
    VGFX3D_METAL_COLOR_FORMAT_UNORM8 = 0,
    VGFX3D_METAL_COLOR_FORMAT_HDR16F = 1,
} vgfx3d_metal_color_format_t;

/// @brief Whether a pass attaches only color or also the motion-vector target.
typedef enum {
    VGFX3D_METAL_MOTION_ATTACHMENTS_COLOR_ONLY = 0,
    VGFX3D_METAL_MOTION_ATTACHMENTS_COLOR_AND_MOTION = 1,
} vgfx3d_metal_motion_attachment_mode_t;

/// @brief Source for canvas readback: the presented backbuffer or the post-FX composite.
typedef enum {
    VGFX3D_METAL_READBACK_BACKBUFFER = 0,
    VGFX3D_METAL_READBACK_POSTFX_COMPOSITE = 1,
} vgfx3d_metal_readback_kind_t;

/// @brief Validate a 2D Metal allocation extent against the runtime portability contract.
/// @param width Requested texture width.
/// @param height Requested texture height.
/// @return Non-zero only for positive dimensions no larger than the portable Metal limit.
int vgfx3d_metal_is_valid_texture2d_extent(int32_t width, int32_t height);

/// @brief Per-frame view/projection history for motion vectors (current/previous/inverse
///   scene VP, draw's previous VP, camera position, and scene/overlay validity flags).
typedef struct {
    float scene_vp[16];
    float scene_prev_vp[16];
    float scene_inv_vp[16];
    float draw_prev_vp[16];
    float scene_cam_pos[3];
    int8_t scene_history_valid;
    int8_t overlay_used_this_frame;
} vgfx3d_metal_frame_history_t;

/// @brief One per-instance Metal buffer entry: model, normal, and previous-frame model
///   matrices (column-major-transposed for MSL by fill_instance_data).
typedef struct {
    float model[16];
    float normal[16];
    float prev_model[16];
} vgfx3d_metal_instance_data_t;

/// @brief Sanitize and transpose a bone palette for MSL (identity-pads unused bones).
/// @param[out] dst Caller-owned fixed-size palette receiving all Metal bone slots.
/// @param src Borrowed array of @p bone_count row-major matrices, or NULL.
/// @param bone_count Number of source matrices; values above the backend maximum are clamped.
void vgfx3d_metal_pack_bone_palette(float *dst, const float *src, int32_t bone_count);
/// @brief Build per-instance Metal buffer entries with column-major transpose for MSL.
/// @param[out] dst Caller-owned array receiving @p instance_count packed entries.
/// @param instance_count Number of instances to populate.
/// @param instance_matrices Borrowed array of current row-major 4x4 model matrices.
/// @param prev_instance_matrices Borrowed array of prior model matrices, or NULL.
/// @param has_prev_instance_matrices Nonzero when @p prev_instance_matrices is valid.
void vgfx3d_metal_fill_instance_data(vgfx3d_metal_instance_data_t *dst,
                                     int32_t instance_count,
                                     const float *instance_matrices,
                                     const float *prev_instance_matrices,
                                     int8_t has_prev_instance_matrices);
/// @brief Roll per-frame VP/inv-VP/cam-pos history forward by one frame.
/// @param[in,out] history Caller-owned temporal history to advance.
/// @param vp Borrowed current row-major view-projection matrix.
/// @param inv_vp Borrowed current row-major inverse view-projection matrix.
/// @param cam_pos Borrowed three-component camera position, or NULL to retain the prior value.
/// @param is_overlay_pass Nonzero when the update describes an overlay rather than the scene.
/// @param uses_separate_overlay_target Nonzero when overlay rendering uses its own attachment.
void vgfx3d_metal_update_frame_history(vgfx3d_metal_frame_history_t *history,
                                       const float *vp,
                                       const float *inv_vp,
                                       const float *cam_pos,
                                       int8_t is_overlay_pass,
                                       int8_t uses_separate_overlay_target);
/// @brief Number of mipmap levels needed to reach 1×1 from (width × height).
/// @param width Base-level width in pixels.
/// @param height Base-level height in pixels.
/// @return The complete mip-chain level count; invalid dimensions produce one level.
int32_t vgfx3d_metal_compute_mip_count(int32_t width, int32_t height);
/// @brief Clamp material sampler anisotropy into the backend cacheable [1,16] range.
/// @param requested Requested anisotropy level.
/// @return The requested value clamped to the Metal backend's supported cache range.
int32_t vgfx3d_metal_sanitize_anisotropy(int32_t requested);
/// @brief Convert sanitized anisotropy to a compact cache index [0,15].
/// @param requested Requested anisotropy level before sanitization.
/// @return Zero-based sampler-cache index corresponding to the clamped anisotropy.
int32_t vgfx3d_metal_sampler_anisotropy_index(int32_t requested);
/// @brief Capacity-doubling growth helper (saturates at INT_MAX).
/// @param current_capacity Existing element capacity.
/// @param needed Minimum capacity required by the caller.
/// @param minimum_capacity Initial capacity used when no positive capacity exists.
/// @return A positive doubled capacity covering @p needed, with overflow-safe saturation.
int32_t vgfx3d_metal_next_capacity(int32_t current_capacity,
                                   int32_t needed,
                                   int32_t minimum_capacity);
/// @brief Clamp morph shapes so shader-side int indexing cannot overflow.
/// @param vertex_count Number of vertices addressed by every morph shape.
/// @param requested_shape_count Requested number of morph shapes.
/// @return The usable shape count, or zero when inputs cannot be indexed safely.
int32_t vgfx3d_metal_clamp_morph_shape_count(uint32_t vertex_count, int32_t requested_shape_count);
/// @brief Pick the right render-target classification for the Metal backend.
/// @param rtt_active Nonzero when rendering into an explicit runtime render target.
/// @param gpu_postfx_enabled Nonzero when the window scene routes through GPU post-processing.
/// @param load_existing_color Nonzero when the pass must preserve previously rendered color.
/// @return The RTT, swapchain, overlay, or offscreen-scene target classification.
vgfx3d_metal_target_kind_t vgfx3d_metal_choose_target_kind(int8_t rtt_active,
                                                           int8_t gpu_postfx_enabled,
                                                           int8_t load_existing_color);
/// @brief Decide whether the next pass should preserve existing color contents.
/// @param target_kind Classification of the target being opened.
/// @param requested_load_existing_color Nonzero when the caller requests preserved color.
/// @param overlay_used_this_frame Nonzero when the overlay attachment already has valid content.
/// @return Nonzero when the render pass should load rather than clear its color attachment.
int8_t vgfx3d_metal_should_load_existing_color(vgfx3d_metal_target_kind_t target_kind,
                                               int8_t requested_load_existing_color,
                                               int8_t overlay_used_this_frame);
/// @brief Pick the color format — HDR16F for the scene pass, UNORM8 elsewhere.
/// @param target_kind Classification of the render target being configured.
/// @return HDR16F for the offscreen scene target, otherwise UNORM8.
vgfx3d_metal_color_format_t vgfx3d_metal_choose_color_format(
    vgfx3d_metal_target_kind_t target_kind);
/// @brief Map a draw command to its required blend state (alpha vs opaque).
/// @param cmd Borrowed draw command whose material blend flags are inspected; may be NULL.
/// @return Additive, alpha, or opaque Metal blend classification.
vgfx3d_metal_blend_mode_t vgfx3d_metal_choose_blend_mode(const vgfx3d_draw_cmd_t *cmd);
/// @brief Decide whether terrain splatting has every required texture bound.
/// @param cmd_has_splat Nonzero when the draw command requests terrain splatting.
/// @param has_splat_map Nonzero when the blend-weight texture is bound.
/// @param has_layer0 Nonzero when splat layer zero is bound.
/// @param has_layer1 Nonzero when splat layer one is bound.
/// @param has_layer2 Nonzero when splat layer two is bound.
/// @param has_layer3 Nonzero when splat layer three is bound.
/// @return Nonzero only when splatting is requested and every required texture is present.
int vgfx3d_metal_has_complete_splat(int8_t cmd_has_splat,
                                    int has_splat_map,
                                    int has_layer0,
                                    int has_layer1,
                                    int has_layer2,
                                    int has_layer3);
/// @brief Decide whether to attach a motion-vector buffer (only for opaque scene draws).
/// @param target_kind Classification of the active render target.
/// @param cmd Borrowed draw command whose depth and blend behavior are inspected.
/// @return Color-and-motion for eligible opaque scene draws, otherwise color-only.
vgfx3d_metal_motion_attachment_mode_t vgfx3d_metal_choose_motion_attachment_mode(
    vgfx3d_metal_target_kind_t target_kind, const vgfx3d_draw_cmd_t *cmd);
/// @brief Decide whether canvas readback should source the backbuffer or postfx target.
/// @param gpu_postfx_enabled Nonzero when the GPU post-processing route is active.
/// @return The post-FX composite source when enabled, otherwise the backbuffer source.
vgfx3d_metal_readback_kind_t vgfx3d_metal_choose_readback_kind(int8_t gpu_postfx_enabled);
/// @brief ADR 0301: decide whether a render-target frame is resolved through the post-FX
///   chain into its display image. Deliberately takes NO window-present-route flag:
///   `gpu_postfx_enabled` is never on during a render-target frame, and gating the
///   resolve on it was exactly the bug that left ADR 0299's display path unreachable.
/// @param chain_valid Nonzero when a usable chain snapshot is installed for this frame.
/// @param pipelines_ready Nonzero when both post-FX pipelines (LDR and HDR) compiled.
/// @param has_command_buffer Nonzero when the frame's command buffer is open.
/// @return 1 when the display resolve should run, otherwise 0.
int8_t vgfx3d_metal_should_resolve_render_target_display(int8_t chain_valid,
                                                         int8_t pipelines_ready,
                                                         int8_t has_command_buffer);
/// @brief Clamp light shadow indices to the currently completed contiguous shadow slots.
/// @param shadow_index Requested zero-based shadow slot.
/// @param shadow_count Number of complete contiguous slots available for sampling.
/// @return @p shadow_index when valid, otherwise -1 to disable shadow sampling.
int32_t vgfx3d_metal_sanitize_shadow_index(int32_t shadow_index, int32_t shadow_count);
/// @brief Project a world-space point through a shadow matrix to MSL UV/depth coordinates.
/// @param shadow_vp Borrowed row-major shadow view-projection matrix.
/// @param projection_type Orthographic, perspective, or cube shadow projection identifier.
/// @param world_pos Borrowed three-component world-space point.
/// @param[out] out_uv_depth Caller-owned UV-depth triplet, zeroed before validation.
/// @return 1 when projection yields finite texture coordinates, or 0 for unusable input.
int vgfx3d_metal_project_shadow_coord(const float *shadow_vp,
                                      int32_t projection_type,
                                      const float world_pos[3],
                                      float out_uv_depth[3]);
/// @brief Decide whether to reuse a cached morph-target Metal buffer (key + revision + counts
/// match).
/// @param cached_key Identity key recorded by the cached payload.
/// @param cached_identity Allocation generation recorded by the cached payload.
/// @param cached_revision Revision recorded by the cached payload.
/// @param cached_shape_count Sanitized shape count recorded by the cached payload.
/// @param cached_vertex_count Vertex count recorded by the cached payload.
/// @param cached_has_normal_deltas Nonzero when the cached payload includes normal deltas.
/// @param cmd Borrowed draw command requesting morph data.
/// @return 1 when every cache identity and layout field matches, otherwise 0.
int vgfx3d_metal_should_reuse_morph_cache(const void *cached_key,
                                          uint64_t cached_identity,
                                          uint64_t cached_revision,
                                          int32_t cached_shape_count,
                                          uint32_t cached_vertex_count,
                                          int8_t cached_has_normal_deltas,
                                          const vgfx3d_draw_cmd_t *cmd);
/// @brief Decide whether a per-mesh GPU cache entry should be evicted (unused for > max_age
/// frames).
/// @param current_frame Current monotonically increasing backend frame number.
/// @param last_used_frame Frame number on which the cache entry was last referenced.
/// @param max_age Maximum permitted number of unused frames; zero disables pruning.
/// @return Nonzero when the entry should be dropped.
int vgfx3d_metal_should_prune_cache_entry(uint64_t current_frame,
                                          uint64_t last_used_frame,
                                          uint64_t max_age);

#ifdef __cplusplus
}
#endif
