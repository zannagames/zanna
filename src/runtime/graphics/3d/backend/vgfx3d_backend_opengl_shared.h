//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_shared.h
// Purpose: Shared declarations/constants for the OpenGL vgfx3d backend —
//   blend-mode enums and shared state used by the OpenGL backend and its
//   shared support unit.
//
// Key invariants:
//   - Blend-mode/limit constants must stay consistent with the GLSL shaders
//     and the D3D11/Metal shared layouts.
//   - Internal to the OpenGL backend; not part of the public Zanna API.
//
// Ownership/Lifetime:
//   - Declarations only; no allocation or ownership semantics here.
//
// Links: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl.c,
//        src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_shared.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares context-independent layouts and policy helpers for the OpenGL 3D backend.
/// @details This internal C interface exposes bounded history, sizing, target-selection, shadow,
///          and cache helpers shared by the OpenGL implementation and its focused support unit.

#pragma once

#include "vgfx3d_backend.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VGFX3D_OPENGL_MAX_BONES 256
#define VGFX3D_OPENGL_BONE_PALETTE_FLOATS (VGFX3D_OPENGL_MAX_BONES * 16u)
#define VGFX3D_OPENGL_BONE_PALETTE_BYTES (sizeof(float) * VGFX3D_OPENGL_BONE_PALETTE_FLOATS)
#define VGFX3D_OPENGL_MAX_MORPH_SHAPES 64
#define VGFX3D_OPENGL_MAX_TEXTURE_ANISOTROPY 16
#define VGFX3D_OPENGL_ANISOTROPY_LEVEL_COUNT VGFX3D_OPENGL_MAX_TEXTURE_ANISOTROPY

/// @brief Blend state required by a draw: opaque, standard alpha, or additive.
typedef enum {
    VGFX3D_OPENGL_BLEND_OPAQUE = 0,
    VGFX3D_OPENGL_BLEND_ALPHA = 1,
    VGFX3D_OPENGL_BLEND_ADDITIVE = 2,
} vgfx3d_opengl_blend_mode_t;

/// @brief Render-target classification: swapchain (default FBO), offscreen HDR scene, or RTT.
typedef enum {
    VGFX3D_OPENGL_TARGET_SWAPCHAIN = 0,
    VGFX3D_OPENGL_TARGET_SCENE = 1,
    VGFX3D_OPENGL_TARGET_RTT = 2,
} vgfx3d_opengl_target_kind_t;

/// @brief Color format of a target: 8-bit UNORM (display) or 16-bit float (HDR scene).
typedef enum {
    VGFX3D_OPENGL_COLOR_FORMAT_UNORM8 = 0,
    VGFX3D_OPENGL_COLOR_FORMAT_HDR16F = 1,
} vgfx3d_opengl_color_format_t;

/// @brief Whether a pass attaches only color or also the motion-vector target.
typedef enum {
    VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_ONLY = 0,
    VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_AND_MOTION = 1,
} vgfx3d_opengl_motion_attachment_mode_t;

/// @brief Source for canvas readback: the swapchain or the post-FX composite.
typedef enum {
    VGFX3D_OPENGL_READBACK_BACKBUFFER = 0,
    VGFX3D_OPENGL_READBACK_POSTFX_COMPOSITE = 1,
} vgfx3d_opengl_readback_kind_t;

/// @brief Per-frame view/projection history for motion vectors (current/previous/inverse
///   scene VP, the draw's previous VP, camera position, and scene-history validity).
typedef struct {
    float scene_vp[16];
    float scene_prev_vp[16];
    float scene_inv_vp[16];
    float draw_prev_vp[16];
    float scene_cam_pos[3];
    int8_t scene_history_valid;
} vgfx3d_opengl_frame_history_t;

/// @brief Roll per-frame VP/inv-VP/cam-pos history forward by one frame.
/// @param[in,out] history Caller-owned temporal history to advance.
/// @param vp Borrowed current row-major view-projection matrix.
/// @param inv_vp Borrowed current inverse view-projection matrix.
/// @param cam_pos Borrowed camera position, or NULL to retain the prior scene value.
/// @param is_overlay_pass Nonzero when updating overlay rather than scene draw history.
void vgfx3d_opengl_update_frame_history(vgfx3d_opengl_frame_history_t *history,
                                        const float *vp,
                                        const float *inv_vp,
                                        const float *cam_pos,
                                        int8_t is_overlay_pass);
/// @brief Number of mipmap levels needed to reach 1×1 from (width × height).
/// @param width Base-level width in pixels.
/// @param height Base-level height in pixels.
/// @return Complete mip-chain length; invalid dimensions produce one.
int32_t vgfx3d_opengl_compute_mip_count(int32_t width, int32_t height);
/// @brief Clamp material sampler anisotropy into the backend cacheable [1,16] range.
/// @param requested Requested anisotropy value.
/// @return Value clamped to the supported cache range.
int32_t vgfx3d_opengl_sanitize_anisotropy(int32_t requested);
/// @brief Convert sanitized anisotropy to a compact cache index [0,15].
/// @param requested Requested anisotropy before sanitization.
/// @return Zero-based sampler-cache index.
int32_t vgfx3d_opengl_sampler_anisotropy_index(int32_t requested);
/// @brief Capacity-doubling growth helper (saturates at INT_MAX).
/// @param current_capacity Existing element capacity.
/// @param needed Minimum required capacity.
/// @param minimum_capacity Initial growth floor.
/// @return Positive doubled capacity covering @p needed.
int32_t vgfx3d_opengl_next_capacity(int32_t current_capacity,
                                    int32_t needed,
                                    int32_t minimum_capacity);
/// @brief Overflow-safe size_t capacity growth helper for GL buffer uploads.
/// @param current_capacity Existing byte capacity.
/// @param needed Minimum required byte capacity.
/// @param minimum_capacity Initial growth floor.
/// @param[out] out_capacity Required destination for the selected capacity.
/// @return 1 when a capacity is produced, otherwise 0.
int vgfx3d_opengl_compute_buffer_capacity(size_t current_capacity,
                                          size_t needed,
                                          size_t minimum_capacity,
                                          size_t *out_capacity);
/// @brief Validate an RGBA8 readback destination span.
/// @param width Destination width in pixels.
/// @param height Destination height in rows.
/// @param stride Destination row pitch in bytes.
/// @param[out] out_bytes Optional destination for the total span.
/// @return 1 when dimensions and byte arithmetic are valid, otherwise 0.
int vgfx3d_opengl_validate_rgba8_destination(int32_t width,
                                             int32_t height,
                                             int32_t stride,
                                             size_t *out_bytes);
/// @brief Clamp morph shapes so shader-side int indexing cannot overflow.
/// @param vertex_count Number of vertices addressed by each shape.
/// @param requested_shape_count Requested morph-shape count.
/// @return Usable shape count, or zero when invalid.
int32_t vgfx3d_opengl_clamp_morph_shape_count(uint32_t vertex_count, int32_t requested_shape_count);
/// @brief Validate an R32F texture-buffer payload against a GL texel limit.
/// @param payload_bytes Proposed payload size in bytes.
/// @param max_texture_buffer_texels Device texture-buffer limit.
/// @return Nonzero when the float payload fits the known limit.
int vgfx3d_opengl_texture_buffer_accepts_r32f_payload(size_t payload_bytes,
                                                      int32_t max_texture_buffer_texels);
/// @brief Pick the right render-target classification for the OpenGL backend.
/// @param rtt_active Nonzero when an explicit runtime target is active.
/// @param gpu_postfx_enabled Nonzero when the window uses the offscreen scene route.
/// @return RTT, scene, or swapchain target classification.
vgfx3d_opengl_target_kind_t vgfx3d_opengl_choose_target_kind(int8_t rtt_active,
                                                             int8_t gpu_postfx_enabled);
/// @brief Pick the color format — HDR16F for the scene pass, UNORM8 elsewhere.
/// @param target_kind Target classification being allocated.
/// @return HDR16F for scene targets, otherwise UNORM8.
vgfx3d_opengl_color_format_t vgfx3d_opengl_choose_color_format(
    vgfx3d_opengl_target_kind_t target_kind);
/// @brief Map a draw command to its required blend state (alpha vs opaque).
/// @param cmd Borrowed draw command; may be NULL.
/// @return Additive, alpha, or opaque blend classification.
vgfx3d_opengl_blend_mode_t vgfx3d_opengl_choose_blend_mode(const vgfx3d_draw_cmd_t *cmd);
/// @brief Decide whether terrain splatting has every required texture bound.
/// @param cmd_has_splat Nonzero when splatting is requested.
/// @param has_splat_map Nonzero when the control texture is bound.
/// @param has_layer0 Nonzero when layer zero is bound.
/// @param has_layer1 Nonzero when layer one is bound.
/// @param has_layer2 Nonzero when layer two is bound.
/// @param has_layer3 Nonzero when layer three is bound.
/// @return Nonzero only when every required binding is available.
int vgfx3d_opengl_has_complete_splat(int8_t cmd_has_splat,
                                     int has_splat_map,
                                     int has_layer0,
                                     int has_layer1,
                                     int has_layer2,
                                     int has_layer3);
/// @brief Decide whether to attach a motion-vector buffer (only for opaque scene draws).
/// @param target_kind Classification of the active target.
/// @param cmd Borrowed draw command whose depth/blend state is inspected.
/// @return Color-and-motion for eligible draws, otherwise color-only.
vgfx3d_opengl_motion_attachment_mode_t vgfx3d_opengl_choose_motion_attachment_mode(
    vgfx3d_opengl_target_kind_t target_kind, const vgfx3d_draw_cmd_t *cmd);
/// @brief Decide whether canvas readback should source the swapchain or postfx target.
/// @param gpu_postfx_enabled Nonzero when GPU post-processing is active.
/// @return Post-FX source when enabled, otherwise the backbuffer.
vgfx3d_opengl_readback_kind_t vgfx3d_opengl_choose_readback_kind(int8_t gpu_postfx_enabled);
/// @brief Clamp light shadow indices to the currently completed contiguous shadow slots.
/// @param shadow_index Requested zero-based slot.
/// @param shadow_count Number of complete contiguous slots.
/// @return @p shadow_index when valid, otherwise -1.
int32_t vgfx3d_opengl_sanitize_shadow_index(int32_t shadow_index, int32_t shadow_count);
/// @brief Project a world-space point through a shadow matrix to GLSL UV/depth coordinates.
/// @param shadow_vp Borrowed row-major shadow view-projection matrix.
/// @param projection_type Shadow projection identifier.
/// @param world_pos Borrowed three-component world position.
/// @param[out] out_uv_depth Required UV-depth destination.
/// @return 1 for a finite valid projection, otherwise 0.
int vgfx3d_opengl_project_shadow_coord(const float *shadow_vp,
                                       int32_t projection_type,
                                       const float world_pos[3],
                                       float out_uv_depth[3]);
/// @brief Decide whether to reuse a cached morph-target GPU buffer (key + revision + counts match).
/// @param cached_key Cached identity key.
/// @param cached_revision Cached revision.
/// @param cached_shape_count Cached sanitized shape count.
/// @param cached_vertex_count Cached vertex count.
/// @param cached_has_normal_deltas Nonzero when cached normal deltas exist.
/// @param cmd Borrowed draw command requesting morph data.
/// @return 1 when identity and layout match, otherwise 0.
int vgfx3d_opengl_should_reuse_morph_cache(const void *cached_key,
                                           uint64_t cached_revision,
                                           int32_t cached_shape_count,
                                           uint32_t cached_vertex_count,
                                           int8_t cached_has_normal_deltas,
                                           const vgfx3d_draw_cmd_t *cmd);
/// @brief Decide whether a per-mesh GPU cache entry should be evicted (unused for > max_age
/// frames).
/// @param current_frame Current frame number.
/// @param last_used_frame Entry's most recent use.
/// @param max_age Maximum unused-frame age; zero disables pruning.
/// @return Nonzero when the entry is old enough to evict.
int vgfx3d_opengl_should_prune_cache_entry(uint64_t current_frame,
                                           uint64_t last_used_frame,
                                           uint64_t max_age);

#ifdef __cplusplus
}
#endif
