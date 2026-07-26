//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11.c
// Purpose: Direct3D 11 GPU backend for Zanna.Graphics3D (Windows).
//
// Key invariants:
//   - Requires Windows 7+ with D3D11 feature level 11_0
//   - Falls back to software if D3D11 unavailable
//   - HLSL shaders compiled at runtime via D3DCompile
//   - row_major float4x4 in HLSL (matches Zanna row-major convention)
//   - Constant buffers packed via vgfx3d_backend_d3d11_shared float4 alignment.
//
// Ownership/Lifetime:
//   - D3D11 device, swapchain, RTVs, DSVs, samplers, blend states, depth
//     states, shaders, and per-mesh GPU caches are owned by the backend
//     context and released in destroy_ctx.
//
// Links: vgfx3d_backend.h, vgfx3d_backend_d3d11_shared.h, plans/3d/03-d3d11-backend.md
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements the Windows Direct3D 11 Graphics3D backend.
///
/// This translation unit owns D3D11 device and swap-chain lifecycle, shader
/// compilation, render-target and shadow resources, draw/texture/morph caches,
/// streaming uploads, post-processing, presentation, readback, depth probes,
/// and backend telemetry. Feature sections are split into implementation
/// fragments included near the end of this file and share the private context
/// and helpers declared here.

#if defined(_WIN32) && defined(ZANNA_ENABLE_GRAPHICS)

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>

#include "rt_textureasset3d.h"
#include "vgfx.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_d3d11_shared.h"
#include "vgfx3d_backend_utils.h"
#include "vgfx3d_brdf_lut.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#define VGFX3D_STR_IMPL(x) #x
#define VGFX3D_STR(x) VGFX3D_STR_IMPL(x)

#ifndef D3D11CalcSubresource
#define D3D11CalcSubresource(MipSlice, ArraySlice, MipLevels)                                      \
    ((MipSlice) + ((ArraySlice) * (MipLevels)))
#endif

static const float k_identity4x4[16] = {
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
};

#include "vgfx3d_backend_d3d11_shaders.inc"

/// @brief One cached 2D texture or native TextureAsset3D upload and its streaming cursor.
typedef struct {
    const void *pixels_ptr;
    void *texture_asset;
    uint64_t generation;
    uint64_t pending_generation;
    uint64_t failed_generation;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    int32_t width;
    int32_t height;
    int32_t upload_next_row;
    int32_t native_format;
    int32_t native_next_block_row;
    int64_t native_next_mip;
    int64_t native_mip_start;
    int64_t native_mip_count;
    uint64_t pending_native_bytes;
    int8_t upload_in_progress;
    uint64_t last_used_frame;
} d3d_tex_cache_entry_t;

/// @brief One cached CubeMap3D resource and its face/row streaming state.
typedef struct {
    const void *cubemap_ptr;
    uint64_t generation;
    uint64_t pending_generation;
    uint64_t failed_generation;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    int32_t face_size;
    int32_t upload_face;
    int32_t upload_next_row;
    int8_t upload_in_progress;
    uint64_t last_used_frame;
    uint64_t applied_ibl_identity; /* prefiltered-mips overlay applied for this IBL payload */
    uint64_t failed_ibl_identity;
    uint64_t pending_ibl_identity;
    uint64_t pending_ibl_bytes;
} d3d_cubemap_cache_entry_t;

/// @brief Shader-resource view borrowed from a cache or temporarily owned by one draw.
typedef struct {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    int temporary;
} d3d_temp_srv_t;

/// @brief Complete set of temporary and cached shader resources resolved for one draw.
typedef struct {
    d3d_temp_srv_t textures[11];
    d3d_temp_srv_t cubemap;
    int has_texture;
    int has_normal_map;
    int has_specular_map;
    int has_emissive_map;
    int has_env_map;
    int has_splat;
    int has_metallic_roughness_map;
    int has_ao_map;
} d3d_draw_resources_t;

typedef vgfx3d_d3d11_per_object_t d3d_per_object_t;

_Static_assert(sizeof(d3d_per_object_t) == 752u,
               "D3D11 PerObject cbuffer must match its HLSL layout");

/// @brief CPU mirror of the D3D11 per-scene constant buffer.
typedef struct {
    float vp[16];
    float prev_vp[16];
    float shadow_vp[VGFX3D_MAX_SHADOW_LIGHTS][16];
    float camera_pos[4];
    float ambient[4];
    float fog_color[4];
    float fog_near;
    float fog_far;
    float shadow_bias;
    int32_t light_count;
    int32_t shadow_count;
    float camera_forward[3];
    float ibl_intensity;
    float _ibl_pad[3];
    float sh[9][4]; /* SH-9 RGB irradiance, one coefficient per float4 */
    /* Plan 06 shadow knobs: x = slope-bias factor, y = strength, z = tap count. */
    float shadow_filter[4];
    /* Track E doc 07: exponential height fog (x = base, y = falloff,
     * z = density*blend with 0 = off, w = pad). Position mirrors the HLSL
     * cbuffer exactly. */
    float height_fog[4];
    /* Plan 07: x/y = viewport size, z/w = znear/zfar; global count -1 = flat loop. */
    float cluster_params[4];
    int32_t cluster_global_count;
    float _cluster_pad1[3];
    /* Height-fog sun inscattering (appended; earlier offsets unchanged):
     * height_fog_sun = tint rgb + amount (0 = off);
     * height_fog_sun_dir = direction toward the sun + power. */
    float height_fog_sun[4];
    float height_fog_sun_dir[4];
} d3d_per_scene_t;

_Static_assert(offsetof(d3d_per_scene_t, sh) == offsetof(d3d_per_scene_t, ibl_intensity) + 16,
               "D3D11 PerScene cbuffer must pad after iblIntensity before float4 arrays");
_Static_assert(offsetof(d3d_per_scene_t, height_fog) % 16 == 0,
               "D3D11 PerScene float4 fields must stay 16-byte aligned");
_Static_assert(sizeof(d3d_per_scene_t) % 16 == 0,
               "D3D11 PerScene cbuffer size must be a 16-byte multiple");

typedef vgfx3d_d3d11_per_material_t d3d_per_material_t;

_Static_assert(sizeof(d3d_per_material_t) == 448u,
               "D3D11 PerMaterial cbuffer must match its HLSL layout");

/// @brief CPU mirror of one packed D3D11 light constant-buffer element.
typedef struct {
    int32_t type;
    int32_t shadow_index;
    int32_t shadow_cascade_count;
    int32_t shadow_projection_type;
    float direction[4];
    float position[4];
    float color[4];
    float intensity;
    float attenuation;
    float inner_cos;
    float outer_cos;
    float shadow_cascade_splits[4];
    float basis_u[4];
    float basis_v[4];
    float shape[4]; /* width, height, radius, range */
    int32_t decay_type;
    int32_t emitter_pad[3];
} d3d_light_t;

_Static_assert(sizeof(d3d_light_t) == 160u,
               "D3D11 Light cbuffer element must match its HLSL layout");
_Static_assert(VGFX3D_MAX_SHADOW_LIGHTS - VGFX3D_CSM_SLOTS ==
                   VGFX3D_D3D11_SHADOW_ATLAS_COLUMNS * VGFX3D_D3D11_SHADOW_ATLAS_ROWS,
               "D3D11 shadow atlas grid must cover every non-CSM shadow slot");
_Static_assert(VGFX3D_SHADOW_CUBE_FACES <=
                   VGFX3D_D3D11_SHADOW_ATLAS_COLUMNS * VGFX3D_D3D11_SHADOW_ATLAS_ROWS,
               "D3D11 shadow atlas must fit one point-light cube");

/// @brief CPU mirror of the D3D11 skybox constant buffer.
typedef struct {
    float inverse_projection[16];
    float inverse_view_rotation[16];
    float camera_forward[4];
} d3d_skybox_cb_t;

_Static_assert(sizeof(d3d_skybox_cb_t) == 144u, "D3D11 Skybox cbuffer must match its HLSL layout");

/// @brief CPU mirror of the D3D11 post-processing constant buffer.
typedef struct {
    float inv_vp[16];
    float prev_vp[16];
    float camera_pos[4];
    float inv_resolution[2];
    int32_t bloom_enabled;
    float bloom_threshold;
    float bloom_intensity;
    int32_t bloom_passes;
    int32_t tonemap_mode;
    float tonemap_exposure;
    int32_t fxaa_enabled;
    int32_t color_grade_enabled;
    float cg_brightness;
    float cg_contrast;
    float cg_saturation;
    int32_t vignette_enabled;
    float vignette_radius;
    float vignette_softness;
    int32_t ssao_enabled;
    float ssao_radius;
    float ssao_intensity;
    int32_t ssao_samples;
    int32_t dof_enabled;
    float dof_focus_distance;
    float dof_aperture;
    float dof_max_blur;
    int32_t motion_blur_enabled;
    float motion_blur_intensity;
    int32_t motion_blur_samples;
    /* Plan 05 additions (tail rounds the cbuffer to a 16-byte multiple). */
    int32_t scene_is_hdr;
    int32_t tonemap_explicit;
    int32_t bloom_tex_enabled;
    float _pad0;
    float _pad1;
} d3d_postfx_cb_t;

_Static_assert(offsetof(d3d_postfx_cb_t, inv_resolution) == 144u,
               "D3D11 PostFX cbuffer matrices must end on an HLSL register boundary");
_Static_assert(offsetof(d3d_postfx_cb_t, scene_is_hdr) == 252u,
               "D3D11 PostFX tail flags must match their HLSL offsets");
_Static_assert(sizeof(d3d_postfx_cb_t) == 272u, "D3D11 PostFX cbuffer must match its HLSL layout");

/* Plan 05: bloom pass constants (must match HLSL BloomCB). */
/// @brief CPU mirror of the bloom downsample/upsample pass constants.
typedef struct {
    float src_inv_size[2];
    float threshold;
    int32_t first_pass;
} d3d_bloom_cb_t;

_Static_assert(sizeof(d3d_bloom_cb_t) == 16u, "D3D11 Bloom cbuffer must match its HLSL layout");

/* Plan 05: TAA resolve constants (must match HLSL TAACB). */
/// @brief CPU mirror of temporal anti-aliasing resolve constants and history state.
typedef struct {
    float inv_vp[16];
    float prev_vp[16];
    float inv_resolution[2];
    float jitter_delta[2];
    float blend;
    int32_t history_valid;
    float _pad0[2];
} d3d_taa_cb_t;

_Static_assert(sizeof(d3d_taa_cb_t) == 160u, "D3D11 TAA cbuffer must match its HLSL layout");

/* Plan 10: SSR constants (must match HLSL SSRCB). */
/// @brief CPU mirror of screen-space reflection reconstruction and tracing constants.
typedef struct {
    float inv_vp[16];
    float vp[16];
    float cam_pos[4];
    float params0[4]; /* x = intensity, y = max roughness (reserved), z = steps */
    float inv_resolution[2];
    float _pad0[2];
} d3d_ssr_cb_t;

_Static_assert(sizeof(d3d_ssr_cb_t) == 176u, "D3D11 SSR cbuffer must match its HLSL layout");

typedef vgfx3d_d3d11_instance_data_t d3d_instance_data_t;

#define D3D11_MESH_CACHE_CAPACITY 256

/// @brief One static mesh cache slot containing immutable vertex and index buffers.
typedef struct {
    const void *key;
    uint32_t revision;
    uint32_t vertex_count;
    uint32_t index_count;
    ID3D11Buffer *vb;
    ID3D11Buffer *ib;
    uint64_t last_used_frame;
    int8_t compact; /* R20: VB holds the packed 48-byte vertex encoding */
} d3d11_mesh_cache_entry_t;

#define D3D11_MORPH_CACHE_CAPACITY 32
#define D3D11_TEXTURE_CACHE_MAX_RESIDENT 512
#define D3D11_TEXTURE_CACHE_MAX_ENTRIES 4096
#define D3D11_TEXTURE_CACHE_HINT_CAPACITY 1024
_Static_assert((D3D11_TEXTURE_CACHE_HINT_CAPACITY & (D3D11_TEXTURE_CACHE_HINT_CAPACITY - 1)) == 0,
               "texture-cache hint capacity must be a power of two");
#define D3D11_TEXTURE_CACHE_PRUNE_AGE 600u
#define D3D11_CUBEMAP_CACHE_MAX_RESIDENT 64
#define D3D11_CUBEMAP_CACHE_MAX_ENTRIES 256
#define D3D11_CUBEMAP_CACHE_PRUNE_AGE 240u

/// @brief One cached morph-target position/normal shader-resource payload.
typedef struct {
    const void *key;
    uint64_t generation;
    uint32_t vertex_count;
    uint32_t shape_count;
    int has_normal_deltas;
    ID3D11Buffer *buffer;
    ID3D11ShaderResourceView *srv;
    size_t element_count;
    ID3D11Buffer *normal_buffer;
    ID3D11ShaderResourceView *normal_srv;
    size_t normal_element_count;
    uint64_t last_used_frame;
} d3d11_morph_cache_entry_t;

/// @brief Complete backend-owned Direct3D 11 renderer context.
///
/// The context groups COM resources, CPU staging buffers, cache tables,
/// pass-routing state, temporal history, shadow slots, streaming counters, and
/// the current camera/frame snapshot. d3d11_destroy_ctx() releases every owned
/// COM interface and heap allocation.
typedef struct {
    ID3D11Device *device;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *swap_chain;
    ID3D11RenderTargetView *rtv;
    ID3D11Texture2D *depth_tex;
    ID3D11DepthStencilView *dsv;

    ID3D11BlendState *blend_state_opaque;
    ID3D11BlendState *blend_state_alpha;
    ID3D11BlendState *blend_state_additive;
    ID3D11BlendState *blend_state_premultiplied_alpha;
    ID3D11DepthStencilState *depth_state;
    ID3D11DepthStencilState *depth_state_no_write;
    ID3D11DepthStencilState *depth_state_disabled;
    ID3D11DepthStencilState *depth_state_skybox;
    ID3D11DepthStencilState *depth_state_shadow; /* standard Less for the shadow pass */
    ID3D11RasterizerState *rs_solid_cull;
    ID3D11RasterizerState *rs_solid_no_cull;
    ID3D11RasterizerState *rs_wire_cull;
    ID3D11RasterizerState *rs_wire_no_cull;
    ID3D11RasterizerState *rs_depth_biased_cached;
    INT rs_depth_biased_depth_bias;
    float rs_depth_biased_slope_bias;
    int8_t rs_depth_biased_wireframe;
    int8_t rs_depth_biased_backface_cull;
    int8_t rs_depth_biased_reversed_z;
    int8_t rs_depth_biased_valid;
    ID3D11SamplerState *linear_wrap_sampler;
    ID3D11SamplerState *linear_clamp_sampler;
    ID3D11SamplerState *shadow_cmp_sampler;
    ID3D11SamplerState *material_samplers[3][3][12][VGFX3D_D3D11_ANISOTROPY_LEVEL_COUNT];
    ID3D11Texture2D *fallback_white_tex;
    ID3D11ShaderResourceView *fallback_white_srv;
    ID3D11Texture2D *fallback_white_cube_tex;
    ID3D11ShaderResourceView *fallback_white_cube_srv;
    ID3D11Texture2D *brdf_lut_tex;
    ID3D11ShaderResourceView *brdf_lut_srv;

    ID3D11VertexShader *vs_main;
    ID3D11VertexShader *vs_instanced;
    ID3D11VertexShader *vs_particles;
    ID3D11PixelShader *ps_main;
    ID3D11VertexShader *vs_shadow;
    ID3D11PixelShader *ps_shadow;
    ID3D11VertexShader *vs_skybox;
    ID3D11PixelShader *ps_skybox;
    ID3D11VertexShader *vs_postfx;
    ID3D11PixelShader *ps_postfx;
    ID3D11PixelShader *ps_overlay_composite;
    /* Plan 05: bloom mip-chain + TAA resolve pixel shaders. */
    ID3D11PixelShader *ps_bloom_down;
    ID3D11PixelShader *ps_bloom_up;
    ID3D11PixelShader *ps_taa;
    ID3D11PixelShader *ps_ssr;

    ID3D11InputLayout *input_layout;
    ID3D11InputLayout *input_layout_instanced;
    ID3D11InputLayout *input_layout_particles;
    ID3D11InputLayout *input_layout_skybox;
    /* R20 compact-vertex-stream twins (48-byte packed static-cache layout).
     * NULL when creation failed; the cache and draw paths then keep the full
     * layout, so availability is an all-or-nothing gate. */
    ID3D11InputLayout *input_layout_compact;
    ID3D11InputLayout *input_layout_instanced_compact;

    ID3D11Buffer *cb_per_object;
    ID3D11Buffer *cb_per_scene;
    ID3D11Buffer *cb_per_material;
    ID3D11Buffer *cb_per_lights;
    /* Plan 07: froxel table (u16 data packed as uint4 lanes, two u16 per uint). */
    ID3D11Buffer *cb_cluster_offsets;
    ID3D11Buffer *cb_cluster_indices;
    /* Plan 10: opaque-pass depth snapshot (CopyResource at the seam) for soft
     * particles; valid resets every begin_frame. */
    ID3D11Texture2D *opaque_depth_tex;
    ID3D11ShaderResourceView *opaque_depth_srv;
    int32_t opaque_depth_w, opaque_depth_h;
    int8_t opaque_depth_valid;
    float cam_znear, cam_zfar;
    ID3D11Buffer *cb_bones;
    ID3D11Buffer *cb_prev_bones;
    ID3D11Buffer *cb_skybox;
    ID3D11Buffer *cb_postfx;
    ID3D11Buffer *cb_bloom;
    ID3D11Buffer *cb_taa;
    ID3D11Buffer *cb_ssr;

    ID3D11Buffer *dynamic_vb;
    ID3D11Buffer *dynamic_ib;
    ID3D11Buffer *instance_buffer;
    ID3D11Buffer *skybox_vb;
    size_t dynamic_vb_size;
    size_t dynamic_ib_size;
    size_t instance_buffer_size;
    d3d_instance_data_t *instance_upload_data;
    size_t instance_upload_capacity;

    ID3D11Buffer *morph_buffer;
    ID3D11ShaderResourceView *morph_srv;
    size_t morph_buffer_size;
    ID3D11Buffer *morph_normal_buffer;
    ID3D11ShaderResourceView *morph_normal_srv;
    size_t morph_normal_buffer_size;
    ID3D11ShaderResourceView *current_morph_srv;
    ID3D11ShaderResourceView *current_morph_normal_srv;
    d3d11_morph_cache_entry_t morph_cache[D3D11_MORPH_CACHE_CAPACITY];

    ID3D11Texture2D *scene_color_tex;
    ID3D11RenderTargetView *scene_color_rtv;
    ID3D11ShaderResourceView *scene_color_srv;
    ID3D11Texture2D *scene_motion_tex;
    ID3D11RenderTargetView *scene_motion_rtv;
    ID3D11ShaderResourceView *scene_motion_srv;
    ID3D11Texture2D *scene_depth_tex;
    ID3D11DepthStencilView *scene_dsv;
    ID3D11ShaderResourceView *scene_depth_srv;
    ID3D11Texture2D *overlay_color_tex;
    ID3D11RenderTargetView *overlay_color_rtv;
    ID3D11ShaderResourceView *overlay_color_srv;
    ID3D11Texture2D *postfx_color_tex;
    ID3D11RenderTargetView *postfx_color_rtv;
    ID3D11ShaderResourceView *postfx_color_srv;
    ID3D11Texture2D *postfx_scratch_tex;
    ID3D11RenderTargetView *postfx_scratch_rtv;
    ID3D11ShaderResourceView *postfx_scratch_srv;
    /* Plan 05: half-res RGBA16F bloom mip chain (rebuilt on scene-size change). */
    ID3D11Texture2D *bloom_mip_tex[6];
    ID3D11RenderTargetView *bloom_mip_rtv[6];
    ID3D11ShaderResourceView *bloom_mip_srv[6];
    int32_t bloom_mip_w[6];
    int32_t bloom_mip_h[6];
    int32_t bloom_mip_count;
    int32_t bloom_base_width;
    int32_t bloom_base_height;
    /* Transient: mip-0 SRV bound at t4 while a chain pass composites bloom. */
    ID3D11ShaderResourceView *postfx_current_bloom_srv;
    /* Plan 05: TAA ping-pong history (RGBA16F, persisted across frames) + jitter state. */
    ID3D11Texture2D *taa_history_tex[2];
    ID3D11RenderTargetView *taa_history_rtv[2];
    /* Plan 10: SSR output target (scene-sized, scene color format). */
    ID3D11Texture2D *ssr_tex;
    ID3D11RenderTargetView *ssr_rtv;
    ID3D11ShaderResourceView *ssr_srv;
    int32_t ssr_width, ssr_height;
    ID3D11ShaderResourceView *taa_history_srv[2];
    int32_t taa_history_width;
    int32_t taa_history_height;
    int32_t taa_history_parity;
    int8_t taa_history_valid;
    float taa_jitter_clip[2];
    float taa_prev_jitter_clip[2];
    uint32_t taa_frame_index;
    ID3D11Texture2D *presented_color_tex;
    ID3D11Texture2D *readback_staging;
    int32_t scene_width;
    int32_t scene_height;
    int32_t overlay_width;
    int32_t overlay_height;
    int32_t postfx_width;
    int32_t postfx_height;
    int32_t postfx_scratch_width;
    int32_t postfx_scratch_height;
    int32_t presented_width;
    int32_t presented_height;
    int32_t readback_staging_width;
    int32_t readback_staging_height;
    DXGI_FORMAT readback_staging_format;

    /* Present pacing: IDXGISwapChain::Present sync interval (1 = vsync, 0 = immediate).
     * Initialized to 1 at context creation; driven by the set_vsync backend hook. */
    int32_t present_sync_interval;

    /* Scene-depth probes (lens flares): requests queued during the frame are copied
     * from scene_depth_tex into a small staging strip at end_frame; the previous
     * frame's strip is harvested at begin_frame with a non-blocking Map, so reads
     * carry one frame of latency and never sync the pipeline. */
    float depth_probe_requests[VGFX3D_DEPTH_PROBE_MAX][2];
    int32_t depth_probe_request_count;
    ID3D11Texture2D *depth_probe_staging;
    int32_t depth_probe_pending_count;
    uint32_t depth_probe_pending_polls;
    float depth_probe_results[VGFX3D_DEPTH_PROBE_MAX];
    int32_t depth_probe_result_count;

    ID3D11Texture2D *rtt_color_tex;
    ID3D11RenderTargetView *rtt_rtv;
    ID3D11Texture2D *rtt_depth_tex;
    ID3D11DepthStencilView *rtt_dsv;
    ID3D11Texture2D *rtt_staging;
    int32_t rtt_width;
    int32_t rtt_height;
    int32_t rtt_color_format;
    int8_t rtt_active;
    vgfx3d_rendertarget_t *rtt_target;

    ID3D11Texture2D *shadow_depth_tex[VGFX3D_MAX_SHADOW_LIGHTS];
    ID3D11DepthStencilView *shadow_dsv[VGFX3D_MAX_SHADOW_LIGHTS];
    ID3D11ShaderResourceView *shadow_srv[VGFX3D_MAX_SHADOW_LIGHTS];
    int32_t shadow_width[VGFX3D_MAX_SHADOW_LIGHTS];
    int32_t shadow_height[VGFX3D_MAX_SHADOW_LIGHTS];
    int32_t shadow_pass_slot;
    int8_t shadow_pass_failed;
    int32_t shadow_count;
    float shadow_vp[VGFX3D_MAX_SHADOW_LIGHTS][16];
    /* Shadow atlas for slots >= VGFX3D_CSM_SLOTS: 4x2 tiles at the per-slot
     * resolution, sampled at t17; cleared once per frame on first tile pass. */
    ID3D11Texture2D *shadow_atlas_tex;
    ID3D11DepthStencilView *shadow_atlas_dsv;
    ID3D11ShaderResourceView *shadow_atlas_srv;
    int32_t shadow_atlas_w;
    int32_t shadow_atlas_h;
    int8_t shadow_atlas_cleared;
    /* Per-slot render completeness (atlas slots have no per-slot resources). */
    int8_t shadow_slot_complete[VGFX3D_MAX_SHADOW_LIGHTS];
    float shadow_bias;
    /* Plan 06: per-frame shadow filtering params from camera params. */
    float shadow_strength;
    float shadow_slope_bias;
    int32_t shadow_quality;

    d3d_tex_cache_entry_t *tex_cache;
    int32_t tex_cache_count;
    int32_t tex_cache_capacity;
    int32_t tex_cache_hints[D3D11_TEXTURE_CACHE_HINT_CAPACITY]; /* entry index + 1 */
    d3d_cubemap_cache_entry_t *cubemap_cache;
    int32_t cubemap_cache_count;
    int32_t cubemap_cache_capacity;
    d3d11_mesh_cache_entry_t mesh_cache[D3D11_MESH_CACHE_CAPACITY];
    uint64_t frame_serial;
    uint64_t texture_upload_bytes;
    uint64_t texture_upload_budget_bytes;
    vgfx3d_backend_stats_t stats;

    ID3D11RenderTargetView *current_rtvs[2];
    UINT current_rtv_count;
    ID3D11DepthStencilView *current_dsv;
    vgfx3d_d3d11_target_kind_t current_target_kind;
    vgfx3d_d3d11_target_kind_t active_target_kind;
    int32_t current_width;
    int32_t current_height;

    int32_t width;
    int32_t height;
    float render_scale;
    float view[16];
    float projection[16];
    float vp[16];
    float inv_vp[16];
    float draw_prev_vp[16];
    float scene_vp[16];
    float scene_prev_vp[16];
    float scene_inv_vp[16];
    int8_t scene_history_valid;
    float cam_pos[3];
    float cam_forward[3];
    int8_t cam_is_ortho;
    float scene_cam_pos[3];
    float clear_r;
    float clear_g;
    float clear_b;
    int8_t fog_enabled;
    float height_fog[4];         /* base, falloff, density*blend (0 = off), pad */
    float height_fog_sun[4];     /* sun tint rgb + amount (0 = off) */
    float height_fog_sun_dir[4]; /* direction toward the sun + power */
    float fog_near;
    float fog_far;
    float fog_color[3];
    int8_t ibl_enabled;
    float ibl_intensity;
    float ibl_sh[27];
    /* Light-snapshot revision last written to cbPerScene/cbPerLights. DISCARD
     * mapping renames the buffers per write, so skipping unchanged revisions
     * is safe for earlier in-flight draws. */
    uint32_t uploaded_lights_revision;
    int8_t gpu_postfx_enabled;
    int8_t gpu_postfx_chain_valid;
    int8_t frame_active;
    int8_t frame_pending_present;
    int8_t current_pass_is_overlay;
    int8_t current_load_existing_color;
    int8_t overlay_used_this_frame;
    int8_t scene_composited_to_swapchain;
    int8_t presented_color_valid;
    vgfx3d_postfx_chain_t gpu_postfx_chain;
    ID3D11Query *frame_time_disjoint_query;
    ID3D11Query *frame_time_start_query;
    ID3D11Query *frame_time_end_query;
    uint64_t frame_gpu_time_us;
    int8_t frame_time_active;
    int8_t frame_time_pending;
    uint32_t frame_time_pending_polls;
} d3d11_context_t;

#define SAFE_RELEASE(x)                                                                            \
    do {                                                                                           \
        if (x) {                                                                                   \
            IUnknown_Release((IUnknown *)(x));                                                     \
            (x) = NULL;                                                                            \
        }                                                                                          \
    } while (0)

/// @brief Normalize a COM factory result that promises a required output interface.
/// @details A successful HRESULT with a null output is not usable by the backend and must not be
///   published as success. Keeping this check in one helper makes every required D3D11 creation
///   path follow the same output contract, including defensive handling of faulty proxy drivers.
/// @param[in] hr HRESULT returned by the COM factory call.
/// @param[in] output Required output-interface pointer written by that call.
/// @return @p hr on failure, S_OK for a non-NULL success output, or E_POINTER otherwise.
static HRESULT d3d11_required_output_result(HRESULT hr, const void *output) {
    if (FAILED(hr))
        return hr;
    return output ? S_OK : E_POINTER;
}

#define D3D11_INITIAL_DYNAMIC_VB_SIZE (4u * 1024u * 1024u)
#define D3D11_INITIAL_DYNAMIC_IB_SIZE (1u * 1024u * 1024u)
#define D3D11_INITIAL_INSTANCE_BUFFER_SIZE (256u * 1024u)

static void d3d11_destroy_ctx(void *ctx_ptr);
static void d3d11_log_hresult(const char *msg, HRESULT hr);
static void d3d11_log_shader_diagnostics(const char *stage, ID3DBlob *diagnostics, int failed);
static void d3d11_present_swapchain(d3d11_context_t *ctx);
static int d3d11_snapshot_backbuffer_for_readback(d3d11_context_t *ctx);
static void d3d11_bind_render_targets(d3d11_context_t *ctx);
static void d3d11_unbind_draw_resources(d3d11_context_t *ctx);
static void d3d11_release_swapchain_main_targets(d3d11_context_t *ctx);
static void d3d11_release_readback_staging_texture(d3d11_context_t *ctx);
static HRESULT d3d11_recreate_swapchain_main_targets(d3d11_context_t *ctx,
                                                     int32_t width,
                                                     int32_t height,
                                                     const char *log_context);
static void d3d11_destroy_opaque_depth_target(d3d11_context_t *ctx);
static void d3d11_begin_frame_timing(d3d11_context_t *ctx);
static void d3d11_end_frame_timing(d3d11_context_t *ctx);
static void d3d11_release_texture_cache(d3d11_context_t *ctx);
static void d3d11_prune_texture_cache(d3d11_context_t *ctx);
static void d3d11_release_cubemap_cache(d3d11_context_t *ctx);
static void d3d11_prune_cubemap_cache(d3d11_context_t *ctx);
static float d3d11_cubemap_max_lod(const rt_cubemap3d *cubemap);
static void d3d11_release_morph_cache_entry(d3d11_morph_cache_entry_t *entry);
static void d3d11_release_morph_cache(d3d11_context_t *ctx);
static void d3d11_release_temp_srv(d3d_temp_srv_t *entry);
static ID3D11ShaderResourceView *d3d11_get_or_create_srv(d3d11_context_t *ctx,
                                                         const void *pixels,
                                                         d3d_temp_srv_t *out_temp);
static ID3D11ShaderResourceView *d3d11_get_or_create_material_srv(d3d11_context_t *ctx,
                                                                  void *asset,
                                                                  const void *pixels,
                                                                  uint64_t asset_cache_key,
                                                                  int64_t mip_start,
                                                                  int64_t mip_count,
                                                                  d3d_temp_srv_t *out_temp);
static ID3D11ShaderResourceView *d3d11_get_or_create_cubemap_srv(d3d11_context_t *ctx,
                                                                 const rt_cubemap3d *cubemap,
                                                                 d3d_temp_srv_t *out_temp);
static HRESULT d3d11_create_staging_texture(d3d11_context_t *ctx,
                                            int32_t width,
                                            int32_t height,
                                            DXGI_FORMAT format,
                                            ID3D11Texture2D **out_tex);
static HRESULT d3d11_ensure_readback_staging_texture(d3d11_context_t *ctx,
                                                     int32_t width,
                                                     int32_t height,
                                                     DXGI_FORMAT format);
static HRESULT d3d11_create_depth_target(d3d11_context_t *ctx,
                                         int32_t width,
                                         int32_t height,
                                         int shader_readable,
                                         ID3D11Texture2D **out_tex,
                                         ID3D11DepthStencilView **out_dsv,
                                         ID3D11ShaderResourceView **out_srv);
static void d3d11_destroy_scene_targets(d3d11_context_t *ctx);
static HRESULT d3d11_ensure_scene_targets(d3d11_context_t *ctx, int32_t width, int32_t height);
static HRESULT d3d11_ensure_overlay_target(d3d11_context_t *ctx, int32_t width, int32_t height);
static void d3d11_destroy_bloom_targets(d3d11_context_t *ctx);
static void d3d11_destroy_taa_targets(d3d11_context_t *ctx);
static void d3d11_destroy_ssr_target(d3d11_context_t *ctx);
static HRESULT d3d11_ensure_bloom_targets(d3d11_context_t *ctx, int32_t width, int32_t height);
static HRESULT d3d11_ensure_taa_targets(d3d11_context_t *ctx, int32_t width, int32_t height);
static HRESULT d3d11_ensure_ssr_target(d3d11_context_t *ctx, int32_t width, int32_t height);
static ID3D11ShaderResourceView *d3d11_encode_bloom_chain(d3d11_context_t *ctx,
                                                          ID3D11ShaderResourceView *source_srv,
                                                          int32_t width,
                                                          int32_t height,
                                                          float threshold);
static ID3D11ShaderResourceView *d3d11_encode_taa_pass(d3d11_context_t *ctx,
                                                       ID3D11ShaderResourceView *source_srv,
                                                       int32_t width,
                                                       int32_t height,
                                                       const vgfx3d_postfx_snapshot_t *snapshot,
                                                       int preserve_history);
static HRESULT d3d11_ensure_postfx_target(d3d11_context_t *ctx, int32_t width, int32_t height);
static HRESULT d3d11_ensure_postfx_scratch_target(d3d11_context_t *ctx,
                                                  int32_t width,
                                                  int32_t height);
static int d3d11_sync_render_target_color(void *ctx_ptr, vgfx3d_rendertarget_t *target);
static void d3d11_destroy_rtt_targets(d3d11_context_t *ctx);
static HRESULT d3d11_ensure_rtt_targets(d3d11_context_t *ctx, vgfx3d_rendertarget_t *rt);
static void d3d11_destroy_shadow_targets(d3d11_context_t *ctx);
static void d3d11_release_shadow_slot(d3d11_context_t *ctx, int32_t slot);
static void d3d11_recompute_shadow_count(d3d11_context_t *ctx);
static HRESULT d3d11_ensure_shadow_targets(d3d11_context_t *ctx,
                                           int32_t slot,
                                           int32_t width,
                                           int32_t height);
static HRESULT d3d11_ensure_shadow_atlas(d3d11_context_t *ctx, int32_t tile_w, int32_t tile_h);

/// @brief Multiply two row-major 4×4 matrices: `out = a * b`.
///
/// Naive triple loop — fine for the once-per-draw matrix combos this
/// backend computes. Row-major order matches our HLSL shader convention.
/// @param[in] a Left row-major matrix.
/// @param[in] b Right row-major matrix.
/// @param[out] out Product matrix; must not alias either input.
static void mat4f_mul_d3d(const float *a, const float *b, float *out) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

/// @brief Format a bounded diagnostic message for debugger and stderr logging.
/// @details Guarantees NUL termination when @p capacity is nonzero and replaces a failed
///          formatting operation with a stable fallback message.
/// @param[out] buffer Destination character buffer.
/// @param[in] capacity Total writable bytes in @p buffer.
/// @param[in] format printf-compatible format string followed by its arguments.
static void d3d11_format_log_message(char *buffer, size_t capacity, const char *format, ...) {
    static const char kFallback[] = "[vgfx3d_d3d11] diagnostic formatting failed\n";
    int written;
    va_list arguments;

    if (!buffer || capacity == 0)
        return;
    buffer[0] = '\0';
    va_start(arguments, format);
    written = vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
    if (written < 0) {
        const size_t copy_length =
            sizeof(kFallback) - 1u < capacity - 1u ? sizeof(kFallback) - 1u : capacity - 1u;
        memcpy(buffer, kFallback, copy_length);
        buffer[copy_length] = '\0';
    } else if ((size_t)written >= capacity && capacity > 1u) {
        buffer[capacity - 2u] = '\n';
        buffer[capacity - 1u] = '\0';
    }
}

/// @brief Log a D3D11 HRESULT to both the debugger output and stderr.
/// @param[in] msg Human-readable label for the failed operation.
/// @param[in] hr HRESULT printed in hexadecimal form.
static void d3d11_log_hresult(const char *msg, HRESULT hr) {
    char buffer[256] = {0};
    d3d11_format_log_message(buffer,
                             sizeof(buffer),
                             "[vgfx3d_d3d11] %s failed (hr=0x%08lx)\n",
                             msg ? msg : "D3D11 call",
                             (unsigned long)hr);
    OutputDebugStringA(buffer);
    fputs(buffer, stderr);
}

/// @brief Whether an HRESULT normally means the D3D11 device is no longer usable.
/// @param[in] hr HRESULT to classify.
/// @return 1 for device removed, reset, hung, or internal-driver errors; otherwise 0.
static int d3d11_hresult_is_device_removed(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

/// @brief Log `ID3D11Device::GetDeviceRemovedReason()` for device-loss failures.
///
/// The HRESULT returned by `Present`, `ResizeBuffers`, or resource creation
/// often only says that the device was removed. The removed-reason HRESULT is
/// the actionable value for driver resets, TDRs, and unsupported operations.
/// This helper is intentionally side-effect-free beyond logging so callers can
/// keep their existing recovery behavior.
/// @param[in] ctx Backend context owning the D3D11 device.
/// @param[in] msg Diagnostic label for the triggering operation.
/// @param[in] hr HRESULT that may represent device loss.
static void d3d11_log_device_removed_reason(d3d11_context_t *ctx, const char *msg, HRESULT hr) {
    HRESULT reason;
    char buffer[256] = {0};

    if (!ctx || !ctx->device || !d3d11_hresult_is_device_removed(hr))
        return;
    reason = ID3D11Device_GetDeviceRemovedReason(ctx->device);
    d3d11_format_log_message(
        buffer,
        sizeof(buffer),
        "[vgfx3d_d3d11] %s device removed/reset reason=0x%08lx (trigger=0x%08lx)\n",
        msg ? msg : "D3D11",
        (unsigned long)reason,
        (unsigned long)hr);
    OutputDebugStringA(buffer);
    fputs(buffer, stderr);
}

/// @brief Validate device health after a D3D11 command that returns no HRESULT.
/// @details `UpdateSubresource`, `GenerateMips`, and `CopyResource` are void
///   commands. A removed device can therefore discard the command without a
///   direct failure result. Callers use this check before publishing CPU-side
///   cursors, generations, or validity flags that claim the GPU work completed.
/// @param[in] ctx Backend context containing the device.
/// @param[in] operation Bounded diagnostic label for the preceding command.
/// @return `S_OK` while the device remains usable, otherwise the removal reason.
static HRESULT d3d11_device_status_after_void_command(d3d11_context_t *ctx, const char *operation) {
    HRESULT hr;
    if (!ctx || !ctx->device)
        return E_POINTER;
    hr = ID3D11Device_GetDeviceRemovedReason(ctx->device);
    if (FAILED(hr)) {
        d3d11_log_hresult(operation, hr);
        d3d11_log_device_removed_reason(ctx, operation, hr);
    }
    return hr;
}

/// @brief Print bounded HLSL compiler diagnostics extracted from an `ID3DBlob`.
/// @param[in] stage Human-readable shader-stage or entry-point label.
/// @param[in] diagnostics Compiler diagnostics blob; NULL is ignored.
/// @param[in] failed Nonzero to use the failure label and larger output allowance.
static void d3d11_log_shader_diagnostics(const char *stage, ID3DBlob *diagnostics, int failed) {
    const char *text;
    SIZE_T text_len;
    int print_len;
    char buffer[8192] = {0};

    if (!diagnostics)
        return;
    text = (const char *)ID3D10Blob_GetBufferPointer(diagnostics);
    text_len = ID3D10Blob_GetBufferSize(diagnostics);
    while (text && text_len > 0 && text[text_len - 1] == '\0')
        text_len--;
    print_len = text_len > (failed ? 7936 : 768) ? (failed ? 7936 : 768) : (int)text_len;
    d3d11_format_log_message(buffer,
                             sizeof(buffer),
                             "[vgfx3d_d3d11] %s compile %s: %.*s%s\n",
                             stage ? stage : "shader",
                             failed ? "failed" : "diagnostics",
                             print_len,
                             text ? text : "",
                             text_len > (SIZE_T)print_len ? "..." : "");
    OutputDebugStringA(buffer);
    fputs(buffer, stderr);
}

/// @brief Best-effort D3D11 timestamp query creation for frame GPU-time telemetry.
/// @param[in,out] ctx Backend context receiving a fresh query set.
static void d3d11_create_frame_timing_queries(d3d11_context_t *ctx) {
    D3D11_QUERY_DESC desc;
    HRESULT hr = S_FALSE;

    if (!ctx || !ctx->device)
        return;
    ctx->frame_time_active = 0;
    ctx->frame_time_pending = 0;
    ctx->frame_time_pending_polls = 0;
    ctx->frame_gpu_time_us = 0;
    SAFE_RELEASE(ctx->frame_time_end_query);
    SAFE_RELEASE(ctx->frame_time_start_query);
    SAFE_RELEASE(ctx->frame_time_disjoint_query);
    memset(&desc, 0, sizeof(desc));
    desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    hr = ID3D11Device_CreateQuery(ctx->device, &desc, &ctx->frame_time_disjoint_query);
    hr = d3d11_required_output_result(hr, ctx->frame_time_disjoint_query);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateQuery(frame timestamp disjoint)", hr);
        d3d11_log_device_removed_reason(ctx, "CreateQuery(frame timestamp disjoint)", hr);
        SAFE_RELEASE(ctx->frame_time_disjoint_query);
        return;
    }
    desc.Query = D3D11_QUERY_TIMESTAMP;
    hr = ID3D11Device_CreateQuery(ctx->device, &desc, &ctx->frame_time_start_query);
    hr = d3d11_required_output_result(hr, ctx->frame_time_start_query);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateQuery(frame timestamp start)", hr);
        d3d11_log_device_removed_reason(ctx, "CreateQuery(frame timestamp start)", hr);
        SAFE_RELEASE(ctx->frame_time_start_query);
        SAFE_RELEASE(ctx->frame_time_disjoint_query);
        return;
    }
    hr = ID3D11Device_CreateQuery(ctx->device, &desc, &ctx->frame_time_end_query);
    hr = d3d11_required_output_result(hr, ctx->frame_time_end_query);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateQuery(frame timestamp end)", hr);
        d3d11_log_device_removed_reason(ctx, "CreateQuery(frame timestamp end)", hr);
        SAFE_RELEASE(ctx->frame_time_end_query);
        SAFE_RELEASE(ctx->frame_time_start_query);
        SAFE_RELEASE(ctx->frame_time_disjoint_query);
    }
}

/// @brief Age one busy timestamp poll and abandon a query that never becomes readable.
/// @param[in,out] ctx Backend context whose pending-poll counter is advanced.
static void d3d11_note_pending_frame_timing_poll(d3d11_context_t *ctx) {
    if (!ctx || !ctx->frame_time_pending)
        return;
    if (ctx->frame_time_pending_polls < UINT32_MAX)
        ctx->frame_time_pending_polls++;
    if (vgfx3d_d3d11_should_abandon_frame_timing(ctx->frame_time_pending_polls)) {
        d3d11_create_frame_timing_queries(ctx);
    }
}

/// @brief Try to read the pending timestamp query into `frame_gpu_time_us`.
/// @param[in,out] ctx Backend context holding the pending query set and result.
/// @return 1 when a valid duration was harvested, otherwise 0.
static int d3d11_harvest_frame_timing(d3d11_context_t *ctx) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
    UINT64 start_ticks = 0;
    UINT64 end_ticks = 0;
    HRESULT hr;
    const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;

    if (!ctx || !ctx->ctx || !ctx->frame_time_pending || !ctx->frame_time_disjoint_query ||
        !ctx->frame_time_start_query || !ctx->frame_time_end_query)
        return 0;
    memset(&disjoint, 0, sizeof(disjoint));
    hr = ID3D11DeviceContext_GetData(ctx->ctx,
                                     (ID3D11Asynchronous *)ctx->frame_time_disjoint_query,
                                     &disjoint,
                                     sizeof(disjoint),
                                     flags);
    if (hr == S_FALSE) {
        d3d11_note_pending_frame_timing_poll(ctx);
        return 0;
    }
    if (FAILED(hr)) {
        d3d11_log_hresult("GetData(frame timestamp disjoint)", hr);
        d3d11_log_device_removed_reason(ctx, "GetData(frame timestamp disjoint)", hr);
        d3d11_create_frame_timing_queries(ctx);
        return 0;
    }
    hr = ID3D11DeviceContext_GetData(ctx->ctx,
                                     (ID3D11Asynchronous *)ctx->frame_time_start_query,
                                     &start_ticks,
                                     sizeof(start_ticks),
                                     flags);
    if (hr == S_FALSE) {
        d3d11_note_pending_frame_timing_poll(ctx);
        return 0;
    }
    if (FAILED(hr)) {
        d3d11_log_hresult("GetData(frame timestamp start)", hr);
        d3d11_log_device_removed_reason(ctx, "GetData(frame timestamp start)", hr);
        d3d11_create_frame_timing_queries(ctx);
        return 0;
    }
    hr = ID3D11DeviceContext_GetData(ctx->ctx,
                                     (ID3D11Asynchronous *)ctx->frame_time_end_query,
                                     &end_ticks,
                                     sizeof(end_ticks),
                                     flags);
    if (hr == S_FALSE) {
        d3d11_note_pending_frame_timing_poll(ctx);
        return 0;
    }
    if (FAILED(hr)) {
        d3d11_log_hresult("GetData(frame timestamp end)", hr);
        d3d11_log_device_removed_reason(ctx, "GetData(frame timestamp end)", hr);
        d3d11_create_frame_timing_queries(ctx);
        return 0;
    }
    ctx->frame_time_pending = 0;
    ctx->frame_time_pending_polls = 0;
    if (vgfx3d_d3d11_compute_gpu_time_us(
            disjoint.Disjoint, disjoint.Frequency, start_ticks, end_ticks, &ctx->frame_gpu_time_us))
        return 1;
    ctx->frame_gpu_time_us = 0;
    return 0;
}

/// @brief Begin a D3D11 timestamp/disjoint query pair for the current backend pass.
/// @param[in,out] ctx Backend context whose timing queries are begun.
static void d3d11_begin_frame_timing(d3d11_context_t *ctx) {
    if (!ctx || !ctx->ctx || !ctx->frame_time_disjoint_query || !ctx->frame_time_start_query ||
        !ctx->frame_time_end_query)
        return;
    if (ctx->frame_time_active)
        return;
    if (ctx->frame_time_pending)
        (void)d3d11_harvest_frame_timing(ctx);
    if (ctx->frame_time_pending)
        return;
    ID3D11DeviceContext_Begin(ctx->ctx, (ID3D11Asynchronous *)ctx->frame_time_disjoint_query);
    ID3D11DeviceContext_End(ctx->ctx, (ID3D11Asynchronous *)ctx->frame_time_start_query);
    if (FAILED(d3d11_device_status_after_void_command(ctx, "Begin(frame timestamp)"))) {
        d3d11_create_frame_timing_queries(ctx);
        return;
    }
    ctx->frame_time_active = 1;
}

/// @brief End the active D3D11 timestamp/disjoint query pair.
/// @param[in,out] ctx Backend context whose active timing query is finalized.
static void d3d11_end_frame_timing(d3d11_context_t *ctx) {
    if (!ctx || !ctx->ctx || !ctx->frame_time_active)
        return;
    ID3D11DeviceContext_End(ctx->ctx, (ID3D11Asynchronous *)ctx->frame_time_end_query);
    ID3D11DeviceContext_End(ctx->ctx, (ID3D11Asynchronous *)ctx->frame_time_disjoint_query);
    if (FAILED(d3d11_device_status_after_void_command(ctx, "End(frame timestamp)"))) {
        d3d11_create_frame_timing_queries(ctx);
        return;
    }
    ctx->frame_time_active = 0;
    ctx->frame_time_pending = 1;
    ctx->frame_time_pending_polls = 0;
}

/// @brief Return the latest completed D3D11 GPU frame timing in microseconds.
/// @param[in,out] ctx_ptr Opaque D3D11 backend context; pending results may be harvested.
/// @return Latest valid GPU duration in microseconds, or zero when unavailable.
static uint64_t d3d11_get_frame_gpu_time_us(void *ctx_ptr) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    if (!ctx)
        return 0;
    if (ctx->frame_time_pending)
        (void)d3d11_harvest_frame_timing(ctx);
    return ctx->frame_gpu_time_us;
}

/// @brief Present the back buffer with vsync (sync interval = 1).
///
/// Logs the HRESULT on failure but doesn't surface the error — present
/// failures during window resize / device removal are expected and the
/// next frame typically recovers.
/// @param[in,out] ctx Backend context owning the swap chain and presented snapshot.
static void d3d11_present_swapchain(d3d11_context_t *ctx) {
    HRESULT hr;
    int snapshot_ok;

    if (!ctx || !ctx->swap_chain)
        return;
    snapshot_ok = d3d11_snapshot_backbuffer_for_readback(ctx);
    hr = IDXGISwapChain_Present(ctx->swap_chain, ctx->present_sync_interval, 0);
    if (hr != S_OK) {
        if (FAILED(hr)) {
            d3d11_log_hresult("IDXGISwapChain::Present", hr);
            d3d11_log_device_removed_reason(ctx, "IDXGISwapChain::Present", hr);
        }
        /* Success-status codes such as DXGI_STATUS_OCCLUDED do not confirm that the captured
         * backbuffer reached the display. Do not expose that copy as a presented-frame snapshot. */
        ctx->presented_color_valid = 0;
        return;
    }
    ctx->presented_color_valid =
        (int8_t)vgfx3d_d3d11_should_keep_presented_snapshot(snapshot_ok, 1);
}

/// @brief Runtime-compile an HLSL shader entry point to bytecode.
///
/// Wraps `D3DCompile` with strict-mode enabled and our standard error
/// reporting. The bytecode blob is returned via `*out_blob`; caller
/// owns the release. Used for both vertex and pixel shader stages
/// (the `target` parameter selects via "vs_5_0" / "ps_5_0" etc).
///
/// @param[in] source Null-terminated HLSL source.
/// @param[in] entry Entry-point function name.
/// @param[in] target Target profile string.
/// @param[out] out_blob Compiled bytecode blob released by the caller.
/// @return S_OK on success, HRESULT failure code otherwise.
static HRESULT d3d11_compile_shader(const char *source,
                                    const char *entry,
                                    const char *target,
                                    ID3DBlob **out_blob) {
    ID3DBlob *blob = NULL;
    ID3DBlob *err_blob = NULL;
    HRESULT hr;

    if (!out_blob)
        return E_POINTER;
    *out_blob = NULL;
    if (!source || !source[0] || !entry || !entry[0] || !target || !target[0])
        return E_INVALIDARG;

    hr = D3DCompile(source,
                    strlen(source),
                    "vgfx3d_d3d11",
                    NULL,
                    NULL,
                    entry,
                    target,
                    D3DCOMPILE_ENABLE_STRICTNESS,
                    0,
                    &blob,
                    &err_blob);
    if (FAILED(hr)) {
        d3d11_log_shader_diagnostics(entry, err_blob, 1);
        SAFE_RELEASE(err_blob);
        SAFE_RELEASE(blob);
        return hr;
    }
    d3d11_log_shader_diagnostics(entry, err_blob, 0);
    SAFE_RELEASE(err_blob);
    if (!blob)
        return E_FAIL;
    *out_blob = blob;
    return S_OK;
}

/// @brief Create a rasterizer state with the given fill + cull modes.
///
/// Used during context init to build the four rasterizer-state combos
/// (solid+cull, solid+nocull, wire+cull, wire+nocull). Front-face is
/// counter-clockwise to match the runtime mesh/OpenGL convention, and depth clipping is on.
/// @param[in] ctx Backend context owning the D3D11 device.
/// @param[in] fill_mode Solid or wireframe fill selection.
/// @param[in] cull_mode Requested D3D11 culling mode.
/// @param[out] out_state Destination cleared before receiving the owned state.
/// @return S_OK on success, otherwise a validation or D3D11 creation HRESULT.
static HRESULT d3d11_create_rasterizer_state(d3d11_context_t *ctx,
                                             D3D11_FILL_MODE fill_mode,
                                             D3D11_CULL_MODE cull_mode,
                                             ID3D11RasterizerState **out_state) {
    D3D11_RASTERIZER_DESC desc;
    HRESULT hr;

    if (out_state)
        *out_state = NULL;
    if (!ctx || !ctx->device || !out_state)
        return E_INVALIDARG;
    memset(&desc, 0, sizeof(desc));
    desc.FillMode = fill_mode;
    desc.CullMode = cull_mode;
    desc.FrontCounterClockwise = TRUE;
    desc.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(ctx->device, &desc, out_state);
    if (FAILED(hr) || !*out_state) {
        SAFE_RELEASE(*out_state);
        return FAILED(hr) ? hr : E_POINTER;
    }
    return S_OK;
}

/// @brief Pick the right pre-built rasterizer state for the given draw flags.
///
/// Two boolean dimensions × four pre-built states. Cheaper than a
/// per-draw `Create*State` call on the device.
/// @param[in] ctx Backend context holding the prebuilt states.
/// @param[in] wireframe Nonzero to select wireframe fill.
/// @param[in] backface_cull Nonzero to select back-face culling.
/// @return Borrowed matching rasterizer state, or NULL for a missing context.
static ID3D11RasterizerState *d3d11_choose_rasterizer(d3d11_context_t *ctx,
                                                      int8_t wireframe,
                                                      int8_t backface_cull) {
    if (!ctx)
        return NULL;
    if (wireframe)
        return backface_cull ? ctx->rs_wire_cull : ctx->rs_wire_no_cull;
    return backface_cull ? ctx->rs_solid_cull : ctx->rs_solid_no_cull;
}

/// @brief Return whether a draw needs a unique rasterizer state for depth bias.
/// @details The common draw path uses four cached rasterizer states. D3D11 stores depth bias on the
///   rasterizer state itself, so only biased draws pay for a temporary state allocation.
/// @param[in] cmd Draw command to inspect.
/// @return 1 when finite constant or slope-scaled bias is materially nonzero, otherwise 0.
static int d3d11_draw_needs_depth_bias(const vgfx3d_draw_cmd_t *cmd) {
    return cmd &&
           (fabsf(cmd->depth_bias) > 1e-8f || fabsf(vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(
                                                  cmd->slope_scaled_depth_bias)) > 1e-8f);
}

/// @brief Convert the renderer's float depth-bias value to D3D11's integer DepthBias field.
/// @details D3D11's constant bias is expressed in implementation-scaled integer units. Scaling the
///   renderer value by the shared backend scale gives useful sub-depth-buffer offsets without
///   material settings; the result is clamped before narrowing to INT. Reversed-Z scene draws and
///   standard-Z shadow draws require opposite signs to preserve the renderer's "positive bias
///   pushes away from the camera" contract.
/// @param[in] bias Renderer-space constant depth bias.
/// @param[in] reversed_z Nonzero to invert the scene-pass sign.
/// @return Saturating D3D11 integer depth-bias value.
static INT d3d11_depth_bias_to_int(float bias, int reversed_z) {
    return (INT)vgfx3d_d3d11_depth_bias_units(bias, reversed_z);
}

/// @brief Sanitized slope bias for either the reversed-Z scene or standard-Z shadow pass.
/// @param[in] slope_bias Renderer-space slope-scaled bias.
/// @param[in] reversed_z Nonzero to invert the scene-pass sign.
/// @return Finite D3D11 slope bias with the selected depth convention.
static float d3d11_slope_bias(float slope_bias, int reversed_z) {
    return vgfx3d_d3d11_depth_slope_bias(slope_bias, reversed_z);
}

/// @brief Create a rasterizer state for a biased draw.
/// @details Mirrors the cached solid/wireframe and cull/no-cull state but fills in `DepthBias` and
///   `SlopeScaledDepthBias` from the draw command. The biased-state cache owns the returned state
///   when it is installed through `d3d11_get_depth_biased_rasterizer`.
/// @param[in] ctx Backend context owning the D3D11 device.
/// @param[in] cmd Draw command providing depth-bias values.
/// @param[in] wireframe Nonzero for wireframe fill.
/// @param[in] backface_cull Nonzero for back-face culling.
/// @param[in] reversed_z Nonzero for reversed-Z scene bias semantics.
/// @param[out] out_state Destination cleared before receiving the owned state.
/// @return S_OK on success, otherwise a validation or D3D11 creation HRESULT.
static HRESULT d3d11_create_depth_biased_rasterizer(d3d11_context_t *ctx,
                                                    const vgfx3d_draw_cmd_t *cmd,
                                                    int8_t wireframe,
                                                    int8_t backface_cull,
                                                    int reversed_z,
                                                    ID3D11RasterizerState **out_state) {
    D3D11_RASTERIZER_DESC desc;
    HRESULT hr;

    if (out_state)
        *out_state = NULL;
    if (!ctx || !ctx->device || !cmd || !out_state)
        return E_INVALIDARG;
    memset(&desc, 0, sizeof(desc));
    desc.FillMode = wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    desc.CullMode = backface_cull ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    desc.FrontCounterClockwise = TRUE;
    desc.DepthClipEnable = TRUE;
    desc.DepthBias = d3d11_depth_bias_to_int(cmd->depth_bias, reversed_z);
    desc.SlopeScaledDepthBias = d3d11_slope_bias(cmd->slope_scaled_depth_bias, reversed_z);
    desc.DepthBiasClamp = 0.0f;
    hr = ID3D11Device_CreateRasterizerState(ctx->device, &desc, out_state);
    if (FAILED(hr) || !*out_state) {
        SAFE_RELEASE(*out_state);
        return FAILED(hr) ? hr : E_POINTER;
    }
    return S_OK;
}

/// @brief Return a cached rasterizer state matching a biased draw.
/// @details D3D11 stores depth bias in the rasterizer state. A single-entry cache removes repeated
///   `CreateRasterizerState` calls for common decal/shadow batches while still falling back to the
///   pre-built unbiased states for normal draws.
/// @param[in,out] ctx Backend context owning the single-entry biased-state cache.
/// @param[in] cmd Draw command providing depth-bias values.
/// @param[in] wireframe Nonzero for wireframe fill.
/// @param[in] backface_cull Nonzero for back-face culling.
/// @param[in] reversed_z Nonzero for reversed-Z scene bias semantics.
/// @param[out] out_state Destination for a borrowed cached rasterizer state.
/// @return S_OK on a cache hit or successful creation, otherwise an HRESULT failure code.
static HRESULT d3d11_get_depth_biased_rasterizer(d3d11_context_t *ctx,
                                                 const vgfx3d_draw_cmd_t *cmd,
                                                 int8_t wireframe,
                                                 int8_t backface_cull,
                                                 int reversed_z,
                                                 ID3D11RasterizerState **out_state) {
    INT depth_bias;
    float slope_bias;
    ID3D11RasterizerState *state = NULL;
    HRESULT hr;

    if (out_state)
        *out_state = NULL;
    if (!ctx || !cmd || !out_state)
        return E_INVALIDARG;
    depth_bias = d3d11_depth_bias_to_int(cmd->depth_bias, reversed_z);
    slope_bias = d3d11_slope_bias(cmd->slope_scaled_depth_bias, reversed_z);
    if (ctx->rs_depth_biased_valid && ctx->rs_depth_biased_cached &&
        ctx->rs_depth_biased_depth_bias == depth_bias &&
        ctx->rs_depth_biased_slope_bias == slope_bias &&
        ctx->rs_depth_biased_wireframe == (wireframe ? 1 : 0) &&
        ctx->rs_depth_biased_backface_cull == (backface_cull ? 1 : 0) &&
        ctx->rs_depth_biased_reversed_z == (reversed_z ? 1 : 0)) {
        *out_state = ctx->rs_depth_biased_cached;
        return S_OK;
    }

    hr = d3d11_create_depth_biased_rasterizer(
        ctx, cmd, wireframe, backface_cull, reversed_z, &state);
    if (FAILED(hr))
        return hr;
    SAFE_RELEASE(ctx->rs_depth_biased_cached);
    ctx->rs_depth_biased_cached = state;
    ctx->rs_depth_biased_depth_bias = depth_bias;
    ctx->rs_depth_biased_slope_bias = slope_bias;
    ctx->rs_depth_biased_wireframe = wireframe ? 1 : 0;
    ctx->rs_depth_biased_backface_cull = backface_cull ? 1 : 0;
    ctx->rs_depth_biased_reversed_z = reversed_z ? 1 : 0;
    ctx->rs_depth_biased_valid = 1;
    *out_state = state;
    return S_OK;
}

/// @brief Overflow-checked size_t multiplication used by backend byte calculations.
/// @details Clears @p out before validation so a failed computation never leaves
///   a stale byte count in the caller. Used for CPU-side allocations and D3D11
///   ByteWidth / pitch calculations before narrowing to UINT.
/// @param[in] a First factor.
/// @param[in] b Second factor.
/// @param[out] out Destination for the product, cleared before validation.
/// @return 1 when multiplication succeeds without overflow, otherwise 0.
static int d3d11_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (out)
        *out = 0;
    if (!out)
        return 0;
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

/// @brief Create a dynamic D3D11 constant buffer for one CPU-side cbuffer struct.
/// @details Centralizes the size validation and alignment policy so every cbuffer
///   created by the backend obeys the same 16-byte and 64 KiB limits.
/// @param[in] ctx Backend context owning the D3D11 device.
/// @param[in] size Requested CPU structure byte size.
/// @param[out] out_buffer Destination cleared before receiving the owned buffer.
/// @return S_OK on success, otherwise a validation or D3D11 creation HRESULT.
static HRESULT d3d11_create_constant_buffer(d3d11_context_t *ctx,
                                            size_t size,
                                            ID3D11Buffer **out_buffer) {
    D3D11_BUFFER_DESC desc;
    uint32_t byte_width;
    HRESULT hr;

    if (out_buffer)
        *out_buffer = NULL;
    if (!ctx || !ctx->device || !out_buffer)
        return E_INVALIDARG;
    memset(&desc, 0, sizeof(desc));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!vgfx3d_d3d11_compute_constant_buffer_byte_width(size, &byte_width))
        return E_OUTOFMEMORY;
    desc.ByteWidth = (UINT)byte_width;
    hr = ID3D11Device_CreateBuffer(ctx->device, &desc, NULL, out_buffer);
    if (FAILED(hr) || !*out_buffer) {
        SAFE_RELEASE(*out_buffer);
        return FAILED(hr) ? hr : E_POINTER;
    }
    return S_OK;
}

/// @brief Initialize common non-comparison sampler fields to D3D11-valid defaults.
/// @details `D3D11_SAMPLER_DESC` is not fully valid when left zeroed: the debug
///   layer can reject `ComparisonFunc == 0` or `MaxAnisotropy == 0` even when the
///   filter is non-comparison. Callers then override filter/address modes.
/// @param[out] desc Sampler descriptor initialized to safe non-comparison defaults.
static void d3d11_init_sampler_desc_defaults(D3D11_SAMPLER_DESC *desc) {
    if (!desc)
        return;
    memset(desc, 0, sizeof(*desc));
    desc->MaxAnisotropy = 1;
    desc->ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc->MaxLOD = D3D11_FLOAT32_MAX;
}

/// @brief Clear the cached description of whatever RTV/DSV set the backend thinks is bound.
/// @details This updates only the CPU mirror in `d3d11_context_t`; callers that need
///   the D3D11 immediate context unbound must call `d3d11_unbind_output_targets` too.
/// @param[in,out] ctx Backend context whose tracked output binding is cleared.
static void d3d11_clear_current_target_bindings(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    ctx->current_rtvs[0] = NULL;
    ctx->current_rtvs[1] = NULL;
    ctx->current_rtv_count = 0;
    ctx->current_dsv = NULL;
    ctx->current_width = 0;
    ctx->current_height = 0;
    ctx->current_target_kind = VGFX3D_D3D11_TARGET_SWAPCHAIN;
}

/// @brief Unbind all output-merger render targets and depth-stencil views.
/// @details Required before releasing backbuffer/offscreen targets and before
///   copying from textures that may currently be bound as RTVs.
/// @param[in,out] ctx Backend context whose immediate output-merger state is cleared.
static void d3d11_unbind_output_targets(d3d11_context_t *ctx) {
    if (!ctx || !ctx->ctx)
        return;
    ID3D11DeviceContext_OMSetRenderTargets(ctx->ctx, 0, NULL, NULL);
}

/// @brief Clear the pixel-shader SRV slots used by post-FX and overlay passes.
/// @details Slots 0..3 are scene color, depth, motion, and overlay; slot 4 carries the
///   Plan 05 bloom mip-chain result. Clearing them prevents read/write hazards before
///   those same textures are rebound as RTVs.
/// @param[in,out] ctx Backend context whose pixel-shader resource slots are cleared.
static void d3d11_unbind_postfx_resources(d3d11_context_t *ctx) {
    ID3D11ShaderResourceView *null_srvs[5] = {NULL, NULL, NULL, NULL, NULL};

    if (!ctx || !ctx->ctx)
        return;
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 5, null_srvs);
}

/// @brief Clear the pixel-shader SRV slots used by shadow-map sampling.
/// @details Shadow maps are rebound as depth outputs during shadow passes; D3D11
///   requires their shader-resource views to be detached first.
/// @param[in,out] ctx Backend context whose shadow SRV slots are cleared.
static void d3d11_unbind_shadow_resources(d3d11_context_t *ctx) {
    ID3D11ShaderResourceView *null_shadow_srvs[VGFX3D_CSM_SLOTS] = {NULL};
    ID3D11ShaderResourceView *null_atlas_srv[1] = {NULL};

    if (!ctx || !ctx->ctx)
        return;
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 4, VGFX3D_CSM_SLOTS, null_shadow_srvs);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 17, 1, null_atlas_srv);
}

/// @brief Rebind the CPU-tracked current output targets after a temporary unbind.
/// @details Readback and RTT sync paths unbind outputs for `CopyResource`; this
///   helper restores the render target set only when one was known to be active.
/// @param[in,out] ctx Backend context holding the tracked target binding.
static void d3d11_restore_current_target_bindings(d3d11_context_t *ctx) {
    if (!ctx || !ctx->ctx)
        return;
    if (ctx->current_rtv_count > 0 || ctx->current_dsv)
        d3d11_bind_render_targets(ctx);
}

/// @brief Map our color-format class to a DXGI texture format.
///
/// HDR16F → R16G16B16A16_FLOAT (linear, 16 bits/channel for HDR);
/// UNORM8 → R8G8B8A8_UNORM; invalid classes map to DXGI_FORMAT_UNKNOWN.
/// @param[in] format_class Backend-independent D3D11 color-format class.
/// @return Matching DXGI format, or DXGI_FORMAT_UNKNOWN for an invalid class.
static DXGI_FORMAT d3d11_color_format_to_dxgi(vgfx3d_d3d11_color_format_t format_class) {
    if (format_class == VGFX3D_D3D11_COLOR_FORMAT_UNORM8)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format_class == VGFX3D_D3D11_COLOR_FORMAT_HDR16F)
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    return DXGI_FORMAT_UNKNOWN;
}

/// @brief Reset scene-history and overlay-pass state after target lifetime changes.
/// @details Resizes, post-FX disable, and target destruction invalidate the prior
///   view-projection/depth/motion history. Resetting to identity avoids using stale
///   matrices for motion blur, SSAO reconstruction, or overlay load decisions.
/// @param[in,out] ctx Backend context whose temporal state is reset.
static void d3d11_reset_temporal_scene_state(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    memcpy(ctx->draw_prev_vp, k_identity4x4, sizeof(ctx->draw_prev_vp));
    memcpy(ctx->scene_vp, k_identity4x4, sizeof(ctx->scene_vp));
    memcpy(ctx->scene_prev_vp, k_identity4x4, sizeof(ctx->scene_prev_vp));
    memcpy(ctx->scene_inv_vp, k_identity4x4, sizeof(ctx->scene_inv_vp));
    memset(ctx->scene_cam_pos, 0, sizeof(ctx->scene_cam_pos));
    ctx->scene_history_valid = 0;
    ctx->current_pass_is_overlay = 0;
    ctx->current_load_existing_color = 0;
    ctx->overlay_used_this_frame = 0;
    ctx->scene_composited_to_swapchain = 0;
    ctx->presented_color_valid = 0;
}

/// @brief Invalidate queued, pending, and published scene-depth probe state.
/// @details Target lifetime changes must not publish one target's asynchronous
///   depth samples under slot ids belonging to a different target or extent.
/// @param[in,out] ctx Backend context whose depth-probe state is invalidated.
static void d3d11_reset_depth_probe_state(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    ctx->depth_probe_request_count = 0;
    ctx->depth_probe_pending_count = 0;
    ctx->depth_probe_pending_polls = 0;
    ctx->depth_probe_result_count = 0;
    memset(ctx->depth_probe_requests, 0, sizeof(ctx->depth_probe_requests));
    memset(ctx->depth_probe_results, 0, sizeof(ctx->depth_probe_results));
}

/// @brief Release the soft-particle opaque-depth snapshot and clear its size/validity state.
/// @param[in,out] ctx Backend context owning the snapshot resources.
static void d3d11_destroy_opaque_depth_target(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    SAFE_RELEASE(ctx->opaque_depth_srv);
    SAFE_RELEASE(ctx->opaque_depth_tex);
    ctx->opaque_depth_w = 0;
    ctx->opaque_depth_h = 0;
    ctx->opaque_depth_valid = 0;
}

/// @brief Return whether every scene color/motion/depth resource is complete.
/// @details Target selection needs more than RTV/DSV pointers: post-FX presentation
///   and readback also require the SRVs and owning textures to exist.
/// @param[in] ctx Backend context to inspect.
/// @return Nonzero when the complete offscreen scene target set exists.
static int d3d11_has_scene_targets(const d3d11_context_t *ctx) {
    return ctx && ctx->scene_color_tex && ctx->scene_color_rtv && ctx->scene_color_srv &&
           ctx->scene_motion_tex && ctx->scene_motion_rtv && ctx->scene_motion_srv &&
           ctx->scene_depth_tex && ctx->scene_dsv && ctx->scene_depth_srv;
}

/// @brief Return whether the next D3D11 window scene uses a reduced render extent.
/// @details The predicate intentionally ignores current RTT binding: RTT selection remains higher
///          priority, while main-window resources may still be rebuilt for the stored scale.
/// @param[in] ctx Borrowed D3D11 backend context.
/// @return Non-zero for a finite stored scale in `[0.25, 0.999)`.
static int d3d11_render_scale_active(const d3d11_context_t *ctx) {
    return ctx && isfinite(ctx->render_scale) && ctx->render_scale >= 0.25f &&
           ctx->render_scale < 0.999f;
}

/// @brief Return whether D3D11 window rendering needs scene targets and a final composite.
/// @details Post-processing and render scaling share the same offscreen HDR scene route; effect
///          selection still depends only on `gpu_postfx_enabled` at the final composite.
/// @param[in] ctx Borrowed D3D11 backend context.
/// @return Non-zero when either feature requires the offscreen scene route.
static int8_t d3d11_window_scene_route_enabled(const d3d11_context_t *ctx) {
    return (ctx && (ctx->gpu_postfx_enabled || d3d11_render_scale_active(ctx))) ? 1 : 0;
}

/// @brief Return whether the separate overlay color target is complete.
/// @param[in] ctx Backend context to inspect.
/// @return Nonzero when the overlay texture, RTV, and SRV all exist.
static int d3d11_has_overlay_target(const d3d11_context_t *ctx) {
    return ctx && ctx->overlay_color_tex && ctx->overlay_color_rtv && ctx->overlay_color_srv;
}

/// @brief Return whether all render-to-texture resources are complete.
/// @details RTT needs color/depth outputs plus staging readback storage; a partial
///   set must fall back before the backend binds stale or NULL resources.
/// @param[in] ctx Backend context to inspect.
/// @return Nonzero when the complete render-to-texture resource set exists.
static int d3d11_has_rtt_targets(const d3d11_context_t *ctx) {
    return ctx && ctx->rtt_color_tex && ctx->rtt_rtv && ctx->rtt_depth_tex && ctx->rtt_dsv &&
           ctx->rtt_staging;
}

/// @brief Recompute pass flags after target allocation or fallback decisions.
/// @details The active target kind is downgraded to a complete target set first,
///   then overlay/load-existing flags are derived from the resolved target so the
///   clear path never preserves stale contents from a failed allocation.
/// @param[in,out] ctx Backend context whose active-pass routing is recomputed.
/// @param[in] cam Camera flags requesting retained color or depth.
static void d3d11_refresh_pass_flags(d3d11_context_t *ctx, const vgfx3d_camera_params_t *cam) {
    if (!ctx || !cam)
        return;
    ctx->active_target_kind = vgfx3d_d3d11_resolve_available_target(ctx->active_target_kind,
                                                                    d3d11_has_scene_targets(ctx),
                                                                    d3d11_has_overlay_target(ctx),
                                                                    d3d11_has_rtt_targets(ctx));
    ctx->current_pass_is_overlay = vgfx3d_d3d11_should_treat_begin_frame_as_overlay(
                                       ctx->active_target_kind, cam->load_existing_color)
                                       ? 1
                                       : 0;
    ctx->current_load_existing_color = vgfx3d_d3d11_should_load_existing_color(
        ctx->active_target_kind, cam->load_existing_color, ctx->overlay_used_this_frame);
    if (ctx->active_target_kind == VGFX3D_D3D11_TARGET_SCENE && cam->load_existing_color)
        ctx->current_load_existing_color = 1;
}

/// @brief Map-write-unmap a constant buffer with `data`.
///
/// `D3D11_MAP_WRITE_DISCARD` so the driver can hand back a fresh GPU
/// allocation if the previous one is in flight, avoiding a CPU stall.
/// Used per-draw for the per-object/per-scene/per-material cbuffers.
/// @param[in] ctx Backend context owning the immediate device context.
/// @param[in,out] buffer Dynamic constant buffer to update.
/// @param[in] data CPU structure bytes to copy.
/// @param[in] size Number of bytes to copy; remaining buffer bytes are zero-filled.
/// @return S_OK on success, otherwise a validation, map, or device-loss HRESULT.
static HRESULT d3d11_update_constant_buffer(d3d11_context_t *ctx,
                                            ID3D11Buffer *buffer,
                                            const void *data,
                                            size_t size) {
    D3D11_BUFFER_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (!ctx || !ctx->ctx || !buffer || !data || size == 0)
        return E_INVALIDARG;
    ID3D11Buffer_GetDesc(buffer, &desc);
    if (!vgfx3d_d3d11_constant_buffer_desc_is_usable(desc.ByteWidth,
                                                     desc.Usage == D3D11_USAGE_DYNAMIC,
                                                     desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER,
                                                     desc.CPUAccessFlags == D3D11_CPU_ACCESS_WRITE,
                                                     desc.MiscFlags,
                                                     desc.StructureByteStride) ||
        size > desc.ByteWidth)
        return E_INVALIDARG;
    hr = ID3D11DeviceContext_Map(
        ctx->ctx, (ID3D11Resource *)buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        d3d11_log_device_removed_reason(ctx, "Map(dynamic constant buffer)", hr);
        return hr;
    }
    if (!mapped.pData) {
        ID3D11DeviceContext_Unmap(ctx->ctx, (ID3D11Resource *)buffer, 0);
        return E_POINTER;
    }
    if ((size_t)desc.ByteWidth > size)
        memset(mapped.pData, 0, (size_t)desc.ByteWidth);
    memcpy(mapped.pData, data, size);
    ID3D11DeviceContext_Unmap(ctx->ctx, (ID3D11Resource *)buffer, 0);
    return S_OK;
}

/// @brief Geometric-growth pattern for dynamic VB/IB resize.
///
/// If the current buffer can hold `needed` bytes, no-op. Otherwise
/// create a new dynamic buffer at the
/// next power-of-two ≥ needed. Caller passes `initial_size` for the
/// first allocation. The old buffer is kept alive until replacement
/// succeeds. Traps via `E_OUTOFMEMORY` on size overflow.
/// @param[in] ctx Backend context owning the D3D11 device.
/// @param[in,out] buffer Owned dynamic buffer slot to grow.
/// @param[in,out] capacity Current capacity, replaced after successful growth.
/// @param[in] bind_flags D3D11 bind flags for the new buffer.
/// @param[in] needed Minimum required byte capacity.
/// @param[in] initial_size Initial growth baseline when no buffer exists.
/// @return S_OK when existing capacity suffices or replacement succeeds, otherwise an HRESULT.
static HRESULT d3d11_ensure_dynamic_buffer(d3d11_context_t *ctx,
                                           ID3D11Buffer **buffer,
                                           size_t *capacity,
                                           UINT bind_flags,
                                           size_t needed,
                                           size_t initial_size) {
    D3D11_BUFFER_DESC desc;
    size_t new_capacity;
    ID3D11Buffer *new_buffer = NULL;
    uint32_t byte_width;
    int cached_usable = 0;
    HRESULT hr;

    if (!ctx || !ctx->device || !buffer || !capacity)
        return E_INVALIDARG;
    if (bind_flags != D3D11_BIND_VERTEX_BUFFER && bind_flags != D3D11_BIND_INDEX_BUFFER)
        return E_INVALIDARG;
    if (needed == 0)
        needed = 4;
    if (*buffer) {
        ID3D11Buffer_GetDesc(*buffer, &desc);
        cached_usable = vgfx3d_d3d11_dynamic_buffer_desc_is_usable(
            desc.ByteWidth,
            *capacity,
            0u,
            desc.Usage == D3D11_USAGE_DYNAMIC,
            desc.BindFlags == bind_flags,
            desc.CPUAccessFlags == D3D11_CPU_ACCESS_WRITE,
            desc.MiscFlags,
            desc.StructureByteStride);
        if (cached_usable && *capacity >= needed)
            return S_OK;
    }

    new_capacity = cached_usable ? *capacity : initial_size;
    if (new_capacity == 0)
        new_capacity = 4;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2)
            return E_OUTOFMEMORY;
        new_capacity *= 2;
    }
    if (!vgfx3d_d3d11_compute_buffer_byte_width(new_capacity, &byte_width))
        return E_OUTOFMEMORY;

    memset(&desc, 0, sizeof(desc));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.ByteWidth = (UINT)byte_width;
    desc.BindFlags = bind_flags;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(ctx->device, &desc, NULL, &new_buffer);
    if (SUCCEEDED(hr) && new_buffer) {
        ID3D11Buffer_GetDesc(new_buffer, &desc);
        if (!vgfx3d_d3d11_dynamic_buffer_desc_is_usable(desc.ByteWidth,
                                                        new_capacity,
                                                        needed,
                                                        desc.Usage == D3D11_USAGE_DYNAMIC,
                                                        desc.BindFlags == bind_flags,
                                                        desc.CPUAccessFlags ==
                                                            D3D11_CPU_ACCESS_WRITE,
                                                        desc.MiscFlags,
                                                        desc.StructureByteStride)) {
            SAFE_RELEASE(new_buffer);
            return E_FAIL;
        }
        SAFE_RELEASE(*buffer);
        *buffer = new_buffer;
        *capacity = new_capacity;
    } else {
        SAFE_RELEASE(new_buffer);
        if (SUCCEEDED(hr))
            hr = E_POINTER;
    }
    return hr;
}

/// @brief Resize-if-needed plus map-write-unmap for a dynamic buffer.
///
/// One-stop helper used by the per-draw vertex / index buffer upload
/// path. Returns 0 on any failure (logged to the HRESULT logger).
/// @param[in] ctx Backend context owning the device and immediate context.
/// @param[in,out] buffer Owned dynamic buffer slot to grow and update.
/// @param[in,out] capacity Current byte capacity.
/// @param[in] bind_flags D3D11 bind flags for the buffer.
/// @param[in] data Source bytes.
/// @param[in] bytes Number of source bytes to upload.
/// @param[in] initial_size Initial growth baseline.
/// @return 1 on complete upload, otherwise 0.
static int d3d11_upload_dynamic_buffer(d3d11_context_t *ctx,
                                       ID3D11Buffer **buffer,
                                       size_t *capacity,
                                       UINT bind_flags,
                                       const void *data,
                                       size_t bytes,
                                       size_t initial_size) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (!ctx || !ctx->ctx || !buffer || !capacity || !data || bytes == 0)
        return 0;
    hr = d3d11_ensure_dynamic_buffer(ctx, buffer, capacity, bind_flags, bytes, initial_size);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(dynamic)", hr);
        d3d11_log_device_removed_reason(ctx, "CreateBuffer(dynamic)", hr);
        return 0;
    }
    if (!*buffer || *capacity < bytes)
        return 0;
    hr = ID3D11DeviceContext_Map(
        ctx->ctx, (ID3D11Resource *)*buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(dynamic)", hr);
        d3d11_log_device_removed_reason(ctx, "Map(dynamic)", hr);
        return 0;
    }
    if (!mapped.pData) {
        ID3D11DeviceContext_Unmap(ctx->ctx, (ID3D11Resource *)*buffer, 0);
        d3d11_log_hresult("Map(dynamic)", E_POINTER);
        return 0;
    }
    memcpy(mapped.pData, data, bytes);
    ID3D11DeviceContext_Unmap(ctx->ctx, (ID3D11Resource *)*buffer, 0);
    return 1;
}

/// @brief Grow the CPU-side instance-data scratch buffer to fit `instance_count`.
///
/// Doubling growth starting at 32 entries. Used by the instanced-draw
/// path to assemble per-instance matrices before uploading.
/// @param[in,out] ctx Backend context owning the CPU scratch allocation.
/// @param[in] instance_count Required number of instance records.
/// @return 1 when sufficient capacity is available, otherwise 0.
static int d3d11_ensure_instance_upload_capacity(d3d11_context_t *ctx, int32_t instance_count) {
    size_t new_capacity;
    size_t upload_bytes;
    d3d_instance_data_t *new_data;

    if (!ctx || instance_count <= 0)
        return 0;
    if (!vgfx3d_d3d11_compute_instance_upload_bytes(
            instance_count, sizeof(d3d_instance_data_t), &upload_bytes))
        return 0;
    if (ctx->instance_upload_capacity >= (size_t)instance_count)
        return 1;

    new_capacity = ctx->instance_upload_capacity > 0 ? ctx->instance_upload_capacity : 32u;
    while (new_capacity < (size_t)instance_count) {
        size_t next_capacity;
        if (new_capacity > SIZE_MAX / 2u)
            return 0;
        next_capacity = new_capacity * 2u;
        if (next_capacity > (size_t)INT_MAX ||
            !vgfx3d_d3d11_compute_instance_upload_bytes(
                (int32_t)next_capacity, sizeof(d3d_instance_data_t), &upload_bytes)) {
            new_capacity = (size_t)instance_count;
            break;
        }
        new_capacity = next_capacity;
    }
    if (new_capacity > SIZE_MAX / sizeof(*new_data))
        return 0;
    new_data =
        (d3d_instance_data_t *)realloc(ctx->instance_upload_data, new_capacity * sizeof(*new_data));
    if (!new_data)
        return 0;
    ctx->instance_upload_data = new_data;
    ctx->instance_upload_capacity = new_capacity;
    return 1;
}

// Mesh cache — keyed by `(geometry_key, geometry_revision)`. Static
// meshes are uploaded once and reused; dynamic meshes go through the
// dynamic VB/IB instead. Bounded to D3D11_MESH_CACHE_CAPACITY entries
// with LRU-style eviction.

/// @brief Release the GPU buffers in a single mesh-cache slot.
/// @param[in,out] entry Cache entry whose COM resources and metadata are cleared.
static void d3d11_release_mesh_cache_entry(d3d11_mesh_cache_entry_t *entry) {
    if (!entry)
        return;
    SAFE_RELEASE(entry->vb);
    SAFE_RELEASE(entry->ib);
    memset(entry, 0, sizeof(*entry));
}

/// @brief Release every mesh-cache slot — called during context teardown.
/// @param[in,out] ctx Backend context owning the static-mesh cache.
static void d3d11_release_mesh_cache(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    for (int32_t i = 0; i < D3D11_MESH_CACHE_CAPACITY; i++)
        d3d11_release_mesh_cache_entry(&ctx->mesh_cache[i]);
}

/// @brief Create an immutable GPU buffer initialized from `data`.
///
/// `D3D11_USAGE_IMMUTABLE` enables driver-side optimizations (no
/// possibility of CPU writes), preferred for static mesh VB/IB. The
/// buffer can never be mapped after creation.
/// @param[in] ctx Backend context owning the D3D11 device.
/// @param[in] bind_flags Vertex- or index-buffer binding flags.
/// @param[in] data Initial immutable bytes.
/// @param[in] bytes Number of bytes in @p data.
/// @param[out] out_buffer Destination cleared before receiving the owned buffer.
/// @return S_OK on success, otherwise a validation or D3D11 creation HRESULT.
static HRESULT d3d11_create_static_buffer(d3d11_context_t *ctx,
                                          UINT bind_flags,
                                          const void *data,
                                          size_t bytes,
                                          ID3D11Buffer **out_buffer) {
    D3D11_BUFFER_DESC desc;
    D3D11_SUBRESOURCE_DATA init;
    uint32_t byte_width;

    if (out_buffer)
        *out_buffer = NULL;
    if (!ctx || !ctx->device || !data || !out_buffer ||
        !vgfx3d_d3d11_compute_buffer_byte_width(bytes, &byte_width))
        return E_INVALIDARG;
    memset(&desc, 0, sizeof(desc));
    memset(&init, 0, sizeof(init));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.ByteWidth = (UINT)byte_width;
    desc.BindFlags = bind_flags;
    init.pSysMem = data;
    {
        HRESULT hr = ID3D11Device_CreateBuffer(ctx->device, &desc, &init, out_buffer);
        if (FAILED(hr) || !*out_buffer) {
            SAFE_RELEASE(*out_buffer);
            return FAILED(hr) ? hr : E_POINTER;
        }
        return S_OK;
    }
}

/// @brief Get VB+IB for a draw command, using the cache for static meshes.
///
/// Decision tree:
///   - No `geometry_key` (dynamic mesh): upload to `dynamic_vb/ib`.
///   - Has key + matching cache slot: return cached buffers (cache hit).
///   - Has key + no matching slot: upload to oldest cache slot
///     (LRU-eviction) using immutable buffers and stash for next time.
/// Caller must NOT release the returned buffers — they're owned by
/// either the cache or the context-level dynamic buffers.
/// @brief True when this draw's static-cache geometry uses the compact 48-byte encoding (R20).
/// @details Availability of both compact input layouts is the single gate consulted by
///   the mesh cache, the stride selection, and the layout binds, so buffer contents and
///   input-assembler layout always agree.
/// @param[in] ctx Backend context holding compact input layouts.
/// @param[in] cmd Draw command requesting compact cached geometry.
/// @return 1 when the complete compact path is available and requested, otherwise 0.
static int d3d11_cmd_uses_compact_stream(const d3d11_context_t *ctx, const vgfx3d_draw_cmd_t *cmd) {
    return ctx && cmd && cmd->compact_vertex_stream && cmd->geometry_key &&
           ctx->input_layout_compact && ctx->input_layout_instanced_compact;
}

/// @brief Acquire dynamic or cached immutable vertex and index buffers for one draw.
/// @details Transient commands upload through shared dynamic buffers. Stable keyed commands use
///          an LRU cache and atomically replace a slot only after both immutable buffers succeed.
/// @param[in,out] ctx Backend context owning dynamic buffers and the mesh cache.
/// @param[in] cmd Validated draw command and geometry identity.
/// @param[out] out_vb Destination for a borrowed vertex buffer.
/// @param[out] out_ib Destination for a borrowed index buffer.
/// @return 1 when both buffers are ready, otherwise 0.
static int d3d11_acquire_mesh_buffers(d3d11_context_t *ctx,
                                      const vgfx3d_draw_cmd_t *cmd,
                                      ID3D11Buffer **out_vb,
                                      ID3D11Buffer **out_ib) {
    size_t vertex_bytes;
    size_t index_bytes;
    uint32_t index_count;

    if (!ctx || !cmd || !out_vb || !out_ib || !cmd->vertices || !cmd->indices ||
        cmd->vertex_count == 0 || cmd->index_count == 0)
        return 0;
    *out_vb = NULL;
    *out_ib = NULL;
    index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
    if (index_count == 0)
        return 0;

    if (!d3d11_checked_mul_size(
            (size_t)cmd->vertex_count, sizeof(vgfx3d_vertex_t), &vertex_bytes) ||
        !d3d11_checked_mul_size((size_t)index_count, sizeof(uint32_t), &index_bytes))
        return 0;
    if (!cmd->geometry_key || cmd->geometry_revision == 0) {
        if (!d3d11_upload_dynamic_buffer(ctx,
                                         &ctx->dynamic_vb,
                                         &ctx->dynamic_vb_size,
                                         D3D11_BIND_VERTEX_BUFFER,
                                         cmd->vertices,
                                         vertex_bytes,
                                         D3D11_INITIAL_DYNAMIC_VB_SIZE) ||
            !d3d11_upload_dynamic_buffer(ctx,
                                         &ctx->dynamic_ib,
                                         &ctx->dynamic_ib_size,
                                         D3D11_BIND_INDEX_BUFFER,
                                         cmd->indices,
                                         index_bytes,
                                         D3D11_INITIAL_DYNAMIC_IB_SIZE))
            return 0;
        *out_vb = ctx->dynamic_vb;
        *out_ib = ctx->dynamic_ib;
        return 1;
    }

    {
        d3d11_mesh_cache_entry_t *slot = NULL;
        d3d11_mesh_cache_entry_t *oldest = NULL;
        int8_t wants_compact = d3d11_cmd_uses_compact_stream(ctx, cmd) ? 1 : 0;
        ID3D11Buffer *new_vb = NULL;
        ID3D11Buffer *new_ib = NULL;
        HRESULT hr;

        for (int32_t i = 0; i < D3D11_MESH_CACHE_CAPACITY; i++) {
            d3d11_mesh_cache_entry_t *entry = &ctx->mesh_cache[i];
            if (entry->key == cmd->geometry_key) {
                slot = entry;
                break;
            }
            if (!entry->key && !slot)
                slot = entry;
            if (!oldest || entry->last_used_frame < oldest->last_used_frame)
                oldest = entry;
        }
        if (!slot)
            slot = oldest;
        if (!slot)
            return 0;

        if (slot->key != cmd->geometry_key || slot->revision != cmd->geometry_revision ||
            slot->vertex_count != cmd->vertex_count || slot->index_count != index_count ||
            slot->compact != wants_compact || !slot->vb || !slot->ib) {
            if (wants_compact) {
                /* R20: encode into the packed 48-byte layout; the compact input
                 * layouts decode it via input-assembler format conversion. */
                size_t compact_bytes;
                uint32_t compact_byte_width;
                if (!d3d11_checked_mul_size(
                        (size_t)cmd->vertex_count, VGFX3D_COMPACT_VERTEX_STRIDE, &compact_bytes) ||
                    !vgfx3d_d3d11_compute_buffer_byte_width(compact_bytes, &compact_byte_width))
                    return 0;
                (void)compact_byte_width;
                uint8_t *packed = (uint8_t *)malloc(compact_bytes);
                if (!packed)
                    return 0;
                vgfx3d_encode_compact_vertices(cmd->vertices, cmd->vertex_count, packed);
                hr = d3d11_create_static_buffer(
                    ctx, D3D11_BIND_VERTEX_BUFFER, packed, compact_bytes, &new_vb);
                free(packed);
            } else {
                hr = d3d11_create_static_buffer(
                    ctx, D3D11_BIND_VERTEX_BUFFER, cmd->vertices, vertex_bytes, &new_vb);
            }
            if (FAILED(hr) || !new_vb) {
                d3d11_log_hresult("CreateBuffer(static vertex)", hr);
                SAFE_RELEASE(new_vb);
                return 0;
            }
            hr = d3d11_create_static_buffer(
                ctx, D3D11_BIND_INDEX_BUFFER, cmd->indices, index_bytes, &new_ib);
            if (FAILED(hr) || !new_ib) {
                d3d11_log_hresult("CreateBuffer(static index)", hr);
                SAFE_RELEASE(new_ib);
                SAFE_RELEASE(new_vb);
                return 0;
            }
            d3d11_release_mesh_cache_entry(slot);
            slot->vb = new_vb;
            slot->ib = new_ib;
            slot->key = cmd->geometry_key;
            slot->revision = cmd->geometry_revision;
            slot->vertex_count = cmd->vertex_count;
            slot->index_count = index_count;
            slot->compact = wants_compact;
        }

        slot->last_used_frame = ctx->frame_serial;
        *out_vb = slot->vb;
        *out_ib = slot->ib;
        return 1;
    }
}

/// @brief Validate a cached typed-float buffer and its paired SRV.
/// @details Descriptor validation prevents stale capacity metadata, a view of a
///   different resource, or a narrowed/offset view from being reused for morph
///   uploads. GetResource adds a temporary reference that is always released.
/// @param[in] buffer Candidate typed-float backing buffer.
/// @param[in] srv Candidate view paired with @p buffer.
/// @param[in] tracked_capacity Backend-tracked float-element capacity.
/// @param[in] required_elements Minimum elements required by the next upload.
/// @return One when the pair exactly matches the backend's update and bind contract.
static int d3d11_float_srv_pair_is_usable(ID3D11Buffer *buffer,
                                          ID3D11ShaderResourceView *srv,
                                          size_t tracked_capacity,
                                          size_t required_elements) {
    D3D11_BUFFER_DESC buffer_desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    ID3D11Resource *view_resource = NULL;
    int usable;

    if (!buffer || !srv)
        return 0;
    memset(&buffer_desc, 0, sizeof(buffer_desc));
    memset(&srv_desc, 0, sizeof(srv_desc));
    ID3D11Buffer_GetDesc(buffer, &buffer_desc);
    ID3D11ShaderResourceView_GetDesc(srv, &srv_desc);
    ID3D11ShaderResourceView_GetResource(srv, &view_resource);
    usable = view_resource == (ID3D11Resource *)buffer &&
             vgfx3d_d3d11_float_srv_buffer_desc_is_usable(buffer_desc.ByteWidth,
                                                          tracked_capacity,
                                                          required_elements,
                                                          buffer_desc.Usage == D3D11_USAGE_DEFAULT,
                                                          buffer_desc.BindFlags ==
                                                              D3D11_BIND_SHADER_RESOURCE,
                                                          buffer_desc.CPUAccessFlags == 0u,
                                                          buffer_desc.MiscFlags,
                                                          buffer_desc.StructureByteStride) &&
             vgfx3d_d3d11_float_srv_view_desc_is_usable(tracked_capacity,
                                                        srv_desc.Format == DXGI_FORMAT_R32_FLOAT,
                                                        srv_desc.ViewDimension ==
                                                            D3D11_SRV_DIMENSION_BUFFER,
                                                        srv_desc.Buffer.FirstElement,
                                                        srv_desc.Buffer.NumElements);
    SAFE_RELEASE(view_resource);
    return usable;
}

/// @brief Resize a Buffer + SRV pair to hold `element_count` floats.
///
/// Creates a replacement buffer/SRV at `element_count` elements and swaps it
/// in only after both objects exist. Used for the morph-delta SRV buffers
/// (one for positions, one for normals).
/// @param[in] ctx Backend context owning the D3D11 device.
/// @param[in,out] buffer Owned float-buffer slot.
/// @param[in,out] srv Owned shader-resource-view slot paired with @p buffer.
/// @param[in,out] capacity Current element capacity, updated after replacement.
/// @param[in] element_count Minimum required float count.
/// @return S_OK when capacity is ready, otherwise a validation or D3D11 creation HRESULT.
static HRESULT d3d11_ensure_float_srv_buffer(d3d11_context_t *ctx,
                                             ID3D11Buffer **buffer,
                                             ID3D11ShaderResourceView **srv,
                                             size_t *capacity,
                                             size_t element_count) {
    D3D11_BUFFER_DESC desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    ID3D11Buffer *new_buffer = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;
    size_t allocation_capacity;
    size_t growth_base = 0u;
    size_t bytes;
    uint32_t byte_width;
    HRESULT hr;

    if (!ctx || !ctx->device || !buffer || !srv || !capacity)
        return E_INVALIDARG;
    if (element_count == 0)
        return S_OK;
    if (d3d11_float_srv_pair_is_usable(*buffer, *srv, *capacity, 0u)) {
        if (*capacity >= element_count)
            return S_OK;
        growth_base = *capacity;
    }

    if (!vgfx3d_d3d11_compute_float_srv_capacity(
            growth_base, element_count, &allocation_capacity) ||
        !d3d11_checked_mul_size(allocation_capacity, sizeof(float), &bytes))
        return E_OUTOFMEMORY;
    if (!vgfx3d_d3d11_compute_buffer_byte_width(bytes, &byte_width))
        return E_OUTOFMEMORY;
    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = (UINT)byte_width;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateBuffer(ctx->device, &desc, NULL, &new_buffer);
    if (FAILED(hr) || !new_buffer) {
        SAFE_RELEASE(new_buffer);
        return FAILED(hr) ? hr : E_POINTER;
    }

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv_desc.Buffer.FirstElement = 0;
    srv_desc.Buffer.NumElements = (UINT)allocation_capacity;
    hr = ID3D11Device_CreateShaderResourceView(
        ctx->device, (ID3D11Resource *)new_buffer, &srv_desc, &new_srv);
    if (FAILED(hr) || !new_srv) {
        SAFE_RELEASE(new_srv);
        SAFE_RELEASE(new_buffer);
        return FAILED(hr) ? hr : E_POINTER;
    }
    if (!d3d11_float_srv_pair_is_usable(new_buffer, new_srv, allocation_capacity, element_count)) {
        SAFE_RELEASE(new_srv);
        SAFE_RELEASE(new_buffer);
        return E_FAIL;
    }
    d3d11_unbind_draw_resources(ctx);
    SAFE_RELEASE(*srv);
    SAFE_RELEASE(*buffer);
    *buffer = new_buffer;
    *srv = new_srv;
    *capacity = allocation_capacity;
    return S_OK;
}

/// @brief Resize-if-needed plus upload for a float SRV buffer.
///
/// Uses `UpdateSubresource` (default-usage buffer) rather than map.
/// @param[in,out] ctx Backend context owning the device and immediate context.
/// @param[in,out] buffer Owned float-buffer slot.
/// @param[in,out] srv Owned shader-resource-view slot paired with @p buffer.
/// @param[in,out] capacity Current element capacity.
/// @param[in] data Float elements to upload.
/// @param[in] element_count Number of elements in @p data.
/// @return S_OK on complete upload, otherwise a validation, allocation, or device HRESULT.
static HRESULT d3d11_update_float_srv_buffer(d3d11_context_t *ctx,
                                             ID3D11Buffer **buffer,
                                             ID3D11ShaderResourceView **srv,
                                             size_t *capacity,
                                             const float *data,
                                             size_t element_count) {
    D3D11_BOX box;
    size_t upload_bytes;
    HRESULT hr;

    if (!ctx || !ctx->ctx || !data || element_count == 0)
        return E_INVALIDARG;
    hr = d3d11_ensure_float_srv_buffer(ctx, buffer, srv, capacity, element_count);
    if (FAILED(hr))
        return hr;
    if (!buffer || !srv || !*buffer || !*srv || !capacity)
        return E_POINTER;
    if (!vgfx3d_d3d11_compute_float_srv_update_bytes(element_count, *capacity, &upload_bytes) ||
        upload_bytes > UINT_MAX)
        return E_INVALIDARG;
    memset(&box, 0, sizeof(box));
    box.right = (UINT)upload_bytes;
    box.bottom = 1;
    box.back = 1;
    d3d11_unbind_draw_resources(ctx);
    ID3D11DeviceContext_UpdateSubresource(ctx->ctx, (ID3D11Resource *)*buffer, 0, &box, data, 0, 0);
    return d3d11_device_status_after_void_command(ctx, "UpdateSubresource(float SRV buffer)");
}

/// @brief Create the context-owned white fallback SRVs for pending texture uploads.
///
/// D3D11 texture and cubemap uploads are paced by a per-frame byte budget. A
/// newly requested material resource can therefore exist in the cache while its
/// pixel payload is incomplete. These 1x1 immutable resources give draw code a
/// valid SRV to bind until the real upload completes, matching the OpenGL
/// backend's bindable-fallback semantics. Material feature flags remain disabled
/// until the requested SRV itself is complete.
/// @param[in,out] ctx Backend context that owns the D3D11 device and receives the SRVs.
/// @return `S_OK` when both fallback resources exist; otherwise the failing HRESULT.
static HRESULT d3d11_create_white_fallback_resources(d3d11_context_t *ctx) {
    static const uint8_t kWhitePixel[4] = {255u, 255u, 255u, 255u};
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D11_SUBRESOURCE_DATA init_data;
    D3D11_SUBRESOURCE_DATA cube_init[6];
    ID3D11Texture2D *new_fallback_white_tex = NULL;
    ID3D11ShaderResourceView *new_fallback_white_srv = NULL;
    ID3D11Texture2D *new_fallback_white_cube_tex = NULL;
    ID3D11ShaderResourceView *new_fallback_white_cube_srv = NULL;
    ID3D11Texture2D *new_brdf_lut_tex = NULL;
    ID3D11ShaderResourceView *new_brdf_lut_srv = NULL;
    HRESULT hr = E_FAIL;

    if (!ctx || !ctx->device)
        return E_INVALIDARG;
    if (ctx->fallback_white_tex && ctx->fallback_white_srv && ctx->fallback_white_cube_tex &&
        ctx->fallback_white_cube_srv && ctx->brdf_lut_tex && ctx->brdf_lut_srv)
        return S_OK;

    memset(&desc, 0, sizeof(desc));
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(&init_data, 0, sizeof(init_data));
    init_data.pSysMem = kWhitePixel;
    init_data.SysMemPitch = sizeof(kWhitePixel);
    init_data.SysMemSlicePitch = sizeof(kWhitePixel);
    hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, &init_data, &new_fallback_white_tex);
    hr = d3d11_required_output_result(hr, new_fallback_white_tex);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateTexture2D(fallbackWhite2D)", hr);
        goto fail;
    }

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    hr = ID3D11Device_CreateShaderResourceView(
        ctx->device, (ID3D11Resource *)new_fallback_white_tex, &srv_desc, &new_fallback_white_srv);
    hr = d3d11_required_output_result(hr, new_fallback_white_srv);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateShaderResourceView(fallbackWhite2D)", hr);
        goto fail;
    }

    desc.ArraySize = 6;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    for (int face = 0; face < 6; face++) {
        cube_init[face] = init_data;
    }
    hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, cube_init, &new_fallback_white_cube_tex);
    hr = d3d11_required_output_result(hr, new_fallback_white_cube_tex);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateTexture2D(fallbackWhiteCube)", hr);
        goto fail;
    }

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srv_desc.TextureCube.MipLevels = 1;
    hr = ID3D11Device_CreateShaderResourceView(ctx->device,
                                               (ID3D11Resource *)new_fallback_white_cube_tex,
                                               &srv_desc,
                                               &new_fallback_white_cube_srv);
    hr = d3d11_required_output_result(hr, new_fallback_white_cube_srv);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateShaderResourceView(fallbackWhiteCube)", hr);
        goto fail;
    }

    /* Split-sum environment BRDF table (shared CPU precomputation, immutable). */
    memset(&desc, 0, sizeof(desc));
    desc.Width = VGFX3D_BRDF_LUT_SIZE;
    desc.Height = VGFX3D_BRDF_LUT_SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(&init_data, 0, sizeof(init_data));
    init_data.pSysMem = vgfx3d_brdf_lut_data();
    init_data.SysMemPitch = (UINT)(VGFX3D_BRDF_LUT_SIZE * 2u * sizeof(float));
    init_data.SysMemSlicePitch = 0;
    hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, &init_data, &new_brdf_lut_tex);
    hr = d3d11_required_output_result(hr, new_brdf_lut_tex);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateTexture2D(brdfLut)", hr);
        goto fail;
    }
    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    hr = ID3D11Device_CreateShaderResourceView(
        ctx->device, (ID3D11Resource *)new_brdf_lut_tex, &srv_desc, &new_brdf_lut_srv);
    hr = d3d11_required_output_result(hr, new_brdf_lut_srv);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateShaderResourceView(brdfLut)", hr);
        goto fail;
    }

    SAFE_RELEASE(ctx->fallback_white_srv);
    SAFE_RELEASE(ctx->fallback_white_tex);
    SAFE_RELEASE(ctx->fallback_white_cube_srv);
    SAFE_RELEASE(ctx->fallback_white_cube_tex);
    SAFE_RELEASE(ctx->brdf_lut_srv);
    SAFE_RELEASE(ctx->brdf_lut_tex);
    ctx->fallback_white_tex = new_fallback_white_tex;
    ctx->fallback_white_srv = new_fallback_white_srv;
    ctx->fallback_white_cube_tex = new_fallback_white_cube_tex;
    ctx->fallback_white_cube_srv = new_fallback_white_cube_srv;
    ctx->brdf_lut_tex = new_brdf_lut_tex;
    ctx->brdf_lut_srv = new_brdf_lut_srv;
    return S_OK;

fail:
    SAFE_RELEASE(new_brdf_lut_srv);
    SAFE_RELEASE(new_brdf_lut_tex);
    SAFE_RELEASE(new_fallback_white_cube_srv);
    SAFE_RELEASE(new_fallback_white_cube_tex);
    SAFE_RELEASE(new_fallback_white_srv);
    SAFE_RELEASE(new_fallback_white_tex);
    return hr;
}

#include "vgfx3d_backend_d3d11_context.inc"
#include "vgfx3d_backend_d3d11_draw.inc"
#include "vgfx3d_backend_d3d11_present.inc"
#include "vgfx3d_backend_d3d11_targets.inc"
#include "vgfx3d_backend_d3d11_texture.inc"

/// @brief Copy a point-in-time telemetry snapshot out of the D3D11 backend context.
///
/// Currently the D3D11 backend reports counters it owns directly, such as
/// texture fallback binds during budgeted streaming. Fields for subsystems not
/// instrumented by D3D11 remain zero rather than fabricating cross-backend
/// values.
/// @param[in] ctx_ptr Opaque D3D11 backend context.
/// @param[out] out_stats Caller-owned snapshot destination, cleared before validation.
static void d3d11_get_backend_stats(void *ctx_ptr, vgfx3d_backend_stats_t *out_stats) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    if (!out_stats)
        return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!ctx)
        return;
    *out_stats = ctx->stats;
}

/// @brief Return D3D11 feature bits that depend on the concrete backend.
/// @details The D3D11 backend builds the scene, bloom, and TAA history targets as RGBA16F
/// resources, so HDR scene color and temporal anti-aliasing are native capabilities whenever the
/// backend initializes successfully.
/// @param[in] ctx_ptr D3D11 backend context, currently unused.
/// @return RT_CANVAS3D_BACKEND_CAP_* feature bits supported by D3D11.
static int64_t d3d11_get_feature_caps(void *ctx_ptr) {
    (void)ctx_ptr;
    return RT_CANVAS3D_BACKEND_CAP_HDR_SCENE | RT_CANVAS3D_BACKEND_CAP_TAA;
}

const vgfx3d_backend_t vgfx3d_d3d11_backend = {
    /* Scene passes render reversed-Z (Canvas3D negates the projection z row;
     * clears are 0 with Greater compares). Shadow maps stay standard. */
    .reversed_z = 1,
    .name = "d3d11",
    .gpu_skinning = 1,
    .particle_instancing = 1,
    /* Slots >= VGFX3D_CSM_SLOTS render into the internal 4x2 depth atlas (t17). */
    .shadow_atlas_slots = 1,
    .create_ctx = d3d11_create_ctx,
    .destroy_ctx = d3d11_destroy_ctx,
    .clear = d3d11_clear,
    .resize = d3d11_resize,
    .begin_frame = d3d11_begin_frame,
    .submit_draw = d3d11_submit_draw,
    .end_frame = d3d11_end_frame,
    .set_render_target = d3d11_set_render_target,
    .shadow_begin = d3d11_shadow_begin,
    .shadow_draw = d3d11_shadow_draw,
    .shadow_end = d3d11_shadow_end,
    .shadow_reuse = d3d11_shadow_reuse,
    .draw_skybox = d3d11_draw_skybox,
    .submit_draw_instanced = d3d11_submit_draw_instanced,
    .present = d3d11_present,
    .readback_rgba = d3d11_readback_rgba,
    .present_postfx = d3d11_present_postfx,
    .resolve_opaque_targets = d3d11_resolve_opaque_targets,
    .apply_postfx = d3d11_apply_postfx,
    .set_gpu_postfx_enabled = d3d11_set_gpu_postfx_enabled,
    .set_gpu_postfx_snapshot = d3d11_set_gpu_postfx_snapshot,
    .set_texture_upload_budget = d3d11_set_texture_upload_budget,
    .get_texture_upload_pending_bytes = d3d11_get_texture_upload_pending_bytes,
    .get_texture_upload_bytes = d3d11_get_texture_upload_bytes,
    .get_frame_gpu_time_us = d3d11_get_frame_gpu_time_us,
    .get_native_texture_caps = d3d11_get_native_texture_caps,
    .get_feature_caps = d3d11_get_feature_caps,
    .get_backend_stats = d3d11_get_backend_stats,
    .set_vsync = d3d11_set_vsync,
    .set_render_scale = d3d11_set_render_scale,
    .queue_depth_probe = d3d11_queue_depth_probe,
    .read_depth_probe = d3d11_read_depth_probe,
};

#endif /* _WIN32 && ZANNA_ENABLE_GRAPHICS */
