//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/world/rt_terrain3d_lod.inc  (#included from rt_terrain3d.c; not a
// standalone translation unit) Purpose: Terrain3D LOD selection, chunk-grid accessors, union AABB,
// and lifecycle
//===----------------------------------------------------------------------===//
/**
 * @file rt_terrain3d_lod.inc
 * @brief Implements Terrain3D resource lifetime, validated cache access, and LOD selection.
 *
 * This implementation fragment is included by `rt_terrain3d.c` before construction and draw
 * helpers so those fragments share one defensive resource and chunk-grid policy.
 */

static int32_t terrain_safe_chunk_count(const rt_terrain3d *t);

/// @brief Release and null a GC-tracked reference slot.
/// @details Shared plumbing for the terrain's texture slots (`base_texture`,
///   `baked_texture`, splat layer textures) and LOD mesh grids. Idempotent on null.
/// @param slot Address of the retained reference slot to release and clear.
static void terrain_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief True if @p ref is a usable terrain texture reference (a Pixels image or a
/// TextureAsset3D).
/// @param ref Candidate runtime texture reference.
/// @return Nonzero for live Pixels or TextureAsset3D handles, otherwise zero.
static int terrain_texture_ref_supported(void *ref) {
    return ref && (rt_pixels_checked_impl_or_null(ref) ||
                   rt_g3d_has_class(ref, RT_G3D_TEXTUREASSET3D_CLASS_ID));
}

/// @brief Release a retained texture slot and tolerate stale, wrong-class handles.
/// @param slot Address of the retained texture slot; unsupported unowned values are only cleared.
static void terrain_release_texture_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!terrain_texture_ref_supported(*slot)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    terrain_release_ref(slot);
}

/// @brief Release and clear a texture slot whose contents are no longer supported.
/// @param slot Address of the texture slot to validate and repair.
static void terrain_repair_texture_slot(void **slot) {
    if (slot && *slot && !terrain_texture_ref_supported(*slot))
        rt_g3d_ref_slot_clear_unowned(slot);
}

/// @brief Release a retained material slot and tolerate stale, wrong-class handles.
/// @param slot Address of the retained material slot; unsupported unowned values are only cleared.
static void terrain_release_material_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MATERIAL3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    terrain_release_ref(slot);
}

/// @brief Release and clear a material slot whose contents are no longer a Material3D.
/// @param slot Address of the material slot to validate and repair.
static void terrain_repair_material_slot(void **slot) {
    if (slot && *slot && !rt_g3d_has_class(*slot, RT_G3D_MATERIAL3D_CLASS_ID))
        rt_g3d_ref_slot_clear_unowned(slot);
}

/// @brief Release a retained mesh slot and tolerate stale, wrong-class handles.
/// @param slot Address of the retained Mesh3D slot; unsupported unowned values are only cleared.
static void terrain_release_mesh_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MESH3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    terrain_release_ref(slot);
}

/// @brief Clamp `value` into `[-TERRAIN3D_ABS_MAX, TERRAIN3D_ABS_MAX]`, substituting `fallback`
/// when not finite.
/// @param value Candidate numeric value.
/// @param fallback Replacement for non-finite input.
/// @return Finite value constrained to the terrain safety range.
static double terrain_clamp_abs_or(double value, double fallback) {
    return rt_world3d_clamp_abs_or(value, fallback, TERRAIN3D_ABS_MAX);
}

/// @brief Return a finite terrain scale axis, repairing degenerate X/Z spacing to fallback.
/// @param t Terrain containing authored scale values.
/// @param axis Scale axis index from 0 through 2.
/// @param fallback Value used for invalid axes, non-finite data, or degenerate horizontal spacing.
/// @return Finite, bounded scale value.
static double terrain_scale_axis_or(const rt_terrain3d *t, int axis, double fallback) {
    double value;
    if (!t || axis < 0 || axis > 2)
        return fallback;
    value = t->scale[axis];
    if (!isfinite(value))
        return fallback;
    if ((axis == 0 || axis == 2) && fabs(value) < 1e-12)
        return fallback;
    return terrain_clamp_abs_or(value, fallback);
}

/// @brief Repair a private/corrupt height sample before it enters queries or generated geometry.
/// @param value Candidate height sample.
/// @return Finite sample clamped to the terrain safety range.
static float terrain_sanitize_height_sample(float value) {
    if (!isfinite(value))
        return 0.0f;
    if (value > (float)TERRAIN3D_ABS_MAX)
        return (float)TERRAIN3D_ABS_MAX;
    if (value < (float)-TERRAIN3D_ABS_MAX)
        return (float)-TERRAIN3D_ABS_MAX;
    return value;
}

/// @brief Return a safe skirt depth for mesh generation.
/// @param t Terrain containing the authored skirt depth.
/// @return Finite depth in `[0, 1000]`.
static float terrain_skirt_depth_or(const rt_terrain3d *t) {
    double depth = t ? (double)t->skirt_depth : 0.0;
    if (!isfinite(depth) || depth <= 0.0)
        return 0.0f;
    if (depth > 1000.0)
        depth = 1000.0;
    return (float)depth;
}

/// @brief Return a safe positive UV tiling scale for a splat layer.
/// @param t Terrain containing per-layer tiling values.
/// @param layer Layer index from 0 through 7.
/// @return Positive finite tiling scale, falling back to 1.
static double terrain_layer_scale_or(const rt_terrain3d *t, int layer) {
    double scale;
    if (!t || layer < 0 || layer >= TERRAIN_MAX_SPLAT_LAYERS)
        return 1.0;
    scale = t->layer_scales[layer];
    if (!isfinite(scale) || scale <= 0.0)
        return 1.0;
    if (scale > TERRAIN3D_ABS_MAX)
        return TERRAIN3D_ABS_MAX;
    return scale;
}

/// @brief Read LOD thresholds, repairing private corruption without mutating the terrain object.
/// @param t Terrain containing authored LOD thresholds.
/// @param out_near Optional output receiving the sanitized LOD0-to-LOD1 distance.
/// @param out_far Optional output receiving the sanitized LOD1-to-LOD2 distance.
static void terrain_lod_distances_or(const rt_terrain3d *t, float *out_near, float *out_far) {
    double near_dist = t ? (double)t->lod_dist1 : 100.0;
    double far_dist = t ? (double)t->lod_dist2 : 250.0;

    if (!isfinite(near_dist) || near_dist < 0.0)
        near_dist = 50.0;
    if (near_dist > TERRAIN3D_ABS_MAX)
        near_dist = TERRAIN3D_ABS_MAX;
    if (!isfinite(far_dist) || far_dist <= near_dist)
        far_dist = near_dist + 100.0;
    if (far_dist > TERRAIN3D_ABS_MAX)
        far_dist = TERRAIN3D_ABS_MAX;
    if (far_dist <= near_dist) {
        far_dist = near_dist + 1.0;
        if (far_dist > TERRAIN3D_ABS_MAX) {
            far_dist = TERRAIN3D_ABS_MAX;
            near_dist = TERRAIN3D_ABS_MAX > 1.0 ? TERRAIN3D_ABS_MAX - 1.0 : 0.0;
        }
    }
    if (out_near)
        *out_near = (float)near_dist;
    if (out_far)
        *out_far = (float)far_dist;
}

/// @brief Return a finite, threshold-safe LOD hysteresis band for chunk selection.
/// @details The band is clamped below half the near/far threshold gap so LOD 1 always has a usable
///   stable interval. Non-finite or negative values become 0, preserving sharp legacy thresholds.
/// @param t Terrain containing the authored hysteresis width.
/// @param lod_near Sanitized near LOD threshold.
/// @param lod_far Sanitized far LOD threshold.
/// @return Finite nonnegative band safe for the supplied threshold interval.
static float terrain_lod_hysteresis_or(const rt_terrain3d *t, float lod_near, float lod_far) {
    double value = t ? (double)t->lod_hysteresis : 0.0;
    double max_band = ((double)lod_far - (double)lod_near) * 0.45;

    if (!isfinite(value) || value < 0.0)
        value = 0.0;
    if (!isfinite(max_band) || max_band < 0.0)
        max_band = 0.0;
    if (value > max_band)
        value = max_band;
    if (value > TERRAIN3D_ABS_MAX)
        value = TERRAIN3D_ABS_MAX;
    return (float)value;
}

/// @brief Clear every chunk's remembered LOD state to the uninitialized sentinel.
/// @details Called after global LOD-distance changes and cache invalidation so the next draw
///   chooses each chunk from raw distance once, then resumes hysteresis-stabilized switching.
/// @param t Terrain whose per-chunk LOD history is reset.
static void terrain_reset_lod_state(rt_terrain3d *t) {
    int32_t n;
    if (!t || !t->chunk_lod_state)
        return;
    n = terrain_safe_chunk_count(t);
    if (n <= 0)
        return;
    memset(t->chunk_lod_state, 0xFF, (size_t)n);
}

/// @brief Select a chunk LOD with hysteresis around the configured distance thresholds.
/// @details The first selection uses the raw thresholds. Later selections require the distance to
///   move outside a threshold plus/minus the hysteresis band before changing state, preventing
///   terrain triangles from visibly popping or flickering while the camera jitters near a boundary.
/// @param t Terrain owning remembered per-chunk LOD state.
/// @param chunk_index Flat chunk-cache index.
/// @param distance Nonnegative camera-to-chunk distance.
/// @param lod_near LOD0-to-LOD1 threshold.
/// @param lod_far LOD1-to-LOD2 threshold.
/// @return Selected LOD index from 0 through 2.
static TERRAIN3D_UNUSED_PRIVATE int32_t terrain_select_lod(
    rt_terrain3d *t, int32_t chunk_index, double distance, float lod_near, float lod_far) {
    float hys;
    uint8_t current;

    if (!t || chunk_index < 0 || chunk_index >= terrain_safe_chunk_count(t))
        return distance >= (double)lod_far ? 2 : (distance >= (double)lod_near ? 1 : 0);
    if (!isfinite(distance))
        distance = (double)lod_far;
    hys = terrain_lod_hysteresis_or(t, lod_near, lod_far);
    if (!t->chunk_lod_state)
        return distance >= (double)lod_far ? 2 : (distance >= (double)lod_near ? 1 : 0);
    current = t->chunk_lod_state[chunk_index];
    if (current > 2)
        current =
            (uint8_t)(distance >= (double)lod_far ? 2 : (distance >= (double)lod_near ? 1 : 0));
    else if (current == 0) {
        if (distance >= (double)lod_near + (double)hys)
            current = 1;
    } else if (current == 1) {
        if (distance < (double)lod_near - (double)hys)
            current = 0;
        else if (distance >= (double)lod_far + (double)hys)
            current = 2;
    } else {
        if (distance < (double)lod_far - (double)hys)
            current = 1;
    }
    t->chunk_lod_state[chunk_index] = current;
    return (int32_t)current;
}

/// @brief Select a chunk LOD from squared distance, preserving hysteresis behavior.
/// @details Terrain draw only compares chunk distance against thresholds; it does not need the
///   actual Euclidean distance. Squaring the sanitized thresholds avoids one `sqrt` per visible
///   chunk while matching `terrain_select_lod` for non-negative distances.
/// @param t Terrain owning remembered per-chunk LOD state.
/// @param chunk_index Flat chunk-cache index.
/// @param distance2 Squared camera-to-chunk distance.
/// @param lod_near LOD0-to-LOD1 threshold.
/// @param lod_far LOD1-to-LOD2 threshold.
/// @return Selected LOD index from 0 through 2.
static int32_t terrain_select_lod_squared(
    rt_terrain3d *t, int32_t chunk_index, double distance2, float lod_near, float lod_far) {
    float hys;
    uint8_t current;
    if (!isfinite(distance2) || distance2 < 0.0)
        distance2 = (double)lod_far * (double)lod_far;
    if (!t || chunk_index < 0 || chunk_index >= terrain_safe_chunk_count(t)) {
        double near2 = (double)lod_near * (double)lod_near;
        double far2 = (double)lod_far * (double)lod_far;
        return distance2 >= far2 ? 2 : (distance2 >= near2 ? 1 : 0);
    }
    hys = terrain_lod_hysteresis_or(t, lod_near, lod_far);
    if (!t->chunk_lod_state) {
        double near2 = (double)lod_near * (double)lod_near;
        double far2 = (double)lod_far * (double)lod_far;
        return distance2 >= far2 ? 2 : (distance2 >= near2 ? 1 : 0);
    }
    current = t->chunk_lod_state[chunk_index];
    if (current > 2) {
        double near2 = (double)lod_near * (double)lod_near;
        double far2 = (double)lod_far * (double)lod_far;
        current = (uint8_t)(distance2 >= far2 ? 2 : (distance2 >= near2 ? 1 : 0));
    } else if (current == 0) {
        double threshold = (double)lod_near + (double)hys;
        if (distance2 >= threshold * threshold)
            current = 1;
    } else if (current == 1) {
        double near_threshold = (double)lod_near - (double)hys;
        double far_threshold = (double)lod_far + (double)hys;
        if (near_threshold < 0.0)
            near_threshold = 0.0;
        if (distance2 < near_threshold * near_threshold)
            current = 0;
        else if (distance2 >= far_threshold * far_threshold)
            current = 2;
    } else {
        double threshold = (double)lod_far - (double)hys;
        if (threshold < 0.0)
            threshold = 0.0;
        if (distance2 < threshold * threshold)
            current = 1;
    }
    t->chunk_lod_state[chunk_index] = current;
    return (int32_t)current;
}

/// @brief Floor a continuous heightfield coordinate to a grid index, clamped to
///        `[0, max_index]`. Non-finite or non-positive input maps to 0.
/// @param coord Continuous heightfield coordinate.
/// @param max_index Inclusive maximum grid index.
/// @return Floored and clamped grid index.
static int32_t terrain_coord_to_index(double coord, int32_t max_index) {
    if (!isfinite(coord))
        return 0;
    if (coord <= 0.0)
        return 0;
    if (coord >= (double)max_index)
        return max_index;
    return (int32_t)floor(coord);
}

/// @brief Map destination sample index to source index for nearest-neighbor resampling.
/// @param dst Destination sample index.
/// @param dst_count Total destination sample count.
/// @param src_count Total source sample count.
/// @return Nearest source sample index with exact endpoint mapping.
static int32_t terrain_resample_index(int32_t dst, int32_t dst_count, int32_t src_count) {
    int64_t numerator;
    int64_t denominator;
    int64_t value;
    if (dst_count <= 1 || src_count <= 1 || dst <= 0)
        return 0;
    if (dst >= dst_count - 1)
        return src_count - 1;
    numerator = (int64_t)dst * (int64_t)(src_count - 1);
    denominator = (int64_t)(dst_count - 1);
    value = (numerator + denominator / 2) / denominator;
    if (value < 0)
        return 0;
    if (value >= src_count)
        return src_count - 1;
    return (int32_t)value;
}

/// @brief Assign @p value into a texture slot, retaining the new value and releasing the old
///   (no-op when unchanged).
/// @param slot Address of the retained texture slot to update.
/// @param value New texture handle, or NULL to clear the slot.
static void terrain_assign_texture_ref(void **slot, void *value) {
    if (!slot || *slot == value)
        return;
    rt_obj_retain_maybe(value);
    terrain_release_texture_slot(slot);
    *slot = value;
}

/// @brief Assign @p value into a material slot, retaining the new value and releasing the old
///   (no-op when unchanged).
/// @param slot Address of the retained material slot to update.
/// @param value New Material3D handle, or NULL to clear the slot.
static void terrain_assign_material_ref(void **slot, void *value) {
    if (!slot || *slot == value)
        return;
    rt_obj_retain_maybe(value);
    terrain_release_material_slot(slot);
    *slot = value;
}

/// @brief Re-bind the terrain's base texture onto its material's albedo slot, first dropping a
///   stale (non-Material3D) material or an unsupported base-texture reference.
/// @param t Terrain whose material albedo binding is restored.
static void terrain_restore_material_base_texture(rt_terrain3d *t) {
    void *base_texture;
    if (!t || !t->material)
        return;
    if (!rt_g3d_has_class(t->material, RT_G3D_MATERIAL3D_CLASS_ID)) {
        terrain_release_material_slot(&t->material);
        return;
    }
    base_texture = terrain_texture_ref_supported(t->base_texture) ? t->base_texture : NULL;
    if (!base_texture)
        terrain_release_texture_slot(&t->base_texture);
    rt_material3d_set_texture(t->material, base_texture);
}

/// @brief True when width/depth still fit inside the allocated height buffer.
/// @param t Terrain whose dimensions and height allocation are validated.
/// @return Nonzero when the sample grid is safe to index.
static int terrain_has_valid_grid(const rt_terrain3d *t) {
    int64_t needed;
    if (!t || !t->heights || t->width < 2 || t->depth < 2 || t->height_count <= 0)
        return 0;
    if ((int64_t)t->width > INT64_MAX / (int64_t)t->depth)
        return 0;
    needed = (int64_t)t->width * (int64_t)t->depth;
    return needed > 0 && needed <= t->height_count;
}

/// @brief Clamp a terrain chunk grid to the number of slots actually allocated.
/// @param t Terrain whose chunk dimensions and allocation capacity are inspected.
/// @return Safely indexable chunk count, or zero for invalid state.
static int32_t terrain_safe_chunk_count(const rt_terrain3d *t) {
    int64_t product;
    if (!t || t->chunk_capacity <= 0)
        return 0;
    if (t->chunk_capacity > TERRAIN3D_MAX_CHUNKS)
        return 0;
    if (t->chunks_x <= 0 || t->chunks_z <= 0)
        return 0;
    product = (int64_t)t->chunks_x * (int64_t)t->chunks_z;
    if (product <= 0)
        return t->chunk_capacity;
    if (product > t->chunk_capacity)
        return t->chunk_capacity;
    return (int32_t)product;
}

/// @brief True when all chunk-side arrays can be indexed by chunks_x * chunks_z.
/// @param t Terrain whose mesh, bound, and LOD-state arrays are validated.
/// @return Nonzero when every required chunk array safely covers the declared grid.
static int terrain_has_valid_chunk_grid(const rt_terrain3d *t) {
    int64_t product;
    if (!t || !t->chunk_meshes || !t->chunk_meshes_lod1 || !t->chunk_meshes_lod2 ||
        !t->chunk_aabbs || !t->chunk_aabbs_lod1 || !t->chunk_aabbs_lod2 || !t->chunk_aabbs_union ||
        !t->chunk_lod_state || t->chunk_capacity <= 0 || t->chunks_x <= 0 || t->chunks_z <= 0)
        return 0;
    if (t->chunk_capacity > TERRAIN3D_MAX_CHUNKS)
        return 0;
    product = (int64_t)t->chunks_x * (int64_t)t->chunks_z;
    return product > 0 && product <= t->chunk_capacity;
}

/// @brief Return the mesh-slot array for @p lod (0, 1, or 2), or NULL for invalid input.
/// @details Keeping this as a single helper prevents draw, prewarm, and fallback logic from
///   accidentally disagreeing about which cache owns a given terrain LOD.
/// @param t Terrain owning per-LOD mesh arrays.
/// @param lod Requested LOD index.
/// @return Borrowed mesh-slot array, or NULL for invalid input.
static void **terrain_lod_mesh_array(rt_terrain3d *t, int32_t lod) {
    if (!t)
        return NULL;
    if (lod <= 0)
        return t->chunk_meshes;
    if (lod == 1)
        return t->chunk_meshes_lod1;
    if (lod == 2)
        return t->chunk_meshes_lod2;
    return NULL;
}

/// @brief Return the AABB slot for @p lod (0, 1, or 2), or NULL for invalid input.
/// @details The slot stores six floats as min.xyz then max.xyz in terrain-local space.
/// @param t Terrain owning per-LOD bounds arrays.
/// @param chunk_index Flat chunk-cache index.
/// @param lod Requested LOD index.
/// @return Borrowed mutable six-float AABB slot, or NULL for invalid input.
static float *terrain_lod_aabb_slot(rt_terrain3d *t, int32_t chunk_index, int32_t lod) {
    float *base;
    if (!terrain_has_valid_chunk_grid(t) || chunk_index < 0 ||
        chunk_index >= terrain_safe_chunk_count(t))
        return NULL;
    if (lod <= 0)
        base = t->chunk_aabbs;
    else if (lod == 1)
        base = t->chunk_aabbs_lod1;
    else if (lod == 2)
        base = t->chunk_aabbs_lod2;
    else
        return NULL;
    return base ? &base[chunk_index * 6] : NULL;
}

/// @brief Return a class-checked resident chunk mesh for @p lod, or NULL if absent/corrupt.
/// @details Corrupt private slots are treated as missing. The release paths already avoid freeing
///   wrong-class slots, so this helper keeps rendering conservative without taking ownership.
/// @param t Terrain owning per-LOD mesh arrays.
/// @param chunk_index Flat chunk-cache index.
/// @param lod Requested LOD index.
/// @return Borrowed validated Mesh3D handle, or NULL when absent or invalid.
static void *terrain_lod_mesh_or_null(const rt_terrain3d *t, int32_t chunk_index, int32_t lod) {
    void *const *array;
    if (!terrain_has_valid_chunk_grid(t) || chunk_index < 0 ||
        chunk_index >= terrain_safe_chunk_count(t))
        return NULL;
    if (lod <= 0)
        array = t->chunk_meshes;
    else if (lod == 1)
        array = t->chunk_meshes_lod1;
    else if (lod == 2)
        array = t->chunk_meshes_lod2;
    else
        array = NULL;
    return array ? rt_g3d_checked_or_null(array[chunk_index], RT_G3D_MESH3D_CLASS_ID) : NULL;
}

/// @brief Return the decimation step used to generate the requested terrain LOD.
/// @param lod Requested LOD index.
/// @return Grid stride 1 for LOD0, 2 for LOD1, or 4 for LOD2 and above.
static int32_t terrain_lod_step(int32_t lod) {
    return lod >= 2 ? 4 : (lod == 1 ? 2 : 1);
}

/// @brief Return whether an AABB slot contains finite, ordered bounds.
/// @param aabb Six-float minimum/maximum bounds to validate.
/// @return Nonzero when all components are finite and each minimum does not exceed its maximum.
static int terrain_aabb_is_valid(const float *aabb) {
    if (!aabb)
        return 0;
    for (int axis = 0; axis < 3; ++axis) {
        if (!isfinite(aabb[axis]) || !isfinite(aabb[axis + 3]) || aabb[axis] > aabb[axis + 3])
            return 0;
    }
    return 1;
}

/// @brief Reset an AABB slot to a harmless empty sentinel.
/// @param aabb Mutable six-float bounds slot; NULL is accepted.
static void terrain_aabb_clear(float *aabb) {
    if (!aabb)
        return;
    for (int i = 0; i < 6; ++i)
        aabb[i] = 0.0f;
}

/// @brief Expand @p dst to include @p src, initializing it when @p has_dst is false.
/// @param dst Six-float destination union bounds.
/// @param has_dst In/out flag indicating whether @p dst is already initialized.
/// @param src Valid six-float bounds to include.
static void terrain_aabb_include(float dst[6], int *has_dst, const float src[6]) {
    if (!dst || !has_dst || !terrain_aabb_is_valid(src))
        return;
    if (!*has_dst) {
        memcpy(dst, src, sizeof(float) * 6);
        *has_dst = 1;
        return;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (src[axis] < dst[axis])
            dst[axis] = src[axis];
        if (src[axis + 3] > dst[axis + 3])
            dst[axis + 3] = src[axis + 3];
    }
}

/// @brief Rebuild the conservative union AABB for one chunk from every resident LOD.
/// @details Terrain frustum culling uses this union so LOD swaps cannot tighten the bounds for a
///   single frame and create visible horizon cracks. Missing LODs are ignored; if no usable mesh
///   exists, the union slot is cleared and the draw path falls back conservatively.
/// @param t Terrain owning resident LOD meshes and AABBs.
/// @param chunk_index Flat chunk-cache index.
/// @return Nonzero when at least one resident LOD contributes valid bounds.
static int terrain_rebuild_chunk_union_aabb(rt_terrain3d *t, int32_t chunk_index) {
    float union_aabb[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    int has_union = 0;
    float *dst;
    if (!terrain_has_valid_chunk_grid(t) || chunk_index < 0 ||
        chunk_index >= terrain_safe_chunk_count(t))
        return 0;
    for (int32_t lod = 0; lod < TERRAIN_LOD_LEVELS; ++lod) {
        if (terrain_lod_mesh_or_null(t, chunk_index, lod))
            terrain_aabb_include(
                union_aabb, &has_union, terrain_lod_aabb_slot(t, chunk_index, lod));
    }
    dst = &t->chunk_aabbs_union[chunk_index * 6];
    if (!has_union) {
        terrain_aabb_clear(dst);
        return 0;
    }
    memcpy(dst, union_aabb, sizeof(union_aabb));
    return 1;
}

/// @brief Return the conservative union AABB for a chunk, rebuilding it from LOD slots if needed.
/// @param t Terrain owning resident LOD bounds and the union cache.
/// @param chunk_index Flat chunk-cache index.
/// @return Borrowed valid six-float union AABB, or NULL when unavailable.
static const float *terrain_chunk_union_aabb(rt_terrain3d *t, int32_t chunk_index) {
    float *aabb;
    if (!terrain_has_valid_chunk_grid(t) || chunk_index < 0 ||
        chunk_index >= terrain_safe_chunk_count(t))
        return NULL;
    aabb = &t->chunk_aabbs_union[chunk_index * 6];
    if (!terrain_aabb_is_valid(aabb) && !terrain_rebuild_chunk_union_aabb(t, chunk_index))
        return NULL;
    return terrain_aabb_is_valid(aabb) ? aabb : NULL;
}

/// @brief Reset per-frame Terrain3D draw diagnostics before a chunk submission pass.
/// @param t Terrain whose draw counters are reset.
/// @param chunk_count Total chunks considered by the pending draw.
static void terrain_reset_draw_stats(rt_terrain3d *t, int32_t chunk_count) {
    if (!t)
        return;
    t->last_chunk_count = chunk_count > 0 ? chunk_count : 0;
    t->last_drawn_chunk_count = 0;
    t->last_frustum_culled_chunk_count = 0;
    t->last_missing_lod_count = 0;
    t->last_lod_clamped_chunk_count = 0;
    for (int i = 0; i < TERRAIN_LOD_LEVELS; ++i)
        t->last_lod_counts[i] = 0;
}

/// @brief Release every LOD mesh stored in the chunk grid.
/// @details Each of the three LOD arrays (`chunk_meshes`, `chunk_meshes_lod1`,
///   `chunk_meshes_lod2`) is a flat `chunks_x * chunks_z` array of optional
///   mesh slots. A null LOD array means that LOD has never been populated
///   and is simply skipped. Individual slot nulls inside a populated array
///   are handled by `terrain_release_ref`'s internal guard, so partial
///   grids (only some chunks meshed) are safe.
/// @param t Terrain whose retained chunk meshes and cached bounds are cleared.
static void terrain_release_chunk_meshes(rt_terrain3d *t) {
    int32_t n;
    if (!t)
        return;
    n = terrain_safe_chunk_count(t);
    for (int32_t i = 0; i < n; i++) {
        if (t->chunk_meshes)
            terrain_release_mesh_slot(&t->chunk_meshes[i]);
        if (t->chunk_meshes_lod1)
            terrain_release_mesh_slot(&t->chunk_meshes_lod1[i]);
        if (t->chunk_meshes_lod2)
            terrain_release_mesh_slot(&t->chunk_meshes_lod2[i]);
        if (t->chunk_aabbs)
            terrain_aabb_clear(&t->chunk_aabbs[i * 6]);
        if (t->chunk_aabbs_lod1)
            terrain_aabb_clear(&t->chunk_aabbs_lod1[i * 6]);
        if (t->chunk_aabbs_lod2)
            terrain_aabb_clear(&t->chunk_aabbs_lod2[i * 6]);
        if (t->chunk_aabbs_union)
            terrain_aabb_clear(&t->chunk_aabbs_union[i * 6]);
    }
}

/// @brief GC finalizer — release every owned resource on a terrain.
/// @details Tears down, in order: per-chunk mesh slots (all three LODs),
///   the material reference, both splat maps, each of the eight layer
///   textures, the original base texture and the baked splat composite,
///   then frees the heightmap + chunk mesh / AABB arrays. Every slot is
///   nulled so a subsequent sweep (possible during shutdown) becomes a
///   no-op rather than a double-free. Order matters only loosely — meshes
///   are released before the arrays that hold them, but nothing here has
///   cross-allocation data dependencies.
/// @param obj Terrain3D runtime object being finalized; NULL is accepted.
static void terrain3d_finalizer(void *obj) {
    rt_terrain3d *t = (rt_terrain3d *)obj;
    if (!t)
        return;
    terrain_release_chunk_meshes(t);
    terrain_release_material_slot(&t->material);
    terrain_release_texture_slot(&t->splat_map);
    terrain_release_texture_slot(&t->splat_map2);
    for (int i = 0; i < TERRAIN_MAX_SPLAT_LAYERS; i++)
        terrain_release_texture_slot(&t->layer_textures[i]);
    terrain_release_texture_slot(&t->base_texture);
    terrain_release_texture_slot(&t->baked_texture);
    free(t->heights);
    free(t->chunk_meshes);
    free(t->chunk_meshes_lod1);
    free(t->chunk_meshes_lod2);
    free(t->chunk_aabbs);
    free(t->chunk_aabbs_lod1);
    free(t->chunk_aabbs_lod2);
    free(t->chunk_aabbs_union);
    free(t->chunk_lod_state);
    free(t->holes);
    free(t->hole_mask);
    t->holes = NULL;
    t->hole_mask = NULL;
    t->hole_count = 0;
    t->hole_capacity = 0;
    t->heights = NULL;
    t->chunk_meshes = NULL;
    t->chunk_meshes_lod1 = NULL;
    t->chunk_meshes_lod2 = NULL;
    t->chunk_aabbs = NULL;
    t->chunk_aabbs_lod1 = NULL;
    t->chunk_aabbs_lod2 = NULL;
    t->chunk_aabbs_union = NULL;
    t->chunk_lod_state = NULL;
    t->height_count = 0;
    t->chunk_capacity = 0;
}

/// @brief Invalidate all cached chunk meshes across all LOD levels.
/// @param t Terrain whose retained mesh caches and LOD history are cleared.
static void invalidate_all_chunks(rt_terrain3d *t) {
    int32_t n;
    if (!t)
        return;
    n = terrain_safe_chunk_count(t);
    for (int32_t i = 0; i < n; i++) {
        if (t->chunk_meshes)
            terrain_release_mesh_slot(&t->chunk_meshes[i]);
        if (t->chunk_meshes_lod1)
            terrain_release_mesh_slot(&t->chunk_meshes_lod1[i]);
        if (t->chunk_meshes_lod2)
            terrain_release_mesh_slot(&t->chunk_meshes_lod2[i]);
    }
    terrain_reset_lod_state(t);
    t->lod_prewarm_dirty = 1;
}
