//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_occlusion.inc
// Purpose: Implement Canvas3D bounds, cascade, frustum, and CPU occlusion
//   processing. Included by rt_canvas3d.c.
// Key invariants:
//   - Occlusion identities remain stable across queue reordering and culling.
//   - Hash capacities are supported powers of two before masked probing.
// Ownership/Lifetime:
//   - Canvas3D owns persistent history and coarse/fine occlusion buffers.
//   - Deferred draws and geometry referenced during a frame are borrowed.
// Links: rt_canvas3d.c, rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deferred bounds, cascade selection, frustum culling, and CPU occlusion.
/// @details This rt_canvas3d.c fragment assigns order-independent temporal
///   identities, caches complete world bounds, maintains conservative
///   history-gated coarse coverage, and rasterizes eligible occluder geometry
///   into a fine Hi-Z buffer without inventing coverage.

/// @brief Hash one float into a stable CPU-occlusion identity fingerprint.
/// @details The occlusion history table must not depend on queue position because streaming,
///   LOD, and debug draws can legitimately change submission order between frames. Quantizing
///   the model matrix keeps static objects stable while forcing moving objects to re-prove
///   visibility instead of inheriting stale covered streaks from an old screen position.
/// @param key Current accumulated 64-bit fingerprint.
/// @param value Float component quantized to 1/1024 before mixing.
/// @return Updated deterministic hash.
static uint64_t canvas3d_occlusion_mix_quantized_float(uint64_t key, float value) {
    double scaled;
    int64_t quantized;
    if (!isfinite(value))
        value = 0.0f;
    scaled = floor((double)value * 1024.0 + ((value >= 0.0f) ? 0.5 : -0.5));
    if (scaled > (double)INT64_MAX)
        quantized = INT64_MAX;
    else if (scaled < (double)INT64_MIN)
        quantized = INT64_MIN;
    else
        quantized = (int64_t)scaled;
    return canvas3d_hash_u64(key ^ (uint64_t)quantized);
}

/// @brief Return the stable base identity used by CPU occlusion-history hashing.
/// @details Uses the material/geometry sort identity when available and falls back to the queued
///   vertex pointer for synthetic draws. This intentionally ignores `occlusion_key`, which may
///   already include duplicate-occurrence disambiguation after the draw has been queued.
/// @param dd Borrowed deferred draw to identify.
/// @return Stable non-zero base when usable identity exists; otherwise zero.
static uintptr_t canvas3d_occlusion_base_identity(const deferred_draw_t *dd) {
    uintptr_t base_key;
    if (!dd)
        return 0;
    base_key = dd->stable_sort_id ? dd->stable_sort_id : (uintptr_t)dd->cmd.vertices;
    return base_key;
}

/// @brief Build a queue-order-independent draw fingerprint for CPU occlusion history.
/// @details Combines persistent mesh/material identity, pass/kind, instance count, and a quantized
///   model matrix. It separates ordinary repeated meshes at different transforms without using the
///   global enqueue index, so unrelated draw insertion does not churn occlusion history.
/// @param dd Borrowed deferred draw whose stable fields are hashed.
/// @return Queue-order-independent non-zero fingerprint, or zero without base identity.
static uintptr_t canvas3d_occlusion_draw_fingerprint(const deferred_draw_t *dd) {
    uint64_t key;
    uintptr_t base_key;
    base_key = canvas3d_occlusion_base_identity(dd);
    if (!base_key)
        return 0;
    key = canvas3d_hash_u64((uint64_t)base_key ^ ((uint64_t)dd->kind << 56u));
    key = canvas3d_hash_u64(key ^ ((uint64_t)dd->pass_kind << 48u));
    key = canvas3d_hash_u64(key ^ ((uint64_t)(uint32_t)dd->instance_count << 32u));
    for (int i = 0; i < 12; i++)
        key = canvas3d_occlusion_mix_quantized_float(key, dd->cmd.model_matrix[i]);
    key = canvas3d_hash_u64(key);
    return key ? (uintptr_t)key : base_key;
}

/// @brief Count already processed draws with @p fingerprint by scanning the current queue prefix.
/// @details Used only as a fallback when the per-frame duplicate-count table cannot be allocated.
/// @param queued Borrowed deferred queue.
/// @param fingerprint Non-zero fingerprint to count.
/// @param prior_count Number of prefix entries already assigned.
/// @return Number of matching prefix entries, or zero for invalid input.
static int32_t canvas3d_occlusion_prior_duplicate_count(const deferred_draw_t *queued,
                                                        uintptr_t fingerprint,
                                                        int32_t prior_count) {
    int32_t count = 0;
    if (!queued || !fingerprint || prior_count <= 0)
        return 0;
    for (int32_t i = 0; i < prior_count; ++i) {
        if (queued[i].occlusion_fingerprint == fingerprint)
            count++;
    }
    return count;
}

/// @brief Clear the per-frame occlusion duplicate-count table.
/// @param c Canvas3D owning the scratch table; missing storage is a no-op.
static void canvas3d_occlusion_duplicate_counts_clear(rt_canvas3d *c) {
    if (!c || !c->occlusion_duplicate_counts || c->occlusion_duplicate_count_capacity <= 0)
        return;
    memset(c->occlusion_duplicate_counts,
           0,
           (size_t)c->occlusion_duplicate_count_capacity * sizeof(*c->occlusion_duplicate_counts));
}

/// @brief Ensure duplicate-count scratch can hold approximately @p draw_count fingerprints.
/// @details Capacity is power-of-two and at least twice the draw count so linear probing remains
///          short even for many repeated meshes.
/// @param c Canvas3D owning duplicate-count scratch.
/// @param draw_count Non-negative anticipated queue length.
/// @return Non-zero when cleared suitable storage is available.
static int canvas3d_ensure_occlusion_duplicate_counts(rt_canvas3d *c, int32_t draw_count) {
    int32_t needed;
    rt_canvas3d_occlusion_duplicate_entry *grown;
    if (!c)
        return 0;
    if (draw_count < 0 || draw_count > (INT32_C(1) << 29))
        return 0;
    needed = canvas3d_next_power_of_two_i32(draw_count > 0 ? draw_count * 2 : 32);
    if (needed < 32)
        needed = 32;
    if (c->occlusion_duplicate_count_capacity >= needed) {
        canvas3d_occlusion_duplicate_counts_clear(c);
        return 1;
    }
    if ((size_t)needed > SIZE_MAX / sizeof(*c->occlusion_duplicate_counts))
        return 0;
    grown = (rt_canvas3d_occlusion_duplicate_entry *)realloc(c->occlusion_duplicate_counts,
                                                             (size_t)needed * sizeof(*grown));
    if (!grown)
        return 0;
    c->occlusion_duplicate_counts = grown;
    c->occlusion_duplicate_count_capacity = needed;
    canvas3d_occlusion_duplicate_counts_clear(c);
    return 1;
}

/// @brief Return the current duplicate ordinal for @p fingerprint, then increment its counter.
/// @details This is the hot-path replacement for scanning all prior queued draws. A return value of
///          zero means the fingerprint is first-seen this frame; one means the second identical
///          draw, and so on.
/// @param c Canvas3D owning initialized duplicate-count scratch.
/// @param fingerprint Non-zero draw fingerprint to advance.
/// @return Zero-based occurrence ordinal before the counter increment.
static int32_t canvas3d_occlusion_duplicate_next_index(rt_canvas3d *c, uintptr_t fingerprint) {
    int32_t mask;
    int32_t slot;
    if (!c || !fingerprint || !c->occlusion_duplicate_counts ||
        c->occlusion_duplicate_count_capacity <= 0)
        return 0;
    mask = c->occlusion_duplicate_count_capacity - 1;
    slot = (int32_t)(canvas3d_hash_u64(fingerprint) & (uint32_t)mask);
    for (int32_t probe = 0; probe < c->occlusion_duplicate_count_capacity; probe++) {
        rt_canvas3d_occlusion_duplicate_entry *entry = &c->occlusion_duplicate_counts[slot];
        if (!entry->fingerprint) {
            entry->fingerprint = fingerprint;
            entry->count = 1;
            return 0;
        }
        if (entry->fingerprint == fingerprint) {
            int32_t duplicate_index = entry->count;
            if (entry->count < INT32_MAX)
                entry->count++;
            return duplicate_index;
        }
        slot = (slot + 1) & mask;
    }
    return 0;
}

/// @brief Build the final CPU occlusion-history key from a fingerprint and duplicate ordinal.
/// @param fingerprint Stable non-zero draw fingerprint.
/// @param duplicate_index Zero-based occurrence ordinal.
/// @return Disambiguated non-zero temporal key, or zero for an empty fingerprint.
static uintptr_t canvas3d_occlusion_key_from_duplicate(uintptr_t fingerprint,
                                                       int32_t duplicate_index) {
    uint64_t key;
    if (!fingerprint)
        return 0;
    if (duplicate_index <= 0)
        return fingerprint;
    key = canvas3d_hash_u64((uint64_t)fingerprint ^
                            ((uint64_t)(uint32_t)duplicate_index + 0x9E3779B97F4A7C15ull));
    return key ? (uintptr_t)key : fingerprint;
}

/// @brief Assign final CPU occlusion keys to every queued draw in one linear pass.
/// @details Draws may patch their stable identity after enqueueing, so key assignment happens after
///          the frame queue is complete. When duplicate-count scratch is unavailable, this falls
///          back to the previous prior-prefix scan for correctness.
/// @param c Canvas3D providing duplicate-count scratch.
/// @param cmds Mutable completed deferred queue.
/// @param count Number of initialized entries in @p cmds.
static void canvas3d_assign_occlusion_keys(rt_canvas3d *c, deferred_draw_t *cmds, int32_t count) {
    int use_table;
    if (!c || !cmds || count <= 0)
        return;
    use_table = canvas3d_ensure_occlusion_duplicate_counts(c, count);
    for (int32_t i = 0; i < count; i++) {
        uintptr_t fingerprint = canvas3d_occlusion_draw_fingerprint(&cmds[i]);
        int32_t duplicate_index;
        cmds[i].occlusion_fingerprint = fingerprint;
        if (!fingerprint) {
            cmds[i].occlusion_key = 0;
            continue;
        }
        duplicate_index = use_table
                              ? canvas3d_occlusion_duplicate_next_index(c, fingerprint)
                              : canvas3d_occlusion_prior_duplicate_count(cmds, fingerprint, i);
        cmds[i].occlusion_key = canvas3d_occlusion_key_from_duplicate(fingerprint, duplicate_index);
    }
}

/// @brief Append a fully prepared deferred draw and stamp shared queue metadata.
/// @details Manual mesh draws and helper-built draws both need stable enqueue order and cached
/// world
///   bounds before end-of-frame sorting/culling. Centralizing that bookkeeping prevents one queue
///   path from diverging and reintroducing unstable transparent ordering.
/// @param c Canvas3D owning the main deferred queue.
/// @param dd Mutable fully prepared draw copied into the queue on success.
/// @param cache_key Optional borrowed cache identity for reusable mesh world bounds.
/// @return Non-zero when the draw is published; zero on invalid input or reserve failure.
static int canvas3d_append_prepared_deferred_draw(
    rt_canvas3d *c, deferred_draw_t *dd, const canvas3d_world_bounds_cache_key_t *cache_key) {
    if (!c || !dd)
        return 0;
    if (c->test_fail_next_queue_reserve) {
        c->test_fail_next_queue_reserve = 0;
        canvas3d_record_submission_failure(c, RT_CANVAS3D_SUBMISSION_QUEUE_FAILURE);
        return 0;
    }
    if (!ensure_deferred_capacity(&c->draw_cmds, &c->draw_capacity, c->draw_count + 1)) {
        canvas3d_record_submission_failure(c, RT_CANVAS3D_SUBMISSION_QUEUE_FAILURE);
        return 0;
    }
    dd->enqueue_index = c->draw_count;
    dd->occlusion_fingerprint = 0;
    dd->occlusion_key = 0;
    canvas3d_cache_deferred_world_bounds(c, dd, cache_key);
    ((deferred_draw_t *)c->draw_cmds)[c->draw_count++] = *dd;
    return 1;
}

/// @brief Append a draw with caller-provided render-space world bounds.
/// @details Instanced submissions already touch every instance matrix while snapshotting;
///   accumulating the world-AABB union in that loop lets append skip its own
///   O(instance) rescan. Provided bounds must cover every submitted instance
///   (conservative supersets are safe — instanced draws never occlusion-test/-write).
///   Pass NULL bounds to fall back to the append-time computation. The queue is
///   dispatched in sorted order; allocation failure traps rather than bypassing
///   ordering with immediate submission.
/// @param c Canvas3D owning the deferred queue.
/// @param cmd Borrowed backend command payload to snapshot.
/// @param kind Mesh or instanced deferred draw kind.
/// @param pass_kind Main or overlay pass classification.
/// @param instance_matrices Optional borrowed current instance matrix array.
/// @param instance_count Number of matrices when instanced.
/// @param include_lights Non-zero to snapshot current frame lighting.
/// @param wireframe Non-zero to request wireframe rasterization.
/// @param backface_cull Non-zero to cull back-facing triangles.
/// @param sort_key Finite ordering key for deferred sorting.
/// @param local_bounds_min Optional local AABB minimum.
/// @param local_bounds_max Optional local AABB maximum paired with @p local_bounds_min.
/// @param world_bounds_min Optional precomputed render-space AABB minimum.
/// @param world_bounds_max Optional precomputed render-space AABB maximum paired
///   with @p world_bounds_min.
/// @return Non-zero when queued; zero after invalid input or allocation failure.
static int canvas3d_enqueue_draw_world_bounds(rt_canvas3d *c,
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
                                              const float *local_bounds_max,
                                              const float *world_bounds_min,
                                              const float *world_bounds_max) {
    deferred_draw_t dd;

    if (!c || !cmd)
        return 0;
    canvas3d_fill_deferred_draw(c,
                                &dd,
                                cmd,
                                kind,
                                pass_kind,
                                instance_matrices,
                                instance_count,
                                include_lights,
                                wireframe,
                                backface_cull,
                                sort_key,
                                local_bounds_min,
                                local_bounds_max);
    if (world_bounds_min && world_bounds_max &&
        canvas3d_bounds_are_valid(world_bounds_min, world_bounds_max)) {
        memcpy(dd.world_bounds_min, world_bounds_min, sizeof(dd.world_bounds_min));
        memcpy(dd.world_bounds_max, world_bounds_max, sizeof(dd.world_bounds_max));
        dd.has_world_bounds = 1;
    }
    if (!canvas3d_append_prepared_deferred_draw(c, &dd, NULL)) {
        rt_trap("Canvas3D: deferred draw queue allocation failed");
        return 0;
    }
    return 1;
}

/// @brief Append a draw while computing its render-space world bounds during publication.
/// @param c Canvas3D owning the deferred queue.
/// @param cmd Borrowed backend command payload to snapshot.
/// @param kind Mesh or instanced deferred draw kind.
/// @param pass_kind Main or overlay pass classification.
/// @param instance_matrices Optional borrowed current instance matrix array.
/// @param instance_count Number of matrices when instanced.
/// @param include_lights Non-zero to snapshot current frame lighting.
/// @param wireframe Non-zero to request wireframe rasterization.
/// @param backface_cull Non-zero to cull back-facing triangles.
/// @param sort_key Finite ordering key for deferred sorting.
/// @param local_bounds_min Optional local AABB minimum.
/// @param local_bounds_max Optional local AABB maximum paired with @p local_bounds_min.
/// @return Non-zero when queued; otherwise zero.
static int canvas3d_enqueue_draw(rt_canvas3d *c,
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
    return canvas3d_enqueue_draw_world_bounds(c,
                                              cmd,
                                              kind,
                                              pass_kind,
                                              instance_matrices,
                                              instance_count,
                                              include_lights,
                                              wireframe,
                                              backface_cull,
                                              sort_key,
                                              local_bounds_min,
                                              local_bounds_max,
                                              NULL,
                                              NULL);
}

/// @brief Append a draw to the final-overlay queue for replay after post-FX.
/// @param c Canvas3D owning retained final-overlay commands.
/// @param cmd Borrowed command payload copied into the queue.
/// @param local_bounds_min Optional local AABB minimum.
/// @param local_bounds_max Optional local AABB maximum paired with @p local_bounds_min.
/// @return Non-zero when queued; zero on invalid input or reserve failure.
static int canvas3d_enqueue_final_overlay_draw(rt_canvas3d *c,
                                               const vgfx3d_draw_cmd_t *cmd,
                                               const float *local_bounds_min,
                                               const float *local_bounds_max) {
    deferred_draw_t *dd;

    if (!c || !cmd)
        return 0;
    if (!ensure_deferred_capacity(
            &c->final_overlay_cmds, &c->final_overlay_capacity, c->final_overlay_count + 1))
        return 0;
    dd = &((deferred_draw_t *)c->final_overlay_cmds)[c->final_overlay_count++];
    canvas3d_fill_deferred_draw(c,
                                dd,
                                cmd,
                                DEFERRED_DRAW_MESH,
                                DEFERRED_PASS_MAIN,
                                NULL,
                                0,
                                0,
                                0,
                                0,
                                0.0f,
                                local_bounds_min,
                                local_bounds_max);
    dd->enqueue_index = c->final_overlay_count - 1;
    return 1;
}

/// @brief Dispatch a single deferred draw to the backend.
///
/// Routes mesh kind directly to `submit_draw`. Routes instanced kind
/// to `submit_draw_instanced` if the backend supports it, otherwise
/// falls back to `canvas3d_submit_instanced_as_meshes`.
/// @param c Canvas3D supplying backend state and pass telemetry.
/// @param dd Borrowed deferred command to submit.
static void canvas3d_submit_deferred(rt_canvas3d *c, const deferred_draw_t *dd) {
    if (!c || !dd)
        return;
    {
        /* Plan 30 pass attribution: 1 = Opaque, 2 = Transparent, 4 = Overlay. */
        int pass = dd->pass_kind != DEFERRED_PASS_MAIN ? 4 : (dd->requires_blend ? 2 : 1);
        int64_t instances =
            dd->kind == DEFERRED_DRAW_INSTANCED && dd->instance_count > 0 ? dd->instance_count : 1;
        c->last_pass_draw_count[pass] += 1;
        c->last_pass_instance_count[pass] += instances;
    }
    if (dd->kind == DEFERRED_DRAW_INSTANCED) {
        if (c->backend->submit_draw_instanced && dd->instance_count > 0) {
            canvas3d_note_backend_submission(c,
                                             &dd->cmd,
                                             dd->wireframe,
                                             dd->backface_cull,
                                             DEFERRED_DRAW_INSTANCED,
                                             dd->pass_kind);
            c->backend->submit_draw_instanced(c->backend_ctx,
                                              c->gfx_win,
                                              &dd->cmd,
                                              dd->instance_matrices,
                                              dd->instance_count,
                                              dd->lights,
                                              dd->light_count,
                                              dd->ambient,
                                              dd->wireframe,
                                              dd->backface_cull);
            return;
        }
        canvas3d_submit_instanced_as_meshes(c, dd, 0);
        return;
    }
    canvas3d_submit_mesh(
        c, &dd->cmd, dd->lights, dd->light_count, dd->ambient, dd->wireframe, dd->backface_cull);
}

/// @brief Submit screen-space overlay geometry without writing depth.
/// @param c Canvas3D supplying backend state.
/// @param dd Borrowed overlay draw copied before blend-state adjustment.
static void canvas3d_submit_screen_overlay_deferred(rt_canvas3d *c, const deferred_draw_t *dd) {
    deferred_draw_t overlay;

    if (!c || !dd)
        return;
    overlay = *dd;
    overlay.cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
    canvas3d_submit_deferred(c, &overlay);
}

/// @brief Dispatch a deferred draw to the shadow-pass path.
///
/// Same dispatch shape as `submit_deferred` but routes through the
/// backend's depth-only shadow draw entry. Instanced shadow draws
/// always decompose to per-mesh draws (most backends don't have an
/// instanced shadow path).
/// @param c Canvas3D supplying the shadow backend and telemetry.
/// @param dd Borrowed deferred caster command.
static void canvas3d_shadow_deferred(rt_canvas3d *c, const deferred_draw_t *dd) {
    vgfx3d_draw_cmd_t shadow_cmd;

    if (!c || !dd || !c->backend || !c->backend->shadow_draw)
        return;
    c->last_pass_draw_count[0] += 1; /* Plan 30: Shadow pass tally. */
    c->last_pass_instance_count[0] +=
        dd->kind == DEFERRED_DRAW_INSTANCED && dd->instance_count > 0 ? dd->instance_count : 1;
    if (dd->kind == DEFERRED_DRAW_INSTANCED) {
        canvas3d_submit_instanced_as_meshes(c, dd, 1);
        return;
    }
    shadow_cmd = dd->cmd;
    canvas3d_prepare_shadow_draw_cmd(c, &shadow_cmd);
    c->backend->shadow_draw(c->backend_ctx, &shadow_cmd);
}

/// @brief In-place AABB union: `[io_min, io_max] ⊇ [mn, mx]`.
/// @param io_min Mutable three-component aggregate minimum.
/// @param io_max Mutable three-component aggregate maximum.
/// @param mn Borrowed three-component contributing minimum.
/// @param mx Borrowed three-component contributing maximum.
static void canvas3d_expand_bounds(float *io_min, float *io_max, const float *mn, const float *mx) {
    if (!io_min || !io_max || !mn || !mx)
        return;
    for (int i = 0; i < 3; i++) {
        if (mn[i] < io_min[i])
            io_min[i] = mn[i];
        if (mx[i] > io_max[i])
            io_max[i] = mx[i];
    }
}

/// @brief Compute a deferred draw's complete world-space AABB without reading cached fields.
/// @details Normal draws transform their one local mesh bound by `model_matrix`. Instanced draws
///   scan every instance matrix and union all transformed bounds, avoiding sampled sort/cull inputs
///   that can flicker when an unsampled instance becomes visible. Invalid transforms are skipped;
///   the function returns 0 only when no finite contribution remains.
/// @param c Optional Canvas3D whose AABB-transform telemetry is incremented.
/// @param dd Borrowed deferred draw with valid local bounds.
/// @param out_min Non-`NULL` three-component world minimum output.
/// @param out_max Non-`NULL` three-component world maximum output.
/// @return Non-zero when at least one finite transformed bound contributes.
static int canvas3d_compute_deferred_world_bounds_uncached(rt_canvas3d *c,
                                                           const deferred_draw_t *dd,
                                                           float *out_min,
                                                           float *out_max) {
    int8_t has_bounds = 0;

    if (!dd || !out_min || !out_max || !dd->has_local_bounds)
        return 0;
    if (dd->kind == DEFERRED_DRAW_INSTANCED && dd->instance_matrices && dd->instance_count > 0) {
        for (int32_t i = 0; i < dd->instance_count; i++) {
            double world_matrix[16];
            float instance_min[3];
            float instance_max[3];
            for (int j = 0; j < 16; j++)
                world_matrix[j] = (double)dd->instance_matrices[(size_t)i * 16u + (size_t)j];
            if (c)
                c->frame_aabb_transforms++;
            if (!vgfx3d_transform_aabb_checked(dd->local_bounds_min,
                                               dd->local_bounds_max,
                                               world_matrix,
                                               instance_min,
                                               instance_max))
                continue;
            if (!has_bounds) {
                memcpy(out_min, instance_min, sizeof(float) * 3);
                memcpy(out_max, instance_max, sizeof(float) * 3);
                has_bounds = 1;
            } else {
                canvas3d_expand_bounds(out_min, out_max, instance_min, instance_max);
            }
        }
        return has_bounds ? 1 : 0;
    }

    {
        double world_matrix[16];
        for (int j = 0; j < 16; j++)
            world_matrix[j] = (double)dd->cmd.model_matrix[j];
        if (c)
            c->frame_aabb_transforms++;
        return vgfx3d_transform_aabb_checked(
            dd->local_bounds_min, dd->local_bounds_max, world_matrix, out_min, out_max);
    }
}

/// @brief Store a deferred draw's world-space AABB on the draw itself for later passes.
/// @details Main-pass culling, occlusion, shadow bounds, and cascade assignment all need the same
///   world bound. Caching it once keeps those passes deterministic and removes repeated full scans
///   over large instanced batches.
/// @param c Optional Canvas3D supplying the fixed-slot world-bounds cache.
/// @param dd Mutable deferred draw receiving world bounds when computable.
/// @param cache_key Optional borrowed exact mesh/matrix cache key.
static void canvas3d_cache_deferred_world_bounds(
    rt_canvas3d *c, deferred_draw_t *dd, const canvas3d_world_bounds_cache_key_t *cache_key) {
    rt_canvas3d_world_bounds_cache_entry *entry;
    uint32_t slot;

    if (!dd)
        return;
    /* Instanced submissions accumulate their union while snapshotting matrices and
     * arrive with bounds already attached; recomputing here would rescan every
     * instance matrix a second time per frame. */
    if (dd->has_world_bounds)
        return;
    if (c && dd->kind == DEFERRED_DRAW_MESH && cache_key && cache_key->valid &&
        cache_key->mesh_key && cache_key->matrix_hash) {
        slot = (uint32_t)(cache_key->matrix_hash &
                          (uint64_t)(RT_CANVAS3D_WORLD_BOUNDS_CACHE_SIZE - 1));
        entry = &c->world_bounds_cache[slot];
        if (entry->occupied && entry->mesh_key == cache_key->mesh_key &&
            entry->matrix_hash == cache_key->matrix_hash &&
            memcmp(entry->matrix_key, cache_key->matrix_key, sizeof(cache_key->matrix_key)) == 0) {
            memcpy(dd->world_bounds_min, entry->world_min, sizeof(dd->world_bounds_min));
            memcpy(dd->world_bounds_max, entry->world_max, sizeof(dd->world_bounds_max));
            dd->has_world_bounds = 1;
            return;
        }
        dd->has_world_bounds = canvas3d_compute_deferred_world_bounds_uncached(
                                   c, dd, dd->world_bounds_min, dd->world_bounds_max)
                                   ? 1
                                   : 0;
        if (dd->has_world_bounds) {
            entry->occupied = 1;
            entry->mesh_key = cache_key->mesh_key;
            entry->matrix_hash = cache_key->matrix_hash;
            memcpy(entry->matrix_key, cache_key->matrix_key, sizeof(entry->matrix_key));
            memcpy(entry->world_min, dd->world_bounds_min, sizeof(entry->world_min));
            memcpy(entry->world_max, dd->world_bounds_max, sizeof(entry->world_max));
        }
        return;
    }
    dd->has_world_bounds = canvas3d_compute_deferred_world_bounds_uncached(
                               c, dd, dd->world_bounds_min, dd->world_bounds_max)
                               ? 1
                               : 0;
}

/// @brief Row-major 4×4 matrix multiply: `out = a * b`.
/// @param a Borrowed left 16-float matrix.
/// @param b Borrowed right 16-float matrix.
/// @param out Non-`NULL` 16-float product output distinct from unsafe aliases.
static void canvas3d_mul_mat4(const float *a, const float *b, float *out) {
    if (!a || !b || !out)
        return;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

/// @brief Compute world-space AABB for a deferred draw, unioning into `[io_min, io_max]`.
///
/// Iterates per-instance for instanced draws (so the shadow VP fits
/// every instance) or just transforms the single mesh AABB for normal
/// draws. `io_has_bounds` flips to true on first contribution.
/// @param c Optional Canvas3D supplying telemetry.
/// @param dd Borrowed deferred draw whose complete bound contributes.
/// @param io_min Mutable three-component aggregate minimum.
/// @param io_max Mutable three-component aggregate maximum.
/// @param io_has_bounds Mutable flag indicating whether the aggregate is initialized.
static void canvas3d_accumulate_deferred_world_bounds(rt_canvas3d *c,
                                                      const deferred_draw_t *dd,
                                                      float *io_min,
                                                      float *io_max,
                                                      int8_t *io_has_bounds) {
    if (!dd || !io_min || !io_max || !io_has_bounds || !dd->has_local_bounds)
        return;

    if (dd->has_world_bounds) {
        if (!*io_has_bounds) {
            memcpy(io_min, dd->world_bounds_min, sizeof(float) * 3);
            memcpy(io_max, dd->world_bounds_max, sizeof(float) * 3);
            *io_has_bounds = 1;
        } else {
            canvas3d_expand_bounds(io_min, io_max, dd->world_bounds_min, dd->world_bounds_max);
        }
        return;
    }

    {
        float world_min[3];
        float world_max[3];
        if (!canvas3d_compute_deferred_world_bounds_uncached(c, dd, world_min, world_max))
            return;
        if (!*io_has_bounds) {
            memcpy(io_min, world_min, sizeof(float) * 3);
            memcpy(io_max, world_max, sizeof(float) * 3);
            *io_has_bounds = 1;
        } else {
            canvas3d_expand_bounds(io_min, io_max, world_min, world_max);
        }
    }
}

/// @brief Compute the camera-forward depth span of a world-space AABB.
/// @param c Borrowed Canvas3D supplying camera position and forward vector.
/// @param world_min Borrowed three-component world AABB minimum.
/// @param world_max Borrowed three-component world AABB maximum.
/// @param out_near_depth Optional nearest projected depth output.
/// @param out_far_depth Optional farthest projected depth output.
/// @return Non-zero when all corner depths produce a finite interval.
static int canvas3d_world_bounds_view_depth_range(const rt_canvas3d *c,
                                                  const float *world_min,
                                                  const float *world_max,
                                                  float *out_near_depth,
                                                  float *out_far_depth) {
    float near_depth = FLT_MAX;
    float far_depth = -FLT_MAX;

    if (out_near_depth)
        *out_near_depth = 0.0f;
    if (out_far_depth)
        *out_far_depth = 0.0f;
    if (!c || !world_min || !world_max)
        return 0;
    for (int xi = 0; xi < 2; xi++) {
        for (int yi = 0; yi < 2; yi++) {
            for (int zi = 0; zi < 2; zi++) {
                float point[3];
                float depth;

                point[0] = xi ? world_max[0] : world_min[0];
                point[1] = yi ? world_max[1] : world_min[1];
                point[2] = zi ? world_max[2] : world_min[2];
                depth = canvas3d_depth_along_view(c, point);
                if (depth < near_depth)
                    near_depth = depth;
                if (depth > far_depth)
                    far_depth = depth;
            }
        }
    }
    if (!isfinite(near_depth) || !isfinite(far_depth))
        return 0;
    if (out_near_depth)
        *out_near_depth = near_depth;
    if (out_far_depth)
        *out_far_depth = far_depth;
    return 1;
}

/// @brief Build practical view-depth split distances for the active shadow-cascade count.
/// @details Prefer the active camera clip range over per-frame caster bounds so cascades do not
///   move every time an object streams, animates, or gets culled. If the cached camera range is
///   unavailable, fall back to the draw bounds to preserve shadows in unusual internal call paths.
///   Uses a uniform/logarithmic blend so near cascades get enough precision to avoid shimmer while
///   far cascades still cover the complete clip range.
/// @param c Borrowed Canvas3D supplying clip and shadow-distance state.
/// @param world_min Borrowed fallback caster AABB minimum.
/// @param world_max Borrowed fallback caster AABB maximum.
/// @param cascade_count Positive requested cascade count, bounded by VGFX3D_CSM_SLOTS.
/// @param out_splits Non-`NULL` cascade-distance output array.
/// @param out_near_depth Optional effective near-depth output.
/// @param out_far_depth Optional effective far-depth output.
/// @return Non-zero when a usable finite cascade interval is produced.
static int canvas3d_compute_shadow_cascade_splits(const rt_canvas3d *c,
                                                  const float *world_min,
                                                  const float *world_max,
                                                  int32_t cascade_count,
                                                  float *out_splits,
                                                  float *out_near_depth,
                                                  float *out_far_depth) {
    float near_depth;
    float far_depth;
    float span;

    if (!out_splits || cascade_count <= 0)
        return 0;
    /* out_splits is a cascade array: callers pass VGFX3D_CSM_SLOTS entries. */
    for (int32_t i = 0; i < VGFX3D_CSM_SLOTS; i++)
        out_splits[i] = 0.0f;
    near_depth = c ? c->cached_cam_near : 0.0f;
    far_depth = c ? c->cached_cam_far : 0.0f;
    if (!isfinite(near_depth) || !isfinite(far_depth) || far_depth <= near_depth + 1e-3f) {
        if (!canvas3d_world_bounds_view_depth_range(
                c, world_min, world_max, &near_depth, &far_depth))
            return 0;
    }
    if (far_depth < near_depth) {
        float tmp = near_depth;
        near_depth = far_depth;
        far_depth = tmp;
    }
    /* Cap cascade coverage at the configured shadow distance: spanning the whole clip
     * range spreads the shadow texel budget over kilometers and collapses near-camera
     * resolution into blocky shimmer. Receivers beyond the cap sample outside the last
     * cascade and render unshadowed (the shader fades them out approaching the cap). */
    if (c) {
        float shadow_far = canvas3d_effective_shadow_distance(c);
        if (isfinite(shadow_far) && shadow_far > near_depth && shadow_far < far_depth)
            far_depth = shadow_far;
    }
    if (far_depth - near_depth < 1e-3f)
        far_depth = near_depth + 1.0f;
    span = far_depth - near_depth;
    for (int32_t i = 0; i < cascade_count && i < VGFX3D_CSM_SLOTS; i++) {
        const float p = (float)(i + 1) / (float)cascade_count;
        const float uniform_split = near_depth + span * p;
        float practical_split = uniform_split;
        if (near_depth > 1e-4f && far_depth > near_depth) {
            /* Practical split scheme, lambda = 0.75: weighting the logarithmic series
             * this heavily keeps cascade 0 tight around the camera at large far/near
             * ratios; the residual uniform term stops the last cascade from collapsing. */
            const float log_split = near_depth * powf(far_depth / near_depth, p);
            if (isfinite(log_split))
                practical_split = 0.25f * uniform_split + 0.75f * log_split;
        }
        if (i > 0 && practical_split <= out_splits[i - 1] + 1e-3f)
            practical_split = out_splits[i - 1] + 1e-3f;
        if (i == cascade_count - 1 || practical_split > far_depth)
            practical_split = far_depth;
        out_splits[i] = practical_split;
    }
    if (out_near_depth)
        *out_near_depth = near_depth;
    if (out_far_depth)
        *out_far_depth = far_depth;
    return 1;
}

/// @brief Build stable render-space bounds covered by one camera-depth shadow cascade.
/// @details Cascades must not resize when individual casters stream in/out or cross split
///   boundaries, because changing the fitted orthographic projection causes visible shadow shimmer.
///   This function therefore derives a conservative sphere from the camera depth interval and
///   forward vector, then returns its AABB in the same render space as queued draw bounds.
/// @param c Borrowed Canvas3D supplying camera position and forward vector.
/// @param cmds Borrowed non-empty caster queue used to validate the call.
/// @param count Number of initialized entries in @p cmds.
/// @param cascade_near_depth Near depth of this cascade.
/// @param cascade_far_depth Far depth of this cascade.
/// @param fallback_min Borrowed fallback world AABB minimum.
/// @param fallback_max Borrowed fallback world AABB maximum.
/// @param out_min Non-`NULL` cascade world minimum output.
/// @param out_max Non-`NULL` cascade world maximum output.
/// @return Non-zero when stable or fallback bounds are written.
static int canvas3d_build_shadow_cascade_world_bounds(const rt_canvas3d *c,
                                                      const deferred_draw_t *cmds,
                                                      int32_t count,
                                                      float cascade_near_depth,
                                                      float cascade_far_depth,
                                                      const float *fallback_min,
                                                      const float *fallback_max,
                                                      float *out_min,
                                                      float *out_max) {
    float near_depth;
    float far_depth;
    float depth_mid;
    float radius;

    if (!c || !cmds || count <= 0 || !fallback_min || !fallback_max || !out_min || !out_max)
        return 0;
    (void)cmds;
    (void)count;
    near_depth = cascade_near_depth;
    far_depth = cascade_far_depth;
    if (!isfinite(near_depth) || !isfinite(far_depth) || far_depth <= near_depth) {
        memcpy(out_min, fallback_min, sizeof(float) * 3);
        memcpy(out_max, fallback_max, sizeof(float) * 3);
        return 1;
    }
    depth_mid = 0.5f * (near_depth + far_depth);
    radius = fmaxf(1.0f, fabsf(far_depth) * 0.75f + fabsf(far_depth - near_depth) * 0.25f);
    if (!isfinite(depth_mid) || !isfinite(radius)) {
        memcpy(out_min, fallback_min, sizeof(float) * 3);
        memcpy(out_max, fallback_max, sizeof(float) * 3);
        return 1;
    }
    for (int axis = 0; axis < 3; axis++) {
        float center = c->cached_cam_pos[axis] + c->cached_cam_forward[axis] * depth_mid;
        out_min[axis] = center - radius;
        out_max[axis] = center + radius;
    }
    return 1;
}

/// @brief True if a queued draw's world bounds overlap the supplied camera-depth interval.
/// @details Used during cascaded shadow rendering to avoid submitting every caster to every split.
///   Commands without usable bounds conservatively pass so procedural or corrupted bounds do not
///   drop valid shadows.
/// @param c Borrowed Canvas3D supplying camera depth projection.
/// @param dd Borrowed deferred caster.
/// @param near_depth Inclusive cascade near depth.
/// @param far_depth Inclusive cascade far depth.
/// @return Non-zero when bounds overlap or cannot be safely classified.
static int canvas3d_deferred_intersects_view_depth(rt_canvas3d *c,
                                                   const deferred_draw_t *dd,
                                                   float near_depth,
                                                   float far_depth) {
    float draw_min[3];
    float draw_max[3];
    float draw_near;
    float draw_far;
    int8_t has_bounds = 0;

    if (!c || !dd)
        return 1;
    canvas3d_accumulate_deferred_world_bounds(c, dd, draw_min, draw_max, &has_bounds);
    if (!has_bounds)
        return 1;
    if (!canvas3d_world_bounds_view_depth_range(c, draw_min, draw_max, &draw_near, &draw_far))
        return 1;
    if (draw_far < draw_near) {
        float tmp = draw_near;
        draw_near = draw_far;
        draw_far = tmp;
    }
    return draw_far >= near_depth && draw_near <= far_depth;
}

/// @brief True if a deferred draw's cached world bounds overlap a supplied world-space AABB.
/// @details Cascaded shadow maps first select casters by camera-depth interval, then by the fitted
///   cascade volume. This second test prevents far-away casters that share a depth slice but sit
///   outside the cascade fit from wasting shadow-map draw calls and perturbing depth precision.
/// @param c Borrowed Canvas3D used only when uncached bounds must be computed.
/// @param dd Borrowed deferred caster.
/// @param world_min Borrowed query AABB minimum.
/// @param world_max Borrowed query AABB maximum.
/// @return Non-zero when bounds overlap or cannot be safely classified.
static int canvas3d_deferred_intersects_world_bounds(rt_canvas3d *c,
                                                     const deferred_draw_t *dd,
                                                     const float *world_min,
                                                     const float *world_max) {
    float draw_min[3];
    float draw_max[3];
    int8_t has_bounds = 0;

    if (!c || !dd || !world_min || !world_max)
        return 1;
    canvas3d_accumulate_deferred_world_bounds(c, dd, draw_min, draw_max, &has_bounds);
    if (!has_bounds)
        return 1;
    for (int axis = 0; axis < 3; axis++) {
        if (draw_max[axis] < world_min[axis] || draw_min[axis] > world_max[axis])
            return 0;
    }
    return 1;
}

/// @brief Decide whether a queued deferred draw survives frustum culling.
/// @details Accumulates the union of the draw's world-space AABBs (one per
///   instance for instanced batches, otherwise the single transformed bound)
///   and tests it against the view frustum. Draws without usable bounds
///   (e.g. procedural geometry with no declared local AABB) conservatively
///   pass so nothing visible is clipped away. Runtime-deformed draws
///   (skinning, morphs) also pass unconditionally: their static Mesh3D AABB
///   describes pre-deformation vertices — for a skinned variant mesh that is
///   a bone-local cluster near the entity origin — so testing it culls
///   characters whose origin leaves the frustum while their posed triangles
///   are on screen (e.g. a broadcast closeup framing a batter's hands).
/// @param c Borrowed Canvas3D used for uncached bound transforms.
/// @param dd Borrowed deferred draw to classify.
/// @param frustum Borrowed normalized view-frustum planes.
/// @return 1 if the draw may be visible (keep it), 0 if definitely outside.
static int canvas3d_deferred_intersects_frustum(rt_canvas3d *c,
                                                const deferred_draw_t *dd,
                                                const vgfx3d_frustum_t *frustum) {
    float world_min[3] = {0.0f, 0.0f, 0.0f};
    float world_max[3] = {0.0f, 0.0f, 0.0f};
    int8_t has_bounds = 0;

    if (!c || !dd || !frustum)
        return 1;
    if (canvas3d_cmd_has_runtime_deformation(&dd->cmd))
        return 1;
    canvas3d_accumulate_deferred_world_bounds(c, dd, world_min, world_max, &has_bounds);
    if (!has_bounds)
        return 1;
    return vgfx3d_frustum_test_aabb_padded(frustum, world_min, world_max, dd->culling_pad) != 0;
}

/// @brief Reset the software occlusion grid (clear all cells to "not occluded").
/// @param grid Mutable coarse grid to initialize.
static void canvas3d_occlusion_grid_clear(canvas3d_occlusion_grid_t *grid) {
    if (!grid)
        return;
    for (int32_t i = 0; i < CANVAS3D_OCCLUSION_GRID_CELLS; ++i) {
        grid->depth[i] = FLT_MAX;
        grid->covered[i] = 0;
    }
}

/// @brief Clear the CPU occlusion-history hash index while keeping allocated storage.
/// @param c Canvas3D owning the hash array.
static void canvas3d_occlusion_history_hash_clear(rt_canvas3d *c) {
    if (!c || !c->occlusion_history_hash || c->occlusion_history_hash_capacity <= 0)
        return;
    for (int32_t i = 0; i < c->occlusion_history_hash_capacity; i++)
        c->occlusion_history_hash[i] = -1;
}

/// @brief Drop all CPU occlusion history while keeping allocated tables reusable.
/// @details Visibility-history streaks are tied to the prior camera/projection/render-target
///          relationship. Clearing counts but retaining capacity makes invalidation cheap during
///          floating-origin rebases, camera cuts, output resizes, and occlusion-setting toggles.
/// @param c Canvas3D whose logical history and hash entries are reset.
void canvas3d_clear_occlusion_history(rt_canvas3d *c) {
    if (!c)
        return;
    c->occlusion_history_count = 0;
    canvas3d_occlusion_history_hash_clear(c);
}

/// @brief Rebuild the CPU occlusion-history open-addressing index from compact entries.
/// @param c Canvas3D whose hash is reconstructed from the authoritative history array.
static void canvas3d_occlusion_history_rebuild_hash(rt_canvas3d *c) {
    if (!c || !c->occlusion_history_hash || c->occlusion_history_hash_capacity <= 0)
        return;
    canvas3d_occlusion_history_hash_clear(c);
    int32_t mask = c->occlusion_history_hash_capacity - 1;
    for (int32_t i = 0; i < c->occlusion_history_count; i++) {
        uintptr_t key = c->occlusion_history[i].key;
        int32_t slot = (int32_t)(canvas3d_hash_u64(key) & (uint32_t)mask);
        for (int32_t probe = 0; probe < c->occlusion_history_hash_capacity; probe++) {
            if (c->occlusion_history_hash[slot] < 0) {
                c->occlusion_history_hash[slot] = i;
                break;
            }
            slot = (slot + 1) & mask;
        }
    }
}

/// @brief Ensure occlusion-history storage can hold @p needed entries and a sparse hash index.
/// @param c Canvas3D owning history and hash allocations.
/// @param needed Non-negative minimum number of entries.
/// @return Non-zero when both allocations have sufficient capacity.
static int canvas3d_ensure_occlusion_history_capacity(rt_canvas3d *c, int32_t needed) {
    int32_t new_cap;
    int32_t hash_cap;

    if (!c || needed < 0 || needed > (INT32_C(1) << 29))
        return 0;
    if (c->occlusion_history_capacity < needed) {
        new_cap = c->occlusion_history_capacity > 0 ? c->occlusion_history_capacity : 64;
        while (new_cap < needed) {
            if (new_cap > INT32_MAX / 2)
                new_cap = needed;
            else
                new_cap *= 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(*c->occlusion_history))
            return 0;
        rt_canvas3d_occlusion_history_entry *grown = (rt_canvas3d_occlusion_history_entry *)realloc(
            c->occlusion_history, (size_t)new_cap * sizeof(*grown));
        if (!grown)
            return 0;
        c->occlusion_history = grown;
        c->occlusion_history_capacity = new_cap;
    }
    hash_cap = canvas3d_next_power_of_two_i32(needed > 0 ? needed * 2 : 64);
    if (hash_cap < 64)
        hash_cap = 64;
    if (c->occlusion_history_hash_capacity < hash_cap) {
        if ((size_t)hash_cap > SIZE_MAX / sizeof(*c->occlusion_history_hash))
            return 0;
        int32_t *grown =
            (int32_t *)realloc(c->occlusion_history_hash, (size_t)hash_cap * sizeof(*grown));
        if (!grown)
            return 0;
        c->occlusion_history_hash = grown;
        c->occlusion_history_hash_capacity = hash_cap;
        canvas3d_occlusion_history_hash_clear(c);
        canvas3d_occlusion_history_rebuild_hash(c);
    }
    return 1;
}

/// @brief Find or optionally create an occlusion-history entry for @p key.
/// @details Returns NULL when the key is absent and @p create is false, or when allocation fails.
/// @param c Canvas3D owning the history table.
/// @param key Non-zero final occlusion identity.
/// @param create Non-zero to append and index a missing entry.
/// @return Mutable matching or newly created entry, or `NULL`.
static rt_canvas3d_occlusion_history_entry *canvas3d_occlusion_history_entry(rt_canvas3d *c,
                                                                             uintptr_t key,
                                                                             int create) {
    int32_t mask;
    int32_t slot;

    if (!c || key == 0)
        return NULL;
    if (!canvas3d_ensure_occlusion_history_capacity(c,
                                                    c->occlusion_history_count + (create ? 1 : 0)))
        return NULL;
    mask = c->occlusion_history_hash_capacity - 1;
    slot = (int32_t)(canvas3d_hash_u64(key) & (uint32_t)mask);
    for (int32_t probe = 0; probe < c->occlusion_history_hash_capacity; probe++) {
        int32_t index = c->occlusion_history_hash[slot];
        if (index < 0) {
            if (!create)
                return NULL;
            index = c->occlusion_history_count++;
            c->occlusion_history[index].key = key;
            c->occlusion_history[index].covered_streak = 0;
            c->occlusion_history[index].last_frame_seen = 0;
            c->occlusion_history_hash[slot] = index;
            return &c->occlusion_history[index];
        }
        if (index >= 0 && index < c->occlusion_history_count &&
            c->occlusion_history[index].key == key)
            return &c->occlusion_history[index];
        slot = (slot + 1) & mask;
    }
    return NULL;
}

/// @brief Number of consecutive covered frames required before CPU occlusion can hide a draw.
/// @details Even solid opaque meshes can flicker when their projected AABB is near a depth edge or
///          the camera jitters. Exact, depth-writing draws use a two-frame gate; conservative or
///          coverage-ambiguous draws require an additional confirmation frame when they are not
///          already excluded from testing.
/// @param dd Borrowed deferred draw whose material and safety flags are inspected.
/// @return Required consecutive-covered-frame count, either two or three.
static int32_t canvas3d_occlusion_required_covered_streak(const deferred_draw_t *dd) {
    const vgfx3d_draw_cmd_t *cmd;

    if (!dd || dd->conservative_bounds || dd->occlusion_test_disabled ||
        dd->occlusion_write_disabled || dd->requires_blend)
        return 3;
    cmd = &dd->cmd;
    return (cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE && !cmd->has_alpha_texture &&
            !canvas3d_cmd_has_runtime_deformation(cmd))
               ? 2
               : 3;
}

/// @brief Update occlusion history and decide whether a covered draw may be culled this frame.
/// @details A draw must be reported covered on at least two consecutive frames before the coarse
///          CPU grid culls it. This one-frame grace period prevents visible flicker from noisy
///          projected AABBs, moving occluders, or camera jitter.
/// @param c Canvas3D owning temporal covered-streak history.
/// @param dd Borrowed covered draw with its final occlusion key.
/// @return Non-zero once the draw reaches its required covered streak.
static int canvas3d_occlusion_history_allows_cull(rt_canvas3d *c, const deferred_draw_t *dd) {
    uintptr_t key = dd ? dd->occlusion_key : 0;
    rt_canvas3d_occlusion_history_entry *entry = canvas3d_occlusion_history_entry(c, key, 1);
    if (!entry)
        return 0;
    if (entry->last_frame_seen == c->frame_serial - 1)
        entry->covered_streak++;
    else
        entry->covered_streak = 1;
    entry->last_frame_seen = c->frame_serial;
    return entry->covered_streak >= canvas3d_occlusion_required_covered_streak(dd);
}

/// @brief Reset the occlusion covered streak for a draw that remained visible.
/// @param c Canvas3D owning temporal covered-streak history.
/// @param dd Borrowed visible draw whose existing entry is updated.
static void canvas3d_occlusion_history_note_visible(rt_canvas3d *c, const deferred_draw_t *dd) {
    rt_canvas3d_occlusion_history_entry *entry;
    if (!c || !dd || !dd->occlusion_key)
        return;
    entry = canvas3d_occlusion_history_entry(c, dd->occlusion_key, 0);
    if (!entry)
        return;
    entry->covered_streak = 0;
    entry->last_frame_seen = c->frame_serial;
}

/// @brief Drop stale CPU occlusion-history entries and rebuild their hash table.
/// @details The hysteresis table is keyed by stable draw identity, but games can still create
///          many short-lived meshes, materials, and instances over a long session. Pruning entries
///          unseen for two seconds at 60 Hz keeps memory bounded without losing the recent
///          covered-streak state that prevents one-frame culling flicker.
/// @param c Canvas3D whose history is compacted and reindexed.
static void canvas3d_occlusion_history_prune(rt_canvas3d *c) {
    int32_t write_index = 0;
    int64_t cutoff;

    if (!c || c->occlusion_history_count <= 0)
        return;
    if (c->frame_serial <= CANVAS3D_OCCLUSION_HISTORY_RETENTION_FRAMES)
        return;
    cutoff = c->frame_serial - CANVAS3D_OCCLUSION_HISTORY_RETENTION_FRAMES;
    for (int32_t read_index = 0; read_index < c->occlusion_history_count; read_index++) {
        rt_canvas3d_occlusion_history_entry entry = c->occlusion_history[read_index];
        if (entry.last_frame_seen >= cutoff) {
            if (write_index != read_index)
                c->occlusion_history[write_index] = entry;
            write_index++;
        }
    }
    if (write_index == c->occlusion_history_count)
        return;
    c->occlusion_history_count = write_index;
    canvas3d_occlusion_history_rebuild_hash(c);
}

/// @brief Return non-zero when scalar camera parameters differ enough to invalidate occlusion.
/// @param a Previous scalar parameter.
/// @param b Current scalar parameter.
/// @return Non-zero when relative difference exceeds the configured tolerance.
static int canvas3d_occlusion_param_changed(float a, float b) {
    float scale = fmaxf(1.0f, fmaxf(fabsf(a), fabsf(b)));
    return fabsf(a - b) > scale * 1e-4f;
}

/// @brief Return non-zero when the current camera/output state is a new occlusion space.
/// @details Small camera movement should keep history so occlusion can work. Large teleports,
///          sharp camera cuts, projection-range changes, output-size changes, and render-target
///          switches invalidate old covered streaks because the old screen coverage no longer
///          predicts the new one.
/// @param c Borrowed Canvas3D containing previous and current space signatures.
/// @param output_w Current render-output width in pixels.
/// @param output_h Current render-output height in pixels.
/// @return Non-zero when history must be invalidated.
static int canvas3d_occlusion_state_changed_for_begin(const rt_canvas3d *c,
                                                      int32_t output_w,
                                                      int32_t output_h) {
    double dx;
    double dy;
    double dz;
    double dist2;
    float far_plane;
    float cut_distance;
    float dot;

    if (!c || !c->occlusion_state_valid)
        return 1;
    if (c->occlusion_last_render_target_identity !=
            (c->render_target ? c->render_target->cache_identity : 0u) ||
        c->occlusion_last_output_width != output_w || c->occlusion_last_output_height != output_h ||
        c->occlusion_last_is_ortho != c->cached_cam_is_ortho)
        return 1;
    if (canvas3d_occlusion_param_changed(c->occlusion_last_near, c->cached_cam_near) ||
        canvas3d_occlusion_param_changed(c->occlusion_last_far, c->cached_cam_far))
        return 1;

    dx = c->cached_world_cam_pos[0] - c->occlusion_last_world_cam_pos[0];
    dy = c->cached_world_cam_pos[1] - c->occlusion_last_world_cam_pos[1];
    dz = c->cached_world_cam_pos[2] - c->occlusion_last_world_cam_pos[2];
    dist2 = dx * dx + dy * dy + dz * dz;
    far_plane =
        isfinite(c->cached_cam_far) && c->cached_cam_far > 1.0f ? c->cached_cam_far : 1000.0f;
    cut_distance = fmaxf(8.0f, fminf(far_plane * 0.20f, 100000.0f));
    if (isfinite(dist2) && dist2 > cut_distance * cut_distance)
        return 1;

    dot = c->cached_cam_forward[0] * c->occlusion_last_cam_forward[0] +
          c->cached_cam_forward[1] * c->occlusion_last_cam_forward[1] +
          c->cached_cam_forward[2] * c->occlusion_last_cam_forward[2];
    if (!isfinite(dot) || dot < 0.75f)
        return 1;
    return 0;
}

/// @brief Update the occlusion-space signature captured at Canvas3D.Begin.
/// @details Clears covered-streak history before storing the new signature when a discontinuity is
///          detected. This keeps the first frame after a camera cut or output switch conservative.
/// @param c Mutable Canvas3D receiving the current space signature.
/// @param output_w Current render-output width in pixels.
/// @param output_h Current render-output height in pixels.
static void canvas3d_update_occlusion_state_for_begin(rt_canvas3d *c,
                                                      int32_t output_w,
                                                      int32_t output_h) {
    if (!c)
        return;
    if (canvas3d_occlusion_state_changed_for_begin(c, output_w, output_h))
        canvas3d_clear_occlusion_history(c);
    c->occlusion_state_valid = 1;
    c->occlusion_last_render_target_identity =
        c->render_target ? c->render_target->cache_identity : 0u;
    c->occlusion_last_output_width = output_w;
    c->occlusion_last_output_height = output_h;
    memcpy(c->occlusion_last_world_cam_pos,
           c->cached_world_cam_pos,
           sizeof(c->occlusion_last_world_cam_pos));
    memcpy(c->occlusion_last_cam_forward,
           c->cached_cam_forward,
           sizeof(c->occlusion_last_cam_forward));
    c->occlusion_last_near = c->cached_cam_near;
    c->occlusion_last_far = c->cached_cam_far;
    c->occlusion_last_is_ortho = c->cached_cam_is_ortho;
}

/// @brief Project a world point to floating-point occlusion-grid coordinates.
/// @param c Borrowed Canvas3D supplying the cached view-projection matrix.
/// @param point Borrowed three-component render-space point.
/// @param out_x Non-`NULL` horizontal grid coordinate output.
/// @param out_y Non-`NULL` vertical grid coordinate output.
/// @return 1 if the point projects in front of the camera and inside the grid, 0 otherwise.
static int canvas3d_project_world_point_to_occlusion_grid(const rt_canvas3d *c,
                                                          const float point[3],
                                                          float *out_x,
                                                          float *out_y) {
    float clip[4];
    float w;
    float ndc_x;
    float ndc_y;
    float clip_slack;

    if (!c || !point || !out_x || !out_y)
        return 0;
    clip[0] = c->cached_vp[0] * point[0] + c->cached_vp[1] * point[1] + c->cached_vp[2] * point[2] +
              c->cached_vp[3];
    clip[1] = c->cached_vp[4] * point[0] + c->cached_vp[5] * point[1] + c->cached_vp[6] * point[2] +
              c->cached_vp[7];
    clip[2] = c->cached_vp[8] * point[0] + c->cached_vp[9] * point[1] +
              c->cached_vp[10] * point[2] + c->cached_vp[11];
    clip[3] = c->cached_vp[12] * point[0] + c->cached_vp[13] * point[1] +
              c->cached_vp[14] * point[2] + c->cached_vp[15];
    w = clip[3];
    if (!isfinite(clip[0]) || !isfinite(clip[1]) || !isfinite(clip[2]) || !isfinite(w) ||
        w <= 1e-6f)
        return 0;
    clip_slack = fmaxf(1e-5f, fabsf(w) * 1e-5f);
    if (clip[2] < -w - clip_slack || clip[2] > w + clip_slack)
        return 0;
    ndc_x = clip[0] / w;
    ndc_y = clip[1] / w;
    if (!isfinite(ndc_x) || !isfinite(ndc_y))
        return 0;
    *out_x = (ndc_x * 0.5f + 0.5f) * (float)(CANVAS3D_OCCLUSION_GRID_W - 1);
    *out_y = (0.5f - ndc_y * 0.5f) * (float)(CANVAS3D_OCCLUSION_GRID_H - 1);
    return 1;
}

/// @brief Compute the occlusion-grid rectangle and conservative depths for a world-space AABB.
/// @details Projects the box corners to grid space. The candidate is tested at its nearest
///          depth, but visible occluders are written at their farthest depth. Writing the far
///          depth keeps large depth-spanning bounds such as floors from acting like near
///          full-screen walls in the coarse grid.
/// @param c Borrowed Canvas3D supplying camera, grid-expansion, and bound state.
/// @param dd Borrowed deferred draw to project.
/// @param out_min_x Non-`NULL` inclusive minimum cell X output.
/// @param out_min_y Non-`NULL` inclusive minimum cell Y output.
/// @param out_max_x Non-`NULL` inclusive maximum cell X output.
/// @param out_max_y Non-`NULL` inclusive maximum cell Y output.
/// @param out_test_depth Non-`NULL` nearest view-depth output used for coverage tests.
/// @param out_write_depth Non-`NULL` farthest view-depth output used by AABB fallback writes.
/// @return Non-zero when a finite conservative rectangle and depth span are available.
static int canvas3d_deferred_occlusion_rect(rt_canvas3d *c,
                                            const deferred_draw_t *dd,
                                            int32_t *out_min_x,
                                            int32_t *out_min_y,
                                            int32_t *out_max_x,
                                            int32_t *out_max_y,
                                            float *out_test_depth,
                                            float *out_write_depth) {
    float world_min[3] = {0.0f, 0.0f, 0.0f};
    float world_max[3] = {0.0f, 0.0f, 0.0f};
    int8_t has_bounds = 0;
    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    float near_depth = 0.0f;
    float far_depth = 0.0f;

    if (!c || !dd || !out_min_x || !out_min_y || !out_max_x || !out_max_y || !out_test_depth ||
        !out_write_depth)
        return 0;
    if (dd->conservative_bounds)
        return 0;
    if (!isfinite(dd->sort_key))
        return 0;
    canvas3d_accumulate_deferred_world_bounds(c, dd, world_min, world_max, &has_bounds);
    if (!has_bounds)
        return 0;
    if (!canvas3d_world_bounds_view_depth_range(c, world_min, world_max, &near_depth, &far_depth))
        return 0;
    if (far_depth < near_depth) {
        float tmp = near_depth;
        near_depth = far_depth;
        far_depth = tmp;
    }
    if (far_depth - near_depth > fmaxf(8.0f, fabsf(near_depth) * 0.5f))
        return 0;

    for (int xi = 0; xi < 2; ++xi) {
        for (int yi = 0; yi < 2; ++yi) {
            for (int zi = 0; zi < 2; ++zi) {
                float point[3];
                float sx;
                float sy;
                point[0] = xi ? world_max[0] : world_min[0];
                point[1] = yi ? world_max[1] : world_min[1];
                point[2] = zi ? world_max[2] : world_min[2];
                if (!canvas3d_project_world_point_to_occlusion_grid(c, point, &sx, &sy))
                    return 0;
                if (sx < min_x)
                    min_x = sx;
                if (sy < min_y)
                    min_y = sy;
                if (sx > max_x)
                    max_x = sx;
                if (sy > max_y)
                    max_y = sy;
            }
        }
    }
    if (!isfinite(min_x) || !isfinite(min_y) || !isfinite(max_x) || !isfinite(max_y))
        return 0;
    if (max_x < 0.0f || max_y < 0.0f || min_x > (float)(CANVAS3D_OCCLUSION_GRID_W - 1) ||
        min_y > (float)(CANVAS3D_OCCLUSION_GRID_H - 1))
        return 0;
    if (min_x < 0.0f)
        min_x = 0.0f;
    if (min_y < 0.0f)
        min_y = 0.0f;
    if (max_x > (float)(CANVAS3D_OCCLUSION_GRID_W - 1))
        max_x = (float)(CANVAS3D_OCCLUSION_GRID_W - 1);
    if (max_y > (float)(CANVAS3D_OCCLUSION_GRID_H - 1))
        max_y = (float)(CANVAS3D_OCCLUSION_GRID_H - 1);

    *out_min_x = canvas3d_floor_to_i32_clamped(min_x, 0, CANVAS3D_OCCLUSION_GRID_W - 1);
    *out_min_y = canvas3d_floor_to_i32_clamped(min_y, 0, CANVAS3D_OCCLUSION_GRID_H - 1);
    *out_max_x = canvas3d_ceil_to_i32_clamped(max_x, 0, CANVAS3D_OCCLUSION_GRID_W - 1);
    *out_max_y = canvas3d_ceil_to_i32_clamped(max_y, 0, CANVAS3D_OCCLUSION_GRID_H - 1);
    {
        int32_t expand = c->occlusion_rect_expand_cells > 0 ? c->occlusion_rect_expand_cells : 0;
        if (expand > 4)
            expand = 4;
        *out_min_x = canvas3d_clamp_i32(*out_min_x - expand, 0, CANVAS3D_OCCLUSION_GRID_W - 1);
        *out_min_y = canvas3d_clamp_i32(*out_min_y - expand, 0, CANVAS3D_OCCLUSION_GRID_H - 1);
        *out_max_x = canvas3d_clamp_i32(*out_max_x + expand, 0, CANVAS3D_OCCLUSION_GRID_W - 1);
        *out_max_y = canvas3d_clamp_i32(*out_max_y + expand, 0, CANVAS3D_OCCLUSION_GRID_H - 1);
    }
    if (*out_max_x < *out_min_x || *out_max_y < *out_min_y)
        return 0;
    /* Large rects are allowed: the most effective occluders — near walls, buildings,
     * terrain ridges — legitimately cover most of the screen, and rejecting them made
     * corridor/urban scenes cull almost nothing. Write safety for deep boxes is handled
     * by canvas3d_occlusion_depth_span_can_write; the coarse grid is small enough that
     * a full-grid scan per candidate stays cheap. */
    *out_test_depth = near_depth;
    *out_write_depth = far_depth;
    return 1;
}

/// @brief Whether the grid already occludes the given cell rect at @p depth.
/// @details The margin intentionally over-estimates visibility so the coarse grid cannot flicker
///   triangles out when projected bounds or depth values jitter by a fraction of a cell.
/// @param c Optional Canvas3D supplying conservative depth margins.
/// @param grid Borrowed coarse coverage/depth grid.
/// @param min_x Inclusive minimum cell X.
/// @param min_y Inclusive minimum cell Y.
/// @param max_x Inclusive maximum cell X.
/// @param max_y Inclusive maximum cell Y.
/// @param depth Candidate's nearest view depth.
/// @return Non-zero only when every addressed cell has safely nearer coverage.
static int canvas3d_occlusion_grid_covers(const rt_canvas3d *c,
                                          const canvas3d_occlusion_grid_t *grid,
                                          int32_t min_x,
                                          int32_t min_y,
                                          int32_t max_x,
                                          int32_t max_y,
                                          float depth) {
    float margin;
    if (!grid || !isfinite(depth))
        return 0;
    margin = c && isfinite(c->occlusion_depth_margin) && c->occlusion_depth_margin >= 0.0f
                 ? c->occlusion_depth_margin
                 : 0.02f;
    margin += fabsf(depth) * 0.002f;
    if (c && isfinite(c->cached_cam_near) && isfinite(c->cached_cam_far) &&
        c->cached_cam_far > c->cached_cam_near)
        margin += (c->cached_cam_far - c->cached_cam_near) * 1e-5f;
    for (int32_t y = min_y; y <= max_y; ++y) {
        for (int32_t x = min_x; x <= max_x; ++x) {
            int32_t index = y * CANVAS3D_OCCLUSION_GRID_W + x;
            if (!grid->covered[index] || grid->depth[index] > depth - margin)
                return 0;
        }
    }
    return 1;
}

/// @brief Write an occluder's depth into the grid cells it covers (keeping the nearest per cell).
/// @param grid Mutable coarse coverage/depth grid.
/// @param min_x Inclusive minimum cell X.
/// @param min_y Inclusive minimum cell Y.
/// @param max_x Inclusive maximum cell X.
/// @param max_y Inclusive maximum cell Y.
/// @param depth Conservative farthest occluder depth written to each cell.
static void canvas3d_occlusion_grid_write(canvas3d_occlusion_grid_t *grid,
                                          int32_t min_x,
                                          int32_t min_y,
                                          int32_t max_x,
                                          int32_t max_y,
                                          float depth) {
    if (!grid || !isfinite(depth))
        return;
    for (int32_t y = min_y; y <= max_y; ++y) {
        for (int32_t x = min_x; x <= max_x; ++x) {
            int32_t index = y * CANVAS3D_OCCLUSION_GRID_W + x;
            if (!grid->covered[index] || depth < grid->depth[index])
                grid->depth[index] = depth;
            grid->covered[index] = 1;
        }
    }
}

/*==========================================================================
 * CPU occlusion Hi-Z rasterizer
 *
 * The coarse grid's coverage TEST is unchanged (all cells covered and nearer
 * than the candidate, with margins and covered-streak hysteresis). WRITES
 * rasterize the occluder's actual triangles into a fine per-texel view-depth
 * buffer and fold each fully-written 4x4 block into the coarse grid as its
 * conservative max depth. A draw is an occluder ONLY when its full triangle
 * set fits the precise budgets: over-budget meshes are occludees only. The
 * retired proxy path (a centered 512-triangle subset) could not invent
 * coverage itself, but once its per-frame budget was exhausted over-budget
 * draws fell through to the AABB-rectangle fallback, which published each
 * mesh's ENTIRE projected bounds as solid — gaps, seams, and sky included —
 * and erased visible geometry behind them (the stadium-shell chunk loss).
 * AABB rectangles remain only as the shallow-depth fallback for
 * non-rasterizable draws.
 *=========================================================================*/

#define CANVAS3D_HIZ_BLOCK 4
#define CANVAS3D_HIZ_W (CANVAS3D_OCCLUSION_GRID_W * CANVAS3D_HIZ_BLOCK)
#define CANVAS3D_HIZ_H (CANVAS3D_OCCLUSION_GRID_H * CANVAS3D_HIZ_BLOCK)
#define CANVAS3D_HIZ_CELLS (CANVAS3D_HIZ_W * CANVAS3D_HIZ_H)
#define CANVAS3D_HIZ_MAX_TRIS_PER_DRAW 1024
#define CANVAS3D_HIZ_MAX_TRIS_PER_FRAME 8192
#define CANVAS3D_HIZ_MAX_VERTS_PER_DRAW 4096

/// @brief Reset the fine Hi-Z buffer and the per-frame triangle budget.
/// @param c Canvas3D owning fine depth storage and budget counters.
static void canvas3d_hiz_frame_reset(rt_canvas3d *c) {
    if (!c)
        return;
    c->hiz_frame_triangles = 0;
    if (c->hiz_depth) {
        for (int32_t i = 0; i < CANVAS3D_HIZ_CELLS; i++)
            c->hiz_depth[i] = FLT_MAX;
    }
}

/// @brief Lazily allocate the fine Hi-Z buffer (initialized to "far").
/// @param c Canvas3D that owns the fixed-size buffer.
/// @return Non-zero when initialized storage is available.
static int canvas3d_hiz_ensure_buffer(rt_canvas3d *c) {
    if (!c)
        return 0;
    if (c->hiz_depth)
        return 1;
    c->hiz_depth = (float *)malloc((size_t)CANVAS3D_HIZ_CELLS * sizeof(float));
    if (!c->hiz_depth)
        return 0;
    for (int32_t i = 0; i < CANVAS3D_HIZ_CELLS; i++)
        c->hiz_depth[i] = FLT_MAX;
    return 1;
}

/// @brief Ensure the projected-vertex scratch can hold @p needed vertices (4 floats each).
/// @param c Canvas3D owning reusable projection scratch.
/// @param needed Positive vertex count within the per-draw limit.
/// @return Non-zero when scratch capacity is available.
static int canvas3d_hiz_ensure_scratch(rt_canvas3d *c, int32_t needed) {
    float *grown;
    if (!c || needed <= 0 || needed > CANVAS3D_HIZ_MAX_VERTS_PER_DRAW)
        return 0;
    if (c->hiz_vertex_scratch && c->hiz_vertex_scratch_capacity >= needed)
        return 1;
    grown = (float *)realloc(c->hiz_vertex_scratch, (size_t)needed * 4u * sizeof(float));
    if (!grown)
        return 0;
    c->hiz_vertex_scratch = grown;
    c->hiz_vertex_scratch_capacity = needed;
    return 1;
}

/// @brief Project one object-space vertex into fine Hi-Z screen coordinates.
/// @details The output stores fine-buffer X/Y, reciprocal clip W for perspective-correct depth,
///   and a validity lane. Vertices outside the conservative clip guard are invalidated so every
///   touching triangle is omitted instead of clipped into potentially invented coverage. The
///   helper is shared by the full preproject path and allocation-free coarse-proxy path.
/// @param mvp Row-major model-view-projection matrix.
/// @param position Object-space XYZ position.
/// @param out Four-float projected record receiving X, Y, reciprocal W, and validity.
/// @return Non-zero when the vertex is finite and within the conservative clip guard.
static int canvas3d_hiz_project_vertex(const float mvp[16], const float position[3], float out[4]) {
    float cx;
    float cy;
    float cz;
    float cw;
    if (!mvp || !position || !out)
        return 0;
    cx = mvp[0] * position[0] + mvp[1] * position[1] + mvp[2] * position[2] + mvp[3];
    cy = mvp[4] * position[0] + mvp[5] * position[1] + mvp[6] * position[2] + mvp[7];
    cz = mvp[8] * position[0] + mvp[9] * position[1] + mvp[10] * position[2] + mvp[11];
    cw = mvp[12] * position[0] + mvp[13] * position[1] + mvp[14] * position[2] + mvp[15];
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(cz) || !isfinite(cw) || cw <= 1e-6f ||
        fabsf(cz) > cw * 1.05f) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 0.0f;
        return 0;
    }
    out[0] = (cx / cw * 0.5f + 0.5f) * (float)CANVAS3D_HIZ_W;
    out[1] = (0.5f - cy / cw * 0.5f) * (float)CANVAS3D_HIZ_H;
    out[2] = 1.0f / cw;
    out[3] = 1.0f;
    return 1;
}

/// @brief Fold every coarse cell overlapping the fine-texel bbox into the grid.
/// @details A cell becomes covered only when all 16 of its fine texels were written;
///   its contribution is the block's MAX (farthest) depth, so a candidate passes the
///   coverage test only when it is farther than every rasterized sample in the cell.
/// @param c Borrowed Canvas3D containing the fine depth buffer.
/// @param grid Mutable coarse grid receiving complete blocks.
/// @param fine_min_x Inclusive fine-buffer minimum X touched.
/// @param fine_min_y Inclusive fine-buffer minimum Y touched.
/// @param fine_max_x Inclusive fine-buffer maximum X touched.
/// @param fine_max_y Inclusive fine-buffer maximum Y touched.
static void canvas3d_hiz_flush_blocks(const rt_canvas3d *c,
                                      canvas3d_occlusion_grid_t *grid,
                                      int32_t fine_min_x,
                                      int32_t fine_min_y,
                                      int32_t fine_max_x,
                                      int32_t fine_max_y) {
    int32_t cell_min_x = fine_min_x / CANVAS3D_HIZ_BLOCK;
    int32_t cell_min_y = fine_min_y / CANVAS3D_HIZ_BLOCK;
    int32_t cell_max_x = fine_max_x / CANVAS3D_HIZ_BLOCK;
    int32_t cell_max_y = fine_max_y / CANVAS3D_HIZ_BLOCK;

    if (!c || !c->hiz_depth || !grid)
        return;
    if (cell_min_x < 0)
        cell_min_x = 0;
    if (cell_min_y < 0)
        cell_min_y = 0;
    if (cell_max_x >= CANVAS3D_OCCLUSION_GRID_W)
        cell_max_x = CANVAS3D_OCCLUSION_GRID_W - 1;
    if (cell_max_y >= CANVAS3D_OCCLUSION_GRID_H)
        cell_max_y = CANVAS3D_OCCLUSION_GRID_H - 1;
    for (int32_t cy = cell_min_y; cy <= cell_max_y; cy++) {
        for (int32_t cx = cell_min_x; cx <= cell_max_x; cx++) {
            float block_max = 0.0f;
            int complete = 1;
            for (int32_t by = 0; by < CANVAS3D_HIZ_BLOCK && complete; by++) {
                const float *row =
                    &c->hiz_depth[(size_t)(cy * CANVAS3D_HIZ_BLOCK + by) * (size_t)CANVAS3D_HIZ_W +
                                  (size_t)(cx * CANVAS3D_HIZ_BLOCK)];
                for (int32_t bx = 0; bx < CANVAS3D_HIZ_BLOCK; bx++) {
                    float d = row[bx];
                    if (d >= FLT_MAX * 0.5f) {
                        complete = 0;
                        break;
                    }
                    if (d > block_max)
                        block_max = d;
                }
            }
            if (!complete)
                continue;
            {
                int32_t index = cy * CANVAS3D_OCCLUSION_GRID_W + cx;
                if (!grid->covered[index] || block_max < grid->depth[index])
                    grid->depth[index] = block_max;
                grid->covered[index] = 1;
            }
        }
    }
}

/// @brief Rasterize one draw's full triangle set into the fine Hi-Z buffer.
/// @details Only meshes within the precise budgets write occlusion: every vertex projects once
///   and every triangle rasterizes, so published coverage is the geometry's own. An over-budget
///   draw (vertex/triangle limit or the exhausted precise frame budget) is consumed WITHOUT
///   writing — it remains an occludee but never an occluder. The retired subset-proxy path was
///   honest per-triangle, but its exhausted frame budget dropped over-budget draws into the
///   projected-AABB fallback, which published whole bounds rectangles (gaps and sky included) and
///   culled visible geometry behind them. Fine blocks still publish only after all 16 texels have
///   real coverage, so edge-visible candidates are never culled by an outer AABB rectangle.
/// @param c Canvas3D owning fine depth, scratch, camera matrix, and budgets.
/// @param grid Mutable coarse grid receiving completed fine blocks.
/// @param dd Borrowed deferred mesh draw considered as an occluder.
/// @return 1 when the draw was consumed (rasterized, or over-budget and deliberately not an
///   occluder — both disallow an AABB write), or 0 when the draw is ineligible or storage is
///   unavailable and the caller may try the separately depth-guarded AABB fallback.
static int canvas3d_hiz_rasterize_draw(rt_canvas3d *c,
                                       canvas3d_occlusion_grid_t *grid,
                                       const deferred_draw_t *dd) {
    const vgfx3d_draw_cmd_t *cmd;
    float mvp[16];
    float *verts;
    int32_t source_tri_count;
    int32_t raster_tri_count;
    int32_t fine_min_x = CANVAS3D_HIZ_W;
    int32_t fine_min_y = CANVAS3D_HIZ_H;
    int32_t fine_max_x = -1;
    int32_t fine_max_y = -1;
    int rasterized_any = 0;

    if (!c || !grid || !dd || c->cached_cam_is_ortho)
        return 0;
    cmd = &dd->cmd;
    if (dd->kind != DEFERRED_DRAW_MESH || dd->instance_count > 0)
        return 0;
    if (!cmd->vertices || !cmd->indices || cmd->index_count < 3u || cmd->vertex_count == 0)
        return 0;
    source_tri_count = (int32_t)(cmd->index_count / 3u);
    raster_tri_count = source_tri_count;
    if (source_tri_count > CANVAS3D_HIZ_MAX_TRIS_PER_DRAW ||
        cmd->vertex_count > (uint32_t)CANVAS3D_HIZ_MAX_VERTS_PER_DRAW ||
        source_tri_count > CANVAS3D_HIZ_MAX_TRIS_PER_FRAME - c->hiz_frame_triangles) {
        /* Consumed, not an occluder: partial coverage cannot be published
         * honestly, and the AABB fallback would invent it wholesale. */
        return 1;
    }
    if (!canvas3d_hiz_ensure_buffer(c) ||
        !canvas3d_hiz_ensure_scratch(c, (int32_t)cmd->vertex_count))
        return 0;

    canvas3d_mul_mat4(c->cached_vp, cmd->model_matrix, mvp);
    verts = c->hiz_vertex_scratch;
    for (uint32_t i = 0; i < cmd->vertex_count; i++)
        (void)canvas3d_hiz_project_vertex(mvp, cmd->vertices[i].pos, &verts[(size_t)i * 4u]);

    for (int32_t source_triangle = 0; source_triangle < raster_tri_count; source_triangle++) {
        uint32_t i0 = cmd->indices[(size_t)source_triangle * 3u];
        uint32_t i1 = cmd->indices[(size_t)source_triangle * 3u + 1u];
        uint32_t i2 = cmd->indices[(size_t)source_triangle * 3u + 2u];
        const float *v0;
        const float *v1;
        const float *v2;
        float area;
        int32_t bb_min_x;
        int32_t bb_min_y;
        int32_t bb_max_x;
        int32_t bb_max_y;
        if (i0 >= cmd->vertex_count || i1 >= cmd->vertex_count || i2 >= cmd->vertex_count)
            continue;
        v0 = &verts[(size_t)i0 * 4u];
        v1 = &verts[(size_t)i1 * 4u];
        v2 = &verts[(size_t)i2 * 4u];
        if (v0[3] == 0.0f || v1[3] == 0.0f || v2[3] == 0.0f)
            continue;
        area = (v1[0] - v0[0]) * (v2[1] - v0[1]) - (v1[1] - v0[1]) * (v2[0] - v0[0]);
        if (!isfinite(area) || fabsf(area) < 1e-6f)
            continue;
        if (area < 0.0f) {
            const float *tmp = v1;
            v1 = v2;
            v2 = tmp;
            area = -area;
        }
        {
            /* A vertex just past the near plane but far off-axis projects to a
             * screen coordinate far outside int32 range; clamp the bounding box
             * to the buffer in FLOAT space before the cast (matching the main SW
             * raster, sw_raster.inc) so (int32_t)floorf(...) can never be UB. The
             * inside test below still uses the true v0/v1/v2 coords, so clamping
             * the scan box does not distort the triangle. */
            float fmin_x = floorf(fminf(v0[0], fminf(v1[0], v2[0])));
            float fmin_y = floorf(fminf(v0[1], fminf(v1[1], v2[1])));
            float fmax_x = ceilf(fmaxf(v0[0], fmaxf(v1[0], v2[0])));
            float fmax_y = ceilf(fmaxf(v0[1], fmaxf(v1[1], v2[1])));
            float max_x_f = (float)(CANVAS3D_HIZ_W - 1);
            float max_y_f = (float)(CANVAS3D_HIZ_H - 1);
            bb_min_x = (int32_t)fminf(fmaxf(fmin_x, 0.0f), max_x_f);
            bb_min_y = (int32_t)fminf(fmaxf(fmin_y, 0.0f), max_y_f);
            bb_max_x = (int32_t)fminf(fmaxf(fmax_x, 0.0f), max_x_f);
            bb_max_y = (int32_t)fminf(fmaxf(fmax_y, 0.0f), max_y_f);
        }
        if (bb_min_x > bb_max_x || bb_min_y > bb_max_y)
            continue;
        for (int32_t py = bb_min_y; py <= bb_max_y; py++) {
            float sy = (float)py + 0.5f;
            for (int32_t px = bb_min_x; px <= bb_max_x; px++) {
                float sx = (float)px + 0.5f;
                float e0 = (v1[0] - v0[0]) * (sy - v0[1]) - (v1[1] - v0[1]) * (sx - v0[0]);
                float e1 = (v2[0] - v1[0]) * (sy - v1[1]) - (v2[1] - v1[1]) * (sx - v1[0]);
                float e2 = (v0[0] - v2[0]) * (sy - v2[1]) - (v0[1] - v2[1]) * (sx - v2[0]);
                float inv_w;
                float depth;
                size_t index;
                if (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f)
                    continue;
                /* Perspective-correct view depth: 1/w interpolates linearly on screen. */
                inv_w = (e1 * v0[2] + e2 * v1[2] + e0 * v2[2]) / area;
                if (!isfinite(inv_w) || inv_w <= 1e-9f)
                    continue;
                depth = 1.0f / inv_w;
                index = (size_t)py * (size_t)CANVAS3D_HIZ_W + (size_t)px;
                if (depth < c->hiz_depth[index])
                    c->hiz_depth[index] = depth;
            }
        }
        if (bb_min_x < fine_min_x)
            fine_min_x = bb_min_x;
        if (bb_min_y < fine_min_y)
            fine_min_y = bb_min_y;
        if (bb_max_x > fine_max_x)
            fine_max_x = bb_max_x;
        if (bb_max_y > fine_max_y)
            fine_max_y = bb_max_y;
        rasterized_any = 1;
    }
    c->hiz_frame_triangles += raster_tri_count;
    if (rasterized_any)
        canvas3d_hiz_flush_blocks(c, grid, fine_min_x, fine_min_y, fine_max_x, fine_max_y);
    /* The draw was consumed by the rasterizer even if every triangle was clipped;
     * the AABB fallback would only reintroduce proxy coverage the geometry lacks. */
    return 1;
}

/// @brief Return whether an AABB depth span is safe to write into the coarse occlusion grid.
/// @details The grid stores one conservative depth per 64x64 cell, so a deep AABB proxy can behave
///   like a wall and incorrectly hide visible triangles behind its nearest face. Testing remains
///   allowed for wider spans, but writing is limited to shallow proxies whose far/near range is
///   small relative to camera depth.
/// @param test_depth Nearest candidate view depth.
/// @param write_depth Farthest candidate view depth.
/// @return Non-zero when the absolute span is below the conservative depth-relative limit.
static int canvas3d_occlusion_depth_span_can_write(float test_depth, float write_depth) {
    float span;
    float limit;

    if (!isfinite(test_depth) || !isfinite(write_depth))
        return 0;
    span = write_depth - test_depth;
    if (span < 0.0f)
        span = -span;
    limit = fmaxf(2.0f, fabsf(test_depth) * 0.10f);
    return span <= limit;
}

/// @brief Test an object's bounds against the occlusion grid and record visible objects as
/// occluders.
/// @details Combines the cover test with a write so a single pass both culls hidden objects and
/// lets
///          a visible one occlude later draws.
/// @param c Canvas3D supplying histories, camera state, buffers, and telemetry.
/// @param grid Mutable coarse occlusion grid.
/// @param dd Borrowed deferred draw to test and possibly record.
/// @return Non-zero if the object is fully covered and can be culled, 0 if it remains visible.
static int canvas3d_occlusion_test_and_write(rt_canvas3d *c,
                                             canvas3d_occlusion_grid_t *grid,
                                             const deferred_draw_t *dd) {
    int32_t min_x;
    int32_t min_y;
    int32_t max_x;
    int32_t max_y;
    float test_depth;
    float write_depth;
    int covered = 0;
    int have_rect;
    if (!c || !grid || !dd)
        return 0;
    if (dd->occlusion_test_disabled && dd->occlusion_write_disabled)
        return 0;
    /* Testing needs the candidate's projected AABB rect; writing no longer does — the
     * Hi-Z rasterizer consumes actual triangles, so a deep-AABB occluder (rotated wall,
     * long ramp) still contributes coverage even when its rect is untestable. */
    have_rect = canvas3d_deferred_occlusion_rect(
        c, dd, &min_x, &min_y, &max_x, &max_y, &test_depth, &write_depth);
    if (have_rect) {
        c->last_occlusion_candidate_count++;
        if (!dd->occlusion_test_disabled &&
            canvas3d_occlusion_grid_covers(c, grid, min_x, min_y, max_x, max_y, test_depth)) {
            covered = 1;
            if (canvas3d_occlusion_history_allows_cull(c, dd))
                return 1;
        }
        if (!covered)
            canvas3d_occlusion_history_note_visible(c, dd);
    }
    /* The AABB fallback publishes a solid rectangle, so it is reserved for
     * non-rasterizable single draws with a shallow depth span. An instanced
     * draw's union rect is not coverage — the instances are scattered. */
    if (!dd->occlusion_write_disabled && !canvas3d_hiz_rasterize_draw(c, grid, dd) &&
        dd->instance_count == 0 && have_rect &&
        canvas3d_occlusion_depth_span_can_write(test_depth, write_depth))
        canvas3d_occlusion_grid_write(grid, min_x, min_y, max_x, max_y, write_depth);
    return 0;
}
