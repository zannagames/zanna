//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_metal_targets.inc
// Purpose: Metal cluster buffers, native target caches, pass descriptors, rebuilds, and readback.
// Key invariants:
//   - Size-dependent attachment sets publish transactionally after every required allocation.
//   - Dirty cached targets are synchronized or remain explicitly dirty when native storage evicts.
// Ownership/Lifetime:
//   - ARC-owned textures/buffers live in VGFXMetalContext or its cache-entry objects.
//   - Borrowed RenderTarget3D shells register a finalization hook that removes their cache entry.
// Links: vgfx3d_backend_metal.m, vgfx3d_backend_metal_present.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Metal attachment allocation, target caching, pass setup, and texture copies.
/// @details This implementation fragment owns the GPU resources that back window scenes and
///          runtime render targets. It also maintains clustered-light buffers, shadow-slot
///          readiness, cache lifetimes, mip generation, channel-order conversion, and readback.

/// @brief Fetch (or upload into the ring) the GPU buffer for a CPU cluster table.
/// @details Keyed by lights revision; keys reset each begin_frame because binning
///          is camera-dependent. The 4-deep ring keeps in-flight frames from
///          reading a buffer the CPU is rewriting.
/// @param ctx Borrowed Metal context owning the clustered-light buffer ring.
/// @param table Borrowed CPU cluster table to validate and upload.
/// @param expected_revision Light revision that @p table must describe.
/// @param light_count Number of lights expected by the current draw.
/// @return The matching or newly uploaded cluster buffer, or nil for unusable input/allocation.
static id<MTLBuffer> metal_cluster_buffer_for_table(VGFXMetalContext *ctx,
                                                    const vgfx3d_cluster_table_t *table,
                                                    uint32_t expected_revision,
                                                    int32_t light_count) {
    int32_t slot;
    if (!ctx || !vgfx3d_cluster_table_is_usable(table, expected_revision, light_count))
        return nil;
    for (int32_t i = 0; i < 4; i++) {
        if (ctx->_clusterBuffers[i] && ctx->_clusterBufferRevisions[i] == table->lights_revision)
            return ctx->_clusterBuffers[i];
    }
    slot = ctx->_clusterBufferCursor;
    ctx->_clusterBufferCursor = (slot + 1) % 4;
    if (!ctx->_clusterBuffers[slot]) {
        ctx->_clusterBuffers[slot] = [ctx.device newBufferWithLength:sizeof(vgfx3d_cluster_table_t)
                                                             options:MTLResourceStorageModeShared];
        if (!ctx->_clusterBuffers[slot])
            return nil;
    }
    memcpy([ctx->_clusterBuffers[slot] contents], table, sizeof(*table));
    ctx->_clusterBufferRevisions[slot] = table->lights_revision;
    return ctx->_clusterBuffers[slot];
}

/// @brief Zero-filled full-size stand-in bound when a draw has no cluster table,
///        keeping fragment buffer 3 valid for the shared fragment shader.
/// @param ctx Borrowed Metal context that owns the lazily allocated dummy buffer.
/// @return The reusable zero-initialized cluster buffer, or nil when allocation fails.
static id<MTLBuffer> metal_cluster_dummy_buffer(VGFXMetalContext *ctx) {
    if (!ctx->_clusterDummyBuffer)
        ctx->_clusterDummyBuffer = [ctx.device newBufferWithLength:sizeof(vgfx3d_cluster_table_t)
                                                           options:MTLResourceStorageModeShared];
    return ctx->_clusterDummyBuffer;
}

typedef struct {
    float dc[4];
    float sc[4];
    float ec[4];
    float scalars[4];
    float pbrScalars0[4];
    float pbrScalars1[4];
    int32_t flags0[4];
    int32_t flags1[4];
    int32_t pbrFlags[4];
    float splatScales[4];
    int32_t shadingModel;
    float customParams[12];
    /* MSL aligns the following int4 to 16 bytes; shadingModel + customParams
     * occupy 36, so pad 12 to keep the C layout byte-identical to PerMaterial
     * in the shader (a mismatch here shifts the UV-set/transform block and
     * breaks all textured sampling). */
    int32_t _pad_uv_block[3];
    int32_t textureUvSets0[4];
    int32_t textureUvSets1[4];
    float textureUvTransform0[RT_MATERIAL3D_TEXTURE_SLOT_COUNT][4];
    float textureUvTransform1[RT_MATERIAL3D_TEXTURE_SLOT_COUNT][4];
} mtl_per_material_t;

typedef struct {
    float model[16];
    float normal[16];
    float prev_model[16];
} mtl_instance_t;

typedef struct {
    float inverseProjection[16];
    float inverseViewRotation[16];
    float cameraForward[4];
} mtl_skybox_params_t;

//=============================================================================
// Helpers
//=============================================================================

/// @brief Transpose a row-major 4x4 float matrix: dst[c][r] = src[r][c] (@p src and @p dst must not
///   alias). Used to convert engine row-major matrices to the column-major layout Metal expects.
/// @param src Borrowed row-major 4x4 source matrix.
/// @param[out] dst Caller-owned 4x4 destination receiving the transpose.
static void transpose4x4(const float *src, float *dst) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            dst[c * 4 + r] = src[r * 4 + c];
}

/// @brief Row-major 4x4 matrix product out = a * b (out must not alias a or b).
/// @param a Borrowed left-hand 4x4 matrix.
/// @param b Borrowed right-hand 4x4 matrix.
/// @param[out] out Caller-owned 4x4 destination that does not alias either input.
static void mat4f_mul(const float *a, const float *b, float *out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

/// @brief Write the 4x4 identity matrix into @p out.
/// @param[out] out Caller-owned storage for 16 float components; may be NULL.
static void mat4f_identity(float *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(float) * 16);
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[15] = 1.0f;
}

/// @brief Whether the GPU storage backing shadow @p slot exists (per-slot depth
///   texture for the classic CSM slots, the shared atlas texture for tile slots).
/// @param ctx Borrowed Metal context containing shadow textures.
/// @param slot Shadow slot whose backing allocation is inspected.
/// @return Nonzero when the slot has the appropriate per-slot or atlas depth texture.
static int metal_shadow_slot_texture_ready(VGFXMetalContext *ctx, int32_t slot) {
    if (!ctx)
        return 0;
    if (slot >= VGFX3D_CSM_SLOTS)
        return ctx->_shadowAtlasTexture != nil;
    return ctx->_shadowDepthTexture[slot] != nil;
}

/// @brief Recount the contiguous run of completed shadow-map slots into ctx->_shadowCount.
/// @details Stops at the first slot that is incomplete or lacks backing storage, so only
///          a prefix of ready shadow maps is reported to the lighting pass. Atlas slots
///          (>= VGFX3D_CSM_SLOTS) are backed by the shared atlas texture, NOT a per-slot
///          depth texture — requiring _shadowDepthTexture there silently zeroed every
///          atlas slot out of _shadowCount and the extra spot/point-cube shadows never
///          reached the shaders.
/// @param ctx Borrowed Metal context whose contiguous ready-prefix count is recomputed.
static void metal_recompute_shadow_count(VGFXMetalContext *ctx) {
    int32_t count = 0;
    if (!ctx)
        return;
    for (int32_t slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++) {
        if (!ctx->_shadowComplete[slot] || !metal_shadow_slot_texture_ready(ctx, slot))
            break;
        count = slot + 1;
    }
    ctx->_shadowCount = count;
}

static const float metal_skybox_vertices[] = {
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

static const uint64_t k_texture_cache_max_age = 240u;
static const uint64_t k_cubemap_cache_max_age = 240u;
static const uint64_t k_morph_cache_max_age = 240u;
static const NSUInteger k_texture_cache_max_entries = 256u;
static const NSUInteger k_cubemap_cache_max_entries = 64u;
static const NSUInteger k_morph_cache_max_entries = 64u;

static int metal_copy_texture_to_rgba(VGFXMetalContext *ctx,
                                      id<MTLTexture> tex,
                                      uint8_t *dst_rgba,
                                      int32_t w,
                                      int32_t h,
                                      int32_t stride,
                                      float *dst_hdr_rgba);
static int metal_sync_render_target_color(void *userdata, vgfx3d_rendertarget_t *target);
static void metal_update_layer_size(VGFXMetalContext *ctx);
static int metal_capture_current_drawable_to_display_texture(VGFXMetalContext *ctx);
static id<MTLCommandBuffer> metal_acquire_command_buffer(VGFXMetalContext *ctx);
static void metal_commit_pending(VGFXMetalContext *ctx, BOOL waitUntilCompleted);
static id<MTLTexture> metal_encode_postfx_if_needed(VGFXMetalContext *ctx,
                                                    const vgfx3d_postfx_chain_t *postfx);

/// @brief Return whether Metal's next window-backed scene must render below logical output size.
/// @details The stored scale never changes explicit render-target dimensions because RTT target
///          selection takes precedence; this predicate remains independent of the currently bound
///          RTT so main-window targets can be rebuilt correctly while an RTT is bound.
/// @param ctx Borrowed Metal context.
/// @return Non-zero only for a finite reduced scale in the supported interval.
static int metal_render_scale_active(VGFXMetalContext *ctx) {
    return ctx && isfinite(ctx.renderScale) && ctx.renderScale >= 0.25f && ctx.renderScale < 0.999f;
}

/// @brief Return whether window rendering needs the offscreen scene/composite route.
/// @details GPU post-processing and reduced render scale share scene color, motion, and depth
///          targets. Keeping one predicate prevents target selection, readback, and presentation
///          from disagreeing about where the current frame lives.
/// @param ctx Borrowed Metal context.
/// @return Non-zero when post-processing or reduced render scale requires an offscreen scene.
static int8_t metal_window_scene_route_enabled(VGFXMetalContext *ctx) {
    return (ctx && (ctx.gpuPostfxEnabled || metal_render_scale_active(ctx))) ? 1 : 0;
}

/// @brief Maximum mip LOD index for a cubemap (0 if it has no mip chain).
/// @param cubemap Borrowed runtime cubemap whose validated face size determines the mip chain.
/// @return The zero-based maximum mip level, or 0 when no usable chain is available.
static float metal_cubemap_max_lod(const rt_cubemap3d *cubemap) {
    int32_t face_size;
    int32_t mip_count;

    if (!cubemap || !vgfx3d_get_cubemap_face_size(cubemap, &face_size) || face_size <= 1)
        return 0.0f;
    mip_count = vgfx3d_metal_compute_mip_count(face_size, face_size);
    return mip_count > 1 ? (float)(mip_count - 1) : 0.0f;
}

/// @brief Resolve whether a draw uses IBL environment lighting, producing the
///   prefiltered chain's lod-base and effective max-lod for the env sampler.
/// @details Non-IBL draws keep the full-pyramid max lod (legacy reflectivity
///   mix); IBL draws address only the GGX-prefiltered tail of the mip chain.
/// @param ctx Borrowed Metal context containing the cubemap cache and IBL state.
/// @param cmd Borrowed draw command whose environment-map request is resolved.
/// @param[out] out_lod_base Required destination for the first prefiltered mip level.
/// @param[out] out_max_lod Required destination for the effective sampler LOD range.
/// @return 1 when a complete cached IBL chain is usable, otherwise 0.
static int metal_cmd_ibl_draw(VGFXMetalContext *ctx,
                              const vgfx3d_draw_cmd_t *cmd,
                              float *out_lod_base,
                              float *out_max_lod) {
    const rt_cubemap3d *env_cm;
    VGFXMetalCubemapCacheEntry *entry;
    int32_t level_base;
    uint64_t generation;
    if (out_lod_base)
        *out_lod_base = 0.0f;
    if (out_max_lod)
        *out_max_lod = 0.0f;
    if (!ctx || !cmd || !out_lod_base || !out_max_lod)
        return 0;
    env_cm = (const rt_cubemap3d *)cmd->env_map;
    *out_max_lod = env_cm ? metal_cubemap_max_lod(env_cm) : 0.0f;
    if (!ctx->_iblEnabled || !cmd->ibl_env || !env_cm || !env_cm->ibl_ready ||
        env_cm->ibl_identity == 0 || env_cm->face_size <= 0 || env_cm->face_size > INT32_MAX ||
        env_cm->ibl_base_size <= 0 || env_cm->ibl_base_size > INT32_MAX ||
        env_cm->ibl_mip_count <= 0 || env_cm->ibl_mip_count > INT32_MAX ||
        !vgfx3d_validate_cubemap_ibl_layout((int32_t)env_cm->face_size,
                                            (int32_t)env_cm->ibl_base_size,
                                            (int32_t)env_cm->ibl_mip_count,
                                            RT_CUBEMAP3D_IBL_MAX_MIPS,
                                            &level_base)) {
        return 0;
    }
    generation = vgfx3d_get_cubemap_generation(env_cm);
    entry = ctx.cubemapCache[[NSValue valueWithPointer:env_cm]];
    if (!entry || !entry.texture || entry.generation != generation || entry.uploadInProgress ||
        entry.appliedIblIdentity != env_cm->ibl_identity) {
        return 0;
    }
    *out_lod_base = (float)level_base;
    *out_max_lod = (float)(env_cm->ibl_mip_count - 1);
    return 1;
}

/// @brief Map a runtime color-format enum to its Metal pixel format (HDR16F → RGBA16Float, else
/// BGRA8).
/// @param format Backend-neutral Metal color-format classification.
/// @return The corresponding `MTLPixelFormat`.
static MTLPixelFormat metal_color_pixel_format(vgfx3d_metal_color_format_t format) {
    return format == VGFX3D_METAL_COLOR_FORMAT_HDR16F ? MTLPixelFormatRGBA16Float
                                                      : MTLPixelFormatBGRA8Unorm;
}

/// @brief Create a 2D color texture with the given format/usage/storage (nil on invalid args).
/// @param ctx Borrowed Metal context providing the allocation device.
/// @param w Texture width in pixels.
/// @param h Texture height in pixels.
/// @param pixel_format Metal pixel format for the allocation.
/// @param usage Bitmask describing intended render, sample, or copy operations.
/// @param storage_mode Metal storage mode controlling CPU/GPU visibility.
/// @param mipmapped YES to allocate the complete mip chain.
/// @return A newly allocated texture, or nil for invalid input or allocation failure.
static id<MTLTexture> metal_new_color_texture(VGFXMetalContext *ctx,
                                              int32_t w,
                                              int32_t h,
                                              MTLPixelFormat pixel_format,
                                              MTLTextureUsage usage,
                                              MTLStorageMode storage_mode,
                                              BOOL mipmapped) {
    MTLTextureDescriptor *td;
    if (!ctx || !ctx.device || !vgfx3d_metal_is_valid_texture2d_extent(w, h))
        return nil;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                            width:(NSUInteger)w
                                                           height:(NSUInteger)h
                                                        mipmapped:mipmapped];
    td.usage = usage;
    td.storageMode = storage_mode;
    return [ctx.device newTextureWithDescriptor:td];
}

/// @brief Create a private-storage Depth32Float depth texture (nil on invalid args).
/// @param ctx Borrowed Metal context providing the allocation device.
/// @param w Texture width in pixels.
/// @param h Texture height in pixels.
/// @param usage Bitmask describing intended depth-texture operations.
/// @return A new private Depth32Float texture, or nil for invalid input/allocation failure.
static id<MTLTexture> metal_new_depth_texture(VGFXMetalContext *ctx,
                                              int32_t w,
                                              int32_t h,
                                              MTLTextureUsage usage) {
    MTLTextureDescriptor *td;
    if (!ctx || !ctx.device || !vgfx3d_metal_is_valid_texture2d_extent(w, h))
        return nil;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                            width:(NSUInteger)w
                                                           height:(NSUInteger)h
                                                        mipmapped:NO];
    td.usage = usage;
    td.storageMode = MTLStorageModePrivate;
    return [ctx.device newTextureWithDescriptor:td];
}

/// @brief Generate the full mip chain for a texture via a one-shot blit encoder.
/// @details The command buffer is committed without a CPU wait. It uses the same serial Metal
///   command queue as frame rendering, so a subsequently committed draw buffer cannot sample the
///   texture before mip generation completes. Waiting here turns every cache miss into a main-
///   thread/GPU round trip and causes persistent frame hitches when content streams in.
/// @param ctx Borrowed Metal context providing the serial command queue.
/// @param texture Borrowed mipmapped texture whose lower levels are generated.
/// @return YES when no generation is needed or the blit was committed, otherwise NO.
static BOOL metal_generate_mipmaps(VGFXMetalContext *ctx, id<MTLTexture> texture) {
    id<MTLCommandBuffer> cmd_buf;
    id<MTLBlitCommandEncoder> blit;

    if (!ctx || !texture)
        return NO;
    if (texture.mipmapLevelCount <= 1)
        return YES;
    cmd_buf = [ctx.commandQueue commandBuffer];
    if (!cmd_buf)
        return NO;
    blit = [cmd_buf blitCommandEncoder];
    if (!blit)
        return NO;
    [blit generateMipmapsForTexture:texture];
    [blit endEncoding];
    [cmd_buf commit];
    return YES;
}

/// @brief Evict texture-cache entries that are empty or older than the max-age threshold.
/// @details Aging is measured in frames against ctx.frameSerial so textures unused for ~240 frames
///          are released, bounding GPU memory.
/// @param ctx Borrowed Metal context whose 2D texture cache is pruned.
static void metal_prune_texture_cache(VGFXMetalContext *ctx) {
    NSMutableArray *keys_to_remove;

    if (!ctx || !ctx.textureCache)
        return;
    keys_to_remove = [NSMutableArray array];
    for (NSValue *key in ctx.textureCache) {
        VGFXMetalTextureCacheEntry *entry = ctx.textureCache[key];
        if (!entry || (!entry.texture && entry.failedGeneration == 0) ||
            vgfx3d_metal_should_prune_cache_entry(
                ctx.frameSerial, entry.lastUsedFrame, k_texture_cache_max_age)) {
            [keys_to_remove addObject:key];
        }
    }
    [ctx.textureCache removeObjectsForKeys:keys_to_remove];
}

/// @brief Evict cubemap-cache entries that are empty or older than the max-age threshold.
/// @param ctx Borrowed Metal context whose cubemap cache is pruned.
static void metal_prune_cubemap_cache(VGFXMetalContext *ctx) {
    NSMutableArray *keys_to_remove;

    if (!ctx || !ctx.cubemapCache)
        return;
    keys_to_remove = [NSMutableArray array];
    for (NSValue *key in ctx.cubemapCache) {
        VGFXMetalCubemapCacheEntry *entry = ctx.cubemapCache[key];
        if (!entry || (!entry.texture && entry.failedGeneration == 0) ||
            vgfx3d_metal_should_prune_cache_entry(
                ctx.frameSerial, entry.lastUsedFrame, k_cubemap_cache_max_age)) {
            [keys_to_remove addObject:key];
        }
    }
    [ctx.cubemapCache removeObjectsForKeys:keys_to_remove];
}

/// @brief Evict morph-cache entries that are empty or older than the max-age threshold.
/// @param ctx Borrowed Metal context whose morph-buffer cache is pruned.
static void metal_prune_morph_cache(VGFXMetalContext *ctx) {
    NSMutableArray *keys_to_remove;

    if (!ctx || !ctx.morphCache)
        return;
    keys_to_remove = [NSMutableArray array];
    for (NSValue *key in ctx.morphCache) {
        VGFXMetalMorphCacheEntry *entry = ctx.morphCache[key];
        if (!entry || !entry.deltaBuffer ||
            vgfx3d_metal_should_prune_cache_entry(
                ctx.frameSerial, entry.lastUsedFrame, k_morph_cache_max_age)) {
            [keys_to_remove addObject:key];
        }
    }
    [ctx.morphCache removeObjectsForKeys:keys_to_remove];
}

/// @brief Remove stale Metal render-target cache entries.
/// @details Entries are keyed by RenderTarget3D cache identity and retain GPU textures after a
/// target
///   is unbound so later readback/rebind is cheap. This periodic prune waits for any pending
///   command buffer before removal and also drops entries whose borrowed CPU target pointer no
///   longer matches its identity.
/// @param ctx Borrowed Metal context whose native render-target cache is pruned.
static void metal_prune_render_target_cache(VGFXMetalContext *ctx) {
    const uint64_t k_render_target_cache_max_age = 120;
    NSMutableArray *keys_to_remove;

    if (!ctx || !ctx.renderTargetCache)
        return;
    keys_to_remove = [NSMutableArray array];
    for (id key in ctx.renderTargetCache) {
        VGFXMetalRenderTargetCacheEntry *entry = ctx.renderTargetCache[key];
        vgfx3d_rendertarget_t *target = entry ? entry.target : NULL;
        int remove_entry =
            !entry || !entry.colorTexture || !entry.motionTexture || !entry.depthTexture ||
            !target || (target && entry.cacheIdentity != target->cache_identity) ||
            (entry && vgfx3d_metal_should_prune_cache_entry(
                          ctx.frameSerial, entry.lastUsedFrame, k_render_target_cache_max_age));
        if (remove_entry) {
            if (entry.pendingCommandBuffer)
                [entry.pendingCommandBuffer waitUntilCompleted];
            /* Preserve a live target's last rendered image before evicting its only native copy.
             * A failed readback keeps the entry for a later retry rather than silently turning a
             * dirty CPU mirror into apparently valid stale pixels. */
            if (target && entry.cacheIdentity == target->cache_identity && target->color_dirty &&
                entry.colorTexture &&
                !metal_sync_render_target_color((__bridge void *)ctx, target)) {
                continue;
            }
            if (target && target->sync_color_userdata == (__bridge void *)ctx) {
                if (target->color_dirty)
                    vgfx3d_rendertarget_detach_sync_preserve_dirty(target);
                else
                    vgfx3d_rendertarget_clear_sync(target);
            }
            if (target && target->release_backend_userdata == (__bridge void *)ctx)
                vgfx3d_rendertarget_clear_backend_release(target);
            [keys_to_remove addObject:key];
        }
    }
    [ctx.renderTargetCache removeObjectsForKeys:keys_to_remove];
}

/// @brief Clear all CPU-side sync callbacks owned by @p ctx before destroying the Metal context.
/// @details RenderTarget3D objects may outlive the backend context. Once the context is being
///   released, any pending lazy readback callback would point at invalid Objective-C state.
/// @param ctx Borrowed Metal context whose registered target callbacks are detached.
static void metal_clear_render_target_sync_callbacks(VGFXMetalContext *ctx) {
    if (!ctx || !ctx.renderTargetCache)
        return;
    for (id key in ctx.renderTargetCache) {
        VGFXMetalRenderTargetCacheEntry *entry = ctx.renderTargetCache[key];
        vgfx3d_rendertarget_t *target = entry ? entry.target : NULL;
        if (entry && entry.pendingCommandBuffer) {
            [entry.pendingCommandBuffer waitUntilCompleted];
            entry.pendingCommandBuffer = nil;
        }
        if (target && target->color_dirty && entry.colorTexture)
            (void)metal_sync_render_target_color((__bridge void *)ctx, target);
        if (target && target->sync_color_userdata == (__bridge void *)ctx) {
            if (target->color_dirty)
                vgfx3d_rendertarget_detach_sync_preserve_dirty(target);
            else
                vgfx3d_rendertarget_clear_sync(target);
        }
        if (target && target->release_backend_userdata == (__bridge void *)ctx)
            vgfx3d_rendertarget_clear_backend_release(target);
    }
}

/// @brief Remove the Metal cache entry that borrows a render-target shell being finalized.
/// @details Registered on the target itself. The runtime invokes this hook before freeing the C
/// shell, closing the lifetime gap between GC-managed RenderTarget3D objects and ARC-managed cache
/// entries.
/// @param userdata Borrowed bridge pointer to the owning `VGFXMetalContext`.
/// @param target Borrowed render-target shell being finalized.
static void metal_release_render_target(void *userdata, vgfx3d_rendertarget_t *target) {
    VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)userdata;
    VGFXMetalRenderTargetCacheEntry *entry;
    NSNumber *key;
    if (!ctx || !ctx.renderTargetCache || !target)
        return;
    key = @(target->cache_identity);
    entry = ctx.renderTargetCache[key];
    if (!entry || entry.target != target || entry.cacheIdentity != target->cache_identity)
        return;
    if (entry.pendingCommandBuffer) {
        [entry.pendingCommandBuffer waitUntilCompleted];
        entry.pendingCommandBuffer = nil;
    }
    entry.target = NULL;
    [ctx.renderTargetCache removeObjectForKey:key];
}

/// @brief Give @p ctx exclusive ownership of @p target's native-cache release notification.
/// @param ctx Borrowed Metal context that will receive the target's release callback.
/// @param target Borrowed render-target shell whose backend hook is installed.
static void metal_install_render_target_release(VGFXMetalContext *ctx,
                                                vgfx3d_rendertarget_t *target) {
    if (!ctx || !target)
        return;
    if (target->release_backend && (target->release_backend != metal_release_render_target ||
                                    target->release_backend_userdata != (__bridge void *)ctx)) {
        vgfx3d_rendertarget_release_backend(target);
    }
    target->release_backend = metal_release_render_target;
    target->release_backend_userdata = (__bridge void *)ctx;
}

/// @brief Look up the cached Metal textures for a RenderTarget3D (NULL if not cached).
/// @param ctx Borrowed Metal context containing the native target cache.
/// @param rt Borrowed render-target shell and cache identity to resolve.
/// @return The matching cache entry, or nil when absent, stale, or invalid.
static VGFXMetalRenderTargetCacheEntry *metal_lookup_render_target_entry(
    VGFXMetalContext *ctx, vgfx3d_rendertarget_t *rt) {
    NSNumber *key;
    VGFXMetalRenderTargetCacheEntry *entry;
    if (!ctx || !ctx.renderTargetCache || !rt)
        return nil;
    key = @(rt->cache_identity);
    entry = ctx.renderTargetCache[key];
    if (entry && (entry.target != rt || entry.cacheIdentity != rt->cache_identity)) {
        if (entry.pendingCommandBuffer)
            [entry.pendingCommandBuffer waitUntilCompleted];
        [ctx.renderTargetCache removeObjectForKey:key];
        return nil;
    }
    return entry;
}

/// @brief Get or create the cached color/depth/motion textures for a render target.
/// @details Recreates the textures when the target's size or HDR/UNORM format changed; otherwise
///          returns the existing entry. NULL on invalid input or allocation failure.
/// @param ctx Borrowed Metal context that owns the native target cache and textures.
/// @param rt Borrowed render-target shell describing the desired attachments.
/// @return A validated cache entry with complete attachments, or nil on failure.
static VGFXMetalRenderTargetCacheEntry *metal_ensure_render_target_entry(
    VGFXMetalContext *ctx, vgfx3d_rendertarget_t *rt) {
    NSNumber *key;
    VGFXMetalRenderTargetCacheEntry *entry;
    MTLTextureDescriptor *color_desc;
    MTLTextureDescriptor *motion_desc;
    MTLTextureDescriptor *depth_desc;
    MTLPixelFormat desired_color_format;

    if (!ctx || !ctx.renderTargetCache || !rt || !vgfx3d_rendertarget_valid_pixels(rt, NULL) ||
        (rt->color_format != (int32_t)VGFX3D_RENDERTARGET_COLOR_FORMAT_UNORM8 &&
         rt->color_format != (int32_t)VGFX3D_RENDERTARGET_COLOR_FORMAT_HDR16F))
        return nil;

    key = @(rt->cache_identity);
    entry = ctx.renderTargetCache[key];
    desired_color_format =
        metal_color_pixel_format(vgfx3d_rendertarget_is_hdr(rt) ? VGFX3D_METAL_COLOR_FORMAT_HDR16F
                                                                : VGFX3D_METAL_COLOR_FORMAT_UNORM8);
    if (entry && entry.width == rt->width && entry.height == rt->height && entry.colorTexture &&
        entry.motionTexture && entry.depthTexture &&
        entry.colorPixelFormat == desired_color_format) {
        entry.target = rt;
        entry.cacheIdentity = rt->cache_identity;
        entry.lastUsedFrame = ctx.frameSerial;
        metal_install_render_target_release(ctx, rt);
        return entry;
    }

    if (entry && entry.pendingCommandBuffer) {
        [entry.pendingCommandBuffer waitUntilCompleted];
        entry.pendingCommandBuffer = nil;
    }

    entry = [[VGFXMetalRenderTargetCacheEntry alloc] init];
    entry.target = rt;
    entry.cacheIdentity = rt->cache_identity;
    entry.width = rt->width;
    entry.height = rt->height;
    entry.colorPixelFormat = desired_color_format;
    entry.lastUsedFrame = ctx.frameSerial;

    color_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:desired_color_format
                                                                    width:(NSUInteger)rt->width
                                                                   height:(NSUInteger)rt->height
                                                                mipmapped:NO];
    color_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    color_desc.storageMode = MTLStorageModeShared;
    entry.colorTexture = [ctx.device newTextureWithDescriptor:color_desc];

    /* Motion is encoded into normalized BGRA8 by every pipeline, independent of the
     * user color target's LDR/HDR format. Reusing color_desc here makes HDR RTTs bind
     * RGBA16F at attachment 1 against a BGRA8 pipeline declaration. */
    motion_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                     width:(NSUInteger)rt->width
                                                                    height:(NSUInteger)rt->height
                                                                 mipmapped:NO];
    motion_desc.usage = MTLTextureUsageRenderTarget;
    motion_desc.storageMode = MTLStorageModePrivate;
    entry.motionTexture = [ctx.device newTextureWithDescriptor:motion_desc];

    depth_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                    width:(NSUInteger)rt->width
                                                                   height:(NSUInteger)rt->height
                                                                mipmapped:NO];
    depth_desc.usage = MTLTextureUsageRenderTarget;
    depth_desc.storageMode = MTLStorageModePrivate;
    entry.depthTexture = [ctx.device newTextureWithDescriptor:depth_desc];
    if (!entry.colorTexture || !entry.motionTexture || !entry.depthTexture)
        return nil;
    ctx.renderTargetCache[key] = entry;
    metal_install_render_target_release(ctx, rt);
    return entry;
}

/// @brief Read a render target's GPU color texture back into its CPU-side Pixels (callback form).
/// @details Invoked when the runtime needs the rendered image as Pixels (e.g.
/// RenderTarget3D.AsPixels).
/// @param userdata Borrowed bridge pointer to the owning `VGFXMetalContext`.
/// @param target Borrowed render target whose CPU color mirrors are synchronized.
/// @return 1 after successful readback, or 0 when validation, GPU completion, or copying fails.
static int metal_sync_render_target_color(void *userdata, vgfx3d_rendertarget_t *target) {
    VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)userdata;
    VGFXMetalRenderTargetCacheEntry *entry;

    if (!ctx || !target)
        return 0;
    if (!vgfx3d_rendertarget_ensure_color(target))
        return 0;
    if (vgfx3d_rendertarget_is_hdr(target) && !vgfx3d_rendertarget_ensure_hdr_color(target))
        return 0;
    entry = metal_lookup_render_target_entry(ctx, target);
    if (!entry || !entry.colorTexture)
        return 0;
    if (entry.pendingCommandBuffer) {
        MTLCommandBufferStatus status;
        [entry.pendingCommandBuffer waitUntilCompleted];
        status = entry.pendingCommandBuffer.status;
        entry.pendingCommandBuffer = nil;
        if (status != MTLCommandBufferStatusCompleted)
            return 0;
    }
    entry.lastUsedFrame = ctx.frameSerial;
    {
        int ok = metal_copy_texture_to_rgba(
            ctx,
            entry.colorTexture,
            target->color_buf,
            target->width,
            target->height,
            target->stride,
            vgfx3d_rendertarget_is_hdr(target) ? target->hdr_color_buf : NULL);
        target->hdr_color_valid = (int8_t)(ok && vgfx3d_rendertarget_is_hdr(target));
        if (ok)
            target->color_dirty = 0;
        return ok;
    }
}

/// @brief Build the render-pass descriptor for the main scene pass (color + motion + depth
/// attachments).
/// @details Configures load/store actions and clear values for the target's color, motion-vector,
/// and
///          depth textures.
/// @param ctx Borrowed Metal context supplying the active target attachments and clear color.
/// @param loadExistingColor YES to preserve existing color and motion attachment contents.
/// @param loadExistingDepth YES to preserve existing depth attachment contents.
/// @return A configured render-pass descriptor, or nil when required attachments are unavailable.
static MTLRenderPassDescriptor *metal_make_scene_pass_descriptor(VGFXMetalContext *ctx,
                                                                 BOOL loadExistingColor,
                                                                 BOOL loadExistingDepth) {
    MTLRenderPassDescriptor *rp;
    id<MTLTexture> color;
    id<MTLTexture> motion;
    id<MTLTexture> depth;
    MTLClearColor clear_color;

    if (!ctx)
        return nil;
    color = nil;
    motion = nil;
    depth = nil;
    clear_color = MTLClearColorMake(ctx.clearR, ctx.clearG, ctx.clearB, 1.0);

    switch (ctx.currentTargetKind) {
        case VGFX3D_METAL_TARGET_SCENE:
            color = ctx.offscreenColor;
            motion = ctx.offscreenMotion;
            depth = ctx.depthTexture;
            break;
        case VGFX3D_METAL_TARGET_OVERLAY:
            color = ctx.overlayColorTexture;
            motion = ctx.overlayMotionTexture;
            depth = ctx.overlayDepthTexture;
            clear_color = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
            break;
        case VGFX3D_METAL_TARGET_RTT:
            color = ctx.rttColorTexture;
            motion = ctx.rttMotionTexture;
            depth = ctx.rttDepthTexture;
            break;
        case VGFX3D_METAL_TARGET_SWAPCHAIN:
            metal_update_layer_size(ctx);
            if (!ctx.drawable && ctx.metalLayer)
                ctx.drawable = [ctx.metalLayer nextDrawable];
            color = ctx.drawable ? ctx.drawable.texture : nil;
            motion = ctx.offscreenMotion;
            depth = ctx.depthTexture;
            break;
        default:
            break;
    }

    if (!color || !motion || !depth)
        return nil;

    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = color;
    rp.colorAttachments[0].loadAction = loadExistingColor ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = clear_color;

    rp.colorAttachments[1].texture = motion;
    rp.colorAttachments[1].loadAction = loadExistingColor ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.colorAttachments[1].storeAction = MTLStoreActionStore;
    rp.colorAttachments[1].clearColor = MTLClearColorMake(0.5, 0.5, 0.0, 0.0);

    rp.depthAttachment.texture = depth;
    rp.depthAttachment.loadAction = loadExistingDepth ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    /* Reversed-Z: 0 is the far plane; scene depth states compare Greater. */
    rp.depthAttachment.clearDepth = 0.0;
    return rp;
}

/// @brief Begin the scene render-command encoder for the frame from a pass descriptor.
/// @details Sets up viewport and shared render state so subsequent draw calls can be encoded.
/// @param ctx Borrowed Metal context that receives the active scene encoder.
/// @param loadExistingColor YES to load rather than clear color and motion attachments.
/// @param loadExistingDepth YES to load rather than clear the depth attachment.
static void metal_begin_scene_encoder(VGFXMetalContext *ctx,
                                      BOOL loadExistingColor,
                                      BOOL loadExistingDepth) {
    double vw, vh;
    MTLViewport viewport;
    MTLRenderPassDescriptor *rp;

    if (!ctx)
        return;
    if (!ctx.cmdBuf) {
        ctx.cmdBuf = metal_acquire_command_buffer(ctx);
        if (!ctx.cmdBuf)
            return;
    }
    rp = metal_make_scene_pass_descriptor(ctx, loadExistingColor, loadExistingDepth);
    if (!rp)
        return;
    ctx.encoder = [ctx.cmdBuf renderCommandEncoderWithDescriptor:rp];
    ctx->_encoderLightsRevision = 0; /* fresh encoder: no constants encoded yet */
    if (!ctx.encoder)
        return;

    [ctx.encoder setRenderPipelineState:metal_target_uses_hdr_color(ctx)
                                            ? ctx.pipelineState
                                            : ctx.pipelineStateColorOnly];
    [ctx.encoder setDepthStencilState:ctx.depthState];
    vw = (double)rp.colorAttachments[0].texture.width;
    vh = (double)rp.colorAttachments[0].texture.height;
    viewport = (MTLViewport){0, 0, vw, vh, 0.0, 1.0};
    [ctx.encoder setViewport:viewport];
    [ctx.encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [ctx.encoder setCullMode:MTLCullModeBack];
}

/// @brief Recreate window scene, overlay, and composite targets for a logical output extent.
/// @details Scene color/motion/depth and temporal resources use the shared scaled extent. Overlay
///          and post-FX output textures remain at @p w × @p h so the first full-screen pass is an
///          explicit upscale and final UI/readback retain logical dimensions. Direct swapchain
///          mode keeps its depth/motion attachments at the logical extent.
/// @param ctx Borrowed Metal context whose prior size-dependent resources remain live on failure.
/// @param w Positive logical output width.
/// @param h Positive logical output height.
/// @return 1 after atomically publishing a complete resource set; 0 with the old set unchanged.
static int metal_recreate_main_targets(VGFXMetalContext *ctx, int32_t w, int32_t h) {
    int32_t scene_w = w;
    int32_t scene_h = h;
    int8_t scene_route;
    id<MTLTexture> new_depth = nil;
    id<MTLTexture> new_offscreen_color = nil;
    id<MTLTexture> new_offscreen_motion = nil;
    id<MTLTexture> new_overlay_color = nil;
    id<MTLTexture> new_overlay_motion = nil;
    id<MTLTexture> new_overlay_depth = nil;
    id<MTLTexture> new_postfx_color = nil;
    id<MTLTexture> new_postfx_scratch = nil;
    id<MTLTexture> new_taa_history_a = nil;
    id<MTLTexture> new_taa_history_b = nil;
    NSMutableArray<id<MTLTexture>> *new_bloom_mips = nil;

    if (!ctx || !vgfx3d_metal_is_valid_texture2d_extent(w, h))
        return 0;

    scene_route = metal_window_scene_route_enabled(ctx);
    if (scene_route && !ctx.postfxPipeline)
        return 0;
    if (scene_route &&
        !vgfx3d_compute_scaled_scene_extent(w, h, ctx.renderScale, &scene_w, &scene_h)) {
        scene_w = w;
        scene_h = h;
    }

    new_depth = metal_new_depth_texture(
        ctx, scene_w, scene_h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    if (!new_depth)
        return 0;

    if (scene_route) {
        new_offscreen_color =
            metal_new_color_texture(ctx,
                                    scene_w,
                                    scene_h,
                                    metal_color_pixel_format(VGFX3D_METAL_COLOR_FORMAT_HDR16F),
                                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                    MTLStorageModePrivate,
                                    NO);
        new_offscreen_motion =
            metal_new_color_texture(ctx,
                                    scene_w,
                                    scene_h,
                                    MTLPixelFormatBGRA8Unorm,
                                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                    MTLStorageModePrivate,
                                    NO);
        new_overlay_color =
            metal_new_color_texture(ctx,
                                    w,
                                    h,
                                    metal_color_pixel_format(VGFX3D_METAL_COLOR_FORMAT_UNORM8),
                                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                    MTLStorageModePrivate,
                                    NO);
        new_overlay_motion =
            metal_new_color_texture(ctx,
                                    w,
                                    h,
                                    MTLPixelFormatBGRA8Unorm,
                                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                    MTLStorageModePrivate,
                                    NO);
        new_overlay_depth = metal_new_depth_texture(ctx, w, h, MTLTextureUsageRenderTarget);
        if (ctx.postfxPipeline) {
            new_postfx_color =
                metal_new_color_texture(ctx,
                                        w,
                                        h,
                                        metal_color_pixel_format(VGFX3D_METAL_COLOR_FORMAT_UNORM8),
                                        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                        MTLStorageModeShared,
                                        NO);
            new_postfx_scratch =
                metal_new_color_texture(ctx,
                                        w,
                                        h,
                                        metal_color_pixel_format(VGFX3D_METAL_COLOR_FORMAT_UNORM8),
                                        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                        MTLStorageModePrivate,
                                        NO);
        }
        if (!new_offscreen_color || !new_offscreen_motion || !new_overlay_color ||
            !new_overlay_motion || !new_overlay_depth ||
            (ctx.postfxPipeline && (!new_postfx_color || !new_postfx_scratch))) {
            return 0;
        }
        /* Plan 05: half-res RGBA16F bloom mip chain (min dim 8, up to 6 levels). */
        if (ctx.gpuPostfxEnabled && ctx.bloomDownPipeline && ctx.bloomUpPipeline) {
            MTLPixelFormat hdrFmt = metal_color_pixel_format(VGFX3D_METAL_COLOR_FORMAT_HDR16F);
            int32_t mw = scene_w / 2;
            int32_t mh = scene_h / 2;
            NSMutableArray<id<MTLTexture>> *mips = [NSMutableArray arrayWithCapacity:6];
            for (int32_t i = 0; i < 6 && mw >= 1 && mh >= 1; i++) {
                id<MTLTexture> mip =
                    metal_new_color_texture(ctx,
                                            mw,
                                            mh,
                                            hdrFmt,
                                            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                            MTLStorageModePrivate,
                                            NO);
                if (!mip)
                    break;
                [mips addObject:mip];
                if (mw / 2 < 8 || mh / 2 < 8)
                    break;
                mw /= 2;
                mh /= 2;
            }
            if (mips.count == 0)
                return 0;
            new_bloom_mips = mips;
        }
        /* Plan 05: TAA ping-pong history (RGBA16F, persisted across frames). */
        if (ctx.gpuPostfxEnabled && ctx.taaResolvePipeline) {
            MTLPixelFormat hdrFmt = metal_color_pixel_format(VGFX3D_METAL_COLOR_FORMAT_HDR16F);
            new_taa_history_a =
                metal_new_color_texture(ctx,
                                        scene_w,
                                        scene_h,
                                        hdrFmt,
                                        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                        MTLStorageModePrivate,
                                        NO);
            new_taa_history_b =
                metal_new_color_texture(ctx,
                                        scene_w,
                                        scene_h,
                                        hdrFmt,
                                        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                        MTLStorageModePrivate,
                                        NO);
            if (!new_taa_history_a || !new_taa_history_b) {
                return 0;
            }
        }
    } else {
        new_offscreen_motion = metal_new_color_texture(ctx,
                                                       w,
                                                       h,
                                                       MTLPixelFormatBGRA8Unorm,
                                                       MTLTextureUsageRenderTarget,
                                                       MTLStorageModePrivate,
                                                       NO);
        if (!new_offscreen_motion)
            return 0;
    }

    /* Publish only after every attachment required by the selected route exists. ARC keeps the
     * previous set alive until these assignments and releases uncommitted locals on failure. */
    ctx.depthTexture = new_depth;
    ctx.offscreenColor = new_offscreen_color;
    ctx.offscreenMotion = new_offscreen_motion;
    ctx.overlayColorTexture = new_overlay_color;
    ctx.overlayMotionTexture = new_overlay_motion;
    ctx.overlayDepthTexture = new_overlay_depth;
    ctx.postfxColorTexture = new_postfx_color;
    ctx.postfxScratchTexture = new_postfx_scratch;
    ctx.bloomMipTextures = new_bloom_mips;
    ctx.taaHistoryA = new_taa_history_a;
    ctx.taaHistoryB = new_taa_history_b;
    ctx.taaHistoryValid = 0;
    ctx.taaHistoryParity = 0;
    ctx.displayTexture = nil;
    ctx.postfxEncodedThisFrame = 0;
    ctx.postfxCompositedToDrawable = 0;
    return 1;
}

/// @brief Upload a band of RGBA8 source rows into a BGRA8 Metal texture, swizzling R↔B per pixel.
/// @details Metal's default textures are BGRA-order; Pixels are RGBA, so the channels are swapped
///          during the copy. Returns 1 on success.
/// @param tex Borrowed destination 2D BGRA8 texture.
/// @param rgba Borrowed tightly packed RGBA8 source rows.
/// @param w Number of pixels in each source row.
/// @param start_y First destination row to replace.
/// @param rows Number of consecutive rows to upload.
/// @return 1 after successful validation, conversion, and replacement; otherwise 0.
static int metal_upload_rgba_to_bgra_texture_rows(
    id<MTLTexture> tex, const uint8_t *rgba, int32_t w, int32_t start_y, int32_t rows) {
    uint8_t *bgra;
    size_t pixel_count;
    if (!tex || !rgba || w <= 0 || rows <= 0 || start_y < 0)
        return 0;
    if ((NSUInteger)w > tex.width || (NSUInteger)start_y > tex.height ||
        (NSUInteger)rows > tex.height - (NSUInteger)start_y)
        return 0;
    pixel_count = (size_t)w * (size_t)rows;
    if ((size_t)w != 0 && pixel_count / (size_t)w != (size_t)rows)
        return 0;
    if (pixel_count > SIZE_MAX / 4u)
        return 0;
    bgra = (uint8_t *)malloc(pixel_count * 4u);
    if (!bgra)
        return 0;
    for (size_t i = 0; i < pixel_count; i++) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    [tex replaceRegion:MTLRegionMake2D(0, (NSUInteger)start_y, (NSUInteger)w, (NSUInteger)rows)
           mipmapLevel:0
             withBytes:bgra
           bytesPerRow:(NSUInteger)(uint32_t)w * 4u];
    free(bgra);
    return 1;
}

/// @brief Upload a band of RGBA8 rows into one BGRA8 cubemap face, swizzling R↔B per pixel.
/// @param tex Borrowed destination BGRA8 cubemap texture.
/// @param rgba Borrowed tightly packed RGBA8 source rows.
/// @param w Number of pixels in each source row.
/// @param face Destination cubemap face index in `[0, 5]`.
/// @param start_y First destination row to replace.
/// @param rows Number of consecutive rows to upload.
/// @return 1 after successful validation, conversion, and replacement; otherwise 0.
static int metal_upload_rgba_to_bgra_cubemap_rows(id<MTLTexture> tex,
                                                  const uint8_t *rgba,
                                                  int32_t w,
                                                  int32_t face,
                                                  int32_t start_y,
                                                  int32_t rows) {
    uint8_t *bgra;
    size_t pixel_count;
    if (!tex || !rgba || w <= 0 || face < 0 || face >= 6 || rows <= 0 || start_y < 0)
        return 0;
    if ((NSUInteger)w > tex.width || (NSUInteger)start_y > tex.height ||
        (NSUInteger)rows > tex.height - (NSUInteger)start_y)
        return 0;
    pixel_count = (size_t)w * (size_t)rows;
    if ((size_t)w != 0 && pixel_count / (size_t)w != (size_t)rows)
        return 0;
    if (pixel_count > SIZE_MAX / 4u)
        return 0;
    bgra = (uint8_t *)malloc(pixel_count * 4u);
    if (!bgra)
        return 0;
    for (size_t i = 0; i < pixel_count; i++) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    [tex replaceRegion:MTLRegionMake2D(0, (NSUInteger)start_y, (NSUInteger)w, (NSUInteger)rows)
           mipmapLevel:0
                 slice:(NSUInteger)face
             withBytes:bgra
           bytesPerRow:(NSUInteger)(uint32_t)w * 4u
         bytesPerImage:(NSUInteger)((size_t)w * (size_t)rows * 4u)];
    free(bgra);
    return 1;
}

/// @brief Round @p value up to a multiple of @p alignment (for buffer/row-pitch alignment).
/// @param value Byte count or offset to round upward.
/// @param alignment Requested positive alignment; zero leaves @p value unchanged.
/// @return The aligned value, or zero when rounding would overflow `size_t`.
static size_t metal_align_up_size(size_t value, size_t alignment) {
    size_t remainder;
    if (alignment == 0)
        return value;
    remainder = value % alignment;
    if (remainder == 0)
        return value;
    if (value > SIZE_MAX - (alignment - remainder))
        return 0;
    return value + (alignment - remainder);
}

/// @brief Read a Metal texture back into a CPU RGBA8 (or HDR float) buffer via a blit + managed
/// copy.
/// @details Handles the BGRA→RGBA swizzle for the LDR path and the float path for HDR targets;
///          @p stride is the destination row pitch in bytes. Returns 1 on success.
/// @param ctx Borrowed Metal context providing the device and readback command queue.
/// @param tex Borrowed source texture in BGRA8 or RGBA16Float format.
/// @param[out] dst_rgba Caller-owned RGBA8 destination buffer.
/// @param w Requested destination width in pixels.
/// @param h Requested destination height in pixels.
/// @param stride Destination RGBA8 row pitch in bytes.
/// @param[out] dst_hdr_rgba Optional tightly packed RGBA32F destination for HDR source data.
/// @return 1 after successful GPU copy and format conversion, otherwise 0.
static int metal_copy_texture_to_rgba(VGFXMetalContext *ctx,
                                      id<MTLTexture> tex,
                                      uint8_t *dst_rgba,
                                      int32_t w,
                                      int32_t h,
                                      int32_t stride,
                                      float *dst_hdr_rgba) {
    int32_t copy_w;
    int32_t copy_h;
    MTLPixelFormat pixel_format;
    size_t dst_row_bytes;
    size_t source_row_bytes;
    size_t bytes_per_row;
    size_t total_bytes;
    size_t hdr_total_bytes = 0;
    size_t bytes_per_pixel;
    id<MTLBuffer> readback_buffer;
    id<MTLCommandBuffer> command_buffer;
    id<MTLBlitCommandEncoder> blit;
    const uint8_t *src;
    if (!ctx || !ctx.device || !ctx.commandQueue || !tex || !dst_rgba || w <= 0 || h <= 0 ||
        stride <= 0 || (dst_hdr_rgba && w > INT32_MAX / 4))
        return 0;
    if (tex.width > (NSUInteger)INT32_MAX || tex.height > (NSUInteger)INT32_MAX)
        return 0;
    if ((size_t)w > SIZE_MAX / 4u)
        return 0;
    dst_row_bytes = (size_t)w * 4u;
    if ((size_t)stride < dst_row_bytes || (size_t)h > SIZE_MAX / (size_t)stride)
        return 0;
    if (dst_hdr_rgba) {
        if ((size_t)w > SIZE_MAX / (4u * sizeof(float)) ||
            (size_t)h > SIZE_MAX / ((size_t)w * 4u * sizeof(float))) {
            return 0;
        }
        hdr_total_bytes = (size_t)w * (size_t)h * 4u * sizeof(float);
    }

    memset(dst_rgba, 0, (size_t)stride * (size_t)h);
    if (dst_hdr_rgba)
        memset(dst_hdr_rgba, 0, hdr_total_bytes);
    copy_w = (int32_t)tex.width < w ? (int32_t)tex.width : w;
    copy_h = (int32_t)tex.height < h ? (int32_t)tex.height : h;
    if (copy_w <= 0 || copy_h <= 0)
        return 0;
    pixel_format = tex.pixelFormat;
    if (pixel_format == MTLPixelFormatRGBA16Float) {
        bytes_per_pixel = 8u;
    } else if (pixel_format == MTLPixelFormatBGRA8Unorm) {
        if (dst_hdr_rgba)
            return 0;
        bytes_per_pixel = 4u;
    } else {
        return 0;
    }

    if ((size_t)copy_w > SIZE_MAX / bytes_per_pixel)
        return 0;
    source_row_bytes = (size_t)copy_w * bytes_per_pixel;
    bytes_per_row = metal_align_up_size(source_row_bytes, 256u);
    if (bytes_per_row == 0 || (size_t)copy_h > SIZE_MAX / bytes_per_row)
        return 0;
    total_bytes = bytes_per_row * (size_t)copy_h;
    readback_buffer = [ctx.device newBufferWithLength:(NSUInteger)total_bytes
                                              options:MTLResourceStorageModeShared];
    if (!readback_buffer)
        return 0;

    command_buffer = [ctx.commandQueue commandBuffer];
    if (!command_buffer)
        return 0;
    blit = [command_buffer blitCommandEncoder];
    if (!blit)
        return 0;
    [blit copyFromTexture:tex
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake((NSUInteger)copy_w, (NSUInteger)copy_h, 1)
                        toBuffer:readback_buffer
               destinationOffset:0
          destinationBytesPerRow:(NSUInteger)bytes_per_row
        destinationBytesPerImage:(NSUInteger)total_bytes];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted)
        return 0;
    src = (const uint8_t *)[readback_buffer contents];
    if (!src)
        return 0;

    if (pixel_format == MTLPixelFormatRGBA16Float) {
        if (bytes_per_row > (size_t)INT32_MAX)
            return 0;
        vgfx3d_copy_linear_rgba16f_to_rgba8(
            dst_rgba, stride, copy_w, copy_h, (const uint16_t *)src, (int32_t)bytes_per_row);
        if (dst_hdr_rgba) {
            vgfx3d_copy_linear_rgba16f_to_rgba32f(
                dst_hdr_rgba, w * 4, copy_w, copy_h, (const uint16_t *)src, (int32_t)bytes_per_row);
        }
        return 1;
    }

    for (int32_t y = 0; y < copy_h; y++) {
        uint8_t *dst_row = dst_rgba + (size_t)y * (size_t)stride;
        const uint8_t *src_row = src + (size_t)y * bytes_per_row;
        for (int32_t x = 0; x < copy_w; x++) {
            dst_row[(size_t)x * 4u + 0u] = src_row[(size_t)x * 4u + 2u];
            dst_row[(size_t)x * 4u + 1u] = src_row[(size_t)x * 4u + 1u];
            dst_row[(size_t)x * 4u + 2u] = src_row[(size_t)x * 4u + 0u];
            dst_row[(size_t)x * 4u + 3u] = src_row[(size_t)x * 4u + 3u];
        }
    }
    return 1;
}
