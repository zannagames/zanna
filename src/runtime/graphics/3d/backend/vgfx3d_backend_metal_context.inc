//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_metal_context.inc
// Purpose: Metal pipeline/state creation, shader loading, context lifecycle, and frame begin.
// Key invariants:
//   - All mandatory target, blend, instancing, and particle pipeline variants publish atomically.
//   - Particle pipelines read compact records at buffer index 6 and retained vertices at index 0.
// Ownership/Lifetime:
//   - VGFXMetalContext strongly owns created Objective-C resources until context destruction.
//   - MTLFunction temporaries remain valid through pipeline creation in the including translation
//   unit.
// Links: vgfx3d_backend_metal.m, vgfx3d_backend_metal_shaders.inc
//
//===----------------------------------------------------------------------===//

/// @file vgfx3d_backend_metal_context.inc
/// @brief Implements Metal context creation, pipeline and fixed-state construction,
///   frame initialization, and backend capability reporting.
/// @details Objective-C resources created here are strongly retained by
///   `VGFXMetalContext`. Context creation assembles mandatory state before publishing
///   the opaque backend handle, while optional compact pipelines fail back to full vertices.

/// @brief Build the MTLVertexDescriptor describing the interleaved 92-byte vgfx3d_vertex_t layout
///   (a single per-vertex buffer at index 0) that every render pipeline consumes.
/// @return A newly-allocated descriptor owned by the caller.
static MTLVertexDescriptor *create_vertex_descriptor(void) {
    MTLVertexDescriptor *d = [[MTLVertexDescriptor alloc] init];
    d.attributes[0].format = MTLVertexFormatFloat3;
    d.attributes[0].offset = 0;
    d.attributes[0].bufferIndex = 0;
    d.attributes[1].format = MTLVertexFormatFloat3;
    d.attributes[1].offset = 12;
    d.attributes[1].bufferIndex = 0;
    d.attributes[2].format = MTLVertexFormatFloat2;
    d.attributes[2].offset = 24;
    d.attributes[2].bufferIndex = 0;
    d.attributes[7].format = MTLVertexFormatFloat2;
    d.attributes[7].offset = 32;
    d.attributes[7].bufferIndex = 0;
    d.attributes[3].format = MTLVertexFormatFloat4;
    d.attributes[3].offset = 40;
    d.attributes[3].bufferIndex = 0;
    d.attributes[4].format = MTLVertexFormatFloat4;
    d.attributes[4].offset = 56;
    d.attributes[4].bufferIndex = 0;
    d.attributes[5].format = MTLVertexFormatUChar4;
    d.attributes[5].offset = 72;
    d.attributes[5].bufferIndex = 0;
    d.attributes[6].format = MTLVertexFormatFloat4;
    d.attributes[6].offset = 76;
    d.attributes[6].bufferIndex = 0;
    d.layouts[0].stride = 92;
    d.layouts[0].stepRate = 1;
    d.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    return d;
}

/// @brief Build the MTLVertexDescriptor for the compact 48-byte static-cache vertex stream
///   (R20). Attribute slots match create_vertex_descriptor(); fixed-function format
///   conversion decodes the packed fields to the same float shader inputs, so every vertex
///   function is shared between the full and compact pipeline variants.
/// @return A newly allocated descriptor owned by the caller.
static MTLVertexDescriptor *create_vertex_descriptor_compact(void) {
    MTLVertexDescriptor *d = [[MTLVertexDescriptor alloc] init];
    d.attributes[0].format = MTLVertexFormatFloat3; /* position */
    d.attributes[0].offset = 0;
    d.attributes[0].bufferIndex = 0;
    d.attributes[1].format = MTLVertexFormatShort4Normalized; /* normal (w unused) */
    d.attributes[1].offset = 12;
    d.attributes[1].bufferIndex = 0;
    d.attributes[2].format = MTLVertexFormatHalf2; /* uv */
    d.attributes[2].offset = 20;
    d.attributes[2].bufferIndex = 0;
    d.attributes[7].format = MTLVertexFormatHalf2; /* uv1 */
    d.attributes[7].offset = 24;
    d.attributes[7].bufferIndex = 0;
    d.attributes[3].format = MTLVertexFormatUChar4Normalized; /* color */
    d.attributes[3].offset = 28;
    d.attributes[3].bufferIndex = 0;
    d.attributes[4].format = MTLVertexFormatShort4Normalized; /* tangent (+w handedness) */
    d.attributes[4].offset = 32;
    d.attributes[4].bufferIndex = 0;
    d.attributes[5].format = MTLVertexFormatUChar4; /* bone indices */
    d.attributes[5].offset = 40;
    d.attributes[5].bufferIndex = 0;
    d.attributes[6].format = MTLVertexFormatUChar4Normalized; /* bone weights */
    d.attributes[6].offset = 44;
    d.attributes[6].bufferIndex = 0;
    d.layouts[0].stride = VGFX3D_COMPACT_VERTEX_STRIDE;
    d.layouts[0].stepRate = 1;
    d.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    return d;
}

/// @brief Configure a pipeline color attachment's blend state for a material alpha mode.
/// @details Sets the blend factors/equation for opaque, alpha-blend, or additive modes (and enables
///          or disables blending accordingly).
/// @param[in,out] attachment Color-attachment descriptor to configure.
/// @param[in] blend_mode Sanitized opaque, alpha, or additive mode.
static void metal_configure_blend_state(MTLRenderPipelineColorAttachmentDescriptor *attachment,
                                        vgfx3d_metal_blend_mode_t blend_mode) {
    if (!attachment)
        return;
    attachment.blendingEnabled = blend_mode != VGFX3D_METAL_BLEND_OPAQUE;
    if (blend_mode == VGFX3D_METAL_BLEND_ADDITIVE) {
        attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        attachment.destinationRGBBlendFactor = MTLBlendFactorOne;
        attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
        attachment.destinationAlphaBlendFactor = MTLBlendFactorOne;
        return;
    }
    attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
    attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
}

/// @brief Compile a render pipeline state for a given vertex/fragment function and blend/format
/// config.
/// @param[in] device Metal device that creates the pipeline.
/// @param[in] vertex_function Required vertex entry point.
/// @param[in] fragment_function Required fragment entry point.
/// @param[in] vertex_descriptor Vertex layout, or `nil` for generated vertices.
/// @param[in] color0_format Primary color attachment format.
/// @param[in] blend_mode Blend configuration for the primary attachment.
/// @param[in] disable_motion_writes Nonzero to mask all writes to attachment one.
/// @param[out] error Optional receiver for pipeline compilation diagnostics.
/// @return The pipeline state, or nil on compilation failure.
static id<MTLRenderPipelineState> metal_create_pipeline_state(
    id<MTLDevice> device,
    id<MTLFunction> vertex_function,
    id<MTLFunction> fragment_function,
    MTLVertexDescriptor *vertex_descriptor,
    MTLPixelFormat color0_format,
    vgfx3d_metal_blend_mode_t blend_mode,
    BOOL disable_motion_writes,
    NSError **error) {
    MTLRenderPipelineDescriptor *descriptor;

    if (!device || !vertex_function || !fragment_function)
        return nil;

    descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex_function;
    descriptor.fragmentFunction = fragment_function;
    descriptor.vertexDescriptor = vertex_descriptor;
    descriptor.colorAttachments[0].pixelFormat = color0_format;
    descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatBGRA8Unorm;
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    metal_configure_blend_state(descriptor.colorAttachments[0], blend_mode);
    descriptor.colorAttachments[1].writeMask =
        disable_motion_writes ? MTLColorWriteMaskNone : MTLColorWriteMaskAll;
    return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
}

/// @brief Compile the skybox render pipeline state (full-screen triangle sampling a cubemap).
/// @param[in] device Metal device that creates the pipeline.
/// @param[in] vertex_function Skybox vertex entry point.
/// @param[in] fragment_function Skybox fragment entry point.
/// @param[in] color0_format Destination color attachment format.
/// @param[out] error Optional receiver for pipeline compilation diagnostics.
/// @return Newly created pipeline state, or `nil` on failure.
static id<MTLRenderPipelineState> metal_create_skybox_pipeline_state(
    id<MTLDevice> device,
    id<MTLFunction> vertex_function,
    id<MTLFunction> fragment_function,
    MTLPixelFormat color0_format,
    NSError **error) {
    MTLRenderPipelineDescriptor *descriptor;

    if (!device || !vertex_function || !fragment_function)
        return nil;

    descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex_function;
    descriptor.fragmentFunction = fragment_function;
    descriptor.vertexDescriptor = nil;
    descriptor.colorAttachments[0].pixelFormat = color0_format;
    descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatBGRA8Unorm;
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    descriptor.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
    return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
}

/// @brief Get or build the cached pipeline state matching a draw's shading model, blend, and
/// formats.
/// @details Keyed by the render-state config so identical draws reuse one compiled pipeline.
/// @param[in] ctx Context whose active target is inspected.
/// @return `YES` when the target uses RGBA16F scene or RTT color; otherwise `NO`.
static BOOL metal_target_uses_hdr_color(VGFXMetalContext *ctx) {
    if (!ctx)
        return NO;
    if (ctx.currentTargetKind == VGFX3D_METAL_TARGET_SCENE)
        return YES;
    return ctx.currentTargetKind == VGFX3D_METAL_TARGET_RTT && ctx.rttColorTexture &&
                   ctx.rttColorTexture.pixelFormat == MTLPixelFormatRGBA16Float
               ? YES
               : NO;
}

/// @brief Select the material pipeline whose attachment formats match the current target.
/// @param[in] ctx Context containing pipeline variants and the active target.
/// @param[in] cmd Optional draw command controlling blend and compact layout.
/// @param[in] instanced Nonzero to select an instanced vertex function.
/// @return Matching pipeline state, or `nil` when the context is unavailable.
static id<MTLRenderPipelineState> metal_select_pipeline_state(VGFXMetalContext *ctx,
                                                              const vgfx3d_draw_cmd_t *cmd,
                                                              BOOL instanced) {
    vgfx3d_metal_blend_mode_t blend_mode;
    BOOL compact;

    if (!ctx)
        return nil;
    blend_mode = vgfx3d_metal_choose_blend_mode(cmd);
    /* R20: compact-encoded cache buffers need the packed vertex descriptor. The
     * flag is only ever set for static-cache draws, and the geometry cache
     * consults the same readiness gate, so buffer encoding and pipeline layout
     * always agree. */
    compact = (cmd && cmd->compact_vertex_stream && ctx.compactPipelinesReady) ? YES : NO;
    if (metal_target_uses_hdr_color(ctx)) {
        if (instanced) {
            if (compact)
                return blend_mode == VGFX3D_METAL_BLEND_ALPHA
                           ? ctx.instancedPipelineStateAlphaCompact
                           : (blend_mode == VGFX3D_METAL_BLEND_ADDITIVE
                                  ? ctx.instancedPipelineStateAdditiveCompact
                                  : ctx.instancedPipelineStateCompact);
            return blend_mode == VGFX3D_METAL_BLEND_ALPHA
                       ? ctx.instancedPipelineStateAlpha
                       : (blend_mode == VGFX3D_METAL_BLEND_ADDITIVE
                              ? ctx.instancedPipelineStateAdditive
                              : ctx.instancedPipelineState);
        }
        if (compact)
            return blend_mode == VGFX3D_METAL_BLEND_ALPHA
                       ? ctx.pipelineStateAlphaCompact
                       : (blend_mode == VGFX3D_METAL_BLEND_ADDITIVE
                              ? ctx.pipelineStateAdditiveCompact
                              : ctx.pipelineStateCompact);
        return blend_mode == VGFX3D_METAL_BLEND_ALPHA
                   ? ctx.pipelineStateAlpha
                   : (blend_mode == VGFX3D_METAL_BLEND_ADDITIVE ? ctx.pipelineStateAdditive
                                                                : ctx.pipelineState);
    }

    if (instanced) {
        if (compact)
            return blend_mode == VGFX3D_METAL_BLEND_ALPHA
                       ? ctx.instancedPipelineStateColorOnlyAlphaCompact
                       : (blend_mode == VGFX3D_METAL_BLEND_ADDITIVE
                              ? ctx.instancedPipelineStateColorOnlyAdditiveCompact
                              : ctx.instancedPipelineStateColorOnlyCompact);
        return blend_mode == VGFX3D_METAL_BLEND_ALPHA
                   ? ctx.instancedPipelineStateColorOnlyAlpha
                   : (blend_mode == VGFX3D_METAL_BLEND_ADDITIVE
                          ? ctx.instancedPipelineStateColorOnlyAdditive
                          : ctx.instancedPipelineStateColorOnly);
    }
    if (compact)
        return blend_mode == VGFX3D_METAL_BLEND_ALPHA
                   ? ctx.pipelineStateColorOnlyAlphaCompact
                   : (blend_mode == VGFX3D_METAL_BLEND_ADDITIVE
                          ? ctx.pipelineStateColorOnlyAdditiveCompact
                          : ctx.pipelineStateColorOnlyCompact);
    return blend_mode == VGFX3D_METAL_BLEND_ALPHA
               ? ctx.pipelineStateColorOnlyAlpha
               : (blend_mode == VGFX3D_METAL_BLEND_ADDITIVE ? ctx.pipelineStateColorOnlyAdditive
                                                            : ctx.pipelineStateColorOnly);
}

/// @brief Select the compact-particle pipeline for the active target and blend equation.
/// @details Particles are always transparent; additive emitters select the additive twin and all
///   other emitters select standard alpha. The unit-quad stream uses the full retained vertex
///   layout, while per-particle data arrives independently at buffer index 6.
/// @param[in] ctx Context containing particle pipeline variants.
/// @param[in] cmd Draw command whose blend mode is inspected.
/// @return Pipeline matching target format and additive/alpha blending, or `nil`.
static id<MTLRenderPipelineState> metal_select_particle_pipeline_state(
    VGFXMetalContext *ctx, const vgfx3d_draw_cmd_t *cmd) {
    BOOL additive;
    if (!ctx)
        return nil;
    additive = vgfx3d_metal_choose_blend_mode(cmd) == VGFX3D_METAL_BLEND_ADDITIVE;
    if (metal_target_uses_hdr_color(ctx))
        return additive ? ctx.particlePipelineStateAdditive : ctx.particlePipelineStateAlpha;
    return additive ? ctx.particlePipelineStateColorOnlyAdditive
                    : ctx.particlePipelineStateColorOnlyAlpha;
}

/// @brief Map a material texture-wrap mode to the corresponding MTLSamplerAddressMode.
/// @param[in] mode Runtime material wrap-mode value.
/// @return Clamp-to-edge, mirror-repeat, or repeat Metal address mode.
static MTLSamplerAddressMode metal_material_address_mode(int32_t mode) {
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE)
        return MTLSamplerAddressModeClampToEdge;
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
        return MTLSamplerAddressModeMirrorRepeat;
    return MTLSamplerAddressModeRepeat;
}

/// @brief Sanitize one imported nearest/linear texel-filter axis.
/// @param value Candidate `RT_MATERIAL3D_TEXTURE_FILTER_*` value.
/// @return Nearest for the explicit nearest constant, otherwise linear.
static int32_t metal_material_texel_filter(int32_t value) {
    return value == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                                                         : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
}

/// @brief Sanitize one imported three-state mip-selection axis.
/// @param value Candidate `RT_MATERIAL3D_TEXTURE_MIP_FILTER_*` value.
/// @return None, nearest, or linear; unknown values become none.
static int32_t metal_material_mip_filter(int32_t value) {
    if (value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST)
        return RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST;
    if (value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR)
        return RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR;
    return RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE;
}

/// @brief Map a sanitized mip-selection axis to Metal's exact sampler enum.
/// @param value Sanitized `RT_MATERIAL3D_TEXTURE_MIP_FILTER_*` value.
/// @return The corresponding Metal mip filter, including the non-mipmapped state.
static MTLSamplerMipFilter metal_material_mip_filter_mode(int32_t value) {
    if (value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST)
        return MTLSamplerMipFilterNearest;
    if (value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR)
        return MTLSamplerMipFilterLinear;
    return MTLSamplerMipFilterNotMipmapped;
}

/// @brief Get or build a cached sampler preserving independent min, mag, mip, and wrap axes.
/// @details The cache key contains every authored axis plus effective anisotropy. Anisotropic
///          sampling remains an explicit material opt-in and is enabled only when both texel
///          axes are linear, so it never overrides an authored nearest choice.
/// @param ctx Active Metal backend context owning the sampler cache.
/// @param cmd Borrowed immutable draw command; NULL selects the shared/default sampler.
/// @param slot Material texture slot, or a value outside the slot range for global mirrors.
/// @return Cached or newly created sampler state; falls back to the shared/default sampler.
static id<MTLSamplerState> metal_get_material_sampler(VGFXMetalContext *ctx,
                                                      const vgfx3d_draw_cmd_t *cmd,
                                                      int32_t slot) {
    int use_slot = cmd && slot >= 0 && slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT;
    if (!ctx)
        return nil;
    if (!cmd || !ctx.samplerCache)
        return ctx.sharedSampler ? ctx.sharedSampler : ctx.defaultSampler;

    int32_t wrap_s = use_slot ? cmd->texture_slot_wrap_s[slot] : cmd->texture_wrap_s;
    int32_t wrap_t = use_slot ? cmd->texture_slot_wrap_t[slot] : cmd->texture_wrap_t;
    int32_t min_filter = metal_material_texel_filter(use_slot ? cmd->texture_slot_min_filter[slot]
                                                              : cmd->texture_min_filter);
    int32_t mag_filter = metal_material_texel_filter(use_slot ? cmd->texture_slot_mag_filter[slot]
                                                              : cmd->texture_mag_filter);
    int32_t mip_filter = metal_material_mip_filter(use_slot ? cmd->texture_slot_mip_filter[slot]
                                                            : cmd->texture_mip_filter);
    int32_t anisotropy = use_slot ? cmd->texture_slot_anisotropy[slot] : cmd->texture_anisotropy;
    anisotropy = (min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ||
                  mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST)
                     ? 1
                     : vgfx3d_metal_sanitize_anisotropy(anisotropy);
    NSString *key = [NSString stringWithFormat:@"%d:%d:%d:%d:%d:%d",
                                               wrap_s,
                                               wrap_t,
                                               min_filter,
                                               mag_filter,
                                               mip_filter,
                                               anisotropy];
    id<MTLSamplerState> sampler = ctx.samplerCache[key];
    if (sampler)
        return sampler;

    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                       ? MTLSamplerMinMagFilterNearest
                       : MTLSamplerMinMagFilterLinear;
    sd.magFilter = mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                       ? MTLSamplerMinMagFilterNearest
                       : MTLSamplerMinMagFilterLinear;
    sd.mipFilter = metal_material_mip_filter_mode(mip_filter);
    sd.maxAnisotropy = (NSUInteger)anisotropy;
    sd.sAddressMode = metal_material_address_mode(wrap_s);
    sd.tAddressMode = metal_material_address_mode(wrap_t);
    sampler = [ctx.device newSamplerStateWithDescriptor:sd];
    if (sampler)
        ctx.samplerCache[key] = sampler;
    return sampler ? sampler : (ctx.sharedSampler ? ctx.sharedSampler : ctx.defaultSampler);
}

//=============================================================================
// Backend vtable
//=============================================================================

typedef struct metal_main_functions_t {
    id<MTLFunction> vertex;
    id<MTLFunction> vertexInstanced;
    id<MTLFunction> vertexParticles;
    id<MTLFunction> vertexShadow;
    id<MTLFunction> fragmentShadow;
    id<MTLFunction> fragment;
} metal_main_functions_t;

/// @brief Allocate the Objective-C context and initialize cache/default fields.
/// @param[in] device Metal device retained by the context.
/// @param[in] win Backend window handle associated with the context.
/// @param[in] w Initial drawable width.
/// @param[in] h Initial drawable height.
/// @return Newly allocated context with caches and defaults initialized.
static VGFXMetalContext *metal_create_context_base(id<MTLDevice> device,
                                                   vgfx_window_t win,
                                                   int32_t w,
                                                   int32_t h) {
    VGFXMetalContext *ctx = [[VGFXMetalContext alloc] init];
    if (!ctx)
        return nil;
    ctx.device = device;
    ctx.width = w;
    ctx.height = h;
    ctx.renderScale = 1.0f;
    ctx.vgfxWin = win;
    ctx.textureCache = [NSMutableDictionary dictionaryWithCapacity:32];
    ctx.cubemapCache = [NSMutableDictionary dictionaryWithCapacity:8];
    ctx.geometryCache = [NSMutableDictionary dictionaryWithCapacity:32];
    ctx.morphCache = [NSMutableDictionary dictionaryWithCapacity:16];
    ctx.extraInfluenceCache = [NSMutableDictionary dictionaryWithCapacity:8];
    ctx.renderTargetCache = [NSMutableDictionary dictionaryWithCapacity:8];
    ctx.samplerCache = [NSMutableDictionary dictionaryWithCapacity:8];
    if (!ctx.textureCache || !ctx.cubemapCache || !ctx.geometryCache || !ctx.morphCache ||
        !ctx.extraInfluenceCache || !ctx.renderTargetCache || !ctx.samplerCache) {
        return nil;
    }
    ctx.currentTargetKind = VGFX3D_METAL_TARGET_SWAPCHAIN;
    ctx.textureUploadBudgetBytes = UINT64_MAX;
    return ctx;
}

/// @brief Attach a CAMetalLayer to the native view and sync its drawable size.
/// @param[in,out] ctx Context receiving the layer.
/// @param[in,out] view Native AppKit view that hosts the layer.
/// @param[in] device Metal device assigned to the new layer.
/// @return YES after a Metal layer is attached, otherwise NO.
static BOOL metal_attach_layer_to_view(VGFXMetalContext *ctx, NSView *view, id<MTLDevice> device) {
    if (!ctx || !view || !device)
        return NO;
    view.wantsLayer = YES;
    if (!view.layer)
        view.layer = [CALayer layer];
    if (!view.layer)
        return NO;
    ctx.metalLayer = [CAMetalLayer layer];
    if (!ctx.metalLayer)
        return NO;
    ctx.metalLayer.device = device;
    ctx.metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ctx.metalLayer.framebufferOnly = NO;
    ctx.metalLayer.hidden = YES;
    ctx.metalLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [view.layer addSublayer:ctx.metalLayer];
    metal_update_layer_size(ctx);
    return YES;
}

/// @brief Create the command queue and reset frame matrices/state.
/// @param[in,out] ctx Context receiving the queue, semaphore, and default matrices.
/// @param[in] device Metal device used to create the command queue.
/// @return `YES` when the queue and defaults are ready; otherwise `NO`.
static BOOL metal_create_command_queue_and_defaults(VGFXMetalContext *ctx, id<MTLDevice> device) {
    ctx.commandQueue = [device newCommandQueue];
    if (!ctx.commandQueue) {
        NSLog(@ "[Metal] newCommandQueue returned nil");
        return NO;
    }
    ctx.inflightSemaphore = dispatch_semaphore_create(VGFX3D_METAL_MAX_INFLIGHT);
    if (!ctx.inflightSemaphore)
        return NO;
    ctx.transientRingCursor = 0;
    ctx.transientRingSlot = 0;
    mat4f_identity(ctx->_view);
    mat4f_identity(ctx->_projection);
    mat4f_identity(ctx->_vp);
    mat4f_identity(ctx->_prevVP);
    mat4f_identity(ctx->_invVP);
    ctx->_prevVPValid = NO;
    return YES;
}

/// @brief Compile the main in-memory Metal shader library.
/// @param[in,out] ctx Context receiving the compiled library.
/// @param[in] device Metal device used for source compilation.
/// @return `YES` when the library compiles; otherwise `NO`.
static BOOL metal_compile_main_library(VGFXMetalContext *ctx, id<MTLDevice> device) {
    NSError *error = nil;
    ctx.library = [device newLibraryWithSource:metal_shader_source options:nil error:&error];
    if (!ctx.library) {
        NSLog(@ "[Metal] Shader error: %@", error);
        return NO;
    }
    return YES;
}

/// @brief Resolve the required main/shadow shader entry points from the main library.
/// @param[in] ctx Context containing the compiled main library.
/// @param[out] funcs Receives all required function objects.
/// @return `YES` when every required entry point is found; otherwise `NO`.
static BOOL metal_load_main_functions(VGFXMetalContext *ctx, metal_main_functions_t *funcs) {
    funcs->vertex = nil;
    funcs->vertexInstanced = nil;
    funcs->vertexParticles = nil;
    funcs->vertexShadow = nil;
    funcs->fragmentShadow = nil;
    funcs->fragment = nil;
    funcs->vertex = [ctx.library newFunctionWithName:@ "vertex_main"];
    funcs->vertexInstanced = [ctx.library newFunctionWithName:@ "vertex_main_instanced"];
    funcs->vertexParticles = [ctx.library newFunctionWithName:@ "vertex_main_particles"];
    funcs->vertexShadow = [ctx.library newFunctionWithName:@ "vertex_shadow"];
    funcs->fragmentShadow = [ctx.library newFunctionWithName:@ "fragment_shadow"];
    funcs->fragment = [ctx.library newFunctionWithName:@ "fragment_main"];
    if (!funcs->vertex || !funcs->vertexInstanced || !funcs->vertexParticles ||
        !funcs->vertexShadow || !funcs->fragmentShadow || !funcs->fragment) {
        NSLog(@ "[Metal] required shader entrypoints missing (vf=%@ inst=%@ particles=%@ shadow=%@ "
               "shadowf=%@ ff=%@)",
              funcs->vertex,
              funcs->vertexInstanced,
              funcs->vertexParticles,
              funcs->vertexShadow,
              funcs->fragmentShadow,
              funcs->fragment);
        return NO;
    }
    return YES;
}

/// @brief Build all main and instanced pipeline-state variants.
/// @param[in,out] ctx Context receiving mandatory pipeline states.
/// @param[in] device Metal device used for compilation.
/// @param[in] funcs Resolved main, particle, and shadow functions.
/// @return `YES` when every mandatory full-layout pipeline is created.
static BOOL metal_create_main_pipeline_states(VGFXMetalContext *ctx,
                                              id<MTLDevice> device,
                                              const metal_main_functions_t *funcs) {
    NSError *error = nil;
    ctx.pipelineState = metal_create_pipeline_state(device,
                                                    funcs->vertex,
                                                    funcs->fragment,
                                                    create_vertex_descriptor(),
                                                    MTLPixelFormatRGBA16Float,
                                                    VGFX3D_METAL_BLEND_OPAQUE,
                                                    NO,
                                                    &error);
    ctx.pipelineStateAlpha = metal_create_pipeline_state(device,
                                                         funcs->vertex,
                                                         funcs->fragment,
                                                         create_vertex_descriptor(),
                                                         MTLPixelFormatRGBA16Float,
                                                         VGFX3D_METAL_BLEND_ALPHA,
                                                         YES,
                                                         &error);
    ctx.pipelineStateAdditive = metal_create_pipeline_state(device,
                                                            funcs->vertex,
                                                            funcs->fragment,
                                                            create_vertex_descriptor(),
                                                            MTLPixelFormatRGBA16Float,
                                                            VGFX3D_METAL_BLEND_ADDITIVE,
                                                            YES,
                                                            &error);
    ctx.pipelineStateColorOnly = metal_create_pipeline_state(device,
                                                             funcs->vertex,
                                                             funcs->fragment,
                                                             create_vertex_descriptor(),
                                                             MTLPixelFormatBGRA8Unorm,
                                                             VGFX3D_METAL_BLEND_OPAQUE,
                                                             NO,
                                                             &error);
    ctx.pipelineStateColorOnlyAlpha = metal_create_pipeline_state(device,
                                                                  funcs->vertex,
                                                                  funcs->fragment,
                                                                  create_vertex_descriptor(),
                                                                  MTLPixelFormatBGRA8Unorm,
                                                                  VGFX3D_METAL_BLEND_ALPHA,
                                                                  YES,
                                                                  &error);
    ctx.pipelineStateColorOnlyAdditive = metal_create_pipeline_state(device,
                                                                     funcs->vertex,
                                                                     funcs->fragment,
                                                                     create_vertex_descriptor(),
                                                                     MTLPixelFormatBGRA8Unorm,
                                                                     VGFX3D_METAL_BLEND_ADDITIVE,
                                                                     YES,
                                                                     &error);
    ctx.instancedPipelineState = metal_create_pipeline_state(device,
                                                             funcs->vertexInstanced,
                                                             funcs->fragment,
                                                             create_vertex_descriptor(),
                                                             MTLPixelFormatRGBA16Float,
                                                             VGFX3D_METAL_BLEND_OPAQUE,
                                                             NO,
                                                             &error);
    ctx.instancedPipelineStateAlpha = metal_create_pipeline_state(device,
                                                                  funcs->vertexInstanced,
                                                                  funcs->fragment,
                                                                  create_vertex_descriptor(),
                                                                  MTLPixelFormatRGBA16Float,
                                                                  VGFX3D_METAL_BLEND_ALPHA,
                                                                  YES,
                                                                  &error);
    ctx.instancedPipelineStateAdditive = metal_create_pipeline_state(device,
                                                                     funcs->vertexInstanced,
                                                                     funcs->fragment,
                                                                     create_vertex_descriptor(),
                                                                     MTLPixelFormatRGBA16Float,
                                                                     VGFX3D_METAL_BLEND_ADDITIVE,
                                                                     YES,
                                                                     &error);
    ctx.instancedPipelineStateColorOnly = metal_create_pipeline_state(device,
                                                                      funcs->vertexInstanced,
                                                                      funcs->fragment,
                                                                      create_vertex_descriptor(),
                                                                      MTLPixelFormatBGRA8Unorm,
                                                                      VGFX3D_METAL_BLEND_OPAQUE,
                                                                      NO,
                                                                      &error);
    ctx.instancedPipelineStateColorOnlyAlpha =
        metal_create_pipeline_state(device,
                                    funcs->vertexInstanced,
                                    funcs->fragment,
                                    create_vertex_descriptor(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_ALPHA,
                                    YES,
                                    &error);
    ctx.instancedPipelineStateColorOnlyAdditive =
        metal_create_pipeline_state(device,
                                    funcs->vertexInstanced,
                                    funcs->fragment,
                                    create_vertex_descriptor(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_ADDITIVE,
                                    YES,
                                    &error);
    ctx.particlePipelineStateAlpha = metal_create_pipeline_state(device,
                                                                 funcs->vertexParticles,
                                                                 funcs->fragment,
                                                                 create_vertex_descriptor(),
                                                                 MTLPixelFormatRGBA16Float,
                                                                 VGFX3D_METAL_BLEND_ALPHA,
                                                                 YES,
                                                                 &error);
    ctx.particlePipelineStateAdditive = metal_create_pipeline_state(device,
                                                                    funcs->vertexParticles,
                                                                    funcs->fragment,
                                                                    create_vertex_descriptor(),
                                                                    MTLPixelFormatRGBA16Float,
                                                                    VGFX3D_METAL_BLEND_ADDITIVE,
                                                                    YES,
                                                                    &error);
    ctx.particlePipelineStateColorOnlyAlpha =
        metal_create_pipeline_state(device,
                                    funcs->vertexParticles,
                                    funcs->fragment,
                                    create_vertex_descriptor(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_ALPHA,
                                    YES,
                                    &error);
    ctx.particlePipelineStateColorOnlyAdditive =
        metal_create_pipeline_state(device,
                                    funcs->vertexParticles,
                                    funcs->fragment,
                                    create_vertex_descriptor(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_ADDITIVE,
                                    YES,
                                    &error);
    if (!ctx.pipelineState || !ctx.pipelineStateAlpha || !ctx.pipelineStateAdditive ||
        !ctx.pipelineStateColorOnly || !ctx.pipelineStateColorOnlyAlpha ||
        !ctx.pipelineStateColorOnlyAdditive || !ctx.instancedPipelineState ||
        !ctx.instancedPipelineStateAlpha || !ctx.instancedPipelineStateAdditive ||
        !ctx.instancedPipelineStateColorOnly || !ctx.instancedPipelineStateColorOnlyAlpha ||
        !ctx.instancedPipelineStateColorOnlyAdditive || !ctx.particlePipelineStateAlpha ||
        !ctx.particlePipelineStateAdditive || !ctx.particlePipelineStateColorOnlyAlpha ||
        !ctx.particlePipelineStateColorOnlyAdditive) {
        NSLog(@ "Metal pipeline error: %@", error);
        return NO;
    }
    return YES;
}

/// @brief Build the compact-vertex-stream twins of every mesh pipeline (R20).
/// @details The same vertex/fragment functions compile against the 48-byte compact
///   vertex descriptor. Failure is non-fatal: compactPipelinesReady stays NO and the
///   backend keeps uploading/drawing the full 92-byte layout.
/// @param[in,out] ctx Context receiving optional compact pipeline states.
/// @param[in] device Metal device used for compilation.
/// @param[in] funcs Resolved mesh and shadow shader functions.
static void metal_create_compact_pipeline_states(VGFXMetalContext *ctx,
                                                 id<MTLDevice> device,
                                                 const metal_main_functions_t *funcs) {
    NSError *error = nil;
    ctx.compactPipelinesReady = NO;
    ctx.pipelineStateCompact = metal_create_pipeline_state(device,
                                                           funcs->vertex,
                                                           funcs->fragment,
                                                           create_vertex_descriptor_compact(),
                                                           MTLPixelFormatRGBA16Float,
                                                           VGFX3D_METAL_BLEND_OPAQUE,
                                                           NO,
                                                           &error);
    ctx.pipelineStateAlphaCompact = metal_create_pipeline_state(device,
                                                                funcs->vertex,
                                                                funcs->fragment,
                                                                create_vertex_descriptor_compact(),
                                                                MTLPixelFormatRGBA16Float,
                                                                VGFX3D_METAL_BLEND_ALPHA,
                                                                YES,
                                                                &error);
    ctx.pipelineStateAdditiveCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertex,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatRGBA16Float,
                                    VGFX3D_METAL_BLEND_ADDITIVE,
                                    YES,
                                    &error);
    ctx.pipelineStateColorOnlyCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertex,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_OPAQUE,
                                    NO,
                                    &error);
    ctx.pipelineStateColorOnlyAlphaCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertex,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_ALPHA,
                                    YES,
                                    &error);
    ctx.pipelineStateColorOnlyAdditiveCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertex,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_ADDITIVE,
                                    YES,
                                    &error);
    ctx.instancedPipelineStateCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertexInstanced,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatRGBA16Float,
                                    VGFX3D_METAL_BLEND_OPAQUE,
                                    NO,
                                    &error);
    ctx.instancedPipelineStateAlphaCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertexInstanced,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatRGBA16Float,
                                    VGFX3D_METAL_BLEND_ALPHA,
                                    YES,
                                    &error);
    ctx.instancedPipelineStateAdditiveCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertexInstanced,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatRGBA16Float,
                                    VGFX3D_METAL_BLEND_ADDITIVE,
                                    YES,
                                    &error);
    ctx.instancedPipelineStateColorOnlyCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertexInstanced,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_OPAQUE,
                                    NO,
                                    &error);
    ctx.instancedPipelineStateColorOnlyAlphaCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertexInstanced,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_ALPHA,
                                    YES,
                                    &error);
    ctx.instancedPipelineStateColorOnlyAdditiveCompact =
        metal_create_pipeline_state(device,
                                    funcs->vertexInstanced,
                                    funcs->fragment,
                                    create_vertex_descriptor_compact(),
                                    MTLPixelFormatBGRA8Unorm,
                                    VGFX3D_METAL_BLEND_ADDITIVE,
                                    YES,
                                    &error);
    {
        MTLRenderPipelineDescriptor *spd = [[MTLRenderPipelineDescriptor alloc] init];
        NSError *shadowErr = nil;
        spd.vertexFunction = funcs->vertexShadow;
        spd.fragmentFunction = funcs->fragmentShadow;
        spd.vertexDescriptor = create_vertex_descriptor_compact();
        spd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        spd.colorAttachments[0].pixelFormat = MTLPixelFormatInvalid;
        ctx.shadowPipelineCompact =
            [device newRenderPipelineStateWithDescriptor:spd error:&shadowErr];
    }
    if (ctx.pipelineStateCompact && ctx.pipelineStateAlphaCompact &&
        ctx.pipelineStateAdditiveCompact && ctx.pipelineStateColorOnlyCompact &&
        ctx.pipelineStateColorOnlyAlphaCompact && ctx.pipelineStateColorOnlyAdditiveCompact &&
        ctx.instancedPipelineStateCompact && ctx.instancedPipelineStateAlphaCompact &&
        ctx.instancedPipelineStateAdditiveCompact && ctx.instancedPipelineStateColorOnlyCompact &&
        ctx.instancedPipelineStateColorOnlyAlphaCompact &&
        ctx.instancedPipelineStateColorOnlyAdditiveCompact && ctx.shadowPipelineCompact) {
        ctx.compactPipelinesReady = YES;
    } else {
        NSLog(@ "Metal compact pipeline error (falling back to full vertex layout): %@", error);
    }
}

/// @brief Create depth-stencil state variants used by opaque, transparent, disabled, and skybox
/// draws.
/// @details Reversed-Z: scene passes clear depth to 0 and compare Greater (near = 1,
///   far = 0), which concentrates float-depth precision in the distance. The shadow
///   pass keeps its own standard Less state (metal_create_shadow_resources) because
///   light view-projections stay in the standard convention.
/// @param[in,out] ctx Context receiving the depth-state variants.
/// @param[in] device Metal device used to create the states.
/// @return YES when every required depth state exists, otherwise NO.
static BOOL metal_create_depth_state_variants(VGFXMetalContext *ctx, id<MTLDevice> device) {
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = MTLCompareFunctionGreater;
    dd.depthWriteEnabled = YES;
    ctx.depthState = [device newDepthStencilStateWithDescriptor:dd];
    dd.depthWriteEnabled = NO;
    ctx.depthStateNoWrite = [device newDepthStencilStateWithDescriptor:dd];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    ctx.depthStateDisabled = [device newDepthStencilStateWithDescriptor:dd];
    dd.depthCompareFunction = MTLCompareFunctionGreaterEqual;
    ctx.skyboxDepthState = [device newDepthStencilStateWithDescriptor:dd];
    return ctx.depthState && ctx.depthStateNoWrite && ctx.depthStateDisabled &&
           ctx.skyboxDepthState;
}

/// @brief Create default white 2D/cubemap textures plus the default sampler.
/// @param[in,out] ctx Context receiving fallback textures, sampler, and BRDF LUT.
/// @param[in] device Metal device used to create the resources.
/// @return YES when all fallback resources exist, otherwise NO.
static BOOL metal_create_default_texture_resources(VGFXMetalContext *ctx, id<MTLDevice> device) {
    MTLTextureDescriptor *dtd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    dtd.usage = MTLTextureUsageShaderRead;
    dtd.storageMode = MTLStorageModeShared;
    ctx.defaultTexture = [device newTextureWithDescriptor:dtd];
    if (!ctx.defaultTexture)
        return NO;
    uint32_t white = 0xFFFFFFFF;
    [ctx.defaultTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                          mipmapLevel:0
                            withBytes:&white
                          bytesPerRow:4];

    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.mipFilter = MTLSamplerMipFilterLinear;
    ctx.defaultSampler = [device newSamplerStateWithDescriptor:sd];
    if (!ctx.defaultSampler)
        return NO;

    MTLTextureDescriptor *cubeDesc =
        [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              size:1
                                                         mipmapped:NO];
    cubeDesc.usage = MTLTextureUsageShaderRead;
    cubeDesc.storageMode = MTLStorageModeShared;
    ctx.defaultCubemap = [device newTextureWithDescriptor:cubeDesc];
    if (!ctx.defaultCubemap)
        return NO;
    for (NSUInteger face = 0; face < 6; face++) {
        uint32_t cube_white = 0xFFFFFFFF;
        [ctx.defaultCubemap replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                              mipmapLevel:0
                                    slice:face
                                withBytes:&cube_white
                              bytesPerRow:4
                            bytesPerImage:4];
    }

    /* Split-sum environment BRDF table (shared CPU precomputation). */
    MTLTextureDescriptor *lutDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Float
                                                           width:VGFX3D_BRDF_LUT_SIZE
                                                          height:VGFX3D_BRDF_LUT_SIZE
                                                       mipmapped:NO];
    lutDesc.usage = MTLTextureUsageShaderRead;
    lutDesc.storageMode = MTLStorageModeShared;
    ctx.brdfLutTexture = [device newTextureWithDescriptor:lutDesc];
    if (!ctx.brdfLutTexture)
        return NO;
    [ctx.brdfLutTexture
        replaceRegion:MTLRegionMake2D(0, 0, VGFX3D_BRDF_LUT_SIZE, VGFX3D_BRDF_LUT_SIZE)
          mipmapLevel:0
            withBytes:vgfx3d_brdf_lut_data()
          bytesPerRow:(NSUInteger)VGFX3D_BRDF_LUT_SIZE * 2 * sizeof(float)];
    return YES;
}

/// @brief Create shared texture and cubemap samplers.
/// @param[in,out] ctx Context receiving shared 2D and cube sampler states.
/// @param[in] device Metal device used to create the samplers.
/// @return YES when both shared samplers exist, otherwise NO.
static BOOL metal_create_shared_sampler_resources(VGFXMetalContext *ctx, id<MTLDevice> device) {
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.mipFilter = MTLSamplerMipFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeRepeat;
    sd.tAddressMode = MTLSamplerAddressModeRepeat;
    ctx.sharedSampler = [device newSamplerStateWithDescriptor:sd];

    MTLSamplerDescriptor *cubeSd = [[MTLSamplerDescriptor alloc] init];
    cubeSd.minFilter = MTLSamplerMinMagFilterLinear;
    cubeSd.magFilter = MTLSamplerMinMagFilterLinear;
    cubeSd.mipFilter = MTLSamplerMipFilterLinear;
    cubeSd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    cubeSd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    cubeSd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    ctx.cubeSampler = [device newSamplerStateWithDescriptor:cubeSd];
    return ctx.sharedSampler && ctx.cubeSampler;
}

/// @brief Create shadow pipeline, comparison sampler, and shadow depth state.
/// @param[in,out] ctx Context receiving the shadow resources.
/// @param[in] device Metal device used for resource creation.
/// @param[in] funcs Resolved shadow vertex and fragment functions.
/// @return YES when the shadow pipeline, sampler, and depth state all exist, otherwise NO.
static BOOL metal_create_shadow_resources(VGFXMetalContext *ctx,
                                          id<MTLDevice> device,
                                          const metal_main_functions_t *funcs) {
    MTLRenderPipelineDescriptor *spd = [[MTLRenderPipelineDescriptor alloc] init];
    spd.vertexFunction = funcs->vertexShadow;
    spd.fragmentFunction = funcs->fragmentShadow;
    spd.vertexDescriptor = create_vertex_descriptor();
    spd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    spd.colorAttachments[0].pixelFormat = MTLPixelFormatInvalid;
    NSError *shadowErr = nil;
    ctx.shadowPipeline = [device newRenderPipelineStateWithDescriptor:spd error:&shadowErr];

    MTLSamplerDescriptor *csd = [[MTLSamplerDescriptor alloc] init];
    csd.compareFunction = MTLCompareFunctionLessEqual;
    csd.minFilter = MTLSamplerMinMagFilterLinear;
    csd.magFilter = MTLSamplerMinMagFilterLinear;
    ctx.shadowSampler = [device newSamplerStateWithDescriptor:csd];

    MTLDepthStencilDescriptor *sdd = [[MTLDepthStencilDescriptor alloc] init];
    sdd.depthCompareFunction = MTLCompareFunctionLess;
    sdd.depthWriteEnabled = YES;
    ctx.shadowDepthState = [device newDepthStencilStateWithDescriptor:sdd];
    return ctx.shadowPipeline && ctx.shadowSampler && ctx.shadowDepthState;
}

/// @brief Backend vtable entry: create the Metal rendering context for window @p win sized @p w×@p
/// h —
///   acquires the system default MTLDevice, attaches a CAMetalLayer to the native NSView, and
///   initializes the per-context resource caches
///   (texture/cubemap/geometry/morph/render-target/sampler).
/// @param[in] win Window whose native view hosts the Metal layer.
/// @param[in] w Initial drawable width.
/// @param[in] h Initial drawable height.
/// @return An opaque VGFXMetalContext* handle (cast to void*), or NULL if no Metal device or native
///   view is available.
static void *metal_create_ctx(vgfx_window_t win, int32_t w, int32_t h) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            NSLog(@ "[Metal] MTLCreateSystemDefaultDevice returned nil");
            return NULL;
        }

        /* Headless contexts (ADR 0191 accelerated offscreen) have no window:
         * the presentation layer is skipped entirely. Offscreen rendering
         * flows through render targets and their existing GPU→CPU color
         * sync, and every present/layer route already no-ops without a
         * CAMetalLayer, so only windowed contexts require a native view. */
        NSView *view = nil;
        if (win) {
            view = (__bridge NSView *)vgfx_get_native_view(win);
            if (!view) {
                NSLog(@ "[Metal] vgfx_get_native_view returned nil");
                return NULL;
            }
        }

        VGFXMetalContext *ctx = metal_create_context_base(device, win, w, h);
        metal_main_functions_t funcs;
        if (!ctx)
            return NULL;
        if (view && !metal_attach_layer_to_view(ctx, view, device))
            return NULL;
        if (!metal_create_command_queue_and_defaults(ctx, device) ||
            !metal_compile_main_library(ctx, device) || !metal_load_main_functions(ctx, &funcs) ||
            !metal_create_main_pipeline_states(ctx, device, &funcs)) {
            [ctx.metalLayer removeFromSuperlayer];
            ctx.metalLayer = nil;
            return NULL;
        }
        if (!metal_create_depth_state_variants(ctx, device) ||
            !metal_create_default_texture_resources(ctx, device) ||
            !metal_create_shared_sampler_resources(ctx, device) ||
            !metal_create_shadow_resources(ctx, device, &funcs)) {
            [ctx.metalLayer removeFromSuperlayer];
            ctx.metalLayer = nil;
            return NULL;
        }
        metal_create_compact_pipeline_states(ctx, device, &funcs);

        /* MTL-11: Post-processing fullscreen quad shader + pipeline */
        {
            NSArray<NSString *> *postfxChunks = @[
                [NSString
                    stringWithUTF8String:
                        "#include <metal_stdlib>\n"
                        "using namespace metal;\n"
                        "struct FullscreenVert { float4 position [[position]]; float2 uv; };\n"
                        "struct PostFXParams {\n"
                        "    float4x4 invViewProjection;\n"
                        "    float4x4 prevViewProjection;\n"
                        "    float4 cameraPosition;\n"
                        "    float2 invResolution;\n"
                        "    int bloomEnabled;\n"
                        "    float bloomThreshold;\n"
                        "    float bloomStrength;\n"
                        "    int bloomPasses;\n"
                        "    int tonemapMode;\n"
                        "    float tonemapExposure;\n"
                        "    int fxaaEnabled;\n"
                        "    int colorGradeEnabled;\n"
                        "    float cgBright;\n"
                        "    float cgContrast;\n"
                        "    float cgSat;\n"
                        "    int vignetteEnabled;\n"
                        "    float vigRadius;\n"
                        "    float vigSoftness;\n"
                        "    int ssaoEnabled;\n"
                        "    float ssaoRadius;\n"
                        "    float ssaoIntensity;\n"
                        "    int ssaoSamples;\n"
                        "    int dofEnabled;\n"
                        "    float dofFocusDist;\n"
                        "    float dofAperture;\n"
                        "    float dofMaxBlur;\n"
                        "    int motionBlurEnabled;\n"
                        "    float motionBlurIntensity;\n"
                        "    int motionBlurSamples;\n"
                        "    int overlayEnabled;\n"
                        "    int sceneIsHdr;\n"
                        "    int tonemapExplicit;\n"
                        "    int bloomTexEnabled;\n"
                        "    int taaPad;\n"
                        "    int lutEnabled;\n"
                        "    float lutBlend;\n"
                        "    float2 lutPad;\n"
                        "    int sharpenEnabled;\n"
                        "    float sharpenAmount;\n"
                        "    float2 sharpenPad;\n"
                        "};\n"
                        "vertex FullscreenVert fullscreen_vs(uint vid [[vertex_id]]) {\n"
                        "    float2 positions[4] = {float2(-1,-1), float2(1,-1), float2(-1,1), "
                        "float2(1,1)};\n"
                        "    float2 uvs[4] = {float2(0,1), float2(1,1), float2(0,0), "
                        "float2(1,0)};\n"
                        "    FullscreenVert out;\n"
                        "    out.position = float4(positions[vid], 0, 1);\n"
                        "    out.uv = uvs[vid];\n"
                        "    return out;\n"
                        "}\n"],
                [NSString
                    stringWithUTF8String:
                        "float3 sampleScene(texture2d<float> sceneTex, sampler s, float2 uv) { "
                        "return sceneTex.sample(s, uv).rgb; }\n"
                        "float sampleDepth(depth2d<float> depthTex, sampler s, float2 uv) { return "
                        "depthTex.sample(s, uv); }\n"
                        "float3 sampleMotion(texture2d<float> motionTex, sampler s, float2 uv) { "
                        "return motionTex.sample(s, uv).rgb; }\n"
                        /* Top-left texture uv -> Y-up NDC needs the Y flip (the D3D11 twin's
                         * uvToNdc); inv VP follows the engine's GL-convention z in [-1,1]. */
                        "float3 reconstructWorld(constant PostFXParams &p, float2 uv, float depth) "
                        "{\n"
                        "    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, "
                        "depth * 2.0 - 1.0, 1.0);\n"
                        "    float4 world = p.invViewProjection * clip;\n"
                        "    return world.xyz / max(world.w, 0.0001);\n"
                        "}\n"
                        "float computeSsao(constant PostFXParams &p, depth2d<float> depthTex, "
                        "sampler s, float2 uv, float centerDepth) {\n"
                        /* Plan 05: normal-aware hemispheric AO with range check. Position is
                         * reconstructed from depth; the normal comes from screen-space
                         * derivatives of that position (no G-buffer). A golden-angle spiral
                         * rotated per-pixel by interleaved-gradient noise replaces the fixed
                         * 8-tap ring, and the smoothstep range check kills the classic
                         * depth-only halo around silhouettes. Reversed-Z: cleared/far depth
                         * is 0, so the sky guard tests the low end. */
                        "    if (centerDepth <= 0.0001) return 1.0;\n"
                        "    float3 P = reconstructWorld(p, uv, centerDepth);\n"
                        "    float3 N = cross(dfdx(P), dfdy(P));\n"
                        "    float nlen = length(N);\n"
                        "    if (nlen < 1e-9) return 1.0;\n"
                        "    N /= nlen;\n"
                        "    float3 V = p.cameraPosition.xyz - P;\n"
                        "    float dist = length(V);\n"
                        "    if (dist < 1e-4) return 1.0;\n"
                        "    if (dot(N, V / dist) < 0.0) N = -N;\n"
                        "    float radius = max(p.ssaoRadius, 0.01);\n"
                        "    float screenRadius = clamp(radius / dist * 0.5, 0.002, 0.05);\n"
                        "    int count = clamp(p.ssaoSamples, 4, 16);\n"
                        "    float2 px = uv / max(p.invResolution, float2(1e-6));\n"
                        "    float ign = fract(52.9829189 * fract(0.06711056 * px.x + "
                        "0.00583715 * px.y));\n"
                        "    float occ = 0.0;\n"
                        "    float wsum = 0.0;\n"
                        "    for (int i = 0; i < count; i++) {\n"
                        "        float ang = (float(i) + ign) * 2.39996323;\n"
                        "        float rad = screenRadius * sqrt((float(i) + 0.5) / "
                        "float(count));\n"
                        "        float2 suv = uv + float2(cos(ang), sin(ang)) * rad;\n"
                        "        float sd = sampleDepth(depthTex, s, suv);\n"
                        "        float3 Ps = reconstructWorld(p, suv, sd);\n"
                        "        float3 d = Ps - P;\n"
                        "        float dl = length(d);\n"
                        "        if (dl < 1e-6) continue;\n"
                        "        float rangeCheck = 1.0 - smoothstep(0.0, 1.0, dl / radius);\n"
                        "        occ += max(dot(N, d / dl) - 0.025, 0.0) * rangeCheck;\n"
                        "        wsum += 1.0;\n"
                        "    }\n"
                        "    return 1.0 - clamp(occ / max(wsum, 1.0) * p.ssaoIntensity * 2.0, "
                        "0.0, 1.0);\n"
                        "}\n"
                        "float2 cameraVelocity(constant PostFXParams &p, float2 uv, float3 "
                        "worldPos) {\n"
                        "    float4 prevClip = p.prevViewProjection * float4(worldPos, 1.0);\n"
                        "    float prevW = (prevClip.w < 0.0 ? -1.0 : 1.0) * max(fabs(prevClip.w), "
                        "0.0001);\n"
                        /* Y-up NDC -> top-left uv: flip Y so the delta is texture-space. */
                        "    float2 prevUv = float2(prevClip.x / prevW * 0.5 + 0.5, "
                        "0.5 - prevClip.y / prevW * 0.5);\n"
                        "    return uv - prevUv;\n"
                        "}\n"],
                [NSString
                    stringWithUTF8String:
                        "float3 applyMotionBlur(constant PostFXParams &p,\n"
                        "                       texture2d<float> sceneTex,\n"
                        "                       texture2d<float> motionTex,\n"
                        "                       depth2d<float> depthTex,\n"
                        "                       sampler s,\n"
                        "                       float2 uv,\n"
                        "                       float3 color,\n"
                        "                       float3 worldPos) {\n"
                        "    float3 motionSample = sampleMotion(motionTex, s, uv);\n"
                        "    float2 velocity = motionSample.xy * 2.0 - 1.0;\n"
                        "    if (motionSample.z < 0.5)\n"
                        "        velocity = cameraVelocity(p, uv, worldPos);\n"
                        "    velocity *= p.motionBlurIntensity;\n"
                        "    if (length(velocity) < 0.0005)\n"
                        "        return color;\n"
                        "    int taps = clamp(p.motionBlurSamples, 2, 8);\n"
                        "    float3 acc = float3(0.0);\n"
                        "    float weight = 0.0;\n"
                        "    for (int i = 0; i < taps; i++) {\n"
                        "        float t = (float(i) / float(taps - 1)) - 0.5;\n"
                        "        acc += sampleScene(sceneTex, s, uv + velocity * t);\n"
                        "        weight += 1.0;\n"
                        "    }\n"
                        "    return acc / max(weight, 1.0);\n"
                        "}\n"
                        "float3 applyDof(constant PostFXParams &p,\n"
                        "                texture2d<float> sceneTex,\n"
                        "                sampler s,\n"
                        "                float2 uv,\n"
                        "                float3 color,\n"
                        "                float3 worldPos) {\n"
                        "    float dist = length(worldPos - p.cameraPosition.xyz);\n"
                        "    float blur = clamp(abs(dist - p.dofFocusDist) * max(p.dofAperture, "
                        "0.0) * 0.02, 0.0, p.dofMaxBlur);\n"
                        "    if (blur < 0.001)\n"
                        "        return color;\n"
                        "    float2 stepUv = p.invResolution * blur;\n"
                        "    float3 acc = color * 0.4;\n"
                        "    acc += sampleScene(sceneTex, s, uv + float2(stepUv.x, 0.0)) * 0.15;\n"
                        "    acc += sampleScene(sceneTex, s, uv - float2(stepUv.x, 0.0)) * 0.15;\n"
                        "    acc += sampleScene(sceneTex, s, uv + float2(0.0, stepUv.y)) * 0.15;\n"
                        "    acc += sampleScene(sceneTex, s, uv - float2(0.0, stepUv.y)) * 0.15;\n"
                        "    return acc;\n"
                        "}\n"],
                [NSString stringWithUTF8String:
                              "float3 applyFxaa(constant PostFXParams &p, texture2d<float> "
                              "sceneTex, sampler s, float2 uv, float3 color) {\n"
                              "    float3 lv = float3(0.299, 0.587, 0.114);\n"
                              "    float lumaM = dot(color, lv);\n"
                              "    float lumaN = dot(sampleScene(sceneTex, s, uv + float2(0.0, "
                              "-p.invResolution.y)), lv);\n"
                              "    float lumaS = dot(sampleScene(sceneTex, s, uv + float2(0.0, "
                              "p.invResolution.y)), lv);\n"
                              "    float lumaE = dot(sampleScene(sceneTex, s, uv + "
                              "float2(p.invResolution.x, 0.0)), lv);\n"
                              "    float lumaW = dot(sampleScene(sceneTex, s, uv + "
                              "float2(-p.invResolution.x, 0.0)), lv);\n"
                              "    float edge = fabs(lumaN + lumaS - 2.0 * lumaM) + fabs(lumaE + "
                              "lumaW - 2.0 * lumaM);\n"
                              "    if (edge < 0.08)\n"
                              "        return color;\n"
                              "    float3 avg = (sampleScene(sceneTex, s, uv + "
                              "float2(p.invResolution.x, 0.0)) +\n"
                              "                  sampleScene(sceneTex, s, uv + "
                              "float2(-p.invResolution.x, 0.0)) +\n"
                              "                  sampleScene(sceneTex, s, uv + float2(0.0, "
                              "p.invResolution.y)) +\n"
                              "                  sampleScene(sceneTex, s, uv + float2(0.0, "
                              "-p.invResolution.y))) * 0.25;\n"
                              "    return mix(color, avg, 0.5);\n"
                              "}\n"],
                [NSString stringWithUTF8String:
                              "fragment float4 postfx_fs(\n"
                              "    FullscreenVert in [[stage_in]],\n"
                              "    texture2d<float> sceneTex [[texture(0)]],\n"
                              "    depth2d<float> depthTex [[texture(1)]],\n"
                              "    texture2d<float> motionTex [[texture(2)]],\n"
                              "    texture2d<float> overlayTex [[texture(3)]],\n"
                              "    texture2d<float> bloomTex [[texture(4)]],\n"
                              "    texture2d<float> lutTex [[texture(5)]],\n"
                              "    sampler s [[sampler(0)]],\n"
                              "    constant PostFXParams &p [[buffer(0)]]) {\n"
                              "    float3 color = sampleScene(sceneTex, s, in.uv);\n"
                              "    float depth = sampleDepth(depthTex, s, in.uv);\n"
                              "    float3 worldPos = reconstructWorld(p, in.uv, depth);\n"
                              "    if (p.motionBlurEnabled != 0) color = applyMotionBlur(p, "
                              "sceneTex, motionTex, depthTex, s, in.uv, color, worldPos);\n"
                              "    if (p.dofEnabled != 0) color = applyDof(p, sceneTex, s, in.uv, "
                              "color, worldPos);\n"
                              "    if (p.ssaoEnabled != 0) color *= computeSsao(p, depthTex, s, "
                              "in.uv, depth);\n"
                              "    if (p.fxaaEnabled != 0) color = applyFxaa(p, sceneTex, s, "
                              "in.uv, color);\n"
                              "    if (p.bloomEnabled) {\n"
                              "        if (p.bloomTexEnabled != 0) {\n"
                              /* Plan 05: composite the prefiltered mip-chain result; the
                               * chain already applied threshold + progressive blur. */
                              "            color += bloomTex.sample(s, in.uv).rgb * "
                              "p.bloomStrength;\n"
                              "        } else {\n"
                              "        float2 bloomStep = p.invResolution * "
                              "float(min(max(p.bloomPasses, 0), 32));\n"
                              "        float3 threshold = float3(p.bloomThreshold);\n"
                              "        float3 bloom = max(color - threshold, float3(0.0));\n"
                              "        bloom += max(sampleScene(sceneTex, s, in.uv + "
                              "float2(bloomStep.x, 0.0)) - threshold, float3(0.0));\n"
                              "        bloom += max(sampleScene(sceneTex, s, in.uv - "
                              "float2(bloomStep.x, 0.0)) - threshold, float3(0.0));\n"
                              "        bloom += max(sampleScene(sceneTex, s, in.uv + "
                              "float2(0.0, bloomStep.y)) - threshold, float3(0.0));\n"
                              "        bloom += max(sampleScene(sceneTex, s, in.uv - "
                              "float2(0.0, bloomStep.y)) - threshold, float3(0.0));\n"
                              "        color += bloom * (p.bloomStrength / 5.0);\n"
                              "        }\n"
                              "    }\n"
                              "    if (p.tonemapMode == 0 && p.tonemapExplicit != 0 && "
                              "p.sceneIsHdr != 0) {\n"
                              /* Plan 05 gamma fix: explicit "tonemap off" on a linear-HDR
                               * scene still applies exposure + gamma-out so its output
                               * transform matches modes 1/2. */
                              "        color = pow(clamp(color * p.tonemapExposure, 0.0, 1.0), "
                              "float3(1.0 / 2.2));\n"
                              "    }\n"
                              /* ADR 0247: every pre-tonemap intermediate in the chain
                               * holds SCENE-LINEAR values regardless of its storage
                               * format (the scene target is linear-HDR and no earlier
                               * pass encodes), so real tone curves consume the input
                               * directly and perform the pipeline's single display
                               * encode. The former `pow(color, 2.2)` "linearization" of
                               * non-HDR sources double-decoded linear data whenever any
                               * pass preceded the tonemap through the 8-bit ping-pong —
                               * the grey-wash regression (ZB-20). */
                              "    if (p.tonemapMode == 1) {\n"
                              "        float3 tc = max(color, 0.0) * p.tonemapExposure;\n"
                              "        tc = tc / (tc + 1.0);\n"
                              "        color = pow(clamp(tc, 0.0, 1.0), float3(1.0 / 2.2));\n"
                              "    }\n"
                              "    if (p.tonemapMode == 2) {\n"
                              "        float3 x = max(color, 0.0) * p.tonemapExposure;\n"
                              "        color = (x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14);\n"
                              "        color = pow(clamp(color, 0.0, 1.0), float3(1.0 / 2.2));\n"
                              "    }\n"
                              "    if (p.colorGradeEnabled) {\n"
                              "        color = (color - 0.5) * p.cgContrast + 0.5;\n"
                              "        color += p.cgBright;\n"
                              /* Rec.709 grade luma — must match the CPU reference
                               * (rt_postfx3d grade path); FXAA keeps its own weights. */
                              "        float luma = dot(color, float3(0.2126,0.7152,0.0722));\n"
                              "        color = mix(float3(luma), color, p.cgSat);\n"
                              "        color = clamp(color, 0.0, 1.0);\n"
                              "    }\n"],
                /* (chunk split keeps each literal under the C99 4095-char cap) */
                [NSString stringWithUTF8String:
                              /* Plan 61: 3D LUT grade from the 256x16 strip (16 tiles of
                               * 16x16: x = red, y = green, tile = blue), 8-corner
                               * trilinear via integer reads — replicates the CPU
                               * reference sampler (rt_postfx3d apply_color_lut_cpu)
                               * texel-for-texel. */
                              "    if (p.lutEnabled != 0) {\n"
                              "        float3 lc = clamp(color, 0.0, 1.0) * 15.0;\n"
                              "        int3 l0 = int3(lc);\n"
                              "        int3 l1 = min(l0 + 1, 15);\n"
                              "        float3 lt = lc - float3(l0);\n"
                              "        float3 acc = float3(0.0);\n"
                              "        for (int corner = 0; corner < 8; corner++) {\n"
                              "            int rr = (corner & 1) != 0 ? l1.x : l0.x;\n"
                              "            int gg = (corner & 2) != 0 ? l1.y : l0.y;\n"
                              "            int bb = (corner & 4) != 0 ? l1.z : l0.z;\n"
                              "            float wgt = ((corner & 1) != 0 ? lt.x : 1.0 - lt.x) *\n"
                              "                        ((corner & 2) != 0 ? lt.y : 1.0 - lt.y) *\n"
                              "                        ((corner & 4) != 0 ? lt.z : 1.0 - lt.z);\n"
                              "            acc += wgt * lutTex.read(uint2(bb * 16 + rr, "
                              "gg)).rgb;\n"
                              "        }\n"
                              "        color = mix(color, acc, p.lutBlend);\n"
                              "    }\n"
                              /* Plan 61 / ADR 0271: clamped unsharp mask — edge gain
                               * bounded by the local 5-tap min/max so no ringing halos.
                               * Neighbors come from the pass source (per-pass authoring:
                               * a sharpen entry runs alone, so color == source here). */
                              "    if (p.sharpenEnabled != 0) {\n"
                              "        float3 shN = sampleScene(sceneTex, s, in.uv + "
                              "float2(0.0, -p.invResolution.y));\n"
                              "        float3 shS = sampleScene(sceneTex, s, in.uv + "
                              "float2(0.0, p.invResolution.y));\n"
                              "        float3 shE = sampleScene(sceneTex, s, in.uv + "
                              "float2(p.invResolution.x, 0.0));\n"
                              "        float3 shW = sampleScene(sceneTex, s, in.uv + "
                              "float2(-p.invResolution.x, 0.0));\n"
                              "        float3 shLo = min(color, min(min(shN, shS), "
                              "min(shE, shW)));\n"
                              "        float3 shHi = max(color, max(max(shN, shS), "
                              "max(shE, shW)));\n"
                              "        color = clamp(color + p.sharpenAmount * (color - "
                              "(shN + shS + shE + shW) * 0.25), shLo, shHi);\n"
                              "    }\n"
                              "    if (p.vignetteEnabled) {\n"
                              /* Pixel-space distance over the half-diagonal so the falloff is
                               * radial at any aspect ratio (matches the CPU reference). */
                              "        float2 vres = 1.0 / max(p.invResolution, "
                              "float2(0.000001));\n"
                              "        float2 vpx = (in.uv - 0.5) * vres;\n"
                              "        float d = length(vpx) / max(0.5 * length(vres), "
                              "0.000001);\n"
                              "        float vig = 1.0;\n"
                              "        if (d > p.vigRadius) vig = 1.0 - clamp((d - "
                              "p.vigRadius) / max(p.vigSoftness, 0.000001), 0.0, 1.0);\n"
                              "        color *= vig;\n"
                              "    }\n"
                              "    if (p.overlayEnabled != 0) {\n"
                              "        float4 overlay = overlayTex.sample(s, in.uv);\n"
                              "        color = mix(color, overlay.rgb, clamp(overlay.a, 0.0, "
                              "1.0));\n"
                              "    }\n"
                              "    return float4(color, 1.0);\n"
                              "}\n"],
                /* Plan 05: mip-chain bloom passes (13-tap Karis downsample + 3x3 tent
                 * upsample, additive) and the TAA resolve pass. */
                [NSString stringWithUTF8String:
                              "struct BloomParams {\n"
                              "    float2 srcInvSize;\n"
                              "    float threshold;\n"
                              "    int firstPass;\n"
                              "};\n"
                              "float3 bloom_safe_color(float3 c) {\n"
                              "    c = select(float3(0.0), c, isfinite(c));\n"
                              "    return clamp(c, float3(0.0), float3(65504.0));\n"
                              "}\n"
                              "float bloom_luma(float3 c) { return dot(c, float3(0.2126, 0.7152, "
                              "0.0722)); }\n"
                              "float3 bloom_karis(float3 c) {\n"
                              "    c = bloom_safe_color(c);\n"
                              "    return c / (1.0 + bloom_luma(c));\n"
                              "}\n"
                              "fragment float4 bloom_down_fs(FullscreenVert in [[stage_in]],\n"
                              "    texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]],\n"
                              "    constant BloomParams &p [[buffer(0)]]) {\n"
                              "    float2 uv = in.uv;\n"
                              "    float2 ts = p.srcInvSize;\n"
                              "    float3 a = src.sample(s, uv + ts * float2(-2.0, -2.0)).rgb;\n"
                              "    float3 b = src.sample(s, uv + ts * float2(0.0, -2.0)).rgb;\n"
                              "    float3 c = src.sample(s, uv + ts * float2(2.0, -2.0)).rgb;\n"
                              "    float3 d = src.sample(s, uv + ts * float2(-2.0, 0.0)).rgb;\n"
                              "    float3 e = src.sample(s, uv).rgb;\n"
                              "    float3 f = src.sample(s, uv + ts * float2(2.0, 0.0)).rgb;\n"
                              "    float3 g = src.sample(s, uv + ts * float2(-2.0, 2.0)).rgb;\n"
                              "    float3 h = src.sample(s, uv + ts * float2(0.0, 2.0)).rgb;\n"
                              "    float3 i2 = src.sample(s, uv + ts * float2(2.0, 2.0)).rgb;\n"
                              "    float3 j = src.sample(s, uv + ts * float2(-1.0, -1.0)).rgb;\n"
                              "    float3 k = src.sample(s, uv + ts * float2(1.0, -1.0)).rgb;\n"
                              "    float3 l = src.sample(s, uv + ts * float2(-1.0, 1.0)).rgb;\n"
                              "    float3 m = src.sample(s, uv + ts * float2(1.0, 1.0)).rgb;\n"
                              "    float3 col;\n"
                              "    if (p.firstPass != 0) {\n"
                              /* Karis average per overlapping box on the first (scene) downsample
                               * suppresses single-pixel fireflies before thresholding. */
                              "        col = bloom_karis((j + k + l + m) * 0.25) * 0.5;\n"
                              "        col += bloom_karis((a + b + d + e) * 0.25) * 0.125;\n"
                              "        col += bloom_karis((b + c + e + f) * 0.25) * 0.125;\n"
                              "        col += bloom_karis((d + e + g + h) * 0.25) * 0.125;\n"
                              "        col += bloom_karis((e + f + h + i2) * 0.25) * 0.125;\n"
                              "        float lum = bloom_luma(col);\n"
                              "        float scale = (lum > p.threshold) ? (lum - p.threshold) / "
                              "(lum + 1e-6) : 0.0;\n"
                              "        col = bloom_safe_color(col * scale);\n"
                              "    } else {\n"
                              "        col = (j + k + l + m) * (0.5 * 0.25);\n"
                              "        col += (a + b + d + e) * (0.125 * 0.25);\n"
                              "        col += (b + c + e + f) * (0.125 * 0.25);\n"
                              "        col += (d + e + g + h) * (0.125 * 0.25);\n"
                              "        col += (e + f + h + i2) * (0.125 * 0.25);\n"
                              "    }\n"
                              "    return float4(bloom_safe_color(col), 1.0);\n"
                              "}\n"
                              "fragment float4 bloom_up_fs(FullscreenVert in [[stage_in]],\n"
                              "    texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]],\n"
                              "    constant BloomParams &p [[buffer(0)]]) {\n"
                              "    float2 uv = in.uv;\n"
                              "    float2 ts = p.srcInvSize;\n"
                              "    float3 col = src.sample(s, uv + ts * float2(-1.0, -1.0)).rgb;\n"
                              "    col += src.sample(s, uv + ts * float2(0.0, -1.0)).rgb * 2.0;\n"
                              "    col += src.sample(s, uv + ts * float2(1.0, -1.0)).rgb;\n"
                              "    col += src.sample(s, uv + ts * float2(-1.0, 0.0)).rgb * 2.0;\n"
                              "    col += src.sample(s, uv).rgb * 4.0;\n"
                              "    col += src.sample(s, uv + ts * float2(1.0, 0.0)).rgb * 2.0;\n"
                              "    col += src.sample(s, uv + ts * float2(-1.0, 1.0)).rgb;\n"
                              "    col += src.sample(s, uv + ts * float2(0.0, 1.0)).rgb * 2.0;\n"
                              "    col += src.sample(s, uv + ts * float2(1.0, 1.0)).rgb;\n"
                              "    return float4(bloom_safe_color(col * (1.0 / 16.0)), 1.0);\n"
                              "}\n"],
                [NSString
                    stringWithUTF8String:
                        "struct TAAParams {\n"
                        "    float4x4 invViewProjection;\n"
                        "    float4x4 prevViewProjection;\n"
                        "    float2 invResolution;\n"
                        "    float2 jitterDelta;\n"
                        "    float blend;\n"
                        "    int historyValid;\n"
                        "    float2 pad0;\n"
                        "};\n"
                        "fragment float4 taa_resolve_fs(FullscreenVert in [[stage_in]],\n"
                        "    texture2d<float> currTex [[texture(0)]],\n"
                        "    texture2d<float> histTex [[texture(1)]],\n"
                        "    texture2d<float> motionTex [[texture(2)]],\n"
                        "    depth2d<float> depthTex [[texture(3)]],\n"
                        "    sampler s [[sampler(0)]],\n"
                        "    constant TAAParams &p [[buffer(0)]]) {\n"
                        "    float3 curr = currTex.sample(s, in.uv).rgb;\n"
                        "    if (p.historyValid == 0)\n"
                        "        return float4(curr, 1.0);\n"
                        "    float3 motionSample = motionTex.sample(s, in.uv).rgb;\n"
                        "    float2 velocity = motionSample.xy * 2.0 - 1.0;\n"
                        "    if (motionSample.z < 0.5) {\n"
                        /* Static geometry: analytic camera reprojection through the
                         * current inverse VP and the previous VP. */
                        "        float depth = depthTex.sample(s, in.uv);\n"
                        /* Same top-left <-> Y-up NDC flips as reconstructWorld/cameraVelocity. */
                        "        float4 clip = float4(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0, "
                        "depth * 2.0 - 1.0, 1.0);\n"
                        "        float4 world = p.invViewProjection * clip;\n"
                        "        world /= max(abs(world.w), 0.0001) * (world.w < 0.0 ? -1.0 : "
                        "1.0);\n"
                        "        float4 prevClip = p.prevViewProjection * float4(world.xyz, "
                        "1.0);\n"
                        "        float prevW = (prevClip.w < 0.0 ? -1.0 : 1.0) * "
                        "max(fabs(prevClip.w), 0.0001);\n"
                        "        float2 prevUv = float2(prevClip.x / prevW * 0.5 + 0.5, "
                        "0.5 - prevClip.y / prevW * 0.5);\n"
                        "        velocity = in.uv - prevUv;\n"
                        "    }\n"
                        /* Motion vectors were rendered under projection jitter; remove the
                         * current-minus-previous jitter delta before reprojecting. */
                        "    velocity -= p.jitterDelta;\n"
                        "    float2 histUv = in.uv - velocity;\n"
                        "    if (histUv.x < 0.0 || histUv.x > 1.0 || histUv.y < 0.0 || "
                        "histUv.y > 1.0)\n"
                        "        return float4(curr, 1.0);\n"
                        "    float3 hist = histTex.sample(s, histUv).rgb;\n"
                        "    float3 cmin = curr;\n"
                        "    float3 cmax = curr;\n"
                        "    for (int dy = -1; dy <= 1; dy++)\n"
                        "        for (int dx = -1; dx <= 1; dx++) {\n"
                        "            if (dx == 0 && dy == 0) continue;\n"
                        "            float3 n = currTex.sample(s, in.uv + float2(dx, dy) * "
                        "p.invResolution).rgb;\n"
                        "            cmin = min(cmin, n);\n"
                        "            cmax = max(cmax, n);\n"
                        "        }\n"
                        "    hist = clamp(hist, cmin, cmax);\n"
                        "    float w = clamp(p.blend, 0.0, 0.98);\n"
                        "    w *= 1.0 - clamp(length(velocity) * 8.0, 0.0, 0.85);\n"
                        "    return float4(mix(curr, hist, w), 1.0);\n"
                        "}\n"],
                [NSString
                    stringWithUTF8String:
                        "struct SSRParams {\n"
                        "    float4x4 invViewProjection;\n"
                        "    float4x4 viewProjection;\n"
                        "    float4 camPos;\n"
                        "    float4 params0;\n"
                        "    float2 invResolution;\n"
                        "    float2 pad1;\n"
                        "};\n"
                        "float3 ssr_safe_normalize3(float3 v, float3 fallback) {\n"
                        "    float len2 = dot(v, v);\n"
                        "    return (len2 > 1e-12 && len2 < 1e20) ? v * rsqrt(len2) : fallback;\n"
                        "}\n"
                        "float3 ssr_world_at(float2 uv, float depth, constant SSRParams &p) {\n"
                        "    float4 clip = float4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);\n"
                        "    float4 world = p.invViewProjection * clip;\n"
                        "    float w = (world.w < 0.0 ? -1.0 : 1.0) * max(fabs(world.w), 0.0001);\n"
                        "    return world.xyz / w;\n"
                        "}\n"
                        "fragment float4 ssr_fs(FullscreenVert in [[stage_in]],\n"
                        "    texture2d<float> sceneTex [[texture(0)]],\n"
                        "    depth2d<float> depthTex [[texture(1)]],\n"
                        "    texture2d<float> motionTex [[texture(2)]],\n"
                        "    sampler s [[sampler(0)]],\n"
                        "    constant SSRParams &p [[buffer(0)]]) {\n"
                        "    float3 base = sceneTex.sample(s, in.uv).rgb;\n"
                        "    float mask = motionTex.sample(s, in.uv).a;\n"
                        "    float depth = depthTex.sample(s, in.uv);\n"
                        /* Reversed-Z: cleared/far depth is 0 and nearer is larger, so the sky
                         * guard and every ray-vs-scene comparison below flip direction. */
                        "    if (mask <= 0.004 || depth <= 0.0001)\n"
                        "        return float4(base, 1.0);\n"
                        "    float3 world = ssr_world_at(in.uv, depth, p);\n"
                        "    float2 du = float2(p.invResolution.x, 0.0);\n"
                        "    float2 dv = float2(0.0, p.invResolution.y);\n"
                        "    float3 wx = ssr_world_at(in.uv + du, depthTex.sample(s, in.uv + du), "
                        "p);\n"
                        "    float3 wy = ssr_world_at(in.uv + dv, depthTex.sample(s, in.uv + dv), "
                        "p);\n"
                        "    float3 N = ssr_safe_normalize3(cross(wx - world, wy - world), "
                        "float3(0.0, 1.0, 0.0));\n"
                        "    float3 V = ssr_safe_normalize3(world - p.camPos.xyz, float3(0.0, 0.0, "
                        "-1.0));\n"
                        "    if (dot(N, V) > 0.0)\n"
                        "        N = -N;\n"
                        "    float3 R = reflect(V, N);\n"
                        "    float2 px = in.uv / p.invResolution;\n"
                        "    float ign = fract(52.9829189 * fract(0.06711056 * px.x + 0.00583715 * "
                        "px.y));\n"
                        "    int steps = clamp(int(p.params0.z), 8, 48);\n"
                        "    float t = 0.05 + 0.08 * ign;\n"
                        "    float prevT = 0.0;\n"
                        "    float2 hitUv = float2(-1.0, -1.0);\n"
                        "    bool hit = false;\n"
                        "    for (int i = 0; i < steps; i++) {\n"
                        "        float3 pos = world + R * t;\n"
                        "        float4 pc = p.viewProjection * float4(pos, 1.0);\n"
                        "        if (pc.w <= 0.0001)\n"
                        "            break;\n"
                        "        float3 ndc = pc.xyz / pc.w;\n"
                        "        float2 uv2 = ndc.xy * 0.5 + 0.5;\n"
                        "        if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0)\n"
                        "            break;\n"
                        "        float rayD = ndc.z * 0.5 + 0.5;\n"
                        "        float sceneD = depthTex.sample(s, uv2);\n"
                        "        if (rayD < sceneD - 0.0002) {\n"
                        "            float lo = prevT;\n"
                        "            float hi = t;\n"
                        "            for (int r = 0; r < 4; r++) {\n"
                        "                float mid = (lo + hi) * 0.5;\n"
                        "                float4 mc = p.viewProjection * float4(world + R * mid, "
                        "1.0);\n"
                        "                float3 mndc = mc.xyz / max(mc.w, 0.0001);\n"
                        "                float2 muv = mndc.xy * 0.5 + 0.5;\n"
                        "                if (mndc.z * 0.5 + 0.5 < depthTex.sample(s, muv) - "
                        "0.0002)\n"
                        "                    hi = mid;\n"
                        "                else\n"
                        "                    lo = mid;\n"
                        "            }\n"
                        "            float4 hc = p.viewProjection * float4(world + R * hi, 1.0);\n"
                        "            float3 hndc = hc.xyz / max(hc.w, 0.0001);\n"
                        "            hitUv = hndc.xy * 0.5 + 0.5;\n"
                        "            float hitRayD = hndc.z * 0.5 + 0.5;\n"
                        "            float hitSceneD = depthTex.sample(s, hitUv);\n"
                        "            hit = (hitSceneD - hitRayD) < 0.02;\n"
                        "            break;\n"
                        "        }\n"
                        "        prevT = t;\n"
                        "        t *= 1.35;\n"
                        "    }\n"
                        "    if (!hit || hitUv.x < 0.0)\n"
                        "        return float4(base, 1.0);\n"
                        "    float2 edge = min(hitUv, 1.0 - hitUv);\n"
                        "    float edgeFade = clamp(min(edge.x, edge.y) * 8.0, 0.0, 1.0);\n"
                        "    float ndv = clamp(dot(-V, N), 0.0, 1.0);\n"
                        "    float fres = 0.2 + 0.8 * pow(1.0 - ndv, 2.0);\n"
                        "    float amt = clamp(p.params0.x, 0.0, 1.0) * mask * edgeFade * fres;\n"
                        "    float3 refl = sceneTex.sample(s, hitUv).rgb;\n"
                        "    return float4(mix(base, refl, amt), 1.0);\n"
                        "}\n"]
            ];
            NSString *postfxSrc = [postfxChunks componentsJoinedByString:@ ""];

            NSError *pfxErr = nil;
            ctx.postfxLibrary = [device newLibraryWithSource:postfxSrc options:nil error:&pfxErr];
            if (ctx.postfxLibrary) {
                id<MTLFunction> pfxVS = [ctx.postfxLibrary newFunctionWithName:@ "fullscreen_vs"];
                id<MTLFunction> pfxFS = [ctx.postfxLibrary newFunctionWithName:@ "postfx_fs"];
                if (pfxVS && pfxFS) {
                    MTLRenderPipelineDescriptor *ppd = [[MTLRenderPipelineDescriptor alloc] init];
                    ppd.vertexFunction = pfxVS;
                    ppd.fragmentFunction = pfxFS;
                    /* ADR 0247 / V1b: chain intermediates ping-pong through RGBA16F
                     * so passes BEFORE the tonemap keep >1.0 energy (ACES' shoulder
                     * works live); the last pass resolves into the BGRA8 target the
                     * present blit requires. Two pipeline variants, picked by the
                     * pass's target format. */
                    ppd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                    ctx.postfxPipeline =
                        [device newRenderPipelineStateWithDescriptor:ppd error:&pfxErr];
                    ppd.colorAttachments[0].pixelFormat =
                        metal_color_pixel_format(VGFX3D_METAL_COLOR_FORMAT_HDR16F);
                    ctx.postfxPipelineHdr =
                        [device newRenderPipelineStateWithDescriptor:ppd error:&pfxErr];
                }
                /* Plan 05: bloom mip-chain pipelines (RGBA16F targets; the upsample pass
                 * accumulates via additive blending) and the TAA resolve pipeline. */
                if (pfxVS) {
                    id<MTLFunction> downFS =
                        [ctx.postfxLibrary newFunctionWithName:@ "bloom_down_fs"];
                    id<MTLFunction> upFS = [ctx.postfxLibrary newFunctionWithName:@ "bloom_up_fs"];
                    id<MTLFunction> taaFS =
                        [ctx.postfxLibrary newFunctionWithName:@ "taa_resolve_fs"];
                    MTLPixelFormat hdrFmt =
                        metal_color_pixel_format(VGFX3D_METAL_COLOR_FORMAT_HDR16F);
                    if (downFS) {
                        MTLRenderPipelineDescriptor *bd =
                            [[MTLRenderPipelineDescriptor alloc] init];
                        bd.vertexFunction = pfxVS;
                        bd.fragmentFunction = downFS;
                        bd.colorAttachments[0].pixelFormat = hdrFmt;
                        ctx.bloomDownPipeline =
                            [device newRenderPipelineStateWithDescriptor:bd error:&pfxErr];
                    }
                    if (upFS) {
                        MTLRenderPipelineDescriptor *bu =
                            [[MTLRenderPipelineDescriptor alloc] init];
                        bu.vertexFunction = pfxVS;
                        bu.fragmentFunction = upFS;
                        bu.colorAttachments[0].pixelFormat = hdrFmt;
                        bu.colorAttachments[0].blendingEnabled = YES;
                        bu.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
                        bu.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
                        bu.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
                        bu.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
                        bu.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
                        bu.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
                        ctx.bloomUpPipeline =
                            [device newRenderPipelineStateWithDescriptor:bu error:&pfxErr];
                    }
                    if (taaFS) {
                        MTLRenderPipelineDescriptor *td =
                            [[MTLRenderPipelineDescriptor alloc] init];
                        td.vertexFunction = pfxVS;
                        td.fragmentFunction = taaFS;
                        td.colorAttachments[0].pixelFormat = hdrFmt;
                        ctx.taaResolvePipeline =
                            [device newRenderPipelineStateWithDescriptor:td error:&pfxErr];
                    }
                    {
                        id<MTLFunction> ssrFS = [ctx.postfxLibrary newFunctionWithName:@ "ssr_fs"];
                        if (ssrFS) {
                            MTLRenderPipelineDescriptor *sd =
                                [[MTLRenderPipelineDescriptor alloc] init];
                            sd.vertexFunction = pfxVS;
                            sd.fragmentFunction = ssrFS;
                            /* SSR runs pre-tonemap, into the RGBA16F ping-pong (V1b). */
                            sd.colorAttachments[0].pixelFormat =
                                metal_color_pixel_format(VGFX3D_METAL_COLOR_FORMAT_HDR16F);
                            ctx.ssrPipeline =
                                [device newRenderPipelineStateWithDescriptor:sd error:&pfxErr];
                        }
                    }
                }
            }
            /* Non-fatal: postfx disabled if pipeline compilation fails */
        }

        {
            NSString *skyboxSrc =
                @
                 "#include <metal_stdlib>\n"
                 "using namespace metal;\n"
                 "struct SkyboxOut { float4 position [[position]]; float2 ndc; };\n"
                 "struct SkyboxParams { float4x4 inverseProjection; float4x4 "
                 "inverseViewRotation; float4 cameraForward; };\n"
                 "float3 skybox_safe_normalize3(float3 v, float3 fallback) {\n"
                 "    float len2 = dot(v, v);\n"
                 "    return (len2 > 1e-12 && len2 < 1e20) ? v * rsqrt(len2) : fallback;\n"
                 "}\n"
                 "vertex SkyboxOut skybox_vs(uint vid [[vertex_id]]) {\n"
                 "    SkyboxOut out;\n"
                 "    float2 clip;\n"
                 "    if (vid == 0) clip = float2(-1.0, -1.0);\n"
                 "    else if (vid == 1) clip = float2(3.0, -1.0);\n"
                 "    else clip = float2(-1.0, 3.0);\n"
                 /* Reversed-Z: the far plane is window depth 0; drawn after opaques with a
                  * GreaterEqual read-only depth state so only uncovered pixels shade. */
                 "    out.position = float4(clip, 0.0, 1.0);\n"
                 "    out.ndc = clip;\n"
                 "    return out;\n"
                 "}\n"
                 "fragment float4 skybox_fs(SkyboxOut in [[stage_in]], constant SkyboxParams &p "
                 "[[buffer(0)]], texturecube<float> skyboxTex [[texture(0)]], sampler s "
                 "[[sampler(0)]]) {\n"
                 "    float3 worldDir;\n"
                 "    if (p.cameraForward.w > 0.5) {\n"
                 "        worldDir = skybox_safe_normalize3(p.cameraForward.xyz, "
                 "float3(0.0, 0.0, -1.0));\n"
                 "    } else {\n"
                 "        float4 clip = float4(in.ndc, 1.0, 1.0);\n"
                 "        float4 view = p.inverseProjection * clip;\n"
                 "        float3 viewDir = skybox_safe_normalize3(view.xyz / "
                 "max(fabs(view.w), 0.0001), float3(0.0, 0.0, -1.0));\n"
                 "        worldDir = skybox_safe_normalize3((p.inverseViewRotation * "
                 "float4(viewDir, 0.0)).xyz, viewDir);\n"
                 "    }\n"
                 "    return skyboxTex.sample(s, worldDir);\n"
                 "}\n";
            NSError *skyErr = nil;
            id<MTLLibrary> skyLib =
                [device newLibraryWithSource:skyboxSrc options:nil error:&skyErr];
            if (skyLib) {
                id<MTLFunction> skyVS = [skyLib newFunctionWithName:@ "skybox_vs"];
                id<MTLFunction> skyFS = [skyLib newFunctionWithName:@ "skybox_fs"];
                if (skyVS && skyFS) {
                    ctx.skyboxPipeline = metal_create_skybox_pipeline_state(
                        device, skyVS, skyFS, MTLPixelFormatRGBA16Float, &skyErr);
                    ctx.skyboxColorPipeline = metal_create_skybox_pipeline_state(
                        device, skyVS, skyFS, MTLPixelFormatBGRA8Unorm, &skyErr);
                }
            }
            ctx.skyboxVertexBuffer = [device newBufferWithBytes:metal_skybox_vertices
                                                         length:sizeof(metal_skybox_vertices)
                                                        options:MTLResourceStorageModeShared];
        }

        if (!metal_recreate_main_targets(ctx, w, h)) {
            [ctx.metalLayer removeFromSuperlayer];
            ctx.metalLayer = nil;
            return NULL;
        }

        /* Backend initialized successfully */
        return (__bridge_retained void *)ctx;
    }
}

/// @brief Destroy the Metal backend context and release all GPU resources and caches.
/// @param[in] ctx_ptr Opaque retained Metal context handle to consume and release.
static void metal_destroy_ctx(void *ctx_ptr) {
    if (!ctx_ptr)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge_transfer VGFXMetalContext *)ctx_ptr;
        metal_detach_render_target(ctx, YES);
        metal_clear_render_target_sync_callbacks(ctx);
        metal_free_gpu_postfx_chain(ctx);
        [ctx.metalLayer removeFromSuperlayer];
        ctx.metalLayer = nil;
        (void)ctx; /* ARC releases all properties */
    }
}

/// @brief Set the clear color used when the next frame's scene pass begins.
/// @param[in,out] ctx_ptr Opaque Metal context handle.
/// @param[in] win Window associated with the clear request; unused by Metal.
/// @param[in] r Requested red component, clamped to the unit interval.
/// @param[in] g Requested green component, clamped to the unit interval.
/// @param[in] b Requested blue component, clamped to the unit interval.
static void metal_clear(void *ctx_ptr, vgfx_window_t win, float r, float g, float b) {
    (void)win;
    VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
    if (!ctx)
        return;
    ctx.clearR = vgfx3d_clamp_float_param(r, 0.0f, 1.0f, 0.0f);
    ctx.clearG = vgfx3d_clamp_float_param(g, 0.0f, 1.0f, 0.0f);
    ctx.clearB = vgfx3d_clamp_float_param(b, 0.0f, 1.0f, 0.0f);
}

/// @brief Radical-inverse Halton sequence sample for TAA sub-pixel jitter.
/// @param[in] index One-based sequence index.
/// @param[in] base Radix whose digits are reflected.
/// @return Radical-inverse sample in the half-open interval `[0, 1)`.
static float metal_halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

/// @brief True when the exported postfx chain contains an effect of @p type_value.
/// @param[in] chain Post-processing chain to inspect.
/// @param[in] type_value Effect type to locate.
/// @return One when the usable chain contains the requested type.
static int metal_chain_has_effect(const vgfx3d_postfx_chain_t *chain, int32_t type_value) {
    if (!vgfx3d_postfx_chain_is_usable(chain))
        return 0;
    for (int32_t i = 0; i < chain->effect_count; i++) {
        if (chain->effects[i].type == type_value)
            return 1;
    }
    return 0;
}

/// @brief Backend `resolve_opaque_targets` op — snapshot the scene depth at the
///   opaque->transparent seam. Metal cannot sample the live depth attachment, so
///   the scene encoder is closed, the depth texture is blit-copied into a
///   sampleable snapshot, and the encoder is reopened with load-existing
///   color+depth. Blend draws then read the snapshot for soft-particle fade.
/// @param[in,out] ctx_ptr Opaque Metal context whose opaque-depth snapshot is refreshed.
static void metal_resolve_opaque_targets(void *ctx_ptr) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        id<MTLTexture> depth;
        id<MTLBlitCommandEncoder> blit;
        if (!ctx || !ctx.cmdBuf)
            return;
        if (ctx.currentTargetKind == VGFX3D_METAL_TARGET_OVERLAY)
            return; /* HUD passes have no opaque scene to fade against */
        depth = ctx.rttActive ? ctx.rttDepthTexture : ctx.depthTexture;
        if (!depth)
            return;
        if (!ctx.opaqueDepthTexture || ctx.opaqueDepthTexture.width != depth.width ||
            ctx.opaqueDepthTexture.height != depth.height) {
            MTLTextureDescriptor *desc =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                   width:depth.width
                                                                  height:depth.height
                                                               mipmapped:NO];
            desc.usage = MTLTextureUsageShaderRead;
            desc.storageMode = MTLStorageModePrivate;
            ctx.opaqueDepthTexture = [ctx.device newTextureWithDescriptor:desc];
            if (!ctx.opaqueDepthTexture)
                return;
        }
        if (ctx.encoder) {
            [ctx.encoder endEncoding];
            ctx.encoder = nil;
        }
        blit = [ctx.cmdBuf blitCommandEncoder];
        if (blit) {
            [blit copyFromTexture:depth
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(depth.width, depth.height, 1)
                        toTexture:ctx.opaqueDepthTexture
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
        }
        metal_begin_scene_encoder(ctx, YES, YES);
        ctx->_opaqueDepthValid = (ctx.encoder && blit) ? 1 : 0;
    }
}

/// @brief 1x1 depth stand-in bound when no snapshot exists, keeping fragment
///        texture 16 valid for the shared fragment shader (never sampled:
///        pbrScalars1.z is forced to 0 while the snapshot is invalid).
/// @param[in,out] ctx Context that lazily owns the dummy texture.
/// @return Borrowed one-by-one depth texture, or `nil` if allocation fails.
static id<MTLTexture> metal_opaque_depth_dummy(VGFXMetalContext *ctx) {
    if (!ctx.opaqueDepthDummy) {
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        ctx.opaqueDepthDummy = [ctx.device newTextureWithDescriptor:desc];
    }
    return ctx.opaqueDepthDummy;
}

/// @brief Begin a Metal frame by latching camera state and opening the selected scene encoder.
/// @details Sanitizes matrices, applies temporal jitter when TAA is latched, updates
///   scene and overlay history, acquires or reuses a command buffer, periodically
///   prunes caches, and begins the RTT, scene, overlay, or drawable render pass.
/// @param[in,out] ctx_ptr Opaque Metal context handle.
/// @param[in] cam Camera and per-frame rendering parameters to sanitize and latch.
static void metal_begin_frame(void *ctx_ptr, const vgfx3d_camera_params_t *cam) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        BOOL new_command_buffer = NO;
        float safe_cam_forward[3];
        float safe_cam_pos[3];
        float safe_view[16];
        float safe_projection[16];
        float inv_vp[16];
        vgfx3d_metal_frame_history_t history;
        vgfx3d_camera_params_t safe_cam;
        int8_t load_existing_color;
        int8_t is_overlay_pass;
        if (!ctx || !cam)
            return;
        vgfx3d_sanitize_camera_params(cam, &safe_cam);
        cam = &safe_cam;

        ctx.frameSerial++;
        /* Plan 07: cluster tables are camera-dependent; a light revision that
         * survives into a new frame does not imply the same froxel contents. */
        memset(ctx->_clusterBufferRevisions, 0, sizeof(ctx->_clusterBufferRevisions));
        /* Plan 10: the opaque depth snapshot is per-pass state. */
        ctx->_opaqueDepthValid = 0;
        /* Depth-probe slot ids restart each frame; published results stay readable. */
        if (!cam->load_existing_color)
            ctx->_depthProbeRequestCount = 0;
        ctx->_camNear = cam->znear;
        ctx->_camFar = cam->zfar;
        float vp[16];
        vgfx3d_copy_mat4_finite_or_identity(safe_view, cam->view);
        vgfx3d_copy_mat4_finite_or_identity(safe_projection, cam->projection);
        vgfx3d_copy_float_array_finite_or(safe_cam_pos, cam->position, 3u, 0.0f);
        vgfx3d_copy_vec3_direction_or(safe_cam_forward, cam->forward, NULL);
        mat4f_mul(safe_projection, safe_view, vp);
        /* Plan 05 TAA: apply Halton(2,3) sub-pixel jitter to the scene pass VP when the
         * latched chain contains a TAA resolve. Jitter is injected before inv/prev VP
         * derivation so every consumer this frame sees one consistent (jittered) VP; the
         * resolve pass removes the current-minus-previous jitter delta from velocities.
         * Overlay passes (load_existing_color) keep the previous state so UI stays fixed
         * and the per-frame jitter sequence advances exactly once per frame. */
        if (!ctx.rttActive && !cam->load_existing_color) {
            ctx->_taaPrevJitterClip[0] = ctx->_taaJitterClip[0];
            ctx->_taaPrevJitterClip[1] = ctx->_taaJitterClip[1];
            ctx->_taaJitterClip[0] = 0.0f;
            ctx->_taaJitterClip[1] = 0.0f;
            ctx->_taaJitterActive = NO;
            if (ctx.gpuPostfxEnabled && ctx.gpuPostfxChainValid && ctx.taaResolvePipeline &&
                ctx.depthTexture.width > 0 && ctx.depthTexture.height > 0) {
                vgfx3d_postfx_chain_t chain = ctx.gpuPostfxChain;
                if (metal_chain_has_effect(&chain, (int32_t)VGFX3D_POSTFX_EFFECT_TAA)) {
                    uint32_t idx = (ctx->_taaFrameIndex++ & 7u) + 1u;
                    float jx = (metal_halton(idx, 2) - 0.5f) * 2.0f / (float)ctx.depthTexture.width;
                    float jy =
                        (metal_halton(idx, 3) - 0.5f) * 2.0f / (float)ctx.depthTexture.height;
                    ctx->_taaJitterClip[0] = jx;
                    ctx->_taaJitterClip[1] = jy;
                    ctx->_taaJitterActive = YES;
                    /* Row-major clip-space translate: x-row += jx * w-row, y-row += jy * w-row. */
                    for (int jc = 0; jc < 4; jc++) {
                        vp[0 * 4 + jc] += jx * vp[3 * 4 + jc];
                        vp[1 * 4 + jc] += jy * vp[3 * 4 + jc];
                    }
                }
            }
        }
        if (!vgfx3d_float_array_is_bounded(vp, 16u, VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX) ||
            vgfx3d_invert_matrix4(vp, inv_vp) != 0) {
            mat4f_identity(vp);
            mat4f_identity(inv_vp);
            ctx->_taaJitterClip[0] = 0.0f;
            ctx->_taaJitterClip[1] = 0.0f;
            ctx->_taaPrevJitterClip[0] = 0.0f;
            ctx->_taaPrevJitterClip[1] = 0.0f;
            ctx.taaHistoryValid = 0;
            history = ctx.frameHistory;
            history.scene_history_valid = 0;
            ctx.frameHistory = history;
        }

        ctx.currentTargetKind = vgfx3d_metal_choose_target_kind(
            ctx.rttActive ? 1 : 0, metal_window_scene_route_enabled(ctx), cam->load_existing_color);
        if (!ctx.rttActive && cam->load_existing_color && ctx.postfxCompositedToDrawable)
            ctx.currentTargetKind = VGFX3D_METAL_TARGET_SWAPCHAIN;
        is_overlay_pass = ctx.currentTargetKind == VGFX3D_METAL_TARGET_OVERLAY ? 1 : 0;
        /* Every pass — the overlay included — starts with a fresh upload budget. Sharing the
         * scene pass's remainder let a heavy cut pause HUD image quads (drawn as nothing
         * since they became texture-required) for the frame; OpenGL/D3D11 reset the same
         * way. */
        ctx.textureUploadBytes = 0;
        history = ctx.frameHistory;
        load_existing_color = vgfx3d_metal_should_load_existing_color(
            ctx.currentTargetKind, cam->load_existing_color, history.overlay_used_this_frame);
        vgfx3d_metal_update_frame_history(&history,
                                          vp,
                                          inv_vp,
                                          safe_cam_pos,
                                          is_overlay_pass,
                                          ctx.currentTargetKind == VGFX3D_METAL_TARGET_OVERLAY);
        ctx.frameHistory = history;

        memcpy(ctx->_view, safe_view, sizeof(float) * 16);
        memcpy(ctx->_projection, safe_projection, sizeof(float) * 16);
        memcpy(ctx->_vp, vp, sizeof(float) * 16);
        memcpy(ctx->_prevVP, ctx.frameHistory.draw_prev_vp, sizeof(ctx->_prevVP));
        memcpy(ctx->_invVP, inv_vp, sizeof(ctx->_invVP));
        memcpy(ctx->_camPos, safe_cam_pos, sizeof(safe_cam_pos));
        memcpy(ctx->_camForward, safe_cam_forward, sizeof(safe_cam_forward));
        ctx->_camIsOrtho = cam->is_ortho ? YES : NO;
        ctx->_prevVPValid = ctx.frameHistory.scene_history_valid ? YES : NO;

        /* MTL-07: Store fog parameters for submit_draw */
        ctx->_fogEnabled = cam->fog_enabled;
        ctx->_heightFog[0] = cam->height_fog_base;
        ctx->_heightFog[1] = cam->height_fog_falloff;
        ctx->_heightFog[2] =
            cam->height_fog_enabled ? cam->height_fog_density * cam->height_fog_blend : 0.0f;
        ctx->_heightFog[3] = 0.0f;
        ctx->_heightFogSun[0] = cam->height_fog_sun_color[0];
        ctx->_heightFogSun[1] = cam->height_fog_sun_color[1];
        ctx->_heightFogSun[2] = cam->height_fog_sun_color[2];
        ctx->_heightFogSun[3] = cam->height_fog_enabled ? cam->height_fog_sun_amount : 0.0f;
        ctx->_heightFogSunDir[0] = cam->height_fog_sun_dir[0];
        ctx->_heightFogSunDir[1] = cam->height_fog_sun_dir[1];
        ctx->_heightFogSunDir[2] = cam->height_fog_sun_dir[2];
        ctx->_heightFogSunDir[3] = cam->height_fog_sun_power;
        ctx->_fogNear = cam->fog_near;
        ctx->_fogFar = cam->fog_far;
        ctx->_fogColor[0] = cam->fog_color[0];
        ctx->_fogColor[1] = cam->fog_color[1];
        ctx->_fogColor[2] = cam->fog_color[2];
        ctx->_iblEnabled = cam->ibl_enabled ? YES : NO;
        ctx->_iblIntensity = cam->ibl_intensity;
        memcpy(ctx->_iblSh, cam->ibl_sh, sizeof(ctx->_iblSh));
        ctx->_shadowStrength = cam->shadow_strength;
        ctx->_shadowSlopeBias = cam->shadow_slope_bias;
        ctx->_shadowQuality = cam->shadow_quality;

        /* Reuse the command buffer if one is already open (multi-pass frame).
         * RTT end_frame commits and nils the cmdBuf, so on-screen passes
         * after an RTT pass will create a fresh one. */
        if (!ctx.cmdBuf) {
            ctx.cmdBuf = metal_acquire_command_buffer(ctx);
            if (!ctx.cmdBuf)
                return;
            ctx.frameBuffers = [NSMutableArray arrayWithCapacity:32];
            ctx.drawable = nil;
            ctx.displayTexture = nil;
            ctx.postfxEncodedThisFrame = 0;
            ctx.postfxCompositedToDrawable = 0;
            new_command_buffer = YES;
        } else if (!ctx.frameBuffers)
            ctx.frameBuffers = [NSMutableArray arrayWithCapacity:32];
        if (new_command_buffer || !is_overlay_pass) {
            ctx->_shadowPassSlot = -1;
            ctx->_shadowCount = 0;
            memset(ctx->_shadowComplete, 0, sizeof(ctx->_shadowComplete));
        }

        if ((ctx.frameSerial & 31u) == 0u) {
            metal_prune_texture_cache(ctx);
            metal_prune_cubemap_cache(ctx);
            metal_prune_morph_cache(ctx);
            metal_prune_render_target_cache(ctx);
        }

        metal_begin_scene_encoder(ctx, load_existing_color, cam->load_existing_depth);
        if (!ctx.encoder) {
            /* Scene target unavailable — drawable acquisition routinely fails during window
             * resize/occlusion. Abandoning the open command buffer here would leak this
             * frame's state into the next begin_frame (whose per-frame resets only run when
             * cmdBuf is nil); commit it empty so the next frame starts clean. */
            metal_commit_pending(ctx, NO);
            return;
        }
        ctx.inFrame = YES;
    }
}

/// @brief Add @p bytes to the context's running per-frame texture-upload total (saturating).
/// @param[in,out] ctx Context containing the upload counter.
/// @param[in] bytes Completed upload bytes to add.
static void metal_record_texture_upload_bytes(VGFXMetalContext *ctx, uint64_t bytes) {
    if (!ctx || bytes == 0)
        return;
    if (ctx.textureUploadBytes > UINT64_MAX - bytes) {
        ctx.textureUploadBytes = UINT64_MAX;
        return;
    }
    ctx.textureUploadBytes += bytes;
}

/// @brief Report which native compressed texture formats this Metal device can sample
/// (BC7/ASTC/ETC2).
/// @details Apple-silicon GPUs support ASTC/ETC2; Intel Macs support BC7. Returns the matching
///          RT_CANVAS3D_BACKEND_CAP_* bitmask.
/// @param[in] ctx_ptr Opaque Metal context handle.
/// @return Bitmask of supported compression and anisotropy capabilities.
static int64_t metal_get_native_texture_caps(void *ctx_ptr) {
    VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
    int64_t caps = 0;
    if (!ctx || !ctx.device)
        return 0;
    caps |= RT_CANVAS3D_BACKEND_CAP_ANISOTROPY;
#if defined(__aarch64__)
    caps |= RT_CANVAS3D_BACKEND_CAP_ASTC | RT_CANVAS3D_BACKEND_CAP_ETC2;
    /* Apple silicon exposes desktop BCn formats when the device reports BC support. */
    if ([ctx.device respondsToSelector:@selector(supportsBCTextureCompression)] &&
        ctx.device.supportsBCTextureCompression)
        caps |= RT_CANVAS3D_BACKEND_CAP_BC7 | RT_CANVAS3D_BACKEND_CAP_BC1 |
                RT_CANVAS3D_BACKEND_CAP_BC3 | RT_CANVAS3D_BACKEND_CAP_BC4 |
                RT_CANVAS3D_BACKEND_CAP_BC5;
#else
    if ([ctx.device respondsToSelector:@selector(supportsBCTextureCompression)] &&
        ctx.device.supportsBCTextureCompression)
        caps |= RT_CANVAS3D_BACKEND_CAP_BC7 | RT_CANVAS3D_BACKEND_CAP_BC1 |
                RT_CANVAS3D_BACKEND_CAP_BC3 | RT_CANVAS3D_BACKEND_CAP_BC4 |
                RT_CANVAS3D_BACKEND_CAP_BC5;
#endif
    return caps;
}

/// @brief Return Metal feature bits that depend on the concrete backend.
/// @details The Metal backend allocates its scene, bloom, and TAA history textures with
/// `MTLPixelFormatRGBA16Float`, so HDR scene color and temporal anti-aliasing are available
/// whenever the backend context exists.
/// @param ctx_ptr Metal backend context, currently unused.
/// @return RT_CANVAS3D_BACKEND_CAP_* feature bits supported by Metal.
static int64_t metal_get_feature_caps(void *ctx_ptr) {
    (void)ctx_ptr;
    return RT_CANVAS3D_BACKEND_CAP_HDR_SCENE | RT_CANVAS3D_BACKEND_CAP_TAA |
           RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET_SAMPLING;
}
