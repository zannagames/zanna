//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_targets.inc
// Purpose: D3D11 scene, overlay, post-FX, render-to-texture, and shadow target lifecycle.
// Key invariants:
//   - Native dimensions, formats, row pitches, and subresources are validated before copies.
//   - Target replacement publishes only complete RTV/SRV/DSV resource sets.
// Ownership/Lifetime:
//   - COM resources belong to d3d11_context_t and are released through SAFE_RELEASE.
//   - RenderTarget3D shells are borrowed only while the Canvas retains their runtime owners.
// Links: vgfx3d_backend_d3d11.c, vgfx3d_backend_d3d11_present.inc
//
//===----------------------------------------------------------------------===//

/// @brief Allocate a render-target color texture (with optional shader-readable SRV).
///
/// Defaults to single-sample, MipLevels=1. Format chosen by the
/// caller-supplied `format_class`. If `out_srv` is non-NULL, also adds
/// `D3D11_BIND_SHADER_RESOURCE` so post-FX passes can sample the
/// rendered scene.
static HRESULT d3d11_create_color_target(d3d11_context_t *ctx,
                                         int32_t width,
                                         int32_t height,
                                         vgfx3d_d3d11_color_format_t format_class,
                                         ID3D11Texture2D **out_tex,
                                         ID3D11RenderTargetView **out_rtv,
                                         ID3D11ShaderResourceView **out_srv) {
    D3D11_TEXTURE2D_DESC desc;
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    HRESULT hr;

    if (out_tex)
        *out_tex = NULL;
    if (out_rtv)
        *out_rtv = NULL;
    if (out_srv)
        *out_srv = NULL;
    if (!ctx || !ctx->device || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height) ||
        !vgfx3d_d3d11_is_valid_color_format((int32_t)format_class) || !out_tex || !out_rtv)
        return E_INVALIDARG;

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = d3d11_color_format_to_dxgi(format_class);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | (out_srv ? D3D11_BIND_SHADER_RESOURCE : 0);

    hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, NULL, out_tex);
    if (FAILED(hr) || !*out_tex) {
        SAFE_RELEASE(*out_tex);
        return FAILED(hr) ? hr : E_POINTER;
    }

    memset(&rtv_desc, 0, sizeof(rtv_desc));
    rtv_desc.Format = desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = ID3D11Device_CreateRenderTargetView(
        ctx->device, (ID3D11Resource *)*out_tex, &rtv_desc, out_rtv);
    if (FAILED(hr) || !*out_rtv) {
        SAFE_RELEASE(*out_rtv);
        SAFE_RELEASE(*out_tex);
        return FAILED(hr) ? hr : E_POINTER;
    }

    if (out_srv) {
        memset(&srv_desc, 0, sizeof(srv_desc));
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        hr = ID3D11Device_CreateShaderResourceView(
            ctx->device, (ID3D11Resource *)*out_tex, &srv_desc, out_srv);
        if (FAILED(hr) || !*out_srv) {
            SAFE_RELEASE(*out_srv);
            SAFE_RELEASE(*out_rtv);
            SAFE_RELEASE(*out_tex);
            return FAILED(hr) ? hr : E_POINTER;
        }
    }

    return S_OK;
}

/// @brief Allocate a depth-stencil texture (with optional shader-readable SRV).
///
/// Uses TYPELESS storage when `shader_readable` is set so the texture
/// can be aliased as both DSV (D32_FLOAT) and SRV (R32_FLOAT) — this
/// pattern lets shadow maps and SSAO read the depth buffer.
static HRESULT d3d11_create_depth_target(d3d11_context_t *ctx,
                                         int32_t width,
                                         int32_t height,
                                         int shader_readable,
                                         ID3D11Texture2D **out_tex,
                                         ID3D11DepthStencilView **out_dsv,
                                         ID3D11ShaderResourceView **out_srv) {
    D3D11_TEXTURE2D_DESC desc;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    HRESULT hr;

    if (out_tex)
        *out_tex = NULL;
    if (out_dsv)
        *out_dsv = NULL;
    if (out_srv)
        *out_srv = NULL;
    if (!ctx || !ctx->device || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height) ||
        !out_tex || !out_dsv)
        return E_INVALIDARG;

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = shader_readable ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags =
        D3D11_BIND_DEPTH_STENCIL | (shader_readable && out_srv ? D3D11_BIND_SHADER_RESOURCE : 0);

    hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, NULL, out_tex);
    if (FAILED(hr) || !*out_tex) {
        SAFE_RELEASE(*out_tex);
        return FAILED(hr) ? hr : E_POINTER;
    }

    memset(&dsv_desc, 0, sizeof(dsv_desc));
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = ID3D11Device_CreateDepthStencilView(
        ctx->device, (ID3D11Resource *)*out_tex, &dsv_desc, out_dsv);
    if (FAILED(hr) || !*out_dsv) {
        SAFE_RELEASE(*out_dsv);
        SAFE_RELEASE(*out_tex);
        return FAILED(hr) ? hr : E_POINTER;
    }

    if (shader_readable && out_srv) {
        memset(&srv_desc, 0, sizeof(srv_desc));
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        hr = ID3D11Device_CreateShaderResourceView(
            ctx->device, (ID3D11Resource *)*out_tex, &srv_desc, out_srv);
        if (FAILED(hr) || !*out_srv) {
            SAFE_RELEASE(*out_srv);
            SAFE_RELEASE(*out_dsv);
            SAFE_RELEASE(*out_tex);
            return FAILED(hr) ? hr : E_POINTER;
        }
    }

    return S_OK;
}

/// @brief Allocate a CPU-readable staging texture for `Map(READ)`-based readback.
///
/// Used by color readback and by the one-row R32 typeless depth-probe strip.
static HRESULT d3d11_create_staging_texture(d3d11_context_t *ctx,
                                            int32_t width,
                                            int32_t height,
                                            DXGI_FORMAT format,
                                            ID3D11Texture2D **out_tex) {
    D3D11_TEXTURE2D_DESC desc;

    if (out_tex)
        *out_tex = NULL;
    if (!ctx || !ctx->device || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height) ||
        !out_tex ||
        (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
         format != DXGI_FORMAT_R32_TYPELESS))
        return E_INVALIDARG;

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    {
        HRESULT hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, NULL, out_tex);
        if (FAILED(hr) || !*out_tex) {
            SAFE_RELEASE(*out_tex);
            return FAILED(hr) ? hr : E_POINTER;
        }
        return S_OK;
    }
}

/// @brief Release the reusable window/post-FX readback staging texture.
///
/// @details The RTT path owns its own staging texture because its lifetime is tied to the active
///          RenderTarget3D. This helper handles the generic screenshot/readback staging surface.
static void d3d11_release_readback_staging_texture(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    SAFE_RELEASE(ctx->readback_staging);
    ctx->readback_staging_width = 0;
    ctx->readback_staging_height = 0;
    ctx->readback_staging_format = DXGI_FORMAT_UNKNOWN;
}

/// @brief Ensure a reusable CPU-readable staging texture for window/post-FX readback.
///
/// @details Reuses the existing staging texture when dimensions and format match, avoiding a
///          CreateTexture2D/Release pair for every screenshot or readback. Size/format changes
///          stage a replacement before releasing the last usable surface.
static HRESULT d3d11_ensure_readback_staging_texture(d3d11_context_t *ctx,
                                                     int32_t width,
                                                     int32_t height,
                                                     DXGI_FORMAT format) {
    ID3D11Texture2D *new_staging = NULL;
    HRESULT hr;
    if (!ctx)
        return E_INVALIDARG;
    if (ctx->readback_staging && ctx->readback_staging_width == width &&
        ctx->readback_staging_height == height && ctx->readback_staging_format == format)
        return S_OK;
    hr = d3d11_create_staging_texture(ctx, width, height, format, &new_staging);
    if (FAILED(hr) || !new_staging) {
        SAFE_RELEASE(new_staging);
        return FAILED(hr) ? hr : E_POINTER;
    }
    d3d11_release_readback_staging_texture(ctx);
    ctx->readback_staging = new_staging;
    ctx->readback_staging_width = width;
    ctx->readback_staging_height = height;
    ctx->readback_staging_format = format;
    return S_OK;
}

/// @brief Allocate/reuse the default-usage texture that snapshots the final swapchain image.
static HRESULT d3d11_ensure_presented_snapshot_texture(d3d11_context_t *ctx,
                                                       int32_t width,
                                                       int32_t height) {
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *new_texture = NULL;
    HRESULT hr;

    if (!ctx || !ctx->device || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return E_INVALIDARG;
    if (ctx->presented_color_tex && ctx->presented_width == width &&
        ctx->presented_height == height)
        return S_OK;

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, NULL, &new_texture);
    if (FAILED(hr) || !new_texture) {
        SAFE_RELEASE(new_texture);
        return FAILED(hr) ? hr : E_POINTER;
    }
    SAFE_RELEASE(ctx->presented_color_tex);
    ctx->presented_color_tex = new_texture;
    ctx->presented_width = width;
    ctx->presented_height = height;
    ctx->presented_color_valid = 0;
    return S_OK;
}

/// @brief Capture the current swapchain backbuffer before Present invalidates it.
static int d3d11_snapshot_backbuffer_for_readback(d3d11_context_t *ctx) {
    ID3D11Texture2D *back_buffer = NULL;
    D3D11_TEXTURE2D_DESC back_buffer_desc;
    HRESULT hr;

    if (!ctx || !ctx->swap_chain || !ctx->ctx)
        return 0;
    ctx->presented_color_valid = 0;
    hr = IDXGISwapChain_GetBuffer(ctx->swap_chain, 0, &IID_ID3D11Texture2D, (void **)&back_buffer);
    if (FAILED(hr) || !back_buffer) {
        if (SUCCEEDED(hr))
            hr = E_POINTER;
        d3d11_log_hresult("IDXGISwapChain::GetBuffer(presentedSnapshot)", hr);
        SAFE_RELEASE(back_buffer);
        return 0;
    }
    ID3D11Texture2D_GetDesc(back_buffer, &back_buffer_desc);
    if (back_buffer_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        !vgfx3d_d3d11_is_single_subresource_texture2d(back_buffer_desc.Width,
                                                      back_buffer_desc.Height,
                                                      back_buffer_desc.MipLevels,
                                                      back_buffer_desc.ArraySize,
                                                      back_buffer_desc.SampleDesc.Count,
                                                      back_buffer_desc.SampleDesc.Quality)) {
        SAFE_RELEASE(back_buffer);
        return 0;
    }
    hr = d3d11_ensure_presented_snapshot_texture(
        ctx, (int32_t)back_buffer_desc.Width, (int32_t)back_buffer_desc.Height);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateTexture2D(presentedSnapshot)", hr);
        SAFE_RELEASE(back_buffer);
        return 0;
    }
    d3d11_unbind_draw_resources(ctx);
    d3d11_unbind_output_targets(ctx);
    ID3D11DeviceContext_CopyResource(
        ctx->ctx, (ID3D11Resource *)ctx->presented_color_tex, (ID3D11Resource *)back_buffer);
    SAFE_RELEASE(back_buffer);
    d3d11_restore_current_target_bindings(ctx);
    hr = ctx->device ? ID3D11Device_GetDeviceRemovedReason(ctx->device) : E_POINTER;
    if (FAILED(hr)) {
        d3d11_log_hresult("CopyResource(presentedSnapshot)", hr);
        d3d11_log_device_removed_reason(ctx, "CopyResource(presentedSnapshot)", hr);
        return 0;
    }
    ctx->presented_color_valid = 1;
    return 1;
}

/// @brief Release the scaled scene and its size-dependent auxiliary targets.
/// @details The native-size overlay is intentionally preserved. This lets a render-scale change
///   stage a replacement scene while retaining both the last usable scene and overlay until every
///   allocation needed for the new route has succeeded.
static void d3d11_destroy_scene_route_targets(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    d3d11_unbind_postfx_resources(ctx);
    d3d11_unbind_shadow_resources(ctx);
    d3d11_unbind_output_targets(ctx);
    if (ctx->current_target_kind == VGFX3D_D3D11_TARGET_SCENE ||
        ctx->current_target_kind == VGFX3D_D3D11_TARGET_OVERLAY) {
        d3d11_clear_current_target_bindings(ctx);
    }
    SAFE_RELEASE(ctx->postfx_scratch_srv);
    SAFE_RELEASE(ctx->postfx_scratch_rtv);
    SAFE_RELEASE(ctx->postfx_scratch_tex);
    SAFE_RELEASE(ctx->postfx_color_srv);
    SAFE_RELEASE(ctx->postfx_color_rtv);
    SAFE_RELEASE(ctx->postfx_color_tex);
    d3d11_destroy_opaque_depth_target(ctx);
    d3d11_destroy_bloom_targets(ctx);
    d3d11_destroy_taa_targets(ctx);
    d3d11_destroy_ssr_target(ctx);
    d3d11_release_readback_staging_texture(ctx);
    SAFE_RELEASE(ctx->presented_color_tex);
    SAFE_RELEASE(ctx->scene_depth_srv);
    SAFE_RELEASE(ctx->scene_dsv);
    SAFE_RELEASE(ctx->scene_depth_tex);
    SAFE_RELEASE(ctx->scene_motion_srv);
    SAFE_RELEASE(ctx->scene_motion_rtv);
    SAFE_RELEASE(ctx->scene_motion_tex);
    SAFE_RELEASE(ctx->scene_color_srv);
    SAFE_RELEASE(ctx->scene_color_rtv);
    SAFE_RELEASE(ctx->scene_color_tex);
    d3d11_reset_depth_probe_state(ctx);
    ctx->scene_width = 0;
    ctx->scene_height = 0;
    ctx->postfx_width = 0;
    ctx->postfx_height = 0;
    ctx->postfx_scratch_width = 0;
    ctx->postfx_scratch_height = 0;
    ctx->presented_width = 0;
    ctx->presented_height = 0;
    ctx->presented_color_valid = 0;
    d3d11_reset_temporal_scene_state(ctx);
}

/// @brief Release every offscreen target used by the window-scene route.
/// @details Full teardown is used on resize and context destruction. Scene-only replacement uses
///   d3d11_destroy_scene_route_targets so a valid native-size overlay survives transactionally.
static void d3d11_destroy_scene_targets(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    d3d11_destroy_scene_route_targets(ctx);
    SAFE_RELEASE(ctx->overlay_color_srv);
    SAFE_RELEASE(ctx->overlay_color_rtv);
    SAFE_RELEASE(ctx->overlay_color_tex);
    ctx->overlay_width = 0;
    ctx->overlay_height = 0;
}

/// @brief Allocate scene render targets at the requested size, idempotent on size match.
///
/// Builds three targets: scene color (HDR-capable), motion vectors
/// (LDR), and depth (R32). A complete replacement is staged before the
/// live scene/dependent targets are retired; allocation failure preserves them.
static HRESULT d3d11_ensure_scene_targets(d3d11_context_t *ctx, int32_t width, int32_t height) {
    ID3D11Texture2D *new_color_tex = NULL;
    ID3D11RenderTargetView *new_color_rtv = NULL;
    ID3D11ShaderResourceView *new_color_srv = NULL;
    ID3D11Texture2D *new_motion_tex = NULL;
    ID3D11RenderTargetView *new_motion_rtv = NULL;
    ID3D11ShaderResourceView *new_motion_srv = NULL;
    ID3D11Texture2D *new_depth_tex = NULL;
    ID3D11DepthStencilView *new_dsv = NULL;
    ID3D11ShaderResourceView *new_depth_srv = NULL;
    HRESULT hr;

    if (!ctx || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return E_INVALIDARG;
    if (ctx->scene_color_tex && ctx->scene_color_rtv && ctx->scene_color_srv &&
        ctx->scene_motion_tex && ctx->scene_motion_rtv && ctx->scene_motion_srv &&
        ctx->scene_depth_tex && ctx->scene_dsv && ctx->scene_depth_srv &&
        ctx->scene_width == width && ctx->scene_height == height)
        return S_OK;

    hr = d3d11_create_color_target(ctx,
                                   width,
                                   height,
                                   vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_SCENE),
                                   &new_color_tex,
                                   &new_color_rtv,
                                   &new_color_srv);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_color_target(ctx,
                                   width,
                                   height,
                                   vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_SWAPCHAIN),
                                   &new_motion_tex,
                                   &new_motion_rtv,
                                   &new_motion_srv);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_depth_target(ctx, width, height, 1, &new_depth_tex, &new_dsv, &new_depth_srv);
    if (FAILED(hr))
        goto fail;

    d3d11_destroy_scene_route_targets(ctx);
    ctx->scene_color_tex = new_color_tex;
    ctx->scene_color_rtv = new_color_rtv;
    ctx->scene_color_srv = new_color_srv;
    ctx->scene_motion_tex = new_motion_tex;
    ctx->scene_motion_rtv = new_motion_rtv;
    ctx->scene_motion_srv = new_motion_srv;
    ctx->scene_depth_tex = new_depth_tex;
    ctx->scene_dsv = new_dsv;
    ctx->scene_depth_srv = new_depth_srv;
    ctx->scene_width = width;
    ctx->scene_height = height;
    return S_OK;

fail:
    SAFE_RELEASE(new_depth_srv);
    SAFE_RELEASE(new_dsv);
    SAFE_RELEASE(new_depth_tex);
    SAFE_RELEASE(new_motion_srv);
    SAFE_RELEASE(new_motion_rtv);
    SAFE_RELEASE(new_motion_tex);
    SAFE_RELEASE(new_color_srv);
    SAFE_RELEASE(new_color_rtv);
    SAFE_RELEASE(new_color_tex);
    return hr;
}

/// @brief Allocate the overlay render target (HUD layer above 3D scene).
///
/// Single color target — overlay doesn't need its own depth (it's
/// drawn 2D on top of the composited scene).
static HRESULT d3d11_ensure_overlay_target(d3d11_context_t *ctx, int32_t width, int32_t height) {
    ID3D11Texture2D *new_tex = NULL;
    ID3D11RenderTargetView *new_rtv = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;
    HRESULT hr;

    if (!ctx || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return E_INVALIDARG;
    if (ctx->overlay_color_tex && ctx->overlay_color_rtv && ctx->overlay_color_srv &&
        ctx->overlay_width == width && ctx->overlay_height == height)
        return S_OK;
    hr = d3d11_create_color_target(ctx,
                                   width,
                                   height,
                                   vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_OVERLAY),
                                   &new_tex,
                                   &new_rtv,
                                   &new_srv);
    if (FAILED(hr))
        return hr;
    if (ctx->overlay_color_tex || ctx->overlay_color_rtv || ctx->overlay_color_srv) {
        d3d11_unbind_postfx_resources(ctx);
        if (ctx->current_target_kind == VGFX3D_D3D11_TARGET_OVERLAY) {
            d3d11_unbind_output_targets(ctx);
            d3d11_clear_current_target_bindings(ctx);
        }
        SAFE_RELEASE(ctx->overlay_color_srv);
        SAFE_RELEASE(ctx->overlay_color_rtv);
        SAFE_RELEASE(ctx->overlay_color_tex);
        ctx->overlay_width = 0;
        ctx->overlay_height = 0;
        ctx->overlay_used_this_frame = 0;
    }
    ctx->overlay_color_tex = new_tex;
    ctx->overlay_color_rtv = new_rtv;
    ctx->overlay_color_srv = new_srv;
    ctx->overlay_width = width;
    ctx->overlay_height = height;
    return S_OK;
}

/// @brief Ensure the post-processing render target is allocated at the requested dimensions.
/// @details Skips reallocation when an existing target already matches @p width × @p height.
///          On a size change (or first call), creates a complete scene-format replacement before
///          unbinding and releasing the prior color SRV/RTV/texture.
/// @return S_OK on success; E_INVALIDARG for a null context or non-positive dimensions;
///         the D3D11 HRESULT from d3d11_create_color_target on allocation failure.
static HRESULT d3d11_ensure_postfx_target(d3d11_context_t *ctx, int32_t width, int32_t height) {
    ID3D11Texture2D *new_tex = NULL;
    ID3D11RenderTargetView *new_rtv = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;
    HRESULT hr;

    if (!ctx || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return E_INVALIDARG;
    if (ctx->postfx_color_tex && ctx->postfx_color_rtv && ctx->postfx_color_srv &&
        ctx->postfx_width == width && ctx->postfx_height == height)
        return S_OK;

    hr = d3d11_create_color_target(ctx,
                                   width,
                                   height,
                                   vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_SCENE),
                                   &new_tex,
                                   &new_rtv,
                                   &new_srv);
    if (FAILED(hr))
        return hr;
    d3d11_unbind_postfx_resources(ctx);
    d3d11_unbind_output_targets(ctx);
    SAFE_RELEASE(ctx->postfx_color_srv);
    SAFE_RELEASE(ctx->postfx_color_rtv);
    SAFE_RELEASE(ctx->postfx_color_tex);
    ctx->postfx_color_tex = new_tex;
    ctx->postfx_color_rtv = new_rtv;
    ctx->postfx_color_srv = new_srv;
    ctx->postfx_width = width;
    ctx->postfx_height = height;
    return S_OK;
}

/// @brief Ensure the secondary post-FX scratch target used for non-destructive ping-pong.
static HRESULT d3d11_ensure_postfx_scratch_target(d3d11_context_t *ctx,
                                                  int32_t width,
                                                  int32_t height) {
    ID3D11Texture2D *new_tex = NULL;
    ID3D11RenderTargetView *new_rtv = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;
    HRESULT hr;

    if (!ctx || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return E_INVALIDARG;
    if (ctx->postfx_scratch_tex && ctx->postfx_scratch_rtv && ctx->postfx_scratch_srv &&
        ctx->postfx_scratch_width == width && ctx->postfx_scratch_height == height)
        return S_OK;

    hr = d3d11_create_color_target(ctx,
                                   width,
                                   height,
                                   vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_SCENE),
                                   &new_tex,
                                   &new_rtv,
                                   &new_srv);
    if (FAILED(hr))
        return hr;
    d3d11_unbind_postfx_resources(ctx);
    d3d11_unbind_output_targets(ctx);
    SAFE_RELEASE(ctx->postfx_scratch_srv);
    SAFE_RELEASE(ctx->postfx_scratch_rtv);
    SAFE_RELEASE(ctx->postfx_scratch_tex);
    ctx->postfx_scratch_tex = new_tex;
    ctx->postfx_scratch_rtv = new_rtv;
    ctx->postfx_scratch_srv = new_srv;
    ctx->postfx_scratch_width = width;
    ctx->postfx_scratch_height = height;
    return S_OK;
}

/// @brief Release the Plan 05 bloom mip-chain textures/RTVs/SRVs.
static void d3d11_destroy_bloom_targets(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    d3d11_unbind_postfx_resources(ctx);
    for (int32_t i = 0; i < VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX; i++) {
        SAFE_RELEASE(ctx->bloom_mip_srv[i]);
        SAFE_RELEASE(ctx->bloom_mip_rtv[i]);
        SAFE_RELEASE(ctx->bloom_mip_tex[i]);
        ctx->bloom_mip_w[i] = 0;
        ctx->bloom_mip_h[i] = 0;
    }
    ctx->bloom_mip_count = 0;
    ctx->bloom_base_width = 0;
    ctx->bloom_base_height = 0;
    ctx->postfx_current_bloom_srv = NULL;
}

/// @brief Whether the cached bloom target chain is complete for the requested scene size.
static int d3d11_bloom_targets_complete(const d3d11_context_t *ctx, int32_t width, int32_t height) {
    if (!ctx || !vgfx3d_d3d11_is_valid_bloom_mip_count(ctx->bloom_mip_count) ||
        ctx->bloom_base_width != width || ctx->bloom_base_height != height)
        return 0;
    for (int32_t i = 0; i < ctx->bloom_mip_count; i++) {
        if (!ctx->bloom_mip_tex[i] || !ctx->bloom_mip_rtv[i] || !ctx->bloom_mip_srv[i] ||
            ctx->bloom_mip_w[i] <= 0 || ctx->bloom_mip_h[i] <= 0)
            return 0;
    }
    return 1;
}

/// @brief Build the Plan 05 half-res RGBA16F bloom mip chain for a scene of (w, h).
/// @details Level 0 starts at half-res clamped to at least 1px; deeper levels halve
///          until the next level would drop below 8px on either axis (at most 6 levels).
///          Rebuilt only when the scene size changes.
static HRESULT d3d11_ensure_bloom_targets(d3d11_context_t *ctx, int32_t width, int32_t height) {
    ID3D11Texture2D *new_tex[VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX] = {NULL};
    ID3D11RenderTargetView *new_rtv[VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX] = {NULL};
    ID3D11ShaderResourceView *new_srv[VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX] = {NULL};
    int32_t new_w[VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX] = {0};
    int32_t new_h[VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX] = {0};
    int32_t new_count = 0;
    int32_t mw;
    int32_t mh;
    HRESULT hr = E_FAIL;

    if (!ctx || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height) || !ctx->ps_bloom_down ||
        !ctx->ps_bloom_up)
        return E_INVALIDARG;
    if (d3d11_bloom_targets_complete(ctx, width, height))
        return S_OK;

    for (int32_t i = 0; i < VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX; i++) {
        if (!vgfx3d_d3d11_compute_bloom_mip_extent(width, height, i, &mw, &mh))
            break;
        hr = d3d11_create_color_target(ctx,
                                       mw,
                                       mh,
                                       vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_SCENE),
                                       &new_tex[i],
                                       &new_rtv[i],
                                       &new_srv[i]);
        if (FAILED(hr))
            goto fail;
        new_w[i] = mw;
        new_h[i] = mh;
        new_count = i + 1;
    }
    if (new_count <= 0)
        goto fail;
    d3d11_destroy_bloom_targets(ctx);
    for (int32_t i = 0; i < new_count; i++) {
        ctx->bloom_mip_tex[i] = new_tex[i];
        ctx->bloom_mip_rtv[i] = new_rtv[i];
        ctx->bloom_mip_srv[i] = new_srv[i];
        ctx->bloom_mip_w[i] = new_w[i];
        ctx->bloom_mip_h[i] = new_h[i];
    }
    ctx->bloom_mip_count = new_count;
    ctx->bloom_base_width = width;
    ctx->bloom_base_height = height;
    return S_OK;

fail:
    for (int32_t i = 0; i < VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX; i++) {
        SAFE_RELEASE(new_srv[i]);
        SAFE_RELEASE(new_rtv[i]);
        SAFE_RELEASE(new_tex[i]);
    }
    return hr;
}

/// @brief Release the Plan 05 TAA ping-pong history textures/RTVs/SRVs.
static void d3d11_destroy_taa_targets(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    d3d11_unbind_postfx_resources(ctx);
    for (int32_t i = 0; i < 2; i++) {
        SAFE_RELEASE(ctx->taa_history_srv[i]);
        SAFE_RELEASE(ctx->taa_history_rtv[i]);
        SAFE_RELEASE(ctx->taa_history_tex[i]);
    }
    ctx->taa_history_width = 0;
    ctx->taa_history_height = 0;
    ctx->taa_history_parity = 0;
    ctx->taa_history_valid = 0;
}

/// @brief Whether both cached TAA history targets are complete for the requested scene size.
static int d3d11_taa_targets_complete(const d3d11_context_t *ctx, int32_t width, int32_t height) {
    if (!ctx || ctx->taa_history_width != width || ctx->taa_history_height != height)
        return 0;
    for (int32_t i = 0; i < 2; i++) {
        if (!ctx->taa_history_tex[i] || !ctx->taa_history_rtv[i] || !ctx->taa_history_srv[i])
            return 0;
    }
    return 1;
}

/// @brief Build the Plan 05 TAA ping-pong history pair (RGBA16F, scene-sized).
static HRESULT d3d11_ensure_taa_targets(d3d11_context_t *ctx, int32_t width, int32_t height) {
    ID3D11Texture2D *new_tex[2] = {NULL, NULL};
    ID3D11RenderTargetView *new_rtv[2] = {NULL, NULL};
    ID3D11ShaderResourceView *new_srv[2] = {NULL, NULL};
    HRESULT hr;

    if (!ctx || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height) || !ctx->ps_taa)
        return E_INVALIDARG;
    if (d3d11_taa_targets_complete(ctx, width, height))
        return S_OK;

    for (int32_t i = 0; i < 2; i++) {
        hr = d3d11_create_color_target(ctx,
                                       width,
                                       height,
                                       vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_SCENE),
                                       &new_tex[i],
                                       &new_rtv[i],
                                       &new_srv[i]);
        if (FAILED(hr))
            goto fail;
    }
    d3d11_destroy_taa_targets(ctx);
    for (int32_t i = 0; i < 2; i++) {
        ctx->taa_history_tex[i] = new_tex[i];
        ctx->taa_history_rtv[i] = new_rtv[i];
        ctx->taa_history_srv[i] = new_srv[i];
    }
    ctx->taa_history_width = width;
    ctx->taa_history_height = height;
    ctx->taa_history_parity = 0;
    ctx->taa_history_valid = 0;
    return S_OK;

fail:
    for (int32_t i = 0; i < 2; i++) {
        SAFE_RELEASE(new_srv[i]);
        SAFE_RELEASE(new_rtv[i]);
        SAFE_RELEASE(new_tex[i]);
    }
    return hr;
}

/// @brief Release the Plan 10 SSR output target.
static void d3d11_destroy_ssr_target(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    d3d11_unbind_postfx_resources(ctx);
    SAFE_RELEASE(ctx->ssr_srv);
    SAFE_RELEASE(ctx->ssr_rtv);
    SAFE_RELEASE(ctx->ssr_tex);
    ctx->ssr_width = 0;
    ctx->ssr_height = 0;
}

/// @brief Whether the cached SSR target is complete for the requested scene size.
static int d3d11_ssr_target_complete(const d3d11_context_t *ctx, int32_t width, int32_t height) {
    return ctx && ctx->ssr_width == width && ctx->ssr_height == height && ctx->ssr_tex &&
           ctx->ssr_rtv && ctx->ssr_srv;
}

/// @brief Build the Plan 10 SSR output target (scene color format, scene-sized).
static HRESULT d3d11_ensure_ssr_target(d3d11_context_t *ctx, int32_t width, int32_t height) {
    ID3D11Texture2D *new_tex = NULL;
    ID3D11RenderTargetView *new_rtv = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;
    HRESULT hr;

    if (!ctx || !vgfx3d_d3d11_is_valid_texture2d_extent(width, height) || !ctx->ps_ssr)
        return E_INVALIDARG;
    if (d3d11_ssr_target_complete(ctx, width, height))
        return S_OK;

    hr = d3d11_create_color_target(ctx,
                                   width,
                                   height,
                                   vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_SCENE),
                                   &new_tex,
                                   &new_rtv,
                                   &new_srv);
    if (FAILED(hr))
        return hr;
    d3d11_destroy_ssr_target(ctx);
    ctx->ssr_tex = new_tex;
    ctx->ssr_rtv = new_rtv;
    ctx->ssr_srv = new_srv;
    ctx->ssr_width = width;
    ctx->ssr_height = height;
    return S_OK;
}

/// @brief Copy the GPU-side RTT color texture down to the CPU-side `rt_pixels` payload.
/// @details When a Canvas3D rendertarget is read back via `RenderTarget3D.Pixels()`, the
///   D3D11 backend copies the bound `rtt_color_tex` through a staging texture (CPU-readable),
///   maps it, and writes the pixels into `target->color_pixels` using `rt_pixels` layout.
///   Returns 0 when preconditions fail so the caller can fall back to the last cached copy.
static int d3d11_sync_render_target_color(void *ctx_ptr, vgfx3d_rendertarget_t *target) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    D3D11_TEXTURE2D_DESC color_desc;
    D3D11_TEXTURE2D_DESC staging_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    DXGI_FORMAT expected_format;
    HRESULT hr;
    int restore_target = 0;
    int mapped_ok = 0;
    int staging_failed = 0;
    int ok = 0;

    if (!ctx || !ctx->ctx || !target || !ctx->rtt_color_tex || !ctx->rtt_staging ||
        ctx->rtt_target != target || target->width <= 0 || target->height <= 0) {
        return 0;
    }
    if (!vgfx3d_d3d11_rtt_readback_state_matches(target->width,
                                                 target->height,
                                                 target->color_format,
                                                 ctx->rtt_width,
                                                 ctx->rtt_height,
                                                 ctx->rtt_color_format))
        return 0;
    expected_format = vgfx3d_rendertarget_is_hdr(target) ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                         : DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D11Texture2D_GetDesc(ctx->rtt_color_tex, &color_desc);
    ID3D11Texture2D_GetDesc(ctx->rtt_staging, &staging_desc);
    if (!vgfx3d_d3d11_is_single_subresource_texture2d(color_desc.Width,
                                                      color_desc.Height,
                                                      color_desc.MipLevels,
                                                      color_desc.ArraySize,
                                                      color_desc.SampleDesc.Count,
                                                      color_desc.SampleDesc.Quality) ||
        color_desc.Width != (UINT)target->width || color_desc.Height != (UINT)target->height ||
        color_desc.Format != expected_format || color_desc.Usage != D3D11_USAGE_DEFAULT ||
        (color_desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 || color_desc.CPUAccessFlags != 0 ||
        color_desc.MiscFlags != 0) {
        return 0;
    }
    if (!vgfx3d_d3d11_is_single_subresource_texture2d(staging_desc.Width,
                                                      staging_desc.Height,
                                                      staging_desc.MipLevels,
                                                      staging_desc.ArraySize,
                                                      staging_desc.SampleDesc.Count,
                                                      staging_desc.SampleDesc.Quality) ||
        staging_desc.Width != color_desc.Width || staging_desc.Height != color_desc.Height ||
        staging_desc.Format != expected_format || staging_desc.Usage != D3D11_USAGE_STAGING ||
        staging_desc.BindFlags != 0 || staging_desc.CPUAccessFlags != D3D11_CPU_ACCESS_READ ||
        staging_desc.MiscFlags != 0) {
        d3d11_log_hresult("invalid rtt staging descriptor", E_INVALIDARG);
        staging_failed = 1;
        goto cleanup;
    }
    if (!vgfx3d_rendertarget_ensure_color(target))
        return 0;
    if (vgfx3d_rendertarget_is_hdr(target) && !vgfx3d_rendertarget_ensure_hdr_color(target))
        return 0;

    restore_target = ctx->current_target_kind == VGFX3D_D3D11_TARGET_RTT ? 1 : 0;
    if (restore_target)
        d3d11_unbind_output_targets(ctx);

    ID3D11DeviceContext_CopyResource(
        ctx->ctx, (ID3D11Resource *)ctx->rtt_staging, (ID3D11Resource *)ctx->rtt_color_tex);
    hr = ID3D11DeviceContext_Map(
        ctx->ctx, (ID3D11Resource *)ctx->rtt_staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        d3d11_log_hresult("Map(rttStaging)", hr);
        d3d11_log_device_removed_reason(ctx, "Map(rttStaging)", hr);
        staging_failed = 1;
        goto cleanup;
    }
    mapped_ok = 1;
    if (!mapped.pData) {
        staging_failed = 1;
        goto cleanup;
    }

    if (vgfx3d_rendertarget_is_hdr(target)) {
        size_t hdr_row_bytes;
        size_t rgba_row_bytes;
        if (target->width > INT32_MAX / 4)
            goto cleanup;
        if (!vgfx3d_d3d11_compute_row_bytes(target->width, 8, &hdr_row_bytes) ||
            !vgfx3d_d3d11_compute_row_bytes(target->width, 4, &rgba_row_bytes))
            goto cleanup;
        if (mapped.RowPitch > (UINT)INT32_MAX || mapped.RowPitch < hdr_row_bytes) {
            d3d11_log_hresult("invalid rtt staging payload", E_INVALIDARG);
            staging_failed = 1;
            goto cleanup;
        }
        if ((size_t)target->stride < rgba_row_bytes)
            goto cleanup;
        vgfx3d_copy_linear_rgba16f_to_rgba8(target->color_buf,
                                            target->stride,
                                            target->width,
                                            target->height,
                                            (const uint16_t *)mapped.pData,
                                            (int32_t)mapped.RowPitch);
        vgfx3d_copy_linear_rgba16f_to_rgba32f(target->hdr_color_buf,
                                              target->width * 4,
                                              target->width,
                                              target->height,
                                              (const uint16_t *)mapped.pData,
                                              (int32_t)mapped.RowPitch);
        target->hdr_color_valid = 1;
        ok = 1;
    } else {
        size_t row_bytes;
        if (!vgfx3d_d3d11_compute_row_bytes(target->width, 4, &row_bytes) ||
            (size_t)target->stride < row_bytes)
            goto cleanup;
        if (mapped.RowPitch < row_bytes) {
            d3d11_log_hresult("invalid rtt staging payload", E_INVALIDARG);
            staging_failed = 1;
            goto cleanup;
        }
        for (int32_t y = 0; y < target->height; y++) {
            memcpy(&target->color_buf[(size_t)y * (size_t)target->stride],
                   (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch,
                   (size_t)row_bytes);
        }
        target->hdr_color_valid = 0;
        ok = 1;
    }

cleanup:
    if (mapped_ok)
        ID3D11DeviceContext_Unmap(ctx->ctx, (ID3D11Resource *)ctx->rtt_staging, 0);
    if (restore_target)
        d3d11_restore_current_target_bindings(ctx);
    if (staging_failed) {
        SAFE_RELEASE(ctx->rtt_staging);
        if (ctx->device && vgfx3d_d3d11_is_valid_texture2d_extent(target->width, target->height)) {
            HRESULT recreate_hr = d3d11_create_staging_texture(
                ctx, target->width, target->height, expected_format, &ctx->rtt_staging);
            if (FAILED(recreate_hr)) {
                d3d11_log_hresult("CreateTexture2D(rttStagingRecovery)", recreate_hr);
                d3d11_log_device_removed_reason(
                    ctx, "CreateTexture2D(rttStagingRecovery)", recreate_hr);
            }
        }
    }
    if (ok)
        target->color_dirty = 0;
    return ok;
}

/// @brief Release every offscreen render-to-texture (RTT) resource.
///
/// `rtt_target` is the user-visible RT object — we set it to NULL so
/// the next bind cycle doesn't think the targets are still live.
static void d3d11_destroy_rtt_targets(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->rtt_target) {
        int sync_ok = 1;
        if (ctx->rtt_target->color_dirty)
            sync_ok = d3d11_sync_render_target_color(ctx, ctx->rtt_target);
        if (sync_ok || !ctx->rtt_target->color_dirty)
            vgfx3d_rendertarget_clear_sync(ctx->rtt_target);
        else
            vgfx3d_rendertarget_detach_sync_preserve_dirty(ctx->rtt_target);
    }
    if (ctx->current_target_kind == VGFX3D_D3D11_TARGET_RTT) {
        d3d11_unbind_output_targets(ctx);
        d3d11_clear_current_target_bindings(ctx);
    }
    SAFE_RELEASE(ctx->rtt_staging);
    SAFE_RELEASE(ctx->rtt_dsv);
    SAFE_RELEASE(ctx->rtt_depth_tex);
    SAFE_RELEASE(ctx->rtt_rtv);
    SAFE_RELEASE(ctx->rtt_color_tex);
    d3d11_destroy_opaque_depth_target(ctx);
    d3d11_reset_depth_probe_state(ctx);
    ctx->rtt_width = 0;
    ctx->rtt_height = 0;
    ctx->rtt_color_format = (int32_t)VGFX3D_RENDERTARGET_COLOR_FORMAT_UNORM8;
    ctx->rtt_active = 0;
    ctx->rtt_target = NULL;
    if (ctx->active_target_kind == VGFX3D_D3D11_TARGET_RTT)
        ctx->active_target_kind = VGFX3D_D3D11_TARGET_SWAPCHAIN;
    ctx->current_pass_is_overlay = 0;
    ctx->current_load_existing_color = 0;
    ctx->overlay_used_this_frame = 0;
}

/// @brief Allocate RTT color + depth + staging textures sized to `rt`.
///
/// Sets `rtt_active` so subsequent bind calls route to the RTT instead
/// of the swapchain. The staging texture is for CPU readback.
static HRESULT d3d11_ensure_rtt_targets(d3d11_context_t *ctx, vgfx3d_rendertarget_t *rt) {
    HRESULT hr;
    vgfx3d_d3d11_color_format_t color_format;
    DXGI_FORMAT staging_format;
    ID3D11Texture2D *new_color_tex = NULL;
    ID3D11RenderTargetView *new_rtv = NULL;
    ID3D11Texture2D *new_depth_tex = NULL;
    ID3D11DepthStencilView *new_dsv = NULL;
    ID3D11Texture2D *new_staging = NULL;

    if (!ctx || !rt || !vgfx3d_d3d11_is_valid_texture2d_extent(rt->width, rt->height) ||
        !vgfx3d_d3d11_is_valid_rtt_color_format(rt->color_format))
        return E_INVALIDARG;
    if (ctx->rtt_color_tex && ctx->rtt_rtv && ctx->rtt_depth_tex && ctx->rtt_dsv &&
        ctx->rtt_staging && ctx->rtt_width == rt->width && ctx->rtt_height == rt->height &&
        ctx->rtt_color_format == rt->color_format) {
        if (!ctx->rtt_target || ctx->rtt_target == rt) {
            ctx->rtt_active = 1;
            ctx->rtt_target = rt;
            ctx->current_pass_is_overlay = 0;
            ctx->current_load_existing_color = 0;
            ctx->overlay_used_this_frame = 0;
            rt->sync_color = d3d11_sync_render_target_color;
            rt->sync_color_userdata = ctx;
            return S_OK;
        }
    }

    color_format = vgfx3d_rendertarget_is_hdr(rt) ? VGFX3D_D3D11_COLOR_FORMAT_HDR16F
                                                  : VGFX3D_D3D11_COLOR_FORMAT_UNORM8;
    staging_format = vgfx3d_rendertarget_is_hdr(rt) ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                    : DXGI_FORMAT_R8G8B8A8_UNORM;

    hr = d3d11_create_color_target(
        ctx, rt->width, rt->height, color_format, &new_color_tex, &new_rtv, NULL);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_depth_target(ctx, rt->width, rt->height, 1, &new_depth_tex, &new_dsv, NULL);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_staging_texture(ctx, rt->width, rt->height, staging_format, &new_staging);
    if (FAILED(hr) || !new_staging) {
        if (SUCCEEDED(hr))
            hr = E_POINTER;
        goto fail;
    }

    if (ctx->rtt_target && ctx->rtt_target->color_dirty &&
        !d3d11_sync_render_target_color(ctx, ctx->rtt_target)) {
        hr = E_FAIL;
        goto fail;
    }
    d3d11_destroy_rtt_targets(ctx);
    ctx->rtt_color_tex = new_color_tex;
    ctx->rtt_rtv = new_rtv;
    ctx->rtt_depth_tex = new_depth_tex;
    ctx->rtt_dsv = new_dsv;
    ctx->rtt_staging = new_staging;
    ctx->rtt_width = rt->width;
    ctx->rtt_height = rt->height;
    ctx->rtt_color_format = rt->color_format;
    ctx->rtt_active = 1;
    ctx->rtt_target = rt;
    ctx->current_pass_is_overlay = 0;
    ctx->current_load_existing_color = 0;
    ctx->overlay_used_this_frame = 0;
    rt->color_dirty = 0;
    rt->sync_color = d3d11_sync_render_target_color;
    rt->sync_color_userdata = ctx;
    return S_OK;

fail:
    SAFE_RELEASE(new_staging);
    SAFE_RELEASE(new_dsv);
    SAFE_RELEASE(new_depth_tex);
    SAFE_RELEASE(new_rtv);
    SAFE_RELEASE(new_color_tex);
    return hr;
}

/// @brief Release shadow-map depth target resources.
static void d3d11_destroy_shadow_targets(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    d3d11_unbind_shadow_resources(ctx);
    if (ctx->shadow_pass_slot >= 0) {
        d3d11_unbind_output_targets(ctx);
        ctx->shadow_pass_slot = -1;
    }
    for (int32_t slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++) {
        SAFE_RELEASE(ctx->shadow_srv[slot]);
        SAFE_RELEASE(ctx->shadow_dsv[slot]);
        SAFE_RELEASE(ctx->shadow_depth_tex[slot]);
        ctx->shadow_width[slot] = 0;
        ctx->shadow_height[slot] = 0;
        ctx->shadow_slot_complete[slot] = 0;
    }
    SAFE_RELEASE(ctx->shadow_atlas_srv);
    SAFE_RELEASE(ctx->shadow_atlas_dsv);
    SAFE_RELEASE(ctx->shadow_atlas_tex);
    ctx->shadow_atlas_w = 0;
    ctx->shadow_atlas_h = 0;
    ctx->shadow_atlas_cleared = 0;
    ctx->shadow_pass_slot = -1;
    ctx->shadow_pass_failed = 0;
    ctx->shadow_count = 0;
}

/// @brief Release one shadow-map slot and detach it from any active bindings.
/// @details Clears all shadow SRVs first because D3D11 forbids releasing/rebinding
///   a depth texture while a shader-resource view of the same texture is still
///   visible to the pixel shader. If the slot is currently being rendered, the
///   output-merger is also detached and the active shadow-pass marker is cleared.
static void d3d11_release_shadow_slot(d3d11_context_t *ctx, int32_t slot) {
    if (!ctx || slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS)
        return;
    d3d11_unbind_shadow_resources(ctx);
    if (ctx->shadow_pass_slot == slot) {
        d3d11_unbind_output_targets(ctx);
        ctx->shadow_pass_slot = -1;
    }
    SAFE_RELEASE(ctx->shadow_srv[slot]);
    SAFE_RELEASE(ctx->shadow_dsv[slot]);
    SAFE_RELEASE(ctx->shadow_depth_tex[slot]);
    ctx->shadow_width[slot] = 0;
    ctx->shadow_height[slot] = 0;
    ctx->shadow_slot_complete[slot] = 0;
    d3d11_recompute_shadow_count(ctx);
}

/// @brief Recompute the highest contiguous shadow slot advertised to shaders.
/// @details The shader receives `shadowCount` and validates each light's
///   `shadowIndex` against it. Recomputing after allocation failures prevents a
///   stale count from making an empty slot sample a NULL SRV.
static void d3d11_recompute_shadow_count(d3d11_context_t *ctx) {
    int complete[VGFX3D_MAX_SHADOW_LIGHTS] = {0};

    if (!ctx)
        return;
    for (int32_t slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++) {
        if (slot < VGFX3D_CSM_SLOTS) {
            if (ctx->shadow_srv[slot] && ctx->shadow_dsv[slot] && ctx->shadow_depth_tex[slot] &&
                ctx->shadow_slot_complete[slot])
                complete[slot] = 1;
        } else if (ctx->shadow_atlas_srv && ctx->shadow_slot_complete[slot]) {
            /* Atlas tiles have no per-slot resources; completeness is stamped
             * by shadow_end after the tile pass renders. */
            complete[slot] = 1;
        }
    }
    ctx->shadow_count = vgfx3d_d3d11_compute_shadow_count(VGFX3D_MAX_SHADOW_LIGHTS, complete);
}

/// @brief Allocate the shadow-map depth target at the requested size.
///
/// Always shader-readable so the main pass can sample shadows.
static HRESULT d3d11_ensure_shadow_targets(d3d11_context_t *ctx,
                                           int32_t slot,
                                           int32_t width,
                                           int32_t height) {
    HRESULT hr;
    ID3D11Texture2D *new_tex = NULL;
    ID3D11DepthStencilView *new_dsv = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;

    if (!ctx || slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS ||
        !vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return E_INVALIDARG;
    if (ctx->shadow_depth_tex[slot] && ctx->shadow_dsv[slot] && ctx->shadow_srv[slot] &&
        ctx->shadow_width[slot] == width && ctx->shadow_height[slot] == height)
        return S_OK;

    hr = d3d11_create_depth_target(ctx, width, height, 1, &new_tex, &new_dsv, &new_srv);
    if (FAILED(hr))
        return hr;
    d3d11_release_shadow_slot(ctx, slot);
    ctx->shadow_depth_tex[slot] = new_tex;
    ctx->shadow_dsv[slot] = new_dsv;
    ctx->shadow_srv[slot] = new_srv;
    ctx->shadow_width[slot] = width;
    ctx->shadow_height[slot] = height;
    /* Allocation alone is not completeness: shadow_end advertises the slot only
     * after its pass finishes without a CPU-side submission failure. */
    d3d11_recompute_shadow_count(ctx);
    return S_OK;
}

/// @brief Allocate (or resize) the shared shadow atlas for slots >= VGFX3D_CSM_SLOTS.
/// @details One depth target holding the fixed tile grid at the per-slot resolution;
///   the main pass samples it at t17 through static per-tile UV rects.
static HRESULT d3d11_ensure_shadow_atlas(d3d11_context_t *ctx, int32_t tile_w, int32_t tile_h) {
    HRESULT hr;
    int32_t atlas_w;
    int32_t atlas_h;
    ID3D11Texture2D *new_tex = NULL;
    ID3D11DepthStencilView *new_dsv = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;

    if (!ctx || tile_w <= 0 || tile_h <= 0 ||
        tile_w > INT32_MAX / VGFX3D_D3D11_SHADOW_ATLAS_COLUMNS ||
        tile_h > INT32_MAX / VGFX3D_D3D11_SHADOW_ATLAS_ROWS)
        return E_INVALIDARG;
    atlas_w = tile_w * VGFX3D_D3D11_SHADOW_ATLAS_COLUMNS;
    atlas_h = tile_h * VGFX3D_D3D11_SHADOW_ATLAS_ROWS;
    if (!vgfx3d_d3d11_is_valid_texture2d_extent(atlas_w, atlas_h))
        return E_INVALIDARG;
    if (ctx->shadow_atlas_tex && ctx->shadow_atlas_dsv && ctx->shadow_atlas_srv &&
        ctx->shadow_atlas_w == atlas_w && ctx->shadow_atlas_h == atlas_h)
        return S_OK;

    hr = d3d11_create_depth_target(ctx, atlas_w, atlas_h, 1, &new_tex, &new_dsv, &new_srv);
    if (FAILED(hr))
        return hr;
    d3d11_unbind_shadow_resources(ctx);
    if (ctx->shadow_pass_slot >= VGFX3D_CSM_SLOTS) {
        d3d11_unbind_output_targets(ctx);
        ctx->shadow_pass_slot = -1;
    }
    SAFE_RELEASE(ctx->shadow_atlas_srv);
    SAFE_RELEASE(ctx->shadow_atlas_dsv);
    SAFE_RELEASE(ctx->shadow_atlas_tex);
    ctx->shadow_atlas_tex = new_tex;
    ctx->shadow_atlas_dsv = new_dsv;
    ctx->shadow_atlas_srv = new_srv;
    ctx->shadow_atlas_w = atlas_w;
    ctx->shadow_atlas_h = atlas_h;
    ctx->shadow_atlas_cleared = 0;
    for (int32_t slot = VGFX3D_CSM_SLOTS; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++)
        ctx->shadow_slot_complete[slot] = 0;
    d3d11_recompute_shadow_count(ctx);
    return S_OK;
}
