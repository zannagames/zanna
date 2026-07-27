//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_present.inc
// Purpose: D3D11 frame lifecycle, resize, readback, post-FX, presentation, shadows, and skybox.
// Key invariants:
//   - Frame matrices and temporal history remain finite, bounded, and mutually coherent.
//   - Render-target transitions restore complete D3D11 pipeline state on every path.
// Ownership/Lifetime:
//   - This file is included by vgfx3d_backend_d3d11.c and is not a standalone translation unit.
//   - COM targets, staging resources, and snapshots belong to d3d11_context_t.
// Links: vgfx3d_backend_d3d11.c, vgfx3d_backend_d3d11_draw.inc
//
//===----------------------------------------------------------------------===//

/// @file vgfx3d_backend_d3d11_present.inc
/// @brief Implements D3D11 frame lifecycle, presentation, readback, post-processing,
///   shadow-pass, and skybox operations.
/// @details This implementation fragment is included by the D3D11 backend translation
///   unit. It owns no context independently; every GPU resource referenced here is
///   borrowed from, created for, or released through the supplied backend context.

/// @brief Backend `clear` op — stores the clear color for the next BeginFrame.
///
/// We don't actually clear the targets here; that happens in
/// `BeginFrame`'s `clear_current_targets` call so we know which
/// target to clear (depending on RTT/scene/swapchain selection).
/// @param[in] ctx_ptr Opaque pointer to the D3D11 backend context.
/// @param[in] win Window handle associated with the clear request; unused by this backend.
/// @param[in] r Requested red clear component, clamped to the unit interval.
/// @param[in] g Requested green clear component, clamped to the unit interval.
/// @param[in] b Requested blue clear component, clamped to the unit interval.
static void d3d11_clear(void *ctx_ptr, vgfx_window_t win, float r, float g, float b) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    (void)win;
    if (!ctx)
        return;
    ctx->clear_r = vgfx3d_d3d11_clamp_float_param(r, 0.0f, 1.0f, 0.0f);
    ctx->clear_g = vgfx3d_d3d11_clamp_float_param(g, 0.0f, 1.0f, 0.0f);
    ctx->clear_b = vgfx3d_d3d11_clamp_float_param(b, 0.0f, 1.0f, 0.0f);
}

/// @brief Radical-inverse Halton sequence sample for TAA sub-pixel jitter (Plan 05).
/// @param[in] index One-based sequence index to evaluate.
/// @param[in] base Radix used to reflect the digits of @p index.
/// @return The radical-inverse sample in the half-open interval `[0, 1)`.
static float d3d11_halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

/// @brief True when the exported postfx chain contains an active effect of @p type_value.
/// @param[in] chain Post-processing chain to inspect; may be `NULL`.
/// @param[in] type_value Numeric effect type to locate.
/// @return Nonzero when an enabled effect with the requested type is present; otherwise zero.
static int d3d11_chain_has_effect(const vgfx3d_postfx_chain_t *chain, int32_t type_value) {
    return vgfx3d_d3d11_postfx_chain_has_active_effect(chain, type_value);
}

static void d3d11_harvest_depth_probes(d3d11_context_t *ctx);
static void d3d11_discard_depth_probe_staging(d3d11_context_t *ctx);

/// @brief Begin a D3D11 frame by capturing camera state, selecting targets, and clearing them.
/// @details Increments the frame serial, sanitizes and derives camera matrices, applies
///   temporal jitter when TAA is active, selects the RTT, scene, overlay, or swapchain
///   route, refreshes frame history, clears the selected targets, and binds common state.
///   Invalid input or an already-active frame leaves the context unchanged.
/// @param[in,out] ctx_ptr Opaque pointer to the D3D11 backend context.
/// @param[in] cam Camera and per-frame rendering parameters to sanitize and latch.
static void d3d11_begin_frame(void *ctx_ptr, const vgfx3d_camera_params_t *cam) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    HRESULT hr;
    int32_t scene_width;
    int32_t scene_height;
    int8_t prior_pending_present;
    vgfx3d_camera_params_t safe_cam;

    if (!ctx || !cam || ctx->frame_active || !ctx->ctx)
        return;
    prior_pending_present = ctx->frame_pending_present;
    vgfx3d_sanitize_camera_params(cam, &safe_cam);
    cam = &safe_cam;
    ctx->frame_active = 1;
    if (!ctx->rtt_active)
        ctx->frame_pending_present = 1;
    if (!vgfx3d_compute_scaled_scene_extent(
            ctx->width, ctx->height, ctx->render_scale, &scene_width, &scene_height)) {
        scene_width = ctx->width;
        scene_height = ctx->height;
    }

    if (ctx->frame_serial < UINT64_MAX)
        ctx->frame_serial++;
    ctx->presented_color_valid = 0;
    ctx->shadow_pass_slot = -1;
    /* Shadow atlas re-clears on its first tile pass each frame. */
    ctx->shadow_atlas_cleared = 0;
    /* Depth-probe slot ids restart each frame; harvest last frame's strip first. */
    if (!cam->load_existing_color) {
        d3d11_harvest_depth_probes(ctx);
        ctx->depth_probe_request_count = 0;
    }

    vgfx3d_d3d11_copy_mat4_finite_or_identity(ctx->view, cam->view);
    vgfx3d_d3d11_copy_mat4_finite_or_identity(ctx->projection, cam->projection);
    mat4f_mul_d3d(ctx->projection, ctx->view, ctx->vp);
    /* Plan 05 TAA: apply Halton(2,3) sub-pixel jitter to the scene-pass VP when the
     * latched chain contains a TAA resolve. Injected before inv/prev VP derivation so
     * every consumer this frame sees one consistent (jittered) VP; the resolve pass
     * removes the current-minus-previous jitter delta from velocities. Overlay passes
     * (load_existing_color) keep the previous state so UI stays fixed and the sequence
     * advances once per frame. */
    if (!ctx->rtt_active && !cam->load_existing_color) {
        ctx->taa_prev_jitter_clip[0] = ctx->taa_jitter_clip[0];
        ctx->taa_prev_jitter_clip[1] = ctx->taa_jitter_clip[1];
        ctx->taa_jitter_clip[0] = 0.0f;
        ctx->taa_jitter_clip[1] = 0.0f;
        if (ctx->gpu_postfx_enabled && ctx->gpu_postfx_chain_valid && ctx->ps_taa &&
            scene_width > 0 && scene_height > 0 &&
            d3d11_chain_has_effect(&ctx->gpu_postfx_chain, (int32_t)VGFX3D_POSTFX_EFFECT_TAA)) {
            uint32_t idx = (ctx->taa_frame_index++ & 7u) + 1u;
            float jx = (d3d11_halton(idx, 2) - 0.5f) * 2.0f / (float)scene_width;
            float jy = (d3d11_halton(idx, 3) - 0.5f) * 2.0f / (float)scene_height;
            ctx->taa_jitter_clip[0] = jx;
            ctx->taa_jitter_clip[1] = jy;
            /* Row-major clip-space translate: x-row += jx * w-row, y-row += jy * w-row. */
            for (int jc = 0; jc < 4; jc++) {
                ctx->vp[0 * 4 + jc] += jx * ctx->vp[3 * 4 + jc];
                ctx->vp[1 * 4 + jc] += jy * ctx->vp[3 * 4 + jc];
            }
        }
    }
    /* A finite view and projection can still overflow when multiplied, and a finite product
     * can be singular. Keep VP/inverse/history as one coherent identity pair in either case;
     * mixing a bad VP with an identity inverse creates full-screen TAA/SSAO reprojection streaks.
     */
    if (!vgfx3d_d3d11_float_array_is_bounded(ctx->vp, 16u, VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX) ||
        vgfx3d_invert_matrix4(ctx->vp, ctx->inv_vp) != 0) {
        memcpy(ctx->vp, k_identity4x4, sizeof(ctx->vp));
        memcpy(ctx->inv_vp, k_identity4x4, sizeof(ctx->inv_vp));
        ctx->taa_jitter_clip[0] = 0.0f;
        ctx->taa_jitter_clip[1] = 0.0f;
        ctx->taa_prev_jitter_clip[0] = 0.0f;
        ctx->taa_prev_jitter_clip[1] = 0.0f;
        ctx->taa_history_valid = 0;
        ctx->scene_history_valid = 0;
    }
    vgfx3d_d3d11_copy_float_array_clamped_finite_or(ctx->cam_pos,
                                                    cam->position,
                                                    3u,
                                                    -VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                    VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                    0.0f);
    {
        static const float forward_fallback[3] = {0.0f, 0.0f, -1.0f};
        vgfx3d_d3d11_copy_vec3_direction_or(ctx->cam_forward, cam->forward, forward_fallback);
    }
    ctx->cam_is_ortho = cam->is_ortho ? 1 : 0;
    ctx->fog_enabled = cam->fog_enabled;
    ctx->height_fog[0] = vgfx3d_d3d11_clamp_float_param(cam->height_fog_base,
                                                        -VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                        VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                        0.0f);
    ctx->height_fog[1] = vgfx3d_d3d11_clamp_float_param(
        cam->height_fog_falloff, 0.0f, VGFX3D_D3D11_POSTFX_SCALAR_MAX, 0.1f);
    ctx->height_fog[2] =
        cam->height_fog_enabled
            ? vgfx3d_d3d11_clamp_float_param(
                  cam->height_fog_density, 0.0f, VGFX3D_D3D11_POSTFX_SCALAR_MAX, 0.0f) *
                  vgfx3d_d3d11_clamp_float_param(cam->height_fog_blend, 0.0f, 1.0f, 0.0f)
            : 0.0f;
    ctx->height_fog[3] = 0.0f;
    ctx->height_fog_sun[0] = vgfx3d_d3d11_clamp_float_param(
        cam->height_fog_sun_color[0], 0.0f, VGFX3D_D3D11_POSTFX_SCALAR_MAX, 1.0f);
    ctx->height_fog_sun[1] = vgfx3d_d3d11_clamp_float_param(
        cam->height_fog_sun_color[1], 0.0f, VGFX3D_D3D11_POSTFX_SCALAR_MAX, 1.0f);
    ctx->height_fog_sun[2] = vgfx3d_d3d11_clamp_float_param(
        cam->height_fog_sun_color[2], 0.0f, VGFX3D_D3D11_POSTFX_SCALAR_MAX, 1.0f);
    ctx->height_fog_sun[3] =
        cam->height_fog_enabled
            ? vgfx3d_d3d11_clamp_float_param(cam->height_fog_sun_amount, 0.0f, 1.0f, 0.0f)
            : 0.0f;
    {
        static const float height_fog_sun_fallback[3] = {0.0f, 1.0f, 0.0f};
        vgfx3d_d3d11_copy_vec3_direction_or(
            ctx->height_fog_sun_dir, cam->height_fog_sun_dir, height_fog_sun_fallback);
    }
    ctx->height_fog_sun_dir[3] = vgfx3d_d3d11_clamp_float_param(
        cam->height_fog_sun_power, 0.0f, VGFX3D_D3D11_POSTFX_SCALAR_MAX, 8.0f);
    vgfx3d_d3d11_sanitize_fog_range(cam->fog_near, cam->fog_far, &ctx->fog_near, &ctx->fog_far);
    ctx->fog_color[0] = vgfx3d_d3d11_clamp_float_param(cam->fog_color[0], 0.0f, 1.0f, 0.5f);
    ctx->fog_color[1] = vgfx3d_d3d11_clamp_float_param(cam->fog_color[1], 0.0f, 1.0f, 0.5f);
    ctx->fog_color[2] = vgfx3d_d3d11_clamp_float_param(cam->fog_color[2], 0.0f, 1.0f, 0.5f);
    ctx->ibl_enabled = cam->ibl_enabled ? 1 : 0;
    ctx->ibl_intensity = vgfx3d_d3d11_clamp_float_param(cam->ibl_intensity, 0.0f, 8.0f, 1.0f);
    vgfx3d_d3d11_copy_float_array_clamped_finite_or(ctx->ibl_sh,
                                                    cam->ibl_sh,
                                                    27u,
                                                    -VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                    VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                                                    0.0f);
    ctx->shadow_strength = cam->shadow_strength;
    ctx->shadow_slope_bias = vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(cam->shadow_slope_bias);
    ctx->shadow_quality = cam->shadow_quality;
    ctx->uploaded_lights_revision = 0; /* new frame: re-upload scene/light constants */
    /* Plan 10: the opaque depth snapshot is per-pass state. */
    ctx->opaque_depth_valid = 0;
    vgfx3d_d3d11_sanitize_cluster_depth_range(
        cam->znear, cam->zfar, &ctx->cam_znear, &ctx->cam_zfar);
    if (vgfx3d_d3d11_should_reset_composited_swapchain_for_frame(ctx->rtt_active,
                                                                 cam->load_existing_color))
        ctx->scene_composited_to_swapchain = 0;
    ctx->active_target_kind = vgfx3d_d3d11_choose_target_kind(
        ctx->rtt_active, d3d11_window_scene_route_enabled(ctx), cam->load_existing_color);
    if (!ctx->rtt_active && cam->load_existing_color && ctx->scene_composited_to_swapchain)
        ctx->active_target_kind = VGFX3D_D3D11_TARGET_SWAPCHAIN;

    if ((!ctx->rtv || !ctx->depth_tex || !ctx->dsv) &&
        vgfx3d_d3d11_is_valid_texture2d_extent(ctx->width, ctx->height)) {
        d3d11_release_swapchain_main_targets(ctx);
        hr = d3d11_recreate_swapchain_main_targets(
            ctx, ctx->width, ctx->height, "Recreate missing swapchain targets");
        if (FAILED(hr))
            d3d11_log_hresult("Recreate missing swapchain targets", hr);
    }

    if (!ctx->rtt_active && ctx->active_target_kind != VGFX3D_D3D11_TARGET_SWAPCHAIN) {
        hr = d3d11_ensure_scene_targets(ctx, scene_width, scene_height);
        if (FAILED(hr)) {
            d3d11_log_hresult("CreateSceneTargets", hr);
            ctx->active_target_kind = VGFX3D_D3D11_TARGET_SWAPCHAIN;
        }
    }
    if (ctx->active_target_kind == VGFX3D_D3D11_TARGET_OVERLAY) {
        hr = d3d11_ensure_overlay_target(ctx, ctx->width, ctx->height);
        if (FAILED(hr))
            d3d11_log_hresult("CreateOverlayTarget", hr);
    }
    d3d11_refresh_pass_flags(ctx, cam);
    d3d11_select_current_targets(ctx);
    if (!vgfx3d_d3d11_draw_submission_is_ready(ctx->frame_active,
                                               ctx->shadow_pass_slot,
                                               ctx->ctx != NULL,
                                               ctx->current_rtv_count,
                                               ctx->current_rtvs[0] != NULL,
                                               ctx->current_width,
                                               ctx->current_height)) {
        d3d11_clear_current_target_bindings(ctx);
        ctx->frame_active = 0;
        ctx->frame_pending_present = prior_pending_present;
        return;
    }

    if (!ctx->current_pass_is_overlay) {
        ctx->texture_upload_bytes = 0;
        ctx->shadow_count = 0;
        memset(ctx->shadow_slot_complete, 0, sizeof(ctx->shadow_slot_complete));
        ctx->overlay_used_this_frame = 0;
    }

    {
        vgfx3d_d3d11_frame_history_t history;

        memcpy(history.scene_vp, ctx->scene_vp, sizeof(history.scene_vp));
        memcpy(history.scene_prev_vp, ctx->scene_prev_vp, sizeof(history.scene_prev_vp));
        memcpy(history.scene_inv_vp, ctx->scene_inv_vp, sizeof(history.scene_inv_vp));
        memcpy(history.draw_prev_vp, ctx->draw_prev_vp, sizeof(history.draw_prev_vp));
        memcpy(history.scene_cam_pos, ctx->scene_cam_pos, sizeof(history.scene_cam_pos));
        history.scene_history_valid = ctx->scene_history_valid;
        history.overlay_used_this_frame = ctx->overlay_used_this_frame;
        vgfx3d_d3d11_update_frame_history(
            &history,
            ctx->vp,
            ctx->inv_vp,
            ctx->cam_pos,
            ctx->current_pass_is_overlay,
            (int8_t)vgfx3d_d3d11_uses_separate_overlay_target(ctx->active_target_kind,
                                                              d3d11_has_overlay_target(ctx)));
        memcpy(ctx->scene_vp, history.scene_vp, sizeof(ctx->scene_vp));
        memcpy(ctx->scene_prev_vp, history.scene_prev_vp, sizeof(ctx->scene_prev_vp));
        memcpy(ctx->scene_inv_vp, history.scene_inv_vp, sizeof(ctx->scene_inv_vp));
        memcpy(ctx->draw_prev_vp, history.draw_prev_vp, sizeof(ctx->draw_prev_vp));
        memcpy(ctx->scene_cam_pos, history.scene_cam_pos, sizeof(ctx->scene_cam_pos));
        ctx->scene_history_valid = history.scene_history_valid;
        ctx->overlay_used_this_frame = history.overlay_used_this_frame;
    }

    d3d11_clear_current_targets(ctx, ctx->current_load_existing_color, cam->load_existing_depth);
    d3d11_bind_common_state(ctx, NULL);
    d3d11_begin_frame_timing(ctx);
    if ((ctx->frame_serial & 31u) == 0u) {
        d3d11_prune_texture_cache(ctx);
        d3d11_prune_cubemap_cache(ctx);
    }
}

/// @brief Copy this frame's queued scene-depth probes into the staging strip.
/// @details Selects RTT, offscreen-scene, or swapchain depth according to the
///   active route, detaches all read/write bindings, and issues one 1x1 copy per
///   probe. The next frame's begin harvests the strip with a non-blocking Map.
/// @param[in,out] ctx Active D3D11 backend context whose queued probes are consumed.
static void d3d11_issue_depth_probes(d3d11_context_t *ctx) {
    int32_t pixels[VGFX3D_DEPTH_PROBE_MAX][2];
    ID3D11Texture2D *source = NULL;
    int32_t source_width = 0;
    int32_t source_height = 0;
    vgfx3d_d3d11_target_kind_t source_kind;

    if (!ctx || ctx->depth_probe_request_count <= 0)
        return;
    /* Never overwrite the single staging strip while a non-blocking Map is still pending.
     * Slot ids are frame-local, so dropping this frame is safer than publishing mismatched data. */
    if (ctx->depth_probe_pending_count > 0) {
        ctx->depth_probe_request_count = 0;
        return;
    }
    if (!ctx->ctx) {
        ctx->depth_probe_request_count = 0;
        return;
    }
    source_kind = vgfx3d_d3d11_choose_depth_probe_target(
        ctx->rtt_active,
        d3d11_window_scene_route_enabled(ctx),
        d3d11_has_rtt_targets(ctx),
        d3d11_has_scene_targets(ctx),
        ctx->depth_tex && ctx->dsv &&
            vgfx3d_d3d11_is_valid_texture2d_extent(ctx->width, ctx->height));
    if (source_kind == VGFX3D_D3D11_TARGET_RTT) {
        source = ctx->rtt_depth_tex;
        source_width = ctx->rtt_width;
        source_height = ctx->rtt_height;
    } else if (source_kind == VGFX3D_D3D11_TARGET_SCENE) {
        source = ctx->scene_depth_tex;
        source_width = ctx->scene_width;
        source_height = ctx->scene_height;
    } else if (source_kind == VGFX3D_D3D11_TARGET_SWAPCHAIN) {
        source = ctx->depth_tex;
        source_width = ctx->width;
        source_height = ctx->height;
    }
    if (!source || !vgfx3d_d3d11_is_valid_texture2d_extent(source_width, source_height)) {
        ctx->depth_probe_request_count = 0;
        return;
    }
    if (ctx->depth_probe_request_count > VGFX3D_DEPTH_PROBE_MAX) {
        ctx->depth_probe_request_count = 0;
        return;
    }
    /* Validate the whole batch before the first GPU copy. Publishing the original count after
     * skipping one malformed request would expose stale staging data under that request's slot. */
    for (int32_t i = 0; i < ctx->depth_probe_request_count; i++) {
        if (!vgfx3d_d3d11_ndc_to_pixel(
                ctx->depth_probe_requests[i][0], source_width, 0, &pixels[i][0]) ||
            !vgfx3d_d3d11_ndc_to_pixel(
                ctx->depth_probe_requests[i][1], source_height, 1, &pixels[i][1])) {
            ctx->depth_probe_request_count = 0;
            return;
        }
    }
    if (!ctx->depth_probe_staging) {
        if (FAILED(d3d11_create_staging_texture(ctx,
                                                VGFX3D_DEPTH_PROBE_MAX,
                                                1,
                                                DXGI_FORMAT_R32_TYPELESS,
                                                &ctx->depth_probe_staging)) ||
            !ctx->depth_probe_staging) {
            ctx->depth_probe_request_count = 0;
            return;
        }
    }
    d3d11_unbind_draw_resources(ctx);
    d3d11_unbind_output_targets(ctx);
    for (int32_t i = 0; i < ctx->depth_probe_request_count; i++) {
        D3D11_BOX box;
        box.left = (UINT)pixels[i][0];
        box.top = (UINT)pixels[i][1];
        box.front = 0;
        box.right = (UINT)pixels[i][0] + 1;
        box.bottom = (UINT)pixels[i][1] + 1;
        box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(ctx->ctx,
                                                  (ID3D11Resource *)ctx->depth_probe_staging,
                                                  0,
                                                  (UINT)i,
                                                  0,
                                                  0,
                                                  (ID3D11Resource *)source,
                                                  0,
                                                  &box);
    }
    {
        HRESULT device_status =
            ctx->device ? ID3D11Device_GetDeviceRemovedReason(ctx->device) : E_POINTER;
        if (FAILED(device_status)) {
            d3d11_log_hresult("CopySubresourceRegion(depthProbeStaging)", device_status);
            d3d11_log_device_removed_reason(
                ctx, "CopySubresourceRegion(depthProbeStaging)", device_status);
            d3d11_restore_current_target_bindings(ctx);
            ctx->depth_probe_request_count = 0;
            d3d11_discard_depth_probe_staging(ctx);
            return;
        }
    }
    d3d11_restore_current_target_bindings(ctx);
    ctx->depth_probe_pending_count = ctx->depth_probe_request_count;
    ctx->depth_probe_pending_polls = 0;
    ctx->depth_probe_request_count = 0;
}

/// @brief Drop a failed/stale probe staging strip so a later frame can recreate it.
/// @param[in,out] ctx Active D3D11 backend context whose staging state is reset.
static void d3d11_discard_depth_probe_staging(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    SAFE_RELEASE(ctx->depth_probe_staging);
    ctx->depth_probe_pending_count = 0;
    ctx->depth_probe_pending_polls = 0;
    ctx->depth_probe_result_count = 0;
}

/// @brief Harvest the previous frame's probe strip without blocking the pipeline.
/// @param[in,out] ctx Active D3D11 backend context receiving completed probe results.
static void d3d11_harvest_depth_probes(d3d11_context_t *ctx) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (!ctx || ctx->depth_probe_pending_count <= 0)
        return;
    if (!ctx->depth_probe_staging || !ctx->ctx) {
        d3d11_discard_depth_probe_staging(ctx);
        return;
    }
    hr = ID3D11DeviceContext_Map(ctx->ctx,
                                 (ID3D11Resource *)ctx->depth_probe_staging,
                                 0,
                                 D3D11_MAP_READ,
                                 D3D11_MAP_FLAG_DO_NOT_WAIT,
                                 &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (ctx->depth_probe_pending_polls < UINT32_MAX)
            ctx->depth_probe_pending_polls++;
        if (vgfx3d_d3d11_should_abandon_depth_probe(ctx->depth_probe_pending_polls))
            d3d11_discard_depth_probe_staging(ctx);
        return;
    }
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(depthProbeStaging)", hr);
        d3d11_log_device_removed_reason(ctx, "Map(depthProbeStaging)", hr);
        d3d11_discard_depth_probe_staging(ctx);
        return;
    }
    if (!mapped.pData ||
        mapped.RowPitch < (UINT)((uint32_t)ctx->depth_probe_pending_count * sizeof(float))) {
        ID3D11DeviceContext_Unmap(ctx->ctx, (ID3D11Resource *)ctx->depth_probe_staging, 0);
        d3d11_log_hresult("Map(depthProbeStaging payload)", E_POINTER);
        d3d11_discard_depth_probe_staging(ctx);
        return;
    }
    /* Probe contract is canonical window depth (larger = farther); the scene depth
     * buffer is reversed-Z, so flip and validate every published sample. */
    {
        const float *src = (const float *)mapped.pData;
        for (int32_t i = 0; i < ctx->depth_probe_pending_count; i++)
            ctx->depth_probe_results[i] = vgfx3d_d3d11_sanitize_depth_probe_result(src[i]);
        ctx->depth_probe_result_count = ctx->depth_probe_pending_count;
    }
    ID3D11DeviceContext_Unmap(ctx->ctx, (ID3D11Resource *)ctx->depth_probe_staging, 0);
    ctx->depth_probe_pending_count = 0;
    ctx->depth_probe_pending_polls = 0;
}

/// @brief Backend hook: choose the swapchain present sync interval (1 = vsync, 0 = off).
/// @param[in,out] ctx_ptr Opaque pointer to the D3D11 backend context.
/// @param[in] enabled Nonzero to present with a synchronization interval of one.
static void d3d11_set_vsync(void *ctx_ptr, int8_t enabled) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    if (!ctx)
        return;
    ctx->present_sync_interval = enabled ? 1 : 0;
}

/// @brief Backend hook: register one NDC scene-depth probe for this frame.
/// @param[in,out] ctx_ptr Opaque pointer to the active D3D11 backend context.
/// @param[in] ndc_x Horizontal normalized-device coordinate, clamped to `[-1, 1]`.
/// @param[in] ndc_y Vertical normalized-device coordinate, clamped to `[-1, 1]`.
/// @return Frame-local probe slot, or `-1` when the request cannot be queued.
static int32_t d3d11_queue_depth_probe(void *ctx_ptr, float ndc_x, float ndc_y) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    int32_t slot;
    if (!ctx || !ctx->frame_active || ctx->depth_probe_pending_count > 0 ||
        ctx->depth_probe_request_count >= VGFX3D_DEPTH_PROBE_MAX || !isfinite(ndc_x) ||
        !isfinite(ndc_y))
        return -1;
    slot = ctx->depth_probe_request_count++;
    ctx->depth_probe_requests[slot][0] = vgfx3d_d3d11_clamp_float_param(ndc_x, -1.0f, 1.0f, 0.0f);
    ctx->depth_probe_requests[slot][1] = vgfx3d_d3d11_clamp_float_param(ndc_y, -1.0f, 1.0f, 0.0f);
    return slot;
}

/// @brief Backend hook: read a probe result captured on a previous frame.
/// @param[in] ctx_ptr Opaque pointer to the D3D11 backend context.
/// @param[in] slot Frame-local probe slot returned by `d3d11_queue_depth_probe()`.
/// @return Canonical window-depth sample, or `-1.0f` when the slot is unavailable.
static float d3d11_read_depth_probe(void *ctx_ptr, int32_t slot) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    if (!ctx || slot < 0 || slot >= ctx->depth_probe_result_count)
        return -1.0f;
    return ctx->depth_probe_results[slot];
}

/// @brief Backend `end_frame` — mark RTT pixels dirty for lazy host readback.
///
/// CPU readback is deferred until a caller explicitly asks for pixels.
/// This keeps render-to-texture frames on the GPU fast instead of forcing
/// a staging copy at the end of every RTT frame.
/// @param[in,out] ctx_ptr Opaque pointer to the active D3D11 backend context.
static void d3d11_end_frame(void *ctx_ptr) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;

    if (!ctx || !ctx->frame_active)
        return;
    ctx->frame_active = 0;
    d3d11_issue_depth_probes(ctx);
    d3d11_end_frame_timing(ctx);
    if (!vgfx3d_d3d11_should_mark_rtt_dirty(ctx->rtt_active,
                                            ctx->rtt_target != NULL,
                                            ctx->rtt_color_tex != NULL,
                                            ctx->rtt_rtv != NULL,
                                            ctx->rtt_depth_tex != NULL,
                                            ctx->rtt_dsv != NULL,
                                            ctx->rtt_staging != NULL))
        return;
    ctx->rtt_target->sync_color = d3d11_sync_render_target_color;
    ctx->rtt_target->sync_color_userdata = ctx;
    ctx->rtt_target->color_dirty = 1;
    ctx->rtt_target->hdr_color_valid = 0;
}

/// @brief Read the total bytes uploaded to D3D11 textures so far this frame (diagnostics counter).
/// @param[in] ctx_ptr Opaque pointer to the D3D11 backend context.
/// @return Accumulated upload byte count, or zero when @p ctx_ptr is `NULL`.
static uint64_t d3d11_get_texture_upload_bytes(void *ctx_ptr) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    return ctx ? ctx->texture_upload_bytes : 0;
}

/// @brief Release swapchain-owned color/depth targets and clear stale CPU binding mirrors.
/// @param[in,out] ctx Active D3D11 backend context whose main targets are released.
static void d3d11_release_swapchain_main_targets(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    d3d11_unbind_output_targets(ctx);
    if (ctx->current_target_kind == VGFX3D_D3D11_TARGET_SWAPCHAIN)
        d3d11_clear_current_target_bindings(ctx);
    SAFE_RELEASE(ctx->dsv);
    SAFE_RELEASE(ctx->depth_tex);
    SAFE_RELEASE(ctx->rtv);
}

/// @brief Rebuild the swapchain-derived main render targets (back-buffer RTV, scene/depth
///   textures) at @p width × @p height after a resize or device reset; @p log_context
///   tags any failure log. Returns the first failing HRESULT, or S_OK on success.
/// @param[in,out] ctx Active context whose swapchain-derived targets are replaced.
/// @param[in] width Required back-buffer width in pixels.
/// @param[in] height Required back-buffer height in pixels.
/// @param[in] log_context Optional label used when reporting creation failures.
/// @return `S_OK` on success, `E_INVALIDARG` for invalid inputs, or the first D3D11 failure.
static HRESULT d3d11_recreate_swapchain_main_targets(d3d11_context_t *ctx,
                                                     int32_t width,
                                                     int32_t height,
                                                     const char *log_context) {
    ID3D11Texture2D *back_buffer = NULL;
    ID3D11RenderTargetView *new_rtv = NULL;
    ID3D11Texture2D *new_depth_tex = NULL;
    ID3D11DepthStencilView *new_dsv = NULL;
    D3D11_TEXTURE2D_DESC back_buffer_desc;
    HRESULT hr;

    if (!ctx || !ctx->device || !ctx->swap_chain ||
        !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return E_INVALIDARG;

    hr = IDXGISwapChain_GetBuffer(ctx->swap_chain, 0, &IID_ID3D11Texture2D, (void **)&back_buffer);
    hr = d3d11_device_child_result(hr, (ID3D11DeviceChild *)back_buffer, ctx->device);
    if (FAILED(hr)) {
        HRESULT failure = hr;
        d3d11_log_hresult(log_context ? log_context : "IDXGISwapChain::GetBuffer", failure);
        d3d11_log_device_removed_reason(ctx, "IDXGISwapChain::GetBuffer", failure);
        SAFE_RELEASE(back_buffer);
        return failure;
    }
    ID3D11Texture2D_GetDesc(back_buffer, &back_buffer_desc);
    if (back_buffer_desc.Width != (UINT)width || back_buffer_desc.Height != (UINT)height ||
        back_buffer_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        !vgfx3d_d3d11_is_single_subresource_texture2d(back_buffer_desc.Width,
                                                      back_buffer_desc.Height,
                                                      back_buffer_desc.MipLevels,
                                                      back_buffer_desc.ArraySize,
                                                      back_buffer_desc.SampleDesc.Count,
                                                      back_buffer_desc.SampleDesc.Quality)) {
        SAFE_RELEASE(back_buffer);
        d3d11_log_hresult(log_context ? log_context : "Validate swapchain backbuffer", E_FAIL);
        return E_FAIL;
    }
    hr = ID3D11Device_CreateRenderTargetView(
        ctx->device, (ID3D11Resource *)back_buffer, NULL, &new_rtv);
    if (FAILED(hr) || !new_rtv ||
        !d3d11_rtv_matches_texture2d(new_rtv, back_buffer, back_buffer_desc.Format)) {
        HRESULT failure = FAILED(hr) ? hr : (new_rtv ? E_FAIL : E_POINTER);
        d3d11_log_hresult(log_context ? log_context : "CreateRenderTargetView(backbuffer)",
                          failure);
        d3d11_log_device_removed_reason(ctx, "CreateRenderTargetView(backbuffer)", failure);
        SAFE_RELEASE(new_rtv);
        SAFE_RELEASE(back_buffer);
        return failure;
    }
    SAFE_RELEASE(back_buffer);
    hr = d3d11_create_depth_target(ctx, width, height, 0, &new_depth_tex, &new_dsv, NULL);
    if (FAILED(hr) || !new_depth_tex || !new_dsv) {
        HRESULT failure = FAILED(hr) ? hr : E_POINTER;
        d3d11_log_hresult(log_context ? log_context : "CreateTexture2D/DepthStencilView(main)",
                          failure);
        d3d11_log_device_removed_reason(ctx, "CreateTexture2D/DepthStencilView(main)", failure);
        SAFE_RELEASE(new_dsv);
        SAFE_RELEASE(new_depth_tex);
        SAFE_RELEASE(new_rtv);
        return failure;
    }
    SAFE_RELEASE(ctx->dsv);
    SAFE_RELEASE(ctx->depth_tex);
    SAFE_RELEASE(ctx->rtv);
    ctx->rtv = new_rtv;
    ctx->depth_tex = new_depth_tex;
    ctx->dsv = new_dsv;
    return S_OK;
}

/// @brief Backend `resize` — handle window-size changes.
///
/// Unbinds targets, releases the backbuffer RTV + main depth, calls
/// `IDXGISwapChain::ResizeBuffers`, gets the new backbuffer, recreates
/// RTV + depth target. Scene-targets are also destroyed; they get
/// recreated in the next `BeginFrame` at the new size. No-op if the
/// dimensions match.
/// @param[in,out] ctx_ptr Opaque pointer to the idle D3D11 backend context.
/// @param[in] w Requested swapchain width in pixels.
/// @param[in] h Requested swapchain height in pixels.
static void d3d11_resize(void *ctx_ptr, int32_t w, int32_t h) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    int32_t old_w;
    int32_t old_h;
    HRESULT hr;

    if (!ctx || !ctx->swap_chain || ctx->frame_active ||
        !vgfx3d_d3d11_is_valid_texture2d_extent(w, h))
        return;
    if (ctx->width == w && ctx->height == h && ctx->rtv && ctx->depth_tex && ctx->dsv)
        return;
    if (ctx->width == w && ctx->height == h) {
        hr = d3d11_recreate_swapchain_main_targets(
            ctx, w, h, "Recreate incomplete same-size swapchain targets");
        if (SUCCEEDED(hr))
            d3d11_bind_swapchain_target(ctx);
        return;
    }
    old_w = ctx->width;
    old_h = ctx->height;

    d3d11_unbind_draw_resources(ctx);
    d3d11_release_swapchain_main_targets(ctx);
    d3d11_clear_current_target_bindings(ctx);

    hr = IDXGISwapChain_ResizeBuffers(ctx->swap_chain, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        d3d11_log_hresult("IDXGISwapChain::ResizeBuffers", hr);
        d3d11_log_device_removed_reason(ctx, "IDXGISwapChain::ResizeBuffers", hr);
        if (SUCCEEDED(d3d11_recreate_swapchain_main_targets(
                ctx, old_w, old_h, "Recreate swapchain targets after failed resize"))) {
            d3d11_bind_swapchain_target(ctx);
        }
        return;
    }

    ctx->frame_pending_present = 0;
    ctx->scene_composited_to_swapchain = 0;
    ctx->presented_color_valid = 0;
    d3d11_destroy_scene_targets(ctx);
    ctx->width = w;
    ctx->height = h;
    d3d11_reset_temporal_scene_state(ctx);
    hr =
        d3d11_recreate_swapchain_main_targets(ctx, w, h, "Recreate swapchain targets after resize");
    if (FAILED(hr))
        return;
    d3d11_bind_swapchain_target(ctx);
}

/// @brief Copy a mapped texture's rows to an RGBA8 destination, format-aware.
///
/// Direct memcpy for R8G8B8A8_UNORM. For R16G16B16A16_FLOAT, decodes
/// each half to float then quantizes to 8-bit. Other formats are
/// silently skipped.
/// @param[out] dst_rgba Destination buffer receiving tightly packed RGBA8 pixels by row.
/// @param[in] dst_stride Destination row stride in bytes.
/// @param[in] copy_w Number of pixels to copy from each row.
/// @param[in] copy_h Number of rows to copy.
/// @param[in] mapped Mapped D3D11 source payload and row pitch.
/// @param[in] format Source texture format.
/// @return One when the requested conversion succeeds; otherwise zero.
static int d3d11_copy_mapped_texture_to_rgba8(uint8_t *dst_rgba,
                                              int32_t dst_stride,
                                              int32_t copy_w,
                                              int32_t copy_h,
                                              const D3D11_MAPPED_SUBRESOURCE *mapped,
                                              DXGI_FORMAT format) {
    if (!dst_rgba || !mapped || !mapped->pData || copy_w <= 0 || copy_h <= 0)
        return 0;

    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        size_t src_row_bytes;
        size_t dst_row_bytes;

        if (mapped->RowPitch > (UINT)INT_MAX ||
            !vgfx3d_d3d11_validate_mapped_texture_copy(
                copy_w, dst_stride, mapped->RowPitch, 8, &src_row_bytes, &dst_row_bytes))
            return 0;
        (void)src_row_bytes;
        (void)dst_row_bytes;
        vgfx3d_copy_linear_rgba16f_to_rgba8(dst_rgba,
                                            dst_stride,
                                            copy_w,
                                            copy_h,
                                            (const uint16_t *)mapped->pData,
                                            (int32_t)mapped->RowPitch);
        return 1;
    }

    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        size_t src_row_bytes;
        size_t dst_row_bytes;

        if (!vgfx3d_d3d11_validate_mapped_texture_copy(
                copy_w, dst_stride, mapped->RowPitch, 4, &src_row_bytes, &dst_row_bytes))
            return 0;
        (void)src_row_bytes;
        for (int32_t y = 0; y < copy_h; y++) {
            uint8_t *dst_row = dst_rgba + (size_t)y * (size_t)dst_stride;
            const uint8_t *src_row = (const uint8_t *)mapped->pData + (size_t)y * mapped->RowPitch;
            memcpy(dst_row, src_row, dst_row_bytes);
        }
        return 1;
    }
    return 0;
}

/// @brief Read back a Texture2D into a host RGBA8 buffer.
///
/// Reuses a staging texture matching the source's format + size, CopyResource into it,
/// Map(READ), copy rows. Caller's destination doesn't have to match the source size —
/// clipping is applied. Returns 0 on any failure.
/// @param[in,out] ctx Active D3D11 backend context that owns the reusable staging texture.
/// @param[in] source_tex Source GPU texture to copy and map.
/// @param[out] dst_rgba Host buffer receiving RGBA8 pixels; cleared before readback.
/// @param[in] w Requested destination width in pixels.
/// @param[in] h Requested destination height in pixels.
/// @param[in] stride Destination row stride in bytes.
/// @return One when the source is copied and converted successfully; otherwise zero.
static int d3d11_readback_texture_rgba(d3d11_context_t *ctx,
                                       ID3D11Texture2D *source_tex,
                                       uint8_t *dst_rgba,
                                       int32_t w,
                                       int32_t h,
                                       int32_t stride) {
    ID3D11Texture2D *staging;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_TEXTURE2D_DESC staging_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    int32_t copy_w;
    int32_t copy_h;
    size_t clear_bytes;
    int restore_target = 0;
    int mapped_ok = 0;
    int discard_staging = 0;
    int ok = 0;

    if (!dst_rgba || !vgfx3d_d3d11_validate_rgba8_destination(w, h, stride, &clear_bytes))
        return 0;
    memset(dst_rgba, 0, clear_bytes);
    if (!ctx || !ctx->ctx || !source_tex)
        return 0;

    ID3D11Texture2D_GetDesc(source_tex, &desc);
    if (!vgfx3d_d3d11_is_single_subresource_texture2d(desc.Width,
                                                      desc.Height,
                                                      desc.MipLevels,
                                                      desc.ArraySize,
                                                      desc.SampleDesc.Count,
                                                      desc.SampleDesc.Quality) ||
        (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        return 0;
    }
    hr = d3d11_ensure_readback_staging_texture(
        ctx, (int32_t)desc.Width, (int32_t)desc.Height, desc.Format);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateTexture2D(sceneStaging)", hr);
        d3d11_log_device_removed_reason(ctx, "CreateTexture2D(sceneStaging)", hr);
        return 0;
    }
    staging = ctx->readback_staging;
    if (!staging)
        return 0;
    ID3D11Texture2D_GetDesc(staging, &staging_desc);
    if (!vgfx3d_d3d11_is_single_subresource_texture2d(staging_desc.Width,
                                                      staging_desc.Height,
                                                      staging_desc.MipLevels,
                                                      staging_desc.ArraySize,
                                                      staging_desc.SampleDesc.Count,
                                                      staging_desc.SampleDesc.Quality) ||
        staging_desc.Width != desc.Width || staging_desc.Height != desc.Height ||
        staging_desc.Format != desc.Format || staging_desc.Usage != D3D11_USAGE_STAGING ||
        staging_desc.BindFlags != 0 || staging_desc.CPUAccessFlags != D3D11_CPU_ACCESS_READ ||
        staging_desc.MiscFlags != 0) {
        d3d11_release_readback_staging_texture(ctx);
        return 0;
    }

    restore_target = (ctx->current_rtv_count > 0 || ctx->current_dsv) ? 1 : 0;
    d3d11_unbind_draw_resources(ctx);
    d3d11_unbind_output_targets(ctx);
    ID3D11DeviceContext_CopyResource(
        ctx->ctx, (ID3D11Resource *)staging, (ID3D11Resource *)source_tex);
    hr =
        ID3D11DeviceContext_Map(ctx->ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(sceneStaging)", hr);
        d3d11_log_device_removed_reason(ctx, "Map(sceneStaging)", hr);
        discard_staging = 1;
        goto cleanup;
    }
    mapped_ok = 1;
    if (!mapped.pData) {
        discard_staging = 1;
        goto cleanup;
    }

    copy_w = (int32_t)desc.Width < w ? (int32_t)desc.Width : w;
    copy_h = (int32_t)desc.Height < h ? (int32_t)desc.Height : h;
    ok = d3d11_copy_mapped_texture_to_rgba8(dst_rgba, stride, copy_w, copy_h, &mapped, desc.Format);
    if (!ok)
        discard_staging = 1;

cleanup:
    if (mapped_ok)
        ID3D11DeviceContext_Unmap(ctx->ctx, (ID3D11Resource *)staging, 0);
    if (restore_target)
        d3d11_restore_current_target_bindings(ctx);
    if (discard_staging)
        d3d11_release_readback_staging_texture(ctx);
    return ok;
}

/// @brief Point the D3D11 output-merger at one postfx render target and set its viewport.
/// @details Each postfx pass (bloom extract, blur, tonemap) draws a full-screen triangle
///   into a different RTV. This helper batches the OMSetRenderTargets + RSSetViewports
///   calls so each pass is one line, not three, and so the viewport tracks the RTV size
///   automatically (missing that is a classic source of postfx-chain letterboxing bugs).
/// @param[in] ctx Active D3D11 backend context.
/// @param[in] rtv Render-target view to bind for the next full-screen pass.
/// @param[in] width Viewport width in pixels.
/// @param[in] height Viewport height in pixels.
static void d3d11_bind_postfx_target(d3d11_context_t *ctx,
                                     ID3D11RenderTargetView *rtv,
                                     int32_t width,
                                     int32_t height) {
    D3D11_VIEWPORT viewport;

    if (!ctx || !rtv || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return;
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (FLOAT)width;
    viewport.Height = (FLOAT)height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    d3d11_unbind_postfx_resources(ctx);
    ID3D11DeviceContext_OMSetRenderTargets(ctx->ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx->ctx, 1, &viewport);
}

/// @brief Execute one post-FX pass: upload the effect's constant data and issue a
///        fullscreen-triangle draw from @p source_color_srv into @p dest_rtv.
/// @details The pass uses `vs_postfx` (a no-input-layout shader that generates a
///   clip-space fullscreen triangle from `SV_VertexID`) together with `ps_postfx`
///   which samples four PS SRV slots:
///     - slot 0: @p source_color_srv — the colour result of the previous pass
///     - slot 1: `ctx->scene_depth_srv` — linear depth for depth-aware effects
///     - slot 2: `ctx->scene_motion_srv` — screen-space motion vectors (TAA / blur)
///     - slot 3: unused (NULL)
///   The postfx constant buffer is updated from @p snapshot before the draw so
///   per-effect uniforms (exposure, bloom threshold, vignette, etc.) are current.
///   Depth testing and stencil are disabled; the opaque blend state is used because
///   the post-FX shaders output the final composited colour directly. SRVs are
///   unbound after the draw to avoid hazards on the next write to the scene targets.
/// @param ctx               Active D3D11 backend context.
/// @param source_color_srv  The colour texture from the previous pass or the scene.
/// @param dest_rtv          The render target to write this pass's output into.
/// @param width             Viewport width in pixels (must match RTV dimensions).
/// @param height            Viewport height in pixels.
/// @param snapshot          Per-effect uniform data written into the postfx cbuffer.
/// @return 1 on success, 0 if any parameter is invalid or the cbuffer update failed.
static int d3d11_draw_postfx_pass(d3d11_context_t *ctx,
                                  ID3D11ShaderResourceView *source_color_srv,
                                  ID3D11RenderTargetView *dest_rtv,
                                  int32_t width,
                                  int32_t height,
                                  const vgfx3d_postfx_snapshot_t *snapshot) {
    d3d_postfx_cb_t postfx_data;
    ID3D11ShaderResourceView *srvs[5];
    ID3D11ShaderResourceView *null_srvs[5] = {NULL, NULL, NULL, NULL, NULL};
    float blend_factor[4] = {0, 0, 0, 0};
    HRESULT hr;

    if (!ctx || !source_color_srv || !dest_rtv ||
        !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return 0;

    d3d11_prepare_postfx_data(ctx, snapshot, &postfx_data);
    hr = d3d11_update_constant_buffer(ctx, ctx->cb_postfx, &postfx_data, sizeof(postfx_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbPostFX)", hr);
        return 0;
    }

    d3d11_bind_postfx_target(ctx, dest_rtv, width, height);
    srvs[0] = source_color_srv;
    srvs[1] = ctx->scene_depth_srv;
    srvs[2] = ctx->scene_motion_srv;
    srvs[3] = NULL;
    srvs[4] = ctx->postfx_current_bloom_srv; /* Plan 05 bloom mip-chain result (may be NULL) */
    ID3D11DeviceContext_IASetInputLayout(ctx->ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx->ctx, ctx->vs_postfx, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx->ctx, ctx->ps_postfx, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_postfx);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_postfx);
    if (ctx->linear_clamp_sampler)
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 0, 1, &ctx->linear_clamp_sampler);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 5, srvs);
    ID3D11DeviceContext_RSSetState(ctx->ctx, ctx->rs_solid_no_cull);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx->ctx, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(
        ctx->ctx, ctx->blend_state_opaque, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_Draw(ctx->ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 5, null_srvs);
    return 1;
}

/// @brief Encode the Plan 05 bloom mip chain from the scene color SRV.
/// @details Pass 0 thresholds + Karis-downsamples the scene into the half-res mip; each
///          further mip is a plain 13-tap downsample. The upsample walk tent-filters each
///          deeper mip and accumulates it into the one above via the additive blend
///          state, leaving mip 0 with the summed octaves.
/// @param[in,out] ctx Active D3D11 backend context and bloom resource owner.
/// @param[in] source_srv Scene or prior-pass color input.
/// @param[in] width Source width in pixels.
/// @param[in] height Source height in pixels.
/// @param[in] threshold Luminance threshold used by the first downsample pass.
/// @return Mip 0's SRV, or NULL when resources are unavailable.
static ID3D11ShaderResourceView *d3d11_encode_bloom_chain(d3d11_context_t *ctx,
                                                          ID3D11ShaderResourceView *source_srv,
                                                          int32_t width,
                                                          int32_t height,
                                                          float threshold) {
    ID3D11ShaderResourceView *null_srv = NULL;
    ID3D11ShaderResourceView *result = NULL;
    float blend_factor[4] = {0, 0, 0, 0};

    if (!ctx || !ctx->ctx || !source_srv || !ctx->ps_bloom_down || !ctx->ps_bloom_up ||
        FAILED(d3d11_ensure_bloom_targets(ctx, width, height)))
        return NULL;

    ID3D11DeviceContext_IASetInputLayout(ctx->ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx->ctx, ctx->vs_postfx, NULL, 0);
    if (ctx->linear_clamp_sampler)
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 0, 1, &ctx->linear_clamp_sampler);
    ID3D11DeviceContext_RSSetState(ctx->ctx, ctx->rs_solid_no_cull);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx->ctx, NULL, 0);

    ID3D11DeviceContext_PSSetShader(ctx->ctx, ctx->ps_bloom_down, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_bloom);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_bloom);
    ID3D11DeviceContext_OMSetBlendState(
        ctx->ctx, ctx->blend_state_opaque, blend_factor, 0xFFFFFFFF);
    for (int32_t i = 0; i < ctx->bloom_mip_count; i++) {
        ID3D11ShaderResourceView *src = (i == 0) ? source_srv : ctx->bloom_mip_srv[i - 1];
        int32_t src_w = (i == 0) ? width : ctx->bloom_mip_w[i - 1];
        int32_t src_h = (i == 0) ? height : ctx->bloom_mip_h[i - 1];
        d3d_bloom_cb_t bloom_data;
        HRESULT hr;
        bloom_data.src_inv_size[0] = src_w > 0 ? 1.0f / (float)src_w : 0.0f;
        bloom_data.src_inv_size[1] = src_h > 0 ? 1.0f / (float)src_h : 0.0f;
        bloom_data.threshold =
            vgfx3d_d3d11_clamp_float_param(threshold, 0.0f, VGFX3D_D3D11_POSTFX_SCALAR_MAX, 1.0f);
        bloom_data.first_pass = (i == 0) ? 1 : 0;
        hr = d3d11_update_constant_buffer(ctx, ctx->cb_bloom, &bloom_data, sizeof(bloom_data));
        if (FAILED(hr)) {
            d3d11_log_hresult("Map(cbBloom downsample)", hr);
            d3d11_log_device_removed_reason(ctx, "Map(cbBloom downsample)", hr);
            goto cleanup;
        }
        d3d11_bind_postfx_target(
            ctx, ctx->bloom_mip_rtv[i], ctx->bloom_mip_w[i], ctx->bloom_mip_h[i]);
        ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 1, &src);
        ID3D11DeviceContext_Draw(ctx->ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 1, &null_srv);
    }

    ID3D11DeviceContext_PSSetShader(ctx->ctx, ctx->ps_bloom_up, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(
        ctx->ctx, ctx->blend_state_additive, blend_factor, 0xFFFFFFFF);
    for (int32_t i = ctx->bloom_mip_count - 1; i >= 1; i--) {
        ID3D11ShaderResourceView *src = ctx->bloom_mip_srv[i];
        d3d_bloom_cb_t bloom_data;
        HRESULT hr;
        bloom_data.src_inv_size[0] =
            ctx->bloom_mip_w[i] > 0 ? 1.0f / (float)ctx->bloom_mip_w[i] : 0.0f;
        bloom_data.src_inv_size[1] =
            ctx->bloom_mip_h[i] > 0 ? 1.0f / (float)ctx->bloom_mip_h[i] : 0.0f;
        bloom_data.threshold = 0.0f;
        bloom_data.first_pass = 0;
        hr = d3d11_update_constant_buffer(ctx, ctx->cb_bloom, &bloom_data, sizeof(bloom_data));
        if (FAILED(hr)) {
            d3d11_log_hresult("Map(cbBloom upsample)", hr);
            d3d11_log_device_removed_reason(ctx, "Map(cbBloom upsample)", hr);
            goto cleanup;
        }
        d3d11_bind_postfx_target(
            ctx, ctx->bloom_mip_rtv[i - 1], ctx->bloom_mip_w[i - 1], ctx->bloom_mip_h[i - 1]);
        ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 1, &src);
        ID3D11DeviceContext_Draw(ctx->ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 1, &null_srv);
    }
    result = ctx->bloom_mip_srv[0];

cleanup:
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 1, &null_srv);
    ID3D11DeviceContext_OMSetBlendState(
        ctx->ctx, ctx->blend_state_opaque, blend_factor, 0xFFFFFFFF);
    if (!result)
        ctx->postfx_current_bloom_srv = NULL;
    return result;
}

/// @brief Encode the Plan 05 TAA resolve into history or a transient readback target.
/// @details Reprojects + neighborhood-clamps the persistent history against
///          @p source_srv. Normal presentation writes the free history slot and flips
///          parity. Screenshot replay writes a transient ping-pong target so observing
///          a frame cannot advance or overwrite temporal history.
/// @param[in,out] ctx Active D3D11 backend context and temporal-history owner.
/// @param[in] source_srv Current-frame color input.
/// @param[in] width Resolve width in pixels.
/// @param[in] height Resolve height in pixels.
/// @param[in] snapshot Optional sanitized TAA settings; defaults are used when `NULL`.
/// @param[in] preserve_history Nonzero to write a transient target without advancing history.
/// @return The resolved SRV, or NULL when resources are unavailable.
static ID3D11ShaderResourceView *d3d11_encode_taa_pass(d3d11_context_t *ctx,
                                                       ID3D11ShaderResourceView *source_srv,
                                                       int32_t width,
                                                       int32_t height,
                                                       const vgfx3d_postfx_snapshot_t *snapshot,
                                                       int preserve_history) {
    d3d_taa_cb_t taa_data;
    ID3D11ShaderResourceView *srvs[4];
    ID3D11ShaderResourceView *null_srvs[4] = {NULL, NULL, NULL, NULL};
    float blend_factor[4] = {0, 0, 0, 0};
    ID3D11RenderTargetView *output_rtv;
    ID3D11ShaderResourceView *output_srv;
    int32_t write_slot;
    int32_t read_slot;
    HRESULT hr;

    if (!ctx || !source_srv || !ctx->ps_taa || !ctx->scene_depth_srv || !ctx->scene_motion_srv ||
        FAILED(d3d11_ensure_taa_targets(ctx, width, height)))
        return NULL;

    write_slot = ctx->taa_history_parity ? 1 : 0;
    read_slot = 1 - write_slot;
    if (preserve_history) {
        if (FAILED(d3d11_ensure_postfx_target(ctx, width, height)) ||
            FAILED(d3d11_ensure_postfx_scratch_target(ctx, width, height)))
            return NULL;
        if (source_srv == ctx->postfx_color_srv) {
            output_rtv = ctx->postfx_scratch_rtv;
            output_srv = ctx->postfx_scratch_srv;
        } else {
            output_rtv = ctx->postfx_color_rtv;
            output_srv = ctx->postfx_color_srv;
        }
    } else {
        output_rtv = ctx->taa_history_rtv[write_slot];
        output_srv = ctx->taa_history_srv[write_slot];
    }

    memset(&taa_data, 0, sizeof(taa_data));
    vgfx3d_d3d11_copy_mat4_finite_or_identity(taa_data.inv_vp, ctx->scene_inv_vp);
    vgfx3d_d3d11_copy_mat4_finite_or_identity(taa_data.prev_vp, ctx->scene_prev_vp);
    taa_data.inv_resolution[0] = width > 0 ? 1.0f / (float)width : 0.0f;
    taa_data.inv_resolution[1] = height > 0 ? 1.0f / (float)height : 0.0f;
    taa_data.jitter_delta[0] = (ctx->taa_jitter_clip[0] - ctx->taa_prev_jitter_clip[0]) * 0.5f;
    taa_data.jitter_delta[1] = -(ctx->taa_jitter_clip[1] - ctx->taa_prev_jitter_clip[1]) * 0.5f;
    taa_data.blend = vgfx3d_d3d11_clamp_float_param(
        (snapshot && snapshot->taa_enabled) ? snapshot->taa_blend : 0.9f, 0.0f, 1.0f, 0.9f);
    taa_data.history_valid = (ctx->taa_history_valid && ctx->scene_history_valid) ? 1 : 0;
    hr = d3d11_update_constant_buffer(ctx, ctx->cb_taa, &taa_data, sizeof(taa_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbTAA)", hr);
        return NULL;
    }

    d3d11_bind_postfx_target(ctx, output_rtv, width, height);
    srvs[0] = source_srv;
    srvs[1] = ctx->taa_history_srv[read_slot];
    srvs[2] = ctx->scene_motion_srv;
    srvs[3] = ctx->scene_depth_srv;
    ID3D11DeviceContext_IASetInputLayout(ctx->ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx->ctx, ctx->vs_postfx, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx->ctx, ctx->ps_taa, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_taa);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_taa);
    if (ctx->linear_clamp_sampler)
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 0, 1, &ctx->linear_clamp_sampler);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 4, srvs);
    ID3D11DeviceContext_RSSetState(ctx->ctx, ctx->rs_solid_no_cull);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx->ctx, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(
        ctx->ctx, ctx->blend_state_opaque, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_Draw(ctx->ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 4, null_srvs);

    if (!preserve_history) {
        ctx->taa_history_parity = ctx->taa_history_parity ? 0 : 1;
        ctx->taa_history_valid = 1;
    }
    return output_srv;
}

/// @brief Encode the Plan 10 SSR pass: ray-march the scene depth in screen space for
///        pixels whose motion-target alpha carries an SSR mask, compositing reflected
///        scene color over @p source_srv into the SSR target.
/// @param[in,out] ctx Active D3D11 backend context and SSR target owner.
/// @param[in] source_srv Scene or prior-pass color input.
/// @param[in] width SSR output width in pixels.
/// @param[in] height SSR output height in pixels.
/// @param[in] snapshot Optional sanitized SSR settings; defaults are used when `NULL`.
/// @return The composited SRV, or NULL when resources are unavailable.
static ID3D11ShaderResourceView *d3d11_encode_ssr_pass(d3d11_context_t *ctx,
                                                       ID3D11ShaderResourceView *source_srv,
                                                       int32_t width,
                                                       int32_t height,
                                                       const vgfx3d_postfx_snapshot_t *snapshot) {
    d3d_ssr_cb_t ssr_data;
    ID3D11ShaderResourceView *srvs[3];
    ID3D11ShaderResourceView *null_srvs[3] = {NULL, NULL, NULL};
    float blend_factor[4] = {0, 0, 0, 0};
    HRESULT hr;

    if (!ctx || !source_srv || !ctx->ps_ssr || !ctx->scene_depth_srv || !ctx->scene_motion_srv ||
        FAILED(d3d11_ensure_ssr_target(ctx, width, height)))
        return NULL;

    memset(&ssr_data, 0, sizeof(ssr_data));
    vgfx3d_d3d11_copy_mat4_finite_or_identity(ssr_data.inv_vp, ctx->scene_inv_vp);
    vgfx3d_d3d11_copy_mat4_finite_or_identity(ssr_data.vp, ctx->scene_vp);
    vgfx3d_d3d11_copy_float_array_clamped_finite_or(ssr_data.cam_pos,
                                                    ctx->scene_cam_pos,
                                                    3u,
                                                    -VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                    VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX,
                                                    0.0f);
    ssr_data.params0[0] = vgfx3d_d3d11_clamp_float_param(
        (snapshot && snapshot->ssr_enabled) ? snapshot->ssr_intensity : 0.5f,
        0.0f,
        VGFX3D_D3D11_POSTFX_SCALAR_MAX,
        0.5f);
    ssr_data.params0[1] = vgfx3d_d3d11_clamp_float_param(
        (snapshot && snapshot->ssr_enabled) ? snapshot->ssr_max_roughness : 0.4f, 0.0f, 1.0f, 0.4f);
    ssr_data.params0[2] = (float)vgfx3d_d3d11_sanitize_ssr_steps(
        (snapshot && snapshot->ssr_steps > 0) ? snapshot->ssr_steps
                                              : VGFX3D_D3D11_SSR_STEPS_DEFAULT);
    ssr_data.inv_resolution[0] = width > 0 ? 1.0f / (float)width : 0.0f;
    ssr_data.inv_resolution[1] = height > 0 ? 1.0f / (float)height : 0.0f;
    hr = d3d11_update_constant_buffer(ctx, ctx->cb_ssr, &ssr_data, sizeof(ssr_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbSSR)", hr);
        return NULL;
    }

    d3d11_bind_postfx_target(ctx, ctx->ssr_rtv, width, height);
    srvs[0] = source_srv;
    srvs[1] = ctx->scene_depth_srv;
    srvs[2] = ctx->scene_motion_srv;
    ID3D11DeviceContext_IASetInputLayout(ctx->ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx->ctx, ctx->vs_postfx, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx->ctx, ctx->ps_ssr, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_ssr);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_ssr);
    if (ctx->linear_clamp_sampler)
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 0, 1, &ctx->linear_clamp_sampler);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 3, srvs);
    ID3D11DeviceContext_RSSetState(ctx->ctx, ctx->rs_solid_no_cull);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx->ctx, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(
        ctx->ctx, ctx->blend_state_opaque, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_Draw(ctx->ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 3, null_srvs);
    return ctx->ssr_srv;
}

/// @brief Composite the 2D GUI overlay texture on top of the 3D scene using alpha blending.
/// @details After all post-FX passes have resolved the scene colour, the 2D overlay
///   (rendered earlier into `ctx->overlay_color_srv`) is blended over @p dest_rtv
///   using `ps_overlay_composite` and a premultiplied-alpha blend state. The overlay SRV is bound
///   at PS slot 3 (matching the shader's expected binding), and the same
///   no-input-layout fullscreen-triangle technique used by `d3d11_draw_postfx_pass`
///   is re-used here for consistency. The function is a no-op (returns 1) when no
///   overlay was produced this frame (`ctx->overlay_used_this_frame == 0`) or when
///   the overlay resources or composite shader are absent, so callers can always
///   call it unconditionally without checking state themselves.
/// @param ctx        Active D3D11 backend context.
/// @param dest_rtv   Render target to composite the overlay into (final output or
///                   intermediate offscreen buffer when `force_offscreen_final` is set).
/// @param width      Viewport width in pixels.
/// @param height     Viewport height in pixels.
/// @return 1 on successful composite or no-op; 0 when the destination is invalid.
static int d3d11_draw_overlay_composite(d3d11_context_t *ctx,
                                        ID3D11RenderTargetView *dest_rtv,
                                        int32_t width,
                                        int32_t height) {
    ID3D11ShaderResourceView *overlay_srv;
    ID3D11BlendState *blend_state;
    ID3D11ShaderResourceView *null_srvs[4] = {NULL, NULL, NULL, NULL};
    float blend_factor[4] = {0, 0, 0, 0};

    if (!ctx || !dest_rtv || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return 0;
    if (!ctx->overlay_used_this_frame || !ctx->overlay_color_srv || !ctx->ps_overlay_composite) {
        return 1;
    }

    overlay_srv = ctx->overlay_color_srv;
    blend_state = ctx->blend_state_premultiplied_alpha ? ctx->blend_state_premultiplied_alpha
                                                       : ctx->blend_state_alpha;
    d3d11_bind_postfx_target(ctx, dest_rtv, width, height);
    ID3D11DeviceContext_IASetInputLayout(ctx->ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx->ctx, ctx->vs_postfx, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx->ctx, ctx->ps_overlay_composite, NULL, 0);
    if (ctx->linear_clamp_sampler)
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 0, 1, &ctx->linear_clamp_sampler);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 3, 1, &overlay_srv);
    ID3D11DeviceContext_RSSetState(ctx->ctx, ctx->rs_solid_no_cull);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx->ctx, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(ctx->ctx, blend_state, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_Draw(ctx->ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 4, null_srvs);
    return 1;
}

/// @brief Resolve the offscreen scene color to a final target without applying user effects.
/// @param[in,out] ctx Active D3D11 backend context.
/// @param[in] final_rtv Final render target used unless @p force_offscreen_final is nonzero.
/// @param[in] final_width Width of @p final_rtv in pixels.
/// @param[in] final_height Height of @p final_rtv in pixels.
/// @param[in] force_offscreen_final Nonzero to resolve into an owned post-FX target.
/// @param[out] out_result_tex Optional receiver for the offscreen result texture.
/// @return One when the resolve and overlay composite succeed; otherwise zero.
static int d3d11_resolve_scene_to_target(d3d11_context_t *ctx,
                                         ID3D11RenderTargetView *final_rtv,
                                         int32_t final_width,
                                         int32_t final_height,
                                         int force_offscreen_final,
                                         ID3D11Texture2D **out_result_tex) {
    ID3D11RenderTargetView *dest_rtv;
    int32_t width;
    int32_t height;

    if (out_result_tex)
        *out_result_tex = NULL;
    if (!ctx || !ctx->scene_color_srv || !ctx->scene_color_tex || !ctx->scene_depth_srv ||
        !ctx->scene_motion_srv)
        return 0;
    if (force_offscreen_final) {
        if (FAILED(d3d11_ensure_postfx_target(ctx, ctx->scene_width, ctx->scene_height)))
            return 0;
        dest_rtv = ctx->postfx_color_rtv;
        width = ctx->scene_width;
        height = ctx->scene_height;
    } else {
        dest_rtv = final_rtv;
        width = final_width;
        height = final_height;
    }
    if (!dest_rtv || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return 0;
    if (!d3d11_draw_postfx_pass(ctx, ctx->scene_color_srv, dest_rtv, width, height, NULL))
        return 0;
    if (!d3d11_draw_overlay_composite(ctx, dest_rtv, width, height))
        return 0;
    if (out_result_tex)
        *out_result_tex = force_offscreen_final ? ctx->postfx_color_tex : NULL;
    return 1;
}

/// @brief Apply a full chain of post-FX effects to the rendered scene using ping-pong buffers.
/// @details Iterates `chain->effects[0..effect_count-1]`, drawing each via
///   `d3d11_draw_postfx_pass`. Passes alternate between `scene_color_srv/rtv` and
///   `postfx_color_srv/rtv` and `postfx_scratch_srv/rtv` as intermediate destinations so
///   no pass reads and writes the same texture simultaneously and screenshot readback does
///   not overwrite the live scene color. For the final
///   pass, unless @p force_offscreen_final is set, the output goes directly into
///   @p final_rtv (usually the swapchain back buffer) at @p final_width × @p
///   final_height so no extra blit is needed.
///
///   When @p force_offscreen_final is non-zero (used by the screenshot readback path)
///   the final pass also writes to an offscreen scene texture and the result texture
///   pointer is returned via @p out_result_tex so the caller can stage-copy it.
///
///   After all post-FX passes, `d3d11_draw_overlay_composite` is called once to
///   alpha-blend the 2D GUI overlay over the result regardless of effect count.
///
///   Returns 0 early if any of the required scene resources (scene_color_srv,
///   scene_depth_srv, scene_motion_srv) are absent, or if any individual pass fails.
/// @param ctx                 Active D3D11 backend context.
/// @param chain               Effect chain descriptor (enabled flag + effects array).
/// @param final_rtv           Swapchain (or caller-supplied) render target for the last pass.
/// @param final_width         Width of @p final_rtv in pixels.
/// @param final_height        Height of @p final_rtv in pixels.
/// @param force_offscreen_final If non-zero, the last pass writes to an offscreen texture
///                             instead of @p final_rtv; the texture is returned via
///                             @p out_result_tex.
/// @param out_result_tex      Receives the offscreen result texture when
///                            @p force_offscreen_final is set; set to NULL otherwise.
/// @return 1 on success, 0 if resources are missing or any pass fails.
static int d3d11_apply_postfx_chain(d3d11_context_t *ctx,
                                    const vgfx3d_postfx_chain_t *chain,
                                    ID3D11RenderTargetView *final_rtv,
                                    int32_t final_width,
                                    int32_t final_height,
                                    int force_offscreen_final,
                                    ID3D11Texture2D **out_result_tex) {
    ID3D11ShaderResourceView *source_srv;
    ID3D11RenderTargetView *dest_rtv;
    ID3D11Texture2D *result_tex = NULL;
    int need_postfx_target;
    int need_scratch_target;
    int32_t width;
    int32_t height;

    if (out_result_tex)
        *out_result_tex = NULL;
    if (!ctx || !vgfx3d_d3d11_postfx_chain_is_usable(chain) || !ctx->scene_color_srv ||
        !ctx->scene_color_rtv || !ctx->scene_color_tex || !ctx->scene_depth_srv ||
        !ctx->scene_motion_srv ||
        (!force_offscreen_final &&
         (!final_rtv || !vgfx3d_d3d11_is_valid_texture2d_extent(final_width, final_height)))) {
        return 0;
    }

    need_postfx_target = force_offscreen_final || chain->effect_count > 1;
    need_scratch_target = force_offscreen_final ? chain->effect_count > 1 : chain->effect_count > 2;
    if (need_postfx_target &&
        FAILED(d3d11_ensure_postfx_target(ctx, ctx->scene_width, ctx->scene_height))) {
        return 0;
    }
    if (need_scratch_target &&
        FAILED(d3d11_ensure_postfx_scratch_target(ctx, ctx->scene_width, ctx->scene_height))) {
        return 0;
    }

    source_srv = ctx->scene_color_srv;
    result_tex = ctx->scene_color_tex;
    for (int32_t i = 0; i < chain->effect_count; i++) {
        const int is_last = (i == chain->effect_count - 1) ? 1 : 0;
        vgfx3d_postfx_snapshot_t safe_snapshot;
        const vgfx3d_postfx_snapshot_t *pass_snapshot;

        vgfx3d_sanitize_postfx_snapshot(&chain->effects[i].snapshot, &safe_snapshot);
        pass_snapshot = &safe_snapshot;

        ctx->postfx_current_bloom_srv = NULL;
        if (chain->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_BLOOM &&
            safe_snapshot.bloom_enabled) {
            ctx->postfx_current_bloom_srv = d3d11_encode_bloom_chain(ctx,
                                                                     source_srv,
                                                                     ctx->scene_width,
                                                                     ctx->scene_height,
                                                                     safe_snapshot.bloom_threshold);
        }

        /* Plan 05: a TAA entry resolves into its own history target, which then feeds
         * the remaining chain; the regular pass below turns into a plain copy so the
         * result still lands in the caller's framebuffer. */
        if (chain->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_TAA &&
            safe_snapshot.taa_enabled) {
            ID3D11ShaderResourceView *resolved = d3d11_encode_taa_pass(ctx,
                                                                       source_srv,
                                                                       ctx->scene_width,
                                                                       ctx->scene_height,
                                                                       pass_snapshot,
                                                                       force_offscreen_final);
            if (resolved)
                source_srv = resolved;
            pass_snapshot = NULL;
        }
        /* Plan 10: an SSR entry composites reflections into its own target, which
         * then feeds the remaining chain exactly like the TAA pre-resolve above. */
        if (chain->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_SSR &&
            safe_snapshot.ssr_enabled) {
            ID3D11ShaderResourceView *reflected = d3d11_encode_ssr_pass(
                ctx, source_srv, ctx->scene_width, ctx->scene_height, &safe_snapshot);
            if (reflected)
                source_srv = reflected;
            pass_snapshot = NULL;
        }

        if (is_last && !force_offscreen_final) {
            dest_rtv = final_rtv;
            width = final_width;
            height = final_height;
            result_tex = NULL;
        } else if (source_srv == ctx->postfx_color_srv) {
            dest_rtv = ctx->postfx_scratch_rtv;
            width = ctx->scene_width;
            height = ctx->scene_height;
            result_tex = ctx->postfx_scratch_tex;
        } else {
            dest_rtv = ctx->postfx_color_rtv;
            width = ctx->scene_width;
            height = ctx->scene_height;
            result_tex = ctx->postfx_color_tex;
        }
        if (!dest_rtv || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height)) {
            ctx->postfx_current_bloom_srv = NULL;
            return 0;
        }
        if (!d3d11_draw_postfx_pass(ctx, source_srv, dest_rtv, width, height, pass_snapshot)) {
            ctx->postfx_current_bloom_srv = NULL;
            return 0;
        }
        if (!is_last || force_offscreen_final) {
            source_srv = result_tex == ctx->postfx_scratch_tex ? ctx->postfx_scratch_srv
                                                               : ctx->postfx_color_srv;
        }
    }
    ctx->postfx_current_bloom_srv = NULL;

    if (force_offscreen_final) {
        dest_rtv =
            result_tex == ctx->postfx_scratch_tex ? ctx->postfx_scratch_rtv : ctx->postfx_color_rtv;
        width = ctx->scene_width;
        height = ctx->scene_height;
        if (!d3d11_draw_overlay_composite(ctx, dest_rtv, width, height))
            return 0;
        if (out_result_tex)
            *out_result_tex = result_tex;
        return result_tex != NULL ? 1 : 0;
    }

    if (!d3d11_draw_overlay_composite(ctx, final_rtv, final_width, final_height))
        return 0;
    return 1;
}

/// @brief Acquire the swapchain backbuffer for readback fallback paths.
/// @param[in] ctx Active D3D11 backend context with a valid swapchain.
/// @param[out] out_back_buffer Receives an owned COM reference to the backbuffer.
/// @return One when a backbuffer reference is acquired; otherwise zero.
static int d3d11_acquire_backbuffer_for_readback(d3d11_context_t *ctx,
                                                 ID3D11Texture2D **out_back_buffer) {
    HRESULT hr;

    if (out_back_buffer)
        *out_back_buffer = NULL;
    if (!ctx || !ctx->swap_chain || !out_back_buffer)
        return 0;
    hr = IDXGISwapChain_GetBuffer(
        ctx->swap_chain, 0, &IID_ID3D11Texture2D, (void **)out_back_buffer);
    hr = d3d11_device_child_result(hr, (ID3D11DeviceChild *)*out_back_buffer, ctx->device);
    if (FAILED(hr)) {
        d3d11_log_hresult("IDXGISwapChain::GetBuffer(readback)", hr);
        SAFE_RELEASE(*out_back_buffer);
        return 0;
    }
    return 1;
}

/// @brief Backend `readback_rgba` op - pick the best source texture to read.
/// @param[in,out] ctx_ptr Opaque pointer to the D3D11 backend context.
/// @param[out] dst_rgba Host buffer receiving the selected frame as RGBA8 pixels.
/// @param[in] w Requested output width in pixels.
/// @param[in] h Requested output height in pixels.
/// @param[in] stride Destination row stride in bytes.
/// @return One on successful readback; otherwise zero.
static int d3d11_readback_rgba(
    void *ctx_ptr, uint8_t *dst_rgba, int32_t w, int32_t h, int32_t stride) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    ID3D11Texture2D *back_buffer = NULL;
    ID3D11Texture2D *source_tex = NULL;
    vgfx3d_d3d11_readback_kind_t readback_kind;

    if (!ctx || !dst_rgba || !vgfx3d_d3d11_validate_rgba8_destination(w, h, stride, NULL))
        return 0;
    readback_kind = vgfx3d_d3d11_choose_readback_kind(
        ctx->presented_color_valid,
        ctx->presented_color_tex != NULL,
        ctx->scene_composited_to_swapchain,
        d3d11_window_scene_route_enabled(ctx),
        ctx->gpu_postfx_chain_valid,
        ctx->gpu_postfx_chain.enabled,
        ctx->gpu_postfx_chain.effect_count,
        vgfx3d_d3d11_postfx_chain_has_active_effects(&ctx->gpu_postfx_chain),
        d3d11_has_scene_targets(ctx),
        ctx->current_target_kind);
    if (readback_kind == VGFX3D_D3D11_READBACK_PRESENTED_SNAPSHOT) {
        source_tex = ctx->presented_color_tex;
    } else if (readback_kind == VGFX3D_D3D11_READBACK_POSTFX_COMPOSITE) {
        if (d3d11_apply_postfx_chain(
                ctx, &ctx->gpu_postfx_chain, ctx->rtv, ctx->width, ctx->height, 0, NULL)) {
            ctx->scene_composited_to_swapchain = 1;
            if (d3d11_acquire_backbuffer_for_readback(ctx, &back_buffer))
                source_tex = back_buffer;
        }
        if (!source_tex && d3d11_has_scene_targets(ctx))
            source_tex = ctx->scene_color_tex;
    } else if (readback_kind == VGFX3D_D3D11_READBACK_SCENE_COLOR && ctx->scene_width > 0 &&
               ctx->scene_height > 0) {
        if (!ctx->rtt_active && d3d11_render_scale_active(ctx)) {
            if (d3d11_resolve_scene_to_target(ctx, ctx->rtv, ctx->width, ctx->height, 0, NULL)) {
                ctx->scene_composited_to_swapchain = 1;
                if (d3d11_acquire_backbuffer_for_readback(ctx, &back_buffer))
                    source_tex = back_buffer;
            }
            if (!source_tex)
                source_tex = ctx->scene_color_tex;
        } else if (ctx->overlay_used_this_frame && d3d11_has_overlay_target(ctx)) {
            if (!d3d11_resolve_scene_to_target(ctx, NULL, 0, 0, 1, &source_tex))
                source_tex = ctx->scene_color_tex;
        } else {
            source_tex = ctx->scene_color_tex;
        }
    }

    if (!source_tex) {
        if (!d3d11_acquire_backbuffer_for_readback(ctx, &back_buffer))
            return 0;
        source_tex = back_buffer;
    }

    {
        int ok = d3d11_readback_texture_rgba(ctx, source_tex, dst_rgba, w, h, stride);
        SAFE_RELEASE(back_buffer);
        return ok;
    }
}

/// @brief Internal present path — composite scene + post-FX + overlay onto the swapchain.
/// @param[in,out] ctx Active D3D11 backend context with a completed pending frame.
/// @param[in] chain Optional ordered post-processing chain to apply before presentation.
static void d3d11_present_internal(d3d11_context_t *ctx, const vgfx3d_postfx_chain_t *chain) {
    int use_postfx;

    if (!ctx || ctx->rtt_active || ctx->frame_active || !ctx->frame_pending_present)
        return;
    ctx->frame_pending_present = 0;
    if (ctx->scene_composited_to_swapchain) {
        d3d11_present_swapchain(ctx);
        ctx->scene_composited_to_swapchain = 0;
        return;
    }
    use_postfx = (vgfx3d_d3d11_postfx_chain_has_active_effects(chain) && ctx->gpu_postfx_enabled &&
                  d3d11_has_scene_targets(ctx))
                     ? 1
                     : 0;
    if (!use_postfx) {
        if (d3d11_window_scene_route_enabled(ctx) && d3d11_has_scene_targets(ctx) &&
            d3d11_resolve_scene_to_target(ctx, ctx->rtv, ctx->width, ctx->height, 0, NULL)) {
            d3d11_present_swapchain(ctx);
            ctx->scene_composited_to_swapchain = 0;
            return;
        }
        d3d11_present_swapchain(ctx);
        ctx->scene_composited_to_swapchain = 0;
        return;
    }

    if (!d3d11_apply_postfx_chain(ctx, chain, ctx->rtv, ctx->width, ctx->height, 0, NULL)) {
        if (d3d11_resolve_scene_to_target(ctx, ctx->rtv, ctx->width, ctx->height, 0, NULL)) {
            d3d11_present_swapchain(ctx);
            ctx->scene_composited_to_swapchain = 0;
            return;
        }
        d3d11_present_swapchain(ctx);
        ctx->scene_composited_to_swapchain = 0;
        return;
    }
    d3d11_present_swapchain(ctx);
    ctx->scene_composited_to_swapchain = 0;
}

/// @brief Backend `apply_postfx` — composite scene/post-FX onto the swapchain without present.
/// @param[in,out] ctx_ptr Opaque pointer to the D3D11 backend context.
/// @param[in] postfx Optional ordered effect chain; an unusable chain triggers a plain resolve.
static void d3d11_apply_postfx(void *ctx_ptr, const vgfx3d_postfx_chain_t *postfx) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    int use_postfx;

    if (!ctx || ctx->rtt_active || ctx->frame_active || !ctx->frame_pending_present)
        return;
    ctx->presented_color_valid = 0;
    if (ctx->scene_composited_to_swapchain)
        return;
    if (!vgfx3d_d3d11_should_composite_to_swapchain(ctx->rtt_active,
                                                    d3d11_window_scene_route_enabled(ctx),
                                                    d3d11_has_scene_targets(ctx),
                                                    ctx->scene_composited_to_swapchain))
        return;
    use_postfx = (vgfx3d_d3d11_postfx_chain_has_active_effects(postfx) && ctx->gpu_postfx_enabled &&
                  d3d11_has_scene_targets(ctx))
                     ? 1
                     : 0;
    if (!use_postfx) {
        if (d3d11_window_scene_route_enabled(ctx) && d3d11_has_scene_targets(ctx) &&
            d3d11_resolve_scene_to_target(ctx, ctx->rtv, ctx->width, ctx->height, 0, NULL))
            ctx->scene_composited_to_swapchain = 1;
        return;
    }
    if (d3d11_apply_postfx_chain(ctx, postfx, ctx->rtv, ctx->width, ctx->height, 0, NULL)) {
        ctx->scene_composited_to_swapchain = 1;
        return;
    }
    if (d3d11_resolve_scene_to_target(ctx, ctx->rtv, ctx->width, ctx->height, 0, NULL))
        ctx->scene_composited_to_swapchain = 1;
}

/// @brief Backend `present` (no post-FX) — direct swapchain present.
/// @param[in,out] ctx_ptr Opaque pointer to the D3D11 backend context.
static void d3d11_present(void *ctx_ptr) {
    d3d11_present_internal((d3d11_context_t *)ctx_ptr, NULL);
}

/// @brief Backend `present_postfx` — present with an ordered post-FX chain applied.
/// @param[in,out] ctx_ptr Opaque pointer to the D3D11 backend context.
/// @param[in] postfx Optional ordered effect chain to composite before presentation.
static void d3d11_present_postfx(void *ctx_ptr, const vgfx3d_postfx_chain_t *postfx) {
    d3d11_present_internal((d3d11_context_t *)ctx_ptr, postfx);
}

/// @brief Backend `set_gpu_postfx_enabled` — toggle the postfx route.
///
/// When enabled, frames render to the offscreen scene target so postfx
/// can read it. When disabled, frames render straight to the swapchain.
/// @param[in,out] ctx_ptr Opaque pointer to an idle D3D11 backend context.
/// @param[in] enabled Nonzero to route window frames through the post-FX scene target.
static void d3d11_set_gpu_postfx_enabled(void *ctx_ptr, int8_t enabled) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    int8_t next_enabled = enabled ? 1 : 0;
    if (!ctx || ctx->frame_active)
        return;
    if (vgfx3d_d3d11_should_reset_composited_swapchain_for_postfx_update(ctx->gpu_postfx_enabled,
                                                                         next_enabled)) {
        ctx->scene_composited_to_swapchain = 0;
        ctx->presented_color_valid = 0;
    }
    ctx->gpu_postfx_enabled = next_enabled;
    if (!ctx->gpu_postfx_enabled) {
        vgfx3d_postfx_chain_reset(&ctx->gpu_postfx_chain);
        ctx->gpu_postfx_chain_valid = 0;
        d3d11_unbind_postfx_resources(ctx);
        d3d11_reset_temporal_scene_state(ctx);
    }
}

/// @brief Apply a reduced window-scene render scale to the D3D11 backend.
/// @details Rebuilds the offscreen HDR scene at the shared scaled extent and keeps the overlay at
///          logical output size. Presentation then resolves through the existing full-screen
///          post-FX shader into the swapchain viewport, whether or not user effects are enabled.
///          Allocation is transactional with respect to both the stored scale and live targets:
///          the native overlay is made ready first, then a complete replacement scene is staged
///          before the old scene is retired. Explicit render-target dimensions are never changed.
/// @param ctx_ptr Borrowed `d3d11_context_t` backend handle.
/// @param scale Finite requested scale in `[0.25, 1]`; values at least `0.999` normalize to 1.
/// @return 1 when the requested/native route is ready, or 0 for invalid input, missing composite
///         shaders, or target allocation failure.
static int8_t d3d11_set_render_scale(void *ctx_ptr, float scale) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    float prior_scale;
    float next_scale;
    int route_required;
    int32_t scene_width;
    int32_t scene_height;
    HRESULT hr;

    if (!ctx || !vgfx3d_compute_scaled_scene_extent(
                    ctx->width, ctx->height, scale, &scene_width, &scene_height)) {
        return 0;
    }
    next_scale = scale >= 0.999f ? 1.0f : scale;
    if (next_scale < 1.0f && (!ctx->vs_postfx || !ctx->ps_postfx))
        return 0;
    if (!vgfx3d_d3d11_frame_state_is_idle(ctx->frame_active, ctx->frame_pending_present))
        return 0;
    prior_scale =
        (isfinite(ctx->render_scale) && ctx->render_scale >= 0.25f && ctx->render_scale <= 1.0f)
            ? ctx->render_scale
            : 1.0f;
    route_required = ctx->gpu_postfx_enabled || next_scale < 1.0f;
    if (route_required) {
        if (!vgfx3d_compute_scaled_scene_extent(
                ctx->width, ctx->height, next_scale, &scene_width, &scene_height))
            return 0;
        /* The overlay stays at native size. Make it ready before publishing a scaled scene so a
         * later allocation failure leaves the previous complete scene route intact. */
        hr = d3d11_ensure_overlay_target(ctx, ctx->width, ctx->height);
        if (FAILED(hr)) {
            if (!ctx->rtt_active)
                d3d11_bind_swapchain_target(ctx);
            return 0;
        }
        hr = d3d11_ensure_scene_targets(ctx, scene_width, scene_height);
        if (FAILED(hr)) {
            if (!ctx->rtt_active)
                d3d11_bind_swapchain_target(ctx);
            return 0;
        }
    }

    if (fabsf(prior_scale - next_scale) > 1.0e-6f)
        d3d11_reset_temporal_scene_state(ctx);
    ctx->render_scale = next_scale;
    ctx->scene_composited_to_swapchain = 0;
    ctx->presented_color_valid = 0;
    if (!route_required)
        d3d11_destroy_scene_targets(ctx);
    if (!ctx->rtt_active)
        d3d11_bind_swapchain_target(ctx);
    return 1;
}

/// @brief Invalidate stale presented snapshots when a new post-FX latch starts a frame.
/// @param[in,out] ctx Active D3D11 backend context whose snapshot validity may be cleared.
static void d3d11_invalidate_presented_snapshot_for_postfx_latch(d3d11_context_t *ctx) {
    if (!ctx || ctx->scene_composited_to_swapchain)
        return;
    ctx->presented_color_valid = 0;
}

/// @brief Backend `set_gpu_postfx_snapshot` - latch the frame's postfx chain for replay.
/// @details Called once per frame from the canvas. A null `postfx` clears the cached
///   chain (equivalent to "no postfx this frame"); otherwise the chain is deep-copied
///   into the context so later present-time passes see a stable snapshot even if the
///   caller mutates their chain afterward. A copy failure aborts-and-resets rather than
///   rendering with a partial chain.
/// @param[in,out] ctx_ptr Opaque pointer to an idle D3D11 backend context.
/// @param[in] postfx Chain to deep-copy, or `NULL` to clear the latched chain.
static void d3d11_set_gpu_postfx_snapshot(void *ctx_ptr, const vgfx3d_postfx_chain_t *postfx) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    if (!ctx || ctx->frame_active)
        return;
    d3d11_invalidate_presented_snapshot_for_postfx_latch(ctx);
    if (!postfx) {
        vgfx3d_postfx_chain_reset(&ctx->gpu_postfx_chain);
        ctx->gpu_postfx_chain_valid = 0;
        ctx->taa_history_valid = 0;
        return;
    }
    if (!vgfx3d_postfx_chain_copy(&ctx->gpu_postfx_chain, postfx)) {
        vgfx3d_postfx_chain_reset(&ctx->gpu_postfx_chain);
        ctx->gpu_postfx_chain_valid = 0;
        ctx->taa_history_valid = 0;
        return;
    }
    ctx->gpu_postfx_chain_valid = 1;
    /* Plan 05: dropping TAA from the chain invalidates temporal history so a later
     * re-enable starts from a fresh accumulation instead of stale frames. */
    if (!d3d11_chain_has_effect(postfx, (int32_t)VGFX3D_POSTFX_EFFECT_TAA))
        ctx->taa_history_valid = 0;
}

/// @brief Backend `set_render_target` — bind / unbind RTT mode.
///
/// NULL `rt` tears down RTT and reverts to swapchain rendering.
/// Non-NULL ensures RTT targets at the requested size and activates them.
/// @param[in,out] ctx_ptr Opaque pointer to an idle D3D11 backend context.
/// @param[in,out] rt Render-target descriptor to bind, or `NULL` to leave RTT mode.
static void d3d11_set_render_target(void *ctx_ptr, vgfx3d_rendertarget_t *rt) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    HRESULT hr;

    if (!ctx || ctx->frame_active)
        return;
    if (!rt) {
        if (ctx->rtt_target && ctx->rtt_target->color_dirty &&
            !d3d11_sync_render_target_color(ctx, ctx->rtt_target)) {
            d3d11_log_hresult("SyncRTTBeforeUnbind", E_FAIL);
            return;
        }
        d3d11_destroy_rtt_targets(ctx);
        ctx->scene_composited_to_swapchain = 0;
        ctx->presented_color_valid = 0;
        return;
    }

    hr = d3d11_ensure_rtt_targets(ctx, rt);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateRTTTargets", hr);
        return;
    }
    ctx->scene_composited_to_swapchain = 0;
    ctx->presented_color_valid = 0;
}

/// @brief Leave any active shadow pass and restore the current color/depth target set.
/// @param[in,out] ctx Active D3D11 backend context.
static void d3d11_leave_shadow_pass(d3d11_context_t *ctx) {
    if (!ctx || ctx->shadow_pass_slot < 0)
        return;
    d3d11_unbind_output_targets(ctx);
    ctx->shadow_pass_slot = -1;
    ctx->shadow_pass_failed = 0;
    d3d11_select_current_targets(ctx);
}

/// @brief Backend `shadow_begin` - switch the pipeline to shadow-pass mode.
///
/// Allocates the shadow depth target if needed, captures the light's
/// view-projection matrix (used by main pass for shadow sampling),
/// unbinds draw/shadow SRVs (so we can write to it), binds the shadow
/// DSV with no color RTV (depth-only pass), clears depth, sets viewport,
/// and binds the shadow vertex shader with no pixel shader.
/// @param[in,out] ctx_ptr Opaque pointer to the active D3D11 backend context.
/// @param[in] slot Shadow slot to render.
/// @param[in] depth_buf Host depth buffer from the backend interface; unused by D3D11.
/// @param[in] width Requested shadow-map or atlas-tile width in pixels.
/// @param[in] height Requested shadow-map or atlas-tile height in pixels.
/// @param[in] light_vp Light view-projection matrix copied for shadow rendering.
static void d3d11_shadow_begin(void *ctx_ptr,
                               int32_t slot,
                               float *depth_buf,
                               int32_t width,
                               int32_t height,
                               const float *light_vp) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    D3D11_VIEWPORT viewport;
    HRESULT hr;

    (void)depth_buf;
    if (!ctx || !ctx->frame_active)
        return;
    d3d11_leave_shadow_pass(ctx);
    if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS)
        return;
    ctx->shadow_pass_slot = -1;
    ctx->shadow_pass_failed = 0;
    ctx->shadow_slot_complete[slot] = 0;
    d3d11_recompute_shadow_count(ctx);
    if (!light_vp || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height) ||
        !vgfx3d_d3d11_shadow_matrix_is_usable(light_vp))
        return;
    if (slot >= VGFX3D_CSM_SLOTS) {
        /* Atlas tile pass: render into the shared depth atlas with a tile
         * viewport; the atlas clears once per frame on its first tile pass. */
        int32_t tile = slot - VGFX3D_CSM_SLOTS;
        hr = d3d11_ensure_shadow_atlas(ctx, width, height);
        if (FAILED(hr)) {
            d3d11_log_hresult("CreateShadowAtlas", hr);
            d3d11_recompute_shadow_count(ctx);
            return;
        }
        ctx->shadow_pass_slot = slot;
        vgfx3d_d3d11_copy_mat4_finite_or_identity(ctx->shadow_vp[slot], light_vp);
        d3d11_unbind_draw_resources(ctx);
        d3d11_unbind_shadow_resources(ctx);
        ID3D11DeviceContext_OMSetRenderTargets(ctx->ctx, 0, NULL, ctx->shadow_atlas_dsv);
        if (!ctx->shadow_atlas_cleared) {
            ID3D11DeviceContext_ClearDepthStencilView(
                ctx->ctx, ctx->shadow_atlas_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
            ctx->shadow_atlas_cleared = 1;
        }
        memset(&viewport, 0, sizeof(viewport));
        viewport.TopLeftX = (FLOAT)((tile % VGFX3D_D3D11_SHADOW_ATLAS_COLUMNS) * width);
        viewport.TopLeftY = (FLOAT)((tile / VGFX3D_D3D11_SHADOW_ATLAS_COLUMNS) * height);
        viewport.Width = (FLOAT)width;
        viewport.Height = (FLOAT)height;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(ctx->ctx, 1, &viewport);
        ID3D11DeviceContext_RSSetState(ctx->ctx, ctx->rs_solid_no_cull);
        /* Shadow maps keep the standard depth convention (Less, clear 1). */
        ID3D11DeviceContext_OMSetDepthStencilState(ctx->ctx, ctx->depth_state_shadow, 0);
        ID3D11DeviceContext_IASetInputLayout(ctx->ctx, ctx->input_layout);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx->ctx, ctx->vs_shadow, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx->ctx, NULL, NULL, 0);
        return;
    }
    hr = d3d11_ensure_shadow_targets(ctx, slot, width, height);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateShadowTargets", hr);
        d3d11_release_shadow_slot(ctx, slot);
        d3d11_recompute_shadow_count(ctx);
        return;
    }

    ctx->shadow_pass_slot = slot;
    vgfx3d_d3d11_copy_mat4_finite_or_identity(ctx->shadow_vp[slot], light_vp);
    d3d11_unbind_draw_resources(ctx);
    d3d11_unbind_shadow_resources(ctx);
    ID3D11DeviceContext_OMSetRenderTargets(ctx->ctx, 0, NULL, ctx->shadow_dsv[slot]);
    ID3D11DeviceContext_ClearDepthStencilView(
        ctx->ctx, ctx->shadow_dsv[slot], D3D11_CLEAR_DEPTH, 1.0f, 0);
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (FLOAT)width;
    viewport.Height = (FLOAT)height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx->ctx, 1, &viewport);
    ID3D11DeviceContext_RSSetState(ctx->ctx, ctx->rs_solid_no_cull);
    /* Shadow maps keep the standard depth convention (Less, clear 1). */
    ID3D11DeviceContext_OMSetDepthStencilState(ctx->ctx, ctx->depth_state_shadow, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx->ctx, ctx->input_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx->ctx, ctx->vs_shadow, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx->ctx, NULL, NULL, 0);
}

/// @brief Backend `shadow_draw` — emit a single mesh into the shadow depth buffer.
///
/// Slimmer than `submit_draw`: only object/scene cbuffers + bone
/// palettes + morph SRV (if any) — no material, no lights, no
/// pixel shader. Uses the light-space VP captured by `shadow_begin`.
/// @param[in,out] ctx_ptr Opaque pointer to the active D3D11 backend context.
/// @param[in] cmd Mesh draw command to sanitize and submit to the active shadow pass.
static void d3d11_shadow_draw(void *ctx_ptr, const vgfx3d_draw_cmd_t *cmd) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    d3d_per_object_t object_data;
    d3d_per_scene_t scene_data;
    d3d_per_material_t material_data;
    d3d_temp_srv_t shadow_diffuse = {0};
    ID3D11ShaderResourceView *shadow_diffuse_srv = NULL;
    ID3D11ShaderResourceView *null_shadow_ps_srv[1] = {NULL};
    ID3D11SamplerState *shadow_diffuse_sampler = NULL;
    HRESULT hr;
    UINT stride = sizeof(vgfx3d_vertex_t);
    UINT offset = 0;
    ID3D11Buffer *mesh_vb = NULL;
    ID3D11Buffer *mesh_ib = NULL;
    ID3D11RasterizerState *shadow_rasterizer = NULL;
    int alpha_masked_shadow;
    uint32_t index_count;
    vgfx3d_draw_cmd_t safe_cmd;

    if (!ctx || !ctx->frame_active)
        return;
    if (!cmd || ctx->shadow_pass_slot < 0 || ctx->shadow_pass_slot >= VGFX3D_MAX_SHADOW_LIGHTS ||
        !cmd->vertices || !cmd->indices || cmd->vertex_count == 0 || cmd->index_count == 0) {
        if (ctx->shadow_pass_slot >= 0)
            ctx->shadow_pass_failed = 1;
        return;
    }
    vgfx3d_sanitize_draw_command(cmd, &safe_cmd);
    cmd = &safe_cmd;
    alpha_masked_shadow = cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK;
    index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
    if (index_count == 0) {
        ctx->shadow_pass_failed = 1;
        return;
    }

    if (d3d11_draw_needs_depth_bias(cmd))
        (void)d3d11_get_depth_biased_rasterizer(
            ctx, cmd, 0, cmd->double_sided ? 0 : 1, 0, &shadow_rasterizer);
    if (!shadow_rasterizer)
        shadow_rasterizer = d3d11_choose_rasterizer(ctx, 0, cmd->double_sided ? 0 : 1);
    if (shadow_rasterizer)
        ID3D11DeviceContext_RSSetState(ctx->ctx, shadow_rasterizer);

    d3d11_prepare_object_data(cmd, &object_data);
    memset(&scene_data, 0, sizeof(scene_data));
    memcpy(scene_data.vp, ctx->shadow_vp[ctx->shadow_pass_slot], sizeof(scene_data.vp));
    d3d11_prepare_anim_resources(ctx, cmd, &object_data);

    hr = d3d11_update_constant_buffer(ctx, ctx->cb_per_object, &object_data, sizeof(object_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbPerObject shadow)", hr);
        ctx->shadow_pass_failed = 1;
        return;
    }
    hr = d3d11_update_constant_buffer(ctx, ctx->cb_per_scene, &scene_data, sizeof(scene_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbPerScene shadow)", hr);
        ctx->shadow_pass_failed = 1;
        return;
    }
    if (alpha_masked_shadow) {
        shadow_diffuse_srv = d3d11_get_or_create_material_srv(
            ctx,
            cmd->texture_asset,
            cmd->texture,
            cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
            cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
            cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
            &shadow_diffuse);
        if ((cmd->texture_asset || cmd->texture) && !shadow_diffuse_srv) {
            ctx->shadow_pass_failed = 1;
            d3d11_release_temp_srv(&shadow_diffuse);
            return;
        }
        d3d11_prepare_material_data(
            cmd, shadow_diffuse_srv != NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, &material_data);
        hr = d3d11_update_constant_buffer(
            ctx, ctx->cb_per_material, &material_data, sizeof(material_data));
        if (FAILED(hr)) {
            d3d11_log_hresult("Map(cbPerMaterial shadow)", hr);
            ctx->shadow_pass_failed = 1;
            d3d11_release_temp_srv(&shadow_diffuse);
            return;
        }
    }

    if (!d3d11_acquire_mesh_buffers(ctx, cmd, &mesh_vb, &mesh_ib)) {
        ctx->shadow_pass_failed = 1;
        d3d11_release_temp_srv(&shadow_diffuse);
        return;
    }

    /* R20: compact-cached geometry needs the packed input layout + stride. */
    if (d3d11_cmd_uses_compact_stream(ctx, cmd)) {
        stride = VGFX3D_COMPACT_VERTEX_STRIDE;
        ID3D11DeviceContext_IASetInputLayout(ctx->ctx, ctx->input_layout_compact);
    } else {
        ID3D11DeviceContext_IASetInputLayout(ctx->ctx, ctx->input_layout);
    }
    ID3D11DeviceContext_IASetVertexBuffers(ctx->ctx, 0, 1, &mesh_vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(ctx->ctx, mesh_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_per_object);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 1, 1, &ctx->cb_per_scene);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 4, 1, &ctx->cb_bones);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 5, 1, &ctx->cb_prev_bones);
    if (alpha_masked_shadow && ctx->ps_shadow) {
        shadow_diffuse_sampler =
            d3d11_get_material_sampler(ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR);
        if (!shadow_diffuse_sampler) {
            ctx->shadow_pass_failed = 1;
            d3d11_release_temp_srv(&shadow_diffuse);
            return;
        }
        ID3D11DeviceContext_PSSetShader(ctx->ctx, ctx->ps_shadow, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx->ctx, 2, 1, &ctx->cb_per_material);
        ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 1, &shadow_diffuse_srv);
        if (shadow_diffuse_sampler)
            ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 0, 1, &shadow_diffuse_sampler);
    } else if (alpha_masked_shadow) {
        ctx->shadow_pass_failed = 1;
        d3d11_release_temp_srv(&shadow_diffuse);
        return;
    } else {
        ID3D11DeviceContext_PSSetShader(ctx->ctx, NULL, NULL, 0);
    }
    {
        ID3D11ShaderResourceView *vs_srvs[2] = {ctx->current_morph_srv, NULL};
        ID3D11DeviceContext_VSSetShaderResources(ctx->ctx, 0, 2, vs_srvs);
    }
    ID3D11DeviceContext_DrawIndexed(ctx->ctx, index_count, 0, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 1, null_shadow_ps_srv);
    ID3D11DeviceContext_PSSetShader(ctx->ctx, NULL, NULL, 0);
    d3d11_release_temp_srv(&shadow_diffuse);
    {
        ID3D11ShaderResourceView *null_vs[2] = {NULL, NULL};
        ID3D11DeviceContext_VSSetShaderResources(ctx->ctx, 0, 2, null_vs);
    }
}

/// @brief Backend `shadow_end` — exit shadow-pass mode and rebind the main targets.
///
/// Marks `shadow_active = 1` so the main pass's PerScene cbuffer
/// reports `shadowEnabled` to the shader. Stores the bias the shader
/// uses to bias depth comparisons. Re-selects the active scene/RTT
/// targets so subsequent draws hit the right buffers.
/// @param[in,out] ctx_ptr Opaque pointer to the active D3D11 backend context.
/// @param[in] slot Shadow slot whose pass is ending.
/// @param[in] bias Depth-comparison bias to sanitize and retain for the main pass.
static void d3d11_shadow_end(void *ctx_ptr, int32_t slot, float bias) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    if (!ctx || !ctx->frame_active)
        return;
    if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS || ctx->shadow_pass_slot != slot) {
        d3d11_leave_shadow_pass(ctx);
        d3d11_recompute_shadow_count(ctx);
        return;
    }
    ctx->shadow_slot_complete[slot] = ctx->shadow_pass_failed ? 0 : 1;
    d3d11_recompute_shadow_count(ctx);
    ctx->shadow_bias = vgfx3d_d3d11_sanitize_shadow_bias(bias);
    ctx->shadow_pass_slot = -1;
    ctx->shadow_pass_failed = 0;
    d3d11_select_current_targets(ctx);
}

/// @brief Backend `shadow_reuse` op — re-arm a slot from its previous-frame depth texture.
///
/// Only the classic per-slot CSM depth textures persist untouched across frames;
/// the shared shadow atlas is whole-cleared on its first tile pass each frame, so
/// atlas slots (>= VGFX3D_CSM_SLOTS) decline reuse and take the normal render path.
/// @param[in,out] ctx_ptr Opaque pointer to the active D3D11 backend context.
/// @param[in] slot Persistent cascade slot to re-arm.
/// @param[in] depth_buf Host depth buffer from the backend interface; unused by D3D11.
/// @param[in] w Expected shadow-map width in pixels.
/// @param[in] h Expected shadow-map height in pixels.
/// @param[in] light_vp Current light view-projection matrix for the reused slot.
/// @return One when a compatible persistent shadow texture is reused; otherwise zero.
static int8_t d3d11_shadow_reuse(
    void *ctx_ptr, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    (void)depth_buf;
    if (!ctx || !ctx->frame_active || slot < 0 || slot >= VGFX3D_CSM_SLOTS || !light_vp || w <= 0 ||
        h <= 0)
        return 0;
    if (!vgfx3d_d3d11_shadow_matrix_is_usable(light_vp))
        return 0;
    if (!ctx->shadow_dsv[slot] || !ctx->shadow_srv[slot] || ctx->shadow_width[slot] != w ||
        ctx->shadow_height[slot] != h)
        return 0;
    vgfx3d_d3d11_copy_mat4_finite_or_identity(ctx->shadow_vp[slot], light_vp);
    ctx->shadow_slot_complete[slot] = 1;
    d3d11_recompute_shadow_count(ctx);
    return 1;
}

/// @brief Backend `draw_skybox` — full-screen triangle draw with the cubemap shader.
///
/// Sets up skybox VS/PS, the fullscreen-triangle VB built at context creation,
/// the inverse-projection/inverse-view-rotation constants, and a depth state
/// that draws "behind" everything else (`<=` compare with no depth writes).
/// The cubemap is bound from the cubemap cache or as a temp resource.
/// @param[in,out] ctx_ptr Opaque pointer to the active D3D11 backend context.
/// @param[in] cubemap_ptr Cubemap runtime object to sample for the skybox.
static void d3d11_draw_skybox(void *ctx_ptr, const void *cubemap_ptr) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    const rt_cubemap3d *cubemap = (const rt_cubemap3d *)cubemap_ptr;
    d3d_skybox_cb_t skybox_data;
    d3d_temp_srv_t cubemap_resource;
    ID3D11ShaderResourceView *srv;
    HRESULT hr;
    UINT stride = sizeof(float) * 3;
    UINT offset = 0;
    D3D11_VIEWPORT viewport;
    float blend_factor[4] = {0, 0, 0, 0};

    if (!ctx || !cubemap || !ctx->vs_skybox || !ctx->ps_skybox || !ctx->skybox_vb ||
        !vgfx3d_d3d11_draw_submission_is_ready(ctx->frame_active,
                                               ctx->shadow_pass_slot,
                                               ctx->ctx != NULL,
                                               ctx->current_rtv_count,
                                               ctx->current_rtvs[0] != NULL,
                                               ctx->current_width,
                                               ctx->current_height))
        return;

    memset(&cubemap_resource, 0, sizeof(cubemap_resource));
    srv = d3d11_get_or_create_cubemap_srv(ctx, cubemap, &cubemap_resource);
    if (!srv)
        return;

    d3d11_prepare_skybox_data(ctx, &skybox_data);
    hr = d3d11_update_constant_buffer(ctx, ctx->cb_skybox, &skybox_data, sizeof(skybox_data));
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(cbSkybox)", hr);
        d3d11_release_temp_srv(&cubemap_resource);
        return;
    }

    ID3D11DeviceContext_IASetInputLayout(ctx->ctx, ctx->input_layout_skybox);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx->ctx, 0, 1, &ctx->skybox_vb, &stride, &offset);
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (FLOAT)ctx->current_width;
    viewport.Height = (FLOAT)ctx->current_height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(ctx->ctx, 1, ctx->current_rtvs, ctx->current_dsv);
    ID3D11DeviceContext_RSSetViewports(ctx->ctx, 1, &viewport);
    ID3D11DeviceContext_VSSetShader(ctx->ctx, ctx->vs_skybox, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx->ctx, ctx->ps_skybox, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx->ctx, 0, 1, &ctx->cb_skybox);
    if (ctx->linear_clamp_sampler)
        ID3D11DeviceContext_PSSetSamplers(ctx->ctx, 0, 1, &ctx->linear_clamp_sampler);
    ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 1, &srv);
    ID3D11DeviceContext_RSSetState(ctx->ctx, ctx->rs_solid_no_cull);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx->ctx, ctx->depth_state_skybox, 0);
    ID3D11DeviceContext_OMSetBlendState(
        ctx->ctx, ctx->blend_state_opaque, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_Draw(ctx->ctx, 3, 0);
    {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx->ctx, 0, 1, &null_srv);
    }
    d3d11_release_temp_srv(&cubemap_resource);
}
