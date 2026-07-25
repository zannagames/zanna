//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_instanced.inc
// Purpose: Canvas3D instanced-batch submission for fallback, hardware, and compact particles.
// Key invariants:
//   - Deferred commands snapshot mutable instance payloads before their callers may reuse storage.
//   - Heap base geometry binds an immutable retained revision; generated tangents use a distinct
//     stable backend cache key while sharing the same source geometry revision.
//   - Compact particles share one process-lifetime unit quad and a stable geometry-cache identity.
// Ownership/Lifetime:
//   - Canvas3D owns queued snapshots and tracked references until the deferred frame is released.
//   - The unit quad is immutable static storage and is never freed or modified after
//   initialization.
// Links: rt_canvas3d.c, rt_canvas3d_internal.h, vgfx3d_backend.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Canvas3D instanced mesh and compact-particle submission.
/// @details This fragment snapshots mutable instance data, retains referenced
///   render resources, selects hardware or per-instance fallback paths, and
///   partitions spatially wide batches for useful aggregate culling.
///
/// @brief Owned per-batch references for an instanced draw, so the many failure
///        exits release the tracked mesh/material temp-objects through one helper.
/// @details The structure borrows the validated canvas, mesh, and material
///   objects while recording only the references newly acquired by one
///   submission attempt for transactional rollback.
typedef struct {
    rt_canvas3d *c;
    rt_mesh3d *mesh;
    rt_material3d *mat;
    void *mesh_obj;
    void *material_obj;
    void *new_refs[CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP];
    int32_t new_ref_count;
} canvas3d_instanced_batch_t;

/// @brief Release the batch's tracked mesh/material temp-object references (material then mesh).
/// @details The helper is safe for a `NULL` descriptor. It releases exactly the
///   resources recorded by this submission attempt and does not free @p b.
/// @param b Borrowed batch descriptor whose newly tracked references are released.
static void canvas3d_instanced_release_refs(const canvas3d_instanced_batch_t *b) {
    if (!b)
        return;
    canvas3d_release_new_draw_resources(b->c, b->new_refs, b->new_ref_count);
}

/// @brief Populate the shared base draw command from a mesh's geometry/skin/morph state.
/// @details Initializes @p base_cmd to an identity-model draw, validates the
///   mesh's safe geometry counts, flattens material and image-based-lighting
///   state, binds shared animation data, and records full-resolution texture
///   streaming demand for the spatially distributed batch.
/// @param mesh Borrowed validated Mesh3D supplying geometry and animation state.
/// @param mat Borrowed validated Material3D to flatten into the command.
/// @param canvas Borrowed Canvas3D supplying frame lighting and texture-stream state.
/// @param base_cmd Non-`NULL` output command overwritten in full.
static void canvas3d_build_instanced_base_cmd(rt_mesh3d *mesh,
                                              rt_material3d *mat,
                                              rt_canvas3d *canvas,
                                              vgfx3d_draw_cmd_t *base_cmd) {
    memset(base_cmd, 0, sizeof(*base_cmd));
    base_cmd->vertices = mesh->vertices;
    base_cmd->vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    base_cmd->indices = mesh->indices;
    base_cmd->index_count = rt_mesh3d_validated_index_count(mesh);
    canvas3d_mark_cmd_validated_indices(base_cmd, mesh);
    base_cmd->model_matrix[0] = base_cmd->model_matrix[5] = base_cmd->model_matrix[10] =
        base_cmd->model_matrix[15] = 1.0f;
    canvas3d_texture_stream_touch_material(canvas, mat);
    canvas3d_fill_material_cmd(mat, base_cmd);
    canvas3d_apply_ibl_env(canvas, base_cmd);
    canvas3d_bind_skinning_cmd(canvas, base_cmd, mesh, NULL);
    canvas3d_bind_morph_cmd(canvas, base_cmd, mesh, NULL);
    /* Instanced batches span many transforms; keep their textures full-res. */
    canvas3d_texture_stream_record_cmd_full(canvas, base_cmd);
}

/// @brief Choose local bounds for an instanced draw, preferring caller-provided conservative data.
/// @details Extras such as vegetation can expand local bounds for shader-like deformation without
///   mutating the shared mesh. Invalid explicit bounds are ignored and the mesh/snapshot AABB is
///   used instead.
/// @param mesh Borrowed Mesh3D providing cached or computable local bounds.
/// @param vertices Borrowed vertex stream used when bounds must be recomputed.
/// @param explicit_min Optional three-component caller-provided AABB minimum.
/// @param explicit_max Optional three-component caller-provided AABB maximum;
///   both explicit arrays must form a valid bound to be selected.
/// @param out_min Non-`NULL` three-component output receiving the selected minimum.
/// @param out_max Non-`NULL` three-component output receiving the selected maximum.
static void canvas3d_select_instanced_local_bounds(const rt_mesh3d *mesh,
                                                   const vgfx3d_vertex_t *vertices,
                                                   const float *explicit_min,
                                                   const float *explicit_max,
                                                   float out_min[3],
                                                   float out_max[3]) {
    if (explicit_min && explicit_max && canvas3d_bounds_are_valid(explicit_min, explicit_max)) {
        memcpy(out_min, explicit_min, sizeof(float) * 3u);
        memcpy(out_max, explicit_max, sizeof(float) * 3u);
        return;
    }
    canvas3d_copy_or_compute_local_bounds(mesh, vertices, out_min, out_max);
}

/// @brief Fallback instanced submission: enqueue each instance as an individual mesh draw.
/// @details Used when the backend lacks hardware instancing or the material needs blending. Queue
///   growth is split into bounded 65,536-instance reservations, but the global instance index is
///   retained across chunks so transform order and motion-history identity remain stable. No
///   instance is omitted merely because the request crosses a chunk boundary.
/// @param b Borrowed validated batch descriptor whose references cover queued commands.
/// @param base_cmd Borrowed command template copied once for each instance.
/// @param instance_matrices Borrowed array of @p instance_count row-major 4-by-4 matrices.
/// @param instance_count Number of matrices and fallback draws to process.
/// @param prev_instance_matrices Optional borrowed array of previous-frame matrices.
/// @param has_prev_instance_matrices Non-zero when @p prev_instance_matrices
///   contains one matrix per current instance.
/// @param rebase_positions64_instances Non-zero to rebase each mesh snapshot
///   independently for camera-relative double-precision geometry.
/// @param generate_rebased_tangents Non-zero to generate tangents on each
///   rebased snapshot when the material needs normal mapping.
/// @param explicit_local_bounds_min Optional local-space AABB minimum paired
///   with @p explicit_local_bounds_max.
/// @param explicit_local_bounds_max Optional local-space AABB maximum paired
///   with @p explicit_local_bounds_min.
/// @param conservative_bounds Non-zero to mark the selected bounds conservative.
/// @param disable_occlusion Non-zero to disable occlusion culling for each
///   generated draw.
static void canvas3d_queue_instanced_fallback(const canvas3d_instanced_batch_t *b,
                                              const vgfx3d_draw_cmd_t *base_cmd,
                                              const float *instance_matrices,
                                              int32_t instance_count,
                                              const float *prev_instance_matrices,
                                              int8_t has_prev_instance_matrices,
                                              int rebase_positions64_instances,
                                              int generate_rebased_tangents,
                                              const float *explicit_local_bounds_min,
                                              const float *explicit_local_bounds_max,
                                              int8_t conservative_bounds,
                                              int8_t disable_occlusion) {
    rt_canvas3d *c = b->c;
    rt_mesh3d *mesh = b->mesh;
    rt_material3d *mat = b->mat;
    float local_bounds_min[3];
    float local_bounds_max[3];
    canvas3d_select_instanced_local_bounds(mesh,
                                           base_cmd ? base_cmd->vertices : NULL,
                                           explicit_local_bounds_min,
                                           explicit_local_bounds_max,
                                           local_bounds_min,
                                           local_bounds_max);
    /* Validate the full request before publishing its first chunk. A non-finite matrix must not
     * leave an otherwise avoidable partial batch in the queue. */
    for (int32_t i = 0; i < instance_count; i++) {
        float preflight[16];
        double shift[3];
        int preflight_ok;
        if (rebase_positions64_instances) {
            preflight_ok = canvas3d_compute_vertex_rebase_shift_f32(
                c, &instance_matrices[(size_t)i * 16u], shift, preflight);
        } else {
            preflight_ok = canvas3d_copy_mat4_f32_for_frame(
                               c, &instance_matrices[(size_t)i * 16u], preflight) &&
                           (!has_prev_instance_matrices || !prev_instance_matrices ||
                            canvas3d_copy_mat4_f32_for_frame(
                                c, &prev_instance_matrices[(size_t)i * 16u], preflight));
        }
        if (!preflight_ok) {
            canvas3d_instanced_release_refs(b);
            rt_trap("Canvas3D.DrawMeshInstanced: rebased instance matrix is out of float range");
            return;
        }
    }
    for (int32_t chunk_begin = 0; chunk_begin < instance_count;) {
        int32_t chunk_count = instance_count - chunk_begin;
        if (chunk_count > CANVAS3D_FALLBACK_CHUNK_INSTANCES)
            chunk_count = CANVAS3D_FALLBACK_CHUNK_INSTANCES;
        if (c->draw_count < 0 || c->draw_count > INT32_MAX - chunk_count ||
            !ensure_deferred_capacity(
                &c->draw_cmds, &c->draw_capacity, c->draw_count + chunk_count)) {
            int64_t remaining = (int64_t)instance_count - (int64_t)chunk_begin;
            canvas3d_record_submission_failure(c, RT_CANVAS3D_SUBMISSION_QUEUE_FAILURE);
            if (remaining > INT64_MAX - c->last_instanced_fallback_dropped_count)
                c->last_instanced_fallback_dropped_count = INT64_MAX;
            else
                c->last_instanced_fallback_dropped_count += remaining;
            if (chunk_begin == 0)
                canvas3d_instanced_release_refs(b);
            rt_trap("Canvas3D.DrawMeshInstanced: deferred draw queue allocation failed");
            return;
        }
        for (int32_t offset = 0; offset < chunk_count; offset++) {
            int32_t i = chunk_begin + offset;
            vgfx3d_draw_cmd_t per_instance = *base_cmd;
            uintptr_t instance_motion_key = canvas3d_instance_motion_key(
                b->mesh_obj, b->material_obj, instance_matrices, instance_count, i);
            if (rebase_positions64_instances) {
                double vertex_rebase_origin[3] = {0.0, 0.0, 0.0};
                vgfx3d_vertex_t *rebased_vertices = NULL;
                uint32_t *rebased_indices = NULL;
                if (!canvas3d_compute_vertex_rebase_shift_f32(c,
                                                              &instance_matrices[(size_t)i * 16u],
                                                              vertex_rebase_origin,
                                                              per_instance.model_matrix) ||
                    !canvas3d_snapshot_mesh_geometry_rebased(
                        c, mesh, vertex_rebase_origin, &rebased_vertices, &rebased_indices)) {
                    rt_trap(
                        "Canvas3D.DrawMeshInstanced: rebased instance geometry allocation failed");
                    return;
                }
                if (generate_rebased_tangents &&
                    !canvas3d_generate_snapshot_tangents(mesh, rebased_vertices, rebased_indices))
                    canvas3d_disable_cmd_normal_map(&per_instance);
                per_instance.vertices = rebased_vertices;
                per_instance.indices = rebased_indices;
                per_instance.geometry_key = NULL;
                per_instance.geometry_revision = 0;
                memcpy(per_instance.prev_model_matrix,
                       per_instance.model_matrix,
                       sizeof(per_instance.prev_model_matrix));
                per_instance.has_prev_model_matrix = 0;
            } else if (!canvas3d_copy_mat4_f32_for_frame(
                           c, &instance_matrices[(size_t)i * 16u], per_instance.model_matrix)) {
                rt_trap(
                    "Canvas3D.DrawMeshInstanced: rebased instance matrix is out of float range");
                return;
            }
            if (!rebase_positions64_instances && has_prev_instance_matrices &&
                prev_instance_matrices) {
                if (!canvas3d_copy_mat4_f32_for_frame(c,
                                                      &prev_instance_matrices[(size_t)i * 16u],
                                                      per_instance.prev_model_matrix)) {
                    rt_trap("Canvas3D.DrawMeshInstanced: rebased previous instance matrix is out "
                            "of float range");
                    return;
                }
                per_instance.has_prev_model_matrix = 1;
            } else if (!rebase_positions64_instances) {
                canvas3d_resolve_previous_model(c,
                                                instance_motion_key,
                                                per_instance.model_matrix,
                                                per_instance.prev_model_matrix,
                                                &per_instance.has_prev_model_matrix);
            }
            per_instance.prev_instance_matrices = NULL;
            per_instance.has_prev_instance_matrices = 0;
            int8_t instance_blends = canvas3d_cmd_requires_blend(&per_instance);
            if (!canvas3d_enqueue_draw(
                    c,
                    &per_instance,
                    DEFERRED_DRAW_MESH,
                    DEFERRED_PASS_MAIN,
                    NULL,
                    0,
                    1,
                    c->wireframe,
                    canvas3d_material_backface_cull(c, mat),
                    canvas3d_sanitize_sort_key(canvas3d_compute_sort_key(c,
                                                                         per_instance.model_matrix,
                                                                         local_bounds_min,
                                                                         local_bounds_max,
                                                                         1,
                                                                         instance_blends),
                                               instance_blends),
                    local_bounds_min,
                    local_bounds_max)) {
                /* Earlier instances may already be queued and still borrow the retained refs. */
                rt_trap("Canvas3D.DrawMeshInstanced: deferred draw queue allocation failed");
                return;
            }
            {
                deferred_draw_t *queued = &((deferred_draw_t *)c->draw_cmds)[c->draw_count - 1];
                queued->stable_sort_id = instance_motion_key
                                             ? instance_motion_key
                                             : (queued->stable_sort_id ^ (uintptr_t)(uint32_t)i);
                queued->occlusion_fingerprint = 0;
                queued->occlusion_key = 0;
                canvas3d_finalize_deferred_visibility_flags(
                    queued, conservative_bounds, disable_occlusion);
            }
            if (c->last_instanced_fallback_count < INT64_MAX)
                c->last_instanced_fallback_count++;
        }
        chunk_begin += chunk_count;
    }
}

/// @brief Allocate a canvas-tracked temp buffer and copy @p instance_count 4×4 instance
///   matrices into it (overflow-checked), used when deferring an instanced draw. Traps with
///   @p trap_message on allocation failure.
/// @details When @p union_local_min / @p union_local_max are provided, the copy loop also
///   accumulates the union of every instance's transformed AABB into
///   @p out_world_min / @p out_world_max (mirroring the skip-invalid-transform contract of
///   canvas3d_compute_deferred_world_bounds_uncached), so append never has to rescan the
///   matrices for world bounds. @p out_has_world_bounds is set when at least one instance
///   contributed a finite bound.
/// @param c Borrowed Canvas3D providing camera-relative matrix conversion and counters.
/// @param matrices Borrowed source array containing @p instance_count matrices.
/// @param instance_count Positive number of matrices to copy.
/// @param trap_message Optional diagnostic used for overflow, allocation, or
///   conversion failure; a default instancing diagnostic is used when `NULL`.
/// @param union_local_min Optional local AABB minimum for accumulating world bounds.
/// @param union_local_max Optional local AABB maximum for accumulating world bounds.
/// @param out_world_min Optional three-component aggregate world minimum output.
/// @param out_world_max Optional three-component aggregate world maximum output.
/// @param out_has_world_bounds Optional flag set to one when at least one finite
///   transformed bound contributes, and initialized to zero otherwise.
/// @return The snapshot buffer, or NULL for invalid arguments.
static float *canvas3d_alloc_instance_matrix_snapshot(rt_canvas3d *c,
                                                      const float *matrices,
                                                      int32_t instance_count,
                                                      const char *trap_message,
                                                      const float *union_local_min,
                                                      const float *union_local_max,
                                                      float out_world_min[3],
                                                      float out_world_max[3],
                                                      int8_t *out_has_world_bounds) {
    size_t matrix_float_count;
    float *snapshot;
    int accumulate_bounds = union_local_min && union_local_max && out_world_min && out_world_max &&
                            out_has_world_bounds &&
                            canvas3d_bounds_are_valid(union_local_min, union_local_max);
    if (out_has_world_bounds)
        *out_has_world_bounds = 0;
    if (!c || !matrices || instance_count <= 0)
        return NULL;
    if ((size_t)instance_count > SIZE_MAX / (16u * sizeof(float))) {
        rt_trap(trap_message ? trap_message
                             : "Canvas3D.DrawMeshInstanced: matrix allocation overflow");
        return NULL;
    }
    matrix_float_count = (size_t)instance_count * 16u;
    snapshot = (float *)malloc(matrix_float_count * sizeof(*snapshot));
    if (!snapshot) {
        canvas3d_record_submission_failure(c, RT_CANVAS3D_SUBMISSION_INSTANCE_FAILURE);
        rt_trap(trap_message ? trap_message
                             : "Canvas3D.DrawMeshInstanced: matrix allocation failed");
        return NULL;
    }
    for (int32_t i = 0; i < instance_count; ++i) {
        if (!canvas3d_copy_mat4_f32_for_frame(
                c, &matrices[(size_t)i * 16u], &snapshot[(size_t)i * 16u])) {
            free(snapshot);
            rt_trap(trap_message
                        ? trap_message
                        : "Canvas3D.DrawMeshInstanced: rebased matrix is out of float range");
            return NULL;
        }
        if (accumulate_bounds) {
            double world_matrix[16];
            float instance_min[3];
            float instance_max[3];
            for (int j = 0; j < 16; j++)
                world_matrix[j] = (double)snapshot[(size_t)i * 16u + (size_t)j];
            if (c)
                c->frame_aabb_transforms++;
            if (!vgfx3d_transform_aabb_checked(
                    union_local_min, union_local_max, world_matrix, instance_min, instance_max))
                continue;
            if (!*out_has_world_bounds) {
                memcpy(out_world_min, instance_min, sizeof(float) * 3);
                memcpy(out_world_max, instance_max, sizeof(float) * 3);
                *out_has_world_bounds = 1;
            } else {
                canvas3d_expand_bounds(out_world_min, out_world_max, instance_min, instance_max);
            }
        }
    }
    return snapshot;
}

/// @brief Hardware instanced submission: snapshot instance (and motion) matrices into
///        frame-owned buffers and enqueue a single instanced draw command.
/// @details @p motion_indices / @p motion_batch_obj / @p motion_batch_count carry the
///          caller-visible identity for per-instance motion-history keys. Spatially split
///          sub-batches pass the ORIGINAL batch pointer/count and an index remap so each
///          instance keeps the same motion key it had before the split; unsplit batches
///          pass NULL / the live arguments.
/// @param b Borrowed validated batch descriptor whose retained resources back the command.
/// @param base_cmd Mutable command template updated with previous-instance storage.
/// @param instance_matrices Borrowed array of @p instance_count current row-major matrices.
/// @param instance_count Positive number of instances in this hardware draw.
/// @param prev_instance_matrices Optional borrowed previous-frame matrix array.
/// @param has_prev_instance_matrices Non-zero when @p prev_instance_matrices
///   supplies one matrix per current instance.
/// @param explicit_local_bounds_min Optional local-space AABB minimum paired
///   with @p explicit_local_bounds_max.
/// @param explicit_local_bounds_max Optional local-space AABB maximum paired
///   with @p explicit_local_bounds_min.
/// @param conservative_bounds Non-zero to mark the selected bounds conservative.
/// @param disable_occlusion Non-zero to disable CPU occlusion; multi-instance
///   aggregate draws also disable it automatically.
/// @param motion_indices Optional mapping from sub-batch positions to original
///   instance indices; `NULL` selects consecutive local indices.
/// @param motion_batch_obj Borrowed identity of the original matrix batch used
///   when generating stable per-instance motion keys.
/// @param motion_batch_count Original unsplit instance count participating in
///   motion-key generation.
static void canvas3d_queue_instanced_hardware(const canvas3d_instanced_batch_t *b,
                                              vgfx3d_draw_cmd_t *base_cmd,
                                              const float *instance_matrices,
                                              int32_t instance_count,
                                              const float *prev_instance_matrices,
                                              int8_t has_prev_instance_matrices,
                                              const float *explicit_local_bounds_min,
                                              const float *explicit_local_bounds_max,
                                              int8_t conservative_bounds,
                                              int8_t disable_occlusion,
                                              const int32_t *motion_indices,
                                              const void *motion_batch_obj,
                                              int32_t motion_batch_count) {
    rt_canvas3d *c = b->c;
    rt_mesh3d *mesh = b->mesh;
    float local_bounds_min[3];
    float local_bounds_max[3];
    float world_bounds_min[3] = {0.0f, 0.0f, 0.0f};
    float world_bounds_max[3] = {0.0f, 0.0f, 0.0f};
    int8_t has_world_bounds = 0;

    /* Select local bounds before the matrix snapshot so the copy loop can accumulate the
     * per-instance world-AABB union in the same pass. */
    canvas3d_select_instanced_local_bounds(mesh,
                                           base_cmd->vertices,
                                           explicit_local_bounds_min,
                                           explicit_local_bounds_max,
                                           local_bounds_min,
                                           local_bounds_max);

    size_t matrix_float_count = (size_t)instance_count * 16u;
    float *queued_instance_matrices = canvas3d_alloc_instance_matrix_snapshot(
        c,
        instance_matrices,
        instance_count,
        "Canvas3D.DrawMeshInstanced: instance matrix allocation failed",
        local_bounds_min,
        local_bounds_max,
        world_bounds_min,
        world_bounds_max,
        &has_world_bounds);
    if (!queued_instance_matrices) {
        canvas3d_instanced_release_refs(b);
        return;
    }
    if (!canvas3d_track_temp_buffer(c, queued_instance_matrices)) {
        free(queued_instance_matrices);
        canvas3d_instanced_release_refs(b);
        return;
    }

    float *queued_prev_instance_matrices = NULL;
    int needs_motion_vectors = canvas3d_frame_needs_motion_vectors(c);
    if (needs_motion_vectors) {
        if (has_prev_instance_matrices && prev_instance_matrices) {
            queued_prev_instance_matrices = canvas3d_alloc_instance_matrix_snapshot(
                c,
                prev_instance_matrices,
                instance_count,
                "Canvas3D.DrawMeshInstanced: previous instance matrix allocation failed",
                NULL,
                NULL,
                NULL,
                NULL,
                NULL);
        } else {
            queued_prev_instance_matrices =
                (float *)malloc(matrix_float_count * sizeof(*queued_prev_instance_matrices));
        }
        if (!queued_prev_instance_matrices) {
            canvas3d_record_submission_failure(c, RT_CANVAS3D_SUBMISSION_INSTANCE_FAILURE);
            canvas3d_release_tracked_temp_buffer(c, queued_instance_matrices);
            canvas3d_instanced_release_refs(b);
            if (!(has_prev_instance_matrices && prev_instance_matrices))
                rt_trap("Canvas3D.DrawMeshInstanced: previous instance matrix allocation failed");
            return;
        }
        if (!canvas3d_track_temp_buffer(c, queued_prev_instance_matrices)) {
            canvas3d_release_tracked_temp_buffer(c, queued_instance_matrices);
            free(queued_prev_instance_matrices);
            canvas3d_instanced_release_refs(b);
            return;
        }
    }
    for (int32_t i = 0; i < instance_count; ++i) {
        int8_t has_prev = 0;
        float ignored_prev[16];
        float *dst_prev =
            needs_motion_vectors ? &queued_prev_instance_matrices[(size_t)i * 16u] : ignored_prev;
        const float *current = &queued_instance_matrices[(size_t)i * 16u];
        int32_t motion_index = motion_indices ? motion_indices[i] : i;
        uintptr_t motion_key = canvas3d_instance_motion_key(
            b->mesh_obj, b->material_obj, motion_batch_obj, motion_batch_count, motion_index);
        if (needs_motion_vectors && has_prev_instance_matrices && prev_instance_matrices) {
            /* Keep the internal motion history current for the next frame even when the caller
             * supplies an explicit previous-instance snapshot for this draw. */
            canvas3d_resolve_previous_model(c, motion_key, current, ignored_prev, &has_prev);
            continue;
        }
        canvas3d_resolve_previous_model(c, motion_key, current, dst_prev, &has_prev);
        if (needs_motion_vectors && !has_prev)
            memcpy(dst_prev, current, 16u * sizeof(float));
    }

    base_cmd->prev_instance_matrices = queued_prev_instance_matrices;
    base_cmd->has_prev_instance_matrices = queued_prev_instance_matrices ? 1 : 0;
    if (!canvas3d_enqueue_draw_world_bounds(
            c,
            base_cmd,
            DEFERRED_DRAW_INSTANCED,
            DEFERRED_PASS_MAIN,
            queued_instance_matrices,
            instance_count,
            1,
            c->wireframe,
            canvas3d_material_backface_cull(c, b->mat),
            canvas3d_compute_instanced_batch_sort_key(
                c, queued_instance_matrices, instance_count, local_bounds_min, local_bounds_max),
            local_bounds_min,
            local_bounds_max,
            has_world_bounds ? world_bounds_min : NULL,
            has_world_bounds ? world_bounds_max : NULL)) {
        rt_trap("Canvas3D.DrawMeshInstanced: deferred draw queue allocation failed");
        canvas3d_release_tracked_temp_buffer(c, queued_prev_instance_matrices);
        canvas3d_release_tracked_temp_buffer(c, queued_instance_matrices);
        canvas3d_instanced_release_refs(b);
        return;
    }
    /* A hardware-instanced draw has one aggregate AABB for many world positions. Let the
     * backend render it normally, but keep the CPU occlusion grid from culling/writing the
     * whole batch from that coarse rectangle. */
    canvas3d_finalize_deferred_visibility_flags(
        &((deferred_draw_t *)c->draw_cmds)[c->draw_count - 1],
        conservative_bounds,
        disable_occlusion || instance_count > 1);
}

/* A hardware batch culls as one unit through its union AABB, so a spatially wide batch (a
 * forest's every trunk in one call) survives the frustum whole and pops in/out as a block.
 * Batches wider than this along X or Z are partitioned into a small world-space grid of
 * sub-batches so distant cells cull independently. */
#define CANVAS3D_INSTANCED_SPLIT_EXTENT 256.0f
#define CANVAS3D_INSTANCED_SPLIT_MIN_INSTANCES 64
#define CANVAS3D_INSTANCED_SPLIT_MAX_CELLS 4

/// @brief Partition a wide opaque instanced batch into XZ-grid sub-batches and queue each.
/// @details Batches below the size or spatial-extent thresholds remain intact.
///   Temporary bucket storage is released before return; allocation failure
///   requests the ordinary single hardware draw rather than dropping the batch.
/// @param b Borrowed validated batch descriptor shared by every generated cell.
/// @param base_cmd Mutable command template copied into each hardware sub-batch.
/// @param instance_matrices Borrowed array of current row-major instance matrices.
/// @param instance_count Number of current matrices available.
/// @param prev_instance_matrices Optional borrowed previous-frame matrix array.
/// @param has_prev_instance_matrices Non-zero when the previous matrix array is present.
/// @param explicit_local_bounds_min Optional local-space AABB minimum for each instance.
/// @param explicit_local_bounds_max Optional local-space AABB maximum paired
///   with @p explicit_local_bounds_min.
/// @param conservative_bounds Non-zero to mark each cell's bounds conservative.
/// @param disable_occlusion Non-zero to disable occlusion for every generated cell.
/// @return 1 when the batch was split and queued (or split was attempted and trapped),
///         0 when the batch is compact enough to queue as a single hardware draw.
static int canvas3d_queue_instanced_split(const canvas3d_instanced_batch_t *b,
                                          vgfx3d_draw_cmd_t *base_cmd,
                                          const float *instance_matrices,
                                          int32_t instance_count,
                                          const float *prev_instance_matrices,
                                          int8_t has_prev_instance_matrices,
                                          const float *explicit_local_bounds_min,
                                          const float *explicit_local_bounds_max,
                                          int8_t conservative_bounds,
                                          int8_t disable_occlusion) {
    float min_x = 0.0f, max_x = 0.0f, min_z = 0.0f, max_z = 0.0f;
    if (instance_count < CANVAS3D_INSTANCED_SPLIT_MIN_INSTANCES)
        return 0;
    for (int32_t i = 0; i < instance_count; i++) {
        float tx = instance_matrices[(size_t)i * 16u + 3u];
        float tz = instance_matrices[(size_t)i * 16u + 11u];
        if (i == 0) {
            min_x = max_x = tx;
            min_z = max_z = tz;
            continue;
        }
        if (tx < min_x)
            min_x = tx;
        if (tx > max_x)
            max_x = tx;
        if (tz < min_z)
            min_z = tz;
        if (tz > max_z)
            max_z = tz;
    }
    float extent_x = max_x - min_x;
    float extent_z = max_z - min_z;
    float extent = extent_x > extent_z ? extent_x : extent_z;
    if (!(extent > CANVAS3D_INSTANCED_SPLIT_EXTENT))
        return 0;

    int32_t cells = (int32_t)(extent / CANVAS3D_INSTANCED_SPLIT_EXTENT) + 1;
    if (cells > CANVAS3D_INSTANCED_SPLIT_MAX_CELLS)
        cells = CANVAS3D_INSTANCED_SPLIT_MAX_CELLS;
    int32_t cell_count = cells * cells;
    float inv_w = extent_x > 1e-6f ? (float)cells / (extent_x * 1.0001f) : 0.0f;
    float inv_d = extent_z > 1e-6f ? (float)cells / (extent_z * 1.0001f) : 0.0f;

    /* Staging: bucket-contiguous matrices (+ optional prev) and the original index remap
     * that keeps per-instance motion-history keys stable across the split. The hardware
     * path snapshots into frame-owned buffers, so this staging is freed before returning. */
    size_t matrices_bytes = (size_t)instance_count * 16u * sizeof(float);
    float *staged = (float *)malloc(matrices_bytes);
    float *staged_prev = (has_prev_instance_matrices && prev_instance_matrices)
                             ? (float *)malloc(matrices_bytes)
                             : NULL;
    int32_t *remap = (int32_t *)malloc((size_t)instance_count * sizeof(int32_t));
    int32_t *cell_of = (int32_t *)malloc((size_t)instance_count * sizeof(int32_t));
    int32_t *starts = (int32_t *)calloc((size_t)cell_count + 1u, sizeof(int32_t));
    int32_t *cursor = (int32_t *)malloc((size_t)cell_count * sizeof(int32_t));
    if (!staged || !remap || !cell_of || !starts || !cursor ||
        (has_prev_instance_matrices && prev_instance_matrices && !staged_prev)) {
        free(staged);
        free(staged_prev);
        free(remap);
        free(cell_of);
        free(starts);
        free(cursor);
        return 0; /* fall back to the single wide batch rather than failing the draw */
    }
    for (int32_t i = 0; i < instance_count; i++) {
        float tx = instance_matrices[(size_t)i * 16u + 3u];
        float tz = instance_matrices[(size_t)i * 16u + 11u];
        int32_t cx = (int32_t)((tx - min_x) * inv_w);
        int32_t cz = (int32_t)((tz - min_z) * inv_d);
        if (cx < 0)
            cx = 0;
        if (cx >= cells)
            cx = cells - 1;
        if (cz < 0)
            cz = 0;
        if (cz >= cells)
            cz = cells - 1;
        cell_of[i] = cz * cells + cx;
        starts[cell_of[i] + 1]++;
    }
    for (int32_t cidx = 0; cidx < cell_count; cidx++)
        starts[cidx + 1] += starts[cidx];
    memcpy(cursor, starts, (size_t)cell_count * sizeof(int32_t));
    for (int32_t i = 0; i < instance_count; i++) {
        int32_t dst = cursor[cell_of[i]]++;
        memcpy(
            &staged[(size_t)dst * 16u], &instance_matrices[(size_t)i * 16u], 16u * sizeof(float));
        if (staged_prev)
            memcpy(&staged_prev[(size_t)dst * 16u],
                   &prev_instance_matrices[(size_t)i * 16u],
                   16u * sizeof(float));
        remap[dst] = i;
    }
    for (int32_t cidx = 0; cidx < cell_count; cidx++) {
        int32_t begin = starts[cidx];
        int32_t count = starts[cidx + 1] - begin;
        if (count <= 0)
            continue;
        canvas3d_queue_instanced_hardware(b,
                                          base_cmd,
                                          &staged[(size_t)begin * 16u],
                                          count,
                                          staged_prev ? &staged_prev[(size_t)begin * 16u] : NULL,
                                          has_prev_instance_matrices,
                                          explicit_local_bounds_min,
                                          explicit_local_bounds_max,
                                          conservative_bounds,
                                          disable_occlusion,
                                          &remap[begin],
                                          instance_matrices,
                                          instance_count);
    }
    free(staged);
    free(staged_prev);
    free(remap);
    free(cell_of);
    free(starts);
    free(cursor);
    return 1;
}

/// @brief Common instanced-mesh queueing body shared by the plain and per-instance-skinned
///   entries.
/// @details When @p override_palette is non-NULL, the base command's skinning binding is
///   replaced with the caller's packed palette block (@p override_bone_count total matrices,
///   @p instance_bone_stride bones per instance) after the mesh-shared binding, and the draw
///   must take the hardware-instanced path: the per-instance fallback replays one command per
///   instance with instance id 0, which would pose every copy from the first palette. In that
///   case the function queues nothing and returns 0 so the caller can fall back to individual
///   skinned draws.
/// @param canvas_obj Borrowed Canvas3D handle with an active 3D frame.
/// @param mesh_obj Borrowed live Mesh3D used as shared base geometry.
/// @param material_obj Borrowed live Material3D shared by every instance.
/// @param instance_matrices Borrowed array of @p instance_count current row-major matrices.
/// @param instance_count Number of instances; non-positive requests are legitimate no-ops.
/// @param prev_instance_matrices Optional borrowed array of previous-frame matrices.
/// @param has_prev_instance_matrices Non-zero when the previous matrix pointer
///   is required and contains @p instance_count matrices.
/// @param explicit_local_bounds_min Optional local-space AABB minimum paired
///   with @p explicit_local_bounds_max.
/// @param explicit_local_bounds_max Optional local-space AABB maximum paired
///   with @p explicit_local_bounds_min.
/// @param conservative_bounds Non-zero to mark the selected bounds conservative.
/// @param disable_occlusion Non-zero to disable occlusion culling.
/// @param override_palette Optional packed per-instance bone matrices replacing
///   the mesh-shared skinning palette.
/// @param override_prev_palette Optional packed previous-frame palette matching
///   @p override_palette; invalid data is treated as absent.
/// @param override_bone_count Total number of matrices across all override palettes.
/// @param instance_bone_stride Number of bone matrices assigned to each instance.
/// @return 1 when the draw was queued (or legitimately dropped), 0 when a per-instance-skinned
///   draw could not take the hardware path.
static int canvas3d_queue_instanced_batch_common(void *canvas_obj,
                                                 void *mesh_obj,
                                                 void *material_obj,
                                                 const float *instance_matrices,
                                                 int32_t instance_count,
                                                 const float *prev_instance_matrices,
                                                 int8_t has_prev_instance_matrices,
                                                 const float *explicit_local_bounds_min,
                                                 const float *explicit_local_bounds_max,
                                                 int8_t conservative_bounds,
                                                 int8_t disable_occlusion,
                                                 const float *override_palette,
                                                 const float *override_prev_palette,
                                                 int32_t override_bone_count,
                                                 int32_t instance_bone_stride) {
    canvas3d_instanced_batch_t batch = {0};
    rt_canvas3d *c;
    rt_mesh3d *mesh;
    rt_material3d *mat;
    vgfx3d_draw_cmd_t base_cmd;
    vgfx3d_vertex_t *queued_vertices;
    uint32_t *queued_indices;
    int needs_generated_tangents;
    int use_fallback_instances;
    int rebase_positions64_instances;
    int suppress_normal_map = 0;

    if (!instance_matrices || instance_count <= 0)
        return 1;
    if (instance_count > CANVAS3D_MAX_INSTANCES) {
        rt_trap("Canvas3D.DrawMeshInstanced: instance count exceeds limit");
        return 1;
    }
    c = rt_canvas3d_checked_or_stack(canvas_obj);
    mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
#if defined(RT_G3D_ALLOW_STACK_FIXTURES) && RT_G3D_ALLOW_STACK_FIXTURES
    if (!mesh && mesh_obj && !rt_heap_is_payload(mesh_obj))
        mesh = (rt_mesh3d *)mesh_obj;
#endif
    mat = (rt_material3d *)rt_g3d_checked_or_null(material_obj, RT_G3D_MATERIAL3D_CLASS_ID);
    if (!c || !mesh || !mat)
        return 1;
    rt_mesh3d_repair_geometry_counts(mesh);
    if (!c->in_frame || !c->backend || !mesh->resident || rt_mesh3d_safe_vertex_count(mesh) == 0 ||
        rt_mesh3d_validated_index_count(mesh) == 0)
        return 1;
    if (has_prev_instance_matrices && !prev_instance_matrices) {
        rt_trap("Canvas3D.DrawMeshInstanced: previous instance matrices pointer is required");
        return 1;
    }
    if (!canvas3d_matrices_f32_are_finite(instance_matrices, instance_count) ||
        (has_prev_instance_matrices && prev_instance_matrices &&
         !canvas3d_matrices_f32_are_finite(prev_instance_matrices, instance_count))) {
        rt_trap("Canvas3D.DrawMeshInstanced: instance matrices must contain finite values");
        return 1;
    }

    rebase_positions64_instances = canvas3d_uses_camera_relative_upload(c) &&
                                   canvas3d_mesh_positions64_needs_vertex_rebase(mesh);
    needs_generated_tangents = canvas3d_prepare_normal_map_tangent_state(mesh, mat);
    rt_mesh3d_refresh_bounds(mesh);
    canvas3d_build_instanced_base_cmd(mesh, mat, c, &base_cmd);
    if (override_palette && override_bone_count > 0) {
        if (!canvas3d_palette_finite(override_palette, override_bone_count))
            return 1; /* invalid palettes: drop the draw like the shared-palette path would */
        base_cmd.bone_palette = override_palette;
        base_cmd.prev_bone_palette =
            canvas3d_palette_finite(override_prev_palette, override_bone_count)
                ? override_prev_palette
                : NULL;
        base_cmd.bone_count = override_bone_count;
        base_cmd.instance_bone_stride = instance_bone_stride;
    }

    use_fallback_instances = rebase_positions64_instances ||
                             canvas3d_cmd_requires_blend(&base_cmd) ||
                             !c->backend->submit_draw_instanced;
    if (override_palette && (use_fallback_instances || instance_bone_stride <= 0))
        return 0; /* per-instance palettes require the hardware-instanced path */

    batch.c = c;
    batch.mesh = mesh;
    batch.mat = mat;
    batch.mesh_obj = mesh_obj;
    batch.material_obj = material_obj;
    if (rt_heap_is_payload(mesh_obj)) {
        if (!canvas3d_track_draw_resource(c,
                                          mesh_obj,
                                          batch.new_refs,
                                          &batch.new_ref_count,
                                          CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP))
            return 1;
    }
    if (!canvas3d_track_draw_resource(c,
                                      material_obj,
                                      batch.new_refs,
                                      &batch.new_ref_count,
                                      CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP)) {
        canvas3d_instanced_release_refs(&batch);
        return 1;
    }
    if (!canvas3d_track_material_resources(
            c, mat, batch.new_refs, &batch.new_ref_count, CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP)) {
        canvas3d_instanced_release_refs(&batch);
        return 1;
    }

    queued_vertices = mesh->vertices;
    queued_indices = mesh->indices;
    if (!rebase_positions64_instances && needs_generated_tangents) {
        if (!canvas3d_snapshot_mesh_geometry_with_tangents_cached(
                c, mesh, mesh_obj, &queued_vertices, &queued_indices)) {
            if (canvas3d_should_snapshot_geometry(mesh, mesh_obj)) {
                if (!canvas3d_snapshot_mesh_geometry_cached(
                        c, mesh, mesh_obj, &queued_vertices, &queued_indices)) {
                    canvas3d_instanced_release_refs(&batch);
                    return 1;
                }
            } else {
                queued_vertices = mesh->vertices;
                queued_indices = mesh->indices;
            }
            needs_generated_tangents = 0;
            suppress_normal_map = 1;
        }
    } else if (!rebase_positions64_instances && canvas3d_should_snapshot_geometry(mesh, mesh_obj) &&
               !canvas3d_snapshot_mesh_geometry_cached(
                   c, mesh, mesh_obj, &queued_vertices, &queued_indices)) {
        canvas3d_instanced_release_refs(&batch);
        return 1;
    }
    base_cmd.vertices = queued_vertices;
    base_cmd.indices = queued_indices;
    base_cmd.geometry_key =
        (rt_heap_is_payload(mesh_obj) && !rebase_positions64_instances)
            ? (needs_generated_tangents ? (const void *)&mesh->retained_tangent_cache_key
                                        : mesh_obj)
            : NULL;
    base_cmd.geometry_revision = base_cmd.geometry_key ? mesh->geometry_revision : 0;
    base_cmd.compact_vertex_stream = (base_cmd.geometry_key && mesh->compact_streams) ? 1 : 0;
    if (suppress_normal_map)
        canvas3d_disable_cmd_normal_map(&base_cmd);

    if (use_fallback_instances) {
        canvas3d_queue_instanced_fallback(&batch,
                                          &base_cmd,
                                          instance_matrices,
                                          instance_count,
                                          prev_instance_matrices,
                                          has_prev_instance_matrices,
                                          rebase_positions64_instances,
                                          needs_generated_tangents,
                                          explicit_local_bounds_min,
                                          explicit_local_bounds_max,
                                          conservative_bounds,
                                          disable_occlusion);
        return 1;
    }
    if (!override_palette && canvas3d_queue_instanced_split(&batch,
                                                            &base_cmd,
                                                            instance_matrices,
                                                            instance_count,
                                                            prev_instance_matrices,
                                                            has_prev_instance_matrices,
                                                            explicit_local_bounds_min,
                                                            explicit_local_bounds_max,
                                                            conservative_bounds,
                                                            disable_occlusion))
        return 1;
    canvas3d_queue_instanced_hardware(&batch,
                                      &base_cmd,
                                      instance_matrices,
                                      instance_count,
                                      prev_instance_matrices,
                                      has_prev_instance_matrices,
                                      explicit_local_bounds_min,
                                      explicit_local_bounds_max,
                                      conservative_bounds,
                                      disable_occlusion,
                                      NULL,
                                      instance_matrices,
                                      instance_count);
    return 1;
}

/// @brief Queue an instanced-mesh draw (mesh + material + @p instance_count transforms, with
///   optional previous-frame matrices for motion vectors) into the canvas's deferred draw queue.
/// @details Mutable matrix inputs are snapshotted before return. Explicit bounds
///   override mesh bounds only when both arrays form a valid local-space AABB.
/// @param canvas_obj Borrowed Canvas3D handle with an active 3D frame.
/// @param mesh_obj Borrowed live Mesh3D used as shared base geometry.
/// @param material_obj Borrowed live Material3D shared by every instance.
/// @param instance_matrices Borrowed array of @p instance_count current row-major matrices.
/// @param instance_count Number of instances to submit.
/// @param prev_instance_matrices Optional borrowed array of previous-frame matrices.
/// @param has_prev_instance_matrices Non-zero when @p prev_instance_matrices
///   contains one matrix per current instance.
/// @param explicit_local_bounds_min Optional three-component local AABB minimum.
/// @param explicit_local_bounds_max Optional three-component local AABB maximum
///   paired with @p explicit_local_bounds_min.
/// @param conservative_bounds Non-zero to mark bounds conservative.
/// @param disable_occlusion Non-zero to disable occlusion culling for the batch.
void rt_canvas3d_queue_instanced_batch_bounds(void *canvas_obj,
                                              void *mesh_obj,
                                              void *material_obj,
                                              const float *instance_matrices,
                                              int32_t instance_count,
                                              const float *prev_instance_matrices,
                                              int8_t has_prev_instance_matrices,
                                              const float *explicit_local_bounds_min,
                                              const float *explicit_local_bounds_max,
                                              int8_t conservative_bounds,
                                              int8_t disable_occlusion) {
    (void)canvas3d_queue_instanced_batch_common(canvas_obj,
                                                mesh_obj,
                                                material_obj,
                                                instance_matrices,
                                                instance_count,
                                                prev_instance_matrices,
                                                has_prev_instance_matrices,
                                                explicit_local_bounds_min,
                                                explicit_local_bounds_max,
                                                conservative_bounds,
                                                disable_occlusion,
                                                NULL,
                                                NULL,
                                                0,
                                                0);
}

/// @brief Queue one per-instance-skinned hardware chunk (R18).
/// @details @p bone_palettes holds @p instance_count consecutive palettes of
///   @p instance_bone_stride bones each (total_bone_count matrices overall, at most
///   VGFX3D_MAX_BONES). Palette storage must stay valid until the frame flushes —
///   callers snapshot into canvas temp buffers.
/// @param canvas_obj Borrowed Canvas3D handle with an active 3D frame.
/// @param mesh_obj Borrowed live skinned Mesh3D used as shared geometry.
/// @param material_obj Borrowed live Material3D shared by every instance.
/// @param instance_matrices Borrowed array of @p instance_count current matrices.
/// @param instance_count Number of skinned instances in the hardware chunk.
/// @param bone_palettes Borrowed packed current-pose bone matrices, grouped by instance.
/// @param prev_bone_palettes Optional borrowed packed previous-pose matrices.
/// @param total_bone_count Total number of matrices in @p bone_palettes.
/// @param instance_bone_stride Number of consecutive matrices belonging to each instance.
/// @return 1 when queued, 0 when the draw cannot take the hardware-instanced path
///   (the caller must fall back to individual skinned draws).
int rt_canvas3d_queue_instanced_batch_skinned(void *canvas_obj,
                                              void *mesh_obj,
                                              void *material_obj,
                                              const float *instance_matrices,
                                              int32_t instance_count,
                                              const float *bone_palettes,
                                              const float *prev_bone_palettes,
                                              int32_t total_bone_count,
                                              int32_t instance_bone_stride) {
    return canvas3d_queue_instanced_batch_common(canvas_obj,
                                                 mesh_obj,
                                                 material_obj,
                                                 instance_matrices,
                                                 instance_count,
                                                 NULL,
                                                 0,
                                                 NULL,
                                                 NULL,
                                                 0,
                                                 0,
                                                 bone_palettes,
                                                 prev_bone_palettes,
                                                 total_bone_count,
                                                 instance_bone_stride);
}

/// @brief Queue an instanced-mesh draw using the mesh's authored local bounds.
/// @param canvas_obj Borrowed Canvas3D handle with an active 3D frame.
/// @param mesh_obj Borrowed live Mesh3D used as shared base geometry.
/// @param material_obj Borrowed live Material3D shared by every instance.
/// @param instance_matrices Borrowed array of @p instance_count current row-major matrices.
/// @param instance_count Number of instances to submit.
/// @param prev_instance_matrices Optional borrowed previous-frame matrix array.
/// @param has_prev_instance_matrices Non-zero when @p prev_instance_matrices
///   contains one matrix per current instance.
void rt_canvas3d_queue_instanced_batch(void *canvas_obj,
                                       void *mesh_obj,
                                       void *material_obj,
                                       const float *instance_matrices,
                                       int32_t instance_count,
                                       const float *prev_instance_matrices,
                                       int8_t has_prev_instance_matrices) {
    rt_canvas3d_queue_instanced_batch_bounds(canvas_obj,
                                             mesh_obj,
                                             material_obj,
                                             instance_matrices,
                                             instance_count,
                                             prev_instance_matrices,
                                             has_prev_instance_matrices,
                                             NULL,
                                             NULL,
                                             0,
                                             0);
}

/// @brief Queue instanced matrices that have already been converted to frame render space.
/// @details Used by InstanceBatch3D's double-matrix camera-relative path. It reuses the normal
///          instanced queueing pipeline but suppresses the queue's translation rebase so the
///          already-rebased matrices are not shifted twice.
/// @param canvas_obj Borrowed Canvas3D handle with an active 3D frame.
/// @param mesh_obj Borrowed live Mesh3D used as shared base geometry.
/// @param material_obj Borrowed live Material3D shared by every instance.
/// @param instance_matrices Borrowed array of current matrices already expressed
///   in the frame's render-space coordinate system.
/// @param instance_count Number of instances to submit.
/// @param prev_instance_matrices Optional borrowed previous-frame matrices in
///   the same frame-render coordinate space.
/// @param has_prev_instance_matrices Non-zero when @p prev_instance_matrices is present.
void rt_canvas3d_queue_instanced_batch_frame_matrices(void *canvas_obj,
                                                      void *mesh_obj,
                                                      void *material_obj,
                                                      const float *instance_matrices,
                                                      int32_t instance_count,
                                                      const float *prev_instance_matrices,
                                                      int8_t has_prev_instance_matrices) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas_obj);
    int8_t saved_camera_relative;

    if (!c)
        return;
    saved_camera_relative = c->camera_relative_upload;
    c->camera_relative_upload = 0;
    rt_canvas3d_queue_instanced_batch_bounds(canvas_obj,
                                             mesh_obj,
                                             material_obj,
                                             instance_matrices,
                                             instance_count,
                                             prev_instance_matrices,
                                             has_prev_instance_matrices,
                                             NULL,
                                             NULL,
                                             0,
                                             0);
    c->camera_relative_upload = saved_camera_relative;
}

/* Process-lifetime unit quad used only by compact particle instancing. Backends key their static
 * geometry cache with g_canvas3d_particle_unit_quad_key, so its four vertices and six indices are
 * uploaded once per backend context rather than rebuilt per particle or per frame. */
static int g_canvas3d_particle_unit_quad_key;
static vgfx3d_vertex_t g_canvas3d_particle_unit_vertices[4] = {
    {.pos = {-1.0f, -1.0f, 0.0f},
     .normal = {0.0f, 0.0f, 1.0f},
     .uv = {0.0f, 1.0f},
     .uv1 = {0.0f, 1.0f},
     .color = {1.0f, 1.0f, 1.0f, 1.0f},
     .tangent = {1.0f, 0.0f, 0.0f, 1.0f}},
    {.pos = {1.0f, -1.0f, 0.0f},
     .normal = {0.0f, 0.0f, 1.0f},
     .uv = {1.0f, 1.0f},
     .uv1 = {1.0f, 1.0f},
     .color = {1.0f, 1.0f, 1.0f, 1.0f},
     .tangent = {1.0f, 0.0f, 0.0f, 1.0f}},
    {.pos = {1.0f, 1.0f, 0.0f},
     .normal = {0.0f, 0.0f, 1.0f},
     .uv = {1.0f, 0.0f},
     .uv1 = {1.0f, 0.0f},
     .color = {1.0f, 1.0f, 1.0f, 1.0f},
     .tangent = {1.0f, 0.0f, 0.0f, 1.0f}},
    {.pos = {-1.0f, 1.0f, 0.0f},
     .normal = {0.0f, 0.0f, 1.0f},
     .uv = {0.0f, 0.0f},
     .uv1 = {0.0f, 0.0f},
     .color = {1.0f, 1.0f, 1.0f, 1.0f},
     .tangent = {1.0f, 0.0f, 0.0f, 1.0f}},
};
static uint32_t g_canvas3d_particle_unit_indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};

/// @brief Validate one compact particle record and expand the aggregate axis-aligned bounds.
/// @details Right/up are arbitrary camera-plane half axes; the exact conservative contribution on
///   each world axis is `abs(right) + abs(up)`. Color is required to remain finite and normalized
///   because every backend multiplies it directly into the material/texture result.
/// @param instance Record to validate.
/// @param first Non-zero when this is the first aggregate record.
/// @param bounds_min Aggregate minimum updated on success.
/// @param bounds_max Aggregate maximum updated on success.
/// @return Non-zero for a finite normalized record whose bound arithmetic remains finite.
static int canvas3d_accumulate_particle_instance_bounds(const vgfx3d_particle_instance_t *instance,
                                                        int first,
                                                        float bounds_min[3],
                                                        float bounds_max[3]) {
    if (!instance || !bounds_min || !bounds_max)
        return 0;
    for (int axis = 0; axis < 3; axis++) {
        float center = instance->center[axis];
        float extent = fabsf(instance->right[axis]) + fabsf(instance->up[axis]);
        float lo = center - extent;
        float hi = center + extent;
        if (!isfinite(center) || !isfinite(extent) || !isfinite(lo) || !isfinite(hi))
            return 0;
        if (first || lo < bounds_min[axis])
            bounds_min[axis] = lo;
        if (first || hi > bounds_max[axis])
            bounds_max[axis] = hi;
    }
    for (int lane = 0; lane < 4; lane++) {
        if (!isfinite(instance->center[lane]) || !isfinite(instance->right[lane]) ||
            !isfinite(instance->up[lane]) || !isfinite(instance->color[lane]) ||
            instance->color[lane] < 0.0f || instance->color[lane] > 1.0f)
            return 0;
    }
    return 1;
}

/// @brief Report whether the active Canvas3D backend accepts compact particle instances.
/// @details The explicit vtable capability avoids backend-name branching and prevents the software
///   instanced fallback from receiving a payload whose vertices have not been CPU-expanded.
/// @param canvas_obj Borrowed Canvas3D handle to query.
/// @return Non-zero only when the canvas has a backend that advertises compact
///   particle records and provides hardware instanced submission.
int rt_canvas3d_supports_particle_instancing(void *canvas_obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas_obj);
    return c && c->backend && c->backend->particle_instancing && c->backend->submit_draw_instanced;
}

/// @brief Queue a sorted compact particle batch against the retained unit quad.
/// @details Instance bytes are copied into Canvas-owned frame storage before the caller may reuse
///   its scratch buffer. The material and its texture dependencies are retained transactionally;
///   every failure before enqueue releases only references acquired by this call. Aggregate bounds
///   permit frustum rejection, while CPU occlusion is disabled for the spatially distributed batch.
/// @param canvas_obj Borrowed Canvas3D handle with an active 3D frame and compact
///   particle-instancing support.
/// @param material_obj Borrowed live Material3D retained with its texture
///   dependencies through deferred replay.
/// @param instances Borrowed array of compact particle records copied before return.
/// @param instance_count Positive number of records in @p instances.
/// @return One when the validated batch is queued; zero for unsupported or
///   invalid state and for any validation, allocation, tracking, or queue failure.
int rt_canvas3d_queue_particle_batch(void *canvas_obj,
                                     void *material_obj,
                                     const vgfx3d_particle_instance_t *instances,
                                     int32_t instance_count) {
    canvas3d_instanced_batch_t batch = {0};
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas_obj);
    rt_material3d *mat =
        (rt_material3d *)rt_g3d_checked_or_null(material_obj, RT_G3D_MATERIAL3D_CLASS_ID);
    rt_mesh3d unit_mesh = {0};
    vgfx3d_draw_cmd_t cmd;
    vgfx3d_particle_instance_t *snapshot;
    float bounds_min[3] = {0.0f, 0.0f, 0.0f};
    float bounds_max[3] = {0.0f, 0.0f, 0.0f};
    size_t bytes;

    if (!c || !mat || !instances || instance_count <= 0 || !c->in_frame ||
        !rt_canvas3d_supports_particle_instancing(canvas_obj))
        return 0;
    if (instance_count > CANVAS3D_MAX_INSTANCES ||
        (size_t)instance_count > SIZE_MAX / sizeof(*snapshot)) {
        rt_trap("Particles3D.Draw: compact instance count exceeds renderer limit");
        return 0;
    }
    for (int32_t i = 0; i < instance_count; i++) {
        if (!canvas3d_accumulate_particle_instance_bounds(
                &instances[i], i == 0, bounds_min, bounds_max)) {
            rt_trap("Particles3D.Draw: compact instance data must be finite");
            return 0;
        }
    }

    bytes = (size_t)instance_count * sizeof(*snapshot);
    snapshot = (vgfx3d_particle_instance_t *)malloc(bytes);
    if (!snapshot) {
        rt_trap("Particles3D.Draw: compact instance allocation failed");
        return 0;
    }
    memcpy(snapshot, instances, bytes);
    if (!canvas3d_track_temp_buffer(c, snapshot)) {
        free(snapshot);
        return 0;
    }

    batch.c = c;
    batch.mat = mat;
    batch.material_obj = material_obj;
    if (!canvas3d_track_draw_resource(c,
                                      material_obj,
                                      batch.new_refs,
                                      &batch.new_ref_count,
                                      CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP) ||
        !canvas3d_track_material_resources(
            c, mat, batch.new_refs, &batch.new_ref_count, CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP)) {
        canvas3d_release_tracked_temp_buffer(c, snapshot);
        canvas3d_instanced_release_refs(&batch);
        return 0;
    }

    unit_mesh.vertices = g_canvas3d_particle_unit_vertices;
    unit_mesh.vertex_count = 4u;
    unit_mesh.vertex_capacity = 4u;
    unit_mesh.indices = g_canvas3d_particle_unit_indices;
    unit_mesh.index_count = 6u;
    unit_mesh.index_capacity = 6u;
    unit_mesh.geometry_revision = 1u;
    unit_mesh.resident = 1;
    canvas3d_build_instanced_base_cmd(&unit_mesh, mat, c, &cmd);
    cmd.geometry_key = &g_canvas3d_particle_unit_quad_key;
    cmd.geometry_revision = 1u;
    cmd.validated_index_count = 6u;
    cmd.particle_instances = snapshot;

    if (!canvas3d_enqueue_draw_world_bounds(
            c,
            &cmd,
            DEFERRED_DRAW_INSTANCED,
            DEFERRED_PASS_MAIN,
            NULL,
            instance_count,
            1,
            c->wireframe,
            canvas3d_material_backface_cull(c, mat),
            canvas3d_compute_sort_key(c, cmd.model_matrix, bounds_min, bounds_max, 1, 1),
            bounds_min,
            bounds_max,
            bounds_min,
            bounds_max)) {
        canvas3d_release_tracked_temp_buffer(c, snapshot);
        canvas3d_instanced_release_refs(&batch);
        return 0;
    }
    canvas3d_finalize_deferred_visibility_flags(
        &((deferred_draw_t *)c->draw_cmds)[c->draw_count - 1], 1, 1);
    return 1;
}
