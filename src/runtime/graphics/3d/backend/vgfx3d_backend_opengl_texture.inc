//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_texture.inc
// Purpose: OpenGL texture/cubemap caches, streaming uploads, and morph resources.
// Key invariants:
//   - Compressed mip chains are validated before any driver length or region call.
//   - Failed source generations stop retrying while budget-paused uploads remain resumable.
// Ownership/Lifetime:
//   - Included by vgfx3d_backend_opengl.c; it is not a standalone translation unit.
//   - Cache entries own GL names; host arrays and shared scratch live for the GL context.
// Links: vgfx3d_backend_opengl.c, vgfx3d_backend_utils.h
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_backend_opengl_texture.inc
 * @brief Implements OpenGL texture, cubemap, and morph-resource caching and streaming uploads.
 *
 * This implementation fragment manages context-owned cache entries, direct-mapped lookup hints,
 * frame-budgeted RGBA and block-compressed uploads, cubemap IBL mip overlays, and least-recently
 * used eviction. Upload failures are memoized by source generation so callers receive a stable
 * fallback without retrying irrecoverable work every frame.
 */

typedef enum {
    GL_UPLOAD_FAILED = -1,
    GL_UPLOAD_PENDING = 0,
    GL_UPLOAD_COMPLETE = 1,
} gl_upload_status_t;

/// @brief Retire an unrecoverable texture upload while preserving any older fallback texture.
/// @param entry Cache entry whose active upload and generation state are retired; null is a no-op.
/// @param generation Source generation that failed and must not be retried unchanged.
/// @param mip_start First resident native mip associated with the failed request.
/// @param mip_count Number of resident native mips associated with the failed request.
static void gl_fail_texture_upload(gl_texture_cache_entry_t *entry,
                                   uint64_t generation,
                                   int64_t mip_start,
                                   int64_t mip_count) {
    if (!entry)
        return;
    if (entry->tex) {
        if (entry->generation != 0 && !entry->fallback_tex)
            entry->fallback_tex = entry->tex;
        else
            gl.DeleteTextures(1, &entry->tex);
    }
    entry->tex = 0;
    entry->generation = 0;
    entry->pending_generation = 0;
    entry->failed_generation = generation;
    entry->failed_mip_start = mip_start;
    entry->failed_mip_count = mip_count;
    entry->upload_in_progress = 0;
    entry->upload_next_row = 0;
    entry->native_next_block_row = 0;
    entry->native_next_mip = 0;
}

/// @brief Retire an unrecoverable cubemap upload while preserving an older complete cubemap.
/// @param entry Cache entry whose candidate texture and upload cursors are reset; null is a no-op.
/// @param generation Cubemap source generation that failed and must not be retried unchanged.
static void gl_fail_cubemap_upload(gl_cubemap_cache_entry_t *entry, uint64_t generation) {
    if (!entry)
        return;
    if (entry->tex) {
        if (entry->generation != 0 && !entry->fallback_tex)
            entry->fallback_tex = entry->tex;
        else
            gl.DeleteTextures(1, &entry->tex);
    }
    entry->tex = 0;
    entry->generation = 0;
    entry->pending_generation = 0;
    entry->failed_generation = generation;
    entry->upload_in_progress = 0;
    entry->upload_face = 0;
    entry->upload_next_row = 0;
    entry->applied_ibl_identity = 0;
    entry->failed_ibl_identity = 0;
    entry->pending_ibl_identity = 0;
    entry->pending_ibl_bytes = 0;
}

/// @brief Compare two `uint64_t` values for ascending `qsort()` ordering.
/// @param lhs Pointer to the first unsigned 64-bit value.
/// @param rhs Pointer to the second unsigned 64-bit value.
/// @return A negative, zero, or positive value when @p lhs sorts before, equal to, or after
///         @p rhs.
static int gl_u64_compare_ascending(const void *lhs, const void *rhs) {
    const uint64_t a = *(const uint64_t *)lhs;
    const uint64_t b = *(const uint64_t *)rhs;
    return (a > b) - (a < b);
}

/// @brief Grow and reuse the age-ordering scratch shared by all OpenGL resource caches.
/// @details Cache pruning can run several times in one frame. A context-owned buffer avoids a
///          malloc/qsort/free cycle for every texture, cubemap, and morph cache while retaining
///          the allocation-free oldest-entry fallback when growth fails.
/// @param ctx Borrowed OpenGL context that owns the reusable scratch allocation.
/// @param needed Positive number of age values the caller needs to store.
/// @return Context-owned storage for at least @p needed values, or null on invalid input,
///         overflow, or allocation failure.
static uint64_t *gl_cache_age_scratch(gl_context_t *ctx, int32_t needed) {
    int32_t new_capacity;
    uint64_t *grown;

    if (!ctx || needed <= 0)
        return NULL;
    if (ctx->cache_age_scratch && ctx->cache_age_scratch_capacity >= needed)
        return ctx->cache_age_scratch;
    new_capacity = ctx->cache_age_scratch_capacity > 0 ? ctx->cache_age_scratch_capacity : 64;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity < needed || (size_t)new_capacity > SIZE_MAX / sizeof(*grown))
        return NULL;
    grown = (uint64_t *)realloc(ctx->cache_age_scratch, (size_t)new_capacity * sizeof(*grown));
    if (!grown)
        return NULL;
    ctx->cache_age_scratch = grown;
    ctx->cache_age_scratch_capacity = new_capacity;
    return grown;
}

/// @brief Replace one texture-cache slot's retained TextureAsset3D identity.
/// @param entry Cache entry whose runtime-object ownership is updated.
/// @param asset Borrowed TextureAsset3D to retain, or null to clear the identity.
static void gl_texture_cache_assign_asset(gl_texture_cache_entry_t *entry, void *asset) {
    void *previous;
    if (!entry || entry->texture_asset == asset)
        return;
    if (asset)
        rt_obj_retain_maybe(asset);
    previous = entry->texture_asset;
    entry->texture_asset = asset;
    if (previous && rt_obj_release_check0(previous))
        rt_obj_free(previous);
}

/// @brief Release one texture-cache slot's owned texture objects and retained asset, then zero it.
/// @param entry Cache entry to release and reset; null is a no-op.
static void texture_cache_release_entry(gl_texture_cache_entry_t *entry) {
    if (!entry)
        return;
    if (entry->tex)
        gl.DeleteTextures(1, &entry->tex);
    if (entry->fallback_tex)
        gl.DeleteTextures(1, &entry->fallback_tex);
    gl_texture_cache_assign_asset(entry, NULL);
    memset(entry, 0, sizeof(*entry));
}

/// @brief Hash an object identity into the direct-mapped texture-cache hint table.
/// @param key Object-identity pointer to hash.
/// @return An index in `[0, GL_TEXTURE_CACHE_HINT_CAPACITY)`.
static uint32_t gl_texture_cache_hint_slot(const void *key) {
    uintptr_t value = (uintptr_t)key;
    value ^= value >> 7u;
    value ^= value >> 17u;
    return (uint32_t)value & (GL_TEXTURE_CACHE_HINT_CAPACITY - 1u);
}

/// @brief Remember the current array index for a Pixels or TextureAsset3D identity.
/// @param ctx Borrowed OpenGL context containing the cache and hint table.
/// @param key Non-null Pixels or TextureAsset3D identity.
/// @param index Valid current index in the texture-cache array.
static void gl_texture_cache_remember(gl_context_t *ctx, const void *key, int32_t index) {
    if (!ctx || !key || index < 0 || index >= ctx->texture_cache_count)
        return;
    ctx->texture_cache_hints[gl_texture_cache_hint_slot(key)] = index + 1;
}

/// @brief Return a validated hinted Pixels entry index, or -1 on collision/staleness.
/// @param ctx Borrowed OpenGL context containing the cache and hint table.
/// @param pixels Pixels object identity to look up.
/// @return The matching cache index, or -1 when no valid hint exists.
static int32_t gl_texture_cache_hint_pixels(const gl_context_t *ctx, const void *pixels) {
    int32_t index;
    if (!ctx || !pixels)
        return -1;
    index = ctx->texture_cache_hints[gl_texture_cache_hint_slot(pixels)] - 1;
    if (index < 0 || index >= ctx->texture_cache_count ||
        ctx->texture_cache[index].pixels != pixels)
        return -1;
    return index;
}

/// @brief Return a validated hinted native-asset entry index, or -1 on collision/staleness.
/// @param ctx Borrowed OpenGL context containing the cache and hint table.
/// @param asset TextureAsset3D identity to look up.
/// @return The matching cache index, or -1 when no valid hint exists.
static int32_t gl_texture_cache_hint_asset(const gl_context_t *ctx, const void *asset) {
    int32_t index;
    if (!ctx || !asset)
        return -1;
    index = ctx->texture_cache_hints[gl_texture_cache_hint_slot(asset)] - 1;
    if (index < 0 || index >= ctx->texture_cache_count ||
        ctx->texture_cache[index].texture_asset != asset)
        return -1;
    return index;
}

/// @brief Rebuild hints after an in-place LRU compaction moved cache entries.
/// @param ctx Borrowed OpenGL context whose entire texture hint table is reconstructed.
static void gl_texture_cache_rebuild_hints(gl_context_t *ctx) {
    if (!ctx)
        return;
    memset(ctx->texture_cache_hints, 0, sizeof(ctx->texture_cache_hints));
    for (int32_t i = 0; i < ctx->texture_cache_count; i++) {
        if (ctx->texture_cache[i].pixels)
            gl_texture_cache_remember(ctx, ctx->texture_cache[i].pixels, i);
        if (ctx->texture_cache[i].texture_asset)
            gl_texture_cache_remember(ctx, ctx->texture_cache[i].texture_asset, i);
    }
}

/// @brief Release one cubemap-cache slot's owned texture object and zero the entry.
/// @param entry Cache entry to release and reset; null is a no-op.
static void cubemap_cache_release_entry(gl_cubemap_cache_entry_t *entry) {
    if (!entry)
        return;
    if (entry->tex)
        gl.DeleteTextures(1, &entry->tex);
    if (entry->fallback_tex)
        gl.DeleteTextures(1, &entry->fallback_tex);
    memset(entry, 0, sizeof(*entry));
}

/// @brief Delete every cached 2D texture and clear all direct-mapped lookup hints.
/// @param ctx Borrowed OpenGL context whose texture entries are released; null is a no-op.
static void texture_cache_clear(gl_context_t *ctx) {
    if (!ctx)
        return;
    memset(ctx->texture_cache_hints, 0, sizeof(ctx->texture_cache_hints));
    if (!ctx->texture_cache)
        return;
    for (int32_t i = 0; i < ctx->texture_cache_count; i++) {
        texture_cache_release_entry(&ctx->texture_cache[i]);
    }
    ctx->texture_cache_count = 0;
}

/// @brief Delete every cached cubemap and reset the count.
/// @param ctx Borrowed OpenGL context whose cubemap entries are released; null or an absent cache
///            is a no-op.
static void cubemap_cache_clear(gl_context_t *ctx) {
    if (!ctx || !ctx->cubemap_cache)
        return;
    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        cubemap_cache_release_entry(&ctx->cubemap_cache[i]);
    }
    ctx->cubemap_cache_count = 0;
}

/// @brief Delete every cached morph-target buffer/TBO pair.
///
/// Each entry holds two `(buffer, TBO)` pairs — one for position
/// deltas and one for normal deltas. Both are released.
/// @param ctx Borrowed OpenGL context whose morph entries are released; null or an absent cache
///            is a no-op.
static void morph_cache_clear(gl_context_t *ctx) {
    if (!ctx || !ctx->morph_cache)
        return;
    for (int32_t i = 0; i < ctx->morph_cache_count; i++) {
        if (ctx->morph_cache[i].morph_tbo)
            gl.DeleteTextures(1, &ctx->morph_cache[i].morph_tbo);
        if (ctx->morph_cache[i].morph_buffer)
            gl.DeleteBuffers(1, &ctx->morph_cache[i].morph_buffer);
        if (ctx->morph_cache[i].morph_normal_tbo)
            gl.DeleteTextures(1, &ctx->morph_cache[i].morph_normal_tbo);
        if (ctx->morph_cache[i].morph_normal_buffer)
            gl.DeleteBuffers(1, &ctx->morph_cache[i].morph_normal_buffer);
    }
    ctx->morph_cache_count = 0;
}

/// @brief Release one morph-cache slot's GL objects and zero the entry.
/// @param entry Cache entry whose position and normal buffer/texture pairs are released; null is
///              a no-op.
static void morph_cache_release_entry(gl_morph_cache_entry_t *entry) {
    if (!entry)
        return;
    if (entry->morph_tbo)
        gl.DeleteTextures(1, &entry->morph_tbo);
    if (entry->morph_buffer)
        gl.DeleteBuffers(1, &entry->morph_buffer);
    if (entry->morph_normal_tbo)
        gl.DeleteTextures(1, &entry->morph_normal_tbo);
    if (entry->morph_normal_buffer)
        gl.DeleteBuffers(1, &entry->morph_normal_buffer);
    memset(entry, 0, sizeof(*entry));
}

/// @brief Drop texture cache entries that exceed the LRU limit.
///
/// "Exceed" = total residency above 256 entries AND last-used frame
/// older than 240 frames ago. In-place compaction (write index < read
/// index when entries are dropped). Keeps frequently-used textures
/// regardless of the count cap.
/// @param ctx Borrowed OpenGL context whose texture cache is compacted to its residency limit.
static void texture_cache_prune(gl_context_t *ctx) {
    int32_t write_index = 0;

    if (!ctx || !ctx->texture_cache)
        return;
    for (int32_t i = 0; i < ctx->texture_cache_count; i++) {
        int keep = 1;
        if (ctx->texture_cache_count - write_index > GL_TEXTURE_CACHE_MAX_RESIDENT &&
            vgfx3d_opengl_should_prune_cache_entry(ctx->frame_serial,
                                                   ctx->texture_cache[i].last_used_frame,
                                                   GL_TEXTURE_CACHE_PRUNE_AGE)) {
            keep = 0;
        }
        if (!keep) {
            texture_cache_release_entry(&ctx->texture_cache[i]);
            continue;
        }
        if (write_index != i)
            ctx->texture_cache[write_index] = ctx->texture_cache[i];
        write_index++;
    }
    ctx->texture_cache_count = write_index;
    if (ctx->texture_cache_count > GL_TEXTURE_CACHE_MAX_RESIDENT) {
        const int32_t remove_goal = ctx->texture_cache_count - GL_TEXTURE_CACHE_MAX_RESIDENT;
        uint64_t *ages = gl_cache_age_scratch(ctx, ctx->texture_cache_count);
        if (ages) {
            uint64_t cutoff;
            int32_t remaining_to_remove = remove_goal;
            write_index = 0;
            for (int32_t i = 0; i < ctx->texture_cache_count; i++) {
                ages[i] = ctx->texture_cache[i].last_used_frame;
            }
            qsort(ages, (size_t)ctx->texture_cache_count, sizeof(*ages), gl_u64_compare_ascending);
            cutoff = ages[remove_goal - 1];
            for (int32_t i = 0; i < ctx->texture_cache_count; i++) {
                const uint64_t age = ctx->texture_cache[i].last_used_frame;
                int evict = 0;
                if (age < cutoff) {
                    evict = 1;
                    remaining_to_remove--;
                } else if (age == cutoff && remaining_to_remove > 0) {
                    evict = 1;
                    remaining_to_remove--;
                }
                if (evict) {
                    texture_cache_release_entry(&ctx->texture_cache[i]);
                    continue;
                }
                if (write_index != i)
                    ctx->texture_cache[write_index] = ctx->texture_cache[i];
                write_index++;
            }
            ctx->texture_cache_count = write_index;
        } else {
            while (ctx->texture_cache_count > GL_TEXTURE_CACHE_MAX_RESIDENT) {
                int32_t oldest = 0;
                for (int32_t i = 1; i < ctx->texture_cache_count; i++) {
                    if (ctx->texture_cache[i].last_used_frame <
                        ctx->texture_cache[oldest].last_used_frame)
                        oldest = i;
                }
                texture_cache_release_entry(&ctx->texture_cache[oldest]);
                if (oldest + 1 < ctx->texture_cache_count) {
                    memmove(&ctx->texture_cache[oldest],
                            &ctx->texture_cache[oldest + 1],
                            (size_t)(ctx->texture_cache_count - oldest - 1) *
                                sizeof(ctx->texture_cache[0]));
                }
                ctx->texture_cache_count--;
            }
        }
    }
    gl_texture_cache_rebuild_hints(ctx);
}

/// @brief LRU prune for the cubemap cache (limit 64, age 240 frames).
/// @param ctx Borrowed OpenGL context whose cubemap cache is compacted to its residency limit.
static void cubemap_cache_prune(gl_context_t *ctx) {
    int32_t write_index = 0;

    if (!ctx || !ctx->cubemap_cache)
        return;
    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        int keep = 1;
        if (ctx->cubemap_cache_count - write_index > GL_CUBEMAP_CACHE_MAX_RESIDENT &&
            vgfx3d_opengl_should_prune_cache_entry(ctx->frame_serial,
                                                   ctx->cubemap_cache[i].last_used_frame,
                                                   GL_CUBEMAP_CACHE_PRUNE_AGE)) {
            keep = 0;
        }
        if (!keep) {
            cubemap_cache_release_entry(&ctx->cubemap_cache[i]);
            continue;
        }
        if (write_index != i)
            ctx->cubemap_cache[write_index] = ctx->cubemap_cache[i];
        write_index++;
    }
    ctx->cubemap_cache_count = write_index;
    if (ctx->cubemap_cache_count > GL_CUBEMAP_CACHE_MAX_RESIDENT) {
        const int32_t remove_goal = ctx->cubemap_cache_count - GL_CUBEMAP_CACHE_MAX_RESIDENT;
        uint64_t *ages = gl_cache_age_scratch(ctx, ctx->cubemap_cache_count);
        if (ages) {
            uint64_t cutoff;
            int32_t remaining_to_remove = remove_goal;
            write_index = 0;
            for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
                ages[i] = ctx->cubemap_cache[i].last_used_frame;
            }
            qsort(ages, (size_t)ctx->cubemap_cache_count, sizeof(*ages), gl_u64_compare_ascending);
            cutoff = ages[remove_goal - 1];
            for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
                const uint64_t age = ctx->cubemap_cache[i].last_used_frame;
                int evict = 0;
                if (age < cutoff) {
                    evict = 1;
                    remaining_to_remove--;
                } else if (age == cutoff && remaining_to_remove > 0) {
                    evict = 1;
                    remaining_to_remove--;
                }
                if (evict) {
                    cubemap_cache_release_entry(&ctx->cubemap_cache[i]);
                    continue;
                }
                if (write_index != i)
                    ctx->cubemap_cache[write_index] = ctx->cubemap_cache[i];
                write_index++;
            }
            ctx->cubemap_cache_count = write_index;
        } else {
            while (ctx->cubemap_cache_count > GL_CUBEMAP_CACHE_MAX_RESIDENT) {
                int32_t oldest = 0;
                for (int32_t i = 1; i < ctx->cubemap_cache_count; i++) {
                    if (ctx->cubemap_cache[i].last_used_frame <
                        ctx->cubemap_cache[oldest].last_used_frame)
                        oldest = i;
                }
                cubemap_cache_release_entry(&ctx->cubemap_cache[oldest]);
                if (oldest + 1 < ctx->cubemap_cache_count) {
                    memmove(&ctx->cubemap_cache[oldest],
                            &ctx->cubemap_cache[oldest + 1],
                            (size_t)(ctx->cubemap_cache_count - oldest - 1) *
                                sizeof(ctx->cubemap_cache[0]));
                }
                ctx->cubemap_cache_count--;
            }
        }
    }
}

/// @brief LRU prune for the morph-target cache (limit 64, age 240 frames).
/// @param ctx Borrowed OpenGL context whose morph cache is compacted to its residency limit.
static void morph_cache_prune(gl_context_t *ctx) {
    int32_t write_index = 0;

    if (!ctx || !ctx->morph_cache)
        return;
    for (int32_t i = 0; i < ctx->morph_cache_count; i++) {
        int keep = 1;
        if (ctx->morph_cache_count - write_index > GL_MORPH_CACHE_MAX_RESIDENT &&
            vgfx3d_opengl_should_prune_cache_entry(
                ctx->frame_serial, ctx->morph_cache[i].last_used_frame, GL_MORPH_CACHE_PRUNE_AGE)) {
            keep = 0;
        }
        if (!keep) {
            morph_cache_release_entry(&ctx->morph_cache[i]);
            continue;
        }
        if (write_index != i)
            ctx->morph_cache[write_index] = ctx->morph_cache[i];
        write_index++;
    }
    ctx->morph_cache_count = write_index;
    if (ctx->morph_cache_count > GL_MORPH_CACHE_MAX_RESIDENT) {
        const int32_t remove_goal = ctx->morph_cache_count - GL_MORPH_CACHE_MAX_RESIDENT;
        uint64_t *ages = gl_cache_age_scratch(ctx, ctx->morph_cache_count);
        if (ages) {
            uint64_t cutoff;
            int32_t remaining_to_remove = remove_goal;
            write_index = 0;
            for (int32_t i = 0; i < ctx->morph_cache_count; i++) {
                ages[i] = ctx->morph_cache[i].last_used_frame;
            }
            qsort(ages, (size_t)ctx->morph_cache_count, sizeof(*ages), gl_u64_compare_ascending);
            cutoff = ages[remove_goal - 1];
            for (int32_t i = 0; i < ctx->morph_cache_count; i++) {
                const uint64_t age = ctx->morph_cache[i].last_used_frame;
                int evict = 0;
                if (age < cutoff) {
                    evict = 1;
                    remaining_to_remove--;
                } else if (age == cutoff && remaining_to_remove > 0) {
                    evict = 1;
                    remaining_to_remove--;
                }
                if (evict) {
                    morph_cache_release_entry(&ctx->morph_cache[i]);
                    continue;
                }
                if (write_index != i)
                    ctx->morph_cache[write_index] = ctx->morph_cache[i];
                write_index++;
            }
            ctx->morph_cache_count = write_index;
        } else {
            while (ctx->morph_cache_count > GL_MORPH_CACHE_MAX_RESIDENT) {
                int32_t oldest = 0;
                for (int32_t i = 1; i < ctx->morph_cache_count; i++) {
                    if (ctx->morph_cache[i].last_used_frame <
                        ctx->morph_cache[oldest].last_used_frame)
                        oldest = i;
                }
                morph_cache_release_entry(&ctx->morph_cache[oldest]);
                if (oldest + 1 < ctx->morph_cache_count) {
                    memmove(&ctx->morph_cache[oldest],
                            &ctx->morph_cache[oldest + 1],
                            (size_t)(ctx->morph_cache_count - oldest - 1) *
                                sizeof(ctx->morph_cache[0]));
                }
                ctx->morph_cache_count--;
            }
        }
    }
}

/// @brief Tear down all three caches and free their backing arrays.
///
/// Called during context destruction. Order matters: clear-then-free
/// each cache so we delete the GL objects before freeing the host array.
/// @param ctx Borrowed OpenGL context whose cache objects, backing arrays, and age scratch are
///            destroyed; null is a no-op.
static void texture_cache_destroy(gl_context_t *ctx) {
    if (!ctx)
        return;
    texture_cache_clear(ctx);
    cubemap_cache_clear(ctx);
    morph_cache_clear(ctx);
    free(ctx->texture_cache);
    ctx->texture_cache = NULL;
    ctx->texture_cache_capacity = 0;
    free(ctx->cubemap_cache);
    ctx->cubemap_cache = NULL;
    ctx->cubemap_cache_capacity = 0;
    free(ctx->morph_cache);
    ctx->morph_cache = NULL;
    ctx->morph_cache_capacity = 0;
    free(ctx->cache_age_scratch);
    ctx->cache_age_scratch = NULL;
    ctx->cache_age_scratch_capacity = 0;
}

/// @brief Add @p bytes to the context's running per-frame texture-upload total (saturating).
/// @param ctx Borrowed OpenGL context whose upload counter is updated; null is a no-op.
/// @param bytes Number of newly uploaded bytes to add with saturation at @c UINT64_MAX.
static void gl_record_texture_upload_bytes(gl_context_t *ctx, uint64_t bytes) {
    if (!ctx || bytes == 0)
        return;
    if (ctx->texture_upload_bytes > UINT64_MAX - bytes) {
        ctx->texture_upload_bytes = UINT64_MAX;
        return;
    }
    ctx->texture_upload_bytes += bytes;
}

/// @brief Report native texture-related capabilities this GL context can use.
/// @details Extension discovery is captured during context creation, but native block-compressed
///          upload is not advertised until OpenGL has an upload+sample conformance gate. Some Mesa
///          paths accept BC/ASTC/ETC2 uploads without producing valid sampled texels, and
///          BackendSupports must mean the full draw path works. Anisotropic filtering is a sampler
///          state and can be advertised directly from the extension/limit query.
/// @param ctx_ptr Opaque borrowed pointer to the queried @c gl_context_t.
/// @return Bitwise OR of supported @c RT_CANVAS3D_BACKEND_CAP_* flags, or zero for a null context.
static int64_t gl_get_native_texture_caps(void *ctx_ptr) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    int64_t caps = 0;
    if (!ctx)
        return 0;
    if (ctx->anisotropy_supported && ctx->max_texture_anisotropy > 1.0f)
        caps |= RT_CANVAS3D_BACKEND_CAP_ANISOTROPY;
    return caps;
}

/// @brief Map a native compressed mip's format/block size to the matching GL internal format enum.
/// @details BC7→BPTC, ETC2→ETC2_EAC, ASTC→the specific ASTC block-size enum. Returns 0 if
/// unsupported.
/// @param mip Borrowed native mip descriptor containing format and compression-block metadata.
/// @return The matching OpenGL compressed internal-format enum, or zero when unsupported.
static GLenum gl_native_texture_internal_format(const vgfx3d_native_texture_mip_t *mip) {
    if (!mip)
        return 0;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7)
        return GL_COMPRESSED_RGBA_BPTC_UNORM;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1)
        return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC3)
        return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC4)
        return GL_COMPRESSED_RED_RGTC1;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_BC5)
        return GL_COMPRESSED_RG_RGTC2;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_ETC2)
        return GL_COMPRESSED_RGBA8_ETC2_EAC;
    if (mip->format_id == RT_TEXTUREASSET3D_NATIVE_FORMAT_ASTC) {
        if (mip->block_width == 4 && mip->block_height == 4)
            return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
        if (mip->block_width == 5 && mip->block_height == 4)
            return GL_COMPRESSED_RGBA_ASTC_5x4_KHR;
        if (mip->block_width == 5 && mip->block_height == 5)
            return GL_COMPRESSED_RGBA_ASTC_5x5_KHR;
        if (mip->block_width == 6 && mip->block_height == 5)
            return GL_COMPRESSED_RGBA_ASTC_6x5_KHR;
        if (mip->block_width == 6 && mip->block_height == 6)
            return GL_COMPRESSED_RGBA_ASTC_6x6_KHR;
        if (mip->block_width == 8 && mip->block_height == 5)
            return GL_COMPRESSED_RGBA_ASTC_8x5_KHR;
        if (mip->block_width == 8 && mip->block_height == 6)
            return GL_COMPRESSED_RGBA_ASTC_8x6_KHR;
        if (mip->block_width == 8 && mip->block_height == 8)
            return GL_COMPRESSED_RGBA_ASTC_8x8_KHR;
        if (mip->block_width == 10 && mip->block_height == 5)
            return GL_COMPRESSED_RGBA_ASTC_10x5_KHR;
        if (mip->block_width == 10 && mip->block_height == 6)
            return GL_COMPRESSED_RGBA_ASTC_10x6_KHR;
        if (mip->block_width == 10 && mip->block_height == 8)
            return GL_COMPRESSED_RGBA_ASTC_10x8_KHR;
        if (mip->block_width == 10 && mip->block_height == 10)
            return GL_COMPRESSED_RGBA_ASTC_10x10_KHR;
        if (mip->block_width == 12 && mip->block_height == 10)
            return GL_COMPRESSED_RGBA_ASTC_12x10_KHR;
        if (mip->block_width == 12 && mip->block_height == 12)
            return GL_COMPRESSED_RGBA_ASTC_12x12_KHR;
    }
    return 0;
}

/// @brief Bytes per block-row of a native compressed mip (block columns times block bytes).
/// @param mip Borrowed native mip descriptor whose compressed row size is calculated.
/// @return Compressed bytes per block row, or zero for invalid metadata or arithmetic overflow.
static uint64_t gl_native_texture_row_bytes(const vgfx3d_native_texture_mip_t *mip) {
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
/// @param entry Borrowed texture-cache entry whose unfinished upload is measured.
/// @return Remaining upload bytes, or zero for a null entry or completed upload.
static uint64_t gl_texture_pending_bytes(const gl_texture_cache_entry_t *entry) {
    if (!entry)
        return 0;
    if (entry->texture_asset)
        return vgfx3d_textureasset_pending_native_snapshot_bytes(entry->texture_asset,
                                                                 entry->native_mip_start,
                                                                 entry->native_mip_count,
                                                                 entry->native_next_mip,
                                                                 entry->native_next_block_row,
                                                                 entry->upload_in_progress);
    return vgfx3d_pending_rgba_upload_bytes(
        entry->width, entry->height, entry->upload_next_row, entry->upload_in_progress);
}

/// @brief Bytes still to upload for a cached cubemap entry across its remaining faces/rows.
/// @param entry Borrowed cubemap-cache entry whose base faces and pending IBL tail are measured.
/// @return Remaining upload bytes, saturated at @c UINT64_MAX.
static uint64_t gl_cubemap_pending_bytes(const gl_cubemap_cache_entry_t *entry) {
    uint64_t base_bytes;
    if (!entry)
        return 0;
    base_bytes = vgfx3d_pending_cubemap_rgba_upload_bytes(
        entry->face_size, entry->upload_face, entry->upload_next_row, entry->upload_in_progress);
    if (base_bytes > UINT64_MAX - entry->pending_ibl_bytes)
        return UINT64_MAX;
    return base_bytes + entry->pending_ibl_bytes;
}

/// @brief Total bytes still pending across all in-progress texture uploads on the context
/// (saturating).
/// @param ctx_ptr Opaque borrowed pointer to the queried @c gl_context_t.
/// @return Sum of pending 2D and cubemap upload bytes, saturated at @c UINT64_MAX.
static uint64_t gl_get_texture_upload_pending_bytes(void *ctx_ptr) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    uint64_t total = 0;

    if (!ctx)
        return 0;
    for (int32_t i = 0; i < ctx->texture_cache_count; i++) {
        uint64_t bytes = gl_texture_pending_bytes(&ctx->texture_cache[i]);
        if (total > UINT64_MAX - bytes)
            return UINT64_MAX;
        total += bytes;
    }
    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        uint64_t bytes = gl_cubemap_pending_bytes(&ctx->cubemap_cache[i]);
        if (total > UINT64_MAX - bytes)
            return UINT64_MAX;
        total += bytes;
    }
    return total;
}

/// @brief Set the per-frame byte budget that paces streaming texture uploads on this context.
/// @param ctx_ptr Opaque borrowed pointer to the updated @c gl_context_t; null is a no-op.
/// @param bytes Maximum bytes streaming uploads may record during the frame, where
///              @c UINT64_MAX means unlimited.
static void gl_set_texture_upload_budget(void *ctx_ptr, uint64_t bytes) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (ctx)
        ctx->texture_upload_budget_bytes = bytes;
}

/// @brief Upload more of an in-progress native compressed texture, bounded by the upload budget.
/// @details Uploads one or more block-row bands through glCompressedTexSubImage2D. Whole-mip image
///          storage is defined with a NULL glCompressedTexImage2D payload when a mip begins.
/// @param ctx Borrowed OpenGL context providing the frame budget and upload accounting.
/// @param entry Mutable cache entry containing the native snapshot and upload cursors.
/// @return @c GL_UPLOAD_COMPLETE when all mips finish, @c GL_UPLOAD_PENDING when budget-paused,
///         or @c GL_UPLOAD_FAILED for invalid metadata or an OpenGL failure.
static gl_upload_status_t gl_continue_native_texture_upload(gl_context_t *ctx,
                                                            gl_texture_cache_entry_t *entry) {
    if (!ctx || !entry || !entry->texture_asset || !entry->upload_in_progress || !entry->tex ||
        !gl.CompressedTexImage2D || !gl.CompressedTexSubImage2D)
        return GL_UPLOAD_FAILED;
    if (entry->native_mip_count <= 0)
        return GL_UPLOAD_FAILED;
    if (entry->native_next_mip < 0)
        return GL_UPLOAD_FAILED;
    if (entry->native_next_mip > entry->native_mip_count)
        return GL_UPLOAD_FAILED;
    gl.BindTexture(GL_TEXTURE_2D, entry->tex);
    while (entry->native_next_mip < entry->native_mip_count) {
        vgfx3d_native_texture_mip_t mip;
        GLenum internal_format;
        uint64_t row_bytes;
        uint64_t block_rows;
        uint64_t required_bytes;
        int32_t rows;
        int32_t y;
        int32_t h;
        uint64_t offset;
        uint64_t upload_bytes;
        if (ctx->texture_upload_budget_bytes != UINT64_MAX &&
            ctx->texture_upload_bytes >= ctx->texture_upload_budget_bytes)
            return GL_UPLOAD_PENDING;
        if (!vgfx3d_textureasset_get_native_snapshot_mip(entry->texture_asset,
                                                         entry->native_mip_start,
                                                         entry->native_mip_count,
                                                         entry->native_next_mip,
                                                         &mip))
            return GL_UPLOAD_FAILED;
        if (!vgfx3d_validate_native_texture_mip(&mip,
                                                entry->width,
                                                entry->height,
                                                entry->native_next_mip,
                                                entry->native_format,
                                                entry->native_block_width,
                                                entry->native_block_height,
                                                entry->native_block_bytes,
                                                (uint64_t)INT32_MAX,
                                                &required_bytes))
            return GL_UPLOAD_FAILED;
        internal_format = gl_native_texture_internal_format(&mip);
        row_bytes = gl_native_texture_row_bytes(&mip);
        block_rows = ((uint64_t)(uint32_t)mip.height + (uint64_t)(uint32_t)mip.block_height - 1u) /
                     (uint64_t)(uint32_t)mip.block_height;
        if (!internal_format || row_bytes == 0 || row_bytes > (uint64_t)INT32_MAX ||
            block_rows > (uint64_t)INT32_MAX)
            return GL_UPLOAD_FAILED;
        if (entry->native_next_block_row < 0 ||
            (uint64_t)(uint32_t)entry->native_next_block_row >= block_rows)
            entry->native_next_block_row = 0;
        if (entry->native_next_block_row == 0) {
            gl.CompressedTexImage2D(GL_TEXTURE_2D,
                                    (GLint)entry->native_next_mip,
                                    internal_format,
                                    (GLsizei)mip.width,
                                    (GLsizei)mip.height,
                                    0,
                                    (GLsizei)required_bytes,
                                    NULL);
            if (!gl_upload_ok())
                return GL_UPLOAD_FAILED;
        }
        rows = vgfx3d_upload_block_rows_for_budget(mip.width,
                                                   mip.height,
                                                   mip.block_width,
                                                   mip.block_height,
                                                   mip.block_bytes,
                                                   entry->native_next_block_row,
                                                   ctx->texture_upload_budget_bytes,
                                                   ctx->texture_upload_bytes);
        if (rows <= 0)
            return GL_UPLOAD_FAILED;
        y = entry->native_next_block_row * mip.block_height;
        h = rows * mip.block_height;
        if (y < 0 || y >= mip.height)
            return GL_UPLOAD_FAILED;
        if (h > mip.height - y)
            h = mip.height - y;
        offset = (uint64_t)(uint32_t)entry->native_next_block_row * row_bytes;
        upload_bytes = (uint64_t)(uint32_t)rows * row_bytes;
        if (offset > required_bytes || upload_bytes > required_bytes - offset ||
            upload_bytes > (uint64_t)INT32_MAX)
            return GL_UPLOAD_FAILED;
        gl.CompressedTexSubImage2D(GL_TEXTURE_2D,
                                   (GLint)entry->native_next_mip,
                                   0,
                                   (GLint)y,
                                   (GLsizei)mip.width,
                                   (GLsizei)h,
                                   internal_format,
                                   (GLsizei)upload_bytes,
                                   (const uint8_t *)mip.data + offset);
        if (!gl_upload_ok())
            return GL_UPLOAD_FAILED;
        gl_record_texture_upload_bytes(ctx, upload_bytes);
        entry->native_next_block_row += rows;
        if ((uint64_t)(uint32_t)entry->native_next_block_row >= block_rows) {
            entry->native_next_mip++;
            entry->native_next_block_row = 0;
        }
        entry->last_used_frame = ctx->frame_serial;
    }
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D,
                     GL_TEXTURE_MIN_FILTER,
                     entry->native_mip_count > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)(entry->native_mip_count - 1));
    if (!gl_upload_ok())
        return GL_UPLOAD_FAILED;
    if (entry->fallback_tex) {
        gl.DeleteTextures(1, &entry->fallback_tex);
        entry->fallback_tex = 0;
    }
    entry->generation = entry->pending_generation;
    entry->pending_generation = 0;
    entry->upload_in_progress = 0;
    return GL_UPLOAD_COMPLETE;
}

/// @brief Begin uploading a native compressed TextureAsset3D: create the GL texture and seed the
/// cursor.
/// @details Allocates the texture object and mip metadata, then kicks the first budgeted upload
/// chunk.
/// @param ctx Borrowed OpenGL context providing capabilities, limits, and upload accounting.
/// @param entry Mutable cache entry to initialize for this upload.
/// @param asset Borrowed TextureAsset3D source exposing the native mip snapshot.
/// @param cache_key Non-zero source generation used to validate cache residency.
/// @param mip_start First resident source mip represented as destination level zero.
/// @param mip_count Number of consecutive native mips to upload.
/// @return @c GL_UPLOAD_COMPLETE when the upload finishes immediately,
///         @c GL_UPLOAD_PENDING when initialized but budget-paused, or
///         @c GL_UPLOAD_FAILED when validation, allocation, or upload fails.
static gl_upload_status_t gl_start_native_texture_upload(gl_context_t *ctx,
                                                         gl_texture_cache_entry_t *entry,
                                                         void *asset,
                                                         uint64_t cache_key,
                                                         int64_t mip_start,
                                                         int64_t mip_count) {
    vgfx3d_native_texture_mip_t first_mip;

    if (!ctx || !entry || !asset ||
        !vgfx3d_textureasset_native_supported(asset, gl_get_native_texture_caps(ctx)) ||
        !vgfx3d_textureasset_get_native_snapshot_mip(asset, mip_start, mip_count, 0, &first_mip) ||
        !gl_native_texture_internal_format(&first_mip) ||
        !vgfx3d_native_texture_block_layout_is_valid(&first_mip))
        return GL_UPLOAD_FAILED;
    if (mip_count <= 0 ||
        mip_count > vgfx3d_opengl_compute_mip_count(first_mip.width, first_mip.height) ||
        mip_count > INT32_MAX)
        return GL_UPLOAD_FAILED;
    if (ctx->max_texture_size > 0 &&
        (first_mip.width > ctx->max_texture_size || first_mip.height > ctx->max_texture_size))
        return GL_UPLOAD_FAILED;
    if (entry->tex && entry->generation != 0 && entry->generation != cache_key) {
        if (entry->fallback_tex)
            gl.DeleteTextures(1, &entry->fallback_tex);
        entry->fallback_tex = entry->tex;
        entry->tex = 0;
    }
    if (!entry->tex) {
        gl.GenTextures(1, &entry->tex);
        if (!entry->tex)
            return GL_UPLOAD_FAILED;
    }
    entry->pixels = NULL;
    gl_texture_cache_assign_asset(entry, asset);
    entry->pending_generation = cache_key;
    entry->failed_generation = 0;
    entry->failed_mip_start = 0;
    entry->failed_mip_count = 0;
    entry->generation = 0;
    entry->width = first_mip.width;
    entry->height = first_mip.height;
    entry->upload_next_row = 0;
    entry->native_format = first_mip.format_id;
    entry->native_block_width = first_mip.block_width;
    entry->native_block_height = first_mip.block_height;
    entry->native_block_bytes = first_mip.block_bytes;
    entry->native_next_block_row = 0;
    entry->native_next_mip = 0;
    entry->native_mip_start = mip_start;
    entry->native_mip_count = mip_count;
    entry->upload_in_progress = 1;
    entry->last_used_frame = ctx->frame_serial;
    return gl_continue_native_texture_upload(ctx, entry);
}

/// @brief Upload more rows of an in-progress RGBA texture, bounded by the upload budget.
/// @param ctx Borrowed OpenGL context providing the frame budget and upload accounting.
/// @param entry Mutable cache entry containing the destination texture and next-row cursor.
/// @param pixels_ptr Borrowed Pixels source whose current generation is being uploaded.
/// @return @c GL_UPLOAD_COMPLETE when all rows and mip generation finish,
///         @c GL_UPLOAD_PENDING when rows remain, or @c GL_UPLOAD_FAILED on invalid data or GL
///         failure.
static gl_upload_status_t gl_continue_texture_upload(gl_context_t *ctx,
                                                     gl_texture_cache_entry_t *entry,
                                                     const void *pixels_ptr) {
    int32_t rows;
    int32_t slice_w = 0;
    int32_t slice_rows = 0;
    uint8_t *rgba;
    size_t rgba_bytes;
    uint64_t bytes;

    if (!ctx || !entry || !pixels_ptr || !entry->upload_in_progress || !entry->tex)
        return GL_UPLOAD_FAILED;
    if (entry->width <= 0 || entry->height <= 0)
        return GL_UPLOAD_FAILED;
    if (entry->upload_next_row < 0 || entry->upload_next_row >= entry->height)
        return GL_UPLOAD_FAILED;
    if (ctx->texture_upload_budget_bytes != UINT64_MAX &&
        ctx->texture_upload_bytes >= ctx->texture_upload_budget_bytes)
        return GL_UPLOAD_PENDING;
    rows = vgfx3d_upload_rows_for_budget(entry->width,
                                         entry->height,
                                         entry->upload_next_row,
                                         ctx->texture_upload_budget_bytes,
                                         ctx->texture_upload_bytes);
    if (rows <= 0)
        return GL_UPLOAD_FAILED;
    if ((size_t)entry->width > SIZE_MAX / 4u ||
        (size_t)rows > SIZE_MAX / ((size_t)entry->width * 4u))
        return GL_UPLOAD_FAILED;
    rgba_bytes = (size_t)entry->width * (size_t)rows * 4u;
    rgba = vgfx3d_ensure_byte_scratch(
        &ctx->texture_upload_scratch_rgba, &ctx->texture_upload_scratch_bytes, rgba_bytes);
    if (!rgba ||
        vgfx3d_unpack_pixels_rgba_rows_into(pixels_ptr,
                                            entry->upload_next_row,
                                            rows,
                                            0,
                                            rgba,
                                            ctx->texture_upload_scratch_bytes,
                                            &slice_w,
                                            &slice_rows) != 0 ||
        slice_w != entry->width || slice_rows <= 0) {
        return GL_UPLOAD_FAILED;
    }

    gl.BindTexture(GL_TEXTURE_2D, entry->tex);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexSubImage2D(GL_TEXTURE_2D,
                     0,
                     0,
                     entry->upload_next_row,
                     slice_w,
                     slice_rows,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     rgba);
    if (!gl_upload_ok()) {
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return GL_UPLOAD_FAILED;
    }
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    bytes = (uint64_t)(uint32_t)slice_w * (uint64_t)(uint32_t)slice_rows * 4u;
    gl_record_texture_upload_bytes(ctx, bytes);

    entry->upload_next_row += slice_rows;
    entry->last_used_frame = ctx->frame_serial;
    if (entry->upload_next_row >= entry->height) {
        gl.GenerateMipmap(GL_TEXTURE_2D);
        if (!gl_upload_ok())
            return GL_UPLOAD_FAILED;
        if (entry->fallback_tex) {
            gl.DeleteTextures(1, &entry->fallback_tex);
            entry->fallback_tex = 0;
        }
        entry->generation = entry->pending_generation;
        entry->pending_generation = 0;
        entry->upload_in_progress = 0;
        return GL_UPLOAD_COMPLETE;
    }
    return GL_UPLOAD_PENDING;
}

/// @brief Begin uploading an RGBA Pixels texture: allocate the GL texture and seed the row cursor.
/// @param ctx Borrowed OpenGL context providing limits, budget, and upload accounting.
/// @param entry Mutable cache entry to initialize for this upload.
/// @param pixels_ptr Borrowed Pixels source to stream into an RGBA8 texture.
/// @param cache_key Source generation used to validate the completed cache entry.
/// @return @c GL_UPLOAD_COMPLETE when the upload finishes immediately,
///         @c GL_UPLOAD_PENDING when initialized with rows remaining, or
///         @c GL_UPLOAD_FAILED when validation, allocation, or upload fails.
static gl_upload_status_t gl_start_texture_upload(gl_context_t *ctx,
                                                  gl_texture_cache_entry_t *entry,
                                                  const void *pixels_ptr,
                                                  uint64_t cache_key) {
    int32_t w = 0;
    int32_t h = 0;

    if (!ctx || !entry || !pixels_ptr || !vgfx3d_get_pixels_extent(pixels_ptr, &w, &h))
        return GL_UPLOAD_FAILED;
    if (ctx->max_texture_size > 0 && (w > ctx->max_texture_size || h > ctx->max_texture_size))
        return GL_UPLOAD_FAILED;
    if (entry->tex && entry->generation != 0 && entry->generation != cache_key) {
        if (entry->fallback_tex)
            gl.DeleteTextures(1, &entry->fallback_tex);
        entry->fallback_tex = entry->tex;
        entry->tex = 0;
    }
    if (!entry->tex) {
        gl.GenTextures(1, &entry->tex);
        if (!entry->tex)
            return GL_UPLOAD_FAILED;
    }

    entry->pixels = pixels_ptr;
    gl_texture_cache_assign_asset(entry, NULL);
    entry->pending_generation = cache_key;
    entry->failed_generation = 0;
    entry->failed_mip_start = 0;
    entry->failed_mip_count = 0;
    entry->generation = 0;
    entry->width = w;
    entry->height = h;
    entry->upload_next_row = 0;
    entry->native_format = RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE;
    entry->native_block_width = 0;
    entry->native_block_height = 0;
    entry->native_block_bytes = 0;
    entry->native_next_block_row = 0;
    entry->native_next_mip = 0;
    entry->native_mip_count = 0;
    entry->upload_in_progress = 1;
    entry->last_used_frame = ctx->frame_serial;

    gl.BindTexture(GL_TEXTURE_2D, entry->tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (!gl_upload_ok())
        return GL_UPLOAD_FAILED;
    return gl_continue_texture_upload(ctx, entry, pixels_ptr);
}

/// @brief Texture-cache lookup with auto-create.
///
/// Three-way:
///   1. Hit (pixels + cache-key match) → bump LRU timestamp, return.
///   2. Pixels match but generation stale → re-upload into the existing
///      texture object (saves a `glDeleteTextures`/`glGenTextures`
///      round-trip).
///   3. Miss → create a new texture, append to cache.
/// Returns 0 on failure (caller falls back to no texture).
/// @param ctx Borrowed OpenGL context containing the texture cache and upload budget.
/// @param index Valid candidate cache index for @p pixels_ptr.
/// @param pixels_ptr Borrowed Pixels identity to resolve.
/// @param cache_key Current Pixels generation.
/// @return A complete texture, preserved fallback, or white fallback texture name; zero only for
///         an invalid cache lookup.
static GLuint gl_resolve_cached_pixels_texture(gl_context_t *ctx,
                                               int32_t index,
                                               const void *pixels_ptr,
                                               uint64_t cache_key) {
    gl_texture_cache_entry_t *entry;
    gl_upload_status_t status;
    if (!ctx || index < 0 || index >= ctx->texture_cache_count)
        return 0;
    entry = &ctx->texture_cache[index];
    if (entry->pixels != pixels_ptr)
        return 0;
    gl_texture_cache_remember(ctx, pixels_ptr, index);
    if (entry->failed_generation == cache_key) {
        ctx->stats.texture_fallback_binds++;
        return entry->fallback_tex ? entry->fallback_tex : gl_fallback_white_texture(ctx);
    }
    if (entry->generation == cache_key) {
        entry->last_used_frame = ctx->frame_serial;
        return entry->tex;
    }
    if (entry->pending_generation == cache_key && entry->upload_in_progress) {
        status = gl_continue_texture_upload(ctx, entry, pixels_ptr);
        if (status == GL_UPLOAD_COMPLETE)
            return entry->tex;
        if (status == GL_UPLOAD_FAILED)
            gl_fail_texture_upload(entry, cache_key, 0, 0);
        ctx->stats.texture_fallback_binds++;
        return entry->fallback_tex ? entry->fallback_tex : gl_fallback_white_texture(ctx);
    }
    status = gl_start_texture_upload(ctx, entry, pixels_ptr, cache_key);
    if (status == GL_UPLOAD_FAILED)
        gl_fail_texture_upload(entry, cache_key, 0, 0);
    if (status != GL_UPLOAD_COMPLETE) {
        ctx->stats.texture_fallback_binds++;
        return entry->fallback_tex ? entry->fallback_tex : gl_fallback_white_texture(ctx);
    }
    return entry->tex;
}

/// @brief Resolve or create a budget-streamed RGBA texture for a Pixels object.
/// @details Uses the direct-mapped identity hint before scanning the cache, grows the cache on a
///          miss, and returns the shared white fallback while a new upload is pending or failed.
/// @param ctx Borrowed OpenGL context containing the texture cache and upload budget.
/// @param pixels_ptr Borrowed Pixels object identity and upload source.
/// @return A resident texture or fallback texture name, or zero for invalid input or cache-growth
///         failure.
static GLuint gl_get_cached_texture(gl_context_t *ctx, const void *pixels_ptr) {
    uint64_t cache_key;
    int32_t hinted;
    if (!ctx || !pixels_ptr)
        return 0;
    cache_key = vgfx3d_get_pixels_cache_key(pixels_ptr);

    hinted = gl_texture_cache_hint_pixels(ctx, pixels_ptr);
    if (hinted >= 0)
        return gl_resolve_cached_pixels_texture(ctx, hinted, pixels_ptr, cache_key);
    for (int32_t i = 0; i < ctx->texture_cache_count; i++) {
        if (ctx->texture_cache[i].pixels == pixels_ptr)
            return gl_resolve_cached_pixels_texture(ctx, i, pixels_ptr, cache_key);
    }

    if (ctx->texture_cache_count >= ctx->texture_cache_capacity) {
        if (ctx->texture_cache_count >= INT_MAX)
            return 0;
        int32_t new_cap = vgfx3d_opengl_next_capacity(
            ctx->texture_cache_capacity, ctx->texture_cache_count + 1, 16);
        if (new_cap <= ctx->texture_cache_count ||
            (size_t)new_cap > SIZE_MAX / sizeof(gl_texture_cache_entry_t))
            return 0;
        gl_texture_cache_entry_t *nv = (gl_texture_cache_entry_t *)realloc(
            ctx->texture_cache, (size_t)new_cap * sizeof(gl_texture_cache_entry_t));
        if (!nv)
            return 0;
        ctx->texture_cache = nv;
        memset(ctx->texture_cache + ctx->texture_cache_capacity,
               0,
               (size_t)(new_cap - ctx->texture_cache_capacity) * sizeof(*ctx->texture_cache));
        ctx->texture_cache_capacity = new_cap;
    }

    int32_t index = ctx->texture_cache_count++;
    gl_texture_cache_entry_t *entry = &ctx->texture_cache[index];
    gl_upload_status_t status;
    memset(entry, 0, sizeof(*entry));
    status = gl_start_texture_upload(ctx, entry, pixels_ptr, cache_key);
    if (status == GL_UPLOAD_FAILED) {
        entry->pixels = pixels_ptr;
        gl_fail_texture_upload(entry, cache_key, 0, 0);
    }
    gl_texture_cache_remember(ctx, pixels_ptr, index);
    if (status != GL_UPLOAD_COMPLETE) {
        ctx->stats.texture_fallback_binds++;
        return gl_fallback_white_texture(ctx);
    }
    return entry->tex;
}

/// @brief Resolve a native TextureAsset3D from a known cache slot.
/// @param ctx Borrowed OpenGL context containing the texture cache and upload budget.
/// @param index Valid candidate cache index for @p asset.
/// @param asset Borrowed TextureAsset3D identity and native-data source.
/// @param cache_key Current native source generation.
/// @param mip_start First resident native mip expected in the cache entry.
/// @param mip_count Number of resident native mips expected in the cache entry.
/// @return A complete texture, preserved fallback, or white fallback texture name; zero only for
///         an invalid cache lookup.
static GLuint gl_resolve_cached_native_texture(gl_context_t *ctx,
                                               int32_t index,
                                               void *asset,
                                               uint64_t cache_key,
                                               int64_t mip_start,
                                               int64_t mip_count) {
    gl_texture_cache_entry_t *entry;
    gl_upload_status_t status;
    if (!ctx || index < 0 || index >= ctx->texture_cache_count)
        return 0;
    entry = &ctx->texture_cache[index];
    if (entry->texture_asset != asset)
        return 0;
    gl_texture_cache_remember(ctx, asset, index);
    if (entry->failed_generation == cache_key && entry->failed_mip_start == mip_start &&
        entry->failed_mip_count == mip_count) {
        ctx->stats.texture_fallback_binds++;
        return entry->fallback_tex ? entry->fallback_tex : gl_fallback_white_texture(ctx);
    }
    if (entry->generation == cache_key && entry->native_mip_start == mip_start &&
        entry->native_mip_count == mip_count) {
        entry->last_used_frame = ctx->frame_serial;
        return entry->tex;
    }
    if (entry->pending_generation == cache_key && entry->native_mip_start == mip_start &&
        entry->native_mip_count == mip_count && entry->upload_in_progress) {
        status = gl_continue_native_texture_upload(ctx, entry);
        if (status == GL_UPLOAD_COMPLETE)
            return entry->tex;
        if (status == GL_UPLOAD_FAILED)
            gl_fail_texture_upload(entry, cache_key, mip_start, mip_count);
        ctx->stats.texture_fallback_binds++;
        return entry->fallback_tex ? entry->fallback_tex : gl_fallback_white_texture(ctx);
    }
    status = gl_start_native_texture_upload(ctx, entry, asset, cache_key, mip_start, mip_count);
    if (status == GL_UPLOAD_FAILED)
        gl_fail_texture_upload(entry, cache_key, mip_start, mip_count);
    if (status != GL_UPLOAD_COMPLETE) {
        ctx->stats.texture_fallback_binds++;
        return entry->fallback_tex ? entry->fallback_tex : gl_fallback_white_texture(ctx);
    }
    return entry->tex;
}

/// @brief Get the GL texture for a native TextureAsset3D, creating/streaming it on a cache miss.
/// @details Keyed by the asset's native cache key so residency changes invalidate the cache entry.
/// @param ctx Borrowed OpenGL context containing the texture cache and native capabilities.
/// @param asset Borrowed TextureAsset3D identity and native-data source.
/// @param cache_key Native source generation, or zero to query it from @p asset.
/// @param mip_start First resident native mip, or a negative value to query it from @p asset.
/// @param mip_count Resident native mip count, or a non-positive value to query it from @p asset.
/// @return The GL texture name, or 0 if it cannot be created.
static GLuint gl_get_cached_native_texture(
    gl_context_t *ctx, void *asset, uint64_t cache_key, int64_t mip_start, int64_t mip_count) {
    int32_t hinted;
    if (!ctx || !asset ||
        !vgfx3d_textureasset_native_supported(asset, gl_get_native_texture_caps(ctx)))
        return 0;
    if (cache_key == 0)
        cache_key = rt_textureasset3d_get_native_cache_key(asset);
    if (mip_start < 0)
        mip_start = rt_textureasset3d_get_resident_mip_start(asset);
    if (mip_count <= 0)
        mip_count = rt_textureasset3d_get_resident_mip_count(asset);
    if (cache_key == 0)
        return 0;

    hinted = gl_texture_cache_hint_asset(ctx, asset);
    if (hinted >= 0)
        return gl_resolve_cached_native_texture(
            ctx, hinted, asset, cache_key, mip_start, mip_count);
    for (int32_t i = 0; i < ctx->texture_cache_count; i++) {
        if (ctx->texture_cache[i].texture_asset == asset)
            return gl_resolve_cached_native_texture(ctx, i, asset, cache_key, mip_start, mip_count);
    }

    if (ctx->texture_cache_count >= ctx->texture_cache_capacity) {
        if (ctx->texture_cache_count >= INT_MAX)
            return 0;
        int32_t new_cap = vgfx3d_opengl_next_capacity(
            ctx->texture_cache_capacity, ctx->texture_cache_count + 1, 16);
        if (new_cap <= ctx->texture_cache_count ||
            (size_t)new_cap > SIZE_MAX / sizeof(gl_texture_cache_entry_t))
            return 0;
        gl_texture_cache_entry_t *nv = (gl_texture_cache_entry_t *)realloc(
            ctx->texture_cache, (size_t)new_cap * sizeof(gl_texture_cache_entry_t));
        if (!nv)
            return 0;
        ctx->texture_cache = nv;
        memset(ctx->texture_cache + ctx->texture_cache_capacity,
               0,
               (size_t)(new_cap - ctx->texture_cache_capacity) * sizeof(*ctx->texture_cache));
        ctx->texture_cache_capacity = new_cap;
    }

    int32_t index = ctx->texture_cache_count++;
    gl_texture_cache_entry_t *entry = &ctx->texture_cache[index];
    gl_upload_status_t status;
    memset(entry, 0, sizeof(*entry));
    status = gl_start_native_texture_upload(ctx, entry, asset, cache_key, mip_start, mip_count);
    if (status == GL_UPLOAD_FAILED) {
        gl_texture_cache_assign_asset(entry, asset);
        gl_fail_texture_upload(entry, cache_key, mip_start, mip_count);
    }
    gl_texture_cache_remember(ctx, asset, index);
    if (status != GL_UPLOAD_COMPLETE) {
        ctx->stats.texture_fallback_binds++;
        return gl_fallback_white_texture(ctx);
    }
    return entry->tex;
}

/// @brief Resolve a material's texture to a GL texture, preferring native blocks then RGBA Pixels.
/// @param ctx Borrowed OpenGL context containing texture caches and backend capabilities.
/// @param asset Optional borrowed TextureAsset3D providing native compressed mips.
/// @param pixels Optional borrowed RGBA Pixels fallback source.
/// @param asset_cache_key Native source generation, or zero to query it from @p asset.
/// @param mip_start First resident native mip, or a negative value to query it from @p asset.
/// @param mip_count Resident native mip count, or a non-positive value to query it from @p asset.
/// @return The GL texture name to bind, or 0 if neither source is uploadable.
static GLuint gl_get_material_texture(gl_context_t *ctx,
                                      void *asset,
                                      const void *pixels,
                                      uint64_t asset_cache_key,
                                      int64_t mip_start,
                                      int64_t mip_count) {
    /* Native-supported assets stay on the native upload path. Falling back to
     * RGBA while native upload is budget-paused leaves abandoned pending bytes. */
    if (asset && vgfx3d_textureasset_native_supported(asset, gl_get_native_texture_caps(ctx)))
        return gl_get_cached_native_texture(ctx, asset, asset_cache_key, mip_start, mip_count);
    return pixels ? gl_get_cached_texture(ctx, pixels) : 0;
}

/// @brief Float max-LOD index for a cubemap's mip pyramid (parity with D3D11 version).
/// @details Counts `log2(face_size)` levels down to 1×1. Returned as a float because
///   GL's `TEXTURE_MAX_LOD` / `textureLod` uniforms take floats, and the returned value
///   feeds prefiltered-environment sampling where fractional LODs select a blend between
///   two adjacent mips for glossy reflection blur.
/// @param cubemap Borrowed cubemap whose base face extent determines the mip pyramid.
/// @return Maximum mip level as a float, or 0 when the cubemap is invalid or has a 1×1 face.
static float gl_cubemap_max_lod(const rt_cubemap3d *cubemap) {
    int32_t size;
    float lod = 0.0f;

    if (!cubemap || !vgfx3d_get_cubemap_face_size(cubemap, &size) || size <= 1)
        return 0.0f;
    while (size > 1) {
        size >>= 1;
        lod += 1.0f;
    }
    return lod;
}

/// @brief Validate and measure a cubemap's complete prefiltered IBL tail without decoding it.
/// @param cubemap Borrowed cubemap containing the candidate prefiltered IBL mip chain.
/// @param face_size Positive base cubemap face extent used to validate tail placement.
/// @param out_level_base Receives the first destination mip level; must be non-null.
/// @param out_bytes Receives total decoded RGBA upload bytes; must be non-null.
/// @return 1 when every face and mip has the expected layout and byte count, otherwise 0 with
///         outputs reset.
static int gl_measure_ibl_upload(const rt_cubemap3d *cubemap,
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
/// @param ctx Borrowed OpenGL context providing budget and upload accounting.
/// @param entry Mutable complete cubemap-cache entry receiving the IBL tail.
/// @param cubemap Borrowed cubemap containing the prefiltered IBL source data.
static void gl_apply_ibl_mips(gl_context_t *ctx,
                              gl_cubemap_cache_entry_t *entry,
                              const rt_cubemap3d *cubemap) {
    uint8_t *rgba[RT_CUBEMAP3D_IBL_MAX_MIPS][6] = {{NULL}};
    uint64_t upload_bytes;
    int32_t level_base;
    int upload_ok = 0;

    if (!ctx || !entry || !entry->tex || entry->upload_in_progress || !cubemap)
        return;
    if (!cubemap->ibl_ready || cubemap->ibl_identity == 0) {
        entry->pending_ibl_identity = 0;
        entry->pending_ibl_bytes = 0;
        return;
    }
    if (entry->failed_ibl_identity == cubemap->ibl_identity)
        return;
    if (entry->applied_ibl_identity == cubemap->ibl_identity) {
        entry->pending_ibl_identity = 0;
        entry->pending_ibl_bytes = 0;
        return;
    }
    if (!gl_measure_ibl_upload(cubemap, entry->face_size, &level_base, &upload_bytes)) {
        entry->failed_ibl_identity = cubemap->ibl_identity;
        entry->pending_ibl_identity = 0;
        entry->pending_ibl_bytes = 0;
        return;
    }
    entry->pending_ibl_identity = cubemap->ibl_identity;
    entry->pending_ibl_bytes = upload_bytes;
    if (!vgfx3d_upload_budget_allows(
            ctx->texture_upload_budget_bytes, ctx->texture_upload_bytes, upload_bytes) &&
        (ctx->texture_upload_budget_bytes == 0 || ctx->texture_upload_bytes != 0))
        return;

    for (int32_t mip = 0; mip < cubemap->ibl_mip_count; mip++) {
        int32_t expected_extent;
        if (!vgfx3d_expected_square_mip_extent(
                entry->face_size, level_base + mip, &expected_extent))
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

    gl.BindTexture(GL_TEXTURE_CUBE_MAP, entry->tex);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int32_t mip = 0; mip < cubemap->ibl_mip_count; mip++) {
        int32_t extent;
        if (!vgfx3d_expected_square_mip_extent(entry->face_size, level_base + mip, &extent))
            goto restore_unpack;
        for (int32_t face = 0; face < 6; face++) {
            gl.TexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)face,
                             level_base + mip,
                             0,
                             0,
                             extent,
                             extent,
                             GL_RGBA,
                             GL_UNSIGNED_BYTE,
                             rgba[mip][face]);
            if (!gl_upload_ok())
                goto restore_unpack;
        }
    }
    upload_ok = 1;

restore_unpack:
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
cleanup:
    for (int32_t mip = 0; mip < RT_CUBEMAP3D_IBL_MAX_MIPS; mip++)
        for (int32_t face = 0; face < 6; face++)
            free(rgba[mip][face]);
    if (upload_ok) {
        gl_record_texture_upload_bytes(ctx, upload_bytes);
        entry->applied_ibl_identity = cubemap->ibl_identity;
        entry->failed_ibl_identity = 0;
    } else {
        entry->failed_ibl_identity = cubemap->ibl_identity;
    }
    entry->pending_ibl_identity = 0;
    entry->pending_ibl_bytes = 0;
}

/// @brief Upload more of an in-progress cubemap (face by face, row band by row band) within budget.
/// @param ctx Borrowed OpenGL context providing the frame budget and upload accounting.
/// @param entry Mutable cubemap-cache entry containing destination and face/row cursors.
/// @param cubemap Borrowed cubemap whose base faces are being decoded and uploaded.
/// @return @c GL_UPLOAD_COMPLETE after all faces, mip generation, and eligible IBL work;
///         @c GL_UPLOAD_PENDING when rows remain; or @c GL_UPLOAD_FAILED on invalid data or GL
///         failure.
static gl_upload_status_t gl_continue_cubemap_upload(gl_context_t *ctx,
                                                     gl_cubemap_cache_entry_t *entry,
                                                     const rt_cubemap3d *cubemap) {
    if (!ctx || !entry || !cubemap || !entry->upload_in_progress || !entry->tex ||
        entry->face_size <= 0)
        return GL_UPLOAD_FAILED;
    if (entry->upload_face < 0 || entry->upload_face >= 6) {
        return GL_UPLOAD_FAILED;
    }
    if (entry->upload_next_row < 0)
        return GL_UPLOAD_FAILED;
    if (entry->upload_next_row >= entry->face_size) {
        entry->upload_face++;
        entry->upload_next_row = 0;
    }

    while (entry->upload_face < 6) {
        if (ctx->texture_upload_budget_bytes != UINT64_MAX &&
            ctx->texture_upload_bytes >= ctx->texture_upload_budget_bytes)
            return GL_UPLOAD_PENDING;
        int32_t rows = vgfx3d_upload_rows_for_budget(entry->face_size,
                                                     entry->face_size,
                                                     entry->upload_next_row,
                                                     ctx->texture_upload_budget_bytes,
                                                     ctx->texture_upload_bytes);
        int32_t slice_size = 0;
        int32_t slice_rows = 0;
        uint8_t *rgba = NULL;
        uint64_t bytes;

        if (rows <= 0)
            return GL_UPLOAD_FAILED;
        if (vgfx3d_unpack_cubemap_rgba_rows(cubemap,
                                            entry->upload_face,
                                            entry->upload_next_row,
                                            rows,
                                            1,
                                            &slice_size,
                                            &slice_rows,
                                            &rgba) != 0 ||
            !rgba || slice_size != entry->face_size || slice_rows <= 0) {
            free(rgba);
            return GL_UPLOAD_FAILED;
        }

        gl.BindTexture(GL_TEXTURE_CUBE_MAP, entry->tex);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.TexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)entry->upload_face,
                         0,
                         0,
                         entry->upload_next_row,
                         slice_size,
                         slice_rows,
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         rgba);
        if (!gl_upload_ok()) {
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
            free(rgba);
            return GL_UPLOAD_FAILED;
        }
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        bytes = (uint64_t)(uint32_t)slice_size * (uint64_t)(uint32_t)slice_rows * 4u;
        gl_record_texture_upload_bytes(ctx, bytes);
        free(rgba);

        entry->upload_next_row += slice_rows;
        entry->last_used_frame = ctx->frame_serial;
        if (entry->upload_next_row < entry->face_size)
            return GL_UPLOAD_PENDING;
        entry->upload_face++;
        entry->upload_next_row = 0;
    }

    gl.GenerateMipmap(GL_TEXTURE_CUBE_MAP);
    if (!gl_upload_ok())
        return GL_UPLOAD_FAILED;
    if (entry->fallback_tex) {
        gl.DeleteTextures(1, &entry->fallback_tex);
        entry->fallback_tex = 0;
    }
    entry->generation = entry->pending_generation;
    entry->pending_generation = 0;
    entry->upload_in_progress = 0;
    gl_apply_ibl_mips(ctx, entry, cubemap);
    return GL_UPLOAD_COMPLETE;
}

/// @brief Begin uploading a cubemap: create the GL cubemap texture and seed the face/row cursor.
/// @param ctx Borrowed OpenGL context providing limits, budget, and upload accounting.
/// @param entry Mutable cubemap-cache entry to initialize.
/// @param cubemap Borrowed cubemap source containing six base faces.
/// @param generation Source generation used to validate the completed cache entry.
/// @return @c GL_UPLOAD_COMPLETE when all faces finish immediately,
///         @c GL_UPLOAD_PENDING when initialized with work remaining, or
///         @c GL_UPLOAD_FAILED when validation, allocation, or upload fails.
static gl_upload_status_t gl_start_cubemap_upload(gl_context_t *ctx,
                                                  gl_cubemap_cache_entry_t *entry,
                                                  const rt_cubemap3d *cubemap,
                                                  uint64_t generation) {
    int32_t face_size = 0;

    if (!ctx || !entry || !cubemap || !vgfx3d_get_cubemap_face_size(cubemap, &face_size))
        return GL_UPLOAD_FAILED;
    if (ctx->max_texture_size > 0 && face_size > ctx->max_texture_size)
        return GL_UPLOAD_FAILED;
    if (entry->tex && entry->generation != 0 && entry->generation != generation) {
        if (entry->fallback_tex)
            gl.DeleteTextures(1, &entry->fallback_tex);
        entry->fallback_tex = entry->tex;
        entry->tex = 0;
    } else if (entry->tex && entry->upload_in_progress && entry->pending_generation != generation) {
        gl.DeleteTextures(1, &entry->tex);
        entry->tex = 0;
    }
    if (!entry->tex) {
        gl.GenTextures(1, &entry->tex);
        if (!entry->tex)
            return GL_UPLOAD_FAILED;
    }

    entry->cubemap = cubemap;
    entry->pending_generation = generation;
    entry->failed_generation = 0;
    entry->generation = 0;
    entry->face_size = face_size;
    entry->upload_face = 0;
    entry->upload_next_row = 0;
    entry->upload_in_progress = 1;
    entry->applied_ibl_identity = 0; /* base faces changed: re-overlay after upload */
    entry->failed_ibl_identity = 0;
    entry->pending_ibl_identity = 0;
    entry->pending_ibl_bytes = 0;
    entry->last_used_frame = ctx->frame_serial;

    gl.BindTexture(GL_TEXTURE_CUBE_MAP, entry->tex);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    for (int face = 0; face < 6; face++) {
        gl.TexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)face,
                      0,
                      GL_RGBA8,
                      face_size,
                      face_size,
                      0,
                      GL_RGBA,
                      GL_UNSIGNED_BYTE,
                      NULL);
    }
    if (!gl_upload_ok())
        return GL_UPLOAD_FAILED;
    return gl_continue_cubemap_upload(ctx, entry, cubemap);
}

/// @brief Cubemap-cache lookup with budgeted face-row upload.
/// @param ctx Borrowed OpenGL context containing the cubemap cache and upload budget.
/// @param cubemap Borrowed cubemap identity and upload source.
/// @return A complete cubemap or shared white fallback texture name, or zero for invalid input or
///         cache-growth failure.
static GLuint gl_get_cached_cubemap(gl_context_t *ctx, const rt_cubemap3d *cubemap) {
    uint64_t generation;
    gl_upload_status_t status;
    if (!ctx || !cubemap)
        return 0;
    generation = vgfx3d_get_cubemap_generation(cubemap);

    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        gl_cubemap_cache_entry_t *entry = &ctx->cubemap_cache[i];
        if (entry->cubemap == cubemap && entry->failed_generation == generation) {
            ctx->stats.texture_fallback_binds++;
            return entry->fallback_tex ? entry->fallback_tex : gl_fallback_white_cubemap(ctx);
        }
        if (entry->cubemap == cubemap && entry->generation == generation && entry->tex) {
            entry->last_used_frame = ctx->frame_serial;
            gl_apply_ibl_mips(ctx, entry, cubemap);
            return entry->tex;
        }
        if (entry->cubemap == cubemap && entry->pending_generation == generation &&
            entry->upload_in_progress) {
            entry->last_used_frame = ctx->frame_serial;
            status = gl_continue_cubemap_upload(ctx, entry, cubemap);
            if (status == GL_UPLOAD_COMPLETE)
                return entry->tex;
            if (status == GL_UPLOAD_FAILED)
                gl_fail_cubemap_upload(entry, generation);
            ctx->stats.texture_fallback_binds++;
            return entry->fallback_tex ? entry->fallback_tex : gl_fallback_white_cubemap(ctx);
        }
    }

    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        if (ctx->cubemap_cache[i].cubemap == cubemap) {
            status = gl_start_cubemap_upload(ctx, &ctx->cubemap_cache[i], cubemap, generation);
            if (status == GL_UPLOAD_FAILED)
                gl_fail_cubemap_upload(&ctx->cubemap_cache[i], generation);
            if (status == GL_UPLOAD_COMPLETE)
                return ctx->cubemap_cache[i].tex;
            ctx->stats.texture_fallback_binds++;
            return ctx->cubemap_cache[i].fallback_tex ? ctx->cubemap_cache[i].fallback_tex
                                                      : gl_fallback_white_cubemap(ctx);
        }
    }

    if (ctx->cubemap_cache_count >= ctx->cubemap_cache_capacity) {
        if (ctx->cubemap_cache_count >= INT_MAX)
            return 0;
        int32_t new_cap = vgfx3d_opengl_next_capacity(
            ctx->cubemap_cache_capacity, ctx->cubemap_cache_count + 1, 8);
        if (new_cap <= ctx->cubemap_cache_count ||
            (size_t)new_cap > SIZE_MAX / sizeof(gl_cubemap_cache_entry_t))
            return 0;
        gl_cubemap_cache_entry_t *nv = (gl_cubemap_cache_entry_t *)realloc(
            ctx->cubemap_cache, (size_t)new_cap * sizeof(gl_cubemap_cache_entry_t));
        if (!nv)
            return 0;
        ctx->cubemap_cache = nv;
        memset(ctx->cubemap_cache + ctx->cubemap_cache_capacity,
               0,
               (size_t)(new_cap - ctx->cubemap_cache_capacity) * sizeof(*ctx->cubemap_cache));
        ctx->cubemap_cache_capacity = new_cap;
    }

    gl_cubemap_cache_entry_t *entry = &ctx->cubemap_cache[ctx->cubemap_cache_count++];
    memset(entry, 0, sizeof(*entry));
    status = gl_start_cubemap_upload(ctx, entry, cubemap, generation);
    if (status == GL_UPLOAD_FAILED) {
        entry->cubemap = cubemap;
        gl_fail_cubemap_upload(entry, generation);
    }
    if (status == GL_UPLOAD_COMPLETE)
        return entry->tex;
    ctx->stats.texture_fallback_binds++;
    return gl_fallback_white_cubemap(ctx);
}
