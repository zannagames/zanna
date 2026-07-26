//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_context.inc
// Purpose: D3D11 device/context creation, shader compilation, state objects, and teardown.
// Key invariants:
//   - Every required shader and input layout is created before a context is published.
//   - Particle input slot 1 is four float4 lanes with a 64-byte per-instance stride.
// Ownership/Lifetime:
//   - Temporary blobs are released after context creation; context COM resources survive to
//   destroy.
//   - The including vgfx3d_backend_d3d11.c translation unit owns all declarations used here.
// Links: vgfx3d_backend_d3d11.c, vgfx3d_backend_d3d11_shaders.inc
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements D3D11 backend context creation, fixed state, and teardown.
///
/// The routines in this fragment compile every required shader, create the
/// matching input layouts and fixed-function state, allocate common buffers and
/// fallback resources, publish a context only after full initialization, and
/// release partial or complete contexts in dependency-safe order.
///
/// @note Included by `vgfx3d_backend_d3d11.c`; not a standalone translation unit.

/// @brief Temporary compiled HLSL bytecode retained only while a context is being built.
typedef struct d3d11_shader_blobs_t {
    /// Main vertex shader bytecode.
    ID3DBlob *vs;
    /// Instanced vertex shader bytecode.
    ID3DBlob *vs_instanced;
    /// Compact particle-instance vertex shader bytecode.
    ID3DBlob *vs_particles;
    /// Main material pixel shader bytecode.
    ID3DBlob *ps;
    /// Shadow-pass vertex shader bytecode.
    ID3DBlob *vs_shadow;
    /// Shadow-pass alpha-mask pixel shader bytecode.
    ID3DBlob *ps_shadow;
    /// Skybox vertex shader bytecode.
    ID3DBlob *vs_skybox;
    /// Skybox pixel shader bytecode.
    ID3DBlob *ps_skybox;
    /// Fullscreen post-processing vertex shader bytecode.
    ID3DBlob *vs_postfx;
    /// Main post-processing pixel shader bytecode.
    ID3DBlob *ps_postfx;
    /// Final overlay composite pixel shader bytecode.
    ID3DBlob *ps_overlay;
    /// Bloom downsample pixel shader bytecode.
    ID3DBlob *ps_bloom_down;
    /// Bloom upsample pixel shader bytecode.
    ID3DBlob *ps_bloom_up;
    /// Temporal anti-aliasing resolve pixel shader bytecode.
    ID3DBlob *ps_taa;
    /// Screen-space reflection pixel shader bytecode.
    ID3DBlob *ps_ssr;
} d3d11_shader_blobs_t;

/// @brief Release all temporary shader blobs used while creating a context.
/// @param[in,out] blobs Blob bundle whose COM references are released and cleared.
static void d3d11_release_shader_blobs(d3d11_shader_blobs_t *blobs) {
    SAFE_RELEASE(blobs->vs);
    SAFE_RELEASE(blobs->vs_instanced);
    SAFE_RELEASE(blobs->vs_particles);
    SAFE_RELEASE(blobs->ps);
    SAFE_RELEASE(blobs->vs_shadow);
    SAFE_RELEASE(blobs->ps_shadow);
    SAFE_RELEASE(blobs->vs_skybox);
    SAFE_RELEASE(blobs->ps_skybox);
    SAFE_RELEASE(blobs->vs_postfx);
    SAFE_RELEASE(blobs->ps_postfx);
    SAFE_RELEASE(blobs->ps_overlay);
    SAFE_RELEASE(blobs->ps_bloom_down);
    SAFE_RELEASE(blobs->ps_bloom_up);
    SAFE_RELEASE(blobs->ps_taa);
    SAFE_RELEASE(blobs->ps_ssr);
}

/// @brief Initialize matrix defaults and target/upload state on a new D3D11 context.
/// @param[out] ctx Zero-initialized backend context receiving deterministic defaults.
/// @param[in] width Initial drawable width.
/// @param[in] height Initial drawable height.
static void d3d11_init_context_defaults(d3d11_context_t *ctx, int32_t width, int32_t height) {
    ctx->width = width;
    ctx->height = height;
    ctx->render_scale = 1.0f;
    memcpy(ctx->view, k_identity4x4, sizeof(ctx->view));
    memcpy(ctx->projection, k_identity4x4, sizeof(ctx->projection));
    memcpy(ctx->vp, k_identity4x4, sizeof(ctx->vp));
    memcpy(ctx->inv_vp, k_identity4x4, sizeof(ctx->inv_vp));
    memcpy(ctx->draw_prev_vp, k_identity4x4, sizeof(ctx->draw_prev_vp));
    memcpy(ctx->scene_vp, k_identity4x4, sizeof(ctx->scene_vp));
    memcpy(ctx->scene_prev_vp, k_identity4x4, sizeof(ctx->scene_prev_vp));
    memcpy(ctx->scene_inv_vp, k_identity4x4, sizeof(ctx->scene_inv_vp));
    ctx->current_target_kind = VGFX3D_D3D11_TARGET_SWAPCHAIN;
    ctx->active_target_kind = VGFX3D_D3D11_TARGET_SWAPCHAIN;
    ctx->texture_upload_budget_bytes = UINT64_MAX;
    ctx->present_sync_interval = 1; /* vsync on by default */
}

/// @brief Fill the swap-chain descriptor for the current window/backbuffer size.
/// @param[out] swap_desc Descriptor initialized for one windowed RGBA8 backbuffer.
/// @param[in] hwnd Native window receiving the swap chain.
/// @param[in] width Backbuffer width in pixels.
/// @param[in] height Backbuffer height in pixels.
static void d3d11_fill_swap_chain_desc(DXGI_SWAP_CHAIN_DESC *swap_desc,
                                       HWND hwnd,
                                       int32_t width,
                                       int32_t height) {
    memset(swap_desc, 0, sizeof(*swap_desc));
    swap_desc->BufferCount = 1;
    swap_desc->BufferDesc.Width = (UINT)width;
    swap_desc->BufferDesc.Height = (UINT)height;
    swap_desc->BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    /* Windowed presentation follows the desktop/output refresh rate. Advertising 60 Hz here
     * can make DXGI choose a mismatched mode on high-refresh and variable-refresh displays. */
    swap_desc->BufferDesc.RefreshRate.Numerator = 0;
    swap_desc->BufferDesc.RefreshRate.Denominator = 1;
    swap_desc->BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc->OutputWindow = hwnd;
    swap_desc->SampleDesc.Count = 1;
    swap_desc->Windowed = TRUE;
}

/// @brief Keep fullscreen transitions under Canvas/window-system ownership.
/// @details DXGI otherwise installs an implicit Alt+Enter handler that can change swapchain mode
///   behind the backend's target-size and lifetime tracking. Failure is non-fatal because some
///   remoting/composition environments do not expose a parent factory.
/// @param[in] ctx Backend context owning the swap chain.
/// @param[in] hwnd Native window whose automatic fullscreen transition is disabled.
static void d3d11_disable_implicit_fullscreen(d3d11_context_t *ctx, HWND hwnd) {
    /* Keep the interface id in the adapter instead of referencing IID_IDXGIFactory from
     * dxguid.lib. Zanna's native-link pipeline does not consume MSVC #pragma comment records. */
    static const GUID factory_iid = {
        0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
    IDXGIFactory *factory = NULL;
    HRESULT hr;

    if (!ctx || !ctx->swap_chain || !hwnd)
        return;
    hr = IDXGISwapChain_GetParent(ctx->swap_chain, &factory_iid, (void **)&factory);
    hr = d3d11_required_output_result(hr, factory);
    if (FAILED(hr)) {
        d3d11_log_hresult("IDXGISwapChain::GetParent(factory)", hr);
        SAFE_RELEASE(factory);
        return;
    }
    hr = IDXGIFactory_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr))
        d3d11_log_hresult("IDXGIFactory::MakeWindowAssociation", hr);
    SAFE_RELEASE(factory);
}

/// @brief Create the swapchain render-target view and matching main depth target.
/// @param[in,out] ctx Backend context receiving the initial targets.
/// @param[in] width Target width in pixels.
/// @param[in] height Target height in pixels.
/// @return S_OK on success, otherwise the resource-creation HRESULT.
static HRESULT d3d11_create_swapchain_targets(d3d11_context_t *ctx, int32_t width, int32_t height) {
    return d3d11_recreate_swapchain_main_targets(
        ctx, width, height, "Create initial swapchain targets");
}

/// @brief Create opaque, transparent, disabled, skybox, and shadow depth-stencil states.
/// @details Reversed-Z: scene passes clear depth to 0 and compare Greater. The shadow
///   pass keeps a dedicated standard Less state because light view-projections stay in
///   the standard convention (their maps are sampled with LessEqual comparisons).
/// @param[in,out] ctx Backend context receiving the depth-stencil states.
/// @return S_OK when every required state exists, otherwise the first creation HRESULT.
static HRESULT d3d11_create_context_depth_states(d3d11_context_t *ctx) {
    D3D11_DEPTH_STENCIL_DESC depth_desc;
    HRESULT hr;
    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_GREATER;
    hr = ID3D11Device_CreateDepthStencilState(ctx->device, &depth_desc, &ctx->depth_state);
    hr = d3d11_required_output_result(hr, ctx->depth_state);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateDepthStencilState(opaque)", hr);
        return hr;
    }
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    hr = ID3D11Device_CreateDepthStencilState(ctx->device, &depth_desc, &ctx->depth_state_no_write);
    hr = d3d11_required_output_result(hr, ctx->depth_state_no_write);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateDepthStencilState(transparent)", hr);
        return hr;
    }
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    hr = ID3D11Device_CreateDepthStencilState(ctx->device, &depth_desc, &ctx->depth_state_disabled);
    hr = d3d11_required_output_result(hr, ctx->depth_state_disabled);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateDepthStencilState(disabled)", hr);
        return hr;
    }
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    hr = ID3D11Device_CreateDepthStencilState(ctx->device, &depth_desc, &ctx->depth_state_skybox);
    hr = d3d11_required_output_result(hr, ctx->depth_state_skybox);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateDepthStencilState(skybox)", hr);
        return hr;
    }
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_LESS;
    hr = ID3D11Device_CreateDepthStencilState(ctx->device, &depth_desc, &ctx->depth_state_shadow);
    hr = d3d11_required_output_result(hr, ctx->depth_state_shadow);
    if (FAILED(hr))
        d3d11_log_hresult("CreateDepthStencilState(shadow)", hr);
    return hr;
}

/// @brief Create opaque, alpha, additive, and premultiplied-alpha blend states.
/// @param[in,out] ctx Backend context receiving the blend states.
/// @return S_OK when every required state exists, otherwise the first creation HRESULT.
static HRESULT d3d11_create_context_blend_states(d3d11_context_t *ctx) {
    D3D11_BLEND_DESC blend_desc;
    HRESULT hr;
    memset(&blend_desc, 0, sizeof(blend_desc));
    blend_desc.IndependentBlendEnable = TRUE;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blend_desc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = ID3D11Device_CreateBlendState(ctx->device, &blend_desc, &ctx->blend_state_opaque);
    hr = d3d11_required_output_result(hr, ctx->blend_state_opaque);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBlendState(opaque)", hr);
        return hr;
    }

    memset(&blend_desc, 0, sizeof(blend_desc));
    blend_desc.IndependentBlendEnable = TRUE;
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blend_desc.RenderTarget[1].BlendEnable = FALSE;
    blend_desc.RenderTarget[1].RenderTargetWriteMask = 0;
    hr = ID3D11Device_CreateBlendState(ctx->device, &blend_desc, &ctx->blend_state_alpha);
    hr = d3d11_required_output_result(hr, ctx->blend_state_alpha);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBlendState(alpha)", hr);
        return hr;
    }

    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    hr = ID3D11Device_CreateBlendState(ctx->device, &blend_desc, &ctx->blend_state_additive);
    hr = d3d11_required_output_result(hr, ctx->blend_state_additive);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBlendState(additive)", hr);
        return hr;
    }

    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    hr = ID3D11Device_CreateBlendState(
        ctx->device, &blend_desc, &ctx->blend_state_premultiplied_alpha);
    hr = d3d11_required_output_result(hr, ctx->blend_state_premultiplied_alpha);
    if (FAILED(hr))
        d3d11_log_hresult("CreateBlendState(premultiplied alpha)", hr);
    return hr;
}

/// @brief Create all solid/wireframe and cull/no-cull rasterizer-state combinations.
/// @param[in,out] ctx Backend context receiving the rasterizer states.
/// @return S_OK when every required state exists, otherwise the first creation HRESULT.
static HRESULT d3d11_create_context_rasterizer_states(d3d11_context_t *ctx) {
    HRESULT hr =
        d3d11_create_rasterizer_state(ctx, D3D11_FILL_SOLID, D3D11_CULL_BACK, &ctx->rs_solid_cull);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateRasterizerState(solid+cull)", hr);
        return hr;
    }
    hr = d3d11_create_rasterizer_state(
        ctx, D3D11_FILL_SOLID, D3D11_CULL_NONE, &ctx->rs_solid_no_cull);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateRasterizerState(solid+nocull)", hr);
        return hr;
    }
    hr = d3d11_create_rasterizer_state(
        ctx, D3D11_FILL_WIREFRAME, D3D11_CULL_BACK, &ctx->rs_wire_cull);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateRasterizerState(wire+cull)", hr);
        return hr;
    }
    hr = d3d11_create_rasterizer_state(
        ctx, D3D11_FILL_WIREFRAME, D3D11_CULL_NONE, &ctx->rs_wire_no_cull);
    if (FAILED(hr))
        d3d11_log_hresult("CreateRasterizerState(wire+nocull)", hr);
    return hr;
}

/// @brief Create default linear wrap/clamp samplers and the shadow comparison sampler.
/// @param[in,out] ctx Backend context receiving the sampler states.
/// @return S_OK when every required sampler exists, otherwise the first creation HRESULT.
static HRESULT d3d11_create_context_sampler_states(d3d11_context_t *ctx) {
    D3D11_SAMPLER_DESC sampler_desc;
    HRESULT hr;
    d3d11_init_sampler_desc_defaults(&sampler_desc);
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    hr = ID3D11Device_CreateSamplerState(ctx->device, &sampler_desc, &ctx->linear_wrap_sampler);
    hr = d3d11_required_output_result(hr, ctx->linear_wrap_sampler);
    if (SUCCEEDED(hr) && !d3d11_sampler_desc_matches(ctx->linear_wrap_sampler, &sampler_desc))
        hr = E_FAIL;
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateSamplerState(linearWrap)", hr);
        SAFE_RELEASE(ctx->linear_wrap_sampler);
        return hr;
    }
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = ID3D11Device_CreateSamplerState(ctx->device, &sampler_desc, &ctx->linear_clamp_sampler);
    hr = d3d11_required_output_result(hr, ctx->linear_clamp_sampler);
    if (SUCCEEDED(hr) && !d3d11_sampler_desc_matches(ctx->linear_clamp_sampler, &sampler_desc))
        hr = E_FAIL;
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateSamplerState(linearClamp)", hr);
        SAFE_RELEASE(ctx->linear_clamp_sampler);
        return hr;
    }
    d3d11_init_sampler_desc_defaults(&sampler_desc);
    sampler_desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler_desc.BorderColor[0] = 1.0f;
    sampler_desc.BorderColor[1] = 1.0f;
    sampler_desc.BorderColor[2] = 1.0f;
    sampler_desc.BorderColor[3] = 1.0f;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = ID3D11Device_CreateSamplerState(ctx->device, &sampler_desc, &ctx->shadow_cmp_sampler);
    hr = d3d11_required_output_result(hr, ctx->shadow_cmp_sampler);
    if (SUCCEEDED(hr) && !d3d11_sampler_desc_matches(ctx->shadow_cmp_sampler, &sampler_desc))
        hr = E_FAIL;
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateSamplerState(shadowCmp)", hr);
        SAFE_RELEASE(ctx->shadow_cmp_sampler);
    }
    return hr;
}

/// @brief Compile every HLSL shader entry point needed by the context.
/// @param[out] blobs Temporary blob bundle populated in dependency order.
/// @return S_OK when every shader compiles, otherwise the first compiler HRESULT.
static HRESULT d3d11_compile_context_shaders(d3d11_shader_blobs_t *blobs) {
    HRESULT hr;
    memset(blobs, 0, sizeof(*blobs));
    hr = d3d11_compile_shader(d3d11_shader_source, "VSMain", "vs_5_0", &blobs->vs);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(
        d3d11_shader_source, "VSMainInstanced", "vs_5_0", &blobs->vs_instanced);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(
        d3d11_shader_source, "VSMainParticles", "vs_5_0", &blobs->vs_particles);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(d3d11_shader_source, "PSMain", "ps_5_0", &blobs->ps);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(d3d11_shader_source, "VSShadow", "vs_5_0", &blobs->vs_shadow);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(d3d11_shader_source, "PSShadow", "ps_5_0", &blobs->ps_shadow);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(d3d11_skybox_shader_source, "VSSkybox", "vs_5_0", &blobs->vs_skybox);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(d3d11_skybox_shader_source, "PSSkybox", "ps_5_0", &blobs->ps_skybox);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(d3d11_postfx_shader_source, "VSPostFX", "vs_5_0", &blobs->vs_postfx);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(d3d11_postfx_shader_source, "PSPostFX", "ps_5_0", &blobs->ps_postfx);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(
        d3d11_postfx_shader_source, "PSOverlayComposite", "ps_5_0", &blobs->ps_overlay);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(
        d3d11_bloom_shader_source, "PSBloomDown", "ps_5_0", &blobs->ps_bloom_down);
    if (FAILED(hr))
        return hr;
    hr =
        d3d11_compile_shader(d3d11_bloom_shader_source, "PSBloomUp", "ps_5_0", &blobs->ps_bloom_up);
    if (FAILED(hr))
        return hr;
    hr = d3d11_compile_shader(d3d11_taa_shader_source, "PSTAAResolve", "ps_5_0", &blobs->ps_taa);
    if (FAILED(hr))
        return hr;
    return d3d11_compile_shader(d3d11_ssr_shader_source, "PSSSR", "ps_5_0", &blobs->ps_ssr);
}

/// @brief Create all shader objects from compiled HLSL blobs.
/// @param[in,out] ctx Backend context receiving the shader COM objects.
/// @param[in] blobs Complete compiled-bytecode bundle.
/// @return S_OK when every shader object exists, otherwise the first creation HRESULT.
static HRESULT d3d11_create_context_shader_objects(d3d11_context_t *ctx,
                                                   const d3d11_shader_blobs_t *blobs) {
    HRESULT hr = ID3D11Device_CreateVertexShader(ctx->device,
                                                 ID3D10Blob_GetBufferPointer(blobs->vs),
                                                 ID3D10Blob_GetBufferSize(blobs->vs),
                                                 NULL,
                                                 &ctx->vs_main);
    hr = d3d11_required_output_result(hr, ctx->vs_main);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateVertexShader(main)", hr);
        return hr;
    }
    hr = ID3D11Device_CreateVertexShader(ctx->device,
                                         ID3D10Blob_GetBufferPointer(blobs->vs_instanced),
                                         ID3D10Blob_GetBufferSize(blobs->vs_instanced),
                                         NULL,
                                         &ctx->vs_instanced);
    hr = d3d11_required_output_result(hr, ctx->vs_instanced);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateVertexShader(instanced)", hr);
        return hr;
    }
    hr = ID3D11Device_CreateVertexShader(ctx->device,
                                         ID3D10Blob_GetBufferPointer(blobs->vs_particles),
                                         ID3D10Blob_GetBufferSize(blobs->vs_particles),
                                         NULL,
                                         &ctx->vs_particles);
    hr = d3d11_required_output_result(hr, ctx->vs_particles);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateVertexShader(particles)", hr);
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(ctx->device,
                                        ID3D10Blob_GetBufferPointer(blobs->ps),
                                        ID3D10Blob_GetBufferSize(blobs->ps),
                                        NULL,
                                        &ctx->ps_main);
    hr = d3d11_required_output_result(hr, ctx->ps_main);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreatePixelShader(main)", hr);
        return hr;
    }
    hr = ID3D11Device_CreateVertexShader(ctx->device,
                                         ID3D10Blob_GetBufferPointer(blobs->vs_shadow),
                                         ID3D10Blob_GetBufferSize(blobs->vs_shadow),
                                         NULL,
                                         &ctx->vs_shadow);
    hr = d3d11_required_output_result(hr, ctx->vs_shadow);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateVertexShader(shadow)", hr);
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(ctx->device,
                                        ID3D10Blob_GetBufferPointer(blobs->ps_shadow),
                                        ID3D10Blob_GetBufferSize(blobs->ps_shadow),
                                        NULL,
                                        &ctx->ps_shadow);
    hr = d3d11_required_output_result(hr, ctx->ps_shadow);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreatePixelShader(shadow)", hr);
        return hr;
    }
    hr = ID3D11Device_CreateVertexShader(ctx->device,
                                         ID3D10Blob_GetBufferPointer(blobs->vs_skybox),
                                         ID3D10Blob_GetBufferSize(blobs->vs_skybox),
                                         NULL,
                                         &ctx->vs_skybox);
    hr = d3d11_required_output_result(hr, ctx->vs_skybox);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateVertexShader(skybox)", hr);
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(ctx->device,
                                        ID3D10Blob_GetBufferPointer(blobs->ps_skybox),
                                        ID3D10Blob_GetBufferSize(blobs->ps_skybox),
                                        NULL,
                                        &ctx->ps_skybox);
    hr = d3d11_required_output_result(hr, ctx->ps_skybox);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreatePixelShader(skybox)", hr);
        return hr;
    }
    hr = ID3D11Device_CreateVertexShader(ctx->device,
                                         ID3D10Blob_GetBufferPointer(blobs->vs_postfx),
                                         ID3D10Blob_GetBufferSize(blobs->vs_postfx),
                                         NULL,
                                         &ctx->vs_postfx);
    hr = d3d11_required_output_result(hr, ctx->vs_postfx);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateVertexShader(postfx)", hr);
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(ctx->device,
                                        ID3D10Blob_GetBufferPointer(blobs->ps_postfx),
                                        ID3D10Blob_GetBufferSize(blobs->ps_postfx),
                                        NULL,
                                        &ctx->ps_postfx);
    hr = d3d11_required_output_result(hr, ctx->ps_postfx);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreatePixelShader(postfx)", hr);
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(ctx->device,
                                        ID3D10Blob_GetBufferPointer(blobs->ps_overlay),
                                        ID3D10Blob_GetBufferSize(blobs->ps_overlay),
                                        NULL,
                                        &ctx->ps_overlay_composite);
    hr = d3d11_required_output_result(hr, ctx->ps_overlay_composite);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreatePixelShader(overlayComposite)", hr);
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(ctx->device,
                                        ID3D10Blob_GetBufferPointer(blobs->ps_bloom_down),
                                        ID3D10Blob_GetBufferSize(blobs->ps_bloom_down),
                                        NULL,
                                        &ctx->ps_bloom_down);
    hr = d3d11_required_output_result(hr, ctx->ps_bloom_down);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreatePixelShader(bloomDown)", hr);
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(ctx->device,
                                        ID3D10Blob_GetBufferPointer(blobs->ps_bloom_up),
                                        ID3D10Blob_GetBufferSize(blobs->ps_bloom_up),
                                        NULL,
                                        &ctx->ps_bloom_up);
    hr = d3d11_required_output_result(hr, ctx->ps_bloom_up);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreatePixelShader(bloomUp)", hr);
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(ctx->device,
                                        ID3D10Blob_GetBufferPointer(blobs->ps_taa),
                                        ID3D10Blob_GetBufferSize(blobs->ps_taa),
                                        NULL,
                                        &ctx->ps_taa);
    hr = d3d11_required_output_result(hr, ctx->ps_taa);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreatePixelShader(taaResolve)", hr);
        return hr;
    }
    hr = ID3D11Device_CreatePixelShader(ctx->device,
                                        ID3D10Blob_GetBufferPointer(blobs->ps_ssr),
                                        ID3D10Blob_GetBufferSize(blobs->ps_ssr),
                                        NULL,
                                        &ctx->ps_ssr);
    hr = d3d11_required_output_result(hr, ctx->ps_ssr);
    if (FAILED(hr))
        d3d11_log_hresult("CreatePixelShader(ssr)", hr);
    return hr;
}

/// @brief Fill the base per-vertex input layout for vgfx3d_vertex_t.
/// @param[out] layout Eight descriptors matching the full-precision vertex structure.
static void d3d11_fill_vertex_layout(D3D11_INPUT_ELEMENT_DESC layout[8]) {
    layout[0].SemanticName = "POSITION";
    layout[0].SemanticIndex = 0;
    layout[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    layout[0].InputSlot = 0;
    layout[0].AlignedByteOffset = (UINT)offsetof(vgfx3d_vertex_t, pos);
    layout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    layout[0].InstanceDataStepRate = 0;
    layout[1] = layout[0];
    layout[1].SemanticName = "NORMAL";
    layout[1].AlignedByteOffset = (UINT)offsetof(vgfx3d_vertex_t, normal);
    layout[2] = layout[0];
    layout[2].SemanticName = "TEXCOORD";
    layout[2].Format = DXGI_FORMAT_R32G32_FLOAT;
    layout[2].AlignedByteOffset = (UINT)offsetof(vgfx3d_vertex_t, uv);
    layout[3] = layout[0];
    layout[3].SemanticName = "TEXCOORD";
    layout[3].SemanticIndex = 1;
    layout[3].Format = DXGI_FORMAT_R32G32_FLOAT;
    layout[3].AlignedByteOffset = (UINT)offsetof(vgfx3d_vertex_t, uv1);
    layout[4] = layout[0];
    layout[4].SemanticName = "COLOR";
    layout[4].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    layout[4].AlignedByteOffset = (UINT)offsetof(vgfx3d_vertex_t, color);
    layout[5] = layout[0];
    layout[5].SemanticName = "TANGENT";
    layout[5].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    layout[5].AlignedByteOffset = (UINT)offsetof(vgfx3d_vertex_t, tangent);
    layout[6] = layout[0];
    layout[6].SemanticName = "BLENDINDICES";
    layout[6].Format = DXGI_FORMAT_R8G8B8A8_UINT;
    layout[6].AlignedByteOffset = (UINT)offsetof(vgfx3d_vertex_t, bone_indices);
    layout[7] = layout[0];
    layout[7].SemanticName = "BLENDWEIGHT";
    layout[7].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    layout[7].AlignedByteOffset = (UINT)offsetof(vgfx3d_vertex_t, bone_weights);
}

/// @brief Fill the compact 48-byte per-vertex input layout (R20).
/// @details Same semantics and slots as d3d11_fill_vertex_layout; only formats and
///   offsets differ. Fixed-function input-assembler conversion decodes the packed
///   fields to the float shader inputs, so the same VS blobs validate both layouts.
/// @param[out] layout Eight descriptors matching VGFX3D_COMPACT_VERTEX_STRIDE.
static void d3d11_fill_vertex_layout_compact(D3D11_INPUT_ELEMENT_DESC layout[8]) {
    layout[0].SemanticName = "POSITION";
    layout[0].SemanticIndex = 0;
    layout[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    layout[0].InputSlot = 0;
    layout[0].AlignedByteOffset = 0;
    layout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    layout[0].InstanceDataStepRate = 0;
    layout[1] = layout[0];
    layout[1].SemanticName = "NORMAL";
    layout[1].Format = DXGI_FORMAT_R16G16B16A16_SNORM;
    layout[1].AlignedByteOffset = 12;
    layout[2] = layout[0];
    layout[2].SemanticName = "TEXCOORD";
    layout[2].Format = DXGI_FORMAT_R16G16_FLOAT;
    layout[2].AlignedByteOffset = 20;
    layout[3] = layout[0];
    layout[3].SemanticName = "TEXCOORD";
    layout[3].SemanticIndex = 1;
    layout[3].Format = DXGI_FORMAT_R16G16_FLOAT;
    layout[3].AlignedByteOffset = 24;
    layout[4] = layout[0];
    layout[4].SemanticName = "COLOR";
    layout[4].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    layout[4].AlignedByteOffset = 28;
    layout[5] = layout[0];
    layout[5].SemanticName = "TANGENT";
    layout[5].Format = DXGI_FORMAT_R16G16B16A16_SNORM;
    layout[5].AlignedByteOffset = 32;
    layout[6] = layout[0];
    layout[6].SemanticName = "BLENDINDICES";
    layout[6].Format = DXGI_FORMAT_R8G8B8A8_UINT;
    layout[6].AlignedByteOffset = 40;
    layout[7] = layout[0];
    layout[7].SemanticName = "BLENDWEIGHT";
    layout[7].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    layout[7].AlignedByteOffset = 44;
}

/// @brief Extend the base layout with model/normal/previous-model instance matrices.
/// @param[out] instanced_layout Twenty per-vertex and per-instance descriptors.
/// @param[in] layout Eight base vertex descriptors copied into the result.
static void d3d11_fill_instanced_layout(D3D11_INPUT_ELEMENT_DESC instanced_layout[20],
                                        const D3D11_INPUT_ELEMENT_DESC layout[8]) {
    memcpy(instanced_layout, layout, sizeof(D3D11_INPUT_ELEMENT_DESC) * 8u);
    instanced_layout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    int elem = 8;
    for (int row = 0; row < 4; row++, elem++) {
        instanced_layout[elem].SemanticName = "TEXCOORD";
        instanced_layout[elem].SemanticIndex = 4u + (UINT)row;
        instanced_layout[elem].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        instanced_layout[elem].InputSlot = 1;
        instanced_layout[elem].AlignedByteOffset =
            (UINT)(offsetof(d3d_instance_data_t, model) + row * 16);
        instanced_layout[elem].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
        instanced_layout[elem].InstanceDataStepRate = 1;
    }
    for (int row = 0; row < 4; row++, elem++) {
        instanced_layout[elem].SemanticName = "TEXCOORD";
        instanced_layout[elem].SemanticIndex = 8u + (UINT)row;
        instanced_layout[elem].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        instanced_layout[elem].InputSlot = 1;
        instanced_layout[elem].AlignedByteOffset =
            (UINT)(offsetof(d3d_instance_data_t, normal) + row * 16);
        instanced_layout[elem].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
        instanced_layout[elem].InstanceDataStepRate = 1;
    }
    for (int row = 0; row < 4; row++, elem++) {
        instanced_layout[elem].SemanticName = "TEXCOORD";
        instanced_layout[elem].SemanticIndex = 12u + (UINT)row;
        instanced_layout[elem].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        instanced_layout[elem].InputSlot = 1;
        instanced_layout[elem].AlignedByteOffset =
            (UINT)(offsetof(d3d_instance_data_t, prev_model) + row * 16);
        instanced_layout[elem].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
        instanced_layout[elem].InstanceDataStepRate = 1;
    }
}

/// @brief Extend the full unit-quad vertex layout with four compact particle vec4 lanes.
/// @details Slot 1 advances once per instance and maps directly onto
///   `vgfx3d_particle_instance_t`; unlike ordinary instancing it carries no model, normal, or
///   previous-model matrices.
/// @param[out] particle_layout Twelve per-vertex and compact per-instance descriptors.
/// @param[in] layout Eight full-precision base vertex descriptors.
static void d3d11_fill_particle_layout(D3D11_INPUT_ELEMENT_DESC particle_layout[12],
                                       const D3D11_INPUT_ELEMENT_DESC layout[8]) {
    memcpy(particle_layout, layout, sizeof(D3D11_INPUT_ELEMENT_DESC) * 8u);
    for (int lane = 0; lane < 4; lane++) {
        D3D11_INPUT_ELEMENT_DESC *element = &particle_layout[8 + lane];
        element->SemanticName = "TEXCOORD";
        element->SemanticIndex = 4u + (UINT)lane;
        element->Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        element->InputSlot = 1;
        element->AlignedByteOffset = (UINT)((size_t)lane * 4u * sizeof(float));
        element->InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
        element->InstanceDataStepRate = 1;
    }
}

/// @brief Create main, instanced, and skybox input layouts.
/// @param[in,out] ctx Backend context receiving required and optional compact layouts.
/// @param[in] blobs Compiled vertex shader bytecode used for signature validation.
/// @return S_OK when every required layout exists, otherwise the first creation HRESULT.
static HRESULT d3d11_create_context_input_layouts(d3d11_context_t *ctx,
                                                  const d3d11_shader_blobs_t *blobs) {
    D3D11_INPUT_ELEMENT_DESC layout[8];
    D3D11_INPUT_ELEMENT_DESC instanced_layout[20];
    D3D11_INPUT_ELEMENT_DESC particle_layout[12];
    D3D11_INPUT_ELEMENT_DESC skybox_layout[1];
    HRESULT hr;

    d3d11_fill_vertex_layout(layout);
    hr = ID3D11Device_CreateInputLayout(ctx->device,
                                        layout,
                                        8,
                                        ID3D10Blob_GetBufferPointer(blobs->vs),
                                        ID3D10Blob_GetBufferSize(blobs->vs),
                                        &ctx->input_layout);
    hr = d3d11_required_output_result(hr, ctx->input_layout);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateInputLayout(main)", hr);
        return hr;
    }

    d3d11_fill_instanced_layout(instanced_layout, layout);
    hr = ID3D11Device_CreateInputLayout(ctx->device,
                                        instanced_layout,
                                        20,
                                        ID3D10Blob_GetBufferPointer(blobs->vs_instanced),
                                        ID3D10Blob_GetBufferSize(blobs->vs_instanced),
                                        &ctx->input_layout_instanced);
    hr = d3d11_required_output_result(hr, ctx->input_layout_instanced);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateInputLayout(instanced)", hr);
        return hr;
    }

    d3d11_fill_particle_layout(particle_layout, layout);
    hr = ID3D11Device_CreateInputLayout(ctx->device,
                                        particle_layout,
                                        12,
                                        ID3D10Blob_GetBufferPointer(blobs->vs_particles),
                                        ID3D10Blob_GetBufferSize(blobs->vs_particles),
                                        &ctx->input_layout_particles);
    hr = d3d11_required_output_result(hr, ctx->input_layout_particles);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateInputLayout(particles)", hr);
        return hr;
    }

    skybox_layout[0].SemanticName = "POSITION";
    skybox_layout[0].SemanticIndex = 0;
    skybox_layout[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    skybox_layout[0].InputSlot = 0;
    skybox_layout[0].AlignedByteOffset = 0;
    skybox_layout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    skybox_layout[0].InstanceDataStepRate = 0;
    hr = ID3D11Device_CreateInputLayout(ctx->device,
                                        skybox_layout,
                                        1,
                                        ID3D10Blob_GetBufferPointer(blobs->vs_skybox),
                                        ID3D10Blob_GetBufferSize(blobs->vs_skybox),
                                        &ctx->input_layout_skybox);
    hr = d3d11_required_output_result(hr, ctx->input_layout_skybox);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateInputLayout(skybox)", hr);
        return hr;
    }

    /* R20 compact-vertex-stream layouts. Failure is non-fatal: both stay NULL
     * and the mesh cache keeps uploading the full 92-byte layout. */
    {
        D3D11_INPUT_ELEMENT_DESC compact_layout[8];
        D3D11_INPUT_ELEMENT_DESC compact_instanced_layout[20];
        HRESULT compact_hr;
        d3d11_fill_vertex_layout_compact(compact_layout);
        compact_hr = ID3D11Device_CreateInputLayout(ctx->device,
                                                    compact_layout,
                                                    8,
                                                    ID3D10Blob_GetBufferPointer(blobs->vs),
                                                    ID3D10Blob_GetBufferSize(blobs->vs),
                                                    &ctx->input_layout_compact);
        compact_hr = d3d11_required_output_result(compact_hr, ctx->input_layout_compact);
        if (FAILED(compact_hr)) {
            d3d11_log_hresult("CreateInputLayout(compact)", compact_hr);
            SAFE_RELEASE(ctx->input_layout_compact);
        }
        d3d11_fill_instanced_layout(compact_instanced_layout, compact_layout);
        compact_hr =
            ID3D11Device_CreateInputLayout(ctx->device,
                                           compact_instanced_layout,
                                           20,
                                           ID3D10Blob_GetBufferPointer(blobs->vs_instanced),
                                           ID3D10Blob_GetBufferSize(blobs->vs_instanced),
                                           &ctx->input_layout_instanced_compact);
        compact_hr = d3d11_required_output_result(compact_hr, ctx->input_layout_instanced_compact);
        if (FAILED(compact_hr)) {
            d3d11_log_hresult("CreateInputLayout(compact instanced)", compact_hr);
            SAFE_RELEASE(ctx->input_layout_instanced_compact);
        }
        if (!ctx->input_layout_compact || !ctx->input_layout_instanced_compact) {
            SAFE_RELEASE(ctx->input_layout_compact);
            SAFE_RELEASE(ctx->input_layout_instanced_compact);
        }
    }
    return S_OK;
}

/// @brief Create constant buffers and the dynamic vertex/index/instance buffers.
/// @param[in,out] ctx Backend context receiving the common buffers.
/// @return S_OK when every required buffer exists, otherwise the first creation HRESULT.
static HRESULT d3d11_create_context_buffers(d3d11_context_t *ctx) {
    HRESULT hr = d3d11_create_constant_buffer(ctx, sizeof(d3d_per_object_t), &ctx->cb_per_object);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbPerObject)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(ctx, sizeof(d3d_per_scene_t), &ctx->cb_per_scene);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbPerScene)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(ctx, sizeof(d3d_per_material_t), &ctx->cb_per_material);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbPerMaterial)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(
        ctx, sizeof(d3d_light_t) * (uint32_t)VGFX3D_MAX_LIGHTS, &ctx->cb_per_lights);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbPerLights)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(
        ctx,
        (uint32_t)((sizeof(((vgfx3d_cluster_table_t *)0)->offsets) + 15u) & ~15u),
        &ctx->cb_cluster_offsets);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbClusterOffsets)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(
        ctx,
        (uint32_t)((sizeof(((vgfx3d_cluster_table_t *)0)->indices) + 15u) & ~15u),
        &ctx->cb_cluster_indices);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbClusterIndices)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(ctx, VGFX3D_D3D11_BONE_PALETTE_BYTES, &ctx->cb_bones);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbBones)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(ctx, VGFX3D_D3D11_BONE_PALETTE_BYTES, &ctx->cb_prev_bones);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbPrevBones)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(ctx, sizeof(d3d_skybox_cb_t), &ctx->cb_skybox);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbSkybox)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(ctx, sizeof(d3d_postfx_cb_t), &ctx->cb_postfx);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbPostFX)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(ctx, sizeof(d3d_bloom_cb_t), &ctx->cb_bloom);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbBloom)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(ctx, sizeof(d3d_ssr_cb_t), &ctx->cb_ssr);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbSSR)", hr);
        return hr;
    }
    hr = d3d11_create_constant_buffer(ctx, sizeof(d3d_taa_cb_t), &ctx->cb_taa);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(cbTAA)", hr);
        return hr;
    }
    hr = d3d11_ensure_dynamic_buffer(ctx,
                                     &ctx->dynamic_vb,
                                     &ctx->dynamic_vb_size,
                                     D3D11_BIND_VERTEX_BUFFER,
                                     D3D11_INITIAL_DYNAMIC_VB_SIZE,
                                     D3D11_INITIAL_DYNAMIC_VB_SIZE);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(dynamicVB)", hr);
        return hr;
    }
    hr = d3d11_ensure_dynamic_buffer(ctx,
                                     &ctx->dynamic_ib,
                                     &ctx->dynamic_ib_size,
                                     D3D11_BIND_INDEX_BUFFER,
                                     D3D11_INITIAL_DYNAMIC_IB_SIZE,
                                     D3D11_INITIAL_DYNAMIC_IB_SIZE);
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(dynamicIB)", hr);
        return hr;
    }
    hr = d3d11_ensure_dynamic_buffer(ctx,
                                     &ctx->instance_buffer,
                                     &ctx->instance_buffer_size,
                                     D3D11_BIND_VERTEX_BUFFER,
                                     D3D11_INITIAL_INSTANCE_BUFFER_SIZE,
                                     D3D11_INITIAL_INSTANCE_BUFFER_SIZE);
    if (FAILED(hr))
        d3d11_log_hresult("CreateBuffer(instanceBuffer)", hr);
    return hr;
}

/// @brief Create the immutable fullscreen-triangle skybox vertex buffer.
/// @param[in,out] ctx Backend context receiving the immutable vertex buffer.
/// @return S_OK on success, otherwise the D3D11 creation HRESULT.
static HRESULT d3d11_create_context_skybox_buffer(d3d11_context_t *ctx) {
    static const float skybox_vertices[] = {
        -1.0f,
        -1.0f,
        1.0f,
        3.0f,
        -1.0f,
        1.0f,
        -1.0f,
        3.0f,
        1.0f,
    };
    D3D11_BUFFER_DESC skybox_desc;
    D3D11_SUBRESOURCE_DATA skybox_init;
    memset(&skybox_desc, 0, sizeof(skybox_desc));
    skybox_desc.Usage = D3D11_USAGE_IMMUTABLE;
    skybox_desc.ByteWidth = sizeof(skybox_vertices);
    skybox_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&skybox_init, 0, sizeof(skybox_init));
    skybox_init.pSysMem = skybox_vertices;
    HRESULT hr =
        ID3D11Device_CreateBuffer(ctx->device, &skybox_desc, &skybox_init, &ctx->skybox_vb);
    hr = d3d11_required_output_result(hr, ctx->skybox_vb);
    if (SUCCEEDED(hr) && !d3d11_buffer_desc_matches(ctx->skybox_vb, &skybox_desc))
        hr = E_FAIL;
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateBuffer(skyboxVB)", hr);
        SAFE_RELEASE(ctx->skybox_vb);
    }
    return hr;
}

/// @brief Release every partial core interface produced by one device-creation attempt.
/// @param[in,out] ctx Backend context whose immediate context, swap chain, and device are cleared.
static void d3d11_release_device_attempt(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    SAFE_RELEASE(ctx->ctx);
    SAFE_RELEASE(ctx->swap_chain);
    SAFE_RELEASE(ctx->device);
}

/// @brief Attempt one feature-level-11 device/swapchain creation transaction.
/// @param[in,out] ctx Backend context receiving the created core interfaces.
/// @param[in] driver_type Hardware or fallback D3D11 driver type to try.
/// @param[in] swap_desc Prepared windowed swap-chain descriptor.
/// @return S_OK for a complete feature-level-11 context, otherwise the creation HRESULT.
static HRESULT d3d11_try_create_device(d3d11_context_t *ctx,
                                       D3D_DRIVER_TYPE driver_type,
                                       DXGI_SWAP_CHAIN_DESC *swap_desc) {
    const D3D_FEATURE_LEVEL requested_feature_level = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL created_feature_level = (D3D_FEATURE_LEVEL)0;
    HRESULT hr;

    if (!ctx || !swap_desc)
        return E_INVALIDARG;
    d3d11_release_device_attempt(ctx);
    hr = D3D11CreateDeviceAndSwapChain(NULL,
                                       driver_type,
                                       NULL,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       &requested_feature_level,
                                       1,
                                       D3D11_SDK_VERSION,
                                       swap_desc,
                                       &ctx->swap_chain,
                                       &ctx->device,
                                       &created_feature_level,
                                       &ctx->ctx);
    if (SUCCEEDED(hr) && (!ctx->swap_chain || !ctx->device || !ctx->ctx))
        hr = E_POINTER;
    if (SUCCEEDED(hr) && created_feature_level != requested_feature_level)
        hr = E_FAIL;
    if (FAILED(hr))
        d3d11_release_device_attempt(ctx);
    return hr;
}

/// @brief Allocate and initialize the entire D3D11 backend context.
///
/// The big one — creates the device + swap chain, compiles every
/// shader (main VS/PS, instanced VS, shadow VS, skybox VS/PS, postfx
/// VS/PS, overlay PS), creates input layouts, depth/blend/sampler
/// states, all four rasterizer-state combos, all cbuffers, and the
/// skybox cube vertex buffer. Returns NULL on any device-create
/// failure (the host falls back to the software backend in that case).
/// @param[in] win Platform window whose native HWND and framebuffer are used.
/// @param[in] width Requested initial width, overridden by a valid framebuffer extent.
/// @param[in] height Requested initial height, overridden by a valid framebuffer extent.
/// @return Fully initialized opaque D3D11 context, or NULL to request software fallback.
static void *d3d11_create_ctx(vgfx_window_t win, int32_t width, int32_t height) {
    d3d11_context_t *ctx = NULL;
    HWND hwnd = (HWND)vgfx_get_native_view(win);
    vgfx_framebuffer_t fb;
    DXGI_SWAP_CHAIN_DESC swap_desc;
    d3d11_shader_blobs_t blobs;
    HRESULT hr;
    memset(&blobs, 0, sizeof(blobs));

    if (!hwnd)
        return NULL;
    if (vgfx_get_framebuffer(win, &fb) && fb.width > 0 && fb.height > 0) {
        width = fb.width;
        height = fb.height;
    }
    if (width <= 0)
        width = 1;
    if (height <= 0)
        height = 1;
    if (!vgfx3d_d3d11_is_valid_texture2d_extent(width, height))
        return NULL;

    ctx = (d3d11_context_t *)calloc(1, sizeof(d3d11_context_t));
    if (!ctx)
        return NULL;
    d3d11_init_context_defaults(ctx, width, height);
    d3d11_fill_swap_chain_desc(&swap_desc, hwnd, width, height);

    hr = d3d11_try_create_device(ctx, D3D_DRIVER_TYPE_HARDWARE, &swap_desc);
    if (FAILED(hr)) {
        d3d11_log_hresult("D3D11CreateDeviceAndSwapChain(hardware)", hr);
        hr = d3d11_try_create_device(ctx, D3D_DRIVER_TYPE_WARP, &swap_desc);
    }
    if (FAILED(hr)) {
        d3d11_log_hresult("D3D11CreateDeviceAndSwapChain(WARP)", hr);
        d3d11_destroy_ctx(ctx);
        return NULL;
    }
    d3d11_disable_implicit_fullscreen(ctx, hwnd);

    hr = d3d11_create_swapchain_targets(ctx, width, height);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_context_depth_states(ctx);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_context_blend_states(ctx);
    if (FAILED(hr))
        goto fail;

    hr = d3d11_create_context_rasterizer_states(ctx);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_context_sampler_states(ctx);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_white_fallback_resources(ctx);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_compile_context_shaders(&blobs);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_context_shader_objects(ctx, &blobs);
    if (FAILED(hr))
        goto fail;

    hr = d3d11_create_context_input_layouts(ctx, &blobs);
    if (FAILED(hr))
        goto fail;

    hr = d3d11_create_context_buffers(ctx);
    if (FAILED(hr))
        goto fail;
    hr = d3d11_create_context_skybox_buffer(ctx);
    if (FAILED(hr))
        goto fail;

    d3d11_create_frame_timing_queries(ctx);
    d3d11_release_shader_blobs(&blobs);
    return ctx;

fail:
    d3d11_release_shader_blobs(&blobs);
    d3d11_destroy_ctx(ctx);
    return NULL;
}

/// @brief Tear down the D3D11 backend context — releases everything created in `_create_ctx`.
///
/// Strict reverse-order release: caches first (so they don't reference
/// the device after it's gone), then framebuffer + offscreen targets,
/// then dynamic buffers, cbuffers, input layouts, shaders, sampler
/// states, rasterizer states, depth-stencil states, blend states,
/// finally swap chain + device. Frees the wrapper struct last.
/// @param[in,out] ctx_ptr Opaque D3D11 context to destroy; NULL is accepted.
static void d3d11_destroy_ctx(void *ctx_ptr) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    if (!ctx)
        return;

    if (ctx->ctx) {
        if (ctx->frame_time_active)
            d3d11_end_frame_timing(ctx);
        d3d11_unbind_draw_resources(ctx);
        d3d11_unbind_shadow_resources(ctx);
        d3d11_unbind_output_targets(ctx);
        ID3D11DeviceContext_ClearState(ctx->ctx);
        ID3D11DeviceContext_Flush(ctx->ctx);
    }

    d3d11_release_texture_cache(ctx);
    d3d11_release_cubemap_cache(ctx);
    d3d11_release_mesh_cache(ctx);
    d3d11_release_morph_cache(ctx);
    d3d11_destroy_shadow_targets(ctx);
    d3d11_destroy_rtt_targets(ctx);
    d3d11_destroy_scene_targets(ctx);
    vgfx3d_postfx_chain_free(&ctx->gpu_postfx_chain);

    SAFE_RELEASE(ctx->frame_time_end_query);
    SAFE_RELEASE(ctx->frame_time_start_query);
    SAFE_RELEASE(ctx->frame_time_disjoint_query);
    SAFE_RELEASE(ctx->depth_probe_staging);
    SAFE_RELEASE(ctx->morph_normal_srv);
    SAFE_RELEASE(ctx->morph_normal_buffer);
    SAFE_RELEASE(ctx->morph_srv);
    SAFE_RELEASE(ctx->morph_buffer);
    SAFE_RELEASE(ctx->skybox_vb);
    SAFE_RELEASE(ctx->instance_buffer);
    SAFE_RELEASE(ctx->dynamic_ib);
    SAFE_RELEASE(ctx->dynamic_vb);
    SAFE_RELEASE(ctx->cb_postfx);
    SAFE_RELEASE(ctx->cb_skybox);
    SAFE_RELEASE(ctx->cb_prev_bones);
    SAFE_RELEASE(ctx->cb_bones);
    SAFE_RELEASE(ctx->cb_per_lights);
    SAFE_RELEASE(ctx->cb_cluster_offsets);
    SAFE_RELEASE(ctx->cb_cluster_indices);
    d3d11_destroy_opaque_depth_target(ctx);
    SAFE_RELEASE(ctx->cb_per_material);
    SAFE_RELEASE(ctx->cb_per_scene);
    SAFE_RELEASE(ctx->cb_per_object);
    SAFE_RELEASE(ctx->input_layout_skybox);
    SAFE_RELEASE(ctx->input_layout_particles);
    SAFE_RELEASE(ctx->input_layout_instanced);
    SAFE_RELEASE(ctx->input_layout);
    SAFE_RELEASE(ctx->input_layout_compact);
    SAFE_RELEASE(ctx->input_layout_instanced_compact);
    SAFE_RELEASE(ctx->ps_postfx);
    SAFE_RELEASE(ctx->ps_overlay_composite);
    SAFE_RELEASE(ctx->ps_bloom_down);
    SAFE_RELEASE(ctx->ps_bloom_up);
    SAFE_RELEASE(ctx->ps_taa);
    SAFE_RELEASE(ctx->ps_ssr);
    SAFE_RELEASE(ctx->cb_bloom);
    SAFE_RELEASE(ctx->cb_taa);
    SAFE_RELEASE(ctx->cb_ssr);
    SAFE_RELEASE(ctx->vs_postfx);
    SAFE_RELEASE(ctx->ps_skybox);
    SAFE_RELEASE(ctx->vs_skybox);
    SAFE_RELEASE(ctx->ps_shadow);
    SAFE_RELEASE(ctx->vs_shadow);
    SAFE_RELEASE(ctx->ps_main);
    SAFE_RELEASE(ctx->vs_instanced);
    SAFE_RELEASE(ctx->vs_particles);
    SAFE_RELEASE(ctx->vs_main);
    for (int ws = 0; ws < 3; ws++) {
        for (int wt = 0; wt < 3; wt++) {
            for (int filter = 0; filter < 12; filter++) {
                for (int anisotropy = 0; anisotropy < VGFX3D_D3D11_ANISOTROPY_LEVEL_COUNT;
                     anisotropy++)
                    SAFE_RELEASE(ctx->material_samplers[ws][wt][filter][anisotropy]);
            }
        }
    }
    SAFE_RELEASE(ctx->shadow_cmp_sampler);
    SAFE_RELEASE(ctx->linear_clamp_sampler);
    SAFE_RELEASE(ctx->linear_wrap_sampler);
    SAFE_RELEASE(ctx->fallback_white_cube_srv);
    SAFE_RELEASE(ctx->fallback_white_cube_tex);
    SAFE_RELEASE(ctx->brdf_lut_srv);
    SAFE_RELEASE(ctx->brdf_lut_tex);
    SAFE_RELEASE(ctx->fallback_white_srv);
    SAFE_RELEASE(ctx->fallback_white_tex);
    SAFE_RELEASE(ctx->rs_wire_no_cull);
    SAFE_RELEASE(ctx->rs_wire_cull);
    SAFE_RELEASE(ctx->rs_solid_no_cull);
    SAFE_RELEASE(ctx->rs_solid_cull);
    SAFE_RELEASE(ctx->rs_depth_biased_cached);
    ctx->rs_depth_biased_valid = 0;
    SAFE_RELEASE(ctx->depth_state_skybox);
    SAFE_RELEASE(ctx->depth_state_shadow);
    SAFE_RELEASE(ctx->depth_state_disabled);
    SAFE_RELEASE(ctx->depth_state_no_write);
    SAFE_RELEASE(ctx->depth_state);
    SAFE_RELEASE(ctx->blend_state_premultiplied_alpha);
    SAFE_RELEASE(ctx->blend_state_additive);
    SAFE_RELEASE(ctx->blend_state_alpha);
    SAFE_RELEASE(ctx->blend_state_opaque);
    SAFE_RELEASE(ctx->dsv);
    SAFE_RELEASE(ctx->depth_tex);
    SAFE_RELEASE(ctx->rtv);
    SAFE_RELEASE(ctx->swap_chain);
    SAFE_RELEASE(ctx->ctx);
    SAFE_RELEASE(ctx->device);
    free(ctx->instance_upload_data);
    free(ctx);
}
