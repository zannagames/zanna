//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_metal_texture.inc
// Purpose: Metal texture/cubemap caches, streaming uploads, and deformation buffers.
// Key invariants:
//   - Streamed uploads distinguish budget pauses from terminal source/resource failures.
//   - Cache generations publish only after every required row, face, or mip uploads.
// Ownership/Lifetime:
//   - Included by vgfx3d_backend_metal.m; it is not a standalone translation unit.
//   - Objective-C cache entries strongly own Metal resources until eviction/context teardown.
// Links: vgfx3d_backend_metal.m, vgfx3d_backend_utils.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Metal texture, cubemap, IBL, skinning, and morph-resource preparation.
/// @details This implementation fragment manages generation-keyed resource caches and
///          budget-paced uploads. It publishes cache generations only after complete transfers
///          and prepares the transient or persistent buffers consumed by deformation shaders.

typedef enum {
    METAL_UPLOAD_FAILED = -1,
    METAL_UPLOAD_PENDING = 0,
    METAL_UPLOAD_COMPLETE = 1,
} metal_upload_status_t;

static void metal_bound_texture_cache(VGFXMetalContext *ctx, NSValue *protected_key);
static void metal_bound_cubemap_cache(VGFXMetalContext *ctx, NSValue *protected_key);

/// @brief Select the complete texture that may be bound for a cache entry.
/// @details A candidate texture is not visible until its streamed upload publishes. While that
///          work is pending, the previous complete entry remains usable as a fallback.
/// @param entry Borrowed texture-cache entry, or nil.
/// @return The active complete texture, the candidate's complete fallback, or nil.
static id<MTLTexture> metal_visible_texture(VGFXMetalTextureCacheEntry *entry) {
    if (!entry)
        return nil;
    return entry.uploadInProgress ? entry.fallbackEntry.texture : entry.texture;
}

/// @brief Select the complete cubemap that may be bound for a cache entry.
/// @details Never exposes a partially uploaded candidate to shader reads.
/// @param entry Borrowed cubemap-cache entry, or nil.
/// @return The active complete cubemap, the candidate's complete fallback, or nil.
static id<MTLTexture> metal_visible_cubemap(VGFXMetalCubemapCacheEntry *entry) {
    if (!entry)
        return nil;
    return entry.uploadInProgress ? entry.fallbackEntry.texture : entry.texture;
}

/// @brief Retire a texture upload that cannot make progress and memoize its generation.
/// @param ctx Borrowed Metal context used to stamp the cache entry's last-used frame; may be NULL.
/// @param entry Borrowed texture-cache entry whose partial resource and cursors are cleared.
/// @param key Cache key whose previous complete entry is restored when available.
/// @param generation Failed source generation to memoize and avoid repeated work.
static void metal_fail_texture_upload(VGFXMetalContext *ctx,
                                      VGFXMetalTextureCacheEntry *entry,
                                      NSValue *key,
                                      uint64_t generation) {
    VGFXMetalTextureCacheEntry *fallback;

    if (!entry)
        return;
    fallback = entry.fallbackEntry;
    entry.fallbackEntry = nil;
    entry.texture = nil;
    entry.generation = 0;
    entry.pendingGeneration = 0;
    entry.failedGeneration = generation;
    entry.uploadInProgress = 0;
    entry.uploadNextRow = 0;
    entry.nativeNextBlockRow = 0;
    entry.nativeNextMip = 0;
    if (ctx) {
        entry.lastUsedFrame = ctx.frameSerial;
        if (key) {
            if (fallback) {
                fallback.failedGeneration = generation;
                fallback.lastUsedFrame = ctx.frameSerial;
                ctx.textureCache[key] = fallback;
            } else {
                ctx.textureCache[key] = entry;
            }
            metal_bound_texture_cache(ctx, key);
        }
    }
}

/// @brief Retire a cubemap upload that cannot make progress and memoize its generation.
/// @param ctx Borrowed Metal context used to stamp the cache entry's last-used frame; may be NULL.
/// @param entry Borrowed cubemap-cache entry whose partial resource and cursors are cleared.
/// @param key Cache key whose previous complete entry is restored when available.
/// @param generation Failed cubemap generation to memoize and avoid repeated work.
static void metal_fail_cubemap_upload(VGFXMetalContext *ctx,
                                      VGFXMetalCubemapCacheEntry *entry,
                                      NSValue *key,
                                      uint64_t generation) {
    VGFXMetalCubemapCacheEntry *fallback;

    if (!entry)
        return;
    fallback = entry.fallbackEntry;
    entry.fallbackEntry = nil;
    entry.texture = nil;
    entry.generation = 0;
    entry.pendingGeneration = 0;
    entry.failedGeneration = generation;
    entry.uploadInProgress = 0;
    entry.uploadFace = 0;
    entry.uploadNextRow = 0;
    entry.appliedIblIdentity = 0;
    entry.failedIblIdentity = 0;
    entry.pendingIblIdentity = 0;
    entry.pendingIblBytes = 0;
    if (ctx) {
        entry.lastUsedFrame = ctx.frameSerial;
        if (key) {
            if (fallback) {
                fallback.failedGeneration = generation;
                fallback.lastUsedFrame = ctx.frameSerial;
                ctx.cubemapCache[key] = fallback;
            } else {
                ctx.cubemapCache[key] = entry;
            }
            metal_bound_cubemap_cache(ctx, key);
        }
    }
}

/// @brief Map a native compressed mip's format/block size to the matching MTLPixelFormat (0 if
/// unsupported).
/// @param mip Borrowed native mip descriptor containing format and block dimensions.
/// @return The matching Metal compressed format, or `MTLPixelFormatInvalid` when unsupported.
static MTLPixelFormat metal_native_texture_pixel_format(const vgfx3d_native_texture_mip_t *mip) {
    if (!mip)
        return MTLPixelFormatInvalid;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7)
        return MTLPixelFormatBC7_RGBAUnorm;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1)
        return MTLPixelFormatBC1_RGBA;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC3)
        return MTLPixelFormatBC3_RGBA;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC4)
        return MTLPixelFormatBC4_RUnorm;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC5)
        return MTLPixelFormatBC5_RGUnorm;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_ETC2)
        return MTLPixelFormatEAC_RGBA8;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_ASTC) {
        if (mip->block_width == 4 && mip->block_height == 4)
            return MTLPixelFormatASTC_4x4_LDR;
        if (mip->block_width == 5 && mip->block_height == 4)
            return MTLPixelFormatASTC_5x4_LDR;
        if (mip->block_width == 5 && mip->block_height == 5)
            return MTLPixelFormatASTC_5x5_LDR;
        if (mip->block_width == 6 && mip->block_height == 5)
            return MTLPixelFormatASTC_6x5_LDR;
        if (mip->block_width == 6 && mip->block_height == 6)
            return MTLPixelFormatASTC_6x6_LDR;
        if (mip->block_width == 8 && mip->block_height == 5)
            return MTLPixelFormatASTC_8x5_LDR;
        if (mip->block_width == 8 && mip->block_height == 6)
            return MTLPixelFormatASTC_8x6_LDR;
        if (mip->block_width == 8 && mip->block_height == 8)
            return MTLPixelFormatASTC_8x8_LDR;
        if (mip->block_width == 10 && mip->block_height == 5)
            return MTLPixelFormatASTC_10x5_LDR;
        if (mip->block_width == 10 && mip->block_height == 6)
            return MTLPixelFormatASTC_10x6_LDR;
        if (mip->block_width == 10 && mip->block_height == 8)
            return MTLPixelFormatASTC_10x8_LDR;
        if (mip->block_width == 10 && mip->block_height == 10)
            return MTLPixelFormatASTC_10x10_LDR;
        if (mip->block_width == 12 && mip->block_height == 10)
            return MTLPixelFormatASTC_12x10_LDR;
        if (mip->block_width == 12 && mip->block_height == 12)
            return MTLPixelFormatASTC_12x12_LDR;
    }
    return MTLPixelFormatInvalid;
}

/// @brief Bytes per block-row of a native compressed mip (block columns × block byte size,
/// overflow-safe).
/// @param mip Borrowed compressed mip descriptor to measure.
/// @return The byte count for one compressed block row, or zero for invalid/overflowing input.
static uint64_t metal_native_texture_row_bytes(const vgfx3d_native_texture_mip_t *mip) {
    uint64_t cols;
    if (!mip || mip->width <= 0 || mip->block_width <= 0 || mip->block_bytes <= 0)
        return 0;
    cols = ((uint64_t)(uint32_t)mip->width + (uint64_t)(uint32_t)mip->block_width - 1u) /
           (uint64_t)(uint32_t)mip->block_width;
    if (cols > UINT64_MAX / (uint64_t)(uint32_t)mip->block_bytes)
        return 0;
    return cols * (uint64_t)(uint32_t)mip->block_bytes;
}

/// @brief Bytes still to upload for a cached texture entry (native or RGBA streaming path).
/// @param entry Borrowed texture-cache entry whose active cursor is measured.
/// @return Remaining upload bytes, or zero when no entry/upload is available.
static uint64_t metal_texture_pending_bytes(VGFXMetalTextureCacheEntry *entry) {
    if (!entry)
        return 0;
    if (entry.textureAsset)
        return vgfx3d_textureasset_pending_native_snapshot_bytes(entry.textureAsset,
                                                                 entry.nativeMipStart,
                                                                 entry.nativeMipCount,
                                                                 entry.nativeNextMip,
                                                                 entry.nativeNextBlockRow,
                                                                 entry.uploadInProgress ? 1 : 0);
    return vgfx3d_pending_rgba_upload_bytes(
        entry.width, entry.height, entry.uploadNextRow, entry.uploadInProgress ? 1 : 0);
}

/// @brief Bytes still to upload for a cached cubemap entry across its remaining faces/rows.
/// @param entry Borrowed cubemap-cache entry whose base and IBL cursors are measured.
/// @return Saturating sum of remaining base-face and prefiltered-IBL bytes.
static uint64_t metal_cubemap_pending_bytes(VGFXMetalCubemapCacheEntry *entry) {
    uint64_t base_bytes;
    if (!entry)
        return 0;
    base_bytes = vgfx3d_pending_cubemap_rgba_upload_bytes(
        entry.faceSize, entry.uploadFace, entry.uploadNextRow, entry.uploadInProgress ? 1 : 0);
    if (base_bytes > UINT64_MAX - entry.pendingIblBytes)
        return UINT64_MAX;
    return base_bytes + entry.pendingIblBytes;
}

/// @brief Total bytes still pending across all in-progress texture/cubemap uploads (saturating).
/// @param ctx_ptr Borrowed `VGFXMetalContext` whose caches are inspected.
/// @return Saturating total number of pending upload bytes, or zero for an invalid context.
static uint64_t metal_get_texture_upload_pending_bytes(void *ctx_ptr) {
    VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
    uint64_t total = 0;
    if (!ctx || !ctx.textureCache)
        return 0;
    for (NSValue *key in ctx.textureCache) {
        VGFXMetalTextureCacheEntry *entry = ctx.textureCache[key];
        uint64_t bytes = metal_texture_pending_bytes(entry);
        if (total > UINT64_MAX - bytes)
            return UINT64_MAX;
        total += bytes;
    }
    for (NSValue *key in ctx.cubemapCache) {
        VGFXMetalCubemapCacheEntry *entry = ctx.cubemapCache[key];
        uint64_t bytes = metal_cubemap_pending_bytes(entry);
        if (total > UINT64_MAX - bytes)
            return UINT64_MAX;
        total += bytes;
    }
    return total;
}

/// @brief Set the per-frame byte budget that paces streaming texture uploads on this context.
/// @param ctx_ptr Borrowed `VGFXMetalContext` whose upload budget is updated.
/// @param bytes Maximum bytes to upload during a frame; `UINT64_MAX` represents no finite cap.
static void metal_set_texture_upload_budget(void *ctx_ptr, uint64_t bytes) {
    VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
    if (ctx)
        ctx.textureUploadBudgetBytes = bytes;
}

/// @brief Upload more of an in-progress native compressed texture, bounded by the upload budget.
/// @param ctx Borrowed Metal context providing the current budget and accounting counters.
/// @param entry Borrowed cache entry containing the destination texture and native upload cursor.
/// @return `METAL_UPLOAD_COMPLETE` when all mips publish, `METAL_UPLOAD_PENDING` when
/// budget-paused,
///         or `METAL_UPLOAD_FAILED` for invalid source/resource state.
static metal_upload_status_t metal_continue_native_texture_upload(
    VGFXMetalContext *ctx, VGFXMetalTextureCacheEntry *entry) {
    if (!ctx || !entry || !entry.textureAsset || !entry.uploadInProgress || !entry.texture)
        return METAL_UPLOAD_FAILED;
    while (entry.nativeNextMip < entry.nativeMipCount) {
        vgfx3d_native_texture_mip_t mip;
        uint64_t row_bytes;
        uint64_t block_rows;
        uint64_t required_bytes;
        int32_t rows;
        uint64_t offset;
        uint64_t upload_bytes;
        uint64_t y;
        uint64_t h;
        MTLRegion region;

        if (ctx.textureUploadBudgetBytes != UINT64_MAX &&
            ctx.textureUploadBytes >= ctx.textureUploadBudgetBytes)
            return METAL_UPLOAD_PENDING;
        if (!vgfx3d_textureasset_get_native_snapshot_mip(entry.textureAsset,
                                                         entry.nativeMipStart,
                                                         entry.nativeMipCount,
                                                         entry.nativeNextMip,
                                                         &mip))
            return METAL_UPLOAD_FAILED;
        if (!vgfx3d_validate_native_texture_mip(&mip,
                                                entry.width,
                                                entry.height,
                                                entry.nativeNextMip,
                                                entry.nativeFormat,
                                                entry.nativeBlockWidth,
                                                entry.nativeBlockHeight,
                                                entry.nativeBlockBytes,
                                                (uint64_t)NSUIntegerMax,
                                                &required_bytes) ||
            metal_native_texture_pixel_format(&mip) != entry.texture.pixelFormat)
            return METAL_UPLOAD_FAILED;
        row_bytes = metal_native_texture_row_bytes(&mip);
        if (row_bytes == 0 || row_bytes > (uint64_t)NSUIntegerMax ||
            mip.bytes > (uint64_t)NSUIntegerMax)
            return METAL_UPLOAD_FAILED;
        block_rows = ((uint64_t)(uint32_t)mip.height + (uint64_t)(uint32_t)mip.block_height - 1u) /
                     (uint64_t)(uint32_t)mip.block_height;
        if (block_rows == 0 || block_rows > (uint64_t)INT32_MAX)
            return METAL_UPLOAD_FAILED;
        if (entry.nativeNextBlockRow < 0 ||
            (uint64_t)(uint32_t)entry.nativeNextBlockRow >= block_rows)
            entry.nativeNextBlockRow = 0;
        rows = vgfx3d_upload_block_rows_for_budget(mip.width,
                                                   mip.height,
                                                   mip.block_width,
                                                   mip.block_height,
                                                   mip.block_bytes,
                                                   entry.nativeNextBlockRow,
                                                   ctx.textureUploadBudgetBytes,
                                                   ctx.textureUploadBytes);
        if (rows <= 0)
            return METAL_UPLOAD_FAILED;
        offset = (uint64_t)(uint32_t)entry.nativeNextBlockRow * row_bytes;
        upload_bytes = (uint64_t)(uint32_t)rows * row_bytes;
        y = (uint64_t)(uint32_t)entry.nativeNextBlockRow * (uint64_t)(uint32_t)mip.block_height;
        h = (uint64_t)(uint32_t)rows * (uint64_t)(uint32_t)mip.block_height;
        if (offset > required_bytes || upload_bytes > required_bytes - offset ||
            y >= (uint64_t)mip.height)
            return METAL_UPLOAD_FAILED;
        if (h > (uint64_t)mip.height - y)
            h = (uint64_t)mip.height - y;
        if (upload_bytes > (uint64_t)NSUIntegerMax || y > (uint64_t)NSUIntegerMax ||
            h > (uint64_t)NSUIntegerMax)
            return METAL_UPLOAD_FAILED;
        region = MTLRegionMake2D(0, (NSUInteger)y, (NSUInteger)mip.width, (NSUInteger)h);
        [entry.texture replaceRegion:region
                         mipmapLevel:(NSUInteger)entry.nativeNextMip
                           withBytes:(const uint8_t *)mip.data + offset
                         bytesPerRow:(NSUInteger)row_bytes];
        metal_record_texture_upload_bytes(ctx, upload_bytes);
        entry.nativeNextBlockRow += rows;
        if ((uint64_t)(uint32_t)entry.nativeNextBlockRow >= block_rows) {
            entry.nativeNextMip++;
            entry.nativeNextBlockRow = 0;
        }
        entry.lastUsedFrame = ctx.frameSerial;
    }
    entry.generation = entry.pendingGeneration;
    entry.pendingGeneration = 0;
    entry.uploadInProgress = 0;
    return METAL_UPLOAD_COMPLETE;
}

/// @brief Begin uploading a native compressed TextureAsset3D: create the texture and seed the
/// cursor.
/// @param ctx Borrowed Metal context providing capabilities, device allocation, and budget state.
/// @param entry Borrowed texture-cache entry initialized for the new upload.
/// @param asset Borrowed native `TextureAsset3D` source.
/// @param cache_key Stable source generation published after the upload completes.
/// @param mip_start First resident native mip to upload.
/// @param mip_count Number of consecutive resident mips to upload.
/// @return Complete, pending, or failed upload status after attempting the first transfer slice.
static metal_upload_status_t metal_start_native_texture_upload(VGFXMetalContext *ctx,
                                                               VGFXMetalTextureCacheEntry *entry,
                                                               void *asset,
                                                               uint64_t cache_key,
                                                               int64_t mip_start,
                                                               int64_t mip_count) {
    vgfx3d_native_texture_mip_t first_mip;
    MTLPixelFormat pixel_format;
    id<MTLTexture> replacement;

    if (!ctx || !entry || !asset ||
        !vgfx3d_textureasset_native_supported(
            asset, metal_get_native_texture_caps((__bridge void *)ctx)) ||
        !vgfx3d_textureasset_get_native_snapshot_mip(asset, mip_start, mip_count, 0, &first_mip))
        return METAL_UPLOAD_FAILED;
    pixel_format = metal_native_texture_pixel_format(&first_mip);
    if (pixel_format == MTLPixelFormatInvalid ||
        !vgfx3d_native_texture_block_layout_is_valid(&first_mip) ||
        !vgfx3d_metal_is_valid_texture2d_extent(first_mip.width, first_mip.height)) {
        return METAL_UPLOAD_FAILED;
    }
    if (mip_count <= 0 ||
        mip_count > vgfx3d_metal_compute_mip_count(first_mip.width, first_mip.height) ||
        (uint64_t)mip_count > (uint64_t)NSUIntegerMax)
        return METAL_UPLOAD_FAILED;

    MTLTextureDescriptor *texDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                           width:(NSUInteger)first_mip.width
                                                          height:(NSUInteger)first_mip.height
                                                       mipmapped:(mip_count > 1)];
    texDesc.mipmapLevelCount = (NSUInteger)mip_count;
    texDesc.usage = MTLTextureUsageShaderRead;
    texDesc.storageMode = MTLStorageModeShared;
    replacement = [ctx.device newTextureWithDescriptor:texDesc];
    if (!replacement)
        return METAL_UPLOAD_FAILED;
    entry.texture = replacement;

    entry.textureAsset = asset;
    entry.nativeFormat = first_mip.format_id;
    entry.nativeBlockWidth = first_mip.block_width;
    entry.nativeBlockHeight = first_mip.block_height;
    entry.nativeBlockBytes = first_mip.block_bytes;
    entry.nativeNextBlockRow = 0;
    entry.nativeNextMip = 0;
    entry.nativeMipStart = mip_start;
    entry.nativeMipCount = mip_count;
    entry.pendingGeneration = cache_key;
    entry.failedGeneration = 0;
    entry.generation = 0;
    entry.width = first_mip.width;
    entry.height = first_mip.height;
    entry.uploadNextRow = 0;
    entry.uploadInProgress = 1;
    entry.lastUsedFrame = ctx.frameSerial;
    return metal_continue_native_texture_upload(ctx, entry);
}

/// @brief Upload more rows of an in-progress RGBA texture (swizzled to BGRA), bounded by the
/// budget.
/// @param ctx Borrowed Metal context providing the current budget and accounting counters.
/// @param entry Borrowed cache entry containing the destination texture and row cursor.
/// @param pixels_ptr Borrowed runtime Pixels source.
/// @return `METAL_UPLOAD_COMPLETE` when all rows publish, `METAL_UPLOAD_PENDING` when
/// budget-paused,
///         or `METAL_UPLOAD_FAILED` for invalid source/resource state.
static metal_upload_status_t metal_continue_texture_upload(VGFXMetalContext *ctx,
                                                           VGFXMetalTextureCacheEntry *entry,
                                                           const void *pixels_ptr) {
    int32_t rows;
    int32_t slice_w = 0;
    int32_t slice_rows = 0;
    uint8_t *rgba = NULL;
    uint64_t bytes;

    if (!ctx || !entry || !pixels_ptr || !entry.uploadInProgress || !entry.texture)
        return METAL_UPLOAD_FAILED;
    if (ctx.textureUploadBudgetBytes != UINT64_MAX &&
        ctx.textureUploadBytes >= ctx.textureUploadBudgetBytes)
        return METAL_UPLOAD_PENDING;
    rows = vgfx3d_upload_rows_for_budget(entry.width,
                                         entry.height,
                                         entry.uploadNextRow,
                                         ctx.textureUploadBudgetBytes,
                                         ctx.textureUploadBytes);
    if (rows <= 0)
        return METAL_UPLOAD_FAILED;
    if (vgfx3d_unpack_pixels_rgba_rows(
            pixels_ptr, entry.uploadNextRow, rows, 0, &slice_w, &slice_rows, &rgba) != 0 ||
        !rgba || slice_w != entry.width || slice_rows <= 0) {
        free(rgba);
        return METAL_UPLOAD_FAILED;
    }
    if (!metal_upload_rgba_to_bgra_texture_rows(
            entry.texture, rgba, slice_w, entry.uploadNextRow, slice_rows)) {
        free(rgba);
        return METAL_UPLOAD_FAILED;
    }
    bytes = (uint64_t)(uint32_t)slice_w * (uint64_t)(uint32_t)slice_rows * 4u;
    metal_record_texture_upload_bytes(ctx, bytes);
    free(rgba);

    entry.uploadNextRow += slice_rows;
    entry.lastUsedFrame = ctx.frameSerial;
    if (entry.uploadNextRow >= entry.height) {
        if (!metal_generate_mipmaps(ctx, entry.texture))
            return METAL_UPLOAD_FAILED;
        entry.generation = entry.pendingGeneration;
        entry.pendingGeneration = 0;
        entry.uploadInProgress = 0;
        return METAL_UPLOAD_COMPLETE;
    }
    return METAL_UPLOAD_PENDING;
}

/// @brief Begin uploading an RGBA Pixels texture: allocate the BGRA texture and seed the row
/// cursor.
/// @param ctx Borrowed Metal context providing the allocation device and upload budget.
/// @param entry Borrowed texture-cache entry initialized for the new upload.
/// @param pixels_ptr Borrowed runtime Pixels source.
/// @param cache_key Stable Pixels generation published after all rows upload.
/// @return Complete, pending, or failed upload status after attempting the first row band.
static metal_upload_status_t metal_start_texture_upload(VGFXMetalContext *ctx,
                                                        VGFXMetalTextureCacheEntry *entry,
                                                        const void *pixels_ptr,
                                                        uint64_t cache_key) {
    int32_t tw = 0;
    int32_t th = 0;
    int32_t mip_count;

    if (!ctx || !entry || !pixels_ptr || !vgfx3d_get_pixels_extent(pixels_ptr, &tw, &th) ||
        !vgfx3d_metal_is_valid_texture2d_extent(tw, th)) {
        return METAL_UPLOAD_FAILED;
    }

    mip_count = vgfx3d_metal_compute_mip_count(tw, th);
    if (!entry.texture || (int32_t)entry.texture.width != tw ||
        (int32_t)entry.texture.height != th) {
        id<MTLTexture> replacement;
        MTLTextureDescriptor *texDesc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:(NSUInteger)tw
                                                              height:(NSUInteger)th
                                                           mipmapped:(mip_count > 1)];
        texDesc.usage = MTLTextureUsageShaderRead;
        texDesc.storageMode = MTLStorageModeShared;
        replacement = [ctx.device newTextureWithDescriptor:texDesc];
        if (!replacement)
            return METAL_UPLOAD_FAILED;
        entry.texture = replacement;
    }
    if (!entry.texture)
        return METAL_UPLOAD_FAILED;

    entry.pendingGeneration = cache_key;
    entry.failedGeneration = 0;
    entry.generation = 0;
    entry.textureAsset = NULL;
    entry.nativeFormat = RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE;
    entry.nativeBlockWidth = 0;
    entry.nativeBlockHeight = 0;
    entry.nativeBlockBytes = 0;
    entry.nativeNextBlockRow = 0;
    entry.nativeNextMip = 0;
    entry.nativeMipCount = 0;
    entry.width = tw;
    entry.height = th;
    entry.uploadNextRow = 0;
    entry.uploadInProgress = 1;
    entry.lastUsedFrame = ctx.frameSerial;
    return metal_continue_texture_upload(ctx, entry, pixels_ptr);
}

/// @brief Enforce the hard 2D texture-cache cardinality limit after a successful insertion.
/// @param ctx Borrowed cache owner.
/// @param protected_key Newly inserted key that must not be selected for eviction.
static void metal_bound_texture_cache(VGFXMetalContext *ctx, NSValue *protected_key) {
    while (ctx && ctx.textureCache.count > k_texture_cache_max_entries) {
        NSValue *oldest_key = nil;
        uint64_t oldest_frame = UINT64_MAX;
        for (NSValue *candidate_key in ctx.textureCache) {
            if ([candidate_key isEqual:protected_key])
                continue;
            VGFXMetalTextureCacheEntry *candidate = ctx.textureCache[candidate_key];
            if (!candidate || candidate.lastUsedFrame < oldest_frame) {
                oldest_key = candidate_key;
                oldest_frame = candidate ? candidate.lastUsedFrame : 0;
            }
        }
        if (!oldest_key)
            break;
        [ctx.textureCache removeObjectForKey:oldest_key];
    }
}

/// @brief Retrieve or create a cached Metal texture from a runtime Pixels object.
/// @details The cache uses the stable Pixels cache key so allocator-reused addresses do not alias
///          a fresh image to an older GPU upload. Conversion is RGBA→BGRA and may span frames.
/// @param ctx Borrowed Metal context owning the texture cache and upload budget.
/// @param pixels_ptr Borrowed runtime Pixels object used as both source and cache identity.
/// @return The complete cached texture, the previous complete generation while replacement is
///         pending/failed, or nil when no complete generation exists.
static id<MTLTexture> metal_get_cached_texture(VGFXMetalContext *ctx, const void *pixels_ptr) {
    uint64_t cache_key;
    NSValue *key;
    VGFXMetalTextureCacheEntry *cached;
    metal_upload_status_t status;

    if (!ctx || !pixels_ptr)
        return nil;
    key = [NSValue valueWithPointer:pixels_ptr];
    cached = ctx.textureCache[key];
    cache_key = vgfx3d_get_pixels_cache_key(pixels_ptr);
    if (cached && cached.failedGeneration == cache_key) {
        cached.lastUsedFrame = ctx.frameSerial;
        return metal_visible_texture(cached);
    }
    if (cached && cached.texture && cached.generation == cache_key) {
        cached.lastUsedFrame = ctx.frameSerial;
        return cached.texture;
    }
    if (cached && cached.pendingGeneration == cache_key && cached.uploadInProgress) {
        status = metal_continue_texture_upload(ctx, cached, pixels_ptr);
        if (status == METAL_UPLOAD_FAILED) {
            metal_fail_texture_upload(ctx, cached, key, cache_key);
        } else if (status == METAL_UPLOAD_COMPLETE) {
            cached.fallbackEntry = nil;
        }
        return status == METAL_UPLOAD_COMPLETE ? cached.texture
                                               : metal_visible_texture(ctx.textureCache[key]);
    }
    VGFXMetalTextureCacheEntry *candidate = [[VGFXMetalTextureCacheEntry alloc] init];
    if (!candidate)
        return metal_visible_texture(cached);
    candidate.fallbackEntry = cached.uploadInProgress ? cached.fallbackEntry : cached;
    status = metal_start_texture_upload(ctx, candidate, pixels_ptr, cache_key);
    if (status == METAL_UPLOAD_FAILED) {
        metal_fail_texture_upload(ctx, candidate, key, cache_key);
        return metal_visible_texture(ctx.textureCache[key]);
    }
    ctx.textureCache[key] = candidate;
    if (status == METAL_UPLOAD_COMPLETE)
        candidate.fallbackEntry = nil;
    metal_bound_texture_cache(ctx, key);
    return status == METAL_UPLOAD_COMPLETE ? candidate.texture : metal_visible_texture(candidate);
}

/// @brief Get the Metal texture for a native TextureAsset3D, creating/streaming it on a cache miss.
/// @details Keyed by the asset's native cache key. A previous complete resident range stays
///          visible while its replacement is still in progress.
/// @param ctx Borrowed Metal context owning the texture cache and native-format capabilities.
/// @param asset Borrowed native `TextureAsset3D` source and cache identity.
/// @param cache_key Stable native snapshot key, or zero to query it from @p asset.
/// @param mip_start First resident mip, or a negative value to query it from @p asset.
/// @param mip_count Resident mip count, or a non-positive value to query it from @p asset.
/// @return The complete cached native texture, the previous complete generation during
///         replacement, or nil when unsupported or no complete generation exists.
static id<MTLTexture> metal_get_cached_native_texture(
    VGFXMetalContext *ctx, void *asset, uint64_t cache_key, int64_t mip_start, int64_t mip_count) {
    NSValue *key;
    VGFXMetalTextureCacheEntry *cached;
    metal_upload_status_t status;

    if (!ctx || !asset ||
        !vgfx3d_textureasset_native_supported(asset,
                                              metal_get_native_texture_caps((__bridge void *)ctx)))
        return nil;
    if (cache_key == 0)
        cache_key = rt_textureasset3d_get_native_cache_key(asset);
    if (mip_start < 0)
        mip_start = rt_textureasset3d_get_resident_mip_start(asset);
    if (mip_count <= 0)
        mip_count = rt_textureasset3d_get_resident_mip_count(asset);
    if (cache_key == 0)
        return nil;
    key = [NSValue valueWithPointer:asset];
    cached = ctx.textureCache[key];
    if (cached && cached.failedGeneration == cache_key) {
        cached.lastUsedFrame = ctx.frameSerial;
        return metal_visible_texture(cached);
    }
    if (cached && cached.texture && cached.textureAsset == asset &&
        cached.generation == cache_key && cached.nativeMipStart == mip_start &&
        cached.nativeMipCount == mip_count) {
        cached.lastUsedFrame = ctx.frameSerial;
        return cached.texture;
    }
    if (cached && cached.textureAsset == asset && cached.pendingGeneration == cache_key &&
        cached.nativeMipStart == mip_start && cached.nativeMipCount == mip_count &&
        cached.uploadInProgress) {
        status = metal_continue_native_texture_upload(ctx, cached);
        if (status == METAL_UPLOAD_FAILED) {
            metal_fail_texture_upload(ctx, cached, key, cache_key);
        } else if (status == METAL_UPLOAD_COMPLETE) {
            cached.fallbackEntry = nil;
        }
        return status == METAL_UPLOAD_COMPLETE ? cached.texture
                                               : metal_visible_texture(ctx.textureCache[key]);
    }
    VGFXMetalTextureCacheEntry *candidate = [[VGFXMetalTextureCacheEntry alloc] init];
    if (!candidate)
        return metal_visible_texture(cached);
    candidate.fallbackEntry = cached.uploadInProgress ? cached.fallbackEntry : cached;
    status =
        metal_start_native_texture_upload(ctx, candidate, asset, cache_key, mip_start, mip_count);
    if (status == METAL_UPLOAD_FAILED) {
        metal_fail_texture_upload(ctx, candidate, key, cache_key);
        return metal_visible_texture(ctx.textureCache[key]);
    }
    ctx.textureCache[key] = candidate;
    if (status == METAL_UPLOAD_COMPLETE)
        candidate.fallbackEntry = nil;
    metal_bound_texture_cache(ctx, key);
    return status == METAL_UPLOAD_COMPLETE ? candidate.texture : metal_visible_texture(candidate);
}

/// @brief Resolve a material's texture to an MTLTexture, preferring native blocks then RGBA Pixels.
/// @param ctx Borrowed Metal context owning both native and decoded texture caches.
/// @param asset Borrowed native texture asset, or NULL when only decoded Pixels are available.
/// @param pixels_ptr Borrowed decoded Pixels fallback, or NULL.
/// @param asset_cache_key Stable native asset generation.
/// @param mip_start First resident native mip.
/// @param mip_count Number of resident native mips.
/// @return The texture to bind, or nil if neither source is uploadable yet.
static id<MTLTexture> metal_get_material_texture(VGFXMetalContext *ctx,
                                                 void *asset,
                                                 const void *pixels_ptr,
                                                 uint64_t asset_cache_key,
                                                 int64_t mip_start,
                                                 int64_t mip_count) {
    /* Native-supported assets stay on the native upload path. Falling back to
     * RGBA while native upload is budget-paused leaves abandoned pending bytes. */
    if (asset && vgfx3d_textureasset_native_supported(
                     asset, metal_get_native_texture_caps((__bridge void *)ctx)))
        return metal_get_cached_native_texture(ctx, asset, asset_cache_key, mip_start, mip_count);
    return pixels_ptr ? metal_get_cached_texture(ctx, pixels_ptr) : nil;
}

/// @brief Upload one full face image (BGRA-swizzled) to a specific cubemap mip level.
/// @param tex Borrowed destination BGRA8 cubemap texture.
/// @param rgba Borrowed tightly packed RGBA8 face image.
/// @param w Source image width in pixels.
/// @param h Source image height in pixels.
/// @param face Destination cubemap face index in `[0, 5]`.
/// @param level Destination mip level.
/// @return 1 after successful validation, conversion, and replacement; otherwise 0.
static int metal_upload_rgba_to_bgra_cubemap_level(
    id<MTLTexture> tex, const uint8_t *rgba, int32_t w, int32_t h, int32_t face, int32_t level) {
    uint8_t *bgra;
    size_t pixel_count;

    if (!tex || !rgba || w <= 0 || h <= 0 || face < 0 || face >= 6 || level < 0)
        return 0;
    if ((NSUInteger)level >= tex.mipmapLevelCount)
        return 0;
    {
        NSUInteger mip_width = tex.width;
        NSUInteger mip_height = tex.height;
        for (int32_t mip = 0; mip < level; mip++) {
            mip_width = mip_width > 1u ? mip_width / 2u : 1u;
            mip_height = mip_height > 1u ? mip_height / 2u : 1u;
        }
        if ((NSUInteger)w != mip_width || (NSUInteger)h != mip_height)
            return 0;
    }
    pixel_count = (size_t)w * (size_t)h;
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
    [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
           mipmapLevel:(NSUInteger)level
                 slice:(NSUInteger)face
             withBytes:bgra
           bytesPerRow:(NSUInteger)(uint32_t)w * 4u
         bytesPerImage:(NSUInteger)(pixel_count * 4u)];
    free(bgra);
    return 1;
}

/// @brief Validate and measure a cubemap's complete prefiltered IBL tail without decoding it.
/// @param cubemap Borrowed runtime cubemap containing the prefiltered face images.
/// @param face_size Validated base cubemap face size.
/// @param[out] out_level_base Required destination for the first IBL mip level.
/// @param[out] out_bytes Required destination for the complete six-face upload byte count.
/// @return 1 when every IBL image has the expected extent and byte size, otherwise 0.
static int metal_measure_ibl_upload(const rt_cubemap3d *cubemap,
                                    int32_t face_size,
                                    int32_t *out_level_base,
                                    uint64_t *out_bytes) {
    int32_t level_base;
    uint64_t total = 0;

    if (out_level_base)
        *out_level_base = 0;
    if (out_bytes)
        *out_bytes = 0;
    if (!cubemap || !out_level_base || !out_bytes || !cubemap->ibl_ready ||
        cubemap->ibl_identity == 0 ||
        !vgfx3d_validate_cubemap_ibl_layout(face_size,
                                            cubemap->ibl_base_size,
                                            cubemap->ibl_mip_count,
                                            RT_CUBEMAP3D_IBL_MAX_MIPS,
                                            &level_base))
        return 0;
    for (int32_t mip = 0; mip < cubemap->ibl_mip_count; mip++) {
        int32_t expected_extent;
        uint64_t expected_bytes;
        if (!vgfx3d_expected_square_mip_extent(face_size, level_base + mip, &expected_extent))
            return 0;
        expected_bytes =
            (uint64_t)(uint32_t)expected_extent * (uint64_t)(uint32_t)expected_extent * 4u;
        for (int32_t face = 0; face < 6; face++) {
            int32_t width;
            int32_t height;
            uint64_t bytes;
            if (!vgfx3d_get_pixels_extent(cubemap->ibl_mips[mip][face], &width, &height) ||
                width != expected_extent || height != expected_extent ||
                !vgfx3d_estimate_pixels_rgba_upload_bytes(cubemap->ibl_mips[mip][face], &bytes) ||
                bytes != expected_bytes || total > UINT64_MAX - bytes)
                return 0;
            total += bytes;
        }
    }
    *out_level_base = level_base;
    *out_bytes = total;
    return 1;
}

/// @brief Overlay the cubemap's GGX-prefiltered IBL chain onto the texture's tail mips.
/// @details Validates and decodes the complete chain before modifying the GPU resource. Whole-chain
///   uploads honor the frame budget, while one oversize chain may run at the start of a nonzero-
///   budget frame so it cannot starve forever. Invalid identities are memoized until content
///   changes.
/// @param ctx Borrowed Metal context providing budget state and upload accounting.
/// @param entry Borrowed complete cubemap-cache entry receiving the IBL mip tail.
/// @param cubemap Borrowed runtime cubemap containing validated prefiltered images.
static void metal_apply_ibl_mips(VGFXMetalContext *ctx,
                                 VGFXMetalCubemapCacheEntry *entry,
                                 const rt_cubemap3d *cubemap) {
    uint8_t *rgba[RT_CUBEMAP3D_IBL_MAX_MIPS][6] = {{NULL}};
    uint64_t upload_bytes;
    int32_t level_base;
    int upload_ok = 0;

    if (!ctx || !entry || !entry.texture || entry.uploadInProgress || !cubemap)
        return;
    if (!cubemap->ibl_ready || cubemap->ibl_identity == 0) {
        entry.pendingIblIdentity = 0;
        entry.pendingIblBytes = 0;
        return;
    }
    if (entry.failedIblIdentity == cubemap->ibl_identity)
        return;
    if (entry.appliedIblIdentity == cubemap->ibl_identity) {
        entry.pendingIblIdentity = 0;
        entry.pendingIblBytes = 0;
        return;
    }
    if (!metal_measure_ibl_upload(cubemap, entry.faceSize, &level_base, &upload_bytes)) {
        entry.failedIblIdentity = cubemap->ibl_identity;
        entry.pendingIblIdentity = 0;
        entry.pendingIblBytes = 0;
        return;
    }
    entry.pendingIblIdentity = cubemap->ibl_identity;
    entry.pendingIblBytes = upload_bytes;
    if (!vgfx3d_upload_budget_allows(
            ctx.textureUploadBudgetBytes, ctx.textureUploadBytes, upload_bytes) &&
        (ctx.textureUploadBudgetBytes == 0 || ctx.textureUploadBytes != 0))
        return;
    for (int32_t mip = 0; mip < cubemap->ibl_mip_count; mip++) {
        int32_t expected_extent;
        if (!vgfx3d_expected_square_mip_extent(entry.faceSize, level_base + mip, &expected_extent))
            goto cleanup;
        for (int32_t face = 0; face < 6; face++) {
            int32_t width = 0;
            int32_t height = 0;
            if (vgfx3d_unpack_pixels_rgba(
                    cubemap->ibl_mips[mip][face], &width, &height, &rgba[mip][face]) != 0 ||
                !rgba[mip][face] || width != expected_extent || height != expected_extent)
                goto cleanup;
        }
    }

    for (int32_t mip = 0; mip < cubemap->ibl_mip_count; mip++) {
        int32_t extent;
        if (!vgfx3d_expected_square_mip_extent(entry.faceSize, level_base + mip, &extent))
            goto cleanup;
        for (int32_t face = 0; face < 6; face++) {
            if (!metal_upload_rgba_to_bgra_cubemap_level(
                    entry.texture, rgba[mip][face], extent, extent, face, level_base + mip))
                goto cleanup;
        }
    }
    upload_ok = 1;

cleanup:
    for (int32_t mip = 0; mip < RT_CUBEMAP3D_IBL_MAX_MIPS; mip++)
        for (int32_t face = 0; face < 6; face++)
            free(rgba[mip][face]);
    if (upload_ok) {
        metal_record_texture_upload_bytes(ctx, upload_bytes);
        entry.appliedIblIdentity = cubemap->ibl_identity;
        entry.failedIblIdentity = 0;
    } else {
        entry.failedIblIdentity = cubemap->ibl_identity;
    }
    entry.pendingIblIdentity = 0;
    entry.pendingIblBytes = 0;
}

/// @brief Upload more of an in-progress cubemap (face by face, row band by row band) within budget.
/// @param ctx Borrowed Metal context providing the current budget and accounting counters.
/// @param entry Borrowed cubemap-cache entry containing the destination and face/row cursor.
/// @param cubemap Borrowed runtime cubemap source.
/// @return `METAL_UPLOAD_COMPLETE` when all faces publish, `METAL_UPLOAD_PENDING` when
/// budget-paused,
///         or `METAL_UPLOAD_FAILED` for invalid source/resource state.
static metal_upload_status_t metal_continue_cubemap_upload(VGFXMetalContext *ctx,
                                                           VGFXMetalCubemapCacheEntry *entry,
                                                           const rt_cubemap3d *cubemap) {
    if (!ctx || !entry || !cubemap || !entry.uploadInProgress || !entry.texture ||
        entry.faceSize <= 0)
        return METAL_UPLOAD_FAILED;

    while (entry.uploadFace < 6) {
        if (ctx.textureUploadBudgetBytes != UINT64_MAX &&
            ctx.textureUploadBytes >= ctx.textureUploadBudgetBytes)
            return METAL_UPLOAD_PENDING;
        int32_t rows = vgfx3d_upload_rows_for_budget(entry.faceSize,
                                                     entry.faceSize,
                                                     entry.uploadNextRow,
                                                     ctx.textureUploadBudgetBytes,
                                                     ctx.textureUploadBytes);
        int32_t slice_size = 0;
        int32_t slice_rows = 0;
        uint8_t *rgba = NULL;
        uint64_t bytes;

        if (rows <= 0)
            return METAL_UPLOAD_FAILED;
        if (vgfx3d_unpack_cubemap_rgba_rows(cubemap,
                                            entry.uploadFace,
                                            entry.uploadNextRow,
                                            rows,
                                            0,
                                            &slice_size,
                                            &slice_rows,
                                            &rgba) != 0 ||
            !rgba || slice_size != entry.faceSize || slice_rows <= 0) {
            free(rgba);
            return METAL_UPLOAD_FAILED;
        }
        if (!metal_upload_rgba_to_bgra_cubemap_rows(entry.texture,
                                                    rgba,
                                                    slice_size,
                                                    entry.uploadFace,
                                                    entry.uploadNextRow,
                                                    slice_rows)) {
            free(rgba);
            return METAL_UPLOAD_FAILED;
        }
        bytes = (uint64_t)(uint32_t)slice_size * (uint64_t)(uint32_t)slice_rows * 4u;
        metal_record_texture_upload_bytes(ctx, bytes);
        free(rgba);

        entry.uploadNextRow += slice_rows;
        entry.lastUsedFrame = ctx.frameSerial;
        if (entry.uploadNextRow < entry.faceSize)
            return METAL_UPLOAD_PENDING;
        entry.uploadFace++;
        entry.uploadNextRow = 0;
    }

    if (!metal_generate_mipmaps(ctx, entry.texture))
        return METAL_UPLOAD_FAILED;
    entry.generation = entry.pendingGeneration;
    entry.pendingGeneration = 0;
    entry.uploadInProgress = 0;
    metal_apply_ibl_mips(ctx, entry, cubemap);
    return METAL_UPLOAD_COMPLETE;
}

/// @brief Begin uploading a cubemap: create the cube texture and seed the face/row cursor.
/// @param ctx Borrowed Metal context providing the allocation device and upload budget.
/// @param entry Borrowed cubemap-cache entry initialized for the new upload.
/// @param cubemap Borrowed runtime cubemap source.
/// @param generation Stable source generation published after all six faces upload.
/// @return Complete, pending, or failed upload status after attempting the first row band.
static metal_upload_status_t metal_start_cubemap_upload(VGFXMetalContext *ctx,
                                                        VGFXMetalCubemapCacheEntry *entry,
                                                        const rt_cubemap3d *cubemap,
                                                        uint64_t generation) {
    int32_t face_size = 0;
    int32_t mip_count;

    if (!ctx || !entry || !cubemap || !vgfx3d_get_cubemap_face_size(cubemap, &face_size) ||
        !vgfx3d_metal_is_valid_texture2d_extent(face_size, face_size)) {
        return METAL_UPLOAD_FAILED;
    }

    mip_count = vgfx3d_metal_compute_mip_count(face_size, face_size);
    if (!entry.texture || (int32_t)entry.texture.width != face_size) {
        id<MTLTexture> replacement;
        MTLTextureDescriptor *cubeDesc =
            [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                  size:(NSUInteger)face_size
                                                             mipmapped:(mip_count > 1)];
        cubeDesc.usage = MTLTextureUsageShaderRead;
        cubeDesc.storageMode = MTLStorageModeShared;
        replacement = [ctx.device newTextureWithDescriptor:cubeDesc];
        if (!replacement)
            return METAL_UPLOAD_FAILED;
        entry.texture = replacement;
    }
    if (!entry.texture)
        return METAL_UPLOAD_FAILED;

    entry.pendingGeneration = generation;
    entry.failedGeneration = 0;
    entry.generation = 0;
    entry.faceSize = face_size;
    entry.uploadFace = 0;
    entry.uploadNextRow = 0;
    entry.uploadInProgress = 1;
    entry.appliedIblIdentity = 0; /* base faces changed: re-overlay after upload */
    entry.failedIblIdentity = 0;
    entry.pendingIblIdentity = 0;
    entry.pendingIblBytes = 0;
    entry.lastUsedFrame = ctx.frameSerial;
    return metal_continue_cubemap_upload(ctx, entry, cubemap);
}

/// @brief Enforce the hard cubemap-cache cardinality limit after successful insertion.
/// @param ctx Borrowed cache owner.
/// @param protected_key Newly inserted key that must remain resident.
static void metal_bound_cubemap_cache(VGFXMetalContext *ctx, NSValue *protected_key) {
    while (ctx && ctx.cubemapCache.count > k_cubemap_cache_max_entries) {
        NSValue *oldest_key = nil;
        uint64_t oldest_frame = UINT64_MAX;
        for (NSValue *candidate_key in ctx.cubemapCache) {
            if ([candidate_key isEqual:protected_key])
                continue;
            VGFXMetalCubemapCacheEntry *candidate = ctx.cubemapCache[candidate_key];
            if (!candidate || candidate.lastUsedFrame < oldest_frame) {
                oldest_key = candidate_key;
                oldest_frame = candidate ? candidate.lastUsedFrame : 0;
            }
        }
        if (!oldest_key)
            break;
        [ctx.cubemapCache removeObjectForKey:oldest_key];
    }
}

/// @brief Get the Metal cubemap texture for a Cubemap3D, creating/streaming it on a cache miss.
/// @details Keeps a previous complete cubemap visible while a replacement upload is in progress.
/// @param ctx Borrowed Metal context owning the cubemap cache and upload budget.
/// @param cubemap Borrowed runtime cubemap used as source and cache identity.
/// @return The complete cached cubemap, its previous complete generation during replacement, or
///         nil when no complete generation exists.
static id<MTLTexture> metal_get_cached_cubemap(VGFXMetalContext *ctx, const rt_cubemap3d *cubemap) {
    uint64_t generation;
    NSValue *key;
    VGFXMetalCubemapCacheEntry *cached;
    metal_upload_status_t status;

    if (!ctx || !cubemap)
        return nil;

    key = [NSValue valueWithPointer:cubemap];
    cached = ctx.cubemapCache[key];
    generation = vgfx3d_get_cubemap_generation(cubemap);
    if (cached && cached.failedGeneration == generation) {
        cached.lastUsedFrame = ctx.frameSerial;
        return metal_visible_cubemap(cached);
    }
    if (cached && cached.texture && cached.generation == generation) {
        cached.lastUsedFrame = ctx.frameSerial;
        metal_apply_ibl_mips(ctx, cached, cubemap);
        return cached.texture;
    }
    if (cached && cached.pendingGeneration == generation && cached.uploadInProgress) {
        cached.lastUsedFrame = ctx.frameSerial;
        status = metal_continue_cubemap_upload(ctx, cached, cubemap);
        if (status == METAL_UPLOAD_FAILED) {
            metal_fail_cubemap_upload(ctx, cached, key, generation);
        } else if (status == METAL_UPLOAD_COMPLETE) {
            cached.fallbackEntry = nil;
        }
        return status == METAL_UPLOAD_COMPLETE ? cached.texture
                                               : metal_visible_cubemap(ctx.cubemapCache[key]);
    }
    VGFXMetalCubemapCacheEntry *candidate = [[VGFXMetalCubemapCacheEntry alloc] init];
    if (!candidate)
        return metal_visible_cubemap(cached);
    candidate.fallbackEntry = cached.uploadInProgress ? cached.fallbackEntry : cached;
    status = metal_start_cubemap_upload(ctx, candidate, cubemap, generation);
    if (status == METAL_UPLOAD_FAILED) {
        metal_fail_cubemap_upload(ctx, candidate, key, generation);
        return metal_visible_cubemap(ctx.cubemapCache[key]);
    }
    ctx.cubemapCache[key] = candidate;
    if (status == METAL_UPLOAD_COMPLETE)
        candidate.fallbackEntry = nil;
    metal_bound_cubemap_cache(ctx, key);
    return status == METAL_UPLOAD_COMPLETE ? candidate.texture : metal_visible_cubemap(candidate);
}

typedef struct {
    int32_t shape_count;
    int has_prev_weights;
    int has_normal_deltas;
} metal_morph_bind_status_t;

/// @brief Compute the byte size of a morph-target GPU payload for a vertex count and channel set.
/// @param vertex_count Number of vertices represented in every morph shape.
/// @param morph_count Number of morph shapes in the payload.
/// @param[out] out_bytes Required destination for the computed byte count.
/// @return 1 with the size written, 0 on overflow or invalid inputs.
static int metal_compute_morph_payload_bytes(uint32_t vertex_count,
                                             int32_t morph_count,
                                             size_t *out_bytes) {
    size_t elements;
    size_t bytes;

    if (out_bytes)
        *out_bytes = 0;
    if (!out_bytes || vertex_count == 0 || morph_count <= 0)
        return 0;
    if ((size_t)morph_count > SIZE_MAX / (size_t)vertex_count)
        return 0;
    elements = (size_t)morph_count * (size_t)vertex_count;
    if (elements > SIZE_MAX / 3u)
        return 0;
    elements *= 3u;
    if (elements > SIZE_MAX / sizeof(float))
        return 0;
    bytes = elements * sizeof(float);
    *out_bytes = bytes;
    return 1;
}

/// @brief Get or build the cached GPU morph-delta buffer for a mesh's morph targets.
/// @details Keyed by mesh + morph revision so it rebuilds when the deltas change. NULL on failure.
/// @param ctx Borrowed Metal context owning the morph-buffer cache.
/// @param cmd Borrowed draw command containing morph identity, deltas, and vertex count.
/// @param morph_count Sanitized number of morph shapes to upload.
/// @return A complete cache entry matching @p cmd, or nil on validation/allocation failure.
static VGFXMetalMorphCacheEntry *metal_get_cached_morph_entry(VGFXMetalContext *ctx,
                                                              const vgfx3d_draw_cmd_t *cmd,
                                                              int32_t morph_count) {
    NSValue *key;
    VGFXMetalMorphCacheEntry *entry;
    id<MTLBuffer> replacement_delta;
    id<MTLBuffer> replacement_normal;
    size_t bytes;

    if (!ctx || !ctx.morphCache || !cmd || morph_count <= 0 || !cmd->morph_key ||
        cmd->morph_revision == 0 || !cmd->morph_deltas || !cmd->morph_weights ||
        cmd->morph_shape_count <= 0 || cmd->vertex_count == 0) {
        return nil;
    }

    key = [NSValue valueWithPointer:cmd->morph_key];
    entry = ctx.morphCache[key];
    if (entry && entry.deltaBuffer &&
        vgfx3d_metal_should_reuse_morph_cache(entry.key,
                                              entry.revision,
                                              entry.shapeCount,
                                              entry.vertexCount,
                                              entry.hasNormalDeltas,
                                              cmd)) {
        entry.lastUsedFrame = ctx.frameSerial;
        return entry;
    }

    if (!metal_compute_morph_payload_bytes(cmd->vertex_count, morph_count, &bytes))
        return nil;
    replacement_delta = metal_new_shared_buffer(ctx, cmd->morph_deltas, bytes);
    replacement_normal = cmd->morph_normal_deltas
                             ? metal_new_shared_buffer(ctx, cmd->morph_normal_deltas, bytes)
                             : nil;
    if (!replacement_delta || (cmd->morph_normal_deltas && !replacement_normal))
        return nil;
    entry = [[VGFXMetalMorphCacheEntry alloc] init];
    if (!entry)
        return nil;
    entry.deltaBuffer = replacement_delta;
    entry.normalBuffer = replacement_normal;
    entry.key = cmd->morph_key;
    entry.revision = cmd->morph_revision;
    entry.shapeCount = morph_count;
    entry.vertexCount = cmd->vertex_count;
    entry.hasNormalDeltas = entry.normalBuffer ? 1 : 0;
    entry.lastUsedFrame = ctx.frameSerial;
    if (!ctx.morphCache[key] && ctx.morphCache.count >= k_morph_cache_max_entries) {
        NSValue *oldest_key = nil;
        uint64_t oldest_frame = UINT64_MAX;
        for (NSValue *candidate_key in ctx.morphCache) {
            VGFXMetalMorphCacheEntry *candidate_entry = ctx.morphCache[candidate_key];
            if (!candidate_entry || candidate_entry.lastUsedFrame < oldest_frame) {
                oldest_key = candidate_key;
                oldest_frame = candidate_entry ? candidate_entry.lastUsedFrame : 0;
            }
        }
        if (oldest_key)
            [ctx.morphCache removeObjectForKey:oldest_key];
    }
    ctx.morphCache[key] = entry;
    return entry;
}

/// @brief Bind a draw's skeletal bone matrices (the skinning palette) into the vertex stage.
/// @details Palettes are appended into the transient ring instead of allocating fresh
///          MTLBuffers — with many skinned characters the per-draw device allocations were a
///          measurable frame-time cost. The palette stays identity-padded to the full
///          VGFX3D_METAL_MAX_BONES because the shader clamps bone indices to the palette
///          size, not the draw's bone count (the MSL PerScene/PerObject ABI is contractual).
/// @param ctx Borrowed Metal context with an active encoder and transient upload ring.
/// @param cmd Borrowed draw command containing current and optional prior bone matrices.
/// @param has_skinning Nonzero when the current palette should be uploaded.
/// @param has_prev_skinning Nonzero when a previous-frame palette should also be attempted.
/// @param[out] out_prev_ok Optional destination set nonzero when the prior palette binds.
/// @return 1 on success, 0 if the palette is missing or the upload failed.
static int metal_bind_bone_palettes(VGFXMetalContext *ctx,
                                    const vgfx3d_draw_cmd_t *cmd,
                                    int has_skinning,
                                    int has_prev_skinning,
                                    int *out_prev_ok) {
    float packed_palette[VGFX3D_METAL_MAX_BONES * 16];
    size_t palette_bytes = sizeof(packed_palette);
    id<MTLBuffer> bone_buf;
    NSUInteger bone_offset = 0;

    if (out_prev_ok)
        *out_prev_ok = 0;

    if (!ctx || !cmd || !has_skinning)
        return 0;

    vgfx3d_metal_pack_bone_palette(packed_palette, cmd->bone_palette, cmd->bone_count);
    bone_buf = metal_upload_transient_buffer(
        ctx, VGFX3D_METAL_TRANSIENT_VERTEX, packed_palette, palette_bytes, &bone_offset);
    if (!bone_buf)
        return 0;
    [ctx.encoder setVertexBuffer:bone_buf offset:bone_offset atIndex:3];
    if (has_prev_skinning) {
        id<MTLBuffer> prev_bone_buf;
        NSUInteger prev_offset = 0;
        vgfx3d_metal_pack_bone_palette(packed_palette, cmd->prev_bone_palette, cmd->bone_count);
        prev_bone_buf = metal_upload_transient_buffer(
            ctx, VGFX3D_METAL_TRANSIENT_VERTEX, packed_palette, palette_bytes, &prev_offset);
        if (prev_bone_buf) {
            [ctx.encoder setVertexBuffer:prev_bone_buf offset:prev_offset atIndex:7];
            if (out_prev_ok)
                *out_prev_ok = 1;
        }
    }
    return 1;
}

/// @brief Bind a mesh's influences 5-8 side stream (vertex buffer index 10).
/// @details Import-time data cached by geometry identity + revision (like the
///   morph cache); cache misses upload a shared buffer and keep it alive via
///   frameBuffers until the cache entry takes over. Returns 1 when bound (the
///   caller sets PerObject flags2.y), 0 when the draw has no extras or the
///   upload failed (CPU fallback already handled 8-weight meshes before the
///   backend advertised gpu_skinning_extras, so failure = fewer influences,
///   never corruption).
/// @param ctx Borrowed Metal context with an active encoder and influence cache.
/// @param cmd Borrowed draw command containing the optional side-stream and geometry identity.
/// @return 1 when a valid side-stream buffer is bound, otherwise 0.
static int metal_bind_extra_influences(VGFXMetalContext *ctx, const vgfx3d_draw_cmd_t *cmd) {
    id<MTLBuffer> buf = nil;
    const void *key;
    NSNumber *dict_key;
    VGFXMetalExtraInfluenceCacheEntry *entry;
    size_t bytes;
    if (!ctx || !cmd || !cmd->extra_influences || cmd->bone_count <= 0 || cmd->vertex_count == 0)
        return 0;
    if ((size_t)cmd->vertex_count > SIZE_MAX / sizeof(vgfx3d_extra_influences_t))
        return 0;
    bytes = (size_t)cmd->vertex_count * sizeof(vgfx3d_extra_influences_t);
    key = cmd->geometry_key ? cmd->geometry_key : (const void *)cmd->extra_influences;
    dict_key = [NSNumber numberWithUnsignedLongLong:(unsigned long long)(uintptr_t)key];
    entry = ctx.extraInfluenceCache ? ctx.extraInfluenceCache[dict_key] : nil;
    if (entry && entry.key == key && entry.revision == cmd->geometry_revision &&
        entry.vertexCount == cmd->vertex_count) {
        buf = entry.buffer;
        entry.lastUsedFrame = ctx.frameSerial;
    } else {
        buf = metal_new_shared_buffer(ctx, cmd->extra_influences, bytes);
        if (!buf)
            return 0;
        if (ctx.extraInfluenceCache) {
            VGFXMetalExtraInfluenceCacheEntry *fresh =
                [[VGFXMetalExtraInfluenceCacheEntry alloc] init];
            fresh.buffer = buf;
            fresh.key = key;
            fresh.revision = cmd->geometry_revision;
            fresh.vertexCount = cmd->vertex_count;
            fresh.lastUsedFrame = ctx.frameSerial;
            ctx.extraInfluenceCache[dict_key] = fresh;
        } else if (ctx.frameBuffers) {
            [ctx.frameBuffers addObject:buf];
        }
    }
    if (!buf)
        return 0;
    [ctx.encoder setVertexBuffer:buf offset:0 atIndex:10];
    return 1;
}

/// @brief Bind a mesh's morph delta buffer and per-shape weights for a draw.
/// @param ctx Borrowed Metal context with an active encoder and morph-buffer cache.
/// @param cmd Borrowed draw command containing morph deltas, weights, and cache identity.
/// @return A status enum indicating whether morphing is bound, skipped, or failed.
static metal_morph_bind_status_t metal_bind_morph_payload(VGFXMetalContext *ctx,
                                                          const vgfx3d_draw_cmd_t *cmd) {
    metal_morph_bind_status_t status = {0, 0, 0};
    VGFXMetalMorphCacheEntry *cached_entry;
    id<MTLBuffer> delta_buf = nil;
    id<MTLBuffer> normal_buf = nil;
    int32_t morph_count;
    size_t delta_bytes;
    size_t weight_bytes;

    if (!ctx || !cmd || !cmd->morph_deltas || !cmd->morph_weights || cmd->morph_shape_count <= 0)
        return status;
    morph_count = vgfx3d_metal_clamp_morph_shape_count(cmd->vertex_count, cmd->morph_shape_count);
    if (morph_count <= 0)
        return status;

    cached_entry = metal_get_cached_morph_entry(ctx, cmd, morph_count);
    if (cached_entry) {
        delta_buf = cached_entry.deltaBuffer;
        normal_buf = cached_entry.normalBuffer;
    } else {
        if (!metal_compute_morph_payload_bytes(cmd->vertex_count, morph_count, &delta_bytes))
            return status;
        delta_buf = metal_new_shared_buffer(ctx, cmd->morph_deltas, delta_bytes);
        normal_buf = cmd->morph_normal_deltas
                         ? metal_new_shared_buffer(ctx, cmd->morph_normal_deltas, delta_bytes)
                         : nil;
        if (!delta_buf || (cmd->morph_normal_deltas && !normal_buf))
            return status;
        if (ctx.frameBuffers && delta_buf)
            [ctx.frameBuffers addObject:delta_buf];
        if (ctx.frameBuffers && normal_buf)
            [ctx.frameBuffers addObject:normal_buf];
    }
    if (!delta_buf || (cmd->morph_normal_deltas && !normal_buf))
        return status;

    weight_bytes = (size_t)morph_count * sizeof(float);
    [ctx.encoder setVertexBuffer:delta_buf offset:0 atIndex:4];
    if (normal_buf)
        [ctx.encoder setVertexBuffer:normal_buf offset:0 atIndex:9];
    [ctx.encoder setVertexBytes:cmd->morph_weights length:weight_bytes atIndex:5];
    if (cmd->prev_morph_weights)
        [ctx.encoder setVertexBytes:cmd->prev_morph_weights length:weight_bytes atIndex:8];
    status.shape_count = morph_count;
    status.has_prev_weights = cmd->prev_morph_weights ? 1 : 0;
    status.has_normal_deltas = normal_buf ? 1 : 0;
    return status;
}

/// @brief Ensure the CPU-side instance staging scratch can hold @p instance_count instances.
/// @details GPU storage is a per-draw slice of the transient ring (see
///          metal_upload_transient_buffer); only the packing scratch persists here.
/// @param ctx Borrowed Metal context owning the mutable instance scratch allocation.
/// @param instance_count Minimum number of packed instance entries required.
/// @return Non-zero when scratch storage for every requested instance is ready.
static int metal_ensure_instance_storage(VGFXMetalContext *ctx, int32_t instance_count) {
    int32_t needed_capacity;
    int32_t next_capacity;
    NSUInteger byte_count;

    if (!ctx || instance_count <= 0)
        return 0;
    if (ctx.instanceCapacity > (NSUInteger)INT32_MAX)
        return 0;
    if (ctx.instanceScratch && ctx.instanceCapacity >= (NSUInteger)instance_count)
        return 1;

    needed_capacity = instance_count;
    next_capacity = vgfx3d_metal_next_capacity((int32_t)ctx.instanceCapacity, needed_capacity, 64);
    if (next_capacity < needed_capacity ||
        (size_t)next_capacity > SIZE_MAX / sizeof(vgfx3d_metal_instance_data_t)) {
        return 0;
    }
    if ((NSUInteger)next_capacity > ctx.instanceCapacity || !ctx.instanceScratch) {
        NSMutableData *replacement;
        byte_count = (NSUInteger)next_capacity * sizeof(vgfx3d_metal_instance_data_t);
        replacement = [NSMutableData dataWithLength:byte_count];
        if (!replacement || replacement.length < byte_count)
            return 0;
        ctx.instanceScratch = replacement;
        ctx.instanceCapacity = (NSUInteger)next_capacity;
    }
    return ctx.instanceScratch && ctx.instanceCapacity >= (NSUInteger)instance_count;
}
