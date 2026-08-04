//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_draw.inc
// Purpose: D3D11 draw-state packing, resource binding, and native submission.
// Key invariants:
//   - Draw resources are validated and unbound on every failed or completed submission path.
//   - Compact particles upload exactly one shared 64-byte record per retained-quad instance.
// Ownership/Lifetime:
//   - Cache buffers belong to the D3D11 context; temporary draw resources are locally released.
//   - Command payloads are borrowed from Canvas3D and remain valid through this synchronous call.
// Links: vgfx3d_backend_d3d11.c, vgfx3d_backend_d3d11_context.inc
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements D3D11 draw-state packing, binding, and submission.
///
/// This fragment resolves render targets and samplers, packs object/scene/
/// material/light constants, uploads animation and clustered-light resources,
/// binds temporary or cached textures, and issues validated ordinary or
/// instanced draw calls. Every failure path detaches and releases draw-local
/// resources without disturbing context-owned caches.
///
/// @note Included by `vgfx3d_backend_d3d11.c`; not a standalone translation unit.

#define D3D11_MATERIAL_EMISSIVE_INTENSITY_MAX VGFX3D_D3D11_POSTFX_SCALAR_MAX
#define D3D11_MATERIAL_EMISSIVE_COLOR_MAX VGFX3D_D3D11_POSTFX_SCALAR_MAX
#define D3D11_MATERIAL_NORMAL_SCALE_MAX 1000.0f

/// @brief Push the currently-selected render target(s) and viewport into the pipeline.
///
/// Combines `OMSetRenderTargets` (color RTVs + depth DSV) with a
/// matching `RSSetViewports`. Called whenever the active target
/// changes.
/// @param[in,out] ctx Backend context holding the tracked target set and viewport extent.
static void d3d11_bind_render_targets(d3d11_context_t *ctx) {
    D3D11_VIEWPORT viewport;

    if (!ctx || !ctx->ctx)
        return;
    ID3D11DeviceContext_OMSetRenderTargets(
        ctx->ctx, ctx->current_rtv_count, ctx->current_rtvs, ctx->current_dsv);
    if (ctx->current_width <= 0 || ctx->current_height <= 0) {
        ID3D11DeviceContext_RSSetViewports(ctx->ctx, 0, NULL);
        return;
    }

    memset(&viewport, 0, sizeof(viewport));
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = (FLOAT)ctx->current_width;
    viewport.Height = (FLOAT)ctx->current_height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx->ctx, 1, &viewport);
}

/// @brief Translate the active target kind into concrete RTV/DSV pointers.
///
/// Decision tree:
///   - RTT mode → user-bound color + depth.
///   - SCENE mode → scene color + motion + depth (MRT, 2 color targets).
///   - OVERLAY mode → overlay color (with fallback to scene/swapchain).
///   - default (SWAPCHAIN) → backbuffer + main depth.
/// Then pushes everything into the pipeline via `bind_render_targets`.
/// @param[in,out] ctx Backend context whose concrete current target set is selected and bound.
static void d3d11_select_current_targets(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->active_target_kind == VGFX3D_D3D11_TARGET_RTT && d3d11_has_rtt_targets(ctx)) {
        ctx->current_rtvs[0] = ctx->rtt_rtv;
        ctx->current_rtvs[1] = NULL;
        ctx->current_rtv_count = 1;
        ctx->current_dsv = ctx->rtt_dsv;
        ctx->current_width = ctx->rtt_width;
        ctx->current_height = ctx->rtt_height;
        ctx->current_target_kind = VGFX3D_D3D11_TARGET_RTT;
    } else if (ctx->active_target_kind == VGFX3D_D3D11_TARGET_SCENE &&
               d3d11_has_scene_targets(ctx)) {
        ctx->current_rtvs[0] = ctx->scene_color_rtv;
        ctx->current_rtvs[1] = ctx->scene_motion_rtv;
        ctx->current_rtv_count = 2;
        ctx->current_dsv = ctx->scene_dsv;
        ctx->current_width = ctx->scene_width;
        ctx->current_height = ctx->scene_height;
        ctx->current_target_kind = VGFX3D_D3D11_TARGET_SCENE;
    } else if (ctx->active_target_kind == VGFX3D_D3D11_TARGET_OVERLAY) {
        ctx->current_rtvs[1] = NULL;
        ctx->current_rtv_count = 1;
        ctx->current_dsv = NULL;
        if (d3d11_has_overlay_target(ctx)) {
            ctx->current_rtvs[0] = ctx->overlay_color_rtv;
            ctx->current_width = ctx->overlay_width;
            ctx->current_height = ctx->overlay_height;
            ctx->current_target_kind = VGFX3D_D3D11_TARGET_OVERLAY;
        } else if (d3d11_has_scene_targets(ctx)) {
            ctx->current_rtvs[0] = ctx->scene_color_rtv;
            ctx->current_width = ctx->scene_width;
            ctx->current_height = ctx->scene_height;
            ctx->current_target_kind = VGFX3D_D3D11_TARGET_SCENE;
        } else {
            ctx->current_rtvs[0] = ctx->rtv;
            ctx->current_dsv = ctx->dsv;
            ctx->current_rtv_count = ctx->rtv ? 1u : 0u;
            ctx->current_width = ctx->rtv ? ctx->width : 0;
            ctx->current_height = ctx->rtv ? ctx->height : 0;
            ctx->current_target_kind = VGFX3D_D3D11_TARGET_SWAPCHAIN;
        }
    } else {
        ctx->current_rtvs[0] = ctx->rtv;
        ctx->current_rtvs[1] = NULL;
        ctx->current_rtv_count = ctx->rtv ? 1u : 0u;
        ctx->current_dsv = ctx->dsv;
        ctx->current_width = ctx->rtv ? ctx->width : 0;
        ctx->current_height = ctx->rtv ? ctx->height : 0;
        ctx->current_target_kind = VGFX3D_D3D11_TARGET_SWAPCHAIN;
    }
    d3d11_bind_render_targets(ctx);
}

/// @brief Force-bind the swapchain backbuffer (skip the kind dispatch).
///
/// Used during present / postfx composition where we always want to
/// write to the swapchain regardless of the active target kind.
/// @param[in,out] ctx Backend context whose backbuffer is force-selected and bound.
static void d3d11_bind_swapchain_target(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    ctx->current_rtvs[0] = ctx->rtv;
    ctx->current_rtvs[1] = NULL;
    ctx->current_rtv_count = ctx->rtv ? 1u : 0u;
    ctx->current_dsv = NULL;
    ctx->current_width = ctx->rtv ? ctx->width : 0;
    ctx->current_height = ctx->rtv ? ctx->height : 0;
    ctx->current_target_kind = VGFX3D_D3D11_TARGET_SWAPCHAIN;
    d3d11_bind_render_targets(ctx);
}

/// @brief Clear color + (optionally) depth on the currently-bound targets.
///
/// Overlay targets always clear to transparent black. Scene MRT also
/// clears the motion-vector target to (0.5, 0.5, 0, 1) — the encoded
/// "no motion" sentinel. The `load_existing_*` flags let callers
/// preserve the contents (used by additive overlay passes).
/// @param[in,out] ctx Backend context holding the current RTV/DSV set.
/// @param[in] load_existing_color Nonzero to preserve all current color targets.
/// @param[in] load_existing_depth Nonzero to preserve the current depth target.
/// @return `S_OK` after successful/no-op clears, otherwise the device-removal reason.
static HRESULT d3d11_clear_current_targets(d3d11_context_t *ctx,
                                           int8_t load_existing_color,
                                           int8_t load_existing_depth) {
    float clear_color[4];
    static const float overlay_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float motion_clear[4] = {0.5f, 0.5f, 0.0f, 0.0f}; /* alpha = SSR mask; empty = 0 */
    int issued_clear = 0;

    if (!ctx || !ctx->ctx)
        return E_POINTER;

    if (ctx->current_target_kind == VGFX3D_D3D11_TARGET_OVERLAY) {
        memcpy(clear_color, overlay_clear, sizeof(clear_color));
    } else {
        clear_color[0] = ctx->clear_r;
        clear_color[1] = ctx->clear_g;
        clear_color[2] = ctx->clear_b;
        clear_color[3] = 1.0f;
    }

    if (!load_existing_color && ctx->current_rtv_count > 0 && ctx->current_rtvs[0]) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx->ctx, ctx->current_rtvs[0], clear_color);
        issued_clear = 1;
    }
    if (!load_existing_color && ctx->current_target_kind == VGFX3D_D3D11_TARGET_SCENE &&
        ctx->current_rtv_count > 1 && ctx->current_rtvs[1]) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx->ctx, ctx->current_rtvs[1], motion_clear);
        issued_clear = 1;
    }
    if (!load_existing_depth && ctx->current_dsv) {
        /* Reversed-Z: 0 is the far plane; scene depth states compare Greater. */
        ID3D11DeviceContext_ClearDepthStencilView(
            ctx->ctx, ctx->current_dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        issued_clear = 1;
    }
    return issued_clear ? d3d11_record_frame_device_status(ctx, "Clear(frame targets)") : S_OK;
}

/// @brief Bind the three sampler states the pixel shader expects.
///
/// Slot 0: linear/wrap (diffuse/normal/specular textures).
/// Slot 1: comparison sampler for shadow PCF.
/// Slot 2: linear/clamp (skybox + post-processing).
/// @param[in] mode RT_MATERIAL3D_TEXTURE_WRAP_* value.
/// @return Internal address-mode table index: 0 repeat, 1 clamp, or 2 mirrored repeat.
static int d3d11_sampler_wrap_index(int32_t mode) {
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE)
        return 1;
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
        return 2;
    return 0;
}

/// @brief Map a Zanna wrap-mode index (returned by d3d11_wrap_mode_index) to the
///        corresponding D3D11_TEXTURE_ADDRESS_MODE enum value.
/// @details index 1 → CLAMP, index 2 → MIRROR, any other value → WRAP (default repeat).
/// @param[in] index Internal address-mode table index.
/// @return Corresponding D3D11 texture-address enumeration.
static D3D11_TEXTURE_ADDRESS_MODE d3d11_sampler_address_mode(int index) {
    if (index == 1)
        return D3D11_TEXTURE_ADDRESS_CLAMP;
    if (index == 2)
        return D3D11_TEXTURE_ADDRESS_MIRROR;
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

/// @brief Pack independent nearest/linear min, mag, and three-state mip axes into a cache index.
/// @param[in] min_filter Sanitized minification filter.
/// @param[in] mag_filter Sanitized magnification filter.
/// @param[in] mip_filter Sanitized mip-selection filter.
/// @return Stable index in `[0, 12)`.
static int d3d11_sampler_filter_index(int32_t min_filter, int32_t mag_filter, int32_t mip_filter) {
    int min_index = min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ? 1 : 0;
    int mag_index = mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ? 1 : 0;
    int mip_index = mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST
                        ? 1
                        : (mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR ? 2 : 0);
    return min_index + mag_index * 2 + mip_index * 4;
}

/// @brief Map independent material sampler axes to the exact D3D11 filter enum.
/// @details `NONE` uses the point-mip tuple with `MaxLOD=0` supplied by the caller, preserving the
/// independent min/mag axes without allowing a lower mip to participate.
/// @param[in] min_filter Sanitized minification filter.
/// @param[in] mag_filter Sanitized magnification filter.
/// @param[in] mip_filter Sanitized mip-selection filter.
/// @return One non-comparison `D3D11_FILTER` tuple.
static D3D11_FILTER d3d11_sampler_filter_mode(int32_t min_filter,
                                              int32_t mag_filter,
                                              int32_t mip_filter) {
    int min_nearest = min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST;
    int mag_nearest = mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST;
    int mip_linear = mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR;
    if (min_nearest && mag_nearest)
        return mip_linear ? D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (min_nearest)
        return mip_linear ? D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR
                          : D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    if (mag_nearest)
        return mip_linear ? D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR
                          : D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    return mip_linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
}

/// @brief Return a cached or newly-created D3D11 sampler state for the given draw command and
/// texture slot.
/// @details Resolves wrap_s, wrap_t, filter, and anisotropy from per-slot or primary material
///          parameters,
///          maps them to cache indices via `d3d11_sampler_wrap_index`, and returns the cached entry
///          in `ctx->material_samplers[wrap_s][wrap_t][filter][anisotropy]` when available.
///          Otherwise creates a new `ID3D11SamplerState` and caches it. Falls back to
///          `ctx->linear_wrap_sampler` on creation failure or null cmd.
/// @param[in,out] ctx Backend context owning the sampler cache.
/// @param[in] cmd Draw command supplying resolved sampler axes.
/// @param[in] slot Material texture slot, or an out-of-range value for legacy defaults.
/// @return Borrowed cached/material sampler, fallback sampler, or NULL without a context.
static ID3D11SamplerState *d3d11_get_material_sampler(d3d11_context_t *ctx,
                                                      const vgfx3d_draw_cmd_t *cmd,
                                                      int32_t slot) {
    int wrap_s;
    int wrap_t;
    int32_t min_filter;
    int32_t mag_filter;
    int32_t mip_filter;
    int filter_index;
    int32_t anisotropy;
    int32_t anisotropy_index;
    D3D11_SAMPLER_DESC desc;
    ID3D11SamplerState *created_sampler = NULL;
    HRESULT hr;
    if (!ctx)
        return NULL;
    if (!cmd)
        return ctx->linear_wrap_sampler;
    if (slot >= 0 && slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT) {
        wrap_s = d3d11_sampler_wrap_index(cmd->texture_slot_wrap_s[slot]);
        wrap_t = d3d11_sampler_wrap_index(cmd->texture_slot_wrap_t[slot]);
        min_filter = cmd->texture_slot_min_filter[slot];
        mag_filter = cmd->texture_slot_mag_filter[slot];
        mip_filter = cmd->texture_slot_mip_filter[slot];
        anisotropy = cmd->texture_slot_anisotropy[slot];
    } else {
        wrap_s = d3d11_sampler_wrap_index(cmd->texture_wrap_s);
        wrap_t = d3d11_sampler_wrap_index(cmd->texture_wrap_t);
        min_filter = cmd->texture_min_filter;
        mag_filter = cmd->texture_mag_filter;
        mip_filter = cmd->texture_mip_filter;
        anisotropy = cmd->texture_anisotropy;
    }
    min_filter = min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                     ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                     : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    mag_filter = mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                     ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                     : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    mip_filter = mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST
                     ? RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST
                     : (mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR
                            ? RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR
                            : RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE);
    anisotropy = (min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ||
                  mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST)
                     ? 1
                     : vgfx3d_d3d11_sanitize_anisotropy(anisotropy);
    filter_index = d3d11_sampler_filter_index(min_filter, mag_filter, mip_filter);
    anisotropy_index = vgfx3d_d3d11_sampler_anisotropy_index(anisotropy);
    d3d11_init_sampler_desc_defaults(&desc);
    desc.Filter = d3d11_sampler_filter_mode(min_filter, mag_filter, mip_filter);
    if (mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE)
        desc.MaxLOD = 0.0f;
    if (anisotropy > 1) {
        desc.Filter = D3D11_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = (UINT)anisotropy;
    }
    desc.AddressU = d3d11_sampler_address_mode(wrap_s);
    desc.AddressV = d3d11_sampler_address_mode(wrap_t);
    desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    if (ctx->material_samplers[wrap_s][wrap_t][filter_index][anisotropy_index]) {
        ID3D11SamplerState **cached =
            &ctx->material_samplers[wrap_s][wrap_t][filter_index][anisotropy_index];
        if (d3d11_device_child_belongs_to_device((ID3D11DeviceChild *)*cached, ctx->device) &&
            d3d11_sampler_desc_matches(*cached, &desc))
            return *cached;
        SAFE_RELEASE(*cached);
    }
    if (!ctx->device)
        return ctx->linear_wrap_sampler;
    hr = ID3D11Device_CreateSamplerState(ctx->device, &desc, &created_sampler);
    hr = d3d11_device_child_result(hr, (ID3D11DeviceChild *)created_sampler, ctx->device);
    if (FAILED(hr) || !d3d11_sampler_desc_matches(created_sampler, &desc)) {
        d3d11_log_hresult("CreateSamplerState(material)", FAILED(hr) ? hr : E_FAIL);
        SAFE_RELEASE(created_sampler);
        return ctx->linear_wrap_sampler;
    }
    ctx->material_samplers[wrap_s][wrap_t][filter_index][anisotropy_index] = created_sampler;
    return created_sampler;
}

/// @brief Bind all material sampler states for a draw command to the pixel shader slots.
/// @details Material textures occupy PS sampler slots 0–7: slot 0 = base color,
///   slot 1 = shadow comparison sampler, slot 2 = linear-clamp (for screen-space
///   textures), slots 3–7 = normal, specular, emissive, metallic-roughness, AO.
///   Each per-material sampler is created lazily (or retrieved from a cache) by
///   `d3d11_get_material_sampler`, so NULL entries are skipped rather than bound.
/// @param[in,out] ctx Backend context and immediate pipeline state.
/// @param[in] cmd Draw command providing per-slot sampler configuration.
static void d3d11_bind_common_state(d3d11_context_t *ctx, const vgfx3d_draw_cmd_t *cmd) {
    ID3D11SamplerState *material_samplers[8] = {NULL};
    if (!ctx)
        return;
    material_samplers[0] =
        d3d11_get_material_sampler(ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR);
    material_samplers[3] = d3d11_get_material_sampler(ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_NORMAL);
    material_samplers[4] =
        d3d11_get_material_sampler(ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR);
    material_samplers[5] =
        d3d11_get_material_sampler(ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE);
    material_samplers[6] =
        d3d11_get_material_sampler(ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS);
    material_samplers[7] = d3d11_get_material_sampler(ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_AO);
    if (material_samplers[0])
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 0, 1, &material_samplers[0]);
    if (ctx->shadow_cmp_sampler)
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 1, 1, &ctx->shadow_cmp_sampler);
    if (ctx->linear_clamp_sampler)
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 2, 1, &ctx->linear_clamp_sampler);
    if (material_samplers[3])
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 3, 1, &material_samplers[3]);
    if (material_samplers[4])
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 4, 1, &material_samplers[4]);
    if (material_samplers[5])
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 5, 1, &material_samplers[5]);
    if (material_samplers[6])
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 6, 1, &material_samplers[6]);
    if (material_samplers[7])
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 7, 1, &material_samplers[7]);
}

/// @brief Pack draw-command per-object state into the cbuffer struct.
///
/// Computes the normal matrix, sets up skinning / morph / instance
/// flags, and packs morph weights into float4 lanes (HLSL doesn't have
/// scalar arrays in cbuffers, so we manually pack 4 weights per vec4).
/// @param[in] cmd Resolved draw command.
/// @param[out] object_data Zeroed, sanitized per-object constant-buffer payload.
static void d3d11_prepare_object_data(const vgfx3d_draw_cmd_t *cmd, d3d_per_object_t *object_data) {
    int morph_count = 0;

    memset(object_data, 0, sizeof(*object_data));
    vgfx3d_d3d11_copy_mat4_finite_or_identity(object_data->model, cmd->model_matrix);
    vgfx3d_d3d11_copy_mat4_finite_or(object_data->prev_model,
                                     cmd->has_prev_model_matrix ? cmd->prev_model_matrix
                                                                : cmd->model_matrix,
                                     object_data->model);
    vgfx3d_compute_normal_matrix4(object_data->model, object_data->normal);

    object_data->has_prev_model_matrix = cmd->has_prev_model_matrix ? 1 : 0;
    object_data->has_skinning =
        vgfx3d_d3d11_should_enable_skinning(cmd->bone_palette, cmd->bone_count) ? 1 : 0;
    object_data->has_prev_skinning =
        (object_data->has_skinning && cmd->prev_bone_palette != NULL) ? 1 : 0;
    object_data->has_prev_instance_matrices = vgfx3d_d3d11_should_use_previous_instance_matrices(
        cmd->prev_instance_matrices, cmd->has_prev_instance_matrices);
    /* Non-instanced draws always use the shared palette at base zero. The instanced
     * submit path installs a validated stride once its instance count is known. */
    object_data->instance_bone_stride = 0;

    if (cmd->morph_deltas && cmd->morph_weights && cmd->morph_shape_count > 0)
        morph_count =
            vgfx3d_d3d11_clamp_morph_shape_count(cmd->vertex_count, cmd->morph_shape_count);
    object_data->morph_shape_count = morph_count;
    object_data->vertex_count = morph_count > 0 ? (int32_t)cmd->vertex_count : 0;
    object_data->has_prev_morph_weights =
        (morph_count > 0 && cmd->prev_morph_weights != NULL) ? 1 : 0;
    object_data->has_morph_normal_deltas =
        (morph_count > 0 && cmd->morph_normal_deltas != NULL) ? 1 : 0;
    if (morph_count > 0) {
        vgfx3d_d3d11_pack_scalar_array4(object_data->morph_weights_packed,
                                        VGFX3D_D3D11_PACKED_MORPH_WEIGHT_VECS,
                                        cmd->morph_weights,
                                        morph_count);
        vgfx3d_d3d11_pack_scalar_array4(object_data->prev_morph_weights_packed,
                                        VGFX3D_D3D11_PACKED_MORPH_WEIGHT_VECS,
                                        cmd->prev_morph_weights ? cmd->prev_morph_weights
                                                                : cmd->morph_weights,
                                        morph_count);
    }
}

/// @brief Pack per-scene state (camera/lights/shadows/fog) into cbuffer struct.
///
/// `lights` parameter is unused at this layer — light data goes into
/// the dedicated PerLights cbuffer via `prepare_light_data`. This
/// struct holds the everything-else scene constants.
/// @param[in] ctx Backend context containing current camera, fog, IBL, and shadows.
/// @param[in] cmd Draw command containing light revision and optional cluster table.
/// @param[in] lights Resolved light array used to validate @p light_count.
/// @param[in] light_count Number of entries in @p lights.
/// @param[in] ambient Optional RGB ambient-light vector.
/// @param[out] scene_data Zeroed, sanitized per-scene constant-buffer payload.
static void d3d11_prepare_scene_data(d3d11_context_t *ctx,
                                     const vgfx3d_draw_cmd_t *cmd,
                                     const vgfx3d_light_params_t *lights,
                                     int32_t light_count,
                                     const float *ambient,
                                     d3d_per_scene_t *scene_data) {
    static const float zero_vec3[3] = {0.0f, 0.0f, 0.0f};
    static const float forward_fallback[3] = {0.0f, 0.0f, -1.0f};

    memset(scene_data, 0, sizeof(*scene_data));
    vgfx3d_d3d11_copy_mat4_finite_or_identity(scene_data->vp, ctx->vp);
    vgfx3d_d3d11_copy_mat4_finite_or(scene_data->prev_vp, ctx->draw_prev_vp, scene_data->vp);
    for (int32_t slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++) {
        vgfx3d_d3d11_copy_mat4_finite_or_identity(scene_data->shadow_vp[slot],
                                                  ctx->shadow_vp[slot]);
    }
    vgfx3d_d3d11_copy_float_array_clamped_finite_or(scene_data->camera_pos,
                                                    ctx->cam_pos,
                                                    3u,
                                                    -VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                    VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                    0.0f);
    scene_data->camera_pos[3] = ctx->cam_is_ortho ? 1.0f : 0.0f;
    vgfx3d_d3d11_copy_vec3_direction_or(
        scene_data->camera_forward, ctx->cam_forward, forward_fallback);
    if (ambient)
        vgfx3d_d3d11_copy_float_array_clamped_finite_or(
            scene_data->ambient, ambient, 3u, 0.0f, 1.0f, 0.0f);
    else
        vgfx3d_d3d11_copy_float_array_clamped_finite_or(
            scene_data->ambient, zero_vec3, 3u, 0.0f, 1.0f, 0.0f);
    scene_data->ambient[3] = 1.0f;
    scene_data->fog_color[0] = vgfx3d_d3d11_clamp_float_param(ctx->fog_color[0], 0.0f, 1.0f, 0.5f);
    scene_data->fog_color[1] = vgfx3d_d3d11_clamp_float_param(ctx->fog_color[1], 0.0f, 1.0f, 0.5f);
    scene_data->fog_color[2] = vgfx3d_d3d11_clamp_float_param(ctx->fog_color[2], 0.0f, 1.0f, 0.5f);
    scene_data->fog_color[3] = ctx->fog_enabled ? 1.0f : 0.0f;
    vgfx3d_d3d11_sanitize_fog_range(
        ctx->fog_near, ctx->fog_far, &scene_data->fog_near, &scene_data->fog_far);
    scene_data->shadow_bias = vgfx3d_d3d11_sanitize_shadow_bias(ctx->shadow_bias);
    scene_data->light_count =
        lights ? (light_count < 0
                      ? 0
                      : (light_count > VGFX3D_MAX_LIGHTS ? VGFX3D_MAX_LIGHTS : light_count))
               : 0;
    scene_data->shadow_count = vgfx3d_d3d11_clamp_shadow_count(ctx->shadow_count);
    scene_data->ibl_intensity =
        vgfx3d_d3d11_clamp_float_param(ctx->ibl_intensity, 0.0f, 8.0f, 1.0f);
    for (int32_t sh_k = 0; sh_k < 9; sh_k++) {
        scene_data->sh[sh_k][0] = vgfx3d_d3d11_clamp_float_param(ctx->ibl_sh[sh_k * 3 + 0],
                                                                 -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                                 VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                                 0.0f);
        scene_data->sh[sh_k][1] = vgfx3d_d3d11_clamp_float_param(ctx->ibl_sh[sh_k * 3 + 1],
                                                                 -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                                 VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                                 0.0f);
        scene_data->sh[sh_k][2] = vgfx3d_d3d11_clamp_float_param(ctx->ibl_sh[sh_k * 3 + 2],
                                                                 -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                                 VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                                 0.0f);
        scene_data->sh[sh_k][3] = 0.0f;
    }
    scene_data->shadow_filter[0] =
        vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(ctx->shadow_slope_bias);
    scene_data->shadow_filter[1] =
        vgfx3d_d3d11_clamp_float_param(ctx->shadow_strength, 0.0f, 1.0f, 0.85f);
    scene_data->shadow_filter[2] =
        (float)(ctx->shadow_quality <= 0 ? 4 : (ctx->shadow_quality == 1 ? 8 : 16));
    memcpy(scene_data->height_fog, ctx->height_fog, sizeof(scene_data->height_fog));
    memcpy(scene_data->height_fog_sun, ctx->height_fog_sun, sizeof(scene_data->height_fog_sun));
    memcpy(scene_data->height_fog_sun_dir,
           ctx->height_fog_sun_dir,
           sizeof(scene_data->height_fog_sun_dir));
    {
        const vgfx3d_cluster_table_t *cluster =
            (const vgfx3d_cluster_table_t *)(cmd ? cmd->cluster_table : NULL);
        const int cluster_usable =
            cluster && vgfx3d_d3d11_cluster_table_is_usable(
                           cluster, cmd ? cmd->lights_revision : 0, scene_data->light_count);
        float cluster_znear;
        float cluster_zfar;
        scene_data->cluster_global_count = cluster_usable ? cluster->global_light_count : -1;
        scene_data->cluster_params[0] = (float)(ctx->rtt_active ? ctx->rtt_width : ctx->width);
        scene_data->cluster_params[1] = (float)(ctx->rtt_active ? ctx->rtt_height : ctx->height);
        vgfx3d_d3d11_sanitize_cluster_depth_range(
            ctx->cam_znear, ctx->cam_zfar, &cluster_znear, &cluster_zfar);
        scene_data->cluster_params[2] = cluster_znear;
        scene_data->cluster_params[3] = cluster_zfar;
    }
}

/// @brief Backend `resolve_opaque_targets` op — snapshot the scene depth at the
///   opaque->transparent seam via CopyResource (both depth targets live in the
///   R32 typeless family). t16 is unbound first so the copy destination is not
///   simultaneously bound for reading. Blend draws then Load() the snapshot for
///   soft-particle fade.
/// @param[in,out] ctx_ptr Opaque D3D11 backend context owning the snapshot.
static void d3d11_resolve_opaque_targets(void *ctx_ptr) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    ID3D11Texture2D *depth;
    ID3D11ShaderResourceView *null_srv = NULL;
    D3D11_TEXTURE2D_DESC depth_desc;
    D3D11_TEXTURE2D_DESC expected_desc;
    int32_t w;
    int32_t h;

    if (!ctx || !ctx->ctx || !ctx->device)
        return;
    ctx->opaque_depth_valid = 0;
    switch (ctx->current_target_kind) {
        case VGFX3D_D3D11_TARGET_RTT:
            depth = ctx->rtt_depth_tex;
            break;
        case VGFX3D_D3D11_TARGET_SCENE:
            depth = ctx->scene_depth_tex;
            break;
        case VGFX3D_D3D11_TARGET_SWAPCHAIN:
            depth = ctx->depth_tex;
            break;
        default:
            return;
    }
    w = ctx->current_width;
    h = ctx->current_height;
    if (!depth || !vgfx3d_d3d11_is_valid_texture2d_extent(w, h))
        return;
    memset(&depth_desc, 0, sizeof(depth_desc));
    ID3D11Texture2D_GetDesc(depth, &depth_desc);
    if (depth_desc.Width != (UINT)w || depth_desc.Height != (UINT)h || depth_desc.MipLevels != 1u ||
        depth_desc.ArraySize != 1u ||
        (depth_desc.Format != DXGI_FORMAT_R32_TYPELESS &&
         depth_desc.Format != DXGI_FORMAT_D32_FLOAT) ||
        depth_desc.SampleDesc.Count != 1u || depth_desc.SampleDesc.Quality != 0u ||
        depth_desc.Usage != D3D11_USAGE_DEFAULT || depth_desc.CPUAccessFlags != 0u)
        return;
    memset(&expected_desc, 0, sizeof(expected_desc));
    expected_desc.Width = (UINT)w;
    expected_desc.Height = (UINT)h;
    expected_desc.MipLevels = 1u;
    expected_desc.ArraySize = 1u;
    expected_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    expected_desc.SampleDesc.Count = 1u;
    expected_desc.Usage = D3D11_USAGE_DEFAULT;
    expected_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (!ctx->opaque_depth_tex || !ctx->opaque_depth_srv || ctx->opaque_depth_w != w ||
        ctx->opaque_depth_h != h ||
        !d3d11_texture2d_desc_matches(ctx->opaque_depth_tex, &expected_desc) ||
        !d3d11_srv_matches_texture2d(
            ctx->opaque_depth_srv, ctx->opaque_depth_tex, DXGI_FORMAT_R32_FLOAT, 1u)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
        ID3D11Texture2D *new_tex = NULL;
        ID3D11ShaderResourceView *new_srv = NULL;
        HRESULT hr;
        hr = ID3D11Device_CreateTexture2D(ctx->device, &expected_desc, NULL, &new_tex);
        if (FAILED(hr) || !new_tex || !d3d11_texture2d_desc_matches(new_tex, &expected_desc)) {
            if (SUCCEEDED(hr))
                hr = new_tex ? E_FAIL : E_POINTER;
            d3d11_log_hresult("CreateTexture2D(opaqueDepth)", hr);
            SAFE_RELEASE(new_tex);
            return;
        }
        memset(&srv_desc, 0, sizeof(srv_desc));
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        hr = ID3D11Device_CreateShaderResourceView(
            ctx->device, (ID3D11Resource *)new_tex, &srv_desc, &new_srv);
        if (FAILED(hr) || !new_srv ||
            !d3d11_srv_matches_texture2d(
                new_srv, new_tex, srv_desc.Format, srv_desc.Texture2D.MipLevels)) {
            if (SUCCEEDED(hr))
                hr = new_srv ? E_FAIL : E_POINTER;
            d3d11_log_hresult("CreateSRV(opaqueDepth)", hr);
            SAFE_RELEASE(new_srv);
            SAFE_RELEASE(new_tex);
            return;
        }
        d3d11_destroy_opaque_depth_target(ctx);
        ctx->opaque_depth_tex = new_tex;
        ctx->opaque_depth_srv = new_srv;
        ctx->opaque_depth_w = w;
        ctx->opaque_depth_h = h;
    }
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 16, 1, &null_srv);
    d3d11_unbind_output_targets(ctx);
    ID3D11DeviceContext_CopyResource(
        ctx->ctx, (ID3D11Resource *)ctx->opaque_depth_tex, (ID3D11Resource *)depth);
    d3d11_restore_current_target_bindings(ctx);
    if (FAILED(d3d11_device_status_after_void_command(ctx, "CopyResource(opaqueDepth)")))
        return;
    ctx->opaque_depth_valid = 1;
}

/// @brief Upload one CPU froxel table into the cluster cbuffers (no-op when the
///        draw carries none; clusterGlobalCount = -1 keeps the shader on the
///        flat loop). u16 arrays are little-endian, matching the uint4 lanes.
/// @param[in,out] ctx Backend context owning cluster constant buffers.
/// @param[in] table Candidate CPU-binned cluster table.
/// @param[in] expected_revision Light-snapshot revision required for use.
/// @param[in] light_count Number of lights addressable by table indices.
/// @return S_OK for an absent/unusable table or complete upload, otherwise an update HRESULT.
static HRESULT d3d11_upload_cluster_table(d3d11_context_t *ctx,
                                          const vgfx3d_cluster_table_t *table,
                                          uint32_t expected_revision,
                                          int32_t light_count) {
    HRESULT hr;
    if (!vgfx3d_d3d11_cluster_table_is_usable(table, expected_revision, light_count) ||
        !ctx->cb_cluster_offsets || !ctx->cb_cluster_indices)
        return S_OK;
    hr = d3d11_update_constant_buffer(
        ctx, ctx->cb_cluster_offsets, table->offsets, sizeof(table->offsets));
    if (FAILED(hr))
        return hr;
    return d3d11_update_constant_buffer(
        ctx, ctx->cb_cluster_indices, table->indices, sizeof(table->indices));
}

/// @brief Resolve whether a draw uses IBL environment lighting, producing the
///   prefiltered chain's lod-base and effective max-lod for the env sampler.
/// @param[in] ctx Backend context containing current IBL and cubemap-cache state.
/// @param[in] cmd Draw command containing the environment CubeMap3D.
/// @param[out] out_lod_base First resident prefiltered mip level.
/// @param[out] out_max_lod Maximum sampler LOD relative to @p out_lod_base.
/// @return 1 when a complete matching IBL payload is resident, otherwise 0.
static int d3d11_cmd_ibl_draw(d3d11_context_t *ctx,
                              const vgfx3d_draw_cmd_t *cmd,
                              float *out_lod_base,
                              float *out_max_lod) {
    const rt_cubemap3d *env_cm;

    if (out_lod_base)
        *out_lod_base = 0.0f;
    if (out_max_lod)
        *out_max_lod = 0.0f;
    if (!ctx || !cmd || !out_lod_base || !out_max_lod)
        return 0;
    env_cm = (const rt_cubemap3d *)cmd->env_map;
    *out_max_lod = env_cm ? d3d11_cubemap_max_lod(env_cm) : 0.0f;
    if (!ctx->ibl_enabled || !cmd->ibl_env || !env_cm || !env_cm->ibl_ready ||
        env_cm->ibl_base_size <= 0 || env_cm->ibl_mip_count <= 0)
        return 0;
    {
        int32_t level_base;
        int ibl_resident = 0;

        if (env_cm->face_size <= 0 || env_cm->face_size > INT32_MAX ||
            env_cm->ibl_base_size > INT32_MAX || env_cm->ibl_mip_count > INT32_MAX ||
            !vgfx3d_d3d11_validate_ibl_layout((int32_t)env_cm->face_size,
                                              (int32_t)env_cm->ibl_base_size,
                                              (int32_t)env_cm->ibl_mip_count,
                                              RT_CUBEMAP3D_IBL_MAX_MIPS,
                                              &level_base))
            return 0;
        for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
            const d3d_cubemap_cache_entry_t *entry = &ctx->cubemap_cache[i];
            if (entry->cubemap_ptr == env_cm &&
                entry->generation == vgfx3d_get_cubemap_generation(env_cm) && entry->srv &&
                !entry->upload_in_progress && entry->applied_ibl_identity == env_cm->ibl_identity) {
                ibl_resident = 1;
                break;
            }
        }
        if (!ibl_resident)
            return 0;
        *out_lod_base = (float)level_base;
    }
    *out_max_lod = (float)(env_cm->ibl_mip_count - 1);
    return 1;
}

/// @brief Pack material colors, scalars, and texture-availability flags into the cbuffer.
///
/// Bundles every per-material constant the pixel shader needs:
/// PBR scalars (metallic, roughness, AO, emissive intensity), normal-
/// scale, alpha-cutoff, custom params, and the boolean flags telling
/// the shader which texture slots are populated.
/// @param[in] cmd Resolved material and draw command.
/// @param[in] has_texture Nonzero when base-color SRV resolution succeeded.
/// @param[in] has_normal_map Nonzero when normal-map SRV resolution succeeded.
/// @param[in] has_specular_map Nonzero when specular-map SRV resolution succeeded.
/// @param[in] has_emissive_map Nonzero when emissive-map SRV resolution succeeded.
/// @param[in] has_env_map Nonzero when an environment cubemap SRV is bound.
/// @param[in] has_splat Nonzero when terrain splat resources are complete.
/// @param[in] has_metallic_roughness_map Nonzero when the combined PBR-map SRV is bound.
/// @param[in] has_ao_map Nonzero when the ambient-occlusion SRV is bound.
/// @param[in] ibl_draw Nonzero when the environment is a resident prefiltered IBL chain.
/// @param[in] ibl_env_lod_base First resident prefiltered environment level.
/// @param[in] ibl_env_max_lod Maximum usable environment sampler LOD.
/// @param[out] material_data Zeroed, sanitized per-material constant-buffer payload.
static void d3d11_prepare_material_data(const vgfx3d_draw_cmd_t *cmd,
                                        int has_texture,
                                        int has_normal_map,
                                        int has_specular_map,
                                        int has_emissive_map,
                                        int has_env_map,
                                        int has_splat,
                                        int has_metallic_roughness_map,
                                        int has_ao_map,
                                        int ibl_draw,
                                        float ibl_env_lod_base,
                                        float ibl_env_max_lod,
                                        d3d_per_material_t *material_data) {
    static const float uv_fallback[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    float safe_custom_params[12];

    memset(material_data, 0, sizeof(*material_data));
    material_data->diffuse[0] =
        vgfx3d_d3d11_clamp_float_param(cmd->diffuse_color[0], 0.0f, 1.0f, 1.0f);
    material_data->diffuse[1] =
        vgfx3d_d3d11_clamp_float_param(cmd->diffuse_color[1], 0.0f, 1.0f, 1.0f);
    material_data->diffuse[2] =
        vgfx3d_d3d11_clamp_float_param(cmd->diffuse_color[2], 0.0f, 1.0f, 1.0f);
    material_data->diffuse[3] =
        vgfx3d_d3d11_clamp_float_param(cmd->diffuse_color[3], 0.0f, 1.0f, 1.0f);
    material_data->specular[0] = vgfx3d_d3d11_clamp_float_param(cmd->specular[0], 0.0f, 1.0f, 1.0f);
    material_data->specular[1] = vgfx3d_d3d11_clamp_float_param(cmd->specular[1], 0.0f, 1.0f, 1.0f);
    material_data->specular[2] = vgfx3d_d3d11_clamp_float_param(cmd->specular[2], 0.0f, 1.0f, 1.0f);
    material_data->specular[3] = vgfx3d_d3d11_clamp_float_param(
        cmd->shininess, 0.0f, VGFX3D_D3D11_MATERIAL_SHININESS_MAX, 32.0f);
    material_data->emissive[0] = vgfx3d_d3d11_clamp_float_param(
        cmd->emissive_color[0], 0.0f, D3D11_MATERIAL_EMISSIVE_COLOR_MAX, 0.0f);
    material_data->emissive[1] = vgfx3d_d3d11_clamp_float_param(
        cmd->emissive_color[1], 0.0f, D3D11_MATERIAL_EMISSIVE_COLOR_MAX, 0.0f);
    material_data->emissive[2] = vgfx3d_d3d11_clamp_float_param(
        cmd->emissive_color[2], 0.0f, D3D11_MATERIAL_EMISSIVE_COLOR_MAX, 0.0f);
    material_data->scalars[0] = vgfx3d_d3d11_clamp_float_param(cmd->alpha, 0.0f, 1.0f, 1.0f);
    material_data->scalars[1] = vgfx3d_d3d11_clamp_float_param(cmd->reflectivity, 0.0f, 1.0f, 0.0f);
    material_data->scalars[2] = ibl_env_max_lod;
    material_data->scalars[3] = ibl_env_lod_base;
    material_data->pbr_scalars0[0] =
        vgfx3d_d3d11_clamp_float_param(cmd->metallic, 0.0f, 1.0f, 0.0f);
    material_data->pbr_scalars0[1] =
        vgfx3d_d3d11_clamp_float_param(cmd->roughness, 0.0f, 1.0f, 0.5f);
    material_data->pbr_scalars0[2] = vgfx3d_d3d11_clamp_float_param(cmd->ao, 0.0f, 1.0f, 1.0f);
    material_data->pbr_scalars0[3] = vgfx3d_d3d11_clamp_float_param(
        cmd->emissive_intensity, 0.0f, D3D11_MATERIAL_EMISSIVE_INTENSITY_MAX, 1.0f);
    material_data->pbr_scalars1[0] = vgfx3d_d3d11_clamp_float_param(
        cmd->normal_scale, 0.0f, D3D11_MATERIAL_NORMAL_SCALE_MAX, 1.0f);
    material_data->pbr_scalars1[1] =
        vgfx3d_d3d11_clamp_float_param(cmd->alpha_cutoff, 0.0f, 1.0f, 0.5f);
    material_data->flags0[0] = vgfx3d_d3d11_sanitize_bool_flag(has_texture);
    material_data->flags0[1] = vgfx3d_d3d11_sanitize_bool_flag(cmd->unlit);
    material_data->flags0[2] = vgfx3d_d3d11_sanitize_bool_flag(has_normal_map);
    material_data->flags0[3] = vgfx3d_d3d11_sanitize_bool_flag(has_specular_map);
    material_data->flags1[0] = vgfx3d_d3d11_sanitize_bool_flag(has_emissive_map);
    material_data->flags1[1] = vgfx3d_d3d11_sanitize_bool_flag(has_env_map);
    material_data->flags1[2] = vgfx3d_d3d11_sanitize_bool_flag(has_splat);
    material_data->flags1[3] = vgfx3d_d3d11_sanitize_bool_flag(ibl_draw);
    material_data->pbr_flags[0] = vgfx3d_d3d11_sanitize_material_workflow(cmd->workflow);
    material_data->pbr_flags[1] = vgfx3d_d3d11_sanitize_alpha_mode(cmd->alpha_mode);
    material_data->pbr_flags[2] = vgfx3d_d3d11_sanitize_bool_flag(has_metallic_roughness_map);
    material_data->pbr_flags[3] = vgfx3d_d3d11_sanitize_bool_flag(has_ao_map);
    material_data->shading_model = vgfx3d_d3d11_sanitize_shading_model(cmd->shading_model);
    vgfx3d_d3d11_copy_float_array_clamped_finite_or(safe_custom_params,
                                                    cmd->custom_params,
                                                    12u,
                                                    -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                    VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                    0.0f);
    vgfx3d_d3d11_pack_scalar_array4(material_data->custom_params_packed,
                                    VGFX3D_D3D11_PACKED_CUSTOM_PARAM_VECS,
                                    safe_custom_params,
                                    12);
    for (int32_t slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
        if (slot < 4)
            material_data->texture_uv_sets0[slot] =
                vgfx3d_d3d11_sanitize_texture_uv_set(cmd->texture_slot_uv_set[slot]);
        else
            material_data->texture_uv_sets1[slot - 4] =
                vgfx3d_d3d11_sanitize_texture_uv_set(cmd->texture_slot_uv_set[slot]);
        material_data->texture_uv_transform0[slot][0] =
            vgfx3d_d3d11_clamp_float_param(cmd->texture_slot_uv_transform[slot][0],
                                           -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           uv_fallback[0]);
        material_data->texture_uv_transform0[slot][1] =
            vgfx3d_d3d11_clamp_float_param(cmd->texture_slot_uv_transform[slot][1],
                                           -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           uv_fallback[1]);
        material_data->texture_uv_transform0[slot][2] =
            vgfx3d_d3d11_clamp_float_param(cmd->texture_slot_uv_transform[slot][2],
                                           -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           uv_fallback[2]);
        material_data->texture_uv_transform0[slot][3] =
            vgfx3d_d3d11_clamp_float_param(cmd->texture_slot_uv_transform[slot][3],
                                           -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           uv_fallback[3]);
        material_data->texture_uv_transform1[slot][0] =
            vgfx3d_d3d11_clamp_float_param(cmd->texture_slot_uv_transform[slot][4],
                                           -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           uv_fallback[4]);
        material_data->texture_uv_transform1[slot][1] =
            vgfx3d_d3d11_clamp_float_param(cmd->texture_slot_uv_transform[slot][5],
                                           -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                           uv_fallback[5]);
    }
    for (int32_t i = 0; i < 4; i++) {
        material_data->splat_scales[i] = vgfx3d_d3d11_finite_or(cmd->splat_layer_scales[i], 1.0f);
        if (material_data->splat_scales[i] <= 0.0f)
            material_data->splat_scales[i] = 1.0f;
        if (material_data->splat_scales[i] > VGFX3D_D3D11_MATERIAL_SPLAT_SCALE_MAX)
            material_data->splat_scales[i] = VGFX3D_D3D11_MATERIAL_SPLAT_SCALE_MAX;
    }
}

/// @brief Copy up to `VGFX3D_MAX_LIGHTS` lights from the scene into the cbuffer Light array.
///
/// Each light contributes type, direction, position, color, intensity,
/// attenuation, and spot-cone parameters. Anything beyond `VGFX3D_MAX_LIGHTS`
/// is dropped silently (the shader's array is fixed-size). Shadow indices are
/// sanitized against the contiguous advertised shadow-slot range so a stale or
/// sparse light cannot sample an unbound shadow SRV.
/// @param[in] lights Resolved Canvas3D light array.
/// @param[in] light_count Number of available entries in @p lights.
/// @param[in] advertised_shadow_count Number of complete shadow slots exposed to shaders.
/// @param[out] light_data Destination array of VGFX3D_MAX_LIGHTS packed elements.
static void d3d11_prepare_light_data(const vgfx3d_light_params_t *lights,
                                     int32_t light_count,
                                     int32_t advertised_shadow_count,
                                     d3d_light_t *light_data) {
    static const float default_light_direction[3] = {0.0f, -1.0f, 0.0f};

    if (!light_data)
        return;
    memset(light_data, 0, sizeof(d3d_light_t) * VGFX3D_MAX_LIGHTS);
    if (!lights || light_count <= 0)
        return;
    for (int32_t i = 0; i < light_count && i < VGFX3D_MAX_LIGHTS; i++) {
        light_data[i].type = vgfx3d_d3d11_sanitize_light_type(lights[i].type);
        light_data[i].shadow_index = vgfx3d_d3d11_sanitize_shadow_index_for_projection(
            lights[i].shadow_index, advertised_shadow_count, lights[i].shadow_projection_type);
        light_data[i].shadow_projection_type = vgfx3d_d3d11_sanitize_shadow_projection_type(
            light_data[i].shadow_index, lights[i].shadow_projection_type);
        light_data[i].shadow_cascade_count =
            vgfx3d_d3d11_sanitize_shadow_cascade_count(lights[i].shadow_cascade_count,
                                                       light_data[i].shadow_index,
                                                       advertised_shadow_count,
                                                       light_data[i].shadow_projection_type);
        vgfx3d_d3d11_copy_vec3_direction_or(
            light_data[i].direction, lights[i].direction, default_light_direction);
        vgfx3d_d3d11_copy_float_array_clamped_finite_or(light_data[i].position,
                                                        lights[i].position,
                                                        3u,
                                                        -VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                        VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                        0.0f);
        light_data[i].color[0] =
            vgfx3d_d3d11_clamp_float_param(lights[i].color[0], 0.0f, 1.0f, 1.0f);
        light_data[i].color[1] =
            vgfx3d_d3d11_clamp_float_param(lights[i].color[1], 0.0f, 1.0f, 1.0f);
        light_data[i].color[2] =
            vgfx3d_d3d11_clamp_float_param(lights[i].color[2], 0.0f, 1.0f, 1.0f);
        light_data[i].intensity = vgfx3d_d3d11_clamp_float_param(
            lights[i].intensity, 0.0f, VGFX3D_D3D11_LIGHT_INTENSITY_MAX, 1.0f);
        light_data[i].attenuation = vgfx3d_d3d11_clamp_float_param(
            lights[i].attenuation, 0.0f, VGFX3D_D3D11_LIGHT_ATTENUATION_MAX, 0.0f);
        vgfx3d_d3d11_sanitize_spot_cone(lights[i].inner_cos,
                                        lights[i].outer_cos,
                                        &light_data[i].inner_cos,
                                        &light_data[i].outer_cos);
        vgfx3d_d3d11_sanitize_shadow_cascade_splits(
            light_data[i].shadow_cascade_splits,
            lights[i].shadow_cascade_splits,
            sizeof(light_data[i].shadow_cascade_splits) /
                sizeof(light_data[i].shadow_cascade_splits[0]));
        vgfx3d_d3d11_copy_float_array_clamped_finite_or(
            light_data[i].basis_u, lights[i].basis_u, 3u, -1.0f, 1.0f, 0.0f);
        vgfx3d_d3d11_copy_float_array_clamped_finite_or(
            light_data[i].basis_v, lights[i].basis_v, 3u, -1.0f, 1.0f, 0.0f);
        light_data[i].shape[0] = vgfx3d_d3d11_clamp_float_param(
            lights[i].width, 0.000001f, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX, 1.0f);
        light_data[i].shape[1] = vgfx3d_d3d11_clamp_float_param(
            lights[i].height, 0.000001f, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX, 1.0f);
        light_data[i].shape[2] = vgfx3d_d3d11_clamp_float_param(
            lights[i].radius, 0.000001f, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX, 1.0f);
        light_data[i].shape[3] = vgfx3d_d3d11_clamp_float_param(
            lights[i].range, 0.000001f, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX, 1.0f);
        light_data[i].decay_type = vgfx3d_d3d11_clamp_int_param(lights[i].decay_type, 0, 3);
    }
}

/// @brief Upload bone palettes and morph deltas, with caching.
///
/// Bones are cbuffer arrays (current + previous-frame for motion
/// blur). Morph deltas live in SRV buffers and are cached by
/// `(morph_key, morph_revision)` so static morph targets aren't
/// re-uploaded each frame. Returns 1 if the SRVs are bound, 0 if
/// the draw should fall back to no-morph.
/// @param[in,out] ctx Backend context owning animation buffers and caches.
/// @param[in] cmd Draw command containing optional skinning and morph payloads.
/// @param[in,out] object_data Per-object flags adjusted to match resources that upload.
/// @return 1 when morph SRVs are ready, or 0 when morphing is absent or unavailable.
static int d3d11_prepare_anim_resources(d3d11_context_t *ctx,
                                        const vgfx3d_draw_cmd_t *cmd,
                                        d3d_per_object_t *object_data) {
    float bone_palette[VGFX3D_D3D11_BONE_PALETTE_FLOATS];
    float prev_bone_palette[VGFX3D_D3D11_BONE_PALETTE_FLOATS];
    HRESULT hr;
    size_t morph_count;
    int current_bone_upload_ok = 1;
    int prev_bone_upload_ok = 1;
    int morph_upload_ok = 1;
    int morph_normal_upload_ok = 1;

    if (!ctx || !cmd || !object_data)
        return 0;

    ctx->current_morph_srv = NULL;
    ctx->current_morph_normal_srv = NULL;

    if (vgfx3d_d3d11_should_upload_bone_palette(object_data->has_skinning,
                                                object_data->has_prev_skinning)) {
        vgfx3d_d3d11_pack_bone_palette(
            bone_palette, object_data->has_skinning ? cmd->bone_palette : NULL, cmd->bone_count);
        vgfx3d_d3d11_pack_bone_palette(prev_bone_palette,
                                       object_data->has_prev_skinning ? cmd->prev_bone_palette
                                                                      : cmd->bone_palette,
                                       cmd->bone_count);

        hr = d3d11_update_constant_buffer(ctx, ctx->cb_bones, bone_palette, sizeof(bone_palette));
        if (FAILED(hr))
            d3d11_log_hresult("Map(cbBones)", hr);
        current_bone_upload_ok = SUCCEEDED(hr);
        hr = d3d11_update_constant_buffer(
            ctx, ctx->cb_prev_bones, prev_bone_palette, sizeof(prev_bone_palette));
        if (FAILED(hr))
            d3d11_log_hresult("Map(cbPrevBones)", hr);
        prev_bone_upload_ok = SUCCEEDED(hr);
        vgfx3d_d3d11_resolve_bone_upload_status(
            object_data, current_bone_upload_ok, prev_bone_upload_ok);
    }

    if (object_data->morph_shape_count <= 0 || !cmd->morph_deltas || !cmd->morph_weights) {
        object_data->morph_shape_count = 0;
        object_data->vertex_count = 0;
        object_data->has_prev_morph_weights = 0;
        object_data->has_morph_normal_deltas = 0;
        return 0;
    }

    if (!vgfx3d_d3d11_compute_morph_float_count(
            cmd->vertex_count, object_data->morph_shape_count, &morph_count)) {
        object_data->morph_shape_count = 0;
        object_data->vertex_count = 0;
        object_data->has_prev_morph_weights = 0;
        object_data->has_morph_normal_deltas = 0;
        return 0;
    }
    morph_upload_ok = 0;
    morph_normal_upload_ok = object_data->has_morph_normal_deltas ? 0 : 1;
    if (cmd->morph_key && cmd->morph_revision != 0) {
        d3d11_morph_cache_entry_t *slot = NULL;
        d3d11_morph_cache_entry_t *oldest = NULL;

        for (int32_t i = 0; i < D3D11_MORPH_CACHE_CAPACITY; i++) {
            d3d11_morph_cache_entry_t *entry = &ctx->morph_cache[i];
            if (entry->key == cmd->morph_key) {
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
        if (slot) {
            int cache_reusable =
                slot->buffer && slot->srv &&
                (!object_data->has_morph_normal_deltas ||
                 (slot->normal_buffer && slot->normal_srv)) &&
                vgfx3d_d3d11_should_reuse_morph_cache(slot->key,
                                                      slot->generation,
                                                      (int32_t)slot->shape_count,
                                                      slot->vertex_count,
                                                      (int8_t)slot->has_normal_deltas,
                                                      cmd);
            if (!cache_reusable) {
                d3d11_morph_cache_entry_t replacement;
                memset(&replacement, 0, sizeof(replacement));
                hr = d3d11_update_float_srv_buffer(ctx,
                                                   &replacement.buffer,
                                                   &replacement.srv,
                                                   &replacement.element_count,
                                                   cmd->morph_deltas,
                                                   morph_count);
                if (FAILED(hr)) {
                    d3d11_log_hresult("Create/UpdateBuffer(morphDeltas cache)", hr);
                } else {
                    if (object_data->has_morph_normal_deltas) {
                        hr = d3d11_update_float_srv_buffer(ctx,
                                                           &replacement.normal_buffer,
                                                           &replacement.normal_srv,
                                                           &replacement.normal_element_count,
                                                           cmd->morph_normal_deltas,
                                                           morph_count);
                        if (FAILED(hr)) {
                            d3d11_log_hresult("Create/UpdateBuffer(morphNormalDeltas cache)", hr);
                        }
                    }
                    if (SUCCEEDED(hr)) {
                        replacement.key = cmd->morph_key;
                        replacement.generation = cmd->morph_revision;
                        replacement.vertex_count = (uint32_t)cmd->vertex_count;
                        replacement.shape_count = (uint32_t)object_data->morph_shape_count;
                        replacement.has_normal_deltas =
                            object_data->has_morph_normal_deltas ? 1 : 0;
                        replacement.last_used_frame = ctx->frame_serial;
                        d3d11_unbind_draw_resources(ctx);
                        d3d11_release_morph_cache_entry(slot);
                        *slot = replacement;
                        memset(&replacement, 0, sizeof(replacement));
                        morph_upload_ok = 1;
                        morph_normal_upload_ok = 1;
                    }
                }
                d3d11_release_morph_cache_entry(&replacement);
            } else {
                morph_upload_ok = 1;
                morph_normal_upload_ok =
                    object_data->has_morph_normal_deltas ? (slot->normal_srv != NULL) : 1;
            }
            if (morph_upload_ok) {
                slot->last_used_frame = ctx->frame_serial;
                ctx->current_morph_srv = slot->srv;
                ctx->current_morph_normal_srv = morph_normal_upload_ok ? slot->normal_srv : NULL;
            }
        }
    }

    if (!ctx->current_morph_srv) {
        hr = d3d11_update_float_srv_buffer(ctx,
                                           &ctx->morph_buffer,
                                           &ctx->morph_srv,
                                           &ctx->morph_buffer_size,
                                           cmd->morph_deltas,
                                           morph_count);
        if (FAILED(hr)) {
            d3d11_log_hresult("Create/UpdateBuffer(morphDeltas)", hr);
        } else {
            morph_upload_ok = 1;
            ctx->current_morph_srv = ctx->morph_srv;
        }
        if (object_data->has_morph_normal_deltas) {
            hr = d3d11_update_float_srv_buffer(ctx,
                                               &ctx->morph_normal_buffer,
                                               &ctx->morph_normal_srv,
                                               &ctx->morph_normal_buffer_size,
                                               cmd->morph_normal_deltas,
                                               morph_count);
            if (FAILED(hr)) {
                d3d11_log_hresult("Create/UpdateBuffer(morphNormalDeltas)", hr);
            } else {
                morph_normal_upload_ok = 1;
                ctx->current_morph_normal_srv = ctx->morph_normal_srv;
            }
        }
    }

    vgfx3d_d3d11_resolve_morph_upload_status(object_data, morph_upload_ok, morph_normal_upload_ok);
    if (object_data->morph_shape_count <= 0) {
        ctx->current_morph_srv = NULL;
        ctx->current_morph_normal_srv = NULL;
        return 0;
    }
    if (!object_data->has_morph_normal_deltas)
        ctx->current_morph_normal_srv = NULL;
    return 1;
}

/// @brief Bind the main vertex+pixel shader pipeline state for a draw.
///
/// Selects rasterizer (wireframe/cull combo), depth-stencil state
/// (alpha blending = no depth write), blend mode, primitive topology,
/// the right input layout (instanced vs. non), main shaders, all four
/// cbuffers, sampler states, and morph SRVs.
/// @param[in,out] ctx Backend context and immediate pipeline state.
/// @param[in] cmd Draw command selecting blend, depth, and compact layout state.
/// @param[in] wireframe Nonzero for wireframe rasterization.
/// @param[in] backface_cull Nonzero for back-face culling.
/// @param[in] instanced Nonzero to select an instanced or particle vertex shader/layout.
static void d3d11_bind_main_pipeline(d3d11_context_t *ctx,
                                     const vgfx3d_draw_cmd_t *cmd,
                                     int8_t wireframe,
                                     int8_t backface_cull,
                                     int instanced) {
    ID3D11RasterizerState *rasterizer;
    ID3D11ShaderResourceView *vs_srvs[2];
    float blend_factor[4] = {0, 0, 0, 0};
    vgfx3d_d3d11_blend_mode_t blend_mode = vgfx3d_d3d11_choose_blend_mode(cmd);
    UINT draw_rtv_count = ctx->current_rtv_count;

    rasterizer = NULL;
    if (d3d11_draw_needs_depth_bias(cmd))
        (void)d3d11_get_depth_biased_rasterizer(ctx, cmd, wireframe, backface_cull, 1, &rasterizer);
    if (!rasterizer)
        rasterizer = d3d11_choose_rasterizer(ctx, wireframe, backface_cull);
    if (rasterizer)
        ID3D11DeviceContext_RSSetState(ctx->ctx, rasterizer);
    if (ctx->current_target_kind == VGFX3D_D3D11_TARGET_SCENE &&
        vgfx3d_d3d11_choose_motion_attachment_mode(ctx->current_target_kind, cmd) ==
            VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_ONLY)
        draw_rtv_count = 1;
    ID3D11DeviceContext_OMSetRenderTargets(
        ctx->ctx, draw_rtv_count, ctx->current_rtvs, ctx->current_dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx->ctx,
                                               cmd->disable_depth_test
                                                   ? ctx->depth_state_disabled
                                                   : (blend_mode == VGFX3D_D3D11_BLEND_OPAQUE
                                                          ? ctx->depth_state
                                                          : ctx->depth_state_no_write),
                                               0);
    ID3D11DeviceContext_OMSetBlendState(ctx->ctx,
                                        blend_mode == VGFX3D_D3D11_BLEND_ALPHA
                                            ? ctx->blend_state_alpha
                                            : (blend_mode == VGFX3D_D3D11_BLEND_ADDITIVE
                                                   ? ctx->blend_state_additive
                                                   : ctx->blend_state_opaque),
                                        blend_factor,
                                        0xFFFFFFFF);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (d3d11_cmd_uses_compact_stream(ctx, cmd)) {
        /* R20: compact-cached geometry decodes through the packed input layouts. */
        ID3D11DeviceContext_IASetInputLayout(
            ctx->ctx, instanced ? ctx->input_layout_instanced_compact : ctx->input_layout_compact);
    } else {
        ID3D11DeviceContext_IASetInputLayout(
            ctx->ctx, instanced ? ctx->input_layout_instanced : ctx->input_layout);
    }
    ID3D11DeviceContext_VSSetShader(
        ctx->ctx, instanced ? ctx->vs_instanced : ctx->vs_main, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx->ctx, ctx->ps_main, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_per_object);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 1, 1, &ctx->cb_per_scene);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 4, 1, &ctx->cb_bones);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 5, 1, &ctx->cb_prev_bones);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 1, 1, &ctx->cb_per_scene);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 2, 1, &ctx->cb_per_material);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 3, 1, &ctx->cb_per_lights);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 6, 1, &ctx->cb_cluster_offsets);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 7, 1, &ctx->cb_cluster_indices);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 16, 1, &ctx->opaque_depth_srv);
    d3d11_bind_common_state(ctx, cmd);

    vs_srvs[0] = ctx->current_morph_srv;
    vs_srvs[1] = ctx->current_morph_normal_srv;
    ID3D11DeviceContext_VSSetShaderResources(ctx->ctx, 0, 2, vs_srvs);
}

/// @brief Clear all VS / PS texture bindings (the "untether" call).
///
/// Necessary before binding the same resource as an RTV next frame —
/// D3D11 forbids a texture being bound as both SRV and RTV simultaneously.
/// @param[in,out] ctx Backend context whose draw resource slots are cleared.
static void d3d11_unbind_draw_resources(d3d11_context_t *ctx) {
    ID3D11ShaderResourceView *null_vs[2] = {NULL, NULL};
    /* Main draws use t17 for the shadow atlas and t18 for the BRDF LUT. Leaving those
     * slots live can keep an atlas resource tethered when it is resized or destroyed. */
    ID3D11ShaderResourceView *null_ps[19] = {NULL};
    if (!ctx || !ctx->ctx)
        return;
    ID3D11DeviceContext_VSSetShaderResources(ctx->ctx, 0, 2, null_vs);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 19, null_ps);
}

/// @brief Pack inverse-projection + inverse-view-rotation data for the skybox shader.
/// @param[in] ctx Backend context containing current camera matrices and forward vector.
/// @param[out] skybox_data Sanitized skybox constant-buffer payload.
static void d3d11_prepare_skybox_data(d3d11_context_t *ctx, d3d_skybox_cb_t *skybox_data) {
    static const float forward_fallback[3] = {0.0f, 0.0f, -1.0f};
    float safe_projection[16];
    float view_rotation[16];

    vgfx3d_d3d11_copy_mat4_finite_or_identity(view_rotation, ctx->view);
    view_rotation[3] = 0.0f;
    view_rotation[7] = 0.0f;
    view_rotation[11] = 0.0f;
    view_rotation[12] = 0.0f;
    view_rotation[13] = 0.0f;
    view_rotation[14] = 0.0f;
    view_rotation[15] = 1.0f;
    vgfx3d_d3d11_copy_mat4_finite_or_identity(safe_projection, ctx->projection);
    if (vgfx3d_invert_matrix4(safe_projection, skybox_data->inverse_projection) != 0)
        memcpy(skybox_data->inverse_projection,
               k_identity4x4,
               sizeof(skybox_data->inverse_projection));
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            skybox_data->inverse_view_rotation[r * 4 + c] = view_rotation[c * 4 + r];
    vgfx3d_d3d11_copy_vec3_direction_or(
        skybox_data->camera_forward, ctx->cam_forward, forward_fallback);
    skybox_data->camera_forward[3] = ctx->cam_is_ortho ? 1.0f : 0.0f;
}

/// @brief Pack the post-FX cbuffer with snapshot data + camera + resolution.
///
/// Bundles every post-process toggle and parameter the shaders need:
/// bloom thresholds, tonemap mode/exposure, FXAA, color grading,
/// vignette, SSAO, depth-of-field, motion blur. NULL snapshot means
/// "post-FX disabled" (most fields stay at their zero defaults).
/// @param[in] ctx Backend context containing current camera/history matrices.
/// @param[in] snapshot Optional resolved post-processing settings.
/// @param[out] postfx_data Zeroed D3D11 post-processing constant-buffer payload.
static void d3d11_prepare_postfx_data(d3d11_context_t *ctx,
                                      const vgfx3d_postfx_snapshot_t *snapshot,
                                      d3d_postfx_cb_t *postfx_data) {
    vgfx3d_postfx_snapshot_t safe_snapshot;

    memset(postfx_data, 0, sizeof(*postfx_data));
    vgfx3d_d3d11_copy_mat4_finite_or_identity(postfx_data->inv_vp, ctx->scene_inv_vp);
    vgfx3d_d3d11_copy_mat4_finite_or_identity(postfx_data->prev_vp, ctx->scene_prev_vp);
    vgfx3d_d3d11_copy_float_array_clamped_finite_or(postfx_data->camera_pos,
                                                    ctx->scene_cam_pos,
                                                    3u,
                                                    -VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                    VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                    0.0f);
    postfx_data->camera_pos[3] = 1.0f;
    postfx_data->inv_resolution[0] = ctx->scene_width > 0 ? 1.0f / (float)ctx->scene_width : 0.0f;
    postfx_data->inv_resolution[1] = ctx->scene_height > 0 ? 1.0f / (float)ctx->scene_height : 0.0f;
    postfx_data->tonemap_exposure = 1.0f;
    postfx_data->cg_contrast = 1.0f;
    postfx_data->cg_saturation = 1.0f;
    postfx_data->vignette_radius = 1.0f;
    /* The D3D11 scene target is always R16G16B16A16_FLOAT (linear HDR). */
    postfx_data->scene_is_hdr = 1;
    postfx_data->bloom_tex_enabled =
        (snapshot && snapshot->bloom_enabled && ctx->postfx_current_bloom_srv) ? 1 : 0;
    if (!snapshot)
        return;
    vgfx3d_sanitize_postfx_snapshot(snapshot, &safe_snapshot);
    snapshot = &safe_snapshot;
    postfx_data->tonemap_explicit = snapshot->tonemap_explicit ? 1 : 0;
    postfx_data->bloom_enabled = snapshot->bloom_enabled ? 1 : 0;
    postfx_data->bloom_threshold =
        vgfx3d_d3d11_clamp_float_param(snapshot->bloom_threshold, 0.0f, 1000000.0f, 1.0f);
    postfx_data->bloom_intensity =
        vgfx3d_d3d11_clamp_float_param(snapshot->bloom_intensity, 0.0f, 1000000.0f, 0.0f);
    postfx_data->bloom_passes = vgfx3d_d3d11_clamp_int_param(snapshot->bloom_passes, 0, 32);
    postfx_data->tonemap_mode = vgfx3d_d3d11_sanitize_tonemap_mode(snapshot->tonemap_mode);
    postfx_data->tonemap_exposure =
        vgfx3d_d3d11_clamp_float_param(snapshot->tonemap_exposure, 0.0f, 1000000.0f, 1.0f);
    postfx_data->fxaa_enabled = snapshot->fxaa_enabled ? 1 : 0;
    postfx_data->color_grade_enabled = snapshot->color_grade_enabled ? 1 : 0;
    postfx_data->cg_brightness = vgfx3d_d3d11_clamp_float_param(snapshot->cg_brightness,
                                                                -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                                VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                                0.0f);
    postfx_data->cg_contrast =
        vgfx3d_d3d11_clamp_float_param(snapshot->cg_contrast, 0.0f, 1000000.0f, 1.0f);
    postfx_data->cg_saturation =
        vgfx3d_d3d11_clamp_float_param(snapshot->cg_saturation, 0.0f, 1000000.0f, 1.0f);
    postfx_data->vignette_enabled = snapshot->vignette_enabled ? 1 : 0;
    postfx_data->vignette_radius =
        vgfx3d_d3d11_clamp_float_param(snapshot->vignette_radius, 0.0f, 1.0f, 1.0f);
    postfx_data->vignette_softness =
        vgfx3d_d3d11_clamp_float_param(snapshot->vignette_softness, 0.0f, 1.0f, 0.0f);
    postfx_data->ssao_enabled = snapshot->ssao_enabled ? 1 : 0;
    postfx_data->ssao_radius =
        vgfx3d_d3d11_clamp_float_param(snapshot->ssao_radius, 0.0f, 1000000.0f, 0.0f);
    postfx_data->ssao_intensity =
        vgfx3d_d3d11_clamp_float_param(snapshot->ssao_intensity, 0.0f, 1000000.0f, 0.0f);
    postfx_data->ssao_samples = vgfx3d_d3d11_clamp_int_param(snapshot->ssao_samples, 4, 16);
    postfx_data->dof_enabled = snapshot->dof_enabled ? 1 : 0;
    postfx_data->dof_focus_distance =
        vgfx3d_d3d11_clamp_float_param(snapshot->dof_focus_distance, 0.0f, 1000000.0f, 0.0f);
    postfx_data->dof_aperture =
        vgfx3d_d3d11_clamp_float_param(snapshot->dof_aperture, 0.0f, 1000000.0f, 0.0f);
    postfx_data->dof_max_blur =
        vgfx3d_d3d11_clamp_float_param(snapshot->dof_max_blur, 0.0f, 1000000.0f, 0.0f);
    postfx_data->motion_blur_enabled = snapshot->motion_blur_enabled ? 1 : 0;
    postfx_data->motion_blur_intensity =
        vgfx3d_d3d11_clamp_float_param(snapshot->motion_blur_intensity, 0.0f, 1000000.0f, 0.0f);
    postfx_data->motion_blur_samples =
        vgfx3d_d3d11_clamp_int_param(snapshot->motion_blur_samples, 2, 8);
}

/// @brief Bind every texture / cubemap SRV the shader expects (PS slots 0..15).
///
/// Slots: 0=diffuse, 1=normal, 2=specular, 3=emissive, 4-7=shadows,
/// 8=splat-control, 9-12=splat layers, 13=env cube, 14=metallic-rough,
/// 15=AO. Unbound slots get NULL so the shader's `has*` flags can
/// gate sampling. Returns whether splat sampling was actually enabled.
/// @param[in,out] ctx Backend context owning caches and immediate pipeline state.
/// @param[in] cmd Draw command containing texture assets and Pixels fallbacks.
/// @param[out] resources Resolution record owning any temporary SRVs.
/// @return 1 when inputs are valid and the resource set was bound, otherwise 0.
static int d3d11_bind_draw_resources(d3d11_context_t *ctx,
                                     const vgfx3d_draw_cmd_t *cmd,
                                     d3d_draw_resources_t *resources) {
    ID3D11ShaderResourceView *srvs[19];

    if (!ctx || !cmd || !resources)
        return 0;

    memset(resources, 0, sizeof(*resources));
    srvs[0] = d3d11_get_or_create_material_srv(
        ctx,
        cmd->texture_asset,
        cmd->texture,
        cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
        cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
        cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
        &resources->textures[0]);
    srvs[1] = d3d11_get_or_create_material_srv(
        ctx,
        cmd->normal_map_asset,
        cmd->normal_map,
        cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL],
        cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL],
        cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL],
        &resources->textures[1]);
    srvs[2] = d3d11_get_or_create_material_srv(
        ctx,
        cmd->specular_map_asset,
        cmd->specular_map,
        cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR],
        cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR],
        cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR],
        &resources->textures[2]);
    srvs[3] = d3d11_get_or_create_material_srv(
        ctx,
        cmd->emissive_map_asset,
        cmd->emissive_map,
        cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE],
        cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE],
        cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE],
        &resources->textures[3]);
    for (int32_t slot = 0; slot < VGFX3D_CSM_SLOTS; slot++)
        srvs[4 + slot] = ctx->shadow_count > slot ? ctx->shadow_srv[slot] : NULL;
    /* Atlas tiles back shadow slots >= VGFX3D_CSM_SLOTS (sampled at t17). */
    srvs[16] = NULL; /* t16 is the opaque-depth SRV, bound separately */
    srvs[17] = ctx->shadow_atlas_srv;
    srvs[8] = cmd->has_splat ? d3d11_get_or_create_srv(ctx, cmd->splat_map, &resources->textures[4])
                             : NULL;
    srvs[9] = cmd->has_splat
                  ? d3d11_get_or_create_srv(ctx, cmd->splat_layers[0], &resources->textures[5])
                  : NULL;
    srvs[10] = cmd->has_splat
                   ? d3d11_get_or_create_srv(ctx, cmd->splat_layers[1], &resources->textures[6])
                   : NULL;
    srvs[11] = cmd->has_splat
                   ? d3d11_get_or_create_srv(ctx, cmd->splat_layers[2], &resources->textures[7])
                   : NULL;
    srvs[12] = cmd->has_splat
                   ? d3d11_get_or_create_srv(ctx, cmd->splat_layers[3], &resources->textures[8])
                   : NULL;
    srvs[13] =
        (cmd->env_map &&
         (vgfx3d_d3d11_clamp_float_param(cmd->reflectivity, 0.0f, 1.0f, 0.0f) > 0.0001f ||
          (cmd->ibl_env && ctx->ibl_enabled && ((const rt_cubemap3d *)cmd->env_map)->ibl_ready)))
            ? d3d11_get_or_create_cubemap_srv(
                  ctx, (const rt_cubemap3d *)cmd->env_map, &resources->cubemap)
            : NULL;
    srvs[14] = d3d11_get_or_create_material_srv(
        ctx,
        cmd->metallic_roughness_map_asset,
        cmd->metallic_roughness_map,
        cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS],
        cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS],
        cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS],
        &resources->textures[9]);
    srvs[15] = d3d11_get_or_create_material_srv(
        ctx,
        cmd->ao_map_asset,
        cmd->ao_map,
        cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_AO],
        cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_AO],
        cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_AO],
        &resources->textures[10]);

    resources->has_texture =
        vgfx3d_d3d11_srv_is_ready(srvs[0] != NULL, srvs[0] == ctx->fallback_white_srv);
    resources->has_normal_map =
        vgfx3d_d3d11_srv_is_ready(srvs[1] != NULL, srvs[1] == ctx->fallback_white_srv);
    resources->has_specular_map =
        vgfx3d_d3d11_srv_is_ready(srvs[2] != NULL, srvs[2] == ctx->fallback_white_srv);
    resources->has_emissive_map =
        vgfx3d_d3d11_srv_is_ready(srvs[3] != NULL, srvs[3] == ctx->fallback_white_srv);
    resources->has_env_map =
        vgfx3d_d3d11_srv_is_ready(srvs[13] != NULL, srvs[13] == ctx->fallback_white_cube_srv);
    resources->has_metallic_roughness_map =
        vgfx3d_d3d11_srv_is_ready(srvs[14] != NULL, srvs[14] == ctx->fallback_white_srv);
    resources->has_ao_map =
        vgfx3d_d3d11_srv_is_ready(srvs[15] != NULL, srvs[15] == ctx->fallback_white_srv);
    resources->has_splat = vgfx3d_d3d11_has_complete_splat(
        cmd->has_splat,
        vgfx3d_d3d11_srv_is_ready(srvs[8] != NULL, srvs[8] == ctx->fallback_white_srv),
        vgfx3d_d3d11_srv_is_ready(srvs[9] != NULL, srvs[9] == ctx->fallback_white_srv),
        vgfx3d_d3d11_srv_is_ready(srvs[10] != NULL, srvs[10] == ctx->fallback_white_srv),
        vgfx3d_d3d11_srv_is_ready(srvs[11] != NULL, srvs[11] == ctx->fallback_white_srv),
        vgfx3d_d3d11_srv_is_ready(srvs[12] != NULL, srvs[12] == ctx->fallback_white_srv));
    if (!resources->has_splat) {
        srvs[8] = NULL;
        srvs[9] = NULL;
        srvs[10] = NULL;
        srvs[11] = NULL;
        srvs[12] = NULL;
    }

    srvs[18] = ctx->brdf_lut_srv;

    /* Bind 0..15 and t17 (atlas) + t18 (BRDF LUT), leaving t16 (opaque depth)
     * alone. */
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 16, srvs);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 17, 1, &srvs[17]);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 18, 1, &srvs[18]);
    return resources->has_splat;
}

/// @brief Release any temp SRVs the per-draw bind path created.
///
/// Called at end of each draw to clean up resources that didn't fit in
/// the texture/cubemap caches. Cached resources are no-ops here.
/// @param[in,out] resources Draw resource record whose temporary COM objects are released.
static void d3d11_release_temporary_resources(d3d_draw_resources_t *resources) {
    if (!resources)
        return;
    for (int i = 0; i < 11; i++)
        d3d11_release_temp_srv(&resources->textures[i]);
    d3d11_release_temp_srv(&resources->cubemap);
}

/// @brief End-to-end draw for a single non-instanced mesh command.
///
/// Composes everything: pack object/scene/light/material data into
/// cbuffers, prep skinning + morph buffers, bind textures + samplers,
/// bind pipeline state, get VB/IB (cached or dynamic), set them on
/// the IA, issue `DrawIndexed`, then release any temp resources.
/// @param[in,out] ctx_ptr Opaque D3D11 backend context.
/// @param[in] win Platform window handle, unused by the synchronous D3D11 draw path.
/// @param[in] cmd Borrowed draw command sanitized before packing.
/// @param[in] lights Borrowed light snapshot.
/// @param[in] light_count Number of entries in @p lights.
/// @param[in] ambient Optional RGB ambient vector.
/// @param[in] wireframe Nonzero for wireframe rasterization.
/// @param[in] backface_cull Nonzero for back-face culling.
static void d3d11_submit_draw(void *ctx_ptr,
                              vgfx_window_t win,
                              const vgfx3d_draw_cmd_t *cmd,
                              const vgfx3d_light_params_t *lights,
                              int32_t light_count,
                              const float *ambient,
                              int8_t wireframe,
                              int8_t backface_cull) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    d3d_per_object_t object_data;
    d3d_per_scene_t scene_data;
    d3d_per_material_t material_data;
    d3d_light_t light_data[VGFX3D_MAX_LIGHTS];
    d3d_draw_resources_t draw_resources;
    int has_splat;
    HRESULT hr;
    UINT stride = sizeof(vgfx3d_vertex_t);
    UINT offset = 0;
    ID3D11Buffer *mesh_vb = NULL;
    ID3D11Buffer *mesh_ib = NULL;
    uint32_t index_count;
    vgfx3d_draw_cmd_t safe_cmd;
    vgfx3d_light_params_t safe_lights[VGFX3D_MAX_LIGHTS];
    float safe_ambient[3];

    (void)win;
    if (!ctx)
        return;
    if (ctx->frame_submission_failed || !cmd || !cmd->vertices || !cmd->indices ||
        cmd->vertex_count == 0 || cmd->index_count == 0 ||
        !vgfx3d_d3d11_draw_submission_is_ready(ctx->frame_active,
                                               ctx->shadow_pass_slot,
                                               ctx->ctx != NULL,
                                               ctx->current_rtv_count,
                                               ctx->current_rtvs[0] != NULL,
                                               ctx->current_width,
                                               ctx->current_height)) {
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }
    vgfx3d_sanitize_draw_command(cmd, &safe_cmd);
    cmd = &safe_cmd;
    light_count = vgfx3d_sanitize_light_array(lights, light_count, safe_lights, VGFX3D_MAX_LIGHTS);
    for (int32_t light_index = 0; light_index < light_count; light_index++)
        vgfx3d_sanitize_light_shadow_span(&safe_lights[light_index], ctx->shadow_count);
    lights = light_count > 0 ? safe_lights : NULL;
    vgfx3d_sanitize_ambient_rgb(ambient, safe_ambient);
    ambient = safe_ambient;
    index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
    if (index_count == 0) {
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }

    d3d11_prepare_object_data(cmd, &object_data);
    d3d11_prepare_anim_resources(ctx, cmd, &object_data);

    hr = d3d11_update_constant_buffer(ctx, ctx->cb_per_object, &object_data, sizeof(object_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbPerObject)", hr);
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }
    /* Scene + light constants only change with the queued light-snapshot
     * revision; DISCARD-mapped buffers keep earlier draws consistent. */
    if (cmd->lights_revision == 0 || cmd->lights_revision != ctx->uploaded_lights_revision) {
        d3d11_prepare_scene_data(ctx, cmd, lights, light_count, ambient, &scene_data);
        d3d11_prepare_light_data(
            lights, scene_data.light_count, scene_data.shadow_count, light_data);
        hr = d3d11_update_constant_buffer(ctx, ctx->cb_per_scene, &scene_data, sizeof(scene_data));
        if (FAILED(hr)) {
            d3d11_log_hresult("Map(cbPerScene)", hr);
            d3d11_stats_count(&ctx->stats.dropped_draws);
            return;
        }
        hr = d3d11_update_constant_buffer(ctx, ctx->cb_per_lights, light_data, sizeof(light_data));
        if (FAILED(hr)) {
            d3d11_log_hresult("Map(cbPerLights)", hr);
            d3d11_stats_count(&ctx->stats.dropped_draws);
            return;
        }
        hr = d3d11_upload_cluster_table(ctx,
                                        (const vgfx3d_cluster_table_t *)cmd->cluster_table,
                                        cmd->lights_revision,
                                        scene_data.light_count);
        if (FAILED(hr)) {
            d3d11_log_hresult("Map(cbCluster)", hr);
            d3d11_stats_count(&ctx->stats.dropped_draws);
            return;
        }
        ctx->uploaded_lights_revision = cmd->lights_revision;
    }

    has_splat = d3d11_bind_draw_resources(ctx, cmd, &draw_resources);
    {
        float ibl_env_lod_base = 0.0f;
        float ibl_env_max_lod = 0.0f;
        int ibl_draw = d3d11_cmd_ibl_draw(ctx, cmd, &ibl_env_lod_base, &ibl_env_max_lod);

        d3d11_prepare_material_data(cmd,
                                    draw_resources.has_texture,
                                    draw_resources.has_normal_map,
                                    draw_resources.has_specular_map,
                                    draw_resources.has_emissive_map,
                                    draw_resources.has_env_map,
                                    has_splat,
                                    draw_resources.has_metallic_roughness_map,
                                    draw_resources.has_ao_map,
                                    ibl_draw,
                                    ibl_env_lod_base,
                                    ibl_env_max_lod,
                                    &material_data);
        /* Plan 10: soft-particle fade rides pbrScalars1.z, forced to 0 while no
         * snapshot exists (or the draw uses splat maps on GL-parity paths). */
        material_data.pbr_scalars1[2] =
            ctx->opaque_depth_valid
                ? vgfx3d_d3d11_clamp_float_param(cmd->soft_particle_fade, 0.0f, 1.0e6f, 0.0f)
                : 0.0f;
        material_data.pbr_scalars1[3] = vgfx3d_draw_cmd_ssr_mask(cmd);
    }
    hr = d3d11_update_constant_buffer(
        ctx, ctx->cb_per_material, &material_data, sizeof(material_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbPerMaterial)", hr);
        d3d11_unbind_draw_resources(ctx);
        d3d11_release_temporary_resources(&draw_resources);
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }

    if (!d3d11_acquire_mesh_buffers(ctx, cmd, &mesh_vb, &mesh_ib)) {
        d3d11_unbind_draw_resources(ctx);
        d3d11_release_temporary_resources(&draw_resources);
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }

    if (d3d11_cmd_uses_compact_stream(ctx, cmd))
        stride = VGFX3D_COMPACT_VERTEX_STRIDE; /* R20 packed static-cache layout */
    ID3D11DeviceContext_IASetVertexBuffers(ctx->ctx, 0, 1, &mesh_vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(ctx->ctx, mesh_ib, DXGI_FORMAT_R32_UINT, 0);
    d3d11_bind_main_pipeline(ctx, cmd, wireframe, backface_cull, 0);
    ID3D11DeviceContext_DrawIndexed(ctx->ctx, index_count, 0, 0);
    (void)d3d11_record_frame_device_status(ctx, "DrawIndexed(main)");
    d3d11_stats_count(&ctx->stats.draw_calls);

    d3d11_unbind_draw_resources(ctx);
    d3d11_release_temporary_resources(&draw_resources);
}

/// @brief Instanced version of `d3d11_submit_draw` (one DrawIndexedInstanced).
///
/// Same flow as the non-instanced path plus: pack per-instance matrix
/// data into a CPU scratch buffer, upload to the dynamic instance
/// vertex buffer, and bind that buffer to slot 1 alongside the mesh
/// VB at slot 0. The instanced input layout reads instance attributes
/// from slot 1 with `D3D11_INPUT_PER_INSTANCE_DATA`.
/// @param[in,out] ctx_ptr Opaque D3D11 backend context.
/// @param[in] win Platform window handle, unused by the synchronous D3D11 draw path.
/// @param[in] cmd Borrowed shared draw command.
/// @param[in] instance_matrices Row-major model transform per ordinary instance.
/// @param[in] instance_count Number of ordinary or particle instances.
/// @param[in] lights Borrowed light snapshot.
/// @param[in] light_count Number of entries in @p lights.
/// @param[in] ambient Optional RGB ambient vector.
/// @param[in] wireframe Nonzero for wireframe rasterization.
/// @param[in] backface_cull Nonzero for back-face culling.
static void d3d11_submit_draw_instanced(void *ctx_ptr,
                                        vgfx_window_t win,
                                        const vgfx3d_draw_cmd_t *cmd,
                                        const float *instance_matrices,
                                        int32_t instance_count,
                                        const vgfx3d_light_params_t *lights,
                                        int32_t light_count,
                                        const float *ambient,
                                        int8_t wireframe,
                                        int8_t backface_cull) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    d3d_per_object_t object_data;
    d3d_per_scene_t scene_data;
    d3d_per_material_t material_data;
    d3d_light_t light_data[VGFX3D_MAX_LIGHTS];
    d3d_draw_resources_t draw_resources;
    int has_splat;
    int particle_mode;
    HRESULT hr;
    size_t instance_upload_bytes;
    const void *instance_upload_source;
    size_t instance_stride;
    UINT strides[2];
    UINT offsets[2] = {0, 0};
    ID3D11Buffer *vertex_buffers[2];
    ID3D11Buffer *mesh_vb = NULL;
    ID3D11Buffer *mesh_ib = NULL;
    uint32_t index_count;
    vgfx3d_draw_cmd_t safe_cmd;
    vgfx3d_light_params_t safe_lights[VGFX3D_MAX_LIGHTS];
    float safe_ambient[3];

    (void)win;
    if (!ctx)
        return;
    if (ctx->frame_submission_failed || !cmd || instance_count <= 0 || !cmd->vertices ||
        !cmd->indices || (!instance_matrices && !cmd->particle_instances) ||
        !vgfx3d_d3d11_draw_submission_is_ready(ctx->frame_active,
                                               ctx->shadow_pass_slot,
                                               ctx->ctx != NULL,
                                               ctx->current_rtv_count,
                                               ctx->current_rtvs[0] != NULL,
                                               ctx->current_width,
                                               ctx->current_height)) {
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }
    vgfx3d_sanitize_draw_command(cmd, &safe_cmd);
    cmd = &safe_cmd;
    light_count = vgfx3d_sanitize_light_array(lights, light_count, safe_lights, VGFX3D_MAX_LIGHTS);
    for (int32_t light_index = 0; light_index < light_count; light_index++)
        vgfx3d_sanitize_light_shadow_span(&safe_lights[light_index], ctx->shadow_count);
    lights = light_count > 0 ? safe_lights : NULL;
    vgfx3d_sanitize_ambient_rgb(ambient, safe_ambient);
    ambient = safe_ambient;
    particle_mode = cmd->particle_instances != NULL;
    index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
    if (index_count == 0) {
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }

    instance_stride =
        particle_mode ? sizeof(vgfx3d_particle_instance_t) : sizeof(d3d_instance_data_t);
    if (!vgfx3d_d3d11_compute_instance_upload_bytes(
            instance_count, instance_stride, &instance_upload_bytes)) {
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }
    if (particle_mode) {
        instance_upload_source = cmd->particle_instances;
    } else {
        if (!d3d11_ensure_instance_upload_capacity(ctx, instance_count)) {
            d3d11_stats_count(&ctx->stats.dropped_draws);
            return;
        }
        vgfx3d_d3d11_fill_instance_data(
            ctx->instance_upload_data,
            instance_count,
            instance_matrices,
            cmd->prev_instance_matrices,
            (int8_t)vgfx3d_d3d11_should_use_previous_instance_matrices(
                cmd->prev_instance_matrices, cmd->has_prev_instance_matrices));
        instance_upload_source = ctx->instance_upload_data;
    }

    d3d11_prepare_object_data(cmd, &object_data);
    object_data.has_prev_model_matrix = 0;
    object_data.has_prev_instance_matrices = vgfx3d_d3d11_should_use_previous_instance_matrices(
        cmd->prev_instance_matrices, cmd->has_prev_instance_matrices);
    if (object_data.has_skinning && cmd->instance_bone_stride > 0) {
        object_data.instance_bone_stride = vgfx3d_d3d11_sanitize_instance_bone_stride(
            cmd->instance_bone_stride, cmd->bone_count, instance_count);
        if (object_data.instance_bone_stride == 0) {
            object_data.has_skinning = 0;
            object_data.has_prev_skinning = 0;
        }
    }
    d3d11_prepare_anim_resources(ctx, cmd, &object_data);

    hr = d3d11_update_constant_buffer(ctx, ctx->cb_per_object, &object_data, sizeof(object_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbPerObject)", hr);
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }
    /* Scene + light constants only change with the queued light-snapshot
     * revision; DISCARD-mapped buffers keep earlier draws consistent. */
    if (cmd->lights_revision == 0 || cmd->lights_revision != ctx->uploaded_lights_revision) {
        d3d11_prepare_scene_data(ctx, cmd, lights, light_count, ambient, &scene_data);
        d3d11_prepare_light_data(
            lights, scene_data.light_count, scene_data.shadow_count, light_data);
        hr = d3d11_update_constant_buffer(ctx, ctx->cb_per_scene, &scene_data, sizeof(scene_data));
        if (FAILED(hr)) {
            d3d11_log_hresult("Map(cbPerScene)", hr);
            d3d11_stats_count(&ctx->stats.dropped_draws);
            return;
        }
        hr = d3d11_update_constant_buffer(ctx, ctx->cb_per_lights, light_data, sizeof(light_data));
        if (FAILED(hr)) {
            d3d11_log_hresult("Map(cbPerLights)", hr);
            d3d11_stats_count(&ctx->stats.dropped_draws);
            return;
        }
        hr = d3d11_upload_cluster_table(ctx,
                                        (const vgfx3d_cluster_table_t *)cmd->cluster_table,
                                        cmd->lights_revision,
                                        scene_data.light_count);
        if (FAILED(hr)) {
            d3d11_log_hresult("Map(cbCluster)", hr);
            d3d11_stats_count(&ctx->stats.dropped_draws);
            return;
        }
        ctx->uploaded_lights_revision = cmd->lights_revision;
    }

    has_splat = d3d11_bind_draw_resources(ctx, cmd, &draw_resources);
    {
        float ibl_env_lod_base = 0.0f;
        float ibl_env_max_lod = 0.0f;
        int ibl_draw = d3d11_cmd_ibl_draw(ctx, cmd, &ibl_env_lod_base, &ibl_env_max_lod);

        d3d11_prepare_material_data(cmd,
                                    draw_resources.has_texture,
                                    draw_resources.has_normal_map,
                                    draw_resources.has_specular_map,
                                    draw_resources.has_emissive_map,
                                    draw_resources.has_env_map,
                                    has_splat,
                                    draw_resources.has_metallic_roughness_map,
                                    draw_resources.has_ao_map,
                                    ibl_draw,
                                    ibl_env_lod_base,
                                    ibl_env_max_lod,
                                    &material_data);
        /* Plan 10: soft-particle fade rides pbrScalars1.z, forced to 0 while no
         * snapshot exists (or the draw uses splat maps on GL-parity paths). */
        material_data.pbr_scalars1[2] =
            ctx->opaque_depth_valid
                ? vgfx3d_d3d11_clamp_float_param(cmd->soft_particle_fade, 0.0f, 1.0e6f, 0.0f)
                : 0.0f;
        material_data.pbr_scalars1[3] = vgfx3d_draw_cmd_ssr_mask(cmd);
    }
    hr = d3d11_update_constant_buffer(
        ctx, ctx->cb_per_material, &material_data, sizeof(material_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbPerMaterial)", hr);
        d3d11_unbind_draw_resources(ctx);
        d3d11_release_temporary_resources(&draw_resources);
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }

    if (!d3d11_acquire_mesh_buffers(ctx, cmd, &mesh_vb, &mesh_ib) ||
        !d3d11_upload_dynamic_buffer(ctx,
                                     &ctx->instance_buffer,
                                     &ctx->instance_buffer_size,
                                     D3D11_BIND_VERTEX_BUFFER,
                                     instance_upload_source,
                                     instance_upload_bytes,
                                     D3D11_INITIAL_INSTANCE_BUFFER_SIZE)) {
        d3d11_unbind_draw_resources(ctx);
        d3d11_release_temporary_resources(&draw_resources);
        d3d11_stats_count(&ctx->stats.dropped_draws);
        return;
    }

    strides[0] = d3d11_cmd_uses_compact_stream(ctx, cmd) ? VGFX3D_COMPACT_VERTEX_STRIDE
                                                         : sizeof(vgfx3d_vertex_t);
    strides[1] = (UINT)instance_stride;
    vertex_buffers[0] = mesh_vb;
    vertex_buffers[1] = ctx->instance_buffer;
    ID3D11DeviceContext_IASetVertexBuffers(ctx->ctx, 0, 2, vertex_buffers, strides, offsets);
    ID3D11DeviceContext_IASetIndexBuffer(ctx->ctx, mesh_ib, DXGI_FORMAT_R32_UINT, 0);
    d3d11_bind_main_pipeline(ctx, cmd, wireframe, backface_cull, 1);
    if (particle_mode) {
        ID3D11DeviceContext_IASetInputLayout(ctx->ctx, ctx->input_layout_particles);
        ID3D11DeviceContext_VSSetShader(ctx->ctx, ctx->vs_particles, NULL, 0);
    }
    ID3D11DeviceContext_DrawIndexedInstanced(ctx->ctx, index_count, instance_count, 0, 0, 0);
    (void)d3d11_record_frame_device_status(ctx, "DrawIndexedInstanced(main)");
    d3d11_stats_count(&ctx->stats.draw_calls);

    d3d11_unbind_draw_resources(ctx);
    d3d11_release_temporary_resources(&draw_resources);
}
