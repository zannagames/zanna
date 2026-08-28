//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_render_pass.inc
// Purpose: Canvas3D shadow/main render passes, deferred submission, and frame end.
// Key invariants:
//   - Included only by rt_canvas3d.c; this is not a standalone translation unit.
//   - Shadow reuse signatures include every stable input that shapes depth.
//   - Main-pass culling never rejects draws lacking trustworthy bounds.
// Ownership/Lifetime:
//   - Pass helpers borrow frame-owned deferred commands and backend resources.
//   - Canvas3D owns reusable shadow-index and auto-instancing scratch storage.
// Links: rt_canvas3d.c, rt_canvas3d_internal.h, rt_canvas3d_occlusion.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Canvas3D shadow rendering, main deferred replay, and frame termination.
/// @details The fragment selects and caches shadow casters, batches compatible
///   opaque draws, applies conservative frustum/occlusion decisions, submits
///   transparent and overlay work in order, and records per-pass diagnostics.

/// @brief Recompute the canvas's latest backend texture-upload byte counter.
/// @param c Canvas3D whose latest-frame diagnostic is replaced; `NULL` is ignored.
static void canvas3d_refresh_texture_upload_bytes(rt_canvas3d *c) {
    uint64_t bytes = 0;

    if (c && c->backend && c->backend->get_texture_upload_bytes)
        bytes = c->backend->get_texture_upload_bytes(c->backend_ctx);
    if (c)
        c->last_texture_upload_bytes = bytes > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)bytes;
}

/// @brief Recompute the canvas's latest backend GPU frame-time counter.
/// @param c Canvas3D whose latest-frame diagnostic is replaced; `NULL` is ignored.
static void canvas3d_refresh_frame_gpu_time(rt_canvas3d *c) {
    uint64_t us = 0;

    if (c && c->backend && c->backend->get_frame_gpu_time_us)
        us = c->backend->get_frame_gpu_time_us(c->backend_ctx);
    if (c)
        c->last_frame_gpu_time_us = us > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)us;
}

/// @brief Ensure the reusable shadow-caster index scratch can hold @p needed entries.
/// @details The shadow pass may render multiple slots/cascades per frame. A compact index list
///          lets each slot iterate only candidate shadow casters instead of rescanning the full
///          deferred queue and rechecking overlay/material state for every light.
/// @param c Canvas that owns the scratch array.
/// @param needed Minimum number of int32 indices required this frame.
/// @return Non-zero when the scratch allocation is available.
static int canvas3d_ensure_shadow_draw_index_capacity(rt_canvas3d *c, int32_t needed) {
    int32_t new_capacity;
    int32_t *grown;

    if (!c || needed < 0)
        return 0;
    if (needed == 0)
        return 1;
    if (c->shadow_draw_index_capacity >= needed)
        return 1;
    new_capacity = c->shadow_draw_index_capacity > 0 ? c->shadow_draw_index_capacity : 64;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(*grown))
        return 0;
    grown = (int32_t *)realloc(c->shadow_draw_indices, (size_t)new_capacity * sizeof(*grown));
    if (!grown)
        return 0;
    c->shadow_draw_indices = grown;
    c->shadow_draw_index_capacity = new_capacity;
    return 1;
}

/// @brief Build the current frame's compact list of shadow-casting draw indices.
/// @details This performs the material/pass shadow eligibility check exactly once per queued draw.
///          Later light/cascade/face passes reuse the list and only apply light-volume culling.
/// @param c Canvas that owns the reusable scratch list.
/// @param cmds Deferred draw queue.
/// @param draw_count Number of queued commands.
/// @return Number of shadow-casting draw indices written, or zero if none/alloc failure.
static int32_t canvas3d_prepare_shadow_draw_indices(rt_canvas3d *c,
                                                    const deferred_draw_t *cmds,
                                                    int32_t draw_count) {
    int32_t *indices;
    int32_t count = 0;

    if (!c || !cmds || draw_count <= 0)
        return 0;
    if (!canvas3d_ensure_shadow_draw_index_capacity(c, draw_count))
        return 0;
    indices = (int32_t *)c->shadow_draw_indices;
    for (int32_t i = 0; i < draw_count; ++i) {
        if (canvas3d_deferred_casts_shadow(&cmds[i]))
            indices[count++] = i;
    }
    return count;
}

/// @brief Ensure the auto-instancing matrix scratch holds @p needed instance matrices.
/// @details Grows the canvas-owned contiguous matrix array geometrically and preserves
///   its previous allocation if growth fails. Existing matrix contents are scratch
///   data and have no lifetime guarantee across a successful call.
/// @param c Canvas that owns the reusable matrix scratch allocation.
/// @param needed Positive number of 4-by-4 matrices required by the pending batch.
/// @return Nonzero when capacity is available; zero for invalid input, overflow, or
///   allocation failure.
static int canvas3d_reserve_auto_instance_matrices(rt_canvas3d *c, int32_t needed) {
    int32_t new_capacity;
    float *grown;
    if (!c || needed <= 0)
        return 0;
    if (c->auto_instance_matrix_capacity >= needed)
        return 1;
    new_capacity = c->auto_instance_matrix_capacity > 0 ? c->auto_instance_matrix_capacity : 64;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / (sizeof(float) * 16u))
        return 0;
    grown = (float *)realloc(c->auto_instance_matrices, (size_t)new_capacity * sizeof(float) * 16u);
    if (!grown)
        return 0;
    c->auto_instance_matrices = grown;
    c->auto_instance_matrix_capacity = new_capacity;
    return 1;
}

/// @brief Whether two state-sorted opaque draws can share one instanced submission.
/// @details Requires the same stable geometry (key + revision), no skinning/morph
///   payloads, matching light slice and raster flags, and full command equality
///   outside the model transform pair. Only draws whose previous transform equals
///   the current one qualify (static this frame): the instanced path carries one
///   cmd-level previous matrix, so batching a mover would corrupt its motion vector.
/// @param a First borrowed deferred mesh draw in the state-sorted opaque sequence.
/// @param b Second borrowed deferred mesh draw to compare with @p a.
/// @return Nonzero when both draws may be represented by one instanced backend
///   submission; zero for null, non-mesh, mutable, or state-incompatible draws.
static int canvas3d_draws_auto_instance_compatible(const deferred_draw_t *a,
                                                   const deferred_draw_t *b) {
    const vgfx3d_draw_cmd_t *ca;
    const vgfx3d_draw_cmd_t *cb;
    if (!a || !b || a->kind != DEFERRED_DRAW_MESH || b->kind != DEFERRED_DRAW_MESH)
        return 0;
    ca = &a->cmd;
    cb = &b->cmd;
    if (!ca->geometry_key || ca->geometry_key != cb->geometry_key ||
        ca->geometry_revision != cb->geometry_revision)
        return 0;
    if (ca->bone_palette || cb->bone_palette || ca->morph_weights || cb->morph_weights)
        return 0;
    if (memcmp(ca->model_matrix, ca->prev_model_matrix, sizeof(ca->model_matrix)) != 0 ||
        memcmp(cb->model_matrix, cb->prev_model_matrix, sizeof(cb->model_matrix)) != 0)
        return 0;
    if (a->lights != b->lights || a->light_count != b->light_count ||
        a->wireframe != b->wireframe || a->backface_cull != b->backface_cull)
        return 0;
    if (memcmp(a->ambient, b->ambient, sizeof(a->ambient)) != 0)
        return 0;
    {
        /* Command payloads are memset-zeroed at fill time, so padding bytes are
         * deterministic and whole-struct comparison is exact. Normalize the two
         * transform fields (the only allowed difference) before comparing. */
        vgfx3d_draw_cmd_t na = *ca;
        vgfx3d_draw_cmd_t nb = *cb;
        memset(na.model_matrix, 0, sizeof(na.model_matrix));
        memset(na.prev_model_matrix, 0, sizeof(na.prev_model_matrix));
        memset(nb.model_matrix, 0, sizeof(nb.model_matrix));
        memset(nb.prev_model_matrix, 0, sizeof(nb.prev_model_matrix));
        return memcmp(&na, &nb, sizeof(na)) == 0;
    }
}

/// @brief Fold one shadow caster's identity into a slot signature.
/// @details Hashes the caster's stable geometry identity (key + revision + counts),
///   its model matrix, and the material bits that affect depth output (alpha
///   masking, double-sidedness). Returns 0 (poison) for content whose depth can
///   change without any hashed field moving: skinned or morphed draws (palette /
///   weight buffers mutate in place), instanced draws (matrix payloads are
///   transient), and transient geometry with no stable key (per-frame temp
///   buffers can be reallocated at the same address with new contents).
/// @param sig Running nonzero signature for the shadow slot; zero remains poisoned.
/// @param dd Borrowed candidate caster whose depth-affecting state is inspected.
/// @return Updated nonzero signature for cacheable stable content, or zero when
///   the input is invalid or cannot be represented by a reliable reuse signature.
static uint64_t canvas3d_shadow_hash_caster(uint64_t sig, const deferred_draw_t *dd) {
    const vgfx3d_draw_cmd_t *cmd;
    if (sig == 0 || !dd)
        return 0;
    cmd = &dd->cmd;
    if (dd->kind != DEFERRED_DRAW_MESH)
        return 0;
    if (cmd->bone_palette || cmd->morph_weights || !cmd->geometry_key)
        return 0;
    sig = canvas3d_mix_sort_ptr(sig, cmd->geometry_key);
    sig = canvas3d_mix_sort_u64(sig, (uint64_t)cmd->geometry_revision);
    sig = canvas3d_mix_sort_u64(sig, (uint64_t)cmd->vertex_count);
    sig = canvas3d_mix_sort_u64(sig, (uint64_t)cmd->index_count);
    for (int i = 0; i < 16; i++) {
        uint32_t bits = 0;
        memcpy(&bits, &cmd->model_matrix[i], sizeof(bits));
        sig = canvas3d_mix_sort_u64(sig, (uint64_t)bits);
    }
    {
        uint32_t cutoff_bits = 0;
        memcpy(&cutoff_bits, &cmd->alpha_cutoff, sizeof(cutoff_bits));
        sig = canvas3d_mix_sort_u64(sig, (uint64_t)(uint32_t)cmd->alpha_mode);
        sig = canvas3d_mix_sort_u64(sig, (uint64_t)cutoff_bits);
        sig = canvas3d_mix_sort_u64(sig, (uint64_t)(uint32_t)cmd->double_sided);
        sig = canvas3d_mix_sort_ptr(sig, cmd->texture);
        sig = canvas3d_mix_sort_ptr(sig, cmd->texture_asset);
    }
    return sig ? sig : 1;
}

/// @brief Seed a shadow-slot signature from everything that shapes the depth pass
///   besides the casters: the light view-projection, target identity/resolution,
///   and the bias applied at shadow_end.
/// @param c Canvas supplying the current shadow-bias configuration.
/// @param shadow_rt Borrowed render target whose identity and depth storage
///   participate in the cache key.
/// @param shadow_resolution Effective pixel resolution of the target slot.
/// @param light_vp Sixteen-element column-major light view-projection matrix.
/// @return Nonzero signature seed, or zero when a required pointer is null or the
///   resolution is not positive.
static uint64_t canvas3d_shadow_slot_signature_seed(const rt_canvas3d *c,
                                                    const vgfx3d_rendertarget_t *shadow_rt,
                                                    int32_t shadow_resolution,
                                                    const float light_vp[16]) {
    uint64_t sig = UINT64_C(0x9E3779B97F4A7C15);
    if (!c || !shadow_rt || !light_vp)
        return 0;
    sig = canvas3d_mix_sort_ptr(sig, shadow_rt);
    sig = canvas3d_mix_sort_ptr(sig, shadow_rt->depth_buf);
    sig = canvas3d_mix_sort_u64(sig, (uint64_t)(uint32_t)shadow_resolution);
    sig = canvas3d_mix_sort_u64(sig, (uint64_t)(uint32_t)shadow_rt->width);
    sig = canvas3d_mix_sort_u64(sig, (uint64_t)(uint32_t)shadow_rt->height);
    {
        uint32_t bias_bits = 0;
        memcpy(&bias_bits, &c->shadow_bias, sizeof(bias_bits));
        sig = canvas3d_mix_sort_u64(sig, (uint64_t)bias_bits);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t bits = 0;
        memcpy(&bits, &light_vp[i], sizeof(bits));
        sig = canvas3d_mix_sort_u64(sig, (uint64_t)bits);
    }
    return sig ? sig : 1;
}

/// @brief Try to satisfy shadow slot @p slot from its previous-frame contents.
/// @details Compares the computed signature against the slot's stored one and, on a
///   match, asks the backend to re-arm the persisted depth (shadow_reuse hook).
///   On success Canvas3D skips the whole begin/draw/end pass for the slot.
/// @param c Canvas owning the slot cache and backend.
/// @param slot Zero-based shadow slot to test.
/// @param signature Complete nonzero signature for the requested slot contents.
/// @param shadow_rt Borrowed target whose persisted depth buffer would be reused.
/// @param light_vp Sixteen-element light view-projection matrix passed to the backend.
/// @return 1 when the slot was reused; 0 to render normally.
static int canvas3d_shadow_slot_try_reuse(rt_canvas3d *c,
                                          int32_t slot,
                                          uint64_t signature,
                                          vgfx3d_rendertarget_t *shadow_rt,
                                          const float light_vp[16]) {
    if (!c || !shadow_rt || signature == 0)
        return 0;
    if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS)
        return 0;
    if (c->shadow_slot_signature[slot] != signature)
        return 0;
    if (!c->backend || !c->backend->shadow_reuse)
        return 0;
    if (!c->backend->shadow_reuse(c->backend_ctx,
                                  slot,
                                  shadow_rt->depth_buf,
                                  shadow_rt->width,
                                  shadow_rt->height,
                                  light_vp))
        return 0;
    c->last_shadow_slots_cached++;
    rt_game3d_diag_record_shadow_slot_reused();
    return 1;
}

/// @brief Render one non-cascaded shadow light into the next available shadow slot.
/// @details Used both by the simple shadow path and by the cascaded path after the primary
///   directional light has consumed its cascade slots. The helper owns slot validation, light VP
///   construction, backend begin/draw/end, and writing the packed `shadow_lights` entry consumed by
///   main-pass light parameter patching.
/// @param c Canvas whose next shadow slot and backend are used.
/// @param cmds Mutable deferred draw array indexed by @p shadow_indices.
/// @param shadow_indices Borrowed array of candidate caster indices.
/// @param shadow_index_count Number of valid entries in @p shadow_indices.
/// @param shadow_world_min Three-element minimum corner of the aggregate caster bounds.
/// @param shadow_world_max Three-element maximum corner of the aggregate caster bounds.
/// @param selected_light Borrowed light parameters to render; copied before modification.
/// @param shadow_lights Output array of `VGFX3D_MAX_SHADOW_LIGHTS` packed light
///   entries; the claimed slot is written on success.
/// @return 1 when a shadow slot was rendered and `c->shadow_count` advanced, 0 otherwise.
static int canvas3d_render_shadow_light_slot(
    rt_canvas3d *c,
    deferred_draw_t *cmds,
    const int32_t *shadow_indices,
    int32_t shadow_index_count,
    const float shadow_world_min[3],
    const float shadow_world_max[3],
    const vgfx3d_light_params_t *selected_light,
    vgfx3d_light_params_t shadow_lights[VGFX3D_MAX_SHADOW_LIGHTS]) {
    vgfx3d_rendertarget_t *shadow_rt;
    vgfx3d_light_params_t light;
    int32_t shadow_resolution;
    float light_vp[16];
    int32_t slot;

    if (!c || !cmds || !shadow_indices || shadow_index_count <= 0 || !shadow_world_min ||
        !shadow_world_max || !selected_light || !shadow_lights || c->shadow_count < 0 ||
        c->shadow_count >= VGFX3D_MAX_SHADOW_LIGHTS)
        return 0;
    slot = c->shadow_count;
    light = *selected_light;
    light.shadow_cascade_count = 1;
    light.shadow_projection_type = light.type == 3 || light.type == 4
                                       ? VGFX3D_SHADOW_PROJECTION_PERSPECTIVE
                                       : VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
    if (!canvas3d_ensure_shadow_target_slot(c, slot, c->shadow_resolution))
        return 0;
    shadow_rt = c->shadow_rts[slot];
    if (!shadow_rt || !shadow_rt->depth_buf)
        return 0;
    shadow_resolution = canvas3d_shadow_slot_resolution(c, slot);
    if (shadow_resolution <= 0)
        return 0;
    {
        /* Directional fit: clamp the caster content box to the shadow-distance window
         * around the camera so one far-away caster cannot stretch the orthographic fit
         * (and collapse near-shadow texel density). Skipped when the clamp would empty
         * the box; spot/point fits are local to the light and never clamped. */
        float fit_min[3];
        float fit_max[3];
        memcpy(fit_min, shadow_world_min, sizeof(fit_min));
        memcpy(fit_max, shadow_world_max, sizeof(fit_max));
        if (light.type == 0) {
            float reach = canvas3d_effective_shadow_distance(c);
            int clamped_valid = 1;
            for (int axis = 0; axis < 3; axis++) {
                float lo = c->cached_cam_pos[axis] - reach;
                float hi = c->cached_cam_pos[axis] + reach;
                if (!isfinite(lo) || !isfinite(hi)) {
                    clamped_valid = 0;
                    break;
                }
                if (fit_min[axis] < lo)
                    fit_min[axis] = lo;
                if (fit_max[axis] > hi)
                    fit_max[axis] = hi;
                if (fit_min[axis] > fit_max[axis]) {
                    clamped_valid = 0;
                    break;
                }
            }
            if (!clamped_valid) {
                memcpy(fit_min, shadow_world_min, sizeof(fit_min));
                memcpy(fit_max, shadow_world_max, sizeof(fit_max));
            }
        }
        if (!canvas3d_build_shadow_light_vp(fit_min, fit_max, &light, shadow_resolution, light_vp))
            return 0;
    }

    memcpy(c->shadow_light_vps[slot], light_vp, sizeof(light_vp));
    shadow_lights[slot] = light;
    shadow_lights[slot].shadow_index = slot;

    /* Spot lights illuminate a bounded sphere around their position, so casters
     * outside `position +/- range` cannot affect this map. Directional lights are
     * unbounded along the light ray (casters behind the fitted box still shadow
     * into it), so they submit the full caster list. */
    int cull_spot = light.type == 3;
    float spot_min[3] = {0.0f, 0.0f, 0.0f};
    float spot_max[3] = {0.0f, 0.0f, 0.0f};
    if (cull_spot) {
        float range =
            canvas3d_shadow_spot_range_from_bounds(&light, shadow_world_min, shadow_world_max);
        if (!isfinite(range) || range <= 0.0f) {
            cull_spot = 0;
        } else {
            for (int axis = 0; axis < 3; axis++) {
                spot_min[axis] = light.position[axis] - range;
                spot_max[axis] = light.position[axis] + range;
            }
        }
    }

    {
        uint64_t signature =
            canvas3d_shadow_slot_signature_seed(c, shadow_rt, shadow_resolution, light_vp);
        for (int32_t si = 0; signature != 0 && si < shadow_index_count; si++) {
            int32_t draw_index = shadow_indices[si];
            if (draw_index < 0)
                continue;
            if (cull_spot && !canvas3d_deferred_intersects_world_bounds(
                                 c, &cmds[draw_index], spot_min, spot_max))
                continue;
            signature = canvas3d_shadow_hash_caster(signature, &cmds[draw_index]);
        }
        if (canvas3d_shadow_slot_try_reuse(c, slot, signature, shadow_rt, light_vp)) {
            c->shadow_count++;
            return 1;
        }

        c->backend->shadow_begin(c->backend_ctx,
                                 slot,
                                 shadow_rt->depth_buf,
                                 shadow_rt->width,
                                 shadow_rt->height,
                                 light_vp);
        for (int32_t si = 0; si < shadow_index_count; si++) {
            int32_t draw_index = shadow_indices[si];
            if (draw_index < 0)
                continue;
            if (cull_spot && !canvas3d_deferred_intersects_world_bounds(
                                 c, &cmds[draw_index], spot_min, spot_max))
                continue;
            canvas3d_shadow_deferred(c, &cmds[draw_index]);
        }
        c->backend->shadow_end(c->backend_ctx, slot, c->shadow_bias);
        c->shadow_slot_signature[slot] = signature;
    }
    c->shadow_count++;
    return 1;
}

/// @brief Render an omnidirectional point-light shadow as six perspective faces.
/// @details Claims VGFX3D_SHADOW_CUBE_FACES consecutive slots (atlas tiles on GPU
///   backends). The light entry advertises the FIRST slot with the CUBE projection
///   type; shaders select the face by the dominant axis of light->fragment.
/// @param c Canvas whose consecutive shadow slots and backend are used.
/// @param cmds Mutable deferred draw array indexed by @p shadow_indices.
/// @param shadow_indices Borrowed array of candidate caster indices.
/// @param shadow_index_count Number of valid entries in @p shadow_indices.
/// @param shadow_world_min Three-element minimum corner of the aggregate caster bounds.
/// @param shadow_world_max Three-element maximum corner of the aggregate caster bounds.
/// @param selected_light Borrowed point-light parameters; copied before modification.
/// @param shadow_lights Output array of `VGFX3D_MAX_SHADOW_LIGHTS` packed light
///   entries; the base face slot is published on success.
/// @param slot_cap Exclusive upper bound on shadow slots available to this backend.
/// @return 1 when all six faces rendered and the light was granted, 0 otherwise.
static int canvas3d_render_point_shadow_cube(
    rt_canvas3d *c,
    deferred_draw_t *cmds,
    const int32_t *shadow_indices,
    int32_t shadow_index_count,
    const float shadow_world_min[3],
    const float shadow_world_max[3],
    const vgfx3d_light_params_t *selected_light,
    vgfx3d_light_params_t shadow_lights[VGFX3D_MAX_SHADOW_LIGHTS],
    int32_t slot_cap) {
    vgfx3d_light_params_t light;
    int32_t base;

    if (!c || !cmds || !shadow_indices || shadow_index_count <= 0 || !shadow_world_min ||
        !shadow_world_max || !selected_light || !shadow_lights || c->shadow_count < 0)
        return 0;
    base = c->shadow_count;
    if (base + VGFX3D_SHADOW_CUBE_FACES > slot_cap)
        return 0;
    light = *selected_light;
    light.shadow_cascade_count = 1;
    light.shadow_projection_type = VGFX3D_SHADOW_PROJECTION_CUBE;

    /* Each face is a ~90-degree frustum from the light position with far plane at
     * the light range, so it fits inside the half-cube along the face axis
     * (laterally widened for the 92-degree guard band). Casters outside that box
     * cannot write depth into the face. */
    float face_range =
        canvas3d_shadow_spot_range_from_bounds(&light, shadow_world_min, shadow_world_max);
    int cull_faces = isfinite(face_range) && face_range > 0.0f;
    float face_lateral = cull_faces ? face_range * 1.05f : 0.0f;

    static const float cube_face_dirs[VGFX3D_SHADOW_CUBE_FACES][3] = {
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
    };

    for (int32_t face = 0; face < VGFX3D_SHADOW_CUBE_FACES; face++) {
        vgfx3d_rendertarget_t *shadow_rt;
        float light_vp[16];
        int32_t slot = base + face;

        if (!canvas3d_ensure_shadow_target_slot(c, slot, c->shadow_resolution))
            return 0;
        shadow_rt = c->shadow_rts[slot];
        if (!shadow_rt || !shadow_rt->depth_buf)
            return 0;
        if (!canvas3d_build_point_shadow_face_vp(
                shadow_world_min, shadow_world_max, &light, face, light_vp))
            return 0;

        float face_min[3];
        float face_max[3];
        if (cull_faces) {
            for (int axis = 0; axis < 3; axis++) {
                face_min[axis] = light.position[axis] - face_lateral;
                face_max[axis] = light.position[axis] + face_lateral;
            }
            for (int axis = 0; axis < 3; axis++) {
                if (cube_face_dirs[face][axis] > 0.5f) {
                    face_min[axis] = light.position[axis] - 0.1f;
                    face_max[axis] = light.position[axis] + face_range;
                } else if (cube_face_dirs[face][axis] < -0.5f) {
                    face_min[axis] = light.position[axis] - face_range;
                    face_max[axis] = light.position[axis] + 0.1f;
                }
            }
        }

        memcpy(c->shadow_light_vps[slot], light_vp, sizeof(light_vp));
        {
            uint64_t signature = canvas3d_shadow_slot_signature_seed(
                c, shadow_rt, canvas3d_shadow_slot_resolution(c, slot), light_vp);
            for (int32_t si = 0; signature != 0 && si < shadow_index_count; si++) {
                int32_t draw_index = shadow_indices[si];
                if (draw_index < 0)
                    continue;
                if (cull_faces && !canvas3d_deferred_intersects_world_bounds(
                                      c, &cmds[draw_index], face_min, face_max))
                    continue;
                signature = canvas3d_shadow_hash_caster(signature, &cmds[draw_index]);
            }
            if (canvas3d_shadow_slot_try_reuse(c, slot, signature, shadow_rt, light_vp)) {
                c->shadow_count++;
                continue;
            }
            c->backend->shadow_begin(c->backend_ctx,
                                     slot,
                                     shadow_rt->depth_buf,
                                     shadow_rt->width,
                                     shadow_rt->height,
                                     light_vp);
            for (int32_t si = 0; si < shadow_index_count; si++) {
                int32_t draw_index = shadow_indices[si];
                if (draw_index < 0)
                    continue;
                if (cull_faces && !canvas3d_deferred_intersects_world_bounds(
                                      c, &cmds[draw_index], face_min, face_max))
                    continue;
                canvas3d_shadow_deferred(c, &cmds[draw_index]);
            }
            c->backend->shadow_end(c->backend_ctx, slot, c->shadow_bias);
            c->shadow_slot_signature[slot] = signature;
        }
        c->shadow_count++;
    }
    shadow_lights[base] = light;
    shadow_lights[base].shadow_index = base;
    return 1;
}

/// @brief Render the directional, spot, and cascaded shadow-map pass for the queued
///   3D draws.
/// @details Populates the canvas-owned shadow targets, selects lights within the
///   configured budget, reuses cacheable slots, and stamps the resulting shadow
///   parameters onto main-pass commands. Unsupported or unavailable shadow work
///   degrades to zero active shadow slots rather than consuming draw commands.
/// @param c Canvas with an active backend and the frame's shadow configuration.
/// @param cmds Mutable array of at least `c->draw_count` deferred commands; light
///   parameters and shadow-slot metadata may be patched in place.
static void canvas3d_render_shadow_pass(rt_canvas3d *c, deferred_draw_t *cmds) {
    vgfx3d_light_params_t shadow_lights[VGFX3D_MAX_SHADOW_LIGHTS];
    vgfx3d_light_params_t selected_lights[VGFX3D_MAX_SHADOW_LIGHTS];

    memset(shadow_lights, 0, sizeof(shadow_lights));
    memset(selected_lights, 0, sizeof(selected_lights));

    int32_t shadow_request_count = 0;
    int32_t shadow_lights_granted = 0;
    const int32_t *shadow_indices = NULL;
    int32_t shadow_index_count = 0;
    c->last_shadow_slots_used = 0;
    c->last_shadow_requests_dropped = 0;
    c->last_shadow_slots_cached = 0;
    if (c->shadows_enabled && c->backend->shadow_begin && c->backend->shadow_draw &&
        c->backend->shadow_end && canvas3d_ensure_shadow_targets(c, c->shadow_resolution)) {
        /* Backends without atlas-slot sampling cap the frame at the classic
         * per-texture slots so no light gets an unresolvable shadow index. */
        int32_t slot_cap =
            c->backend->shadow_atlas_slots ? VGFX3D_MAX_SHADOW_LIGHTS : VGFX3D_CSM_SLOTS;
        int32_t shadow_budget = c->shadow_budget;
        if (shadow_budget < 1 || shadow_budget > slot_cap)
            shadow_budget = slot_cap;
        shadow_index_count = canvas3d_prepare_shadow_draw_indices(c, cmds, c->draw_count);
        shadow_indices = (const int32_t *)c->shadow_draw_indices;
        if (shadow_index_count <= 0 || !shadow_indices)
            return;
        int32_t selected_shadow_count =
            canvas3d_select_shadow_lights_from_draws(c,
                                                     cmds,
                                                     shadow_indices,
                                                     shadow_index_count,
                                                     selected_lights,
                                                     shadow_budget,
                                                     &shadow_request_count);
        float shadow_world_min[3];
        float shadow_world_max[3];
        int has_shadow_bounds = canvas3d_build_shadow_world_bounds(
            c, cmds, shadow_indices, shadow_index_count, shadow_world_min, shadow_world_max);

        if (has_shadow_bounds && selected_shadow_count > 0 && selected_lights[0].type == 0 &&
            c->shadow_cascade_count > 1) {
            vgfx3d_light_params_t primary_light = selected_lights[0];
            int32_t cascade_count = c->shadow_cascade_count;
            float cascade_splits[VGFX3D_CSM_SLOTS];
            float cascade_near = 0.0f;
            float cascade_far = 0.0f;

            if (cascade_count > VGFX3D_CSM_SLOTS)
                cascade_count = VGFX3D_CSM_SLOTS;
            if (!canvas3d_compute_shadow_cascade_splits(c,
                                                        shadow_world_min,
                                                        shadow_world_max,
                                                        cascade_count,
                                                        cascade_splits,
                                                        &cascade_near,
                                                        &cascade_far))
                cascade_count = 0;
            primary_light.shadow_index = 0;
            primary_light.shadow_cascade_count = cascade_count;
            primary_light.shadow_projection_type = VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
            memcpy(primary_light.shadow_cascade_splits,
                   cascade_splits,
                   sizeof(primary_light.shadow_cascade_splits));
            for (int32_t cascade = 0; cascade < cascade_count; cascade++) {
                vgfx3d_rendertarget_t *shadow_rt;
                int32_t shadow_resolution;
                float cascade_min[3];
                float cascade_max[3];
                float cascade_depth_near =
                    cascade == 0 ? cascade_near : cascade_splits[cascade - 1];
                float cascade_depth_far = cascade_splits[cascade];
                float light_vp[16];

                if (!canvas3d_ensure_shadow_target_slot(c, c->shadow_count, c->shadow_resolution))
                    continue;
                shadow_rt = c->shadow_rts[c->shadow_count];
                if (!shadow_rt || !shadow_rt->depth_buf)
                    continue;
                shadow_resolution = canvas3d_shadow_slot_resolution(c, c->shadow_count);
                if (shadow_resolution <= 0)
                    continue;
                if (!canvas3d_build_shadow_cascade_world_bounds(c,
                                                                cmds,
                                                                shadow_index_count,
                                                                cascade_depth_near,
                                                                cascade_depth_far,
                                                                shadow_world_min,
                                                                shadow_world_max,
                                                                cascade_min,
                                                                cascade_max))
                    continue;
                if (!canvas3d_build_shadow_light_vp(
                        cascade_min, cascade_max, &primary_light, shadow_resolution, light_vp))
                    continue;

                memcpy(c->shadow_light_vps[c->shadow_count], light_vp, sizeof(light_vp));
                shadow_lights[c->shadow_count] = primary_light;
                shadow_lights[c->shadow_count].shadow_index = c->shadow_count;
                shadow_lights[c->shadow_count].shadow_cascade_count = cascade_count;
                shadow_lights[c->shadow_count].shadow_projection_type =
                    VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
                memcpy(shadow_lights[c->shadow_count].shadow_cascade_splits,
                       cascade_splits,
                       sizeof(shadow_lights[c->shadow_count].shadow_cascade_splits));
                {
                    uint64_t signature = canvas3d_shadow_slot_signature_seed(
                        c, shadow_rt, shadow_resolution, light_vp);
                    for (int32_t si = 0; signature != 0 && si < shadow_index_count; si++) {
                        int32_t draw_index = shadow_indices[si];
                        if (draw_index < 0)
                            continue;
                        if (!canvas3d_deferred_intersects_view_depth(
                                c, &cmds[draw_index], cascade_depth_near, cascade_depth_far))
                            continue;
                        if (!canvas3d_deferred_intersects_world_bounds(
                                c, &cmds[draw_index], cascade_min, cascade_max))
                            continue;
                        signature = canvas3d_shadow_hash_caster(signature, &cmds[draw_index]);
                    }
                    if (canvas3d_shadow_slot_try_reuse(
                            c, c->shadow_count, signature, shadow_rt, light_vp)) {
                        c->shadow_count++;
                        continue;
                    }
                    c->backend->shadow_begin(c->backend_ctx,
                                             c->shadow_count,
                                             shadow_rt->depth_buf,
                                             shadow_rt->width,
                                             shadow_rt->height,
                                             light_vp);
                    for (int32_t si = 0; si < shadow_index_count; si++) {
                        int32_t draw_index = shadow_indices[si];
                        if (draw_index < 0)
                            continue;
                        if (!canvas3d_deferred_intersects_view_depth(
                                c, &cmds[draw_index], cascade_depth_near, cascade_depth_far))
                            continue;
                        if (!canvas3d_deferred_intersects_world_bounds(
                                c, &cmds[draw_index], cascade_min, cascade_max))
                            continue;
                        canvas3d_shadow_deferred(c, &cmds[draw_index]);
                    }
                    c->backend->shadow_end(c->backend_ctx, c->shadow_count, c->shadow_bias);
                    c->shadow_slot_signature[c->shadow_count] = signature;
                }
                c->shadow_count++;
            }
            if (c->shadow_count > 0) {
                shadow_lights[0] = primary_light;
                shadow_lights[0].shadow_index = 0;
                shadow_lights_granted++;
            }
            for (int32_t slot = 1;
                 has_shadow_bounds && slot < selected_shadow_count && c->shadow_count < slot_cap;
                 slot++) {
                shadow_lights_granted +=
                    selected_lights[slot].type == 1 || selected_lights[slot].type == 5
                        ? canvas3d_render_point_shadow_cube(c,
                                                            cmds,
                                                            shadow_indices,
                                                            shadow_index_count,
                                                            shadow_world_min,
                                                            shadow_world_max,
                                                            &selected_lights[slot],
                                                            shadow_lights,
                                                            slot_cap)
                        : canvas3d_render_shadow_light_slot(c,
                                                            cmds,
                                                            shadow_indices,
                                                            shadow_index_count,
                                                            shadow_world_min,
                                                            shadow_world_max,
                                                            &selected_lights[slot],
                                                            shadow_lights);
            }
        } else {
            for (int32_t slot = 0;
                 has_shadow_bounds && slot < selected_shadow_count && c->shadow_count < slot_cap;
                 slot++)
                shadow_lights_granted +=
                    selected_lights[slot].type == 1 || selected_lights[slot].type == 5
                        ? canvas3d_render_point_shadow_cube(c,
                                                            cmds,
                                                            shadow_indices,
                                                            shadow_index_count,
                                                            shadow_world_min,
                                                            shadow_world_max,
                                                            &selected_lights[slot],
                                                            shadow_lights,
                                                            slot_cap)
                        : canvas3d_render_shadow_light_slot(c,
                                                            cmds,
                                                            shadow_indices,
                                                            shadow_index_count,
                                                            shadow_world_min,
                                                            shadow_world_max,
                                                            &selected_lights[slot],
                                                            shadow_lights);
        }
        c->last_shadow_slots_used = c->shadow_count;
        c->last_shadow_requests_dropped = shadow_request_count > shadow_lights_granted
                                              ? shadow_request_count - shadow_lights_granted
                                              : 0;
    }

    for (int32_t i = 0; i < c->draw_count; i++)
        canvas3d_apply_shadow_light_params(
            cmds[i].lights, cmds[i].light_count, shadow_lights, c->shadow_count);

    /* Shadow patching mutated every queued light snapshot, so the pre-patch
     * revision stamps are stale. Remap each run of identical old stamps to a
     * fresh stamp (preserving distinctness so mid-frame light changes never
     * alias) and invalidate the snapshot cache for the next frame. */
    if (c->shadow_count > 0 && c->draw_count > 0) {
        uint32_t remap_from = 0;
        uint32_t remap_to = 0;
        c->last_light_snapshot_valid = 0;
        /* Shadow patching mutated the shared flattened slice and the remap below issues
         * fresh stamps; draws queued after this point (overlay replay) must re-flatten
         * rather than reuse the pre-patch cached stamp. */
        canvas3d_invalidate_light_flatten_cache(c);
        for (int32_t i = 0; i < c->draw_count; i++) {
            if (cmds[i].light_count <= 0 || cmds[i].cmd.lights_revision == 0)
                continue;
            if (remap_to != 0 && cmds[i].cmd.lights_revision == remap_from) {
                cmds[i].cmd.lights_revision = remap_to;
                continue;
            }
            remap_from = cmds[i].cmd.lights_revision;
            c->lights_revision++;
            if (c->lights_revision == 0)
                c->lights_revision = 1;
            remap_to = c->lights_revision;
            cmds[i].cmd.lights_revision = remap_to;
        }
    }
}

/// @brief Draw the canvas skybox through the GPU backend, at most once per End().
/// @details Every GPU backend renders the skybox as a full-screen triangle at far depth
///   with a read-only LessEqual depth state, so after the opaque pass it shades only
///   pixels no opaque draw covered. The software path has no draw_skybox hook and keeps
///   its CPU pre-pass composite in rt_canvas3d_end.
/// @param c Canvas whose skybox state is inspected and, when eligible, marked drawn.
static void canvas3d_draw_backend_skybox_after_opaque(rt_canvas3d *c) {
    if (!c || c->skybox_pass_done || c->frame_is_2d || c->frame_is_view_model || !c->skybox ||
        !c->backend || !c->backend->draw_skybox)
        return;
    c->skybox_pass_done = 1;
    c->backend->draw_skybox(c->backend_ctx, c->skybox);
}

/// @brief Render the main 3D pass with visibility filtering and ordered submission.
/// @details Applies configured frustum and hierarchical or CPU occlusion tests,
///   submits opaque draws in depth/state order with opportunistic auto-instancing,
///   draws the backend skybox, resolves opaque targets when needed, and finally
///   replays transparent draws back-to-front. Allocation failures report a runtime
///   trap and stop the remaining main-pass submission.
/// @param c Canvas supplying cached camera state, backend hooks, and scratch storage.
/// @param cmds Mutable array of at least `c->draw_count` deferred commands; visibility
///   flags are updated and temporary sorted copies may be produced.
static void canvas3d_render_main_pass(rt_canvas3d *c, deferred_draw_t *cmds) {
    vgfx3d_frustum_t visibility_frustum;
    canvas3d_occlusion_grid_t occlusion_grid;
    int use_occlusion_grid = c->occlusion_culling ? 1 : 0;
    int sort_opaque = use_occlusion_grid || c->opaque_depth_sorting;
    int state_sort_opaque = sort_opaque && !canvas3d_backend_uses_software_raster(c);

    if (use_occlusion_grid) {
        canvas3d_occlusion_history_prune(c);
        canvas3d_occlusion_grid_clear(&occlusion_grid);
        canvas3d_hiz_frame_reset(c);
    }

    if (c->frustum_culling || c->occlusion_culling) {
        vgfx3d_frustum_extract(&visibility_frustum, c->cached_vp);
        for (int32_t i = 0; i < c->draw_count; i++) {
            if (cmds[i].pass_kind == DEFERRED_PASS_MAIN) {
                cmds[i].visible =
                    (int8_t)canvas3d_deferred_intersects_frustum(c, &cmds[i], &visibility_frustum);
                if (!cmds[i].visible) {
                    c->last_occluded_draw_count++;
                    c->last_frustum_culled_draw_count++;
                }
            }
        }
    } else {
        for (int32_t i = 0; i < c->draw_count; i++) {
            if (cmds[i].pass_kind == DEFERRED_PASS_MAIN)
                cmds[i].visible = 1;
        }
    }

    {
        int32_t opaque_count = 0;
        int32_t trans_count = 0;
        int32_t staged_total;
        for (int32_t i = 0; i < c->draw_count; i++) {
            if (cmds[i].pass_kind != DEFERRED_PASS_MAIN || !cmds[i].visible)
                continue;
            if (cmds[i].requires_blend)
                trans_count++;
            else
                opaque_count++;
        }
        staged_total = opaque_count + trans_count;
        if (!sort_opaque) {
            for (int32_t i = 0; i < c->draw_count; i++) {
                if (cmds[i].pass_kind == DEFERRED_PASS_MAIN && cmds[i].visible &&
                    !cmds[i].requires_blend)
                    canvas3d_submit_deferred(c, &cmds[i]);
            }
            canvas3d_draw_backend_skybox_after_opaque(c);
            if (trans_count > 0 && c->backend->resolve_opaque_targets)
                c->backend->resolve_opaque_targets(c->backend_ctx);
            if (trans_count == 1) {
                for (int32_t i = 0; i < c->draw_count; i++) {
                    if (cmds[i].pass_kind == DEFERRED_PASS_MAIN && cmds[i].visible &&
                        cmds[i].requires_blend) {
                        canvas3d_submit_deferred(c, &cmds[i]);
                        break;
                    }
                }
                return;
            }
            if (trans_count > 1 &&
                ensure_deferred_capacity(&c->trans_cmds, &c->trans_capacity, trans_count)) {
                deferred_draw_t *trans = (deferred_draw_t *)c->trans_cmds;
                int32_t ti = 0;
                for (int32_t i = 0; i < c->draw_count; i++) {
                    if (cmds[i].pass_kind == DEFERRED_PASS_MAIN && cmds[i].visible &&
                        cmds[i].requires_blend)
                        trans[ti++] = cmds[i];
                }
                if (!canvas3d_sort_transparent_draws(c, trans, trans_count)) {
                    rt_trap("Canvas3D.End: deferred transparent sort scratch allocation failed");
                    return;
                }
                for (int32_t i = 0; i < trans_count; i++)
                    canvas3d_submit_deferred(c, &trans[i]);
            } else if (trans_count > 1) {
                rt_trap("Canvas3D.End: deferred transparent sort buffer allocation failed");
            }
            return;
        }
        if (staged_total > 0 &&
            ensure_deferred_capacity(&c->trans_cmds, &c->trans_capacity, staged_total)) {
            deferred_draw_t *opaque = (deferred_draw_t *)c->trans_cmds;
            deferred_draw_t *trans = opaque + opaque_count;
            int32_t oi = 0;
            int32_t ti = 0;
            for (int32_t i = 0; i < c->draw_count; i++) {
                if (cmds[i].pass_kind != DEFERRED_PASS_MAIN || !cmds[i].visible)
                    continue;
                if (cmds[i].requires_blend)
                    trans[ti++] = cmds[i];
                else
                    opaque[oi++] = cmds[i];
            }
            /* Depth-first sorting is required for the progressive occlusion grid and
             * remains the final software ordering. A native backend without CPU
             * occlusion immediately replaces it with state grouping, so skip that
             * otherwise-dead O(N log N) pass. */
            int32_t opaque_kept = opaque_count;
            if ((use_occlusion_grid || !state_sort_opaque) && opaque_count > 1 &&
                !canvas3d_sort_opaque_draws_depth_first(c, opaque, opaque_count)) {
                rt_trap("Canvas3D.End: deferred opaque sort scratch allocation failed");
                return;
            }
            if (use_occlusion_grid) {
                int32_t kept = 0;
                for (int32_t i = 0; i < opaque_count; i++) {
                    if (canvas3d_occlusion_test_and_write(c, &occlusion_grid, &opaque[i])) {
                        c->last_occluded_draw_count++;
                        c->last_cpu_occluded_draw_count++;
                        continue;
                    }
                    if (kept != i)
                        opaque[kept] = opaque[i];
                    kept++;
                }
                opaque_kept = kept;
            }
            if (state_sort_opaque && opaque_kept > 1 &&
                !canvas3d_stable_radix_sort_u64(
                    c, opaque, opaque_kept, canvas3d_sort_key_backend_state, NULL)) {
                rt_trap("Canvas3D.End: deferred opaque sort scratch allocation failed");
                return;
            }
            /* Auto-instancing: after the state sort, runs of identical commands
             * (same stable mesh + material + lights, static transforms) collapse
             * into one instanced submission — props, rocks, and foliage clusters
             * stop paying one backend draw each without user-side batching. */
            for (int32_t i = 0; i < opaque_kept;) {
                int32_t run = 1;
                if (state_sort_opaque && c->backend->submit_draw_instanced) {
                    while (i + run < opaque_kept &&
                           canvas3d_draws_auto_instance_compatible(&opaque[i], &opaque[i + run]))
                        run++;
                }
                if (run >= 2 && canvas3d_reserve_auto_instance_matrices(c, run)) {
                    deferred_draw_t inst = opaque[i];
                    for (int32_t k = 0; k < run; k++)
                        memcpy(c->auto_instance_matrices + (size_t)k * 16u,
                               opaque[i + k].cmd.model_matrix,
                               sizeof(float) * 16u);
                    inst.kind = DEFERRED_DRAW_INSTANCED;
                    inst.instance_matrices = c->auto_instance_matrices;
                    inst.instance_count = run;
                    canvas3d_submit_deferred(c, &inst);
                    c->last_auto_instanced_draws += run;
                    rt_game3d_diag_record_auto_instanced_draws(run);
                } else {
                    for (int32_t k = 0; k < run; k++)
                        canvas3d_submit_deferred(c, &opaque[i + k]);
                }
                i += run;
            }
            canvas3d_draw_backend_skybox_after_opaque(c);
            /* Plan 10: snapshot opaque depth for soft particles/SSR before any
             * blend draw samples it. Skipped when nothing transparent follows. */
            if (trans_count > 0 && c->backend->resolve_opaque_targets)
                c->backend->resolve_opaque_targets(c->backend_ctx);
            if (trans_count > 1 && !canvas3d_sort_transparent_draws(c, trans, trans_count)) {
                rt_trap("Canvas3D.End: deferred transparent sort scratch allocation failed");
                return;
            }
            for (int32_t i = 0; i < trans_count; i++)
                canvas3d_submit_deferred(c, &trans[i]);
        } else if (staged_total > 0) {
            rt_trap("Canvas3D.End: deferred sort buffer allocation failed");
            return;
        }
    }
}

/// @brief End drawing for the current scene or 2D pass and flush deferred draws.
/// @details Processes the deferred queue built during Begin/DrawMesh calls
///          in this strict pass order:
///          1. Skybox (if attached) into the scene color buffer.
///          2. Shadow pass: build a tight ortho light-VP, depth-render
///             every opaque main draw into the shadow map.
///          3. Opaque main draws — front-to-back on software when enabled
///             (maximizes early-Z rejection), state-grouped on GPU backends.
///          4. Translucent main draws — sorted back-to-front so alpha
///             blends compose correctly.
///          5. Pre/post-3D HUD overlay draws via `begin_overlay_frame`.
///          Must be called after all DrawMesh calls and before finalization or Flip.
///          Side effects: increments `frame_serial`, clears `draw_count`,
///          drains temp-buffer + temp-object queues, resets `in_frame`.
///          Post-FX, final overlay replay, screenshots, and presentation happen
///          in `FinalizeFrame`, `ScreenshotFinal`, and `Flip`, not here.
/// @param obj Canvas3D receiver or supported stack-wrapper handle. Invalid or
///   inactive receivers are ignored; a backend-less active frame is safely reset.
void rt_canvas3d_end(void *obj) {
    deferred_draw_t *cmds;
    int32_t queued_draw_count;
    int32_t main_count = 0;
    int32_t overlay_count = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!c->in_frame)
        return;
    if (!c->backend) {
        c->in_frame = 0;
        c->frame_is_2d = 0;
        c->frame_is_view_model = 0;
        c->last_draw_count = 0;
        c->last_texture_upload_bytes = 0;
        c->last_frame_gpu_time_us = 0;
        c->draw_count = 0;
        canvas3d_reset_frame_light_snapshots(c);
        canvas3d_clear_temp_buffers(c);
        canvas3d_clear_temp_objects(c);
        return;
    }

    /* Any frame ending into an offscreen target changes its contents (even a
     * clear-only frame) — advance its revision so material mirrors refresh. */
    if (c->render_target) {
        c->render_target->content_revision++;
        /* ADR 0301: remember the chain this frame was rendered under so the
         * target's material mirror can be resolved to display-referred values
         * on mirror-path backends (Metal resolves its own targets natively at
         * end-of-frame; a foreign target it samples still comes through the
         * mirror and picks this up). A canvas without a usable chain records
         * nothing: the mirror keeps the historical scene-referred bytes. */
        if (c->render_target_owner) {
            rt_rendertarget3d *rtd = (rt_rendertarget3d *)c->render_target_owner;
            rtd->display_chain_valid =
                vgfx3d_postfx_get_chain(c->postfx, &rtd->display_chain) ? 1 : 0;
            rtd->display_chain_revision = c->render_target->content_revision;
        }
    }

    cmds = (deferred_draw_t *)c->draw_cmds;
    queued_draw_count = c->draw_count;
    canvas3d_assign_occlusion_keys(c, cmds, queued_draw_count);

    if (c->skybox) {
        if (!rt_g3d_has_class(c->skybox, RT_G3D_CUBEMAP3D_CLASS_ID)) {
            rt_g3d_ref_slot_clear_unowned((void **)&c->skybox);
            rt_canvas3d_invalidate_skybox_cache(c);
        } else if (!rt_cubemap3d_is_complete(c->skybox)) {
            rt_g3d_ref_slot_release((void **)&c->skybox);
            rt_canvas3d_invalidate_skybox_cache(c);
        }
    }

    c->skybox_pass_done = 0;
    if (!c->frame_is_2d && !c->frame_is_view_model && c->skybox) {
        uint8_t *out_pixels = NULL;
        int32_t out_w = 0;
        int32_t out_h = 0;
        int32_t out_stride = 0;

        if (c->backend->draw_skybox) {
            /* GPU backends emit the skybox at far depth with a read-only LessEqual
             * depth state, so it is drawn inside the main pass after opaque draws —
             * early-Z then rejects every covered sky pixel instead of shading the
             * whole screen first and overdrawing it. See
             * canvas3d_draw_backend_skybox_after_opaque. */
        } else {
            if (c->render_target) {
                if (vgfx3d_rendertarget_ensure_color(c->render_target)) {
                    out_pixels = c->render_target->color_buf;
                    out_w = c->render_target->width;
                    out_h = c->render_target->height;
                    out_stride = c->render_target->stride;
                }
            } else {
                vgfx_framebuffer_t fb;
                if (c->gfx_win && vgfx_get_framebuffer(c->gfx_win, &fb)) {
                    out_pixels = fb.pixels;
                    out_w = fb.width;
                    out_h = fb.height;
                    out_stride = fb.stride;
                }
            }
        }

        if (!c->backend->draw_skybox && out_pixels && !canvas3d_backend_owns_gpu_rtt(c) &&
            canvas3d_ensure_skybox_cpu_cache(c, out_w, out_h)) {
            canvas3d_blit_skybox_cpu_cache(c, out_pixels, out_w, out_h, out_stride);
            if (c->render_target)
                c->render_target->color_dirty = 1;
            if (c->render_target)
                c->render_target->hdr_color_valid = 0;
        }
    }

    for (int32_t i = 0; i < c->draw_count; i++) {
        if (cmds[i].pass_kind == DEFERRED_PASS_MAIN)
            main_count++;
        else if (cmds[i].pass_kind == DEFERRED_PASS_SCREEN_OVERLAY)
            overlay_count++;
    }
    c->last_draw_count = main_count;

    if (main_count == 0 && overlay_count == 0) {
        canvas3d_draw_backend_skybox_after_opaque(c);
        c->backend->end_frame(c->backend_ctx);
        canvas3d_refresh_texture_upload_bytes(c);
        canvas3d_refresh_frame_gpu_time(c);
        c->in_frame = 0;
        c->frame_is_2d = 0;
        c->frame_is_view_model = 0;
        c->draw_count = 0;
        canvas3d_reset_frame_light_snapshots(c);
        canvas3d_clear_temp_buffers(c);
        canvas3d_clear_temp_objects(c);
        return;
    }

    if (!c->frame_is_view_model) {
        c->shadow_count = 0;
        memset(c->shadow_light_vps, 0, sizeof(c->shadow_light_vps));
    }
    /* Per-pass CPU timing: encode cost of each stage on this thread. GPU time
     * stays a single frame counter (canvas3d_refresh_frame_gpu_time). */
    memset(c->pass_cpu_ms, 0, sizeof(c->pass_cpu_ms));
    if (!c->frame_is_2d && !c->frame_is_view_model && main_count > 0) {
        int64_t pass_t0 = rt_clock_ticks_us();
        canvas3d_render_shadow_pass(c, cmds);
        c->pass_cpu_ms[RT_CANVAS3D_PASS_SHADOW] = (double)(rt_clock_ticks_us() - pass_t0) / 1000.0;
    }

    if (main_count > 0) {
        int64_t pass_t0 = rt_clock_ticks_us();
        canvas3d_render_main_pass(c, cmds);
        c->pass_cpu_ms[RT_CANVAS3D_PASS_MAIN] = (double)(rt_clock_ticks_us() - pass_t0) / 1000.0;
    }
    /* Catch-all: frames whose main pass culled everything (or had only overlay draws)
     * still need their sky; the latch makes this a no-op when the pass drew it. */
    canvas3d_draw_backend_skybox_after_opaque(c);

    {
        int64_t pass_t0 = rt_clock_ticks_us();
        c->backend->end_frame(c->backend_ctx);
        c->pass_cpu_ms[RT_CANVAS3D_PASS_BACKEND_END] =
            (double)(rt_clock_ticks_us() - pass_t0) / 1000.0;
    }
    canvas3d_refresh_texture_upload_bytes(c);
    canvas3d_refresh_frame_gpu_time(c);
    c->in_frame = 0;

    if (!c->frame_is_2d && overlay_count > 0) {
        int64_t pass_t0 = rt_clock_ticks_us();
        if (canvas3d_begin_overlay_frame(c, 1)) {
            for (int32_t i = 0; i < queued_draw_count; i++) {
                if (cmds[i].pass_kind != DEFERRED_PASS_SCREEN_OVERLAY)
                    continue;
                canvas3d_submit_screen_overlay_deferred(c, &cmds[i]);
            }
            c->backend->end_frame(c->backend_ctx);
            canvas3d_refresh_texture_upload_bytes(c);
            canvas3d_refresh_frame_gpu_time(c);
        }
        c->pass_cpu_ms[RT_CANVAS3D_PASS_OVERLAY] = (double)(rt_clock_ticks_us() - pass_t0) / 1000.0;
    }

    c->in_frame = 0;
    c->frame_is_2d = 0;
    c->frame_is_view_model = 0;
    c->draw_count = 0;
    canvas3d_reset_frame_light_snapshots(c);
    canvas3d_clear_temp_buffers(c);
    canvas3d_clear_temp_objects(c);
}
