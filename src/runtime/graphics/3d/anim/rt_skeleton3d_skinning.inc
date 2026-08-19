//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_skeleton3d_skinning.inc
// Purpose: Mesh3D weights and Canvas3D single, partitioned, instanced, and crowd skinning.
// Key invariants:
//   - Influence weights are finite, normalized, and bounded to valid palette indices.
//   - Checked size products and immutable snapshots precede deferred GPU submission.
// Ownership/Lifetime:
//   - Included by rt_skeleton3d.c; this is not a standalone translation unit.
//   - Meshes retain skeletons; deferred draws retain animator and snapshot storage as required.
// Links: rt_skeleton3d.c, rt_skeleton3d.h, rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//
/**
 * @file rt_skeleton3d_skinning.inc
 * @brief Connects skeleton palettes to mesh weights and Canvas3D draw paths.
 *
 * This implementation fragment owns mesh-to-skeleton binding, four-influence
 * vertex weight normalization, partitioned palette gathering, GPU-versus-CPU
 * skinning selection, temporal previous-pose forwarding, and packed
 * per-instance crowd submission. It is included by `rt_skeleton3d.c` and is
 * not a standalone translation unit.
 */
/*==========================================================================
 * Mesh3D extensions
 *=========================================================================*/

/// @brief Recompute the highest positively weighted bone referenced by a mesh.
/// @param[in] m Mesh whose readable vertices are scanned.
/// @return One greater than the largest referenced index, or zero when the mesh
/// has no readable positively weighted influence.
static int32_t mesh3d_recompute_weight_bone_count(const rt_mesh3d *m) {
    int32_t max_bone = -1;
    uint32_t vertex_count = rt_mesh3d_safe_vertex_count(m);
    if (!m || !m->vertices)
        return 0;
    for (uint32_t vi = 0; vi < vertex_count; vi++) {
        const vgfx3d_vertex_t *v = &m->vertices[vi];
        for (int i = 0; i < 4; i++) {
            if (v->bone_weights[i] > 0.0f && v->bone_indices[i] > max_bone)
                max_bone = v->bone_indices[i];
        }
    }
    return max_bone >= 0 ? max_bone + 1 : 0;
}

/// @brief Bind a skeleton to a mesh so its per-vertex `bone_indices/weights` can drive skinning.
/// @param[in,out] mesh Runtime mesh whose retained skeleton slot is replaced.
/// @param[in] skeleton Runtime skeleton to retain, or `NULL` to unbind.
/// @details Binding freezes skeleton topology and, for an ordinary mesh,
/// derives the palette count from the skeleton capped at the 256-bone draw
/// limit. Partitioned meshes preserve their local remapped palette size.
/// Unbinding clears the count, and every successful change invalidates cached
/// geometry state.
void rt_mesh3d_set_skeleton(void *mesh, void *skeleton) {
    rt_mesh3d *m = (rt_mesh3d *)rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
    rt_skeleton3d *s =
        (rt_skeleton3d *)rt_g3d_checked_or_null(skeleton, RT_G3D_SKELETON3D_CLASS_ID);
    void *old_skeleton;
    int32_t old_bone_count;
    int32_t new_bone_count;
    if (!m)
        return;
    rt_mesh3d_repair_geometry_counts(m);
    if (m->skeleton_ref && !rt_g3d_has_class(m->skeleton_ref, RT_G3D_SKELETON3D_CLASS_ID))
        mesh3d_release_skeleton_slot(&m->skeleton_ref);
    if (skeleton && !s)
        return;
    old_skeleton = m->skeleton_ref;
    old_bone_count = m->bone_count;
    mesh3d_assign_skeleton_ref(&m->skeleton_ref, s);
    if (s) {
        skeleton3d_freeze(s);
        /* Partitioned sub-meshes (bone_map set) keep their local palette size;
         * their vertices index the map, not the skeleton. */
        new_bone_count = skeleton3d_safe_bone_count(s);
        if (!m->bone_map)
            m->bone_count = new_bone_count > VGFX3D_MAX_BONES ? VGFX3D_MAX_BONES : new_bone_count;
        else if (m->bone_count < 0)
            m->bone_count = 0;
        else if (m->bone_count > VGFX3D_MAX_BONES)
            m->bone_count = VGFX3D_MAX_BONES;
    } else {
        m->bone_count = 0;
    }
    if (old_skeleton != m->skeleton_ref || old_bone_count != m->bone_count)
        rt_mesh3d_touch_geometry(m);
}

/// @brief Attach the per-vertex bone influence data (4 indices + 4 weights per vertex) to a mesh.
///
/// Each call overwrites the four influence slots of one existing vertex.
/// Valid finite positive weights are normalized to sum to one; nonpositive,
/// non-finite, or collectively negligible weights become zero.
///
/// @param[in,out] obj Mesh containing the target vertex.
/// @param[in] vertex_index Zero-based vertex index.
/// @param[in] b0 First bone index.
/// @param[in] w0 First influence weight.
/// @param[in] b1 Second bone index.
/// @param[in] w1 Second influence weight.
/// @param[in] b2 Third bone index.
/// @param[in] w2 Third influence weight.
/// @param[in] b3 Fourth bone index.
/// @param[in] w3 Fourth influence weight.
/// @details Bone indices must be in `[0, 255]`; invalid index/weight pairs are
/// cleared. A valid index is preserved even when its weight becomes zero so
/// imported joint attributes can round-trip. The mesh's derived bone count and
/// geometry generation are updated after a successful vertex lookup.
void rt_mesh3d_set_bone_weights(void *obj,
                                int64_t vertex_index,
                                int64_t b0,
                                double w0,
                                int64_t b1,
                                double w1,
                                int64_t b2,
                                double w2,
                                int64_t b3,
                                double w3) {
    rt_mesh3d *m = (rt_mesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_MESH3D_CLASS_ID);
    if (!m)
        return;
    rt_mesh3d_repair_geometry_counts(m);
    if (vertex_index < 0 || vertex_index >= m->vertex_count)
        return;

    // Bone indices are stored as uint8_t. A raw int64 -> uint8 cast silently
    // wraps values outside [0, 255] to bogus valid-looking indices, then the
    // skinning palette applies the wrong transform. Preserve valid authored
    // indices even when their weight is zero so importers can round-trip JOINTS
    // attributes, but only positive finite weights contribute to deformation.
    int64_t idx[4] = {b0, b1, b2, b3};
    double wt[4] = {w0, w1, w2, w3};
    uint8_t normalized_indices[4] = {0, 0, 0, 0};
    float normalized_weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    vgfx3d_vertex_t *v = &m->vertices[vertex_index];
    double max_weight = 0.0;
    double scaled_sum = 0.0;
    int64_t max_bone_index = -1;
    int64_t old_max_bone_index = -1;
    int32_t old_bone_count = m->bone_count;
    int changed = 0;
    for (int i = 0; i < 4; i++) {
        if (v->bone_weights[i] > 0.0f && v->bone_indices[i] > old_max_bone_index)
            old_max_bone_index = v->bone_indices[i];
    }
    for (int i = 0; i < 4; i++) {
        if (idx[i] < 0 || idx[i] >= VGFX3D_MAX_BONES) {
            normalized_indices[i] = 0;
            wt[i] = 0.0;
        } else {
            normalized_indices[i] = (uint8_t)idx[i];
            if (isfinite(wt[i]) && wt[i] > 0.0) {
                if (wt[i] > max_weight)
                    max_weight = wt[i];
                if (idx[i] > max_bone_index)
                    max_bone_index = idx[i];
            } else {
                wt[i] = 0.0;
            }
        }
    }
    if (max_weight > 0.0) {
        for (int i = 0; i < 4; ++i)
            scaled_sum += wt[i] / max_weight;
        if (isfinite(scaled_sum) && scaled_sum > 0.0 &&
            (max_weight > 1e-12 || max_weight * scaled_sum > 1e-12)) {
            for (int i = 0; i < 4; ++i)
                normalized_weights[i] = (float)((wt[i] / max_weight) / scaled_sum);
        } else {
            max_bone_index = -1;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (v->bone_indices[i] != normalized_indices[i] ||
            v->bone_weights[i] != normalized_weights[i])
            changed = 1;
        v->bone_indices[i] = normalized_indices[i];
        v->bone_weights[i] = normalized_weights[i];
    }
    if (max_bone_index >= 0 && max_bone_index + 1 > m->bone_count)
        m->bone_count = (int32_t)(max_bone_index + 1);
    else if (!m->skeleton_ref && old_max_bone_index >= 0 &&
             old_max_bone_index + 1 >= m->bone_count &&
             (max_bone_index < 0 || max_bone_index + 1 < m->bone_count))
        m->bone_count = mesh3d_recompute_weight_bone_count(m);
    if (changed || old_bone_count != m->bone_count)
        rt_mesh3d_touch_geometry(m);
}

/*==========================================================================
 * Canvas3D extension — DrawMeshSkinned
 *=========================================================================*/

/// @brief Write one row-major identity matrix into a skinning-palette slot.
/// @param[out] matrix Writable sixteen-float destination.
static void skinning_write_identity(float *matrix) {
    if (!matrix)
        return;
    memset(matrix, 0, 16u * sizeof(float));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

/// @brief Saturating update of public GPU-skinning diagnostics.
/// @param canvas Canvas counters to update.
/// @param draws Number of actual hardware draw chunks submitted.
/// @param bytes Palette bytes made available to those chunks.
static void skinning_record_gpu_submission(rt_canvas3d *canvas, int64_t draws, size_t bytes) {
    if (!canvas || draws <= 0)
        return;
    if (canvas->gpu_skinned_draw_count < 0)
        canvas->gpu_skinned_draw_count = 0;
    if (canvas->skinning_upload_bytes < 0)
        canvas->skinning_upload_bytes = 0;
    if (canvas->gpu_skinned_draw_count > INT64_MAX - draws)
        canvas->gpu_skinned_draw_count = INT64_MAX;
    else
        canvas->gpu_skinned_draw_count += draws;
    if (bytes > (size_t)INT64_MAX || canvas->skinning_upload_bytes > INT64_MAX - (int64_t)bytes)
        canvas->skinning_upload_bytes = INT64_MAX;
    else
        canvas->skinning_upload_bytes += (int64_t)bytes;
}

/// @brief Queue a skinned mesh draw with per-frame motion-vector continuity.
/// @details Chooses between GPU and CPU skinning at the start:
///   `vgfx3d_backend_prefers_gpu_skinning` asks the backend whether it wants
///   to consume the palette directly (for D3D11/OpenGL with enough uniform
///   space); if yes, we hand the palettes through to the normal draw path
///   via a shallow-copy `rt_mesh3d` so the backend's vertex shader does the
///   matrix blend. Otherwise we `vgfx3d_skin_vertices` on the CPU into a
///   scratch buffer, tracked as a temp allocation so it survives until the
///   frame is submitted and flushed. `motion_key` threads through for
///   temporal effects (TAA, motion blur) so the engine can correlate this
///   draw with the matching one in the previous frame. `prev_bone_palette`
///   supplies the previous-frame palette for per-vertex motion vectors —
///   without it, skinned motion blur falls back to object-space smearing.
///
/// @param[in,out] canvas Canvas receiving the deferred draw.
/// @param[in,out] mesh_obj Mesh whose vertices and influence data are used.
/// @param[in] model_matrix Row-major double-precision model transform.
/// @param[in] material Material for the draw.
/// @param[in] motion_key Stable identity used to correlate temporal draws.
/// @param[in] bone_palette Current row-major skinning matrices.
/// @param[in] prev_bone_palette Optional previous-frame matrices.
/// @param[in] bone_count Number of matrices supplied in each palette.
/// @param[in] local_bounds_min Optional three-float local lower bound.
/// @param[in] local_bounds_max Optional three-float local upper bound.
/// @param[in] conservative_bounds Nonzero when the bounds conservatively cover
/// animated motion.
/// @param[in] disable_occlusion Nonzero to bypass occlusion culling.
/// @details Partitioned meshes gather their remapped subset into frame-arena
/// storage before backend selection. GPU-capable paths attach the palettes to
/// a shallow mesh view; other paths skin a frame-owned vertex copy on the CPU.
/// Bone counts are capped at the renderer's 256-matrix palette limit.
void rt_canvas3d_draw_mesh_matrix_skinned_keyed_bounds(void *canvas,
                                                       void *mesh_obj,
                                                       const double *model_matrix,
                                                       void *material,
                                                       const void *motion_key,
                                                       const float *bone_palette,
                                                       const float *prev_bone_palette,
                                                       int32_t bone_count,
                                                       const float *local_bounds_min,
                                                       const float *local_bounds_max,
                                                       int8_t conservative_bounds,
                                                       int8_t disable_occlusion) {
    rt_mesh3d *mesh;
    rt_canvas3d *c;

    if (!canvas || !mesh_obj || !model_matrix || !material || !bone_palette || bone_count <= 0)
        return;
    mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
#if defined(RT_G3D_ALLOW_STACK_FIXTURES) && RT_G3D_ALLOW_STACK_FIXTURES
    if (!mesh && mesh_obj && !rt_heap_is_payload(mesh_obj))
        mesh = (rt_mesh3d *)mesh_obj;
#endif
    if (!mesh)
        return;
    rt_mesh3d_repair_geometry_counts(mesh);
    if (mesh->vertex_count == 0)
        return;

    c = rt_canvas3d_checked_or_stack(canvas);
    if (!c)
        return;
    if (rt_heap_is_payload(mesh_obj) && !rt_canvas3d_add_temp_object(canvas, mesh_obj))
        return;
    if (mesh->bone_map && mesh->bone_count > 0) {
        /* Partitioned sub-mesh: gather the skeleton-wide palette (possibly larger
         * than VGFX3D_MAX_BONES) down to the slots this sub-mesh references. The
         * gathered copies live in the canvas frame arena (stable until frame
         * flush; no per-draw malloc/track/free) so deferred draws never observe
         * a later animation tick. */
        int32_t local_count =
            mesh->bone_count > VGFX3D_MAX_BONES ? VGFX3D_MAX_BONES : mesh->bone_count;
        size_t lanes = (size_t)local_count * 16u;
        float *gathered = (float *)canvas3d_frame_arena_alloc(c, lanes * sizeof(float));
        float *gathered_prev = prev_bone_palette
                                   ? (float *)canvas3d_frame_arena_alloc(c, lanes * sizeof(float))
                                   : NULL;
        if (!gathered)
            return;
        for (int32_t i = 0; i < local_count; i++) {
            int32_t src = mesh->bone_map[i];
            if (src < 0 || src >= bone_count) {
                skinning_write_identity(gathered + (size_t)i * 16u);
                if (gathered_prev)
                    skinning_write_identity(gathered_prev + (size_t)i * 16u);
            } else {
                memcpy(gathered + (size_t)i * 16u,
                       bone_palette + (size_t)src * 16u,
                       16u * sizeof(float));
                if (gathered_prev)
                    memcpy(gathered_prev + (size_t)i * 16u,
                           prev_bone_palette + (size_t)src * 16u,
                           16u * sizeof(float));
            }
        }
        bone_palette = gathered;
        prev_bone_palette = gathered_prev;
        bone_count = local_count;
    }
    if (bone_count > VGFX3D_MAX_BONES)
        bone_count = VGFX3D_MAX_BONES;
    if (c->backend && !c->force_cpu_skinning &&
        (!mesh->extra_influences || c->backend->gpu_skinning_extras) &&
        vgfx3d_backend_prefers_gpu_skinning(c->backend, bone_count)) {
        rt_mesh3d tmp = *mesh;
        tmp.bone_palette = bone_palette;
        tmp.prev_bone_palette = prev_bone_palette;
        tmp.bone_count = bone_count;
        skinning_record_gpu_submission(
            c, 1, (size_t)bone_count * 16u * sizeof(float) * (prev_bone_palette ? 2u : 1u));
        rt_canvas3d_draw_mesh_matrix_keyed_bounds(canvas,
                                                  &tmp,
                                                  model_matrix,
                                                  material,
                                                  motion_key,
                                                  prev_bone_palette,
                                                  NULL,
                                                  local_bounds_min,
                                                  local_bounds_max,
                                                  conservative_bounds,
                                                  disable_occlusion,
                                                  0.0f);
        return;
    }

    if (!rt_alloc_count_ok(mesh->vertex_count, sizeof(vgfx3d_vertex_t)))
        return;
    /* Frame arena: stable until frame flush, no per-draw malloc + dedup-set
     * tracking + free. This is the CPU-skinning fallback's hottest allocation
     * (a full vertex buffer per skinned draw per frame). */
    vgfx3d_vertex_t *skinned = (vgfx3d_vertex_t *)canvas3d_frame_arena_alloc(
        c, (size_t)mesh->vertex_count * sizeof(vgfx3d_vertex_t));
    if (!skinned)
        return;

    /* An attached MorphTarget3D must blend BEFORE skinning — the GPU vertex
     * path composes morph-then-palette, and bind-space deltas applied to
     * already-posed vertices point the wrong way once a bone leaves its bind
     * orientation. Pre-blend into scratch here and clear the mesh copy's
     * morph binding below so the keyed draw never re-applies it post-skin. */
    const vgfx3d_vertex_t *skin_src = mesh->vertices;
    int morph_consumed = 0;
    if (mesh->morph_targets_ref && mesh->morph_deltas == NULL && mesh->morph_weights == NULL &&
        mesh->morph_shape_count == 0) {
        vgfx3d_vertex_t *morphed = (vgfx3d_vertex_t *)canvas3d_frame_arena_alloc(
            c, (size_t)mesh->vertex_count * sizeof(vgfx3d_vertex_t));
        if (morphed && rt_morphtarget3d_blend_vertices_internal(
                           mesh->morph_targets_ref, mesh->vertices, morphed, mesh->vertex_count))
            skin_src = morphed;
        morph_consumed = 1;
    }

    vgfx3d_skin_vertices_extra(skin_src,
                               skinned,
                               mesh->vertex_count,
                               bone_palette,
                               bone_count,
                               mesh->extra_influences,
                               &c->skinning_scratch);

    rt_mesh3d tmp = *mesh;
    if (morph_consumed) {
        tmp.morph_targets_ref = NULL;
        tmp.morph_deltas = NULL;
        tmp.morph_normal_deltas = NULL;
        tmp.morph_weights = NULL;
        tmp.prev_morph_weights = NULL;
        tmp.morph_shape_count = 0;
    }
    tmp.vertices = skinned;
    tmp.positions64 = NULL;
    tmp.bone_palette = NULL;
    tmp.prev_bone_palette = NULL;
    tmp.bone_count = 0;
    /* The cached AABB copied from the source mesh describes the PRE-skinned
     * vertices — for a skinned variant mesh that is a bone-local cluster near
     * the entity origin — so frustum/occlusion/shadow fits against it hide
     * posed characters whose origin leaves the view. Recompute the bounds
     * from the vertices actually being submitted. */
    tmp.bounds_dirty = 1;
    rt_mesh3d_refresh_bounds(&tmp);
    rt_canvas3d_draw_mesh_matrix_keyed_bounds(canvas,
                                              &tmp,
                                              model_matrix,
                                              material,
                                              motion_key,
                                              NULL,
                                              NULL,
                                              local_bounds_min,
                                              local_bounds_max,
                                              conservative_bounds,
                                              disable_occlusion,
                                              0.0f);
}

/// @brief Draw a skinned mesh using an explicit model matrix and keyed bone palette.
/// @param[in,out] canvas Canvas receiving the draw.
/// @param[in,out] mesh_obj Mesh to skin.
/// @param[in] model_matrix Row-major double-precision model transform.
/// @param[in] material Material for the draw.
/// @param[in] motion_key Stable temporal identity.
/// @param[in] bone_palette Current skinning matrices.
/// @param[in] prev_bone_palette Optional previous-frame matrices.
/// @param[in] bone_count Number of matrices in each palette.
/// @details Delegates to the bounds-aware path with no explicit local bounds
/// and with conservative-bounds and occlusion overrides disabled.
void rt_canvas3d_draw_mesh_matrix_skinned_keyed(void *canvas,
                                                void *mesh_obj,
                                                const double *model_matrix,
                                                void *material,
                                                const void *motion_key,
                                                const float *bone_palette,
                                                const float *prev_bone_palette,
                                                int32_t bone_count) {
    rt_canvas3d_draw_mesh_matrix_skinned_keyed_bounds(canvas,
                                                      mesh_obj,
                                                      model_matrix,
                                                      material,
                                                      motion_key,
                                                      bone_palette,
                                                      prev_bone_palette,
                                                      bone_count,
                                                      NULL,
                                                      NULL,
                                                      0,
                                                      0);
}

/// @brief Draw a skinned mesh — applies an animator's bone palette before rasterising.
/// @details Accepts either a raw `AnimPlayer3D` or an `AnimController3D`. For a
///   controller, the state-machine's computed final/previous bone palettes and
///   skeleton drive the skinning, so idle/walk states and crossfades render directly.
/// @param[in,out] canvas Canvas receiving the draw.
/// @param[in,out] mesh Mesh to skin and draw.
/// @param[in] transform Runtime Mat4 supplying the model transform and stable
/// temporal motion key.
/// @param[in] material Material for the draw.
/// @param[in,out] anim_player `AnimPlayer3D` or `AnimController3D` supplying
/// the skeleton palettes.
/// @details The animator is retained temporarily by the canvas so its palette
/// remains live until submission. Player paths rotate temporal snapshots at
/// most once for the current canvas frame.
void rt_canvas3d_draw_mesh_skinned(
    void *canvas, void *mesh, void *transform, void *material, void *anim_player) {
    rt_mesh3d *m;
    rt_anim_player3d *p;
    rt_skeleton3d *skel;
    const float *prev_palette;
    mat4_impl *transform_mat;
    if (!canvas || !mesh || !transform || !material || !anim_player)
        return;
    if (!rt_canvas3d_checked_or_stack(canvas))
        return;
    transform_mat = skeleton3d_mat4_checked(transform);
    if (!transform_mat)
        return;
    m = (rt_mesh3d *)rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
    if (!m)
        return;
    rt_mesh3d_repair_geometry_counts(m);
    if (m->vertex_count == 0)
        return;

    if (rt_g3d_has_class(anim_player, RT_G3D_ANIMCONTROLLER3D_CLASS_ID)) {
        int32_t ctrl_bone_count = 0;
        const float *ctrl_palette =
            rt_anim_controller3d_get_final_palette_data(anim_player, &ctrl_bone_count);
        const float *ctrl_prev = rt_anim_controller3d_get_previous_palette_data(anim_player, NULL);
        void *ctrl_skel = rt_anim_controller3d_get_skeleton(anim_player);
        if (!ctrl_palette || ctrl_bone_count <= 0 || !ctrl_skel)
            return;
        if (!rt_canvas3d_add_temp_object(canvas, anim_player))
            return;
        {
            int32_t skeleton_bones = skeleton3d_safe_bone_count((rt_skeleton3d *)ctrl_skel);
            if (ctrl_bone_count > skeleton_bones)
                ctrl_bone_count = skeleton_bones;
        }
        if (ctrl_bone_count <= 0)
            return;
        rt_canvas3d_draw_mesh_matrix_skinned_keyed(canvas,
                                                   mesh,
                                                   transform_mat->m,
                                                   material,
                                                   transform,
                                                   ctrl_palette,
                                                   ctrl_prev,
                                                   ctrl_bone_count);
        return;
    }

    p = anim_player3d_checked(anim_player);
    if (!p)
        return;
    skel = animation3d_skeleton_slot((void **)&p->skeleton);
    if (!skel || !p->bone_palette)
        return;
    if (!rt_canvas3d_add_temp_object(canvas, anim_player))
        return;
    prev_palette = anim_player_prepare_prev_palette(p, rt_canvas3d_get_frame_serial(canvas));
    rt_canvas3d_draw_mesh_matrix_skinned_keyed(canvas,
                                               mesh,
                                               transform_mat->m,
                                               material,
                                               transform,
                                               p->bone_palette,
                                               prev_palette,
                                               skeleton3d_safe_bone_count(skel));
}

/*==========================================================================
 * Canvas3D extension — per-instance skinned crowds (R18)
 *=========================================================================*/

/// @brief Queue @p instance_count copies of a skinned mesh, each posed by its own palette.
/// @details GPU-skinning backends pack chunks of `VGFX3D_MAX_BONES / bone_count` instances
///   into the existing per-draw palette slot (the vertex shader indexes
///   `palette[instance_id * stride + bone]`), so a 32-bone crowd rides one hardware draw per
///   32 characters instead of one per character. Palettes are snapshotted into frame-owned
///   buffers before queueing so later animation ticks cannot repose queued draws. Draws that
///   cannot take the hardware path (software backend, blending materials, extra influences,
///   partitioned bone maps) fall back to one skinned draw per instance, which preserves the
///   exact single-draw pose semantics on every backend.
/// @param[in,out] canvas Canvas receiving the instance draws; it must be inside
/// an active frame with a backend.
/// @param[in,out] mesh_obj Mesh shared by every instance.
/// @param[in] material Material shared by every instance.
/// @param[in] instance_matrices Packed row-major float model matrices, one per
/// instance.
/// @param[in] instance_count Number of model/palette records.
/// @param[in] bone_palettes Packed palettes:
/// `instance_count * bone_count * 16` floats.
/// @param[in] prev_bone_palettes Optional previous-frame palettes with the same
/// layout, or `NULL`.
/// @param[in] bone_count Matrices per instance palette, in `[1, 256]`.
/// @details Hardware submission requires a mesh without partition remapping or
/// extra influences and chunks instances so each draw fits the shared palette.
/// Otherwise each instance is submitted through the ordinary bounds-aware
/// skinned path.
void rt_canvas3d_draw_mesh_instanced_skinned(void *canvas,
                                             void *mesh_obj,
                                             void *material,
                                             const float *instance_matrices,
                                             int32_t instance_count,
                                             const float *bone_palettes,
                                             const float *prev_bone_palettes,
                                             int32_t bone_count) {
    rt_canvas3d *c;
    rt_mesh3d *mesh;
    int gpu_ok;

    if (!canvas || !mesh_obj || !material || !instance_matrices || !bone_palettes ||
        instance_count <= 0 || bone_count <= 0)
        return;
    c = rt_canvas3d_checked_or_stack(canvas);
    if (!c || !c->in_frame || !c->backend)
        return;
    if (bone_count > VGFX3D_MAX_BONES) {
        rt_trap("Canvas3D.DrawMeshInstancedSkinned: bone count exceeds the palette limit");
        return;
    }
    mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
#if defined(RT_G3D_ALLOW_STACK_FIXTURES) && RT_G3D_ALLOW_STACK_FIXTURES
    if (!mesh && mesh_obj && !rt_heap_is_payload(mesh_obj))
        mesh = (rt_mesh3d *)mesh_obj;
#endif
    if (!mesh)
        return;

    gpu_ok = !c->force_cpu_skinning && !mesh->extra_influences && !mesh->bone_map &&
             vgfx3d_backend_prefers_gpu_skinning(c->backend, bone_count);
    if (gpu_ok) {
        int32_t per_draw = VGFX3D_MAX_BONES / bone_count;
        size_t palette_lanes;
        size_t palette_bytes;
        float *palette_snapshot = NULL;
        float *prev_snapshot = NULL;
        int32_t start;
        int64_t queued_chunks = 0;
        size_t queued_palette_bytes = 0;

        if ((size_t)instance_count > SIZE_MAX / (size_t)bone_count / 16u)
            return;
        palette_lanes = (size_t)instance_count * (size_t)bone_count * 16u;
        if (palette_lanes > SIZE_MAX / sizeof(float))
            return;
        palette_bytes = palette_lanes * sizeof(float);
        /* Frame arena: stable until frame flush, no per-draw malloc/track/free. */
        palette_snapshot = (float *)canvas3d_frame_arena_alloc(c, palette_bytes);
        if (!palette_snapshot) {
            gpu_ok = 0;
        } else {
            memcpy(palette_snapshot, bone_palettes, palette_bytes);
        }
        if (gpu_ok && prev_bone_palettes) {
            prev_snapshot = (float *)canvas3d_frame_arena_alloc(c, palette_bytes);
            if (prev_snapshot)
                memcpy(prev_snapshot, prev_bone_palettes, palette_bytes);
        }
        for (start = 0; gpu_ok && start < instance_count; start += per_draw) {
            int32_t len = instance_count - start;
            int32_t draw_count_before = c->draw_count;
            if (len > per_draw)
                len = per_draw;
            if (!rt_canvas3d_queue_instanced_batch_skinned(
                    canvas,
                    mesh_obj,
                    material,
                    instance_matrices + (size_t)start * 16u,
                    len,
                    palette_snapshot + (size_t)start * (size_t)bone_count * 16u,
                    prev_snapshot ? prev_snapshot + (size_t)start * (size_t)bone_count * 16u : NULL,
                    len * bone_count,
                    bone_count)) {
                /* Hardware refusal is batch-invariant (same mesh/material), so nothing
                 * was queued yet; drop to the per-instance path for the whole batch. */
                gpu_ok = 0;
                break;
            }
            if (c->draw_count > draw_count_before) {
                int64_t draw_delta = (int64_t)c->draw_count - draw_count_before;
                size_t chunk_bytes = (size_t)len * (size_t)bone_count * 16u * sizeof(float);
                if (prev_snapshot) {
                    if (chunk_bytes > SIZE_MAX - chunk_bytes)
                        chunk_bytes = SIZE_MAX;
                    else
                        chunk_bytes += chunk_bytes;
                }
                if (queued_chunks > INT64_MAX - draw_delta)
                    queued_chunks = INT64_MAX;
                else
                    queued_chunks += draw_delta;
                if (queued_palette_bytes > SIZE_MAX - chunk_bytes)
                    queued_palette_bytes = SIZE_MAX;
                else
                    queued_palette_bytes += chunk_bytes;
            }
        }
        if (gpu_ok) {
            skinning_record_gpu_submission(c, queued_chunks, queued_palette_bytes);
            return;
        }
    }

    for (int32_t i = 0; i < instance_count; i++) {
        double model_d[16];
        const float *m = instance_matrices + (size_t)i * 16u;
        const float *palette_i = bone_palettes + (size_t)i * (size_t)bone_count * 16u;
        const float *prev_i =
            prev_bone_palettes ? prev_bone_palettes + (size_t)i * (size_t)bone_count * 16u : NULL;
        for (int j = 0; j < 16; j++)
            model_d[j] = (double)m[j];
        uintptr_t motion_key =
            canvas3d_instance_motion_key(mesh_obj, material, instance_matrices, instance_count, i);
        rt_canvas3d_draw_mesh_matrix_skinned_keyed_bounds(canvas,
                                                          mesh_obj,
                                                          model_d,
                                                          material,
                                                          (const void *)motion_key,
                                                          palette_i,
                                                          prev_i,
                                                          bone_count,
                                                          NULL,
                                                          NULL,
                                                          0,
                                                          0);
    }
}

/// @brief Zia entry: draw an InstanceBatch3D with one animator palette per instance.
/// @details @p players is a Seq holding one AnimPlayer3D or AnimController3D per batch
///   instance; every animator must drive a skeleton with the same bone count. Palettes are
///   packed into one contiguous block and submitted through
///   rt_canvas3d_draw_mesh_instanced_skinned.
/// @param[in,out] canvas Canvas receiving the crowd draw; it must be inside an
/// active frame.
/// @param[in] batch InstanceBatch3D supplying mesh, material, model matrices,
/// and instance count.
/// @param[in] players Runtime sequence containing exactly one compatible
/// player or controller per instance.
/// @details All current palettes are copied into one temporary instance-major
/// block. Previous palettes are supplied only when every animator has one, so
/// temporal data remains consistently shaped for the entire batch.
void rt_canvas3d_draw_instanced_skinned(void *canvas, void *batch, void *players) {
    rt_canvas3d *c;
    void *mesh;
    void *material;
    const float *transforms;
    int32_t instance_count = 0;
    int64_t player_count;
    int32_t bone_count = 0;
    float *packed = NULL;
    float *packed_prev = NULL;
    int has_all_prev = 1;
    size_t lanes;

    if (!canvas || !batch || !players)
        return;
    c = rt_canvas3d_checked_or_stack(canvas);
    if (!c || !c->in_frame)
        return;
    mesh = rt_instbatch3d_borrow_mesh(batch);
    material = rt_instbatch3d_borrow_material(batch);
    transforms = rt_instbatch3d_borrow_transforms(batch, &instance_count);
    if (!mesh || !material || !transforms || instance_count <= 0)
        return;
    player_count = rt_seq_len(players);
    if (player_count != (int64_t)instance_count) {
        rt_trap("Canvas3D.DrawInstancedSkinned: player count must match the batch instance "
                "count");
        return;
    }

    /* First pass: agree on one bone count across every animator. */
    for (int32_t i = 0; i < instance_count; i++) {
        void *item = rt_seq_get(players, i);
        int32_t item_bones = 0;
        if (rt_g3d_has_class(item, RT_G3D_ANIMCONTROLLER3D_CLASS_ID)) {
            if (!rt_anim_controller3d_get_final_palette_data(item, &item_bones))
                item_bones = 0;
        } else {
            rt_anim_player3d *p = anim_player3d_checked(item);
            rt_skeleton3d *skel = p ? animation3d_skeleton_slot((void **)&p->skeleton) : NULL;
            if (p && p->bone_palette && skel)
                item_bones = skeleton3d_safe_bone_count(skel);
        }
        if (item_bones <= 0) {
            rt_trap("Canvas3D.DrawInstancedSkinned: every instance needs an AnimPlayer3D or "
                    "AnimController3D with an active palette");
            return;
        }
        if (bone_count == 0)
            bone_count = item_bones;
        else if (bone_count != item_bones) {
            rt_trap("Canvas3D.DrawInstancedSkinned: instance palettes must share one skeleton "
                    "bone count");
            return;
        }
    }
    if (bone_count <= 0 || bone_count > VGFX3D_MAX_BONES) {
        rt_trap("Canvas3D.DrawInstancedSkinned: bone count exceeds the palette limit");
        return;
    }

    if ((size_t)instance_count > SIZE_MAX / (size_t)bone_count / 16u)
        return;
    lanes = (size_t)instance_count * (size_t)bone_count * 16u;
    if (lanes > SIZE_MAX / sizeof(float))
        return;
    packed = (float *)canvas3d_frame_arena_alloc(c, lanes * sizeof(float));
    packed_prev = (float *)canvas3d_frame_arena_alloc(c, lanes * sizeof(float));
    if (!packed || !packed_prev) {
        rt_trap("Canvas3D.DrawInstancedSkinned: palette allocation failed");
        return;
    }

    for (int32_t i = 0; i < instance_count; i++) {
        void *item = rt_seq_get(players, i);
        const float *palette = NULL;
        const float *prev = NULL;
        int32_t item_bones = 0;
        int32_t prev_bones = 0;
        if (rt_g3d_has_class(item, RT_G3D_ANIMCONTROLLER3D_CLASS_ID)) {
            palette = rt_anim_controller3d_get_final_palette_data(item, &item_bones);
            prev = rt_anim_controller3d_get_previous_palette_data(item, &prev_bones);
        } else {
            rt_anim_player3d *p = anim_player3d_checked(item);
            rt_skeleton3d *skel = p ? animation3d_skeleton_slot((void **)&p->skeleton) : NULL;
            palette = p ? p->bone_palette : NULL;
            item_bones = skel ? skeleton3d_safe_bone_count(skel) : 0;
            prev = p ? anim_player_prepare_prev_palette(p, rt_canvas3d_get_frame_serial(canvas))
                     : NULL;
            prev_bones = prev ? item_bones : 0;
        }
        if (!palette || item_bones != bone_count) {
            rt_trap("Canvas3D.DrawInstancedSkinned: animator palette changed during packing");
            return;
        }
        memcpy(packed + (size_t)i * (size_t)bone_count * 16u,
               palette,
               (size_t)bone_count * 16u * sizeof(float));
        if (prev && prev_bones == bone_count)
            memcpy(packed_prev + (size_t)i * (size_t)bone_count * 16u,
                   prev,
                   (size_t)bone_count * 16u * sizeof(float));
        else
            has_all_prev = 0;
    }

    rt_canvas3d_draw_mesh_instanced_skinned(canvas,
                                            mesh,
                                            material,
                                            transforms,
                                            instance_count,
                                            packed,
                                            has_all_prev ? packed_prev : NULL,
                                            bone_count);
}
