//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_texture.inc
// Purpose: Implement D3D11 texture/cubemap caches, streamed uploads, and SRV creation.
// Key invariants:
//   - Replacement resources are complete before a live cache entry is evicted.
//   - Pending uploads publish their generation only after every row/face is resident.
// Ownership/Lifetime:
//   - Cache entries own their COM texture and view references until explicit release.
//   - This file is included by vgfx3d_backend_d3d11.c and is not a translation unit.
// Links: vgfx3d_backend_d3d11.c, vgfx3d_backend_d3d11_targets.inc
//
//===----------------------------------------------------------------------===//

/// @file vgfx3d_backend_d3d11_texture.inc
/// @brief Implements D3D11 texture, cubemap, and morph-resource caches plus
///   budgeted upload and shader-resource-view creation paths.
/// @details Cache entries own their COM resources, retain stable runtime identities,
///   and publish new generations only after upload completion. In-progress uploads
///   use white fallback views when rendering may continue safely.

// Texture and cubemap caches — keyed by stable host identity + generation so
// re-uploaded textures (Pixels.Set) get fresh GPU resources without
// aliasing allocator-reused addresses to stale uploads.

static HRESULT d3d11_allocate_texture_srv(d3d11_context_t *ctx,
                                          int32_t w,
                                          int32_t h,
                                          ID3D11Texture2D **out_tex,
                                          ID3D11ShaderResourceView **out_srv);
static HRESULT d3d11_allocate_cubemap_srv(d3d11_context_t *ctx,
                                          int32_t face_size,
                                          ID3D11Texture2D **out_tex,
                                          ID3D11ShaderResourceView **out_srv);

/// @brief Return the white 2D fallback SRV and record the fallback bind.
/// @param[in,out] ctx Context containing the fallback view and diagnostics counters.
/// @return Borrowed white 2D SRV, or `NULL` when unavailable.
static ID3D11ShaderResourceView *d3d11_fallback_white_srv(d3d11_context_t *ctx) {
    if (!ctx || !ctx->fallback_white_srv)
        return NULL;
    ctx->stats.texture_fallback_binds =
        vgfx3d_d3d11_saturating_add_u64(ctx->stats.texture_fallback_binds, 1u);
    return ctx->fallback_white_srv;
}

/// @brief Return the white cubemap fallback SRV and record the fallback bind.
/// @param[in,out] ctx Context containing the fallback view and diagnostics counters.
/// @return Borrowed white cubemap SRV, or `NULL` when unavailable.
static ID3D11ShaderResourceView *d3d11_fallback_white_cubemap_srv(d3d11_context_t *ctx) {
    if (!ctx || !ctx->fallback_white_cube_srv)
        return NULL;
    ctx->stats.texture_fallback_binds =
        vgfx3d_d3d11_saturating_add_u64(ctx->stats.texture_fallback_binds, 1u);
    return ctx->fallback_white_cube_srv;
}

/// @brief Release one 2D-texture cache slot and clear all identity/upload metadata.
/// @param[in,out] entry Cache entry whose owned COM references and metadata are cleared.
static void d3d11_release_texture_cache_entry(d3d_tex_cache_entry_t *entry) {
    if (!entry)
        return;
    SAFE_RELEASE(entry->srv);
    SAFE_RELEASE(entry->tex);
    memset(entry, 0, sizeof(*entry));
}

/// @brief Hash an object identity into the direct-mapped texture-cache hint table.
/// @param[in] key Stable Pixels or TextureAsset3D identity.
/// @return Hint-table slot in the fixed-capacity range.
static uint32_t d3d11_texture_cache_hint_slot(const void *key) {
    uintptr_t value = (uintptr_t)key;
    value ^= value >> 7u;
    value ^= value >> 17u;
    return (uint32_t)value & (D3D11_TEXTURE_CACHE_HINT_CAPACITY - 1u);
}

/// @brief Remember the current array index for a Pixels or TextureAsset3D identity.
/// @param[in,out] ctx Context containing the hint table.
/// @param[in] key Stable source identity.
/// @param[in] index Current cache-array index.
static void d3d11_texture_cache_remember(d3d11_context_t *ctx, const void *key, int32_t index) {
    if (!ctx || !key || index < 0 || index >= ctx->tex_cache_count)
        return;
    ctx->tex_cache_hints[d3d11_texture_cache_hint_slot(key)] = index + 1;
}

/// @brief Return a validated hinted Pixels entry index, or -1 on collision/staleness.
/// @param[in] ctx Context containing the cache and hint table.
/// @param[in] pixels Stable Pixels identity to locate.
/// @return Matching cache index, or `-1`.
static int32_t d3d11_texture_cache_hint_pixels(const d3d11_context_t *ctx, const void *pixels) {
    int32_t index;
    if (!ctx || !pixels)
        return -1;
    index = ctx->tex_cache_hints[d3d11_texture_cache_hint_slot(pixels)] - 1;
    if (index < 0 || index >= ctx->tex_cache_count || ctx->tex_cache[index].pixels_ptr != pixels)
        return -1;
    return index;
}

/// @brief Return a validated hinted native-asset entry index, or -1 on collision/staleness.
/// @param[in] ctx Context containing the cache and hint table.
/// @param[in] asset Stable TextureAsset3D identity to locate.
/// @return Matching cache index, or `-1`.
static int32_t d3d11_texture_cache_hint_asset(const d3d11_context_t *ctx, const void *asset) {
    int32_t index;
    if (!ctx || !asset)
        return -1;
    index = ctx->tex_cache_hints[d3d11_texture_cache_hint_slot(asset)] - 1;
    if (index < 0 || index >= ctx->tex_cache_count || ctx->tex_cache[index].texture_asset != asset)
        return -1;
    return index;
}

/// @brief Rebuild hints after an in-place LRU compaction moved cache entries.
/// @param[in,out] ctx Context whose hint table is reconstructed from live entries.
static void d3d11_texture_cache_rebuild_hints(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    memset(ctx->tex_cache_hints, 0, sizeof(ctx->tex_cache_hints));
    for (int32_t i = 0; i < ctx->tex_cache_count; i++) {
        if (ctx->tex_cache[i].pixels_ptr)
            d3d11_texture_cache_remember(ctx, ctx->tex_cache[i].pixels_ptr, i);
        if (ctx->tex_cache[i].texture_asset)
            d3d11_texture_cache_remember(ctx, ctx->tex_cache[i].texture_asset, i);
    }
}

/// @brief Release one cubemap cache slot and clear all identity/upload metadata.
/// @param[in,out] entry Cache entry whose owned COM references and metadata are cleared.
static void d3d11_release_cubemap_cache_entry(d3d_cubemap_cache_entry_t *entry) {
    if (!entry)
        return;
    SAFE_RELEASE(entry->srv);
    SAFE_RELEASE(entry->tex);
    memset(entry, 0, sizeof(*entry));
}

/// @brief Release a failed partial 2D upload while remembering the rejected generation.
/// @param[in,out] entry Cache entry to reset and mark failed.
/// @param[in] pixels Optional Pixels identity associated with the upload.
/// @param[in] asset Optional native asset identity associated with the upload.
/// @param[in] generation Rejected content generation or native cache key.
/// @param[in] mip_start Native snapshot starting mip.
/// @param[in] mip_count Native snapshot mip count.
/// @param[in] frame_serial Frame serial recorded as the entry's last use.
static void d3d11_mark_texture_upload_failed(d3d_tex_cache_entry_t *entry,
                                             const void *pixels,
                                             void *asset,
                                             uint64_t generation,
                                             int64_t mip_start,
                                             int64_t mip_count,
                                             uint64_t frame_serial) {
    if (!entry)
        return;
    d3d11_release_texture_cache_entry(entry);
    entry->pixels_ptr = pixels;
    entry->texture_asset = asset;
    entry->failed_generation = generation;
    entry->native_mip_start = mip_start;
    entry->native_mip_count = mip_count;
    entry->last_used_frame = frame_serial;
}

/// @brief Release a failed partial cubemap upload while remembering the rejected generation.
/// @param[in,out] entry Cache entry to reset and mark failed.
/// @param[in] cubemap Stable cubemap identity associated with the upload.
/// @param[in] generation Rejected cubemap generation.
/// @param[in] frame_serial Frame serial recorded as the entry's last use.
static void d3d11_mark_cubemap_upload_failed(d3d_cubemap_cache_entry_t *entry,
                                             const rt_cubemap3d *cubemap,
                                             uint64_t generation,
                                             uint64_t frame_serial) {
    if (!entry)
        return;
    d3d11_release_cubemap_cache_entry(entry);
    entry->cubemap_ptr = cubemap;
    entry->failed_generation = generation;
    entry->last_used_frame = frame_serial;
}

/// @brief Release every entry in the 2D-texture cache (teardown path).
/// @param[in,out] ctx Context whose cache allocation and hints are cleared.
static void d3d11_release_texture_cache(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->tex_cache_count > 0)
        d3d11_unbind_draw_resources(ctx);
    for (int32_t i = 0; i < ctx->tex_cache_count; i++)
        d3d11_release_texture_cache_entry(&ctx->tex_cache[i]);
    ctx->tex_cache_count = 0;
    free(ctx->tex_cache);
    ctx->tex_cache = NULL;
    ctx->tex_cache_capacity = 0;
    memset(ctx->tex_cache_hints, 0, sizeof(ctx->tex_cache_hints));
}

/// @brief Evict aged 2D texture-cache entries while keeping a resident floor.
/// @param[in,out] ctx Context whose cache is compacted and hints rebuilt.
static void d3d11_prune_texture_cache(d3d11_context_t *ctx) {
    int32_t write_index = 0;
    int32_t total_count;

    if (!ctx || !ctx->tex_cache)
        return;
    d3d11_unbind_draw_resources(ctx);
    total_count = ctx->tex_cache_count;
    for (int32_t i = 0; i < total_count; i++) {
        uint64_t age = ctx->frame_serial > ctx->tex_cache[i].last_used_frame
                           ? (ctx->frame_serial - ctx->tex_cache[i].last_used_frame)
                           : 0u;
        if (vgfx3d_d3d11_should_prune_cache_entry(total_count,
                                                  write_index,
                                                  i,
                                                  age,
                                                  D3D11_TEXTURE_CACHE_MAX_RESIDENT,
                                                  D3D11_TEXTURE_CACHE_PRUNE_AGE)) {
            d3d11_release_texture_cache_entry(&ctx->tex_cache[i]);
            continue;
        }
        if (write_index != i) {
            ctx->tex_cache[write_index] = ctx->tex_cache[i];
            memset(&ctx->tex_cache[i], 0, sizeof(ctx->tex_cache[i]));
        }
        write_index++;
    }
    ctx->tex_cache_count = write_index;
    d3d11_texture_cache_rebuild_hints(ctx);
}

/// @brief Release every entry in the cubemap cache (teardown path).
/// @param[in,out] ctx Context whose cubemap cache allocation is cleared.
static void d3d11_release_cubemap_cache(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->cubemap_cache_count > 0)
        d3d11_unbind_draw_resources(ctx);
    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++)
        d3d11_release_cubemap_cache_entry(&ctx->cubemap_cache[i]);
    ctx->cubemap_cache_count = 0;
    free(ctx->cubemap_cache);
    ctx->cubemap_cache = NULL;
    ctx->cubemap_cache_capacity = 0;
}

/// @brief Evict aged cubemap-cache entries while keeping a minimum resident set.
/// @details Runs a compacting sweep: entries whose `last_used_frame` is more than
///   `D3D11_CUBEMAP_CACHE_PRUNE_AGE` frames behind the current `frame_serial` are
///   released, and the rest are packed to the front of the array. The
///   `count - write_index > MAX_RESIDENT` guard means we never shrink below the
///   resident floor even if everything is aged — a scene that hasn't drawn in 10
///   seconds still keeps its working set warm for the next frame.
/// @param[in,out] ctx Context whose cubemap cache is pruned and compacted.
static void d3d11_prune_cubemap_cache(d3d11_context_t *ctx) {
    int32_t write_index = 0;
    int32_t total_count;

    if (!ctx || !ctx->cubemap_cache)
        return;
    d3d11_unbind_draw_resources(ctx);
    total_count = ctx->cubemap_cache_count;
    for (int32_t i = 0; i < total_count; i++) {
        uint64_t age = ctx->frame_serial > ctx->cubemap_cache[i].last_used_frame
                           ? (ctx->frame_serial - ctx->cubemap_cache[i].last_used_frame)
                           : 0u;
        if (vgfx3d_d3d11_should_prune_cache_entry(total_count,
                                                  write_index,
                                                  i,
                                                  age,
                                                  D3D11_CUBEMAP_CACHE_MAX_RESIDENT,
                                                  D3D11_CUBEMAP_CACHE_PRUNE_AGE)) {
            d3d11_release_cubemap_cache_entry(&ctx->cubemap_cache[i]);
            continue;
        }
        if (write_index != i) {
            ctx->cubemap_cache[write_index] = ctx->cubemap_cache[i];
            memset(&ctx->cubemap_cache[i], 0, sizeof(ctx->cubemap_cache[i]));
        }
        write_index++;
    }
    ctx->cubemap_cache_count = write_index;
}

/// @brief Grow the texture cache table to hold `needed` entries.
///
/// Geometric growth via the shared `vgfx3d_d3d11_next_capacity` helper.
/// @param[in,out] ctx Context owning the cache allocation.
/// @param[in] needed Minimum required entry capacity.
/// @return One when the cache can hold @p needed entries; otherwise zero.
static int d3d11_ensure_tex_cache_capacity(d3d11_context_t *ctx, int32_t needed) {
    int32_t new_capacity;
    d3d_tex_cache_entry_t *new_entries;

    if (!ctx || needed <= 0 || needed > D3D11_TEXTURE_CACHE_MAX_ENTRIES)
        return 0;
    if (ctx->tex_cache_capacity >= needed)
        return 1;
    new_capacity = vgfx3d_d3d11_next_capacity(ctx->tex_cache_capacity, needed, 64);
    if (new_capacity > D3D11_TEXTURE_CACHE_MAX_ENTRIES)
        new_capacity = D3D11_TEXTURE_CACHE_MAX_ENTRIES;
    if (new_capacity <= ctx->tex_cache_capacity ||
        (size_t)new_capacity > SIZE_MAX / sizeof(*new_entries))
        return 0;
    new_entries = (d3d_tex_cache_entry_t *)realloc(ctx->tex_cache,
                                                   (size_t)new_capacity * sizeof(*new_entries));
    if (!new_entries)
        return 0;
    memset(new_entries + ctx->tex_cache_capacity,
           0,
           (size_t)(new_capacity - ctx->tex_cache_capacity) * sizeof(*new_entries));
    ctx->tex_cache = new_entries;
    ctx->tex_cache_capacity = new_capacity;
    return 1;
}

/// @brief Grow the cubemap cache table to hold `needed` entries.
/// @param[in,out] ctx Context owning the cubemap cache allocation.
/// @param[in] needed Minimum required entry capacity.
/// @return One when the cache can hold @p needed entries; otherwise zero.
static int d3d11_ensure_cubemap_cache_capacity(d3d11_context_t *ctx, int32_t needed) {
    int32_t new_capacity;
    d3d_cubemap_cache_entry_t *new_entries;

    if (!ctx || needed <= 0 || needed > D3D11_CUBEMAP_CACHE_MAX_ENTRIES)
        return 0;
    if (ctx->cubemap_cache_capacity >= needed)
        return 1;
    new_capacity = vgfx3d_d3d11_next_capacity(ctx->cubemap_cache_capacity, needed, 16);
    if (new_capacity > D3D11_CUBEMAP_CACHE_MAX_ENTRIES)
        new_capacity = D3D11_CUBEMAP_CACHE_MAX_ENTRIES;
    if (new_capacity <= ctx->cubemap_cache_capacity ||
        (size_t)new_capacity > SIZE_MAX / sizeof(*new_entries))
        return 0;
    new_entries = (d3d_cubemap_cache_entry_t *)realloc(ctx->cubemap_cache,
                                                       (size_t)new_capacity * sizeof(*new_entries));
    if (!new_entries)
        return 0;
    memset(new_entries + ctx->cubemap_cache_capacity,
           0,
           (size_t)(new_capacity - ctx->cubemap_cache_capacity) * sizeof(*new_entries));
    ctx->cubemap_cache = new_entries;
    ctx->cubemap_cache_capacity = new_capacity;
    return 1;
}

/// @brief Maximum LOD index (float) for a cubemap's full mip chain.
/// @details Returned as a float because D3D11's `SampleLevel` / `SampleBias` expect
///   a float LOD. A 1×1 face has only one mip and thus max-LOD 0. Used to clamp sampler
///   LOD ranges and compute prefiltered environment-map roughness lookups.
/// @param[in] cubemap Runtime cubemap whose face extent determines the chain.
/// @return Largest zero-based mip level, or zero for invalid and 1x1 cubemaps.
static float d3d11_cubemap_max_lod(const rt_cubemap3d *cubemap) {
    int32_t face_size;
    int32_t mip_count;

    if (!cubemap || !vgfx3d_get_cubemap_face_size(cubemap, &face_size) || face_size <= 1)
        return 0.0f;
    mip_count = vgfx3d_d3d11_compute_mip_count(face_size, face_size);
    return mip_count > 1 ? (float)(mip_count - 1) : 0.0f;
}

/// @brief Release the GPU resources in one morph-target cache slot.
///
/// Each slot holds two `(buffer, srv)` pairs — one for position deltas
/// and one for normal deltas. Both are released together.
/// @param[in,out] entry Morph cache entry to release and clear.
static void d3d11_release_morph_cache_entry(d3d11_morph_cache_entry_t *entry) {
    if (!entry)
        return;
    SAFE_RELEASE(entry->normal_srv);
    SAFE_RELEASE(entry->normal_buffer);
    SAFE_RELEASE(entry->srv);
    SAFE_RELEASE(entry->buffer);
    memset(entry, 0, sizeof(*entry));
}

/// @brief Release every morph-cache slot — called during context teardown.
/// @param[in,out] ctx Context whose fixed morph cache is cleared.
static void d3d11_release_morph_cache(d3d11_context_t *ctx) {
    if (!ctx)
        return;
    for (int32_t i = 0; i < D3D11_MORPH_CACHE_CAPACITY; i++)
        d3d11_release_morph_cache_entry(&ctx->morph_cache[i]);
}

/// @brief Release a temporary (non-cached) SRV after the draw consumes it.
///
/// Used when the texture cache is full; we still create the resource
/// for the current draw but don't try to keep it around. Marking
/// `temporary` lets the per-draw cleanup distinguish it from cached
/// resources (which must NOT be released here).
/// @param[in,out] entry Temporary-resource holder; cached entries are ignored.
static void d3d11_release_temp_srv(d3d_temp_srv_t *entry) {
    if (!entry || !entry->temporary)
        return;
    SAFE_RELEASE(entry->srv);
    SAFE_RELEASE(entry->tex);
    entry->temporary = 0;
}

/// @brief Add @p bytes to the context's running per-frame texture-upload total (saturating).
/// @param[in,out] ctx Context containing the upload counter.
/// @param[in] bytes Completed upload bytes to add.
static void d3d11_record_texture_upload_bytes(d3d11_context_t *ctx, uint64_t bytes) {
    if (!ctx || bytes == 0)
        return;
    ctx->texture_upload_bytes = vgfx3d_d3d11_saturating_add_u64(ctx->texture_upload_bytes, bytes);
}

/// @brief Create a one-draw RGBA8 texture SRV when the persistent cache cannot grow.
/// @param[in,out] ctx Context used to allocate, upload, and count the resource.
/// @param[in] pixels Runtime Pixels source to unpack.
/// @param[out] out_temp Receives the temporary texture, SRV, and ownership marker.
/// @return Borrowed view from @p out_temp, or `NULL` on failure.
static ID3D11ShaderResourceView *d3d11_create_temp_texture_srv(d3d11_context_t *ctx,
                                                               const void *pixels,
                                                               d3d_temp_srv_t *out_temp) {
    uint8_t *rgba = NULL;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t row_pitch = 0;
    uint64_t upload_bytes;

    if (!ctx || !ctx->ctx || !pixels || !out_temp)
        return NULL;
    memset(out_temp, 0, sizeof(*out_temp));
    if (vgfx3d_unpack_pixels_rgba(pixels, &width, &height, &rgba) != 0 || !rgba ||
        !vgfx3d_d3d11_is_valid_texture2d_extent(width, height) ||
        !vgfx3d_d3d11_compute_rgba8_upload_pitch(width, &row_pitch)) {
        free(rgba);
        return NULL;
    }
    if (FAILED(d3d11_allocate_texture_srv(ctx, width, height, &out_temp->tex, &out_temp->srv))) {
        free(rgba);
        out_temp->temporary = 1;
        d3d11_release_temp_srv(out_temp);
        return NULL;
    }
    d3d11_unbind_draw_resources(ctx);
    ID3D11DeviceContext_UpdateSubresource(
        ctx->ctx, (ID3D11Resource *)out_temp->tex, 0, NULL, rgba, row_pitch, 0);
    ID3D11DeviceContext_GenerateMips(ctx->ctx, out_temp->srv);
    if (FAILED(d3d11_device_status_after_void_command(ctx, "Upload/generate temporary texture"))) {
        free(rgba);
        out_temp->temporary = 1;
        d3d11_release_temp_srv(out_temp);
        return NULL;
    }
    upload_bytes = (uint64_t)(uint32_t)width * (uint64_t)(uint32_t)height * 4u;
    d3d11_record_texture_upload_bytes(ctx, upload_bytes);
    free(rgba);
    out_temp->temporary = 1;
    return out_temp->srv;
}

/// @brief Create a one-draw cubemap SRV when the persistent cubemap cache cannot grow.
/// @param[in,out] ctx Context used to allocate, upload, and count the resource.
/// @param[in] cubemap Runtime cubemap source to unpack.
/// @param[out] out_temp Receives the temporary texture, SRV, and ownership marker.
/// @return Borrowed view from @p out_temp, or `NULL` on failure.
static ID3D11ShaderResourceView *d3d11_create_temp_cubemap_srv(d3d11_context_t *ctx,
                                                               const rt_cubemap3d *cubemap,
                                                               d3d_temp_srv_t *out_temp) {
    uint8_t *faces[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
    int32_t face_size = 0;
    int32_t mip_count;
    uint32_t row_pitch = 0;
    uint64_t face_bytes;

    if (!ctx || !ctx->ctx || !cubemap || !out_temp)
        return NULL;
    memset(out_temp, 0, sizeof(*out_temp));
    if (vgfx3d_unpack_cubemap_faces_rgba(cubemap, &face_size, faces) != 0 ||
        !vgfx3d_d3d11_is_valid_cubemap_extent(face_size) ||
        !vgfx3d_d3d11_compute_rgba8_upload_pitch(face_size, &row_pitch)) {
        for (int32_t i = 0; i < 6; i++)
            free(faces[i]);
        return NULL;
    }
    for (int32_t i = 0; i < 6; i++) {
        if (!faces[i]) {
            for (int32_t face = 0; face < 6; face++)
                free(faces[face]);
            return NULL;
        }
    }
    if (FAILED(d3d11_allocate_cubemap_srv(ctx, face_size, &out_temp->tex, &out_temp->srv))) {
        for (int32_t i = 0; i < 6; i++)
            free(faces[i]);
        out_temp->temporary = 1;
        d3d11_release_temp_srv(out_temp);
        return NULL;
    }
    mip_count = vgfx3d_d3d11_compute_mip_count(face_size, face_size);
    d3d11_unbind_draw_resources(ctx);
    for (int32_t face = 0; face < 6; face++) {
        ID3D11DeviceContext_UpdateSubresource(ctx->ctx,
                                              (ID3D11Resource *)out_temp->tex,
                                              D3D11CalcSubresource(0, (UINT)face, (UINT)mip_count),
                                              NULL,
                                              faces[face],
                                              row_pitch,
                                              0);
        free(faces[face]);
        faces[face] = NULL;
    }
    ID3D11DeviceContext_GenerateMips(ctx->ctx, out_temp->srv);
    if (FAILED(d3d11_device_status_after_void_command(ctx, "Upload/generate temporary cubemap"))) {
        out_temp->temporary = 1;
        d3d11_release_temp_srv(out_temp);
        return NULL;
    }
    face_bytes = (uint64_t)(uint32_t)face_size * (uint64_t)(uint32_t)face_size * 4u;
    d3d11_record_texture_upload_bytes(ctx, face_bytes * 6u);
    out_temp->temporary = 1;
    return out_temp->srv;
}

/// @brief Bytes still to upload for a cached texture entry (native or RGBA streaming path).
/// @param[in] entry Texture cache entry to inspect.
/// @return Pending byte estimate, or zero when no upload is active.
static uint64_t d3d11_texture_pending_bytes(const d3d_tex_cache_entry_t *entry) {
    if (!entry)
        return 0;
    return vgfx3d_d3d11_cached_pending_texture_bytes(entry->texture_asset != NULL,
                                                     entry->pending_native_bytes,
                                                     entry->width,
                                                     entry->height,
                                                     entry->upload_next_row,
                                                     entry->upload_in_progress);
}

/// @brief Refresh native-upload telemetry while the caller still owns the source asset.
/// @param[in,out] entry In-progress native cache entry whose estimate is refreshed.
/// @param[in] asset Borrowed source asset matching the entry.
static void d3d11_refresh_pending_native_bytes(d3d_tex_cache_entry_t *entry, void *asset) {
    if (!entry)
        return;
    entry->pending_native_bytes = 0;
    if (!asset || entry->texture_asset != asset || !entry->upload_in_progress)
        return;
    entry->pending_native_bytes =
        vgfx3d_textureasset_pending_native_snapshot_bytes(asset,
                                                          entry->native_mip_start,
                                                          entry->native_mip_count,
                                                          entry->native_next_mip,
                                                          entry->native_next_block_row,
                                                          1);
}

/// @brief Measure and validate one cubemap's complete prefiltered IBL payload without decoding it.
/// @param[in] cubemap Cubemap containing the optional prefiltered IBL chain.
/// @param[in] face_size Destination cubemap base extent.
/// @param[out] out_level_base Receives the first destination mip replaced by IBL.
/// @param[out] out_bytes Receives total decoded RGBA upload bytes.
/// @return One when the entire IBL layout and payload extents are valid.
static int d3d11_measure_ibl_upload(const rt_cubemap3d *cubemap,
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
        !vgfx3d_d3d11_validate_ibl_layout(face_size,
                                          cubemap->ibl_base_size,
                                          cubemap->ibl_mip_count,
                                          RT_CUBEMAP3D_IBL_MAX_MIPS,
                                          &level_base))
        return 0;

    for (int32_t mip = 0; mip < cubemap->ibl_mip_count; mip++) {
        int32_t expected_extent;
        uint64_t expected_face_bytes;

        if (!vgfx3d_d3d11_expected_square_mip_extent(face_size, level_base + mip, &expected_extent))
            return 0;
        expected_face_bytes =
            (uint64_t)(uint32_t)expected_extent * (uint64_t)(uint32_t)expected_extent * 4u;
        for (int32_t face = 0; face < 6; face++) {
            int32_t width;
            int32_t height;
            uint64_t bytes = 0;
            if (!vgfx3d_get_pixels_extent(cubemap->ibl_mips[mip][face], &width, &height) ||
                width != expected_extent || height != expected_extent ||
                !vgfx3d_estimate_pixels_rgba_upload_bytes(cubemap->ibl_mips[mip][face], &bytes) ||
                bytes != expected_face_bytes)
                return 0;
            total = vgfx3d_d3d11_saturating_add_u64(total, bytes);
            if (total == UINT64_MAX)
                return 0;
        }
    }
    *out_level_base = level_base;
    *out_bytes = total;
    return 1;
}

/// @brief Cache pending IBL telemetry while the owning cubemap is known to be alive.
/// @param[in,out] entry Cubemap cache entry receiving the pending identity and byte count.
/// @param[in] cubemap Borrowed runtime cubemap matching the entry.
static void d3d11_refresh_pending_ibl_metadata(d3d_cubemap_cache_entry_t *entry,
                                               const rt_cubemap3d *cubemap) {
    int32_t level_base;
    uint64_t bytes;

    if (!entry)
        return;
    entry->pending_ibl_identity = 0;
    entry->pending_ibl_bytes = 0;
    if (!cubemap || entry->applied_ibl_identity == cubemap->ibl_identity ||
        !d3d11_measure_ibl_upload(cubemap, entry->face_size, &level_base, &bytes))
        return;
    (void)level_base;
    entry->pending_ibl_identity = cubemap->ibl_identity;
    entry->pending_ibl_bytes = bytes;
}

/// @brief Bytes still to upload for a cached cubemap entry across base faces and IBL mips.
/// @param[in] entry Cubemap cache entry to inspect.
/// @return Saturating total of base-face and pending IBL upload bytes.
static uint64_t d3d11_cubemap_pending_bytes(const d3d_cubemap_cache_entry_t *entry) {
    uint64_t total;

    if (!entry)
        return 0;
    total = vgfx3d_pending_cubemap_rgba_upload_bytes(
        entry->face_size, entry->upload_face, entry->upload_next_row, entry->upload_in_progress);
    return vgfx3d_d3d11_saturating_add_u64(total, entry->pending_ibl_bytes);
}

/// @brief Total bytes still pending across all in-progress texture/cubemap uploads (saturating).
/// @param[in] ctx_ptr Opaque pointer to the D3D11 context.
/// @return Saturating sum of pending bytes across both caches.
static uint64_t d3d11_get_texture_upload_pending_bytes(void *ctx_ptr) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    uint64_t total = 0;

    if (!ctx)
        return 0;
    for (int32_t i = 0; i < ctx->tex_cache_count; i++) {
        uint64_t bytes = d3d11_texture_pending_bytes(&ctx->tex_cache[i]);
        total = vgfx3d_d3d11_saturating_add_u64(total, bytes);
        if (total == UINT64_MAX)
            return UINT64_MAX;
    }
    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        uint64_t bytes = d3d11_cubemap_pending_bytes(&ctx->cubemap_cache[i]);
        total = vgfx3d_d3d11_saturating_add_u64(total, bytes);
        if (total == UINT64_MAX)
            return UINT64_MAX;
    }
    return total;
}

/// @brief Set the per-frame byte budget that paces streaming texture uploads on this context.
/// @param[in,out] ctx_ptr Opaque pointer to the D3D11 context.
/// @param[in] bytes New per-frame upload budget.
static void d3d11_set_texture_upload_budget(void *ctx_ptr, uint64_t bytes) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    if (ctx)
        ctx->texture_upload_budget_bytes = bytes;
}

/// @brief Whether the device can use @p format as a sampleable 2D texture (CheckFormatSupport
/// query).
/// @param[in] ctx Context containing the D3D11 device.
/// @param[in] format DXGI format to query.
/// @return One when both Texture2D and shader-sampling support are advertised.
static int d3d11_format_supports_texture_sampling(d3d11_context_t *ctx, DXGI_FORMAT format) {
    UINT support = 0;
    if (!ctx || !ctx->device)
        return 0;
    if (FAILED(ID3D11Device_CheckFormatSupport(ctx->device, format, &support)))
        return 0;
    return ((support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0 &&
            (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0);
}

/// @brief Report which native BC compressed texture formats this device can upload on D3D11,
/// plus sampler anisotropy support.
/// @param[in] ctx_ptr Opaque pointer to the D3D11 context.
/// @return Bitmask of supported native texture and sampler capabilities.
static int64_t d3d11_get_native_texture_caps(void *ctx_ptr) {
    d3d11_context_t *ctx = (d3d11_context_t *)ctx_ptr;
    int64_t caps = 0;
    if (ctx && ctx->device)
        caps |= RT_CANVAS3D_BACKEND_CAP_ANISOTROPY;
    if (d3d11_format_supports_texture_sampling(ctx, DXGI_FORMAT_BC7_UNORM))
        caps |= RT_CANVAS3D_BACKEND_CAP_BC7;
    if (d3d11_format_supports_texture_sampling(ctx, DXGI_FORMAT_BC1_UNORM))
        caps |= RT_CANVAS3D_BACKEND_CAP_BC1;
    if (d3d11_format_supports_texture_sampling(ctx, DXGI_FORMAT_BC3_UNORM))
        caps |= RT_CANVAS3D_BACKEND_CAP_BC3;
    if (d3d11_format_supports_texture_sampling(ctx, DXGI_FORMAT_BC4_UNORM))
        caps |= RT_CANVAS3D_BACKEND_CAP_BC4;
    if (d3d11_format_supports_texture_sampling(ctx, DXGI_FORMAT_BC5_UNORM))
        caps |= RT_CANVAS3D_BACKEND_CAP_BC5;
    return caps;
}

/// @brief Map a native texture format id to its DXGI format (UNKNOWN when unsupported).
/// @param[in] format_id Runtime native compressed-format identifier.
/// @return Corresponding DXGI BC format, or `DXGI_FORMAT_UNKNOWN`.
static DXGI_FORMAT d3d11_native_texture_format(int32_t format_id) {
    if (format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7)
        return DXGI_FORMAT_BC7_UNORM;
    if (format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1)
        return DXGI_FORMAT_BC1_UNORM;
    if (format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC3)
        return DXGI_FORMAT_BC3_UNORM;
    if (format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC4)
        return DXGI_FORMAT_BC4_UNORM;
    if (format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC5)
        return DXGI_FORMAT_BC5_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}

/// @brief Bytes per block-row of a native compressed mip (block columns × block byte size).
/// @details D3D11 texture updates need the source row pitch; this computes it overflow-safely.
/// @param[in] mip Native mip descriptor.
/// @return Required block-row byte count, or zero for invalid input or overflow.
static uint64_t d3d11_native_texture_row_bytes(const vgfx3d_native_texture_mip_t *mip) {
    return vgfx3d_d3d11_native_mip_row_bytes(mip);
}

/// @brief Validate a draw-time native compressed mip snapshot before creating D3D resources.
/// @param[in] asset Runtime TextureAsset3D providing the snapshot.
/// @param[in] mip_start First resident mip requested.
/// @param[in] mip_count Number of resident mips requested.
/// @param[out] out_first_mip Optional receiver for the validated first descriptor.
/// @return One when every snapshot mip forms a valid supported chain.
static int d3d11_validate_native_texture_snapshot(void *asset,
                                                  int64_t mip_start,
                                                  int64_t mip_count,
                                                  vgfx3d_native_texture_mip_t *out_first_mip) {
    vgfx3d_native_texture_mip_t first_mip;
    vgfx3d_native_texture_mip_t previous_mip;

    if (out_first_mip)
        memset(out_first_mip, 0, sizeof(*out_first_mip));
    if (!asset || mip_start < 0 || mip_count <= 0)
        return 0;
    if (!vgfx3d_textureasset_get_native_snapshot_mip(asset, mip_start, mip_count, 0, &first_mip))
        return 0;
    if (d3d11_native_texture_format(first_mip.format_id) == DXGI_FORMAT_UNKNOWN ||
        !vgfx3d_d3d11_is_valid_native_mip_count(first_mip.width, first_mip.height, mip_count) ||
        !vgfx3d_d3d11_validate_native_mip_desc(&first_mip,
                                               NULL,
                                               first_mip.format_id,
                                               first_mip.block_width,
                                               first_mip.block_height,
                                               first_mip.block_bytes) ||
        first_mip.bytes > UINT_MAX) {
        return 0;
    }

    previous_mip = first_mip;
    for (int64_t i = 1; i < mip_count; i++) {
        vgfx3d_native_texture_mip_t mip;
        if (!vgfx3d_textureasset_get_native_snapshot_mip(asset, mip_start, mip_count, i, &mip)) {
            return 0;
        }
        if (!vgfx3d_d3d11_validate_native_mip_desc(&mip,
                                                   &previous_mip,
                                                   first_mip.format_id,
                                                   first_mip.block_width,
                                                   first_mip.block_height,
                                                   first_mip.block_bytes) ||
            mip.bytes > UINT_MAX) {
            return 0;
        }
        previous_mip = mip;
    }
    if (out_first_mip)
        *out_first_mip = first_mip;
    return 1;
}

/// @brief Create an RGBA8 2D texture and its shader-resource view at the given size.
/// @param[in] ctx Context containing the D3D11 device.
/// @param[in] w Base texture width.
/// @param[in] h Base texture height.
/// @param[out] out_tex Receives the mipmapped texture.
/// @param[out] out_srv Receives its full-chain shader-resource view.
/// @return S_OK with @p out_tex / @p out_srv set, or a failure HRESULT.
static HRESULT d3d11_allocate_texture_srv(d3d11_context_t *ctx,
                                          int32_t w,
                                          int32_t h,
                                          ID3D11Texture2D **out_tex,
                                          ID3D11ShaderResourceView **out_srv) {
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    int32_t mip_count;
    HRESULT hr;

    if (out_tex)
        *out_tex = NULL;
    if (out_srv)
        *out_srv = NULL;
    if (!ctx || !ctx->device || !out_tex || !out_srv)
        return E_INVALIDARG;
    if (!vgfx3d_d3d11_is_valid_texture2d_extent(w, h) || w > (int32_t)(UINT_MAX / 4u)) {
        return E_INVALIDARG;
    }

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    mip_count = vgfx3d_d3d11_compute_mip_count(w, h);
    desc.MipLevels = (UINT)mip_count;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, NULL, out_tex);
    if (SUCCEEDED(hr) && *out_tex && d3d11_texture2d_desc_matches(*out_tex, &desc)) {
        memset(&srv_desc, 0, sizeof(srv_desc));
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = desc.MipLevels;
        hr = ID3D11Device_CreateShaderResourceView(
            ctx->device, (ID3D11Resource *)*out_tex, &srv_desc, out_srv);
        if (SUCCEEDED(hr) && !*out_srv)
            hr = E_POINTER;
        else if (SUCCEEDED(hr) &&
                 !d3d11_srv_matches_texture2d(*out_srv, *out_tex, srv_desc.Format, desc.MipLevels))
            hr = E_FAIL;
    } else if (SUCCEEDED(hr)) {
        hr = *out_tex ? E_FAIL : E_POINTER;
    }
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateTexture2D/ShaderResourceView(texture)", hr);
        SAFE_RELEASE(*out_srv);
        SAFE_RELEASE(*out_tex);
    }
    return hr;
}

/// @brief Create a native compressed BC-family 2D texture and its shader-resource view for a mip
/// chain.
/// @param[in] ctx Context containing the D3D11 device.
/// @param[in] first_mip Validated descriptor for the first resident mip.
/// @param[in] mip_count Number of compressed levels to allocate.
/// @param[out] out_tex Receives the native texture.
/// @param[out] out_srv Receives its shader-resource view.
/// @return S_OK with the texture/SRV set, or a failure HRESULT.
static HRESULT d3d11_allocate_native_texture_srv(d3d11_context_t *ctx,
                                                 const vgfx3d_native_texture_mip_t *first_mip,
                                                 int64_t mip_count,
                                                 ID3D11Texture2D **out_tex,
                                                 ID3D11ShaderResourceView **out_srv) {
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    DXGI_FORMAT format;
    HRESULT hr;

    if (out_tex)
        *out_tex = NULL;
    if (out_srv)
        *out_srv = NULL;
    if (!ctx || !ctx->device || !out_tex || !out_srv || !first_mip || mip_count <= 0 ||
        mip_count > UINT_MAX)
        return E_INVALIDARG;
    format = d3d11_native_texture_format(first_mip->format_id);
    if (format == DXGI_FORMAT_UNKNOWN || !d3d11_format_supports_texture_sampling(ctx, format) ||
        !vgfx3d_d3d11_is_valid_native_mip_count(first_mip->width, first_mip->height, mip_count) ||
        !vgfx3d_d3d11_validate_native_mip_desc(first_mip,
                                               NULL,
                                               first_mip->format_id,
                                               first_mip->block_width,
                                               first_mip->block_height,
                                               first_mip->block_bytes))
        return E_INVALIDARG;

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)first_mip->width;
    desc.Height = (UINT)first_mip->height;
    desc.MipLevels = (UINT)mip_count;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, NULL, out_tex);
    if (SUCCEEDED(hr) && *out_tex && d3d11_texture2d_desc_matches(*out_tex, &desc)) {
        memset(&srv_desc, 0, sizeof(srv_desc));
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = desc.MipLevels;
        hr = ID3D11Device_CreateShaderResourceView(
            ctx->device, (ID3D11Resource *)*out_tex, &srv_desc, out_srv);
        if (SUCCEEDED(hr) && !*out_srv)
            hr = E_POINTER;
        else if (SUCCEEDED(hr) &&
                 !d3d11_srv_matches_texture2d(*out_srv, *out_tex, srv_desc.Format, desc.MipLevels))
            hr = E_FAIL;
    } else if (SUCCEEDED(hr)) {
        hr = *out_tex ? E_FAIL : E_POINTER;
    }
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateTexture2D/ShaderResourceView(native texture)", hr);
        SAFE_RELEASE(*out_srv);
        SAFE_RELEASE(*out_tex);
    }
    return hr;
}

/// @brief Upload more of an in-progress native compressed texture, bounded by the upload budget.
/// @param[in,out] ctx Context supplying the device, budget, and upload counter.
/// @param[in,out] entry Cache entry containing the texture and mip/block-row cursor.
/// @param[in] asset Borrowed TextureAsset3D source matching the entry.
/// @return COMPLETE, PENDING, or FAILED for this continuation step.
static vgfx3d_d3d11_upload_result_t d3d11_continue_native_texture_upload(
    d3d11_context_t *ctx, d3d_tex_cache_entry_t *entry, void *asset) {
    if (!ctx || !ctx->ctx || !entry || !asset || entry->texture_asset != asset ||
        !entry->upload_in_progress || !entry->tex || !entry->srv)
        return VGFX3D_D3D11_UPLOAD_FAILED;
    if (entry->native_mip_count <= 0 || entry->native_next_mip < 0 ||
        entry->native_next_mip >= entry->native_mip_count || entry->native_next_block_row < 0)
        return VGFX3D_D3D11_UPLOAD_FAILED;
    while (entry->native_next_mip < entry->native_mip_count) {
        vgfx3d_native_texture_mip_t mip;
        uint64_t row_bytes;
        uint64_t block_rows;
        int32_t rows;
        uint64_t offset;
        uint64_t upload_bytes;
        uint64_t remaining_block_rows;
        uint64_t top_row;
        uint64_t bottom_row;
        D3D11_BOX box;
        vgfx3d_native_texture_mip_t previous_mip = {0};
        const vgfx3d_native_texture_mip_t *previous_mip_ptr = NULL;
        int32_t expected_block_width;
        int32_t expected_block_height;
        int32_t expected_block_bytes;

        if (ctx->texture_upload_budget_bytes != UINT64_MAX &&
            ctx->texture_upload_bytes >= ctx->texture_upload_budget_bytes)
            return VGFX3D_D3D11_UPLOAD_PENDING;
        if (!vgfx3d_textureasset_get_native_snapshot_mip(asset,
                                                         entry->native_mip_start,
                                                         entry->native_mip_count,
                                                         entry->native_next_mip,
                                                         &mip))
            return VGFX3D_D3D11_UPLOAD_FAILED;
        if (entry->native_next_mip > 0) {
            if (!vgfx3d_textureasset_get_native_snapshot_mip(asset,
                                                             entry->native_mip_start,
                                                             entry->native_mip_count,
                                                             entry->native_next_mip - 1,
                                                             &previous_mip))
                return VGFX3D_D3D11_UPLOAD_FAILED;
            previous_mip_ptr = &previous_mip;
        }
        expected_block_width = previous_mip_ptr ? previous_mip.block_width : mip.block_width;
        expected_block_height = previous_mip_ptr ? previous_mip.block_height : mip.block_height;
        expected_block_bytes = previous_mip_ptr ? previous_mip.block_bytes : mip.block_bytes;
        if (mip.format_id != entry->native_format ||
            d3d11_native_texture_format(mip.format_id) == DXGI_FORMAT_UNKNOWN ||
            !vgfx3d_d3d11_validate_native_mip_desc(&mip,
                                                   previous_mip_ptr,
                                                   entry->native_format,
                                                   expected_block_width,
                                                   expected_block_height,
                                                   expected_block_bytes) ||
            mip.bytes > UINT_MAX)
            return VGFX3D_D3D11_UPLOAD_FAILED;
        row_bytes = d3d11_native_texture_row_bytes(&mip);
        block_rows = vgfx3d_d3d11_native_mip_block_rows(&mip);
        if (row_bytes == 0 || row_bytes > UINT_MAX || block_rows == 0 || block_rows > UINT_MAX)
            return VGFX3D_D3D11_UPLOAD_FAILED;
        if (!vgfx3d_d3d11_native_upload_cursor_is_valid(entry->native_mip_count,
                                                        entry->native_next_mip,
                                                        block_rows,
                                                        entry->native_next_block_row))
            return VGFX3D_D3D11_UPLOAD_FAILED;
        rows = vgfx3d_upload_block_rows_for_budget(mip.width,
                                                   mip.height,
                                                   mip.block_width,
                                                   mip.block_height,
                                                   mip.block_bytes,
                                                   entry->native_next_block_row,
                                                   ctx->texture_upload_budget_bytes,
                                                   ctx->texture_upload_bytes);
        if (rows <= 0)
            return VGFX3D_D3D11_UPLOAD_PENDING;
        remaining_block_rows = block_rows - (uint64_t)(uint32_t)entry->native_next_block_row;
        if ((uint64_t)(uint32_t)rows > remaining_block_rows)
            return VGFX3D_D3D11_UPLOAD_FAILED;
        offset = (uint64_t)(uint32_t)entry->native_next_block_row * row_bytes;
        upload_bytes = (uint64_t)(uint32_t)rows * row_bytes;
        if (offset > mip.bytes || upload_bytes > mip.bytes - offset || upload_bytes > UINT_MAX)
            return VGFX3D_D3D11_UPLOAD_FAILED;
        top_row =
            (uint64_t)(uint32_t)entry->native_next_block_row * (uint64_t)(uint32_t)mip.block_height;
        bottom_row = top_row + (uint64_t)(uint32_t)rows * (uint64_t)(uint32_t)mip.block_height;
        if (top_row >= (uint64_t)(uint32_t)mip.height || top_row > UINT_MAX)
            return VGFX3D_D3D11_UPLOAD_FAILED;
        if (bottom_row > (uint64_t)(uint32_t)mip.height)
            bottom_row = (uint64_t)(uint32_t)mip.height;
        if (bottom_row > UINT_MAX)
            return VGFX3D_D3D11_UPLOAD_FAILED;
        memset(&box, 0, sizeof(box));
        box.left = 0;
        box.right = (UINT)mip.width;
        box.top = (UINT)top_row;
        box.bottom = (UINT)bottom_row;
        box.front = 0;
        box.back = 1;
        d3d11_unbind_draw_resources(ctx);
        ID3D11DeviceContext_UpdateSubresource(ctx->ctx,
                                              (ID3D11Resource *)entry->tex,
                                              (UINT)entry->native_next_mip,
                                              &box,
                                              (const uint8_t *)mip.data + offset,
                                              (UINT)row_bytes,
                                              (UINT)upload_bytes);
        if (FAILED(d3d11_device_status_after_void_command(
                ctx, "UpdateSubresource(native compressed texture)")))
            return VGFX3D_D3D11_UPLOAD_FAILED;
        d3d11_record_texture_upload_bytes(ctx, upload_bytes);
        entry->native_next_block_row += rows;
        if ((uint64_t)(uint32_t)entry->native_next_block_row >= block_rows) {
            entry->native_next_mip++;
            entry->native_next_block_row = 0;
        }
        d3d11_refresh_pending_native_bytes(entry, asset);
        entry->last_used_frame = ctx->frame_serial;
    }
    entry->generation = entry->pending_generation;
    entry->pending_generation = 0;
    entry->upload_in_progress = 0;
    entry->pending_native_bytes = 0;
    return VGFX3D_D3D11_UPLOAD_COMPLETE;
}

/// @brief Begin uploading a native compressed TextureAsset3D: create the texture and seed the
/// cursor.
/// @param[in,out] ctx Context that owns the cache entry and upload accounting.
/// @param[in,out] entry Cache slot to initialize.
/// @param[in] asset Borrowed TextureAsset3D source.
/// @param[in] cache_key Native snapshot generation key to publish on completion.
/// @param[in] mip_start First resident source mip.
/// @param[in] mip_count Number of resident source mips.
/// @return One when allocation succeeds and upload is complete or pending; otherwise zero.
static int d3d11_start_native_texture_upload(d3d11_context_t *ctx,
                                             d3d_tex_cache_entry_t *entry,
                                             void *asset,
                                             uint64_t cache_key,
                                             int64_t mip_start,
                                             int64_t mip_count) {
    ID3D11Texture2D *new_tex = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;
    vgfx3d_native_texture_mip_t first_mip;
    HRESULT hr;

    if (!ctx || !entry || !asset ||
        !vgfx3d_textureasset_native_supported(asset, d3d11_get_native_texture_caps(ctx)) ||
        !d3d11_validate_native_texture_snapshot(asset, mip_start, mip_count, &first_mip))
        return 0;
    hr = d3d11_allocate_native_texture_srv(ctx, &first_mip, mip_count, &new_tex, &new_srv);
    if (FAILED(hr) || !new_tex || !new_srv) {
        SAFE_RELEASE(new_srv);
        SAFE_RELEASE(new_tex);
        return 0;
    }
    d3d11_unbind_draw_resources(ctx);
    d3d11_release_texture_cache_entry(entry);
    entry->tex = new_tex;
    entry->srv = new_srv;

    entry->pixels_ptr = NULL;
    entry->texture_asset = asset;
    entry->pending_generation = cache_key;
    entry->generation = 0;
    entry->width = first_mip.width;
    entry->height = first_mip.height;
    entry->upload_next_row = 0;
    entry->native_format = first_mip.format_id;
    entry->native_next_block_row = 0;
    entry->native_next_mip = 0;
    entry->native_mip_start = mip_start;
    entry->native_mip_count = mip_count;
    entry->upload_in_progress = 1;
    d3d11_refresh_pending_native_bytes(entry, asset);
    entry->last_used_frame = ctx->frame_serial;
    vgfx3d_d3d11_upload_result_t result = d3d11_continue_native_texture_upload(ctx, entry, asset);
    if (result == VGFX3D_D3D11_UPLOAD_FAILED) {
        d3d11_unbind_draw_resources(ctx);
        d3d11_mark_texture_upload_failed(
            entry, NULL, asset, cache_key, mip_start, mip_count, ctx->frame_serial);
        return 0;
    }
    if (ctx->texture_upload_budget_bytes == UINT64_MAX && entry->upload_in_progress) {
        d3d11_unbind_draw_resources(ctx);
        d3d11_release_texture_cache_entry(entry);
        return 0;
    }
    return 1;
}

/// @brief Upload more rows of an in-progress RGBA texture, bounded by the upload budget.
/// @param[in,out] ctx Context supplying the device, budget, and upload counter.
/// @param[in,out] entry Cache entry containing the texture and next-row cursor.
/// @param[in] pixels Borrowed Pixels source matching the entry.
/// @return COMPLETE, PENDING, or FAILED for this continuation step.
static vgfx3d_d3d11_upload_result_t d3d11_continue_texture_upload(d3d11_context_t *ctx,
                                                                  d3d_tex_cache_entry_t *entry,
                                                                  const void *pixels) {
    int32_t rows;
    int32_t slice_w = 0;
    int32_t slice_rows = 0;
    uint8_t *rgba = NULL;
    D3D11_BOX box;
    uint64_t bytes;
    uint32_t row_pitch;
    int32_t source_width;
    int32_t source_height;

    if (!ctx || !ctx->ctx || !entry || !pixels || !entry->upload_in_progress || !entry->tex ||
        !entry->srv)
        return VGFX3D_D3D11_UPLOAD_FAILED;
    if (!vgfx3d_d3d11_is_valid_texture2d_extent(entry->width, entry->height))
        return VGFX3D_D3D11_UPLOAD_FAILED;
    if (!vgfx3d_get_pixels_extent(pixels, &source_width, &source_height) ||
        source_width != entry->width || source_height != entry->height)
        return VGFX3D_D3D11_UPLOAD_FAILED;
    if (!vgfx3d_d3d11_row_upload_cursor_is_valid(entry->height, entry->upload_next_row))
        return VGFX3D_D3D11_UPLOAD_FAILED;
    rows = vgfx3d_upload_rows_for_budget(entry->width,
                                         entry->height,
                                         entry->upload_next_row,
                                         ctx->texture_upload_budget_bytes,
                                         ctx->texture_upload_bytes);
    if (rows <= 0)
        return VGFX3D_D3D11_UPLOAD_PENDING;
    if (vgfx3d_unpack_pixels_rgba_rows(
            pixels, entry->upload_next_row, rows, 0, &slice_w, &slice_rows, &rgba) != 0 ||
        !rgba) {
        free(rgba);
        return VGFX3D_D3D11_UPLOAD_FAILED;
    }
    if (slice_w != entry->width || slice_rows <= 0 ||
        !vgfx3d_d3d11_validate_row_span(entry->height, entry->upload_next_row, slice_rows)) {
        free(rgba);
        return VGFX3D_D3D11_UPLOAD_FAILED;
    }

    memset(&box, 0, sizeof(box));
    box.left = 0;
    box.right = (UINT)slice_w;
    box.top = (UINT)entry->upload_next_row;
    box.bottom = (UINT)(entry->upload_next_row + slice_rows);
    box.front = 0;
    box.back = 1;
    d3d11_unbind_draw_resources(ctx);
    if (!vgfx3d_d3d11_compute_rgba8_upload_pitch(slice_w, &row_pitch)) {
        free(rgba);
        return VGFX3D_D3D11_UPLOAD_FAILED;
    }
    ID3D11DeviceContext_UpdateSubresource(
        ctx->ctx, (ID3D11Resource *)entry->tex, 0, &box, rgba, (UINT)row_pitch, 0);
    if (FAILED(
            d3d11_device_status_after_void_command(ctx, "UpdateSubresource(streamed texture)"))) {
        free(rgba);
        return VGFX3D_D3D11_UPLOAD_FAILED;
    }
    bytes = (uint64_t)(uint32_t)slice_w * (uint64_t)(uint32_t)slice_rows * 4u;
    d3d11_record_texture_upload_bytes(ctx, bytes);
    free(rgba);

    entry->upload_next_row += slice_rows;
    entry->last_used_frame = ctx->frame_serial;
    if (entry->upload_next_row >= entry->height) {
        ID3D11DeviceContext_GenerateMips(ctx->ctx, entry->srv);
        if (FAILED(d3d11_device_status_after_void_command(ctx, "GenerateMips(streamed texture)")))
            return VGFX3D_D3D11_UPLOAD_FAILED;
        entry->generation = entry->pending_generation;
        entry->pending_generation = 0;
        entry->upload_in_progress = 0;
        return VGFX3D_D3D11_UPLOAD_COMPLETE;
    }
    return VGFX3D_D3D11_UPLOAD_PENDING;
}

/// @brief Begin uploading an RGBA Pixels texture: allocate the texture/SRV and seed the row cursor.
/// @param[in,out] ctx Context that owns the cache entry and upload accounting.
/// @param[in,out] entry Cache slot to initialize.
/// @param[in] pixels Borrowed Pixels source.
/// @param[in] cache_key Pixels generation key to publish on completion.
/// @return One when allocation succeeds and upload is complete or pending; otherwise zero.
static int d3d11_start_texture_upload(d3d11_context_t *ctx,
                                      d3d_tex_cache_entry_t *entry,
                                      const void *pixels,
                                      uint64_t cache_key) {
    ID3D11Texture2D *new_tex = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;
    int32_t w = 0;
    int32_t h = 0;
    HRESULT hr;

    if (!ctx || !entry || !pixels || !vgfx3d_get_pixels_extent(pixels, &w, &h))
        return 0;
    hr = d3d11_allocate_texture_srv(ctx, w, h, &new_tex, &new_srv);
    if (FAILED(hr) || !new_tex || !new_srv) {
        SAFE_RELEASE(new_srv);
        SAFE_RELEASE(new_tex);
        return 0;
    }
    d3d11_unbind_draw_resources(ctx);
    d3d11_release_texture_cache_entry(entry);
    entry->tex = new_tex;
    entry->srv = new_srv;

    entry->pixels_ptr = pixels;
    entry->texture_asset = NULL;
    entry->pending_generation = cache_key;
    entry->generation = 0;
    entry->width = w;
    entry->height = h;
    entry->upload_next_row = 0;
    entry->native_format = RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE;
    entry->native_next_block_row = 0;
    entry->native_next_mip = 0;
    entry->native_mip_count = 0;
    entry->upload_in_progress = 1;
    entry->last_used_frame = ctx->frame_serial;
    vgfx3d_d3d11_upload_result_t result = d3d11_continue_texture_upload(ctx, entry, pixels);
    if (result == VGFX3D_D3D11_UPLOAD_FAILED) {
        d3d11_unbind_draw_resources(ctx);
        d3d11_mark_texture_upload_failed(entry, pixels, NULL, cache_key, 0, 0, ctx->frame_serial);
        return 0;
    }
    if (ctx->texture_upload_budget_bytes == UINT64_MAX && entry->upload_in_progress) {
        d3d11_unbind_draw_resources(ctx);
        d3d11_release_texture_cache_entry(entry);
        return 0;
    }
    return 1;
}

/// @brief Cache lookup for textures, with auto-create and budgeted row uploads.
///
/// Three-way decision:
///   1. Hit (pixels_ptr + cache-key match) → return cached SRV.
///   2. Pixels match but stale generation → restart upload in that slot.
///   3. Miss → create new, append to cache, and return only when upload completes.
/// @param[in,out] ctx Context containing the texture cache and upload budget.
/// @param[in] index Known cache slot associated with @p pixels.
/// @param[in] pixels Borrowed Pixels source identity.
/// @param[in] cache_key Current Pixels generation key.
/// @return Resident SRV, white fallback while upload is pending, or `NULL` on failure.
static ID3D11ShaderResourceView *d3d11_resolve_pixels_cache_entry(d3d11_context_t *ctx,
                                                                  int32_t index,
                                                                  const void *pixels,
                                                                  uint64_t cache_key) {
    d3d_tex_cache_entry_t *entry;
    if (!ctx || index < 0 || index >= ctx->tex_cache_count)
        return NULL;
    entry = &ctx->tex_cache[index];
    if (entry->pixels_ptr != pixels)
        return NULL;
    d3d11_texture_cache_remember(ctx, pixels, index);
    if (entry->failed_generation == cache_key)
        return NULL;
    if (entry->generation == cache_key && entry->tex && entry->srv) {
        entry->last_used_frame = ctx->frame_serial;
        return entry->srv;
    }
    if (entry->pending_generation == cache_key && entry->upload_in_progress) {
        vgfx3d_d3d11_upload_result_t result;
        entry->last_used_frame = ctx->frame_serial;
        result = d3d11_continue_texture_upload(ctx, entry, pixels);
        if (result == VGFX3D_D3D11_UPLOAD_COMPLETE)
            return entry->srv;
        if (result == VGFX3D_D3D11_UPLOAD_FAILED) {
            d3d11_mark_texture_upload_failed(
                entry, pixels, NULL, cache_key, 0, 0, ctx->frame_serial);
            return NULL;
        }
        return d3d11_fallback_white_srv(ctx);
    }
    if (!d3d11_start_texture_upload(ctx, entry, pixels, cache_key))
        return NULL;
    d3d11_texture_cache_remember(ctx, pixels, index);
    return entry->upload_in_progress ? d3d11_fallback_white_srv(ctx) : entry->srv;
}

/// @brief Resolve an RGBA Pixels source through the persistent texture cache.
/// @details Uses the direct-mapped hint before a linear lookup, reuses empty slots,
///   grows the cache when possible, and falls back to a one-draw resource only when
///   the upload fits the current budget.
/// @param[in,out] ctx Context containing the cache, device, and upload budget.
/// @param[in] pixels Borrowed runtime Pixels source.
/// @param[out] out_temp Optional receiver for a temporary resource when caching is unavailable.
/// @return Resident or white fallback SRV, temporary SRV, or `NULL` on failure.
static ID3D11ShaderResourceView *d3d11_get_or_create_srv(d3d11_context_t *ctx,
                                                         const void *pixels,
                                                         d3d_temp_srv_t *out_temp) {
    uint64_t cache_key;
    int32_t hinted;

    if (out_temp)
        memset(out_temp, 0, sizeof(*out_temp));
    if (!ctx || !pixels)
        return NULL;
    cache_key = vgfx3d_get_pixels_cache_key(pixels);

    hinted = d3d11_texture_cache_hint_pixels(ctx, pixels);
    if (hinted >= 0)
        return d3d11_resolve_pixels_cache_entry(ctx, hinted, pixels, cache_key);
    for (int32_t i = 0; i < ctx->tex_cache_count; i++) {
        if (ctx->tex_cache[i].pixels_ptr == pixels)
            return d3d11_resolve_pixels_cache_entry(ctx, i, pixels, cache_key);
    }

    for (int32_t i = 0; i < ctx->tex_cache_count; i++) {
        if (!ctx->tex_cache[i].pixels_ptr && !ctx->tex_cache[i].texture_asset) {
            if (!d3d11_start_texture_upload(ctx, &ctx->tex_cache[i], pixels, cache_key))
                return NULL;
            d3d11_texture_cache_remember(ctx, pixels, i);
            return ctx->tex_cache[i].upload_in_progress ? d3d11_fallback_white_srv(ctx)
                                                        : ctx->tex_cache[i].srv;
        }
    }
    if (ctx->tex_cache_count < INT_MAX &&
        d3d11_ensure_tex_cache_capacity(ctx, ctx->tex_cache_count + 1)) {
        d3d_tex_cache_entry_t *entry = &ctx->tex_cache[ctx->tex_cache_count++];
        memset(entry, 0, sizeof(*entry));
        if (!d3d11_start_texture_upload(ctx, entry, pixels, cache_key)) {
            if (entry->failed_generation != cache_key)
                ctx->tex_cache_count--;
            else
                d3d11_texture_cache_remember(ctx, pixels, ctx->tex_cache_count - 1);
            return NULL;
        }
        d3d11_texture_cache_remember(ctx, pixels, ctx->tex_cache_count - 1);
        return entry->upload_in_progress ? d3d11_fallback_white_srv(ctx) : entry->srv;
    }
    {
        uint64_t upload_bytes = 0;
        if (!vgfx3d_estimate_pixels_rgba_upload_bytes(pixels, &upload_bytes) ||
            !vgfx3d_d3d11_upload_budget_allows(
                ctx->texture_upload_budget_bytes, ctx->texture_upload_bytes, upload_bytes))
            return d3d11_fallback_white_srv(ctx);
    }
    return d3d11_create_temp_texture_srv(ctx, pixels, out_temp);
}

/// @brief Resolve a native TextureAsset3D from a known cache slot.
/// @param[in,out] ctx Context containing the texture cache and upload budget.
/// @param[in] index Known cache slot associated with @p asset.
/// @param[in] asset Borrowed TextureAsset3D identity.
/// @param[in] cache_key Current native snapshot key.
/// @param[in] mip_start First resident source mip.
/// @param[in] mip_count Resident source mip count.
/// @return Resident SRV, white fallback while upload is pending, or `NULL` on failure.
static ID3D11ShaderResourceView *d3d11_resolve_native_cache_entry(d3d11_context_t *ctx,
                                                                  int32_t index,
                                                                  void *asset,
                                                                  uint64_t cache_key,
                                                                  int64_t mip_start,
                                                                  int64_t mip_count) {
    d3d_tex_cache_entry_t *entry;
    if (!ctx || index < 0 || index >= ctx->tex_cache_count)
        return NULL;
    entry = &ctx->tex_cache[index];
    if (entry->texture_asset != asset)
        return NULL;
    d3d11_texture_cache_remember(ctx, asset, index);
    if (entry->failed_generation == cache_key && entry->native_mip_start == mip_start &&
        entry->native_mip_count == mip_count)
        return NULL;
    if (entry->generation == cache_key && entry->native_mip_start == mip_start &&
        entry->native_mip_count == mip_count && entry->tex && entry->srv) {
        entry->last_used_frame = ctx->frame_serial;
        return entry->srv;
    }
    if (entry->pending_generation == cache_key && entry->native_mip_start == mip_start &&
        entry->native_mip_count == mip_count && entry->upload_in_progress) {
        vgfx3d_d3d11_upload_result_t result;
        entry->last_used_frame = ctx->frame_serial;
        result = d3d11_continue_native_texture_upload(ctx, entry, asset);
        if (result == VGFX3D_D3D11_UPLOAD_COMPLETE)
            return entry->srv;
        if (result == VGFX3D_D3D11_UPLOAD_FAILED) {
            d3d11_mark_texture_upload_failed(
                entry, NULL, asset, cache_key, mip_start, mip_count, ctx->frame_serial);
            return NULL;
        }
        return d3d11_fallback_white_srv(ctx);
    }
    if (!d3d11_start_native_texture_upload(ctx, entry, asset, cache_key, mip_start, mip_count))
        return NULL;
    d3d11_texture_cache_remember(ctx, asset, index);
    return entry->upload_in_progress ? d3d11_fallback_white_srv(ctx) : entry->srv;
}

/// @brief Get the SRV for a native TextureAsset3D, creating/streaming it on a cache miss.
/// @details Keyed by the asset's native cache key so residency changes invalidate the entry.
/// @param[in,out] ctx Context containing the texture cache and upload budget.
/// @param[in] asset Borrowed TextureAsset3D source.
/// @param[in] cache_key Native snapshot key, or zero to query the asset.
/// @param[in] mip_start First resident mip, or negative to query the asset.
/// @param[in] mip_count Resident mip count, or nonpositive to query the asset.
/// @return The shader-resource view to bind, or NULL if it cannot be created.
static ID3D11ShaderResourceView *d3d11_get_or_create_native_srv(
    d3d11_context_t *ctx, void *asset, uint64_t cache_key, int64_t mip_start, int64_t mip_count) {
    int32_t hinted;
    if (!ctx || !asset ||
        !vgfx3d_textureasset_native_supported(asset, d3d11_get_native_texture_caps(ctx)))
        return NULL;
    if (cache_key == 0)
        cache_key = rt_textureasset3d_get_native_cache_key(asset);
    if (mip_start < 0)
        mip_start = rt_textureasset3d_get_resident_mip_start(asset);
    if (mip_count <= 0)
        mip_count = rt_textureasset3d_get_resident_mip_count(asset);
    if (cache_key == 0)
        return NULL;

    hinted = d3d11_texture_cache_hint_asset(ctx, asset);
    if (hinted >= 0)
        return d3d11_resolve_native_cache_entry(
            ctx, hinted, asset, cache_key, mip_start, mip_count);
    for (int32_t i = 0; i < ctx->tex_cache_count; i++) {
        if (ctx->tex_cache[i].texture_asset == asset)
            return d3d11_resolve_native_cache_entry(ctx, i, asset, cache_key, mip_start, mip_count);
    }

    for (int32_t i = 0; i < ctx->tex_cache_count; i++) {
        if (!ctx->tex_cache[i].pixels_ptr && !ctx->tex_cache[i].texture_asset) {
            if (!d3d11_start_native_texture_upload(
                    ctx, &ctx->tex_cache[i], asset, cache_key, mip_start, mip_count))
                return NULL;
            d3d11_texture_cache_remember(ctx, asset, i);
            return ctx->tex_cache[i].upload_in_progress ? d3d11_fallback_white_srv(ctx)
                                                        : ctx->tex_cache[i].srv;
        }
    }
    if (ctx->tex_cache_count < INT_MAX &&
        d3d11_ensure_tex_cache_capacity(ctx, ctx->tex_cache_count + 1)) {
        d3d_tex_cache_entry_t *entry = &ctx->tex_cache[ctx->tex_cache_count++];
        memset(entry, 0, sizeof(*entry));
        if (!d3d11_start_native_texture_upload(
                ctx, entry, asset, cache_key, mip_start, mip_count)) {
            if (entry->failed_generation != cache_key)
                ctx->tex_cache_count--;
            else
                d3d11_texture_cache_remember(ctx, asset, ctx->tex_cache_count - 1);
            return NULL;
        }
        d3d11_texture_cache_remember(ctx, asset, ctx->tex_cache_count - 1);
        return entry->upload_in_progress ? d3d11_fallback_white_srv(ctx) : entry->srv;
    }
    return NULL;
}

/// @brief Resolve a material's texture to an SRV, preferring native blocks then RGBA Pixels.
/// @param[in,out] ctx Context containing texture caches and device capabilities.
/// @param[in] asset Optional native TextureAsset3D source.
/// @param[in] pixels Optional decoded RGBA Pixels fallback.
/// @param[in] asset_cache_key Native snapshot key.
/// @param[in] mip_start First native resident mip.
/// @param[in] mip_count Native resident mip count.
/// @param[out] out_temp Optional temporary RGBA resource holder.
/// @return The shader-resource view to bind, or NULL if neither source is uploadable.
static ID3D11ShaderResourceView *d3d11_get_or_create_material_srv(d3d11_context_t *ctx,
                                                                  void *asset,
                                                                  const void *pixels,
                                                                  uint64_t asset_cache_key,
                                                                  int64_t mip_start,
                                                                  int64_t mip_count,
                                                                  d3d_temp_srv_t *out_temp) {
    if (asset && vgfx3d_textureasset_native_supported(asset, d3d11_get_native_texture_caps(ctx))) {
        ID3D11ShaderResourceView *native_srv =
            d3d11_get_or_create_native_srv(ctx, asset, asset_cache_key, mip_start, mip_count);
        if (native_srv)
            return native_srv;
        /* NULL means validation/allocation failed. Budget-paused native uploads return the
         * backend white fallback, so an RGBA fallback is only used for invalid native payloads. */
    }
    return d3d11_get_or_create_srv(ctx, pixels, out_temp);
}

/// @brief Create a cubemap texture (6 faces) and its cube shader-resource view at the given size.
/// @param[in] ctx Context containing the D3D11 device.
/// @param[in] face_size Square base face extent.
/// @param[out] out_tex Receives the six-slice mipmapped texture.
/// @param[out] out_srv Receives its TextureCube shader-resource view.
/// @return S_OK with the texture/SRV set, or a failure HRESULT.
static HRESULT d3d11_allocate_cubemap_srv(d3d11_context_t *ctx,
                                          int32_t face_size,
                                          ID3D11Texture2D **out_tex,
                                          ID3D11ShaderResourceView **out_srv) {
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    int32_t mip_count;
    HRESULT hr;

    if (out_tex)
        *out_tex = NULL;
    if (out_srv)
        *out_srv = NULL;
    if (!ctx || !ctx->device || !out_tex || !out_srv)
        return E_INVALIDARG;
    if (!vgfx3d_d3d11_is_valid_cubemap_extent(face_size) || face_size > (int32_t)(UINT_MAX / 4u))
        return E_INVALIDARG;

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)face_size;
    desc.Height = (UINT)face_size;
    mip_count = vgfx3d_d3d11_compute_mip_count(face_size, face_size);
    desc.MipLevels = (UINT)mip_count;
    desc.ArraySize = 6;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;

    hr = ID3D11Device_CreateTexture2D(ctx->device, &desc, NULL, out_tex);
    if (SUCCEEDED(hr) && *out_tex && d3d11_texture2d_desc_matches(*out_tex, &desc)) {
        memset(&srv_desc, 0, sizeof(srv_desc));
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srv_desc.TextureCube.MipLevels = desc.MipLevels;
        hr = ID3D11Device_CreateShaderResourceView(
            ctx->device, (ID3D11Resource *)*out_tex, &srv_desc, out_srv);
        if (SUCCEEDED(hr) && !*out_srv)
            hr = E_POINTER;
        else if (SUCCEEDED(hr) && !d3d11_srv_matches_texturecube(
                                      *out_srv, *out_tex, srv_desc.Format, desc.MipLevels))
            hr = E_FAIL;
    } else if (SUCCEEDED(hr)) {
        hr = *out_tex ? E_FAIL : E_POINTER;
    }
    if (FAILED(hr)) {
        d3d11_log_hresult("CreateTexture2D/ShaderResourceView(cubemap)", hr);
        SAFE_RELEASE(*out_srv);
        SAFE_RELEASE(*out_tex);
    }
    return hr;
}

/// @brief Overlay the cubemap's GGX-prefiltered IBL chain onto the texture's tail mips.
/// @details Mirrors the OpenGL/Metal backends: box mips above level_base keep the
///   skybox sharp; levels level_base.. are replaced with the prefiltered chain so
///   roughness-mapped environment lookups integrate correctly. Idempotent per
///   ibl_identity; re-applied after base-face re-uploads.
/// @param[in,out] ctx Context supplying the device, budget, and upload accounting.
/// @param[in,out] entry Resident cubemap entry whose tail mips and IBL metadata are updated.
/// @param[in] cubemap Borrowed runtime cubemap containing the IBL payload.
/// @return One when the requested IBL identity is already applied or uploads successfully.
static int d3d11_apply_ibl_mips(d3d11_context_t *ctx,
                                d3d_cubemap_cache_entry_t *entry,
                                const rt_cubemap3d *cubemap) {
    uint8_t *rgba[RT_CUBEMAP3D_IBL_MAX_MIPS][6] = {{NULL}};
    uint32_t row_pitch[RT_CUBEMAP3D_IBL_MAX_MIPS][6] = {{0}};
    uint64_t upload_bytes = 0;
    int32_t level_base;
    int32_t total_mips;
    int ok = 0;

    if (!ctx || !ctx->ctx || !entry || !entry->tex || entry->upload_in_progress || !cubemap ||
        !cubemap->ibl_ready || cubemap->ibl_identity == 0)
        return 0;
    if (entry->failed_ibl_identity == cubemap->ibl_identity)
        return 0;
    if (entry->applied_ibl_identity == cubemap->ibl_identity) {
        entry->pending_ibl_identity = 0;
        entry->pending_ibl_bytes = 0;
        return 1;
    }
    if (!d3d11_measure_ibl_upload(cubemap, entry->face_size, &level_base, &upload_bytes)) {
        entry->failed_ibl_identity = cubemap->ibl_identity;
        entry->pending_ibl_identity = 0;
        entry->pending_ibl_bytes = 0;
        return 0;
    }
    entry->pending_ibl_identity = cubemap->ibl_identity;
    entry->pending_ibl_bytes = upload_bytes;
    if (!vgfx3d_d3d11_upload_budget_allows(
            ctx->texture_upload_budget_bytes, ctx->texture_upload_bytes, upload_bytes) &&
        (ctx->texture_upload_budget_bytes == 0 || ctx->texture_upload_bytes != 0))
        return 0;
    total_mips = vgfx3d_d3d11_compute_mip_count(entry->face_size, entry->face_size);

    /* Decode and validate the entire payload before modifying any destination subresource. */
    for (int32_t m = 0; m < cubemap->ibl_mip_count; m++) {
        for (int f = 0; f < 6; f++) {
            int32_t w = 0;
            int32_t h = 0;
            if (vgfx3d_unpack_pixels_rgba(cubemap->ibl_mips[m][f], &w, &h, &rgba[m][f]) != 0 ||
                !rgba[m][f])
                goto cleanup;
            if (!vgfx3d_d3d11_validate_ibl_mip_extent(entry->face_size, level_base + m, w, h) ||
                !vgfx3d_d3d11_compute_rgba8_upload_pitch(w, &row_pitch[m][f]))
                goto cleanup;
        }
    }

    d3d11_unbind_draw_resources(ctx);
    for (int32_t m = 0; m < cubemap->ibl_mip_count; m++) {
        for (int f = 0; f < 6; f++) {
            ID3D11DeviceContext_UpdateSubresource(
                ctx->ctx,
                (ID3D11Resource *)entry->tex,
                D3D11CalcSubresource((UINT)(level_base + m), (UINT)f, (UINT)total_mips),
                NULL,
                rgba[m][f],
                (UINT)row_pitch[m][f],
                0);
        }
    }
    if (FAILED(d3d11_device_status_after_void_command(ctx, "UpdateSubresource(cubemap IBL)")))
        goto cleanup;
    d3d11_record_texture_upload_bytes(ctx, upload_bytes);
    entry->applied_ibl_identity = cubemap->ibl_identity;
    entry->failed_ibl_identity = 0;
    entry->pending_ibl_identity = 0;
    entry->pending_ibl_bytes = 0;
    ok = 1;

cleanup:
    for (int32_t m = 0; m < RT_CUBEMAP3D_IBL_MAX_MIPS; m++)
        for (int f = 0; f < 6; f++)
            free(rgba[m][f]);
    if (!ok) {
        entry->failed_ibl_identity = cubemap->ibl_identity;
        entry->pending_ibl_identity = 0;
        entry->pending_ibl_bytes = 0;
    }
    return ok;
}

/// @brief Upload more of an in-progress cubemap (face by face, row band by row band) within budget.
/// @param[in,out] ctx Context supplying the device, budget, and upload counter.
/// @param[in,out] entry Cache entry containing the cube texture and face/row cursor.
/// @param[in] cubemap Borrowed source matching the entry.
/// @return COMPLETE, PENDING, or FAILED for this continuation step.
static vgfx3d_d3d11_upload_result_t d3d11_continue_cubemap_upload(d3d11_context_t *ctx,
                                                                  d3d_cubemap_cache_entry_t *entry,
                                                                  const rt_cubemap3d *cubemap) {
    int32_t mip_count;

    if (!ctx || !ctx->ctx || !entry || !cubemap || !entry->upload_in_progress || !entry->tex ||
        !entry->srv || !vgfx3d_d3d11_is_valid_cubemap_extent(entry->face_size))
        return VGFX3D_D3D11_UPLOAD_FAILED;
    if (!vgfx3d_d3d11_cubemap_upload_cursor_is_valid(
            entry->face_size, entry->upload_face, entry->upload_next_row))
        return VGFX3D_D3D11_UPLOAD_FAILED;

    mip_count = vgfx3d_d3d11_compute_mip_count(entry->face_size, entry->face_size);
    while (entry->upload_face < 6) {
        int32_t rows = vgfx3d_upload_rows_for_budget(entry->face_size,
                                                     entry->face_size,
                                                     entry->upload_next_row,
                                                     ctx->texture_upload_budget_bytes,
                                                     ctx->texture_upload_bytes);
        int32_t slice_size = 0;
        int32_t slice_rows = 0;
        uint8_t *rgba = NULL;
        D3D11_BOX box;
        uint64_t bytes;
        uint32_t row_pitch;
        int32_t source_face_size;

        if (rows <= 0)
            return VGFX3D_D3D11_UPLOAD_PENDING;
        if (!vgfx3d_get_cubemap_face_size(cubemap, &source_face_size) ||
            source_face_size != entry->face_size)
            return VGFX3D_D3D11_UPLOAD_FAILED;
        if (vgfx3d_unpack_cubemap_rgba_rows(cubemap,
                                            entry->upload_face,
                                            entry->upload_next_row,
                                            rows,
                                            0,
                                            &slice_size,
                                            &slice_rows,
                                            &rgba) != 0 ||
            !rgba) {
            free(rgba);
            return VGFX3D_D3D11_UPLOAD_FAILED;
        }
        if (slice_size != entry->face_size || slice_rows <= 0 ||
            !vgfx3d_d3d11_validate_row_span(entry->face_size, entry->upload_next_row, slice_rows)) {
            free(rgba);
            return VGFX3D_D3D11_UPLOAD_FAILED;
        }

        memset(&box, 0, sizeof(box));
        box.left = 0;
        box.right = (UINT)slice_size;
        box.top = (UINT)entry->upload_next_row;
        box.bottom = (UINT)(entry->upload_next_row + slice_rows);
        box.front = 0;
        box.back = 1;
        d3d11_unbind_draw_resources(ctx);
        if (!vgfx3d_d3d11_compute_rgba8_upload_pitch(slice_size, &row_pitch)) {
            free(rgba);
            return VGFX3D_D3D11_UPLOAD_FAILED;
        }
        ID3D11DeviceContext_UpdateSubresource(
            ctx->ctx,
            (ID3D11Resource *)entry->tex,
            D3D11CalcSubresource(0, (UINT)entry->upload_face, (UINT)mip_count),
            &box,
            rgba,
            (UINT)row_pitch,
            0);
        if (FAILED(d3d11_device_status_after_void_command(ctx,
                                                          "UpdateSubresource(streamed cubemap)"))) {
            free(rgba);
            return VGFX3D_D3D11_UPLOAD_FAILED;
        }
        bytes = (uint64_t)(uint32_t)slice_size * (uint64_t)(uint32_t)slice_rows * 4u;
        d3d11_record_texture_upload_bytes(ctx, bytes);
        free(rgba);

        entry->upload_next_row += slice_rows;
        entry->last_used_frame = ctx->frame_serial;
        if (entry->upload_next_row < entry->face_size)
            return VGFX3D_D3D11_UPLOAD_PENDING;
        entry->upload_face++;
        entry->upload_next_row = 0;
    }

    ID3D11DeviceContext_GenerateMips(ctx->ctx, entry->srv);
    if (FAILED(d3d11_device_status_after_void_command(ctx, "GenerateMips(streamed cubemap)")))
        return VGFX3D_D3D11_UPLOAD_FAILED;
    entry->generation = entry->pending_generation;
    entry->pending_generation = 0;
    entry->upload_in_progress = 0;
    d3d11_apply_ibl_mips(ctx, entry, cubemap);
    return VGFX3D_D3D11_UPLOAD_COMPLETE;
}

/// @brief Begin uploading a cubemap: create the cube texture/SRV and seed the face/row cursor.
/// @param[in,out] ctx Context that owns the cache entry and upload accounting.
/// @param[in,out] entry Cubemap cache slot to initialize.
/// @param[in] cubemap Borrowed source cubemap.
/// @param[in] generation Source generation to publish on completion.
/// @return One when allocation succeeds and upload is complete or pending; otherwise zero.
static int d3d11_start_cubemap_upload(d3d11_context_t *ctx,
                                      d3d_cubemap_cache_entry_t *entry,
                                      const rt_cubemap3d *cubemap,
                                      uint64_t generation) {
    ID3D11Texture2D *new_tex = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;
    int32_t face_size = 0;
    HRESULT hr;

    if (!ctx || !entry || !cubemap || !vgfx3d_get_cubemap_face_size(cubemap, &face_size))
        return 0;
    hr = d3d11_allocate_cubemap_srv(ctx, face_size, &new_tex, &new_srv);
    if (FAILED(hr) || !new_tex || !new_srv) {
        SAFE_RELEASE(new_srv);
        SAFE_RELEASE(new_tex);
        return 0;
    }
    d3d11_unbind_draw_resources(ctx);
    d3d11_release_cubemap_cache_entry(entry);
    entry->tex = new_tex;
    entry->srv = new_srv;

    entry->cubemap_ptr = cubemap;
    entry->pending_generation = generation;
    entry->generation = 0;
    entry->face_size = face_size;
    entry->upload_face = 0;
    entry->upload_next_row = 0;
    entry->upload_in_progress = 1;
    entry->applied_ibl_identity = 0; /* base faces changed: re-overlay after upload */
    d3d11_refresh_pending_ibl_metadata(entry, cubemap);
    entry->last_used_frame = ctx->frame_serial;
    vgfx3d_d3d11_upload_result_t result = d3d11_continue_cubemap_upload(ctx, entry, cubemap);
    if (result == VGFX3D_D3D11_UPLOAD_FAILED) {
        d3d11_unbind_draw_resources(ctx);
        d3d11_mark_cubemap_upload_failed(entry, cubemap, generation, ctx->frame_serial);
        return 0;
    }
    if (ctx->texture_upload_budget_bytes == UINT64_MAX && entry->upload_in_progress) {
        d3d11_unbind_draw_resources(ctx);
        d3d11_release_cubemap_cache_entry(entry);
        return 0;
    }
    return 1;
}

/// @brief Cubemap-cache lookup analogous to `d3d11_get_or_create_srv`.
///
/// Same hit/stale/miss flow as the 2D texture cache; returns NULL while a
/// cubemap is still uploading under the current per-frame budget.
/// @param[in,out] ctx Context containing the cubemap cache and upload budget.
/// @param[in] cubemap Borrowed runtime cubemap source.
/// @param[out] out_temp Optional receiver for a one-draw cubemap resource.
/// @return Resident or white fallback SRV, temporary SRV, or `NULL` on failure.
static ID3D11ShaderResourceView *d3d11_get_or_create_cubemap_srv(d3d11_context_t *ctx,
                                                                 const rt_cubemap3d *cubemap,
                                                                 d3d_temp_srv_t *out_temp) {
    uint64_t generation;

    if (out_temp)
        memset(out_temp, 0, sizeof(*out_temp));
    if (!ctx || !cubemap)
        return NULL;
    generation = vgfx3d_get_cubemap_generation(cubemap);

    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        if (ctx->cubemap_cache[i].cubemap_ptr == cubemap &&
            ctx->cubemap_cache[i].failed_generation == generation)
            return NULL;
        if (ctx->cubemap_cache[i].cubemap_ptr == cubemap &&
            ctx->cubemap_cache[i].generation == generation && ctx->cubemap_cache[i].tex &&
            ctx->cubemap_cache[i].srv) {
            ctx->cubemap_cache[i].last_used_frame = ctx->frame_serial;
            d3d11_apply_ibl_mips(ctx, &ctx->cubemap_cache[i], cubemap);
            return ctx->cubemap_cache[i].srv;
        }
        if (ctx->cubemap_cache[i].cubemap_ptr == cubemap &&
            ctx->cubemap_cache[i].pending_generation == generation &&
            ctx->cubemap_cache[i].upload_in_progress) {
            ctx->cubemap_cache[i].last_used_frame = ctx->frame_serial;
            vgfx3d_d3d11_upload_result_t result =
                d3d11_continue_cubemap_upload(ctx, &ctx->cubemap_cache[i], cubemap);
            if (result == VGFX3D_D3D11_UPLOAD_COMPLETE)
                return ctx->cubemap_cache[i].srv;
            if (result == VGFX3D_D3D11_UPLOAD_FAILED) {
                d3d11_mark_cubemap_upload_failed(
                    &ctx->cubemap_cache[i], cubemap, generation, ctx->frame_serial);
                return NULL;
            }
            return d3d11_fallback_white_cubemap_srv(ctx);
        }
    }

    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        if (ctx->cubemap_cache[i].cubemap_ptr == cubemap) {
            if (!d3d11_start_cubemap_upload(ctx, &ctx->cubemap_cache[i], cubemap, generation))
                return NULL;
            return ctx->cubemap_cache[i].upload_in_progress ? d3d11_fallback_white_cubemap_srv(ctx)
                                                            : ctx->cubemap_cache[i].srv;
        }
    }

    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        if (!ctx->cubemap_cache[i].cubemap_ptr) {
            if (!d3d11_start_cubemap_upload(ctx, &ctx->cubemap_cache[i], cubemap, generation))
                return NULL;
            return ctx->cubemap_cache[i].upload_in_progress ? d3d11_fallback_white_cubemap_srv(ctx)
                                                            : ctx->cubemap_cache[i].srv;
        }
    }
    if (ctx->cubemap_cache_count < INT_MAX &&
        d3d11_ensure_cubemap_cache_capacity(ctx, ctx->cubemap_cache_count + 1)) {
        d3d_cubemap_cache_entry_t *entry = &ctx->cubemap_cache[ctx->cubemap_cache_count++];
        memset(entry, 0, sizeof(*entry));
        if (!d3d11_start_cubemap_upload(ctx, entry, cubemap, generation)) {
            if (entry->failed_generation != generation)
                ctx->cubemap_cache_count--;
            return NULL;
        }
        return entry->upload_in_progress ? d3d11_fallback_white_cubemap_srv(ctx) : entry->srv;
    }

    {
        uint64_t upload_bytes = 0;
        if (!vgfx3d_estimate_cubemap_rgba_upload_bytes(cubemap, &upload_bytes) ||
            !vgfx3d_d3d11_upload_budget_allows(
                ctx->texture_upload_budget_bytes, ctx->texture_upload_bytes, upload_bytes))
            return d3d11_fallback_white_cubemap_srv(ctx);
    }
    return d3d11_create_temp_cubemap_srv(ctx, cubemap, out_temp);
}
