//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend.h
// Purpose: Backend abstraction vtable for Zanna.Graphics3D. All rendering
//   backends (software, Metal, D3D11, OpenGL) implement this interface.
//   Canvas3D dispatches through the vtable; backend selection is automatic.
//
// Key invariants:
//   - The software backend is always available as a fallback.
//   - GPU backends return NULL from create_ctx() on failure → fallback to software.
//   - Capability flags and optional hook pointers agree; unsupported hooks may be NULL.
//   - Compact particle instances are four contiguous float4 lanes on every GPU backend.
//
// Ownership/Lifetime:
//   - ctx is opaque backend-owned state created by create_ctx and released by destroy_ctx.
//   - Draw commands and pointed payloads remain Canvas3D-owned for the deferred frame lifetime.
//
// Links: plans/3d/05-backend-abstraction.md, rt_canvas3d_internal.h,
//   docs/adr/0173-graphics3d-transactional-hardening-and-retained-work.md
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares the platform-neutral Graphics3D backend contract.
///
/// Canvas3D builds backend-independent camera, light, draw, and clustered-light
/// payloads defined here, then dispatches rendering through
/// vgfx3d_backend_t. The same contract covers the deterministic software
/// implementation and the Metal, D3D11, and OpenGL adapters, with nullable
/// hooks explicitly representing optional GPU capabilities.
///
/// Backend contexts and resources are owned by their implementing vtable.
/// Submission payloads and all pointed-to frame data remain owned by Canvas3D
/// for the deferred frame lifetime.
#pragma once

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_platform.h"
#include "rt_postfx3d.h"
#include "vgfx.h"
#include <limits.h>
#include <math.h>
#include <stdint.h>

/*==========================================================================
 * Draw command — submitted between begin_frame/end_frame
 *=========================================================================*/

/// @brief One renderable submission: geometry (with an optional stable cache key),
///   model/previous-model matrices, the full resolved material (colors, PBR factors,
///   texture slots + samplers, alpha/cull state), and optional GPU skinning, morph,
///   instancing, and terrain-splat payloads. Built by Canvas3D, consumed by submit_draw.
typedef struct vgfx3d_draw_cmd {
    /// Full-precision vertex stream owned by the submitting frame.
    const vgfx3d_vertex_t *vertices;
    /// Number of entries in @ref vertices.
    uint32_t vertex_count;
    /// Triangle-list indices into @ref vertices.
    const uint32_t *indices;
    /// Number of entries in @ref indices.
    uint32_t index_count;
    /// Cached validated count equal to @ref index_count when Canvas3D already checked every index.
    uint32_t validated_index_count; /* non-zero when Canvas3D has already range-validated indices */
    /* Stable identity for backend-side static geometry caches. NULL means the
     * geometry is transient and should use the streaming upload path. */
    /// Stable geometry identity used for backend static-mesh caching, or NULL for transient data.
    const void *geometry_key;
    /// Allocation generation paired with @ref geometry_key; zero disables retained caching.
    uint64_t geometry_identity;
    /// Revision invalidating cached buffers associated with @ref geometry_key.
    uint32_t geometry_revision;
    /* R20: upload this draw's cached static geometry in the compact 48-byte
     * vertex encoding (only meaningful when geometry_key is non-NULL; the
     * software backend and transient uploads ignore it). */
    /// Nonzero to upload cached geometry using VGFX3D_COMPACT_VERTEX_STRIDE encoding.
    int8_t compact_vertex_stream;
    /// Current row-major model-to-world transform.
    float model_matrix[16]; /* row-major float */
    /// Previous-frame row-major model-to-world transform for motion vectors.
    float prev_model_matrix[16]; /* previous-frame row-major float */
    /// Resolved RGBA material multiplier.
    float diffuse_color[4]; /* RGBA material color */
    /// Resolved RGB specular multiplier.
    float specular[3]; /* RGB specular color */
    /// Blinn-Phong specular exponent.
    float shininess; /* specular exponent */
    /// Scalar opacity in the inclusive range 0-1.
    float alpha; /* opacity [0.0=invisible, 1.0=opaque] */
    /// Nonzero to bypass all lighting calculations.
    int8_t unlit; /* skip lighting if true */
    /// Nonzero for overlays that bypass depth testing and writing.
    int8_t disable_depth_test; /* screen-space overlays bypass depth test/write */
    /// Nonzero when the draw is meaningless without its base-colour texture (screen text and
    /// image quads): backends skip the draw while that texture is still uploading instead of
    /// painting the untextured material colour as a solid block.
    int8_t texture_required; /* skip the draw while the base-colour texture is not resident */
    /// Borrowed diffuse Pixels fallback for texture slot zero.
    const void *texture; /* Pixels fallback (diffuse, slot 0) or NULL */
    /// Borrowed live render target sampled natively as the base-colour texture (ADR 0299).
    /// Set only when the backend advertises RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET_SAMPLING;
    /// `texture` is then NULL and no CPU mirror readback happens for this draw.
    const vgfx3d_rendertarget_t *texture_target;
    /// Borrowed RenderTarget3D wrapper owning `texture_target` (retained by the material for
    /// the frame). A backend with no native storage for the target — it was rendered by another
    /// canvas or backend — resolves the wrapper's Pixels mirror through
    /// `rt_rendertarget3d_material_pixels` instead.
    void *texture_target_owner;
    /// Borrowed normal-map Pixels fallback for texture slot one.
    const void *normal_map; /* Pixels fallback (normal map, slot 1) or NULL */
    /// Borrowed specular-map Pixels fallback for texture slot two.
    const void *specular_map; /* Pixels fallback (specular map, slot 2) or NULL */
    /// Borrowed emissive-map Pixels fallback for texture slot three.
    const void *emissive_map; /* Pixels fallback (emissive map, slot 3) or NULL */
    /// Borrowed live render target sampled natively as the emissive map (ADR 0299).
    const vgfx3d_rendertarget_t *emissive_map_target;
    /// Borrowed RenderTarget3D wrapper owning `emissive_map_target` (see `texture_target_owner`).
    void *emissive_map_target_owner;
    /// Borrowed diffuse TextureAsset3D source for native upload.
    void *texture_asset; /* TextureAsset3D native source (diffuse, slot 0) or NULL */
    /// Borrowed normal-map TextureAsset3D source for native upload.
    void *normal_map_asset;
    /// Borrowed specular-map TextureAsset3D source for native upload.
    void *specular_map_asset;
    /// Borrowed emissive-map TextureAsset3D source for native upload.
    void *emissive_map_asset;
    /// Per-slot native texture cache keys captured when the command was queued.
    uint64_t texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot first native mip requested for upload and sampling.
    int64_t texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot count of native mips requested for upload and sampling.
    int64_t texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// RGB emissive multiplier.
    float emissive_color[3]; /* emissive color multiplier */
    /// Metallic factor in the inclusive range 0-1.
    float metallic; /* [0,1] dielectric->metal */
    /// Perceptual roughness factor in the inclusive range 0-1.
    float roughness; /* [0,1] smooth->rough */
    /// Ambient-occlusion multiplier in the inclusive range 0-1.
    float ao; /* [0,1] ambient occlusion multiplier */
    /// Scalar applied after the emissive color and texture.
    float emissive_intensity; /* scalar multiplier applied after emissive color/map */
    /// Multiplier for tangent-space normal-map X and Y perturbation.
    float normal_scale; /* scales tangent-space XY perturbation */
    /// Nonzero to use additive rather than conventional alpha blending.
    int8_t additive_blend; /* use additive blending instead of standard alpha */
    /// Resolved RT_MATERIAL3D_WORKFLOW_* value.
    int32_t workflow; /* RT_MATERIAL3D_WORKFLOW_* */
    /// Resolved RT_MATERIAL3D_ALPHA_MODE_* value.
    int32_t alpha_mode; /* RT_MATERIAL3D_ALPHA_MODE_* */
    /// Alpha threshold used by masked materials.
    float alpha_cutoff; /* alpha-mask cutoff */
    /// Resolved RT_MATERIAL3D_SHADOW_MODE_* value.
    int32_t shadow_mode; /* RT_MATERIAL3D_SHADOW_MODE_* */
    /// Nonzero to disable face culling for this material.
    int32_t double_sided; /* culling disabled when true */
    /// Legacy/default horizontal RT_MATERIAL3D_TEXTURE_WRAP_* mode.
    int32_t texture_wrap_s; /* RT_MATERIAL3D_TEXTURE_WRAP_* */
    /// Legacy/default vertical RT_MATERIAL3D_TEXTURE_WRAP_* mode.
    int32_t texture_wrap_t; /* RT_MATERIAL3D_TEXTURE_WRAP_* */
    /// Legacy/default combined RT_MATERIAL3D_TEXTURE_FILTER_* mode.
    int32_t texture_filter; /* RT_MATERIAL3D_TEXTURE_FILTER_* */
    /// Legacy/default minification filter.
    int32_t texture_min_filter;
    /// Legacy/default magnification filter.
    int32_t texture_mag_filter;
    /// Legacy/default RT_MATERIAL3D_TEXTURE_MIP_FILTER_* mode.
    int32_t texture_mip_filter; /* RT_MATERIAL3D_TEXTURE_MIP_FILTER_* */
    /// Legacy/default anisotropy request.
    int32_t texture_anisotropy;
    /// Per-slot horizontal texture wrap modes.
    int32_t texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot vertical texture wrap modes.
    int32_t texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot combined texture filter modes.
    int32_t texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot minification filters.
    int32_t texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot magnification filters.
    int32_t texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot mip filters.
    int32_t texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot requested anisotropy values.
    int32_t texture_slot_anisotropy[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot UV-set indices.
    int32_t texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    /// Per-slot affine UV transforms stored as six floats.
    float texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_COUNT][6];
    /// Borrowed combined metallic/roughness Pixels fallback.
    const void *metallic_roughness_map; /* Pixels fallback (glTF metallic/roughness map) or NULL */
    /// Borrowed ambient-occlusion Pixels fallback.
    const void *ao_map; /* Pixels fallback (ambient occlusion map) or NULL */
    /// Borrowed baked-GI Pixels atlas sampled with TEXCOORD_1.
    const void *lightmap; /* baked GI atlas (Pixels, TEXCOORD_1) or NULL: replaces flat ambient */
    /// Borrowed combined metallic/roughness TextureAsset3D source.
    void *metallic_roughness_map_asset;
    /// Borrowed ambient-occlusion TextureAsset3D source.
    void *ao_map_asset;
    /* ADR 0312 projected decal layer. `decal_map` is the resident Pixels fallback
     * (assets sample their resident mip); `decal_rows` map a MODEL-space (pre-skin)
     * position to (s, t, d); a fragment inside [0,1]^3 whose bind normal faces
     * against `decal_forward` blends decal.rgb over the albedo by decal.a *
     * decal_opacity before lighting. `has_decal` gates the whole path. */
    /// Borrowed decal Pixels fallback (ADR 0312) or NULL.
    const void *decal_map;
    /// Row-major 3x4 model-space projector (s, t, d rows over x, y, z, 1).
    float decal_rows[12];
    /// Unit projector forward; surfaces facing it take the decal.
    float decal_forward[3];
    /// Multiplier on the decal alpha in the inclusive range 0-1.
    float decal_opacity;
    /// Nonzero when the decal layer is armed for this draw.
    int8_t has_decal;
    /// Bind-pose vertices behind a CPU-skinned submission (NULL = `vertices` are bind).
    const vgfx3d_vertex_t *bind_vertices;
    /// Borrowed CubeMap3D environment used for reflections.
    const void *env_map; /* CubeMap3D (environment reflections) or NULL */
    /// Legacy environment reflection strength in the inclusive range 0-1.
    float reflectivity; /* [0.0=no reflection, 1.0=mirror] */
    /// Nonzero when @ref env_map is the canvas IBL environment.
    int8_t ibl_env; /* env_map is the canvas IBL environment: PBR draws use
                       SH irradiance + prefiltered specular instead of the
                       flat ambient + legacy reflectivity mix */
    /// Revision of the light and ambient snapshot associated with this draw.
    uint32_t lights_revision; /* monotonic stamp of the light+ambient snapshot this
                                 draw was queued with; consecutive draws sharing a
                                 stamp let backends skip re-uploading scene/light
                                 constants (0 = unknown, always upload) */
    /// Optional CPU-binned clustered-light table for the same light snapshot.
    const void *cluster_table; /* vgfx3d_cluster_table_t built for this draw's light
                                  snapshot, or NULL for the flat light loop. Backends
                                  re-upload cluster buffers under the same
                                  lights_revision gate as the light constants. */
    /* Terrain splat mapping (populated by terrain draw path, NULL otherwise) */
    /// Borrowed terrain weight texture; NULL for non-terrain draws.
    const void *splat_map; /* RGBA weight texture (NULL = not terrain) */
    /// Borrowed diffuse textures for the four terrain layers.
    const void *splat_layers[4]; /* Layer textures */
    /// UV tiling factors for the four terrain layers.
    float splat_layer_scales[4]; /* UV tiling per layer */
    /// Nonzero when terrain splat mapping is active.
    int8_t has_splat; /* 1 = terrain splat active */
    /* GPU skeletal skinning (MTL-09): set by rt_skeleton3d.c for GPU path */
    /// Current row-major bone matrices, optionally containing per-instance palettes.
    const float *bone_palette; /* bone_count * 16 floats (4x4 row-major) */
    /// Previous-frame bone palette for motion vectors, or NULL.
    const float *prev_bone_palette; /* previous-frame palette or NULL */
    /// Total number of matrices in @ref bone_palette; zero disables GPU skinning.
    int32_t bone_count; /* number of bones (0 = no skinning) */
    /* R18 per-instance skinning: when nonzero for an instanced draw, bone_palette
     * holds instance_count consecutive palettes of this many bones each
     * (bone_count = instance_count * stride, still <= VGFX3D_MAX_BONES) and the
     * vertex shader indexes palette[instance_id * stride + bone]. 0 keeps the
     * shared-palette behavior; non-instanced draws ignore it (instance_id = 0). */
    /// Bones per instance for instanced skinning, or zero for one shared palette.
    int32_t instance_bone_stride;
    /* Influences 5-8 side stream (vertex_count entries, 24-byte stride) for
     * backends with gpu_skinning_extras; import-time mesh data, cached by
     * geometry_key/geometry_revision. NULL for standard 4-influence meshes. */
    /// Optional fifth-through-eighth skinning influences, one record per vertex.
    const vgfx3d_extra_influences_t *extra_influences;
    /* GPU morph targets (MTL-10): set by rt_morphtarget3d.c for GPU path */
    /// Packed position deltas organized by shape then vertex.
    const float *morph_deltas; /* shape_count * vertex_count * 3 floats */
    /// Optional packed normal deltas organized by shape then vertex.
    const float *morph_normal_deltas; /* shape_count * vertex_count * 3 floats or NULL */
    /// Current scalar weight for each active morph shape.
    const float *morph_weights; /* shape_count floats */
    /// Previous-frame weights for motion vectors, or NULL.
    const float *prev_morph_weights; /* previous-frame shape_count floats or NULL */
    /// Number of active morph shapes; zero disables morphing.
    int32_t morph_shape_count; /* number of active morph shapes (0 = none) */
    /// Stable identity used by backend morph-delta caches.
    const void *morph_key; /* stable identity for backend morph-payload caches */
    /// Allocation generation paired with @ref morph_key; zero disables retained caching.
    uint64_t morph_identity;
    /// Revision invalidating cached morph delta payloads.
    uint64_t morph_revision; /* bumps when morph delta payload changes */
    /// Previous row-major transform per instance for instanced motion vectors.
    const float *prev_instance_matrices; /* N * 16 floats for instanced motion blur */
    /// Nonzero when @ref prev_model_matrix is valid.
    int8_t has_prev_model_matrix; /* 1 when prev_model_matrix is valid */
    /// Nonzero when @ref prev_instance_matrices covers the submitted instance count.
    int8_t has_prev_instance_matrices; /* 1 when prev_instance_matrices matches instance_count */
    /// Resolved shader shading-model identifier.
    int32_t shading_model; /* 0=BlinnPhong, 1=Toon, 2=PBR, 3=Unlit, 4=Fresnel, 5=Emissive */
    /// User-defined material shader parameters.
    float custom_params[12]; /* user-defined shader parameters */
    /* Plan 10: soft-particle fade distance in world units (0 = hard edges).
     * Blend-mode fragments fade out as they approach the opaque depth
     * snapshot; backends without resolve_opaque_targets ignore it. */
    /// World-space fade distance used where particles intersect opaque depth.
    float soft_particle_fade;
    /* Plan 10: material opts into screen-space reflections (mask written to
     * the motion target's blue channel for the SSR post pass). */
    /// Nonzero to write this material into the screen-space reflection mask.
    int8_t ssr_enabled;
    /// Constant renderer-space depth offset.
    float depth_bias; /* constant depth offset; negative pulls coplanar draws forward */
    /// Slope-proportional renderer-space depth offset.
    float slope_scaled_depth_bias; /* slope-proportional depth offset for steep coplanar polygons */
    /// Nonzero when draw-time scanning found a texture with non-opaque alpha.
    int8_t has_alpha_texture; /* draw-time texture scan found non-opaque alpha */
    /* Recommendation 48: non-NULL selects the retained-unit-quad particle vertex path for an
     * instanced submission. Records are frame-owned and already sorted/rebased. */
    /// Optional compact particle instances selecting the retained-unit-quad path.
    const vgfx3d_particle_instance_t *particle_instances;
} vgfx3d_draw_cmd_t;

/// @brief Canvas-layer resolver for a RenderTarget3D wrapper's Pixels mirror (defined in
///        rt_rendertarget3d.c). Backends call it only on the no-native-storage fallback path;
///        it re-reads the target when its content revision advanced.
/// @param obj Borrowed RenderTarget3D wrapper handle.
/// @return Borrowed target-owned Pixels mirror, or NULL when unavailable.
void *rt_rendertarget3d_material_pixels(void *obj);

/// @brief True when a draw command carries any base-colour texture source
///        (Pixels fallback, native asset, or a natively sampled render target).
/// @param cmd Borrowed draw command; NULL reads as untextured.
/// @return Non-zero when slot zero has a texture source.
static inline int vgfx3d_draw_cmd_has_base_texture(const vgfx3d_draw_cmd_t *cmd) {
    return cmd && (cmd->texture || cmd->texture_asset || cmd->texture_target);
}

/// @brief True when a draw command carries any emissive-map source.
/// @param cmd Borrowed draw command; NULL reads as unmapped.
/// @return Non-zero when the emissive slot has a texture source.
static inline int vgfx3d_draw_cmd_has_emissive_map(const vgfx3d_draw_cmd_t *cmd) {
    return cmd && (cmd->emissive_map || cmd->emissive_map_asset || cmd->emissive_map_target);
}

/* R20 compact static-mesh vertex stream: an opt-in 48-byte packed twin of the
 * 92-byte vgfx3d_vertex_t used only for GPU static-geometry-cache uploads.
 * Fixed-function vertex-attribute conversion (Metal stage_in, D3D11 input
 * layouts, GL glVertexAttribPointer) decodes the packed fields to the same
 * float attributes the shaders already consume, so no shader changes.
 * Layout (offsets are the per-backend vertex-descriptor contract):
 *   0  float3   position
 *   12 snorm16x4 normal            (w unused)
 *   20 half2    uv (TEXCOORD_0)
 *   24 half2    uv1 (TEXCOORD_1)
 *   28 unorm8x4 color
 *   32 snorm16x4 tangent           (w = handedness, +-1 exact)
 *   40 uint8x4  bone indices
 *   44 unorm8x4 bone weights       (shaders renormalize by total weight)
 * The software backend never uses this encoding; it samples the full CPU
 * vertex array and remains the golden reference. */
#define VGFX3D_COMPACT_VERTEX_STRIDE 48u

/// @brief Encode @p count full vertices into the compact 48-byte stream layout.
/// @details @p dst must hold count * VGFX3D_COMPACT_VERTEX_STRIDE bytes.
/// @param[in] src Array of full-precision vertices.
/// @param[in] count Number of vertices in @p src.
/// @param[out] dst Destination byte span with room for every compact vertex.
void vgfx3d_encode_compact_vertices(const vgfx3d_vertex_t *src, uint32_t count, uint8_t *dst);

#define VGFX3D_MAX_DRAW_INDEX_COUNT ((uint32_t)INT_MAX)
/* Renderer depth-bias units are NDC-scale offsets (the software rasterizer adds them to the
 * interpolated depth directly). GPU constant-bias units are multiples of the depth buffer's
 * smallest resolvable step — 2^-24 for D24 and, for float depth in the typical [0.5, 1) scene
 * range, effectively the same magnitude. Scaling by 2^24 therefore makes one renderer unit
 * ~one NDC unit on every backend; the previous 65536 scale left GPU biases ~256x weaker than
 * the software reference, so decal/material biases behaved differently per platform. */
#define VGFX3D_DEPTH_BIAS_CONSTANT_SCALE 16777216.0f

/// @brief Return the triangle-list index count accepted by all backends, or 0 if invalid.
/// @details GPU APIs do not share the software backend's per-triangle out-of-range guard. This
///          helper defines a single conservative contract: commands must contain complete
///          triangles, fit APIs with signed draw-count parameters, and every referenced vertex
///          must be inside the submitted vertex buffer. Rejecting bad commands before upload avoids
///          undefined GPU fetches that can surface as flickering or exploding triangles.
/// @param[in] cmd Draw command to validate.
/// @return The complete validated index count, or 0 if the command is not safe to draw.
static inline uint32_t vgfx3d_draw_cmd_validated_index_count(const vgfx3d_draw_cmd_t *cmd) {
    uint32_t count;
    if (!cmd || !cmd->vertices || !cmd->indices || cmd->vertex_count == 0 || cmd->index_count < 3)
        return 0;
    if (cmd->index_count > VGFX3D_MAX_DRAW_INDEX_COUNT)
        return 0;
    if ((cmd->index_count % 3u) != 0u)
        return 0;
    if (cmd->validated_index_count == cmd->index_count)
        return cmd->index_count;
    count = cmd->index_count;
    for (uint32_t i = 0; i < count; i++) {
        if (cmd->indices[i] >= cmd->vertex_count)
            return 0;
    }
    return count;
}

/// @brief True when @p cmd contains a complete, in-range indexed triangle list.
/// @param[in] cmd Draw command to inspect.
/// @return Nonzero when the command passes shared triangle-list validation.
static inline int vgfx3d_draw_cmd_has_valid_triangles(const vgfx3d_draw_cmd_t *cmd) {
    return vgfx3d_draw_cmd_validated_index_count(cmd) != 0u;
}

/// @brief Convert renderer depth-bias units to backend constant-bias units.
/// @details Material3D exposes a small float bias in renderer units. Backends with
///          implementation-specific constant-bias scales use this conversion so decal/shadow
///          offsets remain comparable across GL, D3D11, Metal, and software.
/// @param[in] bias Renderer-space constant depth bias.
/// @return Scaled backend bias clamped to the signed-integer range, or zero for non-finite input.
static inline float vgfx3d_depth_bias_constant_units(float bias) {
    double scaled;
    if (!isfinite(bias))
        return 0.0f;
    scaled = (double)bias * (double)VGFX3D_DEPTH_BIAS_CONSTANT_SCALE;
    if (scaled > (double)INT_MAX)
        return (float)INT_MAX;
    if (scaled < (double)INT_MIN)
        return (float)INT_MIN;
    return (float)scaled;
}

/// @brief Convert renderer depth-bias units to D3D11's integer DepthBias field.
/// @param[in] bias Renderer-space constant depth bias.
/// @return Rounded and saturating D3D11 integer depth-bias value.
static inline int32_t vgfx3d_depth_bias_d3d11_units(float bias) {
    float scaled = vgfx3d_depth_bias_constant_units(bias);
    if (scaled >= (float)INT_MAX)
        return INT_MAX;
    if (scaled <= (float)INT_MIN)
        return INT_MIN;
    return (int32_t)lrintf(scaled);
}

/// @brief Constant depth-bias units for a reversed-Z scene pass (sign-flipped).
/// @details The Material3D contract is standard-Z: positive bias pushes fragments away
///          from the camera. Reversed-Z scene passes (clear 0, Greater compare) invert
///          the window-z direction, so both the constant and slope-scaled terms must be
///          negated to preserve that contract. Shadow-map passes keep the standard
///          convention on every backend and use the unflipped helpers above.
/// @param[in] bias Standard-Z renderer-space constant bias.
/// @return The sign-flipped backend constant-bias value for a reversed-Z pass.
static inline float vgfx3d_depth_bias_constant_units_reversed_z(float bias) {
    return -vgfx3d_depth_bias_constant_units(bias);
}

/// @brief Slope-scaled depth-bias term for a reversed-Z scene pass (sign-flipped).
/// @param[in] slope_bias Standard-Z slope-scaled bias.
/// @return The finite sign-flipped slope bias, or zero for non-finite input.
static inline float vgfx3d_depth_bias_slope_reversed_z(float slope_bias) {
    return isfinite(slope_bias) ? -slope_bias : 0.0f;
}

/// @brief True if the command needs standard (non-additive) alpha blending.
/// @details Honors the draw command's resolved alpha mode first. Material classification promotes
///   decoded fractional-alpha textures to BLEND and binary cutouts to MASK before commands reach
///   this backend helper; a remaining `has_alpha_texture` with OPAQUE mode is treated as masked for
///   conservative depth/shadow behavior. Scalar material alpha still infers blending.
/// @param[in] cmd Resolved draw command to classify.
/// @return 1 when standard alpha blending is required, otherwise 0.
static inline int vgfx3d_draw_cmd_uses_alpha_blend(const vgfx3d_draw_cmd_t *cmd) {
    if (!cmd)
        return 0;
    if (cmd->additive_blend)
        return 0;
    if (cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND)
        return 1;
    if (cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK)
        return 0;
    if (cmd->has_alpha_texture)
        return 0;
    return (cmd->alpha < 0.999f || cmd->diffuse_color[3] < 0.999f) ? 1 : 0;
}

/// @brief True if the command is transparent in any way — additive OR alpha-blended —
///   i.e. it must be drawn in the transparent (depth-sorted, non-depth-writing) pass.
/// @param[in] cmd Resolved draw command to classify.
/// @return 1 for additive or standard alpha-blended draws, otherwise 0.
static inline int vgfx3d_draw_cmd_uses_transparent_blend(const vgfx3d_draw_cmd_t *cmd) {
    if (!cmd)
        return 0;
    return cmd->additive_blend || vgfx3d_draw_cmd_uses_alpha_blend(cmd);
}

/// @brief SSR mask strength for a draw (0 = not reflective in screen space).
/// @details Written to the motion target's alpha channel; the SSR post pass
///          scales its contribution by this per-pixel value. Materials with an
///          explicit reflectivity reuse it; SSR-only surfaces default to 0.5.
/// @param[in] cmd Resolved draw command to inspect.
/// @return Zero when SSR is disabled, otherwise explicit reflectivity or the 0.5 default.
static inline float vgfx3d_draw_cmd_ssr_mask(const vgfx3d_draw_cmd_t *cmd) {
    if (!cmd || !cmd->ssr_enabled)
        return 0.0f;
    return cmd->reflectivity > 0.001f ? cmd->reflectivity : 0.5f;
}

/*==========================================================================
 * Camera parameters — passed to begin_frame
 *=========================================================================*/

/// @brief Per-frame camera state passed to begin_frame: view/projection matrices, eye
///   position/forward, ortho flag, optional distance fog, and load-existing color/depth
///   flags for overlay/secondary passes.
typedef struct vgfx3d_camera_params {
    /// Row-major world-to-view matrix.
    float view[16]; /* view matrix, row-major float */
    /// Row-major view-to-clip projection matrix.
    float projection[16]; /* projection matrix, row-major float */
    /// Camera position in render-world space.
    float position[3]; /* eye position (for specular) */
    /// Normalized camera forward direction in render-world space.
    float forward[3]; /* forward/view direction in world space */
    /// Nonzero for an orthographic projection.
    int8_t is_ortho; /* 1 = orthographic projection */
    /* Distance fog */
    /// Nonzero to enable distance-fog attenuation.
    int8_t fog_enabled;
    /// Near and far distances spanning the linear fog ramp.
    float fog_near, fog_far;
    /// RGB color approached under full distance or height fog.
    float fog_color[3];
    /* Exponential height fog (Track E doc 07): density scales by
     * exp(-(worldY - base) * falloff); combines with distance fog via
     * combined transmittance, weighted by blend. Shares fog_color. */
    /// Nonzero to enable exponential world-height fog.
    int8_t height_fog_enabled;
    /// World-space reference height for exponential density.
    float height_fog_base;
    /// Exponential density falloff per world-height unit.
    float height_fog_falloff;
    /// Base height-fog density.
    float height_fog_density;
    /// Blend weight combining height and distance fog.
    float height_fog_blend;
    /* Height-fog sun inscattering: fog color shifts toward sun_color by
     * amount * pow(max(dot(viewDir, sun_dir), 0), power). amount 0 disables
     * (default), keeping legacy height-fog output bit-identical. sun_dir is
     * the world-space direction TOWARD the sun, resolved per frame from the
     * first enabled directional light. */
    /// RGB inscattering color contributed toward the sun.
    float height_fog_sun_color[3];
    /// Normalized world-space direction toward the resolved sun.
    float height_fog_sun_dir[3];
    /// Angular exponent concentrating sun inscattering.
    float height_fog_sun_power;
    /// Strength of sun inscattering; zero preserves legacy fog output.
    float height_fog_sun_amount;
    /* Secondary passes can preserve the previous scene color while resetting
     * depth for overlays or UI. */
    /// Nonzero to preserve existing scene color at frame begin.
    int8_t load_existing_color;
    /// Nonzero to preserve existing depth at frame begin.
    int8_t load_existing_depth;
    /* Image-based lighting (canvas environment). When ibl_enabled is nonzero,
     * ibl_sh carries SH-9 RGB irradiance coefficients (Ramamoorthi-Hanrahan
     * order, cosine-convolved, 1/pi folded) and PBR draws flagged ibl_env
     * replace the flat ambient term with SH diffuse + prefiltered specular. */
    /// Nonzero when canvas image-based lighting data is valid.
    int8_t ibl_enabled;
    /// Scalar environment-light intensity.
    float ibl_intensity;
    /// Nine RGB cosine-convolved spherical-harmonic irradiance coefficients.
    float ibl_sh[27];
    /* Shadow filtering/bias parameters (Plan 06). shadow_strength is how dark a
     * fully-occluded texel gets (0 = no darkening, 1 = black); shadow_slope_bias
     * scales the per-texel slope-proportional compare bias; shadow_quality picks
     * the PCF tier (0 = 4 taps, 1 = 8, 2 = 16 rotated-Poisson taps). */
    /// Fractional darkening applied to fully occluded shadow samples.
    float shadow_strength;
    /// Slope-proportional shadow comparison bias.
    float shadow_slope_bias;
    /// PCF quality tier: 0 for 4 taps, 1 for 8, and 2 for 16.
    int32_t shadow_quality;
    /* Plan 10: camera clip planes for shader-side depth linearization (soft
     * particles, SSR). Zero/invalid values disable depth-dependent effects. */
    /// Camera near clip distance used for shader-side depth linearization.
    float znear;
    /// Camera far clip distance used for shader-side depth linearization.
    float zfar;
    /// Nonzero for depth-only shading (ADR 0311): the software backend keeps the
    /// vertex stage, clipping, depth test, and opaque depth writes but skips the
    /// fragment shader and colour writes. GPU backends ignore the flag. Headless
    /// probes whose gates never read pixels set it; capture paths clear it.
    int8_t depth_only_shading;
} vgfx3d_camera_params_t;

/// Maximum resident static geometry entries on each GPU backend (not a byte budget).
#define VGFX3D_STATIC_MESH_CACHE_CAPACITY 256

/*==========================================================================
 * Lighting parameters — set before begin_frame
 *=========================================================================*/

#define VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC 0
#define VGFX3D_SHADOW_PROJECTION_PERSPECTIVE 1
/* Omnidirectional point-light shadow: shadow_index is the FIRST of six
 * consecutive slots (atlas tiles), one 90-degree perspective face per axis in
 * the order +X,-X,+Y,-Y,+Z,-Z. Shaders pick the face by the dominant axis of
 * light->fragment and then sample it exactly like a perspective slot. */
#define VGFX3D_SHADOW_PROJECTION_CUBE 2
#define VGFX3D_SHADOW_CUBE_FACES 6

/*==========================================================================
 * Clustered forward+ light culling (Plan 07)
 *=========================================================================*/

#define VGFX3D_CLUSTER_DIM_X 16
#define VGFX3D_CLUSTER_DIM_Y 9
#define VGFX3D_CLUSTER_DIM_Z 24
#define VGFX3D_CLUSTER_COUNT (VGFX3D_CLUSTER_DIM_X * VGFX3D_CLUSTER_DIM_Y * VGFX3D_CLUSTER_DIM_Z)
#define VGFX3D_MAX_CLUSTER_LIGHT_INDICES 8192
#define VGFX3D_CLUSTER_FALLBACK_FLAG 0x8000u
#define VGFX3D_CLUSTER_OFFSET_MASK 0x7fffu

/// @brief CPU-binned froxel light table consumed by GPU backends.
/// @details Built by the canvas per light-snapshot revision (the same stamp that
///   gates scene/light constant uploads): the flattened light array is sorted so
///   directional/ambient lights form a global prefix of length
///   `global_light_count`, and every point/spot light's bounding sphere is
///   conservatively rasterized into a 16x9x24 view froxel grid with exponential
///   Z slicing over [znear, zfar]. `offsets` holds prefix sums into `indices`
///   (light-array indices) per cluster in X-major, then Y, then Z order:
///   cluster = x + y*DIM_X + z*DIM_X*DIM_Y. Per-cluster capacity/index
///   overflow selects the full light loop using the high bit of the cluster's
///   start offset (ADR 0328). Mask both endpoints before indexing. Flagged
///   clusters have empty compact lists; the terminal offset is never flagged.
typedef struct vgfx3d_cluster_table {
    /// Light snapshot revision matched by this table; zero marks invalid data.
    uint32_t lights_revision; /* snapshot revision this table matches (0 = invalid) */
    /// Length of the directional/ambient prefix applied to every cluster.
    int32_t global_light_count; /* directional/ambient prefix length in the light array */
    /// Number of point and spot lights considered during froxel binning.
    int32_t binned_light_count; /* point/spot lights considered for binning */
    /// Local index demand served losslessly by full-light fallback, not dropped.
    int32_t overflow_count; /* capacity pressure; no table-layout change */
    /// Near depth used by exponential Z slicing.
    float znear;
    /// Far depth used by exponential Z slicing.
    float zfar;
    /// Prefix-sum boundaries into @ref indices for every cluster.
    uint16_t offsets[VGFX3D_CLUSTER_COUNT + 1];
    /// Flattened indices into the frame's sorted light array.
    uint16_t indices[VGFX3D_MAX_CLUSTER_LIGHT_INDICES];
} vgfx3d_cluster_table_t;

/// @brief One light passed to submit_draw, including native area/volume emitter state.
/// @details Directional, point, ambient, and spot lights use the historical fields. Rectangle
///   and sphere area lights additionally use their finite emitter dimensions/range and FBX decay
///   mode; volume lights use radius/range and never receive a shadow slot. Rectangle U/V basis
///   vectors are orthonormal in render space. Keeping this as one plain finite payload lets the
///   software and three GPU backends evaluate identical authored light semantics.
typedef struct vgfx3d_light_params {
    /// Light kind: directional, point, ambient, spot, rectangle, sphere, or volume.
    int32_t type; /* 0=directional, 1=point, 2=ambient, 3=spot, 4=rect, 5=sphere, 6=volume */
    /// First assigned shadow slot, or -1 when unshadowed.
    int32_t shadow_index; /* -1 = unshadowed, otherwise [0, VGFX3D_MAX_SHADOW_LIGHTS) */
    /// Number of cascades beginning at @ref shadow_index.
    int32_t shadow_cascade_count; /* >1 means shadow_index is the first cascade slot */
    /// VGFX3D_SHADOW_PROJECTION_* value for the assigned shadow slots.
    int32_t shadow_projection_type; /* VGFX3D_SHADOW_PROJECTION_* */
    /// Nonzero when this light participates in shadow rendering.
    int32_t casts_shadows;
    /// Stable CPU identity used to preserve shadow-slot assignments.
    uintptr_t identity; /* stable CPU light identity used to match shadow slots after packing */
    /// World-space emission direction for directional and spot lights.
    float direction[3];
    /// World-space emitter position.
    float position[3];
    /// First orthonormal rectangle-light basis vector.
    float basis_u[3];
    /// Second orthonormal rectangle-light basis vector.
    float basis_v[3];
    /// Linear RGB light color.
    float color[3];
    /// View-depth split boundaries for cascaded shadow maps.
    float shadow_cascade_splits[VGFX3D_CSM_SLOTS]; /* consumed as one float4 by shaders */
    /// Scalar emitted-light intensity.
    float intensity;
    /// Legacy attenuation coefficient.
    float attenuation;
    /// Cosine of a spot light's full-brightness inner cone.
    float inner_cos; /* spot: cosine of inner cone angle (full brightness) */
    /// Cosine of a spot light's zero-brightness outer cone.
    float outer_cos; /* spot: cosine of outer cone angle (zero brightness) */
    /// Rectangle-emitter width.
    float width;
    /// Rectangle-emitter height.
    float height;
    /// Sphere or volume emitter radius.
    float radius;
    /// Authored influence range; punctual zero preserves the legacy unbounded curve.
    float range;
    /// FBX attenuation mode: none, linear, quadratic, or cubic.
    int32_t decay_type; /* 0=none, 1=linear, 2=quadratic, 3=cubic */
} vgfx3d_light_params_t;

/// @brief Optional backend telemetry snapshot for renderer diagnostics.
/// @details All counters are monotonic for the lifetime of the backend context unless otherwise
///          stated. Backends that do not implement a field should leave it zero. The structure is
///          intentionally plain integers so Canvas3D can expose selected values through stable
///          runtime properties without allocating or depending on backend-private headers.
typedef struct {
    uint64_t draw_calls;        ///< Successful backend draw calls emitted.
    uint64_t dropped_draws;     ///< Draw commands rejected by the backend before issuing API calls.
    uint64_t mesh_cache_hits;   ///< Static mesh VBO/IBO cache hits.
    uint64_t mesh_cache_misses; ///< Static mesh VBO/IBO cache misses or refreshes.
    uint64_t mesh_stream_uploads;    ///< Transient mesh VBO/IBO uploads.
    uint64_t texture_fallback_binds; ///< Times a fallback texture was bound while real payload was
                                     ///< absent.
    uint64_t direct_presents;    ///< Presents through the native GPU swapchain/default framebuffer.
    uint64_t offscreen_presents; ///< Presents resolved through an offscreen/readback path.
    int32_t present_path;        ///< 0 unknown, 1 direct, 2 offscreen.
    int32_t default_framebuffer_writable; ///< Last default framebuffer writability decision.
} vgfx3d_backend_stats_t;

/*==========================================================================
 * Backend vtable
 *=========================================================================*/

/// @brief Renderer abstraction vtable. Each backend (software/Metal/D3D11/OpenGL) fills
///   in these function pointers; Canvas3D dispatches through them without knowing the
///   concrete backend. Core lifecycle/frame/draw hooks must be non-NULL; optional hooks
///   (shadows, skybox, instancing, present, readback, GPU post-FX, layer show/hide) may
///   be NULL, signaling Canvas3D to use its software fallback for that capability.
typedef struct vgfx3d_backend {
    /// Stable backend name: `software`, `metal`, `d3d11`, or `opengl`.
    const char *name; /* "software", "metal", "d3d11", "opengl" */

    /* 1 = the backend can render AND sample shadow slots beyond
     * VGFX3D_CSM_SLOTS (its internal atlas tiles). Canvas3D clamps its
     * per-frame shadow slot usage to VGFX3D_CSM_SLOTS when 0, so lights never
     * receive slot indices the backend's shaders cannot resolve. */
    /// Nonzero when the backend renders and samples general shadow-atlas slots.
    int8_t shadow_atlas_slots;

    /// @brief Prepare storage for the entire shadow pass before any slot renders (ADR 0337).
    /// @param ctx Backend context.
    /// @param slots Required contiguous slot extent, including inherited cached slots.
    /// @param primary_resolution Requested square primary-map dimension.
    /// @param atlas_resolution Requested square secondary-map dimension.
    /// @return Nonzero on success; zero drops all shadow requests for this pass.
    /// NULL preserves legacy lazy allocation for backends without shared storage.
    int8_t (*prepare_shadow_frame)(void *ctx,
                                   int32_t slots,
                                   int32_t primary_resolution,
                                   int32_t atlas_resolution);


    /* 1 = the backend's draw path consumes bone palettes directly (vertex-
     * shader skinning). Replaces the old per-draw backend-name strcmp gate. */
    /// Nonzero when bone palettes are consumed directly by the vertex stage.
    int8_t gpu_skinning;

    /* 1 = the backend's vertex stage also consumes the influences 5-8 side
     * stream (cmd->extra_influences), so 8-weight meshes keep GPU skinning.
     * Backends without it fall back to CPU skinning for such meshes. */
    /// Nonzero when GPU skinning consumes the optional influences 5-8 side stream.
    int8_t gpu_skinning_extras;

    /* 1 = submit_draw_instanced recognizes cmd->particle_instances and consumes the compact
     * retained-unit-quad payload. Software deliberately leaves this false. */
    /// Nonzero when instanced draws accept compact particle-instance payloads.
    int8_t particle_instancing;

    /* 1 = the backend ships the production many-light clustered/forward+ shader
     * and light-table upload path. Replaces the old per-platform backend-identity
     * gate in Canvas3D; partial or fake test backends stay 0 regardless of name. */
    /// Nonzero when the production clustered/forward+ many-light path exists.
    int8_t clustered_lighting;

    /* 1 = the backend consumes multiple primary-light shadow slots as cascades
     * (CSM). Canvas3D additionally requires the shadow_begin/draw/end hooks
     * before advertising the capability. */
    /// Nonzero when shadow slots can be consumed as primary-light cascades.
    int8_t shadow_csm;

    /* Lifecycle */
    /// @brief Create backend-owned rendering state for a window and initial extent.
    /// @param[in] win Platform window handle.
    /// @param[in] w Initial width in pixels.
    /// @param[in] h Initial height in pixels.
    /// @return Opaque backend context, or NULL when initialization fails.
    void *(*create_ctx)(vgfx_window_t win, int32_t w, int32_t h);
    /// @brief Destroy an opaque backend context and all resources it owns.
    /// @param[in,out] ctx Backend context returned by @ref create_ctx.
    void (*destroy_ctx)(void *ctx);

    /* Frame */
    /// @brief Clear window-backed color and reset backend frame targets.
    /// @param[in,out] ctx Backend context.
    /// @param[in] win Platform window handle associated with the frame.
    /// @param[in] r Linear red clear component.
    /// @param[in] g Linear green clear component.
    /// @param[in] b Linear blue clear component.
    void (*clear)(void *ctx, vgfx_window_t win, float r, float g, float b);
    /// @brief Resize backend-owned window targets.
    /// @param[in,out] ctx Backend context.
    /// @param[in] w New width in pixels.
    /// @param[in] h New height in pixels.
    void (*resize)(void *ctx, int32_t w, int32_t h);
    /// @brief Begin a scene pass using the resolved per-frame camera state.
    /// @param[in,out] ctx Backend context.
    /// @param[in] cam Camera, fog, IBL, and shadow-filtering parameters.
    void (*begin_frame)(void *ctx, const vgfx3d_camera_params_t *cam);
    /// @brief Submit one resolved indexed draw to the active scene or target pass.
    /// @param[in,out] ctx Backend context.
    /// @param[in] win Platform window handle for window-backed rendering.
    /// @param[in] cmd Complete geometry and material payload.
    /// @param[in] lights Resolved light snapshot.
    /// @param[in] light_count Number of entries in @p lights.
    /// @param[in] ambient RGB ambient-light vector.
    /// @param[in] wireframe Nonzero to rasterize wireframe edges.
    /// @param[in] backface_cull Nonzero to cull back-facing triangles.
    void (*submit_draw)(void *ctx,
                        vgfx_window_t win,
                        const vgfx3d_draw_cmd_t *cmd,
                        const vgfx3d_light_params_t *lights,
                        int32_t light_count,
                        const float *ambient,
                        int8_t wireframe,
                        int8_t backface_cull);
    /// @brief Finish the active scene pass and finalize intermediate targets.
    /// @param[in,out] ctx Backend context.
    void (*end_frame)(void *ctx);

    /* Render target (NULL = render to window) */
    /// @brief Select an offscreen target, or NULL to restore window-backed rendering.
    /// @param[in,out] ctx Backend context.
    /// @param[in] rt RenderTarget3D implementation to bind, or NULL.
    void (*set_render_target)(void *ctx, vgfx3d_rendertarget_t *rt);

    /* Shadow map pass. All three may be NULL if not supported by this backend.
     * shadow_begin: initialize the indexed depth target and store that light VP.
     * shadow_draw: depth-only rasterize one mesh into the shadow map.
     * shadow_end: finalize that slot for lookup in the main pass. */
    /// @brief Begin rendering one indexed shadow slot.
    /// @param[in,out] ctx Backend context.
    /// @param[in] slot Shadow slot index.
    /// @param[in,out] depth_buf Canvas-owned CPU depth storage used by compatible backends.
    /// @param[in] w Shadow-map width.
    /// @param[in] h Shadow-map height.
    /// @param[in] light_vp Row-major light view-projection matrix.
    void (*shadow_begin)(
        void *ctx, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp);
    /// @brief Rasterize one shadow-casting draw into the active shadow slot.
    /// @param[in,out] ctx Backend context.
    /// @param[in] cmd Resolved shadow draw command.
    void (*shadow_draw)(void *ctx, const vgfx3d_draw_cmd_t *cmd);
    /// @brief Optional static instanced depth draw (ADR 0335); NULL uses per-mesh fallback.
    /// @param ctx Backend with an active shadow slot.
    /// @param cmd Borrowed shared geometry/material; no deformation or particles.
    /// @param matrices Borrowed count * 16 row-major transforms; copy before returning if retained.
    /// @param count Positive number of instances.
    void (*shadow_draw_instanced)(void *ctx,
                                  const vgfx3d_draw_cmd_t *cmd,
                                  const float *matrices,
                                  int32_t count);
    /// @brief Finish an indexed shadow slot and make it sampleable by scene draws.
    /// @param[in,out] ctx Backend context.
    /// @param[in] slot Shadow slot index.
    /// @param[in] bias Constant comparison bias associated with the slot.
    void (*shadow_end)(void *ctx, int32_t slot, float bias);

    /* Optional shadow-slot reuse: re-arm slot for main-pass sampling using the
     * depth contents it already holds from a previous frame, WITHOUT clearing or
     * re-rendering. Canvas3D calls this when a slot's caster set, light VP, and
     * resolution are provably unchanged (per-slot signature), so fully static
     * shadow maps stop costing a render pass every frame. Returns 1 when the
     * slot's stored depth is still available at (w, h) and has been marked
     * complete; 0 tells Canvas3D to fall back to a full begin/draw/end. NULL =
     * backend cannot guarantee persistence, caching disabled. */
    /// @brief Re-arm a persistent shadow slot without rerendering its unchanged contents.
    /// @param[in,out] ctx Backend context.
    /// @param[in] slot Shadow slot index.
    /// @param[in,out] depth_buf Canvas-owned CPU depth storage where applicable.
    /// @param[in] w Required shadow-map width.
    /// @param[in] h Required shadow-map height.
    /// @param[in] light_vp Required row-major light view-projection matrix.
    /// @return 1 when the prior slot contents remain reusable, otherwise 0.
    int8_t (*shadow_reuse)(
        void *ctx, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp);

    /* Optional ADR 0309 render-target shadow inheritance: like shadow_reuse, but
     * atlas slots (>= VGFX3D_CSM_SLOTS) may also re-arm. Canvas3D calls this only
     * for a render-target frame that renders NO atlas slot itself, so the shared
     * atlas is never whole-cleared and its previous-frame tiles remain intact.
     * NULL = the backend cannot guarantee persistence; the render-target
     * inheritance opt-in becomes a no-op on that backend. */
    /// @brief Re-arm any persistent shadow slot (atlas tiles included) for a
    ///   render-target frame that renders no atlas slot of its own.
    /// @param[in,out] ctx Backend context.
    /// @param[in] slot Shadow slot index (classic or atlas).
    /// @param[in,out] depth_buf Canvas-owned CPU depth storage where applicable.
    /// @param[in] w Required shadow-map width.
    /// @param[in] h Required shadow-map height.
    /// @param[in] light_vp Required row-major light view-projection matrix.
    /// @return 1 when the prior slot contents remain reusable, otherwise 0.
    int8_t (*shadow_inherit)(
        void *ctx, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp);

    /* Shadow slots beyond VGFX3D_CSM_SLOTS are "atlas slots": Canvas3D still
     * drives them through shadow_begin/draw/end with per-slot CPU depth
     * buffers, but GPU backends store them as tiles of one internal depth
     * atlas (prepared grid keyed by slot - VGFX3D_CSM_SLOTS) so the general
     * shadow budget can grow without consuming more texture bind points. */

    /* Optional skybox pass. When non-NULL, Canvas3D may delegate cubemap skybox
     * rendering to the backend instead of rasterizing it into the software
     * framebuffer. */
    /// @brief Draw a borrowed CubeMap3D as the scene skybox.
    /// @param[in,out] ctx Backend context.
    /// @param[in] cubemap Opaque CubeMap3D source.
    void (*draw_skybox)(void *ctx, const void *cubemap);


    /* Instanced rendering (MTL-13): draw multiple instances in one GPU call.
     * NULL = fallback to N individual submit_draw() calls (software path). */
    /// @brief Submit multiple instances of one resolved draw in a single backend operation.
    /// @param[in,out] ctx Backend context.
    /// @param[in] win Platform window handle for window-backed rendering.
    /// @param[in] cmd Shared geometry and material payload.
    /// @param[in] instance_matrices Array of @p instance_count row-major model matrices.
    /// @param[in] instance_count Number of transforms in @p instance_matrices.
    /// @param[in] lights Resolved light snapshot.
    /// @param[in] light_count Number of entries in @p lights.
    /// @param[in] ambient RGB ambient-light vector.
    /// @param[in] wireframe Nonzero to rasterize wireframe edges.
    /// @param[in] backface_cull Nonzero to cull back-facing triangles.
    void (*submit_draw_instanced)(void *ctx,
                                  vgfx_window_t win,
                                  const vgfx3d_draw_cmd_t *cmd,
                                  const float *instance_matrices, /* N * 16 floats */
                                  int32_t instance_count,
                                  const vgfx3d_light_params_t *lights,
                                  int32_t light_count,
                                  const float *ambient,
                                  int8_t wireframe,
                                  int8_t backface_cull);

    /* Present the final frame to the display. Called once per Flip().
     * For GPU backends, this presents the drawable / swaps the back buffer.
     * NULL = no-op (software backend — vgfx_update handles display). */
    /// @brief Present the completed window-backed frame.
    /// @param[in,out] ctx Backend context.
    void (*present)(void *ctx);

    /* Optional readback hook for window-backed rendering. When non-NULL, Canvas3D
     * may request the current scene color in RGBA row-major layout. */
    /// @brief Read the current window scene color into caller-owned row-major RGBA8 storage.
    /// @param[in,out] ctx Backend context.
    /// @param[out] dst_rgba Destination image bytes.
    /// @param[in] w Requested width in pixels.
    /// @param[in] h Requested height in pixels.
    /// @param[in] stride Destination row stride in bytes.
    /// @return Nonzero on complete readback, otherwise zero.
    int (*readback_rgba)(void *ctx, uint8_t *dst_rgba, int32_t w, int32_t h, int32_t stride);

    /* Optional GPU post-processing presentation hook. When non-NULL, Canvas3D
     * skips the CPU postfx pass and lets the backend own the final onscreen
     * composite for the supplied ordered effect chain. */
    /// @brief Apply an ordered GPU post-processing chain and present its result.
    /// @param[in,out] ctx Backend context.
    /// @param[in] postfx Resolved post-processing chain for the frame.
    void (*present_postfx)(void *ctx, const vgfx3d_postfx_chain_t *postfx);

    /* Plan 10: snapshot the opaque-pass depth (and any SSR inputs) into
     * sampleable textures at the opaque->transparent seam. Optional; NULL means
     * soft particles/SSR are unsupported and blend draws keep hard edges. */
    /// @brief Resolve opaque-pass depth, color, and motion inputs needed by later effects.
    /// @param[in,out] ctx Backend context.
    void (*resolve_opaque_targets)(void *ctx);

    /* Optional split GPU post-processing hook. When non-NULL, Canvas3D may ask
     * the backend to composite post-FX into the current presentation target,
     * replay final overlays on top, and then call present(). Backends without
     * this hook keep the legacy present_postfx path. */
    /// @brief Composite post-processing into the current target without presenting it.
    /// @param[in,out] ctx Backend context.
    /// @param[in] postfx Resolved post-processing chain for the frame.
    void (*apply_postfx)(void *ctx, const vgfx3d_postfx_chain_t *postfx);

    /* Optional per-frame hint for backends that need to know whether the
     * current window-backed frame will be presented through GPU postfx. */
    /// @brief Set whether the current window frame will use the GPU post-FX presentation path.
    /// @param[in,out] ctx Backend context.
    /// @param[in] enabled Nonzero when GPU post-processing presentation is planned.
    void (*set_gpu_postfx_enabled)(void *ctx, int8_t enabled);

    /* Optional latched postfx chain for GPU backends that need the current
     * frame's effect settings outside the immediate present_postfx call, for
     * example to service screenshots from the backend-owned presentation path. */
    /// @brief Latch a borrowed post-FX snapshot for deferred presentation or readback work.
    /// @param[in,out] ctx Backend context.
    /// @param[in] postfx Current resolved post-processing chain.
    void (*set_gpu_postfx_snapshot)(void *ctx, const vgfx3d_postfx_chain_t *postfx);

    /* Show/hide GPU layer. Called from Canvas3D.Begin/End to toggle
     * visibility of the GPU rendering layer (e.g., CAMetalLayer).
     * NULL = no-op (software backend has no GPU layer). */
    /// @brief Make the backend-owned GPU presentation layer visible.
    /// @param[in,out] ctx Backend context.
    void (*show_gpu_layer)(void *ctx);
    /// @brief Hide the backend-owned GPU presentation layer.
    /// @param[in,out] ctx Backend context.
    void (*hide_gpu_layer)(void *ctx);

    /* Optional streaming/upload controls. Budget is in texture payload bytes
     * uploaded to backend storage per frame; UINT64_MAX means unlimited. */
    /// @brief Set the maximum native texture payload bytes uploaded during one frame.
    /// @param[in,out] ctx Backend context.
    /// @param[in] bytes Per-frame upload budget, or UINT64_MAX for unlimited.
    void (*set_texture_upload_budget)(void *ctx, uint64_t bytes);
    /// @brief Query queued texture bytes not yet uploaded because of budget limits.
    /// @param[in] ctx Backend context.
    /// @return Pending native texture upload bytes.
    uint64_t (*get_texture_upload_pending_bytes)(void *ctx);
    /* Optional streaming/upload telemetry. Returns the texture payload bytes
     * uploaded to backend storage during the current frame. NULL = unsupported. */
    /// @brief Query native texture payload bytes uploaded during the current frame.
    /// @param[in] ctx Backend context.
    /// @return Current-frame texture upload bytes.
    uint64_t (*get_texture_upload_bytes)(void *ctx);
    /* Optional GPU timing telemetry. Returns the latest completed backend GPU
     * frame time in microseconds, or 0 when unsupported/not yet available. */
    /// @brief Query the latest completed GPU frame duration.
    /// @param[in] ctx Backend context.
    /// @return Duration in microseconds, or zero before a result is available.
    uint64_t (*get_frame_gpu_time_us)(void *ctx);
    /* Optional backend texture capability bits. Return a mask using
     * RT_CANVAS3D_BACKEND_CAP_BC1/BC3/BC4/BC5/BC7/ASTC/ETC2 for native block
     * upload and RT_CANVAS3D_BACKEND_CAP_ANISOTROPY when sampler anisotropy is
     * supported. */
    /// @brief Query native compressed-texture and sampler capabilities.
    /// @param[in] ctx Backend context.
    /// @return Bitwise OR of supported RT_CANVAS3D_BACKEND_CAP_* texture flags.
    int64_t (*get_native_texture_caps)(void *ctx);

    /* Optional backend feature capability bits. Return a mask using
     * RT_CANVAS3D_BACKEND_CAP_* values for features that depend on the concrete
     * device/context rather than only vtable hooks. Canvas3D ORs these bits into
     * BackendCapabilities after checking its generic software and hook-based
     * fallbacks. */
    /// @brief Query device-dependent backend feature capabilities.
    /// @param[in] ctx Backend context.
    /// @return Bitwise OR of supported RT_CANVAS3D_BACKEND_CAP_* feature flags.
    int64_t (*get_feature_caps)(void *ctx);

    /// @brief Optional backend diagnostics hook.
    /// @details Copies a point-in-time telemetry snapshot into @p out_stats. NULL means the
    ///          backend has no private telemetry beyond Canvas3D's existing frame counters.
    /// @param[in] ctx Backend context.
    /// @param[out] out_stats Caller-owned telemetry snapshot destination.
    void (*get_backend_stats)(void *ctx, vgfx3d_backend_stats_t *out_stats);

    /* Reversed-Z depth: when non-zero, Canvas3D negates the projection's z row for
     * every pass it sends this backend (scene, view-model, 2D/overlay ortho), and the
     * backend clears depth to 0 with Greater-style compares. Float depth precision then
     * concentrates where standard-Z starves it, eliminating distant z-fighting shimmer.
     * Shadow-map rendering keeps the standard convention on every backend (light VPs are
     * CPU-built and sampled with LessEqual compares). The software backend stays
     * standard as the deterministic golden reference. */
    /// Nonzero when scene passes use reversed-Z projection and depth comparison.
    int8_t reversed_z;

    /* Optional present pacing control. Non-zero synchronizes presentation to the
     * display's vertical blank (the default on every backend); zero presents
     * immediately for lowest latency. NULL = the platform default is fixed. */
    /// @brief Enable or disable synchronization of presentation to vertical blank.
    /// @param[in,out] ctx Backend context.
    /// @param[in] enabled Nonzero to enable synchronized presentation.
    void (*set_vsync)(void *ctx, int8_t enabled);

    /* Optional render-scale control: window-backed scene rendering happens at
     * `scale` times the output size ([0.25, 1]) and is upscaled at presentation.
     * Returns non-zero when the scale was applied. NULL = fixed 1:1 rendering
     * (Canvas3D.TrySetRenderScale reports unsupported). */
    /// @brief Set the window-backed internal scene resolution scale.
    /// @param[in,out] ctx Backend context.
    /// @param[in] scale Requested scale in the inclusive range 0.25-1.0.
    /// @return Nonzero when the backend accepted and applied the scale.
    int8_t (*set_render_scale)(void *ctx, float scale);

    /* Optional capture-after-present support. Nonzero asks the on-screen
     * present path to preserve the presented image in a readable texture
     * BEFORE handing the drawable to the display, so screenshot readback
     * after Present() returns the shown frame instead of undefined
     * contents (costs one full-screen blit per presented frame). NULL =
     * the backend's readback is already present-safe (software renderer,
     * offscreen/composited routes). */
    /// @brief Enable or disable pre-present capture of the on-screen image.
    /// @param[in,out] ctx Backend context.
    /// @param[in] enabled Nonzero to capture every presented frame for readback.
    void (*set_capture_after_present)(void *ctx, int8_t enabled);

    /* Optional active-target depth probes for occlusion-aware effects (lens flares).
     * queue_depth_probe registers one NDC point (x, y in [-1, 1]) during frame
     * building and returns its slot id, or -1 when unsupported/full. Slot ids
     * restart at 0 each frame, so a stable per-frame request order yields stable
     * slots. read_depth_probe returns canonical window depth ([0, 1], larger =
     * farther) from the active window/scene/RTT destination, captured for that
     * slot on a PREVIOUS completed frame, or a negative value while no result is
     * available. GPU backends read the depth back asynchronously (typically one
     * frame of latency, never a pipeline stall); the software backend answers
     * from the active CPU depth buffer. */
    /// @brief Queue one normalized-device-coordinate depth sample for asynchronous resolution.
    /// @param[in,out] ctx Backend context.
    /// @param[in] ndc_x Horizontal normalized-device coordinate in the range -1 to 1.
    /// @param[in] ndc_y Vertical normalized-device coordinate in the range -1 to 1.
    /// @return Per-frame probe slot, or -1 when unsupported or full.
    int32_t (*queue_depth_probe)(void *ctx, float ndc_x, float ndc_y);
    /// @brief Read a depth-probe result from a previously completed frame.
    /// @param[in] ctx Backend context.
    /// @param[in] slot Slot returned by @ref queue_depth_probe.
    /// @return Canonical active-target depth in the range 0-1, or a negative value when
    /// unavailable.
    float (*read_depth_probe)(void *ctx, int32_t slot);
} vgfx3d_backend_t;

/// @brief Maximum scene-depth probes per frame across all users of the hook.
#define VGFX3D_DEPTH_PROBE_MAX 64

/*==========================================================================
 * Backend registry
 *=========================================================================*/

/// @brief The always-available CPU software backend (fallback of last resort).
extern const vgfx3d_backend_t vgfx3d_software_backend;

/// @brief Software backend: read-only NDC depth buffer view for CPU post-FX
///   (NULL when unavailable). Only valid for the software backend's context.
/// @param[in] ctx Software backend context.
/// @param[out] out_w Optional destination for the depth-buffer width.
/// @param[out] out_h Optional destination for the depth-buffer height.
/// @return Borrowed row-major depth buffer, or NULL when unavailable.
const float *vgfx3d_sw_get_zbuf(void *ctx, int32_t *out_w, int32_t *out_h);

#if RT_PLATFORM_MACOS
/// @brief The Metal GPU backend (macOS only).
extern const vgfx3d_backend_t vgfx3d_metal_backend;
#endif
#if RT_PLATFORM_WINDOWS
/// @brief The Direct3D 11 GPU backend (Windows only).
extern const vgfx3d_backend_t vgfx3d_d3d11_backend;
#endif
#if RT_PLATFORM_LINUX && !defined(ZANNA_GRAPHICS_HEADLESS)
/// @brief The OpenGL GPU backend (Linux only).
extern const vgfx3d_backend_t vgfx3d_opengl_backend;
#endif

/// @brief Platform policy buckets used by deterministic default-backend selection.
typedef enum {
    /// Unsupported or unrecognized host platform.
    VGFX3D_BACKEND_PLATFORM_OTHER = 0,
    /// macOS host where Metal is preferred.
    VGFX3D_BACKEND_PLATFORM_MACOS = 1,
    /// Windows x86/x64 host where D3D11 is preferred.
    VGFX3D_BACKEND_PLATFORM_WINDOWS = 2,
    /// Windows ARM64 host currently constrained to software rendering.
    VGFX3D_BACKEND_PLATFORM_WINDOWS_ARM64 = 3,
    /// Linux desktop host where OpenGL is preferred.
    VGFX3D_BACKEND_PLATFORM_LINUX = 4,
} vgfx3d_backend_platform_t;

/// @brief Return the default backend name for a platform policy bucket.
/// @param[in] platform Platform policy bucket.
/// @return Static preferred backend name, falling back to `software`.
static inline const char *vgfx3d_default_backend_name_for_platform(
    vgfx3d_backend_platform_t platform) {
    switch (platform) {
        case VGFX3D_BACKEND_PLATFORM_MACOS:
            return "metal";
        case VGFX3D_BACKEND_PLATFORM_WINDOWS:
            return "d3d11";
        case VGFX3D_BACKEND_PLATFORM_WINDOWS_ARM64:
            return "software";
        case VGFX3D_BACKEND_PLATFORM_LINUX:
            return "opengl";
        case VGFX3D_BACKEND_PLATFORM_OTHER:
        default:
            return "software";
    }
}

/// @brief Select the best available backend: try the platform GPU backend first, then
///   fall back to the software backend. Returns a borrowed pointer to a static vtable.
/// @return Borrowed immutable vtable for the selected backend.
const vgfx3d_backend_t *vgfx3d_select_backend(void);

#endif /* ZANNA_ENABLE_GRAPHICS */
