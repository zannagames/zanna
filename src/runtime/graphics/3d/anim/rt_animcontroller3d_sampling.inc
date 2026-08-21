//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/anim/rt_animcontroller3d_sampling.inc  (#included from rt_animcontroller3d.c; not a standalone translation unit)
// Purpose: AnimController3D pose sampling, palette compute, and blend evaluation
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements AnimController3D pose sampling and layer compositing.
/// @details This private fragment is included by `rt_animcontroller3d.c`. It
///          samples individual states for root-motion wrap compensation,
///          evaluates blend-tree/base, overlay, IK, and bone-LOD stages into
///          model-space globals and skinning palettes, and provides the
///          double-precision quaternion helpers used for root-motion deltas.

/// @brief Compute the model-space matrix of @p bone_index for the given state at @p time
///   by sampling every bone's local transform and walking the parent chain; returns 0
///   on invalid controller/state/bone, else 1 with the result in @p out_global.
/// @details Reuses a controller-owned locals-plus-globals scratch allocation,
///          growing it to the current bone count on demand.
/// @param[in,out] controller Controller providing the skeleton, state table,
///                           and reusable sample buffer.
/// @param[in] state_index Registered state-table index.
/// @param[in] time Clip time in seconds.
/// @param[in] bone_index Skeleton bone whose global matrix to copy.
/// @param[out] out_global Writable row-major matrix of 16 floats.
/// @return `1` when the matrix was sampled; `0` for invalid input, missing
///         animation, or scratch-allocation failure.
static int controller_sample_state_global_matrix(rt_anim_controller3d *controller,
                                                 int32_t state_index,
                                                 float time,
                                                 int32_t bone_index,
                                                 float *out_global) {
    /* Heap-backed: two VGFX3D_MAX_SKELETON_BONES*16 matrices would need 128 KB
     * of stack. Bone-socket queries run this every frame per socketed node, so
     * the buffer persists on the controller (grown on demand, freed with it)
     * instead of paying a calloc/free per sample. */
    float *globals;
    float *locals;
    int32_t state_count;
    int32_t bone_count;
    rt_animation3d *animation;
    if (!controller || !controller->skeleton || !out_global)
        return 0;
    state_count = controller_safe_state_count(controller);
    bone_count = controller_safe_bone_count(controller->skeleton);
    if (state_index < 0 || state_index >= state_count)
        return 0;
    if (bone_index < 0 || bone_index >= bone_count)
        return 0;
    animation = controller->states[state_index].animation;
    if (!animation)
        return 0;
    if (controller->sample_scratch_bones < bone_count) {
        float *grown = (float *)realloc(controller->sample_scratch,
                                        (size_t)bone_count * 32u * sizeof(float));
        if (!grown)
            return 0;
        controller->sample_scratch = grown;
        controller->sample_scratch_bones = bone_count;
    }
    locals = controller->sample_scratch;
    memset(locals, 0, (size_t)bone_count * 32u * sizeof(float));
    globals = locals + (size_t)bone_count * 16u;
    for (int32_t bone = 0; bone < bone_count; bone++) {
        controller_sample_animation_local_matrix(
            controller->skeleton, animation, bone, time, &locals[bone * 16]);
    }
    skeleton3d_compute_globals_from_locals(controller->skeleton, locals, globals, bone_count);
    memcpy(out_global, &globals[bone_index * 16], 16 * sizeof(float));
    return 1;
}

/// @brief Composite the base layer + weighted overlay layers into the final bone palette.
/// @details Two-pass blend:
///          1. A compatible BlendTree3D, the base player's local transforms,
///             or skeleton bind locals seed the working local-pose buffer in
///             that precedence order.
///          2. For each overlay layer (1..MAX-1) with weight > 0, blend the
///             overlay's locals into the working pose at the layer's weight,
///             but only on bones whose `mask_bits` flag is set. The blend
///             decomposes each matrix to TRS, lerps translation/scale, and
///             slerps rotation before recomposition.
///          Replace overlays interpolate toward their pose; additive overlays
///          apply a weighted bind-pose-relative delta. Globals are rebuilt
///          after each contributing overlay, then IK is applied. Bone-count
///          LOD finally restores distal bind locals and recomputes globals
///          before every global is multiplied by inverse bind. Layers with
///          empty masks or negligible weight contribute nothing.
/// @param[in,out] controller Controller whose pose sources, masks, scratch,
///                           globals, and palette are evaluated.
static void controller_compute_final_palette(rt_anim_controller3d *controller) {
    int32_t bone_count;
    int32_t state_count;
    /* Per-controller scratch instead of a ~16 KB stack array: this runs on the parallel
     * animator's worker threads, and each controller is updated by a single job at a time, so
     * the buffer is never shared concurrently. (Not zero-initialised — like the previous stack
     * array — because every entry used is written before it is read.) */
    float *final_locals;
    if (!controller || !controller->final_palette || !controller->final_globals ||
        !controller->skeleton || !controller->blend_locals_scratch)
        return;
    final_locals = controller->blend_locals_scratch;
    controller_repair_state_table(controller);
    controller_repair_layer_state_indices(controller);
    state_count = controller->state_count;
    bone_count = controller_safe_bone_count(controller->skeleton);
    if (bone_count <= 0)
        return;

    for (int32_t layer_index = 0; layer_index < RT_ANIM_CONTROLLER3D_MAX_LAYERS;
         layer_index++) {
        anim_controller3d_layer_t *layer = &controller->layers[layer_index];
        if (layer_index == 0 || layer->mask_root_bone < 0 ||
            layer->mask_root_bone >= bone_count) {
            if (layer->mask_bone_count_seen != bone_count || layer_index == 0)
                controller_set_all_mask_bits(layer, bone_count);
        } else {
            controller_rebuild_layer_mask(controller, layer_index);
        }
    }

    if (controller->blend_tree) {
        void *blend = rt_blend_tree3d_get_blend(controller->blend_tree);
        int32_t blend_bone_count = 0;
        const float *blend_locals =
            rt_anim_blend3d_get_local_transform_data(blend, &blend_bone_count);
        if (blend_locals && blend_bone_count >= bone_count) {
            memcpy(final_locals, blend_locals, (size_t)bone_count * 16 * sizeof(float));
        } else {
            for (int32_t bone = 0; bone < bone_count; bone++) {
                memcpy(&final_locals[bone * 16],
                       controller->skeleton->bones[bone].bind_pose_local,
                       16 * sizeof(float));
            }
        }
    } else if (controller->layers[0].current_state >= 0 &&
               controller->layers[0].current_state < state_count && controller->layers[0].player &&
               controller->layers[0].player->local_transforms) {
        memcpy(final_locals,
               controller->layers[0].player->local_transforms,
               (size_t)bone_count * 16 * sizeof(float));
    } else {
        for (int32_t bone = 0; bone < bone_count; bone++) {
            memcpy(&final_locals[bone * 16],
                   controller->skeleton->bones[bone].bind_pose_local,
                   16 * sizeof(float));
        }
    }

    skeleton3d_compute_globals_from_locals(
        controller->skeleton, final_locals, controller->final_globals, bone_count);

    for (int32_t layer_index = 1; layer_index < RT_ANIM_CONTROLLER3D_MAX_LAYERS; layer_index++) {
        anim_controller3d_layer_t *layer = &controller->layers[layer_index];
        float weight = layer->weight;
        if (!layer->player || !layer->player->local_transforms || layer->current_state < 0 ||
            layer->current_state >= state_count || weight <= 1e-6f)
            continue;
        if (weight > 1.0f)
            weight = 1.0f;
        for (int32_t bone = 0; bone < bone_count; bone++) {
            float *dst;
            const float *src;
            if (!layer->mask_bits[bone])
                continue;
            dst = &final_locals[bone * 16];
            src = &layer->player->local_transforms[bone * 16];
            {
                float dst_pos[3], dst_rot[4], dst_scl[3];
                float src_pos[3], src_rot[4], src_scl[3];
                float blend_pos[3], blend_rot[4], blend_scl[3];
                controller_decompose_trs_float(dst, dst_pos, dst_rot, dst_scl);
                controller_decompose_trs_float(src, src_pos, src_rot, src_scl);
                if (layer->additive) {
                    const float *bind = controller->skeleton->bones[bone].bind_pose_local;
                    float bind_pos[3], bind_rot[4], bind_scl[3];
                    float inv_bind_rot[4], delta_rot[4], weighted_delta[4];
                    const float identity_rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    controller_decompose_trs_float(bind, bind_pos, bind_rot, bind_scl);
                    for (int32_t i = 0; i < 3; i++) {
                        blend_pos[i] = dst_pos[i] + (src_pos[i] - bind_pos[i]) * weight;
                        blend_scl[i] = dst_scl[i] + (src_scl[i] - bind_scl[i]) * weight;
                        if (!isfinite(blend_scl[i]) || fabsf(blend_scl[i]) <= 1e-6f)
                            blend_scl[i] = 1.0f;
                    }
                    controller_quat_conjugate_float(bind_rot, inv_bind_rot);
                    controller_quat_mul_float(src_rot, inv_bind_rot, delta_rot);
                    controller_quat_slerp_float(identity_rot, delta_rot, weight, weighted_delta);
                    controller_quat_mul_float(weighted_delta, dst_rot, blend_rot);
                } else {
                    for (int32_t i = 0; i < 3; i++) {
                        blend_pos[i] = dst_pos[i] + (src_pos[i] - dst_pos[i]) * weight;
                        blend_scl[i] = dst_scl[i] + (src_scl[i] - dst_scl[i]) * weight;
                    }
                    controller_quat_slerp_float(dst_rot, src_rot, weight, blend_rot);
                }
                controller_sanitize_trs_sample(
                    blend_pos, blend_rot, blend_scl, dst_pos, dst_rot, dst_scl);
                controller_build_trs_float(blend_pos, blend_rot, blend_scl, dst);
            }
        }
        skeleton3d_compute_globals_from_locals(
            controller->skeleton, final_locals, controller->final_globals, bone_count);
    }

    int32_t ik_count = controller->ik_solver_count;
    if (ik_count < 0 || ik_count > RT_ANIM_CONTROLLER3D_MAX_IK_SOLVERS)
        ik_count = 0;
    for (int32_t i = 0; i < ik_count; i++) {
        if (rt_g3d_has_class(controller->ik_solvers[i], RT_G3D_IKSOLVER3D_CLASS_ID)) {
            (void)rt_ik_solver3d_apply_to_pose(
                controller->ik_solvers[i], final_locals, controller->final_globals, bone_count);
        }
    }

    /* Bone-count animation LOD: for distant characters, freeze bones at/after the LOD index to
     * their bind-pose local transform. They still follow their (animated) ancestors through the
     * hierarchy, but add no local animation of their own, so distal detail (fingers, toes) costs
     * nothing to pose. The shared hierarchy builder keeps frozen bones hanging off their last
     * animated ancestors even when an imported skeleton is not stored parent-before-child. */
    if (controller->animation_lod_max_bones > 0 &&
        controller->animation_lod_max_bones < bone_count) {
        for (int32_t bone = controller->animation_lod_max_bones; bone < bone_count; bone++) {
            const float *bind = controller->skeleton->bones[bone].bind_pose_local;
            memcpy(&final_locals[bone * 16], bind, 16 * sizeof(float));
        }
        skeleton3d_compute_globals_from_locals(
            controller->skeleton, final_locals, controller->final_globals, bone_count);
    }

    for (int32_t bone = 0; bone < bone_count; bone++) {
        controller_mat4f_mul(&controller->final_globals[bone * 16],
                             controller->skeleton->bones[bone].inverse_bind,
                             &controller->final_palette[bone * 16]);
    }
}

/// @brief Read the translation column from the controller's final global pose.
/// @details Used by root-motion delta accumulation. Out-pointers are zeroed on failure so
///          callers can safely subtract before/after samples.
/// @param[in] controller Controller whose model-space globals to inspect.
/// @param[in] bone_index Zero-based skeleton bone index.
/// @param[out] x Writable X translation.
/// @param[out] y Writable Y translation.
/// @param[out] z Writable Z translation.
static void controller_get_final_translation(
    const rt_anim_controller3d *controller, int32_t bone_index, double *x, double *y, double *z) {
    const float *m;
    if (!x || !y || !z) {
        return;
    }
    *x = 0.0;
    *y = 0.0;
    *z = 0.0;
    if (!controller || !controller->final_globals || !controller->skeleton)
        return;
    if (bone_index < 0 || bone_index >= controller_safe_bone_count(controller->skeleton))
        return;
    m = &controller->final_globals[bone_index * 16];
    *x = (double)m[3];
    *y = (double)m[7];
    *z = (double)m[11];
}

/// @brief Set a quaternion to the identity rotation `(0, 0, 0, 1)` (xyzw).
/// @param[out] out Writable array of at least four doubles; `NULL` is ignored.
static void controller_quat_identity(double *out) {
    if (!out)
        return;
    out[0] = 0.0;
    out[1] = 0.0;
    out[2] = 0.0;
    out[3] = 1.0;
}

/// @brief Normalize a quaternion in place to unit length.
/// @details Falls back to identity if the quaternion is degenerate (squared
///          length below 1e-20) to avoid `1/0` from the inverse-length
///          division. Critical because subsequent quaternion multiplications
///          assume unit operands.
/// @param[in,out] q Writable array of at least four doubles; becomes identity
///                  for non-finite or near-zero length.
static void controller_quat_normalize(double *q) {
    double len_sq;
    double inv_len;
    if (!q)
        return;
    len_sq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!isfinite(len_sq) || len_sq < 1e-20) {
        controller_quat_identity(q);
        return;
    }
    inv_len = 1.0 / sqrt(len_sq);
    q[0] *= inv_len;
    q[1] *= inv_len;
    q[2] *= inv_len;
    q[3] *= inv_len;
}

/// @brief Convert a 3×3 rotation matrix (passed as nine row elements) to a quaternion.
/// @details Implementation of Shepperd's method (1978): pick the branch that
///          maximises numerical stability based on which of `trace`,
///          `m00`, `m11`, or `m22` is largest. This avoids the catastrophic
///          cancellation that the naive `sqrt(1+trace)/2` formulation
///          produces near 180° rotations. A degenerate (near-zero-scale) input
///          matrix collapses the chosen branch's divisor `s` toward zero; each
///          branch guards `s` and falls back to the identity quaternion to avoid
///          dividing by ~0 and emitting a garbage-direction rotation. Output
///          quaternion is normalised before return.
/// @param[in] m00 Row-zero, column-zero basis lane.
/// @param[in] m01 Row-zero, column-one basis lane.
/// @param[in] m02 Row-zero, column-two basis lane.
/// @param[in] m10 Row-one, column-zero basis lane.
/// @param[in] m11 Row-one, column-one basis lane.
/// @param[in] m12 Row-one, column-two basis lane.
/// @param[in] m20 Row-two, column-zero basis lane.
/// @param[in] m21 Row-two, column-one basis lane.
/// @param[in] m22 Row-two, column-two basis lane.
/// @param[out] out Writable normalized quaternion in `(x,y,z,w)` order.
static void controller_quat_from_matrix_rows(double m00,
                                             double m01,
                                             double m02,
                                             double m10,
                                             double m11,
                                             double m12,
                                             double m20,
                                             double m21,
                                             double m22,
                                             double *out) {
    double trace;
    if (!out)
        return;
    if (!isfinite(m00) || !isfinite(m01) || !isfinite(m02) || !isfinite(m10) || !isfinite(m11) ||
        !isfinite(m12) || !isfinite(m20) || !isfinite(m21) || !isfinite(m22)) {
        controller_quat_identity(out);
        return;
    }
    trace = m00 + m11 + m22;
    if (trace > 0.0) {
        double s = sqrt(trace + 1.0) * 2.0;
        if (s < 1e-12) {
            controller_quat_identity(out);
            return;
        }
        out[3] = 0.25 * s;
        out[0] = (m21 - m12) / s;
        out[1] = (m02 - m20) / s;
        out[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        double s = sqrt(1.0 + m00 - m11 - m22) * 2.0;
        if (s < 1e-12) {
            controller_quat_identity(out);
            return;
        }
        out[3] = (m21 - m12) / s;
        out[0] = 0.25 * s;
        out[1] = (m01 + m10) / s;
        out[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        double s = sqrt(1.0 + m11 - m00 - m22) * 2.0;
        if (s < 1e-12) {
            controller_quat_identity(out);
            return;
        }
        out[3] = (m02 - m20) / s;
        out[0] = (m01 + m10) / s;
        out[1] = 0.25 * s;
        out[2] = (m12 + m21) / s;
    } else {
        double s = sqrt(1.0 + m22 - m00 - m11) * 2.0;
        if (s < 1e-12) {
            controller_quat_identity(out);
            return;
        }
        out[3] = (m10 - m01) / s;
        out[0] = (m02 + m20) / s;
        out[1] = (m12 + m21) / s;
        out[2] = 0.25 * s;
    }
    controller_quat_normalize(out);
}

/// @brief Extract the rotation quaternion from the controller's final bone matrix.
/// @details The final global pose stores TRS-composed matrices, so the rotation
///          must be unscaled before being converted to a quaternion.
///          Computes per-axis scale from the columns' Euclidean norms,
///          divides each column out by its scale to recover an orthonormal
///          rotation matrix, then converts via Shepperd's method. Falls
///          back to identity when any axis scale is degenerate (< 1e-8).
/// @param[in] controller Controller whose model-space globals to inspect.
/// @param[in] bone_index Zero-based skeleton bone index.
/// @param[out] out_quat Writable four-double quaternion initialized to
///                      identity before controller/index validation.
static void controller_get_final_rotation(const rt_anim_controller3d *controller,
                                          int32_t bone_index,
                                          double *out_quat) {
    const float *m;
    double sx;
    double sy;
    double sz;
    double inv_sx;
    double inv_sy;
    double inv_sz;

    controller_quat_identity(out_quat);
    if (!controller || !controller->final_globals || !controller->skeleton)
        return;
    if (bone_index < 0 || bone_index >= controller_safe_bone_count(controller->skeleton))
        return;
    m = &controller->final_globals[bone_index * 16];
    sx = sqrt((double)m[0] * (double)m[0] + (double)m[4] * (double)m[4] +
              (double)m[8] * (double)m[8]);
    sy = sqrt((double)m[1] * (double)m[1] + (double)m[5] * (double)m[5] +
              (double)m[9] * (double)m[9]);
    sz = sqrt((double)m[2] * (double)m[2] + (double)m[6] * (double)m[6] +
              (double)m[10] * (double)m[10]);
    if (!isfinite(sx) || !isfinite(sy) || !isfinite(sz) || sx < 1e-8 || sy < 1e-8 ||
        sz < 1e-8)
        return;
    inv_sx = 1.0 / sx;
    inv_sy = 1.0 / sy;
    inv_sz = 1.0 / sz;
    controller_quat_from_matrix_rows((double)m[0] * inv_sx,
                                     (double)m[1] * inv_sy,
                                     (double)m[2] * inv_sz,
                                     (double)m[4] * inv_sx,
                                     (double)m[5] * inv_sy,
                                     (double)m[6] * inv_sz,
                                     (double)m[8] * inv_sx,
                                     (double)m[9] * inv_sy,
                                     (double)m[10] * inv_sz,
                                     out_quat);
}

/// @brief Compute the conjugate of a quaternion (`(-x, -y, -z, w)`).
/// @details For a unit quaternion, the conjugate equals the inverse. Used
///          here to invert the "before" rotation when computing the
///          per-frame rotation delta.
/// @param[in] q Borrowed four-double quaternion.
/// @param[out] out Writable four-double conjugate; may alias @p q.
static void controller_quat_conjugate(const double *q, double *out) {
    if (!q || !out)
        return;
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

/// @brief Multiply two quaternions: `out = a * b` (apply `b` then `a`).
/// @details Standard Hamilton product. Output is normalised before return
///          to absorb the floating-point drift that accumulates as
///          quaternion multiplications are chained over many frames of
///          root-motion accumulation.
/// @param[in] a Borrowed left quaternion operand.
/// @param[in] b Borrowed right quaternion operand.
/// @param[out] out Writable normalized product; must not alias either input.
static void controller_quat_mul(const double *a, const double *b, double *out) {
    double x;
    double y;
    double z;
    double w;
    if (!a || !b || !out)
        return;
    w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
    controller_quat_normalize(out);
}

/// @brief Return @p value when finite, else @p fallback (double scalar sanitizer).
/// @param[in] value Candidate double value.
/// @param[in] fallback Replacement for NaN or infinity.
/// @return @p value when finite; otherwise @p fallback.
static double controller_finite_double_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}
