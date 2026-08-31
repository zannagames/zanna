//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_deferred.inc
// Purpose: Canvas3D deferred-draw command representation, queue growth, sorting,
//   visibility bookkeeping, material flattening, and transient tangent generation.
// Key invariants:
//   - Included only by rt_canvas3d.c; this is not a standalone translation unit.
//   - Published commands reference frame-owned or retained immutable payloads.
//   - Tangent generation through a stack mesh facade cannot publish a global
//     Mesh3D geometry mutation notification.
// Ownership/Lifetime:
//   - The Canvas3D frame owns queued command storage and all tracked resources
//     until submission and frame cleanup complete.
// Links: rt_canvas3d.c, rt_canvas3d_internal.h, rt_canvas3d_snapshot.c,
//        docs/adr/0168-windowless-canvas3d-rendering.md
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements Canvas3D deferred command capture, sorting, material flattening, and helpers.
/// @details This implementation fragment is included by `rt_canvas3d.c`. Its queued commands may
/// only borrow payloads whose lifetime has been transferred to Canvas3D frame storage or retained
/// through the transient-object manager.

/*==========================================================================
 * Deferred draw command (for transparency sorting)
 *=========================================================================*/

typedef enum {
    DEFERRED_DRAW_MESH = 0,
    DEFERRED_DRAW_INSTANCED = 1,
} deferred_draw_kind_t;

typedef enum {
    DEFERRED_PASS_MAIN = 0,
    DEFERRED_PASS_SCREEN_OVERLAY = 1,
} deferred_pass_t;

typedef struct {
    deferred_draw_kind_t kind;
    deferred_pass_t pass_kind;
    vgfx3d_draw_cmd_t cmd;
    const float *instance_matrices; /* row-major float[instance_count * 16] */
    int32_t instance_count;
    vgfx3d_light_params_t *lights;
    int32_t light_count;
    int32_t light_offset;
    float ambient[3];
    int8_t wireframe;
    int8_t backface_cull;
    int8_t has_local_bounds;
    int8_t visible;
    int8_t requires_blend;
    int8_t conservative_bounds;
    int8_t occlusion_test_disabled;
    int8_t occlusion_write_disabled;
    float culling_pad;
    float local_bounds_min[3];
    float local_bounds_max[3];
    int8_t has_world_bounds;
    float world_bounds_min[3];
    float world_bounds_max[3];
    float sort_key;                  /* bounds-aware view-depth key for deferred draw sorting */
    uintptr_t stable_sort_id;        /* persistent tie-breaker for equal sort keys */
    uintptr_t occlusion_fingerprint; /* draw identity before duplicate-occurrence disambiguation */
    uintptr_t occlusion_key;         /* persistent identity for CPU occlusion hysteresis */
    int32_t enqueue_index; /* original queue order; final tie-breaker for equal sort keys */
} deferred_draw_t;

typedef struct {
    int8_t valid;
    const void *mesh_key;
    uint64_t matrix_hash;
    double matrix_key[16];
} canvas3d_world_bounds_cache_key_t;

/// @brief Return whether a queued draw should participate in the shadow pass.
/// @details Material shadow mode is captured in the draw command so deferred submission is
///   independent of later material mutation. AUTO preserves the existing behavior: opaque and
///   alpha-masked draws cast, while alpha-blended/additive draws are skipped. NONE disables
///   casting, and CAST opts transparent draws into shadow rendering without changing the main pass.
/// @param dd Borrowed deferred draw snapshot.
/// @return Non-zero when the draw belongs in shadow submission; zero for overlays, disabled modes,
/// automatic transparent draws, or invalid input.
static int canvas3d_deferred_casts_shadow(const deferred_draw_t *dd) {
    if (!dd || dd->pass_kind != DEFERRED_PASS_MAIN)
        return 0;
    if (dd->cmd.shadow_mode == RT_MATERIAL3D_SHADOW_MODE_NONE)
        return 0;
    if (dd->cmd.shadow_mode == RT_MATERIAL3D_SHADOW_MODE_CAST)
        return 1;
    return dd->requires_blend ? 0 : 1;
}

#define CANVAS3D_OCCLUSION_GRID_W 64
#define CANVAS3D_OCCLUSION_GRID_H 64
#define CANVAS3D_OCCLUSION_GRID_CELLS (CANVAS3D_OCCLUSION_GRID_W * CANVAS3D_OCCLUSION_GRID_H)
#define CANVAS3D_OCCLUSION_HISTORY_RETENTION_FRAMES 120

typedef struct {
    float depth[CANVAS3D_OCCLUSION_GRID_CELLS];
    uint8_t covered[CANVAS3D_OCCLUSION_GRID_CELLS];
} canvas3d_occlusion_grid_t;

// canvas_motion_history_t is defined in rt_canvas3d_internal.h (shared with
// rt_canvas3d_motion.c).

// canvas3d_hash_u64 / canvas3d_next_power_of_two_i32 are declared in rt_canvas3d_internal.h
// (shared with rt_canvas3d_tempmgr.c).

/// @brief Grow the deferred-draw command buffer to hold `needed` entries.
///
/// Geometric growth (cap doubles, starting at 32). Used by the
/// transparency-sort path to buffer commands until end-of-frame.
/// Returns 0 on allocation failure; draw callers trap rather than bypassing
/// the queue because immediate submission breaks transparency ordering.
/// @param[in,out] buf Address of the realloc-compatible command-buffer pointer.
/// @param[in,out] capacity Address of the current element capacity, updated on success.
/// @param needed Positive minimum element capacity.
/// @return Non-zero when capacity is available; zero for invalid input, overflow, or allocation
/// failure. Existing storage remains owned by the caller on failure.
static int ensure_deferred_capacity(void **buf, int32_t *capacity, int32_t needed) {
    if (!buf || !capacity || needed <= 0)
        return 0;
    if (*capacity >= needed)
        return 1;

    int32_t new_cap = *capacity > 0 ? *capacity : 32;
    while (new_cap < needed) {
        if (new_cap > INT32_MAX / 2)
            new_cap = needed;
        else
            new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(deferred_draw_t))
        return 0;

    deferred_draw_t *new_buf =
        (deferred_draw_t *)realloc(*buf, (size_t)new_cap * sizeof(deferred_draw_t));
    if (!new_buf)
        return 0;
    *buf = new_buf;
    *capacity = new_cap;
    return 1;
}

/// @brief Reset the compact per-frame light-snapshot arena for reuse.
/// @details Deferred draws store pointers to exact-sized light slices rather than embedding
///          `VGFX3D_MAX_LIGHTS` entries in every command. Resetting only the used count preserves
///          the allocation across frames and keeps queued draw commands compact.
/// @param c Canvas whose per-frame light arena and flatten cache are reset; NULL is ignored.
static void canvas3d_reset_frame_light_snapshots(rt_canvas3d *c) {
    if (!c)
        return;
    c->frame_light_snapshot_count = 0;
    /* The flattened-light cache stores an offset into this arena; resetting the arena
     * makes any cached offset stale. It also re-arms the once-per-frame flatten against
     * camera-relative-origin changes applied at frame begin. */
    canvas3d_invalidate_light_flatten_cache(c);
    if (c->frame_light_snapshot_capacity > VGFX3D_MAX_LIGHTS * 4096) {
        free(c->frame_light_snapshots);
        c->frame_light_snapshots = NULL;
        c->frame_light_snapshot_capacity = 0;
    }
}

/// @brief Remap deferred-draw light pointers after the light-snapshot arena moves.
/// @details The runtime keeps the direct `dd->lights` pointer for hot backend submission and
///          unit-test visibility. When `realloc` moves the arena, this helper patches every queued
///          draw whose pointer referenced the previous allocation.
/// @param draws Borrowed deferred-draw array whose stored pointers may require rebasing.
/// @param count Number of valid draw entries.
/// @param old_base Previous arena base.
/// @param new_base Replacement arena base after relocation.
/// @param old_capacity Number of addressable elements in the previous arena.
static void canvas3d_remap_light_snapshot_pointers(deferred_draw_t *draws,
                                                   int32_t count,
                                                   vgfx3d_light_params_t *old_base,
                                                   vgfx3d_light_params_t *new_base,
                                                   int32_t old_capacity) {
    if (!draws || count <= 0 || !old_base || !new_base || old_capacity <= 0)
        return;
    vgfx3d_light_params_t *old_end = old_base + old_capacity;
    for (int32_t i = 0; i < count; ++i) {
        if (!draws[i].lights || draws[i].light_count <= 0)
            continue;
        if (draws[i].lights >= old_base && draws[i].lights < old_end)
            draws[i].lights = new_base + (draws[i].lights - old_base);
    }
}

/// @brief Copy @p light_count light parameters into the canvas's compact snapshot arena.
/// @details Returns a stable pointer for the current frame. The arena grows geometrically and
///          remaps existing queued command pointers on relocation; callers therefore keep direct
///          light pointers without paying for a fixed-size light array inside every command.
/// @param c Canvas that owns the per-frame snapshot arena.
/// @param lights Source light parameters, already sanitized by `build_light_params`.
/// @param light_count Number of valid entries in @p lights.
/// @param out_offset Receives the arena element offset, or -1 for no lights/fallback.
/// @return Pointer to the copied light slice, or NULL when @p light_count is zero or allocation
///         fails.
static vgfx3d_light_params_t *canvas3d_store_frame_light_snapshot(
    rt_canvas3d *c, const vgfx3d_light_params_t *lights, int32_t light_count, int32_t *out_offset) {
    vgfx3d_light_params_t *base;
    vgfx3d_light_params_t *grown;
    int32_t needed;
    int32_t new_capacity;
    int32_t old_capacity;

    if (out_offset)
        *out_offset = -1;
    if (!c || !lights || light_count <= 0)
        return NULL;
    if (light_count > VGFX3D_MAX_LIGHTS)
        light_count = VGFX3D_MAX_LIGHTS;
    if (c->frame_light_snapshot_count < 0 ||
        c->frame_light_snapshot_count > INT32_MAX - light_count)
        return NULL;
    needed = c->frame_light_snapshot_count + light_count;
    if (needed > c->frame_light_snapshot_capacity) {
        base = (vgfx3d_light_params_t *)c->frame_light_snapshots;
        old_capacity = c->frame_light_snapshot_capacity;
        new_capacity = old_capacity > 0 ? old_capacity : VGFX3D_MAX_LIGHTS * 32;
        while (new_capacity < needed) {
            if (new_capacity > INT32_MAX / 2) {
                new_capacity = needed;
                break;
            }
            new_capacity *= 2;
        }
        if ((size_t)new_capacity > SIZE_MAX / sizeof(*grown))
            return NULL;
        grown = (vgfx3d_light_params_t *)realloc(c->frame_light_snapshots,
                                                 (size_t)new_capacity * sizeof(*grown));
        if (!grown)
            return NULL;
        c->frame_light_snapshots = grown;
        c->frame_light_snapshot_capacity = new_capacity;
        if (grown != base) {
            canvas3d_remap_light_snapshot_pointers(
                (deferred_draw_t *)c->draw_cmds, c->draw_count, base, grown, old_capacity);
            canvas3d_remap_light_snapshot_pointers((deferred_draw_t *)c->final_overlay_cmds,
                                                   c->final_overlay_count,
                                                   base,
                                                   grown,
                                                   old_capacity);
        }
    }
    base = (vgfx3d_light_params_t *)c->frame_light_snapshots;
    if (!base)
        return NULL;
    if (out_offset)
        *out_offset = c->frame_light_snapshot_count;
    memcpy(&base[c->frame_light_snapshot_count],
           lights,
           (size_t)light_count * sizeof(vgfx3d_light_params_t));
    c->frame_light_snapshot_count += light_count;
    return &base[c->frame_light_snapshot_count - light_count];
}

/// @brief Assign the frame's flattened lights to a queued draw, flattening at most once
///        per light-state generation.
/// @details Previously every queued draw ran build_light_params (two passes over all 64
///   slots) plus a full snapshot memcmp — O(draws x lights) per frame. Draws that share
///   one Light3D mutation generation now share a single snapshot slice, revision stamp,
///   and froxel table. The cache is invalidated at frame begin, on slot/ambient/scene-
///   light changes, and by any Light3D property mutation (global generation counter).
///   Shadow patching in End() mutates the shared slice in place, which is safe because
///   canvas3d_apply_shadow_light_params is idempotent per slice.
/// @param c Canvas owning the flattened-light cache and snapshot arena.
/// @param[out] dd Deferred draw that receives borrowed slice, count, offset, revision, and cluster
/// table fields.
/// @return 1 on success, 0 when the snapshot arena allocation failed (caller traps).
static int canvas3d_assign_frame_lights(rt_canvas3d *c, deferred_draw_t *dd) {
    uint64_t light_revision;
    int32_t limit;

    if (!c || !dd)
        return 0;
    light_revision = rt_light3d_mutation_revision();
    limit = canvas3d_active_light_limit(c);
    if (!c->frame_light_cache_valid || c->frame_light_cache_revision != light_revision ||
        c->frame_light_cache_limit != limit) {
        vgfx3d_light_params_t light_scratch[VGFX3D_MAX_LIGHTS];
        int32_t count = build_light_params(c, light_scratch, limit);
        int32_t offset = -1;
        vgfx3d_light_params_t *stored =
            canvas3d_store_frame_light_snapshot(c, light_scratch, count, &offset);
        if (count > 0 && !stored)
            return 0;
        c->frame_light_cache_stamp = canvas3d_stamp_light_snapshot(c, stored, count);
        c->frame_light_cache_count = count;
        c->frame_light_cache_offset = stored ? offset : -1;
        c->frame_light_cache_revision = light_revision;
        c->frame_light_cache_limit = limit;
        c->frame_light_cache_valid = 1;
    }
    dd->light_count = c->frame_light_cache_count;
    /* Re-derive the pointer from the offset each time: the arena base can move when a
     * later store grows it (queued draws are remapped, this cache is not). */
    dd->lights =
        (c->frame_light_cache_offset >= 0 && c->frame_light_snapshots)
            ? (vgfx3d_light_params_t *)c->frame_light_snapshots + c->frame_light_cache_offset
            : NULL;
    dd->light_offset = c->frame_light_cache_offset;
    dd->cmd.lights_revision = c->frame_light_cache_stamp;
    /* Plan 07: attach the revision's froxel table (NULL keeps the flat loop). */
    dd->cmd.cluster_table = canvas3d_cluster_table_for_revision(
        c, dd->lights, dd->light_count, dd->cmd.lights_revision);
    return 1;
}

static void canvas3d_submit_deferred(rt_canvas3d *c, const deferred_draw_t *dd);
static void canvas3d_cache_deferred_world_bounds(
    rt_canvas3d *c, deferred_draw_t *dd, const canvas3d_world_bounds_cache_key_t *cache_key);
static int canvas3d_backend_uses_software_raster(const rt_canvas3d *c);

/// @brief Sanitize a draw sort key before it enters deferred ordering.
/// @details NaN sort keys violate `qsort`'s strict weak ordering and make fallback selection
///   unstable. Non-finite transparent keys are pushed behind valid back-to-front draws; non-finite
///   opaque keys are pushed behind valid front-to-back occluders.
/// @param sort_key Candidate view-depth key.
/// @param requires_blend Non-zero for back-to-front transparent ordering.
/// @return Finite key, using an ordering-appropriate extreme for non-finite input.
static float canvas3d_sanitize_sort_key(float sort_key, int requires_blend) {
    if (isfinite(sort_key))
        return sort_key;
    return requires_blend ? -FLT_MAX : FLT_MAX;
}

/// @brief Reset per-frame submission-cost counters and fixed-size AABB cache.
/// @details Called by public Begin/Begin2D entry points. Internal overlay replay intentionally
///          does not reset these fields so a frame can include its post-scene overlay submissions.
/// @param c Canvas whose counters and world-bounds cache are cleared; NULL is ignored.
static void canvas3d_reset_frame_submission_stats(rt_canvas3d *c) {
    if (!c)
        return;
    c->frame_draws_submitted = 0;
    c->frame_aabb_transforms = 0;
    c->frame_sort_passes = 0;
    c->frame_backend_state_changes = 0;
    c->frame_last_backend_state_key = 0;
    c->frame_has_backend_state_key = 0;
    c->last_auto_instanced_draws = 0;
    memset(c->world_bounds_cache, 0, sizeof(c->world_bounds_cache));
}

/// @brief Hash the original double-precision model matrix used as a world-bounds cache key.
/// @param matrix Borrowed row-major 4-by-4 matrix.
/// @return Stable non-zero process-local hash of the exact double bit patterns, or zero for NULL.
static uint64_t canvas3d_hash_matrix_f64(const double *matrix) {
    uint64_t key = UINT64_C(1469598103934665603);

    if (!matrix)
        return 0;
    for (int i = 0; i < 16; i++) {
        uint64_t bits = 0;
        memcpy(&bits, &matrix[i], sizeof(bits));
        key ^= bits;
        key *= UINT64_C(1099511628211);
    }
    key ^= key >> 32;
    key *= UINT64_C(0x9E3779B185EBCA87);
    key ^= key >> 29;
    return key ? key : UINT64_C(1);
}

/// @brief Build a source mesh+matrix key for the world-AABB cache.
/// @param[out] out Cache-key structure, always zero-initialized before input validation.
/// @param mesh_key Non-NULL source mesh identity pointer.
/// @param matrix Borrowed row-major double model matrix copied into the key.
static void canvas3d_make_deferred_world_bounds_cache_key(canvas3d_world_bounds_cache_key_t *out,
                                                          const void *mesh_key,
                                                          const double *matrix) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!mesh_key || !matrix)
        return;
    out->valid = 1;
    out->mesh_key = mesh_key;
    out->matrix_hash = canvas3d_hash_matrix_f64(matrix);
    memcpy(out->matrix_key, matrix, sizeof(out->matrix_key));
}

/// @brief Mix one 64-bit value into a process-local deferred-sort grouping key.
/// @param key Existing accumulator.
/// @param value Scalar or normalized pointer bits to mix.
/// @return Updated avalanche-mixed accumulator.
static uint64_t canvas3d_mix_sort_u64(uint64_t key, uint64_t value) {
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    key ^= value + UINT64_C(0x9E3779B97F4A7C15) + (key << 6) + (key >> 2);
    return key;
}

/// @brief Mix a process-local pointer identity into a deferred-sort grouping key.
/// @param key Existing accumulator.
/// @param ptr Borrowed pointer whose numeric identity is used only within this process.
/// @return Updated accumulator.
static uint64_t canvas3d_mix_sort_ptr(uint64_t key, const void *ptr) {
    return canvas3d_mix_sort_u64(key, (uint64_t)(uintptr_t)ptr);
}

/// @brief Pack the backend-visible pipeline/material/texture state for opaque grouping.
/// @details This deliberately excludes depth and enqueue order; those are applied as later stable
///          sort keys. The key is a grouping hint, not serialized data, so process-local pointer
///          identity is sufficient.
/// @param kind Mesh or instanced deferred-draw kind.
/// @param pass_kind Main or screen-overlay pass kind.
/// @param wireframe Captured wireframe state.
/// @param backface_cull Captured culling state.
/// @param cmd Borrowed backend-visible draw command.
/// @return Non-zero process-local grouping key covering pipeline, geometry, textures, and samplers.
static uint64_t canvas3d_backend_state_key_core(deferred_draw_kind_t kind,
                                                deferred_pass_t pass_kind,
                                                int8_t wireframe,
                                                int8_t backface_cull,
                                                const vgfx3d_draw_cmd_t *cmd) {
    uint64_t key = UINT64_C(0xD1B54A32D192ED03);

    if (!cmd)
        return key;
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)kind);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)pass_kind);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint8_t)wireframe);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint8_t)backface_cull);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->workflow);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->alpha_mode);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->shadow_mode);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->shading_model);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint8_t)cmd->unlit);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint8_t)cmd->disable_depth_test);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint8_t)cmd->additive_blend);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->double_sided);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint8_t)cmd->has_alpha_texture);
    key = canvas3d_mix_sort_u64(key, (uint64_t)(uint8_t)cmd->has_splat);
    /* Group identical meshes adjacently so the auto-instancing pass sees runs
     * (and backend geometry caches get consecutive hits). */
    key = canvas3d_mix_sort_ptr(key, cmd->geometry_key);
    key = canvas3d_mix_sort_ptr(key, cmd->texture);
    key = canvas3d_mix_sort_ptr(key, cmd->texture_target);
    key = canvas3d_mix_sort_ptr(key, cmd->normal_map);
    key = canvas3d_mix_sort_ptr(key, cmd->specular_map);
    key = canvas3d_mix_sort_ptr(key, cmd->emissive_map);
    key = canvas3d_mix_sort_ptr(key, cmd->metallic_roughness_map);
    key = canvas3d_mix_sort_ptr(key, cmd->ao_map);
    key = canvas3d_mix_sort_ptr(key, cmd->texture_asset);
    key = canvas3d_mix_sort_ptr(key, cmd->normal_map_asset);
    key = canvas3d_mix_sort_ptr(key, cmd->specular_map_asset);
    key = canvas3d_mix_sort_ptr(key, cmd->emissive_map_asset);
    key = canvas3d_mix_sort_ptr(key, cmd->metallic_roughness_map_asset);
    key = canvas3d_mix_sort_ptr(key, cmd->ao_map_asset);
    key = canvas3d_mix_sort_ptr(key, cmd->env_map);
    key = canvas3d_mix_sort_ptr(key, cmd->splat_map);
    for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
        key = canvas3d_mix_sort_u64(key, cmd->texture_asset_cache_key[slot]);
        key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->texture_slot_wrap_s[slot]);
        key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->texture_slot_wrap_t[slot]);
        key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->texture_slot_filter[slot]);
        key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->texture_slot_min_filter[slot]);
        key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->texture_slot_mag_filter[slot]);
        key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->texture_slot_mip_filter[slot]);
        key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->texture_slot_anisotropy[slot]);
        key = canvas3d_mix_sort_u64(key, (uint64_t)(uint32_t)cmd->texture_slot_uv_set[slot]);
    }
    for (int layer = 0; layer < 4; layer++)
        key = canvas3d_mix_sort_ptr(key, cmd->splat_layers[layer]);
    return key ? key : UINT64_C(1);
}

/// @brief Backend-state grouping key for a queued deferred draw.
/// @param dd Borrowed deferred draw.
/// @return Non-zero process-local backend-state key; invalid input returns the fixed seed.
static uint64_t canvas3d_deferred_backend_state_key(const deferred_draw_t *dd) {
    if (!dd)
        return UINT64_C(0xD1B54A32D192ED03);
    return canvas3d_backend_state_key_core(
        dd->kind, dd->pass_kind, dd->wireframe, dd->backface_cull, &dd->cmd);
}

/// @brief Account for one backend draw submission and material-state transition.
/// @param c Canvas whose frame counters are updated.
/// @param cmd Borrowed submitted command.
/// @param wireframe Effective wireframe state.
/// @param backface_cull Effective backface-cull state.
/// @param kind Submitted deferred-draw kind.
/// @param pass_kind Submitted pass kind.
static void canvas3d_note_backend_submission(rt_canvas3d *c,
                                             const vgfx3d_draw_cmd_t *cmd,
                                             int8_t wireframe,
                                             int8_t backface_cull,
                                             deferred_draw_kind_t kind,
                                             deferred_pass_t pass_kind) {
    uint64_t state_key;

    if (!c || !cmd)
        return;
    c->frame_draws_submitted++;
    if (canvas3d_backend_uses_software_raster(c))
        return;
    state_key = canvas3d_backend_state_key_core(kind, pass_kind, wireframe, backface_cull, cmd);
    if (!c->frame_has_backend_state_key || c->frame_last_backend_state_key != state_key) {
        c->frame_backend_state_changes++;
        c->frame_last_backend_state_key = state_key;
        c->frame_has_backend_state_key = 1;
    }
}

/// @brief Grow the canvas's text-rendering vertex + index scratch buffers.
///
/// Two-pair geometric growth — vertices and indices have separate
/// caps because the ratio depends on the glyph layout. Starting
/// capacities: 256 vertices, 384 indices.
/// @param c Canvas owning both realloc-compatible scratch arrays.
/// @param vertex_count Non-negative required vertex capacity.
/// @param index_count Non-negative required index capacity.
/// @return Non-zero when both capacities are available; zero for invalid input, overflow, or
/// allocation failure. Any successfully grown first array remains owned by the canvas.
static int ensure_text_capacity(rt_canvas3d *c, int32_t vertex_count, int32_t index_count) {
    if (!c || vertex_count < 0 || index_count < 0)
        return 0;
    if ((size_t)vertex_count > SIZE_MAX / sizeof(vgfx3d_vertex_t) ||
        (size_t)index_count > SIZE_MAX / sizeof(uint32_t))
        return 0;

    if (vertex_count > c->text_vertex_capacity) {
        int32_t new_cap = c->text_vertex_capacity > 0 ? c->text_vertex_capacity : 256;
        while (new_cap < vertex_count) {
            if (new_cap > INT32_MAX / 2)
                new_cap = vertex_count;
            else
                new_cap *= 2;
        }
        vgfx3d_vertex_t *new_verts =
            (vgfx3d_vertex_t *)realloc(c->text_vertices, (size_t)new_cap * sizeof(vgfx3d_vertex_t));
        if (!new_verts)
            return 0;
        c->text_vertices = new_verts;
        c->text_vertex_capacity = new_cap;
    }

    if (index_count > c->text_index_capacity) {
        int32_t new_cap = c->text_index_capacity > 0 ? c->text_index_capacity : 384;
        while (new_cap < index_count) {
            if (new_cap > INT32_MAX / 2)
                new_cap = index_count;
            else
                new_cap *= 2;
        }
        uint32_t *new_indices =
            (uint32_t *)realloc(c->text_indices, (size_t)new_cap * sizeof(uint32_t));
        if (!new_indices)
            return 0;
        c->text_indices = new_indices;
        c->text_index_capacity = new_cap;
    }

    return 1;
}

/// @brief `qsort` comparator: back-to-front (largest sort_key first).
///
/// Used to sort transparent draws so they composite correctly (back
/// objects drawn first so front objects blend over them).
/// @param a Borrowed pointer to the first `deferred_draw_t`.
/// @param b Borrowed pointer to the second `deferred_draw_t`.
/// @return Negative, zero, or positive according to sanitized descending depth, stable identity,
/// and enqueue-order comparison.
static int cmp_back_to_front(const void *a, const void *b) {
    const deferred_draw_t *da = (const deferred_draw_t *)a;
    const deferred_draw_t *db = (const deferred_draw_t *)b;
    float ka = canvas3d_sanitize_sort_key(da->sort_key, 1);
    float kb = canvas3d_sanitize_sort_key(db->sort_key, 1);
    if (ka > kb)
        return -1;
    if (ka < kb)
        return 1;
    if (da->stable_sort_id < db->stable_sort_id)
        return -1;
    if (da->stable_sort_id > db->stable_sort_id)
        return 1;
    if (da->enqueue_index < db->enqueue_index)
        return -1;
    if (da->enqueue_index > db->enqueue_index)
        return 1;
    return 0;
}

/// @brief `qsort` comparator: front-to-back (smallest sort_key first).
///
/// Used for opaque draws — front-to-back order maximizes early-Z
/// rejection, dropping pixel-shader work for occluded fragments.
/// @param a Borrowed pointer to the first `deferred_draw_t`.
/// @param b Borrowed pointer to the second `deferred_draw_t`.
/// @return Negative, zero, or positive according to sanitized ascending depth, stable identity,
/// and enqueue-order comparison.
static int cmp_front_to_back(const void *a, const void *b) {
    const deferred_draw_t *da = (const deferred_draw_t *)a;
    const deferred_draw_t *db = (const deferred_draw_t *)b;
    float ka = canvas3d_sanitize_sort_key(da->sort_key, 0);
    float kb = canvas3d_sanitize_sort_key(db->sort_key, 0);
    if (ka < kb)
        return -1;
    if (ka > kb)
        return 1;
    if (da->stable_sort_id < db->stable_sort_id)
        return -1;
    if (da->stable_sort_id > db->stable_sort_id)
        return 1;
    if (da->enqueue_index < db->enqueue_index)
        return -1;
    if (da->enqueue_index > db->enqueue_index)
        return 1;
    return 0;
}

typedef int (*canvas3d_draw_compare_fn)(const void *, const void *);

/// @brief Restore a max-heap below @p root according to a total draw comparator.
static void canvas3d_heap_sift_down(deferred_draw_t *items,
                                    int32_t root,
                                    int32_t count,
                                    canvas3d_draw_compare_fn compare) {
    for (;;) {
        int32_t child = root * 2 + 1;
        if (child >= count)
            return;
        if (child + 1 < count && compare(&items[child], &items[child + 1]) < 0)
            child++;
        if (compare(&items[root], &items[child]) >= 0)
            return;
        deferred_draw_t tmp = items[root];
        items[root] = items[child];
        items[child] = tmp;
        root = child;
    }
}

/// @brief Allocation-free O(n log n) fallback for deferred ordering under scratch OOM.
/// @details The depth/identity/enqueue comparator is a total order, so the in-place heap result is
/// deterministic even though heapsort itself is not stable. This replaces the former O(n²)
/// repeated-selection fallback and lets a recorded frame finish without more memory.
static void canvas3d_heap_sort_draws(deferred_draw_t *items,
                                     int32_t count,
                                     canvas3d_draw_compare_fn compare) {
    if (!items || count <= 1 || !compare)
        return;
    for (int32_t root = count / 2; root-- > 0;)
        canvas3d_heap_sift_down(items, root, count, compare);
    for (int32_t end = count - 1; end > 0; --end) {
        deferred_draw_t tmp = items[0];
        items[0] = items[end];
        items[end] = tmp;
        canvas3d_heap_sift_down(items, 0, end, compare);
    }
}

#define CANVAS3D_SORT_DEPTH_BUCKETS 4096

typedef uint64_t (*canvas3d_sort_key_fn)(const deferred_draw_t *draw, void *ctx);

/// @brief Extract a queued draw's stable identity for radix sorting.
/// @param draw Borrowed deferred draw.
/// @param ctx Unused key-function context.
/// @return Stable sort identity widened to 64 bits, or zero for invalid input.
static uint64_t canvas3d_sort_key_stable_id(const deferred_draw_t *draw, void *ctx) {
    (void)ctx;
    return draw ? (uint64_t)draw->stable_sort_id : 0;
}

/// @brief Extract a queued draw's backend-state grouping key for radix sorting.
/// @param draw Borrowed deferred draw.
/// @param ctx Unused key-function context.
/// @return Process-local backend-state key.
static uint64_t canvas3d_sort_key_backend_state(const deferred_draw_t *draw, void *ctx) {
    (void)ctx;
    return canvas3d_deferred_backend_state_key(draw);
}

/// @brief Grow the cached radix-key scratch arrays that ride along with sort_cmds.
/// @param c Canvas owning both key arrays.
/// @param needed Positive required element capacity.
/// @return Non-zero when both arrays have sufficient capacity; zero on invalid input, overflow, or
/// allocation failure.
static int canvas3d_ensure_sort_key_capacity(rt_canvas3d *c, int32_t needed) {
    uint64_t *keys;
    uint64_t *scratch;
    int32_t capacity;
    if (!c || needed <= 0)
        return 0;
    if (c->sort_key_capacity >= needed && c->sort_keys && c->sort_keys_scratch)
        return 1;
    capacity = c->sort_key_capacity > 0 ? c->sort_key_capacity : 64;
    while (capacity < needed) {
        if (capacity > INT32_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof(uint64_t))
        return 0;
    keys = (uint64_t *)malloc((size_t)capacity * sizeof(*keys));
    scratch = (uint64_t *)malloc((size_t)capacity * sizeof(*scratch));
    if (!keys || !scratch) {
        free(keys);
        free(scratch);
        return 0;
    }
    free(c->sort_keys);
    free(c->sort_keys_scratch);
    c->sort_keys = keys;
    c->sort_keys_scratch = scratch;
    c->sort_key_capacity = capacity;
    return 1;
}

/// @brief Stable-sort deferred draws by a cached unsigned 64-bit key using byte-wise radix passes.
/// @details The key function is evaluated exactly once per item. Scratch arrays are canvas-owned
/// and retained across frames; allocation failure leaves the caller to use a fallback ordering
/// path.
/// @param c Canvas owning command and key scratch storage.
/// @param[in,out] items Deferred-draw array sorted in place.
/// @param count Number of valid entries.
/// @param key_fn Non-NULL key extractor.
/// @param ctx Opaque context forwarded to @p key_fn.
/// @return Non-zero on success or trivial input; zero only when scratch growth fails.
static int canvas3d_stable_radix_sort_u64(
    rt_canvas3d *c, deferred_draw_t *items, int32_t count, canvas3d_sort_key_fn key_fn, void *ctx) {
    deferred_draw_t *scratch;
    uint64_t *keys;
    uint64_t *keys_scratch;

    if (!c || !items || count <= 1 || !key_fn)
        return 1;
    if (!ensure_deferred_capacity(&c->sort_cmds, &c->sort_capacity, count))
        return 0;
    if (!canvas3d_ensure_sort_key_capacity(c, count))
        return 0;
    scratch = (deferred_draw_t *)c->sort_cmds;
    keys = c->sort_keys;
    keys_scratch = c->sort_keys_scratch;
    /* The key function can be arbitrarily expensive (the backend-state key hashes ~40 material
     * fields), so evaluate it exactly once per draw and radix over the cached values. */
    for (int32_t i = 0; i < count; i++)
        keys[i] = key_fn(&items[i], ctx);
    for (int shift = 0; shift < 64; shift += 8) {
        int32_t counts[256] = {0};
        int32_t offsets[256];

        for (int32_t i = 0; i < count; i++)
            counts[(keys[i] >> shift) & 0xFFu]++;
        /* Every key shares this byte (common for high bytes): the pass is a no-op. */
        if (counts[(keys[0] >> shift) & 0xFFu] == count)
            continue;
        offsets[0] = 0;
        for (int i = 1; i < 256; i++)
            offsets[i] = offsets[i - 1] + counts[i - 1];
        for (int32_t i = 0; i < count; i++) {
            uint32_t bucket = (uint32_t)((keys[i] >> shift) & 0xFFu);
            int32_t dst = offsets[bucket]++;
            scratch[dst] = items[i];
            keys_scratch[dst] = keys[i];
        }
        memcpy(items, scratch, (size_t)count * sizeof(*items));
        memcpy(keys, keys_scratch, (size_t)count * sizeof(*keys));
    }
    c->frame_sort_passes++;
    return 1;
}

/// @brief Quantize a finite depth key into the fixed deferred-sort bucket range.
/// @param key Candidate depth key; non-finite input maps to @p min_key.
/// @param min_key Minimum finite depth in the current draw set.
/// @param max_key Maximum finite depth in the current draw set.
/// @return Clamped zero-based bucket index.
static int canvas3d_depth_bucket_for_key(float key, float min_key, float max_key) {
    double t;
    int bucket;

    if (!isfinite(key))
        key = min_key;
    if (max_key <= min_key)
        return 0;
    t = ((double)key - (double)min_key) / ((double)max_key - (double)min_key);
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    bucket = (int)(t * (double)(CANVAS3D_SORT_DEPTH_BUCKETS - 1) + 0.5);
    if (bucket < 0)
        return 0;
    if (bucket >= CANVAS3D_SORT_DEPTH_BUCKETS)
        return CANVAS3D_SORT_DEPTH_BUCKETS - 1;
    return bucket;
}

/// @brief Compare exact depth tuples inside one quantized bucket.
/// @param a Borrowed candidate draw.
/// @param b Borrowed reference draw.
/// @param descending Non-zero for back-to-front order.
/// @param requires_blend Selects transparent versus opaque non-finite-key sanitization.
/// @return Non-zero when @p a belongs before @p b.
static int canvas3d_depth_tuple_before(const deferred_draw_t *a,
                                       const deferred_draw_t *b,
                                       int descending,
                                       int requires_blend) {
    float a_key;
    float b_key;

    if (!a || !b)
        return 0;
    a_key = canvas3d_sanitize_sort_key(a->sort_key, requires_blend);
    b_key = canvas3d_sanitize_sort_key(b->sort_key, requires_blend);
    if (descending) {
        if (a_key > b_key)
            return 1;
        if (a_key < b_key)
            return 0;
    } else {
        if (a_key < b_key)
            return 1;
        if (a_key > b_key)
            return 0;
    }
    if (a->stable_sort_id < b->stable_sort_id)
        return 1;
    if (a->stable_sort_id > b->stable_sort_id)
        return 0;
    return a->enqueue_index < b->enqueue_index;
}

/// @brief Stably insertion-sort one small depth bucket by exact depth and identity tuple.
/// @param[in,out] items Bucket entries to reorder in place.
/// @param count Number of entries.
/// @param descending Non-zero for back-to-front order.
/// @param requires_blend Selects transparent versus opaque key sanitization.
static void canvas3d_insertion_sort_depth_exact(deferred_draw_t *items,
                                                int32_t count,
                                                int descending,
                                                int requires_blend) {
    if (!items || count <= 1)
        return;
    for (int32_t i = 1; i < count; i++) {
        deferred_draw_t value = items[i];
        int32_t j = i;
        while (j > 0 &&
               canvas3d_depth_tuple_before(&value, &items[j - 1], descending, requires_blend)) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = value;
    }
}

/// @brief Stable-sort deferred draws by quantized depth, then exact-sort collisions per bucket.
/// @details Callers first radix-sort by stable identity so equal-depth input remains deterministic.
/// @param c Canvas owning scratch command storage and frame sort counters.
/// @param[in,out] items Deferred draws sorted in place.
/// @param count Number of entries.
/// @param descending Non-zero for back-to-front bucket traversal.
/// @param requires_blend Selects transparent versus opaque key sanitization.
/// @return Non-zero on success or trivial/equal-key input; zero when scratch allocation fails.
static int canvas3d_stable_bucket_sort_depth(
    rt_canvas3d *c, deferred_draw_t *items, int32_t count, int descending, int requires_blend) {
    deferred_draw_t *scratch;
    int32_t counts[CANVAS3D_SORT_DEPTH_BUCKETS] = {0};
    int32_t starts[CANVAS3D_SORT_DEPTH_BUCKETS];
    int32_t offsets[CANVAS3D_SORT_DEPTH_BUCKETS];
    float min_key = FLT_MAX;
    float max_key = -FLT_MAX;

    if (!c || !items || count <= 1)
        return 1;
    for (int32_t i = 0; i < count; i++) {
        float key = canvas3d_sanitize_sort_key(items[i].sort_key, requires_blend);
        if (key < min_key)
            min_key = key;
        if (key > max_key)
            max_key = key;
    }
    if (min_key == FLT_MAX || max_key == -FLT_MAX || min_key == max_key)
        return 1; /* CONTRACT: equal-key input is returned untouched, so callers MUST run a
                   * stable identity pass (radix by stable_sort_id) first — otherwise output
                   * order for equal depths would depend on caller submission order. */
    if (!ensure_deferred_capacity(&c->sort_cmds, &c->sort_capacity, count))
        return 0;
    scratch = (deferred_draw_t *)c->sort_cmds;
    for (int32_t i = 0; i < count; i++) {
        int bucket = canvas3d_depth_bucket_for_key(
            canvas3d_sanitize_sort_key(items[i].sort_key, requires_blend), min_key, max_key);
        counts[bucket]++;
    }
    if (descending) {
        starts[CANVAS3D_SORT_DEPTH_BUCKETS - 1] = 0;
        for (int i = CANVAS3D_SORT_DEPTH_BUCKETS - 2; i >= 0; i--)
            starts[i] = starts[i + 1] + counts[i + 1];
    } else {
        starts[0] = 0;
        for (int i = 1; i < CANVAS3D_SORT_DEPTH_BUCKETS; i++)
            starts[i] = starts[i - 1] + counts[i - 1];
    }
    memcpy(offsets, starts, sizeof(offsets));
    for (int32_t i = 0; i < count; i++) {
        int bucket = canvas3d_depth_bucket_for_key(
            canvas3d_sanitize_sort_key(items[i].sort_key, requires_blend), min_key, max_key);
        scratch[offsets[bucket]++] = items[i];
    }
    memcpy(items, scratch, (size_t)count * sizeof(*items));
    for (int bucket = 0; bucket < CANVAS3D_SORT_DEPTH_BUCKETS; bucket++) {
        if (counts[bucket] > 1)
            canvas3d_insertion_sort_depth_exact(
                &items[starts[bucket]], counts[bucket], descending, requires_blend);
    }
    c->frame_sort_passes++;
    return 1;
}

/// @brief Deterministically sort transparent draws by identity then exact back-to-front depth.
/// @param c Canvas owning sort scratch storage.
/// @param[in,out] items Transparent deferred draws.
/// @param count Number of entries.
/// @return Non-zero on success; zero on scratch allocation failure.
static int canvas3d_sort_transparent_draws(rt_canvas3d *c, deferred_draw_t *items, int32_t count) {
    if (count <= 1)
        return 1;
    if (!canvas3d_stable_radix_sort_u64(c, items, count, canvas3d_sort_key_stable_id, NULL) ||
        !canvas3d_stable_bucket_sort_depth(c, items, count, 1, 1)) {
        canvas3d_heap_sort_draws(items, count, cmp_back_to_front);
        c->frame_sort_passes++;
    }
    return 1;
}

/// @brief Deterministically sort opaque draws by identity then exact front-to-back depth.
/// @param c Canvas owning sort scratch storage.
/// @param[in,out] items Opaque deferred draws.
/// @param count Number of entries.
/// @return Non-zero on success; zero on scratch allocation failure.
static int canvas3d_sort_opaque_draws_depth_first(rt_canvas3d *c,
                                                  deferred_draw_t *items,
                                                  int32_t count) {
    if (count <= 1)
        return 1;
    if (!canvas3d_stable_radix_sort_u64(c, items, count, canvas3d_sort_key_stable_id, NULL) ||
        !canvas3d_stable_bucket_sort_depth(c, items, count, 0, 0)) {
        canvas3d_heap_sort_draws(items, count, cmp_front_to_back);
        c->frame_sort_passes++;
    }
    return 1;
}

/// @brief Report whether backend-state transition telemetry should treat the canvas as software.
/// @param c Canvas whose selected backend pointer/name is inspected.
/// @return Non-zero for invalid, canonical software, or software-named backends.
static int canvas3d_backend_uses_software_raster(const rt_canvas3d *c) {
    const char *name;

    if (!c || !c->backend)
        return 1;
    if (c->backend == &vgfx3d_software_backend)
        return 1;
    name = c->backend->name;
    return name && strcmp(name, "software") == 0;
}

/*==========================================================================
 * Helpers
 *=========================================================================*/

/// @brief Whether a draw command needs alpha blending (transparency).
///
/// Two cases trigger blending:
///   1. Any workflow with explicit `BLEND` alpha mode.
///   2. Scalar alpha < 1.0 or diffuse alpha < 1.0 after material normalization.
/// Used to route the command into the deferred transparency-sorted
/// pass instead of the immediate opaque pass.
/// @param cmd Borrowed normalized backend draw command.
/// @return 1 when transparent/additive blending is required; otherwise 0.
static int8_t canvas3d_cmd_requires_blend(const vgfx3d_draw_cmd_t *cmd) {
    if (!cmd)
        return 0;
    return (int8_t)(vgfx3d_draw_cmd_uses_transparent_blend(cmd) ? 1 : 0);
}

/// @brief Return whether a draw command changes vertex positions after CPU bounds are computed.
/// @details Skinning, morph targets, and instanced motion snapshots can all move triangles away
///   from the static Mesh3D AABB. These draws remain renderable, but the coarse CPU occlusion grid
///   must avoid treating their bounds as exact solid occluders.
/// @param cmd Borrowed draw command.
/// @return Non-zero when skinning or morphing can move submitted vertices.
static int canvas3d_cmd_has_runtime_deformation(const vgfx3d_draw_cmd_t *cmd) {
    return cmd && ((cmd->bone_palette && cmd->bone_count > 0) ||
                   (cmd->morph_deltas && cmd->morph_weights && cmd->morph_shape_count > 0));
}

/// @brief Populate culling/occlusion safety flags for a queued draw.
/// @details Conservative bounds are fine for frustum tests, but are too loose for AABB-grid
///   occlusion. Alpha masks, double-sided foliage, splatted surfaces, and biased overlays also
///   have screen-space coverage that is not represented by their coarse AABB. Such draws can still
///   be depth-tested normally by the backend; they simply do not become CPU occluders.
/// @param[in,out] dd Deferred draw whose conservative/test/write flags are finalized.
/// @param conservative_bounds Non-zero when the stored AABB is deliberately over-inclusive.
/// @param force_disable_occlusion Non-zero to disable both testing and occluder writes
/// unconditionally.
static void canvas3d_finalize_deferred_visibility_flags(deferred_draw_t *dd,
                                                        int conservative_bounds,
                                                        int force_disable_occlusion) {
    const vgfx3d_draw_cmd_t *cmd;
    if (!dd)
        return;
    cmd = &dd->cmd;
    dd->conservative_bounds = conservative_bounds ? 1 : 0;
    dd->occlusion_test_disabled = force_disable_occlusion ? 1 : 0;
    dd->occlusion_write_disabled = force_disable_occlusion ? 1 : 0;
    if (canvas3d_cmd_has_runtime_deformation(cmd)) {
        dd->conservative_bounds = 1;
        dd->occlusion_test_disabled = 1;
        dd->occlusion_write_disabled = 1;
    }
    if (cmd->disable_depth_test || dd->requires_blend)
        dd->occlusion_test_disabled = 1;
    if (cmd->disable_depth_test || dd->requires_blend ||
        cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK || cmd->double_sided || cmd->has_splat ||
        fabsf(cmd->depth_bias) > 1e-8f || fabsf(cmd->slope_scaled_depth_bias) > 1e-8f)
        dd->occlusion_write_disabled = 1;
}

/// @brief Resolve the effective backface-cull flag for a material draw.
///
/// AND of canvas-level backface cull AND material isn't `double_sided`.
/// Materials marked double-sided (e.g., leaves, fabric) override the
/// canvas setting to disable culling per-draw.
/// @param c Borrowed canvas providing the global culling switch.
/// @param mat Optional borrowed material providing the two-sided override.
/// @return 1 only when canvas culling is on and the material is not double-sided.
static int8_t canvas3d_material_backface_cull(const rt_canvas3d *c, const rt_material3d *mat) {
    if (!c)
        return 0;
    return (int8_t)(c->backface_cull && !(mat && mat->double_sided));
}

/// @brief Return 1 only when every vertex in the mesh has a finite non-zero tangent.
/// @details A partially-authored tangent stream is not safe for normal maps: accepting a
///   single non-zero tangent would leave the rest of the mesh with a broken basis. In
///   that case draw submission generates tangents on a queued geometry snapshot.
/// @param mesh Borrowed mesh whose entire tangent stream is inspected.
/// @return 1 when every vertex has a finite non-zero tangent and finite handedness; otherwise 0.
static int canvas3d_mesh_has_complete_tangents(const rt_mesh3d *mesh) {
    if (!mesh || !mesh->vertices)
        return 0;
    if (mesh->vertex_count == 0)
        return 0;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        const float *t = mesh->vertices[i].tangent;
        float len2 = t[0] * t[0] + t[1] * t[1] + t[2] * t[2];
        if (!isfinite(len2) || len2 <= 1e-8f || !isfinite(t[3]))
            return 0;
    }
    return 1;
}

/// @brief Return 1 when a normal-mapped draw needs generated tangents.
/// @details Draw submission must not mutate the caller's mesh. If authored tangents
///   are already present, mark the source mesh cache state. If tangents must be
///   generated, callers snapshot the geometry and generate them on that copy.
/// @param mesh Borrowed mutable mesh whose tangent-cache metadata may be validated.
/// @param mat Borrowed material whose normal-map slots determine whether tangents are needed.
/// @return 1 when the queued snapshot must generate tangents; otherwise 0.
static int canvas3d_prepare_normal_map_tangent_state(rt_mesh3d *mesh, const rt_material3d *mat) {
    if (!mesh || !mat ||
        (!rt_material3d_resolve_texture_pixels(mat->normal_map) &&
         !rt_material3d_resolve_texture_native_asset(mat->normal_map)))
        return 0;
    if (mesh->tangents_ready && mesh->tangent_revision == mesh->geometry_revision)
        return 0;
    if (canvas3d_mesh_has_complete_tangents(mesh)) {
        mesh->tangents_ready = 1;
        mesh->tangent_revision = mesh->geometry_revision;
        return 0;
    }
    return 1;
}

/// @brief Generate tangents for a snapshot copy without touching the source mesh.
/// @param source Borrowed source mesh providing counts, revision, and bounds metadata.
/// @param[in,out] vertices Mutable snapshotted vertex array receiving tangents.
/// @param indices Mutable snapshotted triangle-index array used by tangent generation.
/// @return 1 when the transient facade produced a complete tangent stream; otherwise 0.
static int canvas3d_generate_snapshot_tangents(const rt_mesh3d *source,
                                               vgfx3d_vertex_t *vertices,
                                               uint32_t *indices) {
    rt_mesh3d temp;
    uint32_t vertex_count = rt_mesh3d_safe_vertex_count(source);
    uint32_t index_count = rt_mesh3d_safe_index_count(source);
    if (!source || !vertices || !indices || vertex_count == 0 || index_count == 0)
        return 0;
    memset(&temp, 0, sizeof(temp));
    temp.vertices = vertices;
    temp.vertex_count = vertex_count;
    temp.vertex_capacity = vertex_count;
    temp.indices = indices;
    temp.index_count = index_count;
    temp.index_capacity = index_count;
    temp.geometry_revision = source->geometry_revision ? source->geometry_revision : 1u;
    temp.transient_geometry_facade = 1;
    temp.bounds_dirty = source->bounds_dirty;
    memcpy(temp.aabb_min, source->aabb_min, sizeof(temp.aabb_min));
    memcpy(temp.aabb_max, source->aabb_max, sizeof(temp.aabb_max));
    temp.bsphere_radius = source->bsphere_radius;
    rt_mesh3d_calc_tangents_impl(&temp);
    return temp.tangents_ready ? 1 : 0;
}

/// @brief Stamp @p cmd with a mesh's cached validated index count when it matches the command.
/// @details Backend validation still rescans unmarked commands. This helper only marks commands
///   backed by already-validated Mesh3D geometry, letting repeated static draws avoid a per-pass
///   O(index_count) range scan while preserving the old safety contract for transient callers.
/// @param[in,out] cmd Draw command whose validated count may be stamped.
/// @param mesh Borrowed source mesh providing cached validation state.
static void canvas3d_mark_cmd_validated_indices(vgfx3d_draw_cmd_t *cmd, rt_mesh3d *mesh) {
    uint32_t validated;
    if (!cmd || !mesh)
        return;
    validated = rt_mesh3d_validated_index_count(mesh);
    if (validated == cmd->index_count)
        cmd->validated_index_count = validated;
}

/// @brief Disable normal-map sampling on a draw command when no reliable tangent basis exists.
/// @details Generated tangents are produced on a queued geometry snapshot so caller meshes are not
///   mutated. If snapshot allocation or tangent generation fails, rendering the base material is
///   more stable than dropping the whole draw or sampling a normal map with zero/partial tangents.
/// @param[in,out] cmd Draw command whose normal-map references and scale are cleared.
static void canvas3d_disable_cmd_normal_map(vgfx3d_draw_cmd_t *cmd) {
    if (!cmd)
        return;
    cmd->normal_map = NULL;
    cmd->normal_map_asset = NULL;
    cmd->normal_scale = 0.0f;
}

/// @brief Coerce a texture-wrap value to a known mode, defaulting unknown values to REPEAT.
/// @param value Candidate `RT_MATERIAL3D_TEXTURE_WRAP_*` value.
/// @return Backend-safe wrap mode.
static int32_t canvas3d_sanitize_material_wrap(int32_t value) {
    if (value == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE ||
        value == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
        return value;
    return RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
}

/// @brief Coerce a texture-filter value to NEAREST or LINEAR (default LINEAR).
/// @param value Candidate `RT_MATERIAL3D_TEXTURE_FILTER_*` value.
/// @return Nearest when explicitly requested; otherwise linear.
static int32_t canvas3d_sanitize_material_filter(int32_t value) {
    return value == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                                                         : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
}

/// @brief Coerce a material mip-filter value to none, nearest, or linear.
/// @param value Candidate `RT_MATERIAL3D_TEXTURE_MIP_FILTER_*` value.
/// @return A backend-safe mip-selection filter; unknown values disable mip sampling.
static int32_t canvas3d_sanitize_material_mip_filter(int32_t value) {
    if (value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST)
        return RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST;
    if (value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR)
        return RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR;
    return RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE;
}

/// @brief Clamp requested texture anisotropy to the cross-backend public [1,16] range.
/// @param value Requested anisotropy ratio.
/// @return Clamped integer ratio from 1 through 16.
static int32_t canvas3d_sanitize_material_anisotropy(int32_t value) {
    if (value < 1)
        return 1;
    if (value > 16)
        return 16;
    return value;
}

/// @brief Clamp a double into [lo, hi] and narrow to float; returns `fallback` on
///   non-finite input or an inverted (lo > hi) range.
/// @param value Double-precision material scalar.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @param fallback Float returned for non-finite input or invalid bounds.
/// @return Safely narrowed clamped value, or @p fallback.
static float canvas3d_clamp_material_f64(double value, double lo, double hi, float fallback) {
    if (!isfinite(value) || lo > hi)
        return fallback;
    if (value < lo)
        value = lo;
    if (value > hi)
        value = hi;
    return (float)value;
}

/// @brief Sanitize one UV-transform component into float range; scale lanes (0, 3)
///   default to 1.0 and offset lanes (1, 2) to 0.0 on non-finite input.
/// @param value Double-precision UV transform component.
/// @param component Zero-based component index in the six-lane affine transform.
/// @return Finite float component clamped to the material UV range.
static float canvas3d_material_uv_value(double value, int32_t component) {
    float fallback = (component == 0 || component == 3) ? 1.0f : 0.0f;
    return canvas3d_clamp_material_f64(
        value, -CANVAS3D_MATERIAL_UV_ABS_MAX, CANVAS3D_MATERIAL_UV_ABS_MAX, fallback);
}

/// @brief Clamp a terrain splat-layer UV scale to (0, CANVAS3D_MATERIAL_UV_ABS_MAX],
///   defaulting non-positive or non-finite values to 1.0.
/// @param value Requested splat-layer scale.
/// @return Finite positive backend-safe scale.
static float canvas3d_sanitize_splat_layer_scale(float value) {
    if (!isfinite(value) || value <= 0.0f)
        return 1.0f;
    if (value > (float)CANVAS3D_MATERIAL_UV_ABS_MAX)
        return (float)CANVAS3D_MATERIAL_UV_ABS_MAX;
    return value;
}

/// @brief Return generation-cached alpha classification for a live Pixels object.
/// @details Invalid handles and empty buffers are opaque. A valid image is scanned at most once
///   per content generation by the Pixels-owned cache, yielding opaque, binary cutout, or
///   fractional blend routing without a second per-draw texel pass.
/// @param pixels_ref Candidate live Pixels object.
/// @return Cached opaque, binary, or fractional alpha classification.
static rt_pixels_alpha_classification canvas3d_pixels_alpha_classification(void *pixels_ref) {
    rt_pixels_impl *pixels = rt_pixels_checked_impl_or_null(pixels_ref);

    return rt_pixels_alpha_classification_cached(pixels);
}

/// @brief Classify a material texture source for opaque, cutout, or blended deferred routing.
/// @details Raw Pixels and a TextureAsset3D's decoded resident fallback use their generation cache.
///   Exact asset metadata can prove an otherwise native-only asset opaque; known-alpha and unknown
///   native-only assets conservatively use binary cutout because metadata does not encode whether
///   their alpha is fractional. This preserves foliage depth behavior without rescanning texels.
/// @param texture_ref Candidate Pixels or TextureAsset3D source.
/// @return Conservative alpha classification used for deferred routing.
static rt_pixels_alpha_classification canvas3d_texture_source_alpha_classification(
    void *texture_ref) {
    void *pixels;
    rt_pixels_alpha_classification classification;

    if (!texture_ref)
        return RT_PIXELS_ALPHA_OPAQUE;
    classification = canvas3d_pixels_alpha_classification(texture_ref);
    if (rt_pixels_checked_impl_or_null(texture_ref))
        return classification;
    if (!rt_g3d_has_class(texture_ref, RT_G3D_TEXTUREASSET3D_CLASS_ID))
        return RT_PIXELS_ALPHA_OPAQUE;
    pixels = rt_textureasset3d_get_pixels(texture_ref);
    classification = canvas3d_pixels_alpha_classification(pixels);
    if (classification != RT_PIXELS_ALPHA_OPAQUE)
        return classification;
    if (rt_textureasset3d_alpha_metadata_known(texture_ref))
        return rt_textureasset3d_has_alpha_texels(texture_ref) ? RT_PIXELS_ALPHA_BINARY
                                                               : RT_PIXELS_ALPHA_OPAQUE;
    return RT_PIXELS_ALPHA_BINARY;
}

/// @brief Capture a TextureAsset3D native cache key and mip window into a draw command slot.
/// @details Backends use these draw-time values while uploading native compressed textures, so a
///          later TextureAsset3D residency change in the same frame cannot change what this queued
///          draw submits.
/// @param[in,out] cmd Draw command receiving cache-key and resident-mip fields.
/// @param slot Zero-based material texture slot.
/// @param asset Candidate TextureAsset3D; invalid values leave the slot zeroed.
static void canvas3d_capture_texture_asset_snapshot(vgfx3d_draw_cmd_t *cmd,
                                                    int32_t slot,
                                                    void *asset) {
    if (!cmd || slot < 0 || slot >= RT_MATERIAL3D_TEXTURE_SLOT_COUNT)
        return;
    cmd->texture_asset_cache_key[slot] = 0;
    cmd->texture_asset_mip_start[slot] = 0;
    cmd->texture_asset_mip_count[slot] = 0;
    if (!rt_g3d_has_class(asset, RT_G3D_TEXTUREASSET3D_CLASS_ID))
        return;
    cmd->texture_asset_cache_key[slot] = rt_textureasset3d_get_native_cache_key(asset);
    cmd->texture_asset_mip_start[slot] = rt_textureasset3d_get_resident_mip_start(asset);
    cmd->texture_asset_mip_count[slot] = rt_textureasset3d_get_resident_mip_count(asset);
}

/// @brief Translate every material field into the corresponding draw-command field.
///
/// Per-vertex draw commands are float-typed (matches GPU expectations),
/// materials are double-typed (matches Zia's number type). This helper
/// performs the narrowing conversion plus the material→command name
/// remapping (e.g., `mat->shininess` → `cmd->shininess`).
/// @param mat Borrowed material to snapshot and sanitize.
/// @param[out] cmd Draw command receiving every backend-visible material field.
/// @brief True when the canvas backend samples RenderTarget3D colour natively (ADR 0299).
/// @param c Borrowed canvas; NULL or a backend without the feature hook reads as unsupported.
/// @return Non-zero when RT-bound material slots may skip the CPU mirror.
static int canvas3d_backend_samples_render_targets(const rt_canvas3d *c) {
    if (!c || !c->backend || !c->backend->get_feature_caps)
        return 0;
    return (c->backend->get_feature_caps(c->backend_ctx) &
            RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET_SAMPLING) != 0;
}

/// @brief Resolve one material texture slot into its draw-command sources.
/// @details A RenderTarget3D reference becomes a native target pointer when the backend can
///          sample it directly (the Pixels fallback stays NULL so no mirror readback runs);
///          every other reference, and RTs on mirror-only backends, resolve to Pixels.
/// @param texture_ref Nullable Pixels, TextureAsset3D, or RenderTarget3D handle.
/// @param native_rt Non-zero when the backend advertises native RT sampling.
/// @param out_pixels Receives the borrowed Pixels fallback or NULL.
/// @param out_target Receives the borrowed native target or NULL.
static void canvas3d_resolve_material_slot(void *texture_ref,
                                           int native_rt,
                                           const void **out_pixels,
                                           const vgfx3d_rendertarget_t **out_target,
                                           void **out_target_owner) {
    *out_target = NULL;
    *out_target_owner = NULL;
    if (native_rt && texture_ref && rt_g3d_has_class(texture_ref, RT_G3D_RENDERTARGET3D_CLASS_ID)) {
        const rt_rendertarget3d *rtd = (const rt_rendertarget3d *)texture_ref;
        if (rtd->target) {
            *out_target = rtd->target;
            *out_target_owner = texture_ref;
            *out_pixels = NULL;
            return;
        }
    }
    *out_pixels = rt_material3d_resolve_texture_pixels(texture_ref);
}

static void canvas3d_fill_material_cmd(const rt_canvas3d *c,
                                       const rt_material3d *mat,
                                       vgfx3d_draw_cmd_t *cmd) {
    rt_pixels_alpha_classification alpha_classification;
    int native_rt;
    if (!mat || !cmd)
        return;
    native_rt = canvas3d_backend_samples_render_targets(c);

    cmd->diffuse_color[0] = canvas3d_clamp01_f64(mat->diffuse[0]);
    cmd->diffuse_color[1] = canvas3d_clamp01_f64(mat->diffuse[1]);
    cmd->diffuse_color[2] = canvas3d_clamp01_f64(mat->diffuse[2]);
    cmd->diffuse_color[3] = canvas3d_clamp_material_f64(mat->diffuse[3], 0.0, 1.0, 1.0f);
    cmd->specular[0] = canvas3d_clamp01_f64(mat->specular[0]);
    cmd->specular[1] = canvas3d_clamp01_f64(mat->specular[1]);
    cmd->specular[2] = canvas3d_clamp01_f64(mat->specular[2]);
    cmd->shininess =
        canvas3d_clamp_material_f64(mat->shininess, 0.0, CANVAS3D_MATERIAL_SHININESS_MAX, 32.0f);
    cmd->alpha = canvas3d_clamp_material_f64(mat->alpha, 0.0, 1.0, 1.0f);
    cmd->unlit = (int8_t)(mat->unlit || mat->shading_model == 3);
    canvas3d_resolve_material_slot(
        mat->texture, native_rt, &cmd->texture, &cmd->texture_target, &cmd->texture_target_owner);
    cmd->normal_map = rt_material3d_resolve_texture_pixels(mat->normal_map);
    cmd->specular_map = rt_material3d_resolve_texture_pixels(mat->specular_map);
    canvas3d_resolve_material_slot(mat->emissive_map,
                                   native_rt,
                                   &cmd->emissive_map,
                                   &cmd->emissive_map_target,
                                   &cmd->emissive_map_target_owner);
    cmd->texture_asset = rt_material3d_resolve_texture_native_asset(mat->texture);
    cmd->normal_map_asset = rt_material3d_resolve_texture_native_asset(mat->normal_map);
    cmd->specular_map_asset = rt_material3d_resolve_texture_native_asset(mat->specular_map);
    cmd->emissive_map_asset = rt_material3d_resolve_texture_native_asset(mat->emissive_map);
    canvas3d_capture_texture_asset_snapshot(
        cmd, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR, cmd->texture_asset);
    canvas3d_capture_texture_asset_snapshot(
        cmd, RT_MATERIAL3D_TEXTURE_SLOT_NORMAL, cmd->normal_map_asset);
    canvas3d_capture_texture_asset_snapshot(
        cmd, RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR, cmd->specular_map_asset);
    canvas3d_capture_texture_asset_snapshot(
        cmd, RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE, cmd->emissive_map_asset);
    cmd->metallic_roughness_map = rt_material3d_resolve_texture_pixels(mat->metallic_roughness_map);
    cmd->ao_map = rt_material3d_resolve_texture_pixels(mat->ao_map);
    cmd->lightmap = rt_material3d_resolve_texture_pixels(mat->lightmap);
    cmd->metallic_roughness_map_asset =
        rt_material3d_resolve_texture_native_asset(mat->metallic_roughness_map);
    cmd->ao_map_asset = rt_material3d_resolve_texture_native_asset(mat->ao_map);
    canvas3d_capture_texture_asset_snapshot(
        cmd, RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS, cmd->metallic_roughness_map_asset);
    canvas3d_capture_texture_asset_snapshot(cmd, RT_MATERIAL3D_TEXTURE_SLOT_AO, cmd->ao_map_asset);
    cmd->emissive_color[0] = canvas3d_clamp_material_f64(
        mat->emissive[0], 0.0, CANVAS3D_MATERIAL_EMISSIVE_COLOR_MAX, 0.0f);
    cmd->emissive_color[1] = canvas3d_clamp_material_f64(
        mat->emissive[1], 0.0, CANVAS3D_MATERIAL_EMISSIVE_COLOR_MAX, 0.0f);
    cmd->emissive_color[2] = canvas3d_clamp_material_f64(
        mat->emissive[2], 0.0, CANVAS3D_MATERIAL_EMISSIVE_COLOR_MAX, 0.0f);
    cmd->metallic = canvas3d_clamp01_f64(mat->metallic);
    cmd->roughness = canvas3d_clamp_material_f64(mat->roughness, 0.0, 1.0, 0.5f);
    cmd->ao = canvas3d_clamp_material_f64(mat->ao, 0.0, 1.0, 1.0f);
    cmd->emissive_intensity = canvas3d_clamp_material_f64(
        mat->emissive_intensity, 0.0, CANVAS3D_MATERIAL_EMISSIVE_INTENSITY_MAX, 1.0f);
    cmd->normal_scale = canvas3d_clamp_material_f64(
        mat->normal_scale, 0.0, CANVAS3D_MATERIAL_NORMAL_SCALE_MAX, 1.0f);
    cmd->additive_blend = mat->additive_blend ? 1 : 0;
    cmd->workflow = (mat->workflow == 1) ? 1 : 0;
    cmd->alpha_mode = (mat->alpha_mode >= 0 && mat->alpha_mode <= 2) ? mat->alpha_mode : 0;
    alpha_classification = canvas3d_texture_source_alpha_classification(mat->texture);
    cmd->has_alpha_texture = alpha_classification != RT_PIXELS_ALPHA_OPAQUE ? 1 : 0;
    if (cmd->has_alpha_texture && cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE &&
        !mat->alpha_mode_explicit) {
        /* Auto-promotion is a convenience for DEFAULT materials whose texture
         * carries alpha. An explicit SetAlphaMode(Opaque) must win: imported
         * baseColor textures often ship a soft alpha channel on genuinely
         * opaque surfaces, and the silent BLEND promotion also removed those
         * draws from the shadow pass (no player ever cast a shadow). */
        cmd->alpha_mode = alpha_classification == RT_PIXELS_ALPHA_FRACTIONAL
                              ? RT_MATERIAL3D_ALPHA_MODE_BLEND
                              : RT_MATERIAL3D_ALPHA_MODE_MASK;
    }
    if (cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR && cmd->alpha < 0.999f &&
        cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE)
        cmd->alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
    cmd->alpha_cutoff = canvas3d_clamp_material_f64(mat->alpha_cutoff, 0.0, 1.0, 0.5f);
    cmd->shadow_mode = (mat->shadow_mode >= RT_MATERIAL3D_SHADOW_MODE_AUTO &&
                        mat->shadow_mode <= RT_MATERIAL3D_SHADOW_MODE_CAST)
                           ? mat->shadow_mode
                           : RT_MATERIAL3D_SHADOW_MODE_AUTO;
    cmd->double_sided = mat->double_sided ? 1 : 0;
    cmd->texture_wrap_s = canvas3d_sanitize_material_wrap(mat->texture_wrap_s);
    cmd->texture_wrap_t = canvas3d_sanitize_material_wrap(mat->texture_wrap_t);
    cmd->texture_filter = canvas3d_sanitize_material_filter(mat->texture_filter);
    cmd->texture_min_filter = canvas3d_sanitize_material_filter(mat->texture_min_filter);
    cmd->texture_mag_filter = canvas3d_sanitize_material_filter(mat->texture_mag_filter);
    cmd->texture_mip_filter = canvas3d_sanitize_material_mip_filter(mat->texture_mip_filter);
    cmd->texture_anisotropy = canvas3d_sanitize_material_anisotropy(mat->anisotropy);
    for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
        cmd->texture_slot_wrap_s[slot] =
            canvas3d_sanitize_material_wrap(mat->texture_slot_wrap_s[slot]);
        cmd->texture_slot_wrap_t[slot] =
            canvas3d_sanitize_material_wrap(mat->texture_slot_wrap_t[slot]);
        cmd->texture_slot_filter[slot] =
            canvas3d_sanitize_material_filter(mat->texture_slot_filter[slot]);
        cmd->texture_slot_min_filter[slot] =
            canvas3d_sanitize_material_filter(mat->texture_slot_min_filter[slot]);
        cmd->texture_slot_mag_filter[slot] =
            canvas3d_sanitize_material_filter(mat->texture_slot_mag_filter[slot]);
        cmd->texture_slot_mip_filter[slot] =
            canvas3d_sanitize_material_mip_filter(mat->texture_slot_mip_filter[slot]);
        cmd->texture_slot_anisotropy[slot] =
            canvas3d_sanitize_material_anisotropy(mat->texture_slot_anisotropy[slot]);
        cmd->texture_slot_uv_set[slot] = mat->texture_slot_uv_set[slot] > 0 ? 1 : 0;
        for (int i = 0; i < 6; i++)
            cmd->texture_slot_uv_transform[slot][i] =
                canvas3d_material_uv_value(mat->texture_slot_uv_transform[slot][i], i);
    }
    cmd->env_map = rt_cubemap3d_is_complete(mat->env_map) ? mat->env_map : NULL;
    cmd->reflectivity = cmd->env_map ? canvas3d_clamp01_f64(mat->reflectivity) : 0.0f;
    cmd->shading_model =
        (mat->shading_model >= 0 && mat->shading_model <= 5) ? mat->shading_model : 0;
    for (int pi = 0; pi < (int)(sizeof(cmd->custom_params) / sizeof(cmd->custom_params[0])); pi++)
        cmd->custom_params[pi] =
            canvas3d_clamp_material_f64(mat->custom_params[pi],
                                        -CANVAS3D_MATERIAL_CUSTOM_PARAM_ABS_MAX,
                                        CANVAS3D_MATERIAL_CUSTOM_PARAM_ABS_MAX,
                                        0.0f);
    cmd->depth_bias = canvas3d_clamp_material_f64(mat->depth_bias,
                                                  -CANVAS3D_MATERIAL_DEPTH_BIAS_ABS_MAX,
                                                  CANVAS3D_MATERIAL_DEPTH_BIAS_ABS_MAX,
                                                  0.0f);
    cmd->slope_scaled_depth_bias =
        canvas3d_clamp_material_f64(mat->slope_scaled_depth_bias,
                                    -CANVAS3D_MATERIAL_SLOPE_DEPTH_BIAS_ABS_MAX,
                                    CANVAS3D_MATERIAL_SLOPE_DEPTH_BIAS_ABS_MAX,
                                    0.0f);
    cmd->soft_particle_fade = canvas3d_clamp_material_f64(mat->soft_fade, 0.0, 1.0e6, 0.0f);
    cmd->ssr_enabled = mat->ssr_enabled ? 1 : 0;
}

/// @brief Bind the canvas IBL environment to a PBR draw that has no explicit
///   material env map.
/// @details Materials with their own env map keep the legacy reflectivity-mix
///   semantics untouched; unlit and Blinn-Phong draws never participate. The
///   flag tells backends to light the ambient term from SH irradiance plus the
///   skybox's prefiltered specular chain instead of the flat ambient color.
/// @param c Canvas providing IBL enablement and complete skybox resources.
/// @param[in,out] cmd PBR draw command whose environment fields may be filled.
static void canvas3d_apply_ibl_env(rt_canvas3d *c, vgfx3d_draw_cmd_t *cmd) {
    if (!c || !cmd)
        return;
    cmd->ibl_env = 0;
    if (!c->ibl_enabled || cmd->unlit || cmd->workflow != RT_MATERIAL3D_WORKFLOW_PBR)
        return;
    if (cmd->env_map) /* explicit material env map keeps legacy semantics */
        return;
    if (!c->skybox || !rt_cubemap3d_ensure_ibl(c->skybox))
        return;
    cmd->env_map = c->skybox;
    cmd->ibl_env = 1;
}

/// @brief Grow the motion-history table to hold `needed` entries.
///
/// Motion history is keyed by mesh-identity pointer and stores the
/// previous-frame model matrix for motion-blur / TAA. Geometric
/// growth starting at 32 entries.
// Motion-history (per-object previous-frame model matrices for motion vectors,
// with an open-addressing hash index) lives in rt_canvas3d_motion.c. The
// canvas3d_hash_u64 / canvas3d_next_power_of_two_i32 hash-table utilities moved
// there too (shared via rt_canvas3d_internal.h).

// Light flattening (canvas3d_copy_light_params / canvas3d_active_light_limit /
// build_light_params) lives in rt_canvas3d_lighting.c (shared via
// rt_canvas3d_internal.h).

/// @brief Return luminance-weighted intensity for shadow-caster ranking.
/// @param l Borrowed flattened light parameters.
/// @return Non-negative score for a shadow-requesting light, or -1 for invalid/non-casting input.
static float canvas3d_shadow_light_luminance_score(const vgfx3d_light_params_t *l) {
    float luminance;

    if (!l || !l->casts_shadows)
        return -1.0f;
    luminance = (float)(0.2126 * l->color[0] + 0.7152 * l->color[1] + 0.0722 * l->color[2]);
    return (float)l->intensity * luminance;
}

/// @brief Score a directional light by luminance-weighted intensity.
/// @param l Borrowed flattened light parameters.
/// @return Non-negative directional score, or -1 for another type or invalid input.
static float canvas3d_shadow_directional_light_param_score(const vgfx3d_light_params_t *l) {
    if (!l || l->type != 0)
        return -1.0f;
    return canvas3d_shadow_light_luminance_score(l);
}

/// @brief Score a spot light by intensity, luminance, and proximity to the render-space camera.
/// @param l Borrowed flattened spot-light parameters.
/// @param camera_pos Optional borrowed three-component render-space camera position.
/// @return Positive proximity-weighted score, or -1 for ineligible input.
static float canvas3d_shadow_spot_light_param_score(const vgfx3d_light_params_t *l,
                                                    const float *camera_pos) {
    float base_score;
    float dx;
    float dy;
    float dz;
    float distance;

    if (!l || l->type != 3)
        return -1.0f;
    base_score = canvas3d_shadow_light_luminance_score(l);
    if (base_score <= 0.0f)
        return -1.0f;
    if (!camera_pos)
        return base_score;
    dx = l->position[0] - camera_pos[0];
    dy = l->position[1] - camera_pos[1];
    dz = l->position[2] - camera_pos[2];
    distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!isfinite(distance) || distance < 0.0f)
        distance = 0.0f;
    return base_score / (1.0f + distance);
}

/// @brief Score a shadow-requesting point/area light by luminance and camera proximity.
/// @param l Borrowed flattened point, rectangle-area, or sphere-area light parameters.
/// @param camera_pos Optional borrowed three-component render-space camera position.
/// @return Positive proximity-weighted score, or -1 for ineligible input.
static float canvas3d_shadow_point_light_param_score(const vgfx3d_light_params_t *l,
                                                     const float *camera_pos) {
    float base_score;
    float dx;
    float dy;
    float dz;
    float distance;

    if (!l || (l->type != 1 && l->type != 4 && l->type != 5))
        return -1.0f;
    base_score = canvas3d_shadow_light_luminance_score(l);
    if (base_score <= 0.0f)
        return -1.0f;
    if (!camera_pos)
        return base_score;
    dx = l->position[0] - camera_pos[0];
    dy = l->position[1] - camera_pos[1];
    dz = l->position[2] - camera_pos[2];
    distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!isfinite(distance) || distance < 0.0f)
        distance = 0.0f;
    return base_score / (1.0f + distance);
}

/// @brief Tolerance-based scalar equality for light parameters.
/// @details Uses a *relative* epsilon scaled by the larger magnitude of the two inputs
///          (floored at 1.0 so near-zero values still compare sensibly). This is what
///          keeps shadow-light slot selection stable across frames: if the same light's
///          direction or color wobbles by a few ULPs from floating-point noise, the
///          comparison still says "same light" and the slot assignment doesn't flicker.
/// @param a First scalar.
/// @param b Second scalar.
/// @return Non-zero when the values differ by no more than the relative epsilon.
static int canvas3d_light_param_close(float a, float b) {
    float scale = fmaxf(1.0f, fmaxf(fabsf(a), fabsf(b)));
    return fabsf(a - b) <= 1e-5f * scale;
}

/// @brief Test whether two shadow-light param structs refer to the "same" light
///        for the purpose of shadow-slot de-duplication.
/// @details Compares the fields that define the shadow projection and lighting contribution.
///          Directional lights use direction/color/intensity; spot lights also include position,
///          attenuation, cone, or emitter dimensions. Volume lights never match.
/// @param a Borrowed first flattened light.
/// @param b Borrowed second flattened light.
/// @return Non-zero when both represent the same eligible shadow request.
static int canvas3d_shadow_light_params_match(const vgfx3d_light_params_t *a,
                                              const vgfx3d_light_params_t *b) {
    if (!a || !b || a->type != b->type)
        return 0;
    if ((a->type != 0 && a->type != 1 && a->type != 3 && a->type != 4 && a->type != 5) ||
        !a->casts_shadows || !b->casts_shadows)
        return 0;
    if (a->identity || b->identity)
        return a->identity == b->identity;
    for (int i = 0; i < 3; i++) {
        if (!canvas3d_light_param_close(a->direction[i], b->direction[i]) ||
            !canvas3d_light_param_close(a->color[i], b->color[i]))
            return 0;
    }
    if (!canvas3d_light_param_close(a->intensity, b->intensity))
        return 0;
    if (a->type == 3 || a->type == 1 || a->type == 4 || a->type == 5) {
        for (int i = 0; i < 3; i++) {
            if (!canvas3d_light_param_close(a->position[i], b->position[i]))
                return 0;
        }
        if (!canvas3d_light_param_close(a->attenuation, b->attenuation))
            return 0;
        if (a->type == 1)
            return 1;
        if (a->type == 4)
            return canvas3d_light_param_close(a->width, b->width) &&
                   canvas3d_light_param_close(a->height, b->height);
        if (a->type == 5)
            return canvas3d_light_param_close(a->radius, b->radius);
        return canvas3d_light_param_close(a->inner_cos, b->inner_cos) &&
               canvas3d_light_param_close(a->outer_cos, b->outer_cos);
    }
    return 1;
}

/// @brief Record a distinct shadow-requesting light for drop telemetry.
/// @param[in,out] requests Fixed-capacity array of distinct request snapshots.
/// @param[in,out] request_count Current array population, incremented on insertion.
/// @param light Borrowed candidate light.
/// @param score Eligibility score; non-positive candidates are ignored.
static void canvas3d_note_shadow_request(vgfx3d_light_params_t *requests,
                                         int32_t *request_count,
                                         const vgfx3d_light_params_t *light,
                                         float score) {
    if (!requests || !request_count || !light || score <= 0.0f)
        return;
    for (int32_t i = 0; i < *request_count; i++) {
        if (canvas3d_shadow_light_params_match(light, &requests[i]))
            return;
    }
    if (*request_count < VGFX3D_MAX_LIGHTS)
        requests[(*request_count)++] = *light;
}

/// @brief Insert one distinct shadow-light candidate into a descending-score phase.
/// @details Candidates before @p phase_start are preserved, allowing directional lights to claim
/// slots before local lights. Full arrays replace only a lower-scoring tail entry.
/// @param[in,out] out_lights Ranked output array.
/// @param[in,out] best_scores Score array parallel to @p out_lights.
/// @param phase_start First slot the candidate may occupy.
/// @param max_shadow_lights Output capacity.
/// @param[in,out] count Number of populated slots.
/// @param light Borrowed candidate copied on insertion.
/// @param score Positive ranking score.
static void canvas3d_insert_shadow_light_candidate(vgfx3d_light_params_t *out_lights,
                                                   float *best_scores,
                                                   int32_t phase_start,
                                                   int32_t max_shadow_lights,
                                                   int32_t *count,
                                                   const vgfx3d_light_params_t *light,
                                                   float score) {
    int32_t insert_at;

    if (!out_lights || !best_scores || !count || !light || score <= 0.0f)
        return;
    if (phase_start >= max_shadow_lights)
        return;
    for (int32_t existing = 0; existing < *count; existing++) {
        if (canvas3d_shadow_light_params_match(light, &out_lights[existing]))
            return;
    }
    if (*count >= max_shadow_lights && score <= best_scores[max_shadow_lights - 1])
        return;

    insert_at = *count;
    if (insert_at >= max_shadow_lights)
        insert_at = max_shadow_lights - 1;
    while (insert_at > phase_start && score > best_scores[insert_at - 1]) {
        if (insert_at < max_shadow_lights) {
            best_scores[insert_at] = best_scores[insert_at - 1];
            out_lights[insert_at] = out_lights[insert_at - 1];
        }
        insert_at--;
    }
    if (insert_at < max_shadow_lights) {
        best_scores[insert_at] = score;
        out_lights[insert_at] = *light;
        out_lights[insert_at].shadow_index = -1;
        out_lights[insert_at].shadow_cascade_count = 1;
        out_lights[insert_at].shadow_projection_type = light->type == 3 || light->type == 4
                                                           ? VGFX3D_SHADOW_PROJECTION_PERSPECTIVE
                                                           : VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
        if (*count < max_shadow_lights)
            (*count)++;
    }
}

/// @brief Pick the shadow-casting lights that are actually referenced by opaque draw commands.
/// @details Walks the deferred draw queue and, for every opaque main-pass command, visits the
///          light params the command has accumulated. Directional lights are selected first using
///          the existing luminance-weighted intensity score. Remaining budget is filled by spot
///          lights scored by luminance-weighted intensity divided by camera distance. Duplicate
///          lights are collapsed so one physical light can only claim one shadow slot even when
///          many draws reference it.
///
///          Behaviour difference from the old bind-order-based selection: picks
///          propagate strictly from the draw list, so a bound but unused light can't
///          steal a shadow slot from a bound-and-actually-drawn light. Slot assignment
///          is deterministic per-frame and stable across frames under normal
///          floating-point noise.
///
///          `shadow_index` on unused slots is set to -1 so downstream code can skip
///          them, and the output array is zero-initialized so stale residue from a
///          prior frame can't leak into the shader constants.
/// @param c Borrowed canvas providing the render-space camera position.
/// @param cmds Borrowed deferred draw queue.
/// @param shadow_indices Optional borrowed list selecting queue entries already accepted as
/// casters.
/// @param draw_count Number of commands or selected indices to inspect.
/// @param[out] out_lights Output buffer sized for @p max_shadow_lights.
/// @param max_shadow_lights Caller cap; clamped to `VGFX3D_MAX_SHADOW_LIGHTS`.
/// @param[out] out_request_count Optional destination for the distinct eligible request count,
/// including requests that did not fit the slot budget.
/// @return Number of shadow-light slots populated.
static int32_t canvas3d_select_shadow_lights_from_draws(const rt_canvas3d *c,
                                                        const deferred_draw_t *cmds,
                                                        const int32_t *shadow_indices,
                                                        int32_t draw_count,
                                                        vgfx3d_light_params_t *out_lights,
                                                        int32_t max_shadow_lights,
                                                        int32_t *out_request_count) {
    float best_scores[VGFX3D_MAX_SHADOW_LIGHTS];
    /* Unique shadow-requesting lights seen this frame (for drop telemetry). */
    vgfx3d_light_params_t requests[VGFX3D_MAX_LIGHTS];
    int32_t request_count = 0;
    int32_t count = 0;
    int32_t spot_phase_start;

    if (out_request_count)
        *out_request_count = 0;
    if (!cmds || draw_count <= 0 || !out_lights || max_shadow_lights <= 0)
        return 0;
    if (max_shadow_lights > VGFX3D_MAX_SHADOW_LIGHTS)
        max_shadow_lights = VGFX3D_MAX_SHADOW_LIGHTS;

    for (int32_t i = 0; i < max_shadow_lights; i++) {
        best_scores[i] = -1.0f;
        memset(&out_lights[i], 0, sizeof(out_lights[i]));
        out_lights[i].shadow_index = -1;
    }

    for (int32_t ci = 0; ci < draw_count; ci++) {
        int32_t draw_index = shadow_indices ? shadow_indices[ci] : ci;
        if (draw_index < 0)
            continue;
        if (!shadow_indices && !canvas3d_deferred_casts_shadow(&cmds[draw_index]))
            continue;
        if (!cmds[draw_index].lights)
            continue;
        for (int32_t li = 0; li < cmds[draw_index].light_count; li++) {
            const vgfx3d_light_params_t *light = &cmds[draw_index].lights[li];
            float score = canvas3d_shadow_directional_light_param_score(light);

            canvas3d_note_shadow_request(requests, &request_count, light, score);
            canvas3d_insert_shadow_light_candidate(
                out_lights, best_scores, 0, max_shadow_lights, &count, light, score);
        }
    }

    spot_phase_start = count;
    for (int32_t ci = 0; ci < draw_count; ci++) {
        int32_t draw_index = shadow_indices ? shadow_indices[ci] : ci;
        if (draw_index < 0)
            continue;
        if (!shadow_indices && !canvas3d_deferred_casts_shadow(&cmds[draw_index]))
            continue;
        if (!cmds[draw_index].lights)
            continue;
        for (int32_t li = 0; li < cmds[draw_index].light_count; li++) {
            const vgfx3d_light_params_t *light = &cmds[draw_index].lights[li];
            float score =
                canvas3d_shadow_spot_light_param_score(light, c ? c->cached_cam_pos : NULL);
            if (light->type == 1 || light->type == 4 || light->type == 5)
                score =
                    canvas3d_shadow_point_light_param_score(light, c ? c->cached_cam_pos : NULL);

            canvas3d_note_shadow_request(requests, &request_count, light, score);
            canvas3d_insert_shadow_light_candidate(
                out_lights, best_scores, spot_phase_start, max_shadow_lights, &count, light, score);
        }
    }
    if (out_request_count)
        *out_request_count = request_count;
    return count;
}

/// @brief Stamp shadow-map slot numbers onto a draw's copied light snapshot.
/// @details Shadow casters are selected from queued draw light snapshots, not
///   the canvas's live light slots. That lets transient Scene3D node lights keep
///   shadow support inside user-managed Begin/End frames.
/// @param[in,out] lights Draw-owned flattened light slice whose shadow metadata is replaced.
/// @param light_count Number of entries in @p lights.
/// @param shadow_lights Borrowed selected-shadow array.
/// @param shadow_count Number of selected slots to match.
static void canvas3d_apply_shadow_light_params(vgfx3d_light_params_t *lights,
                                               int32_t light_count,
                                               const vgfx3d_light_params_t *shadow_lights,
                                               int32_t shadow_count) {
    if (!lights || light_count <= 0)
        return;
    for (int32_t i = 0; i < light_count; i++)
        lights[i].shadow_index = -1;
    for (int32_t slot = 0; slot < shadow_count; slot++) {
        if (!shadow_lights)
            break;
        for (int32_t i = 0; i < light_count; i++) {
            if (lights[i].shadow_index < 0 &&
                canvas3d_shadow_light_params_match(&lights[i], &shadow_lights[slot])) {
                lights[i].shadow_index = slot;
                lights[i].shadow_cascade_count = shadow_lights[slot].shadow_cascade_count;
                lights[i].shadow_projection_type = shadow_lights[slot].shadow_projection_type;
                memcpy(lights[i].shadow_cascade_splits,
                       shadow_lights[slot].shadow_cascade_splits,
                       sizeof(lights[i].shadow_cascade_splits));
            }
        }
    }
}

/// @brief Track a malloc'd buffer for end-of-frame cleanup.
///
/// Used when the deferred path needs to allocate a transient instance-
/// matrix buffer that outlives the calling Zia frame. Geometric
/// growth (cap doubles, starting at 8). Ownership transfers only on
/// success; callers keep ownership and must free the buffer on failure.
// Per-frame transient-resource tracking — temp buffers, final-overlay temp
// buffers, and the GC-managed transient-object hash set — lives in
// rt_canvas3d_tempmgr.c (shared via rt_canvas3d_internal.h).

/// @brief Copy mesh vertex+index arrays into canvas-owned temp buffers.
/// @details Used when the mesh's owning heap object may be freed before the GPU
///          consumes the draw command — the snapshot lives on the canvas's temp
///          buffer list, freed at end-of-frame. Returns 1 on success, 0 on
///          allocation failure or invalid mesh state.
// Mesh-geometry snapshotting (copy vertex/index buffers into canvas-owned temp
// buffers for deferred upload, optionally rebased, with a revision-keyed cache)
// lives in rt_canvas3d_snapshot.c (shared via rt_canvas3d_internal.h).

// canvas3d_clear_temp_buffers / canvas3d_clear_temp_objects (end-of-frame
// transient-resource release) live in rt_canvas3d_tempmgr.c.

/// @brief Discard recorded final-overlay commands and owned geometry buffers.
/// @details Frees frame-owned allocations, releases retained objects, resets recording state, and
/// trims unusually large reusable arrays back to empty storage.
/// @param c Canvas owning the final-overlay command and resource lists; NULL is ignored.
void canvas3d_clear_final_overlay(rt_canvas3d *c) {
    if (!c)
        return;
    if (!c->final_overlay_temp_buffers || c->final_overlay_temp_buf_capacity < 0) {
        c->final_overlay_temp_buf_count = 0;
        c->final_overlay_temp_buf_capacity = 0;
    } else {
        if (c->final_overlay_temp_buf_count < 0)
            c->final_overlay_temp_buf_count = 0;
        if (c->final_overlay_temp_buf_count > c->final_overlay_temp_buf_capacity)
            c->final_overlay_temp_buf_count = c->final_overlay_temp_buf_capacity;
    }
    if (!c->final_overlay_temp_objects || c->final_overlay_temp_obj_capacity < 0) {
        c->final_overlay_temp_obj_count = 0;
        c->final_overlay_temp_obj_capacity = 0;
    } else {
        if (c->final_overlay_temp_obj_count < 0)
            c->final_overlay_temp_obj_count = 0;
        if (c->final_overlay_temp_obj_count > c->final_overlay_temp_obj_capacity)
            c->final_overlay_temp_obj_count = c->final_overlay_temp_obj_capacity;
    }
    if (!c->final_overlay_cmds || c->final_overlay_capacity < 0) {
        c->final_overlay_count = 0;
        c->final_overlay_capacity = 0;
    } else {
        if (c->final_overlay_count < 0)
            c->final_overlay_count = 0;
        if (c->final_overlay_count > c->final_overlay_capacity)
            c->final_overlay_count = c->final_overlay_capacity;
    }
    for (int32_t i = 0; i < c->final_overlay_temp_buf_count; i++) {
        free(c->final_overlay_temp_buffers[i]);
        c->final_overlay_temp_buffers[i] = NULL;
    }
    c->final_overlay_temp_buf_count = 0;
    canvas3d_reset_final_overlay_arena(c);
    for (int32_t i = 0; i < c->final_overlay_temp_obj_count; i++) {
        if (c->final_overlay_temp_objects[i] &&
            rt_obj_release_check0(c->final_overlay_temp_objects[i]))
            rt_obj_free(c->final_overlay_temp_objects[i]);
        c->final_overlay_temp_objects[i] = NULL;
    }
    c->final_overlay_temp_obj_count = 0;
    c->final_overlay_count = 0;
    c->final_overlay_recording = 0;
    if (c->final_overlay_capacity > CANVAS3D_FINAL_OVERLAY_RETAIN_CMD_CAP) {
        free(c->final_overlay_cmds);
        c->final_overlay_cmds = NULL;
        c->final_overlay_capacity = 0;
    }
    if (c->final_overlay_temp_buf_capacity > CANVAS3D_FINAL_OVERLAY_RETAIN_TEMP_BUF_CAP) {
        free(c->final_overlay_temp_buffers);
        c->final_overlay_temp_buffers = NULL;
        c->final_overlay_temp_buf_capacity = 0;
    }
    if (c->final_overlay_temp_obj_capacity > CANVAS3D_FINAL_OVERLAY_RETAIN_TEMP_BUF_CAP) {
        free(c->final_overlay_temp_objects);
        c->final_overlay_temp_objects = NULL;
        c->final_overlay_temp_obj_capacity = 0;
    }
}

/// @brief Reset finalized-frame latches when a new frame starts without a previous Flip().
/// @details `FinalizeFrame` may be used for capture or manual compositing. If the caller then
/// begins
///   another frame without presenting, post-FX and final-overlay state must not leak forward.
/// @param c Canvas whose overlay resources, post-FX chain, and finalization latches are reset.
static void canvas3d_reset_finalized_frame_state(rt_canvas3d *c) {
    if (!c)
        return;
    canvas3d_clear_final_overlay(c);
    c->frame_postfx_state_latched = 0;
    c->frame_gpu_postfx_enabled = 0;
    c->frame_postfx_chain_latched = 0;
    c->frame_finalized = 0;
    c->frame_presented_by_finalize = 0;
    vgfx3d_postfx_chain_reset(&c->frame_postfx_chain);
}

/// @brief Transform a local point by a row-major 4x4 model matrix.
/// @param model_matrix Borrowed row-major float model matrix.
/// @param local_point Borrowed three-component local-space point.
/// @param[out] out_world Writable three-component world-space result.
static void canvas3d_transform_point(const float *model_matrix,
                                     const float local_point[3],
                                     float out_world[3]) {
    if (!model_matrix || !local_point || !out_world)
        return;
    out_world[0] = model_matrix[0] * local_point[0] + model_matrix[1] * local_point[1] +
                   model_matrix[2] * local_point[2] + model_matrix[3];
    out_world[1] = model_matrix[4] * local_point[0] + model_matrix[5] * local_point[1] +
                   model_matrix[6] * local_point[2] + model_matrix[7];
    out_world[2] = model_matrix[8] * local_point[0] + model_matrix[9] * local_point[1] +
                   model_matrix[10] * local_point[2] + model_matrix[11];
}

/// @brief Project a world-space point onto the camera's forward axis.
/// @param c Borrowed canvas providing cached camera position and normalized forward direction.
/// @param world_point Borrowed three-component world-space point.
/// @return Signed view depth in world units, or zero for invalid input.
static float canvas3d_depth_along_view(const rt_canvas3d *c, const float world_point[3]) {
    float dx;
    float dy;
    float dz;

    if (!c || !world_point)
        return 0.0f;
    dx = world_point[0] - c->cached_cam_pos[0];
    dy = world_point[1] - c->cached_cam_pos[1];
    dz = world_point[2] - c->cached_cam_pos[2];
    return dx * c->cached_cam_forward[0] + dy * c->cached_cam_forward[1] +
           dz * c->cached_cam_forward[2];
}

/// @brief Compute the min/max view depth covered by a transformed local AABB.
/// @details Without valid local bounds, the transformed local origin supplies both depths. Output
/// destinations are initialized to zero before validation.
/// @param c Borrowed canvas providing cached view position/direction.
/// @param model_matrix Borrowed row-major model matrix.
/// @param local_bounds_min Optional borrowed three-component minimum corner.
/// @param local_bounds_max Optional borrowed three-component maximum corner.
/// @param has_local_bounds Non-zero to transform all eight AABB corners.
/// @param[out] out_near_depth Optional destination for minimum signed view depth.
/// @param[out] out_far_depth Optional destination for maximum signed view depth.
static void canvas3d_compute_depth_range(const rt_canvas3d *c,
                                         const float *model_matrix,
                                         const float *local_bounds_min,
                                         const float *local_bounds_max,
                                         int8_t has_local_bounds,
                                         float *out_near_depth,
                                         float *out_far_depth) {
    float near_depth;
    float far_depth;

    if (out_near_depth)
        *out_near_depth = 0.0f;
    if (out_far_depth)
        *out_far_depth = 0.0f;
    if (!c || !model_matrix)
        return;

    near_depth = FLT_MAX;
    far_depth = -FLT_MAX;
    if (has_local_bounds && local_bounds_min && local_bounds_max) {
        for (int xi = 0; xi < 2; xi++) {
            for (int yi = 0; yi < 2; yi++) {
                for (int zi = 0; zi < 2; zi++) {
                    float local_point[3];
                    float world_point[3];
                    float depth;

                    local_point[0] = xi ? local_bounds_max[0] : local_bounds_min[0];
                    local_point[1] = yi ? local_bounds_max[1] : local_bounds_min[1];
                    local_point[2] = zi ? local_bounds_max[2] : local_bounds_min[2];
                    canvas3d_transform_point(model_matrix, local_point, world_point);
                    depth = canvas3d_depth_along_view(c, world_point);
                    if (depth < near_depth)
                        near_depth = depth;
                    if (depth > far_depth)
                        far_depth = depth;
                }
            }
        }
    } else {
        float origin[3] = {0.0f, 0.0f, 0.0f};
        float world_origin[3];
        canvas3d_transform_point(model_matrix, origin, world_origin);
        near_depth = far_depth = canvas3d_depth_along_view(c, world_origin);
    }

    if (out_near_depth)
        *out_near_depth = near_depth;
    if (out_far_depth)
        *out_far_depth = far_depth;
}

/// @brief Compute the deferred-sort key for one draw.
/// @details Opaque draws sort by nearest depth (front-to-back). Transparent
///          draws sort by farthest depth (back-to-front). When local bounds are
///          available, the key comes from the transformed AABB rather than the
///          model origin, which makes sorting more stable for large or offset
///          meshes.
///
///          KNOWN LIMITATION: transparency is ordered per-object from this single
///          scalar — interpenetrating or mutually overlapping transparent meshes
///          can composite in the wrong order because no per-triangle sort or OIT
///          pass exists. Authors should split large transparent meshes or avoid
///          intersecting them; an order-independent-transparency path is the
///          eventual fix if content outgrows per-object sorting.
/// @param c Borrowed canvas providing cached view state.
/// @param model_matrix Borrowed row-major model matrix.
/// @param local_bounds_min Optional local AABB minimum.
/// @param local_bounds_max Optional local AABB maximum.
/// @param has_local_bounds Non-zero when both AABB corners are authoritative.
/// @param transparent Non-zero to select farthest depth; zero selects nearest depth.
/// @return Signed depth key used by deferred ordering.
static float canvas3d_compute_sort_key(const rt_canvas3d *c,
                                       const float *model_matrix,
                                       const float *local_bounds_min,
                                       const float *local_bounds_max,
                                       int8_t has_local_bounds,
                                       int8_t transparent) {
    float near_depth;
    float far_depth;

    canvas3d_compute_depth_range(c,
                                 model_matrix,
                                 local_bounds_min,
                                 local_bounds_max,
                                 has_local_bounds,
                                 &near_depth,
                                 &far_depth);
    return transparent ? far_depth : near_depth;
}

/// @brief Compute one opaque sort key for an instanced batch.
/// @details A single key can only approximate a spatially wide batch. This
///          uses the center of the aggregate world-space bounds instead of the
///          old minimum-per-instance depth, which biased large batches toward
///          whichever instance happened to be closest to the camera.
/// @param c Borrowed canvas providing cached view state.
/// @param instance_matrices Borrowed contiguous array of row-major float matrices.
/// @param instance_count Positive number of instances.
/// @param local_bounds_min Borrowed local AABB minimum.
/// @param local_bounds_max Borrowed local AABB maximum.
/// @return Signed view depth of the aggregate world-bounds center, or zero when no valid bounds can
/// be formed.
static float canvas3d_compute_instanced_batch_sort_key(const rt_canvas3d *c,
                                                       const float *instance_matrices,
                                                       int32_t instance_count,
                                                       const float *local_bounds_min,
                                                       const float *local_bounds_max) {
    float world_min[3] = {0.0f, 0.0f, 0.0f};
    float world_max[3] = {0.0f, 0.0f, 0.0f};
    int8_t has_bounds = 0;

    if (!c || !instance_matrices || instance_count <= 0 || !local_bounds_min || !local_bounds_max)
        return 0.0f;

    for (int32_t i = 0; i < instance_count; i++) {
        double world_matrix[16];
        float instance_min[3];
        float instance_max[3];
        for (int j = 0; j < 16; j++)
            world_matrix[j] = (double)instance_matrices[(size_t)i * 16u + (size_t)j];
        if (!vgfx3d_transform_aabb_checked(
                local_bounds_min, local_bounds_max, world_matrix, instance_min, instance_max))
            continue;
        if (!has_bounds) {
            memcpy(world_min, instance_min, sizeof(world_min));
            memcpy(world_max, instance_max, sizeof(world_max));
            has_bounds = 1;
        } else {
            for (int axis = 0; axis < 3; axis++) {
                if (instance_min[axis] < world_min[axis])
                    world_min[axis] = instance_min[axis];
                if (instance_max[axis] > world_max[axis])
                    world_max[axis] = instance_max[axis];
            }
        }
    }

    if (has_bounds) {
        float center[3] = {0.5f * (world_min[0] + world_max[0]),
                           0.5f * (world_min[1] + world_max[1]),
                           0.5f * (world_min[2] + world_max[2])};
        return canvas3d_depth_along_view(c, center);
    }
    return 0.0f;
}

/// @brief Extract a normalized forward vector (camera look direction) from a view matrix.
/// @details View row 2 holds the *backward* basis vector (cameras look
///          down −Z), so negating gives forward. Normalizes for safety
///          even though look-at construction already normalizes the
///          basis. Falls back to the conventional `(0, 0, −1)` if either
///          the input is null or the view is degenerate (zero forward),
///          which keeps downstream culling math safe.
/// @param view Optional borrowed row-major double view matrix.
/// @param[out] out_forward Writable three-float normalized direction; NULL is ignored.
static void canvas3d_extract_view_forward(const double *view, float *out_forward) {
    double fx = 0.0;
    double fy = 0.0;
    double fz = -1.0;
    double len;

    if (!out_forward)
        return;
    if (view) {
        fx = -view[8];
        fy = -view[9];
        fz = -view[10];
    }
    len = sqrt(fx * fx + fy * fy + fz * fz);
    if (len > 1e-12) {
        out_forward[0] = (float)(fx / len);
        out_forward[1] = (float)(fy / len);
        out_forward[2] = (float)(fz / len);
    } else {
        out_forward[0] = 0.0f;
        out_forward[1] = 0.0f;
        out_forward[2] = -1.0f;
    }
}

/// @brief Negate a projection's z row when the backend renders reversed-Z.
/// @details A reversed GL-convention projection is exactly the standard one with the z
///   row negated: near maps to NDC +1 and far to -1, which the backends' existing
///   GL-to-[0,1] window remaps turn into 1-at-near/0-at-far float depth — dense float
///   precision lands at the far field where standard-Z starves it. Every consumer of
///   the resulting VP (frustum extraction, unprojection via the inverse, the occlusion
///   projections) is self-consistent, so only the backend's clears/compares differ.
/// @param c Borrowed canvas whose backend depth convention is inspected.
/// @param[in,out] projection Row-major projection matrix adjusted in place when reversed-Z is
/// active.
static void canvas3d_apply_backend_depth_convention(const rt_canvas3d *c, float *projection) {
    if (!c || !projection || !c->backend || !c->backend->reversed_z)
        return;
    for (int i = 8; i < 12; i++)
        projection[i] = -projection[i];
}

/// @brief Build a pixel-space orthographic camera for 2D overlay submission.
/// @details The projection uses a top-left origin with Y down and adds two pixels of extent to
/// avoid half-pixel edge clipping. Fog is disabled while shadow filtering settings remain
/// consistent with replayed 3D overlay draws.
/// @param c Borrowed canvas providing output dimensions, backend depth convention, and shadow
/// state.
/// @param[out] params Camera parameters initialized and populated in place.
static void canvas3d_build_ortho_camera(const rt_canvas3d *c, vgfx3d_camera_params_t *params) {
    float w;
    float h;
    int32_t out_w = 0;
    int32_t out_h = 0;

    if (!c || !params)
        return;
    memset(params, 0, sizeof(*params));
    canvas3d_active_output_size(c, &out_w, &out_h);
    if (out_w <= 0)
        out_w = c->width;
    if (out_h <= 0)
        out_h = c->height;
    w = (float)out_w + 2.0f;
    h = (float)out_h + 2.0f;
    params->projection[0] = 2.0f / w;
    params->projection[5] = -2.0f / h;
    params->projection[10] = -1.0f;
    params->projection[3] = -1.0f + 2.0f / w;
    params->projection[7] = 1.0f - 2.0f / h;
    params->projection[15] = 1.0f;
    canvas3d_apply_backend_depth_convention(c, params->projection);
    params->view[0] = params->view[5] = params->view[10] = params->view[15] = 1.0f;
    params->position[2] = 1.0f;
    params->forward[2] = -1.0f;
    params->is_ortho = 1;
    params->fog_enabled = 0;
    params->znear = 0.1f;
    params->zfar = 1000.0f;
    /* Overlay passes may replay 3D draws; keep shadow filtering consistent. */
    params->shadow_strength = c ? c->shadow_strength : 0.85f;
    params->shadow_slope_bias = c ? c->shadow_slope_bias : 1.0f;
    params->shadow_quality = c ? c->shadow_quality : 1;
}

/// @brief Internal: begin a 2D overlay pass on top of the 3D scene.
///
/// Used by `Canvas3D` to draw HUD elements (text, sprites) on top of
/// the rendered scene. Switches to an orthographic projection,
/// preserves the existing color buffer (so the 3D scene stays
/// visible), and bypasses depth testing. Returns 0 if the canvas is
/// already in a frame or has no backend window.
/// @param c Canvas whose backend frame and cached camera state are initialized.
/// @param preserve_existing_color Non-zero to load the prior color attachment.
/// @return 1 when an overlay frame began; zero for invalid state or unavailable output/backend.
int canvas3d_begin_overlay_frame(rt_canvas3d *c, int8_t preserve_existing_color) {
    vgfx3d_camera_params_t params;

    if (!c || !c->backend || (!c->gfx_win && !c->render_target) || c->in_frame)
        return 0;
    if (c->backend->show_gpu_layer)
        c->backend->show_gpu_layer(c->backend_ctx);
    canvas3d_build_ortho_camera(c, &params);
    params.load_existing_color = preserve_existing_color ? 1 : 0;
    params.load_existing_depth = 0;
    if (!c->frame_postfx_state_latched)
        canvas3d_latch_gpu_postfx_state(c);
    else
        canvas3d_apply_gpu_postfx_state(c);
    c->cached_cam_pos[0] = 0.0f;
    c->cached_cam_pos[1] = 0.0f;
    c->cached_cam_pos[2] = 1.0f;
    c->cached_render_cam_pos[0] = 0.0f;
    c->cached_render_cam_pos[1] = 0.0f;
    c->cached_render_cam_pos[2] = 1.0f;
    c->cached_world_cam_pos[0] = 0.0;
    c->cached_world_cam_pos[1] = 0.0;
    c->cached_world_cam_pos[2] = 1.0;
    canvas3d_reset_camera_relative_origin(c);
    c->cached_cam_forward[0] = params.forward[0];
    c->cached_cam_forward[1] = params.forward[1];
    c->cached_cam_forward[2] = params.forward[2];
    c->cached_cam_near = 0.1f;
    c->cached_cam_far = 1000.0f;
    c->cached_cam_is_ortho = params.is_ortho;
    c->draw_count = 0;
    c->frame_is_2d = 1;
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            c->cached_vp[r * 4 + col] = params.projection[r * 4 + 0] * params.view[0 * 4 + col] +
                                        params.projection[r * 4 + 1] * params.view[1 * 4 + col] +
                                        params.projection[r * 4 + 2] * params.view[2 * 4 + col] +
                                        params.projection[r * 4 + 3] * params.view[3 * 4 + col];
    c->backend->begin_frame(c->backend_ctx, &params);
    c->in_frame = 1;
    return 1;
}

/// @brief Internal: retrieve the active scene view-projection matrix.
///
/// Three-way fallback: in a 3D frame → current frame's VP; otherwise
/// in an overlay frame → last 3D frame's VP (so overlays project
/// using the camera that drew the scene); otherwise → 2D ortho VP.
/// Used by 3D-aware overlay drawables (e.g., world-space tooltips).
/// @param c Borrowed canvas.
/// @return Borrowed pointer to an active/current or last-scene 16-float VP matrix, or NULL when
/// none is available. The pointer remains owned by the canvas.
const float *canvas3d_active_scene_vp(const rt_canvas3d *c) {
    if (!c)
        return NULL;
    if (c->in_frame && !c->frame_is_2d)
        return c->cached_vp;
    if (c->has_last_scene_vp)
        return c->last_scene_vp;
    if (c->in_frame)
        return c->cached_vp;
    return NULL;
}

/// @brief Forward a draw command to the active backend's `submit_draw` op.
///
/// Thin wrapper that exists mostly so call sites read a single noun
/// ("submit a mesh") instead of a 7-arg backend method call.
/// @param c Canvas with an active backend frame.
/// @param cmd Borrowed backend draw command.
/// @param lights Borrowed flattened light slice.
/// @param light_count Number of entries in @p lights.
/// @param ambient Borrowed three-component ambient color.
/// @param wireframe Effective wireframe flag.
/// @param backface_cull Effective backface-cull flag.
static void canvas3d_submit_mesh(rt_canvas3d *c,
                                 const vgfx3d_draw_cmd_t *cmd,
                                 const vgfx3d_light_params_t *lights,
                                 int32_t light_count,
                                 const float *ambient,
                                 int8_t wireframe,
                                 int8_t backface_cull) {
    if (!c || !c->backend || !cmd)
        return;
    canvas3d_note_backend_submission(
        c, cmd, wireframe, backface_cull, DEFERRED_DRAW_MESH, DEFERRED_PASS_MAIN);
    c->backend->submit_draw(
        c->backend_ctx, c->gfx_win, cmd, lights, light_count, ambient, wireframe, backface_cull);
}

/// @brief Apply shadow-pass-only material adjustments before a backend depth draw.
/// @details Explicit `ShadowMode.Cast` lets transparent materials cast shadows. Depth maps cannot
///   represent fractional coverage, so transparent cast mode uses the material alpha cutoff just
///   like alpha-masked foliage. This preserves the feature while avoiding backend divergence where
///   some paths cast a full opaque silhouette and others skipped the draw.
/// @param c Optional borrowed canvas providing the global shadow slope bias.
/// @param[in,out] cmd Per-pass command copy adjusted for depth-only shadow submission.
static void canvas3d_prepare_shadow_draw_cmd(const rt_canvas3d *c, vgfx3d_draw_cmd_t *cmd) {
    if (!cmd)
        return;
    if (c)
        cmd->slope_scaled_depth_bias += c->shadow_slope_bias;
    if (cmd->shadow_mode == RT_MATERIAL3D_SHADOW_MODE_CAST &&
        (cmd->additive_blend || vgfx3d_draw_cmd_uses_alpha_blend(cmd) || cmd->alpha < 0.999f ||
         cmd->diffuse_color[3] < 0.999f) &&
        cmd->alpha_mode != RT_MATERIAL3D_ALPHA_MODE_MASK)
        cmd->alpha_mode = RT_MATERIAL3D_ALPHA_MODE_MASK;
}

/// @brief Decompose an instanced draw into N individual mesh draws.
///
/// Backends without `submit_draw_instanced` (e.g., software fallback)
/// can still render instanced data this way — it's slower but
/// preserves correctness. Per-instance matrices are unpacked into
/// individual `model_matrix`/`prev_model_matrix` pairs, with
/// `has_prev_instance_matrices` translated into per-mesh
/// `has_prev_model_matrix` flags.
/// @param c Canvas with an active backend pass.
/// @param dd Borrowed instanced deferred draw and matrix arrays.
/// @param shadow_only Non-zero to call the backend shadow hook instead of normal submission.
static void canvas3d_submit_instanced_as_meshes(rt_canvas3d *c,
                                                const deferred_draw_t *dd,
                                                int shadow_only) {
    if (!c || !dd || !dd->instance_matrices || dd->instance_count <= 0)
        return;
    for (int32_t i = 0; i < dd->instance_count; i++) {
        vgfx3d_draw_cmd_t per_instance = dd->cmd;
        memcpy(per_instance.model_matrix,
               &dd->instance_matrices[(size_t)i * 16u],
               sizeof(per_instance.model_matrix));
        if (dd->cmd.has_prev_instance_matrices && dd->cmd.prev_instance_matrices) {
            memcpy(per_instance.prev_model_matrix,
                   &dd->cmd.prev_instance_matrices[(size_t)i * 16u],
                   sizeof(per_instance.prev_model_matrix));
            per_instance.has_prev_model_matrix = 1;
        } else {
            memcpy(per_instance.prev_model_matrix,
                   per_instance.model_matrix,
                   sizeof(per_instance.prev_model_matrix));
            per_instance.has_prev_model_matrix = 0;
        }
        if (shadow_only) {
            canvas3d_prepare_shadow_draw_cmd(c, &per_instance);
            if (c->backend->shadow_draw)
                c->backend->shadow_draw(c->backend_ctx, &per_instance);
        } else {
            canvas3d_submit_mesh(c,
                                 &per_instance,
                                 dd->lights,
                                 dd->light_count,
                                 dd->ambient,
                                 dd->wireframe,
                                 dd->backface_cull);
        }
    }
}

/// @brief Fill a deferred-draw snapshot with all backend-visible state.
///
/// Captures every parameter the backend needs so the snapshot survives even if
/// caller-side state (lights, ambient) changes between enqueue and flush.
/// @param c Canvas providing ambient state and optional flattened-light snapshots.
/// @param[out] dd Deferred draw initialized from a zero template.
/// @param cmd Borrowed fully populated backend draw command.
/// @param kind Mesh or instanced draw kind.
/// @param pass_kind Main or screen-overlay pass kind.
/// @param instance_matrices Optional borrowed frame-stable instance matrix array.
/// @param instance_count Number of matrices in @p instance_matrices.
/// @param include_lights Non-zero to attach the cached flattened-light slice and cluster table.
/// @param wireframe Captured wireframe state.
/// @param backface_cull Captured backface-cull state.
/// @param sort_key Raw depth key to sanitize.
/// @param local_bounds_min Optional borrowed local AABB minimum.
/// @param local_bounds_max Optional borrowed local AABB maximum.
static void canvas3d_fill_deferred_draw(rt_canvas3d *c,
                                        deferred_draw_t *dd,
                                        const vgfx3d_draw_cmd_t *cmd,
                                        deferred_draw_kind_t kind,
                                        deferred_pass_t pass_kind,
                                        const float *instance_matrices,
                                        int32_t instance_count,
                                        int include_lights,
                                        int8_t wireframe,
                                        int8_t backface_cull,
                                        float sort_key,
                                        const float *local_bounds_min,
                                        const float *local_bounds_max) {
    static const deferred_draw_t zero_draw = {0};
    *dd = zero_draw;
    dd->kind = kind;
    dd->pass_kind = pass_kind;
    dd->visible = 1;
    dd->light_offset = -1;
    dd->cmd = *cmd;
    dd->requires_blend = canvas3d_cmd_requires_blend(cmd);
    dd->instance_matrices = instance_matrices;
    dd->instance_count = instance_count;
    dd->wireframe = wireframe;
    dd->backface_cull = backface_cull;
    dd->ambient[0] = c->ambient[0];
    dd->ambient[1] = c->ambient[1];
    dd->ambient[2] = c->ambient[2];
    if (include_lights) {
        if (!canvas3d_assign_frame_lights(c, dd)) {
            rt_trap("Canvas3D.Draw: light snapshot allocation failed");
            dd->light_count = 0;
        }
    } else {
        dd->cmd.lights_revision = 0;
        dd->cmd.cluster_table = NULL;
    }
    dd->sort_key = canvas3d_sanitize_sort_key(sort_key, dd->requires_blend);
    dd->stable_sort_id = (uintptr_t)cmd->geometry_key ^ (uintptr_t)cmd->texture ^
                         (uintptr_t)cmd->texture_target ^ ((uintptr_t)cmd->texture_asset << 1) ^
                         ((uintptr_t)instance_matrices << 2);
    dd->occlusion_key = dd->stable_sort_id ? dd->stable_sort_id : (uintptr_t)cmd->vertices;
    if (local_bounds_min && local_bounds_max) {
        dd->has_local_bounds = 1;
        memcpy(dd->local_bounds_min, local_bounds_min, sizeof(dd->local_bounds_min));
        memcpy(dd->local_bounds_max, local_bounds_max, sizeof(dd->local_bounds_max));
    }
    canvas3d_finalize_deferred_visibility_flags(dd, 0, 0);
}
