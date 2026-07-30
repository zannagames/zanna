//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/world/rt_terrain3d_build.inc  (#included from rt_terrain3d.c; not a
// standalone translation unit) Purpose: Terrain3D construction, splat map/texture, and chunk
// surface/skirt building
//===----------------------------------------------------------------------===//
/**
 * @file rt_terrain3d_build.inc
 * @brief Implements Terrain3D construction, height queries, splat authoring, and chunk building.
 *
 * This implementation fragment is included by `rt_terrain3d.c` after the private payload and LOD
 * helpers are defined; it is not a standalone translation unit.
 */

/// @brief Construct a `width × depth` heightmap terrain (heights initially zero). Allocates
/// chunked mesh caches at three LOD levels plus per-chunk AABB storage. Default scale 1×1×1,
/// LOD switches at 100/250 world units, 2-unit crack-hiding skirts. Traps if dimensions are
/// outside [2, 4096] or on allocation failure.
/// @param width Number of height samples along the local x axis.
/// @param depth Number of height samples along the local z axis.
/// @return New GC-managed Terrain3D handle, or NULL after trapping on invalid dimensions or
/// allocation failure.
void *rt_terrain3d_new(int64_t width, int64_t depth) {
    size_t height_count;
    size_t chunk_count;
    size_t aabb_count;
    if (width < 2 || depth < 2 || width > TERRAIN3D_MAX_DIM || depth > TERRAIN3D_MAX_DIM) {
        rt_trap("Terrain3D.New: dimensions must be 2-4096");
        return NULL;
    }
    if ((uint64_t)width > SIZE_MAX / (uint64_t)depth / sizeof(float)) {
        rt_trap("Terrain3D.New: height allocation overflow");
        return NULL;
    }
    height_count = (size_t)width * (size_t)depth;
    rt_terrain3d *t =
        (rt_terrain3d *)rt_obj_new_i64(RT_G3D_TERRAIN3D_CLASS_ID, (int64_t)sizeof(rt_terrain3d));
    if (!t) {
        rt_trap("Terrain3D.New: allocation failed");
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->vptr = NULL;
    t->width = (int32_t)width;
    t->depth = (int32_t)depth;
    t->height_count = (int64_t)height_count;
    t->heights = (float *)calloc(height_count, sizeof(float));
    t->scale[0] = 1.0;
    t->scale[1] = 1.0;
    t->scale[2] = 1.0;
    t->chunks_x = ((int32_t)width - 1 + TERRAIN_CHUNK_SIZE - 1) / TERRAIN_CHUNK_SIZE;
    t->chunks_z = ((int32_t)depth - 1 + TERRAIN_CHUNK_SIZE - 1) / TERRAIN_CHUNK_SIZE;
    int32_t num_chunks = t->chunks_x * t->chunks_z;
    if (num_chunks <= 0 || (size_t)t->chunks_x > SIZE_MAX / (size_t)t->chunks_z ||
        (size_t)num_chunks > SIZE_MAX / 6u) {
        terrain3d_finalizer(t);
        if (rt_obj_release_check0(t))
            rt_obj_free(t);
        rt_trap("Terrain3D.New: chunk allocation overflow");
        return NULL;
    }
    t->chunk_capacity = num_chunks;
    chunk_count = (size_t)num_chunks;
    aabb_count = chunk_count * 6u;
    t->chunk_meshes = (void **)calloc(chunk_count, sizeof(void *));
    t->chunk_meshes_lod1 = (void **)calloc(chunk_count, sizeof(void *));
    t->chunk_meshes_lod2 = (void **)calloc(chunk_count, sizeof(void *));
    t->chunk_aabbs = (float *)calloc(aabb_count, sizeof(float));
    t->chunk_aabbs_lod1 = (float *)calloc(aabb_count, sizeof(float));
    t->chunk_aabbs_lod2 = (float *)calloc(aabb_count, sizeof(float));
    t->chunk_aabbs_union = (float *)calloc(aabb_count, sizeof(float));
    t->chunk_lod_state = (uint8_t *)malloc(chunk_count);
    if (!t->heights || !t->chunk_meshes || !t->chunk_meshes_lod1 || !t->chunk_meshes_lod2 ||
        !t->chunk_aabbs || !t->chunk_aabbs_lod1 || !t->chunk_aabbs_lod2 || !t->chunk_aabbs_union ||
        !t->chunk_lod_state) {
        terrain3d_finalizer(t);
        if (rt_obj_release_check0(t))
            rt_obj_free(t);
        rt_trap("Terrain3D.New: allocation failed");
        return NULL;
    }
    memset(t->chunk_lod_state, 0xFF, chunk_count);
    t->lod_dist1 = 100.0f;
    t->lod_dist2 = 250.0f;
    t->lod_hysteresis = 12.0f;
    t->skirt_depth = 2.0f;
    t->lod_prewarm_dirty = 1;
    t->material = NULL;
    t->splat_map = NULL;
    t->base_texture = NULL;
    t->baked_texture = NULL;
    t->splat_dirty = 0;
    t->cpu_occlusion_enabled = 0;
    for (int i = 0; i < TERRAIN_MAX_SPLAT_LAYERS; i++) {
        t->layer_textures[i] = NULL;
        t->layer_scales[i] = 1.0;
    }
    rt_obj_set_finalizer(t, terrain3d_finalizer);
    return t;
}

/// @brief Generate terrain heights directly from Perlin noise (fast native path).
/// Bypasses the Pixels intermediate — writes directly to float heightmap.
/// @param obj Perlin runtime object.
/// @param x First noise-space coordinate.
/// @param y Second noise-space coordinate.
/// @param octaves Number of fractal noise octaves.
/// @param persistence Amplitude multiplier between octaves.
/// @return Fractal Perlin noise sample.
extern double rt_perlin_octave2d(
    void *obj, double x, double y, int64_t octaves, double persistence);

/// @brief Fill heights directly from a Perlin noise object (octave fractal sum), bypassing the
/// Pixels intermediate. Output values are mapped from [-1, 1] to [0, 1]. `scale` controls the
/// noise frequency (higher = more detail). Invalidates all cached chunk meshes.
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param perlin Perlin generator used to sample the grid.
/// @param scale Noise-coordinate frequency scale; invalid or near-zero values become 1.
/// @param octaves Fractal octave count, clamped to `[1, 16]`.
/// @param persistence Per-octave amplitude multiplier, clamped to `[0, 1]`.
void rt_terrain3d_generate_perlin(
    void *obj, void *perlin, double scale, int64_t octaves, double persistence) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t || !perlin || !terrain_has_valid_grid(t))
        return;
    if (!isfinite(scale) || scale < 1e-8)
        scale = 1.0;
    if (octaves < 1)
        octaves = 1;
    if (octaves > 16)
        octaves = 16;
    if (!isfinite(persistence))
        persistence = 0.5;
    if (persistence < 0.0)
        persistence = 0.0;
    if (persistence > 1.0)
        persistence = 1.0;

    for (int32_t z = 0; z < t->depth; z++) {
        for (int32_t x = 0; x < t->width; x++) {
            double nx = (double)x * scale / (double)t->width;
            double nz = (double)z * scale / (double)t->depth;
            double h = rt_perlin_octave2d(perlin, nx, nz, octaves, persistence);
            if (!isfinite(h))
                h = 0.0;
            /* Map [-1, 1] -> [0, 1] and clamp malformed generators. */
            h = (h + 1.0) * 0.5;
            if (h < 0.0)
                h = 0.0;
            if (h > 1.0)
                h = 1.0;
            t->heights[z * t->width + x] = (float)h;
        }
    }

    invalidate_all_chunks(t);
}

/// @brief Resample a Pixels heightmap into the terrain's height grid. Reads 16-bit precision per
/// sample (R = high byte, G = low byte) for smooth gradients without staircasing. Source pixels
/// are nearest-neighbor sampled to the terrain resolution. Invalidates all cached chunk meshes.
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param pixels Nonempty Pixels heightmap encoded with red as the high byte and green as the low
/// byte.
void rt_terrain3d_set_heightmap(void *obj, void *pixels) {
    if (!obj || !pixels)
        return;
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t || !terrain_has_valid_grid(t))
        return;

    rt_pixels_impl *pv = rt_pixels_checked_impl(pixels, "Terrain3D.SetHeightmap: expected Pixels");
    if (!pv || !pv->data || pv->width <= 0 || pv->height <= 0 || pv->width > INT32_MAX ||
        pv->height > INT32_MAX) {
        rt_trap("Terrain3D.SetHeightmap: heightmap must be non-empty Pixels");
        return;
    }

    int32_t sw = (int32_t)pv->width, sh = (int32_t)pv->height;
    for (int32_t z = 0; z < t->depth; z++) {
        int32_t sz = terrain_resample_index(z, t->depth, sh);
        for (int32_t x = 0; x < t->width; x++) {
            int32_t sx = terrain_resample_index(x, t->width, sw);
            uint32_t pixel = pv->data[(int64_t)sz * sw + sx]; /* 0xRRGGBBAA */
            /* 16-bit height from R (high byte) + G (low byte) for smooth terrain */
            uint32_t hi = (pixel >> 24) & 0xFF;
            uint32_t lo = (pixel >> 16) & 0xFF;
            t->heights[(int64_t)z * t->width + x] = (float)((hi << 8) | lo) / 65535.0f;
        }
    }

    invalidate_all_chunks(t);
}

/// @brief Attach a Material3D used when rendering chunks. Required before draw — the chunks are
/// rendered with this material, optionally overridden by the splat-bake texture if a splat map
/// is set. Does not invalidate chunks (the mesh data is independent of the material).
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param material Material3D handle to retain, or NULL to clear the binding.
void rt_terrain3d_set_material(void *obj, void *material) {
    if (!obj)
        return;
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return;
    terrain_repair_material_slot(&t->material);
    if (material && !rt_g3d_has_class(material, RT_G3D_MATERIAL3D_CLASS_ID))
        return;
    if (t->material == material)
        return;
    if (t->material && (t->base_texture || t->baked_texture || t->splat_map))
        terrain_restore_material_base_texture(t);
    terrain_assign_material_ref(&t->material, material);
    terrain_release_texture_slot(&t->base_texture);
    terrain_release_texture_slot(&t->baked_texture);
    t->splat_dirty = t->splat_map ? 1 : 0;
}

/// @brief Set per-axis world-space scale: `sx` and `sz` are grid-cell spacing in world units;
/// `sy` is the height multiplier (heights in [0,1] become world-Y in [0, sy]). Invalidates
/// cached chunks since vertex world positions change.
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param sx Positive x-axis sample spacing; invalid input becomes 1.
/// @param sy Signed height multiplier; non-finite input becomes 1.
/// @param sz Positive z-axis sample spacing; invalid input becomes 1.
void rt_terrain3d_set_scale(void *obj, double sx, double sy, double sz) {
    if (!obj)
        return;
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return;
    if (!isfinite(sx) || sx <= 0.0)
        sx = 1.0;
    if (!isfinite(sy))
        sy = 1.0;
    if (!isfinite(sz) || sz <= 0.0)
        sz = 1.0;
    sx = terrain_clamp_abs_or(sx, 1.0);
    sy = terrain_clamp_abs_or(sy, 1.0);
    sz = terrain_clamp_abs_or(sz, 1.0);
    if (t->scale[0] == sx && t->scale[1] == sy && t->scale[2] == sz)
        return;
    t->scale[0] = sx;
    t->scale[1] = sy;
    t->scale[2] = sz;
    invalidate_all_chunks(t);
}

/// @brief Attach a splat-weight Pixels map. Each pixel's RGBA channels are the per-texel weights
/// (normalized at bake time) for layers 0..3. Marks the splat bake dirty; mesh geometry is
/// independent of the splat data and remains cached.
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param pixels Nonempty RGBA Pixels weight map, or NULL to clear the first bank.
void rt_terrain3d_set_splat_map(void *obj, void *pixels) {
    if (!obj)
        return;
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return;
    terrain_repair_texture_slot(&t->splat_map);
    if (pixels) {
        rt_pixels_impl *p =
            rt_pixels_checked_impl(pixels, "Terrain3D.SetSplatMap: expected Pixels");
        if (!p || p->width <= 0 || p->height <= 0 || p->width > INT32_MAX ||
            p->height > INT32_MAX || !p->data) {
            rt_trap("Terrain3D.SetSplatMap: splat map must be non-empty Pixels");
            return;
        }
    }
    terrain_assign_texture_ref(&t->splat_map, pixels);
    if (!pixels) {
        void *base_texture =
            terrain_texture_ref_supported(t->base_texture) ? t->base_texture : NULL;
        if (!base_texture)
            terrain_release_texture_slot(&t->base_texture);
        terrain_restore_material_base_texture(t);
        terrain_release_texture_slot(&t->baked_texture);
        t->splat_dirty = 0;
    } else {
        t->splat_dirty = 1;
    }
}

/// @brief Set the texture for a splat layer (0..7). Layer index outside that range is silently
/// ignored. Marks the splat bake dirty without invalidating geometry caches.
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param layer Layer index from 0 through 7.
/// @param pixels Pixels or TextureAsset3D handle to retain, or NULL to clear the layer.
void rt_terrain3d_set_layer_texture(void *obj, int64_t layer, void *pixels) {
    if (!obj || layer < 0 || layer >= TERRAIN_MAX_SPLAT_LAYERS)
        return;
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return;
    terrain_repair_texture_slot(&t->layer_textures[layer]);
    if (pixels && rt_g3d_has_class(pixels, RT_G3D_TEXTUREASSET3D_CLASS_ID)) {
        void *resolved_pixels = rt_material3d_resolve_texture_pixels(pixels);
        rt_pixels_impl *p = rt_pixels_checked_impl_or_null(resolved_pixels);
        if (!p || p->width <= 0 || p->height <= 0 || p->width > INT32_MAX ||
            p->height > INT32_MAX || !p->data) {
            rt_trap("Terrain3D.SetLayerTexture: TextureAsset3D splat layers must expose a "
                    "non-empty RGBA8 Pixels fallback");
            return;
        }
    } else if (pixels) {
        rt_pixels_impl *p =
            rt_pixels_checked_impl(pixels, "Terrain3D.SetLayerTexture: expected Pixels");
        if (!p || p->width <= 0 || p->height <= 0 || p->width > INT32_MAX ||
            p->height > INT32_MAX || !p->data) {
            rt_trap("Terrain3D.SetLayerTexture: layer texture must be non-empty Pixels or "
                    "TextureAsset3D");
            return;
        }
    }
    terrain_assign_texture_ref(&t->layer_textures[layer], pixels);
    t->splat_dirty = 1;
}

/// @brief Set UV-tiling scale for splat layer N. Higher values pack the layer texture into
/// smaller tiles (more repetitions across the terrain). Marks the splat bake dirty when changed.
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param layer Layer index from 0 through 7.
/// @param scale Positive finite UV tiling multiplier.
void rt_terrain3d_set_layer_scale(void *obj, int64_t layer, double scale) {
    if (!obj || layer < 0 || layer >= TERRAIN_MAX_SPLAT_LAYERS)
        return;
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return;
    if (!isfinite(scale) || scale <= 0.0)
        scale = 1.0;
    if (scale > TERRAIN3D_ABS_MAX)
        scale = TERRAIN3D_ABS_MAX;
    if (t->layer_scales[layer] == scale)
        return;
    t->layer_scales[layer] = scale;
    t->splat_dirty = 1;
}

/*==========================================================================
 * Readback symmetry (ADR 0227): every setter over retained state has a
 * matching reader so authored terrain can be inspected, not just replaced.
 *=========================================================================*/

/// @brief Read the per-axis terrain scale (x spacing, height scale, z spacing).
/// @param obj Terrain3D handle.
/// @return New Vec3 of the retained scale, or unit scale for invalid handles.
void *rt_terrain3d_get_scale(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return rt_vec3_new(1.0, 1.0, 1.0);
    return rt_vec3_new(t->scale[0], t->scale[1], t->scale[2]);
}

/// @brief Read the retained terrain material.
/// @param obj Terrain3D handle.
/// @return Borrowed Material3D handle, or `NULL`.
void *rt_terrain3d_get_material(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    return t ? t->material : NULL;
}

/// @brief Read the retained texture reference of one splat layer.
/// @param obj Terrain3D handle.
/// @param layer Layer index from 0 through 7.
/// @return Borrowed Pixels/TextureAsset3D handle, or `NULL`.
void *rt_terrain3d_get_layer_texture(void *obj, int64_t layer) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t || layer < 0 || layer >= TERRAIN_MAX_SPLAT_LAYERS)
        return NULL;
    return t->layer_textures[layer];
}

/// @brief Read one splat layer's UV tiling multiplier.
/// @param obj Terrain3D handle.
/// @param layer Layer index from 0 through 7.
/// @return Retained tiling multiplier, or 1.0 for invalid input.
double rt_terrain3d_get_layer_scale(void *obj, int64_t layer) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t || layer < 0 || layer >= TERRAIN_MAX_SPLAT_LAYERS)
        return 1.0;
    return t->layer_scales[layer];
}

/// @brief Read one retained splat weight map (0 weights layers 0-3, 1 weights 4-7).
/// @param obj Terrain3D handle.
/// @param index Splat map index, 0 or 1.
/// @return Borrowed Pixels handle, or `NULL`.
void *rt_terrain3d_get_splat_map(void *obj, int64_t index) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t || index < 0 || index > 1)
        return NULL;
    return index == 0 ? t->splat_map : t->splat_map2;
}

/// @brief Read the LOD 1 switch distance.
/// @param obj Terrain3D handle.
/// @return Retained distance, or 0.0 for invalid handles.
double rt_terrain3d_get_lod_near_distance(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    return t ? (double)t->lod_dist1 : 0.0;
}

/// @brief Read the LOD 2 switch distance.
/// @param obj Terrain3D handle.
/// @return Retained distance, or 0.0 for invalid handles.
double rt_terrain3d_get_lod_far_distance(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    return t ? (double)t->lod_dist2 : 0.0;
}

/// @brief Read the LOD hysteresis band.
/// @param obj Terrain3D handle.
/// @return Retained band distance, or 0.0 for invalid handles.
double rt_terrain3d_get_lod_hysteresis(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    return t ? (double)t->lod_hysteresis : 0.0;
}

/// @brief Read the crack-hiding skirt depth (0 = disabled).
/// @param obj Terrain3D handle.
/// @return Retained skirt depth, or 0.0 for invalid handles.
double rt_terrain3d_get_skirt_depth(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    return t ? (double)t->skirt_depth : 0.0;
}

/// @brief Read the heightmap sample width set by SetHeightmap.
/// @param obj Terrain3D handle.
/// @return Sample columns, or 0 for invalid handles.
int64_t rt_terrain3d_get_heightmap_width(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    return t ? (int64_t)t->width : 0;
}

/// @brief Read the heightmap sample depth set by SetHeightmap.
/// @param obj Terrain3D handle.
/// @return Sample rows, or 0 for invalid handles.
int64_t rt_terrain3d_get_heightmap_depth(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    return t ? (int64_t)t->depth : 0;
}

/// @brief Shared bounds-checked accessor for one authored hole component.
/// @param obj Terrain3D handle.
/// @param index Zero-based hole index.
/// @param component Component slot: 0=x, 1=z, 2=width, 3=depth.
/// @return The retained component, or 0.0 for invalid input.
static double terrain3d_hole_component(void *obj, int64_t index, int component) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t || !t->holes || index < 0 || index >= t->hole_count)
        return 0.0;
    return t->holes[index][component];
}

/// @brief Read one authored hole's terrain-local X origin.
/// @param obj Terrain3D handle.
/// @param index Zero-based hole index.
/// @return Retained X origin, or 0.0 for invalid input.
double rt_terrain3d_get_hole_x(void *obj, int64_t index) {
    return terrain3d_hole_component(obj, index, 0);
}

/// @brief Read one authored hole's terrain-local Z origin.
/// @param obj Terrain3D handle.
/// @param index Zero-based hole index.
/// @return Retained Z origin, or 0.0 for invalid input.
double rt_terrain3d_get_hole_z(void *obj, int64_t index) {
    return terrain3d_hole_component(obj, index, 1);
}

/// @brief Read one authored hole's width.
/// @param obj Terrain3D handle.
/// @param index Zero-based hole index.
/// @return Retained width, or 0.0 for invalid input.
double rt_terrain3d_get_hole_width(void *obj, int64_t index) {
    return terrain3d_hole_component(obj, index, 2);
}

/// @brief Read one authored hole's depth (Z extent).
/// @param obj Terrain3D handle.
/// @param index Zero-based hole index.
/// @return Retained depth, or 0.0 for invalid input.
double rt_terrain3d_get_hole_depth(void *obj, int64_t index) {
    return terrain3d_hole_component(obj, index, 3);
}

/// @brief Resolve a texture reference to its underlying Pixels and return it only when that is
///   a valid non-empty image, else NULL.
/// @param pixels Pixels or TextureAsset3D reference to resolve.
/// @return Borrowed nonempty Pixels handle, or NULL when resolution or validation fails.
static void *terrain_valid_pixels_or_null(void *pixels) {
    rt_pixels_impl *p;
    if (!pixels)
        return NULL;
    pixels = rt_material3d_resolve_texture_pixels(pixels);
    p = rt_pixels_checked_impl_or_null(pixels);
    if (!p || !p->data || p->width <= 0 || p->height <= 0 || p->width > INT32_MAX ||
        p->height > INT32_MAX)
        return NULL;
    return pixels;
}

/// @brief Return @p pixels only when it is a valid non-empty Pixels image (splat maps use raw
///   Pixels rather than resolved texture references), else NULL.
/// @param pixels Candidate raw splat-map Pixels handle.
/// @return Borrowed nonempty Pixels handle, or NULL when invalid.
static void *terrain_valid_splat_pixels_or_null(void *pixels) {
    rt_pixels_impl *p;
    if (!pixels)
        return NULL;
    p = rt_pixels_checked_impl_or_null(pixels);
    if (!p || !p->data || p->width <= 0 || p->height <= 0 || p->width > INT32_MAX ||
        p->height > INT32_MAX)
        return NULL;
    return pixels;
}

/// @brief Reset the terrain's splat state: release/clear the splat map, restore the base-texture
///   material binding, drop the baked splat texture, and clear the splat-dirty flag.
/// @param t Terrain whose invalid splat state is discarded.
static void terrain_clear_invalid_splat_state(rt_terrain3d *t) {
    if (!t)
        return;
    if (rt_pixels_checked_impl_or_null(t->splat_map))
        terrain_release_texture_slot(&t->splat_map);
    else
        t->splat_map = NULL;
    terrain_restore_material_base_texture(t);
    terrain_release_texture_slot(&t->baked_texture);
    t->splat_dirty = 0;
}

/// @brief Drop stale CPU splat-bake state when the complete real-time splat path can render.
/// @details A terrain with a valid control map and all four layer textures no longer needs a
///          pre-blended material texture. Restoring the base texture avoids leaving an old baked
///          fallback bound after authors repair a previously incomplete splat setup.
/// @param t Terrain switching from the baked fallback to real-time splat rendering.
static void terrain_use_realtime_splat_state(rt_terrain3d *t) {
    if (!t)
        return;
    if (t->baked_texture) {
        terrain_restore_material_base_texture(t);
        terrain_release_texture_slot(&t->baked_texture);
    }
    t->splat_dirty = 0;
}

/// @brief Sample a pixel from a texture reference at UV coordinates (wrapped).
/// @param texture_ref Pixels or TextureAsset3D reference to sample.
/// @param u Horizontal texture coordinate, wrapped into the image width.
/// @param v Vertical texture coordinate, wrapped into the image height.
/// @return Packed `0xRRGGBBAA` texel, or opaque white when the texture is invalid.
static uint32_t sample_pixels_uv(void *texture_ref, double u, double v) {
    void *pixels;
    if (!texture_ref)
        return 0xFFFFFFFF;

    pixels = rt_material3d_resolve_texture_pixels(texture_ref);
    rt_pixels_impl *pv = rt_pixels_checked_impl_or_null(pixels);
    if (!pv || !pv->data || pv->width <= 0 || pv->height <= 0 || pv->width > INT32_MAX ||
        pv->height > INT32_MAX)
        return 0xFFFFFFFF;
    if (!isfinite(u))
        u = 0.0;
    if (!isfinite(v))
        v = 0.0;
    /* Wrap UV */
    u = u - floor(u);
    v = v - floor(v);
    int64_t px = (int64_t)(u * (double)pv->width);
    int64_t py = (int64_t)(v * (double)pv->height);
    if (px < 0)
        px = 0;
    if (py < 0)
        py = 0;
    if (px >= pv->width)
        px = pv->width - 1;
    if (py >= pv->height)
        py = pv->height - 1;
    return pv->data[py * pv->width + px];
}

/// @brief Convert a normalized floating color channel to 8-bit with finite lower/upper clamps.
/// @param value Candidate normalized color component.
/// @return Integer channel value clamped to `[0, 255]`.
static int32_t terrain_channel_to_u8(double value) {
    if (!isfinite(value) || value < 0.0)
        value = 0.0;
    if (value > 1.0)
        value = 1.0;
    return (int32_t)(value * 255.0 + 0.5);
}

/// @brief Pack the terrain material's diffuse RGB + alpha into a single 0xRRGGBBAA pixel
///   (opaque white when there is no valid material) — the fallback color for splat baking.
/// @param t Terrain whose material supplies the fallback modulation color.
/// @return Packed diffuse material color, or opaque white when no valid material exists.
static uint32_t terrain_material_fallback_pixel(const rt_terrain3d *t) {
    if (!t || !t->material || !rt_g3d_has_class(t->material, RT_G3D_MATERIAL3D_CLASS_ID))
        return 0xFFFFFFFFu;
    const rt_material3d *mat = (const rt_material3d *)t->material;
    uint32_t r = (uint32_t)terrain_channel_to_u8(mat->diffuse[0]);
    uint32_t g = (uint32_t)terrain_channel_to_u8(mat->diffuse[1]);
    uint32_t b = (uint32_t)terrain_channel_to_u8(mat->diffuse[2]);
    uint32_t a = (uint32_t)terrain_channel_to_u8(mat->alpha);
    return (r << 24) | (g << 16) | (b << 8) | a;
}

/// @brief Sample height at grid coordinates (clamped).
/// @param t Terrain containing the height grid.
/// @param x Grid x coordinate, clamped to the nearest sample.
/// @param z Grid z coordinate, clamped to the nearest sample.
/// @return Finite normalized height sample, or zero for invalid terrain state.
static float sample_height(const rt_terrain3d *t, int32_t x, int32_t z) {
    if (!terrain_has_valid_grid(t))
        return 0.0f;
    if (x < 0)
        x = 0;
    if (z < 0)
        z = 0;
    if (x >= t->width)
        x = t->width - 1;
    if (z >= t->depth)
        z = t->depth - 1;
    return terrain_sanitize_height_sample(t->heights[z * t->width + x]);
}

/// @brief Number of samples that lie on a terrain edge.
/// @param t Terrain containing the height grid.
/// @param edge Edge identifier from `RT_TERRAIN3D_EDGE_*`.
/// @return Sample count along the selected edge, or zero for invalid input.
static int32_t terrain_edge_sample_count(const rt_terrain3d *t, int64_t edge) {
    if (!terrain_has_valid_grid(t))
        return 0;
    switch (edge) {
        case RT_TERRAIN3D_EDGE_WEST:
        case RT_TERRAIN3D_EDGE_EAST:
            return t->depth;
        case RT_TERRAIN3D_EDGE_NORTH:
        case RT_TERRAIN3D_EDGE_SOUTH:
            return t->width;
        default:
            return 0;
    }
}

/// @brief Return a mutable height sample slot on the requested edge.
/// @param t Terrain containing the height grid.
/// @param edge Edge identifier from `RT_TERRAIN3D_EDGE_*`.
/// @param sample Zero-based sample index along that edge.
/// @return Pointer to the mutable height sample, or NULL for invalid input.
static float *terrain_edge_height_slot(rt_terrain3d *t, int64_t edge, int32_t sample) {
    if (!terrain_has_valid_grid(t))
        return NULL;
    switch (edge) {
        case RT_TERRAIN3D_EDGE_WEST:
            if (sample < 0 || sample >= t->depth)
                return NULL;
            return &t->heights[(int64_t)sample * t->width];
        case RT_TERRAIN3D_EDGE_EAST:
            if (sample < 0 || sample >= t->depth)
                return NULL;
            return &t->heights[(int64_t)sample * t->width + (t->width - 1)];
        case RT_TERRAIN3D_EDGE_NORTH:
            if (sample < 0 || sample >= t->width)
                return NULL;
            return &t->heights[sample];
        case RT_TERRAIN3D_EDGE_SOUTH:
            if (sample < 0 || sample >= t->width)
                return NULL;
            return &t->heights[(int64_t)(t->depth - 1) * t->width + sample];
        default:
            return NULL;
    }
}

/// @brief Convert a normalized height sample into world-Y units.
/// @param t Terrain supplying the y-axis height scale.
/// @param h Normalized height sample.
/// @return Finite world-space height, clamped to the terrain safety range.
static double terrain_sample_to_world_height(const rt_terrain3d *t, float h) {
    double sy = terrain_scale_axis_or(t, 1, 1.0);
    return (double)terrain_sanitize_height_sample(h) * sy;
}

/// @brief Compute the terrain surface normal at grid cell (@p ix, @p iz) from neighboring height
///   samples via central differences scaled by the world cell size, writing the unit normal to
///   out_nx/out_ny/out_nz.
/// @param t Terrain containing the height grid.
/// @param ix Grid x coordinate.
/// @param iz Grid z coordinate.
/// @param sx Positive world-space x sample spacing.
/// @param sy World-space height multiplier.
/// @param sz Positive world-space z sample spacing.
/// @param out_nx Output receiving the normal x component.
/// @param out_ny Output receiving the normal y component.
/// @param out_nz Output receiving the normal z component.
static void terrain_grid_normal_at(const rt_terrain3d *t,
                                   int32_t ix,
                                   int32_t iz,
                                   double sx,
                                   double sy,
                                   double sz,
                                   double *out_nx,
                                   double *out_ny,
                                   double *out_nz) {
    int32_t xl;
    int32_t xr;
    int32_t zd;
    int32_t zu;
    double span_x;
    double span_z;
    double nx;
    double ny;
    double nz;
    double len;
    float hL;
    float hR;
    float hD;
    float hU;
    if (!out_nx || !out_ny || !out_nz)
        return;
    *out_nx = 0.0;
    *out_ny = 1.0;
    *out_nz = 0.0;
    if (!terrain_has_valid_grid(t))
        return;

    ix = terrain_coord_to_index((double)ix, t->width - 1);
    iz = terrain_coord_to_index((double)iz, t->depth - 1);
    xl = ix > 0 ? ix - 1 : ix;
    xr = ix < t->width - 1 ? ix + 1 : ix;
    zd = iz > 0 ? iz - 1 : iz;
    zu = iz < t->depth - 1 ? iz + 1 : iz;
    span_x = (double)(xr - xl) * sx;
    span_z = (double)(zu - zd) * sz;
    if (!isfinite(span_x) || !isfinite(span_z) || fabs(span_x) < 1e-12 || fabs(span_z) < 1e-12)
        return;

    hL = sample_height(t, xl, iz);
    hR = sample_height(t, xr, iz);
    hD = sample_height(t, ix, zd);
    hU = sample_height(t, ix, zu);

    nx = (double)(hL - hR) * sy * span_z;
    nz = (double)(hD - hU) * sy * span_x;
    ny = span_x * span_z;
    len = hypot(hypot(nx, ny), nz);
    if (isfinite(len) && len > 1e-8) {
        *out_nx = nx / len;
        *out_ny = ny / len;
        *out_nz = nz / len;
    }
}

/// @brief Convert a world-Y height back into a terrain height sample.
/// @param t Terrain supplying the y-axis height scale.
/// @param h World-space height.
/// @return Finite normalized sample representation, clamped to the terrain safety range.
static float terrain_world_height_to_sample(const rt_terrain3d *t, double h) {
    double sy = terrain_scale_axis_or(t, 1, 1.0);
    double sample = h / sy;
    if (!isfinite(sample))
        sample = 0.0;
    if (sample > TERRAIN3D_ABS_MAX)
        sample = TERRAIN3D_ABS_MAX;
    if (sample < -TERRAIN3D_ABS_MAX)
        sample = -TERRAIN3D_ABS_MAX;
    return (float)sample;
}

/// @brief Treat sub-ULP seam deltas as unchanged so stitching does not invalidate terrain
///   chunks forever on float round-off noise.
/// @param a First height sample.
/// @param b Second height sample.
/// @return Nonzero when samples are equal within a scale-aware float epsilon.
static int terrain_height_samples_equal_enough(float a, float b) {
    double da;
    double db;
    double scale;
    if (a == b)
        return 1;
    if (!isfinite(a) || !isfinite(b))
        return 0;
    da = (double)a;
    db = (double)b;
    scale = fmax(fmax(fabs(da), fabs(db)), 1.0);
    return fabs(da - db) <= (double)FLT_EPSILON * 4.0 * scale;
}

/// @brief Average two border edges in world-height space and invalidate affected LOD meshes.
/// @details Used by WorldStream3D after adjacent terrain tiles become resident. The helper maps
///   differing edge sample counts by normalized edge distance so a coarse tile can stitch to a
///   denser neighbor without relying on visual skirts to cover the shared border.
/// @param terrain_obj Terrain whose selected border is modified.
/// @param edge Edge identifier on @p terrain_obj.
/// @param neighbor_obj Adjacent Terrain3D object modified symmetrically.
/// @param neighbor_edge Edge identifier on @p neighbor_obj.
/// @return Number of paired samples changed, or zero for invalid input or an already seamless edge.
int64_t rt_terrain3d_stitch_edge(void *terrain_obj,
                                 int64_t edge,
                                 void *neighbor_obj,
                                 int64_t neighbor_edge) {
    rt_terrain3d *terrain =
        (rt_terrain3d *)rt_g3d_checked_or_null(terrain_obj, RT_G3D_TERRAIN3D_CLASS_ID);
    rt_terrain3d *neighbor =
        (rt_terrain3d *)rt_g3d_checked_or_null(neighbor_obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!terrain || !neighbor || terrain == neighbor)
        return 0;
    int32_t terrain_count = terrain_edge_sample_count(terrain, edge);
    int32_t neighbor_count = terrain_edge_sample_count(neighbor, neighbor_edge);
    if (terrain_count <= 0 || neighbor_count <= 0)
        return 0;
    int32_t stitch_count = terrain_count > neighbor_count ? terrain_count : neighbor_count;
    if (stitch_count <= 0)
        return 0;

    int64_t changed = 0;
    for (int32_t i = 0; i < stitch_count; ++i) {
        double u = stitch_count > 1 ? (double)i / (double)(stitch_count - 1) : 0.0;
        int32_t terrain_i =
            terrain_count > 1 ? (int32_t)llround(u * (double)(terrain_count - 1)) : 0;
        int32_t neighbor_i =
            neighbor_count > 1 ? (int32_t)llround(u * (double)(neighbor_count - 1)) : 0;
        float *terrain_h = terrain_edge_height_slot(terrain, edge, terrain_i);
        float *neighbor_h = terrain_edge_height_slot(neighbor, neighbor_edge, neighbor_i);
        if (!terrain_h || !neighbor_h)
            continue;
        double world_h = (terrain_sample_to_world_height(terrain, *terrain_h) +
                          terrain_sample_to_world_height(neighbor, *neighbor_h)) *
                         0.5;
        float stitched_terrain = terrain_world_height_to_sample(terrain, world_h);
        float stitched_neighbor = terrain_world_height_to_sample(neighbor, world_h);
        if (!terrain_height_samples_equal_enough(*terrain_h, stitched_terrain) ||
            !terrain_height_samples_equal_enough(*neighbor_h, stitched_neighbor)) {
            *terrain_h = stitched_terrain;
            *neighbor_h = stitched_neighbor;
            changed++;
        }
    }
    if (changed > 0) {
        invalidate_all_chunks(terrain);
        invalidate_all_chunks(neighbor);
    }
    return changed;
}

/// @brief Build a grayscale Pixels heightmap from the terrain's current height samples.
/// @param terrain_obj Terrain3D handle to serialize.
/// @return New Pixels object encoded with 16-bit red/green heights, or NULL on invalid input or
/// allocation failure.
void *rt_terrain3d_build_heightmap_pixels(void *terrain_obj) {
    rt_terrain3d *t =
        (rt_terrain3d *)rt_g3d_checked_or_null(terrain_obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!terrain_has_valid_grid(t))
        return NULL;
    void *pixels = rt_pixels_new(t->width, t->depth);
    if (!pixels)
        return NULL;
    for (int32_t z = 0; z < t->depth; ++z) {
        for (int32_t x = 0; x < t->width; ++x) {
            double h = (double)t->heights[(int64_t)z * t->width + x];
            if (!isfinite(h))
                h = 0.0;
            if (h < 0.0)
                h = 0.0;
            if (h > 1.0)
                h = 1.0;
            uint32_t sample = (uint32_t)llround(h * 65535.0);
            if (sample > 65535u)
                sample = 65535u;
            uint32_t rgba = ((sample >> 8) & 0xFFu) << 24 | (sample & 0xFFu) << 16 | 0x000000FFu;
            rt_pixels_set(pixels, x, z, (int64_t)rgba);
        }
    }
    return pixels;
}

/// @brief Bilinearly sample the terrain height at world-space (wx, wz). Coordinates outside the
/// grid are clamped to the nearest edge cell. Returns 0 for an invalid handle or a degenerate
/// scale. Result is in world Y units (heights times scale[1]).
/// @param obj Terrain3D handle.
/// @param wx World-space x coordinate in the terrain's local frame.
/// @param wz World-space z coordinate in the terrain's local frame.
/// @return Edge-clamped, bilinearly interpolated world-space height, or zero for invalid input.
double rt_terrain3d_get_height_at(void *obj, double wx, double wz) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    double sx;
    double sy;
    double sz;
    if (!terrain_has_valid_grid(t) || !isfinite(wx) || !isfinite(wz))
        return 0.0;
    sx = terrain_scale_axis_or(t, 0, 1.0);
    sy = terrain_scale_axis_or(t, 1, 1.0);
    sz = terrain_scale_axis_or(t, 2, 1.0);

    double hx = wx / sx;
    double hz = wz / sz;
    int ix, iz;
    float fx, fz;
    if (hx <= 0.0) {
        ix = 0;
        fx = 0;
    } else if (hx >= (double)(t->width - 1)) {
        ix = t->width - 2;
        fx = 1;
    } else {
        double floor_hx = floor(hx);
        ix = (int)floor_hx;
        fx = (float)(hx - floor_hx);
    }
    if (hz <= 0.0) {
        iz = 0;
        fz = 0;
    } else if (hz >= (double)(t->depth - 1)) {
        iz = t->depth - 2;
        fz = 1;
    } else {
        double floor_hz = floor(hz);
        iz = (int)floor_hz;
        fz = (float)(hz - floor_hz);
    }

    float h00 = sample_height(t, ix, iz);
    float h10 = sample_height(t, ix + 1, iz);
    float h01 = sample_height(t, ix, iz + 1);
    float h11 = sample_height(t, ix + 1, iz + 1);
    if (!isfinite(h00))
        h00 = 0.0f;
    if (!isfinite(h10))
        h10 = 0.0f;
    if (!isfinite(h01))
        h01 = 0.0f;
    if (!isfinite(h11))
        h11 = 0.0f;
    float h = h00 * (1 - fx) * (1 - fz) + h10 * fx * (1 - fz) + h01 * (1 - fx) * fz + h11 * fx * fz;
    if (!isfinite(h))
        h = 0.0f;
    return (double)h * sy;
}

/// @brief Compute the surface normal at world-space (wx, wz) using central-difference of the
/// height grid. Returns a fresh Vec3 (always points "up-ish"; defaults to (0,1,0) on invalid
/// input). Normal is normalized; useful for placing props or aligning rotations to slope.
/// @param obj Terrain3D handle.
/// @param wx World-space x coordinate in the terrain's local frame.
/// @param wz World-space z coordinate in the terrain's local frame.
/// @return New GC-managed normalized Vec3, falling back to `(0, 1, 0)` for invalid input.
void *rt_terrain3d_get_normal_at(void *obj, double wx, double wz) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    double sx;
    double sy;
    double sz;
    if (!terrain_has_valid_grid(t) || !isfinite(wx) || !isfinite(wz))
        return rt_vec3_new(0, 1, 0);
    sx = terrain_scale_axis_or(t, 0, 1.0);
    sy = terrain_scale_axis_or(t, 1, 1.0);
    sz = terrain_scale_axis_or(t, 2, 1.0);

    double hx = wx / sx;
    double hz = wz / sz;
    int ix = terrain_coord_to_index(hx, t->width - 1);
    int iz = terrain_coord_to_index(hz, t->depth - 1);

    double nx;
    double ny;
    double nz;
    terrain_grid_normal_at(t, ix, iz, sx, sy, sz, &nx, &ny, &nz);

    return rt_vec3_new(nx, ny, nz);
}

/// @brief Pre-blend the four splat-map layers into a single baked terrain texture.
/// @details Real-time splat-mapping — sampling four layer textures and
///   RGBA-weight blending per fragment — is bandwidth-expensive, so this CPU
///   bake walks every splat-map texel once, reads its RGBA weights, normalizes
///   them, samples each enabled layer texture at its configured UV scale
///   (letting rock/moss tile at different frequencies), and writes the
///   weighted sum into a new `Pixels` object that replaces the terrain's
///   material texture. Degenerate zero-weight texels default to pure layer 0
///   rather than producing black holes. Called only when the splat map or a
///   layer texture is invalidated — subsequent frames just sample the baked
///   result.
/// @param t Terrain whose first splat bank and layer textures are baked.
static void bake_splat_texture(rt_terrain3d *t) {
    if (!t->splat_map || !t->material || !rt_g3d_has_class(t->material, RT_G3D_MATERIAL3D_CLASS_ID))
        return;

    void *splat_obj = terrain_valid_splat_pixels_or_null(t->splat_map);
    rt_pixels_impl *splat = rt_pixels_checked_impl_or_null(splat_obj);
    void *splat2_obj = terrain_valid_splat_pixels_or_null(t->splat_map2);
    rt_pixels_impl *splat2 = rt_pixels_checked_impl_or_null(splat2_obj);
    if (!splat)
        return;

    /* Generate a blended texture at splat map resolution */
    int32_t tw = (int32_t)splat->width, th = (int32_t)splat->height;
    void *baked = rt_pixels_new(tw, th);
    if (!baked)
        return;

    if (!t->base_texture) {
        rt_material3d *material = (rt_material3d *)t->material;
        if (material && material->texture != t->baked_texture &&
            terrain_texture_ref_supported(material->texture))
            terrain_assign_texture_ref(&t->base_texture, material->texture);
    }

    for (int32_t y = 0; y < th; y++) {
        for (int32_t x = 0; x < tw; x++) {
            double u = tw > 1 ? (double)x / (double)(tw - 1) : 0.0;
            double v = th > 1 ? (double)y / (double)(th - 1) : 0.0;

            /* Sample splat weights (RGBA → 4 layer weights) */
            uint32_t sp = splat->data[(int64_t)y * tw + x];
            double weights[TERRAIN_MAX_SPLAT_LAYERS];
            weights[0] = (double)((sp >> 24) & 0xFF) / 255.0;
            weights[1] = (double)((sp >> 16) & 0xFF) / 255.0;
            weights[2] = (double)((sp >> 8) & 0xFF) / 255.0;
            weights[3] = (double)(sp & 0xFF) / 255.0;
            weights[4] = weights[5] = weights[6] = weights[7] = 0.0;
            if (splat2) {
                /* Map 1 weights layers 4-7; sampled with wraparound when sizes differ. */
                uint32_t sp2 =
                    splat2
                        ->data[(int64_t)(y % splat2->height) * splat2->width + (x % splat2->width)];
                weights[4] = (double)((sp2 >> 24) & 0xFF) / 255.0;
                weights[5] = (double)((sp2 >> 16) & 0xFF) / 255.0;
                weights[6] = (double)((sp2 >> 8) & 0xFF) / 255.0;
                weights[7] = (double)(sp2 & 0xFF) / 255.0;
            }

            /* Normalize weights */
            double wsum = 0.0;
            for (int layer = 0; layer < TERRAIN_MAX_SPLAT_LAYERS; layer++)
                wsum += weights[layer];
            if (wsum > 0.001) {
                for (int layer = 0; layer < TERRAIN_MAX_SPLAT_LAYERS; layer++)
                    weights[layer] /= wsum;
            } else {
                weights[0] = 1.0;
                for (int layer = 1; layer < TERRAIN_MAX_SPLAT_LAYERS; layer++)
                    weights[layer] = 0.0;
            }

            uint32_t fallback = t->base_texture ? sample_pixels_uv(t->base_texture, u, v)
                                                : terrain_material_fallback_pixel(t);

            /* Sample each layer texture at its UV scale and blend. Missing layer textures
             * fall back to the base material texture (or white when absent) so partial
             * splat setups do not bake black texels. */
            double br = 0, bg = 0, bb = 0, ba = 0;
            for (int layer = 0; layer < TERRAIN_MAX_SPLAT_LAYERS; layer++) {
                if (weights[layer] < 0.001)
                    continue;
                double layer_scale = terrain_layer_scale_or(t, layer);
                double lu = u * layer_scale;
                double lv = v * layer_scale;
                void *layer_pixels = terrain_valid_pixels_or_null(t->layer_textures[layer]);
                uint32_t lp = layer_pixels ? sample_pixels_uv(layer_pixels, lu, lv) : fallback;
                double lr = (double)((lp >> 24) & 0xFF) / 255.0;
                double lg2 = (double)((lp >> 16) & 0xFF) / 255.0;
                double lb = (double)((lp >> 8) & 0xFF) / 255.0;
                double la = (double)(lp & 0xFF) / 255.0;
                br += lr * weights[layer];
                bg += lg2 * weights[layer];
                bb += lb * weights[layer];
                ba += la * weights[layer];
            }

            int32_t cr = terrain_channel_to_u8(br);
            int32_t cg = terrain_channel_to_u8(bg);
            int32_t cb = terrain_channel_to_u8(bb);
            int32_t ca = terrain_channel_to_u8(ba);
            int64_t color = ((int64_t)cr << 24) | ((int64_t)cg << 16) | ((int64_t)cb << 8) | ca;
            rt_pixels_set(baked, x, y, color);
        }
    }

    rt_material3d_set_texture(t->material, baked);
    terrain_assign_texture_ref(&t->baked_texture, baked);
    if (rt_obj_release_check0(baked))
        rt_obj_free(baked);
    t->splat_dirty = 0;
}

/*==========================================================================
 * Holes — authored XZ rectangles carved out of render/collision/nav
 *=========================================================================*/

/// @brief Clamp authored-hole traversal to initialized backing storage.
/// @param terrain Borrowed Terrain3D payload.
/// @return Non-negative hole count bounded by the owned allocation.
static int32_t terrain_safe_hole_count(const rt_terrain3d *terrain) {
    if (!terrain || !terrain->holes || terrain->hole_count <= 0 || terrain->hole_capacity <= 0)
        return 0;
    return terrain->hole_count < terrain->hole_capacity ? terrain->hole_count
                                                        : terrain->hole_capacity;
}

/// @brief Convert a possibly huge terrain coordinate to a bounded cell edge.
/// @param coordinate Terrain-local coordinate or endpoint.
/// @param scale Positive world-units-per-cell scale.
/// @param limit Exclusive cell count and maximum returned edge.
/// @param round_up Nonzero for an exclusive upper edge, zero for a lower edge.
/// @return Cell edge in `[0, limit]` without an out-of-range floating-to-integer cast.
static int32_t terrain_hole_cell_edge(double coordinate,
                                      double scale,
                                      int32_t limit,
                                      int round_up) {
    if (limit <= 0 || !isfinite(scale) || scale <= 0.0)
        return 0;
    double scaled = coordinate / scale;
    if (isnan(scaled) || scaled <= 0.0)
        return 0;
    if (!isfinite(scaled) || scaled >= (double)limit)
        return limit;
    return (int32_t)(round_up ? ceil(scaled) : floor(scaled));
}

/// @brief Test whether cell (cx, cz) is holed. NULL/absent mask means solid.
/// @param t Terrain containing the rasterized cell mask.
/// @param cx Cell x coordinate.
/// @param cz Cell z coordinate.
/// @return Nonzero when the in-range cell is marked as a hole.
static int terrain_cell_holed(const rt_terrain3d *t, int32_t cx, int32_t cz) {
    int32_t cells_x;
    int32_t cells_z;
    int64_t bit;
    if (!t || !t->hole_mask)
        return 0;
    cells_x = t->width - 1;
    cells_z = t->depth - 1;
    if (cells_x <= 0 || cells_z <= 0 || cx < 0 || cz < 0 || cx >= cells_x || cz >= cells_z)
        return 0;
    bit = (int64_t)cz * cells_x + cx;
    return (t->hole_mask[bit >> 3] >> (bit & 7)) & 1;
}

/// @brief True when any fine cell in [cx0,cx1) x [cz0,cz1) is holed (LOD-conservative).
/// @param t Terrain containing the rasterized cell mask.
/// @param cx0 Inclusive minimum cell x coordinate.
/// @param cz0 Inclusive minimum cell z coordinate.
/// @param cx1 Exclusive maximum cell x coordinate.
/// @param cz1 Exclusive maximum cell z coordinate.
/// @return Nonzero when any cell in the requested half-open rectangle is holed.
static int terrain_cell_range_holed(
    const rt_terrain3d *t, int32_t cx0, int32_t cz0, int32_t cx1, int32_t cz1) {
    if (!t || !t->hole_mask)
        return 0;
    if (cx1 <= cx0)
        cx1 = cx0 + 1;
    if (cz1 <= cz0)
        cz1 = cz0 + 1;
    for (int32_t cz = cz0; cz < cz1; ++cz)
        for (int32_t cx = cx0; cx < cx1; ++cx)
            if (terrain_cell_holed(t, cx, cz))
                return 1;
    return 0;
}

/// @brief Rasterize the authored hole rects into the per-cell bitmask.
/// @param t Terrain whose authored rectangles and derived mask are synchronized.
static void terrain_rebuild_hole_mask(rt_terrain3d *t) {
    int32_t cells_x;
    int32_t cells_z;
    size_t mask_bytes;
    double sx;
    double sz;
    int32_t hole_count;
    if (!t)
        return;
    t->holes_dirty = 0;
    free(t->hole_mask);
    t->hole_mask = NULL;
    if (t->width <= 1 || t->depth <= 1)
        return;
    cells_x = t->width - 1;
    cells_z = t->depth - 1;
    hole_count = terrain_safe_hole_count(t);
    if (hole_count <= 0)
        return;
    if ((size_t)cells_x > (SIZE_MAX - 7u) / (size_t)cells_z)
        return;
    mask_bytes = ((size_t)cells_x * (size_t)cells_z + 7u) / 8u;
    t->hole_mask = (uint8_t *)calloc(1, mask_bytes);
    if (!t->hole_mask)
        return;
    sx = terrain_scale_axis_or(t, 0, 1.0);
    sz = terrain_scale_axis_or(t, 2, 1.0);
    if (sx <= 1e-12)
        sx = 1.0;
    if (sz <= 1e-12)
        sz = 1.0;
    for (int32_t h = 0; h < hole_count; ++h) {
        double hx = t->holes[h][0];
        double hz = t->holes[h][1];
        double hw = t->holes[h][2];
        double hd = t->holes[h][3];
        int32_t cx0 = terrain_hole_cell_edge(hx, sx, cells_x, 0);
        int32_t cz0 = terrain_hole_cell_edge(hz, sz, cells_z, 0);
        int32_t cx1 = terrain_hole_cell_edge(hx + hw, sx, cells_x, 1);
        int32_t cz1 = terrain_hole_cell_edge(hz + hd, sz, cells_z, 1);
        for (int32_t cz = cz0; cz < cz1; ++cz) {
            for (int32_t cx = cx0; cx < cx1; ++cx) {
                int64_t bit = (int64_t)cz * cells_x + cx;
                t->hole_mask[bit >> 3] |= (uint8_t)(1u << (bit & 7));
            }
        }
    }
}

/// @brief Ensure the hole mask reflects the current hole list.
/// @param t Terrain whose mask is rebuilt only when marked dirty.
static void terrain_refresh_hole_mask(rt_terrain3d *t) {
    if (t && t->holes_dirty)
        terrain_rebuild_hole_mask(t);
}

/// @brief Carve a rectangular hole (terrain-local units, corner origin) into the terrain.
/// @details Holes propagate to chunk meshes at every LOD (conservatively — a coarse
///   triangle is skipped when any covered fine cell is holed), to nav meshes built via
///   BuildNavMesh, and to heightfield colliders through the raw hole-mask handoff.
/// @param obj Terrain3D handle; invalid handles are rejected.
/// @param x Local x coordinate of the rectangle origin.
/// @param z Local z coordinate of the rectangle origin.
/// @param width Positive rectangle extent along x.
/// @param depth Positive rectangle extent along z.
/// @return The hole's index for RemoveHole, or -1 on failure.
int64_t rt_terrain3d_set_hole(void *obj, double x, double z, double width, double depth) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return -1;
    if (!isfinite(x) || !isfinite(z) || !isfinite(width) || !isfinite(depth) || width <= 0.0 ||
        depth <= 0.0)
        return -1;
    t->hole_count = terrain_safe_hole_count(t);
    if (!t->holes)
        t->hole_capacity = 0;
    if (t->hole_capacity < t->hole_count)
        t->hole_capacity = t->hole_count;
    if (t->hole_count >= t->hole_capacity) {
        int32_t new_cap;
        if (t->hole_count == INT32_MAX)
            return -1;
        if (t->hole_capacity < 8)
            new_cap = 8;
        else if (t->hole_capacity > INT32_MAX / 2)
            new_cap = t->hole_count + 1;
        else
            new_cap = t->hole_capacity * 2;
        if (new_cap <= t->hole_count || (size_t)new_cap > SIZE_MAX / sizeof(*t->holes))
            return -1;
        double (*grown)[4] = (double (*)[4])realloc(t->holes, (size_t)new_cap * sizeof(*grown));
        if (!grown)
            return -1;
        t->holes = grown;
        t->hole_capacity = new_cap;
    }
    t->holes[t->hole_count][0] = x;
    t->holes[t->hole_count][1] = z;
    t->holes[t->hole_count][2] = width;
    t->holes[t->hole_count][3] = depth;
    t->hole_count++;
    t->holes_dirty = 1;
    terrain_refresh_hole_mask(t);
    invalidate_all_chunks(t);
    return t->hole_count - 1;
}

/// @brief Remove the hole at @p index (later holes shift down). 1 on success.
/// @param obj Terrain3D handle.
/// @param index Zero-based authored-hole index.
/// @return 1 when the hole is removed, otherwise 0.
int8_t rt_terrain3d_remove_hole(void *obj, int64_t index) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return 0;
    t->hole_count = terrain_safe_hole_count(t);
    if (index < 0 || index >= t->hole_count)
        return 0;
    for (int32_t i = (int32_t)index; i + 1 < t->hole_count; ++i) {
        t->holes[i][0] = t->holes[i + 1][0];
        t->holes[i][1] = t->holes[i + 1][1];
        t->holes[i][2] = t->holes[i + 1][2];
        t->holes[i][3] = t->holes[i + 1][3];
    }
    t->hole_count--;
    t->holes_dirty = 1;
    terrain_refresh_hole_mask(t);
    invalidate_all_chunks(t);
    return 1;
}

/// @brief Remove every authored hole.
/// @param obj Terrain3D handle; invalid handles are ignored.
void rt_terrain3d_clear_holes(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return;
    if (t->hole_count == 0 && !t->hole_mask)
        return;
    t->hole_count = 0;
    t->holes_dirty = 1;
    terrain_refresh_hole_mask(t);
    invalidate_all_chunks(t);
}

/// @brief Number of authored holes.
/// @param obj Terrain3D handle.
/// @return Current authored-hole count, or zero for an invalid handle.
int64_t rt_terrain3d_get_hole_count(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    return terrain_safe_hole_count(t);
}

/// @brief Internal: borrow the rasterized hole bitmask for heightfield-collider handoff.
/// @param obj Terrain3D handle.
/// @param out_cells_x Optional output receiving the mask width in cells.
/// @param out_cells_z Optional output receiving the mask depth in cells.
/// @return The bitmask (row-major, bit per cell) or NULL when no holes exist.
const uint8_t *rt_terrain3d_get_hole_mask_raw(void *obj,
                                              int32_t *out_cells_x,
                                              int32_t *out_cells_z) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (out_cells_x)
        *out_cells_x = 0;
    if (out_cells_z)
        *out_cells_z = 0;
    if (!t)
        return NULL;
    terrain_refresh_hole_mask(t);
    if (!t->hole_mask || t->width <= 1 || t->depth <= 1)
        return NULL;
    if (out_cells_x)
        *out_cells_x = t->width - 1;
    if (out_cells_z)
        *out_cells_z = t->depth - 1;
    return t->hole_mask;
}

/*==========================================================================
 * Splat maps 2 + slope/height auto-blend rules
 *=========================================================================*/

/// @brief Set splat map @p index (0 weights layers 0-3, 1 weights layers 4-7).
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param index Splat-map bank index, either 0 or 1.
/// @param pixels Nonempty RGBA Pixels weight map, or NULL to clear the selected bank.
void rt_terrain3d_set_splat_map_at(void *obj, int64_t index, void *pixels) {
    if (index == 0) {
        rt_terrain3d_set_splat_map(obj, pixels);
        return;
    }
    if (index != 1)
        return;
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return;
    terrain_repair_texture_slot(&t->splat_map2);
    if (pixels) {
        rt_pixels_impl *p =
            rt_pixels_checked_impl(pixels, "Terrain3D.SetSplatMapAt: expected Pixels");
        if (!p || p->width <= 0 || p->height <= 0 || p->width > INT32_MAX ||
            p->height > INT32_MAX || !p->data) {
            rt_trap("Terrain3D.SetSplatMapAt: splat map must be non-empty Pixels");
            return;
        }
    }
    terrain_assign_texture_ref(&t->splat_map2, pixels);
    t->splat_dirty = 1;
}

/// @brief Configure a slope-band rule (degrees) generating weights for @p layer.
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param layer Target splat layer from 0 through 7.
/// @param min_slope_deg Lower slope angle for the band, in degrees.
/// @param max_slope_deg Upper slope angle for the band, in degrees.
/// @param sharpness Transition sharpness at the band edges.
void rt_terrain3d_set_slope_layer(
    void *obj, int64_t layer, double min_slope_deg, double max_slope_deg, double sharpness) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t || layer < 0 || layer >= TERRAIN_MAX_SPLAT_LAYERS)
        return;
    t->layer_rules[layer].mode = 1;
    t->layer_rules[layer].min_value = isfinite(min_slope_deg) ? min_slope_deg : 0.0;
    t->layer_rules[layer].max_value = isfinite(max_slope_deg) ? max_slope_deg : 90.0;
    t->layer_rules[layer].sharpness = isfinite(sharpness) && sharpness > 0.0 ? sharpness : 1.0;
}

/// @brief Configure a height-band rule (world Y) generating weights for @p layer.
/// @param obj Terrain3D handle; invalid handles are ignored.
/// @param layer Target splat layer from 0 through 7.
/// @param min_y Lower world-space height for the band.
/// @param max_y Upper world-space height for the band.
/// @param sharpness Transition sharpness at the band edges.
void rt_terrain3d_set_height_layer(
    void *obj, int64_t layer, double min_y, double max_y, double sharpness) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t || layer < 0 || layer >= TERRAIN_MAX_SPLAT_LAYERS)
        return;
    t->layer_rules[layer].mode = 2;
    t->layer_rules[layer].min_value = isfinite(min_y) ? min_y : 0.0;
    t->layer_rules[layer].max_value = isfinite(max_y) ? max_y : 0.0;
    t->layer_rules[layer].sharpness = isfinite(sharpness) && sharpness > 0.0 ? sharpness : 1.0;
}

/// @brief Internal: borrow splat map @p index (0 or 1) for editor/test inspection.
/// @param obj Terrain3D handle.
/// @param index Splat-map bank index, either 0 or 1.
/// @return Borrowed Pixels handle for the requested map, or NULL when unavailable.
void *rt_terrain3d_get_splat_map_raw(void *obj, int64_t index) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!t)
        return NULL;
    if (index == 0)
        return t->splat_map;
    if (index == 1)
        return t->splat_map2;
    return NULL;
}

/// @brief Smoothstep helper for rule bands.
/// @param edge0 Lower transition edge.
/// @param edge1 Upper transition edge.
/// @param x Value to evaluate.
/// @return Cubic Hermite interpolation from zero to one across the ordered edges.
static double terrain_rule_smoothstep(double edge0, double edge1, double x) {
    double range = edge1 - edge0;
    double tval;
    if (!(range > 1e-12))
        return x >= edge1 ? 1.0 : 0.0;
    tval = (x - edge0) / range;
    if (tval < 0.0)
        tval = 0.0;
    if (tval > 1.0)
        tval = 1.0;
    return tval * tval * (3.0 - 2.0 * tval);
}

/// @brief Regenerate both splat maps from the configured slope/height rules.
/// @details Deterministic CPU pass at heightmap resolution: per-texel layer weight =
///   band(smoothstep in, smoothstep out); weights normalize across configured layers.
///   Painting a map afterwards overrides the generated weights (last write wins).
/// @param obj Terrain3D handle; invalid handles are ignored.
void rt_terrain3d_rebuild_splat_weights(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    int configured = 0;
    if (!t || !t->heights || t->width < 2 || t->depth < 2)
        return;
    for (int layer = 0; layer < TERRAIN_MAX_SPLAT_LAYERS; ++layer)
        if (t->layer_rules[layer].mode != 0)
            configured = 1;
    if (!configured)
        return;

    void *map0 = rt_pixels_new(t->width, t->depth);
    void *map1 = rt_pixels_new(t->width, t->depth);
    if (!map0 || !map1) {
        if (map0 && rt_obj_release_check0(map0))
            rt_obj_free(map0);
        if (map1 && rt_obj_release_check0(map1))
            rt_obj_free(map1);
        return;
    }
    double sx = terrain_scale_axis_or(t, 0, 1.0);
    double sy = terrain_scale_axis_or(t, 1, 1.0);
    double sz = terrain_scale_axis_or(t, 2, 1.0);
    for (int32_t iz = 0; iz < t->depth; ++iz) {
        for (int32_t ix = 0; ix < t->width; ++ix) {
            double nx;
            double ny;
            double nz;
            double weights[TERRAIN_MAX_SPLAT_LAYERS];
            double sum = 0.0;
            terrain_grid_normal_at(t, ix, iz, sx, sy, sz, &nx, &ny, &nz);
            if (ny < -1.0)
                ny = -1.0;
            if (ny > 1.0)
                ny = 1.0;
            double slope_deg = acos(ny) * (180.0 / 3.14159265358979323846);
            double height_y = (double)sample_height(t, ix, iz) * sy;
            for (int layer = 0; layer < TERRAIN_MAX_SPLAT_LAYERS; ++layer) {
                double w = 0.0;
                double value = 0.0;
                int mode = t->layer_rules[layer].mode;
                if (mode == 1)
                    value = slope_deg;
                else if (mode == 2)
                    value = height_y;
                if (mode != 0) {
                    double sharp = t->layer_rules[layer].sharpness;
                    double lo = t->layer_rules[layer].min_value;
                    double hi = t->layer_rules[layer].max_value;
                    w = terrain_rule_smoothstep(lo - sharp, lo + sharp, value) *
                        (1.0 - terrain_rule_smoothstep(hi - sharp, hi + sharp, value));
                }
                weights[layer] = w;
                sum += w;
            }
            if (sum <= 1e-9) {
                /* No rule matched: fall back entirely to layer 0. */
                weights[0] = 1.0;
                sum = 1.0;
            }
            int32_t ch[TERRAIN_MAX_SPLAT_LAYERS];
            for (int layer = 0; layer < TERRAIN_MAX_SPLAT_LAYERS; ++layer) {
                double w = weights[layer] / sum;
                ch[layer] = terrain_channel_to_u8(w);
            }
            int64_t p0 = ((int64_t)ch[0] << 24) | ((int64_t)ch[1] << 16) | ((int64_t)ch[2] << 8) |
                         (int64_t)ch[3];
            int64_t p1 = ((int64_t)ch[4] << 24) | ((int64_t)ch[5] << 16) | ((int64_t)ch[6] << 8) |
                         (int64_t)ch[7];
            rt_pixels_set(map0, ix, iz, p0);
            rt_pixels_set(map1, ix, iz, p1);
        }
    }
    terrain_assign_texture_ref(&t->splat_map, map0);
    terrain_assign_texture_ref(&t->splat_map2, map1);
    if (rt_obj_release_check0(map0))
        rt_obj_free(map0);
    if (rt_obj_release_check0(map1))
        rt_obj_free(map1);
    t->splat_dirty = 1;
}

/// @brief Compute per-vertex data at grid position (ix, iz).
/// @param t Terrain containing the source height grid and scale.
/// @param ix Grid x coordinate.
/// @param iz Grid z coordinate.
/// @param wx Output receiving local world x.
/// @param wy Output receiving local world y.
/// @param wz Output receiving local world z.
/// @param nx Output receiving the unit-normal x component.
/// @param ny Output receiving the unit-normal y component.
/// @param nz_n Output receiving the unit-normal z component.
/// @param u Output receiving normalized horizontal terrain UV.
/// @param v Output receiving normalized vertical terrain UV.
static void terrain_vertex(rt_terrain3d *t,
                           int32_t ix,
                           int32_t iz,
                           double *wx,
                           double *wy,
                           double *wz,
                           double *nx,
                           double *ny,
                           double *nz_n,
                           double *u,
                           double *v) {
    double h = (double)sample_height(t, ix, iz);
    double sx = terrain_scale_axis_or(t, 0, 1.0);
    double sy = terrain_scale_axis_or(t, 1, 1.0);
    double sz = terrain_scale_axis_or(t, 2, 1.0);
    if (!isfinite(h))
        h = 0.0;
    *wx = (double)ix * sx;
    *wy = h * sy;
    *wz = (double)iz * sz;
    terrain_grid_normal_at(t, ix, iz, sx, sy, sz, nx, ny, nz_n);
    *u = (double)ix / (double)(t->width - 1);
    *v = (double)iz / (double)(t->depth - 1);
}

/// @brief Emit the LOD-stepped surface grid (vertices + CCW triangles) for one terrain chunk,
///        expanding the running world-space AABB to cover the generated vertices.
/// @param mesh Mesh3D receiving generated vertices and triangles.
/// @param t Terrain containing the height grid, scale, and hole mask.
/// @param x0 First grid x coordinate in the chunk.
/// @param z0 First grid z coordinate in the chunk.
/// @param cols Number of source cells along x.
/// @param rows Number of source cells along z.
/// @param step Grid stride: 1 for LOD0, 2 for LOD1, or 4 for LOD2.
/// @param vert_cols Number of generated surface vertices along x.
/// @param vert_rows Number of generated surface vertices along z.
/// @param aabb_min Running three-component minimum bound.
/// @param aabb_max Running three-component maximum bound.
static void terrain_build_chunk_surface(void *mesh,
                                        rt_terrain3d *t,
                                        int32_t x0,
                                        int32_t z0,
                                        int32_t cols,
                                        int32_t rows,
                                        int32_t step,
                                        int32_t vert_cols,
                                        int32_t vert_rows,
                                        float *aabb_min,
                                        float *aabb_max) {
    for (int32_t rz = 0; rz < vert_rows; rz++) {
        int32_t dz = rz * step;
        if (dz > rows || rz == vert_rows - 1)
            dz = rows;
        for (int32_t rx = 0; rx < vert_cols; rx++) {
            int32_t dx = rx * step;
            if (dx > cols || rx == vert_cols - 1)
                dx = cols;
            int32_t ix = x0 + dx, iz = z0 + dz;
            if (ix >= t->width)
                ix = t->width - 1;
            if (iz >= t->depth)
                iz = t->depth - 1;
            double wx, wy, wz, nx, ny, nz_n, u, v;
            terrain_vertex(t, ix, iz, &wx, &wy, &wz, &nx, &ny, &nz_n, &u, &v);
            rt_mesh3d_add_vertex(mesh, wx, wy, wz, nx, ny, nz_n, u, v);
            if ((float)wx < aabb_min[0])
                aabb_min[0] = (float)wx;
            if ((float)wy < aabb_min[1])
                aabb_min[1] = (float)wy;
            if ((float)wz < aabb_min[2])
                aabb_min[2] = (float)wz;
            if ((float)wx > aabb_max[0])
                aabb_max[0] = (float)wx;
            if ((float)wy > aabb_max[1])
                aabb_max[1] = (float)wy;
            if ((float)wz > aabb_max[2])
                aabb_max[2] = (float)wz;
        }
    }
    for (int32_t rz = 0; rz < vert_rows - 1; rz++) {
        for (int32_t rx = 0; rx < vert_cols - 1; rx++) {
            if (t->hole_mask) {
                /* Conservative hole test: skip the quad when any covered fine
                 * cell is holed so every LOD hides the same footprint. */
                int32_t dz0 = rz * step;
                int32_t dz1 = (rz + 1) * step;
                int32_t dx0 = rx * step;
                int32_t dx1 = (rx + 1) * step;
                if (dz1 > rows || rz + 1 == vert_rows - 1)
                    dz1 = rows;
                if (dx1 > cols || rx + 1 == vert_cols - 1)
                    dx1 = cols;
                if (terrain_cell_range_holed(t, x0 + dx0, z0 + dz0, x0 + dx1, z0 + dz1))
                    continue;
            }
            int64_t base = (int64_t)(rz * vert_cols + rx);
            rt_mesh3d_add_triangle(mesh, base, base + vert_cols, base + 1);
            rt_mesh3d_add_triangle(mesh, base + 1, base + vert_cols, base + vert_cols + 1);
        }
    }
}

/// @brief Emit downward "skirt" geometry around a chunk's four edges to hide cracks between LOD
///        levels. Built for every LOD, including full-detail step==1 chunks, whenever skirt
///        depth @p sd is positive: a fine chunk that borders a coarser neighbor still cracks on
///        its own edge, so skipping skirts for step==1 left those seams open to the sky.
/// @param mesh Mesh3D receiving skirt vertices and triangles.
/// @param t Terrain containing the height grid and scale.
/// @param x0 First grid x coordinate in the chunk.
/// @param z0 First grid z coordinate in the chunk.
/// @param cols Number of source cells along x.
/// @param rows Number of source cells along z.
/// @param step Grid stride for the selected LOD.
/// @param vert_cols Surface-grid vertex count along x.
/// @param vert_rows Surface-grid vertex count along z.
/// @param sd Positive downward skirt depth in world units.
static void terrain_build_chunk_skirts(void *mesh,
                                       rt_terrain3d *t,
                                       int32_t x0,
                                       int32_t z0,
                                       int32_t cols,
                                       int32_t rows,
                                       int32_t step,
                                       int32_t vert_cols,
                                       int32_t vert_rows,
                                       double sd) {
    /* For each of the 4 edges, add skirt triangles */
    /* Top edge (dz=0), bottom edge (dz=rows), left (dx=0), right (dx=cols) */
    int64_t skirt_base = (int64_t)(vert_rows * vert_cols);

    /* Top edge (z = z0) */
    for (int32_t rx = 0; rx < vert_cols; rx++) {
        int32_t sample_dx = rx * step;
        if (sample_dx > cols || rx == vert_cols - 1)
            sample_dx = cols;
        int32_t ix = x0 + sample_dx;
        if (ix >= t->width)
            ix = t->width - 1;
        double wx, wy, wz, nx, ny, nz_n, u, v;
        terrain_vertex(t, ix, z0, &wx, &wy, &wz, &nx, &ny, &nz_n, &u, &v);
        rt_mesh3d_add_vertex(mesh, wx, wy - sd, wz, 0, -1, 0, u, v);
    }
    for (int32_t rx = 0; rx < vert_cols - 1; rx++) {
        int64_t top = (int64_t)rx;              /* top edge vertex */
        int64_t bot = skirt_base + (int64_t)rx; /* skirt vertex */
        rt_mesh3d_add_triangle(mesh, top, bot, top + 1);
        rt_mesh3d_add_triangle(mesh, top + 1, bot, bot + 1);
    }
    skirt_base += vert_cols;

    /* Bottom edge (z = z0 + rows) */
    int64_t bottom_row_start = (int64_t)((vert_rows - 1) * vert_cols);
    for (int32_t rx = 0; rx < vert_cols; rx++) {
        int32_t sample_dx = rx * step;
        if (sample_dx > cols || rx == vert_cols - 1)
            sample_dx = cols;
        int32_t ix = x0 + sample_dx;
        int32_t iz = z0 + rows;
        if (ix >= t->width)
            ix = t->width - 1;
        if (iz >= t->depth)
            iz = t->depth - 1;
        double wx, wy, wz, nx, ny, nz_n, u, v;
        terrain_vertex(t, ix, iz, &wx, &wy, &wz, &nx, &ny, &nz_n, &u, &v);
        rt_mesh3d_add_vertex(mesh, wx, wy - sd, wz, 0, -1, 0, u, v);
    }
    for (int32_t rx = 0; rx < vert_cols - 1; rx++) {
        int64_t top = bottom_row_start + (int64_t)rx;
        int64_t bot = skirt_base + (int64_t)rx;
        rt_mesh3d_add_triangle(mesh, top, top + 1, bot);
        rt_mesh3d_add_triangle(mesh, top + 1, bot + 1, bot);
    }
    skirt_base += vert_cols;

    /* Left edge (x = x0) */
    for (int32_t rz = 0; rz < vert_rows; rz++) {
        int32_t sample_dz = rz * step;
        if (sample_dz > rows || rz == vert_rows - 1)
            sample_dz = rows;
        int32_t iz = z0 + sample_dz;
        if (iz >= t->depth)
            iz = t->depth - 1;
        double wx, wy, wz, nx, ny, nz_n, u, v;
        terrain_vertex(t, x0, iz, &wx, &wy, &wz, &nx, &ny, &nz_n, &u, &v);
        rt_mesh3d_add_vertex(mesh, wx, wy - sd, wz, -1, 0, 0, u, v);
    }
    for (int32_t rz = 0; rz < vert_rows - 1; rz++) {
        int64_t top = (int64_t)(rz * vert_cols); /* left column vertex */
        int64_t bot = skirt_base + (int64_t)rz;
        rt_mesh3d_add_triangle(mesh, top, bot, (int64_t)((rz + 1) * vert_cols));
        rt_mesh3d_add_triangle(mesh, (int64_t)((rz + 1) * vert_cols), bot, bot + 1);
    }
    skirt_base += vert_rows;

    /* Right edge (x = x0 + cols) */
    for (int32_t rz = 0; rz < vert_rows; rz++) {
        int32_t ix = x0 + cols;
        int32_t sample_dz = rz * step;
        if (sample_dz > rows || rz == vert_rows - 1)
            sample_dz = rows;
        int32_t iz = z0 + sample_dz;
        if (ix >= t->width)
            ix = t->width - 1;
        if (iz >= t->depth)
            iz = t->depth - 1;
        double wx, wy, wz, nx, ny, nz_n, u, v;
        terrain_vertex(t, ix, iz, &wx, &wy, &wz, &nx, &ny, &nz_n, &u, &v);
        rt_mesh3d_add_vertex(mesh, wx, wy - sd, wz, 1, 0, 0, u, v);
    }
    for (int32_t rz = 0; rz < vert_rows - 1; rz++) {
        int64_t top = (int64_t)(rz * vert_cols + vert_cols - 1);
        int64_t bot = skirt_base + (int64_t)rz;
        rt_mesh3d_add_triangle(mesh, top, (int64_t)((rz + 1) * vert_cols + vert_cols - 1), bot);
        rt_mesh3d_add_triangle(mesh, (int64_t)((rz + 1) * vert_cols + vert_cols - 1), bot + 1, bot);
    }
}

/// @brief Build one terrain chunk mesh at a requested grid stride.
/// @details Surface vertices, hole-aware triangles, and optional adaptive skirts are emitted in one
/// geometry batch. The returned mesh owns its generated geometry.
/// @param t Terrain containing source heights and construction settings.
/// @param cx Chunk x coordinate.
/// @param cz Chunk z coordinate.
/// @param step Grid stride: 1 for LOD0, 2 for LOD1, or 4 for LOD2.
/// @param aabb_out Optional six-float output receiving minimum then maximum bounds.
/// @return New Mesh3D handle, or NULL when inputs, allocation, or geometry construction fail.
static void *build_chunk(rt_terrain3d *t, int32_t cx, int32_t cz, int32_t step, float *aabb_out) {
    int32_t x0 = cx * TERRAIN_CHUNK_SIZE;
    int32_t z0 = cz * TERRAIN_CHUNK_SIZE;
    void *mesh;
    if (!terrain_has_valid_grid(t) || step <= 0 || cx < 0 || cz < 0) {
        if (aabb_out) {
            for (int i = 0; i < 6; i++)
                aabb_out[i] = 0.0f;
        }
        return NULL;
    }
    mesh = rt_mesh3d_new();
    if (!mesh)
        return NULL;

    /* Determine actual chunk extents (may be smaller at edges) */
    int32_t xend = x0 + TERRAIN_CHUNK_SIZE;
    int32_t zend = z0 + TERRAIN_CHUNK_SIZE;
    if (xend >= t->width)
        xend = t->width - 1;
    if (zend >= t->depth)
        zend = t->depth - 1;
    int32_t cols = xend - x0;
    int32_t rows = zend - z0;
    if (cols <= 0 || rows <= 0) {
        if (aabb_out) {
            for (int i = 0; i < 6; i++)
                aabb_out[i] = 0.0f;
        }
        return mesh;
    }

    /* Track AABB during vertex generation */
    float aabb_min[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float aabb_max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    /* Vertices (with LOD step). Edge chunks can be smaller than the selected
     * step, so force the final row/column to be present. */
    int32_t vert_cols = cols / step + 1;
    int32_t vert_rows = rows / step + 1;
    if (cols % step != 0)
        vert_cols++;
    if (rows % step != 0)
        vert_rows++;
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    terrain_build_chunk_surface(
        mesh, t, x0, z0, cols, rows, step, vert_cols, vert_rows, aabb_min, aabb_max);

    /* Skirt geometry: extend edges downward to hide cracks between LOD levels.
     * Built for every LOD, including full-detail LOD0 (step==1): an LOD0 chunk
     * bordering a coarser neighbor still cracks along its own fine edge, so
     * gating skirts to step>1 left the nearest seams open to the sky.
     *
     * The depth adapts to this chunk's own vertical relief. A fixed depth cannot
     * hide seams on steep terrain: a mountain chunk spans far more height than the
     * flat configured depth, so the gap between a fine edge and its coarser
     * neighbor exceeds a shallow skirt. The finer chunk's edge shares the coarse
     * neighbor's corner samples, so its relief provably bounds the seam gap; a
     * skirt that deep closes it. Flat chunks keep the small configured depth.
     * relief is read before aabb_min[1] is lowered below. */
    float skirt_depth = terrain_skirt_depth_or(t);
    if (skirt_depth > 0.0f) {
        float relief = aabb_max[1] - aabb_min[1];
        float eff_skirt = skirt_depth > relief ? skirt_depth : relief;
        aabb_min[1] -= eff_skirt;
        terrain_build_chunk_skirts(
            mesh, t, x0, z0, cols, rows, step, vert_cols, vert_rows, (double)eff_skirt);
    }
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);

    if (((rt_mesh3d *)mesh)->build_failed) {
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        if (aabb_out) {
            for (int i = 0; i < 6; i++)
                aabb_out[i] = 0.0f;
        }
        return NULL;
    }

    /* Output AABB */
    if (aabb_out) {
        aabb_out[0] = aabb_min[0];
        aabb_out[1] = aabb_min[1];
        aabb_out[2] = aabb_min[2];
        aabb_out[3] = aabb_max[0];
        aabb_out[4] = aabb_max[1];
        aabb_out[5] = aabb_max[2];
    }

    return mesh;
}

/// @brief Build one chunk LOD if its mesh slot is empty or corrupt.
/// @details The generated AABB is written to the matching per-LOD bounds array. Stale private mesh
///   slots are repaired through the retained Graphics3D slot helper so wrong-class heap handles are
///   released only when they are owned retained objects. Returns true only when both a checked
///   Mesh3D and valid AABB are resident afterward.
/// @param t Terrain owning the chunk caches.
/// @param cx Chunk x coordinate.
/// @param cz Chunk z coordinate.
/// @param chunk_index Flat chunk-cache index.
/// @param lod Requested LOD index from 0 through 2.
/// @return Nonzero when the selected cache slot contains a valid mesh and bounds.
static int terrain_build_chunk_lod_if_missing(
    rt_terrain3d *t, int32_t cx, int32_t cz, int32_t chunk_index, int32_t lod) {
    void **array;
    float *aabb;
    if (!terrain_has_valid_chunk_grid(t) || chunk_index < 0 ||
        chunk_index >= terrain_safe_chunk_count(t))
        return 0;
    array = terrain_lod_mesh_array(t, lod);
    aabb = terrain_lod_aabb_slot(t, chunk_index, lod);
    if (!array || !aabb)
        return 0;
    if (!terrain_lod_mesh_or_null(t, chunk_index, lod)) {
        terrain_release_mesh_slot(&array[chunk_index]);
        terrain_aabb_clear(aabb);
        array[chunk_index] = build_chunk(t, cx, cz, terrain_lod_step(lod), aabb);
    }
    return terrain_lod_mesh_or_null(t, chunk_index, lod) != NULL && terrain_aabb_is_valid(aabb);
}

/// @brief Clamp a selected LOD so initialized neighbors differ by at most one level.
/// @details This is a lightweight seam guard for the chunked renderer. It preserves the existing
///   distance/hysteresis selection but prevents a high-detail chunk from sitting directly beside
///   the coarsest LOD, a configuration that makes T-junction cracks much easier to see.
/// @param t Terrain containing per-chunk LOD history.
/// @param chunk_index Flat chunk-cache index.
/// @param lod Initially selected LOD.
/// @return LOD clamped to `[0, 2]` and, when neighbors are initialized, to a one-level difference.
static int32_t terrain_clamp_lod_to_neighbors(rt_terrain3d *t, int32_t chunk_index, int32_t lod) {
    int32_t cx;
    int32_t cz;
    int32_t clamped = lod;
    if (!terrain_has_valid_chunk_grid(t) || !t->chunk_lod_state || chunk_index < 0 ||
        chunk_index >= terrain_safe_chunk_count(t))
        return lod < 0 ? 0 : (lod > 2 ? 2 : lod);
    cx = chunk_index % t->chunks_x;
    cz = chunk_index / t->chunks_x;
    const int32_t offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        int32_t nx = cx + offsets[i][0];
        int32_t nz = cz + offsets[i][1];
        int32_t nidx;
        int32_t neighbor_lod;
        if (nx < 0 || nz < 0 || nx >= t->chunks_x || nz >= t->chunks_z)
            continue;
        nidx = nz * t->chunks_x + nx;
        if (nidx < 0 || nidx >= terrain_safe_chunk_count(t))
            continue;
        neighbor_lod = t->chunk_lod_state[nidx];
        if (neighbor_lod == 0xFF)
            continue;
        if (clamped > neighbor_lod + 1)
            clamped = neighbor_lod + 1;
        else if (neighbor_lod > clamped + 1)
            clamped = neighbor_lod - 1;
    }
    if (clamped < 0)
        clamped = 0;
    if (clamped > 2)
        clamped = 2;
    if (clamped != lod)
        t->last_lod_clamped_chunk_count++;
    t->chunk_lod_state[chunk_index] = (uint8_t)clamped;
    return clamped;
}

/// @brief Resolve the mesh and exact AABB to draw for a chunk, falling back across resident LODs.
/// @details A missing selected LOD must not turn into a terrain hole. The fallback order prefers
///   nearby quality (`LOD1 -> LOD0 -> LOD2`, `LOD2 -> LOD1 -> LOD0`) while ensuring any resident
///   mesh draws. `last_missing_lod_count` records both fallback and complete-miss cases.
/// @param t Terrain owning resident LOD meshes and bounds.
/// @param chunk_index Flat chunk-cache index.
/// @param requested_lod Preferred LOD index.
/// @param out_lod Optional output receiving the actual resident LOD selected.
/// @param out_aabb Optional output receiving the actual LOD's borrowed six-float bounds.
/// @return Borrowed resident Mesh3D handle, or NULL when no valid LOD is available.
static void *terrain_select_draw_mesh_with_fallback(rt_terrain3d *t,
                                                    int32_t chunk_index,
                                                    int32_t requested_lod,
                                                    int32_t *out_lod,
                                                    const float **out_aabb) {
    int32_t order[3];
    if (out_lod)
        *out_lod = requested_lod;
    if (out_aabb)
        *out_aabb = NULL;
    if (!terrain_has_valid_chunk_grid(t))
        return NULL;
    if (requested_lod <= 0) {
        order[0] = 0;
        order[1] = 1;
        order[2] = 2;
    } else if (requested_lod == 1) {
        order[0] = 1;
        order[1] = 0;
        order[2] = 2;
    } else {
        order[0] = 2;
        order[1] = 1;
        order[2] = 0;
    }
    for (int i = 0; i < 3; ++i) {
        int32_t lod = order[i];
        void *mesh = terrain_lod_mesh_or_null(t, chunk_index, lod);
        const float *aabb = terrain_lod_aabb_slot(t, chunk_index, lod);
        if (!mesh || !terrain_aabb_is_valid(aabb))
            continue;
        if (lod != requested_lod) {
            t->last_missing_lod_count++;
            t->lod_prewarm_dirty = 1;
        }
        if (out_lod)
            *out_lod = lod;
        if (out_aabb)
            *out_aabb = aabb;
        return mesh;
    }
    t->last_missing_lod_count++;
    t->lod_prewarm_dirty = 1;
    return NULL;
}

/// @brief Return a stable per-chunk key for sorting, motion history, and optional occlusion
/// history.
/// @details Using the selected LOD mesh as identity churns history when a chunk crosses an LOD
///   threshold. The chunk LOD-state byte has stable storage for the life of the terrain cache.
/// @param t Terrain owning stable per-chunk state bytes.
/// @param chunk_index Flat chunk-cache index.
/// @param fallback Pointer returned when stable terrain storage is unavailable.
/// @return Stable address of the chunk's LOD-state byte, or @p fallback for invalid input.
static const void *terrain_chunk_motion_key(rt_terrain3d *t, int32_t chunk_index, void *fallback) {
    if (!terrain_has_valid_chunk_grid(t) || !t->chunk_lod_state || chunk_index < 0 ||
        chunk_index >= terrain_safe_chunk_count(t))
        return fallback;
    return &t->chunk_lod_state[chunk_index];
}
