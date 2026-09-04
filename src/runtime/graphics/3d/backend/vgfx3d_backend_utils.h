//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_utils.h
// Purpose: Backend-agnostic helper declarations shared by the vgfx3d render
//   backends — Pixels-to-RGBA8 decoding and other small format/utility
//   routines used when uploading CPU images to GPU textures or sizing
//   window-backed scene targets.
//
// Key invariants:
//   - Helpers are pure/stateless; outputs are caller-owned heap buffers.
//
// Ownership/Lifetime:
//   - Functions that return/allocate buffers transfer ownership to the
//     caller (caller frees); inputs are borrowed and not retained.
//
// Links: src/runtime/graphics/3d/backend/vgfx3d_backend_utils.c (implementation)
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_backend_utils.h
 * @brief Declares backend-neutral validation, texture upload, color conversion, matrix, and
 *        compact-vertex utilities shared by Graphics3D render backends.
 *
 * The helpers in this interface validate untrusted render snapshots before backend consumption,
 * expose checked views of Pixels, cubemap, and TextureAsset3D data, pace uploads against frame
 * budgets, and provide deterministic conversions used by every rendering implementation.
 * Unless a function explicitly allocates an output buffer, all inputs are borrowed for the
 * duration of the call and all outputs remain caller-owned.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Borrowed description of one native compressed TextureAsset3D mip.
 *
 * The payload remains owned by the source asset. Callers must consume it while the captured asset
 * residency window remains valid and must not free or modify @ref data.
 */
typedef struct {
    /// Pointer to the first byte of the compressed mip payload.
    const uint8_t *data;
    /// Available payload size in bytes.
    uint64_t bytes;
    /// Logical mip width in texels.
    int32_t width;
    /// Logical mip height in texels.
    int32_t height;
    /// Compression-block width in texels.
    int32_t block_width;
    /// Compression-block height in texels.
    int32_t block_height;
    /// Number of encoded bytes in one compression block.
    int32_t block_bytes;
    /// Runtime-native texture format identifier.
    int32_t format_id;
} vgfx3d_native_texture_mip_t;

/// Backend draw-command type defined by the shared renderer interface.
struct vgfx3d_draw_cmd;
/// Backend camera snapshot type defined by the shared renderer interface.
struct vgfx3d_camera_params;
/// Backend light snapshot type defined by the shared renderer interface.
struct vgfx3d_light_params;
/// Clustered-light lookup table defined by the shared renderer interface.
struct vgfx3d_cluster_table;
/// Post-processing chain type defined by the shared renderer interface.
struct vgfx3d_postfx_chain;
/// Flattened post-processing state defined by the shared renderer interface.
struct vgfx3d_postfx_snapshot;

/// Inclusive absolute component bound accepted for backend-facing matrices and vectors.
#define VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX 1000000000000.0f

/// @brief Return non-zero when every float is finite and within the absolute bound.
/// @param values Borrowed float array; may be null only when @p count is zero.
/// @param count Number of elements to inspect.
/// @param abs_max Finite, non-negative inclusive absolute bound.
/// @return Non-zero when all elements lie in `[-abs_max, abs_max]`, otherwise zero.
int vgfx3d_float_array_is_bounded(const float *values, size_t count, float abs_max);
/// @brief Copy finite floats, substituting a finite fallback for invalid lanes.
/// @param dst Caller-owned destination for @p count floats; null is a no-op.
/// @param src Optional borrowed source; null uses @p fallback for every element.
/// @param count Number of elements to copy.
/// @param fallback Replacement for non-finite elements; a non-finite fallback becomes zero.
void vgfx3d_copy_float_array_finite_or(float *dst, const float *src, size_t count, float fallback);
/// @brief Copy a bounded matrix, or write identity when the source is unusable.
/// @param dst Caller-owned destination for 16 row-major floats; null is a no-op.
/// @param src Optional borrowed matrix accepted only when all components are finite and bounded.
void vgfx3d_copy_mat4_finite_or_identity(float *dst, const float *src);
/// @brief Copy a bounded matrix, falling back to another bounded matrix or identity.
/// @param dst Caller-owned destination for 16 row-major floats; null is a no-op.
/// @param src Preferred optional borrowed matrix.
/// @param fallback Secondary optional borrowed matrix used when @p src is unusable.
void vgfx3d_copy_mat4_finite_or(float *dst, const float *src, const float *fallback);
/// @brief Validate a bounded shadow matrix with at least one useful component.
/// @param matrix Borrowed 16-element row-major matrix.
/// @return Non-zero when every component is bounded and at least one has meaningful magnitude.
int vgfx3d_shadow_matrix_is_usable(const float *matrix);
/// @brief Copy and normalize a direction, using the fallback/default when unusable.
/// @param dst Caller-owned three-component destination; null is a no-op.
/// @param src Preferred optional borrowed direction.
/// @param fallback Secondary optional direction; the built-in negative-Z direction is used if both
///                 candidates are unusable.
void vgfx3d_copy_vec3_direction_or(float *dst, const float *src, const float fallback[3]);
/// @brief Return the requested finite value, or a finite fallback (ultimately zero).
/// @param requested Preferred value.
/// @param fallback Secondary value used when @p requested is non-finite.
/// @return @p requested when finite, otherwise finite @p fallback, otherwise zero.
float vgfx3d_finite_or(float requested, float fallback);
/// @brief Clamp a finite value, substituting the fallback and tolerating inverted bounds.
/// @param requested Preferred value to constrain.
/// @param min_value Requested lower bound.
/// @param max_value Requested upper bound.
/// @param fallback Replacement for non-finite input or unusable bounds.
/// @return Finite value within the normalized inclusive bound pair.
float vgfx3d_clamp_float_param(float requested, float min_value, float max_value, float fallback);
/// @brief Normalize material workflow constants to legacy or PBR.
/// @param requested Caller-provided material workflow.
/// @return PBR for the exact PBR constant, otherwise the legacy workflow.
int32_t vgfx3d_sanitize_material_workflow(int32_t requested);
/// @brief Normalize alpha-mode constants to the public opaque/mask/blend range.
/// @param requested Caller-provided alpha mode.
/// @return Valid opaque, mask, or blend mode; malformed input becomes opaque.
int32_t vgfx3d_sanitize_alpha_mode(int32_t requested);
/// @brief Normalize Game3D shading-model constants to the shader-visible range.
/// @param requested Caller-provided shading model.
/// @return Value in the shader-visible range `[0, 5]`, defaulting to zero.
int32_t vgfx3d_sanitize_shading_model(int32_t requested);
/// @brief Normalize material shadow-mode constants to auto/none/cast.
/// @param requested Caller-provided material shadow mode.
/// @return Valid auto, none, or cast mode; malformed input becomes auto.
int32_t vgfx3d_sanitize_shadow_mode(int32_t requested);
/// @brief Normalize texture-wrap constants, defaulting malformed values to repeat.
/// @param requested Caller-provided texture addressing mode.
/// @return Valid repeat, clamp-to-edge, or mirrored-repeat mode.
int32_t vgfx3d_sanitize_texture_wrap(int32_t requested);
/// @brief Normalize texture-filter constants, defaulting malformed values to linear.
/// @param requested Caller-provided texture filter.
/// @return Nearest for the exact nearest constant, otherwise linear.
int32_t vgfx3d_sanitize_texture_filter(int32_t requested);
/// @brief Normalize texture mip-filter constants, defaulting malformed values to none.
/// @param requested Caller-provided mip filter.
/// @return Valid none, nearest, or linear mip filter.
int32_t vgfx3d_sanitize_texture_mip_filter(int32_t requested);
/// @brief Normalize a texture UV-set selector to the shader-visible uv0/uv1 range.
/// @param requested Caller-provided UV-set selector.
/// @return Zero for UV0 or one for any positive UV1 selector.
int32_t vgfx3d_sanitize_texture_uv_set(int32_t requested);
/// @brief Copy a draw command while normalizing backend-visible material state.
/// @param src Borrowed source command.
/// @param dst Caller-owned destination receiving normalized matrices, material values, sampler
///            state, counts, and flags. Null input pointers make the operation a no-op.
void vgfx3d_sanitize_draw_command(const struct vgfx3d_draw_cmd *src, struct vgfx3d_draw_cmd *dst);
/// @brief Compare a retained backend cache identity without trusting an address alone.
/// @param cached_key Address key stored by the cache entry.
/// @param cached_identity Allocation generation stored by the cache entry.
/// @param requested_key Address key supplied by the current draw.
/// @param requested_identity Allocation generation supplied by the current draw.
/// @return One only when both nonzero identity components match exactly.
int8_t vgfx3d_cache_identity_matches(const void *cached_key,
                                     uint64_t cached_identity,
                                     const void *requested_key,
                                     uint64_t requested_identity);
/// @brief Copy per-frame camera state while normalizing every backend-visible value.
/// @param src Borrowed camera snapshot.
/// @param dst Caller-owned destination receiving normalized matrices, clip planes, fog, IBL,
///            shadow, and boolean state. Null input pointers make the operation a no-op.
void vgfx3d_sanitize_camera_params(const struct vgfx3d_camera_params *src,
                                   struct vgfx3d_camera_params *dst);
/// @brief Copy one light while normalizing all shader-visible scalar/vector state.
/// @param src Borrowed light snapshot.
/// @param dst Caller-owned destination receiving normalized type, directions, shadow metadata,
///            color, attenuation, shape, and range. Null input pointers are a no-op.
void vgfx3d_sanitize_light_params(const struct vgfx3d_light_params *src,
                                  struct vgfx3d_light_params *dst);
/// @brief Copy a bounded light array and return the number of initialized outputs.
/// @param src Borrowed source-light array.
/// @param count Caller-provided source count.
/// @param dst Caller-owned destination array.
/// @param dst_capacity Number of entries available in @p dst.
/// @return Number of sanitized entries, bounded by @p count, @p dst_capacity, and the renderer
///         light limit.
int32_t vgfx3d_sanitize_light_array(const struct vgfx3d_light_params *src,
                                    int32_t count,
                                    struct vgfx3d_light_params *dst,
                                    int32_t dst_capacity);
/// @brief Clamp one light's shadow base/span to the contiguous advertised slot range.
/// @param light Mutable sanitized light whose base slot and cascade or cubemap span are adjusted.
/// @param shadow_count Number of contiguous shadow slots advertised by the backend.
void vgfx3d_sanitize_light_shadow_span(struct vgfx3d_light_params *light, int32_t shadow_count);
/// @brief Copy non-negative bounded ambient RGB, treating a NULL source as black.
/// @param src Optional borrowed RGB triplet.
/// @param dst Caller-owned three-component destination; null is a no-op.
void vgfx3d_sanitize_ambient_rgb(const float *src, float dst[3]);
/// @brief Validate a clustered-light table before any fixed-size GPU upload/indexing.
/// @param table Borrowed clustered-light table.
/// @param expected_revision Non-zero light revision the table must match.
/// @param light_count Number of sanitized lights addressable by the table.
/// @return Non-zero when counts, depth range, offsets, and every indexed light are valid.
int vgfx3d_cluster_table_is_usable(const struct vgfx3d_cluster_table *table,
                                   uint32_t expected_revision,
                                   int32_t light_count);
/// @brief Validate an enabled post-FX chain before indexed effect traversal.
/// @param chain Borrowed post-processing chain.
/// @return Non-zero when the chain is enabled and its storage, count, capacity, and effect types
///         are valid.
int vgfx3d_postfx_chain_is_usable(const struct vgfx3d_postfx_chain *chain);
/// @brief Copy one post-FX snapshot while clamping shader loops and numeric parameters.
/// @param src Optional borrowed snapshot; null produces an all-disabled result.
/// @param dst Caller-owned normalized snapshot; null is a no-op.
void vgfx3d_sanitize_postfx_snapshot(const struct vgfx3d_postfx_snapshot *src,
                                     struct vgfx3d_postfx_snapshot *dst);
/// @brief Convert reversed-Z storage to canonical depth, returning -1 for invalid samples.
/// @param reversed_depth Reversed-Z value where near is one and far is zero.
/// @return Canonical depth in `[0, 1]`, or -1 for non-finite input.
float vgfx3d_sanitize_reversed_depth_probe_result(float reversed_depth);

/// @brief Compute the normative scene-target extent for a logical output and render scale.
/// @details Each result is `floor(output_dimension * scale)`, clamped to one pixel. A scale of
///          exactly 1 preserves the input extent. The accepted scale range is the closed interval
///          `[0.25, 1]`; accepting only that range keeps every backend's direct hook behavior
///          consistent with `Canvas3D.TrySetRenderScale`.
/// @param output_width Logical presentation width in pixels; must be positive.
/// @param output_height Logical presentation height in pixels; must be positive.
/// @param scale Finite scene scale in the closed interval `[0.25, 1]`.
/// @param out_scene_width Receives the scaled scene width; cleared before validation.
/// @param out_scene_height Receives the scaled scene height; cleared before validation.
/// @return 1 when both outputs contain a valid extent, otherwise 0. Any non-null output pointer is
///         cleared on failure.
int vgfx3d_compute_scaled_scene_extent(int32_t output_width,
                                       int32_t output_height,
                                       float scale,
                                       int32_t *out_scene_width,
                                       int32_t *out_scene_height);

/// @brief Compute a valid square texture mip extent from a positive base extent.
/// @param base_extent Positive base-level width and height.
/// @param mip_level Non-negative mip index.
/// @param out_extent Receives the level extent and is cleared before validation.
/// @return 1 when @p mip_level exists in the pyramid, otherwise 0.
int vgfx3d_expected_square_mip_extent(int32_t base_extent, int32_t mip_level, int32_t *out_extent);
/// @brief Validate a prefiltered IBL chain against a cubemap's complete mip pyramid.
/// @param face_size Positive base cubemap face extent.
/// @param ibl_base_size Positive extent of the first supplied IBL mip.
/// @param ibl_mip_count Number of consecutive supplied IBL mips.
/// @param max_ibl_mips Capacity of the caller's IBL mip storage.
/// @param out_level_base Receives the cubemap level corresponding to @p ibl_base_size.
/// @return 1 when the IBL chain exactly fits a suffix of the complete cubemap pyramid, otherwise 0.
int vgfx3d_validate_cubemap_ibl_layout(int32_t face_size,
                                       int32_t ibl_base_size,
                                       int32_t ibl_mip_count,
                                       int32_t max_ibl_mips,
                                       int32_t *out_level_base);
/// @brief Check whether a whole-resource upload fits the remaining frame budget.
/// @param budget Maximum upload bytes for the frame, or @c UINT64_MAX for unlimited.
/// @param used Bytes already consumed from the frame budget.
/// @param requested Bytes required by the indivisible upload.
/// @return Non-zero when the request is empty, unlimited, or fits the remaining budget.
int vgfx3d_upload_budget_allows(uint64_t budget, uint64_t used, uint64_t requested);

/// @brief Decode a Pixels object into a freshly malloc'd RGBA8 byte array (caller frees).
/// @param pixels_ptr Borrowed opaque Pixels object storing packed @c 0xRRGGBBAA values.
/// @param out_w Receives decoded width on success.
/// @param out_h Receives decoded height on success.
/// @param out_rgba Receives newly allocated, tightly packed RGBA8 storage on success.
/// @return 0 on success, otherwise -1 without publishing outputs.
int vgfx3d_unpack_pixels_rgba(const void *pixels_ptr,
                              int32_t *out_w,
                              int32_t *out_h,
                              uint8_t **out_rgba);
/// @brief Return the valid extent of a Pixels object without allocating.
/// @param pixels_ptr Borrowed opaque Pixels object.
/// @param out_w Receives the width and is cleared before validation.
/// @param out_h Receives the height and is cleared before validation.
/// @return 1 for a valid non-empty surface, otherwise 0 with both outputs cleared.
int vgfx3d_get_pixels_extent(const void *pixels_ptr, int32_t *out_w, int32_t *out_h);
/// @brief Decode a row slice from a Pixels object into a freshly malloc'd RGBA8 array.
/// @details The requested row count is clamped at the image boundary. When @p flip_y is non-zero,
///          the band is read through a vertically reversed source mapping.
/// @param pixels_ptr Borrowed opaque Pixels object storing packed @c 0xRRGGBBAA values.
/// @param start_row Zero-based first destination-band row.
/// @param row_count Positive requested row count.
/// @param flip_y Non-zero to reverse the source row mapping.
/// @param out_w Receives decoded row width and is cleared before validation.
/// @param out_rows Receives the actual decoded row count and is cleared before validation.
/// @param out_rgba Receives newly allocated, tightly packed RGBA8 storage and is cleared before
///                 validation. The caller must free a successful result.
/// @return 0 on success, otherwise -1 for invalid input or allocation failure.
int vgfx3d_unpack_pixels_rgba_rows(const void *pixels_ptr,
                                   int32_t start_row,
                                   int32_t row_count,
                                   int flip_y,
                                   int32_t *out_w,
                                   int32_t *out_rows,
                                   uint8_t **out_rgba);
/// @brief Decode a row slice into caller-owned reusable RGBA8 storage without allocating.
/// @param pixels_ptr Borrowed opaque Pixels object.
/// @param start_row Zero-based first destination-band row.
/// @param row_count Positive requested row count, clamped to the image.
/// @param flip_y Non-zero to reverse the source row mapping.
/// @param rgba Caller-owned destination storage.
/// @param rgba_capacity Number of writable bytes at @p rgba.
/// @param out_w Receives decoded width and is cleared before validation.
/// @param out_rows Receives decoded row count and is cleared before validation.
/// @return 0 on success, otherwise -1 for invalid input or insufficient destination capacity.
int vgfx3d_unpack_pixels_rgba_rows_into(const void *pixels_ptr,
                                        int32_t start_row,
                                        int32_t row_count,
                                        int flip_y,
                                        uint8_t *rgba,
                                        size_t rgba_capacity,
                                        int32_t *out_w,
                                        int32_t *out_rows);
/// @brief Grow reusable byte storage geometrically to hold at least @p required_bytes.
/// @param storage Address of caller-owned storage, updated only after successful allocation.
/// @param capacity Address of the current byte capacity, updated on success.
/// @param required_bytes Minimum non-zero capacity requested by the caller.
/// @return The retained or grown storage, or NULL on invalid arguments/allocation failure.
uint8_t *vgfx3d_ensure_byte_scratch(uint8_t **storage, size_t *capacity, size_t required_bytes);
/// @brief Compute the RGBA8 byte count uploaded for one Pixels texture.
/// @param pixels_ptr Borrowed opaque Pixels object.
/// @param out_bytes Receives the tightly packed RGBA8 byte count and is cleared before validation.
/// @return 1 when the surface is valid and its byte count is representable, otherwise 0.
int vgfx3d_estimate_pixels_rgba_upload_bytes(const void *pixels_ptr, uint64_t *out_bytes);
/// @brief Return how many texture rows may be uploaded under a per-frame byte budget.
/// @details A finite non-zero budget always admits at least one row so an active upload progresses.
/// @param width Positive texture width in pixels.
/// @param height Positive texture height in pixels.
/// @param next_row First source row not yet uploaded.
/// @param budget Per-frame byte budget, or @c UINT64_MAX for unlimited.
/// @param used Bytes already consumed during the frame.
/// @return Number of consecutive rows to upload next, or zero for an invalid or completed cursor.
int32_t vgfx3d_upload_rows_for_budget(
    int32_t width, int32_t height, int32_t next_row, uint64_t budget, uint64_t used);
/// @brief Upper bound in bytes for RGBA8 2D textures that bypass a finite per-frame upload budget.
/// @details Text rasters and HUD sprites are tiny and latency-critical: deferring one a frame
///          leaves its quad untextured, which reads as a white block. Anything at or under this
///          size uploads in full the frame it is first seen; the bytes still count against the
///          budget so larger streams observe the consumption.
#define VGFX3D_TEXTURE_UPLOAD_BUDGET_EXEMPT_BYTES (256u * 1024u)
/// @brief Whether an RGBA8 2D texture is small enough to bypass a finite upload budget.
/// @param width Texture width in pixels.
/// @param height Texture height in pixels.
/// @param budget Per-frame byte budget; unlimited and zero budgets never exempt.
/// @return 1 when the whole texture uploads regardless of bytes already consumed, otherwise 0.
int vgfx3d_texture_upload_exempt_from_budget(int32_t width, int32_t height, uint64_t budget);
/// @brief Row budget for a 2D Pixels upload honouring the small-texture exemption.
/// @details Same contract as vgfx3d_upload_rows_for_budget except that a texture admitted by
///          vgfx3d_texture_upload_exempt_from_budget receives every remaining row.
int32_t vgfx3d_upload_rows_for_budget_2d(
    int32_t width, int32_t height, int32_t next_row, uint64_t budget, uint64_t used);
/// @brief Compute remaining RGBA8 row bytes for one in-progress 2D texture upload.
/// @param width Positive texture width in pixels.
/// @param height Positive texture height in pixels.
/// @param next_row First row not yet uploaded.
/// @param upload_in_progress Non-zero only while @p next_row is meaningful.
/// @return Remaining tightly packed RGBA8 bytes, saturated at @c UINT64_MAX.
uint64_t vgfx3d_pending_rgba_upload_bytes(int32_t width,
                                          int32_t height,
                                          int32_t next_row,
                                          int upload_in_progress);
/// @brief Compute remaining RGBA8 row bytes for one in-progress six-face cubemap upload.
/// @param face_size Positive square face extent.
/// @param upload_face Current face index in `[0, 5]`.
/// @param upload_next_row First row not yet uploaded in the current face.
/// @param upload_in_progress Non-zero only while the face and row cursor is meaningful.
/// @return Remaining tightly packed RGBA8 bytes across all pending faces, saturated at
///         @c UINT64_MAX.
uint64_t vgfx3d_pending_cubemap_rgba_upload_bytes(int32_t face_size,
                                                  int32_t upload_face,
                                                  int32_t upload_next_row,
                                                  int upload_in_progress);
/// @brief Return how many compressed block rows may upload under a per-frame byte budget.
/// @param width Positive texture width in texels.
/// @param height Positive texture height in texels.
/// @param block_width Compression-block width in texels.
/// @param block_height Compression-block height in texels.
/// @param block_bytes Encoded bytes per compression block.
/// @param next_block_row First compressed block row not yet uploaded.
/// @param budget Per-frame byte budget, or @c UINT64_MAX for unlimited.
/// @param used Bytes already consumed during the frame.
/// @return Number of consecutive block rows to upload next, or zero for invalid/completed input.
int32_t vgfx3d_upload_block_rows_for_budget(int32_t width,
                                            int32_t height,
                                            int32_t block_width,
                                            int32_t block_height,
                                            int32_t block_bytes,
                                            int32_t next_block_row,
                                            uint64_t budget,
                                            uint64_t used);
/// @brief Compute remaining compressed block-row bytes for one in-progress texture upload.
/// @param width Positive texture width in texels.
/// @param height Positive texture height in texels.
/// @param block_width Compression-block width in texels.
/// @param block_height Compression-block height in texels.
/// @param block_bytes Encoded bytes per compression block.
/// @param next_block_row First compressed block row not yet uploaded.
/// @param upload_in_progress Non-zero only while @p next_block_row is meaningful.
/// @return Remaining compressed bytes, saturated at @c UINT64_MAX.
uint64_t vgfx3d_pending_block_upload_bytes(int32_t width,
                                           int32_t height,
                                           int32_t block_width,
                                           int32_t block_height,
                                           int32_t block_bytes,
                                           int32_t next_block_row,
                                           int upload_in_progress);
/// @brief Read the Pixels generation counter (used to detect when a GPU upload is required).
/// @param pixels_ptr Borrowed opaque Pixels object.
/// @return Current content generation, or zero for a null object.
uint64_t vgfx3d_get_pixels_generation(const void *pixels_ptr);
/// @brief Stable cache signature for a Pixels object (identity + generation).
/// @param pixels_ptr Borrowed opaque Pixels object.
/// @return Non-zero identity/content signature for a populated object, or zero for null.
uint64_t vgfx3d_get_pixels_cache_key(const void *pixels_ptr);
/// @brief True when a TextureAsset3D can use native compressed upload under @p native_caps.
/// @param asset Borrowed TextureAsset3D object.
/// @param native_caps Backend capability bitset.
/// @return Non-zero when the asset has resident native content whose exact format bit is present.
int vgfx3d_textureasset_native_supported(void *asset, int64_t native_caps);
/// @brief Borrow one resident native mip from a TextureAsset3D by relative mip index.
/// @param asset Borrowed TextureAsset3D object.
/// @param relative_mip Zero-based index within the current resident mip window.
/// @param out_mip Receives borrowed payload and format metadata and is cleared before validation.
/// @return 1 with @p out_mip populated, otherwise 0 for an invalid or incomplete mip.
int vgfx3d_textureasset_get_native_resident_mip(void *asset,
                                                int64_t relative_mip,
                                                vgfx3d_native_texture_mip_t *out_mip);
/// @brief Borrow one native mip from a draw-time TextureAsset3D resident-window snapshot.
/// @details @p first_mip and @p mip_count are captured when the draw command is queued, so a
///          later TextureAsset3D residency change cannot alter which native payload a deferred
///          draw uploads. This helper does not mutate the asset or its current resident window.
/// @param asset Borrowed TextureAsset3D object.
/// @param first_mip Absolute first mip captured by the draw command.
/// @param mip_count Number of consecutive captured resident mips.
/// @param relative_mip Zero-based offset within the captured window.
/// @param out_mip Receives borrowed payload and format metadata and is cleared before validation.
/// @return 1 when the requested snapshot mip exists with valid native metadata, otherwise 0.
int vgfx3d_textureasset_get_native_snapshot_mip(void *asset,
                                                int64_t first_mip,
                                                int64_t mip_count,
                                                int64_t relative_mip,
                                                vgfx3d_native_texture_mip_t *out_mip);
/// @brief True when a native mip declares the canonical block footprint for its format.
/// @param mip Borrowed native mip descriptor containing format and block metadata.
/// @return Non-zero when the format is supported and its block dimensions and byte size are
///         canonical.
int vgfx3d_native_texture_block_layout_is_valid(const vgfx3d_native_texture_mip_t *mip);
/// @brief Validate one native compressed mip against its captured chain and block layout.
/// @details Validation includes expected mip dimensions, canonical block geometry, exact payload
///          length, consistent format identity, and the optional backend payload ceiling.
/// @param mip Borrowed native mip descriptor.
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
                                       uint64_t *out_required_bytes);
/// @brief Sum native mip payload bytes still pending from @p next_relative_mip/block-row cursor.
/// @param asset Borrowed TextureAsset3D object whose current resident window is measured.
/// @param next_relative_mip Zero-based current mip within the resident window.
/// @param next_block_row First compressed block row not yet uploaded in the current mip.
/// @param upload_in_progress Non-zero only while the upload cursor is meaningful.
/// @return Remaining native payload bytes, saturated at @c UINT64_MAX.
uint64_t vgfx3d_textureasset_pending_native_bytes(void *asset,
                                                  int64_t next_relative_mip,
                                                  int32_t next_block_row,
                                                  int upload_in_progress);
/// @brief Sum pending bytes for a draw-time TextureAsset3D resident-window snapshot.
/// @param asset Borrowed TextureAsset3D object.
/// @param first_mip Absolute first mip captured by the draw command.
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
                                                           int upload_in_progress);
/// @brief Decode all six cubemap faces into separate RGBA8 byte arrays (caller frees each).
/// @details Every face must be present, square, and identically sized. Partial allocations are
///          freed automatically on failure.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @param out_face_size Receives the common square extent and is cleared before validation.
/// @param out_faces Receives six newly allocated, tightly packed RGBA8 buffers; every entry is
///                  cleared before validation.
/// @return 0 on success, otherwise -1 after freeing any partial outputs.
int vgfx3d_unpack_cubemap_faces_rgba(const void *cubemap_ptr,
                                     int32_t *out_face_size,
                                     uint8_t *out_faces[6]);
/// @brief Return the valid square face size for a cubemap without allocating.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @param out_face_size Receives the common extent and is cleared before validation.
/// @return 1 when all six faces are present, square, and identically sized, otherwise 0.
int vgfx3d_get_cubemap_face_size(const void *cubemap_ptr, int32_t *out_face_size);
/// @brief Decode a cubemap face row slice into a freshly malloc'd RGBA8 array.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @param face_index Face index in `[0, 5]`.
/// @param start_row Zero-based first destination-band row.
/// @param row_count Positive requested row count, clamped at the face boundary.
/// @param flip_y Non-zero to reverse the source row mapping.
/// @param out_face_size Receives the common face extent and is cleared before validation.
/// @param out_rows Receives the actual decoded row count and is cleared before validation.
/// @param out_rgba Receives newly allocated, tightly packed RGBA8 storage and is cleared before
///                 validation. The caller must free a successful result.
/// @return 0 on success, otherwise -1 for invalid input or allocation failure.
int vgfx3d_unpack_cubemap_rgba_rows(const void *cubemap_ptr,
                                    int32_t face_index,
                                    int32_t start_row,
                                    int32_t row_count,
                                    int flip_y,
                                    int32_t *out_face_size,
                                    int32_t *out_rows,
                                    uint8_t **out_rgba);
/// @brief Compute the RGBA8 byte count uploaded for one six-face cubemap.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @param out_bytes Receives the total tightly packed bytes and is cleared before validation.
/// @return 1 when every face is valid and the total is representable, otherwise 0.
int vgfx3d_estimate_cubemap_rgba_upload_bytes(const void *cubemap_ptr, uint64_t *out_bytes);
/// @brief Combined generation hash across all six cubemap faces.
/// @param cubemap_ptr Borrowed opaque cubemap object.
/// @return Non-zero identity/content signature, or zero for an incomplete cubemap.
uint64_t vgfx3d_get_cubemap_generation(const void *cubemap_ptr);
/// @brief Flip an RGBA8 image vertically in place (top<->bottom row swap, OpenGL Y-flip).
/// @param rgba Mutable, tightly packed RGBA8 storage.
/// @param w Positive image width in pixels.
/// @param h Image height in pixels; values below two are a no-op.
void vgfx3d_flip_rgba_rows(uint8_t *rgba, int32_t w, int32_t h);
/// @brief Convert IEEE-754 binary16 to float32.
/// @param bits Raw IEEE-754 binary16 bit pattern.
/// @return Numerically equivalent binary32 value, preserving signed zero, infinity, and NaN class.
float vgfx3d_half_to_float(uint16_t bits);
/// @brief Clamp a linear float to [0,1] and quantize to 8-bit UNORM.
/// @param value Linear normalized value; non-positive and NaN input maps to zero.
/// @return Nearest eight-bit unsigned normalized representation.
uint8_t vgfx3d_float_to_unorm8(float value);
/// @brief Tonemap an HDR linear float with Reinhard and quantize to 8-bit UNORM.
/// @param value Linear HDR channel value. Non-positive and NaN input maps to zero; positive
///              infinity maps to 255.
/// @return Tonemapped eight-bit unsigned normalized representation.
uint8_t vgfx3d_hdr_to_unorm8(float value);
/// @brief Convert linear RGBA16F pixels to displayable RGBA8 (RGB tonemapped, alpha clamped).
/// @param dst_rgba Caller-owned RGBA8 destination buffer.
/// @param dst_stride Distance between destination rows in bytes.
/// @param copy_w Number of pixels converted per row.
/// @param copy_h Number of rows converted.
/// @param src_rgba16f Borrowed source containing IEEE-754 binary16 RGBA pixels.
/// @param src_stride_bytes Distance between source rows in bytes.
void vgfx3d_copy_linear_rgba16f_to_rgba8(uint8_t *dst_rgba,
                                         int32_t dst_stride,
                                         int32_t copy_w,
                                         int32_t copy_h,
                                         const uint16_t *src_rgba16f,
                                         int32_t src_stride_bytes);
/// @brief Convert linear RGBA16F pixels to linear RGBA32F.
/// @param dst_rgba32f Caller-owned RGBA32F destination buffer.
/// @param dst_stride_floats Distance between destination rows in float elements.
/// @param copy_w Number of pixels converted per row.
/// @param copy_h Number of rows converted.
/// @param src_rgba16f Borrowed source containing IEEE-754 binary16 RGBA pixels.
/// @param src_stride_bytes Distance between source rows in bytes.
void vgfx3d_copy_linear_rgba16f_to_rgba32f(float *dst_rgba32f,
                                           int32_t dst_stride_floats,
                                           int32_t copy_w,
                                           int32_t copy_h,
                                           const uint16_t *src_rgba16f,
                                           int32_t src_stride_bytes);
/// @brief Convert linear RGBA32F pixels to displayable RGBA8 (RGB tonemapped, alpha clamped).
/// @param dst_rgba Caller-owned RGBA8 destination buffer.
/// @param dst_stride Distance between destination rows in bytes.
/// @param copy_w Number of pixels converted per row.
/// @param copy_h Number of rows converted.
/// @param src_rgba32f Borrowed source containing linear float RGBA pixels.
/// @param src_stride_bytes Distance between source rows in bytes.
void vgfx3d_copy_linear_rgba32f_to_rgba8(uint8_t *dst_rgba,
                                         int32_t dst_stride,
                                         int32_t copy_w,
                                         int32_t copy_h,
                                         const float *src_rgba32f,
                                         int32_t src_stride_bytes);
/// @brief Compute the normal matrix (inverse-transpose of model 3×3) into a 4×4 output.
/// @param model_matrix Borrowed 16-element row-major model matrix.
/// @param out_matrix Caller-owned 16-element destination. Singular or non-finite input produces
///                   identity; null input pointers make the operation a no-op.
void vgfx3d_compute_normal_matrix4(const float *model_matrix, float *out_matrix);
/// @brief Invert a row-major 4×4 matrix using cofactor expansion.
/// @param matrix Borrowed 16-element source matrix.
/// @param out_matrix Caller-owned 16-element destination, left unchanged on failure.
/// @return 0 on success, or -1 for null pointers, a singular matrix, or a non-finite result.
int vgfx3d_invert_matrix4(const float *matrix, float *out_matrix);

#ifdef __cplusplus
}
#endif
